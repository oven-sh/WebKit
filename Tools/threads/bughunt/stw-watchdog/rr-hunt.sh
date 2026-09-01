#!/usr/bin/env bash
# rr-hunt.sh — record richards-like under rr --chaos until a watchdog abort is captured.
set -u
export LD_LIBRARY_PATH=/usr/local/lib
JSC=/root/WebKit/WebKitBuild/Release/bin/jsc
OUTDIR=/root/WebKit/Tools/threads/bughunt/stw-watchdog/rr-traces
mkdir -p "$OUTDIR"
N="${N:-4}"; SCALE="${SCALE:-0.0625}"; MAX="${MAX:-60}"
for i in $(seq 1 "$MAX"); do
    T="$OUTDIR/try-$i"
    timeout -k 10 200 rr record --chaos -o "$T" "$JSC" \
        --useJSThreads=1 --useThreadGIL=0 --useVMLite=1 --useSharedAtomStringTable=1 \
        --useSharedGCHeap=1 --useThreadGILOffUnsafe=1 \
        -e "globalThis.SCALING_THREADS=$N; globalThis.SCALING_WORK_SCALE=$SCALE;" \
        /root/WebKit/JSTests/threads/scaling/richards-like.js > "$T.log" 2>&1
    rc=$?
    if [ $rc -eq 134 ] && grep -q "failed to reach a stopped world" "$T.log"; then
        echo "CAPTURED watchdog abort in $T (run $i)"
        mv "$T" "$OUTDIR/rr-stw-watchdog-abort"
        mv "$T.log" "$OUTDIR/rr-stw-watchdog-abort.log"
        exit 0
    fi
    echo "try $i: rc=$rc (no watchdog abort)"
    rm -rf "$T" "$T.log"
done
echo "NO CAPTURE in $MAX tries"
exit 1
