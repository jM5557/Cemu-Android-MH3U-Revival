# Save States — audit of HLE call restart safety

**Date:** 2026-09-13
**Branch:** `savestates`
**Scope:** every HLE export in `src/Cafe/OS/libs/` that can block, classified against the
restart-the-HLE-call restore strategy implemented in
`coreinit::__OSRestartHLEBlockedThreadsAfterStateLoad()`
(`src/Cafe/OS/libs/coreinit/coreinit_Thread.cpp:1544`).

Companion documents: `save-states-design.md`, `save-states-phase0-findings.md` (§6 states the
restart strategy and explicitly defers this audit), `save-states-survey.md`.

---

## 1. Summary

I traced every path from a registered export to `OSThreadQueueInternal::queueAndWait()`
(`coreinit_ThreadQueue.cpp:10`), `PPCCore_switchToScheduler[WithLock]()`
(`PPCScheduler.cpp:39`/`:48`) and `__OSSuspendThreadInternal()` (`coreinit_Thread.cpp:860`),
then read each function body. **44 distinct blocking entry points were classified: 19 SAFE,
18 UNSAFE, 7 NEEDS REVIEW.** Counted by registered export rather than by entry point the
balance is much worse, because the unsafe entry points are shared shims: the SAFE set covers
roughly 40 registered exports, the UNSAFE set roughly 360 — every `coreinit.FS*` synchronous
call, every `nn_act` / `nn_acp` / `nn_boss` / `nn_fp` / `nn_save` / `MCP_*` call, and about
70 of GX2's 214 exports. The single biggest UNSAFE class is exactly the one §6.2 predicted,
IPC: `IOS_Ioctl` and friends hand a request to a host IOSU thread *before* they block, so
re-entry re-submits it. Two findings were not predicted and are more damaging than
re-submission. First, `OSWaitCond` (`coreinit_Synchronization.cpp:352`) and `OSFastCond_Wait`
(`:599`) release a guest mutex before blocking and keep the saved lock count only on the host
stack; restarting them makes a thread release a mutex it does not own and cross-link two
intrusive `OSMutexQueue` lists. Second, the restart is not opt-in — `__OSRebuildHostThreadsAfterStateLoad()`
destroys and recreates *every* fiber (`coreinit_Thread.cpp:1711`), so every thread mid-HLE
restarts whether or not the helper touched it; the helper only decides which `STATE_WAITING`
threads become runnable again. That means the helper's `primary opcode == 1` safety check
(`coreinit_Thread.cpp:1581`) gates nothing, and the two blocking mechanisms it skips
(`__OSSuspendThreadInternal`, and threads parked while `STATE_READY`) restart unsupervised.

### Method and its limits

Discovery was a whole-tree call-graph sweep (`cafeExportRegister` / `cafeExportRegisterFunc` /
`osLib_addFunction` call sites, transitively reverse-reachable to the three blocking
primitives). That sweep over-reports badly — it flagged 600 exports, including all of
`coreinit_Atomic.cpp` and most of `snd_core/ax_voice.cpp`, purely through name collisions on
generic identifiers (`lock`, `load`, `connect`, `pop`). **Every classification below comes from
reading the actual function body, not from the graph.** The graph was used only to make sure
nothing was missed. Where I could not settle a question by reading, the row says NEEDS REVIEW
and states precisely what is unresolved.

Two things I did **not** audit and that this report does not cover: whether each subsystem's
*host* state is serialized at all (mostly it is not — see §4.8), and cross-session loads
(loading a state into a fresh process), which fail for far more basic reasons than restart
safety.

---

## 2. The mechanism, restated precisely

Confirmed by reading, because several conclusions below depend on the details:

- `cafeExportCallWrapper` (`src/Cafe/OS/common/OSUtil.h:168`) reads arguments from GPRs at
  entry and only sets `hCPU->instructionPointer = hCPU->spr.LR` *after* the C++ function
  returns (`OSUtil.h:207`). So while an export is executing, `instructionPointer` is still the
  HLE opcode. `__OSThreadStoreContext()` copies it to `thread->context.srr0`
  (`coreinit_Thread.cpp:1139`). The phase-0 measurement is correct: a blocked thread's entry
  context is in guest memory for free.
- `__OSRebuildHostThreadsAfterStateLoad()` (`coreinit_Thread.cpp:1711`) deletes every
  `OSHostThread` and calls `__OSCreateHostThread()` for every active thread, which constructs
  a fresh fiber entered at `__OSFiberThreadEntry` (`coreinit_Thread.cpp:1368`). That entry
  point does `__OSLoadThread()` and executes from `srr0`. **This is unconditional.** A thread
  that was `STATE_READY` mid-HLE (yielded, not waited) restarts its HLE call too, and nothing
  validated its `srr0`.
- Arguments survive only if the export did not clobber GPRs before blocking. `StackAllocator`
  (`src/Common/StackAllocator.h:15`) allocates on the **guest** stack via
  `PPCInterpreter_PushAndReturnStackPointer()`, i.e. it decrements `gpr[1]`, and its destructor
  (`StackAllocator.h:21`) restores it. A restart abandons that destructor, so every restart of
  an export using `StackAllocator` permanently leaks that many bytes of guest stack.

There are three distinct ways to block, and they behave differently under restore:

| Mechanism | Thread state at capture | Helper sees it? | Restarts? |
|---|---|---|---|
| `queueAndWait()` on a guest queue | `STATE_WAITING`, `currentWaitQueue` set | yes | yes, correctly unlinked |
| `queueOnly()` + `switchToSchedulerWithLock()` (OSFastMutex) | `STATE_WAITING`, `currentWaitQueue` set | yes | yes |
| bare `switchToScheduler[WithLock]()` (yield / reschedule-on-wake) | `STATE_READY` | **no** | yes, unsupervised |
| `__OSSuspendThreadInternal(self)` | `STATE_READY`, `suspendCounter > 0` | counted as "skipped", left alone | fiber restarts, but thread never runs until a **host** thread resumes it |

The last row is the legacy IOSU path (`iosuIoctl_pushAndWait`, `iosu_ioctl.cpp:15`) and
`PPCConcurrentQueue::pop` (`src/Cafe/OS/common/PPCConcurrentQueue.h:39`). Both are UNSAFE and
neither is currently handled.

