#!/bin/bash
# Runs a HLDS install with this fork's engine and every option of the
# feature/multithreading branch on, including the ones that are off by default.
#
# usage: ./run.sh <hlds dir> [extra server args...]
#
# The engine built by ./build.sh (build/rehlds/engine_i486.so) is copied over the
# install's engine_i486.so first; the original is kept as engine_i486.so.orig.
#
# Environment:
#   GAME=cstrike  MAP=de_dust2  PORT=27015  MAXPLAYERS=32  TICRATE=1000
#   SYS_THREADS=auto     worker threads for sv_parallel_send (default build: 2)
#   SPIN_US=200          busy-wait before each tick and in the workers (burns CPU,
#                        only for cores dedicated to the server; 0 to sleep)
#   WAKE_ON_PACKET=1     read packets on arrival between ticks (MT-102)
#   PROFILE=1            record the frame profiler continuously
#   CPUS=                cores given to the server with taskset (e.g. 2-5)
set -euo pipefail

die() { echo "run.sh: $*" >&2; exit 1; }

[ $# -ge 1 ] || die "usage: ./run.sh <hlds dir> [extra server args...]"
HLDS_DIR=$(realpath "$1")
shift

ENGINE=$(dirname "$(realpath "$0")")/build/rehlds/engine_i486.so
[ -f "$ENGINE" ] || die "$ENGINE not found, run ./build.sh first"
[ -x "$HLDS_DIR/hlds_run" ] || die "no hlds_run in $HLDS_DIR"

if [ -f "$HLDS_DIR/engine_i486.so" ] && [ ! -f "$HLDS_DIR/engine_i486.so.orig" ]; then
	cp "$HLDS_DIR/engine_i486.so" "$HLDS_DIR/engine_i486.so.orig"
fi
cp "$ENGINE" "$HLDS_DIR/engine_i486.so"

SPIN_US=${SPIN_US:-200}
ARGS=(
	-game "${GAME:-cstrike}"
	-port "${PORT:-27015}"
	+maxplayers "${MAXPLAYERS:-32}"
	-sys_threads "${SYS_THREADS:-auto}"
	+sys_ticrate "${TICRATE:-1000}"
	# On by default, set explicitly so a server.cfg cannot leave one off.
	+sys_scheduler 1
	+sv_rehlds_snapshot_spread 1
	+net_thread_filter 1
	+sv_a2s_cache 1
	+sv_parallel_send 1
	+sv_log_async 1
	+sv_client_lookup_cache 1
	+sv_steam_on_demand 1
	# Off by default.
	+sys_spin_us "$SPIN_US"
	+sys_threads_spin_us "$SPIN_US"
	+sys_wake_on_packet "${WAKE_ON_PACKET:-1}"
	+sv_profile_enable "${PROFILE:-1}"
	"$@"
	+map "${MAP:-de_dust2}"
)

cd "$HLDS_DIR"
if [ -n "${CPUS:-}" ]; then
	exec taskset -c "$CPUS" ./hlds_run "${ARGS[@]}"
fi
exec ./hlds_run "${ARGS[@]}"
