# MT-305 — A2S query answers from a cached snapshot

**Epic:** E3 Async I/O · **Priority:** P2 · **Size:** M · **Depends on:** MT-303

## Context

Server browsers, monitoring sites and attackers send `A2S_INFO`, `A2S_PLAYER` and
`A2S_RULES` queries. Each is handled on the main thread by `SV_ConnectionlessPacket`
and Steam (`Steam_HandleIncomingPacket`), and the answer is rebuilt every time (player
list, all server cvars for rules). Query floods are a classic cause of lag on public
servers.

## User story

As a server operator, I want server queries to be answered without involving the game
loop, so that being listed or attacked does not degrade gameplay.

## Scope

* The main thread publishes, at most every N ms (e.g. 1 s) or when something changes
  (player join/leave, map change, rules cvar change), a snapshot of the encoded
  `A2S_INFO`, `A2S_PLAYER`, `A2S_RULES` responses.
* The network thread answers these queries from the snapshot, including the challenge
  mechanism (challenge validation must not require main-thread state; generate
  challenges from a keyed hash of the address, rotated periodically).
* Response rate limiting per IP and global bandwidth cap.
* Plugins hooking connectionless packets (`SV_ConnectionlessPacket`-related hooks or
  `PreprocessPacket`): if present, fall back to main-thread handling.
* Steam master server heartbeat stays on the main thread.

## Acceptance criteria

* [ ] Answers byte-identical to the current implementation for the same state.
* [ ] 50k queries/s flood: tick jitter within 10 % of no-flood baseline.
* [ ] Player count/names visible in server browsers update within 1 s.

## Risks

* Steam GameServer API also answers queries when it owns the socket
  (`Steam_HandleIncomingPacket`); confirm which path is active in each mode and keep the
  Steam behaviour intact.

## Out of scope

* Changing the query protocol.
