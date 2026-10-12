# Field validation (MT-702)

Real players on a public server running the fork, with a typical plugin stack, to
check what the synthetic load and the replays cannot: plugin compatibility, real
connections, map changes, rare timing.

## Server

`tools/staging/server.sh` (see its header for all commands):

* HLDS `steam_legacy` app 90 (cstrike), ReGameDLL_CS `4a50c42`, Metamod-R 1.3.0.149,
  AMX Mod X 1.9.0.5303 (default plugins, CSX), ReAPI 5.29.0.358.
* From 2026-10-12: CSDM-ReAPI 1.0.8 in free for all (`mp_freeforall 1`, respawn 0.7 s,
  `mp_round_infinite 1`), with the classic CSDM spawn files (all maps of the cycle except
  `as_oilrig`). Constant fighting and respawns: more entities and events per frame than
  normal rounds. Installed by hand in `.bench/staging/`, not by `server.sh setup`.
* Public Steam server (`sv_lan 0`, VAC), 20 slots, bots fill up to 10, `sys_ticrate 1000`,
  `sv_maxupdaterate 102`, `sv_maxrate 100000`.
* Features under validation: `-netthread`, `sys_scheduler 1`,
  `sv_rehlds_snapshot_spread 1`. `FEATURES=0` starts the same build with all of them off,
  for comparison sessions.
* Host: Ryzen 9 3950X, Linux 7.0, behind NAT (UDP 27015 forwarded).

```sh
tools/staging/server.sh start              # latest engine build, features on
FEATURES=0 tools/staging/server.sh start   # same build, original behaviour
tools/staging/server.sh status
tools/staging/server.sh profile 600        # frame profile of the next 10 minutes
tools/staging/server.sh stop
```

## Procedure

For each session (features on, then off, same players and map if possible):

1. During play, `server.sh profile 600`: the CSV goes to `.bench/staging/profiles/`,
   with `status` and `net_thread_stats`.
2. Players keep `net_graph 3` on and note choke, loss and the update rate, and say how
   smooth it feels, without knowing which session is which if possible.
3. After the session: crashes (`docker logs rehlds-staging`, Steam minidumps in the
   server directory), plugin errors (`cstrike/addons/amxmodx/logs/`).

Go/no-go per step: no new crash, tick p99 not worse than the comparison session, no
plugin error, no player complaint specific to the session.

## Sessions

| Date | Engine | Features | Players | Duration | Ticks/s | Frame p99 | Crashes | Notes |
|---|---|---|---|---|---|---|---|---|
| 2026-10-12 | `6405e18` | on | 1 + bots | ≈ 20 min | 1000.0 | 56 µs (bots only, last minute) | 0 | First player: "ultra smooth". |

## Findings

* **Freeze on map change, not engine-related**: the map cycle moved to `de_airstrip`,
  which had no bot navigation file, and ReGameDLL analysed the map on the main thread
  (same with any engine). All maps of the cycle now have their `.nav` generated
  beforehand (`tools/bench/lib/generate_nav.sh`); do the same for any map added.
* `server.sh profile` kept only the last 60 s (the profiler's default buffer); it now
  sizes the buffer for the whole capture.
* Bots-only server, features on: 1000.0 ticks/s, 12 µs tick jitter, 1 % busy, frame p99
  56 µs; the longest frame (2.4 ms) is in `Steam_RunFrame`.
