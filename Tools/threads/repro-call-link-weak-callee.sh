#!/usr/bin/env bash
# repro-call-link-weak-callee.sh <debug jsc> [runs] [parallelism]
#
# The flag-off Debug regression fixed in CallLinkInfo::unlinkOrUpgradeImpl: the monomorphic-upgrade arm passed `m_callee.get()`
# to publishRecord(), which returns at once with the flag off, but the argument is evaluated first, and a build with assertions
# validates the cell that get() returns. m_callee is a weak reference that the collector clears in visitWeak(); between the end of
# marking and that clearing it can name a dead, already swept cell (structure ID zero), so `ASSERTION FAILED: decontaminate()` in
# StructureID::decode() fires. It needs a tier-up installing code while a collection is under way: stress/generator-yield-star.js
# under the DFG-eager option set and collectContinuously, on a loaded machine (15 of 1,000 runs at 48 in parallel before the fix,
# 0 of 1,000 after). FILES="generator-yield-star.js async-stack-trace-promise-all-basic.js" adds the second test that showed it, which
# does not finish in 15 minutes in a Debug build at that load. Release builds only copy the pointer bits and are unaffected.
#
# Counts the runs that fail with the assertion, and the runs that fail otherwise; a run that outlives 15 minutes is killed and
# counted as a timeout (these options make a Debug+ASAN run very slow).
set -u
JSC=${1:?usage: $0 <debug jsc> [runs] [parallelism]}
RUNS=${2:-240}; PAR=${3:-32}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
export ASAN_OPTIONS=detect_stack_use_after_return=0:detect_leaks=0
OPTS="--validateOptions=true --thresholdForJITAfterWarmUp=10 --thresholdForJITSoon=10 --thresholdForOptimizeAfterWarmUp=20 \
--thresholdForOptimizeAfterLongWarmUp=20 --thresholdForOptimizeSoon=20 --thresholdForFTLOptimizeAfterWarmUp=20 \
--thresholdForFTLOptimizeSoon=20 --maximumEvalCacheableSourceLength=150000 --useEagerCodeBlockJettisonTiming=true \
--repatchBufferingCountdown=0 --collectContinuously=true --useGenerationalGC=false --verifyGC=true --forceOSRExitToLLInt=true"
cd "$ROOT/JSTests/stress"
one() { # <test> <n>
    local err; err=$(mktemp)
    timeout 900 "$JSC" $OPTS "$1" > /dev/null 2> "$err"; local rc=$?
    if [ $rc = 0 ]; then echo PASS
    elif [ $rc = 124 ]; then echo TIMEOUT
    elif grep -q 'decontaminate' "$err"; then echo DECONTAMINATE
    else echo OTHER; fi
    rm -f "$err"
}
export -f one; export JSC OPTS
for t in ${FILES:-generator-yield-star.js}; do
    printf '%s: ' "$t"
    seq 1 "$RUNS" | xargs -P "$PAR" -I{} bash -c "one $t {}" | sort | uniq -c | tr '\n' ' '
    echo
done
