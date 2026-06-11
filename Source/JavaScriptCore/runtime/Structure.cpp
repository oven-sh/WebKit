/*
 * Copyright (C) 2008-2025 Apple Inc. All rights reserved.
 * Copyright (C) 2020 Alexey Shvayka <shvaikalesh@gmail.com>.
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

#include "config.h"
#include "Structure.h"

#include "BrandedStructure.h"
#include "BuiltinNames.h"
#include "DumpContext.h"
#include "JSCInlines.h"
#include "PropertyNameArray.h"
#include "PropertyTable.h"
#include "VMLiteShared.h"
#include "WebAssemblyGCStructure.h"
#include <wtf/CommaPrinter.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/RefPtr.h>
#include <wtf/ScopedLambda.h>
#include <wtf/Vector.h>

#define DUMP_STRUCTURE_ID_STATISTICS 0

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

template<typename DetailsFunc>
void Structure::checkOffsetConsistency(PropertyTable* propertyTable, const DetailsFunc& detailsFunc) const
{
    // We cannot reliably assert things about the property table in the concurrent
    // compilation thread. It is possible for the table to be stolen and then have
    // things added to it, which leads to the offsets being all messed up. We could
    // get around this by grabbing a lock here, but I think that would be overkill.
    if (isCompilationThread())
        return;

    // SPEC-objectmodel §6 L3/L4 (same rationale as the compilation-thread
    // escape above, for N mutators): flag-on, a DICTIONARY's pinned table is
    // edited in place under {cell lock, m_lock} while its structureID stays
    // put, so callers that reach this check without those locks (the
    // transition-planning paths in this file, racing another thread's
    // cell-locked dictionary add) read a torn {maxOffset, table} pair — an
    // off-by-N here is a legal transient, not corruption. Mutation sites
    // still self-check under m_lock via Structure::add/remove's
    // checkConsistency(); only the dictionary's unlocked spot-checks are
    // skipped. NOTE (CVE-DBG-2): unlocked NON-dictionary spot-checks are
    // skipped one level up, in the no-arg checkOffsetConsistency() wrapper —
    // flag-on GIL-off, this template is now reached only from under m_lock
    // (checkConsistency) or with a thread-local table
    // (materializePropertyTable), where the dictionary condition below is the
    // only remaining legal-transient case. Flag-off: unchanged (I22).
    if (Options::useJSThreads() && isDictionary()) [[unlikely]]
        return;

    unsigned totalSize = propertyTable->propertyStorageSize();
    unsigned inlineOverflowAccordingToTotalSize = totalSize < m_inlineCapacity ? 0 : totalSize - m_inlineCapacity;

    auto fail = [&] (const char* description) {
        dataLog("Detected offset inconsistency: ", description, "!\n");
        dataLog("this = ", RawPointer(this), "\n");
        dataLog("transitionOffset = ", transitionOffset(), "\n");
        dataLog("maxOffset = ", maxOffset(), "\n");
        dataLog("m_inlineCapacity = ", m_inlineCapacity, "\n");
        dataLog("propertyTable = ", RawPointer(propertyTable), "\n");
        dataLog("numberOfSlotsForMaxOffset = ", numberOfSlotsForMaxOffset(maxOffset(), m_inlineCapacity), "\n");
        dataLog("totalSize = ", totalSize, "\n");
        dataLog("inlineOverflowAccordingToTotalSize = ", inlineOverflowAccordingToTotalSize, "\n");
        dataLog("numberOfOutOfLineSlotsForMaxOffset = ", numberOfOutOfLineSlotsForMaxOffset(maxOffset()), "\n");
        detailsFunc();
        UNREACHABLE_FOR_PLATFORM();
    };

    if (numberOfSlotsForMaxOffset(maxOffset(), m_inlineCapacity) != totalSize)
        fail("numberOfSlotsForMaxOffset doesn't match totalSize");
    if (inlineOverflowAccordingToTotalSize != numberOfOutOfLineSlotsForMaxOffset(maxOffset()))
        fail("inlineOverflowAccordingToTotalSize doesn't match numberOfOutOfLineSlotsForMaxOffset");
}

#if DUMP_STRUCTURE_ID_STATISTICS
static UncheckedKeyHashSet<Structure*>& liveStructureSet = *(new UncheckedKeyHashSet<Structure*>);
#endif

inline void StructureTransitionTable::setSingleTransition(VM& vm, JSCell* owner, Structure* structure)
{
    ASSERT(isUsingSingleSlot());
    intptr_t newData = std::bit_cast<intptr_t>(structure) | UsingSingleSlotFlag;
    if (Options::useJSThreads()) [[unlikely]] {
        // TSAN family structure-fields (UG §K publication): this is the
        // publish store of a freshly constructed transition target. Mutator
        // lookups hold m_lock flag-on (L6), but GC concurrent-marking reads
        // m_data via trySingleTransition under a DIFFERENT lock epoch and the
        // single-slot word is also read during finalization — release here
        // (plus the constructor-tail fence) orders the target's constructor
        // stores before its pointer becomes loadable. Cold path (first
        // transition install), so the arm64 stlr is acceptable; flag-off
        // keeps the plain store below, bit-identical codegen.
        WTF::atomicStore(&m_data, newData, std::memory_order_release);
    } else
        m_data = newData;
    vm.writeBarrier(owner, structure);
}

bool StructureTransitionTable::contains(PointerKey rep, unsigned attributes, TransitionKind transitionKind) const
{
    if (isUsingSingleSlot()) {
        Structure* transition = trySingleTransition();
        return transition && transition->m_transitionPropertyName == rep.pointer() && transition->transitionPropertyAttributes() == attributes && transition->transitionKind() == transitionKind;
    }
    return map()->get(StructureTransitionTable::Hash::createKey(rep, attributes, transitionKind));
}

void StructureTransitionTable::add(VM& vm, JSCell* owner, Structure* structure)
{
    // SPEC-objectmodel Task 3b (SPEC-vmstate §5.3): allocating transition-table
    // insertions (single-slot -> TransitionMap inflation, map node allocation)
    // run under the process-global structure-allocation lock. Every caller in
    // this file acquires it OUTSIDE the owning Structure's m_lock (§6 lock
    // order: SAL rank 7a < JSCellLock 10a < Structure::m_lock 10b); the lock
    // is non-recursive, so it must NOT be re-acquired here (vmstate §5.2).
    ASSERT(!Options::useStructureAllocationLock() || SharedVMState::singleton().structureAllocationRegionDepth() == 1);

    // SPEC-objectmodel L6/I37 (Task 3c): flag-on, inserts run under the owning
    // Structure's m_lock and every insert site dual-checks getMatching() under
    // that lock first (adopting a racing winner instead of inserting), so a
    // duplicate-keyed insert here would silently clobber a published
    // transition — a logic error.
    ASSERT(!Options::useJSThreads() || !getMatching(structure));

    if (isUsingSingleSlot()) {
        Structure* existingTransition = trySingleTransition();

        // This handles the first transition being added.
        if (!existingTransition) {
            setSingleTransition(vm, owner, structure);
            return;
        }

        // This handles the second transition being added
        // (or the first transition being despecified!)
        setMap(new TransitionMap(vm));
        add(vm, owner, existingTransition);
    }

    // Add the structure to the map.
    map()->set(StructureTransitionTable::Hash::createKeyFromStructure(structure), structure);
}

void Structure::dumpStatistics()
{
#if DUMP_STRUCTURE_ID_STATISTICS
    unsigned numberLeaf = 0;
    unsigned numberUsingSingleSlot = 0;
    unsigned numberSingletons = 0;
    unsigned numberWithPropertyTables = 0;
    unsigned totalPropertyTablesSize = 0;

    for (auto* structure : liveStructureSet) {
        switch (structure->m_transitionTable.size()) {
            case 0:
                ++numberLeaf;
                if (!structure->previousID())
                    ++numberSingletons;
                break;

            case 1:
                ++numberUsingSingleSlot;
                break;
        }

        if (PropertyTable* table = structure->propertyTableOrNull()) {
            ++numberWithPropertyTables;
            totalPropertyTablesSize += table->sizeInMemory();
        }
    }

    dataLogF("Number of live Structures: %d\n", liveStructureSet.size());
    dataLogF("Number of Structures using the single item optimization for transition map: %d\n", numberUsingSingleSlot);
    dataLogF("Number of Structures that are leaf nodes: %d\n", numberLeaf);
    dataLogF("Number of Structures that singletons: %d\n", numberSingletons);
    dataLogF("Number of Structures with PropertyTables: %d\n", numberWithPropertyTables);

    dataLogF("Size of a single Structures: %d\n", static_cast<unsigned>(sizeof(Structure)));
    dataLogF("Size of sum of all property maps: %d\n", totalPropertyTablesSize);
    dataLogF("Size of average of all property maps: %f\n", static_cast<double>(totalPropertyTablesSize) / static_cast<double>(liveStructureSet.size()));
#else
    dataLogF("Dumping Structure statistics is not enabled.\n");
#endif
}

#if ASSERT_ENABLED
void Structure::validateFlags()
{
    bool hasStaticPropertyTable = false;
    for (const ClassInfo* ci = classInfoForCells(); ci; ci = ci->parentClass) {
        if (ci->staticPropHashTable)
            hasStaticPropertyTable = true;
    }
    RELEASE_ASSERT(hasStaticPropertyTable == typeInfo().hasStaticPropertyTable());

    const MethodTable& methodTable = m_classInfo->methodTable;

    bool overridesGetCallData = methodTable.getCallData != JSCell::getCallData;
    RELEASE_ASSERT(overridesGetCallData == typeInfo().overridesGetCallData());

    bool overridesGetOwnPropertySlot =
        methodTable.getOwnPropertySlot != JSObject::getOwnPropertySlot
        && methodTable.getOwnPropertySlot != JSCell::getOwnPropertySlot;
    // We can strengthen this into an equivalence test if there are no classes
    // that specifies this flag without overriding getOwnPropertySlot.
    // FIXME: https://bugs.webkit.org/show_bug.cgi?id=212956
    if (overridesGetOwnPropertySlot)
        RELEASE_ASSERT(typeInfo().overridesGetOwnPropertySlot());

    bool overridesGetOwnPropertySlotByIndex =
        methodTable.getOwnPropertySlotByIndex != JSObject::getOwnPropertySlotByIndex
        && methodTable.getOwnPropertySlotByIndex != JSCell::getOwnPropertySlotByIndex;
    // We can strengthen this into an equivalence test if there are no classes
    // that specifies this flag without overriding getOwnPropertySlotByIndex.
    // FIXME: https://bugs.webkit.org/show_bug.cgi?id=212958
    if (overridesGetOwnPropertySlotByIndex)
        RELEASE_ASSERT(typeInfo().interceptsGetOwnPropertySlotByIndexEvenWhenLengthIsNotZero());

    bool overridesGetOwnPropertyNames =
        methodTable.getOwnPropertyNames != JSObject::getOwnPropertyNames
        && methodTable.getOwnPropertyNames != JSCell::getOwnPropertyNames;
    RELEASE_ASSERT(overridesGetOwnPropertyNames == typeInfo().overridesGetOwnPropertyNames());

    bool overridesGetOwnSpecialPropertyNames =
        methodTable.getOwnSpecialPropertyNames != JSObject::getOwnSpecialPropertyNames
        && methodTable.getOwnSpecialPropertyNames != JSCell::getOwnSpecialPropertyNames;
    RELEASE_ASSERT(overridesGetOwnSpecialPropertyNames == typeInfo().overridesGetOwnSpecialPropertyNames());

    bool overridesGetPrototype =
        methodTable.getPrototype != static_cast<MethodTable::GetPrototypeFunctionPtr>(JSObject::getPrototype)
        && methodTable.getPrototype != JSCell::getPrototype;
    RELEASE_ASSERT(overridesGetPrototype == typeInfo().overridesGetPrototype());

    bool overridesPut = methodTable.put != JSObject::put && ((typeInfo().type() == StringType || typeInfo().type() == SymbolType || typeInfo().type() == HeapBigIntType) || methodTable.put != JSCell::put);
    RELEASE_ASSERT(overridesPut == typeInfo().overridesPut());

    bool overridesIsExtensible =
        methodTable.isExtensible != static_cast<MethodTable::IsExtensibleFunctionPtr>(JSObject::isExtensible)
        && methodTable.isExtensible != JSCell::isExtensible;
    RELEASE_ASSERT(overridesIsExtensible == typeInfo().overridesIsExtensible());

    // MasqueradesAsUndefined requires non-null Realm.
    RELEASE_ASSERT(realm() || !typeInfo().masqueradesAsUndefined());
}
#else
inline void Structure::validateFlags() { }
#endif

Structure::Structure(VM& vm, StructureVariant variant, JSGlobalObject* globalObject, const TypeInfo& typeInfo, const ClassInfo* classInfo)
    : Structure(vm, globalObject, jsNull(), typeInfo, classInfo, NonArray, 0)
{
    // §8.9 wave 3: TSAN-relaxed (plain non-TSAN) — concurrent stale-reference
    // readers may probe variant() while this recycled cell is re-initialized.
    tsanRelaxedStore(m_structureVariant, variant);
    ASSERT(this->variant() == StructureVariant::WebAssemblyGC);

    // §10.9 fixShape (2): the delegated-to constructor's publication
    // storeStoreFence ran BEFORE the variant overwrite above, so the variant
    // store was not ordered before a subsequent single-word publish. Re-issue
    // the release so readers that reach this Structure through the publish
    // see WebAssemblyGC, never the delegate's transient Normal. Flag-off
    // codegen unchanged (predicted-not-taken branch only).
    if (Options::useJSThreads()) [[unlikely]]
        WTF::storeStoreFence();
}

Structure::Structure(VM& vm, JSGlobalObject* globalObject, JSValue prototype, const TypeInfo& typeInfo, const ClassInfo* classInfo, IndexingType indexingType, unsigned inlineCapacity)
    : JSCell(vm, vm.structureStructure.get())
    , m_blob(indexingType, typeInfo)
    , m_realm(globalObject, WriteBarrierEarlyInit)
    , m_prototype(prototype, WriteBarrierEarlyInit)
    , m_transitionWatchpointSet(IsWatched)
{
    // TSAN family structure-fields (§8.9 fixShape (2)): the scalar members are
    // initialized here with TSAN-relaxed stores (tsanRelaxedStore compiles to
    // the identical plain store non-TSAN), not in the member-init-list — a
    // member-init-list init is a plain store that races concurrent readers
    // holding stale/recycled cell references (classInfoForCells/typeInfo/...,
    // the 83 one-sided Structure::Structure keys), and clang's coalescing of
    // adjacent plain init-list stores produced wide stores overlapping the
    // m_lock byte and the watchpoint-set words (the Atomic<u8>/Atomic<u64>
    // "ctor" keys). The WriteBarrier members above are already relaxed-atomic
    // through storeCell; m_blob through TypeInfoBlob's relaxed accessors. The
    // Atomic-bearing member TYPES (m_lock, the three watchpoint sets,
    // m_transitionTable, m_seenProperties) construct through
    // ConcurrentCtorMember (Structure.h, §10.9 fixShape (1)): under TSAN
    // their storage is a deferred-construction union written with relaxed
    // atomic stores, so the std::atomic constructors' plain stores never
    // touch the member words; non-TSAN they are the plain types.
    tsanRelaxedStore(m_outOfLineTypeFlags, typeInfo.outOfLineTypeFlags());
    tsanRelaxedStore(m_inlineCapacity, static_cast<uint8_t>(inlineCapacity));
    tsanRelaxedStore(m_bitField, static_cast<uint32_t>(0));
    tsanRelaxedStore(m_transitionPropertyAttributes, static_cast<TransitionPropertyAttributes>(0));
    tsanRelaxedStore(m_structureVariant, StructureVariant::Normal);
    tsanRelaxedStore(m_transitionThreadLocalTID, static_cast<uint16_t>(0));
    tsanRelaxedStore(m_propertyHash, static_cast<uint32_t>(0));
    tsanRelaxedStore(m_classInfo, classInfo);

    // SPEC-objectmodel §5: both TTL sets start IsWatched for new structures
    // flag-on; flag-off they keep their inert NSDMI ClearWatchpoint (I22) and
    // this single predicted-not-taken check is the only flag cost.
    // N1: fresh structure - the creating thread is the sole lock-free
    // butterfly-less transitioner while the TTL sets are valid (0 flag-off and
    // on the main thread; never notTTLTID). Flag-off the field keeps the 0
    // stored above and is never consulted (I22/E3), so skip the out-of-line
    // currentButterflyTID() call (cross-DSO + TLS read) entirely.
    if (Options::useJSThreads()) [[unlikely]] {
        m_transitionThreadLocalWatchpointSet.startWatching();
        m_writeThreadLocalWatchpointSet.startWatching();
        tsanRelaxedStore(m_transitionThreadLocalTID, currentButterflyTID());
    }

    bool hasStaticNonEnumerableProperty = m_classInfo->hasStaticPropertyWithAnyOfAttributes(static_cast<uint8_t>(PropertyAttribute::DontEnum));
    bool hasStaticNonConfigurableProperty = m_classInfo->hasStaticPropertyWithAnyOfAttributes(static_cast<uint8_t>(PropertyAttribute::DontDelete));

    setDictionaryKind(NoneDictionaryKind);
    setIsPinnedPropertyTable(false);
    setHasAnyKindOfGetterSetterProperties(m_classInfo->hasStaticPropertyWithAnyOfAttributes(static_cast<uint8_t>(PropertyAttribute::AccessorOrCustomAccessorOrValue)));
    setHasReadOnlyOrGetterSetterPropertiesExcludingProto(hasAnyKindOfGetterSetterProperties() || m_classInfo->hasStaticPropertyWithAnyOfAttributes(static_cast<uint8_t>(PropertyAttribute::ReadOnly)));
    setHasNonEnumerableProperties(hasStaticNonEnumerableProperty || typeInfo.overridesGetOwnPropertySlot());
    setHasSpecialProperties(false);
    setHasNonConfigurableProperties(hasStaticNonConfigurableProperty || typeInfo.overridesGetOwnPropertySlot());
    setHasNonConfigurableReadOnlyOrGetterSetterProperties(hasStaticNonConfigurableProperty || (typeInfo.overridesGetOwnPropertySlot() && typeInfo.type() != ArrayType));
    setHasUnderscoreProtoPropertyExcludingOriginalProto(false);
    setIsQuickPropertyAccessAllowedForEnumeration(true);
    setTransitionPropertyAttributes(0);
    setTransitionKind(TransitionKind::Unknown);
    setMayBePrototype(false);
    setDidPreventExtensions(typeInfo.overridesIsExtensible());
    setDidTransition(false);
    setStaticPropertiesReified(false);
    setTransitionWatchpointIsLikelyToBeFired(false);
    setHasBeenDictionary(false);
    setProtectPropertyTableWhileTransitioning(false);
    setTransitionOffset(vm, invalidOffset);
    setMaxOffset(vm, invalidOffset);
 
    ASSERT(inlineCapacity <= JSFinalObject::maxInlineCapacity);
    ASSERT(static_cast<PropertyOffset>(inlineCapacity) < firstOutOfLineOffset);
    ASSERT(!hasRareData());
    ASSERT(hasAnyKindOfGetterSetterProperties() == m_classInfo->hasStaticPropertyWithAnyOfAttributes(static_cast<uint8_t>(PropertyAttribute::AccessorOrCustomAccessorOrValue)));
    ASSERT(hasReadOnlyOrGetterSetterPropertiesExcludingProto() == m_classInfo->hasStaticPropertyWithAnyOfAttributes(static_cast<uint8_t>(PropertyAttribute::ReadOnlyOrAccessorOrCustomAccessorOrValue)));

    validateFlags();

    ASSERT(WTF::roundUpToMultipleOf<Structure::atomSize>(this) == this);

    // TSAN family structure-fields (OM §5/§9.4, UG §K): publication release.
    // Flag-on, this Structure becomes visible to other threads through a
    // subsequent single-word publish (cell-header StructureID store,
    // transition-table setSingleTransition, global/IC slots); concurrent
    // readers then load m_classInfo/m_blob/m_realm/m_inlineCapacity without
    // this structure's m_lock. All constructor stores must be ordered before
    // any such publish store — the missing release the triage ruling calls
    // out. Gated so the flag-off path keeps today's codegen (no dmb on arm64).
    if (Options::useJSThreads()) [[unlikely]]
        WTF::storeStoreFence();
}

const ClassInfo Structure::s_info = { "Structure"_s, nullptr, nullptr, nullptr, CREATE_METHOD_TABLE(Structure) };

Structure::Structure(VM& vm, CreatingEarlyCellTag)
    : JSCell(CreatingEarlyCell)
    , m_prototype(jsNull(), WriteBarrierEarlyInit)
    , m_transitionWatchpointSet(IsWatched)
{
    // §8.9 wave 3: TSAN-relaxed scalar init instead of member-init-list plain
    // stores — see the 7-argument constructor above for the full rationale.
    tsanRelaxedStore(m_inlineCapacity, static_cast<uint8_t>(0));
    tsanRelaxedStore(m_bitField, static_cast<uint32_t>(0));
    tsanRelaxedStore(m_transitionPropertyAttributes, static_cast<TransitionPropertyAttributes>(0));
    tsanRelaxedStore(m_structureVariant, StructureVariant::Normal);
    tsanRelaxedStore(m_transitionThreadLocalTID, static_cast<uint16_t>(0));
    tsanRelaxedStore(m_propertyHash, static_cast<uint32_t>(0));
    tsanRelaxedStore(m_classInfo, static_cast<const ClassInfo*>(info()));

    // N1 (early cell: VM startup runs on the creating thread; 0 on main).
    // Flag-off: keep the 0 stored above (TID) and both TTL sets' inert
    // ClearWatchpoint (I22), skip the out-of-line TLS read. Flag-on: start
    // watching both TTL sets (§5).
    if (Options::useJSThreads()) [[unlikely]] {
        m_transitionThreadLocalWatchpointSet.startWatching();
        m_writeThreadLocalWatchpointSet.startWatching();
        tsanRelaxedStore(m_transitionThreadLocalTID, currentButterflyTID());
    }

    TypeInfo typeInfo { StructureType, StructureFlags };
    bool hasStaticNonEnumerableProperty = m_classInfo->hasStaticPropertyWithAnyOfAttributes(static_cast<uint8_t>(PropertyAttribute::DontEnum));
    bool hasStaticNonConfigurableProperty = m_classInfo->hasStaticPropertyWithAnyOfAttributes(static_cast<uint8_t>(PropertyAttribute::DontDelete));

    setDictionaryKind(NoneDictionaryKind);
    setIsPinnedPropertyTable(false);
    setHasAnyKindOfGetterSetterProperties(m_classInfo->hasStaticPropertyWithAnyOfAttributes(static_cast<uint8_t>(PropertyAttribute::AccessorOrCustomAccessorOrValue)));
    setHasReadOnlyOrGetterSetterPropertiesExcludingProto(hasAnyKindOfGetterSetterProperties() || m_classInfo->hasStaticPropertyWithAnyOfAttributes(static_cast<uint8_t>(PropertyAttribute::ReadOnly)));
    setHasNonEnumerableProperties(hasStaticNonEnumerableProperty || typeInfo.overridesGetOwnPropertySlot());
    setHasSpecialProperties(false);
    setHasNonConfigurableProperties(hasStaticNonConfigurableProperty || typeInfo.overridesGetOwnPropertySlot());
    setHasNonConfigurableReadOnlyOrGetterSetterProperties(hasStaticNonConfigurableProperty || (typeInfo.overridesGetOwnPropertySlot() && typeInfo.type() != ArrayType));
    setHasUnderscoreProtoPropertyExcludingOriginalProto(false);
    setIsQuickPropertyAccessAllowedForEnumeration(true);
    setTransitionPropertyAttributes(0);
    setTransitionKind(TransitionKind::Unknown);
    setMayBePrototype(false);
    setDidPreventExtensions(typeInfo.overridesIsExtensible());
    setDidTransition(false);
    setStaticPropertiesReified(false);
    setTransitionWatchpointIsLikelyToBeFired(false);
    setHasBeenDictionary(false);
    setProtectPropertyTableWhileTransitioning(false);
    setTransitionOffset(vm, invalidOffset);
    setMaxOffset(vm, invalidOffset);
 
    m_blob = TypeInfoBlob(0, typeInfo);
    tsanRelaxedStore(m_outOfLineTypeFlags, typeInfo.outOfLineTypeFlags());

    ASSERT(hasAnyKindOfGetterSetterProperties() == m_classInfo->hasStaticPropertyWithAnyOfAttributes(static_cast<uint8_t>(PropertyAttribute::AccessorOrCustomAccessorOrValue)));
    ASSERT(hasReadOnlyOrGetterSetterPropertiesExcludingProto() == m_classInfo->hasStaticPropertyWithAnyOfAttributes(static_cast<uint8_t>(PropertyAttribute::ReadOnlyOrAccessorOrCustomAccessorOrValue)));
    ASSERT(!this->typeInfo().overridesGetCallData() || m_classInfo->methodTable.getCallData != &JSCell::getCallData);

    ASSERT(WTF::roundUpToMultipleOf<Structure::atomSize>(this) == this);

    // Publication release — see the 7-argument constructor above.
    if (Options::useJSThreads()) [[unlikely]]
        WTF::storeStoreFence();
}

Structure::Structure(VM& vm, StructureVariant variant, Structure* previous)
    : JSCell(vm, vm.structureStructure.get())
    , m_seenProperties(previous->m_seenProperties)
    , m_prototype(previous->m_prototype.get(), WriteBarrierEarlyInit)
    , m_transitionWatchpointSet(IsWatched)
{
    // §8.9 wave 3: TSAN-relaxed scalar init instead of member-init-list plain
    // stores — see the 7-argument constructor above for the full rationale.
    // The reads of `previous`'s fields are TSAN-relaxed too: `previous` is a
    // published structure whose same words are concurrently written nowhere
    // (post-ctor immutable) but whose own construction may not be
    // TSAN-visible to this thread. m_seenProperties stays in the init list:
    // its copy reads the source word with a relaxed load, and the member's
    // own storage is written via ConcurrentCtorMember's deferred-ctor
    // relaxed stores (§10.9 fixShape (1) — the r3 70-report key was the
    // TinyBloomFilter NSDMI's std::atomic-constructor plain store, which the
    // union wrapper now skips). m_prototype's EarlyInit ctor stores through
    // the relaxed-atomic WriteBarrierBase accessors.
    tsanRelaxedStore(m_inlineCapacity, tsanRelaxedLoad(previous->m_inlineCapacity));
    tsanRelaxedStore(m_bitField, static_cast<uint32_t>(0));
    tsanRelaxedStore(m_transitionPropertyAttributes, static_cast<TransitionPropertyAttributes>(0));
    tsanRelaxedStore(m_structureVariant, variant);
    tsanRelaxedStore(m_propertyHash, tsanRelaxedLoad(previous->m_propertyHash));
    tsanRelaxedStore(m_classInfo, tsanRelaxedLoad(previous->m_classInfo));

    // SPEC-objectmodel §5: transition targets also start IsWatched flag-on; a
    // shared instance transitioning INTO this structure fires the target's sets
    // per-event (F2/§4.2-0) before publishing, so fresh-valid is sound.
    // Flag-off both sets keep their inert NSDMI ClearWatchpoint (I22).
    //
    // F4 monotonicity at creation time (GIL-ON put_by_id/delete_by_id IC
    // livelock, staging semantics/ic-{put,delete}_by_id-vs-transition.js):
    // each TTL set is born ALREADY-INVALID when the parent's same set has
    // fired. The F4 chain-fire propagates firing to every successor that
    // exists at fire time; a successor created AFTERWARDS used to be born
    // fresh-valid, which is non-monotone — and for transitions that are
    // never cached in the transition table (hasBeenDictionary() sources skip
    // both the existing-transition lookup and the m_transitionTable.add), the
    // §2 RESTART loop in putDirectInternal re-created a fresh-valid target on
    // EVERY iteration: tryStructureOnlyTransition/trySegmentedTransition step
    // 0 then saw anyTTLSetStillValid(source, target) true through the fresh
    // target alone, fired it under a full stop-the-world, returned false
    // (RESTART), and the next iteration made another fresh target — an
    // unbounded fire->RESTART livelock on a shared object whose source family
    // already has fired sets (observed: >12min inside one JS put). Inheriting
    // per-set invalidity makes the retry converge (the re-created target no
    // longer satisfies anyTTLSetStillValid) and matches what the chain-fire
    // would have produced had this structure existed when the parent fired.
    // The plain invalidating store is sound without a stop: this structure is
    // unpublished (no other thread can reference it), the sets are thin (no
    // Watchpoints installed, no compiled code can depend on them), and
    // invalidate() on a thin set neither allocates nor iterates — so the
    // constructor no-fire rule below is respected. Consumers only gate
    // optimizations on IsStillValid/IsValidAndWatched, so born-invalid merely
    // disables thread-locality elision for the new shape, exactly as F4
    // intends for a shared family. Flag-off unchanged (I22).
    if (Options::useJSThreads()) [[unlikely]] {
        if (previous->transitionThreadLocalIsStillValid()) [[likely]]
            m_transitionThreadLocalWatchpointSet.startWatching();
        else
            m_transitionThreadLocalWatchpointSet.invalidate(vm, StringFireDetail("F4: transition target created from a structure whose transitionThreadLocal set already fired"));
        if (previous->writeThreadLocalIsStillValid()) [[likely]]
            m_writeThreadLocalWatchpointSet.startWatching();
        else
            m_writeThreadLocalWatchpointSet.invalidate(vm, StringFireDetail("F4: transition target created from a structure whose writeThreadLocal set already fired"));
    }
    // N1: the structure transition TID is the CREATOR's TID, copied to targets
    // (§2.1) - the shape's butterfly-less transition ownership follows the
    // shape's creator, not whichever thread happens to reuse the shape.
    // (Unconditional: flag-off previous' TID is always 0 - a single 16-bit
    // copy, no Options load. TSAN-relaxed pair, plain non-TSAN.)
    tsanRelaxedStore(m_transitionThreadLocalTID, tsanRelaxedLoad(previous->m_transitionThreadLocalTID));

    setDictionaryKind(previous->dictionaryKind());
    setIsPinnedPropertyTable(false);
    setHasBeenFlattenedBefore(previous->hasBeenFlattenedBefore());
    setHasAnyKindOfGetterSetterProperties(previous->hasAnyKindOfGetterSetterProperties());
    setHasReadOnlyOrGetterSetterPropertiesExcludingProto(previous->hasReadOnlyOrGetterSetterPropertiesExcludingProto());
    setHasNonEnumerableProperties(previous->hasNonEnumerableProperties());
    setHasSpecialProperties(previous->hasSpecialProperties());
    setHasNonConfigurableProperties(previous->hasNonConfigurableProperties());
    setHasNonConfigurableReadOnlyOrGetterSetterProperties(previous->hasNonConfigurableReadOnlyOrGetterSetterProperties());
    setHasUnderscoreProtoPropertyExcludingOriginalProto(previous->hasUnderscoreProtoPropertyExcludingOriginalProto());
    setIsQuickPropertyAccessAllowedForEnumeration(previous->isQuickPropertyAccessAllowedForEnumeration());
    setTransitionPropertyAttributes(0);
    setTransitionKind(TransitionKind::Unknown);
    setMayBePrototype(previous->mayBePrototype());
    setDidPreventExtensions(previous->didPreventExtensions());
    setDidTransition(true);
    setStaticPropertiesReified(previous->staticPropertiesReified());
    setHasBeenDictionary(previous->hasBeenDictionary());
    setProtectPropertyTableWhileTransitioning(false);
    setTransitionOffset(vm, invalidOffset);
    setMaxOffset(vm, invalidOffset);
 
    TypeInfo typeInfo = previous->typeInfo();
    m_blob = TypeInfoBlob(previous->indexingModeIncludingHistory(), typeInfo);
    tsanRelaxedStore(m_outOfLineTypeFlags, typeInfo.outOfLineTypeFlags());

    ASSERT(!previous->typeInfo().structureIsImmortal());
    setPreviousID(vm, previous);

    // Do not fire watchpoint inside Structure constructor since watchpoint can involve further heap allocations.
    // We fire watchpoint separately in Structure::finishCreation.
    previous->didTransitionFromThisStructureWithoutFiringWatchpoint();
    
    // Copy this bit now, in case previous was being watched.
    setTransitionWatchpointIsLikelyToBeFired(previous->transitionWatchpointIsLikelyToBeFired());

    // §10.9: relaxed-atomic store (setWithoutWriteBarrier) + explicit barrier
    // instead of .set()'s plain setEarlyValue exchange — realm() readers can
    // probe this recycled cell concurrently. Identical codegen.
    if (JSGlobalObject* previousRealm = previous->m_realm.get()) {
        ASSERT(!Options::useConcurrentJIT() || !isCompilationThread()); // Same assert .set() performed.
        validateCell(previousRealm);
        m_realm.setWithoutWriteBarrier(previousRealm);
        vm.writeBarrier(this, previousRealm);
    }
    ASSERT(hasAnyKindOfGetterSetterProperties() || !m_classInfo->hasStaticPropertyWithAnyOfAttributes(static_cast<uint8_t>(PropertyAttribute::AccessorOrCustomAccessorOrValue)));
    ASSERT(hasReadOnlyOrGetterSetterPropertiesExcludingProto() || !m_classInfo->hasStaticPropertyWithAnyOfAttributes(static_cast<uint8_t>(PropertyAttribute::ReadOnlyOrAccessorOrCustomAccessorOrValue)));
    ASSERT(!this->typeInfo().overridesGetCallData() || m_classInfo->methodTable.getCallData != &JSCell::getCallData);

    ASSERT(WTF::roundUpToMultipleOf<Structure::atomSize>(this) == this);

    // Publication release — see the 7-argument constructor above.
    if (Options::useJSThreads()) [[unlikely]]
        WTF::storeStoreFence();
}

Structure::~Structure() = default;

void Structure::destroy(JSCell* cell)
{
    auto* structure = static_cast<Structure*>(cell);
    switch (structure->variant()) {
    case StructureVariant::Normal:
        structure->Structure::~Structure();
        break;
    case StructureVariant::Branded:
        static_cast<BrandedStructure*>(structure)->BrandedStructure::~BrandedStructure();
        break;
    case StructureVariant::WebAssemblyGC:
#if ENABLE(WEBASSEMBLY)
        static_cast<WebAssemblyGCStructure*>(structure)->WebAssemblyGCStructure::~WebAssemblyGCStructure();
#endif
        break;
    default:
        RELEASE_ASSERT_NOT_REACHED();
        break;
    }
}

Structure* Structure::create(PolyProtoTag, VM& vm, JSGlobalObject* globalObject, JSObject* prototype, const TypeInfo& typeInfo, const ClassInfo* classInfo, IndexingType indexingType, unsigned inlineCapacity)
{
    // Task 3b: deliberately NO StructureAllocationLocker here — the delegated
    // Structure::create below takes it internally (StructureCreateInlines.h)
    // and the SAL is non-recursive (nesting self-deadlocks; vmstate §5.2/§5.3).
    Structure* result = Structure::create(vm, globalObject, prototype, typeInfo, classInfo, indexingType, inlineCapacity);

    unsigned oldOutOfLineCapacity = result->outOfLineCapacity();
    result->addPropertyWithoutTransition(
        vm, vm.propertyNames->builtinNames().polyProtoName(), static_cast<unsigned>(PropertyAttribute::DontEnum),
        [&] (const GCSafeConcurrentJSLocker&, PropertyOffset offset, PropertyOffset newMaxOffset) {
            RELEASE_ASSERT(Structure::outOfLineCapacity(newMaxOffset) == oldOutOfLineCapacity);
            RELEASE_ASSERT(offset == knownPolyProtoOffset);
            RELEASE_ASSERT(isInlineOffset(knownPolyProtoOffset));
            result->m_prototype.setWithoutWriteBarrier(JSValue());
            result->setMaxOffset(vm, newMaxOffset);
        });

    ASSERT(result->type() == StructureType);
    return result;
}

bool Structure::isValidPrototype(JSValue prototype)
{
    return prototype.isNull() || (prototype.isObject() && prototype.getObject()->mayBePrototype());
}

bool Structure::findStructuresAndMapForMaterialization(Vector<Structure*, 8>& structures, Structure*& structure, PropertyTable*& table)
{
    ASSERT(structures.isEmpty());
    table = nullptr;

    for (structure = this; structure; structure = structure->previousID()) {
        structure->m_lock.lock();
        
        table = structure->propertyTableOrNull();
        if (table) {
            // Leave the structure locked, so that the caller can do things to it atomically
            // before it loses its property table.
            return true;
        }
        
        structures.append(structure);
        structure->m_lock.unlock();
    }
    
    ASSERT(!structure);
    ASSERT(!table);
    return false;
}

// SPEC-objectmodel L6(ii) (Task 3c): materialize is already L6-conformant and
// needs no flag gate —
// - the chain walk (findStructuresAndMapForMaterialization) inspects each
//   structure's table slot under THAT structure's m_lock, one at a time;
// - the SOURCE-table copy below runs while the found structure's m_lock is
//   still held (findStructures... returns with it locked), so it cannot race
//   a locked mutation of that published table;
// - the rebuilt table is PRIVATE until the GCSafe-locked setPropertyTable
//   publication below (mutated lock-free before that, per L6);
// - O1: the function-scope DeferGC below is the sanctioned pre-lock deferral
//   for every allocation made under m_lock here (copy, create, table->add).
// Callers must NOT hold this structure's m_lock.
PropertyTable* Structure::materializePropertyTable(VM& vm, bool setPropertyTable)
{
    ASSERT(!isCompilationThread());
    ASSERT(structure()->classInfoForCells() == info());
    ASSERT(!protectPropertyTableWhileTransitioning());

    DeferGC deferGC(vm);

    Vector<Structure*, 8> structures;
    Structure* structure;
    PropertyTable* table;
    
    bool didFindStructure = findStructuresAndMapForMaterialization(structures, structure, table);
    
    unsigned capacity = numberOfSlotsForMaxOffset(maxOffset(), m_inlineCapacity);
    if (didFindStructure) {
        table = table->copy(vm, capacity);
        structure->m_lock.unlock();
    } else
        table = PropertyTable::create(vm, capacity);
    
    // Must hold the lock on this structure, since we will be modifying this structure's
    // property map. We don't want getConcurrently() to see the property map in a half-baked
    // state.
    GCSafeConcurrentJSLocker locker(m_lock, vm);
    if (setPropertyTable)
        this->setPropertyTable(vm, table);

    for (size_t i = structures.size(); i--;) {
        structure = structures[i];
        if (!structure->m_transitionPropertyName)
            continue;
        switch (structure->transitionKind()) {
        case TransitionKind::PropertyAddition: {
            PropertyTableEntry entry(structure->m_transitionPropertyName.get(), structure->transitionOffset(), structure->transitionPropertyAttributes());
            auto nextOffset = table->nextOffset(structure->inlineCapacity());
            ASSERT_UNUSED(nextOffset, nextOffset == structure->transitionOffset());
            auto [offset, attribute, result] = table->add(vm, entry);
            ASSERT_UNUSED(result, result);
            ASSERT_UNUSED(offset, offset == nextOffset);
            UNUSED_VARIABLE(attribute);
            break;
        }
        case TransitionKind::PropertyDeletion: {
            auto [offset, attributes] = table->take(vm, structure->m_transitionPropertyName.get());
            ASSERT_UNUSED(offset, offset != invalidOffset);
            UNUSED_VARIABLE(attributes);
            table->addDeletedOffset(structure->transitionOffset());
            break;
        }
        case TransitionKind::PropertyAttributeChange: {
            PropertyOffset offset = table->updateAttributeIfExists(structure->m_transitionPropertyName.get(), structure->transitionPropertyAttributes());
            ASSERT_UNUSED(offset, offset == structure->transitionOffset());
            break;
        }
        case TransitionKind::SetBrand: {
            continue;
        }
        default:
            ASSERT_NOT_REACHED();
            break;
        }
    }
    
    checkOffsetConsistency(
        table,
        [&] () {
            dataLog("Detected in materializePropertyTable.\n");
            dataLog("Found structure = ", RawPointer(structure), "\n");
            dataLog("structures = ");
            CommaPrinter comma;
            for (Structure* structure : structures)
                dataLog(comma, RawPointer(structure));
            dataLog("\n");
        });
    
    return table;
}

bool Structure::holesMustForwardToPrototypeSlow(JSObject* base) const
{
    ASSERT(base->structure() == this);

    if (this->mayInterceptIndexedAccesses())
        return true;

    JSValue prototype = this->storedPrototype(base);
    if (!prototype.isObject())
        return false;
    JSObject* object = asObject(prototype);

    while (true) {
        Structure& structure = *object->structure();
        if (hasIndexedProperties(object->indexingType()) || structure.mayInterceptIndexedAccesses())
            return true;
        prototype = structure.storedPrototype(object);
        if (!prototype.isObject())
            return false;
        object = asObject(prototype);
    }

    RELEASE_ASSERT_NOT_REACHED();
    return false;
}

Structure* Structure::addPropertyTransition(VM& vm, Structure* structure, PropertyName propertyName, unsigned attributes, PropertyOffset& offset)
{
    Structure* newStructure = addPropertyTransitionToExistingStructure(structure, propertyName, attributes, offset);
    if (newStructure)
        return newStructure;

    return addNewPropertyTransition(vm, structure, propertyName, attributes, offset, PutPropertySlot::UnknownContext);
}

Structure* Structure::addNewPropertyTransition(VM& vm, Structure* structure, PropertyName propertyName, unsigned attributes, PropertyOffset& offset, PutPropertySlot::Context context, DeferredStructureTransitionWatchpointFire* deferred)
{
    ASSERT(!structure->isDictionary());
    ASSERT(structure->isObject());
    // SPEC-objectmodel L6/I37 (Task 3c): flag-on, the caller's existing-transition
    // lookup (addPropertyTransition / putDirectInternal) and this call are not one
    // atomic step -- a racing mutator can publish the identical (uid, attributes,
    // PropertyAddition) transition between the caller's locked miss and here. Re-check
    // under the source's m_lock and adopt the winner (its transitionOffset is
    // authoritative) instead of asserting; this also skips the doomed Structure
    // allocation. The locked dual-check before m_transitionTable.add() below remains
    // the correctness guard for the window between this recheck and the insert.
    // Note: placing the recheck before shouldDoCacheableDictionaryTransitionForAdd
    // means a loser whose context would have chosen a cacheable-dictionary transition
    // adopts the winner's PropertyAddition instead -- flag-on-only heuristic
    // divergence, intentionally accepted.
    // Flag-off: today's debug assert, bit-identical behavior (I22).
    if (Options::useJSThreads()) [[unlikely]] {
        if (Structure* existing = addPropertyTransitionToExistingStructureConcurrently(structure, propertyName.uid(), attributes, offset)) {
            existing->checkOffsetConsistency();
            return existing;
        }
    } else
        ASSERT(!Structure::addPropertyTransitionToExistingStructure(structure, propertyName, attributes, offset));

    if (structure->shouldDoCacheableDictionaryTransitionForAdd(context)) {
        ASSERT(!isCopyOnWrite(structure->indexingMode()));
        Structure* transition = toCacheableDictionaryTransition(vm, structure, deferred);
        ASSERT(structure != transition);
        offset = transition->add(vm, propertyName, attributes);
        return transition;
    }
    
    // SPEC-objectmodel Task 3b (SPEC-vmstate §5.3): the transition Structure's
    // cell allocation and the allocating transition-table insertion below run
    // under the structure-allocation lock (SAL, rank 7a), acquired OUTSIDE
    // Structure::m_lock per the §6 lock order (SAL < JSCellLock < m_lock).
    // Flag-on only:
    // - salDeferGC keeps GC triggers out of the SAL regions (heap L5 / S1:
    //   never collect or park for STW holding the SAL; O1's sanctioned
    //   pre-lock DeferGC). Structure::create(vm, previous, deferred) cannot
    //   thread the locker's GCDeferralContext into its allocateCell (its body
    //   lives in StructureInlines.h, not a [SAL] emission file) — recorded in
    //   INTEGRATE-objectmodel.md for the vmstate M7 audit.
    // - salDeferredFire guarantees the previous structure's transition
    //   watchpoints never fire inline inside Structure::create while the SAL
    //   is held: watchpoint firing may take rank-6b CodeBlock/jit locks,
    //   which are OUTER to ours and must never be acquired holding the SAL.
    // I22 latched-option pattern (see StructureInlines.h
    // addOrReplacePropertyWithoutTransition): one Config load at function
    // entry; the compiler can then prove every SAL optional below is
    // disengaged on the flag-off arm and elide the engaged-dtor checks, so
    // the flag-off body is the pre-threads body behind a single
    // predicted-false branch. (Options are frozen after init, so the latch
    // is semantics-preserving; the three loads below could not be CSE'd
    // across the opaque Structure::create / GCSafe-locker calls.)
    const bool useSAL = Options::useStructureAllocationLock();
    std::optional<DeferGC> salDeferGC;
    std::optional<DeferredStructureTransitionWatchpointFire> salDeferredFire;
    if (useSAL) [[unlikely]] {
        salDeferGC.emplace(vm);
        if (!deferred) {
            salDeferredFire.emplace(vm, structure);
            deferred = &*salDeferredFire;
        }
    }

    Structure* transition;
    {
        // I10 at the call site: the locker's flag-off no-op lives behind a
        // cross-DSO out-of-line call (VMLiteShared.cpp); gate construction on
        // the same latched option so flag-off emits no call at all. (Same
        // pattern at every StructureAllocationLocker site in this file.)
        std::optional<SharedVMState::StructureAllocationLocker> structureAllocationLocker;
        if (useSAL) [[unlikely]]
            structureAllocationLocker.emplace(vm);
        transition = Structure::create(vm, structure, deferred);
    }

    transition->m_cachedPrototypeChain.setMayBeNull(vm, transition, structure->cachedPrototypeChainConcurrently()); // Relaxed atomic read: the source chain slot is written lock-free (TSAN family structure-fields).

    // While we are adding the property, rematerializing the property table is super weird: we already
    // have a m_transitionPropertyName and transitionPropertyAttributes but the m_transitionOffset is still wrong. If the
    // materialization algorithm runs, it'll build a property table that already has the property but
    // at a bogus offset. Rather than try to teach the materialization code how to create a table under
    // those conditions, we just tell the GC not to blow the table away during this period of time.
    // Holding the lock ensures that we either do this before the GC starts scanning the structure, in
    // which case the GC will not blow the table away, or we do it after the GC already ran in which
    // case all is well.  If it wasn't for the lock, the GC would have TOCTOU: if could read
    // protectPropertyTableWhileTransitioning before we set it to true, and then blow the table away after.
    {
        ConcurrentJSLocker locker(transition->m_lock);
        transition->setProtectPropertyTableWhileTransitioning(true);
    }

    transition->m_blob.setIndexingModeIncludingHistory(structure->indexingModeIncludingHistory() & ~CopyOnWrite);
    transition->m_transitionPropertyName = propertyName.uid();
    transition->setTransitionPropertyAttributes(attributes);
    transition->setTransitionKind(TransitionKind::PropertyAddition);
    transition->setPropertyTable(vm, structure->takePropertyTableOrCloneIfPinned(vm));
    transition->setMaxOffset(vm, structure->maxOffset());

    offset = transition->add(vm, propertyName, attributes);
    transition->setTransitionOffset(vm, offset);

    // Now that everything is fine with the new structure's bookkeeping, the GC is free to blow the
    // table away if it wants. We can now rebuild it fine.
    WTF::storeStoreFence();
    transition->setProtectPropertyTableWhileTransitioning(false);

    checkOffset(transition->transitionOffset(), transition->inlineCapacity());
    if (!structure->hasBeenDictionary()) {
        // Task 3b: SAL outside m_lock (§6 order); salDeferGC above keeps the
        // GCSafe locker's deferred collection from starting under the SAL.
        std::optional<SharedVMState::StructureAllocationLocker> structureAllocationLocker;
        if (useSAL) [[unlikely]]
            structureAllocationLocker.emplace(vm);
        GCSafeConcurrentJSLocker locker(structure->m_lock, vm);
        // SPEC-objectmodel L6/I37 (Task 3c): dual-check under m_lock — a
        // racing thread may have published an identical transition between
        // our locked lookup miss and this insert. Adopt the winner: blindly
        // add()ing would clobber a Structure other instances already use
        // (lost transition). Our candidate is discarded unreferenced; if it
        // stole the source's table, the source simply rematerializes from its
        // transition chain on demand. The winner's offset is authoritative
        // (deleted-offset reuse can make the racers' offsets diverge).
        if (Options::useJSThreads()) [[unlikely]] {
            if (Structure* existing = structure->m_transitionTable.getMatching(transition)) {
                validateOffset(existing->transitionOffset(), existing->inlineCapacity());
                offset = existing->transitionOffset();
                existing->checkOffsetConsistency();
                return existing;
            }
        }
        structure->m_transitionTable.add(vm, structure, transition);
    }
    transition->checkOffsetConsistency();
    structure->checkOffsetConsistency();
    return transition;
}

Structure* Structure::removePropertyTransition(VM& vm, Structure* structure, PropertyName propertyName, PropertyOffset& offset, DeferredStructureTransitionWatchpointFire* deferred)
{
    Structure* newStructure = removePropertyTransitionFromExistingStructure(structure, propertyName, offset);
    if (newStructure)
        return newStructure;

    return removeNewPropertyTransition(
        vm, structure, propertyName, offset, deferred);
}

Structure* Structure::removePropertyTransitionFromExistingStructureImpl(Structure* structure, PropertyName propertyName, unsigned attributes, PropertyOffset& offset)
{
    ASSERT(!structure->isUncacheableDictionary());
    ASSERT(structure->isObject());

    offset = invalidOffset;

    if (structure->hasBeenDictionary())
        return nullptr;

    if (Structure* existingTransition = structure->m_transitionTable.get(propertyName.uid(), attributes, TransitionKind::PropertyDeletion)) {
        validateOffset(existingTransition->transitionOffset(), existingTransition->inlineCapacity());
        offset = existingTransition->transitionOffset();
        return existingTransition;
    }

    return nullptr;
}

Structure* Structure::removePropertyTransitionFromExistingStructure(Structure* structure, PropertyName propertyName, PropertyOffset& offset)
{
    ASSERT(!isCompilationThread());
    // SPEC-objectmodel L6(i)/I37 (Task 3c): flag-on, mutator transition-table
    // lookups hold the source's m_lock — route to the Concurrently variant.
    // Flag-off: today's lock-free lookup (I22).
    if (Options::useJSThreads()) [[unlikely]]
        return removePropertyTransitionFromExistingStructureConcurrently(structure, propertyName, offset);
    unsigned attributes = 0;
    if (structure->getConcurrently(propertyName.uid(), attributes) == invalidOffset)
        return nullptr;
    return removePropertyTransitionFromExistingStructureImpl(structure, propertyName, attributes, offset);
}

Structure* Structure::removePropertyTransitionFromExistingStructureConcurrently(Structure* structure, PropertyName propertyName, PropertyOffset& offset)
{
    unsigned attributes = 0;
    if (structure->getConcurrently(propertyName.uid(), attributes) == invalidOffset)
        return nullptr;
    ConcurrentJSLocker locker(structure->m_lock);
    return removePropertyTransitionFromExistingStructureImpl(structure, propertyName, attributes, offset);
}

Structure* Structure::addPropertyTransitionToExistingStructureConcurrently(Structure* structure, UniquedStringImpl* uid, unsigned attributes, PropertyOffset& offset)
{
    // SPEC-objectmodel L6(i)/I37 (Task 3c): the m_lock-holding lookup. Kept
    // out-of-line so the flag-off fast path in the ALWAYS_INLINE dispatcher
    // (StructureInlines.h) stays at today's code size at every put site.
    ConcurrentJSLocker locker(structure->m_lock);
    return addPropertyTransitionToExistingStructureImpl(structure, uid, attributes, offset);
}

Structure* Structure::removeNewPropertyTransition(VM& vm, Structure* structure, PropertyName propertyName, PropertyOffset& offset, DeferredStructureTransitionWatchpointFire* deferred)
{
    ASSERT(!isCompilationThread());
    ASSERT(!structure->isUncacheableDictionary());
    ASSERT(structure->isObject());
    // SPEC-objectmodel L6/I37: same TOCTOU as addNewPropertyTransition — a
    // racing mutator can publish the identical PropertyDeletion transition
    // between the caller's locked miss (removePropertyTransition line 775)
    // and here. Flag-on, re-check under the source's m_lock and adopt the
    // winner (its transitionOffset is authoritative) instead of asserting;
    // this also skips the doomed Structure allocation + table steal. The
    // locked dual-check before m_transitionTable.add() below remains the
    // guard for the window between this recheck and the insert.
    // Flag-off: today's debug assert, bit-identical behavior (I22).
    if (Options::useJSThreads()) [[unlikely]] {
        if (Structure* existing = removePropertyTransitionFromExistingStructureConcurrently(structure, propertyName, offset)) {
            existing->checkOffsetConsistency();
            return existing;
        }
    } else
        ASSERT(!Structure::removePropertyTransitionFromExistingStructure(structure, propertyName, offset));
    ASSERT(structure->getConcurrently(propertyName.uid()) != invalidOffset);

    if (structure->shouldDoCacheableDictionaryTransitionForRemoveAndAttributeChange()) {
        ASSERT(!isCopyOnWrite(structure->indexingMode()));
        Structure* transition = toUncacheableDictionaryTransition(vm, structure, deferred);
        ASSERT(structure != transition);
        offset = transition->remove(vm, propertyName);
        return transition;
    }

    // Task 3b: SAL emission — see addNewPropertyTransition for the rationale
    // (pre-lock DeferGC per O1/heap L5; deferred watchpoint fire keeps
    // rank-6b locks out of the SAL region). I22 latched-option pattern:
    // one Config load per function (see addNewPropertyTransition).
    const bool useSAL = Options::useStructureAllocationLock();
    std::optional<DeferGC> salDeferGC;
    std::optional<DeferredStructureTransitionWatchpointFire> salDeferredFire;
    if (useSAL) [[unlikely]] {
        salDeferGC.emplace(vm);
        if (!deferred) {
            salDeferredFire.emplace(vm, structure);
            deferred = &*salDeferredFire;
        }
    }

    Structure* transition;
    {
        std::optional<SharedVMState::StructureAllocationLocker> structureAllocationLocker;
        if (useSAL) [[unlikely]]
            structureAllocationLocker.emplace(vm);
        transition = Structure::create(vm, structure, deferred);
    }
    transition->m_cachedPrototypeChain.setMayBeNull(vm, transition, structure->cachedPrototypeChainConcurrently()); // Relaxed atomic read: the source chain slot is written lock-free (TSAN family structure-fields).

    // While we are deleting the property, we need to make sure the table is not cleared.
    {
        ConcurrentJSLocker locker(transition->m_lock);
        transition->setProtectPropertyTableWhileTransitioning(true);
    }

    transition->m_blob.setIndexingModeIncludingHistory(structure->indexingModeIncludingHistory() & ~CopyOnWrite);
    transition->m_transitionPropertyName = propertyName.uid();
    transition->setTransitionKind(TransitionKind::PropertyDeletion);
    transition->setPropertyTable(vm, structure->takePropertyTableOrCloneIfPinned(vm));
    transition->setMaxOffset(vm, structure->maxOffset());

    offset = transition->remove(vm, propertyName);
    ASSERT(offset != invalidOffset);
    transition->setTransitionOffset(vm, offset);

    // Now that everything is fine with the new structure's bookkeeping, the GC is free to blow the
    // table away if it wants. We can now rebuild it fine.
    WTF::storeStoreFence();
    transition->setProtectPropertyTableWhileTransitioning(false);

    checkOffset(transition->transitionOffset(), transition->inlineCapacity());
    if (!structure->hasBeenDictionary()) {
        // Task 3b: SAL outside m_lock (§6 order); salDeferGC above keeps the
        // GCSafe locker's deferred collection from starting under the SAL.
        std::optional<SharedVMState::StructureAllocationLocker> structureAllocationLocker;
        if (useSAL) [[unlikely]]
            structureAllocationLocker.emplace(vm);
        GCSafeConcurrentJSLocker locker(structure->m_lock, vm);
        // SPEC-objectmodel L6/I37 (Task 3c): dual-check under m_lock; see
        // addNewPropertyTransition. The winner's offset is authoritative.
        if (Options::useJSThreads()) [[unlikely]] {
            if (Structure* existing = structure->m_transitionTable.getMatching(transition)) {
                validateOffset(existing->transitionOffset(), existing->inlineCapacity());
                offset = existing->transitionOffset();
                existing->checkOffsetConsistency();
                return existing;
            }
        }
        structure->m_transitionTable.add(vm, structure, transition);
    }
    transition->checkOffsetConsistency();
    structure->checkOffsetConsistency();
    return transition;
}

Structure* Structure::changePrototypeTransition(VM& vm, Structure* structure, JSValue prototype, DeferredStructureTransitionWatchpointFire& deferred)
{
    ASSERT(isValidPrototype(prototype));

    DeferGC deferGC(vm);
    JSObject* key = prototype.isNull() ? nullptr : asObject(prototype);

    bool shouldChain = !structure->hasPolyProto() && structure->typeInfo().type() != GlobalObjectType && !structure->hasBeenDictionary();
    if (shouldChain) {
        ASSERT(structure->isObject());
        // SPEC-objectmodel L6(i)/I37 (Task 3c): flag-on, this mutator
        // transition-table lookup holds the source's m_lock (released at the
        // end of this block, before any allocation). Flag-off: no lock (I22).
        ConcurrentJSLocker locker(Options::useJSThreads() ? &structure->lock() : nullptr);
        if (Structure* existingTransition = structure->m_transitionTable.get(key, 0, TransitionKind::ChangePrototype)) {
            ASSERT(!existingTransition->hasPolyProto());
            existingTransition->checkOffsetConsistency();
            return existingTransition;
        }
    }

    // Changing [[Prototype]] means that we refresh this object completely.
    // This is very likely that this object will behaves differently from the previous one.
    // Let's pin the table and break the edge to the previous Structure.
    // Task 3b: SAL emission. The function-scope DeferGC above discharges
    // O1/heap L5 (no collection under the SAL), and `deferred` is always
    // non-null here, so no watchpoint fires inline within the SAL region.
    Structure* transition;
    {
        std::optional<SharedVMState::StructureAllocationLocker> structureAllocationLocker;
        if (Options::useStructureAllocationLock()) [[unlikely]]
            structureAllocationLocker.emplace(vm);
        transition = Structure::create(vm, structure, &deferred);
    }
    PropertyTable* table = structure->copyPropertyTableForPinning(vm);
    transition->pin(Locker { transition->m_lock }, vm, table);
    transition->fireTTLWatchpointSetsAfterPinning(vm, structure); // SPEC-objectmodel F3
    transition->m_prototype.set(vm, transition, prototype);
    transition->setTransitionKind(TransitionKind::ChangePrototype);
    transition->setMaxOffset(vm, structure->maxOffset());
    checkOffset(transition->transitionOffset(), transition->inlineCapacity());
    if (shouldChain) {
        // Task 3b: SAL outside m_lock (§6 order); the function-scope DeferGC
        // keeps the GCSafe locker's deferred collection out of the SAL region.
        std::optional<SharedVMState::StructureAllocationLocker> structureAllocationLocker;
        if (Options::useStructureAllocationLock()) [[unlikely]]
            structureAllocationLocker.emplace(vm);
        GCSafeConcurrentJSLocker locker(structure->m_lock, vm);
        // SPEC-objectmodel L6/I37 (Task 3c): dual-check under m_lock; see
        // addNewPropertyTransition. (Key: prototype object + ChangePrototype —
        // transition->storedPrototype() was set above, so getMatching mirrors
        // the lookup at the top of this function.)
        if (Options::useJSThreads()) [[unlikely]] {
            if (Structure* existing = structure->m_transitionTable.getMatching(transition)) {
                ASSERT(!existing->hasPolyProto());
                existing->checkOffsetConsistency();
                return existing;
            }
        }
        structure->m_transitionTable.add(vm, structure, transition);
    }

    transition->checkOffsetConsistency();
    structure->checkOffsetConsistency();
    return transition;
}

Structure* Structure::changeGlobalProxyTargetTransition(VM& vm, Structure* structure, JSGlobalObject* globalObject, DeferredStructureTransitionWatchpointFire& deferred)
{
    DeferGC deferGC(vm);
    // Task 3b: SAL emission; DeferGC above discharges O1/heap L5 and
    // `deferred` is always non-null (no inline watchpoint fire under SAL).
    Structure* transition;
    {
        std::optional<SharedVMState::StructureAllocationLocker> structureAllocationLocker;
        if (Options::useStructureAllocationLock()) [[unlikely]]
            structureAllocationLocker.emplace(vm);
        transition = Structure::create(vm, structure, &deferred);
    }

    transition->setRealm(vm, globalObject);

    PropertyTable* table = structure->copyPropertyTableForPinning(vm);
    transition->pin(Locker { transition->m_lock }, vm, table);
    transition->fireTTLWatchpointSetsAfterPinning(vm, structure); // SPEC-objectmodel F3
    transition->setMaxOffset(vm, structure->maxOffset());

    transition->checkOffsetConsistency();
    return transition;
}

Structure* Structure::attributeChangeTransitionToExistingStructureImpl(Structure* structure, PropertyName propertyName, unsigned attributes, PropertyOffset& offset)
{
    ASSERT(structure->isObject());

    offset = invalidOffset;

    if (structure->hasBeenDictionary())
        return nullptr;

    if (Structure* existingTransition = structure->m_transitionTable.get(propertyName.uid(), attributes, TransitionKind::PropertyAttributeChange)) {
        validateOffset(existingTransition->transitionOffset(), existingTransition->inlineCapacity());
        offset = existingTransition->transitionOffset();
        return existingTransition;
    }

    return nullptr;
}

Structure* Structure::attributeChangeTransitionToExistingStructure(Structure* structure, PropertyName propertyName, unsigned attributes, PropertyOffset& offset)
{
    ASSERT(!isCompilationThread());
    // SPEC-objectmodel L6(i)/I37 (Task 3c): flag-on, mutator transition-table
    // lookups hold the source's m_lock — route to the Concurrently variant.
    if (Options::useJSThreads()) [[unlikely]]
        return attributeChangeTransitionToExistingStructureConcurrently(structure, propertyName, attributes, offset);
    return attributeChangeTransitionToExistingStructureImpl(structure, propertyName, attributes, offset);
}

Structure* Structure::attributeChangeTransitionToExistingStructureConcurrently(Structure* structure, PropertyName propertyName, unsigned attributes, PropertyOffset& offset)
{
    ConcurrentJSLocker locker(structure->m_lock);
    return attributeChangeTransitionToExistingStructureImpl(structure, propertyName, attributes, offset);
}

Structure* Structure::attributeChangeTransition(VM& vm, Structure* structure, PropertyName propertyName, unsigned attributes, DeferredStructureTransitionWatchpointFire* deferred)
{
    if (structure->isUncacheableDictionary()) {
        structure->attributeChangeWithoutTransition(vm, propertyName, attributes, [](const GCSafeConcurrentJSLocker&, PropertyOffset, PropertyOffset) { });
        structure->checkOffsetConsistency();
        return structure;
    }

    ASSERT(!structure->isUncacheableDictionary());
    PropertyOffset offset = invalidOffset;
    if (Structure* existingTransition = attributeChangeTransitionToExistingStructure(structure, propertyName, attributes, offset)) {
        validateOffset(existingTransition->transitionOffset(), existingTransition->inlineCapacity());
        existingTransition->checkOffsetConsistency();
        return existingTransition;
    }

    if (structure->shouldDoCacheableDictionaryTransitionForRemoveAndAttributeChange()) {
        ASSERT(!isCopyOnWrite(structure->indexingMode()));
        Structure* transition = toUncacheableDictionaryTransition(vm, structure, deferred);
        ASSERT(structure != transition);
        transition->attributeChange(vm, propertyName, attributes);
        return transition;
    }

    // Even if the current structure is dictionary, we should perform transition since this changes attributes of existing properties to keep
    // structure still cacheable.
    // Task 3b: SAL emission — see addNewPropertyTransition for the rationale.
    // I22 latched-option pattern: one Config load per function.
    const bool useSAL = Options::useStructureAllocationLock();
    std::optional<DeferGC> salDeferGC;
    std::optional<DeferredStructureTransitionWatchpointFire> salDeferredFire;
    if (useSAL) [[unlikely]] {
        salDeferGC.emplace(vm);
        if (!deferred) {
            salDeferredFire.emplace(vm, structure);
            deferred = &*salDeferredFire;
        }
    }

    Structure* transition;
    {
        std::optional<SharedVMState::StructureAllocationLocker> structureAllocationLocker;
        if (useSAL) [[unlikely]]
            structureAllocationLocker.emplace(vm);
        transition = Structure::create(vm, structure, deferred);
    }
    transition->m_cachedPrototypeChain.setMayBeNull(vm, transition, structure->cachedPrototypeChainConcurrently()); // Relaxed atomic read: the source chain slot is written lock-free (TSAN family structure-fields).

    {
        ConcurrentJSLocker locker(transition->m_lock);
        transition->setProtectPropertyTableWhileTransitioning(true);
    }

    transition->m_blob.setIndexingModeIncludingHistory(structure->indexingModeIncludingHistory() & ~CopyOnWrite);
    transition->m_transitionPropertyName = propertyName.uid();
    transition->setTransitionPropertyAttributes(attributes);
    transition->setTransitionKind(TransitionKind::PropertyAttributeChange);
    transition->setPropertyTable(vm, structure->takePropertyTableOrCloneIfPinned(vm));
    transition->setMaxOffset(vm, structure->maxOffset());

    offset = transition->attributeChange(vm, propertyName, attributes);
    transition->setTransitionOffset(vm, offset);

    // Now that everything is fine with the new structure's bookkeeping, the GC is free to blow the
    // table away if it wants. We can now rebuild it fine.
    WTF::storeStoreFence();
    transition->setProtectPropertyTableWhileTransitioning(false);

    checkOffset(transition->transitionOffset(), transition->inlineCapacity());
    if (!structure->hasBeenDictionary()) {
        // Task 3b: SAL outside m_lock (§6 order); salDeferGC above keeps the
        // GCSafe locker's deferred collection from starting under the SAL.
        std::optional<SharedVMState::StructureAllocationLocker> structureAllocationLocker;
        if (useSAL) [[unlikely]]
            structureAllocationLocker.emplace(vm);
        GCSafeConcurrentJSLocker locker(structure->m_lock, vm);
        // SPEC-objectmodel L6/I37 (Task 3c): dual-check under m_lock; see
        // addNewPropertyTransition.
        if (Options::useJSThreads()) [[unlikely]] {
            if (Structure* existing = structure->m_transitionTable.getMatching(transition)) {
                validateOffset(existing->transitionOffset(), existing->inlineCapacity());
                existing->checkOffsetConsistency();
                return existing;
            }
        }
        structure->m_transitionTable.add(vm, structure, transition);
    }
    transition->checkOffsetConsistency();
    structure->checkOffsetConsistency();
    return transition;
}

Structure* Structure::toDictionaryTransition(VM& vm, Structure* structure, DictionaryKind kind, DeferredStructureTransitionWatchpointFire* deferred)
{
    ASSERT(!structure->isUncacheableDictionary());
    DeferGC deferGC(vm);

    // Task 3b: SAL emission. The DeferGC above discharges O1/heap L5; the
    // flag-gated deferred fire keeps rank-6b watchpoint firing out of the
    // SAL region (see addNewPropertyTransition). I22 latched-option pattern:
    // one Config load per function.
    const bool useSAL = Options::useStructureAllocationLock();
    std::optional<DeferredStructureTransitionWatchpointFire> salDeferredFire;
    if (useSAL && !deferred) [[unlikely]] {
        salDeferredFire.emplace(vm, structure);
        deferred = &*salDeferredFire;
    }

    Structure* transition;
    {
        std::optional<SharedVMState::StructureAllocationLocker> structureAllocationLocker;
        if (useSAL) [[unlikely]]
            structureAllocationLocker.emplace(vm);
        transition = Structure::create(vm, structure, deferred);
    }

    PropertyTable* table = structure->copyPropertyTableForPinning(vm);
    transition->pin(Locker { transition->m_lock }, vm, table);
    transition->fireTTLWatchpointSetsAfterPinning(vm, structure); // SPEC-objectmodel F3
    transition->setMaxOffset(vm, structure->maxOffset());
    transition->setDictionaryKind(kind);
    transition->setHasBeenDictionary(true);
    
    transition->checkOffsetConsistency();
    return transition;
}

Structure* Structure::toCacheableDictionaryTransition(VM& vm, Structure* structure, DeferredStructureTransitionWatchpointFire* deferred)
{
    return toDictionaryTransition(vm, structure, CachedDictionaryKind, deferred);
}

Structure* Structure::toUncacheableDictionaryTransition(VM& vm, Structure* structure, DeferredStructureTransitionWatchpointFire* deferred)
{
    return toDictionaryTransition(vm, structure, UncachedDictionaryKind, deferred);
}

Structure* Structure::sealTransition(VM& vm, Structure* structure, DeferredStructureTransitionWatchpointFire* deferred)
{
    return nonPropertyTransition(vm, structure, TransitionKind::Seal, deferred);
}

Structure* Structure::freezeTransition(VM& vm, Structure* structure, DeferredStructureTransitionWatchpointFire* deferred)
{
    return nonPropertyTransition(vm, structure, TransitionKind::Freeze, deferred);
}

Structure* Structure::preventExtensionsTransition(VM& vm, Structure* structure, DeferredStructureTransitionWatchpointFire* deferred)
{
    return nonPropertyTransition(vm, structure, TransitionKind::PreventExtensions, deferred);
}

Structure* Structure::becomePrototypeTransition(VM& vm, Structure* structure, DeferredStructureTransitionWatchpointFire* deferred)
{
    return nonPropertyTransition(vm, structure, TransitionKind::BecomePrototype, deferred);
}

PropertyTable* Structure::takePropertyTableOrCloneIfPinned(VM& vm)
{
    // This must always return a property table. It can't return null.

    // SPEC-objectmodel L6(ii)/I37 (Task 3c): flag-on, the STEAL and the
    // pinned-table CLONE of this PUBLISHED table both run under the SOURCE's
    // m_lock, so they cannot interleave with a locked walk or mutation of the
    // table (a lock-free copy() races a concurrent locked add/rehash and
    // tears). GCSafe locker = m_lock + DeferGC: copy() allocates a
    // PropertyTable cell under the lock — O1's sanctioned DeferGC form. The
    // stolen/cloned/materialized result is PRIVATE to the not-yet-published
    // transition; the caller may mutate it lock-free until its new Structure
    // publishes (L6). Flag-off: today's code — clone without the lock (I22).
    if (Options::useJSThreads()) [[unlikely]] {
        {
            GCSafeConcurrentJSLocker locker(m_lock, vm);
            if (PropertyTable* result = propertyTableOrNull()) {
                if (isPinnedPropertyTable())
                    return result->copy(vm, result->size() + 1);
                setPropertyTable(vm, nullptr);
                return result;
            }
        }
        // Nothing to steal: rebuild from the transition chain. O1 on
        // materialize: it opens with a function-scope DeferGC and takes each
        // chain structure's m_lock itself — never call it holding m_lock.
        bool setPropertyTable = false;
        return materializePropertyTable(vm, setPropertyTable);
    }

    PropertyTable* result = propertyTableOrNull();
    if (result) {
        if (isPinnedPropertyTable())
            return result->copy(vm, result->size() + 1);
        ConcurrentJSLocker locker(m_lock);
        setPropertyTable(vm, nullptr);
        return result;
    }
    bool setPropertyTable = false;
    return materializePropertyTable(vm, setPropertyTable);
}

Structure* Structure::nonPropertyTransitionSlow(VM& vm, Structure* structure, TransitionKind transitionKind, DeferredStructureTransitionWatchpointFire* deferred)
{
    IndexingType indexingModeIncludingHistory = newIndexingType(structure->indexingModeIncludingHistory(), transitionKind);
    
    if (!structure->isDictionary()) {
        // SPEC-objectmodel L6(i)/I37 (Task 3c): flag-on, this mutator
        // transition-table lookup holds the source's m_lock. Flag-off: no
        // lock (I22).
        ConcurrentJSLocker locker(Options::useJSThreads() ? &structure->lock() : nullptr);
        if (Structure* existingTransition = structure->m_transitionTable.get(nullptr, 0, transitionKind)) {
            ASSERT(existingTransition->transitionKind() == transitionKind);
            ASSERT(existingTransition->indexingModeIncludingHistory() == indexingModeIncludingHistory);
            return existingTransition;
        }
    }
    
    DeferGC deferGC(vm);

    // Task 3b: SAL emission. The DeferGC above discharges O1/heap L5; the
    // flag-gated deferred fire keeps rank-6b watchpoint firing out of the
    // SAL region (see addNewPropertyTransition). I22 latched-option pattern:
    // one Config load per function.
    const bool useSAL = Options::useStructureAllocationLock();
    std::optional<DeferredStructureTransitionWatchpointFire> salDeferredFire;
    if (useSAL && !deferred) [[unlikely]] {
        salDeferredFire.emplace(vm, structure);
        deferred = &*salDeferredFire;
    }

    Structure* transition;
    {
        std::optional<SharedVMState::StructureAllocationLocker> structureAllocationLocker;
        if (useSAL) [[unlikely]]
            structureAllocationLocker.emplace(vm);
        transition = Structure::create(vm, structure, deferred);
    }
    transition->setTransitionKind(transitionKind);
    transition->m_blob.setIndexingModeIncludingHistory(indexingModeIncludingHistory);

    if (changesIndexingType(transitionKind) && hasAnyArrayStorage(indexingModeIncludingHistory)) {
        transition->setHasNonEnumerableProperties(true);
        transition->setHasNonConfigurableProperties(true);
        transition->setHasNonConfigurableReadOnlyOrGetterSetterProperties(true);
    }
    
    if (preventsExtensions(transitionKind))
        transition->setDidPreventExtensions(true);

    if (transitionKind == TransitionKind::BecomePrototype)
        transition->setMayBePrototype(true);
    
    if (setsDontDeleteOnAllProperties(transitionKind) || setsReadOnlyOnNonAccessorProperties(transitionKind)) {
        // We pin the property table on transitions that do wholesale editing of the property
        // table, since our logic for walking the property transition chain to rematerialize the
        // table doesn't know how to take into account such wholesale edits.

        ASSERT(transitionKind == TransitionKind::Seal || transitionKind == TransitionKind::Freeze);

        PropertyTable* table = structure->copyPropertyTableForPinning(vm);
        transition->pinForCaching(Locker { transition->m_lock }, vm, table);
        transition->fireTTLWatchpointSetsAfterPinning(vm, structure); // SPEC-objectmodel F3
        transition->setMaxOffset(vm, structure->maxOffset());

        table = transition->propertyTableOrNull();
        RELEASE_ASSERT(table);
        if (transitionKind == TransitionKind::Seal)
            table->seal();
        else
            table->freeze();

        transition->setHasNonEnumerableProperties(true);
        transition->setHasNonConfigurableProperties(true);
        transition->setHasNonConfigurableReadOnlyOrGetterSetterProperties(true);
    } else {
        transition->setPropertyTable(vm, structure->takePropertyTableOrCloneIfPinned(vm));
        transition->setMaxOffset(vm, structure->maxOffset());
        checkOffset(transition->maxOffset(), transition->inlineCapacity());
    }
    
    if (setsReadOnlyOnNonAccessorProperties(transitionKind)
        && !transition->propertyTableOrNull()->isEmpty())
        transition->setHasReadOnlyOrGetterSetterPropertiesExcludingProto(true);
    
    if (structure->isDictionary()) {
        PropertyTable* table = transition->ensurePropertyTable(vm);
        transition->pin(Locker { transition->m_lock }, vm, table);
        transition->fireTTLWatchpointSetsAfterPinning(vm, structure); // SPEC-objectmodel F3
    } else {
        // Task 3b: SAL outside m_lock (§6 order); the function-scope GC
        // deferral above keeps any GC trigger out of the SAL region.
        std::optional<SharedVMState::StructureAllocationLocker> structureAllocationLocker;
        if (Options::useStructureAllocationLock()) [[unlikely]]
            structureAllocationLocker.emplace(vm);
        Locker locker { structure->m_lock };
        // SPEC-objectmodel L6/I37 (Task 3c): dual-check under m_lock; see
        // addNewPropertyTransition. (Key: null pointer + transitionKind.)
        if (Options::useJSThreads()) [[unlikely]] {
            if (Structure* existing = structure->m_transitionTable.getMatching(transition)) {
                ASSERT(existing->transitionKind() == transitionKind);
                ASSERT(existing->indexingModeIncludingHistory() == indexingModeIncludingHistory);
                existing->checkOffsetConsistency();
                return existing;
            }
        }
        structure->m_transitionTable.add(vm, structure, transition);
    }

    transition->checkOffsetConsistency();
    return transition;
}

// In future we may want to cache this property.
bool Structure::isSealed(VM& vm)
{
    if (isStructureExtensible())
        return false;

    // SPEC-objectmodel L6(iii) (Task 3c): flag-on, this uncached table WALK
    // (isSealed iterates every entry's attributes) holds m_lock; the loop
    // retries if a racing transition stole the table between materialization
    // and lock acquisition. Flag-off: today's lock-free walk (I22).
    if (Options::useJSThreads()) [[unlikely]] {
        while (true) {
            PropertyTable* table = ensurePropertyTableIfNotEmpty(vm);
            if (!table)
                return true;
            GCSafeConcurrentJSLocker locker(m_lock, vm);
            if (propertyTableOrNull() == table)
                return table->isSealed();
        }
    }

    PropertyTable* table = ensurePropertyTableIfNotEmpty(vm);
    if (!table)
        return true;
    return table->isSealed();
}

// In future we may want to cache this property.
bool Structure::isFrozen(VM& vm)
{
    if (isStructureExtensible())
        return false;

    // SPEC-objectmodel L6(iii) (Task 3c): see isSealed above.
    if (Options::useJSThreads()) [[unlikely]] {
        while (true) {
            PropertyTable* table = ensurePropertyTableIfNotEmpty(vm);
            if (!table)
                return true;
            GCSafeConcurrentJSLocker locker(m_lock, vm);
            if (propertyTableOrNull() == table)
                return table->isFrozen();
        }
    }

    PropertyTable* table = ensurePropertyTableIfNotEmpty(vm);
    if (!table)
        return true;
    return table->isFrozen();
}

Structure* Structure::flattenDictionaryStructure(VM& vm, JSObject* object)
{
    ASSERT(!isCompilationThread());
    checkOffsetConsistency();
    ASSERT(isDictionary());
    ASSERT(object->structure() == this);

    // SPEC-objectmodel F3 flatten-under-stop (Task 3; review round 2): flatten
    // rearranges out-of-line storage in place (renumbered offsets, zeroed
    // tails, possibly setButterfly(nullptr)/memmove), which is unsound against
    // racing accessors. Flag-on it ALWAYS runs per-event under the §10.6
    // veneer. The previous "unshared" fast path (both TTL sets valid, word
    // not SW/segmented) was unsound: foreign READS fire no watchpoint and
    // never flip SW, so an object other threads only READ always classified
    // unshared - yet the flag-on dictionary read path is lock-free
    // (getDirectConcurrent / locationForOutOfLineOffsetConcurrent take no
    // cell lock for non-AS shapes), so a foreign reader holding a
    // just-resolved offset could read a zeroed/moved slot, or chase a
    // nulled-out butterfly and crash on a pure read race. Read-only foreign
    // sharing is UNDETECTABLE, so the only sound flag-on choice is the stop;
    // flatten is rare and already expensive, and genuinely owner-local
    // objects keep their TTL sets (the firing below stays conditional).
    if (Options::useJSThreads()) [[unlikely]]
        return flattenDictionaryStructureUnderStop(vm, object);

    // Holds our values compacted by insertion order. Pre-sized here so the impl
    // never allocates it under the stop closure on the shared path (O4); on
    // this unshared path the placement is just shared code.
    Vector<JSValue> values;
    if (isUncacheableDictionary()) {
        PropertyTable* table = propertyTableOrNull();
        ASSERT(table);
        values.grow(table->size());
    }
    Structure* result = flattenDictionaryStructureImpl(vm, object, values);
    // Flag-off the impl never bails (its revalidation is flag-on gated), and
    // flag-on this point is unreachable (routed above).
    RELEASE_ASSERT(result);
    return result;
}

bool Structure::flattenTriggerIsShared(JSObject* object) const
{
    ASSERT(Options::useJSThreads());
    // Shared <=> either TTL set has fired (some instance went notTTLTID or
    // SW=1 - I11/I12), or this object's own butterfly word is shared-written or
    // segmented. The word checks are belt-and-braces: F1/F2 fire the sets
    // before any SW/segmented publication (I10b), so invalid sets subsume them.
    //
    // Review round 2: this predicate is NO LONGER a stop-elision gate (it
    // cannot see read-only foreign sharing, which fires nothing - see
    // flattenDictionaryStructure). It only decides whether F3 must FIRE the
    // TTL sets inside the (now unconditional) flatten stop.
    if (!transitionThreadLocalIsStillValid() || !writeThreadLocalIsStillValid())
        return true;
    uint64_t word = object->taggedButterflyWord();
    return butterflySharedWrite(word) || isSegmentedButterfly(word);
}

Structure* Structure::flattenDictionaryStructureUnderStop(VM& vm, JSObject* object)
{
    ASSERT(Options::useJSThreads());

    // O4: stop-window closures never allocate - the scratch buffer is
    // pre-allocated out here and re-validated inside; a refit (racing
    // cell-locked dictionary edit between pre-allocation and stop entry, L3/L4)
    // RESTARTs the whole operation.
    while (true) {
        Vector<JSValue> values;
        if (isUncacheableDictionary()) {
            PropertyTable* table = propertyTableOrNull();
            ASSERT(table);
            values.grow(table->size());
        }

        Structure* result = nullptr;
        bool needsRefit = false;
        // GT11 caller contract honored: we hold no §6-ranked lock and no cell
        // lock here; the impl acquires JSCellLock then m_lock INSIDE the stop
        // (lock spans are bounded and never held across a safepoint - O2 - so
        // no stopped mutator can be parked while holding them).
        jsThreadsStopTheWorldAndRun(vm, scopedLambda<void()>([&] {
            if (isUncacheableDictionary()) {
                PropertyTable* table = propertyTableOrNull();
                ASSERT(table);
                if (table->size() != values.size()) {
                    needsRefit = true;
                    return;
                }
            }
            // Evaluate the trigger's sharedness BEFORE the impl runs (the impl
            // may null out the butterfly word; the TTL-set clauses are
            // monotone either way).
            bool triggerIsShared = flattenTriggerIsShared(object);
            result = flattenDictionaryStructureImpl(vm, object, values);

            // F3: fire both sets on the result (== this for flatten), in the
            // SAME stop, when the TRIGGER is shared. Review round 2: ALL
            // flag-on flattens now come through this stop, so reaching here
            // no longer implies sharedness - genuinely owner-local objects
            // (sets valid, word unshared) keep their sets; firing for them
            // would needlessly push whole shape families off the flat fast
            // paths. The STOP itself - not the firing - is what protects the
            // in-place compaction against lock-free readers (read-only
            // foreign sharing is undetectable, so the stop is unconditional).
            if (triggerIsShared
                && (m_transitionThreadLocalWatchpointSet.isStillValid() || m_writeThreadLocalWatchpointSet.isStillValid()))
                fireTransitionThreadLocal(vm, "F3: flattenDictionaryStructure on a shared structure/object");
        }));
        if (!needsRefit) {
            // The impl's under-lock revalidation bails (nullptr) only when it
            // runs OUTSIDE a stop; inside this stop butterflyWorldIsStopped()
            // holds, so it must have completed.
            RELEASE_ASSERT(result);
            return result;
        }
    }
}

Structure* Structure::flattenDictionaryStructureImpl(VM& vm, JSObject* object, Vector<JSValue>& values)
{
    checkOffsetConsistency();
    ASSERT(isDictionary());

    // Loaded once: flag-on the word cannot change underneath us (unshared
    // thread-local object on the fast path; world stopped on the shared path).
    const bool objectIsSegmented = Options::useJSThreads() && isSegmentedButterfly(object->taggedButterflyWord());

    Locker<JSCellLock> cellLocker(NoLockingNecessary);

    PropertyTable* table = nullptr;
    size_t beforeOutOfLineCapacity = this->outOfLineCapacity();
    size_t afterOutOfLineCapacity = beforeOutOfLineCapacity;
    if (isUncacheableDictionary()) {
        table = propertyTableOrNull();
        ASSERT(table);
        PropertyOffset maxOffset = invalidOffset;
        if (unsigned propertyCount = table->size())
            maxOffset = offsetForPropertyNumber(propertyCount - 1, m_inlineCapacity);
        afterOutOfLineCapacity = outOfLineCapacity(maxOffset);
    }

    // This is the only case we shrink butterfly in this function. We should take a cell lock to protect against concurrent access to the butterfly.
    // SPEC-objectmodel L3/L4 (§6): flag-on, ALL dictionary-mode storage access
    // is serialized by the cell lock, so take it unconditionally there.
    if (beforeOutOfLineCapacity != afterOutOfLineCapacity || Options::useJSThreads())
        cellLocker = Locker { object->cellLock() };

    GCSafeConcurrentJSLocker locker(m_lock, vm);

    // Review round 2: flag-on, this impl may run ONLY under the §10.6 stop -
    // its in-place compaction (renumbered offsets, zeroed tails, possible
    // setButterfly(nullptr)/memmove) is unsound against the lock-free
    // dictionary READ path, and read-only foreign sharing fires no signal a
    // revalidation could observe (see flattenDictionaryStructure). Bail out
    // (nullptr) BEFORE any mutation if a caller ever reaches here unstopped;
    // locks are released by the destructors.
    if (Options::useJSThreads() && !butterflyWorldIsStopped(vm)) [[unlikely]]
        return nullptr;

    object->setStructureIDDirectly(id().nuke());
    WTF::storeStoreFence();

    if (isUncacheableDictionary()) {
        size_t propertyCount = table->size();

        // Holds our values compacted by insertion order, pre-allocated by our
        // callers (O4 on the under-stop path). This is OK since GC is deferred.
        ASSERT(values.size() == propertyCount);

        // Copies out our values from their hashed locations, compacting property table offsets as we go.
        PropertyOffset offset = table->renumberPropertyOffsets(object, m_inlineCapacity, values);
        setMaxOffset(vm, offset);
        ASSERT(transitionOffset() == invalidOffset);

        // Copies in our values to their compacted locations.
        for (unsigned i = 0; i < propertyCount; i++)
            object->putDirectOffset(vm, offsetForPropertyNumber(i, m_inlineCapacity), values[i]);

        // We need to zero our unused property space; otherwise the GC might see a
        // stale pointer when we add properties in the future.
        gcSafeZeroMemory(
            object->inlineStorageUnsafe() + inlineSize(),
            (inlineCapacity() - inlineSize()) * sizeof(EncodedJSValue));

        if (objectIsSegmented) {
            // Segmented leg (only reachable flag-on, under the stop): clear the
            // now-unused out-of-line fragment slots. Spines are immutable and
            // fragments never move (I6), so this is plain slot clearing; the GC
            // value-visits only slots < outOfLineSize (§4.5 step 4), and no
            // reader can hold a stale offset across the stop (I34/M7).
            // THREADS-INTEGRATE(objectmodel): segmentedOutOfLineSlot is defined
            // by Tasks 4/5 (Butterfly.h/ConcurrentButterfly.cpp).
            ButterflySpine* spine = butterflySpine(object->taggedButterflyWord());
            for (size_t i = outOfLineSize(); i < beforeOutOfLineCapacity; ++i)
                segmentedOutOfLineSlot(spine, static_cast<PropertyOffset>(firstOutOfLineOffset + i))->clear();
        } else if (Butterfly* butterfly = object->butterfly()) {
            size_t preCapacity = butterfly->indexingHeader()->preCapacity(this);
            void* base = butterfly->base(preCapacity, beforeOutOfLineCapacity);
            void* startOfPropertyStorageSlots = reinterpret_cast<EncodedJSValue*>(base) + preCapacity;
            gcSafeZeroMemory(static_cast<JSValue*>(startOfPropertyStorageSlots), (beforeOutOfLineCapacity - outOfLineSize()) * sizeof(EncodedJSValue));
        }
        checkOffsetConsistency();
    }

    setDictionaryKind(NoneDictionaryKind);
    setHasBeenFlattenedBefore(true);

    ASSERT(this->outOfLineCapacity() == afterOutOfLineCapacity);

    if (objectIsSegmented) {
        // Segmented objects never shrink or shift their storage: the spine is
        // immutable (I6), the aliased flat allocation (if any) stays recorded on
        // the spine (I7), and the unused slots were cleared above. The slightly
        // larger retained capacity is an accepted cost of the segmented regime.
    } else if (object->butterfly() && beforeOutOfLineCapacity != afterOutOfLineCapacity) {
        ASSERT(beforeOutOfLineCapacity > afterOutOfLineCapacity);
        // If the object had a Butterfly but after flattening/compacting we no longer have need of it,
        // we need to zero it out because the collector depends on the Structure to know the size for copying.
        if (!afterOutOfLineCapacity && !this->hasIndexingHeader(object))
            object->setButterfly(vm, nullptr);
        // If the object was down-sized to the point where the base of the Butterfly is no longer within the
        // first CopiedBlock::blockSize bytes, we'll get the wrong answer if we try to mask the base back to
        // the CopiedBlock header. To prevent this case we need to memmove the Butterfly down.
        else
            object->shiftButterflyAfterFlattening(locker, vm, this, afterOutOfLineCapacity);
    }

    WTF::storeStoreFence();
    object->setStructureIDDirectly(id());

    // We need to do a writebarrier here because a GC thread might be scanning the butterfly while
    // we are shuffling properties around. See: https://bugs.webkit.org/show_bug.cgi?id=166989
    vm.writeBarrier(object);

    return this;
}

void Structure::pinForCaching(const AbstractLocker&, VM& vm, PropertyTable* table)
{
    setIsPinnedPropertyTable(true);
    setPropertyTable(vm, table);
    m_transitionPropertyName = nullptr;
}

// ===== SPEC-objectmodel §9.4 fire functions + F3/F4 wiring (Task 3) =====

void Structure::fireThreadLocalSetsWithChainUnderStop(VM& vm, const char* reason, bool alsoFireTransitionThreadLocal)
{
    ASSERT(butterflyWorldIsStopped(vm));

    auto fireOne = [&](Structure* structure) {
        if (alsoFireTransitionThreadLocal && structure->m_transitionThreadLocalWatchpointSet.isStillValid())
            structure->m_transitionThreadLocalWatchpointSet.fireAll(vm, reason);
        if (structure->m_writeThreadLocalWatchpointSet.isStillValid())
            structure->m_writeThreadLocalWatchpointSet.fireAll(vm, reason);
    };

    fireOne(this);

    // F4 chain-fire (r13): also fire the still-valid sets along this
    // structure's previousID chain and transition-table successor subtree, in
    // the SAME stop. Monotone => sound; bounds N-thread warmup stop counts.
    // Interpretation note (recorded in INTEGRATE-objectmodel.md): the chain
    // propagates the same set kind(s) that fired - fireWriteThreadLocal (F1)
    // chains only writeThreadLocal so foreign WRITES do not destroy E1/E4
    // transition elision for the whole shape family; fireTransitionThreadLocal
    // (F2/F3) chains both (it implies writeThreadLocal, §5).
    //
    // The walk allocates only malloc memory (worklist) - never GC heap (O1/O4;
    // heap §10A exemption covers metadata writes inside stop windows). DeferGC
    // satisfies WeakGCMap::forEach's isDeferred() contract during the
    // transition-table iteration.
    DeferGC deferGC(vm);

    for (Structure* ancestor = previousID(); ancestor; ancestor = ancestor->previousID())
        fireOne(ancestor);

    Vector<Structure*, 16> worklist;
    worklist.append(this);
    while (!worklist.isEmpty()) {
        Structure* current = worklist.takeLast();
        if (current != this)
            fireOne(current); // Fired with NO lock held: fires may take rank-6b CodeBlock locks, which are OUTER to m_lock (I20).
        // Collect successors under m_lock (L6: mutator transition-table walks
        // hold m_lock; mutators are stopped, but compiler threads still run
        // Concurrently readers).
        ConcurrentJSLocker locker(current->m_lock);
        current->m_transitionTable.forEachTransition([&](Structure* successor) {
            if ((alsoFireTransitionThreadLocal && successor->m_transitionThreadLocalWatchpointSet.isStillValid())
                || successor->m_writeThreadLocalWatchpointSet.isStillValid())
                worklist.append(successor);
        });
    }
}

void Structure::fireTransitionThreadLocal(VM& vm, const char* reason)
{
    // §9.4: TTL sets fire only inside a stop-the-world window (I13); callers
    // (F1-F3 sites) reach this through the §10.6 veneer.
    RELEASE_ASSERT(butterflyWorldIsStopped(vm));
    // transitionThreadLocal fire implies writeThreadLocal (§5).
    fireThreadLocalSetsWithChainUnderStop(vm, reason, true);
}

void Structure::fireWriteThreadLocal(VM& vm, const char* reason)
{
    RELEASE_ASSERT(butterflyWorldIsStopped(vm));
    fireThreadLocalSetsWithChainUnderStop(vm, reason, false);
}

void Structure::fireTTLWatchpointSetsAfterPinning(VM& vm, const Structure* source)
{
    if (!Options::useJSThreads()) [[likely]]
        return;

    // F3: pin()/pinForCaching() during a transition rearrange/pin the property
    // table of the RESULT structure; if any input set is invalid (the source's
    // sets - instances of the old shape that will adopt the result may already
    // be shared - or, defensively, the result's own), fire BOTH sets on the
    // result so its instances never run elided fast paths. (GT#8 non-sites -
    // the poly-proto create/materialize/removeTransition helpers at
    // Structure.cpp:415-480/:598-670 - are intentionally NOT wired.)
    bool anyInputInvalid = !transitionThreadLocalIsStillValid() || !writeThreadLocalIsStillValid();
    if (source)
        anyInputInvalid |= !source->transitionThreadLocalIsStillValid() || !source->writeThreadLocalIsStillValid();
    if (!anyInputInvalid)
        return;
    if (!transitionThreadLocalIsStillValid() && !writeThreadLocalIsStillValid())
        return; // Both already invalid; nothing to fire.

    // Called with NO §6-ranked lock held (the pin call sites' Locker temporaries
    // have been destroyed) - GT11 caller contract for the veneer. Pre-M4 the
    // stub runs the closure inline under the GIL.
    jsThreadsStopTheWorldAndRun(vm, scopedLambda<void()>([&] {
        fireTransitionThreadLocal(vm, "F3: pinned-table transition from a structure with fired thread-locality sets");
    }));
}

void Structure::allocateRareData(VM& vm)
{
    if (!Options::useJSThreads()) [[likely]] {
        // Flag-off: single mutator inside the VM at a time, today's code is
        // unconditionally correct (I22) and flag-off codegen on transition
        // paths is preserved per the project rule.
        ASSERT(!hasRareData());
        StructureRareData* rareData = StructureRareData::create(vm, previousID());
        WTF::storeStoreFence();
        m_previousOrRareData.set(vm, this, rareData);
        ASSERT(hasRareData());
        return;
    }

    // Flag-on, rare-data install is idempotent-CAS. Every caller is a
    // check-then-act on hasRareData() (ensureRareData(), setMaxOffset(), ...),
    // so two mutators can both reach here for the same shared Structure; the
    // old ASSERT(!hasRareData()) then fired on the loser (Structure.cpp:1784,
    // races/transition-vs-write.js). We cannot serialize with m_lock here:
    // some callers already HOLD it (setMaxOffset()/setTransitionOffset() run
    // inside Structure::add's GCSafeConcurrentJSLocker lambda) and
    // ConcurrentJSLock is not recursive. (Those m_lock-held callers DO still
    // reach the StructureRareData::create GC allocation below when the
    // offset overflows the uint16 inline encoding — the rare >=
    // useRareDataFlag arms of setMaxOffset()/setTransitionOffset(),
    // Structure.h. That leg is the O1/I20-sanctioned exception, not a
    // violation: GCSafeConcurrentJSLocker carries a DeferGC, so the
    // allocation's collectIfNecessaryOrDefer poll takes the isDeferred()
    // branch (Heap.cpp, "didDeferGCWorkSlot") and DEFERS instead of parking
    // for a foreign stop-the-world — no FIX-2 mechanism-(2) stall under
    // m_lock.) So: allocate a private cell, then CAS it into the
    // m_previousOrRareData slot. Flag-on the slot is monotonic — Structure*
    // or null -> StructureRareData*, never back (clearPreviousID() routes
    // through the installed rare data, see Structure.h) — so the loser just
    // abandons its private cell to the GC.
    JSCell* expected = m_previousOrRareData.get();
    if (isRareData(expected))
        return; // Another thread already installed rare data.
    StructureRareData* rareData = StructureRareData::create(vm, static_cast<Structure*>(expected));
    // Publish the cell's fields before the pointer: rareDataConcurrently() /
    // tryRareData() readers are lock-free.
    WTF::storeStoreFence();
    JSCell** slot = m_previousOrRareData.slot();
    while (true) {
        JSCell* observed = WTF::atomicCompareExchangeStrong(slot, expected, static_cast<JSCell*>(rareData));
        if (observed == expected)
            break;
        if (isRareData(observed))
            return; // Lost the install race; drop our private cell.
        // The previousID we snapshotted changed under us (pin() ->
        // clearPreviousID() runs m_lock-held, but we are lock-free relative
        // to it). Our cell is still private, so refresh it and retry — we
        // must never resurrect a cleared previousID.
        expected = observed;
        if (expected)
            rareData->setPreviousID(vm, static_cast<Structure*>(expected));
        else
            rareData->clearPreviousID();
        WTF::storeStoreFence();
    }
    vm.writeBarrier(this, rareData);
    ASSERT(hasRareData());
}

WatchpointSet* Structure::ensurePropertyReplacementWatchpointSet(VM& vm, PropertyOffset offset)
{
    ASSERT(!isUncacheableDictionary());

    // In some places it's convenient to call this with an invalid offset. So, we do the check here.
    if (!isValidOffset(offset))
        return nullptr;
    
    if (!hasRareData())
        allocateRareData(vm);
    ConcurrentJSLocker locker(m_lock);
    Structure* structure = this;
    StructureRareData* rareData = structure->rareData();
    auto result = rareData->m_replacementWatchpointSets.add(offset, nullptr);
    if (result.isNewEntry) {
        result.iterator->value = WatchpointSet::create(IsWatched);
        rareData->incrementActiveReplacementWatchpointSet();
        structure->setIsWatchingReplacement(true);
    }
    return result.iterator->value.get();
}

WatchpointSet* Structure::firePropertyReplacementWatchpointSet(VM& vm, PropertyOffset offset, const char* reason, DeferredWatchpointFire* deferred)
{
    ASSERT(!isCompilationThread());
    auto* structure = this;
    auto* watchpointSet = structure->ensurePropertyReplacementWatchpointSet(vm, offset);
    if (watchpointSet && watchpointSet->state() == IsWatched) {
        StructureRareData* rareData = structure->rareData();
        if (deferred)
            watchpointSet->fireAllSlow(vm, deferred); // Invalidates now; fires at scope exit (SPEC-jit §5.6 deferral).
        else
            watchpointSet->fireAll(vm, reason);
        if (!Options::useJSThreads()) [[likely]] {
            // Flag-off: single mutator inside the VM, the plain counter is
            // exact; preserve today's codegen per the project rule.
            if (!rareData->decrementActiveReplacementWatchpointSet())
                structure->setIsWatchingReplacement(false);
        } else {
            // T3 residual (ab17e review): flag-on, the IsWatched check above
            // runs lock-free, so two mutators racing didReplaceProperty on
            // the SAME offset can both enter this block (fireAllSlow's
            // internal IsWatched re-check (I11) makes the second FIRE a
            // no-op, but not the second pass through this bookkeeping), and
            // decrements for DIFFERENT offsets are plain-word lost-update
            // races against each other and against the m_lock-held increment
            // in ensurePropertyReplacementWatchpointSet. A counter that
            // prematurely reaches zero clears isWatchingReplacement while
            // another offset's set is still IsWatched; didReplaceProperty
            // (Structure.h) then early-returns forever and DFG/FTL keep
            // constant-folded property values — silent wrong values. So
            // flag-on we do not trust the counter at all: serialize the
            // bookkeeping on m_lock (taken only AFTER the fire completes —
            // nothing fires under the lock, per I20/§5.6) and clear the flag
            // only when a scan of the map shows no set is still IsWatched.
            // Racy duplicate scans are harmless and conservative: a scan can
            // only leave the flag SET too long (extra slow-path calls, never
            // a missed fire), and the thread whose fire performed the last
            // IsWatched -> IsInvalidated transition acquires m_lock after
            // every earlier firing thread released it, so it observes all
            // prior transitions and the flag is always eventually cleared.
            // The m_activeReplacementWatchpointSet counter is advisory-only
            // flag-on (still incremented under m_lock at create; never
            // consulted here).
            ConcurrentJSLocker locker(structure->m_lock);
            bool anyStillWatched = false;
            for (auto& entry : rareData->m_replacementWatchpointSets) {
                if (entry.value && entry.value->state() == IsWatched) {
                    anyStillWatched = true;
                    break;
                }
            }
            if (!anyStillWatched)
                structure->setIsWatchingReplacement(false);
        }
    }
    return watchpointSet;
}

void Structure::startWatchingPropertyForReplacements(VM& vm, PropertyName propertyName)
{
    ASSERT(!isUncacheableDictionary());
    
    startWatchingPropertyForReplacements(vm, get(vm, propertyName));
}

void Structure::didReplacePropertySlow(PropertyOffset offset)
{
    firePropertyReplacementWatchpointSet(vm(), offset, "Property did get replaced");
}

void Structure::startWatchingInternalProperties(VM& vm)
{
    if (!isUncacheableDictionary()) {
        startWatchingPropertyForReplacements(vm, vm.propertyNames->toString);
        startWatchingPropertyForReplacements(vm, vm.propertyNames->valueOf);
    }
    setDidWatchInternalProperties(true);
}

#if DUMP_PROPERTYMAP_STATS

PropertyTableStats* propertyTableStats = 0;

struct PropertyTableStatisticsExitLogger {
    PropertyTableStatisticsExitLogger();
    ~PropertyTableStatisticsExitLogger();
};

DEFINE_GLOBAL_FOR_LOGGING(PropertyTableStatisticsExitLogger, logger, { });

PropertyTableStatisticsExitLogger::PropertyTableStatisticsExitLogger()
{
    propertyTableStats = adoptPtr(new PropertyTableStats()).leakPtr();
}

PropertyTableStatisticsExitLogger::~PropertyTableStatisticsExitLogger()
{
    unsigned finds = propertyTableStats->numFinds;
    unsigned collisions = propertyTableStats->numCollisions;
    dataLogF("\nJSC::PropertyTable statistics for process %d\n\n", getCurrentProcessID());
    dataLogF("%d finds\n", finds);
    dataLogF("%d collisions (%.1f%%)\n", collisions, 100.0 * collisions / finds);
    dataLogF("%d lookups\n", propertyTableStats->numLookups.load());
    dataLogF("%d lookup probings\n", propertyTableStats->numLookupProbing.load());
    dataLogF("%d adds\n", propertyTableStats->numAdds.load());
    dataLogF("%d removes\n", propertyTableStats->numRemoves.load());
    dataLogF("%d rehashes\n", propertyTableStats->numRehashes.load());
    dataLogF("%d reinserts\n", propertyTableStats->numReinserts.load());
}

#endif

PropertyTable* Structure::copyPropertyTableForPinning(VM& vm)
{
    // SPEC-objectmodel L6(ii)/I37 (Task 3c): flag-on, the CLONE of this
    // PUBLISHED table runs under the SOURCE's m_lock (a lock-free clone races
    // a concurrent locked add/rehash and tears). GCSafe locker = m_lock +
    // DeferGC: clone() allocates under the lock — O1's sanctioned DeferGC
    // form. The clone is private to the caller until publication. O1 on the
    // materialize fallback: it opens with DeferGC and locks per-structure
    // itself — never call it holding m_lock. Flag-off: today's code (I22).
    if (Options::useJSThreads()) [[unlikely]] {
        {
            GCSafeConcurrentJSLocker locker(m_lock, vm);
            if (PropertyTable* table = propertyTableOrNull())
                return PropertyTable::clone(vm, *table);
        }
        bool setPropertyTable = false;
        return materializePropertyTable(vm, setPropertyTable);
    }

    if (PropertyTable* table = propertyTableOrNull())
        return PropertyTable::clone(vm, *table);
    bool setPropertyTable = false;
    return materializePropertyTable(vm, setPropertyTable);
}

PropertyOffset Structure::getConcurrently(UniquedStringImpl* uid, unsigned& attributes)
{
    Vector<Structure*, 8> structures;
    Structure* tableStructure;
    PropertyTable* table;

    bool didFindStructure = findStructuresAndMapForMaterialization(structures, tableStructure, table);

    for (auto* structure : structures) {
        if (!structure->m_transitionPropertyName)
            continue;

        switch (structure->transitionKind()) {
        case TransitionKind::PropertyAddition:
        case TransitionKind::PropertyAttributeChange:
            break;
        case TransitionKind::PropertyDeletion:
            if (structure->m_transitionPropertyName.get() == uid) {
                if (didFindStructure) {
                    assertIsHeld(tableStructure->m_lock); // Sadly Clang needs some help here.
                    tableStructure->m_lock.unlock();
                }
                return invalidOffset;
            }
            continue;
        case TransitionKind::SetBrand:
            continue;
        default:
            ASSERT_NOT_REACHED();
            break;
        }

        if (structure->m_transitionPropertyName.get() == uid) {
            PropertyOffset result = structure->transitionOffset();
            attributes = structure->transitionPropertyAttributes();
            if (didFindStructure) {
                assertIsHeld(tableStructure->m_lock); // Sadly Clang needs some help here.
                tableStructure->m_lock.unlock();
            }
            return result;
        }
    }

    PropertyOffset result = invalidOffset;

    if (didFindStructure) {
        assertIsHeld(tableStructure->m_lock); // Sadly Clang needs some help here.
        // Because uid is UniquedStringImpl, it is guaranteed that the hash is already computed.
        // So we can use PropertyTable::get even from the concurrent compilers.
        // Even though taking a lock, all you can do is getting value from this table. We must not modify the table
        // from non mutator thread.
        auto [offset, entryAttributes] = table->get(uid);
        if (offset != invalidOffset) {
            result = offset;
            attributes = entryAttributes;
        }
        tableStructure->m_lock.unlock();
    }

    return result;
}

Vector<PropertyTableEntry> Structure::getPropertiesConcurrently()
{
    Vector<PropertyTableEntry> result;

    forEachPropertyConcurrently(
        [&] (const PropertyTableEntry& entry) -> bool {
            result.append(entry);
            return true;
        });
    
    return result;
}

PropertyOffset Structure::add(VM& vm, PropertyName propertyName, unsigned attributes)
{
    return add<ShouldPin::No>(
        vm, propertyName, attributes,
        [this, &vm] (const GCSafeConcurrentJSLocker&, PropertyOffset, PropertyOffset newMaxOffset) {
            setMaxOffset(vm, newMaxOffset);
        });
}

PropertyOffset Structure::remove(VM& vm, PropertyName propertyName)
{
    return remove<ShouldPin::No>(vm, propertyName, [this, &vm] (const GCSafeConcurrentJSLocker&, PropertyOffset, PropertyOffset newMaxOffset) {
        setMaxOffset(vm, newMaxOffset);
    });
}

PropertyOffset Structure::attributeChange(VM& vm, PropertyName propertyName, unsigned attributes)
{
    return attributeChange<ShouldPin::No>(
        vm, propertyName, attributes,
        [this, &vm] (const GCSafeConcurrentJSLocker&, PropertyOffset, PropertyOffset newMaxOffset) {
            setMaxOffset(vm, newMaxOffset);
        });
}

void Structure::getPropertyNamesFromStructure(VM& vm, PropertyNameArrayBuilder& propertyNames, DontEnumPropertiesMode mode)
{
    PropertyTable* table = ensurePropertyTableIfNotEmpty(vm);
    if (!table)
        return;

    // SPEC-objectmodel L6(iii) (Task 3c): flag-on, hold m_lock across BOTH
    // forEachProperty walks below (one critical section keeps the
    // strings-then-symbols passes mutually consistent). GCSafe locker = m_lock
    // + DeferGC: propertyNames.add may allocate under the lock — O1's
    // sanctioned DeferGC form. Retry if a racing transition stole the table
    // between materialization and lock acquisition. Flag-off: no lock (I22).
    std::optional<GCSafeConcurrentJSLocker> l6Locker;
    if (Options::useJSThreads()) [[unlikely]] {
        while (true) {
            l6Locker.emplace(m_lock, vm);
            if (propertyTableOrNull() == table)
                break;
            l6Locker.reset();
            table = ensurePropertyTableIfNotEmpty(vm);
            if (!table)
                return;
        }
    }

    bool knownUnique = propertyNames.canAddKnownUniqueForStructure();
    bool foundSymbol = false;

    auto checkDontEnumAndAdd = [&](const auto& entry) {
        if (mode == DontEnumPropertiesMode::Include || !(entry.attributes() & PropertyAttribute::DontEnum)) {
            if (knownUnique)
                propertyNames.addUnchecked(entry.key());
            else
                propertyNames.add(entry.key());
        }
    };
    
    table->forEachProperty([&](const auto& entry) {
        ASSERT(!isQuickPropertyAccessAllowedForEnumeration() || !(entry.attributes() & PropertyAttribute::DontEnum));
        ASSERT(!isQuickPropertyAccessAllowedForEnumeration() || !entry.key()->isSymbol());
        if (entry.key()->isSymbol()) {
            foundSymbol = true;
            if (propertyNames.propertyNameMode() != PropertyNameMode::Symbols)
                return IterationStatus::Continue;
        }
        checkDontEnumAndAdd(entry);
        return IterationStatus::Continue;
    });

    if (foundSymbol && propertyNames.propertyNameMode() == PropertyNameMode::StringsAndSymbols) {
        // To ensure the order defined in the spec, we append symbols at the last elements of keys.
        // https://tc39.es/ecma262/#sec-ordinaryownpropertykeys
        table->forEachProperty([&](const auto& entry) {
            if (entry.key()->isSymbol())
                checkDontEnumAndAdd(entry);
            return IterationStatus::Continue;
        });
    }
}

StructureFireDetail::StructureFireDetail(ClangVTableWorkaroundTag)
    : m_structure(nullptr)
{
}

void StructureFireDetail::dump(PrintStream& out) const
{
    out.print("Structure transition from ", *m_structure);
}

void Structure::didTransitionFromThisStructureWithoutFiringWatchpoint() const
{
    // If the structure is being watched, and this is the kind of structure that the DFG would
    // like to watch, then make sure to note for all future versions of this structure that it's
    // unwise to watch it.
    if (m_transitionWatchpointSet.isBeingWatched())
        const_cast<Structure*>(this)->setTransitionWatchpointIsLikelyToBeFired(true);
}

void Structure::fireStructureTransitionWatchpoint(DeferredStructureTransitionWatchpointFire* deferred) const
{
    if (deferred) {
        ASSERT(deferred->structure() == this);
        m_transitionWatchpointSet.fireAll(vm(), deferred);
    } else
        m_transitionWatchpointSet.fireAll(vm(), StructureFireDetail(this));
}

void Structure::didTransitionFromThisStructure(DeferredStructureTransitionWatchpointFire* deferred) const
{
    didTransitionFromThisStructureWithoutFiringWatchpoint();
    fireStructureTransitionWatchpoint(deferred);
}

template<typename Visitor>
void Structure::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    Structure* thisObject = uncheckedDowncast<Structure>(cell);
    ASSERT_GC_OBJECT_INHERITS(thisObject, info());

    Base::visitChildren(thisObject, visitor);
    
    ConcurrentJSLocker locker(thisObject->m_lock);
    
    visitor.append(thisObject->m_realm);
    if (!thisObject->isObject()) {
        // We do not need to clear JSPropertyNameEnumerator since it is never cached for non-object Structure.
        // We do not have code clearing JSPropertyNameEnumerator since this function can be called concurrently.
        // Relaxed atomic null store (same str): this marking-thread write races
        // lock-free mutator readers of the chain slot (prototypeChain/isValid,
        // canCachePropertyNameEnumerator).
        WTF::atomicStore(thisObject->m_cachedPrototypeChain.slot(), static_cast<StructureChain*>(nullptr), std::memory_order_relaxed);
#if ASSERT_ENABLED
        if (auto* rareData = thisObject->tryRareData())
            ASSERT(!rareData->cachedPropertyNameEnumerator());
#endif
    } else {
        visitor.append(thisObject->m_prototype);
        visitor.append(thisObject->m_cachedPrototypeChain);
    }
    visitor.append(thisObject->m_previousOrRareData);

    if (thisObject->isPinnedPropertyTable() || thisObject->protectPropertyTableWhileTransitioning()) {
        // NOTE: This can interleave in pin(), in which case it may see a null property table.
        // That's fine, because then the barrier will fire and we will scan this again.
        visitor.append(thisObject->m_propertyTableUnsafe);
    } else if (visitor.vm().isAnalyzingHeap())
        visitor.append(thisObject->m_propertyTableUnsafe);
    else if (thisObject->m_propertyTableUnsafe)
        thisObject->m_propertyTableUnsafe.clear();

    switch (thisObject->variant()) {
    case StructureVariant::Normal:
        break;
    case StructureVariant::Branded:
        BrandedStructure::visitAdditionalChildren(cell, visitor);
        break;
    case StructureVariant::WebAssemblyGC:
#if ENABLE(WEBASSEMBLY)
        WebAssemblyGCStructure::visitAdditionalChildren(cell, visitor);
        break;
#endif
    default:
        RELEASE_ASSERT_NOT_REACHED();
        break;
    }

    // Mark only in non Full collection. In full collection, we handle it as a weak-link.
    if (!(visitor.heap()->collectionScope() == CollectionScope::Full)) {
        if (auto* transition = thisObject->m_transitionTable.trySingleTransition())
            visitor.appendUnbarriered(transition);
    }
}

DEFINE_VISIT_CHILDREN(Structure);

template<typename Visitor>
ALWAYS_INLINE bool Structure::isCheapDuringGC(Visitor& visitor)
{
    // FIXME: We could make this even safer by returning false if this structure's property table
    // has any large property names.
    // https://bugs.webkit.org/show_bug.cgi?id=157334
    
    return (!m_realm || visitor.isMarked(m_realm.get()))
        && (hasPolyProto() || !storedPrototypeObject() || visitor.isMarked(storedPrototypeObject()));
}

template<typename Visitor>
bool Structure::markIfCheap(Visitor& visitor)
{
    if (!isCheapDuringGC(visitor))
        return visitor.isMarked(this);

    visitor.appendUnbarriered(this);
    return true;
}

template bool Structure::markIfCheap(AbstractSlotVisitor&);
template bool Structure::markIfCheap(SlotVisitor&);

Ref<StructureShape> Structure::toStructureShape(JSValue value, bool& sawPolyProtoStructure)
{
    Ref<StructureShape> baseShape = StructureShape::create();
    RefPtr<StructureShape> curShape = baseShape.ptr();
    Structure* curStructure = this;
    JSValue curValue = value;
    sawPolyProtoStructure = false;
    while (curStructure) {
        sawPolyProtoStructure |= curStructure->hasPolyProto();
        curStructure->forEachPropertyConcurrently(
            [&] (const PropertyTableEntry& entry) -> bool {
                if (!PropertyName(entry.key()).isPrivateName())
                    curShape->addProperty(*entry.key());
                return true;
            });

        if (JSObject* curObject = curValue.getObject())
            curShape->setConstructorName(JSObject::calculatedClassName(curObject));
        else
            curShape->setConstructorName(curStructure->classInfoForCells()->className);

        if (curStructure->isDictionary())
            curShape->enterDictionaryMode();

        curShape->markAsFinal();

        if (!curValue.isObject())
            break;

        JSObject* object = asObject(curValue);
        JSObject* prototypeObject = object->structure()->storedPrototypeObject(object);
        if (!prototypeObject)
            break;

        auto newShape = StructureShape::create();
        curShape->setProto(newShape.copyRef());
        curShape = WTF::move(newShape);
        curValue = prototypeObject;
        curStructure = prototypeObject->structure();
    }
    
    return baseShape;
}

void Structure::dump(PrintStream& out) const
{
    auto* structureID = reinterpret_cast<void*>(id().bits());
    out.print(RawPointer(this), ":[", RawPointer(structureID),
        "/", (uint32_t)(reinterpret_cast<uintptr_t>(structureID)), ", ",
        classInfoForCells()->className, ", (", inlineSize(), "/", inlineCapacity(), ", ",
        outOfLineSize(), "/", outOfLineCapacity(), "){");

    CommaPrinter comma;
    
    const_cast<Structure*>(this)->forEachPropertyConcurrently(
        [&] (const PropertyTableEntry& entry) -> bool {
            out.print(comma, entry.key(), ":"_s, static_cast<int>(entry.offset()));
            return true;
        });

    out.print("}, "_s, IndexingTypeDump(indexingMode()));

    out.print(", "_s, TransitionKindDump(transitionKind()));

    if (hasPolyProto())
        out.print(", PolyProto offset:"_s, knownPolyProtoOffset);
    else if (m_prototype.get().isCell())
        out.print(", Proto:"_s, RawPointer(m_prototype.get().asCell()));

    switch (dictionaryKind()) {
    case NoneDictionaryKind:
        if (hasBeenDictionary())
            out.print(", Has been dictionary"_s);
        break;
    case CachedDictionaryKind:
        out.print(", Dictionary"_s);
        break;
    case UncachedDictionaryKind:
        out.print(", UncacheableDictionary"_s);
        break;
    }

    if (transitionWatchpointSetIsStillValid())
        out.print(", Leaf"_s);
    else if (transitionWatchpointIsLikelyToBeFired())
        out.print(", Shady leaf"_s);
    
    if (transitionWatchpointSet().isBeingWatched())
        out.print(" (Watched)"_s);

    out.print("]"_s);
}

void Structure::dumpInContext(PrintStream& out, DumpContext* context) const
{
    if (context)
        context->structures.dumpBrief(this, out);
    else
        dump(out);
}

void Structure::dumpBrief(PrintStream& out, const CString& string) const
{
    out.print("%", string, ":", classInfoForCells()->className);
    if (indexingType() & IndexingShapeMask)
        out.print(",", IndexingTypeDump(indexingType()));
}

void Structure::dumpContextHeader(PrintStream& out)
{
    out.print("Structures:");
}

bool ClassInfo::hasStaticPropertyWithAnyOfAttributes(uint8_t attributes) const
{
    for (const ClassInfo* ci = this; ci; ci = ci->parentClass) {
        if (const HashTable* table = ci->staticPropHashTable) {
            if (table->seenPropertyAttributes & attributes)
                return true;
        }
    }
    return false;
}

bool ClassInfo::hasStaticProperty(PropertyName propertyName) const
{
    for (const ClassInfo* ci = this; ci; ci = ci->parentClass) {
        if (const HashTable* table = ci->staticPropHashTable) {
            auto* entry = table->entry(propertyName);
            if (entry)
                return true;
        }
    }
    return false;
}

void Structure::setCachedPropertyNameEnumerator(VM& vm, JSPropertyNameEnumerator* enumerator, StructureChain* chain)
{
    ASSERT(typeInfo().isObject());
    ASSERT(!isDictionary());
    if (!hasRareData())
        allocateRareData(vm);
    // The chain == m_cachedPrototypeChain validation moved below, under m_lock:
    // GIL-off, a racing prototypeChain() call can replace m_cachedPrototypeChain
    // between our caller computing `chain` and this point (see interleaving in
    // the comment below), so an unlocked equality assert here is a TOCTOU.
    // AUD1.N4(2)/(3): the multi-word enumerator install {watchpoint vector
    // rebuild + chain installs, then the flag word} runs under the owning
    // Structure's m_lock — two mutators for-in-ing a shared structure GIL-off
    // otherwise interleave FixedVector destroy/rebuild (one thread frees the
    // StructureChainInvalidationWatchpoints the other just linked into live
    // transition WatchpointSets => UAF on fire) and tear the enumerator/flag
    // publication. Winner-keeps under the lock, same as the sibling
    // cacheSpecialPropertySlow install (StructureRareData.cpp AUD1.N4(2)):
    // a loser's enumerator is simply not cached. No nested ConcurrentJSLock:
    // propertyNameEnumerator (JSPropertyNameEnumeratorInlines.h) only reads chain structures
    // and calls addTransitionWatchpoint (InlineWatchpointSet::add, lock-free).
    // Watchpoint FIRES (clearCachedPropertyNameEnumerator) stay K4.VI.2 —
    // inside a §A.3 stop — so the unlocked clear cannot race this install.
    // Flag-off / GIL-on identity: the lock is uncontended. The winner-keeps
    // early-return is reachable single-threaded only via reentrancy (a proxy
    // trap inside getEnumerablePropertyNames caching this structure first);
    // keeping the first-installed enumerator is observably equivalent — both
    // candidates validated against the same structure/chain state and the
    // caller returns its own enumerator either way.
    ConcurrentJSLocker locker(m_lock);
    if (rareData()->cachedPropertyNameEnumerator())
        return; // A racing installer won under the lock; keep its entry.
    // Re-validate the caller's chain snapshot now that we hold m_lock. GIL-off,
    // Structure::prototypeChain() (StructureInlines.h) clears and re-creates
    // m_cachedPrototypeChain WITHOUT taking m_lock, so a concurrent for-in on a
    // sibling thread can have replaced it with a distinct-but-equivalent
    // StructureChain after our caller (propertyNameEnumerator,
    // JSPropertyNameEnumeratorInlines.h) computed `chain`. The safe move is the
    // same as the loser path above: decline to cache when the snapshot is
    // observed superseded at lock entry. Note this is only a check at lock
    // entry, not a happens-before with prototypeChain()'s lock-free writer —
    // it can republish during the install below. That residual window is
    // benign: `chain` is consumed only as a StructureID list for transition
    // watchpoint installs (no chain pointer is stored), and a
    // content-divergent snapshot fails propertyNameEnumeratorShouldWatch into
    // the traversing-validated path where readers re-validate every use.
    // Identity: single-threaded / GIL-on, prototypeChain() returned
    // m_cachedPrototypeChain with no interleaving possible, so chain always
    // equals it and this bail is unreachable — preserved by the ASSERT below.
    if (chain != cachedPrototypeChainConcurrently()) {
        ASSERT(Options::useJSThreads() && !Options::useThreadGIL());
        return;
    }
    // AUD1.N4(3): flag-on, retire (don't free) any previous watchpoint vector
    // BEFORE the delegate's FixedVector reassignment
    // (StructureRareDataInlines.h) would destroy it in place — retired
    // watchpoints stay alive until finalizeUnconditionally because foreign
    // threads can still reach them through watched structures' transition
    // watchpoint sets. We hold m_lock, which serializes this against the
    // flag-on clearCachedPrototypeChain. Flag-off: single mutator, today's
    // immediate in-place destruction is race-free and stays bit-identical (I22).
    if (Options::useJSThreads()) [[unlikely]]
        rareData()->retireCachedPropertyNameEnumeratorWatchpoints();
    rareData()->setCachedPropertyNameEnumerator(vm, this, enumerator, chain);
}

JSPropertyNameEnumerator* Structure::cachedPropertyNameEnumerator() const
{
    if (!hasRareData())
        return nullptr;
    return rareData()->cachedPropertyNameEnumerator();
}

uintptr_t Structure::cachedPropertyNameEnumeratorAndFlag() const
{
    if (!hasRareData())
        return 0;
    return rareData()->cachedPropertyNameEnumeratorAndFlag();
}

bool Structure::canCachePropertyNameEnumerator(VM&) const
{
    if (!this->canCacheOwnPropertyNames())
        return false;

    // GIL-off, a sibling mutator's prototypeChain() (StructureInlines.h) can
    // clearCachedPrototypeChain() and republish a fresh StructureChain
    // lock-free between our caller computing its chain snapshot
    // (propertyNameEnumeratorSlow, JSPropertyNameEnumeratorInlines.h:87-90)
    // and this read. Observing the null window just means "decline to cache"
    // -- the same loser path as the chain-mismatch bail in
    // setCachedPropertyNameEnumerator above. If we instead observe a
    // replacement chain, walking it is still safe (a StructureChain is an
    // immutable null-terminated StructureID list once created) and a stale
    // `true` is caught by the chain == m_cachedPrototypeChain re-validation
    // under m_lock in setCachedPropertyNameEnumerator, which then declines to
    // cache. Identity: single-threaded / GIL-on, the caller's prototypeChain()
    // call just published m_cachedPrototypeChain with no interleaving
    // possible, so null is unreachable -- preserved by the ASSERT below.
    // Note the concurrent-GC clear in visitChildrenImpl (above) is NOT a
    // source of this null: it fires only for !isObject() structures, and this
    // path is object-only, so the identity ASSERT is safe even under
    // concurrent marking.
    // Relaxed atomic load: paired with the atomic null stores in
    // clearCachedPrototypeChain / visitChildrenImpl (same mov/ldr codegen).
    StructureChain* structureChain = cachedPrototypeChainConcurrently();
    if (!structureChain) [[unlikely]] {
        ASSERT(Options::useJSThreads() && !Options::useThreadGIL());
        return false;
    }
    // TSAN family structure-fields (r4 residual "StructureChain::create x
    // canCachePropertyNameEnumerator"): the chain's StructureID lanes live in
    // auxiliary memory that a foreign mutator can recycle into a NEW chain
    // (StructureChain::create zero-fill + finishCreation relaxed lane
    // stores). Relaxed atomic 32-bit lane loads (identical ldr/mov codegen)
    // pair with those writers; a stale/zero lane just takes the
    // decline-to-cache or sentinel exit, per the staleness argument above.
    StructureID* currentStructureID = structureChain->head();
    while (true) {
        static_assert(sizeof(StructureID) == sizeof(uint32_t));
        StructureID structureID = std::bit_cast<StructureID>(WTF::atomicLoad(reinterpret_cast<uint32_t*>(currentStructureID), std::memory_order_relaxed));
        if (!structureID)
            return true;
        Structure* structure = structureID.decode();
        if (!structure->canCacheOwnPropertyNames())
            return false;
        currentStructureID++;
    }

    ASSERT_NOT_REACHED();
    return true;
}
    
bool Structure::canAccessPropertiesQuicklyForEnumeration() const
{
    if (!isQuickPropertyAccessAllowedForEnumeration())
        return false;
    if (hasAnyKindOfGetterSetterProperties())
        return false;
    if (isUncacheableDictionary())
        return false;
    if (typeInfo().overridesGetOwnPropertyNames())
        return false;
    return true;
}

auto Structure::findPropertyHashEntry(PropertyName propertyName) const -> std::optional<PropertyHashEntry>
{
    for (const ClassInfo* info = classInfoForCells(); info; info = info->parentClass) {
        if (const HashTable* propHashTable = info->staticPropHashTable) {
            if (const HashTableValue* entry = propHashTable->entry(propertyName))
                return PropertyHashEntry { propHashTable, entry };
        }
    }
    return std::nullopt;
}

Structure* Structure::setBrandTransitionFromExistingStructureImpl(Structure* structure, UniquedStringImpl* brandID)
{
    ASSERT(structure->isObject());

    if (structure->hasBeenDictionary())
        return nullptr;

    if (Structure* existingTransition = structure->m_transitionTable.get(brandID, 0, TransitionKind::SetBrand))
        return existingTransition;

    return nullptr;
}

Structure* Structure::setBrandTransitionFromExistingStructureConcurrently(Structure* structure, UniquedStringImpl* brandID)
{
    ConcurrentJSLocker locker(structure->m_lock);
    return setBrandTransitionFromExistingStructureImpl(structure, brandID);
}

Structure* Structure::setBrandTransition(VM& vm, Structure* structure, Symbol* brand, DeferredStructureTransitionWatchpointFire* deferred)
{
    // SPEC-objectmodel L6(i)/I37 (Task 3c): flag-on, mutator transition-table
    // lookups hold the source's m_lock — route to the Concurrently variant.
    Structure* existingTransition;
    if (Options::useJSThreads()) [[unlikely]]
        existingTransition = setBrandTransitionFromExistingStructureConcurrently(structure, &brand->uid());
    else
        existingTransition = setBrandTransitionFromExistingStructureImpl(structure, &brand->uid());
    if (existingTransition)
        return existingTransition;

    // Task 3b: SAL emission — see addNewPropertyTransition for the rationale
    // (pre-lock DeferGC per O1/heap L5; deferred watchpoint fire keeps
    // rank-6b locks out of the SAL region). BrandedStructure::create also
    // allocates its Structure cell without the locker's GCDeferralContext
    // (unowned file) — covered by salDeferGC; recorded for the M7 audit.
    std::optional<DeferGC> salDeferGC;
    std::optional<DeferredStructureTransitionWatchpointFire> salDeferredFire;
    if (Options::useStructureAllocationLock()) [[unlikely]] {
        salDeferGC.emplace(vm);
        if (!deferred) {
            salDeferredFire.emplace(vm, structure);
            deferred = &*salDeferredFire;
        }
    }

    Structure* transition;
    {
        std::optional<SharedVMState::StructureAllocationLocker> structureAllocationLocker;
        if (Options::useStructureAllocationLock()) [[unlikely]]
            structureAllocationLocker.emplace(vm);
        transition = BrandedStructure::create(vm, structure, &brand->uid(), deferred);
    }
    transition->setTransitionKind(TransitionKind::SetBrand);

    transition->m_cachedPrototypeChain.setMayBeNull(vm, transition, structure->cachedPrototypeChainConcurrently()); // Relaxed atomic read: the source chain slot is written lock-free (TSAN family structure-fields).
    transition->m_blob.setIndexingModeIncludingHistory(structure->indexingModeIncludingHistory());
    transition->m_transitionPropertyName = &brand->uid();
    transition->setTransitionPropertyAttributes(0);
    transition->setPropertyTable(vm, structure->takePropertyTableOrCloneIfPinned(vm));
    transition->setMaxOffset(vm, structure->maxOffset());
    checkOffset(transition->maxOffset(), transition->inlineCapacity());

    if (structure->isDictionary()) {
        PropertyTable* table = transition->ensurePropertyTable(vm);
        transition->pin(Locker { transition->m_lock }, vm, table);
        transition->fireTTLWatchpointSetsAfterPinning(vm, structure); // SPEC-objectmodel F3
    } else {
        // Task 3b: SAL outside m_lock (§6 order); the function-scope GC
        // deferral above keeps any GC trigger out of the SAL region.
        std::optional<SharedVMState::StructureAllocationLocker> structureAllocationLocker;
        if (Options::useStructureAllocationLock()) [[unlikely]]
            structureAllocationLocker.emplace(vm);
        Locker locker { structure->m_lock };
        // SPEC-objectmodel L6/I37 (Task 3c): dual-check under m_lock; see
        // addNewPropertyTransition. (Key: brand uid + SetBrand.)
        if (Options::useJSThreads()) [[unlikely]] {
            if (Structure* existing = structure->m_transitionTable.getMatching(transition)) {
                existing->checkOffsetConsistency();
                return existing;
            }
        }
        structure->m_transitionTable.add(vm, structure, transition);
    }

    transition->checkOffsetConsistency();
    return transition;
}

void DeferredStructureTransitionWatchpointFire::fireAllSlow()
{
    StructureFireDetail detail(m_structure);
    watchpointsToFire().fireAll(m_vm, detail);
}

void Structure::finalizeUnconditionally(VM& vm, CollectionScope collectionScope)
{
    m_transitionTable.finalizeUnconditionally(vm, collectionScope);
}

void dumpTransitionKind(PrintStream& out, TransitionKind kind)
{
    const char* kindName;
    switch (kind) {
    case TransitionKind::Unknown:
        kindName = "Unknown";
        break;
    case TransitionKind::PropertyAddition:
        kindName = "PropertyAddition";
        break;
    case TransitionKind::PropertyDeletion:
        kindName = "PropertyDeletion";
        break;
    case TransitionKind::PropertyAttributeChange:
        kindName = "PropertyAttributeChange";
        break;
    case TransitionKind::AllocateUndecided:
        kindName = "AllocateUndecided";
        break;
    case TransitionKind::AllocateInt32:
        kindName = "AllocateInt32";
        break;
    case TransitionKind::AllocateDouble:
        kindName = "AllocateDouble";
        break;
    case TransitionKind::AllocateContiguous:
        kindName = "AllocateContiguous";
        break;
    case TransitionKind::AllocateArrayStorage:
        kindName = "AllocateArrayStorage";
        break;
    case TransitionKind::AllocateSlowPutArrayStorage:
        kindName = "AllocateSlowPutArrayStorage";
        break;
    case TransitionKind::SwitchToSlowPutArrayStorage:
        kindName = "SwitchToSlowPutArrayStorage";
        break;
    case TransitionKind::AddIndexedAccessors:
        kindName = "AddIndexedAccessors";
        break;
    case TransitionKind::PreventExtensions:
        kindName = "PreventExtensions";
        break;
    case TransitionKind::Seal:
        kindName = "Seal";
        break;
    case TransitionKind::Freeze:
        kindName = "Freeze";
        break;
    case TransitionKind::BecomePrototype:
        kindName = "BecomePrototype";
        break;
    case TransitionKind::ChangePrototype:
        kindName = "ChangePrototype";
        break;
    case TransitionKind::SetBrand:
        kindName = "SetBrand";
        break;
    }

    out.print(kindName);
}

void Structure::checkOffsetConsistency() const
{
    // CVE-DBG-2: every caller of this no-arg entry is an UNLOCKED spot-check
    // (the transition-planning paths in this file). Flag-on GIL-off, a
    // foreign thread can be rehashing/stealing the published PropertyTable
    // under ITS locks while we read {maxOffset, table} with none:
    // PropertyTable::rehash resets m_keyCount to 0 (concurrentRelaxedStore,
    // PropertyTable.h §3.25 relaxed-reader family) before reinserting, so
    // propertyStorageSize() legally reads 0 against a settled maxOffset.
    // Off-by-N (or N-to-0) here is a sanctioned transient under
    // SPEC-objectmodel L6(ii) (published-table mutation serialized under the
    // owning structure's m_lock) + the OM C4 staleness model — never
    // checker-grade cross-word coherence for lock-free readers.
    //
    // NOTE: this is NOT a Debug-only assert. This wrapper compiles in
    // Release and UNREACHABLE_FOR_PLATFORM is RELEASE_ASSERT_NOT_REACHED, so
    // the same race could spuriously abort Release GIL-off; skipping here
    // intentionally retires that release tripwire on the unlocked spot-check
    // paths for the GIL-off lane only. GIL-on (single mutator) keeps it: the
    // check is race-free there. A known cost: spot-checks of freshly created
    // THREAD-PRIVATE transition targets are skipped too, even though they
    // were race-free; mutation sites stay strict via
    // Structure::checkConsistency() below, which holds m_lock and calls the
    // two-arg checker directly, and materializePropertyTable likewise checks
    // its thread-local table via the two-arg form. Flag-off:
    // behavior-identical (one predicted-false branch, same convention as the
    // existing carve-out in the two-arg template above).
    if (Options::useJSThreads() && !Options::useThreadGIL()) [[unlikely]]
        return;
    if (auto* propertyTable = propertyTableOrNull())
        checkOffsetConsistency(propertyTable, [] { });
    else
        ASSERT(!isPinnedPropertyTable());
}

#if ASSERT_ENABLED
void Structure::checkConsistency()
{
    // Mutation-site self-check: every caller holds m_lock
    // (Structure::add/remove/attributeChange in StructureInlines.h), so the
    // {maxOffset, table} pair is coherent. Bypass the no-arg wrapper above —
    // its flag-on skip is for UNLOCKED spot-checks only — and run the strict
    // two-arg checker. (The template's dictionary carve-out still applies,
    // unchanged.)
    if (auto* propertyTable = propertyTableOrNull())
        checkOffsetConsistency(propertyTable, [] { });
    else
        ASSERT(!isPinnedPropertyTable());
}
#endif

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
