#!/bin/bash
# Checks that an engine build sends exactly the same packets as a reference build
# (MT-004): records a session with the reference build, checks that the reference
# replays it identically (the session is deterministic), then replays it with the
# new build and compares what both sent, per destination.
#
# usage: regression.sh <reference label> <new label> [scenario]
#   labels    builds stored by build.sh; both need -netreplay support
#   scenario  default "replay" (load clients and HLTV proxies, no bots)
#
# Example:
#   git stash; tools/bench/build.sh before; git stash pop
#   tools/bench/build.sh after
#   tools/bench/regression.sh before after
#
# Exit code 0 when the new build's output is byte-identical.
set -euo pipefail
. "$(dirname "$0")/lib/common.sh"

reference=${1:?usage: regression.sh <reference label> <new label> [scenario]}
candidate=${2:?usage: regression.sh <reference label> <new label> [scenario]}
scenario=${3:-replay}

BENCH_RECORD=1 "$BENCH_ROOT/run.sh" "$scenario" "$reference" > /dev/null
result=$(ls -td "$BENCH_DIR"/results/*-"$scenario"-"$reference" | head -1)
log "recorded $(basename "$result")"

if ! "$BENCH_ROOT/replay.sh" "$result" "$reference"; then
	die "the reference build does not replay its own session identically: the session is not deterministic, nothing can be concluded"
fi

"$BENCH_ROOT/replay.sh" "$result" "$candidate"
