# MT-202 — Thread primitives, worker pool and main-thread assertions

**Epic:** E2 Threading foundations · **Priority:** P0 · **Size:** M · **Depends on:** MT-201

## Context

The only thread in the engine today is the Windows-only `-netthread`
(`engine/net_ws.cpp:1083`), written with raw Win32 calls. The project is C++14
(`rehlds/CMakeLists.txt:26`) and targets Linux i386 and Windows, so `std::thread`,
`std::atomic`, `std::mutex`, `std::condition_variable` are available.

Parallel snapshot encoding (MT-403) runs hundreds to thousands of times per second in
short bursts (a few hundred microseconds): a naive "create threads / wake with a
condition variable" design would cost more than it saves.

## User story

As a developer, I want a small, well-tested threading toolkit tuned for per-frame
fan-out, so that threaded stories reuse one consistent implementation.

## Scope

* `rehlds/rehlds/threading/` (new):
  * `WorkerPool`: fixed number of threads created at startup, named
    (`pthread_setname_np` / `SetThreadDescription`), `ParallelFor(count, fn)` that
    blocks until all items are done, with the **main thread participating** in the work;
  * wake strategy: short adaptive spin (`_mm_pause`) then futex / condition variable;
    workers park when the server is idle;
  * work distribution: atomic index counter (clients are few, no work stealing needed);
  * `SpscRing<T>` and `MpscQueue<T>` lock-free bounded queues with preallocated storage
    (used by MT-301, MT-302, MT-304);
  * `IsMainThread()` and `ASSERT_MAIN_THREAD()` (active in debug and in a
    `REHLDS_THREAD_CHECKS` build), placed at the entry of every game DLL call wrapper
    and of hookchain `callChain`.
* Configuration:
  * `-sys_threads N` command line / `sys_threads` cvar (read at startup), default 0 =
    disabled; `auto` = physical cores − 1, capped at 8;
  * optional `-sys_affinity` to pin the main thread and workers.
* Unit tests in `rehlds/unittests/`: `ParallelFor` correctness, queues under contention,
  shutdown while idle and while busy.

## Acceptance criteria

* [ ] `ParallelFor` over 32 trivial items completes in < 20 µs on the reference machine
      when workers are hot.
* [ ] Idle server with `sys_threads 4` uses no measurable extra CPU after the spin
      window.
* [ ] Clean shutdown and restart (map change, `quit`, `restart`) without leaks or hangs.
* [ ] TSan job clean on unit tests.
* [ ] `ASSERT_MAIN_THREAD()` fires in a test that calls a game DLL wrapper from a worker.

## Risks

* Oversubscription on shared hosting (many HLDS instances per machine): `auto` must be
  conservative and the documentation must explain it.

## Out of scope

* Generic task graph / dependencies.
