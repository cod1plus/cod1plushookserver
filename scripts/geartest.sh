#!/bin/bash
# geartest.sh - prove g_useGear actually governs the attached gear, by booting the real
# server twice (1 then 0) and diffing which xmodels it loads. geardiff.sh reads its output.
#   env:   COD1_SERVER_DIR  server folder to copy (never written to; runs on a copy)
#          COD1PLUS_SO      module under test (default: build/cod1plus.so of this repo)
set -u
# optional, git-ignored: export COD1_SERVER_DIR=... (see scripts/local.env.example)
[ -f "$(dirname "$0")/../scripts/local.env" ] && . "$(dirname "$0")/../scripts/local.env"
SRC="${COD1_SERVER_DIR:-$HOME/cod1server}"
SO="${COD1PLUS_SO:-$(cd "$(dirname "$0")/.." && pwd)/build/cod1plus.so}"
DST="$HOME/geartest"
[ -f "$SO" ] || { echo "module not found: $SO (run sh scripts/build.sh first)" >&2; exit 1; }
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
  echo "  log lines           : $(wc -l < gear$v.log)"
  echo "  'gear_' mentions    : $(grep -aic 'gear_' gear$v.log)"
  echo "  gear xmodels named  : $(grep -aoi 'xmodel/gear_[a-z0-9_]*' gear$v.log | sort -u | wc -l)"
  grep -aoi 'xmodel/gear_[a-z0-9_]*' gear$v.log | sort -u | head -8 | sed 's/^/     /'
done
echo "=== difference ==="
diff <(grep -aoi 'xmodel/gear_[a-z0-9_]*' gear1.log | sort -u) \
     <(grep -aoi 'xmodel/gear_[a-z0-9_]*' gear0.log | sort -u) | head -30
