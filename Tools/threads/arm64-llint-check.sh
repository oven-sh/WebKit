#!/usr/bin/env bash
# arm64-llint-check.sh <x86-64 build dir>: generate the interpreter for arm64 and assemble it, on an x86-64 host without an arm64
# sysroot. The offsets extractor is compiled for aarch64 (an object file is enough: the offline assembler reads the offsets
# out of its bytes), the offline assembler is run on it, which fails on an instruction the arm64 backend does not have, and
# LowLevelInterpreter.cpp is compiled for aarch64 with the generated assembly, which fails on what the assembler rejects.
# Complements arm64-syntax-check.sh, which does not see the interpreter.
set -u
BUILD=$(cd "${1:?usage: $0 <build dir>}" && pwd)
SRC=$(cd "$(dirname "$0")/../.." && pwd)/Source/JavaScriptCore
SHIM=$(mktemp -d); WORK=$(mktemp -d); mkdir -p "$SHIM/gnu" "$SHIM/bits"; : > "$SHIM/gnu/stubs-32.h"
printf '#define __WORDSIZE 64\n#define __WORDSIZE_TIME64_COMPAT32 1\n#define __SYSCALL_WORDSIZE 64\n' > "$SHIM/bits/wordsize.h"
sed 's/# define __cleanup_fct_attribute __attribute__ ((__regparm__ (1)))/# define __cleanup_fct_attribute/' /usr/include/x86_64-linux-gnu/bits/pthreadtypes-arch.h > "$SHIM/bits/pthreadtypes-arch.h"
CXX13=$(ls -d /usr/include/c++/* | sort -V | tail -1); V=$(basename "$CXX13")
mkdir -p "$WORK/derived"
cross() { # <source file name> <output>: the build's own command for that file, retargeted, with the files generated here first on the include path
    local cmd
    cmd=$( (cd "$BUILD" && ninja -t commands bin/jsc 2>/dev/null) | grep -E "clang\+\+[^ ]* .* -c [^ ]*/$1( |\$)" | head -1)
    [ -n "$cmd" ] || { echo "no compile command for $1" >&2; return 1; }
    echo "$cmd" | sed -E "s#/usr/bin/ccache ##; s#(clang\+\+[^ ]*) #\1 -I$WORK/derived #; s# -o [^ ]+ -c # --target=aarch64-linux-gnu -isystem $SHIM -isystem $CXX13 -isystem /usr/include/x86_64-linux-gnu/c++/$V -isystem /usr/include/x86_64-linux-gnu -o $2 -c #; s#-MD -MT [^ ]+ -MF [^ ]+##; s# -mcx16##; s# -Winvalid-pch##; s# -Xclang -include-pch -Xclang [^ ]+##; s# -Xclang -include -Xclang [^ ]+##" > "$WORK/cmd.sh"
    (cd "$BUILD" && bash "$WORK/cmd.sh") 2>&1 | sed 's/\x1b\[[0-9;]*m//g' | grep -E ' error: |fatal error' | head -20
    [ -s "$2" ]
}
D="$BUILD/JavaScriptCore/DerivedSources/"
echo "== settings and offsets extractors for aarch64"
ruby "$SRC/offlineasm/generate_settings_extractor.rb" -I"$D" "$SRC/llint/LowLevelInterpreter.asm" "$WORK/derived/LLIntDesiredSettings.h" ARM64 2>&1 | tail -5
cross LLIntSettingsExtractor.cpp "$WORK/settings.o" || { echo "FAIL: the settings extractor does not compile for aarch64"; exit 1; }
ruby "$SRC/offlineasm/generate_offset_extractor.rb" -I"$D" "$SRC/llint/LowLevelInterpreter.asm" "$WORK/settings.o" "$WORK/derived/LLIntDesiredOffsets.h" ARM64 normal 2>&1 | tail -5
cross LLIntOffsetsExtractor.cpp "$WORK/extractor.o" || { echo "FAIL: the offsets extractor does not compile for aarch64"; exit 1; }
echo "== offline assembler, arm64 backend"
(cd "$D" && CMAKE_CXX_COMPILER_ID=Clang ruby "$SRC/offlineasm/asm.rb" -I"$D" "$SRC/llint/LowLevelInterpreter.asm" "$WORK/extractor.o" "$WORK/derived/LLIntAssembly.h" normal --binary-format=ELF 2>&1 | tail -20)
[ -s "$WORK/derived/LLIntAssembly.h" ] || { echo "FAIL: the offline assembler produced nothing"; exit 1; }
echo "lines of assembly: $(wc -l < "$WORK/derived/LLIntAssembly.h"); backend named: $(grep -m1 -o 'OFFLINE_ASM_ARM64[A-Z0-9_]*' "$WORK/derived/LLIntAssembly.h" | head -1)"
echo "== assembling it (LowLevelInterpreter.cpp for aarch64)"
if cross LowLevelInterpreter.cpp "$WORK/llint.o"; then echo "arm64 interpreter: generated and assembled"; else echo "FAIL: LowLevelInterpreter.cpp does not assemble for aarch64"; fi
rm -rf "$SHIM" "$WORK"
