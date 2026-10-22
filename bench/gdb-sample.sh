#!/bin/bash
# Samples the stack of the main thread of a benchmark. usage: gdb-sample.sh <bun-profile> <samples> <out> <script and args...>
exe=$1; n=$2; out=$3; shift 3
"$exe" "$@" > /tmp/gdb-sample-bench.out 2>&1 &
pid=$!
sleep 1
: > "$out"
for i in $(seq 1 $n); do
  kill -0 $pid 2>/dev/null || break
  gdb -p $pid -batch -nx -ex "set pagination off" -ex "thread 1" -ex "bt 36" 2>/dev/null | grep -E "^#" | sed -e 's/ (.*//' -e 's/0x[0-9a-f]* in //' | tr '\n' '|' >> "$out"
  echo >> "$out"
done
kill $pid 2>/dev/null; wait $pid 2>/dev/null
echo "samples: $(wc -l < "$out")"
