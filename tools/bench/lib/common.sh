# Shared settings for the benchmark scripts. Sourced, not executed.

BENCH_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
REPO_ROOT=$(cd "$BENCH_ROOT/../.." && pwd)

# Everything downloaded or generated lives here (HLDS install, builds, results)
BENCH_DIR=${BENCH_DIR:-$REPO_ROOT/.bench}

BENCH_IMAGE=${BENCH_IMAGE:-rehlds-bench:deb11}

# ReGameDLL_CS provides the bots used by the scenarios; pinned for reproducibility
REGAMEDLL_REPO=https://github.com/rehlds/ReGameDLL_CS.git
REGAMEDLL_COMMIT=${REGAMEDLL_COMMIT:-4a50c42}

HLDS_DIR=$BENCH_DIR/hlds
STEAM_HOME=$BENCH_DIR/home

log() {
	echo "[bench] $*" >&2
}

die() {
	echo "[bench] error: $*" >&2
	exit 1
}

# Runs a command inside the benchmark container as the current user.
# BENCH_DIR and the repository are mounted at the same paths as on the host.
in_container() {
	docker run --rm -i \
		-u "$(id -u):$(id -g)" \
		-e HOME="$STEAM_HOME" \
		-e BENCH_SERVER_ARGS="${BENCH_SERVER_ARGS:-}" \
		-v "$BENCH_DIR:$BENCH_DIR" \
		-v "$REPO_ROOT:$REPO_ROOT" \
		${BENCH_DOCKER_ARGS:-} \
		"$BENCH_IMAGE" "$@"
}
