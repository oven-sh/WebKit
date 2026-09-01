#!/usr/bin/env bash
# lint-lockorder-u20.sh — the U20 lock-order lint, EXTENDED per SPEC-congc
# ANNEX CGS2.1 to the LK.9c/9d rows (CG-T2; written by CG-1).
#
# This script is the ONE lock-order authority for the congc surface (the
# rev-7 "U20-class" private lint is retired). Its ADOPTION as the canonical
# U20 extension is SPEC-congc adoption gate §13.5(1) — OPEN until the
# SPEC-ungil owner cross-cites; running it is gate CG-T1/CG-T2 regardless.
#
# Encoded rules:
#   R1  §3.4 disposition coverage (SPEC-congc §3.4; ANNEX CGD6.2): every
#       m_gcConductorLock tryLock SITE in heap/** carries a
#       "§3.4 disposition (ANNEX CGD6.2" marker within the 14 preceding
#       lines (election / poll / §10D revert poll / F47 watchdog-ctor rows).
#   R1b The F47 watchdog-ctor row (PROCEED, no back-off) must EXIST —
#       CG-T1's grep-lint counts this site CLASSIFIED, keeping §3.4's
#       "every site" claim true.
#   R2  F21 clause (1) (CG-I10(1)): m_markingMutex is never acquired in a
#       rank 7-9b owner file (allocation/directory/block side). File-level
#       encoding: acquisition sites are allowed ONLY in the marking-side
#       allowlist below.
#   R3  F21 clause (2) (CG-I10(2)) / LK.9c: GCH::m_mutatorMarkStackLock (CMS,
#       lands at CG-2) is a TERMINAL leaf — no lock acquisition may appear in
#       the 8 lines following a CMS acquisition. Vacuously clean until CG-2.
#   R4  F21 clause (3) (CG-I10(3)): m_markingMutex > CMS nesting only at the
#       7-9b-free drain/donation sites — any file containing BOTH a
#       m_markingMutex acquisition and a CMS acquisition must mark each
#       nested site with "LK.9d>LK.9c". Vacuously clean until CG-2.
#   R5  CGS2.2 chain litmus (NL > GCL > m_markingMutex > CMS), negative
#       edges: (a) no m_gcConductorLock acquisition anywhere in
#       SlotVisitor.cpp / MarkStack.cpp (m_markingMutex holders never
#       acquire GCL — no reverse edge); (b) no m_markingMutex acquisition in
#       runtime/VMManager.cpp or bytecode/JSThreadsSafepoint.cpp outside the
#       heap-owned pause hook (GCL holders reach m_markingMutex only via
#       Heap-owned pause/resume — lands at CG-3). NL terms: ZERO edges to
#       check until nativeaffinity lands m_nativeLockDepth (recorded N/A).
#
# Exit 0 = clean; nonzero = violations printed.

set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HEAP="$ROOT/Source/JavaScriptCore/heap"
FAIL=0

note() { echo "lint-lockorder-u20: $*"; }
violation() { echo "lint-lockorder-u20: VIOLATION: $*"; FAIL=1; }

# --- R1: §3.4 disposition coverage over every GCL tryLock site in heap/** ---
R1_SITES=0
R1_COVERED=0
while IFS=: read -r file line _; do
    [ -z "${file:-}" ] && continue
    R1_SITES=$((R1_SITES + 1))
    start=$((line - 14)); [ "$start" -lt 1 ] && start=1
    if sed -n "${start},${line}p" "$file" | grep -q "§3.4 disposition (ANNEX CGD6.2"; then
        R1_COVERED=$((R1_COVERED + 1))
    else
        violation "R1: unclassified m_gcConductorLock tryLock site $file:$line (no '§3.4 disposition (ANNEX CGD6.2' marker within 14 preceding lines)"
    fi
done < <(grep -rn "m_gcConductorLock\.tryLock()" "$HEAP" --include='*.cpp' --include='*.h' | sed 's/:\([0-9]*\):.*/:\1:/')
note "R1: $R1_COVERED/$R1_SITES GCL tryLock sites classified"
[ "$R1_SITES" -lt 4 ] && violation "R1: expected >= 4 GCL tryLock sites (election, poll, revert poll, watchdog ctor); found $R1_SITES"

# --- R1b: the F47 watchdog-ctor row must exist ---
if grep -q "ANNEX CGD6.2 row: watchdog-ctor tryLock loop, F47" "$HEAP/Heap.cpp"; then
    note "R1b: F47 watchdog-ctor disposition row present (PROCEED)"
else
    violation "R1b: F47 watchdog-ctor disposition row missing from heap/Heap.cpp"
fi

