#!/usr/bin/env bash
# Evidence campaign for the W>=16 crash family. Usage:
#   campaign.sh TAG NRUNS W SMOKE(0/1) [extra jsc flags...]
# Writes per-run logs + summary to OUTDIR (default alongside this script).
set -u
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SB=/root/WebKit/Tools/threads/scalebench
JSC="${JSC_BIN:-/root/WebKit/WebKitBuild/Release/bin/jsc}"
BENCH="${BENCH_JS:-bench.js}"
TAG="$1"; N="$2"; W="$3"; SMOKE="$4"; shift 4
FLAGS=(--useJSThreads=1 --useThreadGIL=0 --useVMLite=1
       --useSharedAtomStringTable=1 --useSharedGCHeap=1
       --useThreadGILOffUnsafe=1 "$@")
OUT="${OUTDIR:-$SCRIPT_DIR}/$TAG"
mkdir -p "$OUT"
ulimit -c 0
ok=0; n133=0; n134=0; n139=0; nother=0
for ((i=1;i<=N;i++)); do
  args=(-- "$W"); [[ "$SMOKE" == 1 ]] && args+=(smoke)
  start=$(date +%s.%N)
  (cd "$SB" && timeout 600 "$JSC" "${FLAGS[@]}" "$BENCH" "${args[@]}") \
      >"$OUT/run$i.out" 2>"$OUT/run$i.err"
  rc=$?
  dur=$(echo "$(date +%s.%N) - $start" | bc)
  case $rc in
    0) ok=$((ok+1));;
    133) n133=$((n133+1));;
    134) n134=$((n134+1));;
    139) n139=$((n139+1));;
    *) nother=$((nother+1));;
  esac
  echo "run$i rc=$rc dur=${dur}s" >>"$OUT/summary.txt"
done
echo "TAG=$TAG N=$N W=$W smoke=$SMOKE flags='$*' ok=$ok sigtrap133=$n133 sigabrt134=$n134 sigsegv139=$n139 other=$nother" | tee -a "$OUT/summary.txt"
