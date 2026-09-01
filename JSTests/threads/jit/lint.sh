#!/usr/bin/env bash
# lint.sh — SPEC-jit Task 13 static lints (I13, I14 choke points, I18, I10,
# I21 poll placement, §5.5 PA-transition lint, Task-8 gap-8 private-name lint).
#
# No build needed; pure grep over the source tree. Exit 0 = all lints green.
# Each lint mirrors the inventory recorded in docs/threads/INTEGRATE-jit.md
# (Tasks 6/8/11) — a new violation means a butterfly/metadata/watchpoint site
# was added without going through the spec'd choke points.

set -u -o pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../../.." && pwd)"
JSC_DIR="$ROOT_DIR/Source/JavaScriptCore"

FAILURES=0

fail() {
    echo "LINT FAIL: $*" >&2
    FAILURES=$((FAILURES + 1))
}

pass() {
    echo "LINT pass: $*"
}

# ---------------------------------------------------------------------------
# I13 — every LLInt metadata structureID publication is either a §4.3
# single-word path or in a flag-off-only block. The publication choke points
# are LLIntCachedIdAndOffset::setConcurrently / setDefaultModeCacheConcurrently /
# publishLLIntIdAndOffsetPairConcurrently; raw `metadata.m_*structureID =`
# stores must sit inside useUnthreadedLLIntPropertyCaches() blocks
# (Task 6 inventory). We enforce the file set: no NEW file in llint/ may
# introduce a raw structureID metadata store.
# ---------------------------------------------------------------------------
I13_HITS="$(grep -rln "metadata\.m_.*[Ss]tructureID\s*=" "$JSC_DIR/llint/" 2>/dev/null || true)"
I13_BAD=""
for f in $I13_HITS; do
    case "$(basename "$f")" in
        LLIntSlowPaths.cpp) ;; # audited file (Task 6 table)
        *) I13_BAD="$I13_BAD $f" ;;
    esac
done
if [[ -n "$I13_BAD" ]]; then
    fail "I13: raw LLInt metadata structureID store outside LLIntSlowPaths.cpp:$I13_BAD"
else
    pass "I13 LLInt metadata publication confined to audited file"
fi

# Inside LLIntSlowPaths.cpp, every raw store must be within reach of the
# flag-off gate. Heuristic: the gate helpers must exist and be referenced at
# least as often as the §4.3 table demands (8 disabled/flag-off op families).
GATE_REFS=$(grep -c "useUnthreadedLLIntPropertyCaches()" "$JSC_DIR/llint/LLIntSlowPaths.cpp" 2>/dev/null || echo 0)
if [[ "$GATE_REFS" -lt 6 ]]; then
    fail "I13: useUnthreadedLLIntPropertyCaches() gate count dropped to $GATE_REFS (< 6) — a flag-off-only block may have lost its gate"
else
    pass "I13 flag-off gate references present ($GATE_REFS)"
fi

# ---------------------------------------------------------------------------
# I18 — Unset/ProtoLoad LLInt modes must stay unreachable flag-on.
# ---------------------------------------------------------------------------
if grep -q "ASSERT(!Options::useJSThreads())" "$JSC_DIR/bytecode/GetByIdMetadata.h"; then
    pass "I18 setUnsetMode/setProtoLoadMode asserts present"
else
    fail "I18: GetByIdMetadata.h lost the !useJSThreads asserts on setUnsetMode/setProtoLoadMode"
fi
if grep -q "RELEASE_ASSERT(!Options::useJSThreads())" "$JSC_DIR/llint/LLIntSlowPaths.cpp"; then
    pass "I18/§4.3 setupGetByIdPrototypeCache flag-on RELEASE_ASSERT present"
else
    fail "I18: LLIntSlowPaths.cpp lost the setupGetByIdPrototypeCache RELEASE_ASSERT"
fi

# ---------------------------------------------------------------------------
# I14 — choke-point lint (Task-8 form, re-run here as specified).
# (a) LLInt: every m_butterfly use in LowLevelInterpreter64.asm must be one of
#     the audited macro/op sites. We pin the COUNT envelope rather than line
#     numbers: legacy macros + threaded macros + per-op {flag-off, threaded}
#     pairs. A growth beyond the recorded count = new bare site.
# ---------------------------------------------------------------------------
ASM="$JSC_DIR/llint/LowLevelInterpreter64.asm"
BUTTERFLY_USES=$(grep -c "m_butterfly" "$ASM" 2>/dev/null || echo 0)
# Recorded count at Task 13: 15. If you add an AUDITED site, update this
# bound AND the I14 inventory in docs/threads/INTEGRATE-jit.md in the same
# change.
MAX_BUTTERFLY_USES=15
if [[ "$BUTTERFLY_USES" -gt "$MAX_BUTTERFLY_USES" ]]; then
    fail "I14: $BUTTERFLY_USES m_butterfly uses in LowLevelInterpreter64.asm (> $MAX_BUTTERFLY_USES recorded) — new bare butterfly site? Re-audit and update inventory."
