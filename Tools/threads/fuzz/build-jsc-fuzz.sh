#!/usr/bin/env bash
# Configure + build the REPRL-enabled, ASAN-instrumented jsc used for
# Fuzzilli thread fuzzing. Builds into WebKitBuild/Fuzz ONLY — never into
# Debug/Release/TSan (those belong to the engine bring-up loop).

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD_DIR="$REPO_ROOT/WebKitBuild/Fuzz"

nice -n 10 cmake -S "$REPO_ROOT" -B "$BUILD_DIR" -G Ninja \
    -DPORT=JSCOnly \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_C_COMPILER=/opt/llvm-21/bin/clang-21 \
    -DCMAKE_CXX_COMPILER=/usr/local/bin/clang++-21 \
    -DENABLE_STATIC_JSC=ON \
    -DUSE_BUN_JSC_ADDITIONS=ON \
    -DUSE_BUN_EVENT_LOOP=ON \
    -DENABLE_FUZZILLI=ON \
    -DENABLE_SANITIZERS=address \
    -DENABLE_FTL_JIT=ON \
    -DCMAKE_C_FLAGS="-fno-omit-frame-pointer -g -fsanitize-coverage=trace-pc-guard" \
    -DCMAKE_CXX_FLAGS="-fno-omit-frame-pointer -g -fsanitize-coverage=trace-pc-guard"

nice -n 10 ninja -C "$BUILD_DIR" jsc

"$BUILD_DIR/bin/jsc" --useJSThreads=1 -e \
    'if (typeof Thread !== "function" || new Thread(()=>42).join() !== 42) throw "Thread API broken"; print("jsc fuzz build OK")'
