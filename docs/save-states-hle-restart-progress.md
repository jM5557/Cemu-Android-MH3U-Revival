# HLE-restart: session log and handoff

**Date:** 2026-09-13 (late session), updated 2026-09-14
**Branch:** `savestates`, on top of `bb31c2cf`
**Read first:** `save-states-phase0-findings.md` §6 (why restarting is necessary),
`save-states-hle-restart-audit.md` (which calls are safe to restart).

---

## 0. The world survives the load (2026-09-14)

**A restored MH3U keeps running.** Measured three seconds after a load, from a save taken in
Moga Village and loaded after walking to Moga Woods:

```
wakeUpCount total : 1353529 -> 1395438   delta=41909
totalCycles total : 55581964341 -> 57322703866   delta=1740739525
>>> GUEST IS RUNNING
```

A healthy baseline is ~36,000 timeslices and ~1.5 billion cycles per three seconds, so this is
a normally running game, not a twitch. Zero crashlogs. Confirmed on screen by the user.

That closes the chain that started with phase 0: memory, threads, scheduler, timers, alarms,
GPU caches, present, **and the world staying alive afterwards**.

It then kept running: **a restored MH3U played through a real area transition**, village ->
Moga Woods -> village, at full throughput (57.3 -> 185.0 billion cycles), with nothing at all
written to the log after the load. No FS errors, no crash, no dropped frames reported.

Two results worth stating plainly because both were predicted to go the other way:

- **FS did not wedge.** Audit §8 item 1 -- `numMaxCommandsInFlight` is 1, so a restart that
  re-increments without a matching decrement was expected to kill file I/O permanently -- is
  still a real hazard in source, but it did not fire. Area loading works after a state load.
- **Newly acquired audio voices play correctly**, which is the independent confirmation that
  the `SNDV` fix put the voice allocator back in agreement with guest memory.

The BGM defect found here (a streamed track looping instead of resuming) was tracked down the
same evening and fixed -- see §3.3. Audio now survives a load intact.

The blocker described in the previous §3.1 is fixed, and the guess recorded there was wrong in
an instructive way -- see §3.1 below for what it actually was.

---

## 1. Where this stands

The restart approach **works mechanically and is the right design**. A blocked thread's saved
context already names the HLE call it is in, so it can be re-invoked against restored memory.
Measured, repeatedly: every blocked thread is at an HLE opcode, the safe ones re-enter their
call and block again correctly, and nothing about the mechanism is speculative any more.

**The crash is fixed and verified** (23:50 run, see §3). Zero crashlogs, and every restarted
thread re-entered its HLE call and re-blocked on a proper wait queue -- `2dcb0560`, which had
crashed on every previous attempt, now sits in `WAITING` like the rest.

**The restored world also renders** -- a complete, uncorrupted Moga Village frame (§3.0). The
full pipeline runs at least once: memory, threads, game logic, draw submission, GPU cache
rebuild, present.

The world also *stays* alive as of 2026-09-14. What used to stall it is fixed; see §3.1.

---

## 2. The crash, fully explained

This is a verified chain, not a hypothesis. Every link was read in the source:

1. `StackAllocator` allocates on the **guest** stack: `PPCInterpreter_PushAndReturnStackPointer`
   moves `hCPU->gpr[1]` down, and the destructor pops it back (`StackAllocator.h:16`, `:23`).
2. `OSSleepTicks` uses one — `StackAllocator<OSThreadQueue> _threadQueue;`
   (`coreinit_Thread.cpp:697`). So does a good deal of the rest of coreinit.
3. A thread that blocks inside such a call is stored with `gpr[1]` **below** its real stack
   pointer, by the size of that scratch area.
4. Restarting the call from the beginning with that stored value makes the re-run allocate a
   *second* scratch area and return one short. Guest code resumes with the stack pointer off
   by N.
5. The guest function's epilogue then reads its saved link register from the wrong slot, gets
   `0`, and `blr` branches to address `0`.

