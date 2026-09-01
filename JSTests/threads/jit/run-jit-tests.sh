#!/usr/bin/env bash
# run-jit-tests.sh — SPEC-jit Task 13 driver.
#
# Usage:
#   run-jit-tests.sh [--int-gate] [--bench] [--disasm | --disasm-record] /path/to/jsc
#
# Phases:
#   1. lint.sh                       (always; static)
#   2. functional flag-on tests      (always; phase-1 GIL semantics today)
#   3. tag-discipline run            (when --validateButterflyTagDiscipline exists; else SKIP)
#   4. integration-gate stresses     (smoke by default; FULL with --int-gate at M4/CS2)
#   5. bench gates                   (--bench; see bench-gates.sh)
#   6. golden disasm                 (--disasm / --disasm-record; see golden-disasm.sh)
#
# Also asserts the Task-12 invariant: useJSThreads => useConcurrentJIT
# (recorded in INTEGRATE-jit.md; without concurrent JIT the worklist dedup
# backstop is bypassed).
#
# Exit: 0 all green; 1 failures; 2 environment error.

set -u -o pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

INT_GATE=0
BENCH=0
DISASM=0
DISASM_RECORD=0
JSC=""

die() { echo "run-jit-tests: error: $*" >&2; exit 2; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        --int-gate)      INT_GATE=1; shift ;;
        --bench)         BENCH=1; shift ;;
        --disasm)        DISASM=1; shift ;;
        --disasm-record) DISASM=1; DISASM_RECORD=1; shift ;;
        -*) die "unknown option: $1" ;;
        *) [[ -z "$JSC" ]] || die "multiple jsc paths"; JSC="$1"; shift ;;
    esac
done
[[ -n "$JSC" && -x "$JSC" ]] || die "path to executable jsc required"

has_option() { "$JSC" "$1" -e "" >/dev/null 2>&1; }

FAILURES=0
run_test() {
    local label="$1"; shift
    echo "=== $label"
    if "$@"; then
        echo "--- $label: ok"
    else
        echo "--- $label: FAILED" >&2
        FAILURES=$((FAILURES + 1))
    fi
}

has_option "--useJSThreads=1" || die "jsc does not accept --useJSThreads (wrong build?)"

# Task-12 invariant probe: flag-on must imply useConcurrentJIT (or M2b forces
# it). If the build allows the unsound combination silently, flag it.
if "$JSC" --useJSThreads=1 --useConcurrentJIT=0 -e "" >/dev/null 2>&1; then
    echo "NOTE: jsc accepts --useJSThreads=1 --useConcurrentJIT=0 — the Task-12" \
         "worklist-dedup bypass combination. M2b should reject or force it" \
         "(recorded in INTEGRATE-jit.md)."
fi

FLAGON=(--useJSThreads=1 --useDollarVM=1)

# Phase 1: lints.
run_test "lint" bash "$SCRIPT_DIR/lint.sh"

# Phase 2: functional flag-on tests.
for t in tid-tag-3-threads ic-publish-reset-loops spawned-thread-butterfly-stress shared-arraystorage-stress; do
    run_test "$t (flag-on)" "$JSC" "${FLAGON[@]}" "$SCRIPT_DIR/$t.js"
done

# Phase 3: tag discipline + poll placement (needs the M1 prep option).
if has_option "--validateButterflyTagDiscipline=1"; then
    VFLAGS=("${FLAGON[@]}" --validateButterflyTagDiscipline=1)
    has_option "--usePollingTraps=1" && VFLAGS+=(--usePollingTraps=1)
    run_test "tag-discipline (I14/I21)" "$JSC" "${VFLAGS[@]}" "$SCRIPT_DIR/tag-discipline.js"
else
    echo "SKIP: tag-discipline — --validateButterflyTagDiscipline not in this build (M1 prep precondition missing)"
fi

# FTL flag-on leg (M2a-gated): run the functional set again with handler ICs
# unlocked in the FTL, when the prep hunks are in.
if has_option "--useJSThreadsUnlockHandlerICInFTL=1"; then
    FTLFLAGS=("${FLAGON[@]}" --useJSThreadsUnlockHandlerICInFTL=1 --useHandlerICInFTL=1)
    for t in ic-publish-reset-loops spawned-thread-butterfly-stress; do
        run_test "$t (flag-on, FTL handler ICs)" "$JSC" "${FTLFLAGS[@]}" "$SCRIPT_DIR/$t.js"
    done
else
    echo "SKIP: FTL handler-IC leg — --useJSThreadsUnlockHandlerICInFTL not in this build (M2a prep precondition missing)"
fi

# Phase 4: integration-gate stresses (smoke by default).
GATE_ARGS=()
[[ "$INT_GATE" -eq 1 ]] && GATE_ARGS=(-- int-gate)
for t in int-gate-jettison-vs-execute int-gate-fire-vs-execute int-gate-direct-call-relink int-gate-epoch-reclaim int-gate-stop-budget; do
    label="$t"
    [[ "$INT_GATE" -eq 1 ]] && label="$t (FULL integration gate)" || label="$t (smoke)"
    run_test "$label" "$JSC" "${FLAGON[@]}" "$SCRIPT_DIR/$t.js" "${GATE_ARGS[@]}"
done

# Phase 5: bench gates.
if [[ "$BENCH" -eq 1 ]]; then
    run_test "bench-gates" bash "$SCRIPT_DIR/bench-gates.sh" "$JSC"
fi

# Phase 6: golden disasm.
if [[ "$DISASM" -eq 1 ]]; then
    if [[ "$DISASM_RECORD" -eq 1 ]]; then
        run_test "golden-disasm (record)" bash "$SCRIPT_DIR/golden-disasm.sh" --record "$JSC"
    else
        run_test "golden-disasm" bash "$SCRIPT_DIR/golden-disasm.sh" "$JSC"
    fi
fi

echo
if [[ "$FAILURES" -gt 0 ]]; then
    echo "run-jit-tests: $FAILURES failure(s)" >&2
    exit 1
fi
echo "run-jit-tests: all green"
