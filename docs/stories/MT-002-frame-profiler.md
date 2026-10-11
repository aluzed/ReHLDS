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

* [ ] With profiling disabled, frame time difference is below measurement noise.
* [ ] With profiling enabled, overhead < 1 % of frame time at 32 players.
* [ ] `sv_profile 10` prints per-phase avg/p99 and tick jitter.
* [ ] CSV output is consumed by the MT-003 harness.
* [ ] Builds on Linux i386 and Windows.

## Risks

* Instrumenting inside hookchain-protected functions must not change the call order
  or the hookchain behaviour.

## Out of scope

* Fixing `host_profile` (done in MT-103).