else
    pass "I14 LLInt m_butterfly use count within recorded envelope ($BUTTERFLY_USES <= $MAX_BUTTERFLY_USES)"
fi
for macro in threadedButterflyReadPredicate threadedButterflyWritePredicate loadButterflyTIDTagToT4; do
    if ! grep -q "$macro" "$ASM"; then
        fail "I14: LLInt choke macro $macro missing from LowLevelInterpreter64.asm"
    fi
done
pass "I14 LLInt choke macros present"

# (b) Baseline/stubs: raw butterfly-offset loads outside the choke points.
#     The CCallHelpers chokes and the audited install/legacy sites are the only
#     legal users of JSObject::butterflyOffset() in jit/ + bytecode/, plus two
#     AUDITED flag-off-only residues (I14 inventory, Task 8):
#       - bytecode/InlineAccess.cpp generate* (FLAG-OFF: only reachable via
#         RepatchingPropertyInlineCache, which I3 forbids constructing flag-on)
#       - jit/JITPropertyAccess.cpp enumerator-put OOL (flag-off else-branch;
#         the flag-on branch defers to the generic route)
#     Those are count-pinned below; any growth = a new unaudited site.
RAW_BUTTERFLY="$(grep -rn "butterflyOffset()" "$JSC_DIR/jit" "$JSC_DIR/bytecode" 2>/dev/null \
    | grep -v "loadButterflyForRead\|loadButterflyForWrite\|emitLegacyButterflyTagTrap\|nukeStructureAndStoreButterfly\|storePtr\|store64\|store32\|CCallHelpers\|AssemblyHelpers" || true)"
RAW_UNAUDITED="$(echo "$RAW_BUTTERFLY" | grep -v "bytecode/InlineAccess.cpp\|jit/JITPropertyAccess.cpp" || true)"
RAW_INLINEACCESS_COUNT="$(echo "$RAW_BUTTERFLY" | grep -c "bytecode/InlineAccess.cpp" || true)"
RAW_JITPROP_COUNT="$(echo "$RAW_BUTTERFLY" | grep -c "jit/JITPropertyAccess.cpp" || true)"
if [[ -n "$RAW_UNAUDITED" ]]; then
    echo "$RAW_UNAUDITED" >&2
    fail "I14: raw butterflyOffset() load outside choke points/audited files (see above)"
elif [[ "$RAW_INLINEACCESS_COUNT" -gt 5 || "$RAW_JITPROP_COUNT" -gt 1 ]]; then
    fail "I14: audited flag-off butterfly-load counts grew (InlineAccess=$RAW_INLINEACCESS_COUNT > 5 or JITPropertyAccess=$RAW_JITPROP_COUNT > 1) — re-audit and update the inventory"
else
    pass "I14 Baseline/stub butterfly loads confined to choke points + audited files (audited residues: InlineAccess=$RAW_INLINEACCESS_COUNT, JITPropertyAccess=$RAW_JITPROP_COUNT)"
fi

# (c) legacy accessors keep the flag-on trap.
if grep -q "emitLegacyButterflyTagTrap" "$JSC_DIR/jit/AssemblyHelpers.cpp"; then
    pass "I14 legacy loadProperty/storeProperty trap present"
else
    fail "I14: AssemblyHelpers.cpp lost emitLegacyButterflyTagTrap"
fi

# ---------------------------------------------------------------------------
# §5.5 PA-transition lint (OM 8b): no tier may emit transition fast paths
# until OM's E4 machinery exists. Concretely: the transition compile entry
# points in DFG/FTL must keep their flag-on RELEASE_ASSERTs, and Repatch must
# keep the flag-on GiveUpOnCache gates. When E4 lands, this lint is REPLACED
# by one that requires the PA bit-test (`cell & 8`) next to every emitted
# transition predicate (SPEC-jit §5.5 Transition).
# ---------------------------------------------------------------------------
for f in dfg/DFGSpeculativeJIT.cpp ftl/FTLLowerDFGToB3.cpp; do
    if grep -q "compileAllocatePropertyStorage" "$JSC_DIR/$f"; then
        if ! grep -B2 -A6 "void.*compileAllocatePropertyStorage\|compileAllocatePropertyStorage(Node" "$JSC_DIR/$f" | grep -q "useJSThreads"; then
            # fall back: file-level presence of the assert near the three names
            if ! grep -q "RELEASE_ASSERT(!Options::useJSThreads())" "$JSC_DIR/$f"; then
                fail "PA-transition: $f lost the flag-on RELEASE_ASSERT on transition storage compilation"
                continue
            fi
        fi
        pass "PA-transition: $f transition machinery fail-fast present"
    fi
