#!/bin/bash
# Shared settings of the flag-off measurement harness. Source it; do not run it.
#
#   FLAGOFF_MAIN    the reference `jsc` (the rebase base built with the same flags as the candidate)
#   FLAGOFF_BRANCH  the candidate `jsc`, run with useJSThreads unset (flag off)
#                   (JSC_MAIN and JSC_BRANCH are still accepted and are taken out of the environment here: the shell
#                   reads every variable that begins with JSC_ as an option, and the branch skips the option scan of
#                   the environment when there is none, so with the harness's own variables in it the start-up
#                   measured was not the start-up of a process in an ordinary environment.)
#   JETSTREAM     the JetStream2 directory (default: PerformanceTests/JetStream2 of this checkout)
#   OUT           where results go (default: ./flagoff-out)
#
# Time is MAIN-THREAD cycles (perf stat --no-inherit), not whole-process instructions: those include compiler and
# collector threads and weigh a locked instruction at one. The score is the geometric mean of the first iteration, the
# mean of the four worst and the mean of all iterations: always look at Startup / Worst Case / Average beside it.
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/../../../.." && pwd)
JETSTREAM=${JETSTREAM:-$REPO/PerformanceTests/JetStream2}
OUT=${OUT:-$PWD/flagoff-out}
mkdir -p "$OUT"
FLAGOFF_MAIN=${FLAGOFF_MAIN:-$JSC_MAIN}; FLAGOFF_BRANCH=${FLAGOFF_BRANCH:-$JSC_BRANCH}
unset JSC_MAIN JSC_BRANCH
: "${FLAGOFF_MAIN:?set FLAGOFF_MAIN to the reference jsc}"
: "${FLAGOFF_BRANCH:?set FLAGOFF_BRANCH to the candidate jsc}"
export FLAGOFF_MAIN FLAGOFF_BRANCH

# The 36 tests of PERF-RESULTS (the gate's own selection).
TESTS="Air Basic ML Babylon cdjs first-inspector-code-load multi-inspector-code-load Box2D octane-code-load crypto delta-blue earley-boyer gbemu mandreel navier-stokes pdfjs raytrace regexp richards splay typescript octane-zlib FlightPlanner OfflineAssembler UniPoker async-fs float-mm.c hash-map ai-astar gaussian-blur stanford-crypto-aes stanford-crypto-pbkdf2 stanford-crypto-sha256 json-stringify-inspector json-parse-inspector WSL"
testlist_js() { # -> testList=["a","b",...] for JetStream's cli.js
  local first=1 out='testList=['
  for t in $TESTS; do [ $first = 1 ] || out+=','; out+="\"$t\""; first=0; done
  echo "$out]"
}
bin_of() { case $1 in main) echo "$FLAGOFF_MAIN";; off) echo "$FLAGOFF_BRANCH";; *) echo "unknown configuration $1" >&2; return 1;; esac; }