`LR = 0` in the crash dump is the tell: had this been the callback mechanism, `IP` would be 0
but `LR` would not. It also explains why it is always an `OSSleepTicks` thread.

### Fix
Restart the call with the stack pointer the thread had on **HLE entry**, not at block time.
That value did not exist anywhere, so it is now captured and serialized:

- `PPCInterpreter_t::hleEntryStackPointer` (`PPCState.h`) — new field
- set on every HLE dispatch, in **three** places (see §3 for why three)
- serialized per thread in the `CPUS` chunk next to `activeThread[]`, because it is host
  state and `MEMR` does not cover it
- consumed in `__OSRestartHLEBlockedThreadsAfterStateLoad()` to rewind `context.gpr[1]`

---

## 3. The stack fix: TESTED AND WORKING

`bin/Cemu_release.exe` (built 23:47) contains the entry-SP capture added to the **recompiler**
backends, which is the part that actually mattered:

```
BackendX64.cpp:168       hCPU->hleEntryStackPointer = hCPU->gpr[1];
BackendAArch64.cpp:879   ppcInterpreter->hleEntryStackPointer = ppcInterpreter->gpr[1];
```

The first attempt hooked only `PPCInterpreter_virtualHLE` and was a **complete no-op**: the
recompiler dispatches HLE calls directly and never calls that function. MH3U runs recompiled,
so nothing was captured and the "fix" changed nothing. Both recompiler backends are now hooked
as well as the interpreter.

**Note: the state file format changed** (the `CPUS` chunk gained the entry-SP array). Existing
`.cst` files will not load. Take a fresh save.

**Result (23:50 run): no crash.** `restarted 13 HLE-blocked threads (13 host threads, 0 not at
an HLE call, 0 suspended)`, zero crashlogs, and afterwards `READY=0 RUNNING=1 WAITING=26` --
every restarted thread had re-entered its call and blocked again correctly. The
`IP 0x00000000` failure that ended every earlier run is gone.

### 3.0 The picture comes back

**The restored world renders.** After the 23:50 load the display shows a complete, correct
Moga Village -- the saved location, not the Moga Woods the load was triggered from -- with
hunter, Felynes, NPCs and textures all intact and no corruption anywhere in the frame.

That is the whole pipeline working at least once: memory restored, threads restarted, game
logic executed, draw commands submitted, GPU caches repopulated from restored memory, frame
presented. It also vindicates the phase-0 cache-drop strategy -- dropping every
guest-address-keyed cache and letting it rebuild yields a clean image rather than artifacts.

The frame is frozen (successive viewport-cropped screenshots are pixel-identical), because the
world wedges immediately afterwards -- see §3.1. Sequence is: resume, render, stall.

> Worth noting how this was nearly missed. `alive.py` correctly reported the guest stopped, and
> that was taken as "nothing happened". The measurement was a 3-second window sampled *after*
> the event; it could not see the burst of execution that drew this frame. When a probe says
> "stopped", it is worth asking "stopped in what state" and looking at the screen.

### 3.1 What actually wedged the core: AX voice lists (FIXED 2026-09-14)

The guess recorded here previously -- "a skipped service thread re-runs its host body from the
top and blocks" -- was wrong in both halves. Worth reading for how it was wrong.

**Measured, not guessed.** Three instruments, in order:

1. The watchdog (§3.2) reported all three cores frozen, core 1 holding thread `0e46baa0`
   with guest `ip 00e0ba04` -- still at the HLE stub it entered, so no guest code was running.
2. Host-level sampling of the live stalled process -- `GetThreadDescription` plus per-thread
   CPU deltas, no rebuild needed -- showed `OSSched[core=1]` burning a **full core**. So it was
   not blocked at all. It was spinning in host C++.
3. Host checkpoints (§3.2) pinned it exactly: `host ax:mix/128046`, label *and* sequence
   number identical at +2s, +5s and +10s.

