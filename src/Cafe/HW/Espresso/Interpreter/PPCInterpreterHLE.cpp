#include "../PPCState.h"
#include "PPCInterpreterInternal.h"
#include "PPCInterpreterHelper.h"

std::unordered_set<std::string> s_unsupportedHLECalls;

void PPCInterpreter_handleUnsupportedHLECall(PPCInterpreter_t* hCPU)
{
	const char* libFuncName = (char*)memory_getPointerFromVirtualOffset(hCPU->instructionPointer + 8);
	std::string tempString = fmt::format("Unsupported lib call: {}", libFuncName);
	if (s_unsupportedHLECalls.find(tempString) == s_unsupportedHLECalls.end())
	{
		cemuLog_log(LogType::UnsupportedAPI, "{}", tempString);
		s_unsupportedHLECalls.emplace(tempString);
	}
	hCPU->gpr[3] = 0;
	PPCInterpreter_nextInstruction(hCPU);
}

static constexpr size_t HLE_TABLE_CAPACITY = 0x4000;
HLECALL s_ppcHleTable[HLE_TABLE_CAPACITY]{};
// The name was previously accepted and discarded. Keeping it lets anything that finds an
// HLE opcode -- a save state restoring blocked threads, a crash dump, the debugger -- say
// which call it is looking at instead of reporting a bare table index.
std::string s_ppcHleNames[HLE_TABLE_CAPACITY];
sint32 s_ppcHleTableWriteIndex = 0;
std::mutex s_ppcHleTableMutex;

HLEIDX PPCInterpreter_registerHLECall(HLECALL hleCall, std::string hleName)
{
	std::unique_lock _l(s_ppcHleTableMutex);
	if (s_ppcHleTableWriteIndex >= HLE_TABLE_CAPACITY)
	{
		cemuLog_log(LogType::Force, "HLE table is full");
		cemu_assert(false);
	}
	for (sint32 i = 0; i < s_ppcHleTableWriteIndex; i++)
	{
		if (s_ppcHleTable[i] == hleCall)
		{
			return i;
		}
	}
	cemu_assert(s_ppcHleTableWriteIndex < HLE_TABLE_CAPACITY);
	s_ppcHleTable[s_ppcHleTableWriteIndex] = hleCall;
	s_ppcHleNames[s_ppcHleTableWriteIndex] = std::move(hleName);
	HLEIDX funcIndex = s_ppcHleTableWriteIndex;
	s_ppcHleTableWriteIndex++;
	return funcIndex;
}

HLECALL PPCInterpreter_getHLECall(HLEIDX funcIndex)
{
	if (funcIndex < 0 || funcIndex >= HLE_TABLE_CAPACITY)
		return nullptr;
	return s_ppcHleTable[funcIndex];
}

const char* PPCInterpreter_getHLEName(HLEIDX funcIndex)
{
	if (funcIndex < 0 || funcIndex >= HLE_TABLE_CAPACITY)
		return "<out of range>";
	if (s_ppcHleNames[funcIndex].empty())
		return "<unnamed>";
	return s_ppcHleNames[funcIndex].c_str();
}

std::mutex s_hleLogMutex;

void PPCInterpreter_virtualHLE(PPCInterpreter_t* hCPU, unsigned int opcode)
{
	uint32 hleFuncId = opcode & 0xFFFF;
	if (hleFuncId == 0xFFD0) [[unlikely]]
	{
		s_hleLogMutex.lock();
		PPCInterpreter_handleUnsupportedHLECall(hCPU);
		s_hleLogMutex.unlock();
	}
	else
	{
		// os lib function
		auto hleCall = PPCInterpreter_getHLECall(hleFuncId);
		cemu_assert(hleCall);
		// Recorded before the call so a save state can restart a blocked HLE function with
		// the stack pointer it was entered with, rather than one left shifted by a
		// StackAllocator the restarted call will allocate again.
		hCPU->hleEntryStackPointer = hCPU->gpr[1];
		hleCall(hCPU);
	}
}