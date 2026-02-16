#!/usr/bin/env bash
# inject_test.sh
# Simule la fin d'une manche S&D en injectant une ligne [STATS_EVENT] dans qconsole.log.
# Usage: bash inject_test.sh [round_number] [allies_score] [axis_score] [winner]
# Exemple: bash inject_test.sh 2 1 1 allies

ROUND=${1:-1}
AS=${2:-0}
XS=${3:-1}
WINNER=${4:-axis}
HT=${5:-0}
LOGFILE=${6:-/home/cod/qconsole.log}

EVENT="[STATS_EVENT]r=${ROUND},as=${AS},xs=${XS},rw=${WINNER},ht=${HT},bp=1,ps=rakz:allies:0:1:0:0:0:0:0:0.0|MAREKPROCHASKA:allies:1:1:0:0:1:0:0:1.0|Tivroxx:allies:0:1:0:0:0:0:0:0.0|oscaR:allies:0:1:0:0:0:0:0:0.0|tweeek:allies:1:1:0:0:0:0:0:1.0|NoOne:axis:1:0:0:100:0:0:0:1.0|CKU5A:axis:2:1:0:0:2:0:0:2.0|rpz:axis:1:0:0:0:0:0:0:1.0|fuckRMAN:axis:1:0:0:0:0:0:0:1.0|Role:axis:0:1:0:0:0:0:0:0.0"

echo "Injecting into: $LOGFILE"
echo "Event: $EVENT"
echo ""
echo "$EVENT" >> "$LOGFILE"
echo "Done. cod1plus.so should detect it within ~1 second."
echo ""
echo "Check backend: curl http://localhost:3005/api/round_end | python3 -m json.tool"
