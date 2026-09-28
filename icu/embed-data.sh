#!/bin/sh
# embed-data.sh <ICU's major version> <package> <libicudata.a> <the target's compiler, with what makes it the target's>...
#
# The library that ICU's data is linked from: the package as it is, under the name udata.cpp looks for. ELF only.
set -eu
major=$1 package=$2 library=$3
shift 3
work=$(mktemp -d)
"$@" -c -x assembler -o "$work/icudt${major}l_dat.o" - <<ASM
.section .rodata
.balign 16
.global icudt${major}_dat
.type icudt${major}_dat, @object
icudt${major}_dat:
.incbin "$package"
.size icudt${major}_dat, . - icudt${major}_dat
.section .note.GNU-stack,"",@progbits
ASM
rm -f "$library"
llvm-ar rcs "$library" "$work/icudt${major}l_dat.o"
rm -r "$work"
