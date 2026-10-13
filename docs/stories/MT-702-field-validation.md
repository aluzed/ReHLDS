# MT-702 — Field validation with real plugins and players

**Epic:** E7 Rollout · **Priority:** P0 · **Size:** M · **Depends on:** MT-701

## Context

Synthetic load (MT-003) and replays (MT-004) cannot cover the variety of real setups:
plugin stacks, mods other than CS, players with bad connections, map changes, crashes
caused by rare timing. Defaults can only be switched on after real-world validation.

## User story

As the maintainer, I want the new features to run on real servers before becoming the
default, so that we ship stability improvements and not new crashes.

## Scope

* Staging server(s) running the fork with a typical public plugin stack (Metamod-R,
  AMXX, ReAPI, ReGameDLL, Reunion), opt-in features enabled progressively:
  1. MT-101/102/103 (timing),
  2. MT-301 (async logs), MT-302/303 (network thread),
  3. MT-403 (parallel encode).
* Each step runs ≥ 1 week with real players; collect `sv_profile` CSVs, crash dumps
  (enable core dumps / minidumps), player feedback on smoothness (e.g. `net_graph 3`
  screenshots: choke, loss, update rate).
* Go/no-go checklist per step: no new crash signature, tick p99 not worse, no plugin
  incompatibility report.
* Decide final defaults; optionally prepare upstream PRs for the low-risk parts
  (MT-101, MT-103, MT-104, MT-301) as separate small PRs to `rehlds/ReHLDS`.

## Acceptance criteria

* [ ] Validation report in `docs/benchmarks/field-validation.md`.
* [ ] Defaults decided and applied.
* [ ] Upstream PRs opened for the generic improvements (if desired).

## Status

In progress. Staging server ready (`tools/staging/server.sh`), sessions and results in
[`../benchmarks/field-validation.md`](../benchmarks/field-validation.md).
