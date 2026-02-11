#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"

mkdir -p "${BUILD_DIR}"

OS_NAME="$(uname -s)"
if [[ "${OS_NAME}" == "Darwin" ]]; then
  echo "Compilation 32-bit non supportée nativement sur macOS."
  echo "Utilise un Linux 32-bit ou un toolchain cross-compilé."
  exit 1
fi

CC="${CC:-gcc}"
${CC} -m32 -shared -fPIC -O2 -Wall -Wextra \
  -I"${ROOT_DIR}/src" \
  "${ROOT_DIR}/src/hooks.c" \
  "${ROOT_DIR}/src/cod1plus.c" \
  -o "${BUILD_DIR}/cod1plus.so" \
  -ldl -pthread