---

## 3. Classification table

`File:line` is the definition site of the blocking entry point; "Blocks on" names the
innermost wait actually reached.

### coreinit — synchronization

| Export | File:line | Blocks on | Class | Reasoning |
|---|---|---|---|---|
| `OSWaitEvent` | `coreinit_Synchronization.cpp:51` | `queueAndWait` on `event->threadQueue` | **SAFE** | Nothing happens before the wait; auto-reset clear only on the non-blocking path (`:40`). |
| `OSWaitEventWithTimeout` | `coreinit_Synchronization.cpp:76` | `queueAndWait` + host alarm | **SAFE** | Creates a host alarm with a *host-stack* context (`:98`), but `AlarmDoState` deliberately tears every host alarm down on load (`coreinit_Alarm.cpp:341-349`), so no dangling callback. Restart re-arms a fresh alarm: the timeout restarts from full duration and one `OSHostAlarm` object leaks per load. |
| `OSWaitRendezvous` | `coreinit_Synchronization.cpp:188` | `OSWaitEventInternal` | **SAFE** | `coreHit[OSGetCoreId()] = 1` (`:190`) is guest memory and idempotent; re-signalling `g_rendezvousEvent` is harmless. |
| `OSLockMutex` | `coreinit_Synchronization.cpp:264` | `queueAndWait` on `mutex->threadQueue` | **SAFE** | Only `currentThread->waitingForMutex` is set before blocking (`:251`), guest memory, and the helper clears it (`coreinit_Thread.cpp:1601`). `failedAttempts` is a host-local log counter. |
| `OSWaitSemaphore` | `coreinit_Synchronization.cpp:407` | `queueAndWait` | **SAFE** | The count is decremented *after* waking (`:398`), never before blocking. |
| `OSFastMutex_Lock` | `coreinit_Synchronization.cpp:520` | `queueOnly` + `switchToSchedulerWithLock` | **SAFE** | The host `g_fastMutexSpinlock` is released before the switch (`:509`), so it is not stranded. Minor: the helper does not clear `waitingForFastMutex` the way it clears `waitingForMutex`. |
| `OSWaitCond` | `coreinit_Synchronization.cpp:352` | `queueAndWait` on `cond->threadQueue` | **UNSAFE** | Releases the mutex before blocking (`:361-365`) and keeps `prevLockCount` **only on the host stack** (`:358`). Restart re-enters with `mutex->owner != currentThread`, sets `owner = nullptr` and calls `mutexQueue.removeMutex()` on a mutex owned by another thread — which unlinks it using the mutex's own prev/next while writing *this* thread's queue head/tail (`coreinit_Thread.h:246-260`), cross-linking two intrusive lists. The original lock count is lost. |
| `OSFastCond_Wait` | `coreinit_Synchronization.cpp:599` | `queueAndWait` on `fastCond->threadQueue` | **UNSAFE** | Same shape: `fastMutex->owner = nullptr` and `lockCount = 0` before the wait (`:604-605`), `prevLockCount` on the host stack (`:603`). |
| `OSSignalEvent`, `OSSignalEventAll` | `coreinit_Synchronization.cpp:139`, `:168` | reschedule inside `wakeupSingleThreadWaitQueue` / `wakeupEntireWaitQueue` (`coreinit_ThreadQueue.cpp:148`, `:167`) | **UNSAFE** | The wake already happened when the signaller is descheduled. Restart re-runs `OSSignalEventInternal`: with waiters still queued it wakes a *second* thread, with an empty queue it sets `STATE_SIGNALED` — an extra signal either way. |
| `OSSignalSemaphore` | `coreinit_Synchronization.cpp:436` | same | **UNSAFE** | `semaphore->count` is incremented at `:429` before `wakeupEntireWaitQueue(true)` can reschedule. Restart increments again: permanent count inflation. |
| `OSUnlockMutex` | `coreinit_Synchronization.cpp:324` | same | **UNSAFE** | `lockCount` decremented and `owner` cleared (`:310-315`) before `wakeupSingleThreadWaitQueue(true, true)`. Restart decrements a lock this thread no longer owns. |

### coreinit — threads, messages, queues

