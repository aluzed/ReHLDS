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

* [ ] `host_profile 1` reports the same average as `sv_profile` — the double count is
      fixed, but `host_profile` prints whole milliseconds, so the two cannot be compared on
      frames of a few tens of microseconds.
* [ ] `Sys_FloatTime` is callable concurrently from multiple threads (TSan clean) — the
      Linux initialization is now thread-safe; concurrent test deferred to MT-202, which
      links pthread, and TSan to MT-004.
* [x] Unit tests pass (41, including the 2 new `Sys` tests); benchmark unchanged
      (`bots20-hltv8` with scheduler: 999.8 fps).

## Status

Done, under `REHLDS_FIXES` where behaviour changes:

* `host_profile`: frame no longer counted twice (`engine/host.cpp`).
* Linux `Sys_FloatTime`: start time is a function-local static (thread-safe one-time
  initialization) and the full start timespec is subtracted, via `Sys_MonotonicNs`.
* `Host_GetTicrate` (added in MT-101) parses `-sys_ticrate` once instead of every frame.
* `Host_FilterTime` threshold: unchanged in legacy mode; with the scheduler (MT-101) the
  threshold is not used at all, which removes the `+1` bias and the float comparison.
* New `rehlds/unittests/sys_tests.cpp`: monotonicity, sub-microsecond resolution, and
  agreement with `Sys_MonotonicNs` over a 20 ms sleep.

Not done:

* **Windows `Sys_FloatTime`** (32-bit truncated counter, wrap heuristics, critical
  section): left as is until the Windows CI build is available, because it goes through
  the platform layer recorded by the testsuite and cannot be built or run locally.
* `Host_Speeds` is already only computed when `host_speeds` is set (`REHLDS_FIXES`).

## Out of scope

* Changing the `double` time type exposed to the game DLL.
