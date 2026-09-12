# Local CoD1 dedicated server, to test cod1plus.so without touching the VPS.
#
# i386 image: cod_lnxded and cod1plus.so are 32-bit ELF. An x86_64 base with multiarch
# would work too, but i386/debian avoids listing the :i386 packages one by one and keeps
# the image reproducible.
#
# The server content (main/, __rPAMv115b5/, cod_lnxded) is NOT copied into the image:
# 1.5 GB of pk3 in a layer would be absurd to rebuild on every try. It is mounted at run
# time - see run.sh.
FROM i386/debian:bookworm-slim

# curl STAYS INSTALLED: the build gate in cod1reloaded.c fetches the client manifest by
# shelling out to curl (its home-grown HTTP is plain text and GitHub is HTTPS). Without
# curl the fetch fails in a loop - and since LD_PRELOAD is inherited by the /bin/sh of
# every call, every attempt prints a "[cod1plus] Loaded" line. That was the log spam.
#
# cod_lnxded dates from 2004 and wants libstdc++.so.5 (GCC 3 ABI). No modern Debian ships
# it: the package comes from the archive. Forgetting it makes the container fail with
# "error while loading shared libraries: libstdc++.so.5".
ARG LIBSTDCPP5=http://archive.debian.org/debian/pool/main/g/gcc-3.3/libstdc++5_3.3.6-30_i386.deb

RUN apt-get update \
 && apt-get install -y --no-install-recommends libstdc++6 zlib1g curl ca-certificates \
 && curl -fsSL -o /tmp/libstdc++5.deb "$LIBSTDCPP5" \
 && dpkg -i /tmp/libstdc++5.deb \
 && rm -f /tmp/libstdc++5.deb \
 && rm -rf /var/lib/apt/lists/* \
 && ldconfig -p | grep -q "libstdc++\.so\.5"
# The check goes through ldconfig and not a hard-coded path: the package installs into
# /usr/lib/i386-linux-gnu/, not /usr/lib/, and a test on the wrong path failed an
# otherwise correct build.

WORKDIR /server

# No fixed ENTRYPOINT: run.sh passes the full command line, like start.sh on the VPS, so
# the two stay comparable.
