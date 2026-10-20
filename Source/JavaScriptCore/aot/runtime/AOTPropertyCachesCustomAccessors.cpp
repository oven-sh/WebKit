/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPropertyCachesCustomAccessors.h"

#if ENABLE(AOT)

#include "AOTBuiltinTableBindings.h"
#include "AOTClosure.h"
#include "AOTHeapCacheRegistry.h"
#include "AOTPropertyCaches.h"
#include "AOTRuntimeCounters.h"
#include "JSCInlines.h"
#include "PropertySlot.h"
#include "PutPropertySlot.h"
#include <algorithm>
#include <wtf/CheckedArithmetic.h>
#include <wtf/MathExtras.h>
#include <wtf/OSAllocator.h>
#include <wtf/PageBlock.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

static size_t sizeOfRecords(uint32_t capacity)
{
    return roundUpToMultipleOf(pageSize(), std::max<size_t>(capacity, 1) * sizeof(CustomAccessorRecord));
}

CustomAccessorTable::CustomAccessorTable(VM& vm, CacheRegistry& registry)
    : m_vm(vm)
    , m_capacity(safeCast<uint32_t>(runtimeLimitValue(vm, LimitID::CustomAccessorRecords)))
{
    m_records = static_cast<CustomAccessorRecord*>(OSAllocator::reserveAndCommit(sizeOfRecords(m_capacity), OSAllocator::FastMallocPages));
    registry.add(CacheKind::WeakByKey, "CustomAccessorTable"_s, *this);
}

CustomAccessorTable::~CustomAccessorTable()
{
    OSAllocator::decommitAndRelease(m_records, sizeOfRecords(m_capacity));
}

std::optional<uint32_t> CustomAccessorTable::find(const CustomAccessorRecord& record) const
{
    auto iterator = m_indexByKey.find(keyOf(record));
    if (iterator == m_indexByKey.end())
        return std::nullopt;
    return iterator->value;
}

std::optional<uint32_t> CustomAccessorTable::add(const CustomAccessorRecord& record)
{
    RELEASE_ASSERT(record.function && record.globalObject);
    if (auto index = find(record))
        return index;

    uint32_t index;
    if (!m_freeIndexes.isEmpty())
        index = m_freeIndexes.takeLast();
    else if (m_numberOfRecordsEverUsed < m_capacity)
        index = m_numberOfRecordsEverUsed++;
    else {
        countLimitHit(m_vm, LimitID::CustomAccessorRecords);
        return std::nullopt;
    }

    m_records[index] = record;
    m_records[index].unused = 0;
    m_indexByKey.add(keyOf(record), index);
    m_writtenSinceCollection.recordAll();
    return index;
}

size_t CustomAccessorTable::removeDeadEntries(VM& vm, CollectionScope scope)
{
    size_t numberOfRecordsFreed = 0;
    m_writtenSinceCollection.forEachRegionToSweep(scope, [&](size_t, size_t) {
        for (uint32_t index = 0; index < m_numberOfRecordsEverUsed; ++index) {
            if (!isInUse(index))
                continue;
            CustomAccessorRecord& record = m_records[index];
            bool isLive = (!record.slotBase || vm.heap.isMarked(record.slotBase)) && vm.heap.isMarked(record.globalObject);
            if (isLive)
                continue;
            m_indexByKey.remove(keyOf(record));
            record = { };
            m_freeIndexes.append(index);
            ++numberOfRecordsFreed;
        }
    });
    return numberOfRecordsFreed;
}

static bool isCacheableNonDictionary(Structure* structure)
{
    return structure->propertyAccessesAreCacheable() && !structure->isDictionary();
}

static PropertyAnswer uncacheableOnce(SlotAccess access)
{
    return PropertyAnswer::uncacheable(access, true);
}

static PropertyAnswer uncacheableAfter(SlotAccess access, ChainWalk&& walk)
{
    PropertyAnswer answer = PropertyAnswer::uncacheable(access, walk.didFlatten);
    answer.changeableObjects = WTF::move(walk.changeableObjects);
    return answer;
}

static PropertyAnswer answerForCustomAccessor(VM& vm, SlotAccess access, ChainWalkMode mode, const CustomAccessorRecord& record, bool isOwn, ChainObjects&& changeableObjects)
{
    CustomAccessorTable& table = vm.aotState().propertyCaches->customAccessors();
    std::optional<uint32_t> index = mode == ChainWalkMode::MayAllocate ? table.add(record) : table.find(record);
    if (!index)
        return uncacheableOnce(access);
    PropertyAnswer answer;
    answer.kind = PropertyAnswerKind::CustomAccessor;
    answer.access = access;
    answer.isOwn = isOwn;
    answer.customAccessor = *index;
    answer.changeableObjects = WTF::move(changeableObjects);
    return answer;
}

