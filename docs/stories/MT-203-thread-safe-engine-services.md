# MT-203 — Thread-safe bit writer, printing and deferred errors

**Epic:** E2 Threading foundations · **Priority:** P0 · **Size:** L · **Depends on:** MT-201, MT-202

## Context

Basic engine services used by the encoding path rely on global state:

* the bit writer `bfwrite` and reader `bfread` are globals (`engine/common.cpp:370-371`),
  used by `MSG_StartBitWriting`, `MSG_WriteBits`, `MSG_EndBitWriting` and therefore by all
  delta encoding;
* `msg_readcount`, `msg_badread`, `net_message`, `net_from` are globals;
* `SZ_GetSpace` (`engine/common.cpp`) calls `Sys_Error` on misuse and `Con_Printf` on
  overflow; `Sys_Error` ends with a `longjmp`/abort, which is undefined from a worker;
* `Con_Printf`/`Con_DPrintf`/`Log_Printf` write to the console, files and redirect
  buffers (`SV_BeginRedirect`) without locking.

## User story

As a developer, I want the low-level message and printing services to be usable from a
worker thread, so that encoding code can run in parallel without rewriting it.

## Scope

* **Bit writer context**: move `bfwrite`/`bfread` into a `MsgContext` struct; keep the
  global API (`MSG_WriteBits(...)`) working by resolving the current context through a
  `thread_local` pointer (or explicit parameter in the hot path if `thread_local` cost
  is measurable on i386 — benchmark both). The main thread uses the existing global
  instance, so behaviour is unchanged when threading is off.
* Keep `ALIGN16` requirement for the SSE path of `MSG_WriteBits`.
* **Deferred diagnostics**: on worker threads, `Con_Printf`/`Con_DPrintf` push to an
  MPSC queue flushed by the main thread at the end of the parallel section, preserving
  per-client order.
* **Deferred fatal errors**: on worker threads, `Sys_Error` records the error and marks
  the job as failed; the main thread raises the real `Sys_Error` after `ParallelFor`
  returns. Overflow handling (`SIZEBUF_OVERFLOWED`) is already flag based and stays as is.
* Audit for function-local `static` buffers on these paths (e.g. `va()`-style helpers)
  and make them `thread_local` or remove them.
* Flight recorder (`REHLDS_FLIGHT_REC`): make `FR_*` calls main-thread only or guarded.

## Acceptance criteria

* [ ] Unit test encoding the same deltas concurrently on N threads produces identical
      buffers to serial encoding.
* [ ] No measurable slowdown on the main-thread-only path (MT-002, `cs32`).
* [ ] A forced `Sys_Error` in a worker produces the same message and exit path as on
      the main thread.
* [ ] TSan clean on the unit tests.

## Risks

* `thread_local` access on i386 shared objects goes through `__tls_get_addr` (slow with
  the general dynamic model); use `-ftls-model=initial-exec` or explicit context
  passing.

## Out of scope

* Making `Z_*`/`Hunk_*` allocators thread-safe (workers must not allocate from them).
