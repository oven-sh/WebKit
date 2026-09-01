#!/usr/bin/env bash
# ubsan.sh - Configure (and optionally build) the UndefinedBehaviorSanitizer jsc.
#
# Mirrors tsan.sh in structure (THREAD.md / docs/threads/TSAN.md), but for
# -fsanitize=undefined. Unlike TSAN, UBSan instruments individual operations
# (shifts, casts, integer overflow, null/misaligned deref, etc.) rather than
# tracking inter-thread memory access ordering, so there is no false-negative
# penalty from JIT-emitted code: the JIT tiers stay ON so the threads-scope
# JIT-side C++ (jit/ConcurrentButterflyOperations*, bytecode/RetiredJITArtifacts*,
# CodeBlock.cpp tier-up/jettison paths) is exercised under the corpus.
#
# Flag provenance (verified against this tree):
#   - ENABLE_SANITIZERS=undefined:
#       Source/cmake/WebKitCompilerFlags.cmake:429-434 appends
#       -fsanitize=undefined -fno-sanitize=vptr (vptr disabled: -fno-rtti).
#   - Base JSCOnly/Bun flags mirror build.ts getCommonFlags() (build.ts:100-115)
#     and tsan.sh.
#
# Output goes to WebKitBuild/UBSan, never colliding with the
# WebKitBuild/{Debug,Release,ReleaseLTO,TSan} dirs.
#
# Usage:
#   bash ubsan.sh                 # configure + build jsc
#   bash ubsan.sh --configure-only
#
# Run with: see Tools/threads/scan/ubsan/run-corpus-ubsan.sh
# (UBSAN_OPTIONS="print_stacktrace=1 halt_on_error=0").

set -euo pipefail

WEBKIT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${WEBKIT_DIR}/WebKitBuild/UBSan"

CONFIGURE_ONLY=0
for arg in "$@"; do
    case "$arg" in
        --configure-only) CONFIGURE_ONLY=1 ;;
        *) echo "unknown argument: $arg" >&2; exit 1 ;;
    esac
done

# Prefer the same clang build.ts prefers (build.ts:79-83).
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

    # UndefinedBehaviorSanitizer.
    -DENABLE_SANITIZERS=undefined

    # JIT stays ON (unlike TSAN): UBSan has no inter-thread access model, and
    # the threads scope explicitly includes JIT-side C++ that only runs with
    # the JIT enabled.
    -DENABLE_FTL_JIT=ON

    # Optimized but with full debug info and frame pointers so UBSan reports
    # have usable stacks.
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

echo "Configuring UBSan jsc in ${BUILD_DIR}"
cmake -S "${WEBKIT_DIR}" -B "${BUILD_DIR}" "${CMAKE_ARGS[@]}"

if [[ "${CONFIGURE_ONLY}" -eq 1 ]]; then
    echo "Configure done (skipping build)."
    exit 0
fi

cmake --build "${BUILD_DIR}" --target jsc

echo
echo "Built: ${BUILD_DIR}/bin/jsc"
echo "Run with:"
echo "  UBSAN_OPTIONS=\"print_stacktrace=1 halt_on_error=0 report_error_type=1\" \\"
echo "    ${BUILD_DIR}/bin/jsc your-test.js"
