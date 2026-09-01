/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

// ConcurrentButterfly.h - the single header the JIT workstream includes for the
// shared-memory-threads object model (SPEC-objectmodel.md, frozen rev 14).
//
// This file provides:
//   - §2 tag-encoding constants for the butterfly word (bit 63 = shared-write
//     bit, bits 62..48 = installing thread's ButterflyTID, bits 47..0 = payload);
//   - §9.1 encode/decode helpers (ALWAYS_INLINE, total over all legal words);
//   - §9.2 the 128-bit {header, butterfly} DCAS plus the §3.0 volatile-byte
//     merge helpers;
//   - the ButterflyRegime enum (§9.5) and the word-level regime decoder used by
//     the §2 dispatch;
//   - a self-test (behind Options::verifyConcurrentButterfly()) covering
//     encode/decode round trips, the merge discipline, and DCAS semantics.
//
// Flag-off (I22): nothing in this header is reached by existing code paths;
// flat butterfly tags are all-zero, so untaggedButterfly() is the identity on
// every pointer today's engine produces.

#include "IndexingType.h"
#include "JSCell.h"
#include "Options.h"
#include "PropertyOffset.h"
#include <atomic>
#include <type_traits>
#include <wtf/Assertions.h>
#include <wtf/Atomics.h>
#include <wtf/ScopedLambda.h>
#include <wtf/StdLibExtras.h>

#if __has_include("VMLite.h")
// currentButterflyTID(): SOLE provider is SPEC-vmstate §6.7 (VMLite.h; NOT
// re-declared while present - ODR/dllimport). Returns 0 on the main thread and
// never notTTLTID.
#include "VMLite.h"
#endif

#if COMPILER(MSVC) && !COMPILER(CLANG)
#include <intrin.h>
#endif

namespace JSC {

class Butterfly;
struct ButterflySpine; // §4.1 layout; defined in Butterfly.h (objectmodel Task 4).
struct ButterflyFragment; // §4.1; defined in Butterfly.h (objectmodel Task 4).

#if !__has_include("VMLite.h")
// THREADS-INTEGRATE(objectmodel): interim shim per SPEC-objectmodel §9.1 -
// vmstate W3 has not landed VMLite.h yet, so until it does every thread is
// treated as the main thread (TID 0). The INTEGRATE doc records the swap: when
// VMLite.h appears, the __has_include above picks it up and this shim compiles
// away. Same pattern as the §10.6 STW stub.
ALWAYS_INLINE uint16_t currentButterflyTID() { return 0; }
#endif

using ButterflyTID = uint16_t; // 15-bit TID space (§2; 2^15 lifetime cap, no recycling this milestone).

// ===== §2 constants (regime selector) =====

static constexpr unsigned butterflyTIDShift = 48;
static constexpr uint64_t butterflyTIDMask = 0x7fffULL << butterflyTIDShift;
static constexpr uint64_t butterflySWBit = 1ULL << 63;
static constexpr uint64_t butterflyTagMask = butterflySWBit | butterflyTIDMask;
static constexpr uint64_t butterflyPointerMask = ~butterflyTagMask; // low 48 bits (§9.1)
static constexpr ButterflyTID mainThreadButterflyTID = 0; // == today's raw pointer (bit-identical, §2)
static constexpr ButterflyTID notTTLTID = 0x7fff; // reserved; never a real TID; TID==notTTLTID <=> segmented (I3)

static_assert(butterflyTagMask == 0xffff000000000000ULL, "tag occupies exactly the high 16 bits");
static_assert(butterflyPointerMask == 0x0000ffffffffffffULL, "payload is the low 48 bits");
static_assert(!(butterflyTIDMask & butterflySWBit), "TID field and SW bit are disjoint");
static_assert((static_cast<uint64_t>(notTTLTID) << butterflyTIDShift) == butterflyTIDMask, "notTTLTID is the all-ones TID");

// ===== Regimes (§9.5 enum; §2 decode is total) =====

enum class ButterflyRegime : uint8_t {
    None, // all-zero word: no out-of-line storage (§2.1)
    Flat, // payload != 0, TID != notTTLTID, SW = 0: today's layout, owner transitions lock-free (§3)
    FlatShared, // payload != 0, TID != notTTLTID, SW = 1: flat, shared-written; any transition => segmented (§3)
    Segmented, // TID == notTTLTID, SW = 1, payload = ButterflySpine* (§4)
};

// ===== §9.6 option probes =====
//
// Options::useJSThreads() is already landed (OptionsList.h). The other three
// §9.6 entries (forceSegmentedButterflies, forceButterflySWBit,
// verifyConcurrentButterfly) belong to shared OptionsList.h (integration
// manifest entry 1, NOT implementer-editable). So this header compiles dark
// either way, the accessors below detect the option's presence by SFINAE:
// absent => constant false (today's behavior); once manifest entry 1 lands the
// real option is picked up automatically with no source change here.
// THREADS-INTEGRATE(objectmodel): exact OptionsList.h text is in
// docs/threads/INTEGRATE-objectmodel.md.

namespace ConcurrentButterflyInternal {

#define JSC_CONCURRENT_BUTTERFLY_OPTION_PROBE(name) \
    template<typename T = ::JSC::Options, typename = void> \
    struct OptionProbe_##name { \
        static ALWAYS_INLINE bool get() { return false; } \
    }; \
    template<typename T> \
    struct OptionProbe_##name<T, std::void_t<decltype(T::name())>> { \
        static ALWAYS_INLINE bool get() { return static_cast<bool>(T::name()); } \
    };

JSC_CONCURRENT_BUTTERFLY_OPTION_PROBE(verifyConcurrentButterfly)
JSC_CONCURRENT_BUTTERFLY_OPTION_PROBE(forceSegmentedButterflies)
JSC_CONCURRENT_BUTTERFLY_OPTION_PROBE(forceButterflySWBit)

#undef JSC_CONCURRENT_BUTTERFLY_OPTION_PROBE

} // namespace ConcurrentButterflyInternal

ALWAYS_INLINE bool verifyConcurrentButterflyEnabled()
{
    return ConcurrentButterflyInternal::OptionProbe_verifyConcurrentButterfly<>::get();
}

ALWAYS_INLINE bool forceSegmentedButterfliesEnabled()
{
    return ConcurrentButterflyInternal::OptionProbe_forceSegmentedButterflies<>::get();
}

ALWAYS_INLINE bool forceButterflySWBitEnabled()
{
    return ConcurrentButterflyInternal::OptionProbe_forceButterflySWBit<>::get();
}

// ===== §9.1 encode/decode =====

