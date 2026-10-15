#!/bin/bash
# Builds the engine and the other binaries from the working tree in a Debian 11
# container with the 32-bit toolchain, so that the host needs nothing but Docker.
# The binaries are 32-bit (engine_i486.so): a 64-bit host without g++-multilib
# cannot build them with ./build.sh.
#
# usage: ./build-docker.sh
#
# Output: build/ (engine: build/rehlds/engine_i486.so). The Docker image
# (tools/bench/docker) is built on the first run.
set -euo pipefail
. "$(dirname "$0")/tools/bench/lib/common.sh"

mkdir -p "$BENCH_DIR" "$STEAM_HOME"
if ! docker image inspect "$BENCH_IMAGE" > /dev/null 2>&1; then
	log "building Docker image $BENCH_IMAGE"
	docker build -q -t "$BENCH_IMAGE" "$BENCH_ROOT/docker" > /dev/null
fi

log "building in $REPO_ROOT/build"
in_container "$BENCH_ROOT/lib/cmake_build.sh" "$REPO_ROOT" "$REPO_ROOT/build"
log "engine: $REPO_ROOT/build/rehlds/engine_i486.so"
