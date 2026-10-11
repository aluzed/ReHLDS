# Benchmark harness

Runs a real HLDS with a locally built engine under a scripted load and reports the
per-phase frame profile recorded by `sv_profile` (MT-002). See
[`docs/stories/MT-003-load-test-harness.md`](../../docs/stories/MT-003-load-test-harness.md).

Requirements: Docker, Python 3, ~2 GB of disk. Everything (HLDS install, builds,
results) goes to `.bench/` at the repository root, or to `$BENCH_DIR`.

## Usage

```sh
tools/bench/setup.sh                       # once: Docker image, HLDS, ReGameDLL_CS, nav mesh
tools/bench/build.sh baseline              # build the working tree, store it as "baseline"
# ... change code ...
tools/bench/build.sh mychange
tools/bench/run.sh bots20-hltv8 baseline
tools/bench/run.sh bots20-hltv8 mychange
tools/bench/report.py .bench/results/<mychange run> --baseline .bench/results/<baseline run>
```

`run.sh` prints a Markdown report and keeps everything in
`.bench/results/<date>-<scenario>-<engine>/`: `report.md`, `profile.csv` (one row per
frame), `server.log`, `hltv-*.log`, `cpu.txt`, `meta.txt`.

Set `BENCH_DOCKER_ARGS`, e.g. `--cpuset-cpus=6,7`, to pass extra options to `docker run`.

## Scenarios

| Scenario | Load |
|---|---|
| `idle` | empty server, loop overhead and tick regularity |
| `bots20` | 20 bots playing rounds (game DLL: AI, physics, movement) |
| `bots20-hltv8` | 20 bots + 8 HLTV proxies receiving snapshots (send path) |

A scenario is a small `.env` file in `scenarios/`: `MAP`, `TICRATE`, `MAXPLAYERS`, `BOTS`,
`HLTV`, `HLTV_UPDATERATE`, `WARMUP`, `DURATION`, `EXTRA_ARGS`.

## How it works

* The Docker image is the CI toolchain (Debian 11, i386 multilib); it builds the engine
  and runs the 32-bit server.
* HLDS is the stock `steam_legacy` app 90 (cstrike) from steamcmd; `run.sh` only replaces
  `engine_i486.so`.
* Bots come from ReGameDLL_CS, built from source at a pinned commit and enabled with
  `bot_enable 1` in `game_init.cfg`. The navigation mesh is generated once by `setup.sh`.
* HLTV proxies (shipped with HLDS) are the network clients: each one receives snapshots
  of every entity. `sv_proxies` is raised to the scenario's `HLTV` count.
* The console is driven through a FIFO; process CPU time is sampled from `/proc` over the
  profiling window.

## Limitations

* HLTV proxies receive snapshots but send almost nothing, and they cap at about 30
  updates per second each. Incoming load (`clc_move` at 100 Hz from 32 players) is not
  reproduced yet: that needs a protocol-level load client.
* Bots make runs non-deterministic: tick metrics are stable within 1–2 % between runs,
  per-phase averages and p99 within 10–20 %. Compare several runs before concluding on
  a phase-level change.
