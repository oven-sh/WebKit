#!/usr/bin/env bash
# §48 campaign-r48 crash triage. Same signature derivation as
# triage-r47.sh; gate cares ONLY about whether any of the r47 family
# signatures (storeTaggedButterflyWordConcurrent / slowDownAndWasteMemory /
# DeferrableRefCounted) reappear.
set -uo pipefail

REPO=/root/WebKit
JSC="$REPO/WebKitBuild/Fuzz/bin/jsc"
OUT="$REPO/Tools/threads/fuzz/crashes/r48"
WORK="$REPO/WebKitBuild/Fuzz/triage-r48-campaign"
mkdir -p "$OUT" "$WORK/stderr"
: > "$WORK/all.tsv"

export ASAN_OPTIONS="detect_stack_use_after_return=0:abort_on_error=1:symbolize=1:detect_leaks=0:allocator_may_return_null=1:handle_segv=1:handle_abort=1:malloc_context_size=20"
export ASAN_SYMBOLIZER_PATH=${ASAN_SYMBOLIZER_PATH:-/opt/llvm-21/bin/llvm-symbolizer}

mapfile -t FILES < <(ls "$REPO"/WebKitBuild/Fuzz/fuzzilli-storage-r48/crashes/*.js 2>/dev/null | sort)
echo "Triaging ${#FILES[@]} r48 crash files..." >&2

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
    # Flaky-tagged sources get more retries.
    tries=3; [[ "$base" == *flaky* ]] && tries=10
    for try in $(seq 1 $tries); do
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
n_r47=$(grep -cE 'storeTaggedButterflyWordConcurrent|slowDownAndWasteMemory|DeferrableRefCounted' "$WORK/all.tsv" || true)
echo "r47 family signatures (storeTaggedButterflyWordConcurrent / slowDownAndWasteMemory / DeferrableRefCounted): $n_r47" >&2

# Dedupe -> OUT
: > "$OUT/INDEX.tsv"; n=0
while IFS=$'\t' read -r sig src rc; do
    [[ "$sig" == "NOREPRO" ]] && continue
    n=$((n+1)); id=$(printf 'r48-%03d' "$n")
    {
        echo "// === r48 triage ==="
        echo "// ID: $id"; echo "// SIGNATURE: $sig"; echo "// EXIT: $rc"; echo "// SOURCE: $src"
        echo "// =================="; cat "$src"
    } > "$OUT/$id.js"
    cp "$WORK/stderr/$(basename "$src" .js).stderr" "$OUT/$id.stderr" 2>/dev/null
    printf '%s\t%s\t%s\n' "$id" "$sig" "$src" >> "$OUT/INDEX.tsv"
done < <(sort -t$'\t' -k3,3 -k1,1 "$WORK/all.tsv" | awk -F'\t' '!seen[$3]++ {print $3"\t"$1"\t"$2}')
echo "Unique signatures: $n" >&2
