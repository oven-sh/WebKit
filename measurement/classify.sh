#!/bin/bash
# usage: classify.sh <log> <results dir> <env> <runs> <binary A> <binary B>
# For each failing test of the log, runs its repro command with both binaries and prints pass counts.
log=$1; results=$2; envs=$3; runs=$4; A=$5; B=$6
grep "^FAIL: " "$log" | awk '{print $2}' | while read name; do
  cmd=$(grep -F "$name: Repro command:" "$log" | head -1 | sed 's/^.*Repro command: *//')
  [ -z "$cmd" ] && { echo "$name: no repro command"; continue; }
  file=${name%.*}            # stress/foo.js
  dir=$(dirname "$file")     # stress
  collection=$(echo "$name" | cut -d/ -f1)
  testdir="$results/.tests/$dir"
  [ -d "$testdir" ] || { echo "$name: no dir $testdir"; continue; }
  line="$name:"
  for bin in "$A" "$B"; do
    pass=0
    # Strip the env prefix and the leading "jsc"; handle the bytecode-cache helper form.
    c=$(echo "$cmd" | sed 's/^\([A-Za-z_0-9]*=[^ ]* \)*//' | sed 's/^jsc //')
    if echo "$c" | grep -q "bytecode-cache-test-helper.sh"; then
      c=$(echo "$c" | sed "s|\.\./\.\./\.vm/JavaScriptCore.framework/Helpers/jsc|$bin|")
      full="env $envs sh $c"
    else
      full="env $envs $bin $c"
    fi
    for i in $(seq 1 $runs); do
      if (cd "$testdir" && timeout 300 $full > /tmp/classify.out 2>&1); then pass=$((pass+1)); fi
    done
    line="$line  $(basename $(dirname $(dirname $bin)))=$pass/$runs"
  done
  echo "$line"
done
