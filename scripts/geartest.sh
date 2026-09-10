#!/bin/bash
# geartest.sh - prove g_useGear actually governs the attached gear, by booting the real
# server twice (1 then 0) and diffing which xmodels it loads.
set -u
SRC="/mnt/c/Users/bitpo/OneDrive/Bureau/cod1 competitive server/live/matchserver1"
SO="/mnt/c/Users/bitpo/OneDrive/Bureau/cod1plushookserver/build/cod1plus.so"
DST="$HOME/geartest"
rm -rf "$DST"; mkdir -p "$DST"; cp -r "$SRC"/. "$DST"/ 2>/dev/null
cd "$DST" || exit 1
cp "$SO" ./cod1plus.so; chmod +x cod_lnxded cod1plus.so 2>/dev/null

run() {
  timeout 30 env LD_PRELOAD=./cod1plus.so ./cod_lnxded \
    +set dedicated 0 +set fs_homepath "$DST" +set fs_game __rPAMv115b5 \
    +set sv_punkbuster 0 +set net_ip 127.0.0.1 +set net_port 2900$2 \
    +set logfile 0 +set developer 1 +set g_useGear "$1" \
    +set g_gametype sd +map mp_carentan > "gear$1.log" 2>&1
}
echo "--- boot with g_useGear 1 ---"; run 1 1
echo "--- boot with g_useGear 0 ---"; run 0 2
for v in 1 0; do
  echo "=== g_useGear $v ==="
  echo "  lignes de log       : $(wc -l < gear$v.log)"
  echo "  mentions 'gear_'    : $(grep -aic 'gear_' gear$v.log)"
  echo "  modeles gear cites  : $(grep -aoi 'xmodel/gear_[a-z0-9_]*' gear$v.log | sort -u | wc -l)"
  grep -aoi 'xmodel/gear_[a-z0-9_]*' gear$v.log | sort -u | head -8 | sed 's/^/     /'
done
echo "=== difference ==="
diff <(grep -aoi 'xmodel/gear_[a-z0-9_]*' gear1.log | sort -u) \
     <(grep -aoi 'xmodel/gear_[a-z0-9_]*' gear0.log | sort -u) | head -30
