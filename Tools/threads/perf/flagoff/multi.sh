#!/bin/bash
# multi.sh <rounds> <phases: comma list of first,six,full,llint,baseline,dfg> <name=jsc>...
# The phases of phases.sh (the 36 tests in one process, main-thread counters) for any number of binaries, interleaved per
# round; one of the names must be "main". Also sjit: the first iteration with the JIT compiling on the main thread
# (--useConcurrentJIT=0), whose instruction count repeats to 0.1 % where the concurrent first iteration's moves by 0.4 %.
# Output: $OUT/multi.txt, lines "<phase> <name> <round> <instructions> <cycles> <locked loads> <task-clock> <score>";
# multi-compare.py prints each binary against main (minima of the counters, median of the score).
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/../../../.." && pwd)
JETSTREAM=${JETSTREAM:-$REPO/PerformanceTests/JetStream2}
OUT=${OUT:-$PWD/flagoff-out}; mkdir -p "$OUT"; O=$OUT/multi.txt
R=$1; PH=$2; shift 2
TESTS=${TESTS:-"Air Basic ML Babylon cdjs first-inspector-code-load multi-inspector-code-load Box2D octane-code-load crypto delta-blue earley-boyer gbemu mandreel navier-stokes pdfjs raytrace regexp richards splay typescript octane-zlib FlightPlanner OfflineAssembler UniPoker async-fs float-mm.c hash-map ai-astar gaussian-blur stanford-crypto-aes stanford-crypto-pbkdf2 stanford-crypto-sha256 json-stringify-inspector json-parse-inspector WSL"}
LIST='testList=['; first=1; for t in $TESTS; do [ $first = 1 ] || LIST+=','; LIST+="\"$t\""; first=0; done; LIST+=']'
cd "$JETSTREAM" || exit 1
stat() { # phase name jsc round js-prefix options...
  local phase=$1 name=$2 J=$3 r=$4 js=$5; shift 5
  local tmp=$(mktemp) out i c l k sc
  out=$(perf stat -i -x, -o "$tmp" -e instructions:u,cycles:u,mem_inst_retired.lock_loads:u,task-clock "$J" "$@" -e "$js$LIST" cli.js 2>&1)
  i=$(grep instructions "$tmp" | cut -d, -f1); c=$(grep cycles "$tmp" | cut -d, -f1); l=$(grep lock_loads "$tmp" | cut -d, -f1); k=$(grep task-clock "$tmp" | cut -d, -f1); rm -f "$tmp"
  sc=$(echo "$out" | grep -m1 "Total Score" | awk '{print $3}')
  echo "$phase $name $r $i $c $l $k ${sc:-NaN}" >> "$O"
}
for r in $(seq 1 "$R"); do
  for nb in "$@"; do
    n=${nb%%=*}; J=${nb#*=}
    for ph in ${PH//,/ }; do
      case $ph in
        full) stat full $n $J $r "" ;;
        first) stat first $n $J $r "testIterationCount=1;" ;;
        sjit) stat sjit $n $J $r "testIterationCount=1;" --useConcurrentJIT=0 ;;
        six) stat six $n $J $r "testIterationCount=6;" ;;
        llint) stat llint $n $J $r "testIterationCount=6;" --useJIT=0 ;;
        baseline) stat baseline $n $J $r "testIterationCount=6;" --useDFGJIT=0 ;;
        dfg) stat dfg $n $J $r "testIterationCount=6;" --useFTLJIT=0 ;;
      esac
    done
  done
done
python3 "$HERE/multi-compare.py" "$OUT"
