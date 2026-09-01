#!/usr/bin/env bash
# r3b re-triage: every r3b crash (.js newer than 2026-06-19 02:25 across the
# main + -B + -C storage dirs, including duplicates/) against the CURRENT
# Fuzz jsc. Adapted from triage-r1-batch.sh; same signature derivation.
#
# Expectation (FUZZ.md "Campaign r3b"): with the §47 AS routing fix landed,
# the 125-count r3-001 family (ASSERT !hasAnyArrayStorage @
# ConcurrentButterfly.cpp trySegmentedTransition) goes NOREPRO. The 1-count
# r3-002 (storeTaggedButterflyWordConcurrent) is a Debug-only assert tripped
# by stack overflow during a property transition; NOREPRO under
# RelWithDebInfo+ASAN (rc=3 uncaught RangeError).
#
# Output: Tools/threads/fuzz/crashes/r3b/INDEX.tsv (sig -> repro path),
#         WebKitBuild/Fuzz/triage-r3b/all.tsv (every crash file -> sig).
set -uo pipefail

REPO=/root/WebKit
JSC="$REPO/WebKitBuild/Fuzz/bin/jsc"
OUT="$REPO/Tools/threads/fuzz/crashes/r3b"
WORK="$REPO/WebKitBuild/Fuzz/triage-r3b"
PAR=${PAR:-12}
mkdir -p "$OUT" "$WORK/stderr"
: > "$WORK/all.tsv"

# Lane pin: detect_stack_use_after_return=0 is REQUIRED (FUZZ.md / CVE-AUDIT B9).
export ASAN_OPTIONS="detect_stack_use_after_return=0:abort_on_error=1:symbolize=1:detect_leaks=0:allocator_may_return_null=1:handle_segv=1:handle_abort=1:malloc_context_size=20"
export ASAN_SYMBOLIZER_PATH=${ASAN_SYMBOLIZER_PATH:-/opt/llvm-21/bin/llvm-symbolizer}

# r3b crash set: the 4h campaign on 2026-06-19 02:28-06:30 across main + -B
# + -C storage (FUZZ.md "Campaign r3b"). Includes duplicates/ so the literal
# "all 128" is covered (on-disk count may exceed 128 by deterministic copies).
mapfile -t FILES < <(find \
        "$REPO"/WebKitBuild/Fuzz/fuzzilli-storage/crashes \
        "$REPO"/WebKitBuild/Fuzz/fuzzilli-storage-B/crashes \
        "$REPO"/WebKitBuild/Fuzz/fuzzilli-storage-C/crashes \
        -name "*.js" -newermt "2026-06-19 02:25" ! -newermt "2026-06-19 06:35" 2>/dev/null | sort)

echo "Triaging ${#FILES[@]} r3b crash files (PAR=$PAR)..." >&2

