# Field validation (MT-702)

Real players on a public server running the fork, with a typical plugin stack, to
check what the synthetic load and the replays cannot: plugin compatibility, real
connections, map changes, rare timing.

## Server

`tools/staging/server.sh` (see its header for all commands):

* HLDS `steam_legacy` app 90 (cstrike), ReGameDLL_CS `4a50c42`, Metamod-R 1.3.0.149,
  AMX Mod X 1.9.0.5303 (default plugins, CSX), ReAPI 5.29.0.358.
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
| | | | | | | | | |

## Findings

(to fill in)
