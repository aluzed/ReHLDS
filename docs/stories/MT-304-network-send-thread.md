# MT-304 — Network send offload

**Epic:** E3 Async I/O · **Priority:** P2 · **Size:** M · **Depends on:** MT-302

## Context

`Netchan_Transmit` builds the packet then calls `NET_SendPacket` → `NET_SendLong` →
`sendto` (`engine/net_ws.cpp:1349,1367`) inline. With 32 clients each tick issues ≥ 32
`sendto` syscalls on the main thread, plus split packets for large updates.

## User story

As a server operator, I want the main thread to hand finished packets over instead of
sending them itself, so that kernel send time is removed from the frame.

## Scope

* `NET_SendPacket` (server socket only) copies the finished datagram into an
  `MpscQueue`/ring of preallocated buffers; a send thread (or the network thread from
  MT-302, alternating receive/send) performs `sendto` (later `sendmmsg`, MT-105).
* Ordering per destination preserved (single consumer).
* Fakelag outgoing (if any) and `NET_SendLong` splitting stay before the queue so the
  wire format is unchanged.
* Backpressure: if the queue is full, send synchronously (never drop).
* Error reporting (`WSAEWOULDBLOCK`, `EMSGSIZE`...) logged through the deferred console
  (MT-203).
* Disabled automatically when the testsuite is recording/replaying.

## Acceptance criteria

* [x] `SV_SendClientMessages` transmit time (MT-002) reduced; total CPU not increased by
      more than the syscall time moved.
* [x] Per-destination output identical (MT-004).
* [x] Works together with MT-403 (multiple producers).

## Out of scope

* Zero-copy sends.

## Status

Experiment on branch `exp/mt304-send-thread`, not merged. `-netsend` (off by default),
`-netsend_queue`, `net_send_stats`. Code: `rehlds/rehlds/net_send_thread.{h,cpp}`, the
`NET_SendDatagram` hook in `NET_SendLong` (`engine/net_ws.cpp`).

* A dedicated thread rather than the receive thread: that one sleeps in `poll` on the
  socket, waking it for each send would cost what the send costs.
* `NET_SendLong` still splits on the main thread: each datagram, split part or not, is
  copied into a slot of a single-producer single-consumer ring (`SpscRing`, 1024 slots
  of 1400 bytes by default). The thread sends the queued datagrams in order, with one
  `sendmmsg` call for all of them on Linux (`sendto` each on Windows), so the per
  destination order is the order of the main thread's calls.
* Only the main thread queues. MT-403 has a single producer anyway: the workers encode,
  the main thread transmits in client order. Other threads (the receive thread's ban
  replies) and the client socket keep sending themselves.
* Wake-up: the thread spins 5 µs once the queue is empty, then sets a flag and sleeps
  on an eventfd (Event on Windows); the main thread writes to the eventfd only when the
  flag is set (Dekker-style fences on both sides). A tick's datagrams are sent a few
  microseconds apart: about one wake-up per frame. A 30 µs spin cost 3.5 % of a core
  for the same main thread gain; 5 µs costs 0.8 %.
* Full queue: the main thread waits for room (never drops nor reorders); after 100 ms
  it sends itself, counted in `net_send_stats` (never seen).
* Errors are counted by the thread and printed by the main thread at the next
  `SV_ReadPackets`, with the same rules as `NET_SendPacket` (would block, connection
  refused and reset stay silent).
* Off during a replay and under the Windows testsuite. A recording still records the
  sends in `NET_SendPacket`, before the queue.
* `SpscRing` gained `Peek(i)` and `Pop(n)` for the batches (unit tests).

Results (`tools/bench`, Ryzen 9 3950X, `clients32`: 32 load clients, 100 commands/s and
100 snapshots/s each):

| Recommended settings (`-netthread -sys_threads 2`, scheduler, spread, parallel send), 3 runs each | without | `-netsend` |
|---|---|---|
| Transmit phase, average per frame | 20.5–21.3 µs | 2.9–3.1 µs |
| Frame, average | 67.6–75.1 µs | 49.9–52.5 µs |
| Main thread CPU | 7.2–8.0 % of a core | 5.3–5.7 % |
| Process CPU | 13.7–15.3 % | 14.9–15.6 % |
| Tick jitter | 12.8–20.2 µs | 12.9–18.7 µs (30 µs spin runs) |

* 3.2 datagrams per `sendmmsg` call, one wake-up per frame (161 929 datagrams in
  51 076 calls over a run).
* Without parallel encoding (`+sys_scheduler 1` only, 2 runs each), where encoding and
  transmitting alternate: transmit p95 on the frames that send 171 → 14 µs, frame p95
  456 → 299 µs, p99 612 → 412 µs, main thread CPU 7.4–7.7 → 6.0–6.1 %.
* Replay (`regression.sh` against `82a7991`, thread off): identical, 24 005 packets.
* Helgrind (`race_check.sh` with `BENCH_SERVER_ARGS=-netsend`, the send thread sending
  4722 datagrams): no report in engine code.
* Not done: Windows not built; not tried on the field validation server.

