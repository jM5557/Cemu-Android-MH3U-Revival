# Save States — phase 0 results and findings

**Date:** 2026-09-13
**Branch:** `savestates` (upstream `main` @ `3310f3b8` + this work)
**Tested with:** Monster Hunter 3 Ultimate (US v32, `0005000010118300`), Vulkan, multi-core recompiler

Companion documents: `save-states-design.md` (the plan), `save-states-survey.md` (code survey).

---

## 1. Status in one line

**Guest memory, timers, alarms and the scheduler's guest structures all save and restore
correctly. The world still does not resume, and the reason is now identified and verified:
Cemu's HLE gives every blocked guest thread a *host C++ continuation* on its fiber stack,
and that continuation is not in guest memory. See §6 — it invalidates a core assumption of
the design.**

> Superseded: earlier revisions of this document said the emulator "runs but the display is
> frozen", and treated the GPU as the open problem. That was wrong. The emulator was not
> running at all; Cemu's FPS readout is driven by host vsync and keeps reporting 60 with the
> guest completely stopped. §4 has the measurement that settles it.

---

## 2. What is implemented

New module `src/Cafe/SaveState/`:

| File | Purpose |
|---|---|
| `StateStream.h/.cpp` | Bidirectional serializer (Measure/Write/Read). One `DoState()` per subsystem runs in both directions, so read/write drift is structurally impossible. Poisoned-stream errors, `DoMarker` section sentinels. |
| `StateFile.h` | Container: `CSTA` header + uncompressed HEAD + zstd body. Fingerprint gate (build version, title id/version, core count). |
| `Quiesce.h/.cpp` | World-stop protocol + GPU ring drain. |
| `SaveState.h/.cpp` | Orchestration, `MEMR` chunk, file I/O (atomic temp+rename). |

Hooks in existing files:

- `coreinit_Thread.cpp` — quiesce barrier, `ThreadDoState` (`CPUS`), host-fiber rebuild
- `coreinit_Alarm.cpp` — `AlarmDoState` (`ALRM`)
- `PPCTimer.cpp` — `PPCTimer_DoState` (`TIME`)
- `TCL.cpp` — `TCLDoState` (`GX2S`), ring-empty check
- `LatteThread.cpp` — `Latte_DoState` (`LATT`)
- `MainWindow.cpp/.h` — File menu entries (`SAVE_STATE = 20108`, `LOAD_STATE = 20109`)

Chunk order in `DoStateBody()` is significant:
`MEMR → CPUS → TIME → ALRM → GX2S → LATT → END`.
`ALRM` must run after `TIME` (it rebuilds fire ticks against the restored clock) and
before the host-thread rebuild (which frees fiber stacks some alarms point into).

---

## 3. Verified working (in real gameplay, not synthetic)

| Behaviour | Evidence |
|---|---|
| Quiesce parks all 3 scheduler cores, no deadlock | Save completes in ~1s, emulator resumes at 60 FPS |
| GPU command ring drains before capture | No `did not drain` warning across many runs |
| `MEMR` captures/restores guest memory | 285–287 MiB captured, ~105 MB zstd, all markers match on load |
| Zero-block elision | ~1.3 GiB of mapped ranges → 286 MiB stored |
| Host fibers destroyed and rebuilt | No crash, guest threads resume and execute |
| Recompiler flush | No stale-code crash |
| **Guest world genuinely rewinds** | Save in village → walk to Moga Woods → load → **village music plays again** |

That last row is the key result. AX voice data lives in guest RAM, so hearing village
audio after loading a village state proves guest memory was restored and the rebuilt
threads are executing village code.

---

## 4. What is actually happening after a load

**The guest is completely stopped.** Not rendering-stalled — stopped.

Two independent measurements, both against the live process via pymem:

**Game logic is not advancing.** Walking the documented `sPlayer` chain
(`[0x10314b80] → +0x40+slot*4 → obj`) reads a coherent player — HP 100/100, stamina
600/600 — so guest memory restored correctly. But every field sampled over several seconds
is byte-identical.

**No guest thread is runnable.** Scanning guest memory for the `OSThread_t` magic `tHrD`
(`+0x320`) and decoding every hit:

```
READY=0  RUNNING=0  WAITING=27   (+5 NONE/MORIBUND)
threads whose scheduler fields changed over 2s: 0
currentRunQueue[0..2] == null for every thread
```

Every guest thread is parked on a wait queue and nothing is on any run queue.