// Validates I2 (structurally: null payload => all-zero word) and I3
// (notTTLTID => SW=1). Liveness of the payload (the rest of I2) is not
// checkable here. RELEASE_ASSERT semantics: only called when
// verifyConcurrentButterfly is on.
ALWAYS_INLINE void validateTaggedButterflyWord(uint64_t tagged)
{
    uint64_t payload = tagged & butterflyPointerMask;
    if (!payload)
        RELEASE_ASSERT(!tagged); // payload 0 + nonzero tag is illegal (§2)
    else if (((tagged & butterflyTIDMask) >> butterflyTIDShift) == notTTLTID)
        RELEASE_ASSERT(tagged & butterflySWBit); // notTTLTID + SW=0 is illegal (I3)
}

ALWAYS_INLINE uint64_t encodeButterflyTag(ButterflyTID tid, bool sharedWrite)
{
    ASSERT(tid <= notTTLTID); // only 2^15 TIDs
    return (static_cast<uint64_t>(tid) << butterflyTIDShift)
        | (sharedWrite ? butterflySWBit : 0);
}

ALWAYS_INLINE uint64_t encodeButterfly(Butterfly* butterfly, ButterflyTID tid, bool sharedWrite)
{
    uint64_t tagged = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(butterfly)) | encodeButterflyTag(tid, sharedWrite);
    ASSERT(butterfly || !tagged); // never tag a null payload (§2)
    if (verifyConcurrentButterflyEnabled()) [[unlikely]]
        validateTaggedButterflyWord(tagged);
    return tagged;
}

// Segmented words are (notTTLTID, SW=1, spine) by construction (§2/I3).
ALWAYS_INLINE uint64_t encodeSegmentedButterfly(ButterflySpine* spine)
{
    ASSERT(spine);
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(spine)) | encodeButterflyTag(notTTLTID, true);
}

ALWAYS_INLINE Butterfly* untaggedButterfly(uint64_t tagged) // masks top 16
{
    return reinterpret_cast<Butterfly*>(static_cast<uintptr_t>(tagged & butterflyPointerMask));
}

ALWAYS_INLINE ButterflyTID butterflyTID(uint64_t tagged)
{
    return static_cast<ButterflyTID>((tagged & butterflyTIDMask) >> butterflyTIDShift);
}

ALWAYS_INLINE bool butterflySharedWrite(uint64_t tagged)
{
    return !!(tagged & butterflySWBit);
}

ALWAYS_INLINE bool isSegmentedButterfly(uint64_t tagged) // != 0 && TID == notTTLTID
{
    return !!(tagged & butterflyPointerMask) && butterflyTID(tagged) == notTTLTID;
}

ALWAYS_INLINE ButterflySpine* butterflySpine(uint64_t tagged) // pre: isSegmented
{
    if (verifyConcurrentButterflyEnabled()) [[unlikely]]
        validateTaggedButterflyWord(tagged);
    ASSERT(isSegmentedButterfly(tagged));
    return reinterpret_cast<ButterflySpine*>(static_cast<uintptr_t>(tagged & butterflyPointerMask));
}

// The 0-based out-of-line slot index k for an out-of-line PropertyOffset: the
// flat slot lives at B-16-8k and the segmented slot is
// spine->outOfLineSlot(k) (§4.1/I8 - the equations coincide on aliased
// fragments). NOTE: offsetInOutOfLineStorage(offset) (PropertyOffset.h) is the
// NEGATIVE PropertyStorage index -(k+1) used for propertyStorage()[...] flat
// indexing - it is NOT this value; spine accessors take k.
ALWAYS_INLINE unsigned outOfLineButterflyIndex(PropertyOffset offset)
{
    ASSERT(isOutOfLineOffset(offset));
    return static_cast<unsigned>(offset - firstOutOfLineOffset);
}

// §3 write-foreignness, with the §9.6 forceButterflySWBit stress fold (Task
// 10): under the stress option EVERY write - the owner's included - takes the
// §3 foreign-write route (F1 fire + SW DCAS in ensureSharedWriteBit, or the
// §4.6/§4.8 carve-outs), exercising I4/I12/M4 and the FlatShared/segmented
// regimes on single-threaded workloads. Without it this is the plain §3 tag
// compare. Precondition: callers dispatch None/Segmented first (§2; segmented
// words are SW=1 by I3, so foreignness is moot there).
ALWAYS_INLINE bool butterflyWriterIsForeign(uint64_t tagged)
{
    return butterflyTID(tagged) != currentButterflyTID() || forceButterflySWBitEnabled();
}

// Total decode per §2: payload==0 is tested BEFORE any TID compare, so an
// all-zero word (today's "no butterfly") never touches the tag logic, and TID 0
// (main thread) is bit-identical to today's raw pointer.
ALWAYS_INLINE ButterflyRegime butterflyRegimeForWord(uint64_t tagged)
{
    if (verifyConcurrentButterflyEnabled()) [[unlikely]]
        validateTaggedButterflyWord(tagged);
    if (!(tagged & butterflyPointerMask)) {
        ASSERT(!tagged); // §2: payload 0 + nonzero tag illegal
        return ButterflyRegime::None; // N4: None payloads never dereferenced
    }
    if (butterflyTID(tagged) == notTTLTID) {
        ASSERT(butterflySharedWrite(tagged)); // I3
        return ButterflyRegime::Segmented;
    }
    return butterflySharedWrite(tagged) ? ButterflyRegime::FlatShared : ButterflyRegime::Flat;
}

// ===== §9.2 DCAS and header-merge helpers =====

// Bytes [0,16) of a JSObjectWithButterfly cell: 8B cell header + 8B tagged
// butterfly word (GT#3). MarkedBlock cell bases are 16B-aligned (I1);
// PreciseAllocation cells sit at 8-mod-16 and are FORBIDDEN here (I36) - the
// RELEASE_ASSERT below enforces both.
struct CellHeaderAndButterfly {
    uint64_t header;
    uint64_t taggedButterfly;
};

static_assert(sizeof(CellHeaderAndButterfly) == 16);
static_assert(OBJECT_OFFSETOF(CellHeaderAndButterfly, header) == 0);
static_assert(OBJECT_OFFSETOF(CellHeaderAndButterfly, taggedButterfly) == 8);
static_assert(sizeof(JSCell) == 8, "cell header is exactly the first 8 bytes");

// §3.0 volatile bytes - never owned by transitions, CASed at any time even
// under the cell lock (GT#2): m_cellState (GC marking CAS), the two lock
// bits in m_indexingTypeAndMisc (held 0x40 / parked 0x80; a waiter may set the
// parked bit while the lock is held), and - review round 4 - the per-cell bit
// (TypeInfoPerCellBit) in m_flags. The per-cell bit is mutated at runtime by
// JSCell::setPerCellBit (e.g. JSFunction allocation-profile
// seenMultipleCalleeObjects) with no cell lock and no awareness of the
// transition protocol; flag-on it is a lock-free byte CAS (JSCellInlines.h)
// and every header CAS/DCAS merges its lane from the freshest read like
// cellState. Structures never carry the bit (TypeInfo::mergeInlineTypeFlags
// exists precisely to preserve it across structure changes), so treating it
// as volatile-merge is exact. Every header CAS/DCAS must copy these lanes
// verbatim from the freshest read (I26), else a lost unpark deadlocks, GC
// marking state is clobbered, or a per-cell-bit flip is silently undone.
// JIT NOTE: emitted §5.5 DCAS sequences must use this same mask (recorded in
// INTEGRATE-objectmodel.md round 4).
static constexpr uint64_t cellHeaderVolatileMask =
    (0xffULL << (8 * JSCell::cellStateOffset()))
    | (static_cast<uint64_t>(IndexingTypeLockIsHeld | IndexingTypeLockHasParked) << (8 * JSCell::indexingTypeAndMiscOffset()))
    | (static_cast<uint64_t>(TypeInfoPerCellBit) << (8 * JSCell::typeInfoFlagsOffset()));

