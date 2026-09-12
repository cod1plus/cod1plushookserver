#!/usr/bin/env bash
# inject_test.sh - simulate the end of an S&D round by appending one [STATS_EVENT] line
# to the games_mp.log that cod1plus.so tails (the mod prints such a line at round end).
#
#   usage: bash test/inject_test.sh [round] [allies_score] [axis_score] [winner] [halftime] [logfile]
#   e.g.   bash test/inject_test.sh 2 1 1 allies
#
# The default logfile is <fs_homepath>/<fs_game>/games_mp.log, which is what the module
# tails when the server runs with a mod (set FS_HOMEPATH / FS_GAME, or pass the path).
# Player fields: name:team:kills:deaths:assists:damage:grenades:plants:defuses:score:headshots:grenade_damage:adr:slot

ROUND=${1:-1}
AS=${2:-0}
XS=${3:-1}
WINNER=${4:-axis}
HT=${5:-0}
LOGFILE=${6:-${FS_HOMEPATH:-$HOME/cod1test}/${FS_GAME:-__rPAMv115b5}/games_mp.log}

PS="player1:allies:0:1:0:0:0:0:0:0.0:0:0:0.0:0"
PS="$PS|player2:allies:1:1:0:100:1:0:0:1.0:1:0:100.0:1"
PS="$PS|player3:allies:0:1:0:35:0:0:0:0.0:0:0:35.0:2"
PS="$PS|player4:allies:0:1:0:0:0:0:0:0.0:0:0:0.0:3"
PS="$PS|player5:allies:1:1:0:100:0:1:0:1.0:0:0:100.0:4"
PS="$PS|player6:axis:1:0:0:100:0:0:0:1.0:1:0:100.0:5"
PS="$PS|player7:axis:2:1:0:200:2:0:0:2.0:0:60:200.0:6"
PS="$PS|player8:axis:1:0:0:100:0:0:0:1.0:0:0:100.0:7"
PS="$PS|player9:axis:1:0:0:100:0:0:1:1.0:0:0:100.0:8"
PS="$PS|player10:axis:0:1:0:0:0:0:0:0.0:0:0:0.0:-1"

EVENT="[STATS_EVENT]r=${ROUND},as=${AS},xs=${XS},rw=${WINNER},ht=${HT},bp=1,ps=${PS}"

echo "Injecting into: $LOGFILE"
echo "Event: $EVENT"
echo ""
echo "$EVENT" >> "$LOGFILE"
echo "Done. cod1plus.so should pick it up within ~1 second (watch the server console)."