`ax:mix` sits immediately before `AXMix_process`. The loop is `while (internalVoice)` in
`AXMix_ProcessVoices` (`ax_mix.cpp`), walking a voice chain that had closed into a cycle.

The thread is Cemu's **AX interrupt-service thread** -- unnamed, which is why an entry address
was all the earlier session had. It is now labelled `(AX IST)` in every save state log line.

#### Root cause

AX keeps its voice allocation in **host** `std::vector<AXVPB*>`: `__AXVoicesPerPriority` and
`__AXFreeVoices` (`ax_voice.cpp`). A state load replaces guest memory and leaves those
untouched, so host and guest disagree about which voices exist. `AXFreeVoice()` does not reset
`vpb->priority`, so its own double-free guard stops working; a voice ends up in two lists,
`AXIst_SyncVPB()` links it to itself when it builds the per-frame `nextToProcess` chain, and
the mixer walks that chain forever.

Because the loop is host C++ inside an HLE body it never reaches a scheduler boundary. It held
core 1 permanently, which stopped `__OSCheckSystemEvents()`, which stopped host alarms, which
left every `OSSleepTicks` sleeper waiting forever. One audio thread took down the machine --
the mechanism the old §3.1 imagined, arrived at from a completely different direction.

Same bug class as the Latte thread and the fibers: **host state describing guest objects**.

#### Fix

A new `SNDV` chunk carrying the voice lists as indices, plus the protection arrays
(`AXVoiceDoState`, `ax_voice.cpp`). This one has to be **serialized rather than rebuilt** --
unlike fibers and run queue counts, nothing in guest memory separates a free voice from an
allocated one, precisely because `priority` survives the free.

The mixer walk is also bounded to `AX_MAX_VOICES` now, logging and dropping the rest of the
frame. A cycle there is unrecoverable and silent; a dropped audio frame is a much better
outcome than a hung emulator with nothing in guest state to explain it.

### 3.2 Instrumentation added (keep it)

- **Stall watchdog** (`__OSStartStallWatchdogAfterStateLoad`, `coreinit_Thread.cpp`). Fires at
  +2s/+5s/+10s after a load. Per core: idle-loop iteration count, run queue count, host
  checkpoint, current thread, live guest IP. Then the whole thread table with
  `wakeUpCount`/`totalCycles`, so running threads separate from merely-waiting ones.
  Deliberately **lock-free**: a core that has stopped dispatching may be holding the scheduler
  lock, and a diagnostic that blocks on what it is diagnosing reports nothing.
- **Host checkpoints** (`Cafe/SaveState/HostCheckpoint.h`). Per-core label plus sequence
  number, marked through the AX frame path. A frozen sequence names the stuck function; an
  advancing one says look further out. Reusable -- add marks wherever a host body is suspect.
- **Thread names in every save state log line.** Cemu knows them (`Alarm Thread`,
  `{SYS IPC Core n}`); an entry address was never the best identification available.
- **Per-host-thread CPU sampling from outside.** `GetThreadDescription` + `TotalProcessorTime`
  deltas over a few seconds, on the stalled process, no rebuild. This is what separated
  "blocked" from "spinning" and it should be the second thing tried after the watchdog.

### 3.3 The BGM loop: IOSU file handles (FIXED 2026-09-14)

After the world ran, one defect remained: the music that was streaming at save time looped a
~10 second buffer instead of continuing. Sound effects were fine, and it cleared itself at the
next area transition.

**Measured, in this order.** Each step killed a hypothesis, which is the only reason the last
one was reached:

1. `threadrates.py` before the save and after the load: the active thread set and their rates
   were **identical**, and eleven wake counters had gone backwards (proof the load took). So no
   thread was parked -- the "stream thread never got signalled" theory was wrong.
2. The AX thread burns ~3 guest cycles per timeslice both before and after, so MH3U does not
   drive streaming from `AXIst_HandleFrameCallbacks`. Callbacks were not it either.
