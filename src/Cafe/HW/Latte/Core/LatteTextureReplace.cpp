#include "Cafe/HW/Latte/Core/LatteTextureReplace.h"
#include "Cafe/HW/Latte/Core/LatteTextureLoader.h"
#include "config/ActiveSettings.h"
#include "Cafe/CafeSystem.h"
#include "util/helpers/helpers.h"

#include <filesystem>
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <set>
#include <mutex>
#include <atomic>
#include <fstream>
#include <vector>
#include <string>
#include <algorithm>
#include <chrono>
#include <functional>
#include <cctype>

namespace fs = std::filesystem;


namespace LatteTextureReplace
{
	// bytesPerBlock is per 4x4 block for the BCn formats and per pixel for the uncompressed one
	struct DDSInfo { uint32_t gx2Format=0; int bytesPerBlock=0; int width=0, height=0, mipCount=1; uint32_t dataOffset=0; bool ok=false; bool blockCompressed=true; };
	static uint32_t rd32(const uint8_t* p){ return p[0]|(p[1]<<8)|(p[2]<<16)|((uint32_t)p[3]<<24); }
	static DDSInfo ddsParseHeader(const uint8_t* d, size_t n)
	{
		DDSInfo r;
		if (n < 128 || rd32(d) != 0x20534444) return r;
		r.height = rd32(d+12); r.width = rd32(d+16); r.mipCount = std::max<uint32_t>(1, rd32(d+28));
		uint32_t fourcc = rd32(d+84), dxgi = 0; r.dataOffset = 128;
		auto FCC=[](const char* s){ return (uint32_t)((uint8_t)s[0]|((uint8_t)s[1]<<8)|((uint8_t)s[2]<<16)|((uint32_t)(uint8_t)s[3]<<24)); };
		if (fourcc == FCC("DX10")) { if (n < 148) return r; dxgi = rd32(d+128); r.dataOffset = 148; }
		auto set=[&](uint32_t gx2,int bpb){ r.gx2Format=gx2; r.bytesPerBlock=bpb; r.ok=true; };
		// Uncompressed 32-bit DDS. Supported so that an RGBA8 guest texture can be replaced without
		// a format overwrite: the replacement is already the format the texture is in, so the host
		// texture needs no redefinition at all.
		if (fourcc==0)
		{
			uint32_t pfFlags=rd32(d+80), bitCount=rd32(d+88);
			uint32_t rMask=rd32(d+92), gMask=rd32(d+96), bMask=rd32(d+100), aMask=rd32(d+104);
			const bool hasRGB=(pfFlags&0x40)!=0, hasAlpha=(pfFlags&0x1)!=0;
			if (hasRGB && hasAlpha && bitCount==32 &&
				rMask==0x000000FF && gMask==0x0000FF00 && bMask==0x00FF0000 && aMask==0xFF000000)
			{
				set(0x01a, 4);
				r.blockCompressed=false;
				return r;
			}
			return r; // some other uncompressed layout -- not supported, and refused rather than guessed at
		}
		if      (fourcc==FCC("DXT1") || dxgi==70||dxgi==71||dxgi==72) set(0x031, 8);
		else if (fourcc==FCC("DXT3") || dxgi==73||dxgi==74||dxgi==75) set(0x032, 16);
		else if (fourcc==FCC("DXT5") || dxgi==76||dxgi==77||dxgi==78) set(0x033, 16);
		else if (fourcc==FCC("ATI1") || fourcc==FCC("BC4U") || dxgi==80||dxgi==81) set(0x034, 8);
		else if (fourcc==FCC("ATI2") || fourcc==FCC("BC5U") || dxgi==83||dxgi==84) set(0x035, 16);
		// DXGI_FORMAT_R8G8B8A8_UNORM / _UNORM_SRGB, written when a converter uses a DX10 header
		else if (dxgi==28) { set(0x01a, 4); r.blockCompressed=false; }
		else if (dxgi==29) { set(0x41a, 4); r.blockCompressed=false; }
		return r;
	}
	static uint32_t mipByteSize(int w,int h,int bpb,bool blockCompressed=true){
		if(!blockCompressed) return (uint32_t)(std::max(1,w)*std::max(1,h)*bpb);
		return (uint32_t)((std::max(1,(w+3)/4))*(std::max(1,(h+3)/4))*bpb);
	}

	// width/height/gx2Format: the DDS itself, read on first use. name*: what the filename says about
	// the game's own texture, which is what an older pack's name has to be checked against.
	struct FileRef { fs::path path; int width=0, height=0; uint32_t gx2Format=0; bool probed=false; std::string pack; int nameW=0, nameH=0; uint32_t nameFmt=0; };
	struct HashGroup { std::unordered_map<int,FileRef> mips; std::unordered_map<int,LatteTextureReplace_Entry> decoded; std::unordered_map<int,LatteTextureReplace_Entry> rgba8; };
	struct TitleSettings { bool enabled=true; std::vector<std::string> packs; };

