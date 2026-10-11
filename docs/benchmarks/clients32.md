# Full server: 32 protocol load clients

`clients32` scenario: 32 load clients (`tools/bench/loadclient.py`) running around
`de_dust2`, each sending 100 commands/s, `cl_updaterate 100`, server at
`sys_ticrate 1000`, `sv_maxupdaterate 100`. Engine from `feature/multithreading` with
MT-101 (scheduler) and MT-106 (snapshot spreading), each switched by its cvar. Same
machine as the other reports (Ryzen 9 3950X, Linux 7.0, `powersave`).

## Summary

| | Default | Scheduler | Spread | Scheduler + spread |
|---|---|---|---|---|
| Server FPS | 878.6 | 1000.0 | 872.8 | **1000.0** |
| Tick jitter (stddev) | 147.4 µs | 26.7 µs | 64.0 µs | **33.1 µs** |
| Frame p95 / p99 | 450 / 587 µs | 452 / 571 µs | 221 / 299 µs | **193 / 259 µs** |
| `send_messages` p99 | 479 µs | 466 µs | 92 µs | **75 µs** |
| Snapshots per client | 97.5 /s | 94.3 /s | 100.0 /s | **100.0 /s** |
| Snapshot interval p99 (client side) | 10.8 ms | 11.9 ms | 10.7 ms | **10.3 ms** |
| Snapshot interval jitter (client side) | 0.2 ms | 0.6 ms | 0.5 ms | **0.1 ms** |
| Process CPU | 7.9 % | 7.9 % | 8.5 % | 8.5 % |

* **All 32 snapshots were built in the same frame** with the default schedule (one frame
  in 9–11, nothing in between). Spreading gives 3–4 per frame and cuts the send p99 by
  more than 80 %. This was the main source of frame spikes on a full server.
* The scheduler alone made the client-side rate slightly worse (94/s): perfectly regular
  ticks land exactly on the 10 ms deadline. The phase-aligned schedule fixes it.
* Both together: exact 1000 Hz ticks, exact 100 snapshots/s, 0.1 ms client-side jitter.
* What remains per frame on a full server: `send_messages` ≈ 45 µs and `read_packets`
  ≈ 23 µs on average, for a 1000 µs budget. See the decision gate in
  [`../stories/README.md`](../stories/README.md).

## Default

| Summary | Value |
|---|---|
| Frames | 26357 over 30.0 s |
| Server FPS | 878.6 |
| Tick jitter (stddev) | 147.4 us |
| Busy (frame / tick) | 7.1 % |
| Process CPU | 7.9 % of one core |

| Load clients in game | 32 / 32 |
| Snapshots per client | 97.5 /s |
| Snapshot interval p50 / p99 | 10.2 / 10.8 ms |
| Snapshot interval jitter (stddev) | 0.2 ms |

| Time (us) | avg | p50 | p95 | p99 | max |
|---|---|---|---|---|---|
| tick_interval | 1138.2 | 1070.8 | 1508.8 | 1647.3 | 2434.6 |
| frame | 80.3 | 11.2 | 450.3 | 587.3 | 1374.2 |
| &nbsp;&nbsp;cbuf | 0.1 | 0.1 | 0.2 | 0.2 | 12.5 |
| &nbsp;&nbsp;sv_frame | 79.4 | 10.1 | 449.3 | 586.1 | 1323.4 |
| &nbsp;&nbsp;&nbsp;&nbsp;read_packets | 22.8 | 1.8 | 128.3 | 167.8 | 363.0 |
| &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;connectionless | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 |
| &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;netchan_process | 0.5 | 0.0 | 2.8 | 4.0 | 29.9 |
| &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;execute_client_msg | 13.5 | 0.0 | 78.6 | 108.0 | 253.6 |
| &nbsp;&nbsp;&nbsp;&nbsp;physics | 4.0 | 3.5 | 5.9 | 9.5 | 68.1 |
| &nbsp;&nbsp;&nbsp;&nbsp;check_timeouts | 0.3 | 0.2 | 0.5 | 0.7 | 10.6 |
| &nbsp;&nbsp;&nbsp;&nbsp;send_messages | 46.1 | 1.4 | 392.7 | 478.6 | 927.6 |
| &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;datagram | 24.8 | 0.0 | 217.3 | 260.6 | 372.5 |
| &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;gather | 12.4 | 0.0 | 108.1 | 131.7 | 200.7 |
| &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;encode | 12.4 | 0.0 | 109.0 | 132.1 | 192.4 |
| &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;transmit | 19.3 | 0.0 | 168.8 | 207.2 | 709.9 |
| &nbsp;&nbsp;&nbsp;&nbsp;steam | 5.7 | 0.1 | 42.1 | 53.6 | 932.1 |
| &nbsp;&nbsp;log_write | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 |

