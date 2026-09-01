#!/usr/bin/env bash
# gc-stress-matrix.sh — re-run the EXISTING threads corpus (JSTests/threads/,
# read-only reference) under a matrix of GC-stress / allocator-reuse-exposure
# option sets, with the same pass/fail discipline as Tools/threads/run-tests.sh
# (header directives honored, option-set probe => SKIP not FAIL, per-test
# timeout, summary table).
#
# Modes (option names verified against Source/JavaScriptCore/runtime/
# OptionsList.h on this tree; all four are availability=Normal — present in
# Release AND Debug builds. useZombieMode's OptionsList description calls it
# a "debugging option" but it is NOT compiled out of release builds; if a
# future rebase demotes any of these to Debug-only availability, the probe
# below turns the whole mode into SKIPs rather than FAILs):
#   scribble  --scribbleFreeCells=1
#                 sweep scribbles freed cells: stale-pointer reads return
#                 poison instead of plausible stale values (this is the mode
#                 that gives zombie-uaf-canary.js its teeth).
#   zombie    --useZombieMode=1
#                 dead objects are scribbled with the 0xbadbeef0 pattern;
#                 complements scribble with object-granularity poisoning.
#   contgc    --collectContinuously=1
#                 a dedicated thread triggers collections continuously:
#                 maximizes mutator/GC overlap windows.
#   eden      --collectContinuously=1 --collectContinuouslyPeriodMS=1
#             --forceGCSlowPaths=1
#                 eden-pressure combo: 1ms continuous collection period keeps
#                 the young generation churning while forceGCSlowPaths sends
#                 every JIT fast allocation down the slow path, so allocation
#                 and collection interleave at maximum frequency.
#
# Mode options are APPENDED AFTER each test's own //@ header options. jsc
# applies the last occurrence of a repeated option, so a test that pins one
# of these options in its header is overridden in that mode — by design: the
# matrix asks "does the corpus survive this GC regime", not "does each test's
# preferred regime hold". Tests whose premise depends on a default are
# expected to self-report THREADS-PREMISE-SKIP (counted as SKIP, exactly like
# run-tests.sh).
#
# Corpus (same globs as run-tests.sh, plus the gc-stress suite when present):
#   JSTests/threads/{api,atomics,races}/*.js
#   JSTests/threads/heap-*.js
#   JSTests/threads/{objectmodel,vmstate,gc-stress}/*.js
#   JSTests/threads/jit/**/*.js
#
# Usage:
#   Tools/threads/gc-stress-matrix.sh (--quick | --full | --mode=NAME[,...])
#                                     [--filter=SUBSTR] [--list]
#
# The full 4-mode whole-corpus matrix is HOURS of saturating load; a bare
# invocation therefore REFUSES to run: it prints the resolved plan size and
# worst-case time bound and exits 2. You must pick a scope explicitly.
#
# Options:
#   --quick          Subset run: corpus restricted to races/, jit/ and
#                    gc-stress/ (the suites with the most GC/allocator
#                    surface), modes restricted to scribble + contgc.
#                    Intended as a fast pre-flight; --full for rungs.
#   --full           The whole matrix: all four modes over the full corpus.
#                    Heavy by design — run only when the machine is yours.
#   --mode=...       Comma-separated subset of: scribble zombie contgc eden.
#                    Overrides --quick's mode subset if both are given.
#   --filter=SUBSTR  Only run tests whose repo-relative path contains SUBSTR.
#   --list           Print the resolved (mode, test) plan and exit (allowed
#                    without a scope flag; lists the full matrix by default).
#
# Every executing invocation prints the resolved plan (modes x runs) and the
# worst-case wall-clock bound (runs x per-mode timeout) BEFORE the first jsc
# invocation.
#
# Per-test budget: GC-stress modes are deliberately one to two orders of
# magnitude slower than the default regime the corpus was calibrated for
# (<30s under default GC). Each mode therefore gets a documented timeout
# MULTIPLIER on top of THREADS_TEST_TIMEOUT_SECS:
#   scribble x1, zombie x1 (sweep-time poison: little slowdown),
#   contgc   x2 (continuous collections steal mutator time),
#   eden     x4 (1ms collection period + every JIT allocation on the slow
#                path: the heaviest regime by far).
# A run that exceeds its budget is reported in a separate TIMEOUT column —
# still a nonzero exit (a hang IS the failure mode threaded tests die by),
# but labeled so triage can distinguish budget exhaustion from a true hang
# without raising THREADS_TEST_TIMEOUT_SECS blindly.
#
# Environment:
#   JSC=/path/to/jsc            jsc binary (default:
#                               WebKitBuild/{Debug,Release}/bin/jsc).
#   THREADS_TEST_TIMEOUT_SECS=N Base per-test timeout (default 120; 0
#                               disables). Per-mode multipliers above apply
#                               on top.
#
# Exit codes: 0 = no failures or timeouts in any executed mode,
#             1 = at least one FAIL or TIMEOUT, 2 = usage/env error or
#             refused bare invocation.
#
# This script is INERT in this change: it is staged for the Validate phase's
# controlled smoke and must not be executed as part of authoring.

