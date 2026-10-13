#!/bin/bash
# Replays a recorded session. Runs inside the benchmark container.
# usage: replay_server.sh <hlds dir> <result dir> <sends file> <log file>
set -euo pipefail

hlds=$1
result=$2
sends=$3
log=$4

mapfile -t server_args < "$result/server_args.txt"

cd "$hlds"
export LD_LIBRARY_PATH=.

# The replay quits at the end of the recording; stdin stays open but is ignored
${BENCH_NO_ASLR:+setarch "$(uname -m)" -R} timeout 1800 ./hlds_linux -netreplay "$result/session.rec" -netsends "$sends" "${server_args[@]}" \
	< <(sleep infinity) > "$log" 2>&1 &
pid=$!

# An engine without -netreplay support would run as a normal server forever
for _ in $(seq 60); do
	grep -q "^Net replay: replaying" "$log" && break
	kill -0 "$pid" 2> /dev/null || break
	sleep 1
done
if ! grep -q "^Net replay: replaying" "$log"; then
	kill "$pid" 2> /dev/null || true
	echo "the engine did not start replaying (no -netreplay support, or a recording in another format?), see $log" >&2
	exit 1
fi

wait "$pid" || true
