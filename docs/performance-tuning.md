# Performance tuning

How to get a steady tick and protect it from packet floods with the options added on the
`feature/multithreading` branch. Every option is off by default: without them, the engine
behaves like upstream ReHLDS. The measurements behind each recommendation are in
[`benchmarks/`](benchmarks/); the design is in [`stories/`](stories/README.md).

## Recommended settings

```sh
./hlds_run -game cstrike -netthread +sys_ticrate 1000 +sys_scheduler 1 \
    +sv_rehlds_snapshot_spread 1 +map de_dust2
```

| Option | Story | Default | Recommended | Effect |
|---|---|---|---|---|
| `sys_scheduler 1` | MT-101 | 0 | 1 | Exact `sys_ticrate`, tick jitter divided by 2.5 to 5 |
| `sys_scheduler_slice` | MT-101 | 100 | 100 | Time (ms) between returns to the launcher |
| `sys_spin_us` | MT-101 | 0 | 0 | Busy-wait before each tick, dedicated cores only |
| `sv_rehlds_snapshot_spread 1` | MT-106 | 0 | 1 | Snapshots spread over the frames, exact update rate |
| `-netthread` | MT-302 | off | on | Network thread, the tick no longer sees floods |
| `-netthread_queue` | MT-302 | 1024 | 1024 | Queue between the network thread and the game loop |
| `net_thread_filter` | MT-303 | 1 | 1 | Early filtering on the network thread |
| `-sys_threads`, `sv_parallel_send 1` | MT-403 | 0, 0 | busy servers: 2, 1 | Snapshots encoded on worker threads |
| `sys_threads_spin_us` | MT-403 | 0 | 0 | Workers spin between frames |
| `sv_profile*` | MT-002 | off | on demand | Per-phase frame profiler |
| `-netrecord`, `-netreplay`, `-netsends` | MT-004 | off | – | Session replay, for developers |

`sys_scheduler` and `sv_rehlds_snapshot_spread` can be changed at any time from the
console or `server.cfg`; `-netthread` is a command-line parameter only.

These three options were validated on a public server with players (Metamod-R, AMX Mod X,
ReAPI, ReGameDLL_CS): see [`benchmarks/field-validation.md`](benchmarks/field-validation.md).

## Tick rate

### `sys_ticrate` and the scheduler

The stock main loop sleeps about 1 ms between frames and rejects the frames that come too
early. At `sys_ticrate 1000`, the sleep alone is longer than a tick: the server runs at
about 935 fps with irregular gaps, and at `sys_ticrate 100` it wakes up ten times per
frame for nothing.

`sys_scheduler 1` replaces this with one deadline every `1 / sys_ticrate` seconds, slept
precisely (`clock_nanosleep` on Linux, high-resolution waitable timer on Windows 10 1803
and later). Measured ([`benchmarks/mt101-scheduler.md`](benchmarks/mt101-scheduler.md)):

| `sys_ticrate 1000` | Default | `sys_scheduler 1` |
|---|---|---|
| Idle server FPS | 935.7 | 1000.0 |
| Idle tick jitter | 14.4 µs | 4.6 µs |
| 20 bots, 8 HLTV: tick p99 | 1386 µs | 1028 µs |

The CPU cost is that of the frames themselves: 10 % more frames at 1000 Hz, no busy-wait.

Choosing `sys_ticrate`:

* The tick rate is how often the server reads commands and simulates; clients receive
  snapshots at their `cl_updaterate` (capped by `sv_maxupdaterate`), which cannot exceed
  the tick rate.
* `1000` gives the lowest input latency and is cheap with the scheduler: an idle frame
  costs about 11 µs, a full 32-player frame about 75 µs on average.
* With many instances per machine, a lower rate (e.g. `500`) runs half as many frames;
  it is still well above the update rates clients use (`cl_updaterate` 100 or less).
  Not measured with players.
* Changing `sys_ticrate` at runtime applies at the next slice.

`sys_scheduler_slice` (1–1000 ms) is how long the engine keeps control before returning
to the launcher for console input. Each return costs one late tick, caught up at the
next one; 100 ms keeps 98 % of ticks within ±10 µs. There is no reason to change it.

`sys_spin_us` (0–2000) wakes up that many microseconds before each deadline and
busy-waits the rest. It removes the kernel's wake-up latency (a few microseconds on a
quiet Linux machine) at the cost of that much CPU per tick: 100 µs at 1000 Hz is 10 % of
a core. Only for a server pinned to a core that nothing else uses.