static_assert(JSCell::structureIDOffset() == 0);
static_assert(JSCell::indexingTypeAndMiscOffset() == 4);
static_assert(JSCell::typeInfoTypeOffset() == 5);
static_assert(JSCell::typeInfoFlagsOffset() == 6);
static_assert(JSCell::cellStateOffset() == 7);
static_assert(cellHeaderVolatileMask == ((0xffULL << 56) | (static_cast<uint64_t>(0xC0) << 32) | (static_cast<uint64_t>(0x80) << 48)),
    "volatile mask covers exactly m_cellState, the two lock bits, and the per-cell type-flags bit");

// NOTE: the byte-offset -> uint64-bit-lane mapping above (and the DCAS lane
// packing below) assumes little-endian, which holds on every flag-on target
// (x86-64/arm64). On other targets this header still compiles (I22) but the
// DCAS traps if reached.

ALWAYS_INLINE bool headerDiffersOnlyInVolatileBits(uint64_t expected, uint64_t fresh)
{
    return !((expected ^ fresh) & ~cellHeaderVolatileMask);
}

ALWAYS_INLINE uint64_t mergeVolatileHeaderBits(uint64_t desired, uint64_t fresh)
{
    return (desired & ~cellHeaderVolatileMask) | (fresh & cellHeaderVolatileMask);
}

#if (CPU(X86_64) || CPU(ARM64)) && USE(JSVALUE64)
#define JSC_CONCURRENT_BUTTERFLY_HAS_HARDWARE_DCAS 1
#else
#define JSC_CONCURRENT_BUTTERFLY_HAS_HARDWARE_DCAS 0
#endif

// Strong 128-bit CAS at the cell base; seq_cst (M3). I32: must lower to inline
// hardware atomics ONLY - `lock cmpxchg16b` (requires -mcx16; integration
// manifest entry 3), `casp` (LSE) or an ldxp/stxp loop on arm64. A lock-based
// libatomic fallback is forbidden: it would not be atomic against the 64-bit
// casButterfly, the 1-byte lock-bit CASes in the same 16 bytes, or plain
// tagged-word loads. The GCC/Clang path uses the __sync builtin, which is
// either inlined or a link error - it never silently routes through libatomic.
//
// V7 (TSAN, review amendment D — PRECONDITION TO VERIFY on the rebuilt
// WebKitBuild/TSan binary): the V7 annotation scheme (relaxed header-byte
// loads in JSCell.h, spine tsanPublish/tsanConsume pairs in Butterfly.h /
// ConcurrentButterfly.cpp) assumes clang lowers this __sync builtin to atomic
// cmpxchg IR that the TSAN pass instruments (__tsan_atomic128_compare_
// exchange_strong), making the publish an ATOMIC write in TSAN's model. One-
// shot empirical check before trusting a green V7 rung: disassemble or
// `nm`-grep the TSAN jsc for __tsan_atomic128_* referenced from this TU (or
// inspect any surviving report's dcas frame for an atomic-op annotation). If
// the CAS is NOT instrumented (raw cmpxchg16b asm), the publish is invisible
// to TSAN and the dcas-vs-header-load family will keep reporting: the fix
// then is to route the publish through explicit __tsan_atomic128 hooks under
// TSAN_ENABLED — do NOT suppress.
ALWAYS_INLINE bool dcasHeaderAndButterfly(JSCell* cell, CellHeaderAndButterfly expected, CellHeaderAndButterfly desired)
{
    RELEASE_ASSERT(!(reinterpret_cast<uintptr_t>(cell) & 15)); // I1; PA cells (8-mod-16) forbidden (I36)
#if JSC_CONCURRENT_BUTTERFLY_HAS_HARDWARE_DCAS
#if COMPILER(MSVC) && !COMPILER(CLANG)
    // _InterlockedCompareExchange128(dest, high, low, comparand): high lane =
    // bytes [8,16) = taggedButterfly, low lane = bytes [0,8) = header.
    // `expected` is by-value, so the comparand write-back is discarded.
    return !!_InterlockedCompareExchange128(
        reinterpret_cast<long long volatile*>(cell),
        static_cast<long long>(desired.taggedButterfly),
        static_cast<long long>(desired.header),
        reinterpret_cast<long long*>(&expected));
#else
    unsigned __int128 expectedWord = (static_cast<unsigned __int128>(expected.taggedButterfly) << 64) | expected.header;
    unsigned __int128 desiredWord = (static_cast<unsigned __int128>(desired.taggedButterfly) << 64) | desired.header;
    return __sync_bool_compare_and_swap(reinterpret_cast<unsigned __int128*>(cell), expectedWord, desiredWord);
#endif
#else
    UNUSED_PARAM(expected);
    UNUSED_PARAM(desired);
    // No 128-bit hardware CAS on this target: useJSThreads must stay off
    // (I22/I32); nothing reaches this function flag-off.
    RELEASE_ASSERT_NOT_REACHED();
    return false;
#endif
}

// I32 startup backstop (integration manifest entry 4a calls this behind
// useJSThreads): the 16-byte CAS at `sampleCell` must be lock-free.
// THREADS-INTEGRATE(objectmodel): wired into VM startup by the integrator.
ALWAYS_INLINE bool concurrentButterflyAtomicsAreLockFree(void* sampleCell)
{
#if JSC_CONCURRENT_BUTTERFLY_HAS_HARDWARE_DCAS
#if COMPILER(CLANG)
    // __atomic_is_lock_free(16, runtimePtr) is a libatomic libcall that
    // reports 16-byte objects as NOT lock-free at runtime even with -mcx16
    // (libatomic ABI quirk), so query the compile-time form with a null
    // pointer (assumes natural max alignment; callers pass alignas(16) cells).
    UNUSED_PARAM(sampleCell);
    return __atomic_always_lock_free(16, nullptr) && __atomic_always_lock_free(8, nullptr) && __atomic_always_lock_free(1, nullptr);
#else
    // GCC: __atomic_is_lock_free(16, ...) is a libatomic libcall, and libatomic
    // must not be linked (I32). The __sync builtin used by
    // dcasHeaderAndButterfly is inlined-or-link-error, never lock-based, so
    // lock-freedom is guaranteed structurally on this path. MSVC:
    // _InterlockedCompareExchange128 is always inline cmpxchg16b/casp.
    UNUSED_PARAM(sampleCell);
    return true;
#endif
#else
    UNUSED_PARAM(sampleCell);
    return false;
#endif
}

