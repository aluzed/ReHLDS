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
* Not done: assertions in `callChain`, the "thread-safe hook" API extension, the
  compatibility matrix and the 1-hour run (field validation, MT-702).
