#!/bin/bash
# usage: configure.sh <dir> <extra cmake args...>
set -e
dir=$1; shift
ICU=/workspace/wk-prebuilt/bun-webkit
CFLAGS_COMMON="-mno-omit-leaf-frame-pointer -g1 -fno-omit-frame-pointer -ffunction-sections -fdata-sections -faddrsig -fno-unwind-tables -fno-asynchronous-unwind-tables -DU_STATIC_IMPLEMENTATION=1 -march=nehalem ${EXTRA_CFLAGS:-}"
cmake -S ${SRC:-/workspace/WebKit} -B /workspace/wkbuild/$dir -G Ninja \
  -DPORT=JSCOnly \
  -DENABLE_STATIC_JSC=ON \
  -DENABLE_BUN_SKIP_FAILING_ASSERTIONS=ON \
  -DCMAKE_BUILD_TYPE=Release \
  -DUSE_THIN_ARCHIVES=OFF \
  -DUSE_BUN_JSC_ADDITIONS=ON \
  -DUSE_BUN_EVENT_LOOP=ON \
  -DENABLE_FTL_JIT=ON \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DALLOW_LINE_AND_COLUMN_NUMBER_IN_BUILTINS=ON \
  -DENABLE_REMOTE_INSPECTOR=ON \
  -DCMAKE_C_COMPILER=/usr/lib/llvm-current/bin/clang \
  -DCMAKE_CXX_COMPILER=/usr/lib/llvm-current/bin/clang++ \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache \
  -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
  -DCMAKE_EXE_LINKER_FLAGS="-fuse-ld=lld" \
  -DCMAKE_AR=/usr/lib/llvm-current/bin/llvm-ar \
  -DCMAKE_RANLIB=/usr/lib/llvm-current/bin/llvm-ranlib \
  -DCMAKE_C_FLAGS="$CFLAGS_COMMON" \
  -DCMAKE_CXX_FLAGS="$CFLAGS_COMMON -fno-c++-static-destructors" \
  -DCMAKE_C_FLAGS_RELEASE="-O3 -DNDEBUG=1" \
  -DCMAKE_CXX_FLAGS_RELEASE="-O3 -DNDEBUG=1" \
  -DICU_ROOT=$ICU \
  -DBUN_ICU_ZSTD_SOURCE_DIR=/workspace/bun/vendor/zstd/lib \
  "$@"
