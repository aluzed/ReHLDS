#!/bin/bash
# Configures and builds a CMake project. Runs inside the benchmark container.
# usage: cmake_build.sh <source dir> <build dir>
set -euo pipefail

src=$1
build=$2

mkdir -p "$build"
cd "$build"
cmake "$src" -DCMAKE_BUILD_TYPE=Release -DREHLDS_HELGRIND="${BENCH_HELGRIND:-OFF}" > cmake.log
make -j"$(nproc)" > make.log 2>&1 || { tail -40 make.log; exit 1; }
