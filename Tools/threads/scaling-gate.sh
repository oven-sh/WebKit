#!/usr/bin/env bash
# scaling-gate.sh — parallel-scalability gate for the shared-memory threads
# work (suite: JSTests/threads/scaling/).
#
# The design's own success criterion (Pizlo) is near-linear scalability
# running a program in parallel with itself, with NO deliberate sharing. For
# each workload and each N in the thread sweep, this script runs N threads
# doing identical INDEPENDENT work, medians the wall time over several runs,
# and computes
#
#     speedup(N) = N * T(1) / T(N)
#
# (each thread does the same fixed work, so total work scales with N; perfect
# scaling is speedup(N) == N). It also runs a serial-identity check: T(1)
# under --useJSThreads=1 must be within SERIAL_IDENTITY_PCT of the flag-OFF
# time for the identical inline work (the threads feature must not tax
# single-threaded execution).
#
# REPORT-ONLY BY DEFAULT. This host is noisy and shared: numbers recorded
# here are for trend tracking, not pass/fail. Use --gate on a quiet machine
# to enforce the thresholds:
#
#     non-allocating / standard workloads:  speedup(4) >= 2.8, speedup(8) >= 4.5
#     splay-like (GC-pressure workload):    speedup(4) >= 2.0, speedup(8) >= 3.0
#
# splay-like's floor is relaxed because stop-the-world GC is a KNOWN SERIAL
# component until SPEC-congc (concurrent marking for N mutators) lands; its
# collections pause all N threads, so Amdahl caps its speedup well below the
# non-allocating workloads' even in a correct implementation. Revisit these
# floors upward when SPEC-congc is in.
#
# Usage:
#   Tools/threads/scaling-gate.sh [options] /path/to/jsc
#
# Options:
#   --gate              Enforce speedup + serial-identity thresholds (exit 1
#                       on violation). Default is report-only (always exit 0
#                       unless a workload ERRORS — wrong answers are never
#                       just "noise").
#   --runs K            Runs per (workload, N) cell; median taken (default: 3).
#   --threads "LIST"    Thread sweep (default: "1 2 4 8"). Must include 1.
#   --scale X           Work multiplier forwarded as SCALING_WORK_SCALE
#                       (default: 1; stretch runs on fast/quiet hosts).
#                       Validated numeric and in (0, 1000] — the harness's
#                       own clamp range; values > 100 (~100s/thread cells)
#                       additionally require SCALING_ALLOW_HEAVY_SCALE=1.
#   --identity-pct P    Serial-identity tolerance in percent (default: 5).
#   --skip-fairness     Skip the lock-fairness.js smoke at the end.
#   -h, --help          Show this help.
#
# Exit codes:
#   0  report-only completed (including a STARVATION-tagged lock-fairness
#      verdict, which report-only mode lists loudly as a would-fail finding
#      rather than exiting 2 — liveness verdicts are timing observations and
#      this host is noisy/shared), or --gate with all thresholds met
#   1  --gate threshold violation (speedup or serial identity)
#   2  usage / environment error (missing jsc, invalid option values; --gate
#      with a --threads sweep missing 4 or 8 — the only Ns with defined
#      floors), OR a workload crashed / produced a wrong answer in EITHER
#      mode (wrong answers are never just "noise"), OR a lock-fairness HARD
#      correctness assert (exclusion overlap / lost update / accounting)
#      tripped in EITHER mode, OR a lock-fairness STARVATION verdict under
#      --gate

set -u -o pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
SCALING_DIR="$ROOT_DIR/JSTests/threads/scaling"

RUNS=3
THREAD_LIST="1 2 4 8"
SCALE=1
GATE=0
IDENTITY_PCT=5
SKIP_FAIRNESS=0
JSC=""

# Per-cell timeout mirrors the corpus runner's per-test bound.
CELL_TIMEOUT_SECS="${SCALING_CELL_TIMEOUT_SECS:-120}"
TIMEOUT_WRAP=()
if [[ "$CELL_TIMEOUT_SECS" != "0" ]] && command -v timeout >/dev/null 2>&1; then
    TIMEOUT_WRAP=(timeout -k 10 "$CELL_TIMEOUT_SECS")
fi

usage() {
    awk 'NR == 1 { next } /^#/ { sub(/^# ?/, ""); print; next } { exit }' \
        "${BASH_SOURCE[0]}"
}

