#!/bin/bash
# Shared settings of the exact-count tools. Source it; do not run it.
#
#   JETSTREAM   the JetStream2 directory (default: PerformanceTests/JetStream2 of this checkout)
#   VALGRIND    the valgrind executable (default: valgrind on PATH); set VALGRIND_LIB too when it is not installed system-wide
#   STRIPPED    where copies of the binaries without debug information are kept (default: $OUT/stripped)
#   TESTS       the tests to run (default: the 36 of the gate)
#
# valgrind 3.18 cannot read the DWARF 5 that clang writes: every tool here runs a copy of the binary stripped of debug
# information (symbols stay), which is all callgrind needs for per-function counts.
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/../../../../.." && pwd)
JETSTREAM=${JETSTREAM:-$REPO/PerformanceTests/JetStream2}
VALGRIND=${VALGRIND:-valgrind}
OUT=${OUT:-$PWD/parity-out}
STRIPPED=${STRIPPED:-$OUT/stripped}
TESTS=${TESTS:-"Air Basic ML Babylon cdjs first-inspector-code-load multi-inspector-code-load Box2D octane-code-load crypto delta-blue earley-boyer gbemu mandreel navier-stokes pdfjs raytrace regexp richards splay typescript octane-zlib FlightPlanner OfflineAssembler UniPoker async-fs float-mm.c hash-map ai-astar gaussian-blur stanford-crypto-aes stanford-crypto-pbkdf2 stanford-crypto-sha256 json-stringify-inspector json-parse-inspector WSL"}
mkdir -p "$OUT" "$STRIPPED"
stripped_copy() { # <jsc> -> path of its copy without debug information
  local s=$STRIPPED/$(basename "$1")
  [ -f "$s" ] && [ "$s" -nt "$1" ] || strip --strip-debug -o "$s" "$1"
  echo "$s"
}
testlist_js() { local first=1 out='testList=['; for t in $TESTS; do [ $first = 1 ] || out+=','; out+="\"$t\""; first=0; done; echo "$out]"; }
