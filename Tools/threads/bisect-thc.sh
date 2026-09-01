#!/usr/bin/env bash
# bisect-thc.sh — quick 9-run median of transition-heavy-constructor only.
# Used by the r27 bench-gate bisection; not part of the gate itself.
set -u -o pipefail
JSC="${1:-/root/WebKit/WebKitBuild/Release/bin/jsc}"
HARNESS=/root/WebKit/JSTests/threads/bench/harness.js
BENCH=/root/WebKit/JSTests/threads/bench/transition-heavy-constructor.js
RUNS="${2:-9}"
samples=()
for ((i=0;i<RUNS;++i)); do
  out="$("$JSC" "$HARNESS" "$BENCH" 2>&1)" || { echo "FAIL: $out" >&2; exit 2; }
  ms="$(printf '%s\n' "$out" | grep '^BENCH ' | tail -n1 | awk '{print $3}')"
  samples+=("$ms")
done
med="$(printf '%s\n' "${samples[@]}" | sort -n | awk '{v[NR]=$1} END{if(NR%2==1)printf"%.3f",v[(NR+1)/2];else printf"%.3f",(v[NR/2]+v[NR/2+1])/2}')"
delta="$(awk -v m="$med" -v b=54.918 'BEGIN{printf"%+.2f",(m-b)/b*100.0}')"
echo "samples: ${samples[*]}"
echo "median:  $med ms   baseline 54.918 ms   delta ${delta}%"