// ===== §9.3 spine/fragment operations (frozen signatures) =====
//
// Declarations only at Task 2: the ButterflySpine/ButterflyFragment layout (§4.1)
// lands with Task 4 (Butterfly.h/ButterflyInlines.h) and the definitions of
// these functions land with Tasks 4/5 (ButterflyInlines.h/ConcurrentButterfly.cpp).
// Nothing calls them flag-off (I22), and segmented words cannot exist before
// Task 5 publishes the first spine, so declaring ahead is sound. Task 2's
// JSObject accessors (locationForOffset / quickly-family / length()) dispatch
// to the *IfWithinBounds/*IfReadable variants below.

JS_EXPORT_PRIVATE ButterflyFragment* spineOutOfLineFragment(ButterflySpine*, unsigned fragmentIndex);
JS_EXPORT_PRIVATE ButterflyFragment* spineIndexedFragment(ButterflySpine*, unsigned fragmentIndex);
JS_EXPORT_PRIVATE WriteBarrierBase<Unknown>* segmentedOutOfLineSlot(ButterflySpine*, PropertyOffset); // pre: I33 bound
WriteBarrierBase<Unknown>* segmentedIndexedSlot(ButterflySpine*, unsigned index); // pre: C4 (index < spine->vectorLength); ALWAYS_INLINE in ConcurrentButterflyInlines.h (T2-segmented-accessors-inline)
uint32_t segmentedPublicLength(ButterflySpine*); // fragment 0 slot 0, low half (C4: shared across spines); ALWAYS_INLINE in ConcurrentButterflyInlines.h
JS_EXPORT_PRIVATE void setSegmentedPublicLength(ButterflySpine*, uint32_t);

// Task-2 consumer additions (not in the frozen §9.3 list; bounds-checked
// variants of the accessors above, so callers cannot violate the I33/C4
// preconditions under a stale-spine race):
//
// - segmentedOutOfLineSlotIfWithinBounds: nullptr iff
//   offsetInOutOfLineStorage(offset) >= 4 * spine->outOfLineFragmentCount
//   (I33 out-of-line clause; nullptr = stale spine => caller acquire-re-loads
//   the tagged word and re-dispatches).
// - segmentedIndexedSlotIfReadable: nullptr iff
//   index >= min(segmentedPublicLength(spine), spine->vectorLength) (C4; reads).
// - segmentedIndexedSlotIfWithinVectorLength: nullptr iff
//   index >= spine->vectorLength (writes that may bump publicLength, mirroring
//   the flat vectorLength bound of setIndexQuickly/trySetIndexQuickly).
// - segmentedVectorLength: the loaded spine's authoritative, per-spine-immutable
//   vectorLength (§4.1).
//
// T2-segmented-accessors-inline: the four below are ALWAYS_INLINE in
// ConcurrentButterflyInlines.h (included from JSObject.h) — they were the
// dominant serial-phase self% in the W>=2 SCALEBENCH profile (PLT stub +
// frame on every segmented indexed read/write). No out-of-line copy is kept:
// every caller reaches them through JSObject.h.
WriteBarrierBase<Unknown>* segmentedOutOfLineSlotIfWithinBounds(ButterflySpine*, PropertyOffset);
WriteBarrierBase<Unknown>* segmentedIndexedSlotIfReadable(ButterflySpine*, unsigned index);
WriteBarrierBase<Unknown>* segmentedIndexedSlotIfWithinVectorLength(ButterflySpine*, unsigned index);
uint32_t segmentedVectorLength(ButterflySpine*);

class JSObject;
class JSObjectWithButterfly;
class Structure;
class VM;

// §4.2 flat->segmented conversion (frozen §9.3 signature; defined in
// ConcurrentButterfly.cpp, Task 5). ONE publication: a transition trigger
// (newStructureOrNull != nullptr, adding the out-of-line property at the given
// PropertyOffset with the given JSValue) is published by the single
// nuke+DCAS together with the fully sized spine - never an intermediate
// {old structure, undersized spine}. newStructureOrNull == nullptr is the
// in-place form (T2 array-resize trigger: structure unchanged; pass
// invalidOffset / JSValue()).
//
// Returns the published spine on success; nullptr means RESTART (§4.2): the
// caller must re-enter the WHOLE operation from §2 dispatch on the fresh
// tag + structureID (fresh target, fresh F1/F2 checks, fresh allocation),
// lock-free at restart.
//
// AB18-S2 stale-parent guard (I21): when the trigger is a transition
// (newStructureOrNull != nullptr), the caller MUST pass the source structure
// it derived the target from as expectedSourceOrNull. The conversion
// publishes newStructureOrNull only while the object's structureID still
// equals that source (checked at entry and re-checked under the cell lock by
// the same sourceID the nuke-CAS expects). Without it, a racing transition
// that publishes between the caller's own source check and this function's
// structureID capture would be silently ERASED: the conversion would
// validate against the racer's fresh structure but publish a target derived
// from the stale parent - a lost property add. The in-place form
// (newStructureOrNull == nullptr) preserves whatever structure is current,
// so it passes nullptr.
JS_EXPORT_PRIVATE ButterflySpine* convertToSegmentedButterfly(VM&, JSObjectWithButterfly*, Structure* expectedSourceOrNull, Structure* newStructureOrNull, PropertyOffset, JSValue);

// ===== §4.4 array transitions (Task 8; defined in ConcurrentButterfly.cpp) =====
//
// casButterfly (frozen §9.3 signature) is THE publication form for every
// element-storage resize (I16: resizes never touch the header) and, more
// generally, for every butterfly-pointer mutation on an object with indexed
// properties - even under the cell lock (I17). One 64-bit seq_cst CAS on the
// tagged word (M3). expected 0 = the §2.1 N3 first install. Returns false on
// CAS failure: the caller must RE-DISPATCH on the fresh tag (§2), NEVER
// blind-retry - in particular an SW flip mid-resize means a foreign store
// landed in the OLD payload after the copy; re-CASing the copied payload
// would drop it (I21), so the re-dispatch lands on T2 (grow segmented) /
// the §4.6 AS route instead.
//
// I27 asserts (debug + verifyConcurrentButterfly):
//   - T1 form: flat payload replacement with tag exactly
//     (currentButterflyTID(), 0) on BOTH sides (lock-free owner-only copying
//     resize), OR cell-locked with an ArrayStorage shape (§4.6 AS-COPY: tag -
//     including SW - preserved verbatim; sole sanctioned non-(currentTID,0)
//     copy outside STW, I27/I31).
//   - T2 form: segmented -> segmented replacement-spine publication
//     ((notTTLTID, 1) both sides; flat->segmented goes through the §4.2
//     nuke + DCAS, never this CAS).
//   - N3 form: 0 -> (currentButterflyTID(), 0) flat first install.
//   - SW monotonic (I4); no payload-preserving tag-only flips here (those are
//     ensureSharedWriteBit's §3.0 DCAS / I36 locked CAS).
JS_EXPORT_PRIVATE bool casButterfly(JSObjectWithButterfly*, uint64_t expectedTagged, uint64_t newTagged);
// §4.4; asserts I27; expected 0=N3 install; false=>re-dispatch, never blind-retry.

