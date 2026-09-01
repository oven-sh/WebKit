#!/usr/bin/env bash
# wd4-hangpin.sh — WD-4 silent-hang pinning loop (EXPERIMENTER round 2).
# Runs jsc-bughunt-expB (H1 fire restored + stop-aware victim-wait probe) on
# repro.js N=4. PASS exits <1s; watchdog aborts exit at ~30s. Anything still
# alive at T+60s is a WD-4 silent hang: attach gdb, dump all threads, THEN kill.
set -u
JSC=/root/WebKit/WebKitBuild/Release/bin/jsc-bughunt-expB
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RUNS="${1:-30}"
FLAGS=(--useJSThreads=1 --useThreadGIL=0 --useVMLite=1 --useSharedAtomStringTable=1 --useSharedGCHeap=1 --useThreadGILOffUnsafe=1)
mkdir -p "$DIR/logs" "$DIR/bts"
pass=0; abort=0; hang=0; other=0
for i in $(seq 1 "$RUNS"); do
    LOG="$DIR/logs/wd4-run$i.log"
    "$JSC" "${FLAGS[@]}" "$DIR/../repro.js" >"$LOG" 2>&1 &
    pid=$!
    start=$(date +%s)
    verdict=""
    while true; do
        if ! kill -0 "$pid" 2>/dev/null; then
            wait "$pid"; rc=$?
            if [ $rc -eq 0 ]; then verdict=PASS; pass=$((pass+1));
            elif [ $rc -eq 134 ]; then verdict=WATCHDOG-ABORT; abort=$((abort+1));
            else verdict="OTHER(rc=$rc)"; other=$((other+1)); fi
            break
        fi
        now=$(date +%s)
        if [ $((now - start)) -ge 60 ]; then
            # Silent hang: pin it with gdb BEFORE killing.
            BT="$DIR/bts/wd4-hang-run$i-pid$pid.bt"
            gdb -batch -ex 'set pagination off' -ex 'thread apply all bt 40' -p "$pid" >"$BT" 2>&1
            kill -9 "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
            verdict="SILENT-HANG (bt: $BT)"; hang=$((hang+1))
            break
        fi
        sleep 2
    done
    dur=$(( $(date +%s) - start ))
    echo "run $i: ${dur}s $verdict"
done
echo "SUMMARY wd4 runs=$RUNS pass=$pass abort=$abort silent_hang=$hang other=$other"