| Per frame | avg | p99 | max |
|---|---|---|---|
| filter_rejects | 0.00 | 0 | 0 |
| packets_in | 3.52 | 28 | 32 |
| bytes_in | 83.91 | 668 | 770 |
| packets_out | 3.49 | 32 | 32 |
| bytes_out | 137.83 | 1270 | 2041 |
| clients_sent | 3.49 | 32 | 32 |
| entities_sent | 40.54 | 416 | 424 |

## Scheduler + spread vs default

* Server args: `+sys_scheduler 1 +sv_rehlds_snapshot_spread 1`

| Summary | Value |
|---|---|
| Frames | 30000 over 30.0 s |
| Server FPS | 1000.0 (+14%) |
| Tick jitter (stddev) | 33.1 (-78%) us |
| Busy (frame / tick) | 7.7 (+9%) % |
| Process CPU | 8.5 (+8%) % of one core |

| Load clients in game | 32 / 32 |
| Snapshots per client | 100.0 (+3%) /s |
| Snapshot interval p50 / p99 | 10.0 (-2%) / 10.3 (-5%) ms |
| Snapshot interval jitter (stddev) | 0.1 (-27%) ms |

| Time (us) | avg | p50 | p95 | p99 | max |
|---|---|---|---|---|---|
| tick_interval | 1000.0 (-12%) | 1000.0 (-7%) | 1003.7 (-33%) | 1063.0 (-35%) | 3313.8 (+36%) |
| frame | 77.2 (-4%) | 57.4 (+412%) | 193.1 (-57%) | 259.4 (-56%) | 2108.3 (+53%) |
| &nbsp;&nbsp;cbuf | 0.1 (+4%) | 0.1 (+0%) | 0.2 (+0%) | 0.2 (+0%) | 18.1 (+45%) |
| &nbsp;&nbsp;sv_frame | 76.1 (-4%) | 56.5 (+459%) | 191.9 (-57%) | 257.5 (-56%) | 2105.2 (+59%) |
| &nbsp;&nbsp;&nbsp;&nbsp;read_packets | 20.7 (-9%) | 1.7 (-6%) | 142.7 (+11%) | 183.1 (+9%) | 2011.1 (+454%) |
| &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;connectionless | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 |
| &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;netchan_process | 0.4 (-11%) | 0.0 | 3.1 (+11%) | 4.3 (+7%) | 28.5 (-5%) |
| &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;execute_client_msg | 12.0 (-11%) | 0.0 | 87.9 (+12%) | 120.4 (+11%) | 240.0 (-5%) |
| &nbsp;&nbsp;&nbsp;&nbsp;physics | 4.1 (+2%) | 3.6 (+3%) | 5.9 (+0%) | 9.3 (-2%) | 75.8 (+11%) |
| &nbsp;&nbsp;&nbsp;&nbsp;check_timeouts | 0.3 (+7%) | 0.3 (+50%) | 0.5 (+0%) | 0.7 (+0%) | 23.0 (+117%) |
| &nbsp;&nbsp;&nbsp;&nbsp;send_messages | 45.5 (-1%) | 42.0 (+2900%) | 62.3 (-84%) | 75.2 (-84%) | 271.3 (-71%) |
| &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;datagram | 23.5 (-5%) | 21.7 (new) | 32.4 (-85%) | 40.6 (-84%) | 162.1 (-56%) |
| &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;gather | 11.7 (-6%) | 10.8 (new) | 16.3 (-85%) | 20.5 (-84%) | 86.4 (-57%) |
| &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;encode | 11.8 (-5%) | 11.0 (new) | 16.2 (-85%) | 20.2 (-85%) | 75.7 (-61%) |
| &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;transmit | 19.9 (+3%) | 18.5 (new) | 27.4 (-84%) | 32.8 (-84%) | 232.0 (-67%) |
| &nbsp;&nbsp;&nbsp;&nbsp;steam | 5.1 (-12%) | 0.1 (+0%) | 41.8 (-1%) | 56.0 (+4%) | 877.4 (-6%) |
| &nbsp;&nbsp;log_write | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 |

| Per frame | avg | p99 | max |
|---|---|---|---|
| filter_rejects | 0.00 | 0 | 0 |
| packets_in | 3.13 (-11%) | 27 | 32 |
| bytes_in | 74.50 (-11%) | 645 | 770 |
| packets_out | 3.18 (-9%) | 4 | 18 |
| bytes_out | 125.40 (-9%) | 200 | 694 |
| clients_sent | 3.18 (-9%) | 4 | 18 |
| entities_sent | 36.88 (-9%) | 56 | 232 |

