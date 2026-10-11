# MT-101 — Deadline-based frame scheduler

**Epic:** E1 Main loop timing · **Priority:** P0 · **Size:** M · **Depends on:** MT-002

## Context

Today the launcher loop (`dedicated/src/sys_ded.cpp:156`) does `sys->Sleep(1)` then
`RunFrame()`, and `Host_FilterTime` (`engine/host.cpp:663`) rejects the frame if
`realtime - oldrealtime < 1 / (fps + 1)`. Consequences:

* the server polls instead of sleeping until the next tick;
* the effective tick interval is quantised by the 1 ms sleep plus its overshoot
  (timer slack, scheduler latency), producing jitter and beat patterns;
* overshoot is never compensated (`oldrealtime = realtime`), so the average tick rate
  is below `sys_ticrate`;
* sleep behaviour depends on `-pingboost` with four different implementations
  (`dedicated/src/sys_linux.cpp:150`), one of them signal based.

Regular ticks are the single most visible improvement for players: snapshots arrive at
constant intervals, so client interpolation is smooth.

## User story

As a player, I want the server to run its ticks at a constant rate, so that movement of
other players and entities looks smooth and my inputs are processed with stable latency.

## Scope

* New frame scheduler owned by the engine, used when the dedicated launcher calls
  `RunFrame()`:
  * computes `next_tick = last_tick + 1 / sys_ticrate` (accumulating, with catch-up
    capped to avoid spiral of death: if late by more than N ticks, resync);
  * sleeps until `next_tick` using an absolute deadline:
    * Linux: `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME)` and
      `prctl(PR_SET_TIMERSLACK, 1)` at startup;
    * Windows: `CreateWaitableTimerExW(..., CREATE_WAITABLE_TIMER_HIGH_RESOLUTION)`
      with fallback to `timeBeginPeriod(1)` + `Sleep`;
  * optional hybrid mode: sleep until `deadline - spin_margin`, then spin with
    `_mm_pause` (cvar `sys_spin_us`, default 0) for operators who dedicate a core.
* The launcher stops calling `Sleep(1)` when the engine reports it handles pacing
  (new capability negotiated through the existing `IDedicatedServerAPI`, keeping old
  launchers working: if the old launcher still sleeps 1 ms, nothing breaks).
* `Host_FilterTime` keeps its role for `host_framerate`/timescale but no longer rejects
  frames when the scheduler is active.
* Command-line `-pingboost` keeps working (legacy path) but the new scheduler is used
  by default only when `sys_scheduler 1` (default off until MT-702).
* `host_frametime` passed to the game stays the measured delta (no behaviour change for
  the game DLL), capped at 0.25 s as today.

## Acceptance criteria

* [ ] `idle` scenario at `sys_ticrate 1000`: achieved fps ≥ 990, p99 tick interval
      < 1.2 ms (to be compared with baseline numbers from MT-003).
* [ ] `cs32` scenario: tick jitter (stddev) reduced by at least 50 % vs baseline.
* [ ] CPU usage at idle not higher than baseline with `sys_spin_us 0`.
* [ ] `sys_ticrate` changes take effect at runtime.
* [ ] Works with the stock Valve `hlds_linux` launcher and the ReHLDS launcher.
* [ ] Regression replay (MT-004) unchanged (time is replayed).

## Risks

* Very high `sys_ticrate` values on oversubscribed VPS: the catch-up cap must prevent
  bursts of back-to-back frames.
* Game DLLs or plugins relying on many tiny frames (e.g. timers checked each frame)
  might behave differently when frames become regular — this is an improvement but
  should be watched in MT-702.

## Out of scope

* Packet-driven wake-up (MT-102).