// §4.6 AS-COPY publication (T3: casButterfly as publication form, I17, under
// the cell lock). Preserves the tag - including SW - verbatim. The CAS cannot
// lose: every AS butterfly mutation is cell-locked (I31), AS SW flips are
// per-event stops, and lock-free §4.4 CASes never target AS words - so failure
// RELEASE_ASSERTs. Emits the publication write barrier.
JS_EXPORT_PRIVATE void publishArrayStorageButterflyLocked(VM&, JSObjectWithButterfly*, Butterfly* newButterfly);

// §4.4 T2: segmented vectorLength growth - allocates a replacement spine
// (copy + append fresh indexed fragments; fragments never move/reused;
// aliasedAllocationBase/Size copied VERBATIM - I7; spineEpoch incremented;
// fresh slots hole-filled per the CURRENT shape: Double => PNaN raw lanes,
// else cleared) and publishes it with one casButterfly, tag (notTTLTID, 1)
// (I27). Coverage is over-allocated geometrically (the same nextLength()
// 1.5x policy as the flat T1 path / flag-off ensureLengthSlow, capped at
// MAX_STORAGE_VECTOR_LENGTH), so a steady append stream takes O(log n)
// replacement spines - pure amortization; the published spine always covers
// >= newVectorLength. Spines still carrying a conversion-era C2 tail
// (vectorLength < fragment coverage: those slots alias memory past the flat
// allocation's precise end and are never dereferenceable) instead migrate to
// fully fresh indexed fragments under a §10.6 per-event stop - sound because
// I34 lets no access hold a slot pointer across a stop; racing requesters of
// the SAME object's migration are collapsed into one stop by a
// stop-participating claim (losers park-wait and re-dispatch stop-free); see
// the definition's comment.
// Caller must hold NO §6-ranked lock (the stop veneer's GT11 contract).
// Returns true when the published (or a racing) spine covers
// newVectorLength; false = the word is no longer segmented, the shape moved,
// or the CAS lost - the caller re-dispatches on the fresh tag (§4.4).
JS_EXPORT_PRIVATE bool tryGrowSegmentedVectorLength(VM&, JSObjectWithButterfly*, unsigned newVectorLength);

// Flag-on replacement for JSObject::ensureLengthSlow (GT10): full §2 dispatch.
//   - Flat (currentTID, 0): T1 - fresh copy (never in-place realloc flag-on;
//     M8 disables them) published by casButterfly expecting exactly
//     (currentTID, 0); failure => re-dispatch, never re-copy (I27). NOTE
//     (review round 1): the former T5 in-place vectorLength growth was
//     REMOVED - lock-free foreign readers have only a control dependency from
//     the vectorLength load to the slot load (no load->load ordering on
//     arm64), so an in-place bound raise could pair the new vectorLength with
//     pre-hole-fill slot garbage. Flag-on, a published flat butterfly's
//     vectorLength is therefore IMMUTABLE: every bound increase publishes
//     fresh storage behind the butterfly word (dependency-ordered).
//   - Flat foreign or SW=1: convert (§4.2, in-place form), re-dispatch => T2.
//   - Segmented: tryGrowSegmentedVectorLength (T2).
//   - CoW: owner => convertFromCopyOnWrite (tags (currentTID, 0)); foreign =>
//     ensureSharedWriteBit's §4.8 materialize-first; then re-dispatch (I35).
// Returns false only on allocation failure (as today's ensureLengthSlow).
JS_EXPORT_PRIVATE bool ensureLengthSlowConcurrent(VM&, JSObjectWithButterfly*, unsigned length);

// Flag-on replacement for JSObject::reallocateAndShrinkButterfly (GT10).
//   - Flat (currentTID, 0): today's copy-shrink, published by casButterfly
//     (I17/I27 owner form); CAS failure => re-dispatch.
//   - Flat SW=1: in-place truncation only - clear [length, min(VL, publicLength))
//     per shape, fence, setPublicLength(length). No copy outside STW (I27);
//     the capacity shrink is forgone (it is only an optimization).
//   - Flat foreign SW=0: a shrink is a foreign WRITE - ensureSharedWriteBit
//     (F1) first, then re-dispatch.
//   - Segmented: clear [length, min(publicLength, spine vectorLength)) through
//     the loaded spine's slots (C4/I33), then setSegmentedPublicLength(length).
//     publicLength is shared across spines (C4), so no republication needed.
// NOTE the post-condition is weaker than flag-off: vectorLength may stay
// larger than `length` on shared objects; publicLength == length always.
JS_EXPORT_PRIVATE void shrinkButterflyForSetLengthConcurrent(VM&, JSObjectWithButterfly*, unsigned length);

// §4.5 GC hook (frozen §9.3 signature; Task 6b), called from the owned
// JSObject.cpp visitButterflyImpl once its §2 dispatch has Dependency-loaded a
// Segmented tagged word (step 2 lives at the call site). The definition is in
// ConcurrentButterfly.cpp, explicitly instantiated for SlotVisitor and
// AbstractSlotVisitor (the two visitButterflyImpl instantiations).
//
// Contract (§4.5 steps 3-6; review round 2): the CALLER supplies the
// {structureID, structure, maxOffset, indexingMode} snapshot it BRACKETED
// around the spine load (visitButterflyImpl's ReadStructureEarly /
// ReadButterfly / ReadStructureLate discipline, M7(a): the spine is
// dependency-ordered after the early structureID, and the late re-check ran
// before this call). The function never re-derives the shape from a fresh
// structureID load - a fresh load could pair a NEWER structure (e.g. a §4.7
// Double->Contiguous relabel, or a structure published after a mode-(b) T2
// migration) with the caller's OLDER spine and value-visit raw double or
// uninitialized lanes as JSValues. It markAuxiliaries the spine and
// aliasedAllocationBase (if set), markAuxiliaries every fragment outside
// [aliasedBase, aliasedBase + aliasedSize), value-visits out-of-line slots
// only up to outOfLineSize (HIGH-end slots per the descending §4.1 equation)
// and indexed slots per C4/I33 (bound = min(publicLength, the SAME spine's
// vectorLength); fragment 0 slot 0 - the frozen flat IndexingHeader - skipped;
// Double shapes never value-visited, §4.7; AS never appears, I31), then
// re-loads the structureID and compares it against the caller's EARLY id (and
// re-compares maxOffset). Returns the matched Structure*, or nullptr on any
// detected race - the caller reports didRace (the object is revisited).
template<typename Visitor>
Structure* visitSegmentedButterfly(Visitor&, JSObjectWithButterfly*, ButterflySpine*, StructureID expectedStructureID, Structure*, PropertyOffset maxOffset, IndexingType indexingMode);

