#!/usr/bin/env bash
# run-tests.sh — SPEC-api §8 runner for the shared-memory threads corpus.
#
# Runs, with their //@ header directives honored:
#   JSTests/threads/{api,atomics,races}/*.js          (api-owned, SPEC-api §8)
# plus, when present (not owned here):
#   JSTests/threads/heap-*.js                          (heap)
#   JSTests/threads/objectmodel/*.js                   (objectmodel)
#   JSTests/threads/vmstate/*.js                       (vmstate N6)
#   JSTests/threads/jit/**/*.js                        (jit)
# With --cve, the corpus is INSTEAD the CVE susceptibility suite
# (JSTests/threads/cve/*.js, thread-cve-audit/-close) — the mechanical form
# of the audit's "literal CVE suite run": same directive handling, plus the
# threadsExpectFail directive below. The cve suite is not part of the
# default corpus because its [EXPECTED-FAIL] rows pin OPEN audit rulings
# (they fail GIL-off by design until the ruled fix lands).
#
# Header directives understood (subset of run-jsc-stress-tests):
#   //@ skip                       skip the file
#   //@ requireOptions("a", ...)   append args to every run of the file
#   //@ runDefault("a", ...)       one run per directive with exactly those
#                                  args (ta-path-unchanged.js runs both ways)
#   //@ threadsExpectFail("gilOff")
#                                  the test pins an OPEN, already-ruled
#                                  audit deferral: under an EFFECTIVE
#                                  GIL-off configuration (decided by
#                                  probing $vm.useThreadGIL() with the
#                                  run's exact option set + ambient env —
#                                  the post-U0-validation mode, never a
#                                  guess from flag spelling) the verdict is
#                                  INVERTED: nonzero exit = XFAIL (counted
#                                  separately, not a suite failure); exit 0
#                                  = XPASS = suite FAILURE (either the
#                                  ruled fix landed — flip the annotation —
#                                  or the pinned failure went latent and
#                                  must be investigated, not celebrated).
#                                  GIL-on (or mode unknown) the test runs
#                                  normally. Annotate ONLY tests whose
#                                  GIL-off failure is deterministic; a
#                                  flaky race pin would flap XFAIL/XPASS.
# blocking-gate.js additionally gets --can-block-is-false appended (annex
# T2: "runner appends it"; the threads.yaml stanza does the same at INT).
#
# Runs whose option set is rejected by the jsc build (unknown option — e.g.
# a foreign workstream's corpus naming options whose OptionsList.h hunk has
# not landed yet) are SKIPped, not FAILed: the option set is probed against
# an empty program first. SKIP happens ONLY on jsc's "ERROR: invalid option"
# stderr diagnostic; any other nonzero probe exit (notably signal deaths
# like 134/SIGABRT under ambient JSC_* env, or probe timeouts 124/137) is a
# FAIL of every run that needs that option set — an engine crash must never
# masquerade as "option not supported". A run where the probe skips >50% of
# planned runs (or skips everything that was planned) exits nonzero as
# vacuous. The API-I1..I24 coverage grep ignores //@ skip'ped files
# (API-I14's SPEC-mandated deferral to INT 9.2-6 is reported explicitly).
#
# Usage:
#   Tools/threads/run-tests.sh [--filter=SUBSTR] [--amplify] [--list] [--cve]
#   Tools/threads/run-tests.sh --gates
#
# Options:
#   --cve             Run the CVE susceptibility suite (JSTests/threads/cve)
#                     INSTEAD of the default corpus, honoring
#                     threadsExpectFail. Composable with --filter/--amplify.
#   --filter=SUBSTR   Only run tests whose repo-relative path contains SUBSTR.
#   --amplify         Wrap every run in Tools/threads/amplify.sh (race
#                     amplification); if amplify.sh is missing, warn once and
#                     run plain.
#   --list            Print the resolved test list and exit.
#   --gates           SPEC-api §10 task-13 gate mode (sole option; degrades
#                     gracefully per G15). Runs, in order:
#                       1. races/ via this runner, --amplify (runner falls
#                          back to plain with a warning if amplify.sh is
#                          absent — frozen §8 fallback);
#                       2. races/ under the TSAN no-JIT target
#                          (WebKitBuild/TSan/bin/jsc, override TSAN_JSC=) —
#                          only if that target exists, else SKIP;
#                       3. bench self-gate (API-I19, implement half):
#                          bench-gate.sh --record then gate on the SAME build
#                          must exit 0; uses a throwaway baseline file so the
#                          integrator-recorded Tools/threads/baseline.json is
#                          never touched (the authoritative I19 comparison
#                          against the pre-workstream baseline runs at INT).
#                     Exit 0 iff every gate that ran passed (SKIPs are not
#                     failures).
#
# Environment:
#   JSC=/path/to/jsc  jsc binary (default: WebKitBuild/{Debug,Release}/bin/jsc).
#   AMPLIFY_RUNS=M    Forwarded to amplify.sh --runs (default: amplify's own).
#   TSAN_JSC=PATH     TSAN no-JIT jsc for gate 2 (default:
#                     WebKitBuild/TSan/bin/jsc; missing => gate SKIPped).
#   TSAN_OPTIONS=...  Honored if set; otherwise gate 2 uses the TSAN.md
#                     defaults (suppressions file, halt_on_error=1,
#                     exitcode=66, history_size=7, second_deadlock_stack=1).
#   BENCH_GATE_RUNS=K Runs per benchmark for gate 3 (default: bench-gate's 9).
#
# Exit codes: 0 all pass (and API-I1..I24 coverage grep is complete),
#             1 failures or missing invariant coverage, 2 usage/env error.
#             In --gates mode: 0 all executed gates pass, 1 a gate failed,
#             2 usage/env error.