	static std::mutex s_mutex;
	static std::unordered_map<uint64_t,HashGroup> s_index;
	// Current-scheme hash -> the plain content hash an older pack names the same texture by. Filled
	// in as textures are hashed (see HashGuestAlways), consulted by every lookup through findGroup.
	static std::unordered_map<uint64_t,uint64_t> s_legacyAlias;
	// [filtering] from the enabled packs' textures.ini, by hash (see FilterOverride)
	static std::unordered_map<uint64_t,uint8_t> s_filters;
	static bool s_enabled=false, s_skipMip=false, s_initDone=false, s_wantEnabled=false;
	static uint64_t s_title=0;
	static size_t s_conflictCount=0;
	// Per-title selection, pushed in from the GUI. A title with no entry has never been configured
	// and defaults to "every pack in its folder", so a freshly dropped-in pack works immediately.
	static std::map<uint64_t,TitleSettings> s_titleSettings;
	static uint32_t s_settingsRevision=0; // bumped on every settings change; drives the rebuild check
	static uint32_t s_builtRevision=0xFFFFFFFF;

	// <UserData>/load/textures/<titleId>/ -- per title so different regions/versions of the same
	// game can carry different packs without their content hashes colliding
	static fs::path titleFolderFor(uint64_t titleId){
		char buf[32]; snprintf(buf,sizeof(buf),"%016llx",(unsigned long long)titleId);
		return ActiveSettings::GetUserDataPath("load/textures") / buf;
	}
	static bool isDDSFile(const fs::path& p){
		std::string e=p.extension().string();
		for(auto& c:e) c=(char)tolower(c);
		return e==".dds";
	}

	static bool parseName(const std::string& n,uint64_t& hash,int& w,int& h,uint32_t& fmt,int& mip){
		unsigned long long uHash=0; unsigned uFmt=0; int uW=0,uH=0,uMip=0;
		if (sscanf(n.c_str(),"%llx_%dx%d_fmt%x_mip%d",&uHash,&uW,&uH,&uFmt,&uMip)!=5) return false;
		hash=(uint64_t)uHash; w=uW; h=uH; fmt=uFmt; mip=uMip; return true;
	}
	static void loadPackConfig(const fs::path& base){
		std::ifstream f(base/"pack.json"); if(!f) return;
		std::string s((std::istreambuf_iterator<char>(f)),{});
		auto b=[&](const char* k,bool def){
			auto p=s.find(k); if(p==std::string::npos) return def;
			auto t=s.find("true",p),fa=s.find("false",p),c=s.find(',',p);
			if(t!=std::string::npos&&(c==std::string::npos||t<c)) return true;
			if(fa!=std::string::npos&&(c==std::string::npos||fa<c)) return false;
			return def; };
		s_skipMip=b("skip_mipmap",s_skipMip);
	}
	static void freeAll(){ for(auto&[h,g]:s_index){ for(auto&[m,e]:g.decoded) if(e.data) free(e.data); for(auto&[m,e]:g.rgba8) if(e.data) free(e.data); } s_index.clear(); s_legacyAlias.clear(); s_filters.clear(); }

	// Every pack folder that exists for a title, sorted. Caller must not hold s_mutex expectations:
	// this only touches the filesystem.
	static std::vector<std::string> scanPackFolders(uint64_t titleId){
		std::vector<std::string> out; std::error_code ec;
		fs::path base=titleFolderFor(titleId);
		if(!fs::exists(base,ec)) return out;
		for(auto& e: fs::directory_iterator(base,ec)){
			if(!e.is_directory(ec)) continue;
			std::string name=_pathToUtf8(e.path().filename()); // UTF-8 so non-ASCII pack names survive the config round trip
			if(!name.empty()) out.push_back(name);
		}
		std::sort(out.begin(),out.end());
		return out;
	}

	// ---- textures.ini ----
	//
	// A pack can list its textures in a textures.ini at the pack's top level instead of (or as well as)
	// naming each file after its hash, so the files can be named and arranged however the author
	// likes. The format follows PPSSPP's:
	//
	//   [hashes]
	//   # comments start with '#', anywhere on a line
	//   22fb5c1e481b3338 = menu/background1.dds
	//
	//   [filtering]
	//   22fb5c1e481b3338 = nearest      # or linear, or auto (the game's own filtering)
	//
	// A key is the 16-digit hash at the start of a dumped texture's filename. The whole dump name
	// (22fb5c1e481b3338_1280x720_fmt0031_mip00) works too, and is the only way to give a mip level
	// other than 0; a plain hash means mip 0, whose DDS can carry the whole mip chain. Paths are
	// relative to the pack folder, with '/' or '\'. Unknown sections are ignored.
	//
	// Within a pack, textures.ini is read first, so where it and a hash-named file both provide a
	// texture, the ini wins. Between packs the usual rule applies: the alphabetically first wins.

	// One texture a pack provides: a hash-named file, or a textures.ini [hashes] line
	struct PackEntry { uint64_t hash=0; int mip=0; fs::path path; int nameW=0, nameH=0; uint32_t nameFmt=0; };

