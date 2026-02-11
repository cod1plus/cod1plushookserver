#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"

mkdir -p "${BUILD_DIR}"

CC="${CC:-gcc}"
${CC} -m32 -shared -fPIC -O2 -Wall -Wextra \
  "${ROOT_DIR}/src/cod1plus.c" \
  -o "${BUILD_DIR}/cod1plus.so" \
  -pthread

echo "✅ Built cod1plus.so successfully"
echo "Load with: LD_PRELOAD=./cod1plus.so ./cod_lnxded ..."