# --- R2: F21(1) — m_markingMutex acquisitions only in marking-side files ---
MARKING_ALLOWLIST="Heap.cpp SlotVisitor.cpp MarkStack.cpp Heap.h SlotVisitor.h"
while IFS=: read -r file line _; do
    [ -z "${file:-}" ] && continue
    base="$(basename "$file")"
    ok=0
    for allowed in $MARKING_ALLOWLIST; do
        [ "$base" = "$allowed" ] && ok=1
    done
    if [ "$ok" -ne 1 ]; then
        violation "R2 (F21(1)): m_markingMutex acquired in non-marking-side file $file:$line — rank 7-9b owner files must never take LK.9d"
    fi
done < <(grep -rnE "Locker[^;]*m_markingMutex|holdLock\([^)]*m_markingMutex|m_markingMutex\.(try)?[Ll]ock" "$HEAP" --include='*.cpp' --include='*.h' | sed 's/:\([0-9]*\):.*/:\1:/')
note "R2: m_markingMutex acquisition files within allowlist"

# --- R3: F21(2)/LK.9c — CMS lock is a TERMINAL leaf ---
CMS_SITES=0
while IFS=: read -r file line _; do
    [ -z "${file:-}" ] && continue
    CMS_SITES=$((CMS_SITES + 1))
    end=$((line + 8))
    if sed -n "$((line + 1)),${end}p" "$file" | grep -qE "Locker[[:space:]]*[{(]|holdLock\(|\.lock\(\)|\.tryLock\(\)"; then
        violation "R3 (F21(2)): lock acquisition within 8 lines after CMS (m_mutatorMarkStackLock) acquisition at $file:$line — LK.9c is a TERMINAL leaf"
    fi
done < <(grep -rnE "Locker[^;]*m_mutatorMarkStackLock|holdLock\([^)]*m_mutatorMarkStackLock|m_mutatorMarkStackLock\.(try)?[Ll]ock" "$HEAP" --include='*.cpp' --include='*.h' | sed 's/:\([0-9]*\):.*/:\1:/')
note "R3: $CMS_SITES CMS acquisition sites (terminal-leaf check; vacuous until CG-2)"

# --- R4: F21(3) — m_markingMutex > CMS only at marked drain/donation sites ---
if [ "$CMS_SITES" -gt 0 ]; then
    while IFS=: read -r file line _; do
        [ -z "${file:-}" ] && continue
        start=$((line - 4)); [ "$start" -lt 1 ] && start=1
        if ! sed -n "${start},${line}p" "$file" | grep -q "LK.9d>LK.9c"; then
            # Only flag if this file also takes m_markingMutex (nesting candidate).
            if grep -qE "Locker[^;]*m_markingMutex" "$file"; then
                violation "R4 (F21(3)): CMS acquisition at $file:$line in a m_markingMutex-taking file without an 'LK.9d>LK.9c' site marker (drain/donation sites only)"
            fi
        fi
    done < <(grep -rnE "Locker[^;]*m_mutatorMarkStackLock|holdLock\([^)]*m_mutatorMarkStackLock" "$HEAP" --include='*.cpp' --include='*.h' | sed 's/:\([0-9]*\):.*/:\1:/')
fi
note "R4: m_markingMutex > CMS nesting markers checked (vacuous until CG-2)"

# --- R5: CGS2.2 negative edges ---
for f in "$HEAP/SlotVisitor.cpp" "$HEAP/MarkStack.cpp"; do
    if grep -nE "m_gcConductorLock\.(try)?[Ll]ock|Locker[^;]*m_gcConductorLock" "$f" >/dev/null 2>&1; then
        violation "R5a: m_gcConductorLock acquisition in $(basename "$f") — m_markingMutex holders must never acquire GCL (no reverse edge)"
    fi
done
note "R5a: no GCL acquisition under marking-internal files"
for f in "$ROOT/Source/JavaScriptCore/runtime/VMManager.cpp" "$ROOT/Source/JavaScriptCore/bytecode/JSThreadsSafepoint.cpp"; do
    if grep -nE "Locker[^;]*m_markingMutex|m_markingMutex\.(try)?[Ll]ock" "$f" >/dev/null 2>&1; then
        violation "R5b: direct m_markingMutex acquisition in $(basename "$f") — GCL holders reach LK.9d only via the Heap-owned pause hook (§9.1(2), CG-3)"
    fi
done
note "R5b: no foreign m_markingMutex acquisition (GCL>markingMutex edge is Heap-owned)"
note "R5c: NL edges N/A — m_nativeLockDepth not yet landed (nativeaffinity BL1.8; CGS2.2 records the GC-conduct NL>GCL edge as REMOVED)"

if [ "$FAIL" -ne 0 ]; then
    echo "lint-lockorder-u20: FAIL"
    exit 1
fi
echo "lint-lockorder-u20: clean (LK.9c/9d rows + three F21 clauses + CGS2.2 chain litmus + §3.4/F47 dispositions)"
