#pragma once

#include <atomic>

// Where each emulated core is inside *host* code.
//
// A core that stops dispatching guest threads is invisible to every guest-side probe. The
// thread it is running has no stored context, and its guest instruction pointer stays parked
// at the HLE opcode it entered no matter what the host body does next -- so a host C++ infinite
// loop and a host C++ block look identical from guest memory. When such a core is also burning
// CPU the only open question is which host loop it is in, and nothing in guest state can
// answer it.
//
// HLE bodies on paths that have gone wrong record their progress here, and the save state
// watchdog prints label and sequence number per core. A frozen sequence names the function
// that is stuck; an advancing one means the stall is further out.
namespace SaveStates
{
	inline std::atomic<const char*> g_hostCheckpointLabel[3]{};
	inline std::atomic<uint64> g_hostCheckpointSeq[3]{};

	// Labels must be string literals -- only the pointer is stored. Calls made from a host
	// thread that is not running a PPC core are ignored.
	void MarkHostCheckpoint(const char* label);
} // namespace SaveStates
