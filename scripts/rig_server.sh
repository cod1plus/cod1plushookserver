#!/bin/bash
# rig_server.sh - local measurement server: real cod_lnxded + the built module, controller
# dump ON, gates relaxed, cheats ON, DM on the requested map. Runs until killed.
set -u
SRC="/mnt/c/Users/bitpo/OneDrive/Bureau/cod1 competitive server/live/matchserver1"
SO="/mnt/c/Users/bitpo/OneDrive/Bureau/cod1plushookserver/build/cod1plus.so"
DST="$HOME/rig"; MAP="${1:-mp_carentan}"
pkill -f cod_lnxded 2>/dev/null; sleep 1
rm -rf "$DST"; mkdir -p "$DST"; cp -r "$SRC"/. "$DST"/ 2>/dev/null
# the mirror is a PASSWORDED match server: open it for the local rig only
for c in "$DST/__rPAMv115b5/config_mp_server.cfg" "$DST/main/config_mp_server.cfg"; do
  sed -i -E 's/^(seta? +(g_password|sv_privatePassword) +)"[^"]*"/""/' "$c" 2>/dev/null
done
cd "$DST" || exit 1
cp "$SO" ./cod1plus.so; chmod +x cod_lnxded cod1plus.so
rm -f __rPAMv115b5/games_mp.log __rPAMv115b5/console_mp_server.log
exec env LD_PRELOAD=./cod1plus.so \
  COD1RELOADED_CTRL_DUMP=400 COD1RELOADED_ALLOW_UNVERSIONED=1 COD1RELOADED_MIN_BUILD_AUTO=0 \
  ./cod_lnxded +set dedicated 2 +set fs_homepath "$DST" +set fs_game __rPAMv115b5 \
  +set sv_punkbuster 0 +set net_ip 0.0.0.0 +set net_port 28999 +set logfile 2 \
  +set sv_cheats 1 +set sv_pure 0 +set g_gametype dm +set g_log games_mp.log +set g_logsync 1 \
  +set sv_maxclients 8 +set scr_dm_timelimit 0 +set scr_dm_scorelimit 0 +map "$MAP" > "$DST/rig.log" 2>&1
