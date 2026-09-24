#!/bin/bash
# profile.sh [rounds]: cycle samples of the 36 tests in one process (main thread only), main and flag off interleaved, for the whole
# run and for the first iteration only; JIT code is attributed by address AND time from --useJITDump (executable memory is reused and
# the dump has no unload records). Output: $OUT/profile/{full,first}-{main,off}-<round>.json; then
#   profile-diff.py full -s 40   and   profile-diff.py first -s 40
# `perf record -c` periods below about 4 M instructions are throttled on some machines; cycles at 2 M / 300 k were not.
. "$(dirname "$0")/common.sh"
# EVENT (default cycles:u), PERIOD_FULL / PERIOD_FIRST and PREFIX (default full/first) select what is sampled: for the locked loads,
#   EVENT=mem_inst_retired.lock_loads:u PERIOD_FULL=1009 PERIOD_FIRST=211 PREFIX=locks profile.sh 1 && profile-diff.py locks-full -s 40
R=${1:-2}; P=$OUT/profile; mkdir -p "$P"; LIST=$(testlist_js)
EVENT=${EVENT:-cycles:u}; PERIOD_FULL=${PERIOD_FULL:-2000003}; PERIOD_FIRST=${PERIOD_FIRST:-300007}; PREFIX=${PREFIX:-}
cd "$JETSTREAM" || exit 1
rec() { # kind cfg round period js-prefix
  local kind=$1 cfg=$2 r=$3 per=$4 js=$5 J d
  J=$(bin_of "$cfg"); d=$P/jd-$kind-$cfg-$r; rm -rf "$d"; mkdir -p "$d"
  perf record -q -k 1 --no-inherit -e "$EVENT" -c "$per" -o "$P/$kind-$cfg-$r.data" "$J" --useJITDump=1 --jitDumpDirectory="$d" -e "$js$LIST" cli.js > /dev/null 2>&1
  local dump; dump=$(ls "$d"/*.dump 2>/dev/null | head -1)
  python3 "$HERE/jit-attribute.py" "$P/$kind-$cfg-$r.data" "$dump" -dump "$P/$kind-$cfg-$r.json" > "$P/$kind-$cfg-$r.cls.txt" 2>&1
  rm -f "$P/$kind-$cfg-$r.data"; rm -rf "$d"
}
for r in $(seq 1 "$R"); do
  for cfg in main off; do rec "${PREFIX:+$PREFIX-}full" "$cfg" "$r" "$PERIOD_FULL" ""; done
  for cfg in main off; do rec "${PREFIX:+$PREFIX-}first" "$cfg" "$r" "$PERIOD_FIRST" "testIterationCount=1;"; done
done
echo "done: $P"
