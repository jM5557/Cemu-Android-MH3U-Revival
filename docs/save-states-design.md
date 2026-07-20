# Save States for Cemu — Design Document

**Status:** Proposal / design only (no implementation yet)
**Scope:** Dolphin-style save states: snapshot the entire running game to a file at any moment, restore it later (same session or a future session), with hotkeys and slots.

---

## 1. Feasibility summary

**Yes, save states are possible in Cemu — but they are architecturally harder than in Dolphin, and the design must lean on the parts of Cemu that make it tractable.**

Dolphin emulates GameCube/Wii mostly at the hardware level: nearly all console state lives in a handful of well-defined blobs (RAM, CPU registers, hardware register blocks, DSP state), so its `DoState()` pass can walk every subsystem and serialize it symmetrically. Cemu is different: it HLEs the entire Cafe OS. A large amount of "console state" is not emulated hardware state but *host-side C++ state* — fibers, host threads, STL containers, open host file handles, GPU objects in VRAM — which cannot be memcpy'd into a file.

The design below works because of four properties of Cemu's architecture (verified against the current code base):

1. **All guest memory lives in one fixed-layout 4 GiB address space** (`memory_base` + fixed per-area offsets, `src/Cafe/HW/MMU/MMU.h`). A save state can capture guest RAM as a set of flat, address-stable region dumps. No pointer relocation is ever needed for guest data.
2. **PPC thread contexts are stored *into guest memory* on every reschedule.** The HLE scheduler (`coreinit_Thread.cpp`, `__OSStoreThread`/`__OSThreadStoreContext`) writes GPRs/FPRs/CR/LR/CTR/etc. into the guest `OSThread_t::context` whenever a thread is switched out. If we quiesce the system at a scheduler boundary, *the CPU state of every guest thread is already inside the RAM snapshot for free.*
3. **The emulated timebase is a virtual accumulated counter, not wall-clock.** `PPCTimer_getFromRDTSC()` (`PPCTimer.cpp`) accumulates RDTSC deltas into `_tickSummary`/`_rdtscAcc`. Serializing a few 64/128-bit counters freezes and restores emulated time exactly; there is no dependency on host absolute time.
4. **Most HLE-internal bookkeeping objects are allocated in guest memory** via `SysAllocator<>` (they live in the `CEMU_PRIVATE` MMU area). Thread queues, run queues, alarm objects, AX voice internals (`AXVPBInternal_t` is `MEMPTR`-based), message queues, mutexes, events, etc. are all captured by the RAM snapshot automatically.

What is *not* captured by a RAM snapshot is the **host-side residue**: fiber objects per guest thread, host alarm timers, open `FSCVirtualFile*` handles, IOSU host threads and their queues, the recompiler code cache, and everything the GPU backend holds in VRAM. The core of this design is a per-subsystem strategy for either (a) serializing that residue, (b) rebuilding it from the restored guest structures, or (c) discarding and lazily regenerating it.

**Honest expectation setting:** phase 1 of this design will boot states reliably for a large class of games with transient visual artifacts after load (render targets regenerate over a few frames). Perfect-fidelity GPU restore and broad compatibility are follow-up phases, exactly as Dolphin's save states took years of per-subsystem `DoState` hardening. Networked titles (e.g. this fork's MH3U revival work) can save, but sockets cannot be restored — connections will be dropped on load (see §7.10).

---

## 2. How Dolphin does it (reference model)

Dolphin's `Core::State` system, summarized, because we borrow its shape:

- A single `PointerWrap p` object is passed through every subsystem's `DoState(p)`; the same code path runs in *read*, *write*, and *measure* modes, guaranteeing symmetric serialization.
- Saving happens on the CPU thread at a safe point ("run as CPU thread" jobs); the GPU thread is synced/drained first.
- Video state: Dolphin serializes FIFO state, XFB/EFB contents, and (optionally) the texture cache; on load it flushes everything not serialized.
- A version number + git revision gate loading; states are compressed (LZO historically, LZ4/zstd now).
- Slots + hotkeys + "undo load state" via an automatic backup state.

