# Baseline: MT-002 (profiler only)

Reference numbers before any timing or threading change. Engine built from
`feature/multithreading` at `9fd125e` (upstream `550f2d6` + frame profiler), measured
with `tools/bench` on 2026-10-11.

Machine: AMD Ryzen 9 3950X, Linux 7.0, `powersave` governor, no CPU pinning.
Game: stock `steam_legacy` HLDS (cstrike) with ReGameDLL_CS `4a50c42`.

## Takeaways

* **The server never reaches `sys_ticrate 1000`**: 936 fps empty, 908 fps with bots
  and HLTV, while it is busy only 1–4 % of the time. The 1 ms launcher sleep plus its
  overshoot sets the pace, not the work (MT-101).
* **Physics (game DLL: bot AI, think, movement) dominates the frame** and produces the
  spikes: p99 ≈ 340 µs, max ≈ 1.8–2.7 ms with 20 bots. This part stays on the main
  thread by design.
* **Snapshot sending** with 8 HLTV proxies: p99 ≈ 115 µs per frame, split roughly into
  equal thirds between gather (game DLL), encode and transmit.
* Run-to-run variance: tick metrics (fps, tick p50/p95) are stable within 1–2 %;
  per-phase averages and p99 move by 10–20 % because the bots' behaviour differs
  between runs. Compare phases over several runs.

## idle

| Summary | Value |
|---|---|
| Frames | 28070 over 30.0 s |
| Server FPS | 935.7 |
| Tick jitter (stddev) | 14.4 us |
| Busy (frame / tick) | 1.1 % |
| Process CPU | 1.9 % of one core |

| Time (us) | avg | p50 | p95 | p99 | max |
|---|---|---|---|---|---|
| tick_interval | 1068.7 | 1064.2 | 1100.7 | 1118.2 | 1302.7 |
| frame | 11.3 | 6.5 | 42.9 | 58.8 | 247.8 |
| &nbsp;&nbsp;cbuf | 0.1 | 0.1 | 0.2 | 0.2 | 8.0 |
| &nbsp;&nbsp;sv_frame | 10.5 | 5.9 | 41.9 | 57.0 | 246.9 |
| &nbsp;&nbsp;&nbsp;&nbsp;read_packets | 1.8 | 1.5 | 3.1 | 3.7 | 27.0 |
| &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;connectionless | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 |
| &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;netchan_process | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 |
| &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;execute_client_msg | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 |
| &nbsp;&nbsp;&nbsp;&nbsp;physics | 3.6 | 3.1 | 5.1 | 6.6 | 64.8 |
| &nbsp;&nbsp;&nbsp;&nbsp;check_timeouts | 0.1 | 0.1 | 0.2 | 0.3 | 9.4 |
| &nbsp;&nbsp;&nbsp;&nbsp;send_messages | 0.7 | 0.6 | 1.1 | 1.3 | 15.3 |
| &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;datagram | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 |
| &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;gather | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 |
| &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;encode | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 |
| &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;transmit | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 |
| &nbsp;&nbsp;&nbsp;&nbsp;steam | 4.0 | 0.1 | 35.4 | 48.5 | 241.3 |
| &nbsp;&nbsp;log_write | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 |

| Per frame | avg | p99 | max |
|---|---|---|---|
| filter_rejects | 0.00 | 0 | 0 |
| packets_in | 0.00 | 0 | 0 |
| bytes_in | 0.00 | 0 | 0 |
| packets_out | 0.00 | 0 | 0 |
| bytes_out | 0.00 | 0 | 0 |
| clients_sent | 0.00 | 0 | 0 |
| entities_sent | 0.00 | 0 | 0 |

## bots20

| Summary | Value |
|---|---|
| Frames | 27355 over 30.0 s |
| Server FPS | 911.8 |
| Tick jitter (stddev) | 110.3 us |
| Busy (frame / tick) | 3.5 % |
| Process CPU | 4.3 % of one core |