### Snapshot spreading

By default, after any long frame (map change, connections, a slow plugin), every
client's next snapshot falls in the same frame, and they stay aligned forever: on a full
32-player server at 1000 Hz, one frame in ten builds all 32 snapshots (≈ 480 µs) and the
others none. With the scheduler alone, the perfectly regular ticks also lose snapshots
(94/s instead of 100 at `cl_updaterate 100`).

`sv_rehlds_snapshot_spread 1` gives each client a fixed phase in its update interval
([`benchmarks/clients32.md`](benchmarks/clients32.md), 32 clients, 1000 Hz):

| | Default | Scheduler + spread |
|---|---|---|
| Server FPS | 878.6 | 1000.0 |
| Frame p99 | 587 µs | 259 µs |
| Snapshots per client at `cl_updaterate 100` | 97.5 /s | 100.0 /s |
| Snapshot interval jitter, client side | 0.2 ms | 0.1 ms |

Snapshot contents are unchanged. The first snapshot after a client becomes active can
come up to 1.5 intervals later (15 ms at 100 Hz), once per connection.

## Packet floods: the network thread

Server browsers, monitoring sites and attackers send queries; on the stock engine, the
game loop reads and answers each one itself. With `-netthread`, a dedicated thread reads
the server socket and passes the game loop only what it would act on. Game code,
Metamod, AMX Mod X and ReAPI hooks still all run on the main thread.

With `net_thread_filter 1` (default), the thread drops:

| Reason in `net_thread_stats` | Packets |
|---|---|
| `banned` | from addresses banned with `addip` (same rules as `sv_filterban`); the "banned" reply is still sent, at most 20 per second |
| `rate limited` | queries above `max_queries_sec`, `max_queries_sec_global`, `max_queries_window` |
| `malformed` | shorter than 4 bytes, larger than the engine accepts, or split packets (only servers send them) |
| `unknown address` | game (non-query) packets from addresses that are not connected clients |
| `queue full` | everything, when the game loop is too far behind (should stay at 0) |

The thread applies the same rules as the game loop would: what it drops would have been
ignored anyway. `addip`, `removeip` and the cvars apply within a frame. When a plugin
hooks the packets through ReAPI (`PreprocessPacket` or the connectionless rate limit
hooks), the thread leaves the corresponding checks to the game loop so that the plugin
still sees the packets; `net_thread_stats` then says so.

Measured on the full 32-player server under 50 000 packets/s from 4096 addresses
([`benchmarks/flood.md`](benchmarks/flood.md)):

| A2S query flood | No flood | Flood, no thread | Flood, `-netthread` |
|---|---|---|---|
| Server FPS | 1000.0 | 1000.0 | 1000.0 |
| Frame p99 | 263 µs | 552 µs | 224 µs |
| Main thread CPU | 8.0 % | 16.4 % | 7.9 % |

The network thread itself used about 23 % of a core for 50 000 packets/s, and almost
nothing when the server is quiet: it sleeps until a packet arrives.

The same benchmark also found that the stock IP rate limiter fell to **45 fps** under such
a flood (it walked its whole address table on every query). That fix is always on, with
or without the thread.

`net_thread_stats` prints the counters:

```
Network thread: running, queue 0/1024 packets (max 58), filter on
  received      6227
  dropped: queue full   0
  ...
```

`max` is the deepest the queue has been since startup. If `queue full` is not 0, the game
loop was stalled for longer than the queue holds (a long map change, a plugin blocking):
raise `-netthread_queue` (64–16384, rounded to a power of two).

## Many players and entities: parallel snapshot encoding

Each snapshot is built in two parts: the game DLL decides what each client sees
(`AddToFullPack`, the gather), then the engine delta-encodes it (the encode). On a full
server with many entities, encoding is the largest part of the send: about 50 µs per
snapshot of 150 entities.

`-sys_threads 2 +sv_parallel_send 1` moves the encoding to two worker threads. The main
thread still gathers every snapshot, in the same order as before, and each one is
encoded by a worker while the main thread gathers the next; the main thread then
transmits the datagrams in client order. The packets are byte-identical to the serial
ones.

Measured with 32 playing clients and 400 moving entities
([`benchmarks/clients32-ents.md`](benchmarks/clients32-ents.md)):

