#!/usr/bin/env bash
# bench-gates.sh — SPEC-jit Task 13 performance gates (r12 = heap dev-7 split).
#
# Gate matrix (PRE-integration, single-threaded, vs the Task-1 GIL stub):
#
#  G1  LLINT-I1     flag-off + --useJIT=0 vs the RECORDED llint baseline
#                   (quantifies the §5.4 gate branch + D7 repack offsets;
#                   threshold 1% per-bench median, mirroring
#                   Tools/threads/bench-gate.sh). FAIL = I1 violation.
#  G2  FLAGON-1-0   {useJSThreads=1, useSharedGCHeap=0} composite:
#                   geomean(flag-on / flag-off) over the bench suite must be
#                   <= 1.05. GATED. A miss TRIGGERS the §4.3 LLInt-cache
#                   revival charter (proto/transition caches as immutable
#                   single-pointer records) — REQUIRED pre-ship on miss.
#  R1  FLAGON-1-1   {useJSThreads=1, useSharedGCHeap=1} composite: MEASURED
#                   and RECORDED only, never gated (heap §5.5 alloc cost;
#                   budget set at GIL-removal chartering). Skipped with a
#                   loud line if the heap option does not exist yet.
#  R2  FIRES        fires-per-sec.js — Class-A watchpoint fire throughput,
#                   RECORDED (flag-off and flag-on).
#  R3  CONSTRUCT    construction-shared-constructor.js — shared-constructor
#                   construction microbench vs the GIL stub, RECORDED
#                   (per-op relative cost, pre-share vs post-share; feeds the
#                   OM 8h Task-14 promotion decision, DECIDED PRE-INT).
#
# Usage:
#   bench-gates.sh [--record-llint] [--runs K] [--out FILE] /path/to/jsc
#
# Exit codes: 0 = all gates green; 1 = a GATED comparison failed; 2 = env.

set -u -o pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../../.." && pwd)"
BENCH_DIR="$ROOT_DIR/JSTests/threads/bench"
BENCH_HARNESS="$BENCH_DIR/harness.js"
LLINT_BASELINE_DEFAULT="$SCRIPT_DIR/baselines/llint-bench-$(uname -m)-$(uname -s).txt"

RUNS=5
RECORD_LLINT=0
OUT=""
JSC=""
GEOMEAN_BUDGET=1.05
LLINT_THRESHOLD_PCT=1

