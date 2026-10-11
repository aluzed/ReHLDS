# MT-402 — Thread-safe delta encoding

**Epic:** E4 Parallel snapshots · **Priority:** P0 · **Size:** L · **Depends on:** MT-203, MT-401

## Context

Delta encoding (`engine/delta.cpp`, JIT in `engine/delta_jit.cpp`) is the core of the
encode phase. Shared state found so far:

* the global bit writer (handled by MT-203);
* `delta_t` descriptors are shared between all clients **and mutated during encoding**:
  field flags (`FDT_MARK`) are set by `DELTA_MarkSendFields` /
  `DELTA_CheckDelta`, and game DLL `conditionalencode` callbacks
  (`engine/delta.cpp:579`, registered by `DELTA_AddEncoder`) call
  `DELTA_SetField`/`DELTA_UnsetField` on the shared descriptor;
* `g_TimeWindowBase`/`g_TimeWindowOffset` statics (`engine/delta.cpp:317`);
* the delta JIT registry (`g_DeltaJitRegistry`) and its generated code: verify the
  generated code only writes to caller-provided buffers;
* `SV_FindBestBaseline` / instanced baselines (read-only during the frame — verify).

The `conditionalencode` callbacks are game DLL code (`Entity_Encode`, `Player_Encode`,
`Custom_Encode` in the HLSDK / ReGameDLL). In the HLSDK they are effectively pure (they
read entity states and toggle fields) except for a lazy `static` initialisation of field
indices.

## User story

As a developer, I want delta encoding to be callable concurrently for different clients,
so that the encode phase can run on the worker pool.

## Scope

* Move per-call markings out of `delta_t`: introduce a per-call `DeltaEncodeState`
  (send-field bitmask, sized to `delta_t::fieldCount`) passed through
  `DELTA_CheckDelta`, `DELTA_MarkSendFields`, `DELTA_WriteMarkedFields` and the JIT path.
  `DELTA_SetField`/`DELTA_UnsetField`/`DELTA_SetFieldByIndex`/`DELTA_UnsetFieldByIndex`
  (exported to the game DLL through `g_engfuncs`) operate on the current thread's state,
  found through the active encode context.
* `conditionalencode` handling, in order of preference:
  1. run the conditional encoders during the **gather** phase on the main thread,
     storing their effect (field mask adjustments per entity per client) in the job —
     this keeps game DLL code on the main thread, at the cost of computing deltas twice
     or precomputing `from`/`to` pairs during gather;
  2. if (1) costs too much, an opt-in cvar `sv_parallel_encoders 1` allowing encoders to
     run on workers for known-pure game DLLs (ReGameDLL, HLSDK-based), after a warm-up
     call on the main thread to trigger their static initialisation.
* Remove or make thread-local the statics in `delta.cpp` used during encoding.
* Delta JIT: document and test reentrancy.
* Stats counters in `delta_t` (`stats.sendcount`, `stats.receivedcount`) become
  per-thread and are summed in the commit phase (or dropped when threaded).

## Acceptance criteria

* [ ] Unit test (`rehlds/unittests/delta_tests.cpp` extended): N threads encoding
      different entity pairs with the same descriptor give the same bits as serial.
* [ ] With CS 1.6 (ReGameDLL and original `mp.so`) the replay output is identical.
* [ ] JIT and non-JIT builds both pass.
* [ ] No `delta_t` field written during the encode phase (checked by a debug build that
      write-protects descriptors with `mprotect` during the parallel section).

## Risks

* Game DLLs with impure encoders (custom mods): option 1 guarantees correctness; option 2
  is opt-in only.
* ABI: `delta_t` layout is visible to the game DLL through engine functions; do not
  change its layout — keep the old flags field and simply stop using it while threaded.

## Out of scope

* Changing delta.lst or the wire encoding.
