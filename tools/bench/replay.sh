#!/bin/bash
# Replays a recorded benchmark session with an engine build and compares the
# packets it sends with the recorded ones (MT-004).
#
# usage: replay.sh <result dir> [engine label]
#   result dir    a run recorded with BENCH_RECORD=1 (tools/bench/run.sh)
#   engine label  build stored by build.sh (default: latest build)
#
# Example:
#   BENCH_RECORD=1 tools/bench/run.sh clients8 baseline
#   tools/bench/replay.sh .bench/results/<that run> baseline   # must match the live run
#   tools/bench/replay.sh .bench/results/<that run> mychange   # differences = output changes
set -euo pipefail
. "$(dirname "$0")/lib/common.sh"

result=$(cd "${1:?usage: replay.sh <result dir> [engine label]}" && pwd)
label=${2:-}

[ -f "$result/session.rec" ] || die "$result has no recording (run it with BENCH_RECORD=1)"

if [ -z "$label" ]; then
	label=$(ls -t "$BENCH_DIR/engines" 2>/dev/null | head -1)
	[ -n "$label" ] || die "no engine build, run tools/bench/build.sh first"
fi
engine=$BENCH_DIR/engines/$label/engine_i486.so
[ -f "$engine" ] || die "no engine build named '$label'"

cp "$engine" "$HLDS_DIR/engine_i486.so"
sends=$result/replay-$label.sends
log=$result/replay-$label.log

log "replaying $(basename "$result") with engine '$label'"
in_container "$BENCH_ROOT/lib/replay_server.sh" "$HLDS_DIR" "$result" "$sends" "$log"
grep "^Net replay" "$log" || true

python3 "$BENCH_ROOT/replay_compare.py" "$result/live.sends" "$sends"
