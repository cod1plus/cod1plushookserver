#!/bin/bash
# reloadtest.sh - prove every game-module patch survives a MAP CHANGE (the module is
# dlclose'd/dlopen'd, usually at the same address). Boots the real server, changes map
# through the tty console after 25 s, and counts each module's "installed" line.
set -u
SRC="/mnt/c/Users/bitpo/OneDrive/Bureau/cod1 competitive server/live/matchserver1"
SO="${1:-/mnt/c/Users/bitpo/OneDrive/Bureau/cod1plushookserver/build/cod1plus.so}"
DST="$HOME/reloadtest"; pkill -f cod_lnxded 2>/dev/null; sleep 1
rm -rf "$DST"; mkdir -p "$DST"; cp -r "$SRC"/. "$DST"/ 2>/dev/null; cd "$DST" || exit 1
cp "$SO" ./cod1plus.so; chmod +x cod_lnxded cod1plus.so
( sleep 25; echo "map mp_carentan"; sleep 25; echo "map mp_harbor"; sleep 20; echo "quit" ) | \
  env LD_PRELOAD=./cod1plus.so ./cod_lnxded +set dedicated 2 +set fs_homepath "$DST" \
  +set fs_game __rPAMv115b5 +set sv_punkbuster 0 +set net_ip 127.0.0.1 +set net_port 28997 \
  +set logfile 0 +set g_gametype dm +map mp_harbor > reload.log 2>&1
echo "=== map loads: $(grep -ac '^Server: ' reload.log)   game module loads: $(grep -ac 'Sys_LoadDll(game) found' reload.log) ==="
grep -a 'Sys_LoadDll(game) found\|^Server: ' reload.log | cut -c1-70
for m in "\[pose_sync\] installed" "\[pose_sync\] lean const" "\[swing_sync\] installed" "\[anim_clamp\] BG_Get" "\[antilag\] installed" "\[competitive\] installed"; do
  printf "  %-32s %s\n" "$m" "$(grep -ac "$m" reload.log)"
done
grep -a 'mismatch\|failed\|cannot' reload.log | head -5
