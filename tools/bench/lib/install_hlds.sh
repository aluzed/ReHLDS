#!/bin/bash
# Installs the steam_legacy HLDS (app 90, cstrike) with steamcmd.
# Runs inside the benchmark container.
# usage: install_hlds.sh <steamcmd dir> <install dir>
set -euo pipefail

steamcmd=$1
install=$2

mkdir -p "$HOME"
cd "$steamcmd"

# app 90 often needs several passes before steamcmd reports it fully installed
for attempt in 1 2 3; do
	./steamcmd.sh +force_install_dir "$install" +login anonymous \
		+app_set_config 90 mod cstrike \
		+app_update 90 -beta steam_legacy validate +quit > "steamcmd-$attempt.log" 2>&1 || true

	if grep -q "Success! App '90' fully installed" "steamcmd-$attempt.log"; then
		exit 0
	fi
done

tail -20 "steamcmd-3.log"
exit 1
