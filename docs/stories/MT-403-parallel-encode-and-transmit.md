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

* [ ] Per-destination byte-identical output vs serial (MT-004).
* [ ] `cs32-ents` scenario: `SV_SendClientMessages` wall time reduced by ≥ 40 % with 4
      workers (to be revised after MT-002 baseline).
* [ ] No regression with `sys_threads 0` / `sv_parallel_send 0`.
* [ ] TSan clean on a 5-minute `cs32` run.
* [ ] Toggling `sv_parallel_send` at runtime is safe.

## Risks

* Gather (game DLL `AddToFullPack`) stays serial and may dominate: Amdahl's law limits
  the gain — decision gate in `README.md`.
* Cache effects of moving client frames between cores: measure, consider pinning a
  client range to a worker.

## Out of scope

* Parallel gather (impossible without game DLL cooperation).
