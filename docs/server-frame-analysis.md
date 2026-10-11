# Server frame analysis (baseline before multithreading)

This document describes how a ReHLDS dedicated server frame runs today, where the
time goes and which parts can or cannot leave the main thread. It is the reference
for every story in [`stories/`](stories/README.md).

All line numbers refer to commit `550f2d6`.

## 1. The outer loop

```
launcher (dedicated/src/sys_ded.cpp:156 RunEngine)
  while (!done)
    sys->Sleep(1)                         // usleep(1000) by default, see -pingboost
    ProcessConsoleInput()
    engineAPI->RunFrame()                 // engine/sys_dll2.cpp:697
      CEngine::Frame()                    // engine/sys_engine.cpp:98
        Host_Frame(dt)                    // engine/host.cpp:951
          _Host_Frame(dt)                 // engine/host.cpp:865
            Host_FilterTime(dt)           // engine/host.cpp:663 -> returns FALSE if "too early"
            Cbuf_Execute()
            SV_Frame()                    // engine/sv_main.cpp:8301
            SV_CheckForRcon()
            ... client stubs (no-op in SWDS) ...
            Host_Speeds / Host_UpdateStats / Rehlds_Security_Frame
```

### Frame pacing problems

* **Polling, not scheduling.** The launcher sleeps a fixed 1 ms, then the engine asks
  `Host_FilterTime` whether enough time has elapsed. If not, the iteration is wasted
  and the server sleeps again. The real tick interval is therefore
  `ceil(target / (1 ms + sleep overshoot)) * (1 ms + sleep overshoot)`, which causes
  beat patterns and jitter (e.g. `sys_ticrate 1000` never reaches 1000 fps).
* **Sleep precision depends on `-pingboost`** (`dedicated/src/sys_linux.cpp:150`):
  `usleep` (default), `setitimer`+`pause` (1, signal based, fragile), `select` (2),
  or `NET_Sleep_Timeout` (3). The latter has integer maths flagged by the original
  authors as "entirely bad code" (`engine/net_ws.cpp:1005`).
* **No wake-up on packet arrival** in modes 0/1/2: an incoming `usercmd` waits up to
  one sleep quantum before being read, adding latency.
* **Non-accumulating threshold.** `Host_FilterTime` compares `1.0f / (fps + 1.0f)`
  (float) against `realtime - oldrealtime` (double) and resets `oldrealtime` to the
  actual time, so overshoot is never compensated: the average tick rate drifts below
  the target.
* `host_profile` increments `timecount` twice per frame (`engine/host.cpp:982,985`),
  so its reported average is wrong.

## 2. `SV_Frame_Internal` (engine/sv_main.cpp:8306)

| Step | Function | Calls into game DLL? | Notes |
|---|---|---|---|
| 1 | `SV_CheckCmdTimes` | no | cheap |
| 2 | `SV_ReadPackets` (`sv_main.cpp:3924`) | **yes** (`SV_ExecuteClientMessage` → `ClientCommand`, `CmdStart`, `PM_Move`, `PlayerPreThink`, ...) | for each packet: linear scan over `maxclients` with `NET_CompareAdr`, then `Netchan_Process` |
| 3 | `SV_Physics` (`sv_phys.cpp:1472`) | **yes** (`StartFrame`, `Think`, `Touch`, `Blocked`) | must stay serial |
| 4 | `SV_RequestMissingResourcesFromClients` | no | |
| 5 | `SV_CheckTimeouts` | indirectly (`SV_DropClient` → `ClientDisconnect`) | |
| 6 | `SV_SendClientMessages` (`sv_main.cpp:5256`) | **yes**, per client | usually the most expensive step with many players, see below |
| 7 | `SV_CheckMapDifferences`, `SV_GatherStatistics` | no | |
| 8 | `Steam_RunFrame` | Steam API | |

### `SV_SendClientMessages` per client

```
SV_SendClientDatagram(cl)                         sv_main.cpp:5043
  SV_WriteClientdataToMessage                     -> gamedll pfnUpdateClientData, pfnGetWeaponData
  SV_WriteEntitiesToClient                        sv_main.cpp:4901
    pfnSetupVisibility                            -> gamedll
    for each player / edict: pfnAddToFullPack     -> gamedll (often hooked by Metamod/AMXX)
    REHLDS_FIXES post-processing                  pure engine
    SV_EmitPacketEntities -> SV_CreatePacketEntities (delta encoding)   pure engine*, ReAPI hook
    SV_EmitEvents                                 pure engine, ReAPI hook
    SV_EmitPings                                  pure engine, ReAPI hook
  Netchan_Transmit                                pure engine (+ sendto)
```

\* Delta encoding calls `conditionalencode` callbacks registered by the game DLL
(`DELTA_AddEncoder`, `engine/delta.cpp:1071`), and those callbacks mutate the shared
`delta_t` field flags. It also writes through the global bit writer `bfwrite`
(`engine/common.cpp:371`).

This splits naturally into:

1. a **gather** part that must run on the main thread (game DLL calls), and
2. an **encode + transmit** part that is independent per client once the global
   state it touches is made per-thread.

## 3. Existing threading

* `-netthread` (`engine/net_ws.cpp:1083`) exists **only on Windows**: a receive thread
  pushing packets into an unbounded linked list (`NET_AllocMsg` per packet, O(n) tail
  append), polled by the main thread. No Linux implementation.
* `Sys_FloatTime` on Windows is protected by a critical section.
* Everything else assumes a single thread.

## 4. Hard constraints

* **The game DLL, Metamod, AMX Mod X and ReAPI plugins are not thread-safe.** Every
  call into `gEntityInterface`, `gNewDLLFunctions`, delta encoder callbacks that we
  cannot prove pure, and every ReAPI hookchain must stay on the main thread.
* **Wire compatibility.** Clients are the original GoldSrc client: packet contents
  must stay byte-identical for a given input (verifiable with the testsuite
  recorder/player in `rehlds/testsuite/`).
* **i386 + Windows builds**, C++14 (`rehlds/CMakeLists.txt:26`). `std::thread`,
  `std::atomic`, `std::mutex` and `std::condition_variable` are available.
* **ReAPI ABI.** Public interfaces in `rehlds/public/rehlds/` must not break.

## 5. What can realistically be parallelised

| Work | Thread | Expected gain |
|---|---|---|
| Frame pacing / sleep | main | large jitter reduction, tick rate actually reaches target |
| Socket receive + early filtering | network thread | lower latency, flood resistance |
| Socket send (`sendto`/`sendmmsg`) | network thread | removes syscalls from the frame |
| Log / console file writes | I/O thread | removes disk stalls from the frame |
| Delta encoding + netchan build per client | worker pool | proportional to players × entities |
| A2S queries | network thread | stability under query floods |
| Physics, think, pmove, gather (`AddToFullPack`) | **main, forever** | none, by design |

Profiling (story MT-002) must confirm the actual split before investing in epic 4.
