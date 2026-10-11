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
