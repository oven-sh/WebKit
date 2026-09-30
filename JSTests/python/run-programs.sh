#!/bin/sh
# run-programs.sh <path to jsc> [program...]
#
# Runs each of programs/*.py in several configurations of the engine, and once by way of its syntax tree. What it prints has to be, byte for byte, what is
# in the file beside it, which is what CPython 3.14 prints:
#
#     python3.14 programs/x.py > programs/x.expected 2>&1
#
# That is on macOS. Where CPython prints something else on Linux, because the system has other things in it or works its sums out otherwise, that is in x.linux.expected, and is what is gone by there.
# A program that is about what only one system has has only that, and is passed over on the other.
# CPython is to be as it comes, of just that version, with its extension modules linked in as they are here (MODULE_BUILDTYPE=static ./configure --disable-test-modules), built with clang, which
# decides where a multiplication and an addition are done as one, and run with PYTHONUTF8=1 PYTHONHASHSEED=0 PYTHONUNBUFFERED=1.
#
# What a run that fails printed is kept, and where is said: one that fails only now and then may not do so again for the asking.

# The part of the library that is written in Python does not come with the engine. It is CPython's, as it is.
[ -n "$PYTHONPATH" ] || { echo "PYTHONPATH is to name the Lib directory of CPython 3.14" >&2; exit 2; }

jsc=$1
shift
cd "$(dirname "$0")/programs" || exit 2
[ $# -eq 0 ] && set -- *.py

eager="--thresholdForJITAfterWarmUp=1 --thresholdForJITSoon=1"
# And the DFG, of nearly everything and with next to nothing known of it, so that it is left again and again.
eagerDFG="$eager --thresholdForOptimizeAfterWarmUp=5 --thresholdForOptimizeAfterLongWarmUp=5 --thresholdForOptimizeSoon=5 --useConcurrentJIT=0 --validateGraphAtEachPhase=1"
# And the FTL. What has been run once is left alone: coming back from something counts for 15.
eagerFTL="$eagerDFG --thresholdForFTLOptimizeAfterWarmUp=20 --thresholdForFTLOptimizeSoon=20"
# And again with something known of what it compiles, which is when it leaves out the most.
soonFTL="--thresholdForJITAfterWarmUp=10 --thresholdForJITSoon=10 --thresholdForOptimizeAfterWarmUp=100 --thresholdForOptimizeAfterLongWarmUp=100 --thresholdForOptimizeSoon=100 --thresholdForFTLOptimizeAfterWarmUp=1000 --thresholdForFTLOptimizeSoon=1000 --useConcurrentJIT=0 --validateGraphAtEachPhase=1"
failures=0
runs=0
kept=${TMPDIR:-/tmp}/python-programs-that-failed
actual=$(mktemp) || exit 2
trap 'rm -f "$actual"' EXIT
system=$(uname -s | tr '[:upper:]' '[:lower:]')
expected() {
    if [ -f "${1%.py}.$system.expected" ]; then
        echo "${1%.py}.$system.expected"
    else
        echo "${1%.py}.expected"
    fi
}
failed() {
    failures=$((failures + 1))
    mkdir -p "$kept"
    cp "$actual" "$kept/$1.$failures.txt"
    echo "FAIL: $1 $2 (see $kept/$1.$failures.txt)"
}
for program in "$@"; do
    [ -f "$(expected "$program")" ] || continue
    for options in "" "--useJIT=0" "$eager" "$eagerDFG" "$eagerFTL" "$soonFTL" "--useLOLJIT=1 $eager" "--collectContinuously=1"; do
        runs=$((runs + 1))
        # shellcheck disable=SC2086
        "$jsc" $options "$program" > "$actual" 2>&1
        cmp -s "$actual" "$(expected "$program")" || failed "$program" "$options"
    done
done
# And by way of its syntax tree. What runs it that way is on the stack beneath it, and is __main__, which shows in what says how deep the stack is, what is on it, or what __main__ is.
for program in "$@"; do
    [ -f "$(expected "$program")" ] || continue
    case $program in looking-at-what-is-run-often.py | recursion-limit.py | recursion-limit-and-frames.py | start-up.py | uncaught-exceptions.py | warning-filters.py) continue ;; esac
    runs=$((runs + 1))
    "$jsc" ../through-a-syntax-tree.py -- "$program" > "$actual" 2>&1
    cmp -s "$actual" "$(expected "$program")" || failed "$program" "through a syntax tree"
done
echo "$((runs - failures)) of $runs pass"
[ $failures -eq 0 ]
