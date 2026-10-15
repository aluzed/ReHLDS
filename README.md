# ReHLDS multithreaded [![C/C++ CI](https://github.com/aluzed/ReHLDS-MT/actions/workflows/build.yml/badge.svg?branch=master)](https://github.com/aluzed/ReHLDS-MT/actions/workflows/build.yml) [![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)](./LICENSE) <img align="right" src="https://user-images.githubusercontent.com/5860435/111066129-040e5e00-84f0-11eb-9e1f-7a7e8611da2b.png" alt="ReHLDS" />
Reverse-engineered (and bugfixed) HLDS, with a multithreaded server loop

## What is this?
A fork of [ReHLDS](https://github.com/rehlds/ReHLDS) whose goal is a steadier, faster server
tick: work that does not touch the game DLL (network input and output, query answers,
snapshot encoding, log writing, Steam calls) moves off the main thread, and the frame is
paced on an exact grid. Game DLL, Metamod, AMX Mod X and ReAPI code still runs on the main
thread only, so mods and plugins keep working unchanged.

ReHLDS itself is a result of reverse engineering of original HLDS (build 6152/6153) using
DWARF debug info embedded into linux version of HLDS, engine_i486.so. Along with reverse
engineering, a lot of defects and (potential) bugs were found and fixed.

### What the fork adds
All of it is on by default once validated on a public server with players, Metamod-R,
AMX Mod X, ReAPI and CSDM; every part can be turned off (see
[performance tuning](docs/performance-tuning.md)).

| | Without | With |
|---|---|---|
| Server FPS at `sys_ticrate 1000` (idle) | 935.7 | 1000.0 |
| Tick jitter (idle) | 14.4 µs | 4.6 µs |
| Snapshots per client at `cl_updaterate 100` | 97.5 /s | 100.0 /s |
| Main thread CPU during an A2S query flood | 16.4 % | 7.9 % (same as without a flood) |
| Players connecting during a 50 000 queries/s spoofed flood | 0/5 | 5/5 |
| Server browser answer | 12.1 ms | 0.09 ms |
| Packet transmit phase, 32 players | 21 µs per frame | 3 µs per frame |

* **Frame scheduler** (`sys_scheduler`): frames on a fixed grid of deadlines, exact `sys_ticrate`.
* **Snapshot spreading** (`sv_rehlds_snapshot_spread`): each client's snapshots at a fixed phase, exact update rate.
* **Network receive thread** (`-nonetthread` to disable): reads the socket and drops floods, banned addresses and junk before the game loop.
* **Server browser answers on the network thread** (`sv_a2s_cache`), with their own rate limits and challenges.
* **Network send thread** (`-nonetsend` to disable): packets handed to the kernel off the main thread, one `sendmmsg` per tick on Linux.
* **Parallel snapshot encoding** (`-sys_threads`, `sv_parallel_send`): delta encoding on worker threads, byte-identical output; game DLL encoders run on the main thread and are replayed by the workers.
* **Asynchronous log writer** (`sv_log_async`), **Steam calls on demand** (`sv_steam_on_demand`), **client lookup by address** (`sv_client_lookup_cache`).
* **Tooling**: per-phase frame profiler (`sv_profile`), byte-exact session record and replay, Docker load tests, race and map change checks (`tools/bench/`), a field validation server (`tools/staging/`).

Design notes and measurements for every change are in [`docs/stories/`](docs/stories/README.md);
the field sessions are in [`docs/benchmarks/field-validation.md`](docs/benchmarks/field-validation.md).
Windows builds have not been run on a server yet.

### Quick start (Linux)

```sh
./build-docker.sh                 # build in a container, output in build/
./run.sh /path/to/hlds            # build, install the engine, start with every option on
```

* `build-docker.sh` builds the engine (`build/rehlds/engine_i486.so`) and the other binaries in
  a Debian 11 container with the 32-bit toolchain: the host needs only Docker. The image
  (`tools/bench/docker`) is built on the first run. `./build.sh` builds on the host, which then
  needs the 32-bit packages listed under [Building](#building).
* `run.sh <hlds dir> [server args...]` runs `build-docker.sh`, copies the engine over the
  install's `engine_i486.so` (the original is kept as `engine_i486.so.orig`) and starts
  `hlds_run` with every option of the fork on, including the ones off by default:
  `-sys_threads auto`, `sys_spin_us` and `sys_threads_spin_us` 200, `sys_wake_on_packet 1`,
  `sv_profile_enable 1`. Environment: `GAME`, `MAP`, `PORT`, `MAXPLAYERS`, `TICRATE` (1000),
  `SYS_THREADS`, `SPIN_US` (0 when the server has no cores of its own: spinning burns them),
  `WAKE_ON_PACKET`, `PROFILE`, `CPUS` (cores given with `taskset`), `NO_BUILD=1` (use
  `build/` as it is), `RESTART=1` (let `hlds_run` restart the server after it exits or crashes;
  by default `-norestart`). Example: `SPIN_US=0 CPUS=2-5 MAP=de_inferno ./run.sh ~/hlds`.

> [!TIP]
> ReHLDS linux-releases now is signed via `GPG`, pubkey is: `63547829004f07716f7be4856c32c4282e60fb67` and could be found at [https://keyserver.ubuntu.com/](https://keyserver.ubuntu.com/pks/lookup?search=63547829004f07716f7be4856c32c4282e60fb67+&fingerprint=on&op=index).
>
> How to:
> 1. [Download](https://keyserver.ubuntu.com/pks/lookup?op=get&search=0x63547829004f07716f7be4856c32c4282e60fb67) `63547829004f07716f7be4856c32c4282e60fb67.asc` key
> 2. Import: `gpg --import 63547829004f07716f7be4856c32c4282e60fb67.asc`
> 3. Download release `archive` and `.asc` file.
> 4. Verify: `gpg --verify some-rehlds.zip.asc some-rehlds.zip`.

## Goals of the project
<ul>
<li>Provide more stable (than official) version of Half-Life dedicated server with extended API for mods and plugins</li>
<li>Performance optimizations (use of SSE for vector math for example) is another goal for the future</li>
<li>This fork: a steadier and faster tick by moving engine work that does not touch the game DLL off the main thread</li>
</ul>

## 🛠 License

ReHLDS is licensed under the [MIT License](./LICENSE).

### License Transition
> [!NOTE]  
> Originally released under [GPLv3](https://www.gnu.org/licenses/gpl-3.0.html), ReHLDS transitioned to the MIT License in July 2025 with the agreement of the core contributors.  
> See [LICENSE-TRANSITION.md](./LICENSE-TRANSITION.md) for details.

## How can use it?
ReHLDS is fully compatible with the official pre-anniversary edition of HLDS (engine version <= 8684) downloaded by steamcmd. All you have to do is to download ReHLDS binaries and replace original swds.dll/engine_i486.so. For windows you can also copy a swds.pdb file with a debug information.

> [!CAUTION]  
> ReHLDS is not compatible with an old 5xxx or below platforms downloaded by hldsupdatetool.

#### Downloading HLDS via steamcmd

```
app_set_config 90 mod cstrike
app_update 90 -beta steam_legacy validate
```

## Downloads
* [Release builds](https://github.com/rehlds/ReHLDS/releases)
* [Dev builds](https://github.com/rehlds/ReHLDS/actions/workflows/build.yml)

ReHLDS binaries require `SSE`, `SSE2` and `SSE3` instruction sets to run and can benefit from `SSE4.1` and `SSE4.2`

<b>Warning!</b> ReHLDS is not binary compatible with original hlds since it's compiled with compilers other than ones used for original hlds.
This means that plugins that do binary code analysis (Orpheu for example) probably will not work with ReHLDS.

## Configuring
<details>
<summary>Click to expand</summary>
<ul>
<li>listipcfgfile &lt;filename&gt; // File for permanent ip bans. Default: listip.cfg
<li>syserror_logfile &lt;filename&gt; // File for the system error log. Default: sys_error.log
<li>sv_auto_precache_sounds_in_models &lt;1|0&gt; // Automatically precache sounds attached to models. Deault: 0
<li>sv_delayed_spray_upload &lt;1|0&gt; // Upload custom sprays after entering the game instead of when connecting. It increases upload speed. Default: 0
<li>sv_echo_unknown_cmd &lt;1|0&gt; // Echo in the console when trying execute an unknown command. Default: 0
<li>sv_rcon_condebug &lt;1|0&gt; // Print rcon debug in the console. Default: 1
<li>sv_force_ent_intersection &lt;1|0&gt; // In a 3-rd party plugins used to force colliding of SOLID_SLIDEBOX entities. Default: 0
<li>sv_rehlds_force_dlmax &lt;1|0&gt; // Force a client's cl_dlmax cvar to 1024. It avoids an excessive packets fragmentation. Default: 0
<li>sv_rehlds_hull_centering &lt;1|0&gt; // Use center of hull instead of corner. Default: 0
<li>sv_rehlds_movecmdrate_max_avg // Max average level of 'move' cmds for ban. Default: 400
<li>sv_rehlds_movecmdrate_avg_punish // Time in minutes for which the player will be banned (0 - Permanent, use a negative number for a kick). Default: 5
<li>sv_rehlds_movecmdrate_max_burst // Max burst level of 'move' cmds for ban. Default: 2500
<li>sv_rehlds_movecmdrate_burst_punish // Time in minutes for which the player will be banned (0 - Permanent, use a negative number for a kick). Default: 5
<li>sv_rehlds_send_mapcycle &lt;1|0&gt; // Send mapcycle.txt in serverinfo message (HLDS behavior, but it is unused on the client). Default: 0
<li>sv_rehlds_stringcmdrate_max_avg // Max average level of 'string' cmds for ban. Default: 80
<li>sv_rehlds_stringcmdrate_avg_punish // Time in minutes for which the player will be banned (0 - Permanent, use a negative number for a kick). Default: 5
<li>sv_rehlds_stringcmdrate_max_burst // Max burst level of 'string' cmds for ban. Default: 400
<li>sv_rehlds_stringcmdrate_burst_punish // Time in minutes for which the player will be banned (0 - Permanent, use a negative number for a kick). Default: 5
<li>sv_rehlds_dlfile_bucket_size // Max size of the dlfile token bucket. 0 - automatic (map resource list size + 16), a negative number disables dlfile rate limiting. Default: 0
<li>sv_rehlds_dlfile_refillrate // How many dlfile tokens are restored per second. Default: 50
<li>sv_rehlds_dlfile_punish // Time in minutes for which the player will be banned for dlfile flooding (0 - Permanent, use a negative number for a kick). Default: -1 (kick)
<li>sv_rehlds_userinfo_transmitted_fields // Userinfo fields only with these keys will be transmitted to clients via network. If not set then all fields will be transmitted (except prefixed with underscore). Each key must be prefixed by backslash, for example "\name\model\*sid\*hltv\bottomcolor\topcolor". See [wiki](https://github.com/rehlds/ReHLDS/wiki/Userinfo-keys) to collect sufficient set of keys for your server. Default: ""
<li>sv_rehlds_attachedentities_playeranimationspeed_fix // Fixes bug with gait animation speed increase when player has some attached entities (aiments). Can cause animation lags when cl_updaterate is low. Default: 0
<li>sv_rehlds_maxclients_from_single_ip // Limit number of connections at the same time from single IP address, not confuse to already connected players. Default: 5
<li>sv_rehlds_local_gametime &lt;1|0&gt; // A feature of local gametime which decrease "lags" if you run same map for a long time. Default: 0
<li>sv_rehlds_allow_large_sprays &lt;1|0&gt; // Allow larger custom logos than 64x64. Default: 1
<li>sv_use_entity_file // Use custom entity file for a map. Path to an entity file will be "maps/[map name].ent". 0 - use original entities. 1 - use .ent files from maps directory. 2 - use .ent files from maps directory and create new .ent file if not exist.
<li>sv_usercmd_custom_random_seed // When enabled server will populate an additional random seed independent of the client. Default: 0
<li>sv_net_incoming_decompression &lt;1|0&gt; // When enabled server will decompress of incoming compressed file transfer payloads. Default: 1
<li>sv_net_incoming_decompression_max_ratio &lt;0|100&gt; // Sets the max allowed ratio between compressed and uncompressed data for file transfer. (A ratio close to 90 indicates large uncompressed data with low entropy) Default: 80.0
<li>sv_net_incoming_decompression_max_size &lt;16|65536&gt; // Sets the max allowed size for decompressed file transfer data. Default: 65536 bytes
<li>sv_net_incoming_decompression_min_failures &lt;0|10&gt; // Sets the min number of decompression failures required before a player's connection is flagged for potential punishment. Default: 4
<li>sv_net_incoming_decompression_max_failures &lt;0|10&gt; // Sets the max number of decompression failures allowed within a specified time window before action is taken against the player. Default: 10
<li>sv_net_incoming_decompression_min_failuretime: &lt;0.1|10.0&gt; // Sets the min time in secs within which decompression failures are tracked to determine if the player exceeds the failure thresholds. Default: 0.1
<li>sv_net_incoming_decompression_punish // Time in minutes for which the player will be banned for malformed/abnormal bzip2 fragments (0 - Permanent, use a negative number for a kick). Default: -1
<li>sv_tags &lt;comma-delimited string list of tags&gt; // Sets a string defining the "gametags" for this server, this is optional, but if it is set it allows users/scripts to filter in the matchmaking/server-browser interfaces based on the value. Default: ""
<li>sv_filterban &lt;-1|0|1&gt;// Set packet filtering by IP mode. -1 - All players will be rejected without any exceptions. 0 - No checks will happen. 1 - All incoming players will be checked if they're IP banned (if they have an IP filter entry), if they are, they will be kicked. Default: 1
<li>sv_rehlds_movecmd_max_ticks // Set maximum amount of movement commands the server is able to process from a single player in a single frame. This includes the commands itself, not packets. Default: 24
<li>sv_rehlds_movecmd_max_null_streak // Defines the maximum allowed consecutive movement commands with zero time duration (empty commands). 0 - disables the check. Default: 0
<li>sv_rehlds_movecmd_clamp_interp &lt;1|0&gt; // Defines whether should the server block movement commands with invalid (out of range) "ex_interp" value. Default: 1
<li>sv_rehlds_movecmdtime_samples // Defines the number of frames the server takes to average the client's movement speed. Higher - more accurate but slower detection, lower - vice versa. Default: 120
<li>sv_rehlds_movecmdtime_max_error // Defines how far a client's internal game clock can go ahead of or behind the server's clock in milliseconds. If this limit is exceeded, the server evaluates the client's game speed. Penalties are ONLY applied if the client also violates the "sv_rehlds_movecmdtime_max_scale" or "sv_rehlds_movecmdtime_min_scale" limits. Default: 300
<li>sv_rehlds_movecmdtime_max_scale // Defines the max client's base game speed ratio. Clients speeding the game up beyond this multiplier will receive warnings. Default: 3.0
<li>sv_rehlds_movecmdtime_min_scale // Defines the min client's base game speed ratio. Clients slowing the game down below this multiplier will receive warnings. Default: 0.5
<li>sv_rehlds_movecmdtime_max_warnings // Maximum allowed speedhack/slowmo warnings before the punishment is applied. -1 - disable detection. Default: -1
<li>sv_rehlds_movecmdtime_punish // Time in minutes for which the player will be banned for speedhacking/slowing (-1 - Kick, 0 - Permanent, use a negative number for a kick). Default: -1
<li>sv_reconnect_timeout // Hard deadline in seconds for a client to re-initiate its connection after a level change, independent of netchan activity. Closes a phantom-slot exploit where a cheat blocks the "reconnect" command and keeps the netchan warm so sv_timeout never fires. 0 - disabled. Default: 30
<li>sys_scheduler &lt;1|0&gt; // Pace server frames on a fixed grid of deadlines, one every 1/sys_ticrate seconds, instead of the launcher's 1 ms sleep loop. Gives the exact sys_ticrate (e.g. 1000.0 instead of ~935 fps) with a much steadier tick. See [performance tuning](docs/performance-tuning.md). Default: 1
<li>sys_scheduler_slice &lt;1-1000&gt; // With sys_scheduler 1, how long in milliseconds the engine keeps running frames before returning to the launcher (console input). Default: 100
<li>sys_spin_us &lt;0-2000&gt; // With sys_scheduler 1, busy-wait this many microseconds before each tick instead of sleeping, for an even steadier tick. Burns CPU: only for a core dedicated to the server. Default: 0
<li>sys_wake_on_packet &lt;1|0&gt; // With sys_scheduler 1 and the network thread, read the packets as they arrive between two ticks instead of at the next tick. Takes the client commands out of the tick (shorter frames) but does not lower the latency players see, which the snapshot interval sets. About 3 % of a core. Default: 0
<li>sv_rehlds_snapshot_spread &lt;1|0&gt; // Spread the clients' snapshots evenly over the frames (each client gets a fixed phase in its update interval) instead of sending them all in the same frame, and keep the exact cl_updaterate. Default: 1
<li>sv_profile_enable &lt;1|0&gt; // Record the per-phase timings of every frame continuously (last sv_profile_frames frames). sv_profile &lt;seconds&gt; records without it. Default: 0
<li>sv_profile_frames &lt;1000-600000&gt; // Number of frames the profiler keeps. Takes effect after sv_profile_reset. Default: 60000
<li>sv_parallel_send &lt;1|0&gt; // Encode the clients' snapshots on the worker threads started with -sys_threads. With game DLL conditional encoders (Counter-Strike), after the main thread gathered them all; otherwise while it gathers the next ones. Snapshots stay byte-identical. Falls back to the main thread for the snapshots a plugin hooks (ReAPI SV_EmitPings: snapshots with pings; SV_CreatePacketEntities, SV_EmitEvents: all). See [performance tuning](docs/performance-tuning.md). Default: 1
<li>sys_threads_spin_us &lt;0-2000&gt; // How long the worker threads spin for the next frame's snapshots before sleeping. Default: 0
<li>sv_log_async &lt;1|0&gt; // Write the server log file and qconsole.log (-condebug) on a dedicated thread, so that a slow disk never stalls a frame. Takes effect at the next log file (map change or "log on"). Default: 1
<li>sv_log_async_overflow &lt;block|drop&gt; // With sv_log_async 1, when 4 MB of log lines are waiting to be written: wait for room (block) or drop the line and count it (drop). Default: block
<li>sv_client_lookup_cache &lt;1|0&gt; // Find the client a game packet comes from through a table indexed by address, each result checked against the slot, instead of comparing the address of every slot. Default: 1
<li>sv_steam_on_demand &lt;1|0&gt; // Read the packets Steam wants sent on the frame after a packet was handed to Steam and every 100 ms, instead of every 10 ms, and tell Steam about a player's name and score only when they change. Saves about 3.5 ms of main thread time per second. See [performance tuning](docs/performance-tuning.md). Default: 1
<li>net_thread_filter &lt;1|0&gt; // With the network thread, drop on the network thread the packets the main thread would ignore: IP-banned addresses (sv_filterban), queries above the rate limits, malformed packets and game packets from addresses that are not connected. Default: 1
<li>sv_a2s_cache &lt;1|0&gt; // With the network thread and net_thread_filter 1, answer the server browser queries (A2S_INFO, A2S_PLAYER, A2S_RULES) on the network thread, as soon as they arrive, with a copy of Steam's answers refreshed every second and when players, cvars or the map change. Queries get their own rate limits, so a query flood no longer keeps players from connecting, and the challenges have their own budget, so a flood from spoofed addresses no longer hides the server from browsers. Off while a plugin hooks PreprocessPacket or the connectionless rate limits. See [performance tuning](docs/performance-tuning.md). Default: 1
<li>sv_a2s_challenges_sec &lt;number&gt; // With sv_a2s_cache 1, most challenges per second the network thread sends to server browsers, for all addresses together; each address is also limited by max_queries_sec. A query flood from spoofed addresses gets as many challenges back (9 bytes each), up to this cap; past it, real browsers compete with the flood. 0: no cap. Default: 0</li>
</ul>
</details>

### Command-line parameters
<ul>
<li>-nonetthread // Do not start the network thread: by default the server socket is read on a dedicated thread, which filters floods and junk before they reach the game loop (see net_thread_filter). -netthread, which started it before it was on by default, is still accepted. Default: thread started
<li>-sys_threads &lt;N|auto&gt; // Start N worker threads (at most 8; auto: one per CPU but one; 0: none, snapshots encoded on the main thread) for sv_parallel_send. Default: 2, fewer on a machine with fewer than 3 CPUs
<li>-netthread_queue &lt;64-16384&gt; // Size in packets of the queue between the network thread and the main thread. Default: 1024
<li>-nonetsend // Do not start the send thread: by default the server's packets are sent from a dedicated thread, the game loop queues them instead of waiting for the kernel (one sendmmsg call for a tick's packets on Linux). Statistics: net_send_stats. -netsend, which started it before it was on by default, is still accepted. Default: thread started
<li>-netsend_queue &lt;64-16384&gt; // Size in packets of the queue between the main thread and the send thread. Default: 1024
<li>-netrecord &lt;file&gt; // Record the inputs of the session (clock, packets, console) to replay it byte for byte. For developers, see [docs/benchmarks/replay.md](docs/benchmarks/replay.md)
<li>-netreplay &lt;file&gt; // Replay a session recorded with -netrecord as fast as possible, without the network, then quit
<li>-netsends &lt;file&gt; // With -netrecord or -netreplay, write every packet sent by the server to &lt;file&gt;
</ul>

See [docs/performance-tuning.md](docs/performance-tuning.md) for the recommended settings.

## Commands
<ul>
<li>rescount // Prints the total count of precached resources in the server console
<li>reslist &lt;sound | model | decal | generic | event&gt; // Separately prints the details of the precached resources for sounds, models, decals, generic and events in server console. Useful for managing resources and dealing with the goldsource precache limits.
<li>rcon_adduser &lt;ipaddress/CIDR&gt; // Add a new IP address or CIDR range to RCON user list (This command adds a new IP address to the RCON user list. The specified IP or CIDR range is granted privileged access to server console. Without any Rcon users, access is allowed to anyone with a valid password)</li>
<li>rcon_deluser &lt;ipaddress&gt; {removeAll} // Remove an IP address or CIDR range from RCON user list</li>
<li>rcon_users // List all IP addresses and CIDR ranges in RCON user list</li>
<li>sv_profile [seconds] // Record the per-phase frame timings for &lt;seconds&gt;, then print a report (tick rate, jitter, percentiles per phase). Without argument, print a report of the frames already recorded</li>
<li>sv_profile_csv &lt;file&gt; // Write the recorded frames to &lt;file&gt; in the game directory, one row per frame (tools/bench/report.py reads it)</li>
<li>sv_profile_reset // Clear the recorded frames and apply a new sv_profile_frames</li>
<li>sv_parallel_send_stats // Print the number of worker threads and whether the snapshots are encoded in parallel, else why not</li>
<li>sv_log_async_stats // Print the log writer thread's counters: lines queued, queue usage, dropped lines, time the main thread waited, write errors</li>
<li>net_thread_stats // Print the network thread's counters: packets received, dropped per reason, ban replies, queue usage</li>
</ul>

## Build instructions
### Checking requirements
There are several software requirements for building ReHLDS:

#### Windows
<pre>
Visual Studio 2015 (C++14 standard) and later
</pre>

#### Linux
<pre>
cmake >= 3.10
GCC >= 4.9.2 (Optional)
ICC >= 15.0.1 20141023 (Optional)
LLVM (Clang) >= 6.0 (Optional)
</pre>

### Building

#### Windows
Use `Visual Studio` to build, open `msvc/ReHLDS.sln` and just select from the solution configurations list `Release Swds` or `Debug Swds`

#### Linux

* With Docker, no toolchain on the host: `./build-docker.sh` (see [Quick start](#quick-start-linux)).
* Optional options using `build.sh --compiler=[gcc] --jobs=[N] -D[option]=[ON or OFF]` (without square brackets)

<pre>
-c=|--compiler=[icc|gcc|clang]  - Select preferred C/C++ compiler to build
-j=|--jobs=[N]                  - Specifies the number of jobs (commands) to run simultaneously (For faster building)

<sub>Definitions (-D)</sub>
DEBUG                           - Enables debugging mode
USE_STATIC_LIBSTDC              - Enables static linking library libstdc++
</pre>

* ICC          <pre>./build.sh --compiler=intel</pre>
* LLVM (Clang) <pre>./build.sh --compiler=clang</pre>
* GCC          <pre>./build.sh --compiler=gcc</pre>

##### Checking build environment (Debian / Ubuntu)

<details>
<summary>Click to expand</summary>

<ul>
<li>
Installing required packages
<pre>
sudo dpkg --add-architecture i386
sudo apt-get update
sudo apt-get install -y gcc-multilib g++-multilib
sudo apt-get install -y build-essential
sudo apt-get install -y libc6-dev libc6-dev-i386
sudo apt-get install -y cmake			
</pre>
</li>

<li>
Select the preferred C/C++ Compiler installation
<pre>
1) sudo apt-get install -y gcc g++
2) sudo apt-get install -y clang
</pre>
</li>
</ul>
</details>

## How can I help the project?
Just install it on your game server and report problems you faced.
Merge requests are also welcome :)
