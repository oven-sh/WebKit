#!/bin/sh
# run-command-line.sh <path to jsc> [program...]
#
# Runs each of command-line/*.py by `python`, which is what the shell is when it is started by a name that begins with that. Each of them starts `python` many times over, as sys.executable, with one
# thing or another on its command line and in its environment. What it prints has to be, byte for byte, what is in the file beside it, which is what CPython 3.14 prints:
#
#     python3.14 command-line/x.py > command-line/x.expected
#
# Where things are is worked out from where the program is, so it is put where `make install` would put it: bin/python3, beside lib/python3.14.

# The part of the library that is written in Python does not come with the engine. It is CPython's, as it is.
[ -n "$PYTHONPATH" ] || { echo "PYTHONPATH is to name the Lib directory of CPython 3.14" >&2; exit 2; }
library=$PYTHONPATH
unset PYTHONPATH

jsc=$1
shift
cd "$(dirname "$0")/command-line" || exit 2
[ $# -eq 0 ] && set -- *.py

home=$(mktemp -d) || exit 2
trap 'rm -rf "$home"' EXIT
# A link to the program would not do: it is where the program really is that counts.
mkdir -p "$home/bin" "$home/lib/python3.14/lib-dynload" || exit 2
cp "$jsc" "$home/bin/python3" || exit 2
for entry in "$library"/*; do
    ln -s "$entry" "$home/lib/python3.14/"
done

failures=0
runs=0
kept=${TMPDIR:-/tmp}/python-command-lines-that-failed
for program in "$@"; do
    runs=$((runs + 1))
    "$home/bin/python3" "$program" > "$home/actual" 2>&1
    if ! cmp -s "$home/actual" "${program%.py}.expected"; then
        failures=$((failures + 1))
        mkdir -p "$kept"
        cp "$home/actual" "$kept/$program.txt"
        echo "FAIL: $program (see $kept/$program.txt)"
    fi
done
echo "$((runs - failures)) of $runs pass"
[ $failures -eq 0 ]
