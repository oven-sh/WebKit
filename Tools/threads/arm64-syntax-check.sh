#!/usr/bin/env bash
# arm64-syntax-check.sh <build dir> [parallelism]: compile every translation unit of the JavaScriptCore, WTF and bmalloc libraries of an
# x86-64 build directory with `-fsyntax-only --target=aarch64-linux-gnu`, using the host's headers with a small shim (glibc chooses its
# 32-bit word size and an x86-only `regparm` attribute when the target is not x86-64). It finds code that names an x86-64 emitter without
# a CPU guard, or uses a platform macro that only one target defines: what breaks an arm64 build at compile time, not at link or run
# time (inline assembly is not assembled and no library is linked). Prints the translation units with errors and the first error of each.
set -u
BUILD=$(cd "${1:?usage: $0 <build dir> [parallelism]}" && pwd); PAR=${2:-32}
SHIM=$(mktemp -d); mkdir -p "$SHIM/gnu" "$SHIM/bits"; : > "$SHIM/gnu/stubs-32.h"
printf '#define __WORDSIZE 64\n#define __WORDSIZE_TIME64_COMPAT32 1\n#define __SYSCALL_WORDSIZE 64\n' > "$SHIM/bits/wordsize.h"
sed 's/# define __cleanup_fct_attribute __attribute__ ((__regparm__ (1)))/# define __cleanup_fct_attribute/' /usr/include/x86_64-linux-gnu/bits/pthreadtypes-arch.h > "$SHIM/bits/pthreadtypes-arch.h"
CXX13=$(ls -d /usr/include/c++/* | sort -V | tail -1); V=$(basename "$CXX13")
WORK=$(mktemp -d)
(cd "$BUILD" && ninja -t commands lib/libJavaScriptCore.a lib/libWTF.a lib/libbmalloc.a 2>/dev/null) | grep -E '(clang\+\+|clang)[^ ]* ' | grep -E ' -c ' > "$WORK/commands.txt"
n=$(wc -l < "$WORK/commands.txt")
transform() {
    sed -E "s#/usr/bin/ccache ##; s# -o [^ ]+ -c # -fsyntax-only --target=aarch64-linux-gnu -isystem $SHIM -isystem $CXX13 -isystem /usr/include/x86_64-linux-gnu/c++/$V -isystem /usr/include/x86_64-linux-gnu -c #; s#-MD -MT [^ ]+ -MF [^ ]+##; s# -mcx16##; s# -Winvalid-pch##; s# -Xclang -include-pch -Xclang [^ ]+##; s# -Xclang -include -Xclang [^ ]+##"
}
i=0
while IFS= read -r cmd; do i=$((i + 1)); echo "$cmd" | transform > "$WORK/cmd-$i.sh"; done < "$WORK/commands.txt"
export BUILD WORK
run_one() { local i=$1; local out; out=$(cd "$BUILD" && bash "$WORK/cmd-$i.sh" 2>&1 | sed 's/\x1b\[[0-9;]*m//g' | grep -E ' error: |fatal error' | head -3); [ -n "$out" ] && { echo "== $(grep -o '[^ ]*\.cpp' "$WORK/cmd-$i.sh" | tail -1)"; echo "$out"; }; true; }
export -f run_one
seq 1 "$i" | xargs -P "$PAR" -I{} bash -c 'run_one {}' > "$WORK/result.txt" 2>&1
echo "arm64 syntax check: $n translation units, $(grep -c '^== ' "$WORK/result.txt") with errors"
cat "$WORK/result.txt" | head -80
rm -rf "$SHIM" "$WORK"
