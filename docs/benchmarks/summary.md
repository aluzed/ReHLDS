# Before / after summary

Where the fork starts from and where it is, scenario by scenario. "Before" is the engine
with its default settings, which behaves like upstream ReHLDS (every new option is off by
default). "After" is the fork with the recommended settings:

```
-netthread -sys_threads 2 +sys_ticrate 1000 +sys_scheduler 1 +sv_rehlds_snapshot_spread 1 +sv_parallel_send 1
```

Same machine for every run: Ryzen 9 3950X, Linux 7.0, `powersave` governor, no CPU
pinning, server at `sys_ticrate 1000`. "Cores" are CPU time in fractions of one core:
100 % = one core fully busy. Details and methods in the linked reports.

## Idle server ([`mt101-scheduler.md`](mt101-scheduler.md))

| | Before | After | Change |
|---|---|---|---|
| Server FPS (target 1000) | 935.7 | 1000.0 | +7 %, exact |
| Tick interval p99 | 1118 µs | 1018 µs | −9 % |
| Tick jitter (stddev) | 14.4 µs | 4.6 µs | ÷3 |
| CPU | 1.9 % | 1.7 % | = |

## 20 bots and 8 HLTV proxies ([`mt101-scheduler.md`](mt101-scheduler.md))

| | Before | After | Change |
|---|---|---|---|
| Server FPS | 908.1 | 999.9 | +10 %, exact |
| Tick interval p95 / p99 | 1205 / 1386 µs | 1004 / 1028 µs | −26 % at p99 |
| Tick jitter (stddev) | 82.5 µs | 33.3 µs | ÷2.5 |
| CPU | 4.8 % | 5.0 % | +10 % frames for +4 % CPU |

## Full server: 32 players, ordinary rounds ([`clients32.md`](clients32.md))

| | Before | After | Change |
|---|---|---|---|
| Server FPS | 878.6 | 1000.0 | +14 %, exact |
| Tick jitter (stddev) | 147.4 µs | 33.1 µs | ÷4.5 |
| Frame p95 / p99 | 450 / 587 µs | 193 / 259 µs | −56 % at p99 |
| Snapshots per client (`cl_updaterate 100`) | 97.5 /s | 100.0 /s | full rate |
| Snapshot interval jitter, client side | 0.2 ms | 0.1 ms | ÷2 |
| CPU | 7.9 % | 8.5 % | ≈ (+14 % frames) |

The load clients of this report stayed dead spectators (see `clients32-ents.md`): the
snapshots are small, there is nothing to encode in parallel, the gain comes from the
scheduler and snapshot spreading.

## Full server, many entities: 32 playing clients, 400 moving entities ([`clients32-ents.md`](clients32-ents.md))

| | Before | After | Change |
|---|---|---|---|
| Server FPS | 607.2 | 1000.0 | +65 %, exact |
| Tick interval p99 | 3497 µs | 1711 µs | −51 % |
| Tick jitter (stddev) | 617 µs | 222 µs | ÷2.8 |
| Frame avg / p99 | 588 / 2429 µs | 346 / 1673 µs | −41 % / −31 % |
| Snapshot send, main thread, avg / p99 | 319 / 1770 µs | 152 / 498 µs | −52 % / −72 % |
| Snapshot encoding on the main thread, avg / p99 | 164 / 943 µs | 48 / 99 µs | −71 % / −90 % |
| Main thread CPU | 35.9 % | 35.1 % | = for +65 % frames |
| Whole process CPU (cores used) | 36.5 % | 59.1 % | +0.23 core (2 encoding threads) |
| Snapshots per client | 59.7 /s | 58.2 /s | = (limited by `sv_maxrate`) |

The snapshots carry ≈ 150 entities and exceed `sv_maxrate 100000`: the clients get what
their rate allows in both cases. The frame p99 still exceeds the 1 ms tick: it is the
game DLL's player movement and `AddToFullPack`, which the engine cannot move off the
main thread.

## Full server under a query flood: 50 000 packets/s from 4096 addresses ([`flood.md`](flood.md))

| | Before | After | Change |
|---|---|---|---|
| Server FPS | 45.1 | 1000.0 | ×22 |
| Frame avg / p99 | 21 567 / 149 762 µs | 75 / 224 µs | ÷290 / ÷670 |
| Snapshots per client | 14.8 /s | 99.8 /s | full rate |
| Snapshot interval p99, client side | 252.7 ms | 10.9 ms | ÷23 |
| Main thread CPU | 97.6 % | 7.9 % | ÷12 |
| Whole process CPU (cores used) | 97.9 % | 31.3 % | −0.67 core |

"Before" is the original engine, which fell to 45 fps under the flood (the IP rate
limiter walked its whole table on every query). That fix is always on in the fork; the
network thread then drops the flood for ≈ 23 % of a core, and the main thread no longer
sees it.

## What it costs

| | Before | After |
|---|---|---|
| Threads used by the server | 1 (+ Steam) | 1 game loop + 1 network thread + 2 encoding threads |
| Extra CPU, quiet server | – | ≈ 0 (the threads sleep) |
| Extra CPU, heavy server | – | ≈ 0.25 core (encoding moved off the main thread, plus spinning) |
| Extra CPU, query flood | – | ≈ 0.25 core (filtering the flood) |
| Output to the clients | – | byte-identical for the same inputs (replays) |
| Plugins | – | game DLL, Metamod, AMX Mod X and ReAPI hooks still on the main thread |

## Where it goes

* The engine side of the frame is now small and regular; what is left above the 1 ms
  tick on busy servers is game DLL time (player movement, `AddToFullPack`, bots' AI),
  which runs on the main thread by design.
* Field validation with several players (MT-702) is in progress; Windows is written but
  not built or tested yet.