	static std::string trimmed(const std::string& s){
		size_t b=0, e=s.size();
		while(b<e && std::isspace((unsigned char)s[b])) b++;
		while(e>b && std::isspace((unsigned char)s[e-1])) e--;
		return s.substr(b,e-b);
	}
	static std::string lowered(std::string s){ for(auto& c:s) c=(char)std::tolower((unsigned char)c); return s; }

	// A plain hash (mip 0), or a whole dump name, which also carries the mip level and the size and
	// format the older-hash fallback checks against.
	static bool parseKey(const std::string& key,uint64_t& hash,int& mip,int& w,int& h,uint32_t& fmt){
		if(parseName(key,hash,w,h,fmt,mip)) return true;
		std::string k=key;
		if(k.size()>2 && k[0]=='0' && (k[1]=='x'||k[1]=='X')) k=k.substr(2);
		if(k.empty()||k.size()>16) return false;
		for(char c:k) if(!std::isxdigit((unsigned char)c)) return false;
		hash=std::stoull(k,nullptr,16); mip=0; w=0; h=0; fmt=0;
		return hash!=0;
	}

	static void readTexturesIni(const fs::path& dir,const std::function<void(const PackEntry&)>& onEntry,
								std::unordered_map<uint64_t,uint8_t>* filters){
		std::error_code ec;
		const fs::path iniPath=dir/"textures.ini";
		if(!fs::is_regular_file(iniPath,ec)) return;
		std::ifstream f(iniPath,std::ios::binary);
		if(!f) return;
		const std::string where=_pathToUtf8(iniPath);
		uint32_t warnings=0;
		// Only the index build (the one call that collects filters) logs. The conflict check reads the
		// same file again every time the Custom textures window refreshes, and would repeat each warning.
		auto warn=[&](int lineNo,const std::string& what){
			if(filters && ++warnings<=16) cemuLog_log(LogType::Force,"[TextureReplace] {} line {}: {}",where,lineNo,what);
		};
		std::string line, section;
		int lineNo=0;
		std::set<std::pair<uint64_t,int>> listed; // a texture listed twice keeps its first file
		while(std::getline(f,line)){
			lineNo++;
			if(lineNo==1 && line.size()>=3 && (uint8_t)line[0]==0xEF && (uint8_t)line[1]==0xBB && (uint8_t)line[2]==0xBF)
				line.erase(0,3); // UTF-8 byte order mark
			if(auto c=line.find('#'); c!=std::string::npos) line.erase(c);
			line=trimmed(line);
			if(line.empty() || line[0]==';') continue;
			if(line[0]=='['){
				const size_t close=line.find(']');
				section=lowered(trimmed(line.substr(1,close==std::string::npos?std::string::npos:close-1)));
				continue;
			}
			const size_t eq=line.find('=');
			if(eq==std::string::npos){ if(section=="hashes"||section=="filtering") warn(lineNo,"expected <hash> = <value>"); continue; }
			const std::string key=trimmed(line.substr(0,eq));
			std::string value=trimmed(line.substr(eq+1));
			if(value.size()>=2 && value.front()=='"' && value.back()=='"') value=value.substr(1,value.size()-2);
			if(section!="hashes" && section!="filtering") continue;
			uint64_t hash; int mip,w,h; uint32_t fmt;
			if(!parseKey(key,hash,mip,w,h,fmt)){ warn(lineNo,"'"+key+"' is not a texture hash"); continue; }
			if(section=="hashes"){
				if(value.empty()) continue; // listed but not replaced
				std::replace(value.begin(),value.end(),'\\','/');
				// The file has to be inside the folder: a path with a drive letter or through ".." works
				// on the author's PC and nowhere else
				const fs::path rel=_utf8ToPath(value);
				bool inside=!rel.is_absolute() && !rel.has_root_name() && !rel.has_root_directory();
				for(const auto& part: rel) if(part==fs::path("..")) inside=false;
				if(!inside){ warn(lineNo,"'"+value+"' is not inside the pack folder"); continue; }
				const fs::path p=dir/rel;
				if(!isDDSFile(p)){ warn(lineNo,"'"+value+"' is not a .dds file"); continue; }
				if(!fs::is_regular_file(p,ec)){ warn(lineNo,"'"+value+"' does not exist"); continue; }
				if(!listed.emplace(hash,mip).second){ warn(lineNo,"'"+key+"' is listed more than once, keeping the first"); continue; }
				onEntry(PackEntry{hash,mip,p,w,h,fmt});
			}
			else if(filters){
				const std::string v=lowered(value);
				uint8_t filter;
				if(v=="nearest"||v=="point") filter=(uint8_t)FilterOverride::Nearest;
				else if(v=="linear"||v=="bilinear") filter=(uint8_t)FilterOverride::Linear;
				else if(v=="auto"||v=="default") filter=(uint8_t)FilterOverride::None;
				else { warn(lineNo,"unknown filter '"+value+"' (use nearest, linear or auto)"); continue; }
				filters->try_emplace(hash,filter); // an earlier pack's setting stands
			}
		}
	}

