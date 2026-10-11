# MT-301 — Asynchronous log and console writer

**Epic:** E3 Async I/O · **Priority:** P1 · **Size:** S · **Depends on:** MT-202

## Context

`Log_Printf` (`engine/sv_log.cpp`) formats the line, then writes it synchronously with
`FS_FPrintf` to the log file (and sends it over UDP with `logaddress`). Console output
also goes synchronously to stdout / the launcher. AMXX and game DLLs log a lot (kills,
chat, rounds): on slow disks, network file systems or a blocked terminal/pipe
(`screen`, `tmux`, systemd journal backpressure) this stalls the frame and causes
visible hitches.

## User story

As a server operator, I want logging never to block the game loop, so that disk or
terminal latency cannot cause lag spikes.

## Scope

* A dedicated I/O thread consuming an MPSC queue of preformatted lines (bounded memory,
  e.g. 4 MB; when full, either drop with a counter or block — cvar
  `sv_log_async_overflow drop|block`, default `block` to never lose logs silently).
* `Log_Printf` file writes and console stdout writes go through the queue when
  `sv_log_async 1`.
* `logaddress` UDP sends stay on the main thread (or go through MT-304 later).
* Flush semantics: `log off`, map change, `quit`, crash handler and `Sys_Error` flush the
  queue synchronously before closing files.
* Timestamps keep being computed at call time (not at write time).

## Acceptance criteria

* [ ] With a log file on an artificially slow device (e.g. `dm-delay`), `cs32` frame
      p99 is unaffected with `sv_log_async 1`.
* [ ] Log content and ordering identical to synchronous mode.
* [ ] No lost lines on `quit` and on `Sys_Error`.

## Out of scope

* Log rotation or format changes.