| Export | File:line | Blocks on | Class | Reasoning |
|---|---|---|---|---|
| `OSSleepThread` | `coreinit_Thread.cpp:895` | `queueAndWait` on the caller's queue | **SAFE** | One statement, no prior work. |
| `OSSleepTicks` | `coreinit_Thread.cpp:694` | `queueAndWait` on a private queue + host alarm | **SAFE** | The queue is `StackAllocator<OSThreadQueue>`, i.e. **guest** stack, so it is in `MEMR`; the host alarm is dropped by `AlarmDoState`. Caveats: the sleep restarts from its full duration (`ticks` is re-read from GPRs), and the `StackAllocator` destructor never runs, leaking ~152 bytes of guest stack per sleeping thread per load. |
| `OSYieldThread` | `coreinit_Thread.cpp:682` | `PPCCore_switchToScheduler` | **SAFE** | Pure reschedule. |
| `OSJoinThread` | `coreinit_Thread.cpp:728` | `queueAndWait` on `thread->joinQueue` (`:736`) | **SAFE** | Only condition checks precede the wait. (The *second* block at `:760` is a different case — see NEEDS REVIEW below.) |
| `OSTestThreadCancel` | `coreinit_Thread.cpp:1068` | `OSExitThread` | **SAFE** | Re-reads `requestFlags` from guest memory. |
| `OSSuspendThread` (self) | `coreinit_Thread.cpp:883` | `switchToSchedulerWithLock` (`:866`) | **SAFE** | `suspendCounter` is guest memory; restart re-increments it, so a self-suspend that a save/load straddles ends up needing an extra `OSResumeThread`. Benign in isolation, damaging when the resumer is a host thread (see `PPCConcurrentQueue`). |
| `OSReceiveMessage` (blocking wait path) | `coreinit_MessageQueue.cpp:30` | `queueAndWait` on `threadQueueReceive` (`:40`) | **SAFE** | The message is copied and `firstIndex`/`usedCount` advanced only after waking (`:48-52`). |
| `OSSendMessage` (blocking wait path) | `coreinit_MessageQueue.cpp:81` | `queueAndWait` on `threadQueueSend` (`:89`) | **SAFE** | Same: the message is written only after the wait returns. |
| `OSReceiveMessage` (reschedule path) | `coreinit_MessageQueue.cpp:55` | reschedule inside `wakeupSingleThreadWaitQueue(true)` | **UNSAFE** | The message was already consumed (`:48-52`) when the thread is descheduled at `:55`. Restart consumes a **second** message and the first is lost. |
| `OSSendMessage` (reschedule path) | `coreinit_MessageQueue.cpp:115` | same | **UNSAFE** | The message was already written into the ring and `usedCount` incremented. Restart appends a duplicate. |
| `OSWakeupThread`, `OSResumeThread`, `OSContinueThread`, `OSSetThreadPriority` | `coreinit_Thread.cpp:902`, `:845`, `:855`, `:982` | reschedule after mutating guest state | **UNSAFE** | Each mutates before the reschedule can park it (`__OSResumeThreadInternal` decrements `suspendCounter` at `coreinit_Thread.cpp:829` then may switch at `:839`; `OSSetThreadPriority` updates priority at `:988` then may switch at `:993`). Restart re-applies the mutation. |
| `MPWaitTaskQ`, `MPWaitTaskQWithTimeout`, `MPTermTaskQ` | `coreinit_MPQueue.cpp:316`, `:356`, `:186` | `OSSleepTicks` / yield | **SAFE** | Read-only polling loops; `MPTermTaskQ`'s `state = STOP` write (`:193`) is idempotent. The timeout deadline is recomputed on restart, so the timeout restarts. |
| `MPRunTasksFromTaskQ` | `coreinit_MPQueue.cpp:233` | `PPCCore_switchToScheduler` (`:260`) | **UNSAFE** | **Claims a batch of tasks before yielding**: `nextIndex` advanced, `taskReadyCount -= workCount`, `taskRunCount += workCount` at `:253-255`, released at `:256`, then yields at `:260` — before the task loop at `:262` and before the compensating `taskRunCount -= workCount` at `:281`. Restart claims a *different* batch; the first batch never runs and `taskRunCount` stays permanently inflated, so `MPTermTaskQ`'s `while (taskRunCount != 0)` (`:249`) never terminates. |
| `IPCBufPoolAllocate`, `IPCBufPoolFree` | `coreinit_IPCBuf.cpp:116`, `:131` | `OSLockMutex` | **SAFE** | The FIFO pop/push happens under the lock, after it is acquired. |
| `__ghs_flock_file`, `__ghs_mtx_lock` | `coreinit_GHS.cpp:117`, `:204` | `OSLockMutex` | **SAFE** | Guest mutex only. |
| `OSUninterruptibleSpinLock_Acquire` and the three other spinlock acquires | `coreinit_Spinlock.cpp:118`, `:40`, `:80`, `:172` | `OSYieldThread` (single-core) / `_mm_pause` busy spin (multicore) | **SAFE** (restart), **capture hazard** | The lock is guest memory and acquisition is a CAS, so restart is fine. But in multicore mode the loop at `coreinit_Spinlock.cpp:130-133` never yields, so a core spinning there **never reaches the quiesce barrier** and `QuiesceScope` times out (`Quiesce.cpp:190`). |

### coreinit — IPC and filesystem

| Export | File:line | Blocks on | Class | Reasoning |
|---|---|---|---|---|
| `IOS_Open`, `IOS_Close`, `IOS_Ioctl`, `IOS_Ioctlv` | `coreinit_IPC.cpp:367`, `:377`, `:386`, `:417` | `OSWaitEvent` on `descriptor->eventSynchronousIPC` (`:261`) | **UNSAFE** | Two independent violations before the wait. (1) `IPCDriver_AllocateResource` (`:136`) **pops** a descriptor from the guest `fifoFreeBuffers` FIFO and sets `IsAllocated = 1`; restart pops another and the first is never released (only 48 per core, then `cemu_assert_unimplemented()` at `:147`). (2) `_IPCDriver_SubmitCmd` → `_IPCDriver_SubmitCmdAllQueued` (`:215`) calls `iosu::kernel::IPCSubmitFromCOS()` (`:246`), which allocates a **host** `IOSDispatchableCommand` and dispatches to a host IOSU service thread (`iosu_kernel.cpp:673-712`). Restart re-submits the request verbatim. |
| `IOS_IoctlAsync`, `IOS_IoctlvAsync` | `coreinit_IPC.cpp:402`, `:433` | — (does not block) | **UNSAFE to capture across** | Non-blocking, but leaves a request in flight on a host thread that the quiesce does not park. Its reply lands via `IPCDriver_HandleResponse` (`:266`) → `OSSendMessage`/`OSSignalEvent` — a host thread mutating the guest scheduler and guest memory while `MEMR` is rewriting it. |
| `__depr__IOS_Ioctlv` (backs all `MCP_*`, `nn_act.*`, `nn_acp.*`, `nn_nim.*`) | `coreinit_IOS.cpp:67` → `iosuIoctl_pushAndWait`, `iosu_ioctl.cpp:15` | `__OSSuspendThreadInternal` (`iosu_ioctl.cpp:31`) | **UNSAFE, and unhandled** | Pushes the request onto a **host** ring buffer and increments a host semaphore (`:28-29`) before self-suspending. The thread is `STATE_READY` + `suspendCounter > 0`, so the restart helper skips it (`coreinit_Thread.cpp:1594`) and nothing makes it runnable. Completion comes from `iosuIoctl_completeRequest` (`iosu_ioctl.cpp:56`) on a host thread, which writes `returnValue`/`isCompleted` into `ioQueueEntry` — a `StackAllocator` object on the **guest stack** (`coreinit_IOS.cpp:69`) whose address no longer refers to that frame — and then `OSResumeThread` on a guest thread pointer captured pre-load. |
| `FSOpenFile`, `FSReadFile`, `FSWriteFile`, `FSCloseFile`, `FSRemove`, `FSRename`, `FSMakeDir`, `FSOpenDir`, `FSReadDir`, `FSCloseDir`, `FSGetStat`, `FSGetStatFile`, `FSSetPosFile`, `FSGetPosFile`, `FSTruncateFile`, `FSAppendFile`, `FSFlushFile`, `FSFlushQuota`, `FSChangeDir`, `FSGetCwd`, `FSIsEof`, `FSRewindDir`, `FSGetFreeSpaceSize`, `FSOpenFileEx`, `FSReadFileWithPos`, `FSWriteFileWithPos` (26 exports) | `coreinit_FS.cpp:841` (`__FSProcessAsyncResult`), reached from each sync wrapper e.g. `FSOpenFile` at `:920` | `OSReceiveMessage` on `fsCmdBlockBody->syncTaskMsgQueue` (`:848`) | **UNSAFE** | Before blocking: the command block is linked into the client queue (`__FSQueueCmd`, `:600`), then `__FSUpdateQueue` (`:494`) dequeues it, increments `numCommandsInFlight` (`:499`) and calls `__FSAIPCSubmitCommandAsync` (`:456`) → `IOS_IoctlAsync`. Restart re-queues and re-submits. Two consequences: the FSA operation runs twice (a duplicated `WRITE`/`APPENDFILE` corrupts the file), and `numCommandsInFlight` gains a permanent +1 that only `__FSAIoctlResponseCallback` (`:737`) could ever remove. **`numMaxCommandsInFlight` is 1** (`FSAddClientEx`, `coreinit_FS.cpp:303`), so *one* save-state load taken while any synchronous FS call is in flight permanently wedges that `FSClient`: `__FSUpdateQueue`'s `numCommandsInFlight < numMaxCommandsInFlight` test (`:497`) is false forever. |
| `OSLaunchTitle`, `OSLaunchTitleByPathl`, `coreinit.exit` | `coreinit_Misc.cpp:~700`, `:748`, `:869` | `OSSuspendThread(self)` after `std::thread(...).detach()` | **UNSAFE** | A detached host relaunch/shutdown thread is already running. Capturing here is meaningless; restarting spawns a second one. |
| `OSFatal` | `coreinit_Misc.cpp:632` | `OSSleepTicks` in an infinite loop | **SAFE** | Restart re-enters the same infinite error loop. Correct, if academic. |

