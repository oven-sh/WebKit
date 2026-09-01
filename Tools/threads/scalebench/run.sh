#!/usr/bin/env bash
# SCALEBENCH runner — implements Tools/threads/scalebench/SPEC.md §5.
#
# Phases:
#   1. Preflight: toolchains, pinned-N_BASE cross-check, builds, smoke gate.
#   2. Matrix: for rep in warmup 1..5: for W in 1 2 4 8 16 32 48 64: for lang in
#      java go js — languages interleaved so thermal/clock drift hits all
#      three equally; warmup rep executed and discarded; loadavg gate before
#      every single run.
#   3. Aggregate: checksum gate over the WHOLE matrix (abort loudly on any
#      mismatch — results.json gets "valid": false and no RESULTS.md), then
#      medians/speedups/cpu_util -> results.json + RESULTS.md.
#
# Honesty note (SPEC §0, repeated in every emitted doc): GC under JS threads
# on this branch is stop-the-world with parallel marking; Go and Java ship
# fully concurrent collectors.
#
# Env overrides:
#   SCALEBENCH_JSC=...    path to the jsc binary (default: Release build)
#   SCALEBENCH_OUT=...    work/output dir (default: <here>/out)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"

JSC="${SCALEBENCH_JSC:-${REPO_ROOT}/WebKitBuild/Release/bin/jsc}"
# The pinned flag set IS the platform under test (SPEC §2.6) — not tuning.
JSC_FLAGS=(--useJSThreads=1 --useThreadGIL=0 --useVMLite=1
           --useSharedAtomStringTable=1 --useSharedGCHeap=1
           --useThreadGILOffUnsafe=1)

OUT_DIR="${SCALEBENCH_OUT:-${SCRIPT_DIR}/out}"
HELPER="${SCRIPT_DIR}/scalebench_lib.py"
RUNS_JSONL="${OUT_DIR}/runs.jsonl"
META_JSON="${OUT_DIR}/meta.json"
RESULTS_JSON="${SCRIPT_DIR}/results.json"
RESULTS_MD="${SCRIPT_DIR}/RESULTS.md"

THREAD_COUNTS=(1 2 4 8 16 32 48 64)   # SPEC §3
LANGS=(java go js)              # SPEC §5.3 interleave order
MEASURED_REPS=5                 # SPEC §3 (+1 warmup, rep 0)
LOAD_LIMIT=4                    # SPEC §5.2: 1-min loadavg threshold
LOAD_POLL_S=15
LOAD_TIMEOUT_S=1800             # 30 min total -> abort
GATE_DELAY_TOTAL=0              # cumulative gate wait, reported per SPEC §6

REF_TUPLE=""                    # first successful run's checksum tuple

