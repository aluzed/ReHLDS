#!/bin/bash
# Builds the engine from the current working tree and stores it under a label,
# so that several builds can be benchmarked against each other.
#
# usage: build.sh [label]
#   label defaults to the short commit hash, suffixed with -dirty when the
#   working tree has uncommitted changes.
set -euo pipefail
. "$(dirname "$0")/lib/common.sh"

label=${1:-}
if [ -z "$label" ]; then
	label=$(git -C "$REPO_ROOT" rev-parse --short HEAD)
	if [ -n "$(git -C "$REPO_ROOT" status --porcelain -- rehlds)" ]; then
		label=$label-dirty
	fi
fi

build=$BENCH_DIR/build/rehlds
engines=$BENCH_DIR/engines/$label

log "building engine '$label'"
in_container "$BENCH_ROOT/lib/cmake_build.sh" "$REPO_ROOT" "$build"

mkdir -p "$engines"
cp "$build/rehlds/engine_i486.so" "$engines/engine_i486.so"
git -C "$REPO_ROOT" log -1 --format='%H %s' > "$engines/commit.txt"
log "engine stored in $engines"
echo "$label"
