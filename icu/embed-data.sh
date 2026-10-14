#!/bin/sh
# embed-data.sh elf|coff <ICU's major version> <library> <the target's compiler, with what makes it the target's>...
#
# The library that ICU's data is linked from: /icudt.dat as it is, under the name udata.cpp looks for.
set -eu
format=$1 major=$2 library=$3
shift 3
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
{
    # COFF's read-only section is .rdata, and its symbols have no type or size.
    if [ "$format" = coff ]; then echo '.section .rdata,"dr"'; else echo '.section .rodata'; fi
    echo ".balign 16"
    echo ".global icudt${major}_dat"
    [ "$format" = coff ] || echo ".type icudt${major}_dat, @object"
    echo "icudt${major}_dat:"
    echo '.incbin "/icudt.dat"'
    [ "$format" = coff ] || echo ".size icudt${major}_dat, . - icudt${major}_dat"
    [ "$format" = coff ] || echo '.section .note.GNU-stack,"",@progbits'
} > "$work/icudt.S"
"$@" -c -o "$work/icudt${major}l_dat.o" "$work/icudt.S"
rm -f "$library"
llvm-ar rcs "$library" "$work/icudt${major}l_dat.o"
