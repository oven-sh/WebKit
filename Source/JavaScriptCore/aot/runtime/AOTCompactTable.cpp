/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCompactTable.h"

#if ENABLE(AOT)

#include "AOTProgramStrings.h"
#include "AOTRuntimeTable.h"
#include "AOTRuntimeValidation.h"
#include "AOTState.h"
#include "AOTTableInImage.h"
#include "DeferTermination.h"
#include "JSCInlines.h"
#include "JSCellButterfly.h"
#include "JSMapInlines.h"
#include "JSSetInlines.h"
#include "MarkedBlockCage.h"
#include <array>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

const ClassInfo CompactTable::s_info = { "CompactTable"_s, nullptr, nullptr, nullptr, CREATE_METHOD_TABLE(CompactTable) };

static_assert(CompactTable::offsetOfAliveCount() == CompactTable::offsetOfUsed() + 4 && CompactTable::offsetOfIndexBits() == CompactTable::offsetOfCapacity() + 4);
static_assert(CompactTable::offsetOfKeysShift() == CompactTable::offsetOfCapacity() + 5 && CompactTable::offsetOfValueWidth() == CompactTable::offsetOfCapacity() + 6 && CompactTable::offsetOfStatus() == CompactTable::offsetOfCapacity() + 7);
static_assert(OBJECT_OFFSETOF(JSOrderedHashTable<MapTraits>, m_storage) == OBJECT_OFFSETOF(JSOrderedHashTable<SetTraits>, m_storage));

static constexpr uint32_t maxCountWithGrowthPlan = 64;
static constexpr std::array growthPlanValueWidths { CompactTable::Width::None, CompactTable::Width::Narrow, CompactTable::Width::Wide };

struct CompactTable::GrowthPlan {
    uint32_t capacity;
    uint8_t indexBits;
};

static WriteBarrier<JSCell>& storageOf(JSObject* owner)
{
    return static_cast<JSOrderedHashTable<MapTraits>*>(owner)->m_storage;
}

Structure* CompactTable::createStructure(VM& vm)
{
    return Structure::create(vm, nullptr, jsNull(), TypeInfo(CellType, StructureFlags), info());
}

Structure* CompactTable::structureOf(VM& vm)
{
    return vm.aotState().runtimeTable->compactTableStructure();
}

CompactTable* CompactTable::tableOf(JSObject* owner)
{
    // A VM with compiled code gives every Map and Set this storage, and add() never declines a key.
    JSCell* storage = storageOf(owner).get();
    RELEASE_ASSERT(!storage || static_cast<JSOrderedHashTable<MapTraits>*>(owner)->hasCompactStorage());
    return uncheckedDowncast<CompactTable>(storage);
}

void CompactTable::setStorage(VM& vm, JSObject* owner, JSCell* storage)
{
    storageOf(owner).set(vm, owner, storage);
}

unsigned CompactTable::indexBitsFor(uint32_t capacity)
{
    if (capacity <= maxCapacityWithTags)
        return 0;
    unsigned bits = 4;
    while ((static_cast<uint64_t>(1) << bits) * 3 < static_cast<uint64_t>(capacity) * 4)
        ++bits;
    return bits;
}

uint32_t CompactTable::capacityFor(uint32_t minCapacity, Width key, Width value)
{
    uint32_t capacity = roundUpToMultipleOf<2>(std::max<uint32_t>(minCapacity, 2));
    size_t size = MarkedSpace::optimalSizeFor(allocationSize(capacity, key, value));
    while (capacity < maxCapacity && allocationSize(capacity + 2, key, value) <= size)
        capacity += 2;
    return capacity;
}

uint32_t CompactTable::capacityAfterGrowth(uint32_t alive, uint32_t used)
{
    uint64_t deleted = used - alive;
    uint64_t count = alive;
    return std::min<uint64_t>(deleted >= used / 2 ? count + 1 : count < 64 ? count + count / 2 + 1 : count * 2, maxCapacity);
}

