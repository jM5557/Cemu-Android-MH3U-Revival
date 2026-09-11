#include "Cafe/HW/Latte/Core/LatteTextureReplace.h"
#include "config/ActiveSettings.h"
#include "Cafe/CafeSystem.h"
#include "util/helpers/helpers.h"

#include <filesystem>
#include <unordered_map>
#include <map>
#include <mutex>
#include <fstream>
#include <vector>
#include <string>
#include <algorithm>
#include <chrono>

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

	struct FileRef { fs::path path; int width=0, height=0; uint32_t gx2Format=0; bool probed=false; std::string pack; };
	struct HashGroup { std::unordered_map<int,FileRef> mips; std::unordered_map<int,LatteTextureReplace_Entry> decoded; };
	struct TitleSettings { bool enabled=true; std::vector<std::string> packs; };

	static std::mutex s_mutex;
	static std::unordered_map<uint64_t,HashGroup> s_index;
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

	static bool parseName(const std::string& n,uint64_t& hash,int& w,int& h,int& mip){
		unsigned long long uHash=0; unsigned uFmt=0; int uW=0,uH=0,uMip=0;
		if (sscanf(n.c_str(),"%llx_%dx%d_fmt%x_mip%d",&uHash,&uW,&uH,&uFmt,&uMip)!=5) return false;
		hash=(uint64_t)uHash; w=uW; h=uH; mip=uMip; return true;
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
	static void freeAll(){ for(auto&[h,g]:s_index) for(auto&[m,e]:g.decoded) if(e.data) free(e.data); s_index.clear(); }

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

	// Index one folder. A (hash,mip) already claimed by an earlier pack is left alone: the
	// alphabetically first enabled pack wins, and the user is warned about the overlap in the UI.
	static size_t indexFolder(const fs::path& dir, bool recurse, const std::string& packName, bool reportConflicts){
		size_t added=0; std::error_code ec;
		auto handleFile=[&](const fs::path& p){
			if(!isDDSFile(p)) return;
			uint64_t hash; int w,h,mip;
			if(!parseName(p.filename().string(),hash,w,h,mip)) return;
			if(s_skipMip && mip!=0) return;
			HashGroup& grp=s_index[hash];
			auto ex=grp.mips.find(mip);
			if(ex!=grp.mips.end()){
				if(reportConflicts && ex->second.pack!=packName){
					s_conflictCount++;
					if(s_conflictCount<=8)
						cemuLog_log(LogType::Force,"[TextureReplace] '{}' also replaced by pack '{}' - keeping the one from '{}'",p.filename().string(),packName,ex->second.pack);
				}
				return; // first claim wins
			}
			grp.mips[mip]=FileRef{p,w,h,0,false,packName};
			added++;
		};
		if(recurse){ for(auto& e: fs::recursive_directory_iterator(dir,ec)) if(e.is_regular_file()) handleFile(e.path()); }
		else       { for(auto& e: fs::directory_iterator(dir,ec))           if(e.is_regular_file()) handleFile(e.path()); }
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
		s_enabled = count>0;
		auto tMs=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-tStart).count();
		cemuLog_log(LogType::Force,"[TextureReplace] {} files from {} pack(s), skipMip={}, title {:016x}, index built in {} ms",count,packs.size(),s_skipMip,tid,tMs);
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

	uint64_t HashGuest(uint32_t physImagePtr, uint32_t sizeBytes, uint32_t pixelCount, Latte::E_GX2SURFFMT fmt){
		if(!s_enabled) { std::scoped_lock lock(s_mutex); EnsureInit(); if(!s_enabled) return 0; }
		const uint8* p=(const uint8*)memory_getPointerFromPhysicalOffset(physImagePtr);
		if(!p||!sizeBytes) return 0;
		return HashData(p, sizeBytes);
	}

	uint64_t HashGuestRaw(uint32_t physImagePtr, uint32_t sizeBytes){
		const uint8* p=(const uint8*)memory_getPointerFromPhysicalOffset(physImagePtr);
		if(!p||!sizeBytes) return 0;
		return HashData(p, sizeBytes);
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
		auto it=s_index.find(contentHash); if(it==s_index.end()) return false;
		auto m0=it->second.mips.find(0); if(m0==it->second.mips.end()) return false;
		FileRef& ref=m0->second;
		probeFile(ref);
		out.width=ref.width; out.height=ref.height;
		out.hasFormat=(ref.gx2Format!=0); out.gx2Format=ref.gx2Format;
		return true;
	}

	const LatteTextureReplace_Entry* GetSlice(uint64_t contentHash, int mipIndex){
		std::scoped_lock lock(s_mutex); EnsureInit();
		if(!s_enabled) return nullptr;
		if(s_skipMip && mipIndex!=0) return nullptr;
		auto it=s_index.find(contentHash); if(it==s_index.end()) return nullptr;
		HashGroup& g=it->second;
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
	// clashing texture: "<hash> mip<N>: PackA, PackB". Reads filenames only - no files are opened.
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
			for(auto& e: fs::recursive_directory_iterator(packDir,ec)){
				if(!e.is_regular_file()) continue;
				if(!isDDSFile(e.path())) continue;
				uint64_t hash; int w,h,mip;
				if(!parseName(e.path().filename().string(),hash,w,h,mip)) continue;
				auto& v=owners[{hash,mip}];
				if(std::find(v.begin(),v.end(),p)==v.end()) v.push_back(p);
			}
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
