#!/usr/bin/env bash
# golden-disasm.sh — SPEC-jit Task 13 / I1 golden disassembly diff, flag-off.
#
# I1: with useJSThreads=0, emitted instruction sequences must be identical to
# today's MODULO the blessed carve-outs:
#   (1) field-offset immediates moved by the UNCONDITIONAL repacks (D7:
#       §4.2 packed self word, §4.3 LLIntCachedIdAndOffset, §5.8 m_record
#       append), and
#   (2) the §5.4 LLInt gate branch (one not-taken leap+bbneq per affected
#       fast path; LLInt is asm, not captured by --dumpDisassembly — its
#       flag-off cost is gated by bench-gates.sh's --useJIT=0 run instead.
#
# Mechanism: run the fixed corpus with --dumpDisassembly=true in a maximally
# deterministic configuration, normalize out addresses/hashes, and diff
# against the recorded golden file. Any diff is a FAILURE unless it falls in
# carve-out (1) — in which case re-record (--record) IN THE SAME CHANGE that
# moved the offsets, with a note in docs/threads/INTEGRATE-jit.md.
#
# Usage:
#   golden-disasm.sh [--record] [--golden FILE] /path/to/jsc
#
# Exit codes: 0 = identical (or recorded); 1 = diff found; 2 = usage/env error.

set -u -o pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CORPUS="$SCRIPT_DIR/golden-disasm-corpus.js"
RECORD=0
GOLDEN=""
JSC=""

die() { echo "golden-disasm: error: $*" >&2; exit 2; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        --record) RECORD=1; shift ;;
        --golden) [[ $# -ge 2 ]] || die "--golden needs a value"; GOLDEN="$2"; shift 2 ;;
        -*) die "unknown option: $1" ;;
        *) [[ -z "$JSC" ]] || die "multiple jsc paths"; JSC="$1"; shift ;;
    esac
done
[[ -n "$JSC" && -x "$JSC" ]] || die "path to executable jsc required"

ARCH="$(uname -m)-$(uname -s)"
[[ -n "$GOLDEN" ]] || GOLDEN="$SCRIPT_DIR/golden/disasm-$ARCH.txt"

# Deterministic single-tier-ordering configuration: concurrent JIT off so
# compile order is stable; fixed thresholds; flag OFF (that is the point);
# seed-free corpus. --useDollarVM for ensureArrayStorage.
JSC_FLAGS=(
    --useJSThreads=0
    --useConcurrentJIT=0
    --useDollarVM=1
    --dumpDisassembly=true
    --thresholdForJITAfterWarmUp=100
    --thresholdForJITSoon=100
    --thresholdForOptimizeAfterWarmUp=1000
    --thresholdForOptimizeAfterLongWarmUp=1000
    --thresholdForFTLOptimizeAfterWarmUp=10000
)

normalize() {
    # Strip run-to-run noise, keep instruction shapes + small immediates
    # (field offsets are the carve-out we WANT visible):
    #  - hex addresses / code pointers (>= 5 hex digits) -> ADDR
    #  - disassembly line prefixes "0x...: " -> ""
    #  - CodeBlock hashes (#XXXXXX) and pointer-ish identifiers -> HASH/PTR
    #  - compilation UIDs and byte counts in headers
    sed -E \
        -e 's/0x[0-9a-fA-F]{5,}/ADDR/g' \
        -e 's/#[A-Za-z0-9]{6}/HASH/g' \
        -e 's/\b[0-9a-fA-F]{8,16}\b/PTR/g' \
        -e 's/^ *ADDR: *//' \
        -e '/^Generated JIT code for /s/code for .*$/code for <unit>/' \
        -e '/^Code at \[/d' \
        -e '/CORPUS-SINK/d'
}

OUT="$(mktemp)"
trap 'rm -f "$OUT" "$OUT.norm"' EXIT

"$JSC" "${JSC_FLAGS[@]}" "$CORPUS" > "$OUT" 2>&1
STATUS=$?
if [[ $STATUS -ne 0 ]]; then
    sed -n '1,20p' "$OUT" >&2
    die "jsc failed running the corpus (status $STATUS)"
fi
grep -q "CORPUS-SINK" "$OUT" || die "corpus did not complete (no CORPUS-SINK line)"

normalize < "$OUT" > "$OUT.norm"

if [[ "$RECORD" -eq 1 ]]; then
    mkdir -p "$(dirname "$GOLDEN")"
    cp "$OUT.norm" "$GOLDEN"
    echo "golden-disasm: recorded $(wc -l < "$GOLDEN") normalized lines to $GOLDEN"
    exit 0
fi

[[ -f "$GOLDEN" ]] || die "no golden file at $GOLDEN — run with --record on a blessed build first"

if diff -u "$GOLDEN" "$OUT.norm" > "$OUT.diff" 2>&1; then
    echo "golden-disasm: PASS (flag-off disassembly identical to golden)"
    exit 0
fi

echo "golden-disasm: FAIL — flag-off disassembly differs from golden." >&2
echo "If every hunk is a D7 repack offset-immediate move (I1 carve-out 1)," >&2
echo "re-record with --record and note the blessing in INTEGRATE-jit.md." >&2
sed -n '1,80p' "$OUT.diff" >&2
exit 1
