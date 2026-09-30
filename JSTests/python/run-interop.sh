#!/bin/sh
#
# run-interop.sh <path to jsc> [test ...]
#
# Runs each of interop/*.mjs and interop/*.py that has a file of what it should print beside it, in several configurations of the engine.
# There is nothing to compare these with, since no other Python has JavaScript in it: what is expected was read and found right.
#
# What a run that fails printed is kept, and where is said: one that fails only now and then may not do so again for the asking.

# The part of the library that is written in Python does not come with the engine. It is CPython's, as it is.
[ -n "$PYTHONPATH" ] || { echo "PYTHONPATH is to name the Lib directory of CPython 3.14" >&2; exit 2; }

jsc=$1
shift
cd "$(dirname "$0")/interop" || exit 2
[ $# -eq 0 ] && set -- *.expected

eager="--thresholdForJITAfterWarmUp=1 --thresholdForJITSoon=1"
# And the DFG, of nearly everything and with next to nothing known of it, so that it is left again and again.
eagerDFG="$eager --thresholdForOptimizeAfterWarmUp=5 --thresholdForOptimizeAfterLongWarmUp=5 --thresholdForOptimizeSoon=5 --useConcurrentJIT=0 --validateGraphAtEachPhase=1"
# And the FTL. What has been run once is left alone: coming back from something counts for 15.
eagerFTL="$eagerDFG --thresholdForFTLOptimizeAfterWarmUp=20 --thresholdForFTLOptimizeSoon=20"
# And again with something known of what it compiles, which is when it leaves out the most.
soonFTL="--thresholdForJITAfterWarmUp=10 --thresholdForJITSoon=10 --thresholdForOptimizeAfterWarmUp=100 --thresholdForOptimizeAfterLongWarmUp=100 --thresholdForOptimizeSoon=100 --thresholdForFTLOptimizeAfterWarmUp=1000 --thresholdForFTLOptimizeSoon=1000 --useConcurrentJIT=0 --validateGraphAtEachPhase=1"
failures=0
runs=0
kept=${TMPDIR:-/tmp}/python-interop-that-failed
actual=$(mktemp) || exit 2
trap 'rm -f "$actual"' EXIT
for expected in "$@"; do
    test=${expected%.expected}
    for options in "" "--useJIT=0" "$eager" "$eagerDFG" "$eagerFTL" "$soonFTL" "--useLOLJIT=1 $eager" "--collectContinuously=1"; do
        runs=$((runs + 1))
        case $test in
            *.mjs) module=-m ;;
            *) module= ;;
        esac
        # Where things are in memory is not part of what is expected.
        # shellcheck disable=SC2086
        "$jsc" --useDollarVM=1 $options $module "$test" 2>&1 | sed -e 's/0x[0-9a-f]*/0x/g' > "$actual"
        if ! cmp -s "$actual" "$expected"; then
            failures=$((failures + 1))
            mkdir -p "$kept"
            cp "$actual" "$kept/$test.$failures.txt"
            echo "FAIL: $test $options (see $kept/$test.$failures.txt)"
        fi
    done
done
echo "$((runs - failures)) of $runs pass"
[ $failures -eq 0 ]