We adopt: the single bidirectional stream pass, the "quiesce, then serialize" model, drain-the-GPU-before-save, version gating, compression, slots/hotkeys, and undo-state. We *cannot* adopt: "everything is a register blob" — Cemu's HLE requires explicit rebuild logic per subsystem.

---

## 3. State inventory — where Wii U state actually lives in Cemu

| State | Where it lives | Captured by RAM dump? | Strategy |
|---|---|---|---|
| Game code + data, heaps, stacks | Guest areas: `CODE_*`, `MEM2_DATA`, `MEM1`, `FGBUCKET`, `SHAREDDATA`, `RPLLOADER`, `OVERLAY`, `TILING_APERATURE` | ✅ | Dump mapped `MMURange`s |
| Per-thread CPU contexts (GPR/FPR/CR/…) | `OSThread_t::context` in guest RAM once thread is switched out | ✅ (after quiesce) | Quiesce scheduler, then free |
| Currently-running timeslice register state | Host `PPCInterpreter_t` inside `OSHostThread` | ❌ | Quiesce forces store-to-guest first |
| Scheduler queues (run queues, active thread list) | `SysAllocator` guest objects (`g_activeThreadQueue`, `g_coreRunQueue`) | ✅ | Rebuild host fiber per thread on load |
| Host fibers (`OSHostThread`, `s_threadToFiber`) | Host heap | ❌ | Destroy on load; recreate one fiber per restored guest thread, parked at scheduler entry |
| Emulated time (`OSGetTime`, decrementer, tick counter) | Host globals: `_tickSummary`, `_rdtscAcc`, `ppcCyclesSince2000*`, `ppcMainThreadDEC*` | ❌ | Serialize counters (small POD chunk) |
| Alarms | Guest `OSAlarm_t` objects ✅ + host `OSHostAlarm` set ❌ | partial | Serialize host alarm list (fire tick, period, callback *ID*); recreate on load |
| HLE import trampolines (call indices baked into guest code) | Guest RAM (trampoline area) + host HLE table (`PPCInterpreter_registerHLECall`) | RAM half ✅ | Deterministic re-registration + fingerprint gate (§6) |
| Recompiler code cache | Host | ❌ | Discard; `PPCRecompiler_invalidateRange(0, ~0)` on load, lazy re-JIT |
| GX2 command data / ring buffers | Guest RAM + small host write-gather pointers | mostly ✅ | Drain GPU before save; serialize write-gather cursors |
| Latte GPU register state | Host `LatteGPUState` struct (POD-ish) | ❌ | Serialize register block |
| Textures uploaded from RAM | Host VRAM caches | ❌ | Discard; repopulate from restored RAM |
| Render targets / GPU-only surfaces | Host VRAM only | ❌ | Phase 1: discard (transient artifacts). Phase 3: readback + serialize (§7.7) |
| Audio (AX voices) | Guest RAM (`AXVPBInternal_t` via `MEMPTR`) ✅ + host voice lists of guest pointers | mostly ✅ | Serialize the host lists as guest addresses; re-derive on load |
| Open files (FS) | Guest `FSClient` etc. ✅ + host `FSCVirtualFile*` handle table in `iosu_fsa` ❌ | partial | Serialize per-handle {device, path, access flags, seek offset}; reopen on load |
| IOSU services (act, save, boss, …) | Mix of host statics and guest shared buffers | partial | Per-service `DoState`; most are small POD/statics |
| In-flight IPC / async FS jobs | Host queues + guest request blocks | ❌ | Drain before save (§5.1) |
| Input, audio output devices | Host | ❌ | Not state — reinitialize naturally |
| Network sockets (nsysnet / nlibcurl) | Host OS | ❌ | Cannot restore; invalidate on load (§7.10) |

The single most important consequence of this table: **because thread contexts, synchronization primitives, heaps, and most HLE bookkeeping live in guest RAM, the RAM snapshot does ~90 % of the work.** The remaining host residue is enumerable and bounded.

---

## 4. Architecture overview

