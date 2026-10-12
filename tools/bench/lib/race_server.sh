#!/bin/bash
# Runs the server under a Valgrind tool with the network thread, the snapshot
# worker threads, playing load clients, a small query flood and a map change. Runs inside the benchmark container.
# usage: race_server.sh <hlds dir> <output dir> <tool> <seconds>
set -euo pipefail

hlds=$1
out=$2
tool=$3
secs=$4
bench=$(cd "$(dirname "$0")/.." && pwd)

cd "$hlds"
export LD_LIBRARY_PATH=.

fifo=$out/console.fifo
rm -f "$fifo"
mkfifo "$fifo"

# Plugins call the engine back from the encoding threads through Metamod (game DLL
# conditional encoders): load it when setup.sh installed it
dll_args=()
[ -f cstrike/addons/metamod/metamod_i386.so ] && dll_args=(-dll addons/metamod/metamod_i386.so)

# Valgrind runs the server about 50 times slower: low tick rate, few clients. It runs
# one thread at a time, and the main thread then encodes nearly every snapshot itself:
# mapchange_check.sh covers the encoding threads calling game DLL and Metamod code
valgrind --tool="$tool" --log-file="$out/$tool.log" --error-limit=no \
	./hlds_linux -game cstrike "${dll_args[@]}" -console -port 27015 +sv_lan 1 +maxplayers 8 -netthread -sys_threads 2 +sv_parallel_send 1 \
	+sys_ticrate 100 +mp_consistency 0 +sv_rehlds_maxclients_from_single_ip 8 \
	+max_queries_sec 1000 +max_queries_sec_global 100000 +map de_dust2 \
	< "$fifo" > "$out/server.log" 2>&1 &
server_pid=$!

exec 3> "$fifo"
send() {
	echo "$*" >&3
}

# Map load under Valgrind
sleep 60

python3 "$bench/loadclient.py" --server 127.0.0.1:27015 --clients 4 --cmdrate 30 --play \
	--measure-at "$(date +%s)" --duration "$secs" --output "$out/clients.json" > /dev/null 2> "$out/loadclient.log" &
# Query limits are raised so that the queries also reach the IP filters and the
# main thread; junk exercises the unknown address filter
python3 "$bench/flood.py" --kind a2s --pps 200 --sources 64 --duration "$secs" > /dev/null 2>&1 &
python3 "$bench/flood.py" --kind junk --pps 100 --sources 16 --duration "$secs" > /dev/null 2>&1 &

# The clients spawn at the next round (see run_server.sh)
sleep 20
send "sv_restart 1"

# Filter list changes while the thread is filtering
sleep $((secs / 3))
send "addip 0 127.2.0.0/24"
sleep $((secs / 3))
send listip
send net_thread_stats
send "removeip 127.2.0.0/24"
sleep $((secs - 2 * (secs / 3)))
send net_thread_stats
send sv_parallel_send_stats
send status
# Map change with the threads running and clients connected
send "changelevel de_dust2"
sleep 60
send status
sleep 3
send quit
exec 3>&-
wait "$server_pid" || true
rm -f "$fifo"
