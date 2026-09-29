#!/bin/sh
#
# bun-as-jsc.sh [--option=value ...] [-m] <file> [argument ...]
#
# Runs a test with bun, taking what the shell would take, so that it can be given to the scripts here in place of the shell:
#
#     BUN=/path/to/bun ./run-programs.sh "$PWD/bun-as-jsc.sh"
#
# bun has the engine's options from the environment, tells a module by its name, and has the library in it.
#
# These of interop/ depend on the shell, and are not expected to pass: contexts.mjs and resized-while-being-compiled.mjs use functions that only it has, errors.mjs has its way of writing a stack in it, and
# import-python.mjs and importing-javascript.py have how it names a module and what it says when it cannot find one.

[ -n "$BUN" ] || { echo "BUN is to be the path of bun" >&2; exit 2; }
here=$(cd "$(dirname "$0")" && pwd)
while [ $# -gt 0 ]; do
    case $1 in
        --*=*) option=${1#--}; export "BUN_JSC_${option%%=*}=${option#*=}" ;;
        -m) ;;
        *) break ;;
    esac
    shift
done
# The shell does not keep back what Python writes, so that it comes out in order with what JavaScript writes. bun does as `python` does, for a program that is Python's.
export PYTHONUNBUFFERED=1
exec "$BUN" --preload "$here/shell-for-bun.js" "$@"
