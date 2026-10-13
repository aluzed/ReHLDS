# MT-106 — Spread client snapshots over frames

**Epic:** E1 Main loop timing · **Priority:** P0 · **Size:** S · **Depends on:** MT-002, MT-003

Added after the first full-server measurements (`clients32`); not in the original plan.

## Context

`SV_SendClientMessages` (`engine/sv_main.cpp`) sends a snapshot to a client when
`realtime + host_frametime >= cl->next_messagetime`, then sets
`next_messagetime = realtime + host_frametime + interval`.

The next send time is relative to the frame where the snapshot was sent. After any long
frame (connections, round restart, a slow plugin), every client becomes due in the same
frame, gets the same next send time, and they stay aligned forever. With 32 players at
`cl_updaterate 100` on a 1000 Hz server, the profiler shows all 32 snapshots built in
one frame out of 9–11 and nothing in between:

```
frames by clients_sent: 0 → 23205, 2 → 274, 30 → 274, 32 → 2604
send_messages p95 393 µs, p99 479 µs, max 928 µs (frame budget 1000 µs)
```

The relative schedule also loses snapshots when frames are perfectly regular (MT-101):
the 10 ms deadline lands exactly on a tick and timer noise decides between 10 and 11
ticks, giving 94 snapshots/s instead of 100.

## User story

As a player on a full server, I want my snapshots at a steady rate and the server's
frames not to spike every N ticks, so that the game is smooth for everyone.

## Scope

* With `sv_rehlds_snapshot_spread 1`, each client gets a fixed phase within its update
  interval (`slot × interval / maxclients`) and its next send time is the first
  phase-aligned time at least half an interval ahead. Sends are spread evenly over the
  frames and the rate is exact (no drift, no lost tick).
* Snapshot contents and the send condition are unchanged; only `next_messagetime` is
  computed differently. Default 0 (original behaviour), under `REHLDS_FIXES`.

## Acceptance criteria

* [x] `clients32`: no frame builds more than a handful of snapshots in steady state —
      3–4 per frame (rare outliers after long frames).
* [x] `send_messages` p99 reduced by at least 50 % — −84 % with the scheduler
      (479 → 75 µs), −81 % without.
* [x] Clients receive their full update rate — 100.0 snapshots/s with `cl_updaterate 100`.
* [x] Off by default, original behaviour unchanged when off.

Results: [`docs/benchmarks/clients32.md`](../benchmarks/clients32.md).

## Risks

* The first snapshot after a client becomes active can be delayed by up to 1.5 intervals
  (15 ms at 100 Hz) — once per connection.
* Plugins that expect all clients to be updated in the same frame (none known).

## Out of scope

* Per-client send cost (parallel encoding, epic 4).