> **Why this looked like a GPU problem for three rounds of GPU fixes.** Cemu's FPS readout
> is driven by host vsync, so it reports a steady 60.00 with the guest entirely stopped.
> The audio backend keeps looping its last buffer. Nothing crashes. Every signal that is
> cheap to read says "alive, stuck on a frame".
>
> The earlier "village music plays after loading from the woods" result was real evidence
> that *memory* restored — but it was not evidence that anything was *executing*, and it
> was read as though it were.
>
> Methodology that actually discriminates, in increasing order of authority: viewport-cropped
> screenshot diffs (never full-window — the title bar's FPS text always changes), then a game
> field known to tick, then the scheduler's own structures. Only the last one is conclusive.
> Pick a liveness probe that is *validated to change during normal play* — the hunger timer
> used first does not tick in the village, so its being static proved nothing.

---

## 5. Three host/guest splits found and fixed

A recurring pattern: state split across the host/guest boundary where only the guest half
lives in the RAM snapshot. Each was correctly diagnosed and genuinely fixed; none restored
the picture.

### 5.1 Host alarms (`ALRM`) — also a use-after-free
`OSHostAlarm` objects hold absolute fire ticks against the emulated clock. `TIME` rewinds
that clock, so unrestored alarms appear scheduled far in the future and stop firing
(`_OSSleepTicks_alarmHandler`, the AX frame alarm, vsync).

Worse: of the three `OSHostAlarmCreate` call sites, two capture a context pointer into the
**calling fiber's stack** and are only destroyed when those functions return normally:

- `coreinit_Thread.cpp:700` — `OSSleepTicks`
- `coreinit_Synchronization.cpp:103` — `OSWaitEventWithTimeout` (`&data`, a host local)

A load destroys every host fiber, so leaving those armed fires callbacks into freed
memory. They are now dropped before the rebuild.

Simplification the design doc did not anticipate: it specified a callback-ID registry so
no function pointer enters a state file. Unnecessary — every guest-visible alarm uses the
same callback (`__OSHostAlarmTriggered`) with a null context, so only addresses are stored.

### 5.2 GPU command ring (`GX2S`)
`TCL.cpp` keeps the ring entirely in host memory:

```cpp
std::atomic<uint32> tclRingBufferA[4096];
std::atomic<uint32> tclRingBufferA_readIndex;   // GPU cursor
std::atomic<uint32> tclRingBufferA_writeIndex;  // CPU cursor
uint64 s_currentRetireMarker;
```

Guest submission bookkeeping is restored; these are not. Fixed by draining the ring during
the quiesce and restoring it as empty.

### 5.3 Flip handshake (`LATT`)
```
LatteCommandProcessor.cpp:946  GPU consumes swap cmd → LatteGPUState.flipRequestCount++   [host]
LatteTiming.cpp:112            if (flipRequestCount > 0) { --; guest flipExecuteCountBE++ } [host→guest]
```
Guest counters live in the GX2 shared area (restored); `flipRequestCount` is a host atomic
(was not). Note 5.2 made this worse: emptying the ring discards an in-flight swap command,
so the guest returns expecting a flip the GPU will never see.

---

## 6. Root cause: HLE blocked threads have host continuations

**This is the blocker, and it is architectural rather than a missing chunk.**

Cemu runs each guest thread on its own host fiber. When guest code calls an HLE function
that blocks, the *host C++ call stack* parks on that fiber:

```
guest code
  -> OSWaitEvent()                          [host C++]
     -> OSThreadQueueInternal::queueAndWait [host C++]   coreinit_ThreadQueue.cpp:10
        thread->state = STATE_WAITING;
        PPCCore_switchToSchedulerWithLock();   <-- fiber parks HERE
        cemu_assert_debug(state == RUNNING);   <-- resumes HERE when woken
```

Waking the thread means `Fiber::Switch` back into that stack, which returns up through
`queueAndWait` → `OSWaitEvent` → the HLE dispatcher → guest code. **None of that host stack
is in guest memory.**

`__OSRebuildHostThreadsAfterStateLoad()` deletes every fiber and creates fresh ones starting
at `__OSFiberThreadEntry`, which does `__OSLoadThread()` and then immediately begins
executing guest instructions at the restored PC. It has no way to resume a parked C++ frame.

So the rebuild is sound *only* for a thread descheduled at a guest instruction boundary —
one that was preempted mid-timeslice. It is unsound for any thread blocked inside an HLE
call. And §4 measured the split in a real game: **27 of 27 live threads were blocked inside
HLE calls; zero were resumable.** That is not an unlucky sample — waiting on events, mutexes,
message queues and vsync is what guest threads spend their lives doing.

This invalidates the design's central claim (`Quiesce.h`, and §5.1 of the design doc): that
parking cores after `__OSStoreThread()` puts "every guest CPU context inside the RAM snapshot
for free". The *guest* context is captured. The *host* continuation is not, and in Cemu's HLE
model a blocked thread has both.

### 6.1 Why the earlier fixes could not have worked
All three host/guest splits in §5 were real and correctly fixed, and none could have restored
the world, because no guest thread could resume regardless. The same applies to the two fixes
added after them:

- **GPU thread parked in the quiesce** (`Quiesce.cpp`, `LatteCommandProcessor.cpp`) — worth
  keeping on its own merits: previously the Latte thread read guest memory while `MEMR`
  rewrote 286 MiB underneath it.
- **GPU cache drop on load** (`Latte_DropCachesForStateLoad`) — verified to work: the stale
  pre-load frame is now correctly discarded (display goes black instead of holding a ghost
  frame). It also exposed and fixed a genuine upstream use-after-free
  (`LatteMRT::NotifyTextureDeletion` never cleared the depth attachment).
- **Run queue count rebuild** (`__OSRebuildRunQueueCountsAfterStateLoad`) — correct and worth
  keeping (the host `CounterSemaphore` array does mirror guest run queues and was not
  restored), but it changes nothing here: with every thread WAITING the correct count *is*
  zero, and the cores are right to sleep.

### 6.1b Measured: the HLE entry context is already in the snapshot

Taken against a live title, for all 29 waiting threads:

```
srr0 (host-endian! see below) lands in 0x00e00e44 .. 0x00e0ba04 for every thread
12 distinct values across 29 threads -- they cluster by which HLE function each is in
lr holds real guest return addresses (0x02xxxxxx = main module)

instruction word at those addresses:
  0x00e00e44: 04000178   primary_op=1  hleFuncId=0x0178
  0x00e0117c: 0400002b   primary_op=1  hleFuncId=0x002b
  0x00e01190: 04000151   primary_op=1  hleFuncId=0x0151
  0x00e0b460: 0400047f   primary_op=1  hleFuncId=0x047f
```

Primary opcode 1 is Cemu's reserved HLE opcode (`PPCInterpreterImpl.cpp:464` →
`PPCInterpreter_virtualHLE`), and the low 16 bits index `s_ppcHleTable`.

