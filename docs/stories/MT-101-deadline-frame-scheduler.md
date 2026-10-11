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

* [x] `idle` scenario at `sys_ticrate 1000`: achieved fps ≥ 990, p99 tick interval
      < 1.2 ms — 1000.0 fps, p99 1018 µs.
* [x] Loaded scenario: tick jitter (stddev) reduced by at least 50 % vs baseline —
      −60 % on `bots20-hltv8` (82.5 → 33.3 µs), 908 → 999.9 fps.
* [x] CPU usage at idle not higher than baseline with `sys_spin_us 0` — 1.7 % vs 1.9 %.
* [x] `sys_ticrate` changes take effect at runtime — applied at the next slice.
* [x] Works with the stock Valve `hlds_linux` launcher and the ReHLDS launcher.
* [ ] Regression replay (MT-004) unchanged — MT-004 not implemented yet; the scheduler is
      off by default and does not change frame contents.
* [ ] Windows: high resolution waitable timer path written but not built or run yet.

Results: [`docs/benchmarks/mt101-scheduler.md`](../benchmarks/mt101-scheduler.md).

## Implementation notes

Code: `rehlds/rehlds/frame_scheduler.{h,cpp}`, `CDedicatedServerAPI::RunFrame`
(`engine/sys_dll2.cpp`), `Host_GetTicrate` / `Host_FilterTime` (`engine/host.cpp`).

| Cvar | Default | Effect |
|---|---|---|
| `sys_scheduler` | 0 | 1 = pace frames on a fixed grid of absolute deadlines |
| `sys_scheduler_slice` | 100 | ms spent inside one `RunFrame()` call before returning to the launcher (1–1000) |
| `sys_spin_us` | 0 | µs before each deadline spent spinning instead of sleeping (0–2000) |

* No launcher change was needed: the launcher's ~1 ms sleep before every `RunFrame()`
  is longer than a 1000 Hz tick, so `RunFrame()` keeps control for a slice and runs
  several frames on the grid. The slice ends when the next deadline falls after it, so at
  lower tick rates the launcher sleeps inside the gap between two ticks.
* Grid in integer nanoseconds (`Sys_MonotonicNs`), deadlines accumulate (`next += period`)
  so there is no drift. More than 2 ticks late (map load, stall) restarts the grid from
  now instead of running the backlog back to back.
* Linux: `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME)` and `PR_SET_TIMERSLACK` 1 ns.
  Windows: `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION` timer, falling back to
  `timeBeginPeriod(1)` (winmm loaded dynamically) + 1 ms sleeps + spin.
* `Host_FilterTime` skips its minimum-interval check while the scheduler paces frames;
  `host_frametime` is still the measured delta.
* The tick rate source (`-sys_ticrate` command line, else the cvar) is shared through
  `Host_GetTicrate`.

## Risks

* Very high `sys_ticrate` values on oversubscribed VPS: the catch-up cap must prevent
  bursts of back-to-back frames.
* Game DLLs or plugins relying on many tiny frames (e.g. timers checked each frame)
  might behave differently when frames become regular — this is an improvement but
  should be watched in MT-702.

## Out of scope

* Packet-driven wake-up (MT-102).