### GX2 and TCL

| Export | File:line | Blocks on | Class | Reasoning |
|---|---|---|---|---|
| `GX2WaitForVsync` | `GX2_Event.cpp:201` | `queueAndWait` on `g_vsyncThreadQueue` | **SAFE** | One statement. The wake (`__GX2NotifyEvent(VSYNC)`, `:176`) is driven by host vsync unconditionally, so it keeps arriving after a load. |
| `GX2WaitForFlip` | `GX2_Event.cpp:208` | `queueAndWait` on `g_flipThreadQueue` | **SAFE** | Reads the guest flip counters and returns early if equal (`:210`); nothing consumed. `LatteGPUState.flipRequestCount` — the host half — *is* serialized (`LatteThread.cpp:40-56`), and `__GX2NotifyEvent(FLIP)` fires every `swapInterval` vsyncs regardless (`LatteTiming.cpp:120`), so the queue is still pumped. |
| `GX2WaitTimeStamp` | `GX2_Command.cpp:318` → `TCLWaitTimestamp` | `OSWaitEvent(s_updateRetirementEvent)` (`TCL.cpp:57`) | **SAFE** | Pure compare-and-wait against `s_tclStatePPC->gpuRetireMarker`, guest memory. `s_currentRetireMarker` is serialized (`TCL.cpp:104`) and the quiesce drains the ring, so the comparison is satisfied on restore. |
| `TCLWaitTimestamp` | `TCL.cpp:47` | as above | **SAFE** | Same reasoning. |
| `TCLSubmitToRing` | `TCL.cpp:174` | `TCLWaitForRBSpace` → `OSSleepTicks` (`TCL.cpp:130`) | **SAFE** | The space wait (`:171`) strictly precedes `TCLWriteCmd` (`:174`), so nothing is half-written when it blocks. |
| `GX2DrawDone` (and `GX2Flush`, `GX2ResetGPU`, `GX2Shutdown`, which call it at `GX2_Command.cpp:300`, `GX2_Misc.cpp:199`, `:176`) | `GX2_Event.cpp:217` | `GX2WaitTimeStamp` | **UNSAFE** | Before waiting it emits an `IT_HLE_SYNC_ASYNC_OPERATIONS` packet (`:226-228`) and calls `GX2Command_Flush(0x100, true)` (`:231`), which submits the accumulated command buffer to the TCL ring. Restart re-emits the sync packet and re-flushes. |
| `GX2SwapScanBuffers` | `GX2.cpp:48` | `GX2WaitForFlip` in a loop (`:84-87`) | **UNSAFE** | Blocks *after* incrementing `LatteGPUState.sharedArea->flipRequestCountBE` (`:71`) and writing four PM4 words including `IT_HLE_REQUEST_SWAP_BUFFERS` and `IT_HLE_TRIGGER_SCANBUFFER_SWAP` (`:73-80`). Restart adds a second phantom flip request and a second swap trigger. The guest's request counter then drifts +1 per load, and the `> 5` guard at `:84` makes the title wait for flips nobody asked for. |
| ~70 PM4-emitting GX2 exports (`GX2SetAlphaTestReg`, `GX2SetContextState`, the `GX2Draw*` family, `GX2Init*Reg` setters, `GX2CopySurface`, …) | 73 `GX2ReserveCmdSpace` call sites across `gx2/*.cpp`; entry at `GX2_Command.cpp:329` | `GX2Command_Flush` → `GX2Command_StartNewCommandBuffer` → `GX2Command_WaitForNextBufferRetired` (`GX2_Command.cpp:209`) | **UNSAFE** | The reserve is always the first statement, so *this* export's own writes are not torn. The problem is one level up: `GX2Command_Flush` (`:268`) has already called `GX2Command_SubmitCommandBuffer` (`:281`) — pushing the previous buffer into the TCL ring — and then blocks inside `GX2Command_StartNewCommandBuffer` *before* `GX2Command_SetupCoreCommandBuffer` runs (`:215`). So `s_perCoreCBState[core]` still describes the just-submitted buffer with `currentWritePtr` at its end. On restart `GX2ReserveCmdSpace` sees the same over-full write distance, flushes again, and **submits the identical command-buffer range to the GPU a second time**. Compounding this, `s_perCoreCBState` is a plain host array (`GX2_Command.cpp:17`) holding raw pointers into the guest command pool and is not serialized at all. |

