#!/bin/bash
# Runs the server under a Valgrind race detector (Helgrind by default) with the
# network thread, load clients, a query flood and filter list changes, then
# reports the data races found in engine code (MT-004).
#
# ThreadSanitizer needs a 64-bit build, which the engine does not have: Valgrind
# runs the real 32-bit server instead. Build the engine with the annotations that
# tell Valgrind about the lock-free code first:
#
#   BENCH_HELGRIND=ON tools/bench/build.sh helgrind
#   tools/bench/race_check.sh helgrind [seconds] [helgrind|drd]
#
# BENCH_SERVER_ARGS adds server arguments, e.g. BENCH_SERVER_ARGS=-netsend.
set -euo pipefail
. "$(dirname "$0")/lib/common.sh"

label=${1:?usage: race_check.sh <engine label> [seconds] [tool]}
secs=${2:-60}
tool=${3:-helgrind}

engine=$BENCH_DIR/engines/$label/engine_i486.so
[ -f "$engine" ] || die "no engine build named '$label'"
[ -x "$HLDS_DIR/hlds_linux" ] || die "HLDS not installed, run tools/bench/setup.sh first"

out=$BENCH_DIR/results/$(date +%Y%m%d-%H%M%S)-race-$tool-$label
mkdir -p "$out"
cp "$engine" "$HLDS_DIR/engine_i486.so"

log "running the server under $tool for ${secs}s with engine '$label' (several minutes)"
in_container timeout $((secs + 600)) "$BENCH_ROOT/lib/race_server.sh" "$HLDS_DIR" "$out" "$tool" "$secs"

grep -A7 "^Network thread" "$out/server.log" | head -8 || true
python3 "$BENCH_ROOT/race_report.py" "$out/$tool.log" | tee "$out/report.txt"