3. `voices.py`: the BGM is twelve voices with `loopflag=1` over ~393k-sample buffers -- a ~10
   second loop, matching the symptom exactly.
4. `streambuf.py` on those buffers, **with a control**: static after a load, changing in a
   healthy session. The game genuinely stops refilling.

#### Root cause

An FSA file handle is `(slotIndex << 16) | checkValue` into `sFileHandleTable` -- a **host**
table whose check values come from a host counter (`iosu_fsa.cpp`). Guest memory keeps its
handles across a load; the `FSCVirtualFile` behind them belongs to the loading session. The
slot has since been released or reallocated, so `GetByHandle` returns null, the read fails, the
game treats it as an ordinary error and stops refilling, and the voice loops. Nothing hangs and
nothing is logged.

It recovers at an area transition because the game closes and reopens the stream, getting a
fresh handle that host and guest agree on.

#### Fix

An `FSAH` chunk recording each open handle's path, access flags and seek position, reopening
them on load at the same slot index and check value so guest handles resolve again. Confirmed:
`Save state: reopened 2 file handles (0 failed)`, and the music survives.

Two deliberate limits:

- **Create and truncate flags are stripped on reopen.** The host filesystem is not part of a
  save state, so the file on disk is whatever the loading session left. Reopening a `"w"`
  handle as originally opened would truncate it, and a game save is exactly the kind of file
  held open that way.
- **Directory iterators are excluded.** Reopening one resets its position, so a game mid-listing
  would silently restart the directory. A clean failure beats quietly wrong data.

A handle that cannot be reopened stays unallocated, so the guest gets a clean error rather than
reading from the wrong file.

### 3.4 The through-line

Four separate failures, one cause. Every one was **host state describing guest objects**,
invisible to a snapshot of guest RAM:

| Failure | Host state |
|---|---|
| Nothing resumes | Host C++ continuations parked on fibers |
| Corrupt frames, torn memory | Latte thread reading guest memory during MEMR |
| A core spins forever, world dies | `__AXVoicesPerPriority` / `__AXFreeVoices` |
| Streamed audio silently stops | `sFileHandleTable` |

This is the finding, and it generalises past MH3U and past Cemu: in an HLE emulator a RAM
snapshot is necessary and nowhere near sufficient. `save-states-hle-restart-audit.md` classifies
44 blocking calls on the same principle.

---

## 3.5 Tracking next

**Done 2026-09-14, after this was first written:** undo load state. The snapshot is taken
inside the load's own quiesce (the world is already stopped and `DoStateBody` is already
running, so the marginal cost is one capture rather than a second stop-the-world), kept even
when a load fails partway through the body, fingerprint-checked so a stale snapshot cannot be
dropped into a different title, and held in memory only. Undo is itself a load, so it snapshots
on the way through and the action toggles.

**Next: make the fingerprint pin what actually matters.**

A Cemu state is locked to one *build*, and more strictly than a Dolphin state is locked to a
Dolphin version. Dolphin only has to version its serialization format, because a Wii state is
console state and contains no host-derived values. Cemu is HLE, so host-derived values are baked
into guest memory:

- **HLE call indices.** `PPCInterpreter_registerHLECall` assigns them in registration order, and
  they live inside the opcode-1 instruction words patched into guest RPL code. Add an export or
  reorder a library and a saved state's memory is full of calls naming different functions.
- **`SysAllocator` placement**, assigned by static-init order.
- **`RPLLoader_MakePPCCallable` stub addresses**, which is what every service thread's entry
  point is.

None of that is detectable after the fact: the state loads and the game does something insane.

`StateFingerprint` already gates on the build -- but via `BUILD_VERSION_STRING`, which for a dev
build is the git hash. That does not change across rebuilds with uncommitted changes, and five
different binaries carried `bb31c2cf` in one evening. The proxy failed in practice; it is why the
format version had to be bumped mid-session to stop a stale state getting past the header and
dying inside `MEMR`.

