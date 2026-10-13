# MT-101: deadline-based frame scheduler

Engine built from `feature/multithreading` with the frame scheduler (MT-101), run with
`+sys_scheduler 1` (default slice 100 ms, no spin), compared with the MT-002 baseline
([baseline-mt002.md](baseline-mt002.md)). Same machine and setup as the baseline.

## Summary

| Scenario | Metric | Baseline | Scheduler |
|---|---|---|---|
| idle, `sys_ticrate 1000` | server FPS | 935.7 | **1000.0** |
| | tick p99 | 1118 µs | **1018 µs** |
| | tick jitter (stddev) | 14.4 µs | **4.6 µs** |
| | process CPU | 1.9 % | 1.7 % |
| idle, `sys_ticrate 100` | server FPS | 93.7 | **100.0** |
| | wasted wake-ups per frame | 9 | **0** |
| | tick jitter (stddev) | – | 2.1 µs |
| bots20-hltv8, `sys_ticrate 1000` | server FPS | 908.1 | **999.9** |
| | tick p95 / p99 | 1205 / 1386 µs | **1004 / 1028 µs** |
| | tick jitter (stddev) | 82.5 µs | **33.3 µs** |
| | process CPU | 4.8 % | 5.0 % (+10 % frames) |

With the scheduler off (`sys_scheduler 0`, default), the same build matches the baseline
(906 fps on bots20-hltv8, within run-to-run noise).

Tick distribution, idle at 1000 Hz: 97.9 % of ticks within ±10 µs of 1 ms. The remaining
ones come from the return to the launcher every slice (its ~1.07 ms sleep is longer than
one tick, so one tick is late and the next one catches up) and from rare system noise.
A 50 ms slice gave 95.9 % within ±10 µs; 100 ms was kept as the default.

Also checked: the ReHLDS launcher (`rehlds/dedicated`) gives the same 1000.0 fps as the
stock Valve launcher, and changing `sys_ticrate` at runtime (1000 → 500) applies at the
next slice (500.0 fps, 1.9 µs jitter).

## bots20-hltv8, scheduler vs baseline

* Server args: `+sys_scheduler 1`

| Summary | Value |
|---|---|
| Frames | 29997 over 30.0 s |
| Server FPS | 999.9 (+10%) |
| Tick jitter (stddev) | 33.3 (-60%) us |
| Busy (frame / tick) | 4.2 (+6%) % |
| Process CPU | 5.0 (+5%) % of one core |

| Time (us) | avg | p50 | p95 | p99 | max |
|---|---|---|---|---|---|
| tick_interval | 1000.1 (-9%) | 999.9 (-7%) | 1004.1 (-17%) | 1028.1 (-26%) | 3217.8 (-14%) |
| frame | 42.0 (-4%) | 15.6 (-8%) | 136.2 (-7%) | 467.7 (+43%) | 3205.8 (+20%) |
| &nbsp;&nbsp;cbuf | 0.1 (+4%) | 0.1 (+0%) | 0.2 (+0%) | 0.2 (+0%) | 2.8 (-24%) |
| &nbsp;&nbsp;sv_frame | 41.0 (-3%) | 14.6 (-7%) | 134.7 (-7%) | 466.2 (+43%) | 3204.6 (+20%) |
| &nbsp;&nbsp;&nbsp;&nbsp;read_packets | 2.8 (-1%) | 2.0 (+0%) | 6.6 (-6%) | 8.5 (-9%) | 31.9 (-15%) |
| &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;connectionless | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 |
| &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;netchan_process | 0.1 (-2%) | 0.0 | 0.4 (+0%) | 0.7 (+0%) | 1.8 (-86%) |
| &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;execute_client_msg | 0.1 (-7%) | 0.0 | 0.8 (+0%) | 1.1 (-8%) | 24.1 (+30%) |
| &nbsp;&nbsp;&nbsp;&nbsp;physics | 25.7 (-3%) | 7.4 (-10%) | 74.4 (-34%) | 405.3 (+39%) | 3149.9 (+18%) |
| &nbsp;&nbsp;&nbsp;&nbsp;check_timeouts | 0.2 (-8%) | 0.2 (-33%) | 0.4 (+0%) | 0.5 (+0%) | 10.9 (+5%) |
| &nbsp;&nbsp;&nbsp;&nbsp;send_messages | 7.2 (-3%) | 1.0 (+0%) | 47.2 (-1%) | 99.7 (-12%) | 279.6 (+55%) |
| &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;datagram | 4.0 (-4%) | 0.0 | 30.6 (+3%) | 66.8 (-12%) | 208.5 (+59%) |
| &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;gather | 1.9 (-6%) | 0.0 | 14.5 (-1%) | 31.4 (-15%) | 87.8 (+22%) |
| &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;encode | 2.1 (-1%) | 0.0 | 16.0 (+5%) | 35.5 (-12%) | 131.4 (+63%) |
| &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;transmit | 2.0 (-4%) | 0.0 | 15.1 (-7%) | 30.0 (-11%) | 68.6 (+15%) |
| &nbsp;&nbsp;&nbsp;&nbsp;steam | 4.5 (-6%) | 0.1 (+0%) | 37.4 (-0%) | 46.8 (+4%) | 795.7 (-9%) |
| &nbsp;&nbsp;log_write | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 |