// ===== §4.3 transition protocol + N2 (Task 6; defined in ConcurrentButterfly.cpp) =====
//
// trySegmentedTransition is the restartable §4.3 core ("segmented transition
// protocol, also locked flat transitions"): allocate -> cell lock -> re-verify
// (structureID == expectedSource, allocation still fits) -> release-store the
// new property's value (M2/I9) -> nuke + 128-bit DCAS of {type header,
// butterfly word} (PA cells: the I36/M8 fenced order) -> unlock, with the full
// §4.3 DCAS-failure taxonomy (a)-(d). It dispatches on the §2 regime:
//   - None: first out-of-line install, N3 published through the §4.3 DCAS
//     with expected butterfly word 0, tag (currentButterflyTID(), 0);
//   - Flat, owner, SW=0: locked stay-flat transition - reuse (capacity
//     suffices) or copy-grow (fresh flat allocation); expected SW=0; a SW flip
//     against a copied payload is taxonomy (b2) => RESTART (I21);
//   - Flat, foreign or SW=1 (after the step-0 F2 stop): routed to §4.2
//     (convertToSegmentedButterfly) - never converts holding the cell lock;
//   - Segmented: §4.3 proper - spine reuse, or replacement spine
//     (copy + append fresh fragments; aliasedAllocationBase/Size copied
//     VERBATIM, I7; spineEpoch incremented).
//
// Returns false for RESTART (§4.2 rule): the caller must re-enter its WHOLE
// operation from §2 dispatch on the fresh tag + structureID (fresh target
// recomputation, fresh F1/F2 checks, fresh allocation). false covers: the
// step-0 F2 stop fired, expectedSource is no longer the object's structure,
// taxonomy (b2), and a needed in-place conversion (re-dispatch lands back here
// with the object segmented). expectedSource anchors applicability: a caller
// computed newStructure FROM expectedSource, and proceeding after a racing
// transition replaced it would drop the racer's property (I21).
JS_EXPORT_PRIVATE bool trySegmentedTransition(VM&, JSObjectWithButterfly*, Structure* expectedSource, Structure* newStructure, PropertyOffset, JSValue);

// §4.6 (I31) out-of-line property transition on an ArrayStorage-shaped
// object. AS never segments (I31), so the §4.3 / §4.2 protocols above are
// excluded (their entry RELEASE_ASSERTs trip). E4 is also excluded
// (Structure::mayTransitionLockFreeFromThisStructure rejects AS shapes). So
// every AS out-of-line property add lands here: cell-locked AS-COPY (T3/I17,
// publishArrayStorageButterflyLocked) when capacity grows, or a cell-locked
// in-place slot store when it does not, then a nuke-bracketed structure
// publish (M5/M8). A foreign first WRITE first runs the §4.6 per-event SW
// stop (ensureSharedWriteBit) and RESTARTs (I12). Same false=RESTART contract
// as trySegmentedTransition. FUZZ r3-001 routing: pre-existing - not §45 -
// non-dictionary AS objects with out-of-line property adds (e.g. an object
// literal with a sparse-index integer key plus enough named properties to
// spill out of line) had no route between the E4 AS exclusion and the §4.3
// I31 entry assert.
JS_EXPORT_PRIVATE bool tryArrayStoragePropertyTransition(VM&, JSObjectWithButterfly*, Structure* expectedSource, Structure* newStructure, PropertyOffset, JSValue);

// tryStructureOnlyTransition is the restartable N2 (§2.1) locked core for
// butterfly-untouched transitions (inline adds, attribute/brand/structure-only
// reshapes): step-0 F2 firing for foreign/shared triggers, cell lock,
// structureID re-verification, release-store of the inline value FIRST (no
// holes, I9), then ONE 64-bit header CAS under the §3.0 volatile-byte merge
// discipline (no nuke - the butterfly word is untouched; 8B-aligned, so legal
// on PA cells too). Same false=RESTART contract as trySegmentedTransition.
// inlineOffset == invalidOffset means no value store (pure reshape).
JS_EXPORT_PRIVATE bool tryStructureOnlyTransition(VM&, JSObject*, Structure* expectedSource, Structure* newStructure, PropertyOffset inlineOffset, JSValue);

// Frozen §9.3 signatures (=§4.3 / N2 locked path). These drivers anchor the
// source on the object's settled structure at entry (spinning past a
// mid-publication nuked StructureID, M5) and retry the try* core through every
// recoverable RESTART (step-0 firing, refits, taxonomy (a)-(c)/(b2), racing
// resizes/conversions). If a racing transition changes the SOURCE structure,
// the supplied target no longer applies and proceeding would lose the racer's
// property (I21): callers that can lose that race MUST use the try* forms from
// their own §2 dispatch loop - the void drivers RELEASE_ASSERT it (sole
// exception: the race published exactly newStructure, in which case the value
// is stored into the now-existing slot - SAB last-writer-wins).
JS_EXPORT_PRIVATE void segmentedTransition(VM&, JSObjectWithButterfly*, Structure*, PropertyOffset, JSValue); // = §4.3
JS_EXPORT_PRIVATE void structureOnlyTransition(VM&, JSObject*, Structure*, PropertyOffset inlineOffset, JSValue); // N2 locked path

// Frozen §9.3 signature; defined in ConcurrentButterfly.cpp (Task 7). The §3
// foreign-first-write handler: ensures the object's butterfly word carries
// SW=1 or has left the flat regime, then returns - the caller re-dispatches on
// the fresh tag (§2). Covers: F1 fire-then-DCAS under the §3.0 merge loop
// (I12/I13/I10b; divergence => abandon + re-dispatch); the §4.6 AS carve-out
// (per-event STW that fires the set and publishes (installerTID, 1) FLAT in
// the same stop, I31); the §4.8/I35 CoW carve-out (materialize a private flat
// butterfly tagged (currentButterflyTID(), 0) FIRST - the CoW check precedes
// the SW DCAS - after which the caller's re-dispatch lands on the owner
// path); the I36 PA carve-out (cell-locked 64-bit CAS flip, no 16B DCAS);
// and R-DOUBLE (§4.7): shared ContiguousDouble stays Double - the flip is
// shape-blind, with no reboxing and no sharing-onset stop beyond the F1 fire.
// putDirectConcurrent (Task 6) dispatches its foreign-SW=0 write through it.
JS_EXPORT_PRIVATE void ensureSharedWriteBit(VM&, JSObjectWithButterfly*); // §3 foreign write (F1+R-DOUBLE+§4.8)

