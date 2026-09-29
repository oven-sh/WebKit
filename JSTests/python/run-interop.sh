#!/bin/sh
#
# run-interop.sh <path to jsc> [test ...]
#
# Runs each of interop/*.mjs and interop/*.py that has a file of what it should print beside it, in several configurations of the engine.
# There is nothing to compare these with, since no other Python has JavaScript in it: what is expected was read and found right.

# The part of the library that is written in Python does not come with the engine. It is CPython's, as it is.
[ -n "$PYTHONPATH" ] || { echo "PYTHONPATH is to name the Lib directory of CPython 3.14" >&2; exit 2; }

jsc=$1
shift
cd "$(dirname "$0")/interop" || exit 2
[ $# -eq 0 ] && set -- *.expected

eager="--thresholdForJITAfterWarmUp=1 --thresholdForJITSoon=1"
failures=0
runs=0
for expected in "$@"; do
    test=${expected%.expected}
    for options in "" "--useJIT=0" "$eager" "--useLOLJIT=1 $eager" "--collectContinuously=1"; do
        runs=$((runs + 1))
        case $test in
            *.mjs) module=-m ;;
            *) module= ;;
        esac
        # Where things are in memory is not part of what is expected.
        # shellcheck disable=SC2086
        if ! "$jsc" --useDollarVM=1 $options $module "$test" 2>&1 | sed -e 's/0x[0-9a-f]*/0x/g' | cmp -s - "$expected"; then
            failures=$((failures + 1))
            echo "FAIL: $test $options"
        fi
    done
done
echo "$((runs - failures)) of $runs pass"
[ $failures -eq 0 ]
