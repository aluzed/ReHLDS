#!/bin/bash
# Starts the server once with bots so that ReGameDLL generates the navigation
# mesh of the map (maps/<map>.nav). Runs inside the benchmark container.
# usage: generate_nav.sh <hlds dir> <map>
set -euo pipefail

hlds=$1
map=$2
nav=$hlds/cstrike/maps/$map.nav

cd "$hlds"
export LD_LIBRARY_PATH=.

(
	sleep 5
	echo "bot_join_after_player 0"
	echo "bot_quota 1"
	# Wait for the analysis to write the file, then quit
	for _ in $(seq 1 600); do
		[ -f "$nav" ] && break
		sleep 1
	done
	sleep 2
	echo quit
) | ./hlds_linux -game cstrike -console -port 27015 +sv_lan 1 +maxplayers 32 +map "$map" > generate_nav.log 2>&1

[ -f "$nav" ] || { tail -20 generate_nav.log; exit 1; }
