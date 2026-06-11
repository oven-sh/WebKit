#!/usr/bin/env bash
# amplify.sh — race-amplification harness for the shared-memory threads work.
#
# Runs a given JS file M times under jsc with the RaceAmplifier enabled,
# using a fresh random --randomYieldSeed per run, and reports any crash or
# output divergence. The first run's output is the reference; later runs that
# print something different are flagged as divergent. Crashing/divergent
# seeds are printed so they can be replayed deterministically.
#
# Usage:
#   Tools/threads/amplify.sh [options] /path/to/jsc script.js
#
# Options:
#   --runs M           Number of runs (default: 100).
#   --period N         --randomYieldPeriod value (default: 64).
#   --seed-base S      Derive run seeds as S+1, S+2, ... instead of randomly
#                      (makes the whole campaign reproducible).
#   --max-sleep-us U   --randomYieldMaxMicroseconds value (default: 100).
#   --timeout SECS     Per-run timeout; a timeout counts as a hang finding
#                      (default: 60; 0 disables).
#   --jsc-arg ARG      Extra argument passed to jsc (repeatable).
#   --keep-logs        Keep per-run logs even for clean runs.
#   -h, --help         Show this help.
#
# Exit codes:
#   0  all runs clean (same exit status and output as the reference run)
#   1  at least one crash, hang, or divergence
#   2  usage / environment error

set -u -o pipefail

RUNS=100
PERIOD=64
SEED_BASE=""
MAX_SLEEP_US=100
TIMEOUT_SECS=60
KEEP_LOGS=0
EXTRA_JSC_ARGS=()
JSC=""
SCRIPT=""

