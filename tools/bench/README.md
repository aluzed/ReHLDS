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
frame), `server.log`, `hltv-*.log`, `cpu.txt` (process and main thread), `meta.txt`, and
`clients.json` / `flood.json` when the scenario has load clients or a flood.

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
| `flood-a2s` | `clients32` + 50 000 A2S_INFO queries/s from 4096 addresses |
| `replay` | short session for replay checks: 8 load clients, 2 HLTV proxies, no bots |
| `flood-junk` | `clients32` + 50 000 junk packets/s from 4096 addresses, shaped like connected-client traffic |
| `replay-hooks` | `replay` with a plugin hooking `SV_EmitPings` (ReAPI): checks the `sv_parallel_send` fallback. Not deterministic (AMX Mod X): run it, do not replay it |
| `clients32-ents` | 32 load clients **playing** (alive, they see each other) + 400 moving entities around them: ≈ 150 packet entities per snapshot (epic 4) |

A scenario is a small `.env` file in `scenarios/`: `MAP`, `TICRATE`, `MAXPLAYERS`, `BOTS`,
`HLTV`, `HLTV_UPDATERATE`, `CLIENTS`, `CLIENT_CMDRATE`, `WARMUP`, `DURATION`, `EXTRA_ARGS`,
`FLOOD_KIND`, `FLOOD_PPS`, `FLOOD_SOURCES`, `CLIENTS_ALIVE` (the load clients join a team
and spawn; without it they stay dead spectators), `METAMOD` (loads Metamod-R and
AMX Mod X with the plugins of `plugins/`, compiled by `setup.sh`), `ENTITIES` (moving
entities spawned by `plugins/bench_entities.sma`, needs `METAMOD=1`), `SERVER_COMMANDS` and
`FINAL_COMMANDS` (console commands sent after the map load and at the end).

## Replay and race checks (MT-004)

```sh
tools/bench/regression.sh before after       # same packets as the reference build?
BENCH_RECORD=1 tools/bench/run.sh replay x   # record a session (any scenario)
tools/bench/replay.sh .bench/results/<run> y # replay it with build y, compare
BENCH_HELGRIND=ON tools/bench/build.sh helgrind
tools/bench/race_check.sh helgrind 90        # Helgrind on the server with the network thread
```

The `replay` scenario has load clients and HLTV proxies but no bots: sessions with bots
are not deterministic. `BENCH_NO_ASLR=1` (with `BENCH_DOCKER_ARGS="--security-opt
seccomp=unconfined"`) runs the server without address randomisation. Details and results:
[`docs/benchmarks/replay.md`](../../docs/benchmarks/replay.md).

## Flood generator

`flood.py` sends connectionless packets (`a2s` queries, `challenge` requests) or `junk`
during the measurement window, from many source addresses (127.2.x.y: the whole
127.0.0.0/8 range is local on Linux), so that per-address rate limits alone do not stop
it. One process sends up to about 100 000 packets/s. Server replies are counted in
`flood.json`.

Compare the network thread with the synchronous path:

```sh
BENCH_SERVER_ARGS="+sys_scheduler 1 +sv_rehlds_snapshot_spread 1" tools/bench/run.sh flood-a2s mybuild
BENCH_SERVER_ARGS="-netthread +sys_scheduler 1 +sv_rehlds_snapshot_spread 1" tools/bench/run.sh flood-a2s mybuild
```

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
