#!/bin/bash
cd "$HOME/geartest" || exit 1
echo "=== diff des deux boots (g_useGear 1 vs 0) ==="
diff gear1.log gear0.log | head -25
echo
for v in 1 0; do
  echo "-- g_useGear $v"
  grep -ai "hunkusage\|hunk\|xmodel\|precache" "gear$v.log" | head -8
done