| | Serial | 2 workers |
|---|---|---|
| `send_messages` avg / p99 | 251 / 790 µs | 152 / 498 µs |
| Frame avg / p99 | 453 / 1896 µs | 346 / 1673 µs |
| Main thread CPU | 45.9 % | 35.1 % |
| Process CPU | 46.4 % | 59.1 % |

When to use it:

* Servers whose snapshots are big: many players in sight of each other, many visible
  entities (deathmatch, mods with many props or projectiles). On a 32-player server with
  ordinary rounds the encoding is ≈ 15 µs per frame, and there is nothing to gain.
* Two workers are enough: with snapshot spreading, a frame has 2–3 snapshots. A third
  one only adds CPU. `-sys_threads auto` starts one per CPU but one, up to 8, which is
  more than useful.
* Count the workers in the cores per instance: they use ≈ 12 % of a core each on the
  heavy benchmark, mostly encoding moved off the main thread.
* `sys_threads_spin_us` makes the workers spin between frames instead of sleeping. The
  workers are already woken while the first snapshot of a frame is gathered, so it only
  costs CPU; leave it at 0.

Plugins: the game DLL's own encoders (`Entity_Encode`, `Player_Encode`) run on the
workers, one at a time. A plugin hooking the encode through ReAPI keeps its hook on the
main thread: with `SV_EmitPings` hooked (scoreboard plugins), the snapshots carrying
pings are encoded by the main thread; with `SV_CreatePacketEntities` or `SV_EmitEvents`
hooked, all of them. `sv_parallel_send_stats` says which applies; the console also logs
it when it changes.

## The machine

These matter as much as the engine options at 1000 Hz:

* **CPU frequency**: the `powersave` / `schedutil` governors let the core sleep between
  ticks and wake up slowly. `cpupower frequency-set -g performance` (or the BIOS power
  profile) gives steadier frames. All the benchmarks were measured with `powersave`, the
  worst case.
* **One core per instance**: the game loop is single-threaded. When running several
  servers on one machine, give each its own physical core with `taskset -c N` and keep
  the network threads off the game loops' cores if possible (one spare core can serve
  several instances' network threads). Avoid putting two instances on the two hardware
  threads of the same core. With `-sys_threads`, give the instance's workers cores of
  their own as well, or leave them out (`sv_parallel_send 0`) when cores are short.
* **Socket buffers**: the engine does not set the socket buffer size, so the kernel's
  default applies (`net.core.rmem_default`, often 208 KB). Under a
  flood, a larger buffer gives the network thread room to catch up:

  ```sh
  sysctl -w net.core.rmem_default=4194304 net.core.rmem_max=4194304
  ```

  Packets lost there are counted by the kernel (`netstat -su`, "receive buffer errors").
* **Timer slack**: with `sys_scheduler 1`, the engine sets its own timer slack to 1 ns on
  Linux; nothing to do.
* **Virtual machines and containers**: shared vCPUs add milliseconds of scheduling noise
  that no engine option can remove. Containers are fine: the benchmarks ran in Docker, and the
  field validation server in Docker with `--network host`.

## Game DLL and plugins

The remaining spikes on a busy server usually come from game code, which the engine
cannot spread or move to another thread:

* **Bot navigation**: ReGameDLL_CS analyses a map without a `.nav` file on the main thread
  when bots join, which freezes the server for seconds to minutes. Generate the files
  beforehand for every map of the cycle (`tools/bench/lib/generate_nav.sh`).
* Bots' AI takes up to a few milliseconds in some frames.
* Plugins doing file or database I/O in a forward block the frame.

Measure before blaming the engine: `sv_profile 60` prints the tick rate, jitter and the
time spent in each phase; `physics`, `execute_client_msg` and `gather` are mostly game
DLL and plugin time.

## Measuring

```
sv_profile 60                 // record 60 s, then print the report
sv_profile_csv profile.csv    // all frames, for tools/bench/report.py
```

The profiler keeps the last `sv_profile_frames` frames (60 000 by default, 1 minute at
1000 Hz); for longer captures, raise it, then `sv_profile_reset` to apply it. It costs
about 1 µs per frame while recording (more with many clients and packets) and nothing
otherwise.

`tools/bench/` (Docker load tests: bots, protocol clients, HLTV, floods) and
`tools/staging/server.sh` (a test server with a typical plugin stack) reproduce every
number in this guide.
