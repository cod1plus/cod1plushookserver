#!/bin/sh
# build_msgguard.sh - src/msg_guard.c alone, as msgguard.so: preloaded next to an older
# cod1plus.so that lacks it (the Docker test server). cod1plus.so built by build.sh has it.
set -eu
cd "$(dirname "$0")/.."
gcc -m32 -O2 -Wall -shared -fPIC -DMSG_GUARD_STANDALONE -include src/glibc_compat.h -o msgguard.so src/msg_guard.c src/hooks.c
if nm -D msgguard.so | grep -E 'GLIBC_2\.(3[5-9]|[4-9][0-9])|__isoc23' ; then
    echo "build_msgguard.sh: needs a glibc newer than 2.34 - refused" >&2
    rm -f msgguard.so
    exit 1
fi
md5sum msgguard.so
