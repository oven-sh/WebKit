/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCompactTable.h"

#if ENABLE(AOT)

#include "AOTDataBuilder.h"
#include "AOTInstance.h"
#include "AOTState.h"
#include "AOTTableInImage.h"
#include "DeferGCInlines.h"
#include "JSCInlines.h"
#include "JSMapInlines.h"
#include "JSSetInlines.h"
#include "MarkedBlockCage.h"
#include <array>
#include <wtf/IterationStatus.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

CompactTable* CompactTable::createInImage(VM& vm, State& state, const TableInImage& entries)
{
    CompactTable* table = new (NotNull, allocateCell<CompactTable>(vm, offsetOfIndex() + 2 * sizeof(void*))) CompactTable(vm, structureOf(vm));
    table->m_used = entries.count;
    table->m_alive = entries.count;
    table->m_capacity = entries.capacity | capacityIsInImage;
    table->m_indexBits = entries.indexBits | indexIsInImage;
    table->m_keysShiftAndFlags = entries.keysShift | keysAreInImage;
    table->m_valueWidth = entries.hasValues ? Width::InImage : Width::None;
    *table->at<const TableInImage*>(offsetOfIndex()) = &entries;
    *table->at<State*>(offsetOfIndex() + sizeof(void*)) = &state;
    table->finishCreation(vm);
    return table;
}

JSValue CompactTable::valueInImage(uint32_t entry) const
{
    return constantOfDataWord(stateOfTableInImage(), tableInImage().values()[entry]);
}

JSSet* CompactTable::createSetInImage(JSGlobalObject* globalObject, const TableInImage& entries)
{
    VM& vm = globalObject->vm();
    JSSet* set = JSSet::create(vm, globalObject->setStructure());
    setStorage(vm, set, createInImage(vm, State::of(globalObject), entries));
    return set;
}

JSMap* CompactTable::createMapInImage(JSGlobalObject* globalObject, const TableInImage& entries)
{
    VM& vm = globalObject->vm();
    JSMap* map = JSMap::create(vm, globalObject->mapStructure());
    setStorage(vm, map, createInImage(vm, State::of(globalObject), entries));
    return map;
}

CompactTable* CompactTable::tryMoveToHeap(JSGlobalObject* globalObject, JSObject* owner)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    CompactTable* table = tableOf(owner);
    RELEASE_ASSERT(table->isInImage());
    uint32_t count = table->m_used;
    DeferGCForAWhile deferGC(vm);
    Width keyWidth = table->stateOfTableInImage().instance.options().useWideReferences() ? Width::Wide : Width::Narrow;
    Width valueWidth = Width::None;
    if (table->hasValues()) {
        valueWidth = keyWidth;
        for (uint32_t entry = 0; valueWidth == Width::Narrow && entry < count; ++entry)
            valueWidth = widthFor(vm, table->valueInImage(entry));
    }
    CompactTable* copy = tryCreate(globalObject, keyWidth, valueWidth, count);
    RETURN_IF_EXCEPTION(scope, nullptr);
    for (uint32_t entry = 0; entry < count; ++entry) {
        JSValue key = table->keyAt(entry);
        RELEASE_ASSERT(canHold(vm, keyWidth, key));
        copy->append(vm, key, hashOfStoredKey(key), table->valueAt(entry));
    }
    table->becomeObsolete(vm, Status::Replaced, copy);
    setStorage(vm, owner, copy);
    return copy;
}

static ALWAYS_INLINE bool holdsSameText(uintptr_t cageBase, uint32_t narrowKey, const StringImpl* impl, unsigned hash)
{
    const StringImpl* other = uncheckedDowncast<JSString>(MarkedBlockCage::decode(cageBase, narrowKey))->tryGetValueImpl();
    return other == impl || (other->existingHash() == hash && equal(other, impl));
}

static ALWAYS_INLINE bool isResolvedString(JSValue value)
{
    return value && value.isString() && !asString(value)->isRope();
}

static NEVER_INLINE bool gatherDistinctStrings(uintptr_t cageBase, std::span<const WriteBarrier<Unknown>> strings, uint32_t* keys, uint64_t& tags, uint32_t& used)
{
    for (auto& string : strings) {
        JSValue value = string.get();
        if (!isResolvedString(value))
            return false;
        const StringImpl* impl = asString(value)->tryGetValueImpl();
        unsigned hash = impl->hash();
        bool isPresent = CompactTable::probeIndex<CompactTable::SlotKind::Tag>(reinterpret_cast<const uint8_t*>(&tags), 0, used, hash, [&](uint32_t entry) {
            return holdsSameText(cageBase, keys[entry], impl, hash);
        }).has_value();
        if (isPresent)
            continue;
        keys[used] = MarkedBlockCage::encode(cageBase, asString(value));
        tags |= static_cast<uint64_t>(hash >> CompactTable::tagShiftOfTags) << (used * 8);
        ++used;
    }
    return true;
}