set -u -o pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
JT="$ROOT/JSTests/threads"
AMPLIFY_SH="$ROOT/Tools/threads/amplify.sh"

FILTER=""
AMPLIFY=0
LIST=0
GATES=0
CVE=0

die() { echo "run-tests: error: $*" >&2; exit 2; }

print_help() {
    # Print the header comment block (everything between the shebang and the
    # first non-comment line), stripped of the leading "# ".
    awk 'NR == 1 { next } /^#/ { sub(/^# ?/, ""); print; next } { exit }' \
        "${BASH_SOURCE[0]}"
}

for arg in "$@"; do
    case "$arg" in
        --filter=*) FILTER="${arg#--filter=}" ;;
        --amplify)  AMPLIFY=1 ;;
        --list)     LIST=1 ;;
        --gates)    GATES=1 ;;
        --cve)      CVE=1 ;;
        -h|--help)  print_help; exit 0 ;;
        *)          die "unknown argument: $arg" ;;
    esac
done

if [[ "$GATES" -eq 1 && ( -n "$FILTER" || "$AMPLIFY" -eq 1 || "$LIST" -eq 1 || "$CVE" -eq 1 ) ]]; then
    die "--gates must be the sole option"
fi

# ---- ambient JSC_* option env: detect and WARN, never scrub ----
# jsc applies JSC_<option> environment variables in Options::initialize(),
# BEFORE the //@ header argv options this runner forwards, so ambient
# exports are a second option channel that can invert an option-off /
# option-default test's premise (e.g. JSC_useSharedGCHeap=true vs
# heap-option-off.js). Some rungs deliver their entire configuration this
# way on purpose (e.g. the GIL-off flag set over the whole corpus), so the
# runner MUST NOT unset these: silently scrubbing would convert an
# env-configured rung into a default-config run whose green is vacuous,
# with no trace in the log. Instead: name every JSC_* variable loudly so
# each log records the ambient channel, and let premise-checked tests
# report THREADS-PREMISE-SKIP (counted as SKIP below, not PASS or FAIL),
# so a premise/env contradiction surfaces as an actionable skip at the
# rung definition rather than a fake pass or fake fail here. The plain
# JSC=/path/to/jsc selector does not match the JSC_ prefix.
AMBIENT_JSC_ENV="$(env | LC_ALL=C grep -o '^JSC_[A-Za-z0-9_]*' | LC_ALL=C sort | tr '\n' ' ')"
AMBIENT_JSC_ENV="${AMBIENT_JSC_ENV% }"
if [[ -n "$AMBIENT_JSC_ENV" ]]; then
    echo "run-tests: WARNING: ambient JSC_* option env present and HONORED (not scrubbed): $AMBIENT_JSC_ENV" >&2
    echo "run-tests: WARNING: JSC_* env applies before //@ header options and can invert option-default test premises; premise-self-checking tests will report SKIP (THREADS-PREMISE-SKIP)" >&2