**So a blocked thread's saved guest context already records which HLE function it is inside
(`srr0`), with what arguments (GPRs), and where to return (`LR`) — and all of it is in guest
memory, already covered by `MEMR`, for free.** No dispatcher instrumentation is needed to
capture an entry context; it is the saved context.

This makes the *mechanism* of Path A small. On load, for each `STATE_WAITING` thread: unlink
it from `currentWaitQueue`, set `STATE_READY`, add to the run queue, and leave the context
untouched. A fresh fiber then starts executing at `srr0`, hits the HLE opcode, and re-invokes
the original call with the original arguments against restored guest memory — returning
immediately if the wait is already satisfied, or blocking again correctly if not. The unlink
is what stops the re-entry from double-queueing the thread.

> Gotcha for anyone reading `OSContext_t`: `srr0` is stored **host-endian**
> (`coreinit_Thread.cpp:1140` assigns `hCPU->instructionPointer` with no `_swapEndianU32`,
> and the field is a plain `uint32`, not a `betype`). Every neighbouring field is big-endian.
> Read it as BE and you get nonsense like `0x7048e000` instead of `0x00e04870`.

### 6.2 Options, honestly
- **Restart the HLE call (recommended).** Per §6.1b the mechanism is small — unlink the
  thread from its wait queue, mark it READY, let it re-enter the HLE call its context already
  points at. Portable, survives cross-session, upstreamable. The *cost is not the mechanism,
  it is the audit*: a call that only waits on a guest primitive (`OSWaitEvent`, `OSLockMutex`,
  `OSReceiveMessage`, `GX2WaitForVsync`) restarts cleanly, but one that already handed work to
  a host subsystem before blocking must not be re-entered — restarting an `IOS_Ioctl` would
  re-submit the IPC request. Every blocking export needs classifying, and the unsafe ones need
  either draining before capture or their own restore logic. That is the same FSAS/IOSU
  problem the design doc deferred to phase 1, now with a concrete reason it cannot be skipped.
  A cheap first experiment exists: implement the unlink-and-restart for all waiting threads and
  observe how far a title gets before something re-entrant breaks.
- **Snapshot the host fiber stacks.** The fallback, and in-session only. Requires
  replacing Win32 `CreateFiber` (opaque, OS-owned stack) with a fiber whose stack Cemu
  allocates — `FiberUnix.cpp` is already closer to this — then saving `[SP, stackBase)` plus
  the switch context per thread. Sound only while host code addresses and referenced host
  objects are unchanged, i.e. same process, no cross-session load. Host objects reachable
  from those stacks (notably `OSHostAlarm`) must then be *restored as the same objects*
  rather than recreated, which conflicts with the current `ALRM` approach.