static unsigned growthPlanRowFor(CompactTable::Width value)
{
    switch (value) {
    case CompactTable::Width::None:
        return 0;
    case CompactTable::Width::Narrow:
        return 1;
    case CompactTable::Width::Wide:
        return 2;
    case CompactTable::Width::InImage:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

const CompactTable::GrowthPlan& CompactTable::growthPlanFor(Width value, uint32_t count)
{
    using GrowthPlans = std::array<std::array<GrowthPlan, maxCountWithGrowthPlan / 2>, growthPlanValueWidths.size()>;
    static const GrowthPlans plans = [] {
        GrowthPlans result { };
        for (Width width : growthPlanValueWidths) {
            auto& row = result[growthPlanRowFor(width)];
            for (uint32_t column = 0; column < row.size(); ++column) {
                uint32_t capacity = capacityFor(capacityAfterGrowth(column * 2, column * 2), Width::Narrow, width);
                RELEASE_ASSERT(capacity <= maxCapacityWithShortSlots);
                row[column] = { capacity, static_cast<uint8_t>(indexBitsFor(capacity)) };
            }
        }
        return result;
    }();
    return plans[growthPlanRowFor(value)][count / 2];
}

size_t CompactTable::offsetOfKeys(uint32_t capacity)
{
    return offsetOfIndex() + (static_cast<size_t>(2) << keysShiftFor(indexBitsFor(capacity), capacity));
}

size_t CompactTable::allocationSize(uint32_t capacity, Width key, Width value)
{
    return offsetOfKeys(capacity) + static_cast<size_t>(capacity) * (static_cast<size_t>(key) + static_cast<size_t>(value));
}

CompactTable* CompactTable::tryCreate(JSGlobalObject* globalObject, Width key, Width value, uint32_t minCapacity)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    RELEASE_ASSERT((key == Width::Narrow || key == Width::Wide) && value != Width::InImage);
    if (minCapacity > maxCapacity) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        return nullptr;
    }
    uint32_t capacity = capacityFor(minCapacity, key, value);
    size_t size = MarkedSpace::optimalSizeFor(allocationSize(capacity, key, value));
    void* cell = tryAllocateCell<CompactTable>(vm, size);
    if (!cell) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        return nullptr;
    }

    CompactTable* table = new (NotNull, cell) CompactTable(vm, structureOf(vm));
    table->m_capacity = capacity;
    table->m_indexBits = indexBitsFor(capacity);
    table->m_keysShiftAndFlags = keysShiftFor(table->m_indexBits, capacity) | (key == Width::Wide ? keysAreWide : 0);
    table->m_valueWidth = value;
    size_t keys = table->offsetOfKeys();
    memsetSpan(std::span { table->at<uint8_t>(offsetOfIndex()), keys - offsetOfIndex() }, table->slotKind() == SlotKind::Tag ? 0 : 0xff);
    zeroSpan(std::span { table->at<uint8_t>(keys), size - keys });
    table->finishCreation(vm);
    return table;
}

// A stub creates the first table of a Map or Set, and leaves that to the run time while the size has no allocator.
void CompactTable::ensureInitialTableAllocators(VM& vm)
{
    for (Width key : { Width::Narrow, Width::Wide }) {
        for (Width value : growthPlanValueWidths)
            vm.cellSpace().allocatorFor(initialSize(key, value), AllocatorForMode::EnsureAllocator);
    }
}

bool CompactTable::canHold(VM& vm, Width width, JSValue value)
{
    if (width != Width::Narrow || !value || value.isUndefinedOrNull() || value.isBoolean())
        return true;
    return value.isCell() && vm.heap.cage().contains(value.asCell()) && !(std::bit_cast<uintptr_t>(value.asCell()) & (MarkedBlock::atomSize - 1));
}

CompactTable::Width CompactTable::widthFor(VM& vm, JSValue value)
{
    return canHold(vm, Width::Narrow, value) ? Width::Narrow : Width::Wide;
}