die() {
    echo "scaling-gate: error: $*" >&2
    exit 2
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --gate)          GATE=1; shift ;;
        --runs)          [[ $# -ge 2 ]] || die "--runs needs a value"; RUNS="$2"; shift 2 ;;
        --threads)       [[ $# -ge 2 ]] || die "--threads needs a value"; THREAD_LIST="$2"; shift 2 ;;
        --scale)         [[ $# -ge 2 ]] || die "--scale needs a value"; SCALE="$2"; shift 2 ;;
        --identity-pct)  [[ $# -ge 2 ]] || die "--identity-pct needs a value"; IDENTITY_PCT="$2"; shift 2 ;;
        --skip-fairness) SKIP_FAIRNESS=1; shift ;;
        -h|--help)       usage; exit 0 ;;
        -*)              die "unknown option: $1" ;;
        *)               [[ -z "$JSC" ]] || die "multiple jsc paths given"; JSC="$1"; shift ;;
    esac
done

[[ -n "$JSC" ]] || { usage >&2; die "path to jsc binary is required"; }
[[ -x "$JSC" ]] || die "jsc binary not found or not executable: $JSC"
[[ -d "$SCALING_DIR" ]] || die "scaling suite not found: $SCALING_DIR"
[[ "$RUNS" =~ ^[0-9]+$ && "$RUNS" -ge 1 ]] || die "--runs must be a positive integer"
# --scale is interpolated into a jsc -e snippet and silently clamped by the
# harness if out of range; reject anything non-numeric or outside the
# harness's own (0, 1000] clamp BEFORE running a sweep, so the recorded
# parameters always equal the effective ones (and no arbitrary JS rides in).
[[ "$SCALE" =~ ^[0-9]*\.?[0-9]+$ ]] || die "--scale must be a plain non-negative number (got: $SCALE)"
awk -v s="$SCALE" 'BEGIN { exit !(s > 0 && s <= 1000) }' \
    || die "--scale must be in (0, 1000] — the harness clamp range (got: $SCALE)"
# Heavy-mode guard: >100 means >100s per thread per cell across the whole
# sweep; require an explicit env override rather than accepting it silently.
if awk -v s="$SCALE" 'BEGIN { exit !(s > 100) }'; then
    [[ "${SCALING_ALLOW_HEAVY_SCALE:-0}" == "1" ]] \
        || die "--scale $SCALE exceeds 100 (~${SCALE}s/thread per cell); set SCALING_ALLOW_HEAVY_SCALE=1 to confirm"
fi
[[ "$IDENTITY_PCT" =~ ^[0-9]*\.?[0-9]+$ ]] || die "--identity-pct must be a plain non-negative number (got: $IDENTITY_PCT)"
[[ "$CELL_TIMEOUT_SECS" =~ ^[0-9]+$ ]] || die "SCALING_CELL_TIMEOUT_SECS must be a non-negative integer (got: $CELL_TIMEOUT_SECS)"
# Split the user-facing space-separated list into an array ONCE; everything
# downstream iterates the array (keeps shellcheck clean of SC2086 and
# validates each entry exactly once).
read -ra THREAD_NS <<< "$THREAD_LIST"
[[ ${#THREAD_NS[@]} -gt 0 ]] || die "--threads list must not be empty"
HAS_ONE=0
for n in "${THREAD_NS[@]}"; do
    [[ "$n" =~ ^[0-9]+$ && "$n" -ge 1 ]] || die "--threads entries must be positive integers (got: $n)"
    [[ "$n" == "1" ]] && HAS_ONE=1
done
[[ "$HAS_ONE" -eq 1 ]] || die "--threads list must include 1 (speedup is relative to T(1))"
# Mirror of the heavy-mode guard, in the other direction: --gate must not be
# able to enforce NOTHING. Speedup floors are defined only at N=4 and N=8, so
# a gate sweep that omits either evaluates fewer floors than the pinned rung
# while still printing PASS — a green that gated nothing. Partial sweeps are
# what report-only mode is for.
if [[ "$GATE" -eq 1 ]]; then
    HAS_FOUR=0
    HAS_EIGHT=0
    for n in "${THREAD_NS[@]}"; do
        [[ "$n" == "4" ]] && HAS_FOUR=1
        [[ "$n" == "8" ]] && HAS_EIGHT=1
    done
    [[ "$HAS_FOUR" -eq 1 && "$HAS_EIGHT" -eq 1 ]] \
        || die "--gate requires the --threads sweep to include both 4 and 8 (speedup floors are defined only there; got: \"$THREAD_LIST\") — use report-only mode for partial sweeps"
fi

# The flag must exist in this build, and the build must speak the harness
# protocol at all; probe cheaply before burning a sweep.
"$JSC" --useJSThreads=1 -e '' >/dev/null 2>&1 \
    || die "this jsc build rejects --useJSThreads=1 (wrong build for this gate)"

WORKLOADS=()
for f in "$SCALING_DIR"/*.js; do
    base="$(basename "$f")"
    [[ "$base" == "harness.js" || "$base" == "lock-fairness.js" ]] && continue
    WORKLOADS+=("$f")
done
[[ ${#WORKLOADS[@]} -gt 0 ]] || die "no workloads found in $SCALING_DIR"

# Per-workload --gate floors. splay-like is the GC-pressure workload: STW
# collections are a known serial component until SPEC-congc lands (see
# header), so its floor is relaxed.
floor4_for() { case "$1" in splay-like) echo 2.0 ;; *) echo 2.8 ;; esac; }
floor8_for() { case "$1" in splay-like) echo 3.0 ;; *) echo 4.5 ;; esac; }

# run_cell <file> <nthreads> <flag:on|off> -> echoes median ms or returns 1.
# Thread count and work scale ride in via -e before the workload file; the
# //@ requireOptions header is a corpus-runner channel, so the flag is passed
# explicitly here.
run_cell() {
    local file="$1" n="$2" flag="$3"
    local args=()
    [[ "$flag" == "on" ]] && args+=("--useJSThreads=1")
    args+=(-e "globalThis.SCALING_THREADS=$n; globalThis.SCALING_WORK_SCALE=$SCALE;")
    local samples=() out line ms i
    for ((i = 0; i < RUNS; ++i)); do
        out="$(${TIMEOUT_WRAP[@]+"${TIMEOUT_WRAP[@]}"} "$JSC" "${args[@]}" "$file" 2>&1)" || {
            echo "scaling-gate: $file (N=$n, flag=$flag) FAILED:" >&2
            printf '%s\n' "$out" | sed 's/^/    | /' >&2
            return 1
        }
        line="$(printf '%s\n' "$out" | grep '^SCALING ' | tail -n 1)"
        [[ -n "$line" ]] || {
            echo "scaling-gate: $file (N=$n, flag=$flag) produced no SCALING line; output:" >&2
            printf '%s\n' "$out" | sed 's/^/    | /' >&2
            return 1
        }
        ms="$(printf '%s\n' "$line" | awk '{ print $4 }')"
        samples+=("$ms")
    done
    printf '%s\n' "${samples[@]}" | sort -n | awk '
        { v[NR] = $1 }
        END {
            if (NR % 2 == 1)
                printf "%.3f\n", v[(NR + 1) / 2];
            else
                printf "%.3f\n", (v[NR / 2] + v[NR / 2 + 1]) / 2;
        }'
}

MODE_LABEL="REPORT-ONLY (host is noisy/shared; recording, not gating)"
[[ "$GATE" -eq 1 ]] && MODE_LABEL="GATE (thresholds enforced)"

echo "scaling-gate: jsc        = $JSC"
echo "scaling-gate: mode       = $MODE_LABEL"
echo "scaling-gate: runs/cell  = $RUNS (median)"
echo "scaling-gate: threads    = $THREAD_LIST"
echo "scaling-gate: work scale = $SCALE"
echo "scaling-gate: serial-identity tolerance = ${IDENTITY_PCT}%"

VIOLATIONS=()
FLOORS_EVALUATED=()

for file in "${WORKLOADS[@]}"; do
    name="$(basename "$file" .js)"
    echo
    echo "== $name =="

    # Sweep T(N) flag-on.
    declare -a NS=() TS=()
    T1=""
    for n in "${THREAD_NS[@]}"; do
        t="$(run_cell "$file" "$n" on)" || die "workload $name failed at N=$n (wrong answers are never noise)"
        NS+=("$n")
        TS+=("$t")
        [[ "$n" == "1" ]] && T1="$t"
    done
    [[ -n "$T1" ]] || die "internal: no T(1) for $name"

    # Serial identity: flag-off inline run of the identical work.
    TOFF="$(run_cell "$file" 1 off)" || die "workload $name failed flag-off (serial-identity leg)"

    printf '  %-8s %12s %12s %10s\n' "threads" "T(N) ms" "ideal ms" "speedup"
    for ((i = 0; i < ${#NS[@]}; ++i)); do
        n="${NS[$i]}"
        t="${TS[$i]}"
        awk -v n="$n" -v t="$t" -v t1="$T1" 'BEGIN {
            speedup = (t > 0) ? n * t1 / t : 0;
            printf "  %-8s %12.3f %12.3f %9.2fx\n", n, t, t1, speedup;
        }'
    done

    idline="$(awk -v on="$T1" -v off="$TOFF" -v tol="$IDENTITY_PCT" 'BEGIN {
        delta = (off > 0) ? (on - off) / off * 100.0 : 0;
        verdict = (delta < 0 ? -delta : delta) <= tol ? "ok" : "VIOLATION";
        printf "%s %.2f", verdict, delta;
    }')"
    idverdict="${idline%% *}"
    iddelta="${idline##* }"
    echo "  serial identity: T(1) flag-on ${T1} ms vs flag-off ${TOFF} ms (${iddelta}% vs +/-${IDENTITY_PCT}%) ${idverdict}"
    [[ "$idverdict" == "VIOLATION" ]] && VIOLATIONS+=("$name: serial identity ${iddelta}% exceeds +/-${IDENTITY_PCT}%")

    # Threshold checks at N=4 and N=8 (only for Ns actually swept).
    floor4="$(floor4_for "$name")"
    floor8="$(floor8_for "$name")"
    for ((i = 0; i < ${#NS[@]}; ++i)); do
        n="${NS[$i]}"
        t="${TS[$i]}"
        floor=""
        [[ "$n" == "4" ]] && floor="$floor4"
        [[ "$n" == "8" ]] && floor="$floor8"
        [[ -n "$floor" ]] || continue
        verdict="$(awk -v n="$n" -v t="$t" -v t1="$T1" -v f="$floor" 'BEGIN {
            speedup = (t > 0) ? n * t1 / t : 0;
            printf "%s %.2f", (speedup >= f) ? "ok" : "VIOLATION", speedup;
        }')"
        status="${verdict%% *}"
        sp="${verdict##* }"
        note=""
        [[ "$name" == "splay-like" ]] && note=" [relaxed floor: STW GC serial until SPEC-congc]"
        echo "  threshold: speedup($n) = ${sp}x vs floor ${floor}x ${status}${note}"
        FLOORS_EVALUATED+=("$name@$n")
        [[ "$status" == "VIOLATION" ]] && VIOLATIONS+=("$name: speedup($n) = ${sp}x < ${floor}x")
    done
done

# lock-fairness smoke: correctness (no starvation, no exclusion overlap, no
# lost updates), not throughput; one run. HARD correctness asserts (exclusion
# overlap, lost updates, checksum/accounting mismatches) are WRONG ANSWERS
# and fatal in EITHER mode, per this script's own "wrong answers are never
# noise" rule. STARVATION/liveness verdicts are different in kind: even with
# the test's in-test corroboration and host-stall grace, they remain timing
# observations on a host this script's own header declares noisy and shared —
# a starvation verdict that exit-2's a report-only trend run would teach
# people to dismiss the one channel that detects lost wakeups. So in
# report-only mode a STARVATION-tagged failure is reported LOUDLY as a
# would-fail finding (and listed in the summary) but does not exit 2; under
# --gate it is fatal like everything else.
if [[ "$SKIP_FAIRNESS" -eq 0 && -f "$SCALING_DIR/lock-fairness.js" ]]; then
    echo
    echo "== lock-fairness (smoke) =="
    if out="$(${TIMEOUT_WRAP[@]+"${TIMEOUT_WRAP[@]}"} "$JSC" --useJSThreads=1 \
            -e "globalThis.SCALING_THREADS=8;" "$SCALING_DIR/lock-fairness.js" 2>&1)"; then
        printf '%s\n' "$out" | grep '^lock-fairness' | sed 's/^/  /'
        echo "  lock-fairness: ok"
    elif [[ "$GATE" -eq 0 ]] && printf '%s\n' "$out" | grep -q 'STARVATION'; then
        printf '%s\n' "$out" | sed 's/^/    | /'
        echo "  lock-fairness: STARVATION verdict in report-only mode — recorded as a"
        echo "  would-fail finding (NOT exit 2 here: liveness verdicts are timing"
        echo "  observations and this host is noisy/shared; rerun under --gate on a"
        echo "  quiet machine to enforce — repeated verdicts there are real)"
        VIOLATIONS+=("lock-fairness: STARVATION verdict (see output above; fatal under --gate)")
    else
        printf '%s\n' "$out" | sed 's/^/    | /'
        die "lock-fairness smoke FAILED (exclusion/lost-update/accounting assert tripped, or starvation under --gate — fatal)"
    fi
fi

echo
if [[ ${#VIOLATIONS[@]} -gt 0 ]]; then
    if [[ "$GATE" -eq 1 ]]; then
        echo "scaling-gate: FAIL — threshold violations:"
        printf '  %s\n' "${VIOLATIONS[@]}"
        exit 1
    fi
    echo "scaling-gate: report-only — the following WOULD fail under --gate (host noise caveat applies):"
    printf '  %s\n' "${VIOLATIONS[@]}"
    echo "scaling-gate: REPORT COMPLETE (not gating; rerun with --gate on a quiet machine to enforce)"
    exit 0
fi

if [[ "$GATE" -eq 1 ]]; then
    FAIRNESS_NOTE="fairness smoke met"
    [[ "$SKIP_FAIRNESS" -eq 1 ]] && FAIRNESS_NOTE="fairness smoke SKIPPED (--skip-fairness)"
    echo "scaling-gate: PASS — ${#FLOORS_EVALUATED[@]} speedup floors evaluated (${FLOORS_EVALUATED[*]}), serial identity within +/-${IDENTITY_PCT}%, ${FAIRNESS_NOTE}"
else
    echo "scaling-gate: REPORT COMPLETE — no would-fail findings this run (still report-only)"
fi
exit 0
