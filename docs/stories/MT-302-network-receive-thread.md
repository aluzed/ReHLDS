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

* [ ] Same replay results with net thread on/off (MT-004).
* [ ] Under the `flood` scenario (MT-003), legitimate clients see no increase in packet
      loss and the main frame time does not include receive syscalls.
* [ ] `stats`/new counters show ring overflows and drops.
* [ ] Clean start/stop on `map`, `restart`, `quit`; sockets re-created on `ip`/`port`
      change without races.

## Risks

* Socket recreation (`NET_Config`, `NET_OpenIP`) while the thread is waiting: the thread
  must be stopped and restarted around socket changes.

## Out of scope

* Filtering in the thread (MT-303), sending (MT-304).
