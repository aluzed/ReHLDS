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
. "$scenario"

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

./hlds_linux -game cstrike -console -port 27015 \
	+sv_lan 1 +maxplayers "$MAXPLAYERS" +sv_proxies "$HLTV" +sys_ticrate "$TICRATE" $client_args $EXTRA_ARGS ${BENCH_SERVER_ARGS:-} +map "$MAP" \
	< "$fifo" > "$out/server.log" 2>&1 &
server_pid=$!

exec 3> "$fifo"
send() {
	echo "$*" >&3
}

sleep 8
send "bot_join_after_player 0"
send "bot_quota $BOTS"

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
		--output "$out/clients.json" > /dev/null 2> "$out/loadclient.log" &
fi

sleep "$WARMUP"
send status

ticks_start=$(cpu_ticks "$server_pid")
time_start=$(date +%s.%N)
send "sv_profile $DURATION"
sleep "$DURATION"
ticks_end=$(cpu_ticks "$server_pid")
time_end=$(date +%s.%N)

# Let the profiler finish its capture and print the report
sleep 2
send "sv_profile_csv bench_profile.csv"
send status
sleep 2
send quit
exec 3>&-
wait "$server_pid" || true

mv cstrike/bench_profile.csv "$out/profile.csv"
rm -f "$fifo"

cat > "$out/cpu.txt" <<EOF
cpu_ticks=$((ticks_end - ticks_start))
clk_tck=$(getconf CLK_TCK)
wall_start=$time_start
wall_end=$time_end
EOF

# Wait for the HLTV proxies to exit
wait || true
