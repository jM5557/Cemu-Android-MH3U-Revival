#include "Cafe/OS/libs/gx2/GX2.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/OS/libs/coreinit/coreinit_Alarm.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "Cafe/HW/Latte/Core/LattePerformanceMonitor.h"

#include "Cafe/HW/Espresso/Recompiler/PPCRecompiler.h"
#include "Cafe/CafeSystem.h"

uint32 ppcThreadQuantum = 45000; // execute 45000 instructions before thread reschedule happens, this value can be overwritten by game profiles

void PPCInterpreter_relinquishTimeslice()
{
	PPCInterpreter_t* hCPU = PPCInterpreter_getCurrentInstance();
	if( hCPU->remainingCycles >= 0 )
	{
		hCPU->skippedCycles = hCPU->remainingCycles + 1;
		hCPU->remainingCycles = -1;
	}
}

void PPCCore_boostQuantum(sint32 numCycles)
{
	PPCInterpreter_t* hCPU = PPCInterpreter_getCurrentInstance();
	hCPU->remainingCycles += numCycles;
}

void PPCCore_deboostQuantum(sint32 numCycles)
{
	PPCInterpreter_t* hCPU = PPCInterpreter_getCurrentInstance();
	hCPU->remainingCycles -= numCycles;
}

namespace coreinit
{
	void __OSThreadSwitchToNext();
}

void PPCCore_switchToScheduler()
{
	cemu_assert_debug(__OSHasSchedulerLock() == false); // scheduler lock must not be hold past thread time slice
	cemu_assert_debug(PPCInterpreter_getCurrentInstance()->coreInterruptMask != 0 || CafeSystem::GetForegroundTitleId() == 0x000500001019e600);
	__OSLockScheduler();
	coreinit::__OSThreadSwitchToNext();
	__OSUnlockScheduler();
}

void PPCCore_switchToSchedulerWithLock()
{
	cemu_assert_debug(__OSHasSchedulerLock() == true); // scheduler lock must be hold
	cemu_assert_debug(PPCInterpreter_getCurrentInstance()->coreInterruptMask != 0 || CafeSystem::GetForegroundTitleId() == 0x000500001019e600);
	coreinit::__OSThreadSwitchToNext();
}

void _PPCCore_callbackExit(PPCInterpreter_t* hCPU)
{
	PPCInterpreter_relinquishTimeslice();
	hCPU->instructionPointer = 0;
}

// Address of the guest-callable stub installed below as a guest callback's return address.
// Cached so there is one stable value to compare return addresses against, which is how
// anything else can recognise "this thread is inside a PPC callback".
//
// Save states need exactly that. The instructionPointer == 0 written by _PPCCore_callbackExit
// only means anything to the host loop in PPCCore_executeCallbackInternal, and a state load
// destroys that loop along with the fiber it lives on. A thread with this stub anywhere in
// its guest call chain therefore cannot be restarted, however ordinary its own frame looks.
uint32 PPCCore_getCallbackExitStubAddr()
{
	static uint32 s_callbackExitStub = 0;
	if (s_callbackExitStub == 0)
		s_callbackExitStub = PPCInterpreter_makeCallableExportDepr(_PPCCore_callbackExit);
	return s_callbackExitStub;
}

PPCInterpreter_t* PPCCore_executeCallbackInternal(uint32 functionMPTR)
{
	cemu_assert_debug(functionMPTR != 0);
	PPCInterpreter_t* hCPU = PPCInterpreter_getCurrentInstance();
	// remember LR and instruction pointer
	uint32 lr = hCPU->spr.LR;
	uint32 ip = hCPU->instructionPointer;
	// save area
	hCPU->gpr[1] -= 16 * 4;
	// set LR
	hCPU->spr.LR = PPCCore_getCallbackExitStubAddr();
	// set instruction pointer
	hCPU->instructionPointer = functionMPTR;
	// execute code until we return from the function
	while (true)
	{
		hCPU->remainingCycles = ppcThreadQuantum;
		hCPU->skippedCycles = 0;
		if (hCPU->remainingCycles > 0)
		{
			// try to enter recompiler immediately
			PPCRecompiler_attemptEnter(hCPU, hCPU->instructionPointer);
			// execute any remaining instructions in interpreter
			while ((--hCPU->remainingCycles) >= 0)
			{
				PPCInterpreterSlim_executeInstruction(hCPU);
			};
		}
		if (hCPU->instructionPointer == 0)
		{
			// restore remaining cycles
			hCPU->remainingCycles += hCPU->skippedCycles;
			hCPU->skippedCycles = 0;
			break;
		}
		coreinit::OSYieldThread();
	}
	// save area
	hCPU->gpr[1] += 16 * 4;
	// restore LR and instruction pointer
	hCPU->spr.LR = lr;
	hCPU->instructionPointer = ip;
	return hCPU;
}

void PPCCore_init()
{
}