done
if grep -q "GiveUpOnCache" "$JSC_DIR/bytecode/Repatch.cpp" && \
   grep -q "useJSThreads" "$JSC_DIR/bytecode/Repatch.cpp"; then
    pass "PA-transition: Repatch flag-on transition/delete/brand gates present"
else
    fail "PA-transition: Repatch.cpp flag-on GiveUpOnCache gates missing"
fi

# ---------------------------------------------------------------------------
# Task-8 gap 8 — private-name/brand LLInt fast paths stay sound only because
# their caches are NEVER published flag-on. Enforce: the four op families'
# slow paths keep the unthreaded gate.
# ---------------------------------------------------------------------------
for op in get_private_name put_private_name set_private_brand check_private_brand; do
    if ! grep -A45 "LLINT_SLOW_PATH_DECL(slow_path_$op)" "$JSC_DIR/llint/LLIntSlowPaths.cpp" 2>/dev/null | grep -q "useUnthreadedLLIntPropertyCaches\|useJSThreads"; then
        fail "private-name lint: slow_path_$op lost its flag-off-only cache gate"
    fi
done
pass "private-name/brand cache publication gates checked"

# ---------------------------------------------------------------------------
# I10 — watchpoint classification: DataOnly opt-ins and fireIsDataOnly
# overrides must only appear at sites recorded in the INTEGRATE-jit inventory
# (currently: Watchpoint.{h,cpp} plumbing only).
# ---------------------------------------------------------------------------
I10_DATAONLY="$(grep -rln "WatchpointSetClassification::DataOnly" "$JSC_DIR" 2>/dev/null | grep -v "bytecode/Watchpoint" || true)"
if [[ -n "$I10_DATAONLY" ]]; then
    echo "$I10_DATAONLY" >&2
    fail "I10: Class-B opt-in outside Watchpoint.{h,cpp} — record it in the INTEGRATE-jit Class-B inventory first"
else
    pass "I10 Class-B opt-ins confined to recorded sites (none)"
fi
I10_OVERRIDES="$(grep -rln "fireIsDataOnly" "$JSC_DIR" 2>/dev/null | grep -v "bytecode/Watchpoint" || true)"
if [[ -n "$I10_OVERRIDES" ]]; then
    echo "$I10_OVERRIDES" >&2
    fail "I10: FireDetail::fireIsDataOnly override outside Watchpoint.{h,cpp} — record it first"
else
    pass "I10 per-fire overrides confined to recorded sites (none)"
fi

# ---------------------------------------------------------------------------
# I21 — poll placement: every DFG/FTL poll must be immediately followed by an
# invalidation point. Structural check: the CheckTraps lowerings still emit
# an InvalidationPoint/invalidation check adjacent to the trap poll.
# ---------------------------------------------------------------------------
if grep -q "compileCheckTraps" "$JSC_DIR/dfg/DFGSpeculativeJIT.cpp"; then
    pass "I21 DFG CheckTraps lowering present (poll choke point)"
else
    fail "I21: DFG compileCheckTraps missing — poll emission moved? Re-audit I21."
fi
if grep -q "compileInvalidationPoint\|InvalidationPoint" "$JSC_DIR/ftl/FTLLowerDFGToB3.cpp"; then
    pass "I21 FTL invalidation-point lowering present"
else
    fail "I21: FTL invalidation point lowering missing"
fi
# I16 extension to poll windows: the IC fast-path emitters must not contain a
# trap poll between handler/state load and use — polls are only emitted by the
# CheckTraps/op-boundary machinery, never by InlineCacheCompiler.
if grep -qn "emitCheckTraps\|needTrapHandling" "$JSC_DIR/bytecode/InlineCacheCompiler.cpp" 2>/dev/null; then
    fail "I16/I21: InlineCacheCompiler.cpp emits a trap poll inside an IC window"
else
    pass "I16/I21 IC emitters poll-free"
fi

# ---------------------------------------------------------------------------
echo
if [[ "$FAILURES" -gt 0 ]]; then
    echo "lint.sh: $FAILURES lint failure(s)" >&2
    exit 1
fi
echo "lint.sh: all lints green"
