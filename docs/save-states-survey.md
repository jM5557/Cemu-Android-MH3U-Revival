# Save States — code survey vs. design doc

**Date:** 2026-09-13
**Surveyed tree:** `savestates` branch @ upstream `main` `3310f3b8`
**Doc under review:** `docs/save-states-design.md` (written against `3163f353`, 78 commits older)

Purpose: verify the design doc's architectural claims against the real code before
phase 0 starts. Every line reference below was read, not inferred.

---

## 1. Claims that hold (with exact symbols)

| # | Doc claim | Verified at | Notes |
|---|---|---|---|
| 1 | Flat, address-stable guest space (`memory_base` + fixed offsets) | `Cafe/HW/MMU/MMU.h:19,60,279` | Holds. |
| 2 | Thread CPU context is written *into guest RAM* on reschedule | `coreinit_Thread.cpp:1103` `__OSThreadStoreContext`, `:1132` `__OSStoreThread`, `:1161` `PPCInterpreter_setCurrentInstance(nullptr)` | Holds — this is the load-bearing claim of the whole design. |
| 3 | A fresh fiber and a restored thread are indistinguishable | `coreinit_Thread.cpp:1342/1346` `__OSFiberThreadEntry` → `:1354` `__OSLoadThread` | Holds exactly as §7.2 describes. |
| 4 | Emulated time is a virtual accumulator, not wall-clock | `PPCTimer.cpp:12` `_rdtscLastMeasure`, `:23` `_rdtscAcc` (uint128), `:85` `_tickSummary`, `:127` `PPCTimer_getFromRDTSC()` | Holds; the `TIME` chunk field list matches real symbol names. |
| 5 | HLE bookkeeping lives in guest memory via `SysAllocator` | `MMU.cpp:115` `mmuRange_CEMU_AREA` (0x0E000000, 32 MiB, `FLAG_MAP_EARLY`, comment: *"Should be allocated early for SysAllocator"*); `coreinit_Thread.cpp:71` `SysAllocator<OSThreadQueue> g_activeThreadQueue` | Holds. |
| 6 | Host fiber residue is enumerable | `coreinit_Thread.cpp:81` `OSHostThread`, `:97` `s_threadToFiber`, `:1478-1483` bulk teardown | Holds; teardown loop already exists. |
| 7 | Host alarms are a discrete set needing a callback registry | `coreinit_Alarm.cpp:20` `class OSHostAlarm`, `:147/152` `static std::set<OSHostAlarm*, ComparatorFireTime> g_activeAlarmList` | Holds. |
| 8 | Recompiler cache can be dropped wholesale | `PPCRecompiler.h:163` `PPCRecompiler_invalidateRange(uint32,uint32)` | Holds; API present as described. |
| 9 | AX host residue = priority-bucketed vectors of guest pointers | `ax_voice.cpp:73` `AXVoiceList_GetListByPriority` → `std::vector<AXVPB*>&`, `:79` `AXVoiceList_Reset` | Holds. |
| 10 | Latte state is a serializable register block | `Latte.h:17` `struct LatteGPUState_t`, `:71` `extern LatteGPUState_t LatteGPUState` | Holds. |

**Greenfield confirmed:** no pre-existing save-state code anywhere in `src/`.

---

## 2. Better than the doc assumed

**`MEMR` has a ready-made enumeration API.** The doc describes dumping "each mapped
`MMURange`" as if the set must be hand-listed. It does not:

- `MMU.cpp:67` `std::vector<struct MMURange*> g_mmuRanges;`
- `MMU.cpp:69` `std::vector<MMURange*> memory_getMMURanges()`
- `MMU.cpp:86` — every `MMURange` **self-registers in its constructor**
- `MMU.h:107` `bool isMapped()`

So the entire `MEMR` chunk is `for (auto* r : memory_getMMURanges()) if (r->isMapped()) …`.
New areas added upstream are picked up automatically — one less drift risk.

