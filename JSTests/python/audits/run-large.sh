#!/bin/sh
#
# run-large.sh <path to jsc> [seconds for each]
#
# What comes of making things that there is no room for, or barely room for: strings of 2**31 characters, bytes of 2**32, lists of 2**28. It is not one of the audits that run-audits.sh runs, since it takes
# gigabytes of memory and a quarter of an hour, and there is nothing to compare it with: CPython would make most of these, if the machine had the memory.
#
# Each line of large.txt is what to do first, ` ;; `, and an expression. Each is run in a process of its own, and what became of it is one line: `made`, with the class and the length of what was made, or
# the class of what was raised and the beginning of what it says. large.expected has what those lines should be, which was read and found right. Nothing at all is what is looked for: the engine fell over.

jsc=$1
seconds=${2:-120}
cd "$(dirname "$0")" || exit 2
scratch=$(mktemp -d) || exit 2
trap 'rm -rf "$scratch"' EXIT
program=$scratch/large.py
results=$scratch/results

while IFS= read -r line; do
    setup=${line%% ;; *}
    expression=${line##* ;; }
    cat > "$program" <<END
$setup
try:
    r = $expression
    print('made', type(r).__name__, len(r) if hasattr(r, '__len__') else '')
except BaseException as e:
    print(type(e).__name__, str(e)[:40])
END
    # Whatever it is doing when the time is up, it is not going to finish.
    outcome=$(perl -e 'alarm shift; exec @ARGV' "$seconds" "$jsc" "$program" 2>&1 | tail -1 | cut -c1-60)
    printf '%s => %s\n' "$line" "$outcome" | sed -e 's/ *$//' >> "$results"
done < large.txt

if [ ! -f large.expected ]; then
    cp "$results" large.expected
    echo "wrote large.expected"
    exit 0
fi
diff large.expected "$results" && echo "$(wc -l < "$results" | tr -d ' ') the same"
