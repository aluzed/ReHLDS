#!/bin/bash
# Runs the server with Metamod-R, AMX Mod X, bots and playing load clients, with
# snapshots encoded on worker threads, and changes the map in a loop. Runs inside the
# benchmark container.
# usage: mapchange_server.sh <hlds dir> <output dir> <cycles> <server args...>
#
# Game DLL conditional encoders, which run on the encoding threads, call the engine
# back through Metamod's callbacks; a race on Metamod's state corrupted the heap,
# which crashed the server at the next map change (Metamod rebuilds its callbacks).
set -uo pipefail

hlds=$1
out=$2
cycles=$3
shift 3
bench=$(cd "$(dirname "$0")/.." && pwd)

cd "$hlds"
export LD_LIBRARY_PATH=.
# glibc checks the heap more often and aborts on the first inconsistency
export MALLOC_CHECK_=3

fifo=$out/console.fifo
rm -f "$fifo"
mkfifo "$fifo"

./hlds_linux -game cstrike -dll addons/metamod/metamod_i386.so -console -port 27015 \
	+sv_lan 1 +maxplayers 20 +mp_consistency 0 +sv_rehlds_maxclients_from_single_ip 8 \
	"$@" +map de_dust2 < "$fifo" > "$out/server.log" 2>&1 &
server_pid=$!

exec 3> "$fifo"
send() {
	echo "$*" >&3
}

alive() {
	kill -0 "$server_pid" 2> /dev/null
}

sleep 8
send "bot_join_after_player 0"
send "bot_quota 12"

result=0
for cycle in $(seq 1 "$cycles"); do
	# Load clients reconnect after each map change; they join at the next round
	python3 "$bench/loadclient.py" --server 127.0.0.1:27015 --clients 6 --cmdrate 60 --play \
		--warmup 5 --duration 30 > /dev/null 2>> "$out/loadclient.log" &
	clients=$!
	(sleep 12; alive && echo "sv_restart 1" >&3) &

	sleep 40
	send status
	send sv_parallel_send_stats
	wait "$clients" 2> /dev/null

	send "changelevel de_dust2"
	sleep 15
	if ! alive; then
		echo "crash after map change $cycle" | tee -a "$out/result.txt"
		result=1
		break
	fi
	echo "map change $cycle: ok" >> "$out/result.txt"
done

if alive; then
	send quit
	exec 3>&-
	sleep 5
	if alive; then
		kill "$server_pid"
	elif ! wait "$server_pid"; then
		echo "crash on quit" | tee -a "$out/result.txt"
		result=1
	fi
fi

rm -f "$fifo"
exit $result