| Time (us) | avg | p50 | p95 | p99 | max |
|---|---|---|---|---|---|
| tick_interval | 1096.7 | 1070.1 | 1188.2 | 1612.8 | 5284.2 |
| frame | 38.7 | 11.5 | 129.0 | 553.0 | 4225.8 |
| &nbsp;&nbsp;cbuf | 0.1 | 0.1 | 0.2 | 0.2 | 13.1 |
| &nbsp;&nbsp;sv_frame | 37.8 | 10.7 | 128.1 | 549.3 | 4224.6 |
| &nbsp;&nbsp;&nbsp;&nbsp;read_packets | 2.0 | 1.7 | 3.3 | 3.9 | 51.1 |
| &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;connectionless | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 |
| &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;netchan_process | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 |
| &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;execute_client_msg | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 |
| &nbsp;&nbsp;&nbsp;&nbsp;physics | 29.5 | 6.7 | 115.7 | 516.0 | 4173.3 |
| &nbsp;&nbsp;&nbsp;&nbsp;check_timeouts | 0.2 | 0.1 | 0.3 | 0.4 | 18.9 |
| &nbsp;&nbsp;&nbsp;&nbsp;send_messages | 0.7 | 0.6 | 1.1 | 1.4 | 20.2 |
| &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;datagram | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 |
| &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;gather | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 |
| &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;encode | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 |
| &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;transmit | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 |
| &nbsp;&nbsp;&nbsp;&nbsp;steam | 4.9 | 0.1 | 39.2 | 53.3 | 787.8 |
| &nbsp;&nbsp;log_write | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 |

| Per frame | avg | p99 | max |
|---|---|---|---|
| filter_rejects | 0.00 | 0 | 0 |
| packets_in | 0.00 | 0 | 0 |
| bytes_in | 0.00 | 0 | 0 |
| packets_out | 0.00 | 0 | 0 |
| bytes_out | 0.00 | 0 | 0 |
| clients_sent | 0.00 | 0 | 0 |
| entities_sent | 0.00 | 0 | 0 |

## bots20-hltv8

| Summary | Value |
|---|---|
| Frames | 27244 over 30.0 s |
| Server FPS | 908.1 |
| Tick jitter (stddev) | 82.5 us |
| Busy (frame / tick) | 3.9 % |
| Process CPU | 4.8 % of one core |

| Time (us) | avg | p50 | p95 | p99 | max |
|---|---|---|---|---|---|
| tick_interval | 1101.2 | 1075.8 | 1204.8 | 1385.8 | 3732.5 |
| frame | 43.5 | 16.9 | 145.9 | 327.3 | 2673.3 |
| &nbsp;&nbsp;cbuf | 0.1 | 0.1 | 0.2 | 0.2 | 3.7 |
| &nbsp;&nbsp;sv_frame | 42.4 | 15.7 | 144.5 | 326.2 | 2672.3 |
| &nbsp;&nbsp;&nbsp;&nbsp;read_packets | 2.8 | 2.0 | 7.0 | 9.3 | 37.5 |
| &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;connectionless | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 |
| &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;netchan_process | 0.1 | 0.0 | 0.4 | 0.7 | 13.0 |
| &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;execute_client_msg | 0.1 | 0.0 | 0.8 | 1.2 | 18.5 |
| &nbsp;&nbsp;&nbsp;&nbsp;physics | 26.5 | 8.2 | 112.9 | 291.7 | 2668.5 |
| &nbsp;&nbsp;&nbsp;&nbsp;check_timeouts | 0.3 | 0.3 | 0.4 | 0.5 | 10.4 |
| &nbsp;&nbsp;&nbsp;&nbsp;send_messages | 7.4 | 1.0 | 47.5 | 113.7 | 180.9 |
| &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;datagram | 4.1 | 0.0 | 29.8 | 76.3 | 131.5 |
| &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;gather | 2.0 | 0.0 | 14.7 | 36.8 | 71.8 |
| &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;encode | 2.1 | 0.0 | 15.2 | 40.2 | 80.5 |
| &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;transmit | 2.1 | 0.0 | 16.2 | 33.7 | 59.4 |
| &nbsp;&nbsp;&nbsp;&nbsp;steam | 4.8 | 0.1 | 37.5 | 44.8 | 870.6 |
| &nbsp;&nbsp;log_write | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 |

| Per frame | avg | p99 | max |
|---|---|---|---|
| filter_rejects | 0.00 | 0 | 0 |
| packets_in | 0.21 | 2 | 3 |
| bytes_in | 3.33 | 32 | 74 |
| packets_out | 0.26 | 5 | 5 |
| bytes_out | 46.68 | 984 | 4665 |
| clients_sent | 0.26 | 5 | 5 |
| entities_sent | 15.39 | 295 | 320 |

