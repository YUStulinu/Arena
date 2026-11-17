#!/bin/sh
# Build the engine (native) and run its tests. On Windows: MSYS2 UCRT64 g++ first on PATH.
set -e
cd "$(dirname "$0")"
if [ -d /c/msys64/ucrt64/bin ]; then
  export PATH=/c/msys64/ucrt64/bin:$PATH
  GEN="-G Ninja -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++"
fi
cmake -S . -B build/native $GEN "$@"
cmake --build build/native -j 8
