# MT-201 — Thread-safety audit and global state map

**Epic:** E2 Threading foundations · **Priority:** P0 · **Size:** M · **Depends on:** –

## Context

The engine is a reverse-engineered single-threaded Quake/GoldSrc engine: state lives in
globals (`g_psv`, `g_psvs`, `host_client`, `net_message`, `net_from`, `msg_readcount`,
`bfwrite`/`bfread` in `engine/common.cpp:370`, `g_svmove`, `gGlobalVariables`...) and in
function-local `static` buffers. Before moving any code to another thread we need a map
of what each candidate code path reads and writes.

## User story

As a developer, I want a written map of the global state touched by each candidate code
path, so that threading stories are scoped on facts rather than guesses.

## Scope

Produce `docs/thread-safety-map.md` covering at least these paths:

| Path | Entry points |
|---|---|
| Delta encoding | `SV_CreatePacketEntities_internal`, `DELTA_WriteDelta`, `DELTA_CheckDelta`, delta JIT (`engine/delta_jit.cpp`), `conditionalencode` callbacks |
| Events / pings | `SV_EmitEvents_internal`, `SV_EmitPings` |
| Client data | `SV_WriteClientdataToMessage` (encoding part) |
| Netchan out | `Netchan_Transmit`, `Netchan_CreateFragments`, `NET_SendPacket`, `NET_SendLong`, `NET_LagPacket` |
| Netchan in | `Netchan_Process`, fragment reassembly, bzip2 decompression |
| Messages | `MSG_*`, `SZ_*`, `Sys_Error`, `Con_Printf`, `Con_DPrintf`, `Log_Printf` |
| Memory | `Mem_*` (malloc wrappers), `Z_*`, `Hunk_*`, `Cache_*` |
| Networking | `NET_GetPacket`, `SV_FilterPacket`, `ipratelimit`, `SV_CheckConnectionLessRateLimits` |
| Time | `Sys_FloatTime`, `realtime`, `host_frametime` |
| ReHLDS extras | hookchains, flight recorder (`REHLDS_FLIGHT_REC`), security module, `CGameClient` wrappers |

For each function: globals read, globals written, statics, calls into the game DLL /
plugins, allocator used, error paths (`Sys_Error` uses `longjmp`!). Classify each path:

* **Safe** – pure on its arguments;
* **Safe with context** – becomes safe if a global is turned into a per-thread context;
* **Main-thread only** – calls the game DLL / plugins or mutates shared server state.

Also list existing `setjmp`/`longjmp` usage (`host_abortserver`, `host_enddemo`) since a
`longjmp` from a worker thread is undefined behaviour.

## Acceptance criteria

* [ ] `docs/thread-safety-map.md` reviewed and merged.
* [ ] Every global referenced by the delta/netchan-out paths is classified.
* [ ] Follow-up items are filed as subtasks of MT-203 / MT-402.

## Out of scope

* Code changes.