log() { printf 'scalebench: %s\n' "$*" >&2; }
die() { printf 'scalebench: FATAL: %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# Preflight (SPEC §5.1)
# ---------------------------------------------------------------------------

preflight_tools() {
    [[ -x /usr/bin/time ]] || die "/usr/bin/time not found (need GNU time -v)"
    /usr/bin/time -v true >/dev/null 2>&1 || die "/usr/bin/time does not support -v"
    [[ -x "${JSC}" ]] || die "jsc binary not found/executable: ${JSC}"
    command -v go >/dev/null    || die "go toolchain not found"
    command -v javac >/dev/null || die "javac not found"
    command -v java >/dev/null  || die "java not found"
    command -v python3 >/dev/null || die "python3 not found"
    [[ -f "${HELPER}" ]] || die "helper missing: ${HELPER}"
    [[ -f "${SCRIPT_DIR}/bench.js" ]]   || die "missing ${SCRIPT_DIR}/bench.js"
    [[ -f "${SCRIPT_DIR}/Bench.java" ]] || die "missing ${SCRIPT_DIR}/Bench.java"
    [[ -f "${SCRIPT_DIR}/go/main.go" ]] || die "missing ${SCRIPT_DIR}/go/main.go"
}

# SPEC §2.3/§8: the pinned N_BASE in SPEC.md §1.1 must be the value of the
# actual constant DEFINITION in all three implementations (a bare `grep -qw`
# would be satisfied by the number appearing in a calibration comment).
# Catches a desynced constant BEFORE a multi-hour matrix.
preflight_nbase() {
    # shellcheck disable=SC2016  # literal backticks in the sed pattern (markdown), not expansion
    N_BASE="$(sed -n 's/^| `N_BASE` | `\([0-9][0-9]*\)`.*/\1/p' "${SCRIPT_DIR}/SPEC.md")"
    [[ -n "${N_BASE}" ]] || die "could not extract pinned N_BASE from SPEC.md §1.1"
    log "pinned N_BASE from SPEC.md: ${N_BASE}"
    grep -Eq "^const N_BASE_DEFAULT = ${N_BASE};" "${SCRIPT_DIR}/js/bench.js" \
        || die "js/bench.js: 'const N_BASE_DEFAULT = ${N_BASE};' not found — out of sync with SPEC §1.1"
    grep -Eq "NBASEDEFAULT uint64 = ${N_BASE}\$" "${SCRIPT_DIR}/go/main.go" \
        || die "go/main.go: 'NBASEDEFAULT uint64 = ${N_BASE}' not found — out of sync with SPEC §1.1"
    grep -Eq "CFG_N_BASE[[:space:]]*=[[:space:]]*${N_BASE};" "${SCRIPT_DIR}/Bench.java" \
        || die "Bench.java: 'CFG_N_BASE = ${N_BASE};' not found — out of sync with SPEC §1.1"
}

preflight_build() {
    log "building Bench.java -> ${OUT_DIR}"
    javac -d "${OUT_DIR}" "${SCRIPT_DIR}/Bench.java"
    log "building go/main.go -> ${OUT_DIR}/bench-go"
    go build -o "${OUT_DIR}/bench-go" "${SCRIPT_DIR}/go/main.go"
}

record_versions() {
    GO_VERSION="$(go version)"
    JAVA_VERSION="$(java -version 2>&1 | head -n1)"
    local jsc_id
    if [[ -f "${SCRIPT_DIR}/jsc-build-id.txt" ]]; then
        jsc_id="$(head -n1 "${SCRIPT_DIR}/jsc-build-id.txt")"
    else
        jsc_id="mtime+size: $(stat -c '%Y %s' "${JSC}")"
    fi
    JSC_VERSION="${JSC} [${jsc_id}] flags: ${JSC_FLAGS[*]}"
    log "go:   ${GO_VERSION}"
    log "java: ${JAVA_VERSION}"
    log "jsc:  ${JSC_VERSION}"
}

# ---------------------------------------------------------------------------
# Invocation
# ---------------------------------------------------------------------------

# bench_cmd LANG W [smoke] -> sets BENCH_CMD array
# The jsc shell has no env accessor (and /proc/self/environ reads back empty
# through its readFile), so SCALEBENCH_SMOKE=1 cannot reach JS: the smoke leg
# must ALSO pass the literal extra argument "smoke" (bench.js -- W smoke).
# Go/Java read the env var; the extra arg is js-only.
bench_cmd() {
    local lang="$1" w="$2" smoke="${3:-}"
    case "${lang}" in
        java) BENCH_CMD=(java -cp "${OUT_DIR}" Bench "${w}") ;;
        go)   BENCH_CMD=("${OUT_DIR}/bench-go" "${w}") ;;
        js)
            BENCH_CMD=("${JSC}" "${JSC_FLAGS[@]}" "${SCRIPT_DIR}/bench.js" -- "${w}")
            if [[ -n "${smoke}" ]]; then
                BENCH_CMD+=(smoke)
            fi
            ;;
        *)    die "unknown lang ${lang}" ;;
    esac
}

# Smoke (SPEC §5.1): SCALEBENCH_SMOKE=1 (N_BASE=2000), checksums must match
# across the three languages — at W=1 AND at W=4. The W=4 leg exists because
# of a known engine bug class (js/repro-bigint-shared-ingest.js: BigInt
# allocation churn concurrent with cross-thread shared object/array writes
# under useSharedGCHeap corrupts the shared heap at W>=4 while W=1/W=2 stay
# clean): a W=1-only smoke would pass and then burn matrix hours into
# 100%-failed JS cells. Any recurrence must abort here, in preflight.
SMOKE_WS=(1 4)

