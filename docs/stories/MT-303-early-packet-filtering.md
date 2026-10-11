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

* [ ] `flood` scenario: main thread `SV_ReadPackets` time and tick jitter stay within
      10 % of the no-flood `cs32` numbers.
* [ ] `addip` / `removeip` take effect within one frame.
* [ ] With a ReAPI plugin hooking `PreprocessPacket`, the plugin still sees every packet
      it saw before (except malformed ones).

## Risks

* Behaviour difference for plugins expecting to see banned traffic: document it, guard
  with cvar `net_thread_filter 0/1`.

## Out of scope

* Kernel-level filtering (eBPF/XDP, iptables) — can be documented as operator advice.