JSValue CompactTable::keyAt(uint32_t entry) const
{
    RELEASE_ASSERT(entry < used());
    if (isInImage())
        return stateOfTableInImage().strings->string(vm(), tableInImage().stringAt(entry));
    return heapKeyAt(entry);
}

uint32_t CompactTable::encodeNarrow(JSValue value) const
{
    return value && value.isCell() ? MarkedBlockCage::encode(cageBaseOf(this), value.asCell()) : static_cast<uint32_t>(JSValue::encode(value));
}

void CompactTable::setKey(uint32_t entry, JSValue key)
{
    if (keyWidth() == Width::Wide)
        at<EncodedJSValue>(offsetOfKeys())[entry] = JSValue::encode(key);
    else
        at<uint32_t>(offsetOfKeys())[entry] = encodeNarrow(key);
}

void CompactTable::setValue(uint32_t entry, JSValue value)
{
    switch (m_valueWidth) {
    case Width::None:
        return;
    case Width::InImage:
        RELEASE_ASSERT_NOT_REACHED();
    case Width::Wide:
        at<EncodedJSValue>(offsetOfValues())[entry] = JSValue::encode(value);
        return;
    case Width::Narrow:
        at<uint32_t>(offsetOfValues())[entry] = encodeNarrow(value);
        return;
    }
}

unsigned CompactTable::hashOfStoredKey(JSValue key)
{
    if (key.isString())
        return asString(key)->tryGetValueImpl()->hash();
    if (key.isHeapBigInt())
        return jsMapHash(key.asHeapBigInt()) & hashMask;
    return rapidHashMix64(JSValue::encode(key)) & hashMask;
}

