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

Closed without implementation (2026-10-12): it would not lower the latency a player
sees.

* `SV_Frame` reads the packets, runs physics, then sends the snapshots, in that order
  (`engine/sv_main.cpp`, `SV_Frame`). A command that arrives between two ticks is read at
  the next tick and is in every snapshot that tick sends, whether it was read on arrival
  or at the tick. Reading it earlier changes nothing in what a client receives, nor
  when.
* The draining part of the scope is done by the network thread (MT-302): the sockets
  are read on arrival, packets wait in its queue, and the profiler records that wait
  (`net_queue_wait_us`). On the field server it is 502 µs per packet on average, half a
  1 ms tick as expected, against a 10 ms snapshot interval at `cl_updaterate 100`.
* Running the client commands on arrival, between ticks, would save that half tick on
  the command's processing but not on any snapshot, since snapshots only leave on the
  tick; running whole frames on arrival is what `-pingboost` did and what the fixed tick
  grid of MT-101 replaced.
* What remains of the scope, `-pingboost 3`'s broken timeout, does not apply with
  `sys_scheduler 1`, which does its own waiting.