- **Make HLE blocking points re-enterable**, so a blocked thread's whole state lives in guest
  memory. Correct and portable; a very large refactor across coreinit/GX2.
- **Restrict when a state may be taken** to moments with no thread blocked in HLE. Measured:
  that never happens.

Nothing here is fixable by adding another chunk to the state file.

---

## 6b. Superseded hypothesis (kept for the record)

*Both steps below were implemented and neither restored the world — §6 explains why. The
rationale is kept because the reasoning was sound given what was measurable at the time, and
because it shows how a plausible story survived three rounds of evidence that never actually
tested it.*

The Latte thread is a separate host thread that **is never stopped** by the quiesce and owns
a large amount of *derived* state:

- texture cache, buffer cache, render-target database (host VRAM keyed by guest addresses)
- command-processor parse state (`m_queuePosStack`, drawcall context)
- Vulkan swapchain, command buffers, fences

Restoring 286 MiB of guest memory underneath all of that leaves it describing a world that
no longer exists. A GPU thread wedged on a fence that will never signal fits the symptom
exactly (CPU runs, display frozen).

**The design doc's phase-0 plan called for a cache-drop on load — drop texture cache, buffer
cache, cached FBOs and shader binding state, keep compiled pipelines. None of that was
implemented.** Only the ring and the counters were done.

So the next step is not another counter. It is:

1. **Park the Latte thread** as part of the quiesce (currently only PPC cores are parked),
   so the snapshot is taken with the GPU genuinely idle rather than merely drained.
2. **Invalidate the GPU caches on load** per the design doc's cache-drop strategy.
3. Only then consider the `LatteGPUState` register block and render-target readback.

---

## 7. Reproduction recipe

```
# build (VS2022 pinned via vcpkg_overlay_triplets/ - see section 8)
build_ss.bat

# launch with MH3U, get in-game, then drive the menu commands directly:
#   posting WM_COMMAND is more reliable than synthetic Alt+F, which the
#   emulator's input grab frequently swallows
python menucmd.py 20108   # save state
python menucmd.py 20109   # load state
```

State lands at `<cemu user dir>/savestates/<titleId>/slot0.cst`.
Success/failure is logged (`Save state: captured N MiB` / `restored N MiB`).

**Do not judge a load by the picture, the FPS readout or the audio** — all three lie (§4).
Judge it with the thread scan: if `READY == 0 && RUNNING == 0` after a load, the guest is
stopped no matter what the window shows. The probe scans guest memory for `tHrD` at
`OSThread_t+0x320` and decodes `state/attr/id/suspendCounter/currentRunQueue/currentWaitQueue`
from the field layout in `coreinit_Thread.h:424`.

Note for anyone rebuilding that probe: the pymem backend's `read_region()` is indexed from
guest `0x02000000`, not `0`, so magic hits need that added before use as guest addresses.

---

## 8. Build environment gotcha (cost more time than the feature)

This machine has **two MSVC toolchains**: VS2022 (14.44) and VS2026 BuildTools (14.51,
installed 2026-08-16). CMake is invoked with the VS2022 generator, but vcpkg does its own
discovery and picks the newest — so dependencies were built with 14.51 while Cemu's objects
were 14.44. Release builds enable LTCG (`CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE`), and
link-time code generation refuses to mix compiler versions:

```
fatal error C1047: ... created by a different version of the compiler
fatal error LNK1257: code generation failed
```

Fixed by `vcpkg_overlay_triplets/x64-windows-static.cmake` pinning the VS instance, passed
via `-DVCPKG_OVERLAY_TRIPLETS`. **The path must use escaped backslashes** — CMake unescapes
them to the backslash form vcpkg compares its discovered instances against; a forward-slash
path silently never matches, and a single backslash breaks CMake's own parser.

⚠️ `E:\Cemu-src` (the MH3U/server build) links today **only because its `vcpkg_installed`
predates the VS2026 install**. Any dependency rebuild there will hit the identical failure.
The same triplet file fixes it.

---

## 9. Things deliberately not done

- Graphic-pack patch hash in the fingerprint is still `0`, so a state taken with a different
  patch set is accepted and may desync.
- Single fixed slot; no hotkeys, thumbnails or undo-load.
- Menu handlers run synchronously on the GUI thread (bounded by the quiesce timeout).
- `FSAS` (file handles), `AXST` (audio voice lists), per-service `IOSU` chunks — all still
  unimplemented, per the design doc's phase 1.
- On-disk magic reads as `ATSC` in a hex dump (`kStateFileMagic` byte order), which defeats
  the "greppable FourCC" intent. Cosmetic, but fix it before the format is depended on.