void CompactTable::insertIntoIndex(SlotKind kind, uint8_t* index, unsigned indexBits, uint32_t entry, unsigned hash)
{
    switch (kind) {
    case SlotKind::Tag:
        return insertIntoIndex<SlotKind::Tag>(index, indexBits, entry, hash);
    case SlotKind::Short:
        return insertIntoIndex<SlotKind::Short>(index, indexBits, entry, hash);
    case SlotKind::Long:
        return insertIntoIndex<SlotKind::Long>(index, indexBits, entry, hash);
    case SlotKind::Huge:
        return insertIntoIndex<SlotKind::Huge>(index, indexBits, entry, hash);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void CompactTable::addToIndex(uint32_t entry, unsigned hash)
{
    insertIntoIndex(slotKind(), at<uint8_t>(offsetOfIndex()), m_indexBits, entry, hash);
}

template<CompactTable::SlotKind kind>
void CompactTable::fillIndexOfKind(uint32_t count)
{
    uint8_t* index = at<uint8_t>(offsetOfIndex());
    unsigned indexBits = m_indexBits;
    for (uint32_t entry = 0; entry < count; ++entry)
        insertIntoIndex<kind>(index, indexBits, entry, hashOfStoredKey(heapKeyAt(entry)));
}

void CompactTable::fillIndex(uint32_t count)
{
    switch (slotKind()) {
    case SlotKind::Tag:
        return fillIndexOfKind<SlotKind::Tag>(count);
    case SlotKind::Short:
        return fillIndexOfKind<SlotKind::Short>(count);
    case SlotKind::Long:
        return fillIndexOfKind<SlotKind::Long>(count);
    case SlotKind::Huge:
        return fillIndexOfKind<SlotKind::Huge>(count);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

template<typename KeyWord, typename Word>
static uint32_t copyColumnOfAliveEntries(const KeyWord* keys, uint32_t used, const Word* from, Word* to)
{
    uint32_t count = 0;
    for (uint32_t entry = 0; entry < used; ++entry) {
        if (keys[entry])
            to[count++] = from[entry];
    }
    return count;
}

// The columns of the copy are as wide as the columns of this table.
uint32_t CompactTable::copyAliveEntriesTo(CompactTable& copy) const
{
    auto copyColumns = [&]<typename KeyWord>(const KeyWord* keys) {
        switch (m_valueWidth) {
        case Width::None:
            break;
        case Width::Narrow:
            copyColumnOfAliveEntries(keys, m_used, at<uint32_t>(offsetOfValues()), copy.at<uint32_t>(copy.offsetOfValues()));
            break;
        case Width::Wide:
            copyColumnOfAliveEntries(keys, m_used, at<EncodedJSValue>(offsetOfValues()), copy.at<EncodedJSValue>(copy.offsetOfValues()));
            break;
        case Width::InImage:
            RELEASE_ASSERT_NOT_REACHED();
        }
        return copyColumnOfAliveEntries(keys, m_used, keys, copy.at<KeyWord>(copy.offsetOfKeys()));
    };
    if (keyWidth() == Width::Wide)
        return copyColumns(at<EncodedJSValue>(offsetOfKeys()));
    return copyColumns(at<uint32_t>(offsetOfKeys()));
}

void CompactTable::append(VM& vm, JSValue key, unsigned hash, JSValue value)
{
    RELEASE_ASSERT(m_used < m_capacity);
    uint32_t entry = m_used;
    setKey(entry, key);
    setValue(entry, value);
    addToIndex(entry, hash);
    WTF::storeStoreFence();
    m_used = entry + 1;
    ++m_alive;
    vm.writeBarrier(this);
}

CompactTable* CompactTable::tryCopyWithWidths(JSGlobalObject* globalObject, Width key, Width value) const
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (m_alive >= maxCapacity) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        return nullptr;
    }
    CompactTable* copy = tryCreate(globalObject, key, value, capacityAfterGrowth(m_alive, m_used));
    RETURN_IF_EXCEPTION(scope, nullptr);
    uint32_t count = 0;
    bool keepsWidths = key == keyWidth() && value == m_valueWidth;
    if (!deleted() && keepsWidths) {
        count = m_used;
        memcpy(copy->at<uint8_t>(copy->offsetOfKeys()), at<uint8_t>(offsetOfKeys()), count * static_cast<size_t>(key));
        memcpy(copy->at<uint8_t>(copy->offsetOfValues()), at<uint8_t>(offsetOfValues()), count * static_cast<size_t>(value));
    } else if (keepsWidths)
        count = copyAliveEntriesTo(*copy);
    else {
        for (uint32_t entry = 0; entry < m_used; ++entry) {
            JSValue entryKey = heapKeyAt(entry);
            if (!entryKey)
                continue;
            copy->setKey(count, entryKey);
            copy->setValue(count, valueAt(entry));
            ++count;
        }
    }
    // Entries keep their numbers, and an index of the same form has them at the same positions.
    if (!deleted() && slotKind() == copy->slotKind() && m_indexBits == copy->m_indexBits)
        memcpy(copy->at<uint8_t>(offsetOfIndex()), at<uint8_t>(offsetOfIndex()), offsetOfKeys() - offsetOfIndex());
    else
        copy->fillIndex(count);
    WTF::storeStoreFence();
    copy->m_used = count;
    copy->m_alive = count;
    vm.writeBarrier(copy);
    return copy;
}

CompactTable* CompactTable::tryCopy(JSGlobalObject* globalObject) const
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (isInImage())
        return createInImage(vm, stateOfTableInImage(), tableInImage());
    size_t used = allocationSize(m_capacity, keyWidth(), m_valueWidth);
    size_t size = MarkedSpace::optimalSizeFor(used);
    void* cell = tryAllocateCell<CompactTable>(vm, size);
    if (!cell) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        return nullptr;
    }
    CompactTable* copy = new (NotNull, cell) CompactTable(vm, structure());
    copy->m_capacity = m_capacity;
    copy->m_indexBits = m_indexBits;
    copy->m_keysShiftAndFlags = m_keysShiftAndFlags;
    copy->m_valueWidth = m_valueWidth;
    memcpy(copy->at<uint8_t>(offsetOfIndex()), at<uint8_t>(offsetOfIndex()), used - offsetOfIndex());
    zeroSpan(std::span { copy->at<uint8_t>(used), size - used });
    copy->finishCreation(vm);
    WTF::storeStoreFence();
    copy->m_used = m_used;
    copy->m_alive = m_alive;
    vm.writeBarrier(copy);
    return copy;
}

