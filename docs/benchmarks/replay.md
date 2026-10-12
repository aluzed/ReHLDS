# Byte-exact replay and race checks (MT-004)

How to prove that an engine change does not alter what the server sends, and that the
threads added by the roadmap do not race. Both run on Linux with the benchmark harness
(`tools/bench/`). The Windows testsuite (`rehlds/testsuite/`) records Win32 calls through
`hlds.exe` and cannot run on Linux.

## Replay

The engine records the inputs of a session and can replay them (`rehlds/rehlds/net_replay.cpp`):

| Option | Effect |
|---|---|
| `-netrecord <file>` | records the inputs |
| `-netreplay <file>` | replays them instead of reading the clock, the server socket and the console, as fast as possible, then quits |
| `-netsends <file>` | in both modes, writes every packet the server socket sends |

Inputs, in the order the engine consumed them:

* the `realtime` of each accepted frame;
* the packets returned by `NET_GetPacket(NS_SERVER)`, and its "no packet" answers;
* console text;
* the values that come from the system clock or the hardware: the `srand` and engine
  random seeds, the challenge salts (`rdtsc`), `PF_Time` (`pfnTime`), `time()` in the IP
  rate limiter, `Sys_FloatTime` for `sv_usercmd_custom_random_seed`.

Each frame also records a fingerprint of the simulation: the engine's random generator
state and the positions of the entities. A replay reports the first frame where its
fingerprint differs, which points at a missing input.

The comparison (`tools/bench/replay_compare.py`) is per destination, in order: with
several threads, the order between destinations may change, but the order towards one
destination must not.

### Results

Same machine as the other reports.

| Session | Replay with the same build | Replay with `ping + 1` in `SV_EmitPings` |
|---|---|---|
| `clients8`: 8 load clients, 46 000 frames | **identical** (9 378 packets), 3 s | — |
| `replay`: 8 load clients and 2 HLTV proxies | **identical** (23 556 packets), twice | **different**: 9 of 10 destinations |
| `replay` recorded with `-netthread +sys_scheduler 1 +sv_rehlds_snapshot_spread 1` | **identical** (25 423 packets) | — |
| `bots20-hltv8`: 20 bots and 8 HLTV proxies | **different** from frame ≈ 21 000 | — |

* A replay of a session without bots reproduces the recorded session exactly: the same
  bytes as the live server, to every destination.
* The intentional one-byte change is caught. The pings are only sent to players
  holding the scoreboard key and to HLTV proxies, so the load clients now hold it for a
  second every 6–10 s; without it, the change was not exercised at all.
* **Sessions with bots are not deterministic.** Two replays of the same recording differ
  from each other too, also with ASLR off and with `bot_chatter off`. The first
  difference is an isolated `BotVoice` message, sent when a bot dies, respawns or
  leaves, while the random generator and the entity positions are still identical. The
  cause is in the game DLL's bot AI and was not found. Use sessions without bots.
* A recording is tied to the engine's input format, the command line, the HLDS files and
  the game DLL; builds without `-netreplay` cannot replay it.

### Use

```sh
# Is my change output-neutral?
git stash; tools/bench/build.sh before; git stash pop
tools/bench/build.sh after
tools/bench/regression.sh before after          # exit code 0: byte-identical

# Or by hand
BENCH_RECORD=1 tools/bench/run.sh replay before  # any scenario, any BENCH_SERVER_ARGS
tools/bench/replay.sh .bench/results/<that run> after
```

`regression.sh` first replays the session with the reference build: if that is not
identical, the session is not deterministic and nothing can be concluded.

When a change is meant to alter the output, record a new session with the new build;
there are no stored reference sessions.

## Race checks

ThreadSanitizer needs a 64-bit build, which the engine does not have (i386 only).
`tools/bench/race_check.sh` runs the real 32-bit server under Valgrind's Helgrind instead:
network thread on, 4 load clients, an A2S flood from 64 addresses, a junk flood from 16,
`addip` and `removeip` while the thread filters.

Helgrind does not understand C++11 atomics. A build with `-DREHLDS_HELGRIND=ON`
(`BENCH_HELGRIND=ON tools/bench/build.sh helgrind`) annotates the lock-free code
(`threading/race_annotations.h`): the queue's release/acquire publication, and the
statistics and flags that are read without synchronisation on purpose. Release builds
contain nothing of it.

| Build | Reports in engine code | Reports in other libraries |
|---|---|---|
| Without annotations | 52 (queue indices, slots, statistics) | 848 |
| With annotations | **0** | 684 |

* The run without annotations shows that Helgrind does see both threads' accesses: every
  report is on the queue or the statistics, as expected.
* One report was a shared `static` in `CUtlRBTree` (the sentinel node of every tree of the
  same type, used by both rate limiters), initialised on first use under a compiler
  guard that Helgrind does not understand. Not a race; the thread now initialises it
  before it starts, which keeps the check clean.
* The reports in other libraries are inside the closed-source Steam client, racing with
  its own threads.
* During the annotated run: 5 998 packets dropped as banned, 600 ban replies, 6 003
  unknown addresses dropped, no queue overflow: all the thread's paths were exercised.

```sh
BENCH_HELGRIND=ON tools/bench/build.sh helgrind
tools/bench/race_check.sh helgrind 90    # several minutes; exit code 0: no race in engine code
```
