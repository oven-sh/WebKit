#!/bin/bash
# cg.sh <out dir> <name> <jsc without debug information> <jsc arguments...>: exact instruction counts of the main thread, per
# function (callgrind). Output: <out dir>/<name>.fn, lines "<exclusive instructions><TAB><function>".
# VGOPTS: more valgrind options (--smc-check=all-non-file whenever the JIT is on). INCLUSIVE=1: see below.
. "$(dirname "$0")/common.sh"
O=$1; N=$2; J=$3; shift 3; mkdir -p "$O"
rm -f "$O/$N".cg*
"$VALGRIND" --tool=callgrind $VGOPTS --separate-threads=yes --callgrind-out-file="$O/$N.cg" "$J" "$@" > "$O/$N.out" 2> "$O/$N.err"
f=$(ls -S "$O/$N.cg-01" 2>/dev/null || ls -S "$O/$N".cg* | head -1)
callgrind_annotate --threshold=100 --inclusive=no "$f" 2>/dev/null | awk '/^ *[0-9,]+ +\(/ {c=$1; gsub(",","",c); $1=""; $2=""; sub(/^ +/,""); sub(/^[^:]*:/,""); sub(/ \[.*\]$/,""); print c "\t" $0}' > "$O/$N.fn"
# INCLUSIVE=1: also <name>.inc, a function with everything it calls: what an entry point costs whatever is inlined into what
# (incdiff.py). Functions on a cycle of calls have inflated counts; leaves and operations are exact.
[ -n "$INCLUSIVE" ] && callgrind_annotate --threshold=100 --inclusive=yes "$f" 2>/dev/null | awk '/^ *[0-9,]+ +\(/ {c=$1; gsub(",","",c); $1=""; $2=""; sub(/^ +/,""); sub(/^[^:]*:/,""); sub(/ \[.*\]$/,""); print c "\t" $0}' > "$O/$N.inc"
rm -f "$O/$N".cg*
echo "$N total $(awk -F'\t' '{s+=$1} END{print s}' "$O/$N.fn")"
