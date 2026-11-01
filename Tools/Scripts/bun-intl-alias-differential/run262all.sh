#!/bin/bash
# usage: run262all.sh <jsc> <outfile>  ; same harness as the reviewer's run262all.sh, own tmp dir
J=$1; OUT=$2; T=/workspace/WebKit/JSTests/test262
run_one() {
  f=$1; J=$2; T=/workspace/WebKit/JSTests/test262
  inc=$(sed -n 's/^includes: *\[\(.*\)\]/\1/p' "$T/test/$f" | tr ',' ' ')
  tmp=$(mktemp /tmp/refute-t262/tmp/XXXXXXXX.js)
  cat $T/harness/assert.js $T/harness/sta.js $T/harness/doneprintHandle.js > $tmp
  for i in $inc; do cat $T/harness/$i >> $tmp 2>/dev/null; done
  cat "$T/test/$f" >> $tmp
  out=$(ASAN_OPTIONS=detect_leaks=0 timeout 120 "$J" --useTemporal=1 $tmp 2>&1); rc=$?
  rm -f $tmp
  if [ $rc -eq 0 ] && ! echo "$out" | grep -q "Test262:AsyncTestFailure"; then echo "$f PASS"; else echo "$f FAIL $(echo "$out" | grep -m1 -E "Test262Error|Exception|Error|ASSERT|Sanitizer" | cut -c1-200)"; fi
}
export -f run_one
(cd $T/test && find intl402 -name "*.js" | grep -v "_FIXTURE" | sort) | xargs -P 11 -I{} bash -c "run_one {} $J" | sort > $OUT
