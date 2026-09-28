#!/bin/sh
# run-programs.sh <path to jsc> [program...]
#
# Runs each of programs/*.py in several configurations of the engine. What it prints has to be, byte for byte, what is in the file
# beside it, which is what CPython 3.14 prints:
#
#     python3.14 programs/x.py > programs/x.expected

jsc=$1
shift
cd "$(dirname "$0")/programs" || exit 2
[ $# -eq 0 ] && set -- *.py

eager="--thresholdForJITAfterWarmUp=1 --thresholdForJITSoon=1"
failures=0
runs=0
for program in "$@"; do
    for options in "" "--useJIT=0" "$eager" "--collectContinuously=1"; do
        runs=$((runs + 1))
        # shellcheck disable=SC2086
        if ! "$jsc" $options "$program" 2>&1 | cmp -s - "${program%.py}.expected"; then
            failures=$((failures + 1))
            echo "FAIL: $program $options"
        fi
    done
done
echo "$((runs - failures)) of $runs pass"
[ $failures -eq 0 ]
