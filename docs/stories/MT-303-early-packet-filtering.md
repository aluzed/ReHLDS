# MT-303 — Early packet filtering on the network thread

**Epic:** E3 Async I/O · **Priority:** P1 · **Size:** M · **Depends on:** MT-302

## Context

Stability problems on public servers are often caused by floods of connectionless
packets (A2S queries, fake `getchallenge`/`connect`, reflected traffic) or by banned
addresses. Today every packet is popped on the main thread and goes through
`SV_FilterPacket`, the `PreprocessPacket` hookchain and
`SV_CheckConnectionLessRateLimits` (`engine/sv_main.cpp:3930-3955`). A flood therefore
consumes main-thread time directly. Recent upstream fixes (`dlfile` flood DoS,
`550f2d6`) show this is an active attack surface.

## User story

As a server operator, I want junk and banned traffic to be dropped before it reaches the
game loop, so that floods cannot lower the server tick rate.

## Scope

* On the network thread, before pushing to the ring:
  * drop packets shorter than 4 bytes / larger than the protocol maximum;
  * drop packets from IPs in the ban list (`SV_FilterPacket` data): the main thread
    publishes an immutable snapshot of the filter list (copy-on-write, atomic pointer
    swap, old snapshot freed with an epoch/RCU-style scheme) whenever `addip`/`removeip`
    run;
  * apply a per-IP connectionless rate limit (port of `ipratelimit`) on the thread;
* Keep, on the main thread, everything that is hookable by plugins
  (`PreprocessPacket`, `SV_CheckConnectionLessRateLimits` hookchains): if a plugin
  registers those hooks, the thread-level filter only does the cheap length/ban checks.
* Counters per drop reason exposed in `stats` / `sv_profile`.
* Under `REHLDS_FIXES` semantics: `SV_SendBan` replies are rate limited and sent from
  the network thread.

## Acceptance criteria

* [x] `flood` scenario: main thread `SV_ReadPackets` time and tick jitter stay within
      10 % of the no-flood numbers — `flood-a2s` with the thread: `read_packets` 15 µs
      avg (20 µs without flood), jitter 15.4 µs (17.3 µs).
* [x] `addip` / `removeip` take effect within one frame — checked: the first query after
      `addip` gets the ban reply, the first one after `removeip` is answered.
* [ ] With a ReAPI plugin hooking `PreprocessPacket`, the plugin still sees every packet
      it saw before (except malformed and banned ones) — implemented (rate limits stay on
      the main thread when either hook is registered), not tested with a plugin.

## Status

Done (`rehlds/rehlds/net_receive_thread.cpp`), cvar `net_thread_filter` (default 1).

* Always dropped, like the synchronous path: oversized packets and split packets.
* With `net_thread_filter 1`: packets shorter than 4 bytes; for connectionless packets,
  in the order of `SV_ReadPackets`, the IP rate limits (`max_queries_*`, own
  `CIPRateLimit` instance, skipped when a plugin hooks `PreprocessPacket` or
  `SV_CheckConnectionLessRateLimits`) and the IP filters (`sv_filterban` semantics).
  Banned addresses get the same "You have been banned" reply as `SV_SendBan`, sent from
  the thread, at most 20 per second.
* The main thread publishes the filters and limits at the start of `SV_ReadPackets` when
  they changed (`ipfilters_version` bumped by `addip` / `removeip`, cvars, hook
  registration): an immutable snapshot swapped under a mutex, taken by the thread when
  the version changes. Ban expiry is converted to a monotonic deadline.
* Counters per drop reason in `net_thread_stats`.
* `CIPRateLimit::CheckIP` takes the time and limits as parameters for the thread, and
  trims its tree at most once per second (`REHLDS_FIXES`): trimming on every packet
  made an A2S flood from more than 512 addresses take the stock engine down to 45 FPS.

Not done:

* Connected-client traffic is not filtered: a junk flood shaped like netchan packets
  still costs the main thread +15 % per frame (`flood-junk`). Follow-up: publish the
  client address list and drop packets from unknown addresses on the thread.
* `sv_logblocks` messages are not printed for packets the thread rate limits.

Results: [`../benchmarks/flood.md`](../benchmarks/flood.md).

## Risks

* Behaviour difference for plugins expecting to see banned traffic: document it, guard
  with cvar `net_thread_filter 0/1`.

## Out of scope

* Kernel-level filtering (eBPF/XDP, iptables) — can be documented as operator advice.
