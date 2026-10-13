#!/bin/bash
# Runs one benchmark scenario. Runs inside the benchmark container.
# usage: run_server.sh <hlds dir> <scenario file> <output dir>
#
# The server console is driven through a FIFO. After the warm-up, the engine
# profiler records for DURATION seconds and the frames are dumped to CSV.
# Process CPU time is sampled over the same window.
set -euo pipefail

hlds=$1
scenario=$2
out=$3

# Scenario defaults, overridden by the scenario file
MAP=de_dust2
MAXPLAYERS=32
TICRATE=1000
BOTS=0
HLTV=0
HLTV_UPDATERATE=100
CLIENTS=0
CLIENT_CMDRATE=100
WARMUP=20
DURATION=30
EXTRA_ARGS=
# Connectionless flood during the measurement window (tools/bench/flood.py)
FLOOD_KIND=
FLOOD_PPS=50000
FLOOD_SOURCES=1024
# METAMOD=1 loads Metamod-R and AMX Mod X with the benchmark plugins
METAMOD=0
# Moving entities spawned around the players (tools/bench/plugins/bench_entities.sma)
ENTITIES=0
# CLIENTS_ALIVE=1: the load clients join a team and spawn as players (loadclient.py
# --play); by default they stay dead, as in the first reports
CLIENTS_ALIVE=0
# Console commands sent once the map is loaded, and at the end, separated by ';'
SERVER_COMMANDS=
FINAL_COMMANDS=
. "$scenario"

# utime + stime of every thread: "<tid> <name> <ticks>" lines, names made shell-safe
thread_ticks() {
	for task in /proc/"$1"/task/*; do
		echo "${task##*/} $(tr -c 'A-Za-z0-9\n-' _ < "$task/comm") $(awk '{ print $14 + $15 }' "$task/stat")"
	done 2> /dev/null | sort
}

cpu_ticks() {
	# utime + stime, fields 14 and 15 of /proc/<pid>/stat
	awk '{ print $14 + $15 }' "/proc/$1/stat"
}

cd "$hlds"
export LD_LIBRARY_PATH=.

# Protocol load clients all come from 127.0.0.1 and do not answer consistency checks
client_args=
if [ "$CLIENTS" -gt 0 ]; then
	client_args="+mp_consistency 0 +sv_rehlds_maxclients_from_single_ip $CLIENTS"
fi

fifo=$out/console.fifo
rm -f "$fifo"
mkfifo "$fifo"

dll_args=()
if [ "$METAMOD" = 1 ]; then
	dll_args=(-dll addons/metamod/metamod_i386.so)
fi

server_args=(-game cstrike "${dll_args[@]}" -console -port 27015
	+sv_lan 1 +maxplayers "$MAXPLAYERS" +sv_proxies "$HLTV" +sys_ticrate "$TICRATE" $client_args $EXTRA_ARGS ${BENCH_SERVER_ARGS:-} +map "$MAP")

# BENCH_RECORD=1: record the session's inputs and sends for tools/bench/replay.sh,
# which replays them with the same arguments
record_args=()
if [ "${BENCH_RECORD:-0}" = 1 ]; then
	printf '%s\n' "${server_args[@]}" > "$out/server_args.txt"
	record_args=(-netrecord "$out/session.rec" -netsends "$out/live.sends")
fi

${BENCH_NO_ASLR:+setarch "$(uname -m)" -R} ./hlds_linux "${record_args[@]}" "${server_args[@]}" < "$fifo" > "$out/server.log" 2>&1 &
server_pid=$!

exec 3> "$fifo"
send() {
	echo "$*" >&3
}

sleep 8
send "bot_join_after_player 0"
send "bot_quota $BOTS"
if [ -n "$SERVER_COMMANDS" ]; then
	send "$SERVER_COMMANDS"
fi

# HLTV proxies are real network clients that receive full snapshots
for i in $(seq 1 "$HLTV"); do
	(sleep $((WARMUP + DURATION + 15)); echo quit) \
		| ./hltv -port $((27020 + i)) +updaterate "$HLTV_UPDATERATE" +connect 127.0.0.1:27015 > "$out/hltv-$i.log" 2>&1 &
done

# Protocol load clients measure snapshot intervals over the profiling window
if [ "$CLIENTS" -gt 0 ]; then
	python3 "$(dirname "$0")/../loadclient.py" --server 127.0.0.1:27015 \
		--clients "$CLIENTS" --cmdrate "$CLIENT_CMDRATE" \
		--measure-at "$(( $(date +%s) + WARMUP ))" --duration "$DURATION" \
		$([ "$CLIENTS_ALIVE" = 1 ] && echo --play) \
		--output "$out/clients.json" > /dev/null 2> "$out/loadclient.log" &
fi

if [ -n "$FLOOD_KIND" ]; then
	python3 "$(dirname "$0")/../flood.py" --server 127.0.0.1:27015 \
		--kind "$FLOOD_KIND" --pps "$FLOOD_PPS" --sources "$FLOOD_SOURCES" \
		--start-at "$(( $(date +%s) + WARMUP ))" --duration "$DURATION" \
		--output "$out/flood.json" > /dev/null 2> "$out/flood.log" &
fi

# Players who join during a round wait for the next one, which never comes
# without bots: restart the round once the load clients are in the game
if [ "$CLIENTS" -gt 0 ] && [ "$CLIENTS_ALIVE" = 1 ]; then
	(sleep 12; echo "sv_restart 1" >&3) &
fi

if [ "$ENTITIES" -gt 0 ]; then
	# Spawned once the load clients are in the game, around them
	(sleep 10; echo "bench_spawn $ENTITIES" >&3) &
fi

sleep "$WARMUP"
send status

ticks_start=$(cpu_ticks "$server_pid")
main_ticks_start=$(cpu_ticks "$server_pid/task/$server_pid")
threads_start=$(thread_ticks "$server_pid")
time_start=$(date +%s.%N)
send "sv_profile $DURATION"
sleep "$DURATION"
ticks_end=$(cpu_ticks "$server_pid")
main_ticks_end=$(cpu_ticks "$server_pid/task/$server_pid")
threads_end=$(thread_ticks "$server_pid")
time_end=$(date +%s.%N)

# Let the profiler finish its capture and print the report
sleep 2
send "sv_profile_csv bench_profile.csv"
send status
send net_thread_stats
send sv_parallel_send_stats
if [ -n "$FINAL_COMMANDS" ]; then
	send "$FINAL_COMMANDS"
fi
sleep 2
send quit
exec 3>&-
wait "$server_pid" || true

mv cstrike/bench_profile.csv "$out/profile.csv"
rm -f "$fifo"

cat > "$out/cpu.txt" <<EOF
cpu_ticks=$((ticks_end - ticks_start))
main_thread_cpu_ticks=$((main_ticks_end - main_ticks_start))
$(join <(echo "$threads_start") <(echo "$threads_end") | awk '{ ticks[$2] += $5 - $3 } END { for (n in ticks) if (ticks[n] > 0) print "thread_" n "_ticks=" ticks[n] }' | sort)
clk_tck=$(getconf CLK_TCK)
wall_start=$time_start
wall_end=$time_end
EOF

# Wait for the HLTV proxies to exit
wait || true
