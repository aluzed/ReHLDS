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
| [MT-106](MT-106-snapshot-send-spreading.md) | Spread client snapshots over frames | E1 Main loop timing | P0 | S | MT-002, MT-003 |
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

## Status and decision gate (2026-10-12)

Done: MT-002, MT-003, MT-101, MT-103, MT-106, MT-302, MT-303, MT-004 (Linux, not in CI),
MT-401 to MT-404, MT-701 (review pending); MT-102 as an option (no latency gain, see the story); MT-202 and MT-203 in part; MT-001 except the
Windows CI run. MT-702 in progress.

Measured on a full server (`clients32`, see
[`../benchmarks/clients32.md`](../benchmarks/clients32.md)): with the scheduler (MT-101)
and snapshot spreading (MT-106), the server runs exact 1000 Hz ticks, every client gets
exactly 100 snapshots/s with 0.1 ms jitter, and the frame p99 dropped from 587 µs to
259 µs. The remaining per-frame work is small for a 1000 µs budget: `send_messages`
≈ 45 µs and `read_packets` ≈ 23 µs on average.

Consequences for the plan:

* **Epic 4 (parallel snapshot encoding) is re-scoped.** The bursts that motivated it
  (all 32 snapshots in one frame, ≈ 480 µs) were a scheduling artefact fixed by MT-106.
  Parallelising the remaining ≈ 45 µs per frame would save a few tens of microseconds at
  the cost of the riskiest refactor of the plan (thread-safe delta encoding, plugin
  guards). It stays in the backlog for servers with many more slots or entities, to be
  reconsidered with numbers from such a setup.
* **MT-104** (client lookup by address): ≈ 160 ns per packet, 0.05 % of a core at
  3200 packets/s. Deferred.
* **MT-102** (wake on packet): with the scheduler, packets are processed on the tick
  anyway; waking earlier only moves the receive syscalls. Folded into MT-302.
* **MT-301** (async logs): log writes measured at 2 µs max with `log on`; only protects
  against pathological disks. Kept, lower priority.
* Remaining spikes come from the game DLL (bots' AI up to several ms) and from rare long
  frames; they are outside the engine's reach. Stability work is better spent on the
  network thread with early filtering (MT-302/MT-303), which protects the tick against
  packet floods, and on Windows parity (CI).

### Update: network thread (MT-302, MT-303)

Done on Linux (Windows code written, not built yet). Under a 50 000 queries/s A2S flood
from 4096 addresses on the full server ([`../benchmarks/flood.md`](../benchmarks/flood.md)):

* the stock engine fell to **45 FPS and 15 snapshots/s per player**, because of the IP
  rate limiter walking its whole tree on every packet; fixed (`REHLDS_FIXES`);
* with the fix only, the main thread keeps 1000 FPS but spends twice as much time per
  frame on the flood;
* with `-netthread`, the main thread's frame times and CPU are those of the no-flood
  run; the thread drops the flood for about 23 % of a core.

The thread also drops netchan packets from addresses that are not connected clients:
under a junk flood of 50 000 packets/s, the main thread keeps its no-flood numbers.

### Update: regression replay and race checks (MT-004)

Done on Linux ([`../benchmarks/replay.md`](../benchmarks/replay.md)): sessions without bots
replay byte-identically, a one-byte change in the output is caught, and Helgrind finds
no race in engine code with the network thread under load. Not in CI yet. Sessions with
bots are not deterministic (game DLL bot AI).

Next: Windows CI build (all the Windows code paths of MT-002, MT-101, MT-302).

### Update: documentation (MT-701)

Operator guide: [`../performance-tuning.md`](../performance-tuning.md); every option is in
`README.md` and `CHANGELOG.md`.


### Update: parallel snapshot encoding (epic 4)

Reopened with a heavier benchmark, `clients32-ents` ([`../benchmarks/clients32-ents.md`](../benchmarks/clients32-ents.md)):
32 load clients that play and 400 moving entities. It showed encoding as the largest
part of the send (126 µs per frame, 404 µs p99), and MT-401 to MT-404 were done: with
`-sys_threads 2 +sv_parallel_send 1`, worker threads encode the snapshots, with
byte-identical replays. The first version encoded during the gather and crashed a server
through Metamod (MT-404); with Counter-Strike the encoding now starts once the frame's
snapshots are gathered.

That benchmark was wrong: the load client mishandled the server's split datagrams and
acknowledged stale frames, so the server sent much bigger snapshots than to real
clients (found with MT-102). With the load client fixed, the send takes 49 µs per frame
(285 µs p99) on that benchmark and the encoding threads save 2 µs on average, 50 µs at
p99, for 7 % of a core. The benchmark does not reproduce a real busy server (many
players, grenades and smokes): the decision on parallel encoding waits for the field
validation (MT-702), with it on and off under the same load.
What remains above the tick on busy servers is game DLL time (player movement,
`AddToFullPack`).

## Explicit non-goals

* Running physics, `Think`/`Touch`, player movement (`PM_Move`) or any game DLL code in
  parallel. The game DLL ABI is single-threaded by contract.
* Changing the network protocol or anything the client can observe besides timing.
* Changing the ReAPI / Metamod ABI.
* Asynchronous map loading.

## Story template

Each story file contains: **Context** (with code references), **User story**,
**Scope**, **Technical approach**, **Acceptance criteria**, **Risks**, **Out of scope**.
