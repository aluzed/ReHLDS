# Full server with many entities: baseline for epic 4

`clients32-ents` scenario: the 32 protocol load clients of `clients32`, but **playing**
(they join a team and spawn), plus 400 entities flying around them
(`tools/bench/plugins/bench_entities.sma`, loaded through Metamod-R and AMX Mod X). Every
snapshot carries the other players and most of the entities near them: about 150
packet entities per snapshot, up to the 256 limit. `sys_ticrate 1000`,
`cl_updaterate 100`, `sv_maxrate 100000`. Same machine as the other reports.

This is the load epic 4 (parallel snapshot encoding) targets: on the plain `clients32`
scenario, the remaining send cost was too small to be worth parallelising
([`clients32.md`](clients32.md)).

## Load clients were dead in the earlier reports

The load clients used to send `jointeam` and `joinclass` right after signing on. The
game DLL only accepts a team once the player has closed the message of the day, so the
clients stayed dead spectators: they did not see each other, and each snapshot carried
about 13 map entities. With `CLIENTS_ALIVE=1` (`loadclient.py --play`), they close the
message of the day, join, and the round is restarted once they are all in. The earlier
scenarios keep the old behaviour so that their reports stay comparable; their
`send_messages` numbers underestimate a real full server.

## Baseline

Engine `49dd8b8` (3.15.0-mt.1).

| | Default | Scheduler + spread |
|---|---|---|
| Server FPS | 607.2 | 999.3 |
| Tick jitter (stddev) | 617 µs | 289 µs |
| Frame avg / p99 | 588 / 2429 µs | 453 / 1896 µs |
| `send_messages` avg / p99 | 319 / 1770 µs | 251 / 790 µs |
| &nbsp;&nbsp;gather (game DLL) avg / p99 | 127 / 703 µs | 102 / 321 µs |
| &nbsp;&nbsp;encode avg / p99 | 164 / 943 µs | 126 / 404 µs |
| &nbsp;&nbsp;transmit avg / p99 | 25 / 130 µs | 21 / 62 µs |
| `execute_client_msg` avg / p99 | 181 / 1344 µs | 123 / 1395 µs |
| Packet entities per snapshot | 167 | 157 |
| Snapshots per client | 59.7 /s | 79.8 /s |
| Main thread CPU | 35.9 % | 45.9 % |

* **The frame no longer fits in a 1 ms tick at p99**, even with the scheduler and
  snapshot spreading: more than one frame in twenty (p95 1556 µs) is longer than a tick.
* **Encoding is now the largest phase of the send**: about 50 µs per client snapshot of
  ≈ 150 entities. Encode and transmit (≈ 146 µs per frame on average, ≈ 470 µs at p99)
  are what epic 4 can move to other cores; the gather (≈ 100 µs, game DLL
  `AddToFullPack` over 400+ entities per client) stays on the main thread.
* The clients get 60–80 snapshots/s instead of 100: snapshots of 150 entities exceed
  `sv_maxrate` and are choked. This is the rate limit, not the server's speed.
* `execute_client_msg` (player movement in the game DLL) has its own p99 spikes of
  1.4 ms, when many clients' commands arrive in the same frame. Out of reach of epic 4.
* Runs vary more than the other scenarios (snapshots per client between 64 and 80 /s
  over three runs): the entities follow random players.

## Reproduce

```sh
tools/bench/setup.sh        # once: installs Metamod-R, AMX Mod X and the bench plugins
tools/bench/build.sh base
BENCH_SERVER_ARGS="+sys_scheduler 1 +sv_rehlds_snapshot_spread 1" tools/bench/run.sh clients32-ents base
```
