# MT-306 — Steam calls on demand

**Epic:** E3 Async I/O · **Priority:** P2 · **Size:** S · **Depends on:** MT-002

## Context

`CSteam3Server::RunFrame` runs on the main thread at the end of every frame. Besides
Steam's callbacks (every 100 ms), it does two things on a timer:

* every 10 ms, it reads the packets Steam wants sent on the game socket
  (`GetNextOutgoingPacket`) until the queue is empty;
* once a second, it sends the server details and, for every player, their name and
  score (`BUpdateUserData`).

On the field server (19 bots, `sys_ticrate 1000`), the frame profiler showed the Steam
phase at 4.6 ms per second, 10 % of the main thread's frame work: about 90 calls a
second of 39 µs each (median), nearly always finding an empty queue, and a 120 to 660 µs
spike once a second. The median frame is 13 µs, so each poll was a regular spike three
times as long as a frame.

## User story

As a server operator, I want the engine to talk to Steam only when there is something to
say, so that the frame does not pay for empty Steam calls.

## Scope

* `sv_steam_on_demand` (default 1; 0 restores the original timers).
* Steam only queues a packet for the game socket in answer to one it was handed
  (`HandleIncomingPacket`: a server browser query, a master server challenge) or on its
  own timers (master server heartbeats). The queue is read on the frame after a packet
  was handed to Steam (also by the A2S cache, MT-305) or callbacks ran, and every 100 ms
  otherwise.
* Player data: `BUpdateUserData` only for the players whose name or score changed since
  the last call, per client slot (user id and Steam ID checked); the slot is forgotten
  on disconnect, and all of them when the server reconnects to Steam.
* Off in the Windows testsuite, which replays Steam calls in their original order.

## Acceptance criteria

* [x] Steam phase per second divided by at least 2 on the field server.
* [x] Queries handed to Steam still answered (A2S cache off).
* [x] The server stays registered with the master server.
* [ ] A player's score updates in the server browser.

## Risks

* A Steam packet queued by something other than an incoming packet or a callback waits
  up to 100 ms instead of 10 ms. Heartbeats are sent every few minutes; 100 ms more is
  not noticed.
* Steam forgetting a player's data without the engine knowing (the reconnection case is
  handled): the score would stay stale until it changes again.

## Status

Done (`exp/steam-poll`, merged into `feature/multithreading`). 60 s profiles on the
field server with 19 bots, same build and settings:

| | `sv_steam_on_demand 0` | `sv_steam_on_demand 1` |
|---|---|---|
| Steam phase | 4.61 ms/s (10.0 % of `sv_frame`) | 1.05 ms/s (2.5 %) |
| Frames with a 15–60 µs Steam call | 89 per second | 6 per second |
| Longest Steam phase | 659 µs | 306 µs |

A query handed to Steam (A2S cache off) is answered in 0.4 ms, instead of up to 10 ms.
The once a second spike left (about 200 to 300 µs) is the server details update
(`SendUpdatedServerDetails`, `SetGameDescription`), not the player data.

### Server details sent when they change

Measured on the field server (temporary timers, 19 bots): every Steam call costs about
20 µs, and the once a second update made seven of them whatever changed: server details
(max players, bots, name, map, tags) about 120 µs, `SetPasswordProtected` 22 µs,
`SetGameDescription` 22 µs, `WasRestartRequested` 20 µs, plus 23 µs per player whose data
changed. With `sv_steam_on_demand 1`, the setters are only called when their value
changed since the last call; everything is sent again on map change, on activation and
when the server reconnects to Steam. `WasRestartRequested` is still asked every second.

Before (`feature/multithreading` `c5e7fbf`) and after, 60 s profiles on the field server,
19 bots, two runs each, alternated:

| | Before | After |
|---|---|---|
| Steam phase | 1.00, 0.99 ms/s | 0.86, 0.90 ms/s |
| Frames with a Steam phase of 100 µs or more | 1.03, 1.05 per second | 0.13, 0.25 per second |
| Steam phase p99.99 | 265, 243 µs | 111, 144 µs |

A hostname change reaches the server browser answers within a second, a map change
updates the map name and the bot count, and the server stays registered with the
master server.