void CompactTable::becomeObsolete(VM& vm, Status status, JSCell* next)
{
    RELEASE_ASSERT(isLive() && status != Status::Live);
    {
        Locker locker { cellLock() };
        uint32_t count = 0;
        if (status == Status::Replaced && deleted()) {
            for (uint32_t entry = 0; entry < m_used; ++entry) {
                if (!heapKeyAt(entry))
                    at<uint32_t>(offsetOfIndex())[count++] = entry;
            }
        }
        m_status = status;
        *at<JSCell*>(offsetOfUsed()) = next;
        m_capacity = count;
    }
    vm.writeBarrier(this);
}

CompactTable* CompactTable::copyWithAbsentKey(VM& vm, JSString* key, JSValue value) const
{
    uint32_t count = m_used;
    Width valueWidth = m_valueWidth;
    const GrowthPlan& plan = growthPlanFor(valueWidth, count);
    uint32_t capacity = plan.capacity;
    unsigned indexBits = plan.indexBits;
    uint8_t keysShift = keysShiftFor(indexBits, capacity);
    size_t keys = offsetOfIndex() + (static_cast<size_t>(2) << keysShift);
    size_t size = MarkedSpace::optimalSizeFor(keys + capacity * (static_cast<size_t>(Width::Narrow) + static_cast<size_t>(valueWidth)));
    CompactTable* larger = new (NotNull, allocateCell<CompactTable>(vm, size)) CompactTable(vm, structure());
    larger->m_capacity = capacity;
    larger->m_indexBits = indexBits;
    larger->m_keysShiftAndFlags = keysShift;
    larger->m_valueWidth = valueWidth;
    constexpr size_t maxInlineWords = 8;
    auto copyWords = [](uint64_t*& to, const uint64_t* from, size_t words) ALWAYS_INLINE_LAMBDA {
        if (words > maxInlineWords) {
            memcpy(to, from, words * sizeof(uint64_t));
            to += words;
            return;
        }
        for (size_t i = 0; i < words; ++i) {
            *to++ = from[i];
            WTF::compilerFence();
        }
    };
    auto fillWords = [](uint64_t*& to, uint64_t word, size_t words) ALWAYS_INLINE_LAMBDA {
        if (words > maxInlineWords) {
            memset(to, static_cast<int>(word & 0xff), words * sizeof(uint64_t));
            to += words;
            return;
        }
        for (size_t i = 0; i < words; ++i) {
            *to++ = word;
            WTF::compilerFence();
        }
    };
    unsigned hash = key->tryGetValueImpl()->existingHash();
    const uint64_t* from = at<uint64_t>(offsetOfKeys());
    uint64_t* to = larger->at<uint64_t>(offsetOfIndex());
    bool keepsSlots = indexBits && indexBits == m_indexBits;
    if (keepsSlots)
        copyWords(to, at<uint64_t>(offsetOfIndex()), (keys - offsetOfIndex()) / sizeof(uint64_t));
    else if (indexBits)
        fillWords(to, std::numeric_limits<uint64_t>::max(), (keys - offsetOfIndex()) / sizeof(uint64_t));
    else
        *to++ = *at<uint64_t>(offsetOfIndex()) | static_cast<uint64_t>(hash >> tagShiftOfTags) << (count * 8);
    const uint32_t* largerKeys = reinterpret_cast<const uint32_t*>(to);
    copyWords(to, from, count / 2);
    fillWords(to, 0, (capacity - count) / 2);
    copyWords(to, from + count / 2, count * static_cast<size_t>(valueWidth) / sizeof(uint64_t));
    fillWords(to, 0, larger->at<uint64_t>(size) - to);
    larger->finishCreation(vm);
    larger->setKey(count, key);
    larger->setValue(count, value);
    if (indexBits) {
        uint8_t* index = larger->at<uint8_t>(offsetOfIndex());
        for (uint32_t entry = keepsSlots ? count : 0; entry <= count; ++entry) {
            unsigned entryHash = entry == count ? hash : hashOfStoredKey(decodeNarrow(largerKeys[entry]));
            insertIntoIndex<SlotKind::Short>(index, indexBits, entry, entryHash);
        }
    }
    WTF::storeStoreFence();
    larger->m_used = count + 1;
    larger->m_alive = count + 1;
    vm.writeBarrier(larger);
    return larger;
}

