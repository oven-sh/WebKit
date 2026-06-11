/*
 * Copyright (C) 2008-2026 Apple Inc. All rights reserved.
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

#include "CacheableIdentifier.h"
#include "CodeBlock.h"
#include "CodeOrigin.h"
#include "InlineCacheCompiler.h"
#include "JITStubRoutine.h"
#include "MacroAssembler.h"
#include "Options.h"
#include "PropertyInlineCacheClearingWatchpoint.h"
#include "PropertyInlineCacheSummary.h"
#include "RegisterSet.h"
#include "Structure.h"
#include <atomic>
#include <wtf/Atomics.h>
#include <wtf/Lock.h>
#include <wtf/TZoneMalloc.h>


#if ENABLE(JIT)

namespace JSC {

namespace DFG {
struct UnlinkedPropertyInlineCache;
}

class AccessCase;
class AccessGenerationResult;
class PolymorphicAccess;

enum class DisarmClearingWatchpoints : bool; // Defined in RetiredJITArtifacts.h.

#define JSC_FOR_EACH_PROPERTY_INLINE_CACHE_ACCESS_TYPE(macro) \
    macro(GetById) \
    macro(GetByIdWithThis) \
    macro(GetByIdDirect) \
    macro(TryGetById) \
    macro(GetByVal) \
    macro(GetByValWithThis) \
    macro(PutByIdStrict) \
    macro(PutByIdSloppy) \
    macro(PutByIdDirectStrict) \
    macro(PutByIdDirectSloppy) \
    macro(PutByValStrict) \
    macro(PutByValSloppy) \
    macro(PutByValDirectStrict) \
    macro(PutByValDirectSloppy) \
    macro(DefinePrivateNameByVal) \
    macro(DefinePrivateNameById) \
    macro(SetPrivateNameByVal) \
    macro(SetPrivateNameById) \
    macro(InById) \
    macro(InByVal) \
    macro(HasPrivateName) \
    macro(HasPrivateBrand) \
    macro(InstanceOf) \
    macro(DeleteByIdStrict) \
    macro(DeleteByIdSloppy) \
    macro(DeleteByValStrict) \
    macro(DeleteByValSloppy) \
    macro(GetPrivateName) \
    macro(GetPrivateNameById) \
    macro(CheckPrivateBrand) \
    macro(SetPrivateBrand) \


enum class AccessType : int8_t {
#define JSC_DEFINE_ACCESS_TYPE(name) name,
    JSC_FOR_EACH_PROPERTY_INLINE_CACHE_ACCESS_TYPE(JSC_DEFINE_ACCESS_TYPE)
#undef JSC_DEFINE_ACCESS_TYPE
};

#define JSC_INCREMENT_ACCESS_TYPE(name) + 1
static constexpr unsigned numberOfAccessTypes = 0 JSC_FOR_EACH_PROPERTY_INLINE_CACHE_ACCESS_TYPE(JSC_INCREMENT_ACCESS_TYPE);
#undef JSC_INCREMENT_ACCESS_TYPE

// This file defines two distinct inline cache (IC) dispatch strategies used across
// JSC's JIT tiers. Both strategies work the same way conceptually: each IC site is
// guarded by one or more conditions, either runtime checks (e.g. a structureID
// comparison, a property UID check) or Watchpoints on values assumed to be stable.
// When a guard passes, the IC performs the cached access; when it fails, we fall
// through to the next case or the slow path, which may add new cases.
//
// The two strategies differ in how they store and dispatch through those cases:
//
//   HandlerIC: used by Baseline JIT and DFG. The call site never modifies machine
//   code; instead it loads a pointer to the head of a singly-linked list of
//   InlineCacheHandler nodes and dispatches through them at runtime.
//
//   RepatchingIC: used by FTL only. The call site owns a fixed-size
//   slab of inline machine code embedded in the compiled function body. That slab
//   is rewritten at runtime as new cases are learned.
//
// The choice between them is a throughput vs. cost tradeoff. RepatchingIC can generate
// bespoke, case-specific code and, once sufficiently polymorphic, dispatch via a binary switch,
// which is meaningfully faster than walking a chain of indirect branches.
// However, rewriting machine code at runtime is expensive. So we only pay that
// cost in the FTL, where we have substantially more profiling and thus think the code
// is most likely to be in a steady state.

enum class PropertyInlineCacheType : uint8_t { Handler, Repatching };

// TSAN ic-stubinfo family (SPEC-jit §5.7.7 / D3): single-byte advisory IC
// state flag. These used to be `bool : 1` bitfields packed into one byte
// together with the const m_icType bit, but they are written from IC slow
// paths on ANY thread with no lock held (operation*GaveUp sets tookSlowPath,
// considerRepatchingCache* sets everConsidered/sawNonCell), so every bitfield
// write was a read-modify-write of the shared byte: it raced with itself
// (lost updates of NEIGHBORING flags) and paired with every lock-free reader
// of another bit in the byte — including isHandlerIC(), the
// "isHandlerIC vs operation*GaveUp" TSAN signature. Each flag is now its own
// relaxed atomic byte: same plain-mov codegen flag-off (semantics unchanged),
// no cross-flag lost updates (whole-byte store), and the concurrent accesses
// are defined. §5.7.7 blesses the value-level racing: every consumer treats
// these as advisory profiling hints.
//
// ACKNOWLEDGED FOOTPRINT COST (TSAN-TRIAGE §3.4 wave-2 amendment): the nine
// advisory flags + m_icType previously occupied 10 BITS (2 bytes); as one
// byte each they occupy 10 bytes, growing sizeof(PropertyInlineCache) by
// ~8 bytes per IC. These are trailing members: no OBJECT_OFFSETOF/JIT-emitted
// offset references any of them, and no sizeof-sensitive consumer exists
// in-tree, so the cost is memory only (tens of KB on IC-heavy workloads).
// Accepted for clarity; if the footprint ever matters, the nine mutable
// flags can be re-co-packed into one shared Atomic<uint8_t> accessed with
// relaxed load/store bit ops (cross-flag lost updates are §5.7.7-blessed —
// the old bitfield code already lost them); only m_icType must stay out of
// the racy byte, because it is const and read lock-free by isHandlerIC().
// TSAN ic-stubinfo ctor publication (TSAN-TRIAGE §10.4, campaign convention
// mirroring Structure.h §9.1 concurrentRelaxedLoad/Store): UNCONDITIONAL
// relaxed atomics over plain storage. A single-byte/single-word relaxed
// atomic load/store is the identical mov flag-off (no codegen change), and
// unlike std::atomic/WTF::Atomic members it lets CONSTRUCTION also be a
// relaxed atomic store: the std::atomic constructor initializes with a PLAIN
// store, which TSAN pairs against concurrent relaxed readers when an IC is
// (re)initialized in recycled memory or published without a synchronizing
// edge (the "DataOnly IC ctor publication" reports at the countdown bytes).
template<typename T>
ALWAYS_INLINE T icConcurrentRelaxedLoad(const T& field)
{
    static_assert(std::is_trivially_copyable_v<T>);
    T result;
    __atomic_load(const_cast<T*>(&field), &result, __ATOMIC_RELAXED);
    return result;
}

template<typename T>
ALWAYS_INLINE void icConcurrentRelaxedStore(T& field, std::type_identity_t<T> value)
{
    static_assert(std::is_trivially_copyable_v<T>);
    __atomic_store(&field, &value, __ATOMIC_RELAXED);
}

// Racy advisory IC state cell (SPEC-jit §5.7.7): every access — including the
// constructor's initialization — is a relaxed atomic on plain storage, so
// concurrent value-level racing is defined while flag-off codegen stays the
// plain mov the previous fields compiled to. Lost updates are tolerated by
// design (§5.7.7); JIT-emitted plain accesses via OBJECT_OFFSETOF are
// unchanged (same size, same offset; the §0 JIT-blindness tradeoff).
template<typename T>
class ICRacyCell {
public:
    ICRacyCell(T value = T())
    {
        icConcurrentRelaxedStore(m_value, value);
    }

    ALWAYS_INLINE operator T() const { return icConcurrentRelaxedLoad(m_value); }

    ALWAYS_INLINE ICRacyCell& operator=(T value)
    {
        icConcurrentRelaxedStore(m_value, value);
        return *this;
    }

    ALWAYS_INLINE T loadRelaxed() const { return icConcurrentRelaxedLoad(m_value); }
    ALWAYS_INLINE void storeRelaxed(T value) { icConcurrentRelaxedStore(m_value, value); }

private:
    T m_value;
};

using ICRacyStateBool = ICRacyCell<bool>;
static_assert(sizeof(ICRacyStateBool) == 1);
static_assert(sizeof(ICRacyCell<uint8_t>) == 1);

struct UnlinkedPropertyInlineCache;
struct BaselineUnlinkedPropertyInlineCache;

class HandlerPropertyInlineCache;
class RepatchingPropertyInlineCache;

class PropertyInlineCache {
    WTF_MAKE_NONCOPYABLE(PropertyInlineCache);
    WTF_MAKE_TZONE_ALLOCATED(PropertyInlineCache);
public:

    ~PropertyInlineCache();

    void initGetByIdSelf(const ConcurrentJSLockerBase&, CodeBlock*, Structure* inlineAccessBaseStructure, PropertyOffset);
    void NODELETE initArrayLength(const ConcurrentJSLockerBase&);
    void NODELETE initStringLength(const ConcurrentJSLockerBase&);
    void initPutByIdReplace(const ConcurrentJSLockerBase&, CodeBlock*, Structure* inlineAccessBaseStructure, PropertyOffset);
    void initInByIdSelf(const ConcurrentJSLockerBase&, CodeBlock*, Structure* inlineAccessBaseStructure, PropertyOffset);

    AccessGenerationResult addAccessCase(const GCSafeConcurrentJSLocker&, JSGlobalObject*, CodeBlock*, ECMAMode, CacheableIdentifier, RefPtr<AccessCase>);

    // AB18-E rule: the VM& is the caller's (the retiring mutator's), never
    // re-derived from a cell inside — reset() retires the displaced inlined
    // handler chain, and deriving the retire VM via codeBlock->vm()
    // (MarkedBlock header) is the exact stale-owner pattern that produced the
    // DirectCallLinkInfo::retireRecord UAF (CallLinkInfo.cpp:805).
    void reset(const ConcurrentJSLockerBase&, VM&, CodeBlock*);

    // Drops the IC's generated-dispatch state at jettison/destruction time.
    // With useJSThreads (SPEC-jit section 5.1/section 4.4, I9) the dropped
    // artifacts are routed through RetiredJITArtifacts instead of being freed
    // inline, hence the VM& (for the heap's safepoint epoch).
    // DisarmClearingWatchpoints (AB18-F amendment, thread-closeout final
    // review): pass Yes ONLY when the owner CodeBlock is dying (~CodeBlock);
    // pass No when the retired chains stay installed on a live owner
    // (jettison, post-reset), so a later watched-set fire still resets the
    // IC for straggler frames — see the enum comment in RetiredJITArtifacts.h.
    void deref(VM&, DisarmClearingWatchpoints);
    void aboutToDie();

    void NODELETE initializePredefinedRegisters();

    DECLARE_VISIT_AGGREGATE;

    // Check if the stub has weak references that are dead. If it does, then it resets itself,
    // either entirely or just enough to ensure that those dead pointers don't get used anymore.
    void visitWeak(const ConcurrentJSLockerBase&, CodeBlock*);

    // This returns true if it has marked everything that it will ever mark.
    template<typename Visitor> void propagateTransitions(Visitor&);

    PropertyInlineCacheSummary summary(const ConcurrentJSLocker&, VM&) const;

    static PropertyInlineCacheSummary summary(const ConcurrentJSLocker&, VM&, const PropertyInlineCache*);

    // TSAN ic-stubinfo ctor publication (TSAN-TRIAGE §10.4 "x identifier"):
    // m_identifier is written once at IC initialization (initializeFrom*
    // below; the IC lives inside a Baseline/DFG JITData published to racing
    // mutators via CodeBlock::m_jitData with a storeStoreFence TSAN cannot
    // see) and read lock-free from IC slow paths on any thread. Same
    // ICRacyCell discipline as m_globalObject: relaxed atomics over the
    // trivially-copyable one-word value — identical mov codegen flag-off,
    // defined under the reported cross-thread pairing. Real publication
    // ordering remains the owning CodeBlock's install protocol (F1/I5).
    // KNOWN RESIDUAL WRITER (file owned by another workstream): the plain
    // assignment in jit/JITInlineCacheGenerator.h (setUpStubInfo) still
    // pairs plain-vs-atomic; convert it to setIdentifierConcurrently() when
    // that file is in scope.
    CacheableIdentifier identifier() const { return icConcurrentRelaxedLoad(m_identifier); }
    void setIdentifierConcurrently(CacheableIdentifier value) { icConcurrentRelaxedStore(m_identifier, value); }

    bool NODELETE containsPC(void* pc) const;

    JSValueRegs valueRegs() const
    {
        return JSValueRegs(
#if USE(JSVALUE32_64)
            m_valueTagGPR,
#endif
            m_valueGPR);
    }

    JSValueRegs propertyRegs() const
    {
        return JSValueRegs(
#if USE(JSVALUE32_64)
            propertyTagGPR(),
#endif
            propertyGPR());
    }

    JSValueRegs baseRegs() const
    {
        return JSValueRegs(
#if USE(JSVALUE32_64)
            m_baseTagGPR,
#endif
            m_baseGPR);
    }

    bool thisValueIsInExtraGPR() const { return accessType == AccessType::GetByIdWithThis || accessType == AccessType::GetByValWithThis; }

    bool isHandlerIC() const { return m_icType == PropertyInlineCacheType::Handler; }

#if ASSERT_ENABLED
    void checkConsistency();
#else
    ALWAYS_INLINE void checkConsistency() { }
#endif

    CacheType cacheType() const { return m_cacheType; }

    // Not ByVal and ById case: e.g. instanceof, by-index etc.
    ALWAYS_INLINE bool considerRepatchingCacheGeneric(VM& vm, CodeBlock* codeBlock, Structure* structure)
    {
        // We never cache non-cells.
        if (!structure) {
            sawNonCell = true;
            return false;
        }
        return considerRepatchingCacheImpl(vm, codeBlock, structure, CacheableIdentifier());
    }

    ALWAYS_INLINE bool considerRepatchingCacheBy(VM& vm, CodeBlock* codeBlock, Structure* structure, CacheableIdentifier impl)
    {
        // We never cache non-cells.
        if (!structure) {
            sawNonCell = true;
            return false;
        }
        return considerRepatchingCacheImpl(vm, codeBlock, structure, impl);
    }

    ALWAYS_INLINE bool considerRepatchingCacheMegamorphic(VM& vm)
    {
        return considerRepatchingCacheImpl(vm, nullptr, nullptr, CacheableIdentifier());
    }

    Structure* inlineAccessBaseStructure() const
    {
        return m_inlineAccessBaseStructureID.get();
    }

    CallLinkInfo* callLinkInfoAt(const ConcurrentJSLocker&, unsigned index, const AccessCase&);


    Vector<AccessCase*, 16> listedAccessCases(const AbstractLocker&) const;

private:
    AccessGenerationResult upgradeForPolyProtoIfNecessary(const GCSafeConcurrentJSLocker&, VM&, CodeBlock*, const Vector<AccessCase*, 16>&, AccessCase&);

    ALWAYS_INLINE bool considerRepatchingCacheImpl(VM& vm, CodeBlock* codeBlock, Structure* structure, CacheableIdentifier impl)
    {
        AssertNoGC assertNoGC;


        // This method is called from the Optimize variants of IC slow paths. The first part of this
        // method tries to determine if the Optimize variant should really behave like the
        // non-Optimize variant and leave the IC untouched.
        //
        // If we determine that we should do something to the IC then the next order of business is
        // to determine if this Structure would impact the IC at all. We know that it won't, if we
        // have already buffered something on its behalf. That's what the m_bufferedStructures set is
        // for.

        // TSAN ic-stubinfo (SPEC-jit §5.7.7): the countdown/cool-down counters
        // below are advisory u8 profiling state mutated from IC slow paths on
        // any thread with no lock held; all accesses are relaxed atomics so
        // they are defined under concurrency, and lost updates are tolerated
        // by design. Single-threaded (and flag-off) the load/modify/store
        // sequences below are exactly the old plain-field logic.
        everConsidered = true;
        uint8_t countdownValue = countdown.loadRelaxed();
        if (!countdownValue) {
            // Check if we have been doing repatching too frequently. If so, then we should cool off
            // for a while.
            uint8_t newRepatchCount = repatchCount.loadRelaxed();
            WTF::incrementWithSaturation(newRepatchCount);
            repatchCount.storeRelaxed(newRepatchCount);
            if (newRepatchCount > Options::repatchCountForCoolDown()) {
                // We've been repatching too much, so don't do it now.
                repatchCount.storeRelaxed(0);
                // The amount of time we require for cool-down depends on the number of times we've
                // had to cool down in the past. The relationship is exponential. The max value we
                // allow here is 2^256 - 2, since the slow paths may increment the count to indicate
                // that they'd like to temporarily skip patching just this once.
                countdown.storeRelaxed(WTF::leftShiftWithSaturation(
                    static_cast<uint8_t>(Options::initialCoolDownCount()),
                    numberOfCoolDowns.loadRelaxed(),
                    static_cast<uint8_t>(std::numeric_limits<uint8_t>::max() - 1)));
                uint8_t newNumberOfCoolDowns = numberOfCoolDowns.loadRelaxed();
                WTF::incrementWithSaturation(newNumberOfCoolDowns);
                numberOfCoolDowns.storeRelaxed(newNumberOfCoolDowns);

                // We may still have had something buffered. Trigger generation now.
                bufferingCountdown.storeRelaxed(0);
                return true;
            }

            // We don't want to return false due to buffering indefinitely.
            uint8_t bufferingCountdownValue = bufferingCountdown.loadRelaxed();
            if (!bufferingCountdownValue) {
                // Note that when this returns true, it's possible that we will not even get an
                // AccessCase because this may cause Repatch.cpp to simply do an in-place
                // repatching.
                return true;
            }

            bufferingCountdown.storeRelaxed(bufferingCountdownValue - 1);

            if (!structure)
                return true;

            // Now protect the IC buffering. We want to proceed only if this is a structure that
            // we don't already have a case buffered for. Note that if this returns true but the
            // bufferingCountdown is not zero then we will buffer the access case for later without
            // immediately generating code for it.
            //
            // NOTE: This will behave oddly for InstanceOf if the user varies the prototype but not
            // the base's structure. That seems unlikely for the canonical use of instanceof, where
            // the prototype is fixed.
            bool isNewlyAdded = false;
            StructureID structureID = structure->id();
            {
                Locker locker { m_bufferedStructuresLock };
                if (std::holds_alternative<std::monostate>(m_bufferedStructures)) {
                    if (identifier())
                        m_bufferedStructures = Vector<StructureID>();
                    else
                        m_bufferedStructures = Vector<std::tuple<StructureID, CacheableIdentifier>>();
                }
                WTF::switchOn(m_bufferedStructures,
                    [&](std::monostate) { },
                    [&](Vector<StructureID>& structures) {
                        for (auto bufferedStructureID : structures) {
                            if (bufferedStructureID == structureID)
                                return;
                        }
                        structures.append(structureID);
                        isNewlyAdded = true;
                    },
                    [&](Vector<std::tuple<StructureID, CacheableIdentifier>>& structures) {
                        ASSERT(!identifier());
                        for (auto& [bufferedStructureID, bufferedCacheableIdentifier] : structures) {
                            if (bufferedStructureID == structureID && bufferedCacheableIdentifier == impl)
                                return;
                        }
                        structures.append(std::tuple { structureID, impl });
                        isNewlyAdded = true;
                    });
            }
            if (isNewlyAdded)
                vm.writeBarrier(codeBlock);
            return isNewlyAdded;
        }
        countdown.storeRelaxed(countdownValue - 1);
        return false;
    }

    void setCacheType(const ConcurrentJSLockerBase&, CacheType);

    void clearBufferedStructures()
    {
        Locker locker { m_bufferedStructuresLock };
        WTF::switchOn(m_bufferedStructures,
            [&](std::monostate) { },
            [&](Vector<StructureID>& structures) {
                structures.shrink(0);
            },
            [&](Vector<std::tuple<StructureID, CacheableIdentifier>>& structures) {
                structures.shrink(0);
            });
    }

protected:
    PropertyInlineCache(PropertyInlineCacheType icType, AccessType accessType, CodeOrigin codeOrigin)
        : codeOrigin(codeOrigin)
        // SPEC-jit section 4.2: zero-initialize the packed unit; all-zero is
        // the "no inlined fast path" state ({0, StructureID()} never matches).
        , m_packedSelfWord(0)
        , accessType(accessType)
        , bufferingCountdown { static_cast<uint8_t>(Options::initialRepatchBufferingCountdown()) }
        , m_icType(icType)
    {
        // TSAN ic-stubinfo ctor publication (§10.4): make the LAST
        // constructor-time write to m_identifier a relaxed atomic store so a
        // concurrent relaxed reader (identifier()) of a just-published or
        // recycled IC pairs atomic-vs-atomic, mirroring the ICRacyCell
        // members' rationale. The preceding implicit zero-init is overwritten
        // program-order before the IC can escape this thread.
        icConcurrentRelaxedStore(m_identifier, CacheableIdentifier());
    }

    PropertyInlineCache(PropertyInlineCacheType icType)
        : PropertyInlineCache(icType, AccessType::GetById, { })
    { }

    // AB18-G (extends the AB18-E rule structurally): every entry point that
    // can displace-and-retire a handler chain takes the VM& from the caller's
    // operation context; no retire path re-derives it via codeBlock->vm()
    // (MarkedBlock header read — the stale-owner pattern behind the
    // DirectCallLinkInfo::retireRecord UAF).
    void initializeWithUnitHandler(VM&, CodeBlock*, Ref<InlineCacheHandler>&&);
    void prependHandler(VM&, CodeBlock*, Ref<InlineCacheHandler>&&, bool isMegamorphic);
    void rewireStubAsJumpInAccess(VM&, CodeBlock*, Ref<InlineCacheHandler>&&);

public:
    static constexpr ptrdiff_t offsetOfByIdSelfOffset() { return OBJECT_OFFSETOF(PropertyInlineCache, byIdSelfOffset); }
    static constexpr ptrdiff_t offsetOfInlineAccessBaseStructureID() { return OBJECT_OFFSETOF(PropertyInlineCache, m_inlineAccessBaseStructureID); }
    static constexpr ptrdiff_t offsetOfPackedInlineAccessSelfWord() { return OBJECT_OFFSETOF(PropertyInlineCache, m_packedSelfWord); }

    // SPEC-jit section 4.2 (Task 4) accessors for the inlined fast-path unit.
    //
    // setInlineAccessSelfState: flag-off, exactly today's per-field stores
    // (WriteBarrierStructureID::set + plain offset store). Flag-on: build the
    // word -> one relaxed 64-bit store via m_packedSelfWord ->
    // vm.writeBarrier(codeBlock). Flag-on callers must be serialized as
    // today's writers are (CodeBlock::m_lock or pre-publication init).
    //
    // clearInlineAccessSelfState: flag-off = m_inlineAccessBaseStructureID
    // .clear() (byIdSelfOffset left stale, as today - it is unreachable once
    // the id half is zero). Flag-on: one all-zero 64-bit store; barrier-free.
    void setInlineAccessSelfState(VM&, CodeBlock*, Structure*, PropertyOffset);
    void clearInlineAccessSelfState();

    // The 64-bit memory image of {byIdSelfOffset = offset,
    // m_inlineAccessBaseStructureID = structureID} on this target.
    static uint64_t packedInlineAccessSelfWord(StructureID structureID, PropertyOffset offset)
    {
#if CPU(LITTLE_ENDIAN)
        return (static_cast<uint64_t>(structureID.bits()) << 32) | static_cast<uint32_t>(offset);
#else
        return (static_cast<uint64_t>(static_cast<uint32_t>(offset)) << 32) | structureID.bits();
#endif
    }
    static constexpr ptrdiff_t offsetOfInlineHolder() { return OBJECT_OFFSETOF(PropertyInlineCache, m_inlineHolder); }
    static constexpr ptrdiff_t offsetOfDoneLocation() { return OBJECT_OFFSETOF(PropertyInlineCache, doneLocation); }
    static constexpr ptrdiff_t offsetOfCountdown() { return OBJECT_OFFSETOF(PropertyInlineCache, countdown); }
    static constexpr ptrdiff_t offsetOfCallSiteIndex() { return OBJECT_OFFSETOF(PropertyInlineCache, callSiteIndex); }
    static constexpr ptrdiff_t offsetOfSlowPathStartLocation() { return OBJECT_OFFSETOF(PropertyInlineCache, slowPathStartLocation); }
    static constexpr ptrdiff_t offsetOfHandler() { return OBJECT_OFFSETOF(PropertyInlineCache, m_handler); }
    static constexpr ptrdiff_t offsetOfGlobalObject() { return OBJECT_OFFSETOF(PropertyInlineCache, m_globalObject); }

    InlineCacheHandler* firstHandler() const { return m_handler.get(); }

    JSGlobalObject* globalObject() const { return m_globalObject; }

    // AB18-G: VM& comes from the caller (AB18-E rule), see reset().
    void resetStubAsJumpInAccess(VM&, CodeBlock*);

    GPRReg thisGPR() const { return m_extraGPR; }
    GPRReg prototypeGPR() const { return m_extraGPR; }
    GPRReg brandGPR() const { return m_extraGPR; }
    GPRReg propertyGPR() const
    {
        switch (accessType) {
        case AccessType::GetByValWithThis:
            return m_extra2GPR;
        default:
            return m_extraGPR;
        }
    }

#if USE(JSVALUE32_64)
    GPRReg thisTagGPR() const { return m_extraTagGPR; }
    GPRReg prototypeTagGPR() const { return m_extraTagGPR; }
    GPRReg propertyTagGPR() const
    {
        switch (accessType) {
        case AccessType::GetByValWithThis:
            return m_extra2TagGPR;
        default:
            return m_extraTagGPR;
        }
    }
#endif

    CodeOrigin codeOrigin { };
    // SPEC-jit section 4.2 (Task 4): the inlined fast-path pair
    // {byIdSelfOffset, m_inlineAccessBaseStructureID} forms one 8-byte-aligned
    // unit. The repack is UNCONDITIONAL (D7): the per-field names keep exactly
    // their pre-repack offsets (byIdSelfOffset at +0, the structure id at +4),
    // so flag-off code - C++ and JIT emitters alike - is unchanged modulo
    // nothing (same offsets, same shapes; I1). With Options::useJSThreads()
    // the pair is accessed ONLY through m_packedSelfWord:
    //   - JIT'd readers issue ONE relaxed 64-bit load, compare the id half and
    //     use the offset half of that same load, so a valid structure id can
    //     never be observed alongside a mismatched offset (I6); compare-then-
    //     reload of independent fields is unsound on ARM64 (F2: compare/branch
    //     does not order subsequent loads).
    //   - Writers (serialized by CodeBlock::m_lock / IC initialization)
    //     publish with one 64-bit store and then issue
    //     vm.writeBarrier(codeBlock), preserving the GC write barrier the
    //     WriteBarrierStructureID::set() path provided (section 4.2).
    //   - Invalidation is one all-zero 64-bit store {0, StructureID()};
    //     structure id 0 never matches, so this is barrier-free and ABA-safe.
    // GC code (visitWeak/propagateTransitions) keeps reading the id half via
    // inlineAccessBaseStructureID/inlineAccessBaseStructure() as today.
    // m_inlineHolder stays outside the unit: holder-bearing inlined forms
    // cannot pack and are disabled flag-on (section 4.2; setInlinedHandler).
    union alignas(8) {
        struct {
            PropertyOffset byIdSelfOffset;
            WriteBarrierStructureID m_inlineAccessBaseStructureID;
        };
        std::atomic<uint64_t> m_packedSelfWord;
    };
    JSCell* m_inlineHolder { nullptr };
    CacheableIdentifier m_identifier;
    CodeLocationLabel<JSInternalPtrTag> doneLocation;
    CodeLocationLabel<JITStubRoutinePtrTag> slowPathStartLocation;

    // TSAN ic-stubinfo (TSAN-TRIAGE §10.4): written by IC initialization
    // (initializeFromUnlinkedPropertyInlineCache /
    // initializeFromDFGUnlinkedPropertyInlineCache /
    // JITInlineCacheGenerator's finalize) and read lock-free by IC slow
    // paths on any thread via globalObject(). ICRacyCell routes the
    // constructor init, every writer assignment (including the ones in
    // jit/JITInlineCacheGenerator.h, which assign through operator=), and
    // the reader through relaxed atomics: identical pointer-mov codegen,
    // defined under the cross-thread pairing TSAN reported
    // ("initializeFromDFGUnlinkedPropertyInlineCache x globalObject").
    // Real publication ordering is the storeStoreFence/install protocol of
    // the owning CodeBlock (F1/I5); JIT'd readers via offsetOfGlobalObject
    // are unchanged (same size/offset).
    ICRacyCell<JSGlobalObject*> m_globalObject { nullptr };
private:
    // Handler chain: used by both modes. Handler IC uses this as the main dispatch chain
    // (accessed from JIT via offsetOfHandler()). Repatching IC uses it in
    // rewireStubAsJumpInAccess() and initializeWithUnitHandler().
    RefPtr<InlineCacheHandler> m_handler;
    // Represents those structures that already have buffered AccessCases in the PolymorphicAccess.
    // Note that it's always safe to clear this. If we clear it prematurely, then if we see the same
    // structure again during this buffering countdown, we will create an AccessCase object for it.
    // That's not so bad - we'll get rid of the redundant ones once we regenerate.
    Variant<std::monostate, Vector<StructureID>, Vector<std::tuple<StructureID, CacheableIdentifier>>> m_bufferedStructures WTF_GUARDED_BY_LOCK(m_bufferedStructuresLock);
public:

    ScalarRegisterSet usedRegisters;

    CallSiteIndex callSiteIndex;

    // FIXME: These should only be needed by the repatching ICs but it's slightly non-trivial to move them there as different AccessTypes use different pinned registers.
    GPRReg m_baseGPR { InvalidGPRReg };
    GPRReg m_valueGPR { InvalidGPRReg };
    GPRReg m_extraGPR { InvalidGPRReg };
    GPRReg m_extra2GPR { InvalidGPRReg };
    GPRReg m_propertyCacheGPR { InvalidGPRReg };
    GPRReg m_arrayProfileGPR { InvalidGPRReg };
#if USE(JSVALUE32_64)
    GPRReg m_valueTagGPR { InvalidGPRReg };
    // FIXME: [32-bits] Check if PropertyInlineCache::m_baseTagGPR is used somewhere.
    // https://bugs.webkit.org/show_bug.cgi?id=204726
    GPRReg m_baseTagGPR { InvalidGPRReg };
    GPRReg m_extraTagGPR { InvalidGPRReg };
    GPRReg m_extra2TagGPR { InvalidGPRReg };
#endif

    AccessType accessType { AccessType::GetById };
protected:
    CacheType m_cacheType { CacheType::Unset };
public:
    CacheType preconfiguredCacheType { CacheType::Unset };
    // We repatch only when this is zero. If not zero, we decrement.
    // Setting 1 for a totally clear stub, we'll patch it after the first execution.
    //
    // TSAN ic-stubinfo (SPEC-jit §5.7.7): these four cool-down bytes are
    // advisory profiling state mutated from IC slow paths on any thread with
    // no lock held (considerRepatchingCacheImpl); they are relaxed atomics so
    // the cross-thread accesses are defined, with lost updates tolerated.
    // Same size and offset as the previous plain uint8_t fields
    // (offsetOfCountdown unchanged); JIT'd fast-path accesses to countdown
    // stay plain by design (§5.7.1 analogue: TSAN does not see JIT'd code,
    // and the value race is blessed). ICRacyCell (not WTF::Atomic) so that
    // CONSTRUCTION is also a relaxed store — the WTF::Atomic constructor
    // initializes with a plain store, which TSAN paired against concurrent
    // relaxed readers (TSAN-TRIAGE §10.4 "DataOnly IC ctor publication").
    ICRacyCell<uint8_t> countdown { 1 };
    ICRacyCell<uint8_t> repatchCount { 0 };
    ICRacyCell<uint8_t> numberOfCoolDowns { 0 };
    ICRacyCell<uint8_t> bufferingCountdown;
private:
    Lock m_bufferedStructuresLock;
public:
    // See ICRacyStateBool above: advisory flags, each its own relaxed atomic
    // byte (previously one shared bitfield byte whose RMW writes raced with
    // every reader of every other bit, m_icType included).
    ICRacyStateBool resetByGC { false };
    ICRacyStateBool tookSlowPath { false };
    ICRacyStateBool everConsidered { false };
    ICRacyStateBool prototypeIsKnownObject { false }; // Only relevant for InstanceOf.
    ICRacyStateBool sawNonCell { false };
    ICRacyStateBool propertyIsString { false };
    ICRacyStateBool propertyIsInt32 { false };
    ICRacyStateBool propertyIsSymbol { false };
    ICRacyStateBool canBeMegamorphic { false };
    // No longer a bitfield: this const discriminator used to share its byte
    // with the mutable flags above, so every racy flag write also "wrote" the
    // m_icType bit, pairing with lock-free isHandlerIC() readers in TSAN.
    // It is written once at construction and immutable afterwards.
    const PropertyInlineCacheType m_icType;
};

static_assert(sizeof(WTF::Atomic<uint8_t>) == 1);

// SPEC-jit section 4.2 layout proof (Task 4): the repacked unit is exactly one
// aligned 64-bit word and the per-field views sit at their pre-repack offsets
// (offset half at +0, structure-id half at +4 on little-endian), so flag-off
// emission and C++ accesses are unchanged (I1) and the flag-on single 64-bit
// load/store cannot tear the pair (I6/F3).
static_assert(sizeof(PropertyOffset) == 4);
static_assert(sizeof(WriteBarrierStructureID) == 4);
static_assert(sizeof(std::atomic<uint64_t>) == 8);
static_assert(alignof(std::atomic<uint64_t>) == 8);
static_assert(std::atomic<uint64_t>::is_always_lock_free);
static_assert(std::is_trivially_destructible_v<WriteBarrierStructureID>);
static_assert(PropertyInlineCache::offsetOfByIdSelfOffset() == PropertyInlineCache::offsetOfPackedInlineAccessSelfWord());
static_assert(PropertyInlineCache::offsetOfInlineAccessBaseStructureID() == PropertyInlineCache::offsetOfPackedInlineAccessSelfWord() + 4);
static_assert(!(PropertyInlineCache::offsetOfPackedInlineAccessSelfWord() % 8));

// HandlerPropertyInlineCache
// ==========================
// Implements handler-list dispatch. The call site never modifies machine code;
// instead, as new cases are learned, handler nodes are prepended to a linked list
// that the call site walks at runtime.
//
// Call site layout (Baseline / DFG JIT):
//
//     load  handlerGPR, [PropertyInlineCache + offsetOfHandler]   // head of list
//     call  [handlerGPR + InlineCacheHandler::offsetOfJumpTarget] // enter first handler
//
// Handler chain layout in memory:
//
//   PropertyInlineCache
//   +------------------+
//   | m_handler        |---> InlineCacheHandler #N  (most recently added, checked first)
//   +------------------+     +-------------------+
//                            | structureID        |
//                            | offset / uid       |
//                            | m_next             |---> InlineCacheHandler #N-1
//                            +-------------------+     +-------------------+
//                                                      | ...               |
//                                                      | m_next            |---> (slow-path handler)
//                                                      +-------------------+
//
// Most handler stubs follow a uniform pattern compiled by InlineCacheCompiler::compileHandler():
//
//     emitDataICPrologue()       // x86_64 pushes FP; ARM64E tags return address;
//                                //   other ISAs do nothing. callFrameRegister is NOT updated.
//     check guard                // e.g. do a structure check: load from base, compare against
//                                //   [handlerGPR + offsetOfStructureID]
//     --- on match ---
//     perform access             // load / store, depending on AccessCase kind
//     emitDataICEpilogue()       // minimal inverse of prologue
//     return
//
//     --- on match (JS/C++ call needed, e.g. getter/setter/custom accessor) ---
//     emitDataICPrepareForCall() // lazily save LR/FP now that we know we need a full frame
//     call into JS/C++
//     emitDataICRestoreAfterCall() // restore LR/FP
//     emitDataICEpilogue()
//     return
//
//     --- on miss ---
//     load  handlerGPR, [handlerGPR + offsetOfNext]
//     jump  [handlerGPR + offsetOfJumpTarget]            // offset skips prologue
//
// Not modifying callFrameRegister is important because it means handler stubs execute in the caller's frame
// context, so exception unwinding and CallFrame* access via callFrameRegister need no special
// handling in the handler prologue/epilogue. The lazy save/restore pattern also means that
// simple load/store handlers (the common case) pay no frame-setup cost at all.
//
// The terminal handler is always the slow-path. It calls m_slowOperation
// to fall back to the C++ runtime. The slow path may generate and prepend a new
// InlineCacheHandler to the front of the list (LIFO ordering).
//
// Cached-field fast path (m_inlinedHandler):
//
// For simple access patterns (GetByIdSelf, PutByIdReplace, InByIdSelf, etc.) that match
// preconfiguredCacheType, we also store the handler in m_inlinedHandler and write the
// structure, offset, and holder into the IC's own fields (m_inlineAccessBaseStructureID,
// byIdSelfOffset, m_inlineHolder). The JIT emits a structure check inline at the call
// site (before loading m_handler) that can succeed without touching the chain at all.
// These fields are read from memory at runtime, so no constants are embedded in the
// generated assembly.
//
// Watchpoint invalidation:
//
// Some access cases (e.g. loading a property from a prototype assumed not to change)
// attach a PropertyInlineCacheClearingWatchpoint to the relevant WatchpointSet. When
// the watchpoint fires, PropertyInlineCacheClearingWatchpoint::fireInternal() eventually calls
// resetStubAsJumpInAccess(). That function walks the entire m_handler chain calling
// removeOwner() on each node, then assigns m_handler to a freshly generated slow-path
// handler, dropping the old chain's RefPtr and potentially running every
// InlineCacheHandler destructor in the chain.
//
// N.B. A watchpoint can fire while a handler stub is mid-execution — for example, a
// getter or custom accessor calls JS, that JS mutates a prototype, and the watchpoint
// fires before the getter returns. We rely on two distinct practices to avoid
// use-after-free:
//
//   1. InlineCacheHandler struct: if the chain is reset while a stub is on the
//      stack and no other Ref holds the node, the InlineCacheHandler wrapper and its
//      trailing DataOnlyCallLinkInfo array are freed immediately. This is safe because
//      handler stubs access InlineCacheHandler fields only before making calls: the
//      structure check and the m_next load both occur on the miss path, ahead of any
//      JS call. After the call returns, the result is in a register and the stub
//      performs the epilogue and returns without touching the handler struct again.
//
//   2. Machine code: each InlineCacheHandler holds a
//      Ref<GCAwareJITStubRoutine>. GCAwareJITStubRoutine does not free the routine
//      immediately when its refcount reaches zero; it sets m_isJettisoned = true and
//      defers actual deletion until the GC confirms that the routine is no longer on
//      any call stack. So the code being executed remains valid even if m_handler is
//      cleared under us.
//
// Shared handler thunks:
//
// Many common handler shapes (e.g. getByIdLoadOwnPropertyHandler, putByIdReplaceHandler)
// are pre-compiled as shared thunks stored in VM::m_sharedJITStubs. compileHandler()
// reuses an existing shared stub before generating a new one, so multiple IC sites with
// the same access pattern share the same machine code. This sharing works because
// everything is data only, unlike with repatching ICs.
class HandlerPropertyInlineCache final : public PropertyInlineCache {
    WTF_MAKE_NONCOPYABLE(HandlerPropertyInlineCache);
public:
    HandlerPropertyInlineCache()
        : PropertyInlineCache(PropertyInlineCacheType::Handler)
    { }

    HandlerPropertyInlineCache(AccessType accessType, CodeOrigin codeOrigin)
        : PropertyInlineCache(PropertyInlineCacheType::Handler, accessType, codeOrigin)
    { }

    void initializeFromUnlinkedPropertyInlineCache(VM&, CodeBlock*, const BaselineUnlinkedPropertyInlineCache&);
    void initializeFromDFGUnlinkedPropertyInlineCache(CodeBlock*, const DFG::UnlinkedPropertyInlineCache&);

    // FTL handler-IC sites (useHandlerICInFTL): the generator recorded
    // identifier/registers/locations at compile and link time; this installs the
    // shared slow-path handler as the initial chain head. Main thread only, and
    // must run before the owning code is installed (FTL::JITFinalizer::finalize).
    void initializeHandlerForOptimizingJIT(CodeBlock*);

    void setInlinedHandler(CodeBlock*, Ref<InlineCacheHandler>&&);
    void clearInlinedHandler(CodeBlock*);

    static constexpr ptrdiff_t offsetOfSlowOperation() { return OBJECT_OFFSETOF(HandlerPropertyInlineCache, m_slowOperation); }

    CodePtr<OperationPtrTag> m_slowOperation;
    RefPtr<InlineCacheHandler> m_inlinedHandler;
};

// RepatchingPropertyInlineCache
// =============================
// Implements slab-patching dispatch (used by FTL only). The call site owns a
// fixed-size region of inline machine code embedded in the compiled function body.
// When new cases are learned, we rewrite either that slab in place, or a
// separately-allocated stub it jumps to.
//
// Inline code region layout inside the JIT-compiled function body:
//
//   startLocation --> +--------------------------------------+
//                     |  inline IC code (inlineCodeSize      |
//                     |  bytes; initially: call to slow path)|
//                     +--------------------------------------+
//   doneLocation  --> (next instruction in the function)
//
// The IC evolves through the following states:
//
//   1. [slow path]: initial state; the slab contains a call to operationXyzOptimize.
//
//   2. [inline access]: after the first hit. InlineAccess patches the slab in-place
//      with a monomorphic structure check and access (e.g. a direct load at a known
//      offset). No separate stub is needed yet.
//
//   3. [jump -> stub]: when a second structure is seen, rewireStubAsJumpInAccess()
//      overwrites the slab with a direct jump to a PolymorphicAccess stub. The stub
//      holds all accumulated AccessCases compiled together by InlineCacheCompiler::compile().
//
// The stub uses one of two dispatch strategies, chosen when the stub is (re)generated:
//
//   Cascade (linear, newest-first):
//       case N:   check guard --match--> perform access, jump to doneLocation
//                              --miss --> fall through
//       case N-1: check guard --match--> perform access, jump to doneLocation
//                              --miss --> fall through
//       ...
//       slow path
//
//   BinarySwitch (O(log n)):  used when every case is guarded solely by a structure
//       check (no proxies, no non-structure guards). A balanced binary tree of
//       structureID comparisons is emitted; each leaf performs its access and jumps
//       to doneLocation. We cannot use this form if any case involves a proxy, since
//       proxies require additional checks beyond the structureID.
//
// Because compiling a new stub is expensive, new cases are buffered in the
// PolymorphicAccess case list and the stub is only regenerated when bufferingCountdown
// reaches zero (reset to Options::repatchBufferingCountdown() after each regeneration).
// This batches multiple new cases into a single regeneration pass.
//
// Why only in FTL:
//
// Rewriting machine code at runtime incurs instruction cache flushes and, on some
// platforms, requires toggling memory write permissions. The generated stub code is
// also bespoke (built for exactly the set of cases we have seen), so every new case
// requires a full recompile of the entire stub. The payoff is tight dispatch: no
// indirect branches through a handler chain, and potentially O(log n) dispatch via
// binary switch. This tradeoff is only worthwhile in the FTL, where a function is
// hot enough to amortize the patching cost over many executions.
//
// Watchpoint invalidation:
//
// Cases that depend on stable conditions (prototype-chain stability, equivalence of a
// property value, etc.) register watchpoints on the relevant WatchpointSets. When a
// watchpoint fires, PropertyInlineCacheClearingWatchpoint::fireInternal() eventually calls
// resetStubAsJumpInAccess(). For RepatchingIC that function overwrites the inline slab
// with a jump back to the slow path and drops the reference to the PolymorphicAccess
// stub; the IC then begins accumulating cases from scratch.
//
// If a stub is mid-execution when the watchpoint fires (e.g. a polymorphic accessor
// case calls JS), the machine code is protected by GCAwareJITStubRoutine (as described above),
// this guarantees the code of the stub is still valid. No inline slabs can be at the
// first instruction when this rewrite happens either.
class RepatchingPropertyInlineCache final : public PropertyInlineCache {
    WTF_MAKE_NONCOPYABLE(RepatchingPropertyInlineCache);
public:
    RepatchingPropertyInlineCache()
        : PropertyInlineCache(PropertyInlineCacheType::Repatching)
    {
        // SPEC-jit I3: with shared-memory threads enabled, every property IC is
        // a handler IC (pure data dispatch); repatching ICs would patch machine
        // code in place under concurrent execution (sections 5.2/5.3).
        RELEASE_ASSERT(!Options::useJSThreads());
    }

    RepatchingPropertyInlineCache(AccessType accessType, CodeOrigin codeOrigin)
        : PropertyInlineCache(PropertyInlineCacheType::Repatching, accessType, codeOrigin)
    {
        // SPEC-jit I3: see above.
        RELEASE_ASSERT(!Options::useJSThreads());
    }

    // This is either the start of the inline IC for *byId caches, or the location of patchable jump for 'instanceof' caches.
    CodeLocationLabel<JITStubRoutinePtrTag> startLocation;
    CodeLocationCall<JSInternalPtrTag> m_slowPathCallLocation;
    std::unique_ptr<PolymorphicAccess> m_stub;

    uint32_t inlineCodeSize() const
    {
        int32_t inlineSize = MacroAssembler::differenceBetweenCodePtr(startLocation, doneLocation);
        ASSERT(inlineSize >= 0);
        return inlineSize;
    }
};

inline auto appropriateGetByIdOptimizeFunction(AccessType type) -> decltype(&operationGetByIdOptimize)
{
    switch (type) {
    case AccessType::GetById:
        return operationGetByIdOptimize;
    case AccessType::TryGetById:
        return operationTryGetByIdOptimize;
    case AccessType::GetByIdDirect:
        return operationGetByIdDirectOptimize;
    case AccessType::GetPrivateNameById:
        return operationGetPrivateNameByIdOptimize;
    case AccessType::GetByIdWithThis:
    default:
        ASSERT_NOT_REACHED();
        return nullptr;
    }
}

inline auto appropriateGetByIdGenericFunction(AccessType type) -> decltype(&operationGetByIdGeneric)
{
    switch (type) {
    case AccessType::GetById:
        return operationGetByIdGeneric;
    case AccessType::TryGetById:
        return operationTryGetByIdGeneric;
    case AccessType::GetByIdDirect:
        return operationGetByIdDirectGeneric;
    case AccessType::GetPrivateNameById:
        return operationGetPrivateNameByIdGeneric;
    case AccessType::GetByIdWithThis:
    default:
        ASSERT_NOT_REACHED();
        return nullptr;
    }
}

inline auto appropriatePutByIdOptimizeFunction(AccessType type) -> decltype(&operationPutByIdStrictOptimize)
{
    switch (type) {
    case AccessType::PutByIdStrict:
        return operationPutByIdStrictOptimize;
    case AccessType::PutByIdSloppy:
        return operationPutByIdSloppyOptimize;
    case AccessType::PutByIdDirectStrict:
        return operationPutByIdDirectStrictOptimize;
    case AccessType::PutByIdDirectSloppy:
        return operationPutByIdDirectSloppyOptimize;
    case AccessType::DefinePrivateNameById:
        return operationPutByIdDefinePrivateFieldStrictOptimize;
    case AccessType::SetPrivateNameById:
        return operationPutByIdSetPrivateFieldStrictOptimize;
    default:
        break;
    }
    // Make win port compiler happy
    RELEASE_ASSERT_NOT_REACHED();
    return nullptr;
}

inline bool hasConstantIdentifier(AccessType accessType)
{
    switch (accessType) {
    case AccessType::DeleteByValStrict:
    case AccessType::DeleteByValSloppy:
    case AccessType::GetByVal:
    case AccessType::GetPrivateName:
    case AccessType::InstanceOf:
    case AccessType::InByVal:
    case AccessType::HasPrivateName:
    case AccessType::HasPrivateBrand:
    case AccessType::GetByValWithThis:
    case AccessType::PutByValStrict:
    case AccessType::PutByValSloppy:
    case AccessType::PutByValDirectStrict:
    case AccessType::PutByValDirectSloppy:
    case AccessType::DefinePrivateNameByVal:
    case AccessType::SetPrivateNameByVal:
    case AccessType::SetPrivateBrand:
    case AccessType::CheckPrivateBrand:
        return false;
    case AccessType::DeleteByIdStrict:
    case AccessType::DeleteByIdSloppy:
    case AccessType::InById:
    case AccessType::TryGetById:
    case AccessType::GetByIdDirect:
    case AccessType::GetById:
    case AccessType::GetPrivateNameById:
    case AccessType::GetByIdWithThis:
    case AccessType::PutByIdStrict:
    case AccessType::PutByIdSloppy:
    case AccessType::PutByIdDirectStrict:
    case AccessType::PutByIdDirectSloppy:
    case AccessType::DefinePrivateNameById:
    case AccessType::SetPrivateNameById:
        return true;
    }
    return false;
}

struct UnlinkedPropertyInlineCache {
    AccessType accessType;
    CacheType preconfiguredCacheType { CacheType::Unset };
    bool propertyIsInt32 : 1 { false };
    bool propertyIsString : 1 { false };
    bool propertyIsSymbol : 1 { false };
    bool prototypeIsKnownObject : 1 { false };
    bool canBeMegamorphic : 1 { false };
    CacheableIdentifier m_identifier; // This only comes from already marked one. Thus, we do not mark it via GC.
    CodeLocationLabel<JSInternalPtrTag> doneLocation;
    CodeLocationLabel<JITStubRoutinePtrTag> slowPathStartLocation;
};

struct BaselineUnlinkedPropertyInlineCache : JSC::UnlinkedPropertyInlineCache {
    BytecodeIndex bytecodeIndex;
};

} // namespace JSC

SPECIALIZE_TYPE_TRAITS_BEGIN(JSC::HandlerPropertyInlineCache)
    static bool isType(const JSC::PropertyInlineCache& cache)
    {
        return cache.isHandlerIC();
    }
SPECIALIZE_TYPE_TRAITS_END()

SPECIALIZE_TYPE_TRAITS_BEGIN(JSC::RepatchingPropertyInlineCache)
    static bool isType(const JSC::PropertyInlineCache& cache)
    {
        return !cache.isHandlerIC();
    }
SPECIALIZE_TYPE_TRAITS_END()

namespace WTF {

template<typename T> struct DefaultHash;
template<> struct DefaultHash<JSC::AccessType> : public IntHash<JSC::AccessType> { };

template<typename T> struct HashTraits;
template<> struct HashTraits<JSC::AccessType> : public StrongEnumHashTraits<JSC::AccessType> { };

} // namespace WTF

#endif // ENABLE(JIT)
