#!/usr/bin/env bash
# tsan.sh - Configure (and optionally build) the ThreadSanitizer no-JIT jsc.
#
# This is the race-hunting configuration described in THREAD.md: TSAN with the
# JIT fully disabled, so every VM/runtime memory access is instrumented by the
# compiler. JIT-emitted code cannot be instrumented by TSAN, which would turn
# races reached from jitted frames into silent false negatives — hence the
# CLoop interpreter build.
#
# Flag provenance (verified against this tree):
#   - ENABLE_SANITIZERS=thread:
#       Source/cmake/WebKitCompilerFlags.cmake:436-438 appends
#       -fsanitize=thread to compile and link flags ("thread AND NOT MSVC").
#   - ENABLE_C_LOOP / ENABLE_JIT / ENABLE_DFG_JIT / ENABLE_FTL_JIT /
#     ENABLE_SAMPLING_PROFILER / ENABLE_WEBASSEMBLY[_BBQJIT|_OMGJIT]:
#       Source/cmake/WebKitFeatures.cmake:195,199,203,216,257,275-277.
#       Lines 312-315 declare WEBKIT_OPTION_CONFLICT(ENABLE_JIT ENABLE_C_LOOP),
#       (ENABLE_SAMPLING_PROFILER ENABLE_C_LOOP) and
#       (ENABLE_WEBASSEMBLY ENABLE_C_LOOP), so all of these must be explicitly
#       OFF when the CLoop is ON or configure fails.
#   - USE_SYSTEM_MALLOC=ON:
#       bmalloc already self-disables under TSAN
#       (Source/bmalloc/bmalloc/BPlatform.h:388-390 sets BUSE_SYSTEM_MALLOC
#       when BTSAN_ENABLED), but we set it at the CMake level too so WTF's
#       FastMalloc path goes through the system allocator TSAN intercepts.
#   - TSAN_ENABLED is auto-detected from -fsanitize=thread
#       (Source/WTF/wtf/Compiler.h:166-177); no extra define needed.
#   - Base JSCOnly/Bun flags mirror build.ts getCommonFlags() (build.ts:100-115).
#
# Output goes to WebKitBuild/TSan, never colliding with the
# WebKitBuild/{Debug,Release,ReleaseLTO} dirs used by build.ts.
#
# Usage:
#   bash tsan.sh                 # configure + build jsc
#   bash tsan.sh --configure-only
#
# Run with: see docs/threads/TSAN.md (TSAN_OPTIONS, suppressions, JSC options).

set -euo pipefail

WEBKIT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${WEBKIT_DIR}/WebKitBuild/TSan"

CONFIGURE_ONLY=0
for arg in "$@"; do
    case "$arg" in
        --configure-only) CONFIGURE_ONLY=1 ;;
        *) echo "unknown argument: $arg" >&2; exit 1 ;;
    esac
done

# Prefer the same clang build.ts prefers (build.ts:79-83). TSAN requires
# clang/gcc; clang gives the best TSAN runtime.
find_tool() {
    for name in "$@"; do
        if command -v "$name" >/dev/null 2>&1; then
            command -v "$name"
            return 0
        fi
    done
    return 1
}

CC_BASE="$(find_tool clang-21 clang)" || { echo "clang not found" >&2; exit 1; }
CXX_BASE="$(find_tool clang++-21 clang++)" || { echo "clang++ not found" >&2; exit 1; }
CCACHE="$(find_tool ccache || true)"

mkdir -p "${BUILD_DIR}"

CMAKE_ARGS=(
    -G Ninja
    -DPORT=JSCOnly
    -DENABLE_STATIC_JSC=ON
    -DALLOW_LINE_AND_COLUMN_NUMBER_IN_BUILTINS=ON
    -DUSE_THIN_ARCHIVES=OFF
    -DUSE_BUN_JSC_ADDITIONS=ON
    -DUSE_BUN_EVENT_LOOP=ON

    # ThreadSanitizer.
    -DENABLE_SANITIZERS=thread

    # JIT fully disabled: CLoop interpreter so all interpreter memory accesses
    # are TSAN-instrumented C++. Everything that conflicts with or depends on
    # the JIT must be explicitly OFF (WebKitFeatures.cmake:312-323).
    -DENABLE_C_LOOP=ON
    -DENABLE_JIT=OFF
    -DENABLE_DFG_JIT=OFF
    -DENABLE_FTL_JIT=OFF
    -DENABLE_WEBASSEMBLY=OFF
    -DENABLE_WEBASSEMBLY_BBQJIT=OFF
    -DENABLE_WEBASSEMBLY_OMGJIT=OFF
    -DENABLE_SAMPLING_PROFILER=OFF

    # Route all allocation through the system malloc TSAN intercepts.
    -DUSE_SYSTEM_MALLOC=ON

    # Optimized but with full debug info and frame pointers so TSAN reports
    # have usable stacks. (Debug+TSAN is impractically slow.)
    -DCMAKE_BUILD_TYPE=RelWithDebInfo
    "-DCMAKE_C_FLAGS=-fno-omit-frame-pointer -g"
    "-DCMAKE_CXX_FLAGS=-fno-omit-frame-pointer -g"

    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
)

if [[ -n "${CCACHE}" ]]; then
    CMAKE_ARGS+=(
        "-DCMAKE_C_COMPILER_LAUNCHER=${CCACHE}"
        "-DCMAKE_CXX_COMPILER_LAUNCHER=${CCACHE}"
    )
fi
CMAKE_ARGS+=(
    "-DCMAKE_C_COMPILER=${CC_BASE}"
    "-DCMAKE_CXX_COMPILER=${CXX_BASE}"
)

echo "Configuring TSAN no-JIT jsc in ${BUILD_DIR}"
cmake -S "${WEBKIT_DIR}" -B "${BUILD_DIR}" "${CMAKE_ARGS[@]}"

if [[ "${CONFIGURE_ONLY}" -eq 1 ]]; then
    echo "Configure done (skipping build)."
    exit 0
fi

cmake --build "${BUILD_DIR}" --target jsc

echo
echo "Built: ${BUILD_DIR}/bin/jsc"
echo "Run with:"
echo "  TSAN_OPTIONS=\"suppressions=${WEBKIT_DIR}/Tools/tsan/suppressions.txt history_size=7 second_deadlock_stack=1\" \\"
echo "    ${BUILD_DIR}/bin/jsc your-test.js"