### snd_core, nn_*, and the rest

| Export | File:line | Blocks on | Class | Reasoning |
|---|---|---|---|---|
| `AXRegisterAppFrameCallback`, `AXDeregisterAppFrameCallback` | `ax_ist.cpp:106`, `:124` | `OSLockMutex` | **SAFE** | The lock is the first statement after a null check; the table scan is under it. (The rest of the AX voice API was flagged by the call graph and is a false positive — `AXIst_IsFrameBeingProcessed` at `ax_ist.cpp:1069` just reads an atomic.) |
| `SwkbdCreate`, `SwkbdCalcSubThreadFont`, `SwkbdCalcSubThreadPredict` | `swkbd.cpp:92`, `:378`, `:386` | `OSSleepTicks` | **SAFE** | Simulated work delays. `SwkbdCreate` guards its allocation on `swkbdInternalState != nullptr` so restart does not re-allocate; the two `Calc` functions decrement a host counter before sleeping, which restart repeats, but the counter only simulates work. |
| `nn_nfp.GetNfpState` | `nn_nfp.cpp:907` | `OSSleepTicks` (title-specific workaround, `:915`) | **SAFE** | The sleep precedes all state reads. |
| `vpad.VPADRead` | `vpad.cpp:~200` | `PPCCore_switchToScheduler` (`:263`, ELF-homebrew path only) | **SAFE** | The yield is before the sample read; a restart just re-samples current input. |
| `ErrEulaCreate` and the other mutex-guarded erreula entry points | `erreula.cpp:248` | `OSLockMutex` | **SAFE** | Lock first, work under it. |
| `nn_save.SAVEOpenFile`, `SAVEOpenDir`, `SAVEMakeDir`, `SAVERemove`, `SAVERename`, `SAVEChangeDir`, `SAVEGetStat`, `SAVEFlushQuota`, `SAVEGetFreeSpaceSize`, `SAVEGetStatOtherApplication`, `SAVEOpenDirOtherApplication`, `SAVEOpenFileOtherApplication` | `nn_save.cpp:309` and the 11 siblings; wait at `:199` | `OSWaitEvent` on a guest-stack `OSEvent` | **UNSAFE** | Each submits an async FS command (which itself reaches `IOS_IoctlAsync`) and then waits on `StackAllocator<AsyncToSyncWrapper>` (`:311`) — guest stack. Restart re-submits the whole save-data operation *and* allocates a new wrapper at a different guest-stack address, so the in-flight completion signals an `OSEvent` nobody is waiting on. Inherits the FS `numCommandsInFlight` wedge above. |
| `nn_fp.*` (44 exports) | `nn_fp.cpp:118` (`FPIpcContext::Submit`) | `IOS_Ioctlv` | **UNSAFE** | Inherits every `IOS_Ioctlv` problem. Additionally `Submit` owns a `std::unique_ptr<FPIpcContext>` released at `:121`, so a restart leaks a host allocation per load. |
| `nn_boss.*` (80 exports) | `nn_client_service.h:72` (`IPCServiceCall::Submit`) | `IOS_Ioctlv` (`:90`) | **UNSAFE** | Inherits `IOS_Ioctlv`, plus a host free-list leak: `IPCServiceCall`'s constructor pops buffers from `IPCServiceClient::m_commandBuffersFree` (`nn_client_service.h:259`) and only its **destructor** returns them (`:34-47`). A restart abandons that destructor, so each interrupted call permanently leaks 2–4 `IPCBuffer`s from a fixed pool guarded by `cemu_assert(m_commandBuffersFree.size() > 0)` (`:242`). |
| `coreinit.MCP_*` (22), `nn_act.*` (44), `nn_acp.*` (18), `nn_nim.*` (13) | `coreinit_MCP.cpp:96` etc., `nn_act.cpp:40`, `nn_acp.cpp:104`, `nn_nim.cpp` | `__depr__IOS_Ioctlv` | **UNSAFE, and unhandled** | See the `__depr__IOS_Ioctlv` row. These are the worst-behaved of the IPC family because the suspend mechanism leaves the thread invisible to the restart helper. |
| `nlibcurl.curl_easy_perform`, `curl_easy_pause` | `nlibcurl.cpp:1343`, `:890` | `PPCConcurrentQueue::pop` → `__OSSuspendThreadInternal` (`PPCConcurrentQueue.h:53`) | **UNSAFE (use-after-free)** | `SendOrderToWorker` (`:314`) creates `callerQueue` and `threadQueue` as **host stack locals** (`:320-321`), spawns a **detached** `std::thread` holding pointers to them (`:323-324`), and then blocks. Destroying the fiber frees those objects while the detached worker is still running `curl_easy_perform`; when it finishes it does `callerQueue->push(...)` (`:311`) into freed memory. Restart also spawns a second worker issuing the same HTTP request. |
| `nsyshid.HIDSetReport`, `HIDRead`, `HIDWrite` | `nsyshid.cpp:~570`, `~690`, `~790`; waits at `:610`, `:715`, `:812` | `OSWaitEvent` on a guest-stack `OSEvent` | **UNSAFE** | Each launches `std::async` doing real USB I/O and passes `&event` where `event` is `StackAllocator<coreinit::OSEvent>` (`:608`, `:712`). Restart re-issues the transfer — for `HIDWrite`/`HIDSetReport` that means physically writing the same report to the device twice — and abandons the `std::future`. |
| `nn_idbe.DownloadIconFile` | `nn_idbe.cpp:59` | `OSWaitEvent` on a guest-stack `OSEvent` (`:70`) | **UNSAFE** | `std::async` host download (`:69`) writing into `encryptedIconData` and signalling a guest-stack `OSEvent`. Restart starts a second download. |
| `nn_olv` network entry points (`Initialize` discovery, `DownloadCommunityData*`, `UploadFavorite*`, `UploadCommunity*`, OfflineDB) | `nn_olv_InitializeTypes.cpp:202`, `nn_olv_DownloadCommunityTypes.cpp:53`, `nn_olv_UploadCommunityTypes.cpp:59`, `nn_olv_UploadFavoriteTypes.cpp:50`, `nn_olv_OfflineDB.cpp:180`, `:209` | `OSWaitEvent` on a guest-stack `OSEvent` | **UNSAFE** | Same `std::async` + HTTP shape. Restart re-issues the request; for the upload paths that means a duplicate POST. |
| `H264DECExecute` | `H264Dec.cpp:~620` | `OSWaitEvent(session->GetFrameOutputEvent())` (`:673`) | **UNSAFE** | `session->QueueForDecode(...)` (`:662`) has already handed the bitstream to the host decoder backend, and `numFramesInFlight` was incremented at `:663` then decremented at `:668`. Restart re-queues the same bitstream, so the decoder emits an extra frame and the output queue runs permanently one ahead. |
| `H264DECEnd` | `H264Dec.cpp:449` | `OSWaitEvent(flushEvt)` (`:461`) | **UNSAFE** | `session->QueueFlush()` (`:460`) is a host-side flush; restart flushes twice. |
| `proc_ui.ProcUIProcessMessages` | `proc_ui.cpp:~655` | `OSReceiveMessage(..., OS_MESSAGE_BLOCK)` (`:704`), `OSWaitEvent` (`:710`), `OSJoinThread` (`:712`) | **UNSAFE** | Reachable only after the non-blocking `OSReceiveMessage(..., 0)` at `:691` has already **consumed and processed** a system message through `ProcessSysMessage`, which mutates unserialized host statics (`s_isInForeground`, `s_currentProcUIStatus`). Restart loses that message. The background-thread creation at `:672-681` would also re-run `__OSCreateThreadType` on an already-active `OSThread_t`. Bounded in practice: this path is only entered on foreground/background transitions. |

