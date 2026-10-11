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

* [ ] CI replays at least 3 recorded sessions on every PR.
* [ ] An intentional one-byte change in `SV_EmitPings` makes the job fail.
* [ ] TSan job runs on PRs labelled `threading` and on `mt/main` nightly.
* [ ] Documented procedure to re-record sessions when an intended change alters output.

## Risks

* A 64-bit build may not exist upstream: the TSan target may require compiling only the
  engine core plus a stub game DLL. If too costly, fall back to Helgrind/DRD on i386.

## Out of scope

* Fuzzing (could be a later story reusing the same harness).
