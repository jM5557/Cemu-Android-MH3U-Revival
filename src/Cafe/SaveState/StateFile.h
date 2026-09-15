#pragma once

#include "Cafe/SaveState/StateStream.h"

namespace SaveStates
{
	// On-disk container:
	//
	//   [StateFileHeader]  fixed size, never compressed
	//   [HEAD payload]     fingerprint + metadata, deliberately NOT compressed so a slot
	//                      browser can read title/timestamp without inflating a body
	//                      that may be several hundred megabytes
	//   [zstd frame]       every other chunk, as a single stream
	constexpr uint32 kStateFileMagic = 0x43535441; // 'CSTA'
	// Bumped whenever the body layout changes. It is checked before a single byte of guest
	// memory is touched, which is the whole point: a layout change is otherwise only caught
	// at the first mismatching chunk marker, by which time MEMR has already overwritten
	// hundreds of megabytes and there is no way back. Refusing is recoverable; corrupting
	// is not.
	constexpr uint32 kStateFormatVersion = 1;

	struct StateFileHeader
	{
		uint32 magic;
		uint32 formatVersion;
		uint32 flags; // reserved, must be 0
		uint32 headSize;
		uint64 bodySizeUncompressed;
		uint64 bodySizeCompressed;
	};
	static_assert(sizeof(StateFileHeader) == 32);

	// Identifies the HLE world a state was taken in.
	//
	// A RAM snapshot carries HLE call indices, SysAllocator addresses and graphic-pack
	// patched code baked into guest memory. Those are only reproducible in an identical
	// build running an identical title, so loading is gated on all of it. This is
	// stricter than Dolphin's version check, and it is what makes snapshotting raw guest
	// RAM sound in an HLE emulator.
	struct StateFingerprint
	{
		std::string buildVersion;
		uint64 titleId = 0;
		uint16 titleVersion = 0;
		uint8 isMulticoreMode = 0; // scheduler shape differs between 1-core and 3-core
		uint64 graphicPackPatchHash = 0; // phase 1: hash of applied patch bytes

		void DoState(StateStream& s)
		{
			s.DoString(buildVersion);
			s.Do(titleId);
			s.Do(titleVersion);
			s.Do(isMulticoreMode);
			s.Do(graphicPackPatchHash);
		}

		// Returns an empty string when compatible, otherwise a user-facing reason.
		std::string GetIncompatibilityReason(const StateFingerprint& current) const;
	};

	struct StateHead
	{
		StateFingerprint fingerprint;
		uint64 unixTimestamp = 0;

		void DoState(StateStream& s)
		{
			fingerprint.DoState(s);
			s.Do(unixTimestamp);
		}
	};
} // namespace SaveStates