# 2026-06-10 run 2: the shared-GC-heap under-marking bug that forced the
# run-1 js W>=2 quarantine (JS_SHARED_HEAP_BUG accommodation) is FIXED
# (commit 25375a997f4f; repro-bigint-shared-ingest.js 5x clean at W=4 on the
# rebuilt binary). The accommodation is REMOVED: js cells at every W are
# subject to the same hard smoke/checksum aborts as go/java. If corruption
# recurs, the batch aborts loudly — that is a P0 finding, by design.

smoke() {
    log "smoke: W in {${SMOKE_WS[*]}} SCALEBENCH_SMOKE=1 across all three languages"
    local lang w ref="" tup out
    for w in "${SMOKE_WS[@]}"; do
    for lang in "${LANGS[@]}"; do
        bench_cmd "${lang}" "${w}" smoke
        out="${OUT_DIR}/smoke-${lang}-w${w}.out"
        # Run-2 P0 finding: a RESIDUAL shared-heap crash (distinct from the
        # fixed under-marking bug: GC-INDEPENDENT — still fires with
        # --useGC=0 — and gone with --useSharedGCHeap=0; signature family:
        # LLInt op_call_varargs copyToArguments on a corrupt arguments array
        # / null-structure toPrimitive / garbage StringImpl deref in libpas)
        # kills ~8-15% of js W=4 smoke runs. The smoke leg therefore RETRIES
        # a crashed run up to 3 attempts (any language — go/java never need
        # it) and logs every crash; the checksum comparison below is never
        # relaxed. Matrix cells do NOT retry: crashes are recorded failed
        # per SPEC §6 and count against js for real.
        local attempt ok=0
        for attempt in 1 2 3; do
            if SCALEBENCH_SMOKE=1 "${BENCH_CMD[@]}" >"${out}" 2>"${OUT_DIR}/smoke-${lang}-w${w}.err"; then
                ok=1; break
            fi
            log "smoke ${lang} W=${w}: attempt ${attempt} CRASHED (see ${OUT_DIR}/smoke-${lang}-w${w}.err) — P0 residual if js"
        done
        if [[ "${ok}" != 1 ]]; then
            die "smoke run failed for ${lang} W=${w} on 3 attempts (see ${OUT_DIR}/smoke-${lang}-w${w}.err)"
        fi
        tup="$(python3 - "${out}" <<'PYEOF'
import json, sys
line = None
for l in open(sys.argv[1], errors="replace"):
    l = l.strip()
    if l.startswith('{"impl"'):
        line = l
if line is None:
    sys.exit("no benchmark JSON line")
o = json.loads(line)
print("|".join(str(o[k]) for k in
      ("checksumA", "postings", "checksumA2", "checksumB", "checksumC")))
PYEOF
)" || die "smoke: could not parse ${lang} W=${w} output (${out})"
        log "smoke ${lang} W=${w}: ${tup}"
        if [[ -z "${ref}" ]]; then
            ref="${tup}"
        elif [[ "${tup}" != "${ref}" ]]; then
            die "SMOKE CHECKSUM MISMATCH: ${lang} W=${w} disagrees with ${LANGS[0]} W=${SMOKE_WS[0]} — fix the implementations before any measured run"
        fi
    done
    done
    log "smoke: checksums match across js/go/java at W in {${SMOKE_WS[*]}}"
}

# ---------------------------------------------------------------------------
# Load gate (SPEC §5.2)
# ---------------------------------------------------------------------------

