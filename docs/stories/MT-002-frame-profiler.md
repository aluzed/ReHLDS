# MT-002 — Per-phase frame profiler

**Epic:** E0 Foundations · **Priority:** P0 · **Size:** M · **Depends on:** MT-001

## Context

The only built-in instruments are `host_speeds` (client-oriented passes, meaningless on
a dedicated server), `host_profile` (buggy average, see `engine/host.cpp:982-985`) and
`stats`. None of them tell how long each part of `SV_Frame_Internal`
(`engine/sv_main.cpp:8306`) takes, nor how regular the ticks are.

Every later story needs before/after numbers. Without them we cannot tell whether
`SV_SendClientMessages` deserves to be parallelised (decision gate after phase 2).

## User story

As a server developer, I want precise per-phase timings and tick-interval statistics,
so that I know where frame time goes and can prove that a change helps.

## Scope

* A lightweight scoped timer (`FrameProfileScope`) based on a monotonic high resolution
  clock (`clock_gettime(CLOCK_MONOTONIC)` / `QueryPerformanceCounter`), zero cost when
  disabled (single branch on a cached flag).
* Instrumented phases:
  * outer loop: sleep time, `Host_FilterTime` rejections (wasted wake-ups);
  * `Cbuf_Execute`, `SV_ReadPackets` (split: receive / `Netchan_Process` /
    `SV_ExecuteClientMessage`), `SV_Physics`, `SV_CheckTimeouts`;
  * `SV_SendClientMessages` split into **gather** (`pfnSetupVisibility`,
    `pfnAddToFullPack`, `pfnUpdateClientData`), **encode** (`SV_EmitPacketEntities`,
    `SV_EmitEvents`, `SV_EmitPings`) and **transmit** (`Netchan_Transmit`);
  * `Steam_RunFrame`, log writes.
* Tick statistics: actual interval between two `SV_Frame` calls (min / avg / p50 / p95 /
  p99 / max, stddev = jitter), frames per second achieved vs `sys_ticrate`.
* Output:
  * console command `sv_profile [seconds]` printing a table;
  * optional CSV dump (`sv_profile_csv <file>`) for the benchmark harness;
  * cvar `sv_profile_enable 0/1`.
* Counters: packets in/out per frame, bytes in/out, number of entities sent per client.

## Technical approach

* Fixed-size ring of samples per phase (no allocation in the frame), percentiles
  computed on demand when printing.
* Put the code in `rehlds/rehlds/` (new `frame_profiler.cpp/h`), not in the reversed
  engine files, to ease upstream rebases.

## Acceptance criteria

* [x] With profiling disabled, frame time difference is below measurement noise
      (each instrumentation point is a single test on a cached `bool`).
* [ ] With profiling enabled, overhead < 1 % of frame time at 32 players — estimated
      (see below), to be measured with the MT-003 harness.
* [x] `sv_profile 10` prints per-phase avg/p99 and tick jitter.
* [ ] CSV output is consumed by the MT-003 harness — format ready, harness pending.
* [ ] Builds on Linux i386 (done, Debian 11 toolchain as in CI) and Windows (not
      verified locally, needs CI).

## Implementation notes

Code: `rehlds/rehlds/frame_profiler.{h,cpp}`, instrumentation in `engine/host.cpp`,
`engine/sv_main.cpp`, `engine/net_ws.cpp`, `engine/sv_log.cpp`.

| Command / cvar | Effect |
|---|---|
| `sv_profile <seconds>` | records for the given duration, then prints the report |
| `sv_profile` | prints the report of the frames currently recorded |
| `sv_profile_enable 1` | records continuously into the ring buffer |
| `sv_profile_frames N` | ring buffer size (1000–600000, default 60000), applied on allocation |
| `sv_profile_csv <file>` | writes recorded frames to `<file>` in the game directory |
| `sv_profile_reset` | stops recording and frees the buffer |

Design choices:

* Explicit `BeginPhase`/`EndPhase` calls instead of RAII scopes: the engine leaves frames
  with `longjmp` (`Host_Error` → `host_abortserver`), which would skip destructors. A
  frame that never reaches `EndFrame` is discarded.
* Own clock (`clock_gettime(CLOCK_MONOTONIC)` / `QueryPerformanceCounter`) instead of
  `Sys_FloatTime`, which takes a critical section on Windows.
* Rows are stored in a ring buffer allocated when profiling starts; nothing is allocated
  and no file is written while frames run. `sv_profile_csv` dumps the buffer on demand
  instead of streaming, for the same reason.
* `encode` is derived as `datagram − gather`, so the per-client hot path only times the
  game DLL calls.
* The launcher's sleep is not visible to the engine: idle time is `tick_interval − frame`,
  and wasted wake-ups are counted by `filter_rejects`.

Cost when enabled: one `clock_gettime` is 36 ns on the reference machine (i386 build), so
≈ 0.6 µs per frame for the fixed phases, ≈ 0.36 µs per client snapshot and ≈ 0.15 µs per
received packet.

## First measurements

Stock `steam_legacy` HLDS (cstrike, `de_dust2`, `sys_ticrate 1000`) with this engine and
one HLTV proxy connected, 10 s capture:

```
sv_profile: 9357 frames over 10.00 s, 935.8 fps (sys_ticrate 1000), busy 1.0%
time (us)                      avg       p50       p95       p99       max    stddev
tick_interval               1068.6    1063.2    1102.6    1123.0    1318.9      16.2
frame                         11.1       5.5      44.5      62.2     246.5      14.6
```

and with `sys_ticrate 100`: 93.7 fps, 9 `filter_rejects` per frame.

The server is busy 1 % of the time but misses its target rate by 6–7 %: this is the
polling loop described in MT-101, now measurable.

## Risks

* Instrumenting inside hookchain-protected functions must not change the call order
  or the hookchain behaviour.

## Out of scope

* Fixing `host_profile` (done in MT-103).