die() { echo "bench-gates: error: $*" >&2; exit 2; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        --record-llint) RECORD_LLINT=1; shift ;;
        --runs) [[ $# -ge 2 ]] || die "--runs needs a value"; RUNS="$2"; shift 2 ;;
        --out)  [[ $# -ge 2 ]] || die "--out needs a value"; OUT="$2"; shift 2 ;;
        -*) die "unknown option: $1" ;;
        *) [[ -z "$JSC" ]] || die "multiple jsc paths"; JSC="$1"; shift ;;
    esac
done
[[ -n "$JSC" && -x "$JSC" ]] || die "path to executable jsc required"

record_line() {
    echo "$1"
    [[ -n "$OUT" ]] && echo "$1" >> "$OUT"
}

# Probe whether jsc accepts an option (prep preconditions may be missing).
has_option() {
    "$JSC" "$1" -e "" >/dev/null 2>&1
}

SHARED_HEAP_OPT=""
if has_option "--useSharedGCHeap=0"; then
    SHARED_HEAP_OPT="--useSharedGCHeap"
fi
has_option "--useJSThreads=1" || die "jsc does not accept --useJSThreads (wrong build?)"

BENCHES=()
for f in "$BENCH_DIR"/*.js; do
    [[ "$(basename "$f")" == "harness.js" ]] && continue
    BENCHES+=("$f")
done
[[ ${#BENCHES[@]} -gt 0 ]] || die "no benchmarks in $BENCH_DIR"

# run_suite <label> <flags...> — prints "name medianMs" per bench.
run_suite() {
    local label="$1"; shift
    local bench name
    for bench in "${BENCHES[@]}"; do
        name="$(basename "$bench" .js)"
        local times=()
        for ((i = 0; i < RUNS; ++i)); do
            local line
            line="$("$JSC" "$@" "$BENCH_HARNESS" "$bench" 2>/dev/null | grep '^BENCH ')" \
                || { echo "bench-gates: $label/$name FAILED to run" >&2; return 1; }
            times+=("$(echo "$line" | awk '{print $3}')")
        done
        local median
        median="$(printf '%s\n' "${times[@]}" | sort -g | awk -v n="${#times[@]}" 'NR == int((n + 1) / 2) { print; exit }')"
        echo "$name $median"
    done
}

FAILURES=0

# ---------------------------------------------------------------------------
# G1 — LLINT-I1 (--useJIT=0, flag OFF, vs recorded baseline)
# ---------------------------------------------------------------------------
LLINT_FLAGS=(--useJSThreads=0 --useJIT=0)
LLINT_RESULTS="$(run_suite "llint" "${LLINT_FLAGS[@]}")" || die "llint suite failed"

if [[ "$RECORD_LLINT" -eq 1 ]]; then
    mkdir -p "$(dirname "$LLINT_BASELINE_DEFAULT")"
    echo "$LLINT_RESULTS" > "$LLINT_BASELINE_DEFAULT"
    record_line "LLINT-I1 baseline recorded to $LLINT_BASELINE_DEFAULT"
elif [[ -f "$LLINT_BASELINE_DEFAULT" ]]; then
    while read -r name median; do
        base="$(awk -v n="$name" '$1 == n { print $2 }' "$LLINT_BASELINE_DEFAULT")"
        if [[ -z "$base" ]]; then
            record_line "LLINT-I1 $name NEW (no baseline entry)"
            continue
        fi
        over="$(awk -v m="$median" -v b="$base" -v t="$LLINT_THRESHOLD_PCT" \
            'BEGIN { print (m > b * (1 + t / 100)) ? 1 : 0 }')"
        record_line "LLINT-I1 $name median=${median}ms baseline=${base}ms"
        if [[ "$over" == "1" ]]; then
            echo "bench-gates: GATE FAIL (G1 LLINT-I1): $name regressed > ${LLINT_THRESHOLD_PCT}% — §5.4 gate branch / repack cost exceeds the I1 envelope" >&2
            FAILURES=$((FAILURES + 1))
        fi
    done <<< "$LLINT_RESULTS"
else
    record_line "LLINT-I1 SKIP: no baseline at $LLINT_BASELINE_DEFAULT (record one on the pre-change blessed build with --record-llint)"
fi

# ---------------------------------------------------------------------------
# G2 — FLAGON-1-0 composite geomean gate (<= 5%)
# ---------------------------------------------------------------------------
OFF_FLAGS=(--useJSThreads=0)
ON10_FLAGS=(--useJSThreads=1)
[[ -n "$SHARED_HEAP_OPT" ]] && { OFF_FLAGS+=("$SHARED_HEAP_OPT=0"); ON10_FLAGS+=("$SHARED_HEAP_OPT=0"); }

OFF_RESULTS="$(run_suite "flag-off" "${OFF_FLAGS[@]}")" || die "flag-off suite failed"
ON10_RESULTS="$(run_suite "flag-on-1-0" "${ON10_FLAGS[@]}")" || die "flag-on {1,0} suite failed"

GEOMEAN_10="$(
    paste <(echo "$OFF_RESULTS") <(echo "$ON10_RESULTS") | awk '
        $1 != $3 { print "NAME-MISMATCH"; exit }
        { logsum += log($4 / $2); n++; printf "RATIO %s %.4f\n", $1, $4 / $2 > "/dev/stderr" }
        END { if (n) printf "%.4f\n", exp(logsum / n) }
    ' 2> >(while read -r l; do record_line "FLAGON-1-0 $l"; done)
)"
[[ "$GEOMEAN_10" != "NAME-MISMATCH" && -n "$GEOMEAN_10" ]] || die "geomean computation failed"
record_line "FLAGON-1-0 GEOMEAN $GEOMEAN_10 (budget $GEOMEAN_BUDGET)"
if awk -v g="$GEOMEAN_10" -v b="$GEOMEAN_BUDGET" 'BEGIN { exit !(g > b) }'; then
    echo "bench-gates: GATE FAIL (G2 FLAGON-1-0): geomean $GEOMEAN_10 > $GEOMEAN_BUDGET." >&2
    echo "  CONSEQUENCE (SPEC-jit §4.3 charter): the disabled LLInt proto/transition" >&2
    echo "  caches MUST return as immutable single-pointer records (§5.8 pattern)" >&2
    echo "  before ship. File the revival task now." >&2
    FAILURES=$((FAILURES + 1))
fi

# ---------------------------------------------------------------------------
# R1 — FLAGON-1-1 measured + recorded (NOT gated)
# ---------------------------------------------------------------------------
if [[ -n "$SHARED_HEAP_OPT" ]]; then
    ON11_RESULTS="$(run_suite "flag-on-1-1" --useJSThreads=1 "$SHARED_HEAP_OPT=1")" \
        && paste <(echo "$OFF_RESULTS") <(echo "$ON11_RESULTS") | awk '
            { logsum += log($4 / $2); n++; printf "FLAGON-1-1 RATIO %s %.4f\n", $1, $4 / $2 }
            END { if (n) printf "FLAGON-1-1 GEOMEAN %.4f (RECORDED, not gated)\n", exp(logsum / n) }
        ' | while read -r l; do record_line "$l"; done \
        || record_line "FLAGON-1-1 SKIP: suite failed under --useSharedGCHeap=1 (record once heap config runs)"
else
    record_line "FLAGON-1-1 SKIP: --useSharedGCHeap not in this build (heap manifest 2 not applied)"
fi

# ---------------------------------------------------------------------------
# R2/R3 — recorded microbenches (fires/sec, shared-constructor)
# ---------------------------------------------------------------------------
for rec in fires-per-sec construction-shared-constructor; do
    for cfg in "off:--useJSThreads=0" "on:--useJSThreads=1"; do
        label="${cfg%%:*}"; flag="${cfg#*:}"
        lines="$("$JSC" "$flag" --useDollarVM=1 "$BENCH_HARNESS" "$SCRIPT_DIR/$rec.js" 2>/dev/null | grep '^BENCH ')" \
            && while read -r l; do record_line "RECORD[$label] $l"; done <<< "$lines" \
            || record_line "RECORD[$label] $rec SKIP (failed to run)"
    done
done

echo
if [[ "$FAILURES" -gt 0 ]]; then
    echo "bench-gates: $FAILURES GATED failure(s)" >&2
    exit 1
fi
echo "bench-gates: all gates green (recorded lines above${OUT:+, also in $OUT})"