| Per frame | avg | p99 | max |
|---|---|---|---|
| filter_rejects | 0.00 | 0 | 0 |
| packets_in | 0.19 (-9%) | 2 | 3 |
| bytes_in | 3.02 (-9%) | 32 | 116 |
| packets_out | 0.24 (-9%) | 4 | 7 |
| bytes_out | 50.74 (+9%) | 916 | 4260 |
| clients_sent | 0.24 (-9%) | 4 | 7 |
| entities_sent | 13.84 (-10%) | 236 | 441 |

## idle, scheduler vs baseline

* Server args: `+sys_scheduler 1`

| Summary | Value |
|---|---|
| Frames | 30000 over 30.0 s |
| Server FPS | 1000.0 (+7%) |
| Tick jitter (stddev) | 4.6 (-68%) us |
| Busy (frame / tick) | 1.0 (-6%) % |
| Process CPU | 1.7 (-11%) % of one core |

| Time (us) | avg | p50 | p95 | p99 | max |
|---|---|---|---|---|---|
| tick_interval | 1000.0 (-6%) | 1000.0 (-6%) | 1001.7 (-9%) | 1017.9 (-9%) | 1190.1 (-9%) |
| frame | 9.9 (-12%) | 5.9 (-9%) | 39.1 (-9%) | 52.0 (-12%) | 351.3 (+42%) |
| &nbsp;&nbsp;cbuf | 0.1 (-7%) | 0.1 (+0%) | 0.1 (-50%) | 0.2 (+0%) | 0.4 (-95%) |
| &nbsp;&nbsp;sv_frame | 9.2 (-12%) | 5.3 (-10%) | 38.1 (-9%) | 50.2 (-12%) | 349.3 (+41%) |
| &nbsp;&nbsp;&nbsp;&nbsp;read_packets | 1.6 (-10%) | 1.4 (-7%) | 2.6 (-16%) | 3.6 (-3%) | 6.0 (-78%) |
| &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;connectionless | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 |
| &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;netchan_process | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 |
| &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;execute_client_msg | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 |
| &nbsp;&nbsp;&nbsp;&nbsp;physics | 3.2 (-11%) | 2.9 (-6%) | 4.4 (-14%) | 5.6 (-15%) | 52.0 (-20%) |
| &nbsp;&nbsp;&nbsp;&nbsp;check_timeouts | 0.1 (-13%) | 0.1 (+0%) | 0.2 (+0%) | 0.2 (-33%) | 0.9 (-90%) |
| &nbsp;&nbsp;&nbsp;&nbsp;send_messages | 0.6 (-13%) | 0.5 (-17%) | 0.9 (-18%) | 1.2 (-8%) | 4.3 (-72%) |
| &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;datagram | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 |
| &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;gather | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 |
| &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;encode | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 |
| &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;transmit | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 |
| &nbsp;&nbsp;&nbsp;&nbsp;steam | 3.4 (-13%) | 0.1 (+0%) | 32.3 (-9%) | 43.0 (-11%) | 336.8 (+40%) |
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