```
                 ┌──────────────────────────────────────────┐
                 │       SaveStateService (new)             │
                 │  src/Cafe/SaveState/…                    │
                 │                                          │
  hotkey/menu ──▶│  RequestSave(slot) / RequestLoad(slot)   │
                 │        │                                 │
                 │        ▼                                 │
                 │  Quiesce world (§5.1)                    │
                 │        │                                 │
                 │        ▼                                 │
                 │  StateStream pass over chunks (§5.2)     │
                 │   MEMR  CPUS  TIME  ALRM  OSLB  GX2S     │
                 │   LATT  AXST  FSAS  IOSU  MISC           │
                 │        │                                 │
                 │        ▼                                 │
                 │  zstd-compress → .cst file (§5.3)        │
                 └──────────────────────────────────────────┘
```

### 4.1 The `StateStream` abstraction

Modeled on Dolphin's `PointerWrap`, built on the existing `MemStreamWriter`/`MemStreamReader` (`src/util/helpers/Serializer.h`):

```cpp
// src/Cafe/SaveState/StateStream.h
class StateStream
{
public:
    enum class Mode { Measure, Write, Read, Verify };

    template<typename T>            // POD only, static_assert(std::is_trivially_copyable_v<T>)
    void Do(T& v);
    void DoBytes(void* ptr, size_t size);
    template<typename T> void DoVector(std::vector<T>& v);
    void DoGuestPtr(MEMPTR<void>& p);   // stored as 32-bit guest address — always stable
    void DoMarker(uint32 magic);        // section sentinels; mismatch => hard error with subsystem name

    Mode GetMode() const;
    bool HasError() const;
};
```

Every participating subsystem exposes one symmetric entry point:

```cpp
namespace coreinit { void AlarmDoState(StateStream& s); }
namespace GX2      { void GX2DoState(StateStream& s); }
// …
```

Symmetry (same code path for save and load) is the key defense against the classic save-state bug class of asymmetric read/write drift. `Verify` mode (save, immediately load into a scratch pass, compare) is cheap to add and becomes our primary CI test (§9).

### 4.2 New files

```
src/Cafe/SaveState/
    SaveState.h / SaveState.cpp        // service, orchestration, file I/O, slots
    StateStream.h / StateStream.cpp
    StateFile.h                        // container format structs (§5.3)
    Quiesce.h / Quiesce.cpp            // world-stop protocol (§5.1)
```

Hook points (small edits) in: `CafeSystem.cpp`, `coreinit_Thread.cpp` (quiesce check in scheduler idle loop + fiber rebuild), `coreinit_Alarm.cpp`, `coreinit_FS.cpp`/`iosu_fsa.cpp`, `GX2_Command.cpp`, `LatteThread.cpp`/`LatteCommandProcessor.cpp`, `PPCTimer.cpp`, `ax_ist.cpp`, GUI (`wxgui` main window: menu + hotkeys).

---

## 5. Core mechanics

### 5.1 Quiescing the world (the safe point)

A state may only be taken when no guest thread is mid-timeslice and no host worker is mutating guest RAM. Cemu's cooperative fiber scheduler gives us a natural rendezvous:

1. `SaveStateService` sets `s_stateRequest` (atomic).
2. The scheduler idle loops / `__OSThreadSwitchToNext()` path (all guest execution funnels through here every ~45 000 instructions — `ppcThreadQuantum`) checks the flag. Each of the 1–3 host scheduler threads parks at a barrier *after* `__OSStoreThread()` has run — i.e. after the current thread's full register context has been written into guest RAM and `PPCInterpreter_setCurrentInstance(nullptr)` was called. Interrupt-disabled sections (`OSDisableInterrupts` inflates `remainingCycles`) are naturally respected because the check only happens at the scheduler boundary.
3. Drain asynchronous producers/consumers of guest RAM, in order:
   - **GPU:** stop feeding new data; wait until the Latte command processor has consumed the ring buffer up to the write pointer and the frame in flight has completed (same "sync GPU on save state" Dolphin does). The write-gather cursors are then quiescent and serializable.
   - **IOSU IPC:** wait until all submitted IPC requests have completed and their replies were delivered (queues empty). Async FS commands (`coreinit_FS` async jobs) count as IPC here.
   - **Host alarms:** hold the alarm dispatcher so no `OSHostAlarm` callback fires during the snapshot.
   - **Audio:** park the AX IST processing between frames.
