#!/usr/bin/env bash
# Round-1 crash triage: rerun every Fuzzilli crash .js under the Fuzz jsc,
# capture the ASAN/crash report, derive a dedupe signature
# (crash kind + top in-JSC frames), and emit one line per crash:
#   <file>\t<kind>\t<signature>
# Usage: triage-r1.sh <crash.js> <extra jsc flags...>
set -u
JSC=/root/WebKit/WebKitBuild/Fuzz/bin/jsc
f="$1"; shift
export ASAN_OPTIONS="abort_on_error=1:symbolize=1:detect_leaks=0:malloc_context_size=30:allocator_may_return_null=1:handle_segv=1"
# Pull per-crash flags from the Fuzzilli header (lines like "// FLAGS: ..." absent;
# fuzzilli stores target args in settings.json, plus per-program flags are uniform).
out=$(timeout -s KILL 10 "$JSC" --useJSThreads=1 "$@" "$f" 2>&1)
rc=$?
kind="none"
sig=""
if [ $rc -eq 137 ] || [ $rc -eq 124 ]; then
    kind="timeout"
    sig="TIMEOUT"
elif echo "$out" | grep -q "AddressSanitizer"; then
    kind=$(echo "$out" | grep -m1 -oE "SEGV|heap-use-after-free|heap-buffer-overflow|stack-buffer-overflow|stack-use-after-return|global-buffer-overflow|use-after-poison|attempting double-free|SUMMARY: AddressSanitizer: [a-zA-Z-]+" | head -1)
    [ -z "$kind" ] && kind="asan-other"
    # top frames: first 4 #N lines, function names only, strip addresses/paths
    sig=$(echo "$out" | grep -E "^\s*#[0-9]+ 0x" | head -6 \
        | sed -E 's/^\s*#[0-9]+ 0x[0-9a-f]+ in //; s/\(.*//; s/ .*\/.*//; s/\+0x[0-9a-f]+//' \
        | sed -E 's/<[^>]*>//g; s/[[:space:]]+$//' | head -4 | paste -sd'|')
    [ -z "$sig" ] && sig=$(echo "$out" | grep -m1 "SUMMARY:" | sed 's/.*SUMMARY: //')
elif [ $rc -eq 134 ] || echo "$out" | grep -qE "ASSERTION FAILED|RELEASE_ASSERT|FATAL"; then
    kind="abort"
    sig=$(echo "$out" | grep -m1 -E "ASSERTION FAILED|RELEASE_ASSERT|FATAL|Aborted" | cut -c1-160)
    # add the crash-context line (file:line) if present
    loc=$(echo "$out" | grep -m1 -oE "[A-Za-z0-9_/.-]+\.(cpp|h):[0-9]+")
    sig="${sig}@${loc:-}"
elif [ $rc -ne 0 ]; then
    kind="exit-$rc"
    sig=$(echo "$out" | tail -2 | tr '\n' '|' | cut -c1-160)
fi
printf '%s\t%s\t%s\t%s\n' "$f" "$rc" "$kind" "$sig"
