#!/usr/bin/env bash
# §48 re-triage: every r47 crash file + the two r3b survivors (r3b-001 /
# r3b-002 SOURCE paths) against the CURRENT Fuzz jsc. Same signature
# derivation as triage-r47.sh / triage-r3b-batch.sh; expectation: with the
# r47 setButterfly-audit-escape fixes (slowDownAndWasteMemory cell-locked
# tag-preserving CAS, shiftButterflyAfterFlattening / flattenDictionary-
# StructureImpl world-stopped tag-preserving store, existingBufferInButterfly
# segment-aware) all 11 go NOREPRO.
set -uo pipefail

REPO=/root/WebKit
JSC="$REPO/WebKitBuild/Fuzz/bin/jsc"
WORK="$REPO/WebKitBuild/Fuzz/triage-r48"
mkdir -p "$WORK/stderr"
: > "$WORK/all.tsv"

export ASAN_OPTIONS="detect_stack_use_after_return=0:abort_on_error=1:symbolize=1:detect_leaks=0:allocator_may_return_null=1:handle_segv=1:handle_abort=1:malloc_context_size=20"
export ASAN_SYMBOLIZER_PATH=${ASAN_SYMBOLIZER_PATH:-/opt/llvm-21/bin/llvm-symbolizer}

# r47 storage crashes (9 files) + the two r3b survivor SOURCE paths.
mapfile -t FILES < <(
    ls "$REPO"/WebKitBuild/Fuzz/fuzzilli-storage-r47/crashes/*.js 2>/dev/null
    awk -F'\t' '{print $NF}' "$REPO/Tools/threads/fuzz/crashes/r3b/INDEX.tsv" 2>/dev/null
)
echo "Triaging ${#FILES[@]} files (r47 storage + r3b survivors)..." >&2

for f in "${FILES[@]}"; do
    base=$(basename "$f" .js)
    errf="$WORK/stderr/$base.stderr"
    targs=$(grep -m1 '^// TARGET ARGS:' "$f" \
        | sed -E 's@^// TARGET ARGS: [^ ]+/jsc @@; s/--reprl//')
    targs_arr=()
    for tok in $targs; do
        [[ "$tok" =~ ^--[A-Za-z0-9][A-Za-z0-9=._-]*$ ]] && targs_arr+=("$tok")
    done
    [[ ${#targs_arr[@]} -eq 0 ]] && targs_arr=(--useJSThreads=true)
    rc=0
    for try in 1 2 3; do
        timeout -s KILL 25 "$JSC" "${targs_arr[@]}" "$f" >/dev/null 2>"$errf"
        rc=$?
        [[ $rc -ne 0 ]] && break
    done
    out=$(cat "$errf" 2>/dev/null)
    if [[ $rc -eq 124 || $rc -eq 137 ]]; then
        sig="TIMEOUT::hang"
    elif [[ $rc -eq 0 ]]; then
        sig="NOREPRO"
    elif grep -q "AddressSanitizer" <<<"$out"; then
        kind=$(grep -m1 -oE 'AddressSanitizer: [A-Za-z_-]+|SEGV on unknown address' <<<"$out" | head -1)
        frames=$(grep -E '^\s*#[0-9]+ 0x' <<<"$out" \
            | grep -vE '__asan|__sanitizer|__interceptor|libc\.so|ld-linux|libpthread|__libc|__GI_|_start\b|WTFCrash\b|abort\b|raise\b|gsignal' \
            | sed -E 's/^\s*#[0-9]+ 0x[0-9a-f]+ (in )?//; s/ \(BuildId.*//; s/\+0x[0-9a-f]+//; s/ \/[^ ]*$//; s/<[^>]*>//g' \
            | awk 'NF' | head -3 | paste -sd'|')
        sig="${kind}::${frames}"
    elif grep -qE 'ASSERTION FAILED|RELEASE_ASSERT' <<<"$out"; then
        msg=$(grep -m1 -E 'ASSERTION FAILED|RELEASE_ASSERT' <<<"$out" | cut -c1-120)
        loc=$(grep -m1 -oE '[A-Za-z0-9_./-]+\.(cpp|h):[0-9]+' <<<"$out" | head -1)
        sig="ASSERT::${msg}@${loc}"
    elif [[ $rc -ge 128 ]]; then
        sig="signal-$((rc-128))::$(grep -m1 -oE '[A-Za-z0-9_./-]+\.(cpp|h):[0-9]+' <<<"$out" | head -1)"
    else
        sig="exit-$rc"
    fi
    printf '%s\t%d\t%s\n' "$f" "$rc" "$sig" >> "$WORK/all.tsv"
done

echo >&2; echo "=== Signature histogram ===" >&2
cut -f3 "$WORK/all.tsv" | sort | uniq -c | sort -rn | tee "$WORK/histogram.txt" >&2

echo >&2
n_r47sig=$(grep -cE 'storeTaggedButterflyWordConcurrent|slowDownAndWasteMemory|DeferrableRefCounted' "$WORK/all.tsv" || true)
echo "r47 family signatures (storeTaggedButterflyWordConcurrent / slowDownAndWasteMemory / DeferrableRefCounted): $n_r47sig" >&2
