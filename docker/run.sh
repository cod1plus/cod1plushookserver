#!/bin/sh
# Local CoD1 dedicated server in Docker, with the freshly built cod1plus.so.
#
#   sh docker/run.sh                 -> mp_harbor, 28965/udp
#   sh docker/run.sh mp_carentan     -> another map
#   sh docker/run.sh mp_harbor sync  -> re-sync the server content from the host folder
#
# Connect from the Windows client:  /connect 127.0.0.1:28965
#
# env:  COD1_SERVER_DIR  host folder with cod_lnxded, main/ and the mod dir (copied once
#                        into a Docker volume, see below)
#       COD1PLUS_SO      module to preload (default: build/cod1plus.so of this repo)
#       PORT             UDP port published on the host (default 28965)
#
# NO module environment variable is passed, on purpose: the validated configuration
# (pose_sync on, yaw forcing on, lean const 7.5, perbone off) is compiled into
# cod1plus.so. Adding one here would test something other than production. The log must
# print "installed (yaw=1 engine_lateral=7.50" by itself.
set -eu

MAP="${1:-mp_harbor}"
# 28965 and not 28960: the CoD CLIENT already holds 28960/udp on Windows (its default
# net_port), so Docker cannot publish it. The container would start anyway, without the
# mapping, and the game would sit on "awaiting connection" without a word.
PORT="${PORT:-28965}"
MODE="${2:-}"

# optional, git-ignored: export COD1_SERVER_DIR=... (see scripts/local.env.example)
[ -f "$(dirname "$0")/../scripts/local.env" ] && . "$(dirname "$0")/../scripts/local.env"
SERVER_DIR="${COD1_SERVER_DIR:-$HOME/cod1server}"
SO_PATH="${COD1PLUS_SO:-$(cd "$(dirname "$0")/.." && pwd)/build/cod1plus.so}"
IMAGE=cod1-test
VOLUME=cod1data

[ -f "$SO_PATH" ] || { echo "module not found: $SO_PATH (run sh scripts/build.sh first)" >&2; exit 1; }

export MSYS_NO_PATHCONV=1
export MSYS2_ARG_CONV_EXCL='*'

docker build -q -t "$IMAGE" "$(dirname "$0")" >/dev/null

# THE CONTENT LIVES IN A VOLUME, NOT IN A BIND MOUNT.
# Mounted straight from Windows, the engine listed the directories and found
# "0 files in pk3 files": the Docker Desktop share driver does not fill in the entry
# type the way the 2004 code expects. Copied once into a native volume, everything
# behaves as on the VPS.
if ! docker volume inspect "$VOLUME" >/dev/null 2>&1 || [ "$MODE" = "sync" ]; then
    echo "Syncing the server content into the volume $VOLUME (1.5 GB, once)..."
    docker volume create "$VOLUME" >/dev/null
    docker run --rm -v "${SERVER_DIR}:/src:ro" -v "${VOLUME}:/dst" "$IMAGE" \
        cp -a /src/. /dst/
fi

# The .so is mounted separately and read-only: the one lying in the server folder is
# whatever was deployed last, we want the one just compiled.
docker rm -f cod1test >/dev/null 2>&1 || true
exec docker run --rm -it --name cod1test \
  -p ${PORT}:${PORT}/udp \
  -v "${VOLUME}:/server" \
  -v "${SO_PATH}:/opt/cod1plus.so:ro" \
  -w /server \
  -e LD_PRELOAD=/opt/cod1plus.so \
  "$IMAGE" \
  ./cod_lnxded \
    +set dedicated 2 \
    +set fs_homepath /server \
    +set fs_basepath /server \
    +set fs_game __rPAMv115b5 \
    +set sv_punkbuster 0 \
    +set net_ip 0.0.0.0 \
    +set net_port "$PORT" \
    +set sv_maxclients 8 \
    +set g_gametype sd \
    +map "$MAP"
