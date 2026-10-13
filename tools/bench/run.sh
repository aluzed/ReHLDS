#!/bin/bash
# Runs a benchmark scenario against a stored engine build and writes a report.
#
# usage: run.sh <scenario> [engine label] [-- extra docker args]
#   scenario      name of a file in tools/bench/scenarios (without .env)
#   engine label  build stored by build.sh (default: latest build)
#
# Example:
#   tools/bench/build.sh baseline
#   tools/bench/run.sh bots20-hltv8 baseline
#   BENCH_DOCKER_ARGS="--cpuset-cpus=2,3" tools/bench/run.sh bots20-hltv8 baseline
#   BENCH_SERVER_ARGS="+sys_scheduler 1" tools/bench/run.sh idle baseline
set -euo pipefail
. "$(dirname "$0")/lib/common.sh"

scenario=${1:?usage: run.sh <scenario> [engine label]}
label=${2:-}

scenario_file=$BENCH_ROOT/scenarios/$scenario.env
[ -f "$scenario_file" ] || die "unknown scenario '$scenario' (see $BENCH_ROOT/scenarios)"
[ -x "$HLDS_DIR/hlds_linux" ] || die "HLDS not installed, run tools/bench/setup.sh first"

if [ -z "$label" ]; then
	label=$(ls -t "$BENCH_DIR/engines" 2>/dev/null | head -1)
	[ -n "$label" ] || die "no engine build, run tools/bench/build.sh first"
fi
engine=$BENCH_DIR/engines/$label/engine_i486.so
[ -f "$engine" ] || die "no engine build named '$label'"

out=$BENCH_DIR/results/$(date +%Y%m%d-%H%M%S)-$scenario-$label
mkdir -p "$out"

cp "$engine" "$HLDS_DIR/engine_i486.so"
cp "$scenario_file" "$out/scenario.env"
{
	echo "scenario=$scenario"
	echo "engine=$label"
	echo "engine_commit=$(cat "$BENCH_DIR/engines/$label/commit.txt" 2>/dev/null || echo unknown)"
	echo "cpu=$(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ *//')"
	echo "kernel=$(uname -r)"
	echo "governor=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo unknown)"
	echo "docker_args=${BENCH_DOCKER_ARGS:-}"
	echo "server_args=${BENCH_SERVER_ARGS:-}"
} > "$out/meta.txt"

log "running '$scenario' with engine '$label'"
in_container "$BENCH_ROOT/lib/run_server.sh" "$HLDS_DIR" "$scenario_file" "$out"

[ -s "$out/profile.csv" ] || die "no profile recorded, see $out/server.log"

python3 "$BENCH_ROOT/report.py" "$out" > "$out/report.md"
cat "$out/report.md"
log "results in $out"