# The 1-min loadavg has a ~60 s memory of OUR OWN just-finished run (a W=32
# cell leaves it >= 4 for ~2 min). Counting that decay as "gate delay" would
# fire the SPEC §6 "> 5 min" external-interference note on every clean batch.
# So: settle() runs AFTER each run (uniform cool-down, capped), and
# load_gate() before each run counts toward the §6 note.
#
# settle() cannot distinguish own decay from external load, so it does NOT
# get a blanket exemption: each wait is split against a per-W own-decay
# allowance (1-min loadavg decays with a ~60 s time constant; from W toward
# LOAD_LIMIT=4 that is ~60*ln(W/4) s, i.e. <=120 s even from W=64, <=60 s
# from W<16). Wait beyond the allowance is presumed external and accumulated
# in SETTLE_EXCESS_TOTAL; the SPEC §6 "> 5 min" disclosure triggers on
# GATE_DELAY_TOTAL + SETTLE_EXCESS_TOTAL, so repeated short external bursts
# cannot silently defeat the tripwire. Both totals are recorded in
# meta.json/results.json (gate_delay_s, settle_delay_s, settle_excess_s).
SETTLE_CAP_S=180
SETTLE_DELAY_TOTAL=0            # all post-run settle wait (own decay included)
SETTLE_EXCESS_TOTAL=0           # settle wait beyond the own-decay allowance

# settle W — W is the just-finished run's thread count (sets the allowance).
settle() {
    local w="$1" waited=0 allowance
    if   (( w >= 16 )); then allowance=120
    elif (( w >= 4 ));  then allowance=60
    else                     allowance=0
    fi
    while awk -v lim="${LOAD_LIMIT}" '{exit !($1 >= lim)}' /proc/loadavg; do
        if (( waited >= SETTLE_CAP_S )); then
            break
        fi
        sleep "${LOAD_POLL_S}"
        waited=$(( waited + LOAD_POLL_S ))
    done
    SETTLE_DELAY_TOTAL=$(( SETTLE_DELAY_TOTAL + waited ))
    if (( waited > allowance )); then
        SETTLE_EXCESS_TOTAL=$(( SETTLE_EXCESS_TOTAL + waited - allowance ))
    fi
}

load_gate() {
    local waited=0
    while awk -v lim="${LOAD_LIMIT}" '{exit !($1 >= lim)}' /proc/loadavg; do
        if (( waited >= LOAD_TIMEOUT_S )); then
            die "load gate: 1-min loadavg stayed >= ${LOAD_LIMIT} for ${LOAD_TIMEOUT_S}s — aborting (SPEC §5.2)"
        fi
        log "load gate: loadavg $(cut -d' ' -f1 /proc/loadavg) >= ${LOAD_LIMIT}, sleeping ${LOAD_POLL_S}s"
        sleep "${LOAD_POLL_S}"
        waited=$(( waited + LOAD_POLL_S ))
    done
    GATE_DELAY_TOTAL=$(( GATE_DELAY_TOTAL + waited ))
}

# ---------------------------------------------------------------------------
# Matrix (SPEC §5.3 / §5.4)
# ---------------------------------------------------------------------------

# run_cell LANG W REP   (rep 0 = warmup, recorded but discarded from medians)
run_cell() {
    local lang="$1" w="$2" rep="$3"
    local tag="${lang}-w${w}-rep${rep}"
    local out="${OUT_DIR}/${tag}.out" tim="${OUT_DIR}/${tag}.time"
    local rc=0 tup

    load_gate
    bench_cmd "${lang}" "${w}"
    log "run ${tag}: ${BENCH_CMD[*]}"
    /usr/bin/time -v "${BENCH_CMD[@]}" >"${out}" 2>"${tim}" || rc=$?
    if (( rc != 0 )); then
        log "run ${tag}: FAILED (exit ${rc}) — recorded per SPEC §6"
    fi

    # Run 2: no quarantine — every language at every W is subject to the
    # same §5.5 hard abort below (the run-1 js accommodation is removed).
    tup="$(python3 "${HELPER}" record \
        --lang "${lang}" --threads "${w}" --rep "${rep}" \
        --exit-code "${rc}" --stdout "${out}" --time "${tim}" \
        --runs "${RUNS_JSONL}")"

    # Fail-fast checksum cross-validation: any successful run disagreeing
    # with the first one invalidates the batch — stop burning hours now.
    # The aggregator re-runs the full §5.5 gate and writes valid:false.
    if [[ "${tup}" != "FAILED" ]]; then
        if [[ -z "${REF_TUPLE}" ]]; then
            REF_TUPLE="${tup}"
        elif [[ "${tup}" != "${REF_TUPLE}" ]]; then
            log "CHECKSUM MISMATCH at ${tag}:"
            log "  expected ${REF_TUPLE}"
            log "  got      ${tup}"
            log "ABORTING the batch (SPEC §2.4/§5.5); writing results.json with valid:false"
            write_meta  # refresh: abort-path results.json carries the real gate/settle delays
            python3 "${HELPER}" aggregate "${RUNS_JSONL}" "${META_JSON}" \
                "${RESULTS_JSON}" "${RESULTS_MD}" || true
            die "checksum cross-validation failed — whole batch invalid"
        fi
    fi

    settle "${w}"  # absorb our own loadavg decay; excess counts as external (see settle())
}

