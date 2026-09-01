#!/usr/bin/env bash
# bench-gate.sh — serial-performance gate for the shared-memory threads work.
#
# Runs the microbenchmark suite in JSTests/threads/bench/ K times against a
# given jsc binary, takes the per-benchmark median, and compares it against
# Tools/threads/baseline.json. The gate FAILS if any benchmark's median is
# more than REGRESSION_THRESHOLD_PCT slower than its baseline median.
#
# Usage:
#   Tools/threads/bench-gate.sh [options] /path/to/jsc
#
# Options:
#   --record            (Re)write baseline.json from this run instead of
#                       comparing. The Verify phase records the baseline.
#   --runs K            Number of runs per benchmark (default: 9).
#   --baseline FILE     Baseline file (default: Tools/threads/baseline.json).
#   --threshold PCT     Allowed regression in percent (default: 1).
#   -h, --help          Show this help.
#
# Exit codes:
#   0  all benchmarks within threshold (or baseline recorded)
#   1  at least one benchmark regressed beyond the threshold
#   2  usage / environment error (missing jsc, missing baseline, bench failed)

set -u -o pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
BENCH_DIR="$ROOT_DIR/JSTests/threads/bench"
HARNESS="$BENCH_DIR/harness.js"

RUNS=9
BASELINE="$SCRIPT_DIR/baseline.json"
THRESHOLD_PCT=1
RECORD=0
JSC=""

usage() {
    sed -n '2,22p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

die() {
    echo "bench-gate: error: $*" >&2
    exit 2
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --record)    RECORD=1; shift ;;
        --runs)      [[ $# -ge 2 ]] || die "--runs needs a value"; RUNS="$2"; shift 2 ;;
        --baseline)  [[ $# -ge 2 ]] || die "--baseline needs a value"; BASELINE="$2"; shift 2 ;;
        --threshold) [[ $# -ge 2 ]] || die "--threshold needs a value"; THRESHOLD_PCT="$2"; shift 2 ;;
        -h|--help)   usage; exit 0 ;;
        -*)          die "unknown option: $1" ;;
        *)           [[ -z "$JSC" ]] || die "multiple jsc paths given"; JSC="$1"; shift ;;
    esac
done

[[ -n "$JSC" ]] || { usage >&2; die "path to jsc binary is required"; }
[[ -x "$JSC" ]] || die "jsc binary not found or not executable: $JSC"
[[ -f "$HARNESS" ]] || die "harness not found: $HARNESS"
[[ "$RUNS" =~ ^[0-9]+$ && "$RUNS" -ge 1 ]] || die "--runs must be a positive integer"

BENCH_FILES=()
for f in "$BENCH_DIR"/*.js; do
    [[ "$(basename "$f")" == "harness.js" ]] && continue
    BENCH_FILES+=("$f")
done
[[ ${#BENCH_FILES[@]} -gt 0 ]] || die "no benchmarks found in $BENCH_DIR"

if [[ "$RECORD" -eq 0 && ! -f "$BASELINE" ]]; then
    die "baseline not found: $BASELINE (run with --record first)"
fi

# run_bench <file> -> echoes milliseconds, or returns nonzero.
run_bench() {
    local file="$1" out line
    out="$("$JSC" "$HARNESS" "$file" 2>&1)" || {
        echo "bench-gate: $file failed:" >&2
        echo "$out" >&2
        return 1
    }
    line="$(printf '%s\n' "$out" | grep '^BENCH ' | tail -n 1)"
    [[ -n "$line" ]] || {
        echo "bench-gate: $file produced no BENCH line; output was:" >&2
        echo "$out" >&2
        return 1
    }
    printf '%s\n' "$line" | awk '{ print $3 }'
}

# median <space-separated values> -> echoes median (one decimal place).
median() {
    printf '%s\n' "$@" | sort -n | awk '
        { v[NR] = $1 }
        END {
            if (NR % 2 == 1)
                printf "%.3f\n", v[(NR + 1) / 2];
            else
                printf "%.3f\n", (v[NR / 2] + v[NR / 2 + 1]) / 2;
        }'
}

# baseline_value <name> -> echoes baseline median or empty.
baseline_value() {
    awk -v name="$1" '
        $0 ~ "\"" name "\"[[:space:]]*:" {
            line = $0
            sub(/^[^:]*:[[:space:]]*/, "", line)
            sub(/[,[:space:]]*$/, "", line)
            print line
            exit
        }' "$BASELINE"
}

echo "bench-gate: jsc = $JSC"
echo "bench-gate: runs per benchmark = $RUNS"
echo "bench-gate: threshold = ${THRESHOLD_PCT}%"
echo

declare -a NAMES MEDIANS
FAILED=0

for file in "${BENCH_FILES[@]}"; do
    name="$(basename "$file" .js)"
    samples=()
    for ((i = 0; i < RUNS; ++i)); do
        ms="$(run_bench "$file")" || die "benchmark $name failed to run"
        samples+=("$ms")
    done
    med="$(median "${samples[@]}")"
    NAMES+=("$name")
    MEDIANS+=("$med")

    if [[ "$RECORD" -eq 1 ]]; then
        printf '  %-32s median %8s ms   (samples: %s)\n' "$name" "$med" "${samples[*]}"
        continue
    fi

    base="$(baseline_value "$name")"
    if [[ -z "$base" ]]; then
        echo "bench-gate: error: no baseline entry for $name (re-run with --record)" >&2
        FAILED=1
        continue
    fi

    verdict="$(awk -v m="$med" -v b="$base" -v t="$THRESHOLD_PCT" 'BEGIN {
        limit = b * (1 + t / 100.0);
        delta = (b > 0) ? (m - b) / b * 100.0 : 0;
        printf "%s %.2f", (m > limit) ? "FAIL" : "ok", delta;
    }')"
    status="${verdict%% *}"
    delta="${verdict##* }"
    printf '  %-32s median %8s ms   baseline %8s ms   %+6s%%   %s\n' \
        "$name" "$med" "$base" "$delta" "$status"
    [[ "$status" == "FAIL" ]] && FAILED=1
done

echo

if [[ "$RECORD" -eq 1 ]]; then
    {
        echo '{'
        echo '  "meta": {'
        echo "    \"recorded_at\": \"$(date -u +%Y-%m-%dT%H:%M:%SZ)\","
        echo "    \"runs\": $RUNS,"
        echo "    \"jsc\": \"$JSC\","
        echo "    \"host\": \"$(uname -sm)\""
        echo '  },'
        echo '  "benchmarks": {'
        for ((i = 0; i < ${#NAMES[@]}; ++i)); do
            sep=','
            [[ $i -eq $((${#NAMES[@]} - 1)) ]] && sep=''
            echo "    \"${NAMES[$i]}\": ${MEDIANS[$i]}$sep"
        done
        echo '  }'
        echo '}'
    } > "$BASELINE"
    echo "bench-gate: baseline recorded to $BASELINE"
    exit 0
fi

if [[ "$FAILED" -ne 0 ]]; then
    echo "bench-gate: FAIL — at least one benchmark regressed more than ${THRESHOLD_PCT}% vs $BASELINE"
    exit 1
fi

echo "bench-gate: PASS — all benchmarks within ${THRESHOLD_PCT}% of baseline"
exit 0
