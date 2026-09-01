#!/usr/bin/env bash
# Round-3 runner: solo runs, pinned GIL-off flags, counts corruption / races /
# fix-active canary / getConcurrently canary trips.
set -u
ROOT=/root/WebKit
JSC=${JSC:-$ROOT/WebKitBuild/Debug/bin/jsc}
TEST=${TEST:-$ROOT/JSTests/threads/jit/spawned-thread-butterfly-stress.js}
N=${N:-12}
OUT=${OUT:-/tmp/r3-out}
SEED_BASE=${SEED_BASE:-3000}
mkdir -p "$OUT"
corrupt=0; races=0; fails=0; canary=0
for ((i=0;i<N;++i)); do
  seed=$((SEED_BASE+i))
  log="$OUT/r${i}-s${seed}.log"
  timeout -k 5 240 "$JSC" --useJSThreads=1 --useThreadGIL=0 --useVMLite=1 \
    --useSharedAtomStringTable=1 --useSharedGCHeap=1 --useThreadGILOffUnsafe=1 \
    --useDollarVM=1 --randomYieldPeriod=64 --randomYieldSeed=$seed "$TEST" >"$log" 2>&1
  rc=$?
  r=$(grep -c "KEYATOM-RACE" "$log"); races=$((races+r))
  grep -q "KEYATOM-SNAPSHOT-FIX-ACTIVE" "$log" && canary=$((canary+1))
  if grep -qE "named property corrupt|MISMATCH prop" "$log"; then corrupt=$((corrupt+1)); mv "$log" "$OUT/CORRUPT-r${i}-s${seed}.log";
  elif [[ $rc -ne 0 ]]; then fails=$((fails+1)); mv "$log" "$OUT/FAIL-rc${rc}-r${i}-s${seed}.log"; fi
done
echo "runs=$N corrupt=$corrupt other-fails=$fails keyatom-race-lines=$races fix-active-canary=$canary out=$OUT"