4. Take the snapshot (or restore), release the barrier, resume.

Natural trigger point: the vsync boundary in `LatteTiming` — request the quiesce there so states align with frame boundaries (better UX for TAS-style stepping, deterministic GPU drain point).

Timeout guard: if the world fails to quiesce within ~2 s (a title sitting in an interrupt-disabled spin, a stuck IPC), abort the save with a user-visible message rather than deadlocking.

### 5.2 Chunked snapshot pass

| Chunk | Contents |
|---|---|
| `HEAD` | Format version, Cemu version/commit, title ID + title version, fingerprint (§6), timestamp, screenshot thumbnail |
| `MEMR` | One record per **mapped** `MMURange`: `{areaId, baseAddress, size, data}`. Unmapped/optional areas recorded as absent. Sparse optimization: zero-page skipping per 64 KiB block |
| `CPUS` | Per-core: which guest thread was current (guest address or null), `PPCInterpreterGlobal_t` (`scr`, `car`, `tb`), quantum RNG (`s_lehmer_lcg`), multicore mode flag |
| `TIME` | `_tickSummary`, `_rdtscAcc` remainder, `ppcCyclesSince2000`, `ppcCyclesSince2000_UTC`, `ppcCyclesSince2000TimerClock`, `ppcMainThreadDECCycleValue/Start` |
| `ALRM` | Host alarm list: `{fireTick, period, callbackId, contextGuestAddr}` — callbacks referenced by registered stable ID, never raw pointer (§7.4) |
| `OSLB` | coreinit host statics not covered above: scheduler bookkeeping, `OSGetSystemTime` offsets, IM/FG state, dynload host state (list of loaded RPLs with load addresses) |
| `GX2S` | Write-gather cursors per core, `GX2DrawDone`/flush bookkeeping, event callback registrations (by guest address), occlusion query host mirrors |
| `LATT` | `LatteGPUState` register block, command-processor ring read state, vsync/timing counters, streamout cursors |
| `AXST` | AX host voice lists (as guest addresses), IST frame counter, output mixer state, device remap tables |
| `FSAS` | `iosu_fsa` handle tables: per handle `{clientId, devicePath, accessFlags, seekPos, isDir, dirIterationPos}`; mount table; `coreinit_FS` client/cmdblock host mirrors |
| `IOSU` | Per-service small state: act (account slot selected), save, mcp, boss (task list), pdm, etc. Each service gets its own tagged sub-chunk so unimplemented ones can be added incrementally |
| `MISC` | Input mapping snapshot not needed; RNG seeds used by HLE (`s_lehmer_lcg` already in CPUS); padscore/vpad sampling counters; anything found during hardening |

Rule: **every chunk begins and ends with `DoMarker()`**, so a drifted deserializer fails loudly at the exact subsystem, not 200 MB later.

### 5.3 Container format & size

```
File: <cemu>/savestates/<titleId>/<slot>.cst

[FileHeader]     magic 'CSTA', format version, flags, uncompressed size
[HEAD chunk]     stored uncompressed (readable without full decompress → slot browser UI)
[zstd frame]     all remaining chunks, compressed as one stream (level 3 default)
```

