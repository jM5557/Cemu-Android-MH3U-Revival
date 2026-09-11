#pragma once
#include "Cafe/HW/Latte/ISA/LatteReg.h"
#include <cstdint>
#include <string>
#include <vector>
#include <filesystem>

// Runtime custom-texture replacement for Cemu (direct BCn/DDS).
//   Folder:   <UserData>/load/textures/<titleId>/<pack>/...
//             Per title, so different regions or versions of the same game can carry different
//             packs. Subfolders below a pack are walked recursively - organise however you like.
//   Match by: a full-data content hash of the guest texture (NOT Cemu's sparse
//             texDataHash2, which samples only ~296 bytes and collides between
//             different textures -- e.g. monster subspecies sharing a base texture)
//   Filename: <hash16>_<w>x<h>_fmt<XXXX>_mip<NN>.dds
// DDS only (BC1/BC2/BC3/BC4/BC5), uploaded compressed with no intermediate decode. A single
// mip00 file carrying a full internal mip chain serves every mip level.

struct LatteTextureReplace_Entry
{
	uint8_t* data     = nullptr;   // BCn blocks
	int      width    = 0;
	int      height   = 0;
	uint32_t dataSize = 0;
	uint32_t gx2Format = 0;
};

namespace LatteTextureReplace
{
	bool IsEnabled();
	// Uncompressed guest formats a replacement may target. RGBA8 only, which is what the UI sheets
	// use; this keeps the expensive full-surface hash off depth buffers and the single/dual channel
	// render target formats.
	bool IsReplaceableUncompressed(Latte::E_GX2SURFFMT format);
	const LatteTextureReplace_Entry* GetSlice(uint64_t contentHash, int mipIndex);

	// size/format of the replacement, used to size the host texture (no [TextureRedefine] needed)
	struct ReplacementInfo { int width=0, height=0; bool hasFormat=false; uint32_t gx2Format=0; };
	bool GetInfo(uint64_t contentHash, ReplacementInfo& out);
	// full-data hash over the guest mip0 surface; stable and unique per distinct texture
	uint64_t HashGuest(uint32_t physImagePtr, uint32_t sizeBytes, uint32_t pixelCount, Latte::E_GX2SURFFMT fmt);
	uint64_t HashGuestRaw(uint32_t physImagePtr, uint32_t sizeBytes); // always computes (used for dump naming)

	// ---- texture packs (per title) ----
	// A pack is one folder directly inside load/textures/<titleId>/. Loose files in the title
	// folder belong to no pack and always load, acting as a base layer a pack can override. Where
	// two enabled packs provide the same texture, the alphabetically first one wins.
	// A title that has never been configured defaults to "all packs enabled".
	// The first three are safe to call from the UI thread.
	std::filesystem::path GetTitleFolder(uint64_t titleId);
	std::vector<std::string> ListPacks(uint64_t titleId);
	// one line per texture provided by more than one of the given packs
	std::vector<std::string> FindPackConflicts(uint64_t titleId, const std::vector<std::string>& packs);
	void SetTitleSettings(uint64_t titleId, bool enabled, const std::vector<std::string>& packs);
	void ClearTitleSettings(uint64_t titleId);
}
