#!/bin/bash
# campaign.sh <runs> <script> [extra jsc flags...]
# Prints per-run one-line outcome and a summary failure rate.
JSC=${JSC:-/root/WebKit/WebKitBuild/Release/bin/jsc}
BASE="--useJSThreads=1 --useThreadGIL=0 --useVMLite=1 --useSharedAtomStringTable=1 --useSharedGCHeap=1 --useThreadGILOffUnsafe=1"
RUNS=$1; SCRIPT=$2; shift 2
fail=0; crash=0; ok=0
for ((i=1;i<=RUNS;i++)); do
  out=$(timeout 180 $JSC $BASE "$@" "$SCRIPT" 2>&1)
  rc=$?
  last=$(echo "$out" | tail -1)
  if [[ $rc -ne 0 ]]; then crash=$((crash+1)); echo "run$i: CRASH rc=$rc :: $(echo "$out" | tail -2 | head -1) | $last";
  elif echo "$out" | grep -q '^OK'; then ok=$((ok+1)); echo "run$i: $last";
  else fail=$((fail+1)); echo "run$i: $last"; fi
done
echo "SUMMARY: ok=$ok corrupt=$fail crash=$crash / $RUNS  flags: $*"
