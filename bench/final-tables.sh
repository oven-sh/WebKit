#!/bin/bash
# All tables for the PR bodies with one build per arm. usage: final-tables.sh <final arm dir name> <out prefix>
F=/workspace/bins/$1/bun; M=/workspace/bins/main/bun; W=/workspace/bins/wk800/bun; O=/tmp/$2; R=/tmp/repro; B=$F
cd $R
$B ab.mjs 7 $R/lru-gc.mjs "500" main=$M wk800=$W final=$F final-one=$F,BUN_JSC_numberOfGCMarkers=1 main-one=$M,BUN_JSC_numberOfGCMarkers=1 node=/usr/local/bin/node > $O-A.txt 2>&1
for a in "100 16384" "25 65536" "12 131072"; do $B ab.mjs 5 $R/lru-gc.mjs "$a" main=$M final=$F one=$F,BUN_JSC_numberOfGCMarkers=1 node=/usr/local/bin/node; done > $O-B.txt 2>&1
for a in "500 8 3200" "4000 8 400" "16000 8 100"; do $B app-matrix.mjs $R/bench-app.mjs 7 "$a" main=$M wk800=$W final=$F one=$F,BUN_JSC_numberOfGCMarkers=1 node=/usr/local/bin/node; done > $O-C.txt 2>&1
$B churn-phases.mjs 9 "$R/bench-app.mjs 20000 8 100" main=$M wk800=$W final=$F one=$F,BUN_JSC_numberOfGCMarkers=1 > $O-D.txt 2>&1
$B app-matrix.mjs $R/bench-app.mjs 7 "20000 8 100" main=$M wk800=$W final=$F one=$F,BUN_JSC_numberOfGCMarkers=1 > $O-D2.txt 2>&1
$B eden-matrix.mjs 7 2,8,32 main=$M wk800=$W final=$F one=$F,BUN_JSC_numberOfGCMarkers=1 > $O-E.txt 2>&1
for s in tree records map wide list; do $B ab.mjs 9 $R/shapes.mjs "$s 100" main=$M wk800=$W final=$F one=$F,BUN_JSC_numberOfGCMarkers=1; done > $O-F.txt 2>&1
$B ab.mjs 5 $R/list.mjs "2000000" main=$M wk800=$W final=$F >> $O-F.txt 2>&1
for a in "4 50" "12 25"; do $B ab.mjs 5 $R/workers-lru.mjs "$a" main=$M final=$F one=$F,BUN_JSC_numberOfGCMarkers=1; done > $O-W.txt 2>&1
for a in "targets 30" "weakmaps 30"; do MIN_MB=0 $B churn-phases.mjs 5 "$R/weak-heavy.mjs $a" main=$M final=$F one=$F,BUN_JSC_numberOfGCMarkers=1; done > $O-K.txt 2>&1
echo DONE > $O-done.txt
