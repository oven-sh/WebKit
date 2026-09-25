#!/bin/bash
# gatepar.sh <name> <jsc> <iterations> <parallel> [jsc options...]: how often each function executes a load of the
# threads-mode byte (gate-exec.py), over the tests, one callgrind process per test.
# Output: $OUT/<name>/<test>.gates and $OUT/<name>/summary.txt
. "$(dirname "$0")/common.sh"
N=$1; J=$2; IT=$3; P=$4; shift 4
S=$(stripped_copy "$J"); O=$OUT/$N; mkdir -p "$O"
cd "$JETSTREAM" || exit 1
export OPTS="$*" O S IT HERE VALGRIND VALGRIND_LIB VGOPTS
one() { t=$1; "$VALGRIND" --tool=callgrind $VGOPTS --dump-instr=yes --separate-threads=yes --callgrind-out-file="$O/$t.cg" "$S" $OPTS -e "testIterationCount=$IT;testList=[\"$t\"]" cli.js > /dev/null 2>&1; f=$O/$t.cg-01; [ -f "$f" ] || f=$(ls -S "$O/$t".cg* | head -1); python3 "$HERE/gate-exec.py" "$S" "$f" 100000 > "$O/$t.gates" 2>/dev/null; rm -f "$O/$t".cg*; }
export -f one
for t in $TESTS; do echo $t; done | xargs -P "$P" -I{} bash -c 'one {}'
python3 - "$O" <<'PY' > "$O/summary.txt"
import sys, glob, collections, re
tot = 0; g = 0; c = collections.Counter()
for p in glob.glob(sys.argv[1] + '/*.gates'):
    ls = open(p, errors='replace').read().split('\n')
    m = re.match(r'instructions (\d+), gate instructions executed (\d+)', ls[0]) if ls and ls[0] else None
    if not m: continue
    tot += int(m.group(1)); g += int(m.group(2))
    for l in ls[1:]:
        m = re.match(r'\s*(\d+)\s+[\d.]+%\s+(.*)', l)
        if m: c[m.group(2)] += int(m.group(1))
print("instructions %d, gate tests executed %d (%.3f%% of instructions)" % (tot, g, 100.0 * g / max(tot, 1)))
for f, n in c.most_common(150): print("%12d %.4f%%  %s" % (n, 100.0 * n / tot, f[:170]))
PY
head -60 "$O/summary.txt"
