# Shared settings for the benchmark scripts. Sourced, not executed.

BENCH_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
REPO_ROOT=$(cd "$BENCH_ROOT/../.." && pwd)

# Everything downloaded or generated lives here (HLDS install, builds, results)
BENCH_DIR=${BENCH_DIR:-$REPO_ROOT/.bench}

BENCH_IMAGE=${BENCH_IMAGE:-rehlds-bench:deb11}

# ReGameDLL_CS provides the bots used by the scenarios; pinned for reproducibility
REGAMEDLL_REPO=https://github.com/rehlds/ReGameDLL_CS.git
REGAMEDLL_COMMIT=${REGAMEDLL_COMMIT:-4a50c42}

# Plugin stack: the field validation server (tools/staging) and the benchmark
# scenarios that load AMX Mod X plugins
METAMOD_RELEASE=1.3.0.149
REAPI_RELEASE=5.29.0.358
AMXX_RELEASE=1.9.0.5303

HLDS_DIR=$BENCH_DIR/hlds

# The fork's optimisations are on by default: these arguments turn them all off, for
# runs of the original engine behaviour
ORIGINAL_ENGINE_ARGS=(-nonetthread -nonetsend -sys_threads 0 +sys_scheduler 0 +sv_rehlds_snapshot_spread 0
	+sv_parallel_send 0 +sv_a2s_cache 0 +sv_log_async 0 +sv_steam_on_demand 0 +sv_client_lookup_cache 0)
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
		-e BENCH_RECORD="${BENCH_RECORD:-0}" \
		-e BENCH_NO_ASLR="${BENCH_NO_ASLR:-}" \
		-e BENCH_HELGRIND="${BENCH_HELGRIND:-OFF}" \
		-v "$BENCH_DIR:$BENCH_DIR" \
		-v "$REPO_ROOT:$REPO_ROOT" \
		${BENCH_DOCKER_ARGS:-} \
		"$BENCH_IMAGE" "$@"
}
