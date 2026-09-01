#!/usr/bin/env bash
# V5a flag-off identity check: every-50th JSTests/stress test (40 tests),
# run with --useJSThreads=false vs no flags; rc+stdout+stderr must match.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
JSC="${JSC:-$ROOT/WebKitBuild/Release/bin/jsc}"
mapfile -t ALL < <(ls "$ROOT/JSTests/stress"/*.js | sort)
TESTS=()
i=0
for f in "${ALL[@]}"; do
    if (( i % 50 == 0 )); then TESTS+=("$f"); fi
    i=$((i+1))
    [[ ${#TESTS[@]} -ge 40 ]] && break
done
echo "v5a: ${#TESTS[@]} tests, jsc=$JSC"
MISMATCH=0
for f in "${TESTS[@]}"; do
    a_out=$(timeout -k 5 60 "$JSC" --useJSThreads=false "$f" 2>&1); a_rc=$?
    b_out=$(timeout -k 5 60 "$JSC" "$f" 2>&1); b_rc=$?
    if [[ "$a_rc" != "$b_rc" || "$a_out" != "$b_out" ]]; then
        MISMATCH=$((MISMATCH+1))
        echo "MISMATCH ${f#$ROOT/} rc:$a_rc/$b_rc"
        diff <(echo "$a_out") <(echo "$b_out") | head -10
    else
        echo "OK ${f#$ROOT/} rc=$a_rc"
    fi
done
echo "v5a: mismatches=$MISMATCH"
[[ "$MISMATCH" -eq 0 ]]
