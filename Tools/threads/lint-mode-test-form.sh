#!/usr/bin/env bash
# lint-mode-test-form.sh <jsc or library, x86-64 ELF>: every test of the threads mode must address the mode page directly
# (`cmpb $0, page(%rip)` or a load from it). An instruction that only computes the page's address (`lea page(%rip), reg`)
# is a test that went through the global offset table: the code was compiled without the hidden name of the page
# (runtime/ThreadsModePage.h), and costs one instruction more at each of its tests. The functions that write or freeze
# the page take its address and are allowed. Prints the forms and the offending functions; exit 1 when there is one.
set -u
BIN=${1:?usage: $0 <binary>}
OUT=$(objdump -d -C --no-show-raw-insn "$BIN" 2>/dev/null | awk '
  /^[0-9a-f]+ <.*>:$/ { fn = $0; sub(/^[0-9a-f]+ </, "", fn); sub(/>:$/, "", fn); next }
  /<g_jscThreadsModePage/ { form[$2]++; if ($2 == "lea" && fn !~ /latchThreadsModePage|freezeThreadsModePage/) bad[fn]++ }
  END { for (f in form) printf "form %s %d\n", f, form[f]; for (f in bad) printf "bad %d %s\n", bad[f], f }')
echo "$OUT" | grep '^form' | sort -k3 -n -r | awk '{printf "%s%s: %s", (NR > 1 ? ", " : "mode tests by form: "), $2, $3} END {print ""}'
if echo "$OUT" | grep -q '^bad'; then
    echo "tests of the mode through the offset table:"; echo "$OUT" | grep '^bad' | sort -k2 -n -r | head -40 | cut -c5-
    exit 1
fi
echo "no test of the mode goes through the offset table"