	// Every texture a folder provides: its textures.ini first, then files named by their hash.
	static void forEachPackEntry(const fs::path& dir,bool recurse,const std::function<void(const PackEntry&)>& onEntry,
								 std::unordered_map<uint64_t,uint8_t>* filters){
		readTexturesIni(dir,onEntry,filters);
		std::error_code ec;
		auto handleFile=[&](const fs::path& p){
			if(!isDDSFile(p)) return;
			PackEntry e; e.path=p;
			if(!parseName(p.filename().string(),e.hash,e.nameW,e.nameH,e.nameFmt,e.mip)) return;
			onEntry(e);
		};
		if(recurse){ for(auto& e: fs::recursive_directory_iterator(dir,ec)) if(e.is_regular_file()) handleFile(e.path()); }
		else       { for(auto& e: fs::directory_iterator(dir,ec))           if(e.is_regular_file()) handleFile(e.path()); }
	}

	// Index one folder. A (hash,mip) already claimed by an earlier pack is left alone: the
	// alphabetically first enabled pack wins, and the user is warned about the overlap in the UI.
	static size_t indexFolder(const fs::path& dir, bool recurse, const std::string& packName, bool reportConflicts){
		size_t added=0;
		forEachPackEntry(dir,recurse,[&](const PackEntry& e){
			if(s_skipMip && e.mip!=0) return;
			HashGroup& grp=s_index[e.hash];
			auto ex=grp.mips.find(e.mip);
			if(ex!=grp.mips.end()){
				if(reportConflicts && ex->second.pack!=packName){
					s_conflictCount++;
					if(s_conflictCount<=8)
						cemuLog_log(LogType::Force,"[TextureReplace] '{}' also replaced by pack '{}' - keeping the one from '{}'",e.path.filename().string(),packName,ex->second.pack);
				}
				return; // first claim wins
			}
			// width/height start from the filename and are replaced by the DDS header on first use;
			// a textures.ini entry has no name to go by, so they start at 0
			grp.mips[e.mip]=FileRef{e.path,e.nameW,e.nameH,0,false,packName,e.nameW,e.nameH,e.nameFmt};
			added++;
		},&s_filters);
		return added;
	}