bool CompactTable::add(JSGlobalObject* globalObject, JSObject* owner, JSValue key, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    key = normalizeMapKey(key);
    CompactTable* table = tableOf(owner);
    if (table->isInImage()) {
        if (!table->hasValues() && key.isString()) {
            auto entry = table->findInImage(globalObject, asString(key));
            RETURN_IF_EXCEPTION(scope, false);
            if (entry)
                return true;
        }
        table = tryMoveToHeap(globalObject, owner);
        RETURN_IF_EXCEPTION(scope, false);
    }
    auto replaceWith = [&](CompactTable* replacement) {
        table->becomeObsolete(vm, Status::Replaced, replacement);
        setStorage(vm, owner, replacement);
        table = replacement;
    };

    if (uint32_t count = table->m_used; key.isString() && count == table->m_capacity && count == table->m_alive && count < maxCountWithGrowthPlan
        && table->tryFindQuickly(asString(key)).result == QuickLookup::Absent && canHold(vm, table->m_valueWidth, value) && canHold(vm, Width::Narrow, key)) {
        CompactTable* larger = table->copyWithAbsentKey(vm, asString(key), value);
        if (validatesRuntime(vm)) [[unlikely]] {
            DeferTerminationForAWhile deferTermination(vm);
            CompactTable* expected = table->tryCopyWithWidths(globalObject, Width::Narrow, table->m_valueWidth);
            scope.releaseAssertNoException();
            expected->append(vm, key, hashOfStoredKey(key), value);
            size_t size = MarkedSpace::optimalSizeFor(allocationSize(larger->m_capacity, Width::Narrow, larger->m_valueWidth));
            if (expected->m_capacity != larger->m_capacity || memcmp(expected->at<uint8_t>(sizeof(JSCell)), larger->at<uint8_t>(sizeof(JSCell)), size - sizeof(JSCell)))
                crashOnValidationFailure(vm, { ValidationFailureKind::FastPathResultDiffers, { }, { }, larger });
        }
        replaceWith(larger);
        return true;
    }

    auto entry = table->find(globalObject, key);
    RETURN_IF_EXCEPTION(scope, false);
    if (entry) {
        if (!canHold(vm, table->m_valueWidth, value)) [[unlikely]] {
            uint32_t deletedBefore = 0;
            for (uint32_t i = 0; i < *entry; ++i)
                deletedBefore += !table->heapKeyAt(i);
            CompactTable* wider = table->tryCopyWithWidths(globalObject, table->keyWidth(), Width::Wide);
            RETURN_IF_EXCEPTION(scope, false);
            replaceWith(wider);
            *entry -= deletedBefore;
        }
        table->setValue(*entry, value);
        vm.writeBarrier(table);
        return true;
    }

    if (key.isString()) {
        asString(key)->value(globalObject);
        RETURN_IF_EXCEPTION(scope, false);
    }
    unsigned hash = hashOfStoredKey(key);
    Width keyWidth = canHold(vm, table->keyWidth(), key) ? table->keyWidth() : Width::Wide;
    Width valueWidth = canHold(vm, table->m_valueWidth, value) ? table->m_valueWidth : Width::Wide;
    if (table->m_used == table->m_capacity || keyWidth != table->keyWidth() || valueWidth != table->m_valueWidth) {
        CompactTable* replacement = table->tryCopyWithWidths(globalObject, keyWidth, valueWidth);
        RETURN_IF_EXCEPTION(scope, false);
        replaceWith(replacement);
    }
    table->append(vm, key, hash, value);
    return true;
}