The fix is to hash the HLE call table -- its length plus the registered names in order -- into
the fingerprint. The names are retained now (`s_ppcHleNames`, added for the restart logs). That
catches the real incompatibility, catches it across rebuilds of one git hash, and as a bonus
lets states survive rebuilds that do not touch the table, which is most of them. Keep the format
version as well; it is cheap and catches chunk-layout churn. `SysAllocator` layout is a
second-order candidate after that.

Also still open: the known-unfixed list in §8, and a dedicated save state manager window (the
slot submenus cover picking, deleting, to/from file and undo today).

---

## 4. Test procedure

```
bin\Cemu_release.exe -g "D:\Matt\Wii U\Games\MONSTER HUNTER 3 ULTIMATE [Game] [0005000010118300]\code\MH3G_Cafe_US.rpx"
# reach Moga Village
python tools\savestate-probes\menucmd.py 20108     # save
# walk to Moga Woods (a real area transition)
python tools\savestate-probes\menucmd.py 20109     # load
python tools\savestate-probes\alive.py
```

**Pass:** `alive.py` reports `GUEST IS RUNNING` with a non-zero `wakeUpCount` delta, and no
`Crashlog for Cemu` appears in `log.txt`. A healthy baseline is ~36,000 timeslices and ~1.5
billion cycles per 3 seconds.

**Judge it only on that.** See §6.

---

## 5. What the log now tells you

```
Save state: callback-exit stub is at 00e0b488
Save state:   skip host thread 0e314940 entry 00e0b464 in PPCCallback7ff69786fd20
Save state:   restart 2dcb0560 lr 02c070fc sp 39006f18 stack [39002000..39007000] in coreinit.OSSleepTicks
Save state: restarted 12 HLE-blocked threads (13 host threads, 0 not at an HLE call, 0 suspended)
```

Named calls exist because `PPCInterpreter_registerHLECall` previously took a name and threw it
away; it now keeps it (`PPCInterpreter_getHLEName`). Useful well beyond save states — crash
dumps and the debugger get names too.

The split is stable and correct: **13 skipped** service threads, all inside `PPCCoreCallback`
(`{SYS IPC}`, `Alarm Thread`, `Callback Thread`, `GX2 event callback`, `{SYS ProcUI}`,
`{SYS Thread Terminator}`); **12-14 restarted** game threads, all in `OSSleepTicks`,
`OSWaitEvent`, `OSWaitSemaphore`, `OSReceiveMessage` — exactly the audit's SAFE list.

---

## 6. Measurement: three probes that lied

Kept because each cost real time and would cost it again.

| Probe | Why it lied |
|---|---|
| Cemu's FPS readout | Host-vsync driven. Reads a steady 60.00 with the guest completely stopped. |
| Audio still looping | The backend loops its last buffer. Sounds alive; proves nothing. |
| Game field (hunger timer) | Does not tick in the village. Static in a perfectly healthy game. |
| Thread state scan | `READY=0 RUNNING=0 WAITING=27` is *also* what a healthy game looks like at any instant. Comparing fields 2s apart aliases: a thread that wakes, works and re-waits on the same queue looks identical. |

**The only probe that works is `alive.py`**: `OSThread_t.wakeUpCount` (+0x608) is incremented in
`__OSLoadThread` on every timeslice, and `totalCycles` (+0x610) accumulates executed cycles.
Both are monotonic and driven only by real execution.

Also: read the **crash log**, not the process list. Cemu's process lingers for several seconds
after a crash, so "still running" at the 6-second mark meant nothing twice.

---

## 7. Traps hit (do not re-learn these)

- **`context.lr` is big-endian, `context.srr0` is host-endian.** Both declared plain `uint32`,
  adjacent in `OSContext_t`. `__OSThreadStoreContext` swaps one and not the other
  (`coreinit_Thread.cpp:1121` vs `:1140`). Reading `lr` raw yields addresses plausible enough
  to pass an accessibility check, so the bug looks like bad luck rather than byte order.