// §4.8 driver (review round 3): the flag-on route for BOTH the owner's
// JSObject::convertFromCopyOnWrite and any caller that must leave the CoW
// regime before proceeding. Loops the cell-locked materializer
// (nuke-CAS + DCAS, F2 fired for foreign triggers) until the object is no
// longer CopyOnWrite. The winner may be a FOREIGN thread, so on return the
// word may be a foreign-tagged flat: re-dispatch on the fresh tag before
// dereferencing flat storage as the owner. Defined in ConcurrentButterfly.cpp.
JS_EXPORT_PRIVATE void materializeCopyOnWriteButterflyConcurrent(VM&, JSObjectWithButterfly*);

// §6 dictionary growth (review round 3): raise a segmented word's out-of-line
// fragment coverage to >= neededCapacitySlots without a structure transition
// (replacement spine, fragments aliased verbatim + fresh cleared out-of-line
// fragments appended; casButterfly publication, I16/I17). Coverage is
// monotone across replacement spines. false => the word is no longer
// segmented; re-dispatch. Used by the cell-locked dictionary /
// without-transition add sites (JSObjectInlines.h) so their growth lambda
// never replaces a segmented word. Defined in ConcurrentButterfly.cpp.
JS_EXPORT_PRIVATE bool ensureSegmentedOutOfLineCapacity(VM&, JSObjectWithButterfly*, size_t neededCapacitySlots);

// ===== §10.6 stop-the-world veneer + world-stopped witness (manifest entry 6) =====
//
// Declarations only here (Task 3 consumes them from Structure.cpp's §9.4 fire
// functions and the F3/flatten-under-stop wiring); the DEFINITIONS are owned by
// runtime/ConcurrentButterfly.cpp (Task 5), per SPEC-objectmodel manifest
// entry 6:
//
//   - jsThreadsStopTheWorldAndRun: DELEGATES to
//     JSThreadsSafepoint::stopTheWorldAndRun (jit CS6 preferred option), whose
//     entry checks RELEASE_ASSERT the caller holds the API lock and that at most
//     one VM is entered (phase-1 GIL). The owned witness is raised INSIDE the
//     delegated closure (not around the call) so the delegate's worldIsStopped()
//     early-return can never be triggered by our own witness and its
//     single-mutator RELEASE_ASSERTs always execute on the outermost call
//     (adversarial-review round 1 fix). At integration manifest M4 the body
//     becomes the real VMManager STWR (+ CS2 GC-conductor bracket).
//     THREADS-INTEGRATE(objectmodel)
//   - g_jsThreadsStubWorldStopped: the pre-M4 witness; under the GIL it is
//     written unraced. SPEC-jit section 5.6 disjunct 4 reads it (see
//     JSThreadsSafepoint.cpp) once JSC_OM_PROVIDES_JSTHREADS_STUB_WITNESS is
//     defined alongside the Task 5 definition.
//   - butterflyWorldIsStopped: the predicate the §9.4 fire functions
//     RELEASE_ASSERT (I13). It is `g_jsThreadsStubWorldStopped ||
//     JSThreadsSafepoint::worldIsStopped(vm)`; at M4 integration it becomes the
//     jit predicate alone.
//
// Caller contract for the veneer (GT11): entered mutator; no §6-ranked lock
// (SAL / JSCellLock / Structure::m_lock) held; the closure must not allocate in
// the GC heap (O4 - pre-allocate and re-validate inside, RESTART on refit).

class VM;

// Advertises the pre-M4 stub witness to SPEC-jit section 5.6 disjunct 4:
// bytecode/JSThreadsSafepoint.cpp includes this header and reads
// g_jsThreadsStubWorldStopped iff this macro is defined (jit CS6). The Task 5
// veneer ALSO delegates to JSThreadsSafepoint::stopTheWorldAndRun (the CS6
// preferred option), so the disjunct is redundant-but-harmless; both are
// deleted together at M4 integration.
#define JSC_OM_PROVIDES_JSTHREADS_STUB_WITNESS 1

// std::atomic<bool>: read cross-thread by JSThreadsSafepoint::worldIsStopped()
// (disjunct 4) and by compiler/GC threads through butterflyWorldIsStopped() -
// matching the jit side's atomic depth-counter discipline (no TSAN hit; the
// implicit conversion keeps `if (g_jsThreadsStubWorldStopped)` readers valid).
extern JS_EXPORT_PRIVATE std::atomic<bool> g_jsThreadsStubWorldStopped; // defined in ConcurrentButterfly.cpp (Task 5)

JS_EXPORT_PRIVATE void jsThreadsStopTheWorldAndRun(VM&, const ScopedLambda<void()>&); // defined in ConcurrentButterfly.cpp (Task 5)
JS_EXPORT_PRIVATE bool butterflyWorldIsStopped(VM&); // defined in ConcurrentButterfly.cpp (Task 5)

// ===== §9.6 stress mode: forceSegmentedButterflies (Task 10) =====
//
// When Options::forceSegmentedButterflies() is on (manifest entry 1), every
// butterfly allocation/transition must end up publishing a SEGMENTED butterfly,
// so the spine/fragment machinery (§4) and the (notTTLTID, 1) dispatch rows run
// on single-threaded workloads. Enforcement points (all in owned files):
//
//   - trySegmentedTransition routes owner stay-flat transitions to §4.2
//     (conversion carries the trigger property) and suppresses the StayFlat
//     flavor (RESTART), and calls this helper after every successful
//     publication - covering N3 first installs, which legitimately install
//     flat first (§2.1; a spine needs a flat butterfly to alias) and are then
//     converted in place;
//   - ensureLengthSlowConcurrent / shrinkButterflyForSetLengthConcurrent route
//     their owner flat resize forms (T1 / copy-shrink) to conversion + T2;
//   - owned JSObject.cpp/JSObjectInlines.h install sites (Task 2 stamping)
//     call this helper after installing a flat butterfly flag-on.
//
// The helper: no-op unless useJSThreads + the stress option are on and the
// word is Flat/FlatShared; otherwise loops convertToSegmentedButterfly
// (in-place §4.2 form, RESTART-tolerant - step 0 may fire the TTL sets and
// return nullptr; the next pass converts) until the word is segmented (or
// left the flat regime). Exemptions, asserted at the §4.2 boundary: AS shapes
// never segment (I31) and CoW words never reach §4.2 (I35; their materialized
// replacement re-enters here). Caller contract = the §10.6 veneer's (GT11):
// entered mutator, NO §6-ranked lock held (the conversion stops and locks).
JS_EXPORT_PRIVATE void applyForceSegmentedButterfliesStressIfNeeded(VM&, JSObjectWithButterfly*);

