#!/bin/bash
# Samples the stack of the JS thread of a benchmark with one gdb session: run 40 ms, stop, backtrace, repeat.
# usage: gdb-sample2.sh <bun-profile> <samples> <out> <script and args...>
exe=$1; n=$2; out=$3; shift 3
"$exe" "$@" > /tmp/gdb-sample-bench.out 2>&1 &
pid=$!
sleep 2
cat > /tmp/gdb-sample2.gdb <<G
set pagination off
set confirm off
handle SIGINT stop print nopass
set \$i = 0
while \$i < $n
  thread 1
  bt 36
  echo ===SAMPLE===\n
  set \$i = \$i + 1
  shell (sleep 0.04; kill -INT $pid) > /dev/null 2>&1 &
  continue
end
detach
quit
G
gdb -p $pid -batch -nx -x /tmp/gdb-sample2.gdb > /tmp/gdb-sample2.raw 2>&1
kill $pid 2>/dev/null; wait $pid 2>/dev/null
awk 'BEGIN{RS="===SAMPLE===\n"} {n=split($0, L, "\n"); s=""; for(i=1;i<=n;i++) if (L[i] ~ /^#/) { l=L[i]; sub(/ \(.*/, "", l); sub(/0x[0-9a-f]+ in /, "", l); s = s l "|" } if (s != "") print s}' /tmp/gdb-sample2.raw > "$out"
echo "samples: $(wc -l < "$out")"
