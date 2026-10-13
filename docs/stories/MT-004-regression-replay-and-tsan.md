# MT-004 — Byte-exact regression replay and ThreadSanitizer CI

**Epic:** E0 Foundations · **Priority:** P0 · **Size:** M · **Depends on:** MT-001

## Context

`rehlds/testsuite/` contains a recorder/player (`recorder.cpp`, `player.cpp`) that
records platform calls (sockets, time, ...) and replays them, comparing the engine's
behaviour. This is the ideal tool to prove that refactors and threading keep the
server output **byte-identical**.

Threading bugs (data races) are often invisible in tests; ThreadSanitizer finds them.

## User story

As a developer, I want CI to fail when outgoing packets change or when a data race is
detected, so that threading work cannot silently break the game.

## Scope

* Verify the testsuite still builds and runs; fix it if it has rotted.
* Record reference sessions (`cs32` short session, map change, client connect/disconnect,
  file download) and store them (Git LFS or CI artifact cache).
* CI job: build with the testsuite enabled, replay all sessions, fail on any diff in
  `sendto` payloads.
* With threading on, the order of `sendto` calls may legitimately differ between clients:
  compare **per destination address** sequences, not global order.
* Add a 64-bit Linux build target with `-fsanitize=thread` used only for TSan runs (TSan
  does not support i386), running the load harness (MT-003) for a short scenario.
* Unit test target (`rehlds/unittests/`) runs in CI.

## Acceptance criteria

* [ ] CI replays at least 3 recorded sessions on every push — the replay runs locally
      (`tools/bench/regression.sh`); not wired into CI, which is not enabled on the fork
      and would need HLDS and ReGameDLL installed by the job.
* [x] An intentional one-byte change in `SV_EmitPings` makes the job fail —
      `regression.sh` exits 1, 9 of 10 destinations differ.
* [ ] TSan job runs on every push to `feature/multithreading` and nightly — replaced by
      Helgrind on the real 32-bit server (`tools/bench/race_check.sh`), run locally.
* [x] Documented procedure to re-record sessions when an intended change alters output —
      sessions are recorded on demand with the reference build, nothing is stored.

## Status

The Windows testsuite records Win32 calls through `hlds.exe` and cannot run on Linux.
Done instead, on Linux:

* **Replay** (`rehlds/rehlds/net_replay.{h,cpp}`, under `REHLDS_FIXES`): `-netrecord`,
  `-netreplay`, `-netsends`. Inputs are frame times, server socket packets, console text,
  and the values read from the clock or the hardware (seeds, challenge salts, `pfnTime`,
  rate limiter time). A per-frame fingerprint (random generator, entity positions) locates
  the first divergence. Replays run as fast as possible: 46 000 frames in 3 s.
* **Comparison** per destination (`tools/bench/replay_compare.py`), and differential
  check of two builds (`tools/bench/regression.sh`).
* Sessions without bots replay byte-identically, including one recorded with the network
  thread, the scheduler and snapshot spreading. **Sessions with bots do not**: the bot AI
  is not deterministic across runs, cause not found.
* **Race check** (`tools/bench/race_check.sh`): Helgrind on the real server with the
  network thread, floods and filter changes; `-DREHLDS_HELGRIND=ON` annotates the
  lock-free code. No race in engine code.
* The load clients now hold the scoreboard key from time to time, so that pings are sent.
* Unit tests: unchanged, already run by the Linux CI job.

Results: [`../benchmarks/replay.md`](../benchmarks/replay.md).

## Risks

* A 64-bit build may not exist upstream: the TSan target may require compiling only the
  engine core plus a stub game DLL. If too costly, fall back to Helgrind/DRD on i386.

## Out of scope

* Fuzzing (could be a later story reusing the same harness).