// ===== Self-test (behind Options::verifyConcurrentButterfly, §11 Task 1) =====
//
// Pure-memory checks of the helpers above: no VM, no heap, no JSCell is
// constructed - the DCAS leg runs on a 16-byte-aligned stack buffer that
// stands in for a MarkedBlock cell's first 16 bytes. Driven by VM startup
// behind verifyConcurrentButterfly (integration manifest entry 4a) and by
// Task 12's i03-selftest.js; ConcurrentButterfly.cpp (Task 5) re-exercises it.

inline void concurrentButterflySelfTest()
{
    // --- §9.1 encode/decode round trips ---
    RELEASE_ASSERT(encodeButterfly(nullptr, 0, false) == 0);
    RELEASE_ASSERT(butterflyRegimeForWord(0) == ButterflyRegime::None);
    RELEASE_ASSERT(!isSegmentedButterfly(0));
    RELEASE_ASSERT(!untaggedButterfly(0));

    alignas(16) uint64_t fakeCell[2] = { 0, 0 };
    Butterfly* payload = reinterpret_cast<Butterfly*>(&fakeCell[0]);

    const ButterflyTID tids[] = { mainThreadButterflyTID, 1, 0x7ffe };
    for (ButterflyTID tid : tids) {
        for (bool sw : { false, true }) {
            uint64_t tagged = encodeButterfly(payload, tid, sw);
            RELEASE_ASSERT(untaggedButterfly(tagged) == payload);
            RELEASE_ASSERT(butterflyTID(tagged) == tid);
            RELEASE_ASSERT(butterflySharedWrite(tagged) == sw);
            RELEASE_ASSERT(!isSegmentedButterfly(tagged));
            RELEASE_ASSERT(butterflyRegimeForWord(tagged) == (sw ? ButterflyRegime::FlatShared : ButterflyRegime::Flat));
        }
    }

    // TID 0, SW 0 must be bit-identical to today's raw pointer (§2).
    RELEASE_ASSERT(encodeButterfly(payload, mainThreadButterflyTID, false) == static_cast<uint64_t>(reinterpret_cast<uintptr_t>(payload)));

    // --- Segmented words (§2/I3) ---
    ButterflySpine* spine = reinterpret_cast<ButterflySpine*>(&fakeCell[0]);
    uint64_t segmented = encodeSegmentedButterfly(spine);
    RELEASE_ASSERT(isSegmentedButterfly(segmented));
    RELEASE_ASSERT(butterflySharedWrite(segmented));
    RELEASE_ASSERT(butterflyTID(segmented) == notTTLTID);
    RELEASE_ASSERT(butterflySpine(segmented) == spine);
    RELEASE_ASSERT(butterflyRegimeForWord(segmented) == ButterflyRegime::Segmented);

    // --- §3.0 merge helpers ---
    constexpr uint64_t cellStateLane = 0xffULL << (8 * JSCell::cellStateOffset());
    constexpr uint64_t lockBitsLane = static_cast<uint64_t>(IndexingTypeLockIsHeld | IndexingTypeLockHasParked) << (8 * JSCell::indexingTypeAndMiscOffset());
    constexpr uint64_t typeLane = 0xffULL << (8 * JSCell::typeInfoTypeOffset());

    uint64_t expectedHeader = 0x0011223344556677ULL & ~cellHeaderVolatileMask;
    uint64_t freshVolatileOnly = expectedHeader ^ cellStateLane ^ lockBitsLane; // flips only volatile bytes
    uint64_t freshSemantic = expectedHeader ^ typeLane; // flips a semantic byte (m_type)

    RELEASE_ASSERT(headerDiffersOnlyInVolatileBits(expectedHeader, expectedHeader));
    RELEASE_ASSERT(headerDiffersOnlyInVolatileBits(expectedHeader, freshVolatileOnly));
    RELEASE_ASSERT(!headerDiffersOnlyInVolatileBits(expectedHeader, freshSemantic));

    uint64_t desiredHeader = ~expectedHeader;
    uint64_t merged = mergeVolatileHeaderBits(desiredHeader, freshVolatileOnly);
    RELEASE_ASSERT((merged & cellHeaderVolatileMask) == (freshVolatileOnly & cellHeaderVolatileMask)); // volatile bytes from fresh (I26)
    RELEASE_ASSERT((merged & ~cellHeaderVolatileMask) == (desiredHeader & ~cellHeaderVolatileMask)); // semantic bytes from desired

#if JSC_CONCURRENT_BUTTERFLY_HAS_HARDWARE_DCAS
    // --- §9.2 DCAS semantics on a fake 16B-aligned cell ---
    JSCell* cellPointer = reinterpret_cast<JSCell*>(&fakeCell[0]);
    RELEASE_ASSERT(concurrentButterflyAtomicsAreLockFree(&fakeCell[0])); // I32

    fakeCell[0] = 0x1111111111111111ULL;
    fakeCell[1] = 0x2222222222222222ULL;

    CellHeaderAndButterfly current { 0x1111111111111111ULL, 0x2222222222222222ULL };
    CellHeaderAndButterfly replacement { 0x3333333333333333ULL, 0x4444444444444444ULL };

    // Failure leaves memory untouched.
    CellHeaderAndButterfly stale { 0xdeadbeefdeadbeefULL, 0x2222222222222222ULL };
    RELEASE_ASSERT(!dcasHeaderAndButterfly(cellPointer, stale, replacement));
    RELEASE_ASSERT(fakeCell[0] == 0x1111111111111111ULL && fakeCell[1] == 0x2222222222222222ULL);

    // Success swaps both lanes atomically.
    RELEASE_ASSERT(dcasHeaderAndButterfly(cellPointer, current, replacement));
    RELEASE_ASSERT(fakeCell[0] == 0x3333333333333333ULL && fakeCell[1] == 0x4444444444444444ULL);

    // A second attempt with the old expected value must fail.
    RELEASE_ASSERT(!dcasHeaderAndButterfly(cellPointer, current, replacement));
    RELEASE_ASSERT(fakeCell[0] == 0x3333333333333333ULL && fakeCell[1] == 0x4444444444444444ULL);
#endif // JSC_CONCURRENT_BUTTERFLY_HAS_HARDWARE_DCAS
}

// Task 10 extension (defined in ConcurrentButterfly.cpp): pure-memory checks of
// the §9.6 stress predicates plus targeted witnesses for I16 (a 64-bit
// butterfly-lane CAS never touches the header lane), I4 (SW-monotone flip
// shapes), I26 (the §3.0 merge loop converges after a simulated volatile-byte
// race) and the outOfLineButterflyIndex mapping (§4.1/I8 companion).
JS_EXPORT_PRIVATE void concurrentButterflyStressSelfTest();

ALWAYS_INLINE void concurrentButterflySelfTestIfNeeded()
{
    if (verifyConcurrentButterflyEnabled()) [[unlikely]] {
        concurrentButterflySelfTest();
        concurrentButterflyStressSelfTest();
    }
}

} // namespace JSC