- **`00e0b45c` is not `00e0b488`.** The first is the thread-entry/exit trampoline that appears
  on every thread's stack and in every service thread's `lr`; the second is the callback-exit
  stub. Conflating them produced a whole wrong theory. The stub address is now logged — check
  it rather than assuming.
- **The recompiler bypasses `PPCInterpreter_virtualHLE`.** Any HLE-dispatch hook must be added
  to `BackendX64.cpp` and `BackendAArch64.cpp` as well, or it silently never runs.
- **Do not walk the guest stack back-chain** to find a callback frame. A thread parked in an
  HLE call need not have `r1` at a well-formed frame — Cemu's own tracer *scores* candidate
  frame pointers instead of trusting it (`coreinit.cpp:60-91`) — and
  `PPCCore_executeCallbackInternal` reserves its save area without writing a back-chain link,
  breaking the chain at exactly the boundary of interest. Scan the live window instead.
- **`pymem`'s `read_region()` is indexed from guest `0x02000000`**, not 0.

---

## 8. Known-unfixed, from the audit

In rough order of when they will bite:

1. **FS wedges permanently after one load.** `numMaxCommandsInFlight` is **1**
   (`coreinit_FS.cpp:303`) and dispatch is gated on `numCommandsInFlight < max` (`:497`). A
   restart that re-increments without a matching decrement stops that `FSClient` dispatching
   forever. Verified in source, and was called near-certain to trigger once the world ran --
   **it did not**. A restored session loaded two areas without trouble on 2026-09-14. The
   hazard is real but needs a command actually in flight at save time; none of the observed
   saves caught one. Do not treat it as fixed.
2. **IOSU host threads are not parked by the quiesce** (`FSAIoThread`, nn service threads).
   They mutate guest memory *during* the 286 MiB `MEMR` restore. Same class as the Latte thread
   problem, and the Latte fix is the template.
3. `__depr__IOS_Ioctlv` (MCP, `nn_act`, `nn_acp`, `nn_nim`) blocks via **self-suspend**, so it
   is invisible to the restart helper, which only looks at `STATE_WAITING`.
4. `OSWaitCond` / `OSFastCond_Wait` release their mutex before blocking and keep
   `prevLockCount` only on the host stack.
5. `nlibcurl.curl_easy_perform` — detached host worker holding pointers into a fiber stack the
   rebuild frees. Host use-after-free.

Superseded from phase 0: the claim that the 13 skipped service threads "never run" was wrong.
They are skipped by the *restart helper*, which only means their guest opcode is not re-invoked
-- the scheduler still dispatches them normally once something signals them, and their rebuilt
fiber re-enters the host body from the top. Alarm, IPC, AX and GX2 threads all demonstrably run
after a load. That re-entry is exactly how the AX bug in §3.1 surfaced, and it is why host
state those bodies depend on has to be made consistent (see `SNDV`).

New, from the same session: **a format mismatch corrupts rather than refuses.** `SNDV` sits
between `ALRM` and `GX2S`, so an older state file fails at that marker *after* `MEMR` has
already overwritten guest memory, and the header's build-version check does not catch it while
the tree is uncommitted at one hash. Chunk compatibility should be validated from the header
before a single byte of memory is touched.

---

## 9. Git state

Nothing in this session is committed. `bb31c2cf` is the last commit (GPU world-stop, cache
drop, run-queue rebuild, root-cause docs). Uncommitted working tree:

- `PPCState.h`, `PPCInterpreterHLE.cpp`, `BackendX64.cpp`, `BackendAArch64.cpp` — entry-SP capture
- `PPCScheduler.cpp` — cached callback-exit stub accessor
- `coreinit_Thread.cpp`, `coreinit_Thread.h` — restart helper, callback detection, serialization
- `SaveState.cpp` — calls the restart helper
- `docs/save-states-hle-restart-audit.md`, this file — new
- `tools/savestate-probes/` — new, the diagnostic scripts
