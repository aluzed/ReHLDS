# MT-403 — Parallel per-client encode and transmit

**Epic:** E4 Parallel snapshots · **Priority:** P0 · **Size:** L · **Depends on:** MT-402

## Context

After MT-401 and MT-402, the encode phase works on per-client jobs and only reads shared
server state. This story runs it on the worker pool (MT-202).

## User story

As a server operator running high player counts or high tick rates, I want snapshot
encoding to use several CPU cores, so that the main thread finishes each tick sooner and
ticks stay regular under load.

## Scope

* In `SV_SendClientMessages`: decide + gather on the main thread for all clients, then
  `WorkerPool::ParallelFor(jobs, EncodeClientSnapshot)`, then commit on the main thread.
* `EncodeClientSnapshot` per client:
  * builds the datagram (`svc_time`, client data, packet entities, events, pings,
    appended unreliable `client->datagram`);
  * calls `Netchan_Transmit` for that client: verify (MT-201 map) that it only touches
    `cl->netchan`, the client's fragment buffers and the send path (MT-304 queue or
    thread-safe `sendto`); allocations use `Mem_*` (malloc, thread-safe);
  * any diagnostic goes through deferred console (MT-203).
* Read-only guarantee during the parallel section: no code on the main thread mutates
  `g_psv.edicts`, baselines, `g_psvs.clients[*]` of other clients, or cvars while
  workers run (the main thread only participates in `ParallelFor`).
* Cvars: `sv_parallel_send 0/1` (default 0), uses `sys_threads` workers.
* Load balancing: sort jobs by previous-frame cost (entities count) descending.
* Profiling (MT-002): per-worker busy time, parallel section wall time, imbalance.

## Acceptance criteria

* [x] Byte-identical output vs serial (MT-004), in the same global order.
* [x] `clients32-ents` scenario: `SV_SendClientMessages` main-thread time −40 % on average
      (251 → 152 µs), −37 % at p99, with 2 workers.
* [x] No regression with `sys_threads 0` / `sv_parallel_send 0`.
* [ ] TSan clean on a 5-minute `cs32` run: TSan cannot run (32-bit engine), Helgrind instead.
* [x] Toggling `sv_parallel_send` at runtime is safe (read once per frame).

## Risks

* Gather (game DLL `AddToFullPack`) stays serial and may dominate: Amdahl's law limits
  the gain — decision gate in `README.md`.
* Cache effects of moving client frames between cores: measure, consider pinning a
  client range to a worker.

## Out of scope

* Parallel gather (impossible without game DLL cooperation).

## Status

Done, with a different split than planned: **transmit stays on the main thread**, after
the encode, in client order; only the encode runs on the workers. The output is then
byte-identical in global order too, and `Netchan_Transmit`, the network replay and the
profiler counters stay single-threaded. Transmit is ≈ 15 µs per frame.

* `rehlds/threading/worker_pool.{h,cpp}` (MT-202): fixed threads, batches whose items
  are published one by one while the main thread prepares the next, main thread
  participating, spin then condition variable.
* `rehlds/parallel_send.{h,cpp}`: `-sys_threads N|auto` (≤ 8), `sv_parallel_send`,
  `sys_threads_spin_us` (default 0), `sv_parallel_send_stats`; profiler counter
  `send_worker_items`.
* Pipelined: the workers are woken by the first gathered snapshot of the frame and
  encode each snapshot while the main thread gathers the next one, which hides their
  wake-up; there is no "gather all, then encode in parallel" step.
* Results: [`../benchmarks/clients32-ents.md`](../benchmarks/clients32-ents.md).
* Helgrind (`tools/bench/race_check.sh`, now with 2 encoding threads and playing
  clients): 0 report in engine code once the pool and the encoder lock are annotated.
  One of three runs crashed under Valgrind before any client connected, with a null
  access in the game DLL's `StartFrame`, before any snapshot was encoded; not reproduced
  since, cause unknown. Never seen outside Valgrind.