template<CompactTable::SlotKind kind>
uint32_t CompactTable::fillWithHashedStrings(uintptr_t cageBase, std::span<const WriteBarrier<Unknown>> strings)
{
    using Format = SlotFormat<kind>;
    using IndexSlot = typename Format::Slot;
    IndexSlot* slots = at<IndexSlot>(offsetOfIndex());
    uint32_t* keys = at<uint32_t>(offsetOfKeys());
    uint32_t used = 0;
    for (auto& string : strings) {
        JSString* key = asString(string.get());
        const StringImpl* impl = key->tryGetValueImpl();
        unsigned hash = impl->existingHash();
        uint32_t tag = Format::tagOf(hash) << Format::entryBits;
        forEachSlotInProbeOrder(slots, m_indexBits, hash, [&](IndexSlot& slot) ALWAYS_INLINE_LAMBDA {
            if (slot == Format::freeSlot) {
                slot = static_cast<IndexSlot>(tag | used);
                keys[used++] = MarkedBlockCage::encode(cageBase, key);
                return IterationStatus::Done;
            }
            uint32_t entry = slot ^ tag;
            if (entry < 1u << Format::entryBits && holdsSameText(cageBase, keys[entry], impl, hash))
                return IterationStatus::Done;
            return IterationStatus::Continue;
        });
    }
    return used;
}

JSSet* CompactTable::tryCreateSetOfStrings(JSGlobalObject* globalObject, std::span<const WriteBarrier<Unknown>> values)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    Instance& instance = *Instance::of(globalObject);
    const WriteBarrier<Unknown>* strings = values.data();
    size_t length = values.size();
    if (!length || length > maxCapacity || instance.options().useWideReferences()) [[unlikely]]
        return nullptr;
    uintptr_t cageBase = instance.cellCageBase;

    CompactTable* table;
    if (length <= maxCapacityWithTags) {
        std::array<uint32_t, maxCapacityWithTags> storage { };
        uint32_t* keys = storage.data();
        constexpr uint64_t onesInBytes = 0x0101010101010101ull;
        uint64_t tags = 0;
        uint64_t occupiedTagBits = 0;
        size_t i = 0;
        for (; i < length; ++i) {
            JSValue value = strings[i].get();
            if (!isResolvedString(value))
                return nullptr;
            const StringImpl* impl = asString(value)->tryGetValueImpl();
            if (!impl->hasHash()) [[unlikely]]
                break;
            uint64_t tag = impl->existingHash() >> tagShiftOfTags;
            uint64_t difference = tags ^ tag * onesInBytes;
            if ((difference - onesInBytes) & ~difference & occupiedTagBits) [[unlikely]]
                break;
            keys[i] = MarkedBlockCage::encode(cageBase, asString(value));
            tags |= tag << (i * 8);
            occupiedTagBits = occupiedTagBits << 8 | 0x80;
        }
        uint32_t used = i;
        if (i < length && !gatherDistinctStrings(cageBase, { strings + i, length - i }, keys, tags, used)) [[unlikely]]
            return nullptr;
        static_assert(initialCapacity * 2 == maxCapacityWithTags && sizeof(storage) == 4 * sizeof(uint64_t) && !(initialSize(Width::Narrow, Width::None) % MarkedSpace::sizeStep) && !(initialCapacity * sizeof(uint32_t) % MarkedSpace::sizeStep));
        bool isInitialSize = used <= initialCapacity;
        size_t size = isInitialSize ? initialSize(Width::Narrow, Width::None) : initialSize(Width::Narrow, Width::None) + initialCapacity * sizeof(uint32_t);
        table = new (NotNull, allocateCell<CompactTable>(vm, size)) CompactTable(vm, structureOf(vm));
        table->m_used = used;
        table->m_alive = used;
        table->m_capacity = isInitialSize ? initialCapacity : maxCapacityWithTags;
        table->m_keysShiftAndFlags = keysShiftWithTags;
        table->m_valueWidth = Width::None;
        *table->at<uint64_t>(offsetOfIndex()) = tags;
        memcpy(table->at<uint8_t>(offsetOfIndex() + maxCapacityWithTags), keys, initialCapacity * sizeof(uint32_t));
        if (!isInitialSize)
            memcpy(table->at<uint8_t>(offsetOfIndex() + maxCapacityWithTags + initialCapacity * sizeof(uint32_t)), keys + initialCapacity, initialCapacity * sizeof(uint32_t));
        table->finishCreation(vm);
    } else {
        for (size_t i = 0; i < length; ++i) {
            JSValue value = strings[i].get();
            if (!isResolvedString(value))
                return nullptr;
            asString(value)->tryGetValueImpl()->hash();
        }
        table = tryCreate(globalObject, Width::Narrow, Width::None, length);
        RETURN_IF_EXCEPTION(scope, nullptr);
        uint32_t used = 0;
        switch (table->slotKind()) {
        case SlotKind::Tag:
            RELEASE_ASSERT_NOT_REACHED();
        case SlotKind::Short:
            used = table->fillWithHashedStrings<SlotKind::Short>(cageBase, values);
            break;
        case SlotKind::Long:
            used = table->fillWithHashedStrings<SlotKind::Long>(cageBase, values);
            break;
        case SlotKind::Huge:
            used = table->fillWithHashedStrings<SlotKind::Huge>(cageBase, values);
            break;
        }
        WTF::storeStoreFence();
        table->m_used = used;
        table->m_alive = used;
    }
    JSSet* set = JSSet::create(vm, globalObject->setStructure());
    set->m_storage.setWithoutWriteBarrier(table);
    vm.mutatorFence();
    return set;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
