# MT-404 — Plugin compatibility guard and serial fallback

**Epic:** E4 Parallel snapshots · **Priority:** P0 · **Size:** M · **Depends on:** MT-403

## Context

ReHLDS exposes hookchains used by ReAPI / AMXX / Metamod plugins, several of them on the
encode path: `SV_CreatePacketEntities`, `SV_EmitEvents`, `SV_EmitPings`,
`SV_WriteFullClientUpdate`, `SV_Frame` (`rehlds/rehlds/rehlds_api_impl.h`). Plugin code is
not thread-safe (AMXX's Pawn VM certainly is not). `IHookChainRegistryImpl::isEmpty()`
(`rehlds/rehlds/hookchains_impl.h:141`) tells whether a hookchain has subscribers.

## User story

As a server operator using plugins, I want threading to turn itself off for any part a
plugin hooks, so that enabling `sv_parallel_send` can never crash my server.

## Scope

* Before each parallel section, check the hookchains used in the encode path; if any is
  non-empty, run that frame's encode serially (log once per map with the hook names).
* Assertion layer: every `callChain` asserts `IsMainThread()` in `REHLDS_THREAD_CHECKS`
  builds (MT-202).
* Engine functions exported to plugins/game DLL (`g_engfuncs`, `IRehldsServerData`,
  `IGameClient` ...) that could observe mid-encode state: document them as
  "main thread only" in `rehlds/public/rehlds/` headers.
* Optional ReHLDS API extension (minor version bump, backward compatible): let a plugin
  declare a hook as "thread-safe" so that the guard does not disable parallelism for it.
* Compatibility matrix tested: Metamod-R, AMX Mod X 1.9/1.10, ReAPI, ReGameDLL,
  Reunion, popular plugins (e.g. semiclip, which hooks `AddToFullPack`).

## Acceptance criteria

* [x] With ReAPI hooking `SV_EmitPings`, the snapshots with pings are encoded serially
      and `sv_parallel_send_stats` says why (`tools/bench/plugins/bench_hooks.sma`).
* [x] Game DLL conditional encoders, which call the engine back through Metamod, never
      run while the main thread runs game DLL or Metamod code: no crash in 8 map changes
      with Metamod-R, AMX Mod X, bots and playing clients (`tools/bench/mapchange_check.sh`,
      which crashed the engine before the fix at the 2nd).
* [x] Hookchains, conditional encoders and their engine callbacks checked at run time
      (`sys_thread_checks`): no report in 8 map changes, reports from the first frames
      when the encoders are forced to overlap the gather.
* [ ] Compatibility matrix documented in `docs/` with results.
* [ ] No crash in a 1-hour `cs32` run with the matrix plugins and `sv_parallel_send 1`.

## Out of scope

* Making AMXX itself thread-safe.

## Status

Done for the guard:

* `SV_CreatePacketEntities` or `SV_EmitEvents` hooked: every snapshot is encoded on the
  main thread (logged once when the state changes, and by `sv_parallel_send_stats`).
* `SV_EmitPings` hooked (the only one of the three that ReAPI exposes, used by scoreboard
  plugins): only the snapshots that carry pings (players holding the scoreboard key,
  HLTV every 2 s) are encoded on the main thread, after the others.
* Console output from the encode path is deferred to the main thread, so `Con_Printf`
  hooks still run there.
* Game DLL conditional encoders (`Entity_Encode`, `Player_Encode`) run on the encoding
  threads, one at a time. They call the engine back (`DELTA_SetFieldByIndex`,
  `GetCurrentPlayer`...) through the game DLL's engine function table, which Metamod-R
  points to its JIT callbacks as soon as a plugin provides engine functions (AMX Mod X
  always does). Those callbacks save, change and restore `g_metaGlobals` (hook results,
  `esp_save`) without any lock: with the encoders running while the main thread gathered
  the next snapshot through Metamod, the restores interleaved and left `esp_save` pointing
  into an encoding thread's stack. At the next map change, Metamod-R rebuilt its
  callbacks and rewrote "return addresses" found from that stale pointer: heap
  corruption, crash (`malloc(): unsorted double linked list corrupted`) on the field
  server, and crashes on `quit` in the benchmark. Found from the field crash core: three
  freed 16-byte chunks with their first word decremented by one, held by a Steam client
  object, and a reproduction at the first map change with load clients.
  Fix: when one of the encode path's deltas has a conditional encoder, the snapshots are
  published to the workers once every snapshot of the frame is gathered; the workers are
  woken at the first one and wait. In `ParallelSend_Finish`, the main thread only
  encodes, so the encoders never run alongside game DLL or Metamod code. Cost: the
  gather no longer overlaps the encoding, the send gain drops from 40 % to 13 % on
  average (28 % at p99) on `clients32-ents` ([`../benchmarks/clients32-ents.md`](../benchmarks/clients32-ents.md)).
  Restoring the overlap needs the encoders out of the encode phase, for instance run
  during the gather on the main thread with their marks stored in the snapshot.
* Helgrind did not see the race: it runs one thread at a time and the main thread then
  encodes nearly every snapshot itself. `race_check.sh` now loads Metamod and counts the
  reports on threads the engine created, and `mapchange_check.sh` covers this path.
* Run-time checks (`sys_thread_checks`, `rehlds/rehlds/thread_checks.{h,cpp}`), in
  release builds, on by default. In the parallel window (`ParallelSend_Begin` to the end
  of `ParallelSend_Finish`) one thread owns game DLL code: the main thread while it
  gathers, then, once it has published every snapshot, the thread running a conditional
  encoder (checked under the encoder lock). Reported: a conditional encoder entered while
  another thread owns game DLL code, an engine function the encoders call back
  (`DELTA_SetField*`, `DELTA_UnsetField*`, `DELTA_FindFieldIndex`, `GetCurrentPlayer`)
  on a worker that does not own it, a hooked ReHLDS hookchain called on a worker
  (`callChain`). `sys_thread_checks 2` aborts on the first one.
  Checked both ways with `mapchange_check.sh` (Metamod-R, AMX Mod X, bots, playing
  clients): the engine of this story with `sys_thread_checks 2` changes the map 8 times
  without a report; the same engine forced to overlap the gather with the encoders (the
  bug above) reports from the first frames (23 violations in the first 10 s, 190 000 by
  the crash at the first map change). Replays byte-identical. Cost: on `clients32-ents`
  (3 runs each, `sv_parallel_send 1`),
  snapshot send 48.3 / 230 µs avg / p99 without the checks, 48.7 / 249 µs with them
  (encode p99 99 / 113 µs); frame, tick interval and CPU unchanged. The p99 difference
  shows in every run (233, 234, 223 against 255, 249, 244 µs).
  Limits: the main thread's game DLL calls are not checked one by one (the whole gather
  counts as game DLL code), and the engine functions the encoders do not use are not
  checked on the workers.
* Not done: the "thread-safe hook" API extension, the compatibility matrix and the
  1-hour run (field validation, MT-702).
