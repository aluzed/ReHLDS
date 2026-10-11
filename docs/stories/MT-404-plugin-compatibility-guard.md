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

* [ ] With ReAPI hooking `SV_EmitPings`, the server runs serial encode and logs why.
* [ ] Compatibility matrix documented in `docs/` with results.
* [ ] No crash in a 1-hour `cs32` run with the matrix plugins and `sv_parallel_send 1`.

## Out of scope

* Making AMXX itself thread-safe.
