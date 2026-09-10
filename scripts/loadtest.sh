#!/bin/bash
# Controlled A/B: does the NEW module behave like the one that is running in production?
set -u
NEW="/mnt/c/Users/bitpo/OneDrive/Bureau/cod1plushookserver/build/cod1plus.so"
LIVE="/mnt/c/Users/bitpo/OneDrive/Bureau/cod1 competitive server/live/matchserver1/cod1plus.so"
D="$HOME/lt"; rm -rf "$D"; mkdir -p "$D"; cd "$D" || exit 1
cp "$NEW" new.so; cp "$LIVE" live.so
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
