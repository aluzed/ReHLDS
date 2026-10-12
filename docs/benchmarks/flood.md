# Full server under packet floods: network thread (MT-302, MT-303)

Scenarios `flood-a2s` and `flood-junk`: the `clients32` full server (32 protocol load
clients at 100 commands/s, `cl_updaterate 100`, `sys_ticrate 1000`) while
`tools/bench/flood.py` sends 50 000 packets/s from 4096 source addresses during the
whole measurement window. Every run uses `+sys_scheduler 1 +sv_rehlds_snapshot_spread 1`
(MT-101, MT-106); "thread" adds `-netthread`. Same machine as the other reports
(Ryzen 9 3950X, Linux 7.0, `powersave`).

## A2S query flood

| | No flood | Flood, sync, before the rate limiter fix | Flood, sync | Flood, network thread |
|---|---|---|---|---|
| Server FPS | 1000.0 | **45.1** | 1000.0 | 1000.0 |
| Tick jitter (stddev) | 17.3 µs | 31 775 µs | 27.8 µs | **15.4 µs** |
| Frame avg / p99 | 75 / 263 µs | 21 567 / 149 762 µs | 160 / 552 µs | **75 / 224 µs** |
| `read_packets` avg / p99 | 20 / 186 µs | 21 366 / 149 680 µs | 102 / 473 µs | **15 / 145 µs** |
| Packets read by the main thread | 3.1 /frame | 532 /frame | 53 /frame | **3.1 /frame** |
| Main thread CPU | 8.0 % | 97.6 % | 16.4 % | **7.9 %** |
| Process CPU (all threads) | 8.3 % | 97.9 % | 16.9 % | 31.3 % |
| Snapshots per client | 100.0 /s | 14.8 /s | 99.7 /s | 99.8 /s |
| Snapshot interval p99 (client side) | 10.3 ms | 252.7 ms | 10.6 ms | 10.9 ms |

* **Before the fix, 50 000 queries/s took the server down to 45 FPS** and 15 snapshots/s
  per player (250 ms p99 gaps). `CIPRateLimit::CheckIP` walks its whole tree to trim it
  on *every* packet once it tracks more than 512 addresses, and finds nothing to trim
  while every address is active: tens of microseconds per query. It now trims at most
  once per second (`REHLDS_FIXES`, `engine/ipratelimit.cpp`); trimming only removes
  addresses idle for 120 s, so the result is the same.
* With the fix, the synchronous path keeps 1000 FPS but the main thread still reads
  every query: `read_packets` ×5, frame p99 ×2, main thread CPU ×2.
* **With the network thread, the main thread does not see the flood**: its frame times,
  packet count and CPU are those of the no-flood run. The thread drops the queries
  (1 498 264 rate limited out of 1 660 601 received) and uses about 23 % of a core for
  50 000 packets/s. No packet was lost in the queue or the socket.
* The client-side snapshot jitter rises from 0.1 to 1.0 ms with both the synchronous
  path and the thread, while the server's send timings are unchanged: most likely the
  load clients and the flooder competing for loopback processing. It does not show with
  the junk flood (0.1–0.2 ms).

Before the fix, the network thread kept the tick at 1000 FPS as well, but it spent a full
core in the rate limiter and read only about 27 000 of the 50 000 packets/s: the kernel
dropped the rest, legitimate packets included.

## Junk flood (connected-client traffic)

| | No flood | Flood, sync | Flood, network thread |
|---|---|---|---|
| Server FPS | 1000.0 | 999.9 | 1000.0 |
| Tick jitter (stddev) | 17.3 µs | 32.6 µs | **17.7 µs** |
| Frame avg / p99 | 75 / 263 µs | 156 / 510 µs | **87 / 240 µs** |
| `read_packets` avg / p99 | 20 / 186 µs | 100 / 434 µs | **28 / 160 µs** |
| Main thread CPU | 8.0 % | 16.1 % | **9.2 %** |
| Snapshots per client | 100.0 /s | 100.0 /s | 100.0 /s |

Packets that are not connectionless are not filtered by the thread: the main thread
still matches each one against the client list (53 packets/frame). The thread removes
the receive syscalls from the main thread (`read_packets` −72 %), but the frame average
stays 15 % above the no-flood run. Dropping packets from addresses that are not
connected clients on the thread would remove the rest; it needs the client address list
published to the thread (follow-up, see MT-303).

## Functional checks with the thread

Same results with and without `-netthread`:

* an A2S query is answered; after `addip 0 127.2.0.0/16`, queries from that range get
  "You have been banned from this server." at once, others are still answered;
  `removeip` restores answers immediately;
* map change, `restart` and `quit` with the thread running; exit code 0.

## Reproduce

```sh
tools/bench/build.sh mt302
A="+sys_scheduler 1 +sv_rehlds_snapshot_spread 1"
BENCH_SERVER_ARGS="$A" tools/bench/run.sh clients32 mt302
BENCH_SERVER_ARGS="$A" tools/bench/run.sh flood-a2s mt302
BENCH_SERVER_ARGS="-netthread $A" tools/bench/run.sh flood-a2s mt302
BENCH_SERVER_ARGS="$A" tools/bench/run.sh flood-junk mt302
BENCH_SERVER_ARGS="-netthread $A" tools/bench/run.sh flood-junk mt302
```
