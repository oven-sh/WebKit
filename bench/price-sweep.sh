#!/bin/bash
# The price of a wake-up (BUN_JSC_minimumBytesMarkedForParallelMarking) on the final build. usage: price-sweep.sh <arm> <out prefix>
F=/workspace/bins/$1/bun; O=/tmp/$2; R=/tmp/repro; P=BUN_JSC_minimumBytesMarkedForParallelMarking
arms="p0=$F,$P=0 p512k=$F,$P=524288 p1m=$F,$P=1048576 p2m=$F p4m=$F,$P=4194304 p8m=$F,$P=8388608 one=$F,BUN_JSC_numberOfGCMarkers=1"
cd $R
$F ab.mjs 5 $R/lru-gc.mjs "500" $arms > $O-P-lru4k.txt 2>&1
$F ab.mjs 5 $R/lru-gc.mjs "100 16384" $arms > $O-P-lru16k.txt 2>&1
$F eden-matrix.mjs 7 2,8,32 $arms > $O-P-eden.txt 2>&1
$F app-matrix.mjs $R/bench-app.mjs 5 "20000 8 100" $arms > $O-P-churn.txt 2>&1
$F ab.mjs 5 $R/shapes.mjs "tree 100" $arms > $O-P-tree.txt 2>&1
echo DONE > $O-P-done.txt
