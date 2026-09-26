#!/bin/bash
# event-by-symbol.sh <event> <period> <phase: full|six|first|llint> <name=jsc>...
# Samples one hardware event on the main thread of each binary running the 36 tests in one process, and writes the
# samples per symbol (generated code and everything without a symbol is one row each) to $OUT/<event>-<name>.txt.
# event-by-symbol-compare.py prints the symbols that account for the difference against main.
# For events that count stalls the sample lands on or just after the instruction that stalled: read it per function.
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/../../../.." && pwd)
JETSTREAM=${JETSTREAM:-$REPO/PerformanceTests/JetStream2}
OUT=${OUT:-$PWD/flagoff-out}; mkdir -p "$OUT"
EV=$1; PERIOD=$2; PH=$3; shift 3
TESTS=${TESTS:-"Air Basic ML Babylon cdjs first-inspector-code-load multi-inspector-code-load Box2D octane-code-load crypto delta-blue earley-boyer gbemu mandreel navier-stokes pdfjs raytrace regexp richards splay typescript octane-zlib FlightPlanner OfflineAssembler UniPoker async-fs float-mm.c hash-map ai-astar gaussian-blur stanford-crypto-aes stanford-crypto-pbkdf2 stanford-crypto-sha256 json-stringify-inspector json-parse-inspector WSL"}
LIST='testList=['; first=1; for t in $TESTS; do [ $first = 1 ] || LIST+=','; LIST+="\"$t\""; first=0; done; LIST+=']'
case $PH in full) JS=""; OPT="" ;; first) JS="testIterationCount=1;"; OPT="" ;; six) JS="testIterationCount=6;"; OPT="" ;; llint) JS="testIterationCount=6;"; OPT="--useJIT=0" ;; esac
cd "$JETSTREAM" || exit 1
for nb in "$@"; do
  n=${nb%%=*}; J=${nb#*=}
  D=$OUT/$EV-$n.data
  perf record -q --no-inherit -e "$EV:u" -c "$PERIOD" -o "$D" "$J" $OPT -e "$JS$LIST" cli.js > /dev/null 2>&1
  # comm of the main thread is the binary's name; the helper threads are not recorded (--no-inherit)
  perf report -i "$D" --no-children --sort symbol -F sample,symbol --stdio 2>/dev/null \
    | awk '/^#/ || NF < 2 {next} {c=$1; $1=""; s=$0; sub(/^ +\[[.k]\] /, "", s); sub(/^ +/, "", s); sub(/( +-)+ *$/, "", s); if (s ~ /^0x[0-9a-f]+/) s="(generated code and other code without a symbol)"; a[s]+=c} END{for (s in a) print a[s] "\t" s}' \
    | sort -rn > "$OUT/$EV-$n.txt"
  rm -f "$D"
  echo "$n: $(awk -F'\t' '{t+=$1} END{print t}' "$OUT/$EV-$n.txt") samples of $EV"
done
