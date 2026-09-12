#!/bin/bash
# Controlled A/B: does the NEW module behave like the one that is running in production?
# Loads both into a tiny 32-bit host process (no game needed) and compares the output.
#   env:   COD1PLUS_SO       new module   (default: build/cod1plus.so of this repo)
#          COD1PLUS_SO_LIVE  live module  (default: the one deployed in COD1_SERVER_DIR)
#          COD1_SERVER_DIR   server folder holding the deployed cod1plus.so
set -u
# optional, git-ignored: export COD1_SERVER_DIR=... (see scripts/local.env.example)
[ -f "$(dirname "$0")/../scripts/local.env" ] && . "$(dirname "$0")/../scripts/local.env"
SERVER_DIR="${COD1_SERVER_DIR:-$HOME/cod1server}"
NEW="${COD1PLUS_SO:-$(cd "$(dirname "$0")/.." && pwd)/build/cod1plus.so}"
LIVE="${COD1PLUS_SO_LIVE:-$SERVER_DIR/cod1plus.so}"
D="$HOME/lt"; rm -rf "$D"; mkdir -p "$D"; cd "$D" || exit 1
cp "$NEW" new.so || exit 1; cp "$LIVE" live.so || exit 1
printf '#include <stdio.h>\nint main(void){ puts("HOST PROCESS ALIVE"); return 0; }\n' > t.c
gcc -m32 -o t t.c || exit 1
ulimit -c 0
for m in live new; do
  echo "================ $m.so ================"
  LD_PRELOAD=./$m.so ./t > o.txt 2> e.txt
  rc=$?
  echo "rc=$rc  (139 = SIGSEGV)"
  echo "--- stdout ---"; cat o.txt
  echo "--- stderr ---"; cat e.txt
done
echo "================ where does it die? (new.so under gdb) ================"
if command -v gdb >/dev/null 2>&1; then
  gdb -batch -ex run -ex bt -ex quit --args env LD_PRELOAD=./new.so ./t 2>&1 | tail -25
else
  echo "gdb not installed"
fi
