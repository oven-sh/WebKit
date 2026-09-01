#!/usr/bin/env bash
# Run a Fuzzilli campaign against the REPRL+ASAN jsc in WebKitBuild/Fuzz
# using the "jscthreads" profile (Thread/Lock/Condition/ThreadLocal +
# shared-object race generators).
#
# Usage:
#   Tools/threads/fuzz/run-fuzzilli.sh [--smoke] [extra fuzzilli args...]
#
#   --smoke   10-minute, single-worker, timeout-bounded smoke run.
#
# Environment overrides:
#   FUZZILLI_DIR  (default /root/fuzzilli)
#   JSC_BINARY    (default WebKitBuild/Fuzz/bin/jsc relative to repo root)
#   STORAGE       (default WebKitBuild/Fuzz/fuzzilli-storage)
#   JOBS          (default 4; smoke forces 1)

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
FUZZILLI_DIR="${FUZZILLI_DIR:-/root/fuzzilli}"
FUZZILLI_BIN="$FUZZILLI_DIR/.build/release/FuzzilliCli"
JSC_BINARY="${JSC_BINARY:-$REPO_ROOT/WebKitBuild/Fuzz/bin/jsc}"
STORAGE="${STORAGE:-$REPO_ROOT/WebKitBuild/Fuzz/fuzzilli-storage}"
JOBS="${JOBS:-4}"

SMOKE=0
if [[ "${1:-}" == "--smoke" ]]; then
    SMOKE=1
    shift
fi

[[ -x "$FUZZILLI_BIN" ]] || { echo "FuzzilliCli not found at $FUZZILLI_BIN (build with: cd $FUZZILLI_DIR && swift build -c release)"; exit 1; }
[[ -x "$JSC_BINARY" ]] || { echo "jsc not found at $JSC_BINARY (see docs/threads/FUZZ.md for the cmake line)"; exit 1; }

mkdir -p "$STORAGE"

# ASAN: let Fuzzilli see crashes, don't die on the allocator's own limits.
# detect_stack_use_after_return=0 is REQUIRED on Linux threads lanes (see
# docs/threads/FUZZ.md "ASAN_OPTIONS lane pin" / CVE-AUDIT B9 / MC-GC S2a).
export ASAN_OPTIONS="detect_stack_use_after_return=0:abort_on_error=1:symbolize=1:detect_leaks=0:malloc_context_size=5:allocator_may_return_null=1"

ARGS=(
    --profile=jscthreads
    --storagePath="$STORAGE"
    --resume
    --timeout=1000
)

if [[ "$SMOKE" == "1" ]]; then
    # Fuzzilli only supports whole-hour --maxRuntimeInHours, so bound the
    # smoke run externally with timeout(1).
    ARGS+=(--jobs=1)
    exec nice -n 10 timeout --signal=INT --kill-after=30 600 \
        "$FUZZILLI_BIN" "${ARGS[@]}" "$@" "$JSC_BINARY"
fi

ARGS+=(--jobs="$JOBS")
exec nice -n 10 "$FUZZILLI_BIN" "${ARGS[@]}" "$@" "$JSC_BINARY"