set -u -o pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
JT="$ROOT/JSTests/threads"

MODE_NAMES=(scribble zombie contgc eden)
MODE_OPTS=(
    "--scribbleFreeCells=1"
    "--useZombieMode=1"
    "--collectContinuously=1"
    "--collectContinuously=1 --collectContinuouslyPeriodMS=1 --forceGCSlowPaths=1"
)
# Per-mode timeout multipliers on THREADS_TEST_TIMEOUT_SECS (see header).
MODE_TIMEOUT_MULT=(1 1 2 4)

FILTER=""
QUICK=0
FULL=0
LIST=0
MODES_ARG=""

die() { echo "gc-stress-matrix: error: $*" >&2; exit 2; }

print_help() {
    awk 'NR == 1 { next } /^#/ { sub(/^# ?/, ""); print; next } { exit }' \
        "${BASH_SOURCE[0]}"
}

for arg in "$@"; do
    case "$arg" in
        --filter=*) FILTER="${arg#--filter=}" ;;
        --quick)    QUICK=1 ;;
        --full)     FULL=1 ;;
        --mode=*)   MODES_ARG="${arg#--mode=}" ;;
        --list)     LIST=1 ;;
        -h|--help)  print_help; exit 0 ;;
        *)          die "unknown argument: $arg" ;;
    esac
done

