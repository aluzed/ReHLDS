# MT-103 — Timer precision and profiling bug fixes

**Epic:** E1 Main loop timing · **Priority:** P1 · **Size:** S · **Depends on:** MT-002

## Context

Several small timing defects add noise to the loop:

* `Host_FilterTime` (`engine/host.cpp:690`) compares a `float` threshold
  `1.0f / (fps + 1.0f)` with a `double` delta; `+1` biases the target rate.
* `Host_FilterTime` re-parses `-sys_ticrate` from the command line with `Q_atof` on
  every frame (`engine/host.cpp:685`).
* `host_profile` increments `timecount` twice per frame (`engine/host.cpp:982,985`),
  halving the reported average.
* Linux `Sys_FloatTime` (`engine/sys_dll.cpp`) computes
  `(now.tv_sec - start.tv_sec) + now.tv_nsec * 1e-9`, ignoring `start.tv_nsec`
  (constant offset, harmless) and uses a non-thread-safe lazy static init (matters
  once other threads call it).
* Windows `Sys_FloatTime` takes a critical section and truncates the performance
  counter to 32 bits with a shift, then uses heuristics to handle wrap-around.
* `Host_Speeds` meaningless on dedicated servers but still computed when enabled.

## User story

As a developer, I want the time base and the existing profiling commands to be correct,
so that scheduling decisions and measurements are trustworthy.

## Scope

* Cache the command-line ticrate once; compute the threshold in `double` without the
  `+1` bias when the new scheduler (MT-101) is active (keep legacy behaviour otherwise
  under `#ifndef REHLDS_FIXES` conventions).
* Fix `host_profile` double increment under `REHLDS_FIXES`.
* Linux `Sys_FloatTime`: initialise at startup (`Sys_InitFloatTime`), subtract full start
  timespec, make it safe to call from any thread.
* Windows `Sys_FloatTime`: use the full 64-bit `QueryPerformanceCounter` value with a
  start offset; remove the mutex (QPC is thread-safe).
* Add a `Sys_FloatTime` precision unit test (monotonic, sub-microsecond resolution).

## Acceptance criteria

* [ ] `host_profile 1` reports the same average as `sv_profile`.
* [ ] `Sys_FloatTime` is callable concurrently from multiple threads (TSan clean).
* [ ] Replay tests (MT-004) pass; behaviour without `REHLDS_FIXES` unchanged.

## Out of scope

* Changing the `double` time type exposed to the game DLL.