---

## 4. Unsafe calls and what they would need

### 4.1 IPC / IOSU (the largest class)

Covers `IOS_Open`/`Close`/`Ioctl`/`Ioctlv`/`*Async` and everything layered on them:
`coreinit.FS*`, `nn_save`, `nn_fp`, `nn_boss`, plus the legacy `__depr__IOS_Ioctlv` users
`MCP_*`, `nn_act`, `nn_acp`, `nn_nim`.

The defining property is that the request crosses into host code before the block:
`IPCSubmitFromCOS` (`iosu_kernel.cpp:673`) copies the command into a host
`IOSDispatchableCommand` and dispatches it to a host service thread, and those threads
(`iosu::fsa::FSAIoThread`, `iosu_fsa.cpp:833`; the `IPCService`/`IPCSimpleService` threads,
`iosu_nn_service.cpp:18`, `:143`; the legacy ACT/ACP/MCP/NIM loops) **are not parked by the
quiesce** — `QuiesceScope` parks the PPC cores and the Latte thread only (`Quiesce.cpp:145-209`).

A correct restore needs all three of:

1. **Drain before capture.** Extend the quiesce to wait until every IPC driver's
   `fifoBuffersInFlight` is empty and `iosu::kernel`'s dispatchable pool has no allocated
   entries, with a timeout that aborts the *save* rather than proceeding. This is the only
   option that makes restart sound, because it guarantees no thread is ever captured between
   submit and reply. It needs a "no new submissions" gate too, since a parked core can still
   have an `IOS_IoctlAsync` outstanding that a *guest* callback thread would answer.
2. **Park the IOSU host threads** for the duration of the `MEMR` write, whether or not
   draining succeeds. Today an FSA read can complete into a guest buffer, and
   `IPCDriver_HandleResponse` can call `OSSignalEvent` into the guest scheduler, in the middle
   of a 286 MiB memory restore. This is a data-race bug independent of the restart question.
3. **Restore-side compensation for what cannot be drained.** If draining is best-effort, the
   restore must at minimum release leaked IPC descriptors (`IsAllocated` set with no matching
   in-flight command) and recompute `FSCmdQueue::numCommandsInFlight` from the actual queue
   contents, the same way `__OSRebuildRunQueueCountsAfterStateLoad` recomputes run-queue
   counts. Without the FS recount, a single load wedges the client permanently
   (`numMaxCommandsInFlight` is 1).

The legacy `iosuIoctl_pushAndWait` path needs one extra thing: it blocks with
`__OSSuspendThreadInternal`, which the restart helper skips. Even after a drain, a thread
suspended there must be explicitly resumed by the restore, because the host thread that would
have resumed it no longer has a matching request. Alternatively, migrate these four libraries
onto the modern `IOS_Ioctlv` path and delete the legacy mechanism — `iosu_ioctl.cpp` is
already marked "deprecated IOCTL handling code" (`:7`).

### 4.2 GX2 command submission

`GX2DrawDone`, `GX2SwapScanBuffers`, and every export that can flush from
`GX2ReserveCmdSpace`. The pre-block work is a submission into the TCL ring plus, for the swap
path, a guest counter increment.

What a correct restore needs:

- **Serialize `GX2::s_perCoreCBState`** (`GX2_Command.cpp:17`). It is the write-gather cursor
  and it is host state holding raw pointers into the guest command pool. Nothing in the current
  design captures it, which means GX2 state is torn across *every* load, not only when a
  thread is blocked. The pointers are into guest memory, so they can be stored as offsets from
  `s_commandState->commandPoolBase` and rebuilt.
- **Capture only at a flush boundary.** The clean fix is to require `currentWritePtr ==
  bufferPtr` on every core before a state may be taken: the pre-block work then does not exist,
  and every reserve/flush export becomes trivially restartable. Practically this means the
  quiesce asks each core to flush its command buffer before parking, which it can do because
  the core is at a safe point.
- For `GX2SwapScanBuffers` specifically, the `flipRequestCountBE` increment (`GX2.cpp:71`) must
  not be repeated. If the capture-at-flush-boundary rule is adopted, the thread cannot be
  captured between the increment and the wait — it can only be captured inside
  `GX2WaitForFlip`, which is already SAFE. Without that rule, this export needs restore-side
  compensation (decrement the guest counter by the number of restarts), which is fragile.

### 4.3 Condition variables — `OSWaitCond` and `OSFastCond_Wait`

