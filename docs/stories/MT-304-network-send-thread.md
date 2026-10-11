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

* [ ] `SV_SendClientMessages` transmit time (MT-002) reduced; total CPU not increased by
      more than the syscall time moved.
* [ ] Per-destination output identical (MT-004).
* [ ] Works together with MT-403 (multiple producers).

## Out of scope

* Zero-copy sends.
