#!/bin/bash
# parity.sh <name> <jsc> <iterations> <parallel> [jsc options...]: cg.sh for each test, one process per test.
# Output: $OUT/<name>/<test>.fn. Compare two with cgdiff.py -sum $OUT/<a> $OUT/<b>, or by family with famdiff.py.
#
# What to pass, and why:
#   --useJIT=0 --useConcurrentGC=0                       the interpreter; counts repeat to the instruction
#   --useConcurrentJIT=0 --useConcurrentGC=0             every tier, compiling on the main thread (VGOPTS=--smc-check=all-non-file)
# With the concurrent collector on, the write barrier's slow path runs 5 times more or less often between two runs of the
# same binary (it depends on when marking happens to be active): do not read a difference off such a pair. With the concurrent
# JIT on, the main thread's count includes its waiting for the compiler threads, which valgrind serializes.
. "$(dirname "$0")/common.sh"
N=$1; J=$2; IT=$3; P=$4; shift 4
S=$(stripped_copy "$J"); mkdir -p "$OUT/$N"
cd "$JETSTREAM" || exit 1
export OPTS="$*" VGOPTS VALGRIND VALGRIND_LIB OUT STRIPPED JETSTREAM
for t in $TESTS; do echo $t; done | xargs -P "$P" -I{} bash -c "bash '$HERE/cg.sh' '$OUT/$N' {} '$S' \$OPTS -e 'testIterationCount=$IT;testList=[\"{}\"]' cli.js > /dev/null 2>&1"
echo "$N: $(ls "$OUT/$N"/*.fn | wc -l) tests, total $(cat "$OUT/$N"/*.fn | grep -v 'PROGRAM TOTALS' | awk -F'\t' '{s+=$1} END{print s}')"
