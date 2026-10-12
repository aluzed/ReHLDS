# MT-701 — Configuration, documentation and release notes

**Epic:** E7 Rollout · **Priority:** P1 · **Size:** S · **Depends on:** all shipped stories

## User story

As a server operator, I want clear documentation of the new options and their trade-offs,
so that I can enable them safely on my server.

## Scope

* Document every new cvar / command-line switch in `README.md` (existing "Configuring"
  section) with defaults, e.g.:

  | Option | Story | Default |
  |---|---|---|
  | `sys_scheduler`, `sys_spin_us` | MT-101 | 0 |
  | `-sys_threads N` / `sys_threads` | MT-202 | 0 |
  | `sv_profile*` | MT-002 | 0 |
  | `sv_log_async`, `sv_log_async_overflow` | MT-301 | 0 / block |
  | `-netthread`, `net_thread_filter` | MT-302/303 | 0 |
  | `sv_parallel_send`, `sv_parallel_encoders` | MT-403/402 | 0 |
  | `sv_parallel_recv` | MT-501 | 0 |

* Operator guide `docs/performance-tuning.md`: choosing `sys_ticrate`, number of threads
  per instance when running several servers on one machine, CPU pinning, kernel advice
  (timer slack, `performance` governor, IRQ affinity, socket buffers).
* CHANGELOG entries.
* Updated benchmark reports in `docs/benchmarks/`.

## Acceptance criteria

* [x] Every new option documented, with its story ID.
* [ ] Tuning guide reviewed by someone who runs public servers.

## Status

Done for the shipped stories (MT-002, MT-004, MT-101, MT-103, MT-106, MT-302, MT-303):

* `README.md`: the cvars in "Configuring", a new "Command-line parameters" list, the
  profiler and `net_thread_stats` in "Commands". The options of stories not implemented
  (`sys_threads`, `sv_log_async`, `sv_parallel_*`) are not documented.
* [`docs/performance-tuning.md`](../performance-tuning.md): recommended settings with
  their story IDs, choosing `sys_ticrate`, the scheduler, snapshot spreading, the network
  thread and its counters, machine advice (governor, one core per instance, socket
  buffers), game DLL spikes (bot navigation files), measuring with the profiler.
* `CHANGELOG.md`: "Unreleased" section.

Review by a public server operator pending.
