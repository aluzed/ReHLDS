# MT-401 — Split `SV_SendClientMessages` into gather / encode / commit

**Epic:** E4 Parallel snapshots · **Priority:** P0 · **Size:** L · **Depends on:** MT-004

## Context

`SV_SendClientMessages` (`engine/sv_main.cpp:5256`) handles each client end to end:
decide whether to send, build the datagram (`SV_SendClientDatagram`, `sv_main.cpp:5043`)
which interleaves game DLL calls (`pfnSetupVisibility`, `pfnAddToFullPack`,
`pfnUpdateClientData`, `pfnGetWeaponData`) with pure engine encoding
(`SV_EmitPacketEntities`, `SV_EmitEvents`, `SV_EmitPings`), then `Netchan_Transmit`.
It also sets the global `host_client` used deep inside the callees
(e.g. `SV_WriteEntitiesToClient` passes `host_client->edict` to `pfnAddToFullPack`,
`sv_main.cpp:4934`).

To encode in parallel, the game DLL part must run first for all clients, on the main
thread, and store its results; then the encoding can run independently per client.

## User story

As a developer, I want the snapshot pipeline split into explicit phases with per-client
state, without changing behaviour, so that the encode phase can be parallelised safely.

## Scope

Single-threaded refactor only:

1. **Decide** (main): the existing per-client checks (overflow drop, `send_message`,
   `Netchan_CanPacket`, `next_messagetime`, `chokecount`) produce a list of clients to
   send to this frame.
2. **Gather** (main, per selected client): everything that calls the game DLL or a
   plugin:
   * `pfnSetupVisibility`, the `pfnAddToFullPack` loops and the `REHLDS_FIXES`
     post-processing of `SV_WriteEntitiesToClient` → fills `frame->entities`;
   * `pfnUpdateClientData`, `pfnGetWeaponData` → fills `frame->clientdata` /
     `frame->weapondata`;
   * any data needed later that lives in mutable globals (copy into a per-client
     `ClientSnapshotJob`: `realtime`, `host_frametime`, `sv_rehlds_*` cvar values,
     `CGameClient` local game time, ...).
3. **Encode** (still main in this story): write `svc_time`, client data delta,
   packet entities delta, events, pings, `client->datagram` into a per-client message
   buffer owned by the job, then `Netchan_Transmit`.
4. **Commit** (main): statistics, `SV_CleanupEnts`, deferred diagnostics.

* `host_client` must no longer be read in the encode phase: pass the client explicitly.
* Order of game DLL calls must stay **exactly** the same per client
  (all calls for client 1, then client 2, ...) because plugins may depend on it; only
  the encoding is moved after the gather of the same client in this story. A second step,
  validated separately, gathers all clients first, then encodes all.
* Keep all ReAPI hookchain entry points and their arguments unchanged.

## Acceptance criteria

* [x] Byte-identical replay (MT-004) for both steps.
* [x] No measurable performance regression (MT-002, `cs32`).
* [x] Encode functions take all their inputs from the job / client and from read-only
      server state (verified by Helgrind and concurrent replays rather than a written map).

## Risks

* Plugins calling engine functions from `AddToFullPack` that write into
  `client->datagram` or reliable buffers: those writes happen during gather, which is
  still before encode — order preserved.

## Out of scope

* Any thread (MT-403).

## Status

Done (`engine/sv_main.cpp`, `client_snapshot_t` in `engine/server.h`):

* `SV_GatherClientDatagram` (game DLL calls, client state changes), then
  `SV_EncodeClientDatagram` (writes the datagram from the snapshot), then
  `SV_TransmitClientDatagram` (diagnostics, `Netchan_Transmit`). `SV_WriteClientdataToMessage`
  is split the same way (`SV_GatherClientdata` / `SV_EncodeClientdata`) and still used
  for the spawn.
* The gather takes everything the encode would otherwise read from state that later
  gathers can change: the pings (`SV_GetNetInfo`), the client's queued events and
  unreliable datagram, the local game time. Events queued by later gathers go to the
  next snapshot, as before.
* Step 2 (gather all clients, then encode all) is `sv_parallel_send 1`; step 1 is the
  default path.
* Replays byte-identical with both orders, with snapshot spreading and with
  `sv_rehlds_local_gametime 1`; serial `send_messages` unchanged on `clients32`.
