#!/bin/bash
# geardiff.sh - compare the two boots recorded by geartest.sh (g_useGear 1 vs 0).
cd "$HOME/geartest" || { echo "run scripts/geartest.sh first" >&2; exit 1; }
echo "=== diff of the two boots (g_useGear 1 vs 0) ==="
diff gear1.log gear0.log | head -25
echo
for v in 1 0; do
  echo "-- g_useGear $v"
  grep -ai "hunkusage\|hunk\|xmodel\|precache" "gear$v.log" | head -8
done