	// lazy (re)build when the title, the global setting, or the pack selection changes
	static void EnsureInit(){
		bool globalWant = ActiveSettings::LoadCustomTexturesEnabled();
		uint64_t tid = CafeSystem::GetForegroundTitleId();
		if (s_initDone && tid==s_title && globalWant==s_wantEnabled && s_settingsRevision==s_builtRevision) return;
		s_initDone=true; s_title=tid; s_wantEnabled=globalWant; s_builtRevision=s_settingsRevision;
		freeAll(); s_enabled=false; s_skipMip=false; s_conflictCount=0;
		if (!globalWant) return;

		// resolve this title's selection; an unconfigured title gets everything in its folder
		bool titleEnabled=true;
		std::vector<std::string> packs;
		auto ts=s_titleSettings.find(tid);
		if(ts!=s_titleSettings.end()){ titleEnabled=ts->second.enabled; packs=ts->second.packs; }
		else packs=scanPackFolders(tid);
		if(!titleEnabled) return;

		fs::path base=titleFolderFor(tid);
		std::error_code ec; if(!fs::exists(base,ec)) return;
		loadPackConfig(base);
		size_t count=0;
		auto tStart=std::chrono::steady_clock::now();
		// Enabled packs first, alphabetically -- the first pack to claim a texture keeps it.
		std::sort(packs.begin(),packs.end());
		for(const auto& p : packs){
			fs::path packDir = base / _utf8ToPath(p);
			if(!fs::is_directory(packDir,ec)) continue;
			count += indexFolder(packDir,true,p,true);
		}
		// Then any loose files sitting directly in the title folder. These belong to no pack and
		// act as a base layer that an enabled pack can override.
		count += indexFolder(base,false,std::string(),false);
		s_enabled = count>0 || !s_filters.empty();
		auto tMs=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-tStart).count();
		cemuLog_log(LogType::Force,"[TextureReplace] {} files from {} pack(s), {} filter setting(s), skipMip={}, title {:016x}, index built in {} ms",count,packs.size(),s_filters.size(),s_skipMip,tid,tMs);
		if(s_conflictCount>0)
			cemuLog_log(LogType::Force,"[TextureReplace] {} texture(s) claimed by more than one enabled pack",s_conflictCount);
	}

	bool IsEnabled(){ std::scoped_lock lock(s_mutex); EnsureInit(); return s_enabled; }

	bool IsReplaceableUncompressed(Latte::E_GX2SURFFMT format)
	{
		return format == Latte::E_GX2SURFFMT::R8_G8_B8_A8_UNORM
			|| format == Latte::E_GX2SURFFMT::R8_G8_B8_A8_SRGB;
	}

	// Full-data content hash. Every byte of the guest mip0 surface contributes, so distinct
	// textures always get distinct hashes (unlike Cemu's 37-sample texDataHash2, which
	// collides between e.g. monster subspecies built from the same base texture).
	// Must stay bit-identical to strong_hash() in cemu_names.py.
	uint64_t HashData(const uint8* p, uint32_t size){
		uint64_t h = 0;
		const uint32_t nWords = size / 8;
		const uint64_t* w = (const uint64_t*)p;
		for (uint32_t i = 0; i < nWords; i++)
		{
			uint64_t m = (w[i] ^ ((uint64_t)i * 0x9E3779B97F4A7C15ULL)) * 0xFF51AFD7ED558CCDULL;
			m ^= m >> 29;
			h ^= m;
		}
		for (uint32_t i = nWords * 8; i < size; i++)
			h = (h ^ (uint64_t)p[i]) * 0x100000001B3ULL;
		return h;
	}

	// pixelCount and fmt were accepted and discarded, so the lookup key was pure byte content with
	// nothing identifying the surface. Two textures whose bytes agreed over the hashed range mapped
	// to the same pack entry regardless of dimensions or format, and the only downstream check
	// (slice->width == hostW) passes whenever the collision partner happens to be the same size --
	// which for armour and monster variants it usually is. Folding the discriminators in makes
	// cross-dimension and cross-format collisions impossible rather than merely unlikely.
	//
	// HashData's XOR accumulation also leaves the combined value with no avalanche, so near-identical
	// inputs produced near-identical hashes. A splitmix64 finalizer fixes that.
	static uint64_t MixDiscriminators(uint64_t h, uint32_t sizeBytes, uint32_t pixelCount, Latte::E_GX2SURFFMT fmt){
		h ^= (uint64_t)pixelCount        * 0x9E3779B97F4A7C15ULL;
		h ^= ((uint64_t)(uint32_t)fmt + 0x165667B19E3779F9ULL) * 0xC2B2AE3D27D4EB4FULL;
		h ^= (uint64_t)sizeBytes         * 0xD6E8FEB86659FD93ULL;
		h ^= h >> 33; h *= 0xFF51AFD7ED558CCDULL;
		h ^= h >> 33; h *= 0xC4CEB9FE1A85EC53ULL;
		h ^= h >> 33;
		return h ? h : 1; // 0 is the "no hash" sentinel elsewhere
	}

	// Packs made before the discriminators were mixed in are named by the plain content hash, so
	// every one of them stopped matching. Their filenames still record the game texture's size and
	// format, so such a name is accepted for a texture whose pixel count and format agree with it,
	// which is the same protection the new scheme gives. A pack using the new names wins when both
	// have the texture. Dumps and rename_map.csv keep using the new names.
	static bool matchesLegacyName(const HashGroup& grp, uint32_t pixelCount, Latte::E_GX2SURFFMT fmt){
		for(const auto& [mip,ref]: grp.mips)
			if((uint64_t)ref.nameW*(uint64_t)ref.nameH==pixelCount && ref.nameFmt==(uint32_t)fmt) return true;
		return false;
	}

	// The pack entry for a hash, by its current name or the older name recorded for it.
	// Caller holds s_mutex.
	static HashGroup* findGroup(uint64_t hash){
		if(auto it=s_index.find(hash); it!=s_index.end()) return &it->second;
		if(auto a=s_legacyAlias.find(hash); a!=s_legacyAlias.end())
			if(auto it=s_index.find(a->second); it!=s_index.end()) return &it->second;
		return nullptr;
	}

	// [filtering] for a texture, by its current hash or by the older hash an older pack names it by
	// (a full dump name as the key). Caller holds s_mutex.
	static FilterOverride lookupFilter(uint64_t hash){
		if(s_filters.empty() || hash==0) return FilterOverride::None;
		if(auto it=s_filters.find(hash); it!=s_filters.end()) return (FilterOverride)it->second;
		if(auto a=s_legacyAlias.find(hash); a!=s_legacyAlias.end())
			if(auto it=s_filters.find(a->second); it!=s_filters.end()) return (FilterOverride)it->second;
		return FilterOverride::None;
	}

	uint64_t HashGuestAlways(uint32_t physImagePtr, uint32_t sizeBytes, uint32_t pixelCount, Latte::E_GX2SURFFMT fmt,
							 FilterOverride* filterOut){
		if(filterOut) *filterOut=FilterOverride::None;
		const uint8* p=(const uint8*)memory_getPointerFromPhysicalOffset(physImagePtr);
		if(!p||!sizeBytes) return 0;
		const uint64_t raw = HashData(p, sizeBytes);
		const uint64_t current = MixDiscriminators(raw, sizeBytes, pixelCount, fmt);
		if(raw!=0){
			// EnsureInit here too: with dumping on, the loader hashes without asking IsEnabled() first
			std::scoped_lock lock(s_mutex); EnsureInit();
			if(s_enabled && !s_index.count(current)){
				auto it=s_index.find(raw);
				if(it!=s_index.end() && matchesLegacyName(it->second, pixelCount, fmt)) s_legacyAlias[current]=raw;
			}
			// Under the lock already held here, instead of a second locked call per texture load
			if(filterOut && s_enabled) *filterOut=lookupFilter(current);
		}
		return current;
	}

	uint64_t HashGuest(uint32_t physImagePtr, uint32_t sizeBytes, uint32_t pixelCount, Latte::E_GX2SURFFMT fmt){
		if(!s_enabled) { std::scoped_lock lock(s_mutex); EnsureInit(); if(!s_enabled) return 0; }
		return HashGuestAlways(physImagePtr, sizeBytes, pixelCount, fmt);
	}

	// Read the real dimensions/format out of the DDS header. Deliberately NOT done while indexing:
	// probing every file up front meant one cold file open per file (thousands for a full pack),
	// which dominated first-launch time on a cold disk cache. We probe only the files that are
	// actually looked up -- and GetSlice opens the same file moments later anyway.
	static void probeFile(FileRef& ref){
		if(ref.probed) return;
		ref.probed=true; // a header that won't parse is a property of the file, not a transient miss
		uint8_t hdr[148]={0}; std::ifstream fi(ref.path,std::ios::binary); fi.read((char*)hdr,148);
		DDSInfo di=ddsParseHeader(hdr,(size_t)fi.gcount());
		if(di.ok){ ref.gx2Format=di.gx2Format; ref.width=di.width; ref.height=di.height; }
		else cemuLog_log(LogType::Force,"[TextureReplace] unreadable DDS header: {}",ref.path.string());
	}

	bool GetInfo(uint64_t contentHash, ReplacementInfo& out){
		std::scoped_lock lock(s_mutex); EnsureInit();
		if(!s_enabled||contentHash==0) return false;
		HashGroup* grp=findGroup(contentHash); if(!grp) return false;
		auto m0=grp->mips.find(0); if(m0==grp->mips.end()) return false;
		FileRef& ref=m0->second;
		probeFile(ref);
		// no usable size: a textures.ini entry whose DDS header cannot be read (a hash-named file
		// at least falls back to the size in its name)
		if(ref.width<=0 || ref.height<=0) return false;
		out.width=ref.width; out.height=ref.height;
		out.hasFormat=(ref.gx2Format!=0); out.gx2Format=ref.gx2Format;
		return true;
	}

	FilterOverride GetFilter(uint64_t contentHash){
		if(contentHash==0) return FilterOverride::None;
		std::scoped_lock lock(s_mutex); EnsureInit();
		if(!s_enabled) return FilterOverride::None;
		return lookupFilter(contentHash);
	}

	const LatteTextureReplace_Entry* GetSlice(uint64_t contentHash, int mipIndex){
		std::scoped_lock lock(s_mutex); EnsureInit();
		if(!s_enabled) return nullptr;
		if(s_skipMip && mipIndex!=0) return nullptr;
		HashGroup* grp=findGroup(contentHash); if(!grp) return nullptr;
		HashGroup& g=*grp;
		if(auto d=g.decoded.find(mipIndex); d!=g.decoded.end() && d->second.data) return &d->second; // trust only cache hits with real data; never cache misses
		// Prefer the base (mip0) file and pull this level from its internal DDS mip chain, so a
		// single mip00 DDS with a full chain serves every mip level (no per-mip copies needed).
		auto f=g.mips.find(0);
		if(f==g.mips.end()) f=g.mips.find(mipIndex);
		if(f==g.mips.end()){ return nullptr; } // don't cache the miss -> allow retry
		const bool usingBaseFile = (f->first==0); // false = we fell back to a standalone per-mip file
		FileRef& ref = f->second;

		// One file read serves every level. Previously the whole DDS was re-read once per mip
		// level, so a 10-mip texture read the same file 10 times.
		std::ifstream in(ref.path,std::ios::binary);
		std::vector<uint8_t> buf((std::istreambuf_iterator<char>(in)),{});
		if(buf.empty()){ cemuLog_log(LogType::Force,"[TextureReplace] load failed (empty/unreadable): {}",ref.path.string()); return nullptr; } // don't cache failure -> allow retry

		DDSInfo di=ddsParseHeader(buf.data(),buf.size());
		if(di.ok){
			ref.gx2Format=di.gx2Format; ref.width=di.width; ref.height=di.height; ref.probed=true; // free probe
			uint32_t off=di.dataOffset; int w=di.width,h=di.height;
			for(int m=0;m<di.mipCount;m++){
				uint32_t sz=mipByteSize(w,h,di.bytesPerBlock,di.blockCompressed);
				if((size_t)off+(size_t)sz>buf.size()) break; // truncated file -> stop, keep what we have
				// A base (mip00) file supplies level m as our mip m; a standalone per-mip file
				// supplies only its own level, which is the map key we found it under.
				const int key = usingBaseFile ? m : f->first;
				const bool wanted = !(s_skipMip && key!=0);
				auto ex=g.decoded.find(key);
				if(wanted && (ex==g.decoded.end() || !ex->second.data)){
					LatteTextureReplace_Entry ent;
					ent.data=(uint8_t*)malloc(sz);
					if(ent.data){
						memcpy(ent.data,buf.data()+off,sz);
						ent.width=w; ent.height=h; ent.dataSize=sz; ent.gx2Format=di.gx2Format;
						g.decoded[key]=ent;
					}
				}
				if(!usingBaseFile) break; // a per-mip file only ever contributes its own level
				off+=sz; w=std::max(1,w>>1); h=std::max(1,h>>1);
			}
		}
		auto d=g.decoded.find(mipIndex);
		if(d==g.decoded.end() || !d->second.data){
			// rate limited on purpose: this sits on a per-frame path, and an unbounded log here once
			// wrote gigabytes before the display driver gave up
			static uint32_t s_failLog=0;
			if(s_failLog<32){ s_failLog++; cemuLog_log(LogType::Force,"[TextureReplace] load failed: {}",ref.path.string()); }
			return nullptr; // don't cache failure -> allow retry
		}
		return &d->second;
	}

	// ---- texture packs -------------------------------------------------------------------
	// A pack is one folder directly inside load/textures/<titleId>/. Everything below it, however
	// deeply nested, belongs to that pack. Files sitting loose in the title folder belong to no
	// pack and are always loaded, acting as a base layer an enabled pack can override.
	// The first three are called from the UI thread.

	fs::path GetTitleFolder(uint64_t titleId){ return titleFolderFor(titleId); }

	std::vector<std::string> ListPacks(uint64_t titleId){ return scanPackFolders(titleId); }

	// Which textures are provided by more than one of the given packs. Returns one line per
	// clashing texture: "<hash> mip<N>: PackA, PackB". Reads filenames and textures.ini only.
	std::vector<std::string> FindPackConflicts(uint64_t titleId, const std::vector<std::string>& packs){
		std::vector<std::string> out;
		std::error_code ec;
		fs::path base=titleFolderFor(titleId);
		if(!fs::exists(base,ec)) return out;
		std::vector<std::string> sorted=packs;
		std::sort(sorted.begin(),sorted.end());
		sorted.erase(std::unique(sorted.begin(),sorted.end()),sorted.end());
		std::map<std::pair<uint64_t,int>,std::vector<std::string>> owners;
		for(const auto& p : sorted){
			fs::path packDir=base/_utf8ToPath(p);
			if(!fs::is_directory(packDir,ec)) continue;
			forEachPackEntry(packDir,true,[&](const PackEntry& e){
				auto& v=owners[{e.hash,e.mip}];
				if(std::find(v.begin(),v.end(),p)==v.end()) v.push_back(p);
			},nullptr);
		}
		for(const auto& kv : owners){
			if(kv.second.size()<2) continue;
			char buf[32]; snprintf(buf,sizeof(buf),"%016llx",(unsigned long long)kv.first.first);
			std::string line=std::string(buf)+" mip"+std::to_string(kv.first.second)+": ";
			for(size_t i=0;i<kv.second.size();i++){ if(i) line+=", "; line+=kv.second[i]; }
			out.push_back(line);
		}
		return out;
	}

	// ---- CPU-side BCn -> R8G8B8A8 -------------------------------------------------------------
	// The DDS payload is linear (row-major 4x4 blocks), unlike guest textures which are tiled, so
	// the TextureDecoder_* classes cannot be reused directly -- but their per-block helpers can.
	static bool decodeBCnLinear(const LatteTextureReplace_Entry& src, std::vector<uint8_t>& out)
	{
		const uint32_t fmt = src.gx2Format & ~0x400u; // ignore the sRGB bit: the host image carries it
		int bytesPerBlock;
		switch (fmt)
		{
		case 0x031: bytesPerBlock = 8;  break; // BC1
		case 0x032: bytesPerBlock = 16; break; // BC2
		case 0x033: bytesPerBlock = 16; break; // BC3
		case 0x034: bytesPerBlock = 8;  break; // BC4
		case 0x035: bytesPerBlock = 16; break; // BC5
		default: return false;
		}
		const int w = src.width, h = src.height;
		if (w <= 0 || h <= 0 || !src.data)
			return false;
		const int blocksX = std::max(1, (w + 3) / 4);
		const int blocksY = std::max(1, (h + 3) / 4);
		if ((uint64_t)blocksX * blocksY * bytesPerBlock > src.dataSize)
			return false;
		out.assign((size_t)w * h * 4, 0);
		for (int by = 0; by < blocksY; by++)
		{
			for (int bx = 0; bx < blocksX; bx++)
			{
				uint8_t* blockData = src.data + (size_t)(by * blocksX + bx) * bytesPerBlock;
				float rgba[4 * 4 * 4] = {};
				float single[4 * 4 * 1] = {};
				float dual[4 * 4 * 2] = {};
				switch (fmt)
				{
				case 0x031: decodeBC1Block(blockData, rgba); break;
				case 0x032: decodeBC2Block_UNORM(blockData, rgba); break;
				case 0x033: decodeBC3Block_UNORM(blockData, rgba); break;
				case 0x034: decodeBC4Block_UNORM(blockData, single); break;
				case 0x035: decodeBC5Block_UNORM(blockData, dual); break;
				}
				const int blockW = std::min(4, w - bx * 4);
				const int blockH = std::min(4, h - by * 4);
				for (int py = 0; py < blockH; py++)
				{
					for (int px = 0; px < blockW; px++)
					{
						const size_t o = ((size_t)(by * 4 + py) * w + (bx * 4 + px)) * 4;
						float r, g, b, a;
						if (fmt == 0x034)      { r = single[px + py * 4]; g = 0.0f; b = 0.0f; a = 1.0f; }
						else if (fmt == 0x035) { r = dual[(px + py * 4) * 2 + 0]; g = dual[(px + py * 4) * 2 + 1]; b = 0.0f; a = 1.0f; }
						else
						{
							r = rgba[(px + py * 4) * 4 + 0]; g = rgba[(px + py * 4) * 4 + 1];
							b = rgba[(px + py * 4) * 4 + 2]; a = rgba[(px + py * 4) * 4 + 3];
						}
						auto q = [](float v) -> uint8_t { return (uint8_t)std::clamp(v * 255.0f + 0.5f, 0.0f, 255.0f); };
						out[o + 0] = q(r); out[o + 1] = q(g); out[o + 2] = q(b); out[o + 3] = q(a);
					}
				}
			}
		}
		return true;
	}

	const LatteTextureReplace_Entry* GetSliceRGBA8(uint64_t contentHash, int mipIndex)
	{
		// GetSlice takes s_mutex itself, so it must be called before we lock.
		const LatteTextureReplace_Entry* src = GetSlice(contentHash, mipIndex);
		if (!src)
			return nullptr;
		// already uncompressed on disk -> nothing to do
		if ((src->gx2Format & ~0x400u) == 0x01a)
			return src;
		std::scoped_lock lock(s_mutex);
		HashGroup* grp = findGroup(contentHash);
		if (!grp)
			return nullptr;
		HashGroup& g = *grp;
		if (auto d = g.rgba8.find(mipIndex); d != g.rgba8.end() && d->second.data)
			return &d->second;
		std::vector<uint8_t> decoded;
		if (!decodeBCnLinear(*src, decoded))
		{
			cemuLog_log(LogType::Force, "[TextureReplace] cannot decode format 0x{:03x} to RGBA8 (hash {:016x} mip {})", src->gx2Format, contentHash, mipIndex);
			return nullptr;
		}
		LatteTextureReplace_Entry ent;
		ent.data = (uint8_t*)malloc(decoded.size());
		if (!ent.data)
			return nullptr;
		memcpy(ent.data, decoded.data(), decoded.size());
		ent.width = src->width;
		ent.height = src->height;
		ent.dataSize = (uint32_t)decoded.size();
		ent.gx2Format = 0x01a | (src->gx2Format & 0x400u);
		g.rgba8[mipIndex] = ent;
		return &g.rgba8[mipIndex];
	}

	// Texture dump bookkeeping. Written from the GPU thread, read and reset from the UI thread.
	static std::mutex s_dumpMutex;
	static std::unordered_set<std::string> s_dumpSeen;
	static DumpStats s_dumpStats;

	bool DumpShouldWrite(const std::filesystem::path& path){
		std::scoped_lock lock(s_dumpMutex);
		if(!s_dumpSeen.insert(path.filename().string()).second) return false;
		std::error_code ec;
		// Left over from an earlier session: keep it rather than rewriting it on every launch.
		return !std::filesystem::exists(path, ec);
	}

	void DumpRecordResult(const std::filesystem::path& path, bool ok, const std::string& error){
		std::scoped_lock lock(s_dumpMutex);
		if(ok){
			if(s_dumpStats.written == 0)
				cemuLog_log(LogType::Force, "[TextureDump] first texture written: {}", path.string());
			s_dumpStats.written++;
			s_dumpStats.lastFile = path.filename().string();
			return;
		}
		// A failing dump usually fails for every texture, so only the first few are logged.
		if(s_dumpStats.failed < 5)
			cemuLog_log(LogType::Force, "[TextureDump] failed to write {}: {}", path.string(), error);
		s_dumpStats.failed++;
		s_dumpStats.lastError = path.filename().string() + ": " + error;
		// Let it be retried on the next load rather than giving up on it for the session.
		s_dumpSeen.erase(path.filename().string());
	}

	DumpStats GetDumpStats(){ std::scoped_lock lock(s_dumpMutex); return s_dumpStats; }

	void ResetDumpSession(){
		std::scoped_lock lock(s_dumpMutex);
		s_dumpSeen.clear();
		s_dumpStats = DumpStats{};
	}

	void SetTitleSettings(uint64_t titleId, bool enabled, const std::vector<std::string>& packs){
		std::scoped_lock lock(s_mutex);
		TitleSettings ts;
		ts.enabled=enabled;
		ts.packs=packs;
		std::sort(ts.packs.begin(),ts.packs.end());
		ts.packs.erase(std::unique(ts.packs.begin(),ts.packs.end()),ts.packs.end());
		s_titleSettings[titleId]=ts;
		s_settingsRevision++; // EnsureInit rebuilds on the GPU thread when it next runs
	}

	void ClearTitleSettings(uint64_t titleId){
		std::scoped_lock lock(s_mutex);
		s_titleSettings.erase(titleId);
		s_settingsRevision++;
	}
}
