#!/bin/bash
# Session 2026-06-11d verification:
#  leg S: 10x ASAN Debug W=16 nojit N=400 smoke (the A2 signature profile), waves of 5
#  leg J: JIT-on ASAN coverage of getHostCallReturnValueThunk/RepatchInlines: W=4 x10 + W=8 x10, waves
#  leg G: GIL-on JIT-on identity arm, W=4 smoke x3
set -u
cd /root/WebKit
C=Tools/threads/bughunt/w16/campaign.sh
bash $C a2amend-sig1 "16:400:5:nojit:--useJIT=0"
bash $C a2amend-sig2 "16:400:5:nojit:--useJIT=0"
bash $C a2amend-jit1 "4:400:5:jiton" "8:400:5:jiton"
bash $C a2amend-jit2 "4:400:5:jiton" "8:400:5:jiton"
# GIL-on arm (manual flags; campaign.sh pins GIL-off)
OUT=Tools/threads/bughunt/w16/rounds/a2amend-gilon; mkdir -p $OUT
export ASAN_OPTIONS="abort_on_error=0:fast_unwind_on_malloc=0:malloc_context_size=30:detect_leaks=0:handle_segv=1:handle_abort=1:exitcode=99"
for i in 1 2 3; do
  log=$OUT/W4-N400-gilon-$i.log
  timeout 720 WebKitBuild/Debug/bin/jsc --useJSThreads=1 --useThreadGIL=1 --useVMLite=1 Tools/threads/bughunt/w16/bench-local.js -- 4 smoke 400 > $log 2>&1
  rc=$?; echo "RC=$rc" >> $log
  if [ "$rc" = "0" ]; then rm -f $log; echo "[gilon] #$i PASS"; else echo "[gilon] #$i FAIL rc=$rc"; fi
done
echo ALLDONE