usage() {
    sed -n '2,27p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

die() {
    echo "amplify: error: $*" >&2
    exit 2
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --runs)         [[ $# -ge 2 ]] || die "--runs needs a value"; RUNS="$2"; shift 2 ;;
        --period)       [[ $# -ge 2 ]] || die "--period needs a value"; PERIOD="$2"; shift 2 ;;
        --seed-base)    [[ $# -ge 2 ]] || die "--seed-base needs a value"; SEED_BASE="$2"; shift 2 ;;
        --max-sleep-us) [[ $# -ge 2 ]] || die "--max-sleep-us needs a value"; MAX_SLEEP_US="$2"; shift 2 ;;
        --timeout)      [[ $# -ge 2 ]] || die "--timeout needs a value"; TIMEOUT_SECS="$2"; shift 2 ;;
        --jsc-arg)      [[ $# -ge 2 ]] || die "--jsc-arg needs a value"; EXTRA_JSC_ARGS+=("$2"); shift 2 ;;
        --keep-logs)    KEEP_LOGS=1; shift ;;
        -h|--help)      usage; exit 0 ;;
        -*)             die "unknown option: $1" ;;
        *)
            if [[ -z "$JSC" ]]; then JSC="$1"
            elif [[ -z "$SCRIPT" ]]; then SCRIPT="$1"
            else die "unexpected argument: $1"
            fi
            shift ;;
    esac
done

[[ -n "$JSC" && -n "$SCRIPT" ]] || { usage >&2; die "need a jsc binary and a JS file"; }
[[ -x "$JSC" ]] || die "jsc binary not found or not executable: $JSC"
[[ -f "$SCRIPT" ]] || die "JS file not found: $SCRIPT"
[[ "$RUNS" =~ ^[0-9]+$ && "$RUNS" -ge 1 ]] || die "--runs must be a positive integer"
[[ "$PERIOD" =~ ^[0-9]+$ && "$PERIOD" -ge 1 ]] || die "--period must be a positive integer"

TIMEOUT_CMD=()
if [[ "$TIMEOUT_SECS" != "0" ]]; then
    if command -v timeout >/dev/null 2>&1; then
        TIMEOUT_CMD=(timeout -k 5 "$TIMEOUT_SECS")
    elif command -v gtimeout >/dev/null 2>&1; then
        TIMEOUT_CMD=(gtimeout -k 5 "$TIMEOUT_SECS")
    else
        echo "amplify: warning: no timeout(1) available; hangs will block the harness" >&2
    fi
fi

WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/amplify.XXXXXX")" || die "mktemp failed"
cleanup() {
    if [[ "$KEEP_LOGS" -eq 1 || "$FINDINGS" -gt 0 ]]; then
        echo "amplify: logs kept in $WORK_DIR"
    else
        rm -rf "$WORK_DIR"
    fi
}
FINDINGS=0
trap cleanup EXIT

random_seed() {
    # 32-bit nonzero seed (seed 0 means "pick one" to the amplifier; the
    # --randomYieldSeed option is a 32-bit Unsigned).
    local s
    s=$(( (RANDOM << 30) ^ (RANDOM << 15) ^ RANDOM ^ $$ ^ $(date +%s%N 2>/dev/null || date +%s) ))
    s=$(( s & 0xffffffff ))
    [[ "$s" -eq 0 ]] && s=1
    echo "$s"
}

REF_OUT="$WORK_DIR/reference.out"
REF_STATUS=""
BAD_SEEDS=()

# The amplifier logs its effective seed ("[RaceAmplifier] enabled: period=..
# seed=<N> ..", RaceAmplifier.cpp) and every run uses a different seed, so a
# raw byte comparison would diverge on every run by construction. Compare
# outputs with amplifier banner lines stripped from BOTH sides; exit-status
# divergence checking is unaffected. Raw logs are kept untouched for replay.
outputs_differ() {
    ! cmp -s <(grep -v '^\[RaceAmplifier\]' "$1") <(grep -v '^\[RaceAmplifier\]' "$2")
}

echo "amplify: $RUNS runs of $SCRIPT under $JSC (period=$PERIOD, maxSleepUs=$MAX_SLEEP_US, timeout=${TIMEOUT_SECS}s)"

for ((i = 1; i <= RUNS; i++)); do
    if [[ -n "$SEED_BASE" ]]; then
        SEED=$((SEED_BASE + i))
    else
        SEED="$(random_seed)"
    fi

    OUT="$WORK_DIR/run-$i.out"
    ${TIMEOUT_CMD[@]+"${TIMEOUT_CMD[@]}"} "$JSC" \
        "--randomYieldPeriod=$PERIOD" \
        "--randomYieldSeed=$SEED" \
        "--randomYieldMaxMicroseconds=$MAX_SLEEP_US" \
        ${EXTRA_JSC_ARGS[@]+"${EXTRA_JSC_ARGS[@]}"} \
        "$SCRIPT" >"$OUT" 2>&1
    STATUS=$?

    KIND=""
    if [[ "$STATUS" -eq 124 || "$STATUS" -eq 137 ]] && [[ ${#TIMEOUT_CMD[@]} -gt 0 ]]; then
        KIND="HANG (timeout after ${TIMEOUT_SECS}s)"
    elif [[ "$STATUS" -gt 128 ]]; then
        KIND="CRASH (signal $((STATUS - 128)))"
    fi

    if [[ -z "$REF_STATUS" && -z "$KIND" ]]; then
        # First non-crashing run establishes the reference.
        REF_STATUS="$STATUS"
        cp "$OUT" "$REF_OUT"
    elif [[ -z "$KIND" ]]; then
        if [[ "$STATUS" -ne "$REF_STATUS" ]]; then
            KIND="DIVERGENCE (exit $STATUS vs reference $REF_STATUS)"
        elif outputs_differ "$OUT" "$REF_OUT"; then
            KIND="DIVERGENCE (output differs from reference)"
        fi
    fi

    if [[ -n "$KIND" ]]; then
        FINDINGS=$((FINDINGS + 1))
        BAD_SEEDS+=("$SEED")
        echo "amplify: run $i/$RUNS seed=$SEED: $KIND"
        echo "         log: $OUT"
        echo "         replay: $JSC --randomYieldPeriod=$PERIOD --randomYieldSeed=$SEED --randomYieldMaxMicroseconds=$MAX_SLEEP_US ${EXTRA_JSC_ARGS[*]:-} $SCRIPT"
    else
        [[ "$KEEP_LOGS" -eq 1 ]] || rm -f "$OUT"
        printf 'amplify: run %d/%d seed=%s: ok\r' "$i" "$RUNS" "$SEED"
    fi
done
printf '\n'

if [[ "$FINDINGS" -gt 0 ]]; then
    echo "amplify: FAIL — $FINDINGS finding(s) in $RUNS runs"
    echo "amplify: bad seeds: ${BAD_SEEDS[*]}"
    exit 1
fi

echo "amplify: PASS — $RUNS runs clean"
exit 0
