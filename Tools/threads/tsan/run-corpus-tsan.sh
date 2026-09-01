#!/usr/bin/env bash
# run-corpus-tsan.sh — one full TSAN corpus snapshot run (FULL JIT, GIL-off).
# Saves every TSAN report verbatim (per-test logs + concatenated reports log).
set -u
ROOT=/root/WebKit
JSC=$ROOT/WebKitBuild/TSan/bin/jsc
OUT=${1:-$ROOT/Tools/threads/tsan/r0}
mkdir -p "$OUT"
export JSC_useThreadGIL=false JSC_useVMLite=true JSC_useSharedAtomStringTable=true \
       JSC_useSharedGCHeap=true JSC_useThreadGILOffUnsafe=true
export TSAN_OPTIONS="suppressions=$ROOT/Tools/tsan/suppressions.txt halt_on_error=0 history_size=7 second_deadlock_stack=1"

run_one() {
  local f=$1
  local rel=${f#$ROOT/JSTests/threads/}
  local slug=$(echo "$rel" | tr '/'. '__')
  local log="$OUT/$slug.log"
  # header directives
  if head -10 "$f" | grep -q '^//@ skip'; then echo "SKIP $rel"; return; fi
  local args=()
  local req
  req=$(head -10 "$f" | grep -m1 '^//@ requireOptions' | grep -oE '"[^"]*"' | tr -d '"')
  for a in $req; do args+=("$a"); done
  # ensure threads flag present for consistency with run config
  case " ${args[*]:-} " in *--useJSThreads*) ;; *) args+=(--useJSThreads=1);; esac
  ( cd "$ROOT/JSTests/threads" && timeout -k 10 420 "$JSC" "${args[@]}" "$f" ) >"$log" 2>&1
  local st=$?
  local n=$(grep -c 'WARNING: ThreadSanitizer' "$log")
  echo "DONE $rel exit=$st tsan_reports=$n"
}
export -f run_one
export ROOT JSC OUT

find "$ROOT/JSTests/threads" -name '*.js' ! -path '*/resources/*' ! -name 'harness.js' | sort | \
  xargs -P 12 -I{} bash -c 'run_one "$@"' _ {} | tee "$OUT/run-summary.txt"