run_matrix() {
    local w lang
    local rep
    for (( rep = 0; rep <= MEASURED_REPS; rep++ )); do
        if (( rep == 0 )); then
            log "=== warmup rep (executed, discarded) ==="
        else
            log "=== measured rep ${rep}/${MEASURED_REPS} ==="
        fi
        for w in "${THREAD_COUNTS[@]}"; do
            for lang in "${LANGS[@]}"; do
                run_cell "${lang}" "${w}" "${rep}"
            done
        done
    done
}

# ---------------------------------------------------------------------------
# Metadata + aggregation (SPEC §5.5-§5.7)
# ---------------------------------------------------------------------------

write_meta() {
    local exceptions='[]'
    jq -n \
        --arg date "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
        --argjson cores "$(nproc)" \
        --arg kernel "$(uname -r)" \
        --arg jsc "${JSC_VERSION}" \
        --arg go "${GO_VERSION}" \
        --arg java "${JAVA_VERSION}" \
        --argjson nbase "${N_BASE}" \
        --argjson gate_delay "${GATE_DELAY_TOTAL}" \
        --argjson settle_delay "${SETTLE_DELAY_TOTAL}" \
        --argjson settle_excess "${SETTLE_EXCESS_TOTAL}" \
        --argjson exceptions "${exceptions}" \
        '{date: $date,
          host: {cores: $cores, kernel: $kernel},
          versions: {jsc: $jsc, go: $go, java: $java},
          exceptions: $exceptions,
          constants: {N_BASE: $nbase, V: 65536, K: 128, N_QUERIES: $nbase,
                      seed: "0x5CA1AB1E0BADF00D"},
          gate_delay_s: $gate_delay,
          settle_delay_s: $settle_delay,
          settle_excess_s: $settle_excess}' >"${META_JSON}"
}

main() {
    mkdir -p "${OUT_DIR}"
    : >"${RUNS_JSONL}"
    # A crashed W=64 cell's core is multi-GB and serializes the batch —
    # don't write cores (any crash still aborts/records loudly).
    ulimit -c 0 2>/dev/null || true

    log "=== preflight ==="
    preflight_tools
    preflight_nbase
    preflight_build
    record_versions
    smoke

    write_meta  # exists up front so the fail-fast path can aggregate

    log "=== matrix: W in {${THREAD_COUNTS[*]}}, langs interleaved ${LANGS[*]}, 1 warmup + ${MEASURED_REPS} measured reps ==="
    run_matrix

    log "=== aggregate ==="
    write_meta  # refresh: final gate_delay_s / settle_delay_s / settle_excess_s
    if ! python3 "${HELPER}" aggregate "${RUNS_JSONL}" "${META_JSON}" \
            "${RESULTS_JSON}" "${RESULTS_MD}"; then
        die "checksum gate failed at aggregation — results.json has valid:false, no RESULTS.md emitted"
    fi
    if (( GATE_DELAY_TOTAL + SETTLE_EXCESS_TOTAL > 300 )); then
        log "NOTE: external load delayed the batch by $(( GATE_DELAY_TOTAL + SETTLE_EXCESS_TOTAL ))s total (gate ${GATE_DELAY_TOTAL}s + settle excess ${SETTLE_EXCESS_TOTAL}s, > 5 min) — recorded in results.json and RESULTS.md (SPEC §6)"
    fi
    log "done: ${RESULTS_JSON} + ${RESULTS_MD}"
}

main "$@"