PropertyAnswer classifyCustomGetter(const ReadQuery& query)
{
    VM& vm = query.globalObject->vm();
    const PropertySlot& slot = query.slot;
    if (!query.base.isObject() || !isCacheableNonDictionary(query.structureBefore))
        return PropertyAnswer::uncacheable(SlotAccess::Read);
    JSObject* base = asObject(query.base);
    JSObject* holder = slot.slotBase();
    if (base->structure() != query.structureBefore)
        return uncacheableOnce(SlotAccess::Read);

    ChainWalk walk;
    if (holder != base) {
        if (auto* closure = dynamicDowncast<Closure>(base); closure && closure->isLazyClassObject())
            return PropertyAnswer::uncacheable(SlotAccess::Read);
        if (!structureProvesOwnAbsence(vm, base, query.structureBefore, query.uid, query.mode))
            return PropertyAnswer::uncacheable(SlotAccess::Read);
        walk = walkChain({ query.globalObject, query.structureBefore, base, query.mode }, query.uid);
        if (walk.result != ChainWalkResult::Found || walk.holder != holder || !(walk.attributes & PropertyAttribute::CustomAccessorOrValue))
            return uncacheableAfter(SlotAccess::Read, WTF::move(walk));
    }

    bool passesHolder = !(slot.attributes() & PropertyAttribute::CustomAccessor);
    auto domAttribute = slot.domAttribute();
    if (domAttribute && !(passesHolder ? holder : base)->inherits(domAttribute->classInfo))
        return uncacheableAfter(SlotAccess::Read, WTF::move(walk));
    uint32_t offsetOfCachedValue = !passesHolder && domAttribute ? BuiltinBindings::cachedValueOffsetOf(domAttribute->classInfo, query.uid).value_or(0) : 0;

    // The slot serves every object of the Structure, and each of them holds its own property: a null slotBase passes the receiver.
    JSObject* slotBase = passesHolder && holder != base ? holder : nullptr;
    CustomAccessorRecord record { std::bit_cast<void*>(slot.customGetter().taggedPtr()), slotBase, holder->realm(), offsetOfCachedValue, 0 };
    return answerForCustomAccessor(vm, SlotAccess::Read, query.mode, record, holder == base, WTF::move(walk.changeableObjects));
}

PropertyAnswer classifyAbsentClosurePrototype(const ReadQuery& query)
{
    if (!query.base.isObject() || !isCacheableNonDictionary(query.structureBefore))
        return PropertyAnswer::uncacheable(SlotAccess::Read);
    ChainWalk walk = walkChain({ query.globalObject, query.structureBefore, asObject(query.base), query.mode }, query.uid);
    if (walk.result != ChainWalkResult::Absent)
        return uncacheableAfter(SlotAccess::Read, WTF::move(walk));

    CustomAccessorRecord record { std::bit_cast<void*>(closurePrototypePropertyGetter().taggedPtr()), nullptr, query.structureBefore->realm(), 0, 0 };
    return answerForCustomAccessor(query.globalObject->vm(), SlotAccess::Read, query.mode, record, true, WTF::move(walk.changeableObjects));
}

PropertyAnswer classifyCustomSetter(const StoreQuery& query)
{
    ASSERT(query.mode != PutMode::Define && query.slot.isCustomAccessor() && query.slot.isCacheableCustom());
    VM& vm = query.globalObject->vm();
    SlotAccess access = accessFor(query.mode);
    JSObject* base = query.base;
    JSObject* holder = query.slot.base();
    Structure* structureBefore = query.structureBefore;
    if (base->structure() != structureBefore)
        return uncacheableOnce(access);
    if (!isCacheableNonDictionary(structureBefore) || structureBefore->typeInfo().overridesPut() || structureBefore->isImmutableProperty(query.uid))
        return PropertyAnswer::uncacheable(access);

    ChainWalk walk;
    if (holder != base) {
        if (!structureProvesOwnAbsence(vm, base, structureBefore, query.uid, ChainWalkMode::MayAllocate))
            return PropertyAnswer::uncacheable(access);
        walk = walkChain({ query.globalObject, structureBefore, base, ChainWalkMode::MayAllocate }, query.uid);
        if (storeThroughChain(walk) != StoreThroughChain::CallsCustomSetter || walk.holder != holder)
            return uncacheableAfter(access, WTF::move(walk));
    }

    CustomAccessorRecord record { std::bit_cast<void*>(query.slot.customSetter().taggedPtr()), nullptr, holder->realm(), 0, 0 };
    return answerForCustomAccessor(vm, access, ChainWalkMode::MayAllocate, record, holder == base, WTF::move(walk.changeableObjects));
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