---

## 3. Corrections the doc needs

All four items below **predate the doc's own base commit**, i.e. they are inaccuracies in
the original survey, not upstream drift. Treat the doc's file/line specifics as
approximate and re-verify while implementing.

### 3.1 FSA lives elsewhere, and there are two tables
Doc says `iosu_fsa` (implying `Cafe/IOSU/legacy/`). Actual: **`Cafe/IOSU/fsa/iosu_fsa.cpp`**.
There are **two** handle tables, not one:
- `:235` `_FSAHandleTable sFileHandleTable;`
- `:236` `_FSAHandleTable sDirHandleTable;`
- `:232` each is `std::array<_FSAHandleResource, 0x3C0>`

### 3.2 The `FSAS` chunk field list is incomplete — restored handles would all be rejected
`_FSAHandleResource` (`:172-177`) is `{bool isAllocated; FSCVirtualFile* fscFile; uint16 handleCheckValue;}`
and a handle is composed at `:196` as `((uint32)i << 16) | (uint32)checkValue`.

`GetByHandle` (`:216-229`) validates **both** the index slot *and* `handleCheckValue`.
The doc's §7.9 plan ("install into the table at the *same* handle value") therefore also
requires serializing, per slot, the `handleCheckValue`, plus the table-level
`:231 uint32 m_currentCounter`. None of these appear in the doc's `FSAS` field list
(`{clientId, devicePath, accessFlags, seekPos, isDir, dirIterationPos}`). Omit them and
every restored handle fails validation.

The doc's other FS note is confirmed correct: `FSCVirtualFile` stores no path
(`fsc.h:133/138` expose only `fscSetSeek`/`fscGetSeek`), so an open-time path field really
must be added.

### 3.3 Missing host state: the active-thread registry
Doc §7.2 asserts host-side scheduler state is only "quantum RNG, multicore flag,
per-core current-thread". There is a third item:

- `coreinit_Thread.cpp:48` `MPTR activeThread[256];`
- `coreinit_Thread.cpp:49` `sint32 activeThreadCount = 0;`
- declared `coreinit_Thread.h:627-628`, guarded by `:630 SlimRWLock srwlock_activeThreadList`

This is a **host** array of guest pointers, distinct from the guest-side
`g_activeThreadQueue` (`:71`). It is cheap to handle — serialize the MPTRs, or rebuild by
walking `g_activeThreadQueue` — but it must be an explicit decision. Left alone, a
restored world comes back with an empty active-thread registry.

This is exactly the failure class the doc's own §11 names ("hidden host state not in the
inventory"), found on the first pass — which argues for making the `DoMarker`/symmetry
discipline real from day one rather than in phase 1.

### 3.4 The GUI is no longer wx-only
Doc §4.2 hooks "GUI (`wxgui` main window: menu + hotkeys)". `src/gui/` is now split:
- `gui/interface/WindowSystem.h` (126 lines) — backend-agnostic namespace
- `gui/wxgui/` — the wx implementation

landed by `67de63be` *"UI+build: Isolate wxWidgets code from non-GUI code (#1633)"*.
Menu and hotkey wiring still belongs in `wxgui/`, but anything the save-state **core**
needs to tell the GUI must go through the `WindowSystem` namespace or it re-couples
non-GUI code to wx. The doc's choice of `LatteOverlay` for toasts already avoids this.

---

## 4. Net assessment

The design's four load-bearing assumptions (§1.1–1.4) all hold, and the most
implementation-heavy one — `MEMR` — is easier than written thanks to `memory_getMMURanges()`.
Nothing found invalidates the phased plan.

The corrections are all in the "wide but shallow" phase-1 subsystems, not in phase 0's
hard core (quiesce + fiber rebuild), which surveyed clean. Phase 0 can start on the
design as written, with §3.3 folded into the `CPUS` chunk.
