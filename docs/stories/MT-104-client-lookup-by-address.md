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

## Status

Done (`sv_client_lookup_cache`, default 1, merged into `feature/multithreading`).

`SV_FindClientSlot` keeps a 256-entry table indexed by address. The table is only a
hint: the slot it gives is checked (connected, same address) before it is used, and on
a miss the slots are scanned as before and the table updated, so a stale entry (client
gone, slot reused) cannot send a packet to the wrong client. Only the first slot with
the address gets the packet: a client reconnecting from its address takes its slot back
(`SV_ConnectClient`), and a measurement with 32 clients found exactly one slot per
packet.

Before and after, `clients32`, three runs each, alternated; dispatch is `read_packets`
minus `netchan_process`, `execute_client_msg` and `connectionless`:

| | Before (`feature/multithreading`) | `sv_client_lookup_cache 1` | `sv_client_lookup_cache 0` (same build) |
|---|---|---|---|
| Dispatch per packet | 624 ns (594, 646, 633) | 502 ns (487, 513, 506) | 639 ns (634, 640, 644) |
| Dispatch per frame | 1.95 µs | 1.55 µs | 2.02 µs |
| `sv_frame` per frame | 47.4 µs | 46.9 µs | 48.8 µs |
| Main thread CPU | 5.45 % | 5.41 % | 5.60 % |

The dispatch is 20 % shorter, outside the run to run spread; at the frame level the
0.4 µs saved (under 1 %) is within the noise. Unit tests pass.
