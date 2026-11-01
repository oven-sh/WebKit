#!/bin/bash
# usage: run-stress.sh <jsc> <outfile> ; runs the Intl-touching stress tests once each in the default mode
J=$1; OUT=$2
run_one() {
  f=$1; J=$2
  opts=$(sed -n 's/^\/\/@ *requireOptions(\(.*\))/\1/p' "$f" | tr -d '"' | tr ',' ' ' | head -1)
  out=$(ASAN_OPTIONS=detect_leaks=0 timeout 300 "$J" --useDollarVM=1 $opts "$f" 2>&1); rc=$?
  if [ $rc -eq 0 ]; then echo "$f PASS"; else echo "$f FAIL($rc) $(echo "$out" | grep -m1 -E "Error|ASSERT|Sanitizer|Exception" | cut -c1-160)"; fi
}
export -f run_one
cd /workspace/WebKit/JSTests/stress && grep -l -E "Intl\.|toLocale|localeCompare" *.js | sort | xargs -P 11 -I{} bash -c "run_one {} $J" | sort > $OUT
