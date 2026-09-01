#!/bin/bash
# Parallel ASAN sweep round; keeps failure logs only.
# Usage: campaign.sh <round-tag> <cells...>   where each cell is W:N:count, e.g. 16:1000:2 32:600:2
set -u
TAG=$1; shift
cd /root/WebKit
OUT=/root/WebKit/Tools/threads/bughunt/w16/rounds/$TAG
mkdir -p "$OUT"
BENCH=/root/WebKit/Tools/threads/bughunt/w16/bench-local.js
FLAGS="--useJSThreads=1 --useThreadGIL=0 --useVMLite=1 --useSharedAtomStringTable=1 --useSharedGCHeap=1 --useThreadGILOffUnsafe=1"
export ASAN_OPTIONS="abort_on_error=0:fast_unwind_on_malloc=0:malloc_context_size=30:detect_leaks=0:handle_segv=1:handle_abort=1:exitcode=99"
# A4-amend round (auditability finding): cores enabled per the A3-round F5
# convention, and every run leaves a one-line receipt in RECEIPTS.txt even on
# PASS — a GREEN gate claim must be re-derivable from artifacts, not from the
# narrative plus directory mtimes. Failure logs are still kept in full.
ulimit -c unlimited
run_one() {
  local w=$1 n=$2 i=$3 extra=$4 label=$5
  local log="$OUT/W${w}-N${n}-${label}-${i}.log"
  local t0=$(date +%s)
  timeout 720 WebKitBuild/Debug/bin/jsc $FLAGS $extra "$BENCH" -- "$w" smoke "$n" > "$log" 2>&1
  local rc=$?
  local t1=$(date +%s)
  echo "RC=$rc" >> "$log"
  echo "EXTRA=$extra" >> "$log"
  local suspects=$(grep -c -E "MISMATCH|AddressSanitizer|pas panic|ASSERT FAILED" "$log" 2>/dev/null)
  echo "$(date -u +%FT%TZ) W=$w N=$n label=$label run=$i rc=$rc wall=$((t1-t0))s flags_sha=$(echo "$FLAGS $extra" | sha1sum | cut -c1-12) suspect_lines=${suspects:-0}" >> "$OUT/RECEIPTS.txt"
  if [ "$rc" = "0" ]; then rm -f "$log"; echo "[$TAG] W=$w N=$n $label #$i PASS"; else echo "[$TAG] W=$w N=$n $label #$i FAIL rc=$rc"; fi
}
pids=()
for cell in "$@"; do
  IFS=: read -r w n c label extraraw <<< "$cell"
  label=${label:-base}
  extra=$(echo "${extraraw:-}" | tr ',' ' ')
  for i in $(seq 1 "$c"); do run_one "$w" "$n" "$i" "$extra" "$label" & pids+=($!); done
done
for p in "${pids[@]}"; do wait "$p"; done
echo "[$TAG] round complete; failures: $(ls "$OUT" 2>/dev/null | grep -v -c '^RECEIPTS.txt$')"
