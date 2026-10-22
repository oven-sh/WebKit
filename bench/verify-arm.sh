#!/bin/bash
# Checks a new build of the PR. usage: verify-arm.sh <arm> <out prefix>
F=/workspace/bins/$1/bun; M=/workspace/bins/main/bun; P=/workspace/bins/final3/bun; O=/tmp/$2; R=/tmp/repro; Z=BUN_JSC_minimumBytesMarkedForParallelMarking=0
cd /workspace/bun-pr
for i in 1 2 3; do $F test test/js/bun/gc/gc-controller-cadence.test.ts -t "marking helper threads" 2>&1 | grep -E "^ +[0-9]+ (pass|fail)|^\(fail\)" | tr '\n' ' '; echo; done > $O-tests.txt 2>&1
for t in test/js/web/abort/abort-controller-gc-reason.test.ts test/js/web/broadcastchannel/broadcast-channel-worker-gc.test.ts test/js/bun/resolve/dynamic-import-evaluation-error-gc.test.ts test/js/node/module/module-children-concurrent-gc.test.ts test/regression/issue/29519.test.ts test/js/bun/gc/gc-controller-cadence.test.ts; do echo "$t: $($F test $t 2>&1 | grep -E "^ +[0-9]+ (pass|fail|skip)" | tr '\n' ' ')"; done >> $O-tests.txt 2>&1
cd $R
$F ab.mjs 5 $R/lru-gc.mjs "500" main=$M prev=$P new=$F new-p0=$F,$Z new-one=$F,BUN_JSC_numberOfGCMarkers=1 > $O-A.txt 2>&1
$F ab.mjs 5 $R/shapes.mjs "tree 100" main=$M prev=$P new=$F new-p0=$F,$Z > $O-tree.txt 2>&1
$F ab.mjs 5 $R/shapes.mjs "list 100" main=$M prev=$P new=$F new-p0=$F,$Z >> $O-tree.txt 2>&1
$F eden-matrix.mjs 5 2,8 main=$M prev=$P new=$F new-p0=$F,$Z > $O-E.txt 2>&1
$F app-matrix.mjs $R/bench-app.mjs 5 "20000 8 100" main=$M prev=$P new=$F new-p0=$F,$Z > $O-D2.txt 2>&1
echo DONE > $O-done.txt
