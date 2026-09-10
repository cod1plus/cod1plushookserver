#!/bin/bash
# smoketest.sh - run a real cod_lnxded with the freshly built cod1plus.so preloaded, long
# enough for every module to report in. Catches the two failures a compile cannot see: a
# symbol the build host has and the target does not, and a byte pattern that no longer
# matches the shipped game.mp.i386.so.
#   usage: bash smoketest.sh [seconds] [path/to/cod1plus.so]
set -u
SRC="/mnt/c/Users/bitpo/OneDrive/Bureau/cod1 competitive server/live/matchserver1"
SECS="${1:-25}"
SO="${2:-/mnt/c/Users/bitpo/OneDrive/Bureau/cod1plushookserver/build/cod1plus.so}"
DST="$HOME/cod1test"

rm -rf "$DST"; mkdir -p "$DST"
cp -r "$SRC"/. "$DST"/ 2>/dev/null
cd "$DST" || exit 1
cp "$SO" ./cod1plus.so
chmod +x cod_lnxded cod1plus.so 2>/dev/null

echo "=== module under test: $SO ==="
md5sum ./cod1plus.so
echo "=== launching for ${SECS}s ==="
# LD_PRELOAD must be set on the GAME, not on `timeout` (a 64-bit binary would just
# refuse the 32-bit module and drop it).
timeout "${SECS}" env LD_PRELOAD=./cod1plus.so ./cod_lnxded \
  +set dedicated 0 \
  +set fs_homepath "$DST" \
  +set fs_game __rPAMv115b5 \
  +set sv_punkbuster 0 \
  +set net_ip 127.0.0.1 \
  +set net_port 28999 \
  +set logfile 0 \
  +set g_gametype sd \
  +map mp_harbor > run.log 2>&1
echo "exit=$? (124 = still running when the timer fired, which is the good case)"
echo
echo "=== module reports ==="
grep -aiE "cod1plus|cod1reloaded|swing_sync|pose_sync|lean|antilag|anim_clamp|cheat_gate|competitive|hitbox|preload|symbol" run.log | head -40
echo
echo "=== errors ==="
grep -aiE "error|fatal|signal|segmentation|not installing|mismatch" run.log | head -20
echo
echo "=== tail ==="
tail -12 run.log
