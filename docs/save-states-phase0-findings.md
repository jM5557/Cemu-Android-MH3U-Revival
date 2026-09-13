# Save States — phase 0 results and findings

**Date:** 2026-09-13
**Branch:** `savestates` (upstream `main` @ `3310f3b8` + this work)
**Tested with:** Monster Hunter 3 Ultimate (US v32, `0005000010118300`), Vulkan, multi-core recompiler

Companion documents: `save-states-design.md` (the plan), `save-states-survey.md` (code survey).

---

## 1. Status in one line

**Save and restore of the entire CPU-side world works in real gameplay. The GPU does not
come back — after a load the emulator runs but the display stays frozen on the last
pre-load frame.**

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

## 4. The open problem

After a load: **emulator alive, FPS counter reads 60, audio is the restored world's audio,
display frozen on the last frame rendered before the load.**

Measured rigorously — three screenshots seconds apart, cropped to the game viewport
(excluding the title bar, whose FPS text changes constantly) are pixel-identical.

> Methodology note: an earlier full-window diff appeared to show live frames and led to a
> wrong conclusion. Always crop to the viewport when testing this.

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

## 6. Leading hypothesis for the freeze (NOT yet verified)

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

Test that actually discriminates: save in Moga Village, walk to Moga Woods (an area
transition, not just a few steps), load, and check whether the *picture* returns — not just
the audio.

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
