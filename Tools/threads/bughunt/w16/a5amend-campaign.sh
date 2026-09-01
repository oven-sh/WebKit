#!/bin/bash
# 2026-06-11 A5 amend round verification (post adversarial-review amendments:
# gilOffWithProcessGate accessor switch + comment/ledger corrections).
#  leg S: 10x ASAN Debug nojit N=400 smoke at the A5 signature W mix
#         (W16 x6 + W32 x2 + W8 x2), two waves of 5 — the pinned A5 gate.
#  leg H: a5-megamorphic-hammer re-cover of the amended gates:
#         ASAN JIT-on GIL-off x3, ASAN nojit GIL-off x2, ASAN flag-off x1.
#  leg R: Release identity arms (flag-off / GIL-on / GIL-off jit / GIL-off nojit).
set -u
cd /root/WebKit
C=Tools/threads/bughunt/w16/campaign.sh
bash $C a5amend-w1 "16:400:3:nojit:--useJIT=0" "32:400:1:nojit:--useJIT=0" "8:400:1:nojit:--useJIT=0"
bash $C a5amend-w2 "16:400:3:nojit:--useJIT=0" "32:400:1:nojit:--useJIT=0" "8:400:1:nojit:--useJIT=0"

HAM=Tools/threads/bughunt/w16/a5-megamorphic-hammer.js
GILOFF="--useJSThreads=1 --useThreadGIL=0 --useVMLite=1 --useSharedAtomStringTable=1 --useSharedGCHeap=1 --useThreadGILOffUnsafe=1"
OUT=Tools/threads/bughunt/w16/rounds/a5amend-hammer; mkdir -p $OUT
export ASAN_OPTIONS="abort_on_error=0:fast_unwind_on_malloc=0:malloc_context_size=30:detect_leaks=0:handle_segv=1:handle_abort=1:exitcode=99"
run_ham() { # label flags...
  local label=$1; shift
  local log=$OUT/$label.log
  timeout 900 WebKitBuild/Debug/bin/jsc "$@" $HAM > $log 2>&1
  local rc=$?
  if [ "$rc" = "0" ] && grep -q "A5-HAMMER-PASS" $log && ! grep -q "MISMATCH" $log; then
    echo "[hammer] $label PASS $(grep -o 'checksum=[0-9a-f]*' $log | head -1)"; else
    echo "[hammer] $label FAIL rc=$rc"; fi
}
for i in 1 2 3; do run_ham jiton-giloff-$i $GILOFF; done
for i in 1 2; do run_ham nojit-giloff-$i $GILOFF --useJIT=0; done
run_ham flagoff-1

RJ=WebKitBuild/Release/bin/jsc
for arm in "flagoff:" "gilon:--useJSThreads=1 --useThreadGIL=1 --useVMLite=1" "giloff-jit:$GILOFF" "giloff-nojit:$GILOFF --useJIT=0"; do
  label=${arm%%:*}; flags=${arm#*:}
  log=$OUT/rel-$label.log
  timeout 300 $RJ $flags $HAM > $log 2>&1
  rc=$?
  cs=$(grep -o 'checksum=[0-9a-f]*' $log | head -1)
  echo "[rel] $label rc=$rc $cs $(grep -c MISMATCH $log) mismatches"
done
echo ALLDONE
