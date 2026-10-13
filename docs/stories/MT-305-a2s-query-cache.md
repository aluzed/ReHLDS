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

* [x] Answers byte-identical to the current implementation for the same state.
* [x] 50k queries/s flood: tick jitter within 10 % of no-flood baseline.
* [x] Player count/names visible in server browsers update within 1 s.

## Risks

* Steam GameServer API also answers queries when it owns the socket
  (`Steam_HandleIncomingPacket`); confirm which path is active in each mode and keep the
  Steam behaviour intact.

## Out of scope

* Changing the query protocol.

## Status

Experiment on branch `exp/mt305-a2s-cache`, not merged. `sv_a2s_cache` (default 0), with
`-netthread` and `net_thread_filter 1`. Code: `rehlds/rehlds/a2s_cache.{h,cpp}`, the
network thread (`net_receive_thread.cpp`), hooks in `engine/sv_steam3.cpp`.

What the engine does today, found while doing it:

* The engine's own `SVC_Info`, `SVC_PlayerInfo`, `SVC_RuleInfo` are dead code:
  `SV_ConnectionlessPacket` returns early for A2S queries. The Steam library answers
  them (`HandleIncomingPacket`), queues the answers, and `CSteam3Server::RunFrame` sends
  them every 10 ms; long answers (rules, 2545 bytes here) are split by `NET_SendLong`.
* Steam answers the loopback addresses (127.0.0.0/8) directly and every other address,
  private ones included, with a challenge first. A player's browser waits twice for the
  main thread and the 10 ms send: 12.1 ms on the same machine.
* Each Steam call is slow: `HandleIncomingPacket` 20 to 45 µs, `GetNextOutgoingPacket`
  about 20 µs even when the queue is empty (RunFrame polls it every 10 ms: about 2 ms of
  main thread time per second, not changed here).

How it works:

* The main thread asks Steam the queries itself from 127.0.0.1:1 (loopback: no
  challenge; port 1: no client uses it) and takes the answer from Steam's queue, sending
  the packets queued before it. Only the query types clients asked for since the last
  capture, again when the answer is 1 s old or when what Steam reports changed (cvar
  through `Steam_SetCVar`, player connect or disconnect, map change), one per frame.
  An answer not asked for is dropped when it changes, and not used after 2 s.
* The answers are published to the network thread like the filter configuration
  (immutable copy, swapped under the mutex).
* The network thread hands out challenges (SipHash-2-4 of the address with a random key,
  30 s periods, the current and the previous one accepted) and answers the queries that
  carry a valid one, and the loopback ones directly, like Steam. Long answers are split
  like `NET_SendLong` (own sequence numbers, from 0x40000000).
* With the cache on, every A2S query the thread sees is counted in its own rate limiter
  (same `max_queries_*` values): queries no longer use up the budget of connection
  attempts. Queries without a captured answer yet go to the main thread (Steam).
* Queries that get a challenge are counted in a separate limiter (`A2SChallengeLimit`):
  per address `max_queries_sec` over `max_queries_window` like `CIPRateLimit`, and for
  all addresses `sv_a2s_challenges_sec` per second (default 0: no cap). Addresses are
  tracked in a fixed 65 536-entry table indexed by a keyed hash (1 MB, allocated when
  the thread starts): a flood from random addresses neither grows it (`CIPRateLimit`'s
  tree only trims entries idle for 120 s) nor evicts an address over its limit, which
  keeps refusing the address it shares its entry with until its window ends. Answers
  keep the previous limiter (`max_queries_*`), only for loopback queries and queries
  with a valid challenge, which spoofed addresses cannot have.
* A plugin hooking `PreprocessPacket` or the connectionless rate limits turns it off
  (not tested with such a plugin).

Results (`tools/bench`, Ryzen 9 3950X):

* **Same answers**: captured `A2S_INFO` byte-identical to Steam's; `A2S_RULES` identical
  but for `mp_timeleft` (queries 4 s apart); `A2S_PLAYER` same names and scores, the
  connection times differ by the time between the queries.
* **Latency**, query with its challenge from the container's address: 12.1 ms median
  with Steam, 0.09 ms with the cache; from loopback (no challenge) 1.4 ms vs 0.06 ms.
* **Main thread cost**: one capture 55 to 90 µs on average (max 176 µs), at most one per
  frame, at most about 3 per second, none when nobody queries. With Steam, each query
  from the internet costs two Steam calls (40 to 70 µs) plus the sending.
* **Updates**: map change visible at once; a bot joining shows 1.4 to 1.7 s after the
  command, as fast as Steam's own count (updated once per second by `RunFrame`).
* **Flood** (50 000 A2S_INFO/s from 4096 loopback addresses, 5 bots): a player's
  `getchallenge` is answered 5/5 with the cache, 0/5 without (the flood used up the
  global connectionless budget). The server stays invisible to browsers in both cases:
  loopback queries are answered without a challenge, so the flood uses up the answers'
  global budget. The network thread's queue to the main thread stays at depth 1 (196
  without). Loopback sources cannot come from the network: this is the worst case, not
  the usual one.
* **Spoofed flood** (same, from 4096 addresses in 10.99.0.0/16 sent with
  `IP_TRANSPARENT`, `flood.py --source-prefix`): without the cache, browsers 0/5 per
  query type and `getchallenge` 0/5; with it, browsers 5/5 per query type (0.3 to 0.4 ms
  median) and `getchallenge` 5/5. 803 021 challenges sent in 40 s, 1 140 844 refused by
  the per address limit (each address sent about 12 queries/s); about 3 % of the flood
  dropped by the kernel before the thread read it (none without the cache, which sends
  nothing).
* **Tick under flood** (`flood-a2s`, 32 clients, 3 runs each): jitter 14.8 / 17.9 /
  18.5 µs without, 22.1 / 15.4 / 19.6 µs with, main thread CPU 8.1–8.5 % in both, Steam
  phase 5.2–5.7 µs per frame in both: no difference beyond run-to-run noise.
* **Replay** (`regression.sh` against `82a7991`, cache off): identical, 24 061 packets.
* Unit tests: query classification, answer recognition, challenges, split like
  `NET_SendLong`.

Not done, to decide:

* `sv_a2s_challenges_sec` defaults to no cap, so that browsers get through any spoofed
  flood: the outgoing traffic is at most the flood's, in fewer bytes. A cap is for
  hosts that pay for or limit their upload.
* Windows code written (random key from `CryptGenRandom`) but not built.
* Not tested with a plugin hooking `PreprocessPacket`, nor on the field validation server.
