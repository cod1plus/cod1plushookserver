#!/bin/sh
# POSIX sh compatible (works with `sh build.sh` or `bash build.sh`).
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"

mkdir -p "${BUILD_DIR}"

CC="${CC:-gcc}"
${CC} -m32 -shared -fPIC -O2 -Wall -Wextra \
  "${ROOT_DIR}/src/cod1plus.c" \
  "${ROOT_DIR}/src/cod1reloaded.c" \
  "${ROOT_DIR}/src/lean_hitbox.c" \
  "${ROOT_DIR}/src/perbone_hit.c" \
  "${ROOT_DIR}/src/antilag.c" \
  "${ROOT_DIR}/src/anim_clamp.c" \
  "${ROOT_DIR}/src/competitive_sv.c" \
  "${ROOT_DIR}/src/cheat_gate.c" \
  "${ROOT_DIR}/archive/hooks.c" \
  -o "${BUILD_DIR}/cod1plus.so" \
  -pthread -lm

echo "OK: built ${BUILD_DIR}/cod1plus.so"
echo "Load with: LD_PRELOAD=./cod1plus.so ./cod_lnxded ..."
