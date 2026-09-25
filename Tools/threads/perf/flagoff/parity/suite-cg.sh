#!/bin/bash
# suite-cg.sh <name> <jsc> <iterations> [jsc options...]: the suite in ONE process under callgrind. Output: $OUT/<name>/suite.fn
# The per-test passes of parity.sh miss what grows with the process: collections of a heap that holds every test's
# objects, the code of every test. One iteration takes about a quarter of an hour.
. "$(dirname "$0")/common.sh"
N=$1; J=$2; IT=$3; shift 3
S=$(stripped_copy "$J"); mkdir -p "$OUT/$N"
cd "$JETSTREAM" || exit 1
bash "$HERE/cg.sh" "$OUT/$N" suite "$S" "$@" -e "testIterationCount=$IT;$(testlist_js)" cli.js
