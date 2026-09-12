#!/bin/sh
# POSIX sh compatible (works with `sh build.sh` or `bash build.sh`).
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"

mkdir -p "${BUILD_DIR}"

# PORTABILITY TRAP - do not remove CFLAGS_COMPAT.
# glibc 2.38 (Ubuntu 23.10+, incl. WSL 24.04) redirects strtol / strtoul / sscanf / fscanf
# to __isoc23_* symbols versioned GLIBC_2.38. A module built on such a host compiles and
# links cleanly, then dies at LD_PRELOAD time on the game VPS with a symbol lookup error,
# and the server comes up with no hooks at all. src/glibc_compat.h binds those four calls
# to the classic unversioned symbols; it has to be first in every translation unit, hence
# -include. See the header for why the documented -D switch does not work.
CFLAGS_COMPAT="-include ${ROOT_DIR}/src/glibc_compat.h"

# src/shared/ is compiled into the client's mss32.dll from a byte-identical copy in the
# cod1reloaded repo. If you change it in one repo, copy it to the other - both sides print
# lc_banner() at install so a drift shows up as two different lines in the two logs.
CC="${CC:-gcc}"
${CC} -m32 -shared -fPIC -O2 -Wall -Wextra ${CFLAGS_COMPAT} \
  "${ROOT_DIR}/src/cod1plus.c" \
  "${ROOT_DIR}/src/cod1reloaded.c" \
  "${ROOT_DIR}/src/shared/lean_controllers.c" \
  "${ROOT_DIR}/src/lean_hitbox.c" \
  "${ROOT_DIR}/src/perbone_hit.c" \
  "${ROOT_DIR}/src/pose_sync.c" \
  "${ROOT_DIR}/src/swing_sync.c" \
  "${ROOT_DIR}/src/hitbox_draw.c" \
  "${ROOT_DIR}/src/antilag.c" \
  "${ROOT_DIR}/src/anim_clamp.c" \
  "${ROOT_DIR}/src/competitive_sv.c" \
  "${ROOT_DIR}/src/gear_force.c" \
  "${ROOT_DIR}/src/cheat_gate.c" \
  "${ROOT_DIR}/src/hooks.c" \
  -o "${BUILD_DIR}/cod1plus.so" \
  -pthread -lm -ldl

# Guard: refuse to ship a module the VPS cannot load. MAX_GLIBC is the highest version the
# production cod1plus.so has ever required; anything above it is a build-host artefact and
# would otherwise only be discovered by the server failing to start.
MAX_GLIBC="2.34"
HIGHEST="$(readelf -V "${BUILD_DIR}/cod1plus.so" 2>/dev/null |
           grep -o 'GLIBC_[0-9.]*' | sed 's/GLIBC_//' | sort -uV | tail -1)"
if [ -n "${HIGHEST}" ]; then
  NEWEST="$({ echo "${MAX_GLIBC}"; echo "${HIGHEST}"; } | sort -V | tail -1)"
  if [ "${NEWEST}" != "${MAX_GLIBC}" ]; then
    echo "BUILD REJECTED: module needs GLIBC_${HIGHEST}, the game VPS provides at most GLIBC_${MAX_GLIBC}." >&2
    echo "  undefined symbols pulling it in:" >&2
    readelf -Ws "${BUILD_DIR}/cod1plus.so" | grep UND | grep "GLIBC_${HIGHEST}" >&2
    rm -f "${BUILD_DIR}/cod1plus.so"
    exit 1
  fi
fi
echo "glibc floor OK: highest requirement GLIBC_${HIGHEST} (limit GLIBC_${MAX_GLIBC})"

echo "OK: built ${BUILD_DIR}/cod1plus.so"
echo "Load with: LD_PRELOAD=./cod1plus.so ./cod_lnxded ..."
