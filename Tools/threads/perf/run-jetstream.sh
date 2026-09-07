#!/bin/bash
# run-jetstream.sh <outdir> [runs]: JetStream2 (cli.js) on main, branch flag-off, GIL-on, GIL-off.
# All four run the same list: every default test except the four *-wasm ones,
# bomb-workers and segmentation (GIL off has no WebAssembly; the last two use
# workers, which the shell runs as agents), so the totals compare.
set -u
OUT=$1; RUNS=${2:-5}; mkdir -p "$OUT"
R=$(cd "$(dirname "$0")/../../.." && pwd)
MAIN=${MAINJSC:?}; BR=${BRJSC:?}
LIST='testList=["Air","Basic","ML","Babylon","cdjs","first-inspector-code-load","multi-inspector-code-load","Box2D","octane-code-load","crypto","delta-blue","earley-boyer","gbemu","mandreel","navier-stokes","pdfjs","raytrace","regexp","richards","splay","typescript","octane-zlib","FlightPlanner","OfflineAssembler","UniPoker","async-fs","float-mm.c","hash-map","ai-astar","gaussian-blur","stanford-crypto-aes","stanford-crypto-pbkdf2","stanford-crypto-sha256","json-stringify-inspector","json-parse-inspector","WSL"]'
GILOFF="JSC_useSharedGCHeap=1 JSC_useThreadGILOffUnsafe=1 JSC_useThreadGIL=0"
cd $R/PerformanceTests/JetStream2
run() { local cfg=$1 jsc=$2 envs=$3 opts=$4
  for i in $(seq 1 $RUNS); do
    env $envs $jsc $opts -e "$LIST" cli.js > "$OUT/$cfg-$i.txt" 2>&1
    echo "$cfg run $i: $(grep 'Total Score' $OUT/$cfg-$i.txt)"
  done; }
run main $MAIN "" ""
run off $BR "" ""
run gilon $BR "" "--useJSThreads=1"
run giloff $BR "$GILOFF" "--useJSThreads=1"
python3 - "$OUT" <<'PY'
import sys, re, glob, statistics, collections, os
out=sys.argv[1]; per=collections.defaultdict(lambda: collections.defaultdict(list)); tot=collections.defaultdict(list)
for f in glob.glob(out+'/*-*.txt'):
    cfg=os.path.basename(f).rsplit('-',1)[0]; cur=None
    for line in open(f):
        m=re.match(r'Running (.*):', line)
        if m: cur=m.group(1); continue
        m=re.match(r'\s+Score: ([\d.]+)', line)
        if m and cur: per[cur][cfg].append(float(m.group(1)))
        m=re.match(r'Total Score:\s+([\d.]+)', line)
        if m: tot[cfg].append(float(m.group(1)))
cfgs=['main','off','gilon','giloff']
print('| test | main | flag off | GIL on | GIL off |'); print('|---|---|---|---|---|')
for t in sorted(per):
    print('| '+t+' | '+' | '.join(f"{statistics.median(per[t][c]):.1f}" if per[t][c] else '-' for c in cfgs)+' |')
print('| **Total (geomean)** | '+' | '.join(f"{statistics.median(tot[c]):.1f}" if tot[c] else '-' for c in cfgs)+' |')
PY