- Worst-case raw size is bounded by mapped areas (MEM2 1 GiB dominates), but titles rarely dirty all of it; zero-block skipping + zstd typically lands states at 100–400 MB. Async compression on a worker thread keeps the pause under ~1 s for saves (the copy-out must happen inside the quiesce; compression must not).
- Slots 1–10 + `auto` + `undo` (automatic backup of the pre-load state, restoring Dolphin's "undo load state" affordance).

---

## 6. The determinism fingerprint (HLE's version of Dolphin's version gate)

HLE calls are dispatched through indices (`HLEIDX`) baked into guest trampoline code at title load; `SysAllocator` objects get their guest addresses from static-init order; graphic pack patches rewrite guest code. A state is only loadable into an *identical* HLE world. Loading therefore requires:

- same Cemu build (commit hash),
- same title ID + title version + applied update/DLC,
- same set of enabled graphic pack **patches** (hash of applied patch bytes),
- same multicore setting (single/dual/triple-core affects scheduler shape).

All four are hashed into `HEAD.fingerprint`. On mismatch: refuse load with a precise reason. This is stricter than Dolphin (which only gates on version) but is what makes the "RAM snapshot carries baked HLE indices" approach sound. A future relaxation (re-linking trampolines on load) is possible but explicitly out of scope.

Load-into-running-title is the only supported path in phase 1: the user must have the same title running; `RequestLoad` then reuses the already-built deterministic host world (module loading, HLE registration, `SysAllocator` placement) and only replaces RAM + rebuilds residue. Loading a state from a cold start = auto-launch title, wait for `LaunchForegroundTitle` to reach the first vsync, then perform the same in-place restore.

---

## 7. Per-subsystem restore plans

### 7.1 Memory (`MMU`)
Save: dump each mapped `MMURange`. Load: verify identical layout (fingerprint makes this near-certain), map any optional areas that were mapped at save time (e.g. overlay arena), then bulk-copy. No relocation ever — `memory_base`-relative layout is fixed by design.

### 7.2 Threads & scheduler (`coreinit_Thread`)
The delicate one, but well-contained:

Load sequence (inside quiesce, all scheduler host threads parked at the barrier):
1. Destroy every existing `OSHostThread` fiber and clear `s_threadToFiber` (all fibers are parked at scheduler boundaries, so destruction is safe).
2. Restore RAM (§7.1). Guest now contains the saved `g_activeThreadQueue`, run queues, and every `OSThread_t` with its full stored context.
3. Walk `g_activeThreadQueue`; for each live guest thread create a fresh `OSHostThread`. New fibers start at `__OSFiberThreadEntry`, which already begins with `__OSLoadThread()` → loads the guest context and runs — i.e. **a freshly created fiber and a restored thread are indistinguishable**; this is the same path used when a thread gets its first timeslice today.
4. Release the barrier; scheduler picks threads off the restored run queues normally.

The only host-side scheduler state to serialize is trivia: quantum RNG, multicore flag, per-core current-thread (which after quiesce is always "none" — an invariant we assert).

### 7.3 Time (`PPCTimer`, `coreinit_Time`)
Serialize the accumulated counters listed in §5.2/`TIME`. On load, also reset `_rdtscLastMeasure = __rdtsc()` so the next delta starts from zero. Emulated clock resumes exactly where it stopped; titles perceive no discontinuity. (`ppcCyclesSince2000_UTC` restoration means the in-game wall clock resumes from save time — matching Dolphin's behavior with `OSTicks`; acceptable and consistent.)

### 7.4 Alarms (`coreinit_Alarm`)
Guest `OSAlarm_t` objects and their queues live in guest RAM (restored for free). Host `OSHostAlarm` objects hold `{nextFire, period, C++ callback, context}`. Raw function pointers must never enter a state file: introduce a tiny registry — each host alarm callback site registers a stable `AlarmCallbackId` at startup (`_OSSleepTicks_alarmHandler`, AX frame alarm, vsync alarm, …; the set is small and static). Serialize `{fireTick, period, callbackId, contextGuestAddr}`; on load, re-create host alarms via the registry. Contexts are guest addresses in all current uses (e.g. `_threadQueue.GetPointer()`), which restore trivially.

### 7.5 HLE oslib / dynload
Baked trampoline indices are covered by the fingerprint (§6). `OSDynLoad` of RPLs *at runtime* changes the RPL loader's host module list, so `OSLB` serializes the loaded-module list `{moduleName, loadBase, linkState}`; on load, compare against the live list and (phase 1) refuse the state if a runtime-loaded module set differs from the running world — replaying module loads is a phase-3 improvement. In practice most titles load all RPLs at boot, so the lists match.

### 7.6 Recompiler
Discard entirely on load: `PPCRecompiler_invalidateRange(0, 0xFFFFFFFF)` (API already exists), let it re-JIT lazily. Costs a few seconds of reduced performance post-load; zero correctness risk. Same policy Dolphin applies to its JIT cache.

### 7.7 GPU (`GX2` + `Latte`)
Save side is made easy by the drain (§5.1): after the ring buffer is fully consumed and the in-flight frame completed, GPU state = Latte register block + small host cursors (`GX2S`/`LATT` chunks) + whatever is in guest RAM (command buffers, shaders, textures, vertex data — all restored with RAM).

Load side, phase 1 (**cache-drop strategy**):
- Drop texture cache, buffer cache, cached FBOs, shader binding state; keep compiled shader/pipeline caches (they're keyed by content hash — still valid and expensive to rebuild).
- Restore the Latte register block and ring/write-gather cursors.
- Consequence: surfaces whose only copy lived in VRAM (render targets, GPU-composited buffers) come back stale/black until the game redraws them — typically 1–2 frames for the main scene; longer for accumulation-style effects (e.g. static UI drawn once, photo snapshots). This is the same class of artifact early Dolphin states had before EFB serialization.

Phase 3 (**fidelity strategy**): enumerate live render-target surfaces in the texture cache at save time, use the existing readback path (`LatteTextureReadback.cpp`) to pull them into CPU memory, store them in an optional `RTGT` chunk, and re-upload on load. This mirrors Dolphin's texture-cache/EFB serialization and closes the visual gap; it's isolated behind the chunk system so it ships independently.

### 7.8 Audio (`snd_core` / AX)
Voice state (`AXVPBInternal_t`, `AXVPB`) is guest memory. Host residue is the priority-bucketed voice lists (`AXVoiceList_*` — vectors of guest pointers) and IST counters: serialize as guest addresses + PODs in `AXST`, rebuild vectors on load. Host audio output device is not state — reset/flush the output buffer on load (brief silence, then resumes).

### 7.9 Filesystem (`fsc` / `iosu_fsa` / `coreinit_FS`)
Guest-visible objects (FSClient/FSCmdBlock) are guest RAM. Host residue = the `iosu_fsa` handle table mapping `FSResHandle → FSCVirtualFile*`.

- Save: for each open handle record `{handle, clientId, virtualPath, accessFlags, seekPos, dirPos}`. `FSCVirtualFile` already exposes `fscGetSeek`; add a `fscGetPath()`/stored-path field at open time (small change in `fsc.cpp` open path).
- Load: close all current handles, reopen each recorded path through `fsc_open` with original flags, `fscSetSeek` to the recorded offset, and install into the table at the *same* handle value (the handle values are baked into guest RAM).
- Failure to reopen (file deleted between sessions) → refuse the load with a clear message, before touching the running world (do a dry-run reopen pass first).
- Save-directory writes that happened *after* the state was taken are simply visible to the restored game — identical to Dolphin's behavior with the Wii NAND, and acceptable (documented caveat: loading an old state after newer in-game saves can confuse titles that cache save-file state; the dominant use case, retry loops within a session, is unaffected).

### 7.10 Network (`nsysnet`, `nlibcurl`, `nn_*` online)
Sockets, TLS sessions and HTTP transfers are host-OS objects that cannot be checkpointed. Policy (same as Dolphin's netplay-adjacent stance):
- Saving is always allowed.
- On load, every guest socket descriptor that was open at save time is transitioned to an errored/closed state so the next guest operation returns a clean error (`ECONNRESET`-equivalent); nn services get a "connection lost" style notification where the HLE supports it. Games universally handle console network loss.
- The UI warns when loading a state that had live connections. For this fork's MH3U revival online play specifically: save states while in an online lobby will drop the session on load — by design.

### 7.11 IOSU services
Each service (`act`, `save`, `boss`, `pdm`, `mcp`, …) gets an optional tagged sub-chunk. Most hold small static tables (account info, task lists) that serialize in a few lines. Services with no chunk yet are *reset to their boot state* on load — with the note that anything they had cached for the title is rebuilt through normal IPC. This incremental model is what lets phase 1 ship without touching all ~20 services.

---

## 8. UI / UX

- **Hotkeys** (configurable, Dolphin defaults): `F1` load slot, `Shift+F1` save slot, `F2–F8` direct slots, hotkeys for slot cycling.
- **Menu**: `File → Save State → Slot 1–10 / Save to file…`, `File → Load State → …`, `Undo Load State`.
- Slot browser shows the `HEAD` thumbnail + timestamp (readable without decompressing the body).
- Non-blocking toasts via the existing overlay (`LatteOverlay`) for "State saved (slot 3)" / errors.
- Auto-save slot on a timer (opt-in), mirroring Dolphin.

---

## 9. Testing strategy

1. **Symmetry harness (CI-able):** run a headless title to frame N, save, `Verify`-mode pass (save → load into scratch → byte-compare every chunk). Catches asymmetric `DoState` drift — the #1 source of savestate corruption in every emulator.
2. **Save/load/save determinism:** save A at frame N, load A, immediately save B; A and B must be byte-identical outside the header. This is the gold-standard invariant (Dolphin enforces the same in its TAS workflows).
3. **Soak:** scripted load-every-30 s loops on a compatibility set (a few first-party titles + MH3U for this fork), watching for asserts (`cemu_assert_debug` density in the scheduler is high — it will catch quiesce-invariant violations early).
4. **Cross-session:** save, full Cemu restart, load.

---

## 10. Phased delivery plan

| Phase | Contents | Outcome |
|---|---|---|
| **0 — Infrastructure** | `StateStream`, container format, quiesce protocol, `MEMR`+`CPUS`+`TIME` chunks, fiber rebuild, recompiler flush, GPU cache-drop, in-session load only, single slot, no UI polish | States work in simple titles; visual artifacts on load; the hard invariants (quiesce, thread rebuild) proven |
| **1 — Correctness breadth** | `ALRM`, `AXST`, `FSAS` (+dry-run reopen), `GX2S`/`LATT`, socket invalidation, fingerprint gating, cross-session load, undo-state | Most offline titles save/load reliably; artifacts limited to stale render targets |
| **2 — UX** | Slots, hotkeys, thumbnails, menu, overlay toasts, auto-save | Feature parity with Dolphin's UX |
| **3 — Fidelity** | `RTGT` render-target serialization via readback, per-service IOSU chunks, runtime-RPL replay, compression tuning / dirty-page tracking for sub-100 ms saves | Near-artifact-free loads; fast enough for TAS-style use |

Effort calibration: phase 0 is dominated by the quiesce protocol and thread-fiber rebuild (the two places where a subtle bug deadlocks or corrupts), not by serialization volume. Phases 1–3 are wide but shallow — each subsystem's `DoState` is small once the pattern exists.

---

## 11. Key risks

| Risk | Mitigation |
|---|---|
| Quiesce deadlock (title in interrupt-disabled spin; stuck IPC) | Timeout + abort save; never block the GUI thread on the barrier |
| Hidden host state not in the inventory (a static someone adds later) | `DoMarker` hard-fails per subsystem; symmetry harness in CI; code-review rule: new host-side statics in `Cafe/` need a `DoState` or an explicit "not state" comment |
| HLE index / layout drift across builds | Fingerprint refuses load; states are explicitly build-locked (documented) |
| Render-target loss confuses specific titles (read-back-from-framebuffer tricks) | Phase 3 `RTGT`; per-title compatibility notes until then |
| Multicore timing: restored run queues assume 3 scheduler threads | Multicore mode is part of the fingerprint |
| Large state files | zstd + zero-block skip; header readable without decompress; future dirty-page tracking |

---

## 12. Verdict

Save states in Cemu are **feasible and worth building**. The HLE architecture that makes them *look* impossible actually cooperates in the ways that matter most: guest RAM is one flat address-stable space, every thread's CPU context is parked in that RAM at scheduler boundaries, and emulated time is already virtualized. The genuinely hard engineering is (a) a correct world-quiesce protocol, (b) rebuilding host fibers/handles/alarms from restored guest structures, and (c) accepting — then progressively closing — the GPU fidelity gap, in exactly the order Dolphin historically did.