These are the cheapest to fix and the most dangerous to leave. The lost value is a single
`sint32` (`prevLockCount`) that lives only on the host stack.

The fix does not require draining anything: **spill it to guest memory**. `OSThread_t` has
room, or the value can be pushed onto the guest stack with a `StackAllocator` before the
release so it is covered by `MEMR`. Then make the restart idempotent by having the function
detect "I already released the mutex" — e.g. a per-thread `waitingForCond` field set before
the release and cleared after reacquisition, checked on entry. Without that, the restarted call
releases a mutex owned by another thread and corrupts two `OSMutexQueue` intrusive lists.

Until it is fixed, a state must not be captured with any thread waiting on a guest condition
variable — which is detectable cheaply at capture time by walking the active thread list for
`currentWaitQueue` pointing at an `OSCond`/`OSFastCond` (the queue's `userData` is the owning
primitive, set by `OSInitThreadQueueEx`, `coreinit_Synchronization.cpp:195`, `:597`).

### 4.4 Reschedule-after-mutation in the signalling primitives

`OSSignalEvent`, `OSSignalEventAll`, `OSSignalSemaphore`, `OSUnlockMutex`, `OSSendMessage`,
`OSReceiveMessage`, `OSWakeupThread`, `OSResumeThread`, `OSContinueThread`,
`OSSetThreadPriority`. Each mutates a guest primitive and then may be descheduled inside
`wakeupEntireWaitQueue` / `wakeupSingleThreadWaitQueue` (`coreinit_ThreadQueue.cpp:148`, `:167`)
or `__OSResumeThreadInternal` (`coreinit_Thread.cpp:839`).

These threads are `STATE_READY`, so the current helper never sees them — but the fiber rebuild
restarts them anyway, which is how a single save/load can silently double a semaphore count or
duplicate a message.

What this needs is not compensation but **avoiding the capture point**. The reschedule is a
pure optimisation (switch to the higher-priority thread we just woke); it is always legal to
skip it. The cleanest fix is to suppress the opportunistic reschedule while
`SaveStates::IsQuiesceRequested()` is set, so a core heading for the barrier finishes the
signalling call first and parks at a guest instruction boundary instead. That turns this whole
class SAFE for free.

### 4.5 Consume-before-block

`MPRunTasksFromTaskQ` (`coreinit_MPQueue.cpp:253-260`), `OSReceiveMessage`'s reschedule path
(`coreinit_MessageQueue.cpp:55`), `ProcUIProcessMessages` (`proc_ui.cpp:691`).

`MPRunTasksFromTaskQ` is the clearest example the brief asked for: it claims a batch of work
from the guest task queue and *then* yields, before running it. Restart claims a different
batch, orphaning the first and permanently inflating `taskRunCount`, which deadlocks
`MPTermTaskQ`'s drain loop.

