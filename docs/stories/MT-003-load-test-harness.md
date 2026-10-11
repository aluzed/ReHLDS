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

## Acceptance criteria

* [ ] `tools/bench/run.sh cs32` produces a report with tick p50/p99, jitter, per-phase
      times and CPU usage.
* [ ] Two runs of the same build differ by < 5 % on p50 frame time.
* [ ] Baseline report for upstream `master` committed.
* [ ] Client-perceived metric included: interval between snapshots received by the
      load client (p99 and jitter).

## Risks

* Protocol implementation effort; mitigation: reuse the testsuite recorder format in
  `rehlds/testsuite/` or an existing open source GoldSrc client library.
* Noisy machines: pin CPU frequency, document `taskset` usage.

## Out of scope

* Distributed load generation across several machines.
