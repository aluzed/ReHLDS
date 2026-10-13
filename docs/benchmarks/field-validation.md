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
* Public Steam server (`sv_lan 0`, VAC), 20 slots, `sys_ticrate 1000`,
  `sv_maxupdaterate 102`, `sv_maxrate 100000`. Bots filled up to 10 at first; from
  2026-10-13, 19 bots at all times (`bot_quota 19`, `bot_quota_mode normal`,
  `bot_auto_vacate 0`, `bot_join_after_player 0`) and CSDM grenades on (HE, flash, smoke).
* Features under validation: at first `-netthread`, `sys_scheduler 1`,
  `sv_rehlds_snapshot_spread 1`; then `-sys_threads 2 +sv_parallel_send 1`, `-netsend`,
  `sv_a2s_cache 1`, `sv_log_async 1`, `sv_steam_on_demand 1`. They are all on by default
  since 2026-10-13 (see [`../performance-tuning.md`](../performance-tuning.md)). `FEATURES=0`
  starts the same build with all of them off, for comparison sessions.
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
| 2026-10-12 | `6405e18` | on | 1 + 10 bots, CSDM FFA | ≈ 35 min | – | – (no capture) | 0 | 227 kills; no timeout, all disconnects client-initiated. |
| 2026-10-12 | `c937268` | on, `sv_parallel_send 1` | 1 + 18 bots, CSDM FFA | ≈ 7 min | 999.9 | 328 µs | 1 | Crash at the map change after the player left: heap corrupted by the encoding threads through Metamod, fixed (MT-404). |
| 2026-10-13 | `ef9a314` | all: the above, encoders recorded during the gather (MT-404), `-netsend`, `sv_a2s_cache 1`, `sv_log_async 1` | 1 + 19 bots, CSDM FFA with grenades | ≈ 2 h of uptime | – | – | 0 | Player: "hyper stable, very pleasant". Counters below. |
| 2026-10-13 | `452f477` | the same, `sv_steam_on_demand 1` | 19 bots | 60 s profiles | 1000 | – | 0 | Steam phase 4.6 → 1.0 ms per second, see below. |

## Findings

* **Freeze on map change, not engine-related**: the map cycle moved to `de_airstrip`,
  which had no bot navigation file, and ReGameDLL analysed the map on the main thread
  (same with any engine). All maps of the cycle now have their `.nav` generated
  beforehand (`tools/bench/lib/generate_nav.sh`); do the same for any map added.
* `server.sh profile` kept only the last 60 s (the profiler's default buffer); it now
  sizes the buffer for the whole capture.
* Bots-only server, features on: 1000.0 ticks/s, 12 µs tick jitter, 1 % busy, frame p99
  56 µs; the longest frame (2.4 ms) is in `Steam_RunFrame`.
* Over the first day (2 h 45 of uptime, 1 player, 10 bots, 10 map changes including
  `changelevel` by RCON, CSDM loaded at runtime): no crash, no restart, no AMX Mod X
  error, no client timeout (every disconnect is "Client sent 'drop'"), no packet dropped
  by the network thread (queue at most 58 of 1024 packets).
* **Crash with `sv_parallel_send 1`** (`c937268`, second day): one player with 18 bots
  in CSDM FFA. The player left, `bot_join_after_player` kicked the bots, and the
  `changelevel` 45 s later aborted in `malloc` (`unsorted double linked list
  corrupted`) while Metamod-R re-read `plugins.ini`. The core dump showed three freed
  16-byte chunks with their first word decremented by one, and the same build crashed
  on the first map change with load clients instead of the player, never with
  `sv_parallel_send 0`. Cause: the game DLL's conditional encoders, on the encoding
  threads, called the engine through Metamod's callbacks, which save and restore global
  state without a lock, while the main thread ran Metamod code too; at the map change
  Metamod-R rebuilt its callbacks from a stale stack pointer. Fixed in MT-404 (the
  encoders no longer run alongside game DLL code); `tools/bench/mapchange_check.sh`
  covers it. The release `3.15.0-mt.1` does not contain the encoding threads.
* Profile of that session before the crash (402 s at 1000 Hz, the player alone for the
  first minute, then with 18 bots): 999.9 ticks/s, 38.7 µs tick jitter, 4 % busy;
  frame avg / p95 / p99 40 / 150 / 328 µs. The frame peaks are game DLL physics with
  the bots (`physics` p99 292 µs, max 3.3 ms); the send to one player takes 3.1 µs on
  average (54 µs p99), 96 % of the snapshots encoded by the workers. The network thread
  dropped nothing.
* **CSDM crash investigation** (2026-10-13): a crash the player met in CSDM could not be
  reproduced: 2 h with five servers, map changes every 5 min and players joining and
  leaving, then Memcheck runs with a Japanese player name and with a real client's
  userinfo, all clean. The investigation stopped there, the server being stable in
  play since.
* **Combined build** (`ef9a314`, 2026-10-13): every feature on, one player with 19 bots
  in CSDM with grenades. Counters at the end of the session: 70 859 snapshots encoded by
  the workers and 4 851 by the main thread, none re-encoded after a replay mismatch (10
  full updates not recorded); 75 962 packets sent by the send thread, no error, queue
  never full; 6 browser queries answered by the network thread with their challenge;
  1 916 log lines written asynchronously, none dropped, the main thread never waited.
  With one player the send thread hands about one packet per `sendmmsg` call: batching
  pays with several players.
* **Steam calls** (2026-10-13, 19 bots, 60 s profiles): the Steam phase took 10 % of the
  main thread's frame work, mostly `GetNextOutgoingPacket` every 10 ms (39 µs each,
  nearly always for nothing) and the once a second player data update (120 to 660 µs).
  With `sv_steam_on_demand 1`: 4.6 → 1.0 ms per second, longest 659 → 306 µs; queries
  handed to Steam answered in 0.4 ms; the server stays registered with the master
  server.
* **Bots leaving**: with `bot_join_after_player 1` (the default), the bots left whenever
  no human was on the server, so idle profiles had no load. Now 0.
* Still missing for the go/no-go: a frame profile with several players, the comparison
  session with the features off, and any run on Windows.
