#!/bin/sh
# run-programs.sh <path to jsc> [program...]
#
# Runs each of programs/*.py in several configurations of the engine, and once by way of its syntax tree. What it prints has to be, byte for byte, what is
# in the file beside it, which is what CPython 3.14 prints:
#
#     python3.14 programs/x.py > programs/x.expected
#
# What a run that fails printed is kept, and where is said: one that fails only now and then may not do so again for the asking.

# The part of the library that is written in Python does not come with the engine. It is CPython's, as it is.
[ -n "$PYTHONPATH" ] || { echo "PYTHONPATH is to name the Lib directory of CPython 3.14" >&2; exit 2; }

jsc=$1
shift
cd "$(dirname "$0")/programs" || exit 2
[ $# -eq 0 ] && set -- *.py

eager="--thresholdForJITAfterWarmUp=1 --thresholdForJITSoon=1"
failures=0
runs=0
kept=${TMPDIR:-/tmp}/python-programs-that-failed
actual=$(mktemp) || exit 2
trap 'rm -f "$actual"' EXIT
failed() {
    failures=$((failures + 1))
    mkdir -p "$kept"
    cp "$actual" "$kept/$1.$failures.txt"
    echo "FAIL: $1 $2 (see $kept/$1.$failures.txt)"
}
for program in "$@"; do
    for options in "" "--useJIT=0" "$eager" "--useLOLJIT=1 $eager" "--collectContinuously=1"; do
        runs=$((runs + 1))
        # shellcheck disable=SC2086
        "$jsc" $options "$program" > "$actual" 2>&1
        cmp -s "$actual" "${program%.py}.expected" || failed "$program" "$options"
    done
done
# And by way of its syntax tree. What runs it that way is on the stack beneath it, and is __main__, which shows in what says how deep the stack is, what is on it, or what __main__ is.
for program in "$@"; do
    case $program in recursion-limit.py | recursion-limit-and-frames.py | start-up.py | uncaught-exceptions.py | warning-filters.py) continue ;; esac
    runs=$((runs + 1))
    "$jsc" ../through-a-syntax-tree.py -- "$program" > "$actual" 2>&1
    cmp -s "$actual" "${program%.py}.expected" || failed "$program" "through a syntax tree"
done
echo "$((runs - failures)) of $runs pass"
[ $failures -eq 0 ]
