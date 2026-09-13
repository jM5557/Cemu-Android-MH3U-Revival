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
} // namespace SaveStates
