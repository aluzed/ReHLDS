# MT-302 — Cross-platform network receive thread

**Epic:** E3 Async I/O · **Priority:** P1 · **Size:** L · **Depends on:** MT-202, MT-101

## Context

`-netthread` exists only on Windows (`engine/net_ws.cpp:1083`): it allocates one
`net_messages_t` per packet (`NET_AllocMsg`), appends to a linked list by walking it to
the tail (O(n)), and sleeps 1 ms when the sockets are empty. Linux, which runs most
servers, has nothing: packets are read on the main thread in `NET_GetPacket`
(`engine/net_ws.cpp:1213`).

A receive thread lets the socket buffers be drained continuously (no drops during long
frames, e.g. map entity spawns or heavy plugin work), gives each packet an accurate
arrival timestamp and is the prerequisite for early filtering (MT-303).

## User story

As a server operator, I want packets to be received continuously on a dedicated thread,
so that bursts and long frames do not cause packet loss or extra latency.

## Scope

* New implementation used on Linux and Windows when `-netthread` (or `net_thread 1`) is set:
  * blocking wait on all server sockets (`epoll` / `WSAPoll`), drain with `recvfrom`
    (later `recvmmsg`, MT-105);
  * push into an `SpscRing` of preallocated slots (`MAX_UDP_PACKET` bytes + `netadr_t` +
    arrival time); on overflow, drop and count;
  * `NET_GetPacket(NS_SERVER)` pops from the ring and fills `net_message`/`net_from`
    exactly like today, so the rest of the engine is unchanged.
* Keep the existing ordering guarantees per socket.
* `NET_LagPacket` (fakelag/fakeloss) keeps operating on the main thread on popped
  packets.
* Split packets (`NET_GetLong`) reassembly stays on the main thread in this story.
* Wake-up integration with the scheduler (MT-101/MT-102): the network thread signals the
  main thread through an event when it wants it (optional; by default the main thread
  only drains at the tick).
* Testsuite: when recording/replaying, force the synchronous path (deterministic).
* Remove the old Windows-only implementation once parity is proven.

## Acceptance criteria

* [ ] Same replay results with net thread on/off (MT-004) — not available yet; the
      functional checks below give the same results on both paths.
* [x] Under the `flood-a2s` scenario, legitimate clients see no increase in packet
      loss and the main frame time does not include receive syscalls — main thread frame
      times and CPU equal to the no-flood run, no queue or socket drop, 99.8 snapshots/s.
* [x] Counters show queue overflows and drops — `net_thread_stats`, and
      `net_thread_drops` / `net_queue_wait_us` in `sv_profile`.
* [x] Clean start/stop on `map`, `restart`, `quit` — checked; the thread is stopped
      before `NET_Config` closes the sockets and started after it opens them.

## Status

Done on Linux; the Windows code is written but has not been built (no CI yet).

* `rehlds/rehlds/net_receive_thread.{h,cpp}`: `CNetReceiveThread`, started with
  `-netthread` (`-netthread_queue N`, default 1024 packets, 64–16384). Waits with `poll`
  (`select` on Windows, 50 ms timeout to see the stop request), drains the server socket
  (IP, plus IPX on Windows) into an `SpscRing` of preallocated slots, stamps the arrival
  time. Native threads (`pthread_create` / `CreateThread`): the engine is built without
  exceptions, so `std::thread` cannot report a failed creation.
* `rehlds/rehlds/threading/spsc_ring.h`: bounded lock-free single-producer
  single-consumer queue, filled in place (from MT-202), with unit tests.
* `NET_GetPacket(NS_SERVER)` pops from the queue and goes through `NET_LagPacket` like
  `NET_QueuePacket`; loopback packets keep priority; other sockets stay synchronous.
* Under `REHLDS_FIXES`, `-netthread` no longer enables the old implementation (Windows
  only, a linked list walked to its tail, `TerminateThread` on stop; on Linux it stopped
  the server from reading packets at all). The old code stays under `#ifndef REHLDS_FIXES`.
* Disabled when the Windows testsuite records or replays.
* The release engine now links `pthread` and needs `GLIBCXX_3.4.11` (from `std::mutex`)
  and `GLIBC_2.12` (`pthread_setname_np`); HLDS ships `GLIBCXX_3.4.16`.
  `std::make_shared` is avoided: it needs `GLIBCXX_3.4.26`.

Not done:

* Wake-up of the main thread on packet arrival (MT-102): packets are processed on the
  next tick, as before.
* `recvmmsg` batching (MT-105): the thread spends about 4.6 µs per packet, 23 % of a core
  at 50 000 packets/s.
* TSan run: TSan does not support i386; it needs a 64-bit unit test build (MT-004).

Results: [`../benchmarks/flood.md`](../benchmarks/flood.md).

## Risks

* Socket recreation (`NET_Config`, `NET_OpenIP`) while the thread is waiting: the thread
  must be stopped and restarted around socket changes.

## Out of scope

* Filtering in the thread (MT-303), sending (MT-304).
