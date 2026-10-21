#!/bin/bash
# usage: stress-run.sh <jsc> <outfile> [extra jsc options...]   runs every JSTests/stress/*.js with a 30 s timeout, 8 at a time; prints "<exit> <ms> <test>"
jsc=$1; out=$2; shift 2
cd /workspace/WebKit/JSTests/stress
ls *.js | xargs -P 8 -I{} bash -c 's=$(date +%s%N); timeout 30 '"$jsc"' '"$*"' {} > /dev/null 2>&1; c=$?; echo "$c $(( ($(date +%s%N) - s) / 1000000 )) {}"' > $out
