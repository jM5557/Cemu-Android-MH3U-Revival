#pragma once

#include "Cafe/SaveState/StateFile.h"

namespace SaveStates
{
	struct OperationResult
	{
		bool success = false;
		std::string message;

		static OperationResult Ok() { return OperationResult{true, {}}; }
		static OperationResult Fail(std::string msg) { return OperationResult{false, std::move(msg)}; }
	};

	// Capture the running title to a file.
	//
	// Must be called from a non-scheduler thread. The world is stopped only for the
	// duration of the in-memory capture; compression and disk I/O happen after the
	// emulator has resumed.
	OperationResult SaveToFile(const fs::path& path);

	// Restore a previously captured state into the currently running title.
	//
	// Phase 0 supports load-into-running-title only: the same title must already be
	// running, so the deterministic parts of the host world (module load addresses, HLE
	// call indices, SysAllocator placement) are already built and match the state.
	// The file is fully read, decompressed and fingerprint-checked before the world is
	// stopped, so a rejected state leaves the running game untouched.
	OperationResult LoadFromFile(const fs::path& path);

	// Fingerprint of the currently running world, for UI and for gating loads.
	StateFingerprint BuildCurrentFingerprint();

	// Numbered slots for the running title. Slot 0 is what the plain save/load commands
	// use, so existing muscle memory and the test tooling keep working.
	constexpr uint32 kSlotCount = 10;

	// Empty when no title is running -- states are per title and meaningless without one.
	fs::path GetSlotPath(uint32 slot);

	struct SlotInfo
	{
		uint32 slot = 0;
		bool exists = false;
		StateHead head;
		// Empty when this state can be loaded into the running world, otherwise the reason,
		// phrased for display.
		std::string incompatibility;
	};

	// Describes a slot without inflating it. The container keeps HEAD outside the zstd
	// frame precisely so a picker can list every slot without reading hundreds of
	// megabytes of body per entry.
	SlotInfo QuerySlot(uint32 slot);
} // namespace SaveStates