fi

# ---- resolve jsc ----
JSC="${JSC:-}"
if [[ -z "$JSC" ]]; then
    for candidate in "$ROOT/WebKitBuild/Debug/bin/jsc" "$ROOT/WebKitBuild/Release/bin/jsc"; do
        [[ -x "$candidate" ]] && JSC="$candidate" && break
    done
fi
if [[ "$LIST" -eq 0 ]]; then
    [[ -n "$JSC" && -x "$JSC" ]] || die "no jsc binary (set JSC=/path/to/jsc)"
fi

# ============================ --gates mode ============================
# SPEC-api §10 task 13: degrade gracefully (G15). Each gate that can run is
# a hard gate; substrate that is absent (TSAN target) is SKIPped, never
# failed. Semantic questions are settled by SPEC-api §4/§6, not here.
if [[ "$GATES" -eq 1 ]]; then
    SELF="${BASH_SOURCE[0]}"
    GATE_FAILURES=()
    GATE_SKIPS=()

    gate_banner() {
        echo
        echo "==== gate: $* ===="
    }

    # ---- gate 1: race corpus via this runner, amplified (annex §T:
    # "races/ (GI; amplifier+TSAN when present, G15)"). --amplify itself
    # degrades to plain runs with one warning when amplify.sh is absent
    # (frozen §8 fallback), so this gate always executes. ----
    gate_banner "races (run-tests.sh --amplify; jsc = $JSC)"
    if JSC="$JSC" bash "$SELF" --filter=/races/ --amplify; then
        echo "gate[races]: PASS"
    else
        echo "gate[races]: FAIL"
        GATE_FAILURES+=("races")
    fi

    # ---- gate 2: race corpus under the TSAN no-JIT target, only if one
    # exists in this tree (docs/threads/TSAN.md; built by `bash tsan.sh`
    # into WebKitBuild/TSan/bin/jsc). Run plain, not amplified: TSAN's
    # 5-15x slowdown makes amplified campaigns impractical, and TSAN itself
    # is the race oracle here. exitcode=66 in a FAIL line distinguishes a
    # TSAN report from an ordinary test failure. ----
    TSAN_JSC="${TSAN_JSC:-$ROOT/WebKitBuild/TSan/bin/jsc}"
    if [[ -x "$TSAN_JSC" ]]; then
        TSAN_SUPP="$ROOT/Tools/tsan/suppressions.txt"
        TSAN_DEFAULT_OPTS="suppressions=$TSAN_SUPP history_size=7 second_deadlock_stack=1 halt_on_error=1 exitcode=66"
        gate_banner "tsan no-JIT races (jsc = $TSAN_JSC)"
        if TSAN_OPTIONS="${TSAN_OPTIONS:-$TSAN_DEFAULT_OPTS}" JSC="$TSAN_JSC" \
            bash "$SELF" --filter=/races/; then
            echo "gate[tsan]: PASS"
        else
            echo "gate[tsan]: FAIL (exit 66 above = TSAN race report;" \
                 "see docs/threads/TSAN.md, including the CLoop" \
                 "shared-stack known limitation for intermittent" \
                 "CLoop::execute SEGVs under the phase-1 GIL stub)"
            GATE_FAILURES+=("tsan")
        fi
    else
        echo
        echo "gate[tsan]: SKIP — no TSAN no-JIT target at $TSAN_JSC" \
             "(build with: bash tsan.sh; docs/threads/TSAN.md)"
        GATE_SKIPS+=("tsan")
    fi

    # ---- gate 3: bench self-gate (API-I19, implement half). The
    # authoritative I19 comparison uses the integrator-recorded
    # pre-workstream baseline at INT (self-recorded = vacuous, SPEC-api §6
    # I19); the implement-phase bar is mechanical: --record then gate on
    # the SAME build must exit 0. A throwaway baseline file keeps the
    # integrator's Tools/threads/baseline.json untouched. ----
    BENCH_GATE="$ROOT/Tools/threads/bench-gate.sh"
    if [[ -x "$BENCH_GATE" && -d "$ROOT/JSTests/threads/bench" ]]; then
        gate_banner "bench self-gate (bench-gate.sh --record + gate, same build)"
        TMP_BASELINE="$(mktemp "${TMPDIR:-/tmp}/threads-bench-baseline.XXXXXX")" \
            || die "mktemp failed"
        BENCH_RUNS="${BENCH_GATE_RUNS:-9}"
        if bash "$BENCH_GATE" --record --baseline "$TMP_BASELINE" \
                --runs "$BENCH_RUNS" "$JSC" \
           && bash "$BENCH_GATE" --baseline "$TMP_BASELINE" \
                --runs "$BENCH_RUNS" "$JSC"; then
            echo "gate[bench]: PASS"
        else
            echo "gate[bench]: FAIL (record+gate on the same build must" \
                 "exit 0; I19)"
            GATE_FAILURES+=("bench")
        fi
        rm -f "$TMP_BASELINE"
    else
        echo
        echo "gate[bench]: SKIP — bench substrate missing" \
             "($BENCH_GATE / JSTests/threads/bench)"
        GATE_SKIPS+=("bench")
    fi

    # ---- summary ----
    echo
    if [[ ${#GATE_SKIPS[@]} -gt 0 ]]; then
        echo "run-tests: gates skipped (substrate absent, G15): ${GATE_SKIPS[*]}"
    fi
    if [[ ${#GATE_FAILURES[@]} -gt 0 ]]; then
        echo "run-tests: GATES FAIL: ${GATE_FAILURES[*]}"
        exit 1
    fi
    echo "run-tests: GATES PASS (all executed gates green)"
    exit 0
fi
# ========================== end --gates mode ==========================

# ---- collect the corpus (SPEC-api §8 globs; foreign dirs only when present) ----
# --cve swaps in the CVE susceptibility suite (thread-cve-audit/-close): it
# is a separate lane, not part of the default corpus, because its
# [EXPECTED-FAIL] rows (threadsExpectFail) pin OPEN audit rulings and fail
# GIL-off by design until the ruled fix lands.
FILES=()
if [[ "$CVE" -eq 1 ]]; then
    for f in "$JT"/cve/*.js; do
        [[ -e "$f" ]] && FILES+=("$f")
    done
else
    for f in "$JT"/api/*.js "$JT"/atomics/*.js "$JT"/races/*.js \
             "$JT"/heap-*.js "$JT"/objectmodel/*.js "$JT"/vmstate/*.js; do
        [[ -e "$f" ]] && FILES+=("$f")
    done
    if [[ -d "$JT/jit" ]]; then
        while IFS= read -r f; do
            FILES+=("$f")
        done < <(find "$JT/jit" -name '*.js' -type f 2>/dev/null | sort)
    fi
fi
[[ ${#FILES[@]} -gt 0 ]] || die "no tests found under $JT"

# ---- header parsing ----
# Echoes one line per run: the jsc args for that run, space-separated
# (threads options never contain spaces). Echoes nothing for a plain run
# marker "<plain>", and "<skip>" for skipped files.
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
                : # other directives (e.g. comments) ignored
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

WARNED_NO_AMPLIFY=0
PASS=0
FAIL=0
XFAIL=0
SKIPPED=0
PLANNED_RUNS=0
PROBE_SKIPS=0
FAILED_TESTS=()
TMP_OUT="$(mktemp "${TMPDIR:-/tmp}/threads-run-tests.XXXXXX")" || die "mktemp failed"
trap 'rm -f "$TMP_OUT"' EXIT

# Option-availability probe: a requireOptions header naming an option this
# build does not have would turn every run of that file into a FAIL that is
# really a missing cross-workstream OptionsList.h hunk (e.g. threads/vmstate
# files needing --useVMLite / --useStructureAllocationLock before vmstate
# M_opts lands). Probe the run's option set against an empty program first.
# Contract: SKIP ONLY on jsc's "ERROR: invalid option" stderr diagnostic;
# any other nonzero probe exit (notably signal deaths like 134/SIGABRT
# under ambient JSC_* GIL-off env, or timeout kills 124/137) is a probe
# CRASH and FAILs every run that needs that option set. Exit-code class is
# NOT a discriminator: jsc CRASH()es on unknown options too when
# validateOptions is on. >50% probe-skips fails the whole run (vacuous
# green guard at the bottom). Cached per option set (plain indexed arrays:
# macOS bash 3.2 has no associative arrays).
PROBED_OPTSETS=()
PROBED_RESULTS=()   # "yes" | "no" | "crash:<status>"
PROBE_VERDICT=""    # out-param of probe_options
probe_options() { # $1 = space-joined args ("" => trivially supported)
    local key="$1"
    PROBE_VERDICT=yes
    [[ -z "$key" ]] && return 0
    local i
    for i in "${!PROBED_OPTSETS[@]}"; do
        if [[ "${PROBED_OPTSETS[$i]}" == "$key" ]]; then
            PROBE_VERDICT="${PROBED_RESULTS[$i]}"
            return 0
        fi
    done
    local probe_args=()
    read -r -a probe_args <<<"$key"
    local probe_err
    probe_err="$(mktemp "${TMPDIR:-/tmp}/threads-probe-err.XXXXXX")" || die "mktemp failed"
    local status=0
    # Wrapped in the per-test timeout: an ambient-env config that deadlocks
    # (rather than aborts) during empty-program bring-up must not hang the
    # whole harness at the first probe; 124/137 classify as crash below.
    ${TIMEOUT_WRAP[@]+"${TIMEOUT_WRAP[@]}"} "$JSC" "${probe_args[@]}" -e '' >/dev/null 2>"$probe_err" || status=$?
    if [[ "$status" -eq 0 ]]; then
        PROBE_VERDICT=yes
    elif grep -qs 'ERROR: invalid option' "$probe_err"; then
        # jsc's unknown-option diagnostic (jsc.cpp CommandLine::parseArguments,
        # Options::setOption failure path). ONLY this means "option not in
        # this build" => SKIP is legitimate. NOTE: exit-code class alone is
        # NOT a discriminator — jsc CRASH()es (signal death) on unknown
        # options too when validateOptions is on, so we key on the
        # diagnostic, not on status<128.
        # A mixed set (unknown option AND engine crash) classifies as SKIP
        # because the diagnostic wins — acceptable since plain-option runs
        # bypass the probe and surface the crash as ordinary FAILs.
        # Caveat: if WTF_DATA_LOG_FILENAME is exported, dataLog leaves
        # stderr and a genuine unknown option would misclassify as a
        # probe-crash FAIL (fail-loud, not green-hiding).
        PROBE_VERDICT=no
    else
        # Nonzero exit WITHOUT the unknown-option diagnostic: the build
        # accepted the option set and then died running an empty program
        # (e.g. SIGABRT=exit 134 under ambient JSC_* GIL-off env), or hung
        # until the timeout wrap killed it (124/137). That is a real crash
        # of the engine under this configuration — it must surface as FAIL,
        # never as "option not supported" SKIP.
        PROBE_VERDICT="crash:$status"
        echo "run-tests: PROBE CRASH (exit $status) for option set [$key]; ambient JSC_* env: ${AMBIENT_JSC_ENV:-none detected}" >&2
        sed 's/^/     | /' "$probe_err" >&2
    fi
    rm -f "$probe_err"
    PROBED_OPTSETS+=("$key")
    PROBED_RESULTS+=("$PROBE_VERDICT")
    return 0
}

# ---- threadsExpectFail("gilOff") support (cve suite) ----
# Header scan: 0 (true) iff the file's leading //@ block carries the
# directive. Same block-termination rule as plan_runs.
file_expectfail_giloff() { # $1 = file
    local line
    while IFS= read -r line; do
        [[ "$line" == "//@"* ]] || return 1
        [[ "$line" == '//@ threadsExpectFail("gilOff")'* ]] && return 0
    done < "$1"
    return 1
}

# GIL-mode probe: decides whether a run's EFFECTIVE configuration is
# GIL-off by asking the build itself — $vm.useThreadGIL() returns the
# post-U0-validation option value under the run's exact argv + ambient
# JSC_* env — never by re-implementing the U0 activation checklist from
# flag spellings here (env JSC_useThreadGIL=false without the four enable
# flags is forced back ON by U0, and a spelling-based guess would invert
# the verdict of a GIL-ON run). Cached per option set. Result "on", "off",
# or "unknown"; unknown (probe died, $vm unavailable, stray output) falls
# back to running the test NORMALLY — fail-loud: a pinned expected-fail
# then surfaces as an ordinary FAIL, never as a silent inversion.
GIL_PROBED_OPTSETS=()
GIL_PROBED_RESULTS=()
GIL_MODE=""
probe_gil_mode() { # $1 = space-joined args
    local key="$1"
    local i
    for i in ${GIL_PROBED_OPTSETS[@]+"${!GIL_PROBED_OPTSETS[@]}"}; do
        if [[ "${GIL_PROBED_OPTSETS[$i]}" == "$key" ]]; then
            GIL_MODE="${GIL_PROBED_RESULTS[$i]}"
            return 0
        fi
    done
    local probe_args=()
    [[ -n "$key" ]] && read -r -a probe_args <<<"$key"
    local out
    out="$(${TIMEOUT_WRAP[@]+"${TIMEOUT_WRAP[@]}"} "$JSC" ${probe_args[@]+"${probe_args[@]}"} --useDollarVM=1 \
        -e 'print("THREADS-GIL-MODE:" + ($vm.useThreadGIL() ? "on" : "off"))' 2>/dev/null \
        | grep '^THREADS-GIL-MODE:' || true)"
    case "$out" in
        "THREADS-GIL-MODE:on")  GIL_MODE=on ;;
        "THREADS-GIL-MODE:off") GIL_MODE=off ;;
        *)                      GIL_MODE=unknown ;;
    esac
    GIL_PROBED_OPTSETS+=("$key")
    GIL_PROBED_RESULTS+=("$GIL_MODE")
    return 0
}

# Per-test timeout: threaded tests hang by failure mode (lost wakeup, GIL
# hand-off livelock, deadlocked safepoint), so an unbounded run blocks the
# whole harness. timeout(1) kills the process group; a timeout is a FAIL.
# Override with THREADS_TEST_TIMEOUT_SECS (0 disables).
TEST_TIMEOUT_SECS="${THREADS_TEST_TIMEOUT_SECS:-120}"
TIMEOUT_WRAP=()
if [[ "$TEST_TIMEOUT_SECS" != "0" ]] && command -v timeout >/dev/null 2>&1; then
    TIMEOUT_WRAP=(timeout -k 10 "$TEST_TIMEOUT_SECS")
fi

run_one() { # file, args...
    local file="$1"; shift
    if [[ "$AMPLIFY" -eq 1 && -x "$AMPLIFY_SH" ]]; then
        local aargs=()
        [[ -n "${AMPLIFY_RUNS:-}" ]] && aargs+=(--runs "$AMPLIFY_RUNS")
        local opt
        for opt in "$@"; do
            aargs+=(--jsc-arg "$opt")
        done
        "$AMPLIFY_SH" ${aargs[@]+"${aargs[@]}"} "$JSC" "$file" >"$TMP_OUT" 2>&1
        return $?
    fi
    if [[ "$AMPLIFY" -eq 1 && "$WARNED_NO_AMPLIFY" -eq 0 ]]; then
        echo "run-tests: warning: $AMPLIFY_SH not found/executable; running plain" >&2
        WARNED_NO_AMPLIFY=1
    fi
    ${TIMEOUT_WRAP[@]+"${TIMEOUT_WRAP[@]}"} "$JSC" "$@" "$file" >"$TMP_OUT" 2>&1
    local status=$?
    if [[ ${#TIMEOUT_WRAP[@]} -gt 0 && ( $status -eq 124 || $status -eq 137 ) ]]; then
        echo "run-tests: TIMEOUT after ${TEST_TIMEOUT_SECS}s (hang): $file" >>"$TMP_OUT"
    fi
    return $status
}

for file in "${FILES[@]}"; do
    rel="${file#$ROOT/}"
    if [[ -n "$FILTER" && "$rel" != *"$FILTER"* ]]; then
        continue
    fi
    if [[ "$LIST" -eq 1 ]]; then
        echo "$rel"
        continue
    fi

    FILE_XFAIL_GILOFF=0
    if file_expectfail_giloff "$file"; then
        FILE_XFAIL_GILOFF=1
    fi

    runindex=0
    while IFS= read -r runspec; do
        if [[ "$runspec" == "<skip>" ]]; then
            SKIPPED=$((SKIPPED + 1))
            echo "SKIP $rel"
            continue
        fi
        args=()
        if [[ "$runspec" != "<plain>" ]]; then
            read -r -a args <<<"$runspec"
        fi
        # annex T2 / I18: the runner appends --can-block-is-false for the
        # blocking-gate test (G34).
        if [[ "$(basename "$file")" == "blocking-gate.js" ]]; then
            args+=("--can-block-is-false")
        fi
        runindex=$((runindex + 1))
        label="$rel"
        [[ "$runindex" -gt 1 || "$runspec" != "<plain>" ]] && label="$rel [${args[*]:-default}]"
        PLANNED_RUNS=$((PLANNED_RUNS + 1))
        probe_options "${args[*]:-}"
        case "$PROBE_VERDICT" in
        yes) ;;
        no)
            SKIPPED=$((SKIPPED + 1))
            PROBE_SKIPS=$((PROBE_SKIPS + 1))
            echo "SKIP $label (option(s) not supported by this jsc build — likely a not-yet-landed OptionsList.h hunk from another workstream, not an api failure)"
            continue
            ;;
        crash:*)
            FAIL=$((FAIL + 1))
            FAILED_TESTS+=("$label [probe-crash exit ${PROBE_VERDICT#crash:}]")
            echo "FAIL $label (PROBE CRASH: \"$JSC\" ${args[*]:-} -e '' exited ${PROBE_VERDICT#crash:} with no unknown-option diagnostic — engine crash under this option set/ambient env, NOT a missing OptionsList.h hunk)"
            continue
            ;;
        esac
        # threadsExpectFail("gilOff"): invert the verdict ONLY when the
        # run's EFFECTIVE mode is GIL-off ($vm.useThreadGIL() probe — mode
        # "unknown" runs normally, fail-loud).
        RUN_XFAIL=0
        if [[ "$FILE_XFAIL_GILOFF" -eq 1 ]]; then
            probe_gil_mode "${args[*]:-}"
            [[ "$GIL_MODE" == "off" ]] && RUN_XFAIL=1
        fi
        if run_one "$file" ${args[@]+"${args[@]}"}; then
            # A test whose option-default premise is inverted by ambient
            # configuration (JSC_* env, warned about above) self-reports
            # with a THREADS-PREMISE-SKIP marker instead of asserting under
            # the wrong premise. Count it as SKIP — never PASS — so an
            # env-configured rung cannot quietly absorb a vacuous green.
            if grep -qs '^THREADS-PREMISE-SKIP:' "$TMP_OUT"; then
                SKIPPED=$((SKIPPED + 1))
                echo "SKIP $label (premise inverted by ambient configuration; ambient JSC_* env: ${AMBIENT_JSC_ENV:-none detected})"
                sed 's/^/     | /' "$TMP_OUT"
            elif [[ "$RUN_XFAIL" -eq 1 ]]; then
                FAIL=$((FAIL + 1))
                FAILED_TESTS+=("$label [XPASS — expected-fail test passed GIL-off]")
                echo "FAIL $label (XPASS: threadsExpectFail(\"gilOff\") test PASSED under an effective GIL-off configuration — either the ruled fix landed (flip the annotation and the audit-status row) or the pinned failure went latent and must be investigated, not celebrated)"
            else
                PASS=$((PASS + 1))
                echo "PASS $label"
            fi
        else
            status=$?
            if [[ "$RUN_XFAIL" -eq 1 ]]; then
                XFAIL=$((XFAIL + 1))
                echo "XFAIL $label (exit $status — expected GIL-off failure pinning an OPEN audit ruling; this row flips to PASS, and the annotation must be removed, when the ruled fix lands)"
            else
                FAIL=$((FAIL + 1))
                FAILED_TESTS+=("$label")
                echo "FAIL $label (exit $status)"
                sed 's/^/     | /' "$TMP_OUT"
            fi
        fi
    done < <(plan_runs "$file")
done

if [[ "$LIST" -eq 1 ]]; then
    exit 0
fi

# ---- invariant coverage grep (SPEC-api §6: every API-I1..I24 cited by >=1
# §8 test; CI greps this) ----
# A //@ skip'ped file never executes, so its citations are NOT coverage:
# count only non-skipped files. API-I14 is special-cased — its sole citation
# (api/thread-restrict.js) is SPEC-mandatedly skipped until the integrator
# applies the 9.2-6 hooks ("INT gate via 9.2-6; //@ skipped until then"), so
# the deferral is reported visibly on every run instead of counting as
# silently-green coverage.
# The grep is an API-corpus (§8) contract; the --cve lane runs a different
# suite entirely, so the grep is skipped there (it would either pass
# vacuously off files this lane never ran, or fail spuriously in a tree
# without the api corpus).
COVERAGE_FILES=()
SKIPPED_COVERAGE_FILES=()
if [[ "$CVE" -eq 0 ]]; then
    for f in "$JT"/api/*.js "$JT"/atomics/*.js "$JT"/races/*.js; do
        [[ -e "$f" ]] || continue
        if grep -qs '^//@ skip' "$f"; then
            SKIPPED_COVERAGE_FILES+=("$f")
        else
            COVERAGE_FILES+=("$f")
        fi
    done
fi
MISSING=()
if [[ "$CVE" -eq 0 ]]; then
    for i in $(seq 1 24); do
        if grep -qsE "API-I$i([^0-9]|\$)" ${COVERAGE_FILES[@]+"${COVERAGE_FILES[@]}"} /dev/null; then
            continue
        fi
        if [[ ${#SKIPPED_COVERAGE_FILES[@]} -gt 0 ]] \
            && grep -qsE "API-I$i([^0-9]|\$)" "${SKIPPED_COVERAGE_FILES[@]}" /dev/null; then
            if [[ "$i" -eq 14 ]]; then
                echo "run-tests: note: API-I14 coverage deferred to INT 9.2-6 (sole citation, api/thread-restrict.js, is //@ skip'ped per SPEC-api I14)"
                continue
            fi
            MISSING+=("API-I$i(only-in-skipped-files)")
        else
            MISSING+=("API-I$i")
        fi
    done
fi

echo
if [[ "$XFAIL" -gt 0 ]]; then
    echo "run-tests: $PASS passed, $FAIL failed, $XFAIL expected-fail (XFAIL — open audit rulings pinned), $SKIPPED skipped"
else
    echo "run-tests: $PASS passed, $FAIL failed, $SKIPPED skipped"
fi
if [[ ${#MISSING[@]} -gt 0 ]]; then
    echo "run-tests: COVERAGE FAIL — uncited invariants: ${MISSING[*]}"
fi
if [[ "$FAIL" -gt 0 ]]; then
    echo "run-tests: failed tests:"
    printf '  %s\n' "${FAILED_TESTS[@]}"
fi

PROBE_SKIP_FAIL=0
if [[ "$PLANNED_RUNS" -gt 0 ]]; then
    if [[ $((PROBE_SKIPS * 2)) -gt "$PLANNED_RUNS" ]]; then
        echo "run-tests: PROBE-SKIP FAIL — $PROBE_SKIPS of $PLANNED_RUNS planned runs were skipped by the option probe (>50%); result is vacuous, refusing to exit 0"
        PROBE_SKIP_FAIL=1
    elif [[ "$PASS" -eq 0 && "$FAIL" -eq 0 && "$PROBE_SKIPS" -gt 0 ]]; then
        echo "run-tests: PROBE-SKIP FAIL — nothing executed ($PROBE_SKIPS probe-skips, 0 pass, 0 fail); result is vacuous, refusing to exit 0"
        PROBE_SKIP_FAIL=1
    fi
fi
[[ "$FAIL" -eq 0 && ${#MISSING[@]} -eq 0 && "$PROBE_SKIP_FAIL" -eq 0 ]] || exit 1
exit 0
