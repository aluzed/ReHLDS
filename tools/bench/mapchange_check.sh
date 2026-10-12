#!/bin/bash
# Changes the map in a loop on a server with Metamod-R, AMX Mod X, bots, playing load
# clients and snapshots encoded on worker threads, and fails if the server crashes.
# Covers the encoding threads calling game DLL and Metamod code, which Valgrind cannot
# exercise (race_check.sh): it runs one thread at a time.
#
#   tools/bench/mapchange_check.sh <engine label> [cycles]
set -euo pipefail
. "$(dirname "$0")/lib/common.sh"

label=${1:?usage: mapchange_check.sh <engine label> [cycles]}
cycles=${2:-8}

engine=$BENCH_DIR/engines/$label/engine_i486.so
[ -f "$engine" ] || die "no engine build named '$label'"
[ -f "$HLDS_DIR/cstrike/addons/metamod/metamod_i386.so" ] || die "Metamod not installed, run tools/bench/setup.sh first"

out=$BENCH_DIR/results/$(date +%Y%m%d-%H%M%S)-mapchange-$label
mkdir -p "$out"
cp "$engine" "$HLDS_DIR/engine_i486.so"

log "changing the map $cycles times with engine '$label' (about $((cycles * 55 / 60 + 1)) minutes)"
status=0
in_container "$BENCH_ROOT/lib/mapchange_server.sh" "$HLDS_DIR" "$out" "$cycles" \
	-netthread -sys_threads 2 +sys_ticrate 1000 +sys_scheduler 1 +sv_rehlds_snapshot_spread 1 +sv_parallel_send 1 \
	|| status=$?

grep -A1 "^[0-9]* worker threads" "$out/server.log" | tail -2 || true
if [ "$status" = 0 ]; then
	log "no crash after $cycles map changes, results in $out"
else
	log "server crashed, see $out/server.log"
fi
exit "$status"
