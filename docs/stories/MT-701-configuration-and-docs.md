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

* [ ] Every new option documented, with its story ID.
* [ ] Tuning guide reviewed by someone who runs public servers.
