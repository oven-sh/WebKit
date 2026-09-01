#!/usr/bin/env bash
# Gate (5): ALL JSTests/stress/*bigint*.js + big-int-*.js under GIL-off AND flag-off, v39.
set -u
ROOT=/root/WebKit
V39="$ROOT/WebKitBuild/Release/bin/jsc-v39"
PINNED="JSC_useJSThreads=1 JSC_useThreadGIL=0 JSC_useVMLite=1 JSC_useSharedAtomStringTable=1 JSC_useSharedGCHeap=1 JSC_useThreadGILOffUnsafe=1"
mapfile -t TESTS < <( { ls "$ROOT"/JSTests/stress/*bigint*.js "$ROOT"/JSTests/stress/big-int-*.js 2>/dev/null; } | sort -u )
echo "bigint-stress: ${#TESTS[@]} tests"
MODE="${1:-giloff}"
case "$MODE" in
  giloff) ENVV="$PINNED"; BIN="$V39" ;;
  flagoff) ENVV=""; BIN="$V39" ;;
esac
FAIL=0; PASS=0; SKIP=0
for f in "${TESTS[@]}"; do
  if head -1 "$f" | grep -q '//@ skip'; then SKIP=$((SKIP+1)); continue; fi
  out=$(timeout -k 5 90 env $ENVV "$BIN" "$f" 2>&1); rc=$?
  if [[ $rc -eq 0 ]]; then PASS=$((PASS+1));
  else FAIL=$((FAIL+1)); echo "FAIL[$MODE] rc=$rc ${f#$ROOT/}"; echo "$out" | head -3; fi
done
echo "bigint-stress[$MODE]: pass=$PASS fail=$FAIL skip=$SKIP total=${#TESTS[@]}"
[[ $FAIL -eq 0 ]]
