#!/bin/sh
# headers.sh <directory> <-DNAME=VALUE>...    in icu4c/source
#
# ICU's public headers, for whoever compiles against the libraries. uconfig.h starts with the switches that the
# libraries were compiled with, so that what those leave out is not declared either: using it then fails to compile,
# where it would fail to link, or do nothing, in the builds that use these libraries and in no others.
set -eu
to=$1
shift
mkdir -p "$to"
cp common/unicode/*.h i18n/unicode/*.h "$to"
{
    for flag; do
        name=${flag#-D}
        printf '#define %s %s\n' "${name%%=*}" "${name#*=}"
    done
    cat common/unicode/uconfig.h
} > "$to/uconfig.h"