bool CompactTable::remove(JSGlobalObject* globalObject, JSObject* owner, JSValue key)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    CompactTable* table = tableOf(owner);
    auto entry = table->find(globalObject, normalizeMapKey(key));
    RETURN_IF_EXCEPTION(scope, false);
    if (!entry)
        return false;
    if (table->isInImage()) {
        table = tryMoveToHeap(globalObject, owner);
        RETURN_IF_EXCEPTION(scope, false);
    }
    table->setKey(*entry, JSValue());
    table->setValue(*entry, JSValue());
    --table->m_alive;
    return true;
}

void CompactTable::clear(JSGlobalObject* globalObject, JSObject* owner)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    CompactTable* table = tableOf(owner);
    JSCell* fresh = CompactStorage::tryCreate(globalObject, table->hasValues());
    RETURN_IF_EXCEPTION(scope, void());
    table->becomeObsolete(vm, Status::Cleared, fresh);
    setStorage(vm, owner, fresh);
}

template<typename Traits>
static JSCellButterfly* tryCreateOrdinaryStorage(JSGlobalObject* globalObject, const CompactTable& table)
{
    using Helper = JSOrderedHashTableHelper<Traits>;
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    typename Helper::TableSize capacity = Helper::InitialCapacity;
    while (Helper::dataCapacity(capacity) <= table.size())
        capacity *= 2;
    JSCellButterfly* storage = Helper::tryCreate(globalObject, 0, 0, capacity);
    RETURN_IF_EXCEPTION(scope, nullptr);
    for (uint32_t entry = 0; entry < table.used(); ++entry) {
        JSValue key = table.keyAt(entry);
        if (!key)
            continue;
        Helper::add(globalObject, nullptr, *storage, key, table.valueAt(entry));
        RETURN_IF_EXCEPTION(scope, nullptr);
    }
    return storage;
}

JSCellButterfly* CompactTable::convertToOrdinaryStorage(JSGlobalObject* globalObject, CompactTable* table)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSCellButterfly* storage = table->hasValues() ? tryCreateOrdinaryStorage<MapTraits>(globalObject, *table) : tryCreateOrdinaryStorage<SetTraits>(globalObject, *table);
    RETURN_IF_EXCEPTION(scope, nullptr);
    table->becomeObsolete(vm, Status::Replaced, storage);
    return storage;
}

CompactStorage::Position CompactTable::next(VM&, JSCell* storage, uint32_t entry)
{
    for (;;) {
        if (!storage)
            return { };
        if (storage->type() == JSCellButterflyType)
            return { storage, entry, { }, { } };
        auto* table = uncheckedDowncast<CompactTable>(storage);
        switch (table->m_status) {
        case Status::Live:
            for (; entry < table->m_used; ++entry) {
                if (JSValue key = table->keyAt(entry))
                    return { table, entry, key, table->valueAt(entry) };
            }
            return { };
        case Status::Cleared:
            storage = table->successor();
            entry = 0;
            break;
        case Status::Replaced: {
            uint32_t deletedBefore = 0;
            while (deletedBefore < table->numberOfDeletedEntries() && table->deletedEntries()[deletedBefore] < entry)
                ++deletedBefore;
            entry -= deletedBefore;
            storage = table->successor();
            break;
        }
        }
    }
}

template<typename Visitor>
void CompactTable::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    Base::visitChildren(cell, visitor);
    visitFields(uncheckedDowncast<CompactTable>(cell), visitor);
}

DEFINE_VISIT_CHILDREN(CompactTable);

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