# ---- resolve the mode subset ----
SELECTED_NAMES=()
SELECTED_OPTS=()
SELECTED_MULTS=()
select_mode() { # $1 = mode name
    local i
    for i in "${!MODE_NAMES[@]}"; do
        if [[ "${MODE_NAMES[$i]}" == "$1" ]]; then
            SELECTED_NAMES+=("${MODE_NAMES[$i]}")
            SELECTED_OPTS+=("${MODE_OPTS[$i]}")
            SELECTED_MULTS+=("${MODE_TIMEOUT_MULT[$i]}")
            return 0
        fi
    done
    die "unknown mode: $1 (known: ${MODE_NAMES[*]})"
}
if [[ -n "$MODES_ARG" ]]; then
    IFS=',' read -r -a requested <<<"$MODES_ARG"
    for m in "${requested[@]}"; do
        [[ -n "$m" ]] && select_mode "$m"
    done
    [[ ${#SELECTED_NAMES[@]} -gt 0 ]] || die "--mode selected no modes"
elif [[ "$QUICK" -eq 1 ]]; then
    select_mode scribble
    select_mode contgc
else
    # --full, --list, or a bare invocation: resolve the whole matrix. A bare
    # invocation is REFUSED below (after the plan is counted) — the full
    # matrix only executes behind an explicit --full.
    for m in "${MODE_NAMES[@]}"; do select_mode "$m"; done
fi

# Does this invocation intend to execute tests?
EXECUTE=0
if [[ "$LIST" -eq 0 && ( "$FULL" -eq 1 || "$QUICK" -eq 1 || -n "$MODES_ARG" ) ]]; then
    EXECUTE=1
fi

# ---- ambient JSC_* option env: detect and WARN, never scrub (same policy
# and rationale as run-tests.sh — some rungs configure entirely via env) ----
AMBIENT_JSC_ENV="$(env | LC_ALL=C grep -o '^JSC_[A-Za-z0-9_]*' | LC_ALL=C sort | tr '\n' ' ')"
AMBIENT_JSC_ENV="${AMBIENT_JSC_ENV% }"
if [[ -n "$AMBIENT_JSC_ENV" ]]; then
    echo "gc-stress-matrix: WARNING: ambient JSC_* option env present and HONORED (not scrubbed): $AMBIENT_JSC_ENV" >&2
    echo "gc-stress-matrix: WARNING: JSC_* env applies before all argv options including mode options; premise-self-checking tests will report SKIP (THREADS-PREMISE-SKIP)" >&2
fi

# ---- resolve jsc ----
JSC="${JSC:-}"
if [[ -z "$JSC" ]]; then
    for candidate in "$ROOT/WebKitBuild/Debug/bin/jsc" "$ROOT/WebKitBuild/Release/bin/jsc"; do
        [[ -x "$candidate" ]] && JSC="$candidate" && break
    done
fi
if [[ "$EXECUTE" -eq 1 ]]; then
    [[ -n "$JSC" && -x "$JSC" ]] || die "no jsc binary (set JSC=/path/to/jsc)"
fi

# ---- collect the corpus ----
FILES=()
if [[ "$QUICK" -eq 1 ]]; then
    for f in "$JT"/races/*.js "$JT"/gc-stress/*.js; do
        [[ -e "$f" ]] && FILES+=("$f")
    done
    if [[ -d "$JT/jit" ]]; then
        while IFS= read -r f; do FILES+=("$f"); done \
            < <(find "$JT/jit" -name '*.js' -type f 2>/dev/null | sort)
    fi
else
    for f in "$JT"/api/*.js "$JT"/atomics/*.js "$JT"/races/*.js \
             "$JT"/heap-*.js "$JT"/objectmodel/*.js "$JT"/vmstate/*.js \
             "$JT"/gc-stress/*.js; do
        [[ -e "$f" ]] && FILES+=("$f")
    done
    if [[ -d "$JT/jit" ]]; then
        while IFS= read -r f; do FILES+=("$f"); done \
            < <(find "$JT/jit" -name '*.js' -type f 2>/dev/null | sort)
    fi
fi
[[ ${#FILES[@]} -gt 0 ]] || die "no tests found under $JT"

# ---- header parsing (same directive subset as run-tests.sh) ----
# Echoes one line per run: space-joined jsc args, "<plain>" for a bare run,
# "<skip>" for //@ skip files.
plan_runs() {
    local file="$1"
    local req_opts=()
    local rundefault_lines=()
    local line
    while IFS= read -r line; do
        [[ "$line" == "//@"* ]] || break
        case "$line" in
            "//@ skip"*)
                echo "<skip>"
                return
                ;;
            "//@ requireOptions("*)
                while IFS= read -r opt; do
                    req_opts+=("$opt")
                done < <(grep -o '"[^"]*"' <<<"$line" | tr -d '"')
                ;;
            "//@ runDefault"*)
                local opts
                opts="$(grep -o '"[^"]*"' <<<"$line" | tr -d '"' | tr '\n' ' ')"
                rundefault_lines+=("${opts% }")
                ;;
            *)
                :
                ;;
        esac
    done < "$file"

    local req="${req_opts[*]:-}"
    if [[ ${#rundefault_lines[@]} -eq 0 ]]; then
        if [[ -n "$req" ]]; then echo "$req"; else echo "<plain>"; fi
        return
    fi
    local rd
    for rd in "${rundefault_lines[@]}"; do
        local combined="$req${req:+ }$rd"
        combined="${combined# }"
        if [[ -n "$combined" ]]; then echo "$combined"; else echo "<plain>"; fi
    done
}

# ---- option-availability probe (cached; plain arrays for bash 3.2) ----
# An option set this jsc build rejects (e.g. a Debug-only option against a
# Release binary, or a not-yet-landed OptionsList.h hunk) is a SKIP, never a
# FAIL: probe against an empty program first.
PROBED_OPTSETS=()
PROBED_RESULTS=()
options_supported() { # $1 = space-joined args ("" => trivially supported)
    local key="$1"
    [[ -z "$key" ]] && return 0
    local i
    for i in "${!PROBED_OPTSETS[@]}"; do
        if [[ "${PROBED_OPTSETS[$i]}" == "$key" ]]; then
            [[ "${PROBED_RESULTS[$i]}" == "yes" ]]
            return
        fi
    done
    local probe_args=()
    read -r -a probe_args <<<"$key"
    local verdict=no
    if "$JSC" "${probe_args[@]}" -e '' >/dev/null 2>&1; then
        verdict=yes
    fi
    PROBED_OPTSETS+=("$key")
    PROBED_RESULTS+=("$verdict")
    [[ "$verdict" == "yes" ]]
}

# ---- per-test timeout (timeouts are reported in their own TIMEOUT column:
# still a nonzero overall exit — threaded tests hang by failure mode — but
# distinguishable from FAILs so budget exhaustion under the deliberately-slow
# modes does not masquerade as a regression) ----
TEST_TIMEOUT_SECS="${THREADS_TEST_TIMEOUT_SECS:-120}"
HAVE_TIMEOUT=0
if [[ "$TEST_TIMEOUT_SECS" != "0" ]] && command -v timeout >/dev/null 2>&1; then
    HAVE_TIMEOUT=1
fi

# ---- count the resolved plan (runs are mode-independent: header parsing
# does not depend on mode options) ----
RUNS_PER_MODE=0
for file in "${FILES[@]}"; do
    rel="${file#"$ROOT"/}"
    [[ -n "$FILTER" && "$rel" != *"$FILTER"* ]] && continue
    while IFS= read -r runspec; do
        [[ "$runspec" == "<skip>" ]] && continue
        RUNS_PER_MODE=$((RUNS_PER_MODE + 1))
    done < <(plan_runs "$file")
done

print_plan() {
    local total=$((RUNS_PER_MODE * ${#SELECTED_NAMES[@]}))
    echo "gc-stress-matrix: plan: ${#SELECTED_NAMES[@]} mode(s) x $RUNS_PER_MODE run(s) = $total total runs"
    if [[ "$TEST_TIMEOUT_SECS" == "0" || "$HAVE_TIMEOUT" -eq 0 ]]; then
        echo "gc-stress-matrix: worst-case bound: UNBOUNDED (no per-test timeout in effect)"
        return
    fi
    local worst=0 mi
    for mi in "${!SELECTED_NAMES[@]}"; do
        worst=$((worst + RUNS_PER_MODE * TEST_TIMEOUT_SECS * SELECTED_MULTS[mi]))
    done
    echo "gc-stress-matrix: worst-case bound: ~$((worst / 60)) minutes" \
         "(per-mode timeouts:$(for mi in "${!SELECTED_NAMES[@]}"; do printf ' %s=%ss' "${SELECTED_NAMES[$mi]}" "$((TEST_TIMEOUT_SECS * SELECTED_MULTS[mi]))"; done))"
}

# ---- refuse a bare invocation: the full matrix is hours of saturating
# load and must be requested explicitly ----
if [[ "$EXECUTE" -eq 0 && "$LIST" -eq 0 ]]; then
    echo "gc-stress-matrix: REFUSING to run without an explicit scope." >&2
    print_plan >&2
    echo "gc-stress-matrix: pass --full for the whole matrix, --quick for the pre-flight subset," >&2
    echo "gc-stress-matrix: --mode=NAME[,...] for a surgical run, or --list to print the plan." >&2
    exit 2
fi

if [[ "$EXECUTE" -eq 1 ]]; then
    print_plan
fi

TMP_OUT="$(mktemp "${TMPDIR:-/tmp}/threads-gc-stress.XXXXXX")" || die "mktemp failed"
trap 'rm -f "$TMP_OUT"' EXIT

RUN_ONE_TIMED_OUT=0
run_one() { # timeout_secs, file, args...
    local tmo="$1" file="$2"; shift 2
    RUN_ONE_TIMED_OUT=0
    local status
    if [[ "$HAVE_TIMEOUT" -eq 1 ]]; then
        timeout -k 10 "$tmo" "$JSC" "$@" "$file" >"$TMP_OUT" 2>&1
        status=$?
        if [[ $status -eq 124 || $status -eq 137 ]]; then
            RUN_ONE_TIMED_OUT=1
            echo "gc-stress-matrix: TIMEOUT after ${tmo}s (budget exhausted or hang): $file" >>"$TMP_OUT"
        fi
    else
        "$JSC" "$@" "$file" >"$TMP_OUT" 2>&1
        status=$?
    fi
    return $status
}

# ---- matrix loop ----
MODE_PASS=()
MODE_FAIL=()
MODE_TIMEOUTED=()
MODE_SKIP=()
ALL_FAILED_LABELS=()
ALL_TIMEOUT_LABELS=()
TOTAL_FAIL=0
TOTAL_TIMEOUT=0

for mi in "${!SELECTED_NAMES[@]}"; do
    mode="${SELECTED_NAMES[$mi]}"
    mode_opts_str="${SELECTED_OPTS[$mi]}"
    mode_opts=()
    read -r -a mode_opts <<<"$mode_opts_str"
    mode_timeout=$((TEST_TIMEOUT_SECS * SELECTED_MULTS[mi]))

    if [[ "$LIST" -eq 0 ]]; then
        echo
        echo "==== mode: $mode ($mode_opts_str; per-test timeout ${mode_timeout}s) ===="
    fi

    PASS=0
    FAIL=0
    TIMEOUTED=0
    SKIPPED=0

    for file in "${FILES[@]}"; do
        rel="${file#"$ROOT"/}"
        if [[ -n "$FILTER" && "$rel" != *"$FILTER"* ]]; then
            continue
        fi

        runindex=0
        while IFS= read -r runspec; do
            if [[ "$runspec" == "<skip>" ]]; then
                SKIPPED=$((SKIPPED + 1))
                [[ "$LIST" -eq 0 ]] && echo "SKIP [$mode] $rel (//@ skip)"
                continue
            fi
            args=()
            if [[ "$runspec" != "<plain>" ]]; then
                read -r -a args <<<"$runspec"
            fi
            # blocking-gate.js gets --can-block-is-false appended (annex T2:
            # the runner appends it; same as run-tests.sh).
            if [[ "$(basename "$file")" == "blocking-gate.js" ]]; then
                args+=("--can-block-is-false")
            fi
            # Mode options go LAST so they win over header options.
            args+=(${mode_opts[@]+"${mode_opts[@]}"})

            runindex=$((runindex + 1))
            label="[$mode] $rel"
            [[ "$runindex" -gt 1 || "$runspec" != "<plain>" ]] && label="[$mode] $rel [${args[*]}]"

            if [[ "$LIST" -eq 1 ]]; then
                echo "$label"
                continue
            fi

            if ! options_supported "${args[*]:-}"; then
                SKIPPED=$((SKIPPED + 1))
                echo "SKIP $label (option(s) not supported by this jsc build — e.g. a Debug-only option vs a Release binary, or a not-yet-landed OptionsList.h hunk)"
                continue
            fi
            if run_one "$mode_timeout" "$file" ${args[@]+"${args[@]}"}; then
                if grep -qs '^THREADS-PREMISE-SKIP:' "$TMP_OUT"; then
                    SKIPPED=$((SKIPPED + 1))
                    echo "SKIP $label (premise inverted by mode/ambient configuration; ambient JSC_* env: ${AMBIENT_JSC_ENV:-none detected})"
                    sed 's/^/     | /' "$TMP_OUT"
                else
                    PASS=$((PASS + 1))
                    echo "PASS $label"
                fi
            else
                status=$?
                if [[ "$RUN_ONE_TIMED_OUT" -eq 1 ]]; then
                    TIMEOUTED=$((TIMEOUTED + 1))
                    TOTAL_TIMEOUT=$((TOTAL_TIMEOUT + 1))
                    ALL_TIMEOUT_LABELS+=("$label")
                    echo "TIMEOUT $label (exit $status after ${mode_timeout}s — budget exhausted or hang; see header for per-mode budgets)"
                    sed 's/^/     | /' "$TMP_OUT"
                else
                    FAIL=$((FAIL + 1))
                    TOTAL_FAIL=$((TOTAL_FAIL + 1))
                    ALL_FAILED_LABELS+=("$label")
                    echo "FAIL $label (exit $status)"
                    sed 's/^/     | /' "$TMP_OUT"
                fi
            fi
        done < <(plan_runs "$file")
    done

    MODE_PASS+=("$PASS")
    MODE_FAIL+=("$FAIL")
    MODE_TIMEOUTED+=("$TIMEOUTED")
    MODE_SKIP+=("$SKIPPED")
done

if [[ "$LIST" -eq 1 ]]; then
    exit 0
fi

# ---- summary table ----
echo
echo "gc-stress-matrix: summary"
printf '  %-10s %6s %6s %8s %6s\n' "mode" "pass" "fail" "timeout" "skip"
for mi in "${!SELECTED_NAMES[@]}"; do
    printf '  %-10s %6s %6s %8s %6s\n' "${SELECTED_NAMES[$mi]}" \
        "${MODE_PASS[$mi]}" "${MODE_FAIL[$mi]}" "${MODE_TIMEOUTED[$mi]}" \
        "${MODE_SKIP[$mi]}"
done
if [[ "$TOTAL_FAIL" -gt 0 || "$TOTAL_TIMEOUT" -gt 0 ]]; then
    echo
    if [[ "$TOTAL_FAIL" -gt 0 ]]; then
        echo "gc-stress-matrix: failed runs:"
        printf '  %s\n' "${ALL_FAILED_LABELS[@]}"
    fi
    if [[ "$TOTAL_TIMEOUT" -gt 0 ]]; then
        echo "gc-stress-matrix: timed-out runs (budget exhausted or hang — triage separately from FAILs):"
        printf '  %s\n' "${ALL_TIMEOUT_LABELS[@]}"
    fi
    exit 1
fi
echo
echo "gc-stress-matrix: PASS (all executed modes green)"
exit 0
