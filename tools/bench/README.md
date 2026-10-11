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

Set `BENCH_DOCKER_ARGS`, e.g. `--cpuset-cpus=6,7`, to pass extra options to `docker run`,
and `BENCH_SERVER_ARGS`, e.g. `+sys_scheduler 1`, to append arguments to the server command line.

## Scenarios

| Scenario | Load |
|---|---|
| `idle` | empty server, loop overhead and tick regularity |
| `bots20` | 20 bots playing rounds (game DLL: AI, physics, movement) |
| `bots20-hltv8` | 20 bots + 8 HLTV proxies receiving snapshots (send path) |
| `clients8` | 8 protocol load clients, smoke test of `loadclient.py` |
| `clients32` | full server: 32 protocol load clients at 100 commands/s and 100 snapshots/s |

A scenario is a small `.env` file in `scenarios/`: `MAP`, `TICRATE`, `MAXPLAYERS`, `BOTS`,
`HLTV`, `HLTV_UPDATERATE`, `CLIENTS`, `CLIENT_CMDRATE`, `WARMUP`, `DURATION`, `EXTRA_ARGS`.

## Protocol load client

`loadclient.py` (Python 3, no dependency) connects fake players to the server with the
real GoldSrc protocol: challenge, `connect`, netchan with reliable acknowledgements and
bzip2 fragment reassembly, sign-on (`new`, `sendres`, `spawn`, `sendents`), team join,
then `clc_move` at `CLIENT_CMDRATE` with moving players and `clc_delta` acknowledgements.
Server packets are only decoded during sign-on; in game the netchan header is enough, so
one process drives 32 players.

It records the interval between server packets of each client over the profiling window
(`clients.json`), which the report shows as the client-perceived snapshot rate and jitter.

The server runs with `sv_lan 1` (the Steam ticket is random), `mp_consistency 0` and
`sv_rehlds_maxclients_from_single_ip` raised, all set by `run_server.sh` when `CLIENTS` > 0.

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

* HLTV proxies receive snapshots but send almost nothing; use `CLIENTS` for incoming load.
* Load clients do not decode snapshots, so they do not exercise client-side prediction
  or detect corrupted entity data; byte-exact checks belong to MT-004.
* Bots make runs non-deterministic: tick metrics are stable within 1–2 % between runs,
  per-phase averages and p99 within 10–20 %. Compare several runs before concluding on
  a phase-level change.
