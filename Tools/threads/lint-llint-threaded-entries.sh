#!/usr/bin/env bash
# lint-llint-threaded-entries.sh <generated LLIntAssembly.h>: the threaded twin of every gated LLInt opcode must be installed by
# _llint_threaded_entry, and nothing else may be. The twins are the labels threaded_llint_* that gateVariants() (LowLevelInterpreter64.asm)
# makes; the entries are the setThreadedEntries() lines of LowLevelInterpreter.asm. Exit 0 when the two lists are equal.
set -eu
GEN=${1:?usage: $0 <path to LLIntAssembly.h>}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
ASM=$ROOT/Source/JavaScriptCore/llint/LowLevelInterpreter.asm
grep -o '"\.type threaded_llint_[a-z_0-9]* , function' "$GEN" | grep -v '_wide' | sed -E 's/.*type threaded_llint_([a-z_0-9]*) .*/\1/' | sort -u > /tmp/llint-twins.$$
grep -o '^ *setThreadedEntries(Op[A-Za-z0-9]*, [a-z_0-9]*)' "$ASM" | sed -E 's/.*, ([a-z_0-9]*)\)/\1/' | sort -u > /tmp/llint-entries.$$
if diff /tmp/llint-twins.$$ /tmp/llint-entries.$$ > /tmp/llint-lint.$$; then
    echo "llint threaded entries: $(wc -l < /tmp/llint-twins.$$) twins, all installed"
    rm -f /tmp/llint-*.$$; exit 0
fi
echo "llint threaded entries differ (< a twin without an entry, > an entry without a twin):"; cat /tmp/llint-lint.$$
rm -f /tmp/llint-*.$$; exit 1
