#!/bin/bash
# pertest.sh <rounds> <parallel> <interp|first> <name=jsc>...: main-thread counters of each test in a process of its own
# (interp: --useJIT=0, two iterations; first: one iteration), the binaries interleaved per round; one name must be "main".
# Prints the geometric mean over the tests of each binary's minimum against main's. Cycles need <parallel> = 1.
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/../../../.." && pwd)
JETSTREAM=${JETSTREAM:-$REPO/PerformanceTests/JetStream2}
OUT=${OUT:-$PWD/flagoff-out}; mkdir -p "$OUT"
R=$1; P=$2; MODE=$3; shift 3
O=$OUT/pertest-$MODE.txt; : > "$O"
TESTS=${TESTS:-"Air Basic ML Babylon cdjs first-inspector-code-load multi-inspector-code-load Box2D octane-code-load crypto delta-blue earley-boyer gbemu mandreel navier-stokes pdfjs raytrace regexp richards splay typescript octane-zlib FlightPlanner OfflineAssembler UniPoker async-fs float-mm.c hash-map ai-astar gaussian-blur stanford-crypto-aes stanford-crypto-pbkdf2 stanford-crypto-sha256 json-stringify-inspector json-parse-inspector WSL"}
cd "$JETSTREAM" || exit 1
one() { # name jsc test
  local tmp=$(mktemp)
  if [ "$MODE" = interp ]; then perf stat -i -x, -o "$tmp" -e instructions:u,cycles:u "$2" --useJIT=0 -e "testIterationCount=2; testList=[\"$3\"]" cli.js > /dev/null 2>&1
  else perf stat -i -x, -o "$tmp" -e instructions:u,cycles:u "$2" -e "testIterationCount=1; testList=[\"$3\"]" cli.js > /dev/null 2>&1; fi
  echo "$3 $1 $(grep instructions "$tmp" | cut -d, -f1) $(grep cycles "$tmp" | cut -d, -f1)" >> "$O"; rm -f "$tmp"
}
export -f one; export O MODE
for r in $(seq 1 "$R"); do
  for nb in "$@"; do
    n=${nb%%=*}; J=${nb#*=}
    for t in $TESTS; do echo $t; done | xargs -P "$P" -I{} bash -c "one $n $J {}"
  done
done
python3 - "$O" <<'PY'
import sys, collections, math
d = collections.defaultdict(lambda: collections.defaultdict(list))
for l in open(sys.argv[1]):
    p = l.split()
    if len(p) == 4 and p[2].isdigit() and p[3].isdigit(): d[p[1]][p[0]].append((int(p[2]), int(p[3])))
def g(x): return math.exp(sum(math.log(v) for v in x) / len(x))
for n in d:
    ts = [t for t in d['main'] if t in d[n]]
    ri = g([min(v[0] for v in d[n][t]) / min(v[0] for v in d['main'][t]) for t in ts])
    rc = g([min(v[1] for v in d[n][t]) / min(v[1] for v in d['main'][t]) for t in ts])
    worst = sorted(((min(v[1] for v in d[n][t]) / min(v[1] for v in d['main'][t]), t) for t in ts), reverse=True)[:4]
    print("%-8s per-test geometric mean: instructions %.4f cycles %.4f  n=%d  highest: %s" % (n, ri, rc, len(ts), ' '.join('%s %.3f' % (t, r) for r, t in worst)))
PY
