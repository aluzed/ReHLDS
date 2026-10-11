# Build and runtime environment for the benchmark harness.
# Same toolchain as the Linux CI job (.github/workflows/build.yml): Debian 11 with
# i386 multilib, which is also what the 32-bit HLDS binaries need to run.
FROM debian:11-slim

# Debian 11 is EOL: fetch the base suite from the archive and pin the security
# suite to its last complete snapshot, exactly like the CI job does.
RUN printf 'deb http://archive.debian.org/debian bullseye main\ndeb http://archive.debian.org/debian bullseye-updates main\n' > /etc/apt/sources.list \
 && apt-get -o Acquire::Check-Valid-Until=false update \
 && apt-get install -y --no-install-recommends ca-certificates \
 && echo 'deb https://snapshot-cloudflare.debian.org/archive/debian-security/20260831T000000Z bullseye-security main' > /etc/apt/sources.list.d/bullseye-security.list \
 && apt-get -o Acquire::Check-Valid-Until=false -o Acquire::Retries=5 update \
 && apt-get -o Acquire::Retries=5 install -y \
      gcc-multilib g++-multilib build-essential \
      libc6-dev libc6-dev-i386 \
      cmake git curl procps \
 && rm -rf /var/lib/apt/lists/*
