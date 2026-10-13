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
* Not done: assertions in `callChain`, the "thread-safe hook" API extension, the
  compatibility matrix and the 1-hour run (field validation, MT-702).

### Encoders recorded during the gather (branch `exp/mt404-encoders-in-gather`)

Experiment, not merged. The main thread runs the conditional encoders while it gathers
each snapshot (`SV_RecordEntityEncoders`, `sv_main.cpp`), with the same (from, to) pairs
and in the same order as `SV_CreatePacketEntities_internal`, and records the fields they
set and unset (`encoder_record_t`, `server.h`). The workers replay the record
(`DELTAJit_CallConditionalEncoder`) and never call game DLL code: an encoding thread
without a matching record changes nothing and hands the snapshot back to the main
thread, which encodes it once the gather is done (the snapshot's copy of the event queue
is restored first: emitting the events changes it). Encoders cannot read the marks, so
the replayed marks are the same. The workers encode each snapshot as soon as it is
gathered again, and the encoder lock is gone.

* Full updates (`SV_FindBestBaseline`) are not recorded: one snapshot per connection.
* Replay of a serial `replay` session with 2 and 3 workers: identical (24 005 packets),
  0 replay misses. With every replay forced to miss (temporary build): identical.
* `mapchange_check.sh`, 8 map changes with Metamod-R, AMX Mod X and bots: no crash.
  Field server with CSDM: stable.
* Unit tests: concurrent replay gives the serial bits and never calls the encoder off
  the main thread; a replay that does not match changes nothing.

`clients32-ents`, 3 runs each, `-netthread +sys_scheduler 1 +sv_rehlds_snapshot_spread 1`,
2 workers:

| | Serial | Parallel, encoders after the gather | Parallel, encoders recorded |
|---|---|---|---|
| Encode on the main thread avg / p99 | 23.8 / 150 µs | 20.9 / 100 µs | 19.2 / 90 µs |
| `send_messages` avg / p99 | 49.8 / 287 µs | 47.5 / 230 µs | 47.3 / 229 µs |
| Gather avg | 17.4 µs | 17.2 µs | 18.7 µs |
| Process CPU | 24.0 % | 30.9 % | 29.4 % |
| Frame avg | 206 µs | 201 µs | 203 µs |

The overlap saves about 2 µs of encoding on average and 10 µs at p99, and the recording
costs 1.5 µs of gather: on this benchmark the frame does not change. The encoding cost
it targets is small since the load client was fixed (the "40 % to 13 %" above was
measured with the broken client). What it brings is that game DLL and Metamod code
never runs on an encoding thread. Heavier snapshots (a busy public server) are the
field validation's to measure.

