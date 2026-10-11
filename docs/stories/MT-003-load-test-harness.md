# MT-003 — Load-test harness and reference benchmarks

**Epic:** E0 Foundations · **Priority:** P0 · **Size:** L · **Depends on:** MT-002

## Context

Performance work needs reproducible load. Bots (`fakeclient`) are skipped by
`SV_SendClientMessages` (`engine/sv_main.cpp:5266`), so they exercise physics but not
the network/snapshot path, which is exactly the path we want to optimise.

## User story

As a developer, I want a scripted way to load a server with N realistic network clients
and get comparable numbers, so that every optimisation can be measured objectively.

## Scope

* **Network load client** (`tools/loadclient/`, any language, Python or C++): speaks
  the GoldSrc protocol enough to connect (challenge, `connect`, `new`, `spawn`), then
  sends `clc_move` at a configurable rate (e.g. 100 Hz) with scripted movement and
  acknowledges server packets. Runs N connections from one process.
  * Steam auth: run the server with `sv_lan 1` or with a dev auth setting.
* **Scenario scripts** (`tools/bench/`):
  * `idle` – empty map, measures loop overhead and tick regularity;
  * `cs32` – `de_dust2`, 32 load clients moving, `sys_ticrate 1000`;
  * `cs32-ents` – same plus many dynamic entities (grenades, dropped weapons);
  * `flood` – connectionless packet flood (A2S, bogus) while 32 clients play.
* **Runner** that starts HLDS with ReHLDS, enables `sv_profile_csv`, runs a scenario for a
  fixed duration, collects CSV + CPU usage, and outputs a Markdown report.
* **Reference results** committed in `docs/benchmarks/` for `master` on a documented
  machine (CPU, kernel, governor).

## Status

Implemented as `tools/bench/` (see its README), with a different load source than first
planned:

* **Protocol load client** `tools/bench/loadclient.py`: fake players speaking the real
  GoldSrc protocol (handshake, netchan, sign-on, `clc_move` at 100 Hz, delta acks).
  32 of them reproduce a full public server (`clients32`).
* **HLTV proxies** shipped with HLDS: network clients receiving snapshots of every entity.
* **Moving players: ReGameDLL_CS bots**, built from source at a pinned commit.
* Scenarios: `idle`, `bots20`, `bots20-hltv8`, `clients8`, `clients32` (the `flood`
  scenario is left for MT-303).
* Baseline: [`docs/benchmarks/baseline-mt002.md`](../benchmarks/baseline-mt002.md).

## Acceptance criteria

* [x] `tools/bench/run.sh <scenario>` produces a report with tick p50/p99, jitter,
      per-phase times and CPU usage.
* [x] Two runs of the same build differ by < 5 % on p50 frame time (−3 % measured). Per-phase
      averages and p99 vary by 10–20 % because of the bots; CPU pinning does not help.
* [x] Baseline report committed (profiler build, since upstream `master` has no profiler).
* [x] Client-perceived snapshot interval: measured by the load clients (rate, p50/p99,
      jitter) and shown in the report.

## Risks

* Protocol implementation effort; mitigation: reuse the testsuite recorder format in
  `rehlds/testsuite/` or an existing open source GoldSrc client library.
* Noisy machines: pin CPU frequency, document `taskset` usage.

## Out of scope

* Distributed load generation across several machines.
