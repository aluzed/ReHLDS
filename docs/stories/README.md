# Multithreading & frame timing roadmap

Goal: **improve server stability and server FPS (tick regularity) so the game feels
smoother for clients**, by removing waste from the main loop and moving work that does
not touch the game DLL off the main thread.

Read [`../server-frame-analysis.md`](../server-frame-analysis.md) first: it describes the
current frame, the hard constraints and what can be parallelised.

## Guiding principles

1. **Measure first.** No optimisation story is merged without before/after numbers from
   the benchmark harness (MT-002, MT-003).
2. **The main thread owns the game.** Game DLL, Metamod, AMXX and ReAPI hooks are only
   ever called from the main thread. Worker threads only touch engine-owned data.
3. **Byte-identical output.** For the same input, the server sends the same bytes with
   threading on or off (verified by MT-004).
4. **Everything is switchable.** Each threaded feature has a cvar / command-line switch
   and a serial fallback; default stays off until validated in production (MT-702).
5. **Refactor, then thread.** Every parallelisation is preceded by a single-threaded
   refactor that isolates the work, merged and validated on its own.

## Epics and stories

| ID | Title | Epic | Priority | Size | Depends on |
|---|---|---|---|---|---|
| [MT-001](MT-001-fork-and-ci.md) | Fork, branching model and CI on the fork | E0 Foundations | P0 | S | – |
| [MT-002](MT-002-frame-profiler.md) | Per-phase frame profiler | E0 Foundations | P0 | M | MT-001 |
| [MT-003](MT-003-load-test-harness.md) | Load-test harness and reference benchmarks | E0 Foundations | P0 | L | MT-002 |
| [MT-004](MT-004-regression-replay-and-tsan.md) | Byte-exact regression replay and ThreadSanitizer CI | E0 Foundations | P0 | M | MT-001 |
| [MT-101](MT-101-deadline-frame-scheduler.md) | Deadline-based frame scheduler | E1 Main loop timing | P0 | M | MT-002 |
| [MT-102](MT-102-wake-on-packet.md) | Wake the main loop on packet arrival | E1 Main loop timing | P1 | M | MT-101 |
| [MT-103](MT-103-timer-precision-fixes.md) | Timer precision and profiling bug fixes | E1 Main loop timing | P1 | S | MT-002 |
| [MT-104](MT-104-client-lookup-by-address.md) | O(1) client lookup by address | E1 Main loop timing | P2 | S | MT-002 |
| [MT-105](MT-105-batched-socket-syscalls.md) | Batched socket syscalls (recvmmsg/sendmmsg) | E1 Main loop timing | P2 | M | MT-302 |
| [MT-201](MT-201-thread-safety-audit.md) | Thread-safety audit and global state map | E2 Threading foundations | P0 | M | – |
| [MT-202](MT-202-job-system.md) | Thread primitives, worker pool and main-thread assertions | E2 Threading foundations | P0 | M | MT-201 |
| [MT-203](MT-203-thread-safe-engine-services.md) | Thread-safe bit writer, printing and deferred errors | E2 Threading foundations | P0 | L | MT-201, MT-202 |
| [MT-301](MT-301-async-log-writer.md) | Asynchronous log and console writer | E3 Async I/O | P1 | S | MT-202 |
| [MT-302](MT-302-network-receive-thread.md) | Cross-platform network receive thread | E3 Async I/O | P1 | L | MT-202, MT-101 |
| [MT-303](MT-303-early-packet-filtering.md) | Early packet filtering on the network thread | E3 Async I/O | P1 | M | MT-302 |
| [MT-304](MT-304-network-send-thread.md) | Network send offload | E3 Async I/O | P2 | M | MT-302 |
| [MT-305](MT-305-a2s-query-cache.md) | A2S query answers from a cached snapshot | E3 Async I/O | P2 | M | MT-303 |
| [MT-401](MT-401-split-send-client-messages.md) | Split `SV_SendClientMessages` into gather / encode / commit | E4 Parallel snapshots | P0 | L | MT-004 |
| [MT-402](MT-402-thread-safe-delta-encoding.md) | Thread-safe delta encoding | E4 Parallel snapshots | P0 | L | MT-203, MT-401 |
| [MT-403](MT-403-parallel-encode-and-transmit.md) | Parallel per-client encode and transmit | E4 Parallel snapshots | P0 | L | MT-402 |
| [MT-404](MT-404-plugin-compatibility-guard.md) | Plugin compatibility guard and serial fallback | E4 Parallel snapshots | P0 | M | MT-403 |
| [MT-501](MT-501-parallel-netchan-process.md) | Parallel netchan decoding of incoming packets | E5 Parallel input | P3 | L | MT-302, MT-203 |
| [MT-601](MT-601-offload-file-transfers.md) | Offload file transfer processing | E6 Background work | P3 | M | MT-202 |
| [MT-701](MT-701-configuration-and-docs.md) | Configuration, documentation and release notes | E7 Rollout | P1 | S | all shipped stories |
| [MT-702](MT-702-field-validation.md) | Field validation with real plugins and players | E7 Rollout | P0 | M | MT-701 |

Sizes: **S** ≤ 2 days, **M** ≤ 1 week, **L** ≤ 2 weeks. Priorities: P0 must have, P3 optional.

## Suggested order

```
Phase 1  (measure)        MT-001 → MT-002 → MT-003, MT-004, MT-201
Phase 2  (cheap wins)     MT-101 → MT-102, MT-103, MT-104          ← re-benchmark
Phase 3  (infrastructure) MT-202 → MT-203, MT-301
Phase 4  (big win)        MT-401 → MT-402 → MT-403 → MT-404       ← re-benchmark
Phase 5  (network)        MT-302 → MT-303 → MT-304, MT-305, MT-105
Phase 6  (optional)       MT-501, MT-601
Phase 7  (rollout)        MT-701 → MT-702
```

Epic 1 alone is expected to give most of the perceived smoothness (regular ticks,
lower input latency) at very low risk. Epic 4 is where CPU headroom for high player
counts / high tick rates comes from. There is a **decision gate after phase 2**: if
MT-002 shows `SV_SendClientMessages` encode time is not a significant share of the
frame on target servers, epic 4 is re-scoped.

## Branching

* `origin` is the fork `aluzed/ReHLDS`, `upstream` is `rehlds/ReHLDS`.
* `master` mirrors `upstream/master` and is never committed to directly.
* All work lives on `feature/multithreading`: one or more commits per story, with the
  story ID in the commit subject, e.g. `feat: add per-phase server frame profiler (MT-002)`.
* Sync with upstream:

  ```
  git fetch upstream
  git checkout master && git merge --ff-only upstream/master && git push origin master
  git checkout feature/multithreading && git rebase master
  git push --force-with-lease origin feature/multithreading
  ```

* Generic, low-risk improvements meant for upstream (see MT-702) are cherry-picked onto
  a separate branch from `master` when the PR is prepared.

## Explicit non-goals

* Running physics, `Think`/`Touch`, player movement (`PM_Move`) or any game DLL code in
  parallel. The game DLL ABI is single-threaded by contract.
* Changing the network protocol or anything the client can observe besides timing.
* Changing the ReAPI / Metamod ABI.
* Asynchronous map loading.

## Story template

Each story file contains: **Context** (with code references), **User story**,
**Scope**, **Technical approach**, **Acceptance criteria**, **Risks**, **Out of scope**.
