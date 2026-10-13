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

* [x] With log files on an artificially slow device (`tools/bench/slowlog`, 20 ms per
      write), the frame is unaffected with `sv_log_async 1`.
* [x] Log content and ordering identical to synchronous mode.
* [x] No lost lines on `quit`. `Sys_Error` waits up to 2 s for the queue (not tested).

## Out of scope

* Log rotation or format changes.

## Status

Experiment on the `exp/mt301-async-log` branch (2026-10-13), not merged.

* `rehlds/rehlds/async_log_writer.{h,cpp}`: one writer thread (native threads, the
  engine is built without exceptions), a 4 MB byte queue under a lock, records in
  order across files. `sv_log_async` (default 0), `sv_log_async_overflow block|drop`
  (default `block`), `sv_log_async_stats`.
* The log file is still opened by the main thread through the file system (name,
  creation, `logsdir`); the writer appends to it through its own handle. The mode is
  decided when the file is opened, so changing `sv_log_async` takes effect at the next
  log file. `qconsole.log` keeps its open, write, close per line, now on the writer.
* The writer flushes its files whenever the queue empties: the log file on disk is
  more up to date than with the synchronous path (stdio buffer written out every 8 KB),
  and a crash loses less.
* Flushed on `Log_Close` (map change, `log off`, quit), at shutdown and in `Sys_Error`
  (2 s at most, the disk may be stuck).
* Out of the scope after reading the launcher: console output. `Con_Printf` reaches the
  terminal through the launcher (`Launcher_ConsolePrintf`, `CTextConsole::Print`),
  which also redraws the line being typed; calling it from another thread would race
  with the launcher's own input handling, code the engine does not own. Left
  synchronous.

Measurements (benchmark, `bots20`, `tools/bench/slowlog` at 20 ms per write, 30 s, two
runs each):

| | sync | async |
|---|---|---|
| Log file only (`mp_logdetail 3`): frames > 10 ms | 0, 0 | 0, 0 |
| Log file only: frame max | 3.0, 3.1 ms | 2.2, 3.6 ms |
| With `-condebug`: frames > 10 ms | 7, 5 | 0, 0 |
| With `-condebug`: frame max | 41.6, 21.2 ms | 2.4, 3.2 ms |
| With `-condebug`: server FPS | 989, 992 | 1000, 1000 |

The log file alone shows nothing on the benchmark: about 4 KB of log lines per minute,
the 8 KB stdio buffer is written out about every two minutes, and a 3-minute window
(`bots20-logs`) caught none of those writes in the frame. On a slow disk the
synchronous path stalls one frame for the length of that write each time.

Replay (`replay` scenario, `+log on +mp_logdetail 3`) with `sv_log_async 0` and `1`:
packets byte-identical, log files identical once the timestamps and the file name in
the first line are removed (207 lines, 13 281 bytes each).
