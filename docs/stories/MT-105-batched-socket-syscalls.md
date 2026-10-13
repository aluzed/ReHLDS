# MT-105 — Batched socket syscalls (recvmmsg/sendmmsg)

**Epic:** E1 Main loop timing · **Priority:** P2 · **Size:** M · **Depends on:** MT-302

## Context

Each received packet costs one `recvfrom` and each sent packet one `sendto`
(`engine/net_ws.cpp:890`, `engine/net_ws.cpp:1367`), all going through
`CRehldsPlatformHolder` (so that the testsuite can record them). At 32 clients and
`sys_ticrate 1000` this is tens of thousands of syscalls per second, made more
expensive by kernel mitigations (Spectre/Meltdown).

## User story

As a server operator, I want the server to spend less CPU in the kernel, so that more
CPU is available for simulation and more servers fit on one machine.

## Scope

* Linux: receive with `recvmmsg` (batch of e.g. 64) on the network thread (MT-302),
  send with `sendmmsg` at the end of the frame / on the send thread (MT-304).
* Extend the platform abstraction (`IReHLDSPlatform`) with batch functions and a default
  implementation looping over `recvfrom`/`sendto`, so that the testsuite recorder and
  Windows keep working.
* Keep split packets (`NET_SendLong`) ordered per destination.
* Optional: `SO_RCVBUF`/`SO_SNDBUF` sizing cvars to avoid drops during bursts.

## Acceptance criteria

* [ ] Syscall count per frame reduced (measure with `perf trace -s` / `strace -c`).
* [ ] No ordering change per destination (MT-004 per-address comparison).
* [ ] Fallback path used on Windows and in the testsuite, with identical results.

## Out of scope

* io_uring (could be evaluated later if batching is not enough).

## Status

Sending is batched since MT-304 (`sendmmsg` on the send thread). Receiving with
`recvmmsg` was tried on `exp/mt105-recvmmsg` (`-netbatch`, Linux), not merged: the
network thread keeps up with the socket, so the batches stay small.

Per-thread CPU over 30 s (`tools/bench/lib/run_server.sh` now records it in `cpu.txt`),
in % of a core:

| | Network thread, `recvfrom` | Network thread, `-netbatch` | Packets per `recvmmsg` |
|---|---|---|---|
| `flood-junk` (50 000 packets/s) | 22.6 % | 20.3 % | 1.7 |
| `clients32` | 2.4 % | 2.3 % | 1.0 |

The main thread is the same either way (5.5 %): since MT-302 the receive system calls
are on the network thread, and the tick does not see them. The thread's cost per
packet is mostly classifying it and waking up, not the call.
