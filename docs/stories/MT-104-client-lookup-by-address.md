# MT-104 — O(1) client lookup by address

**Epic:** E1 Main loop timing · **Priority:** P2 · **Size:** S · **Depends on:** MT-002

## Context

For every non-connectionless packet, `SV_ReadPackets` (`engine/sv_main.cpp:3960`) scans
all `maxclients` slots and calls `NET_CompareAdr` on each. At 32 players × 100 packets/s
plus fragments this is small but pure waste, and it grows quadratically when servers run
with high `maxplayers` and high `cl_cmdrate`. Note the loop also does not `break` after a
match.

## User story

As a server operator, I want packet dispatch to cost the same regardless of the number
of slots, so that large servers keep a stable frame time.

## Scope

* Maintain a small open-addressing hash map `netadr (ip:port) → client index`, updated in
  the places that set or clear `cl->netchan.remote_address` (connect, drop, reconnect,
  `Netchan_Setup`).
* `SV_ReadPackets` uses the map; keep the linear scan as a debug-only cross-check
  (`#ifdef _DEBUG` assert both agree).
* Stop scanning after the matching client is found (under `REHLDS_FIXES`; verify that
  no two slots can share an address — if they can, document the case).

## Acceptance criteria

* [ ] Identical replay output (MT-004).
* [ ] `SV_ReadPackets` dispatch time measurably lower with 32 clients (MT-002).
* [ ] Map stays consistent across map change, `retry`, and slot reuse (unit test).

## Out of scope

* Changing how connectionless packets are handled.