The fix for all three is the same as §4.4 — these are yields, not waits, so suppressing the
yield during a quiesce request removes the capture point entirely. For `MPRunTasksFromTaskQ`
the yield at `:260` is explicitly discretionary ("we might want to give the scheduler the
chance for other stuff").

### 4.6 Host worker threads holding host-stack objects

`nlibcurl.curl_easy_perform`/`curl_easy_pause` (`nlibcurl.cpp:314-386`) is the severe case: a
detached `std::thread` holds pointers to two host-stack-allocated queues that the fiber
rebuild frees, so the worker writes into freed memory. `nsyshid.HIDSetReport`/`HIDRead`/
`HIDWrite`, `nn_idbe.DownloadIconFile`, the `nn_olv` network calls and `H264DECExecute`/
`H264DECEnd` are the same family with a milder failure (the worker signals a stale guest-stack
`OSEvent` rather than corrupting host memory), plus real external side effects on restart —
a duplicated USB write, a duplicated HTTP POST, a duplicated frame submitted to the decoder.

What these need:

- **Refuse to capture while one is outstanding.** These are all rare, bounded operations, and
  a per-subsystem "in-flight" counter checked by the quiesce is a few lines each. This is the
  honest answer for `nlibcurl` specifically — there is no way to make a detached worker
  pointing at a dead stack safe after the fact.
- Longer term, move the queues off the host stack (heap-allocate, refcounted by the worker) so
  the fiber rebuild cannot free them out from under a running thread. That fixes the
  use-after-free but not the duplicate request, which still needs the drain.

### 4.7 The nesting hazard: blocking inside `PPCCoreCallback`

This is not an export class but it invalidates the restart argument wherever it occurs, so it
belongs here.

`PPCCore_executeCallbackInternal` (`PPCScheduler.cpp:61-101`) saves the guest `LR` and
`instructionPointer` into **host locals** (`:66-67`), pushes 64 bytes of guest stack (`:69`),
runs the guest callback in a host loop, and restores all of it afterwards (`:96-99`). If a
thread is captured anywhere inside that callback — blocked at a nested HLE call, or merely
preempted at a guest instruction — the fiber rebuild discards the loop and the saved `ip`. The
restarted thread runs the callback to its `blr`, jumps to `LR` (the `_PPCCore_callbackExit`
stub), which sets `hCPU->instructionPointer = 0` (`PPCScheduler.cpp:57`) expecting the host
loop to notice. There is no host loop. `__OSFiberThreadEntry`'s interpreter loop
(`coreinit_Thread.cpp:1380`) then executes guest instructions **at address 0**.

Callers that can put a guest callback on the stack of an HLE export include
`__FSCmdSubmitResult` (`coreinit_FS.cpp:713`), `__FSAIoctlResponseCallback`
(`coreinit_FS.cpp:741`), `__IPCDriverThreadFunc` (`coreinit_IPC.cpp:194`),
`__GX2CallbackThread` (`GX2_Event.cpp:193`), `MPRunTasksFromTaskQ` (`coreinit_MPQueue.cpp:272`),
`OSDynLoad`'s allocator hooks (`coreinit_DynLoad.cpp:48`, `:62`), every `nlibcurl` callback
(`nlibcurl.cpp:342`-`:376`, `:480`-`:496`), and `camera` (`camera.cpp:175`). Note the first
four run on long-lived driver threads that spend their lives blocked, so they are exactly the
threads a capture is most likely to catch.

What this needs: a per-thread host callback-depth counter, incremented in
`PPCCore_executeCallbackInternal` and stored somewhere the quiesce can read, and a capture-time
refusal if any thread has depth > 0. A guest-side heuristic exists as a fallback — walk the
thread's guest stack back-chain and refuse if any saved LR equals the
`_PPCCore_callbackExit` stub MPTR — but the counter is cheaper and exact.

### 4.8 Not restart-safety, but adjacent and load-bearing

These came up repeatedly while reading and will defeat a restore even if every restart above is
made sound. Listing them so they are not rediscovered later:

- `GX2::s_perCoreCBState` (`GX2_Command.cpp:17`) — host write-gather cursor, unserialized.
- `IPCServiceClient::m_commandBuffersFree` (`nn_client_service.h:259`) — host free-list.
- `proc_ui`'s `s_commandForCoreThread`, `s_isInForeground`, `s_currentProcUIStatus` — host
  atomics/statics driving the guest core-thread protocol.
- `swkbd`'s `swkbdInternalState` (`swkbd.cpp:96`) — host pointer into a guest allocation.
- `nsysnet`'s entire socket table — host `SOCKET` handles behind guest descriptors. In-session
  restart of `recv`/`recvfrom`/`select` is individually re-entrant (they are `MSG_PEEK` /
  `EWOULDBLOCK` poll loops, `nsysnet.cpp:1047-1060`, `:1243`, `:1676-1686`), but the socket
  option toggling around them (`_setSocketSendRecvNonBlockingMode`, `:1046`, `:1061`) is host
  state mutated before the block.

---

## 5. Highest-risk findings

Ordered by severity, most severe first. Severity here weighs *silent corruption* above
*crash*, and *certain* above *conditional*.

1. **`nlibcurl.curl_easy_perform` is a use-after-free on load** (`nlibcurl.cpp:320-324`). A
   detached host worker holds pointers to two queues allocated on the fiber's host stack; the
   fiber rebuild frees them while the worker is still running, and the worker then writes into
   them. Not a duplicate-request problem — a memory-corruption one, in host memory, on a thread
   nothing controls.

2. **`OSWaitCond` / `OSFastCond_Wait` corrupt guest mutex ownership**
   (`coreinit_Synchronization.cpp:352`, `:599`). Restarting makes a thread release a mutex
   owned by *another* thread and call `removeMutex` on it, which cross-links two intrusive
   `OSMutexQueue` lists (`coreinit_Thread.h:246-260`). The saved recursion count is lost.
   Silent, and the damage surfaces arbitrarily far away.

3. **One save-state load permanently wedges the filesystem** (`coreinit_FS.cpp:497`, `:499`,
   `:303`). Restarting a synchronous `FS*` call re-increments `numCommandsInFlight` with no
   matching decrement, and `numMaxCommandsInFlight` is **1**, so `__FSUpdateQueue` never
   dispatches another command for that client. Certain to happen — a running title almost
   always has an FS call outstanding — and easy to misdiagnose as a hang.

4. **The `__depr__IOS_Ioctlv` family is invisible to the restart helper** (`iosu_ioctl.cpp:31`,
   `coreinit_Thread.cpp:1594`). `MCP_*`, `nn_act`, `nn_acp` and `nn_nim` block via
   `__OSSuspendThreadInternal`, leaving the thread `STATE_READY` with `suspendCounter > 0` and
   no wait queue. The helper counts it as "skipped"; nothing ever resumes it except a host
   thread whose matching request no longer exists. The same applies to
   `PPCConcurrentQueue::pop` (`PPCConcurrentQueue.h:53`), which backs `nlibcurl`.

5. **Restart is unconditional, so the helper's safety check gates nothing**
   (`coreinit_Thread.cpp:1711` vs `:1581`). Because `__OSRebuildHostThreadsAfterStateLoad`
   recreates every fiber at `__OSFiberThreadEntry`, every thread mid-HLE restarts regardless of
   its state or its `srr0`. The `primary opcode == 1` check only decides which `STATE_WAITING`
   threads are made runnable — it does not prevent any restart. Any future "refuse to restart
   this one" logic has to live in the capture path or in the fiber rebuild, not here.

6. **IOSU host threads run through the entire `MEMR` restore** (`Quiesce.cpp:145-209` parks
   only the PPC cores and the Latte thread; `iosu_fsa.cpp:833`, `iosu_nn_service.cpp:18`,
   `:143` are never parked). An FSA completion can write a response into a guest buffer, and
   `IPCDriver_HandleResponse` (`coreinit_IPC.cpp:266`) can call `OSSendMessage`/`OSSignalEvent`
   into the guest scheduler, while 286 MiB of guest memory is being rewritten underneath them.

7. **`GX2SwapScanBuffers` leaks a phantom flip per load** (`GX2.cpp:71`, `:84`). The guest's
   `flipRequestCountBE` is incremented before the wait, so each restart adds one. The counter
   never recovers, and the `> 5` guard turns the drift into a stall. The accompanying duplicate
   `IT_HLE_TRIGGER_SCANBUFFER_SWAP` presents a frame twice.

8. **Blocking inside `PPCCoreCallback` sends the thread to guest address 0**
   (`PPCScheduler.cpp:57`, `:96-99`). The host loop that would have restored `instructionPointer`
   is gone, so the callback's return sets IP to 0 and the interpreter runs from there. The
   threads most exposed are the long-lived driver threads (IPC driver, GX2 event callback, FS
   completion) that are blocked essentially all the time.

9. **Every PM4-emitting GX2 export can re-submit a command buffer** (`GX2_Command.cpp:281`,
   `:209`, `:329`). About 70 exports share the flush path, and `s_perCoreCBState`
   (`GX2_Command.cpp:17`) is unserialized host state, so the restarted export re-flushes a
   buffer the GPU already executed.

10. **A multicore spin in `OSUninterruptibleSpinLock_Acquire` prevents capture entirely**
    (`coreinit_Spinlock.cpp:130-133`). That loop never yields, so the core cannot reach the
    quiesce barrier while the guest spinlock is held by a thread the barrier has already
    parked; `QuiesceScope` times out (`Quiesce.cpp:190`) and the save is refused. A liveness
    bug rather than a correctness one, but it will present as "save states randomly don't work".