triage_one() {
    local f="$1"
    local base targs errf rc kind frames sig out
    base=$(basename "$f" .js)
    errf="$WORK/stderr/$base.stderr"
    # Extract per-crash target args (drop the binary path and --reprl). The
    # header is Fuzzilli-authored (not fuzzed-JS-controlled), but allowlist
    # anyway: only --opt[=val] tokens with [A-Za-z0-9=._-], no shell metachars.
    targs=$(grep -m1 '^// TARGET ARGS:' "$f" \
        | sed -E 's@^// TARGET ARGS: [^ ]+/jsc @@; s/--reprl//')
    local -a targs_arr=()
    for tok in $targs; do
        [[ "$tok" =~ ^--[A-Za-z0-9][A-Za-z0-9=._-]*$ ]] && targs_arr+=("$tok")
    done
    [[ ${#targs_arr[@]} -eq 0 ]] && targs_arr=(--useJSThreads=true)
    rc=0; kind=""; sig=""
    for try in 1 2 3; do
        timeout -s KILL 25 "$JSC" "${targs_arr[@]}" "$f" >/dev/null 2>"$errf"
        rc=$?
        [[ $rc -ne 0 ]] && break
    done
    out=$(cat "$errf" 2>/dev/null)
    if [[ $rc -eq 124 || $rc -eq 137 ]]; then
        kind="TIMEOUT"; sig="TIMEOUT::hang"
    elif [[ $rc -eq 0 ]]; then
        kind="NOREPRO"; sig="NOREPRO"
    elif grep -q "AddressSanitizer" <<<"$out"; then
        kind=$(grep -m1 -oE 'AddressSanitizer: [A-Za-z_-]+|SEGV on unknown address' <<<"$out" | head -1)
        [[ -z "$kind" ]] && kind="asan-other"
        frames=$(grep -E '^\s*#[0-9]+ 0x' <<<"$out" \
            | grep -vE '__asan|__sanitizer|__interceptor|libc\.so|ld-linux|libpthread|__libc|__GI_|_start\b|WTFCrash\b|abort\b|raise\b|gsignal' \
            | sed -E 's/^\s*#[0-9]+ 0x[0-9a-f]+ (in )?//; s/ \(BuildId.*//; s/\+0x[0-9a-f]+//; s/ \/[^ ]*$//; s/<[^>]*>//g' \
            | awk 'NF' | head -3 | paste -sd'|')
        [[ -z "$frames" ]] && frames=$(grep -m1 'SUMMARY:' <<<"$out" | sed 's/^.*SUMMARY: //; s/ (.*//')
        sig="$kind::$frames"
    elif grep -qE 'ASSERTION FAILED|RELEASE_ASSERT|SHOULD NEVER BE REACHED|FATAL' <<<"$out"; then
        kind="ASSERT"
        local msg loc
        msg=$(grep -m1 -E 'ASSERTION FAILED|RELEASE_ASSERT|SHOULD NEVER BE REACHED|FATAL' <<<"$out" | sed 's/^.*ASSERTION FAILED: //; s/^.*RELEASE_ASSERT//' | cut -c1-120)
        loc=$(grep -m1 -oE '[A-Za-z0-9_./-]+\.(cpp|h):[0-9]+' <<<"$out" | head -1)
        sig="ASSERT::${msg}@${loc}"
    elif [[ $rc -eq 132 || $rc -eq 133 || $rc -eq 134 || $rc -eq 135 || $rc -eq 136 || $rc -eq 138 || $rc -eq 139 ]]; then
        kind="signal-$((rc-128))"
        local loc
        loc=$(grep -m1 -oE '[A-Za-z0-9_./-]+\.(cpp|h):[0-9]+' <<<"$out" | head -1)
        sig="$kind::${loc:-<no-report>}"
    else
        kind="exit-$rc"
        sig="exit-$rc::$(tail -1 <<<"$out" | cut -c1-100)"
    fi
    printf '%s\t%d\t%s\t%s\n' "$f" "$rc" "$kind" "$sig"
}
export -f triage_one
export WORK JSC

# Parallel triage.
printf '%s\n' "${FILES[@]}" | xargs -P "$PAR" -I{} bash -c 'triage_one "$@"' _ {} >> "$WORK/all.tsv"

echo >&2; echo "=== Signature histogram ===" >&2
cut -f4 "$WORK/all.tsv" | sort | uniq -c | sort -rn | tee "$WORK/histogram.txt" >&2

# Dedupe -> OUT
echo >&2; echo "=== Unique repros -> $OUT ===" >&2
: > "$OUT/INDEX.tsv"
n=0
while IFS=$'\t' read -r sig src rc kind; do
    [[ "$sig" == "NOREPRO" ]] && continue
    n=$((n+1))
    id=$(printf 'r3b-%03d' "$n")
    {
        echo "// === r3b triage ==="
        echo "// ID: $id"
        echo "// SIGNATURE: $sig"
        echo "// KIND: $kind"
        echo "// EXIT: $rc"
        echo "// SOURCE: $src"
        echo "// =================="
        cat "$src"
    } > "$OUT/$id.js"
    sb="$WORK/stderr/$(basename "$src" .js).stderr"
    [[ -f "$sb" ]] && cp "$sb" "$OUT/$id.stderr"
    printf '%s\t%s\t%s\t%s\n' "$id" "$kind" "$sig" "$src" >> "$OUT/INDEX.tsv"
    echo "  $id  [$kind]  $sig" >&2
done < <(sort -t$'\t' -k4,4 -k1,1 "$WORK/all.tsv" | awk -F'\t' '!seen[$4]++ {print $4"\t"$1"\t"$2"\t"$3}')

echo >&2
echo "Unique signatures: $n" >&2
echo "Full index: $WORK/all.tsv" >&2
echo "Repros:     $OUT/" >&2
