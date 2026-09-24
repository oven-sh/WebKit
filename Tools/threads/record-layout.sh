#!/usr/bin/env bash
# record-layout.sh <build dir> <class> <header> [<header>...] : clang's layout of a class (offsets, sizes, holes) as the build compiles it,
# to find the padding a change to a hot cell type (Structure, the executables, CodeBlock, CallLinkInfo, ...) put in or can take out.
# The build directory supplies the compile flags; nothing is built.
set -eu
BUILD=${1:?usage: $0 <build dir> <class> <header>...}; CLS=$2; shift 2
TU=$(mktemp --suffix=.cpp); OUT=$(mktemp); { echo '#include "config.h"'; for h in "$@"; do echo "#include \"$h\""; done; } > "$TU"
CMD=$(cd "$BUILD" && ninja -t commands Source/JavaScriptCore/CMakeFiles/JavaScriptCore.dir/__/__/JavaScriptCore/DerivedSources/unified-sources/UnifiedSource-jit-1.cpp.o | tail -1)
CMD=$(echo "$CMD" | sed -E "s#/usr/bin/ccache ##; s# -o [^ ]+ -c [^ ]+# -fsyntax-only -Xclang -fdump-record-layouts-complete -c $TU#; s#-MD -MT [^ ]+ -MF [^ ]+##; s# -Winvalid-pch##; s# -Xclang -include-pch -Xclang [^ ]+##; s# -Xclang -include -Xclang [^ ]+##")
(cd "$BUILD" && bash -c "$CMD") > "$OUT" 2>&1 || true
start=$(grep -n -E "^ +0 \| (class|struct) (JSC::)?${CLS}\$" "$OUT" | head -1 | cut -d: -f1)
[ -n "$start" ] || { echo "class $CLS not found"; rm -f "$TU" "$OUT"; exit 1; }
tail -n +"$start" "$OUT" | awk '{print} /\[sizeof=/ {exit}'
rm -f "$TU" "$OUT"
