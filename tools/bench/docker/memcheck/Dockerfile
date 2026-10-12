# Runtime environment for Valgrind memcheck (tools/bench/race_check.sh <engine> <secs> memcheck).
# Memcheck needs the debug symbols of the 32-bit dynamic loader, which the multilib
# glibc of the main image does not ship: this image is a native i386 Debian 11
# with libc6-dbg, whose version must match its glibc (same suites as the main image). It only runs the server, the engine is built in the main image.
FROM i386/debian:11-slim

RUN printf 'deb http://archive.debian.org/debian bullseye main\ndeb http://archive.debian.org/debian bullseye-updates main\n' > /etc/apt/sources.list \
 && apt-get -o Acquire::Check-Valid-Until=false update \
 && apt-get install -y --no-install-recommends ca-certificates \
 && echo 'deb https://snapshot-cloudflare.debian.org/archive/debian-security/20260831T000000Z bullseye-security main' > /etc/apt/sources.list.d/bullseye-security.list \
 && apt-get -o Acquire::Check-Valid-Until=false -o Acquire::Retries=5 update \
 && apt-get -o Acquire::Retries=5 install -y --no-install-recommends \
      valgrind libc6-dbg python3 procps \
 && rm -rf /var/lib/apt/lists/*
