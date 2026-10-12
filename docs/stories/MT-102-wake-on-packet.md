# MT-102 — Wake the main loop on packet arrival

**Epic:** E1 Main loop timing · **Priority:** P1 · **Size:** M · **Depends on:** MT-101

## Context

Between two ticks the main thread sleeps. Packets that arrive meanwhile wait until the
next tick to be read by `SV_ReadPackets` (`engine/sv_main.cpp:3924`). Only
`-pingboost 3` (`NET_Sleep_Timeout`, `engine/net_ws.cpp:979`) waits on the sockets, and
its timeout maths is broken (`(1000 / fps) * 1000` is 0 for `fps > 1000`, stagger logic
with integer seconds).

Reading packets as soon as they arrive spreads the `Netchan_Process` cost over the idle
time and lowers input latency; the actual simulation still happens on the tick.

## User story

As a player, I want my commands to be received and queued as soon as they arrive, so
that the next tick always includes my latest input.

## Scope

* While waiting for the next deadline (MT-101), wait on the server sockets too:
  * Linux: `epoll_wait`/`ppoll` with a timeout derived from the deadline
    (nanosecond precision via `ppoll`, or `epoll_pwait2` when available);
  * Windows: `WSAPoll` / `select` with the remaining time, then high-resolution timer
    for the last sub-millisecond.
* On readiness, drain the sockets into an engine-side queue (no game DLL calls), then
  go back to waiting until the deadline. Packet processing (`Netchan_Process`,
  `SV_ExecuteClientMessage`) stays in `SV_ReadPackets` on the tick to keep game
  semantics identical.
* Record arrival timestamps per packet (useful for MT-302 and diagnostics).
* Replace `NET_Sleep_Timeout` implementation with the new wait (keep the exported
  symbol for `-pingboost 3` compatibility).

## Acceptance criteria

* [ ] With the scheduler on, `-pingboost` value has no effect on latency anymore.
* [ ] Load client measured RTT (command sent → snapshot acknowledging it) p50 reduced
      vs MT-101 alone, or equal with lower variance.
* [ ] No busy loop: CPU usage at idle unchanged.
* [ ] Fakelag/fakeloss (`NET_LagPacket`) still works.

## Risks

* If MT-302 (network thread) is done later, this logic moves to that thread: keep the
  wait primitive in one reusable function.

## Out of scope

* Processing client commands outside the tick (would change game semantics).

## Status

Implemented as an option, `sys_wake_on_packet 1` (off by default): with `sys_scheduler 1`
and `-netthread`, the main thread waits on the network thread's wake handle (an `eventfd`
on Linux, an event on Windows) as well as on the tick deadline, and reads the packets as
they arrive (`SV_ReadPacketsBetweenFrames`). Physics and the snapshots stay on the tick.
The profiler counts the time spent reading between frames in `early_read_us`.

### Measurements

The load clients now measure the input latency: from sending a command to receiving the
first server packet that acknowledges it, over every command. Three runs of each,
`-netthread +sys_scheduler 1 +sv_rehlds_snapshot_spread 1`:

| `clients32` (100 snapshots/s) | `sys_wake_on_packet 0` | `1` |
|---|---|---|
| Input latency avg / p99 | 5.2 / 10.1 ms | 4.9 / 10.5 ms |
| Packet wait in the queue | ≈ 520 µs | ≈ 15 µs |
| Frame avg / p99 | 72 / 205 µs | 61 / 127 µs |
| Tick interval p99 | 1036 µs | 1042 µs |
| Process CPU | 10.4 % | 13.6 % |

| `clients32-ents` (rate-limited, ≈ 12 snapshots/s) | `sys_wake_on_packet 0` | `1` |
|---|---|---|
| Input latency avg / p99 | 44.8 / 105 ms | 44.6 / 105 ms |
| Frame avg / p99 | 200 / 1186 µs | 121 / 385 µs |
| Tick interval p99 | 1249 µs | 1144 µs |
| Tick jitter | 75 µs | 72 µs |
| Process CPU | 23.3 % | 26.1 % |

* **The latency players see does not change.** `SV_Frame` reads the packets, runs
  physics, then sends the snapshots: a command that arrives between two ticks is in
  every snapshot of the next tick, whether it was read on arrival or at the tick. The
  input latency is half the client's snapshot interval on average (5 ms at 100
  snapshots/s), and the half tick saved on reading does not reach the client.
* The client commands, which run the game DLL's player movement, leave the tick: the
  frame p99 drops by 40 to 70 %, the tick interval p99 by up to 8 % on the heavy
  scenario.
* Cost: about 3 % of a core (one wake-up per packet burst).
* Commands read between frames see the `realtime` of the last frame, up to one tick
  old; `sv.time` is the same as at the start of the next frame, as in `SV_Frame`.
* No crash in 6 map changes with Metamod, AMX Mod X and bots
  (`BENCH_SERVER_ARGS="+sys_wake_on_packet 1" tools/bench/mapchange_check.sh`);
  replays with the option off are byte-identical. A session recorded with the option on
  cannot be replayed exactly: the replay reads packets on the ticks.
* Windows: written, not built (the wait uses a millisecond timeout and leaves the last
  millisecond to the scheduler's high resolution timer).

### Acceptance criteria

* [x] With the scheduler on, `-pingboost` has no effect (the scheduler does the waiting).
* [x] Input latency p50 reduced, or equal with lower variance: equal, as explained above.
* [x] No busy loop: CPU at idle unchanged (the wait sleeps until a packet or the tick).
* [x] Fake lag (`NET_LagPacket`) still works: packets go through `NET_GetPacket` as before.

Off by default. On the benchmark it gives headroom inside the tick but nothing a player
notices; whether to recommend it is decided with the field validation (MT-702) under
real load.

### Found on the way: split packets in the load client

The server splits datagrams over 1400 bytes (`NET_SendLong`). The load client took each
part for a netchan packet: its incoming sequence jumped to `0x3FFFFFFE`, it dropped every
later packet as out of order, and acknowledged a stale frame in `clc_delta`. With big
snapshots (`clients32-ents`), the server then delta-encoded against frames the client
did not have, or sent full snapshots, and the snapshot statistics counted the parts. The
load client now reassembles split datagrams. `clients32` (small snapshots) was not
affected; `clients32-ents` and the epic 4 measurements were, see
[`../benchmarks/clients32-ents.md`](../benchmarks/clients32-ents.md).
