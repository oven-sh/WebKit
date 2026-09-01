#!/usr/bin/env bash
# soak4.sh — EXPERIMENTER round 4 consolidated soak under the WD-4 gate.
# Generalization of wd4-hangpin.sh to arbitrary workloads on the stock
# landed-fix jsc. PASS = rc 0. rc 134 + "failed to reach a stopped world"
# = WATCHDOG-ABORT. Alive past HANG_T seconds = SILENT-HANG: gdb all-thread
# dump captured BEFORE SIGKILL, classified FAILURE (never flake).
#
# Usage: soak4.sh <runs> <tag> <hang_t_seconds> [--sample-first K] -- <jsc argv...>
#   --sample-first K: for the first K runs, additionally take a NON-fatal
#   mid-run gdb all-thread dump at T+SAMPLE_AT (default 12s) without killing
#   the process (NPR4-A confirmIf mid-run stack sampling).
set -u
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RUNS="$1"; TAG="$2"; HANG_T="$3"; shift 3
SAMPLE_FIRST=0
if [ "${1:-}" = "--sample-first" ]; then SAMPLE_FIRST="$2"; shift 2; fi
[ "${1:-}" = "--" ] && shift
SAMPLE_AT="${SAMPLE_AT:-12}"
mkdir -p "$DIR/logs" "$DIR/bts"
pass=0; abort=0; hang=0; other=0
for i in $(seq 1 "$RUNS"); do
    LOG="$DIR/logs/$TAG-run$i.log"
    "$@" >"$LOG" 2>&1 &
    pid=$!
    start=$(date +%s)
    sampled=0
    verdict=""
    while true; do
        if ! kill -0 "$pid" 2>/dev/null; then
            wait "$pid"; rc=$?
            if [ $rc -eq 0 ]; then verdict=PASS; pass=$((pass+1));
            elif [ $rc -eq 134 ]; then
                if grep -q "failed to reach a stopped world" "$LOG"; then verdict=WATCHDOG-ABORT; else verdict="OTHER-ABORT"; fi
                abort=$((abort+1))
            else verdict="OTHER(rc=$rc)"; other=$((other+1)); fi
            break
        fi
        now=$(date +%s)
        if [ "$i" -le "$SAMPLE_FIRST" ] && [ $sampled -eq 0 ] && [ $((now - start)) -ge "$SAMPLE_AT" ]; then
            gdb -batch -ex 'set pagination off' -ex 'thread apply all bt 25' -p "$pid" \
                >"$DIR/bts/$TAG-midrun-run$i.bt" 2>&1
            sampled=1
        fi
        if [ $((now - start)) -ge "$HANG_T" ]; then
            BT="$DIR/bts/$TAG-hang-run$i-pid$pid.bt"
            gdb -batch -ex 'set pagination off' -ex 'thread apply all bt 40' -p "$pid" >"$BT" 2>&1
            kill -9 "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
            verdict="SILENT-HANG (bt: $BT)"; hang=$((hang+1))
            break
        fi
        sleep 1
    done
    dur=$(( $(date +%s) - start ))
    echo "run $i: ${dur}s $verdict"
done
echo "SUMMARY tag=$TAG runs=$RUNS pass=$pass watchdog_abort=$abort silent_hang=$hang other=$other"
