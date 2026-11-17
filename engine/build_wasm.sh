#!/bin/sh
# The engine (and Lua, compiled as C++ so that its errors are C++ exceptions)
# for the browser: wasi-sdk, Wasm exception handling, a reactor module with
# the C interface exported. Output: ../web/public/arena.wasm
#   WASI_SDK=/path/to/wasi-sdk engine/build_wasm.sh
set -e
cd "$(dirname "$0")"
W=${WASI_SDK:-/c/Users/iusti/tools/wasi-sdk-34.0-x86_64-windows}
S=$W/share/wasi-sysroot
OUT=../web/public/arena.wasm
mkdir -p build/wasm ../web/public
# the sandbox opens only base, table and string: no OS, I/O or module loading
LUA=$(ls ../third_party/lua/src/*.c | grep -v -E '/(lua|luac|loslib|liolib|loadlib|linit)\.c$')
"$W/bin/clang++" --target=wasm32-wasip1 -O2 -std=c++20 -fwasm-exceptions -mllvm -wasm-use-legacy-eh=false -mexec-model=reactor -nostdlib++ \
  -I"$S/include/wasm32-wasip1/eh" -Iinclude -I../third_party/lua/src -D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_PROCESS_CLOCKS -DLUA_USE_C89 -DARENA_LUA_CPP \
  -x c++ $LUA -x none src/fixed.cpp src/game.cpp src/session.cpp src/capi.cpp \
  -L"$S/lib/wasm32-wasip1/eh" -lc++ -lc++abi -lunwind -lwasi-emulated-signal -lwasi-emulated-process-clocks \
  -o "$OUT"
ls -la "$OUT"
cp scripts/arena.lua ../web/public/arena.lua  # served next to the engine
