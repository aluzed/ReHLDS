#!/bin/bash
# One-time setup of the benchmark environment:
#   - builds the Docker image (Debian 11 + i386 toolchain, same as CI),
#   - installs the steam_legacy HLDS (cstrike) with steamcmd,
#   - builds ReGameDLL_CS from source (provides bots) and installs it,
#   - generates the navigation mesh for the benchmark maps,
#   - installs Metamod-R and AMX Mod X with the benchmark plugins (tools/bench/plugins),
#     loaded only by the scenarios that set METAMOD=1 (-dll), never through liblist.gam.
#
# Everything goes to $BENCH_DIR (default: <repo>/.bench).
set -euo pipefail
. "$(dirname "$0")/lib/common.sh"

mkdir -p "$BENCH_DIR" "$STEAM_HOME"

log "building Docker image $BENCH_IMAGE"
docker build -q -t "$BENCH_IMAGE" "$BENCH_ROOT/docker" > /dev/null

# HLDS
if [ ! -x "$HLDS_DIR/hlds_linux" ]; then
	steamcmd=$BENCH_DIR/steamcmd
	if [ ! -x "$steamcmd/steamcmd.sh" ]; then
		log "downloading steamcmd"
		mkdir -p "$steamcmd"
		curl -sSL https://steamcdn-a.akamaihd.net/client/installer/steamcmd_linux.tar.gz | tar xz -C "$steamcmd"
	fi

	log "installing HLDS (app 90, steam_legacy, cstrike) - this downloads ~800 MB"
	in_container "$BENCH_ROOT/lib/install_hlds.sh" "$steamcmd" "$HLDS_DIR"

	# Keep the stock engine to restore it or compare against it
	cp "$HLDS_DIR/engine_i486.so" "$HLDS_DIR/engine_i486.so.valve"
fi

# The Steam API looks for steamclient.so in ~/.steam/sdk32
mkdir -p "$STEAM_HOME/.steam/sdk32"
cp "$HLDS_DIR/steamclient.so" "$STEAM_HOME/.steam/sdk32/steamclient.so"

# ReGameDLL_CS
regamedll_src=$BENCH_DIR/src/ReGameDLL_CS
regamedll_build=$BENCH_DIR/build/regamedll-$REGAMEDLL_COMMIT
if [ ! -f "$regamedll_build/regamedll/cs.so" ]; then
	if [ ! -d "$regamedll_src/.git" ]; then
		log "cloning ReGameDLL_CS"
		git clone -q "$REGAMEDLL_REPO" "$regamedll_src"
	fi
	git -C "$regamedll_src" checkout -q "$REGAMEDLL_COMMIT"

	log "building ReGameDLL_CS $REGAMEDLL_COMMIT"
	in_container "$BENCH_ROOT/lib/cmake_build.sh" "$regamedll_src" "$regamedll_build"
fi

log "installing ReGameDLL_CS and bot profiles"
cp "$regamedll_build/regamedll/cs.so" "$HLDS_DIR/cstrike/dlls/cs.so"
python3 -c 'import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])' \
	"$regamedll_src/regamedll/extra/zBot/bot_profiles.zip" "$HLDS_DIR"

# ReGameDLL reads bot_enable once, at game DLL init, from game_init.cfg
game_init=$HLDS_DIR/cstrike/game_init.cfg
grep -qs '^bot_enable 1' "$game_init" || echo 'bot_enable 1' >> "$game_init"

# Navigation meshes used by the scenarios
for map in de_dust2; do
	if [ ! -f "$HLDS_DIR/cstrike/maps/$map.nav" ]; then
		log "generating navigation mesh for $map (takes about a minute)"
		in_container "$BENCH_ROOT/lib/generate_nav.sh" "$HLDS_DIR" "$map"
	fi
done

# Metamod-R and AMX Mod X, with only the benchmark plugins
game=$HLDS_DIR/cstrike
downloads=$BENCH_DIR/downloads
if [ ! -f "$game/addons/metamod/metamod_i386.so" ]; then
	log "installing Metamod-R $METAMOD_RELEASE and AMX Mod X $AMXX_RELEASE"
	mkdir -p "$downloads"/{metamod,amxx}
	gh release download "$METAMOD_RELEASE" -R rehlds/Metamod-R -p 'metamod-bin-*.zip' -D "$downloads/metamod" --clobber
	gh release download "$AMXX_RELEASE" -R alliedmodders/amxmodx -p '*base-linux.tar.gz' -D "$downloads/amxx" --clobber
	(cd "$downloads/metamod" && unzip -q -o ./*.zip -d x)
	mkdir -p "$game/addons/metamod"
	cp "$downloads/metamod/x/addons/metamod/metamod_i386.so" "$downloads/metamod/x/addons/metamod/config.ini" "$game/addons/metamod/"
	tar xzf "$downloads"/amxx/*base-linux.tar.gz -C "$game"
	echo 'linux addons/amxmodx/dlls/amxmodx_mm_i386.so' > "$game/addons/metamod/plugins.ini"
fi

amxx=$game/addons/amxmodx
: > "$amxx/configs/plugins.ini"
for source in "$BENCH_ROOT"/plugins/*.sma; do
	name=$(basename "$source" .sma)
	log "compiling $name"
	in_container bash -c 'cd "$1" && ./amxxpc "$2" -o"$3" > /dev/null' _ \
		"$amxx/scripting" "$source" "$amxx/plugins/$name.amxx"
	echo "$name.amxx" >> "$amxx/configs/plugins.ini"
done

log "setup done in $BENCH_DIR"
