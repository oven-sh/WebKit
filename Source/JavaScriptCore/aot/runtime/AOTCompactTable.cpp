/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCompactTable.h"

#if ENABLE(AOT)

#include "AOTImage.h"
#include "AOTInstance.h"
#include "AOTOperationScope.h"
#include "AOTProgramStrings.h"
#include "AOTRuntimeCounters.h"
#include "AOTRuntimeFunctions.h"
#include "AOTRuntimeTable.h"
#include "AOTRuntimeValidation.h"
#include "AOTSchemaRuntimeOperations.h"
#include "AOTState.h"
#include "AOTStringBuilderStorage.h"
#include "AOTTableInImage.h"
#include "AOTVector.h"
#include "DeferGCInlines.h"
#include "DeferTermination.h"
#include "HashMapHelper.h"
#include "JSBigInt.h"
#include "JSCInlines.h"
#include "JSCellButterfly.h"
#include "JSMapInlines.h"
#include "JSSetInlines.h"
#include "JSWeakMapInlines.h"
#include "TopExceptionScope.h"
#include "WeakMapImplInlines.h"
#include <algorithm>
#include <array>
#include <limits>
#include <wtf/MathExtras.h>
#include <wtf/text/StringHasherInlines.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

const ClassInfo CompactTable::s_info = { "CompactTable"_s, nullptr, nullptr, nullptr, CREATE_METHOD_TABLE(CompactTable) };

static_assert(CompactTable::offsetOfAliveCount() == CompactTable::offsetOfUsed() + 4 && CompactTable::offsetOfIndexBits() == CompactTable::offsetOfCapacity() + 4);
static_assert(CompactTable::offsetOfKeysShift() == CompactTable::offsetOfCapacity() + 5 && CompactTable::offsetOfValueWidth() == CompactTable::offsetOfCapacity() + 6 && CompactTable::offsetOfStatus() == CompactTable::offsetOfCapacity() + 7);
static_assert(OBJECT_OFFSETOF(JSOrderedHashTable<MapTraits>, m_storage) == OBJECT_OFFSETOF(JSOrderedHashTable<SetTraits>, m_storage));

static constexpr std::array growthPlanValueWidths { CompactTable::Width::None, CompactTable::Width::Narrow, CompactTable::Width::Wide };

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

uint32_t& CompactTable::deletedPrefixOf(JSObject* owner)
{
    return static_cast<JSOrderedHashTable<MapTraits>*>(owner)->m_deletedPrefix;
}

void CompactTable::setStorage(VM& vm, JSObject* owner, JSCell* storage)
{
    storageOf(owner).set(vm, owner, storage);
    deletedPrefixOf(owner) = 0;
}

unsigned CompactTable::indexBitsFor(uint32_t capacity)
{
    static_assert((static_cast<uint64_t>(1) << 28) * 3 >= static_cast<uint64_t>(maxCapacity) * 4 && keysShiftFor(28, maxCapacity) <= 30);
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

static constexpr unsigned firstGrowthPlanFor(CompactTable::Width value)
{
    return static_cast<unsigned>(value) / static_cast<unsigned>(CompactTable::Width::Narrow) * CompactTable::growthPlansPerValueWidth;
}

const uint64_t* CompactTable::growthPlans()
{
    using GrowthPlans = std::array<uint64_t, growthPlanValueWidths.size() * growthPlansPerValueWidth>;
    static const GrowthPlans plans = [] {
        GrowthPlans result { };
        for (Width width : growthPlanValueWidths) {
            for (uint32_t column = 0; column < growthPlansPerValueWidth; ++column) {
                uint32_t capacity = capacityFor(capacityAfterGrowth(column * 2, column * 2), Width::Narrow, width);
                uint64_t indexBits = indexBitsFor(capacity);
                uint64_t sizeStep = MarkedSpace::sizeClassToIndex(allocationSize(capacity, Width::Narrow, width));
                RELEASE_ASSERT(capacity <= maxCapacityWithShortSlots && sizeStep < 1u << (64 - sizeStepShiftOfGrowthPlan) && allocationSize(capacity, Width::Narrow, width) <= MarkedSpace::largeCutoff);
                result[firstGrowthPlanFor(width) + column] = capacity | indexBits << 32 | static_cast<uint64_t>(keysShiftFor(indexBits, capacity)) << 40 | static_cast<uint64_t>(width) << 48 | sizeStep << sizeStepShiftOfGrowthPlan;
            }
        }
        return result;
    }();
    return plans.data();
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
    table->m_indexBits = markedIndexBits(indexBitsFor(capacity));
    table->m_keysShiftAndFlags = keysShiftFor(table->indexBits(), capacity) | (key == Width::Wide ? keysAreWide : 0);
    table->m_valueWidth = value;
    size_t keys = table->offsetOfKeys();
    if (table->slotKind() == SlotKind::Tag)
        *table->at<uint64_t>(offsetOfIndex()) = 0;
    else
        zeroSpan(std::span { table->at<uint8_t>(offsetOfIndex()), keys - offsetOfIndex() });
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

CompactTable* CompactTable::tryCreateForFirstEntry(JSGlobalObject* globalObject, bool hasValues, JSValue key, JSValue value)
{
    VM& vm = globalObject->vm();
    bool useWideReferences = vm.aotState().image->header().options.useWideReferences();
    auto widthOf = [&](JSValue held) {
        return useWideReferences ? Width::Wide : widthFor(vm, held);
    };
    return tryCreate(globalObject, widthOf(normalizeMapKey(key)), hasValues ? widthOf(value) : Width::None, initialCapacity);
}

CompactTable::Width CompactTable::widthFor(VM& vm, JSValue value)
{
    return canHold(vm, Width::Narrow, value) ? Width::Narrow : Width::Wide;
}

JSValue CompactTable::keyAt(uint32_t entry) const
{
    RELEASE_ASSERT(entry < used());
    if (isInImage())
        return stateOfTableInImage().strings->constant(vm(), tableInImage().stringConstants()[entry]);
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

uint32_t CompactTable::nextSlotWithTag(const uint8_t* tags, unsigned indexBits, uint8_t tag, uint32_t homeSlot, uint32_t previous)
{
    uint32_t mask = (1u << indexBits) - 1;
    uint32_t first = (previous == noSlot ? homeSlot : previous) & ~(slotsPerGroup - 1);
    unsigned skipped = previous == noSlot ? 0 : (previous & (slotsPerGroup - 1)) + 1;
    while (true) {
        uint64_t eightTags = WTF::unalignedLoad<uint64_t>(tags + first);
        uint64_t flags = slotsWithTagAmong(eightTags, tag);
        flags = skipped == slotsPerGroup ? 0 : flags >> (skipped * 8) << (skipped * 8);
        for (; flags; flags &= flags - 1) {
            uint32_t slot = first + WTF::ctz(flags) / 8;
            if (slot != homeSlot)
                return slot;
        }
        if (freeSlotsAmong(eightTags))
            return noSlot;
        first = (first + slotsPerGroup) & mask;
        skipped = 0;
    }
}

uint32_t CompactTable::findFreeSlotBehindHome(const uint8_t* tags, unsigned indexBits, uint32_t homeSlot)
{
    uint32_t mask = (1u << indexBits) - 1;
    for (uint32_t first = homeSlot & ~(slotsPerGroup - 1);; first = (first + slotsPerGroup) & mask) {
        if (uint64_t free = freeSlotsAmong(WTF::unalignedLoad<uint64_t>(tags + first)))
            return first + WTF::ctz(free) / 8;
    }
}

void CompactTable::insertIntoIndex(SlotKind kind, uint8_t* index, unsigned indexBits, uint32_t entry, unsigned hash)
{
    withSlotKind(kind, [&](auto constantKind) ALWAYS_INLINE_LAMBDA {
        insertIntoIndex<constantKind()>(index, indexBits, entry, hash);
    });
}

void CompactTable::addToIndex(uint32_t entry, unsigned hash)
{
    if (slotKind() == SlotKind::Tag)
        insertIntoIndex<SlotKind::Tag>(at<uint8_t>(offsetOfIndex()), 0, entry, hash);
    else
        insertIntoIndex(slotKind(), at<uint8_t>(offsetOfIndex()), indexBits(), entry, hash);
}

void CompactTable::removeFromIndex(uint32_t entry, unsigned hash)
{
    SlotKind kind = slotKind();
    if (kind == SlotKind::Tag)
        return;
    uint8_t* tags = at<uint8_t>(offsetOfIndex());
    unsigned indexBits = this->indexBits();
    uint32_t homeSlot = probeStartFor(indexBits, hash);
    uint8_t tag = tagOfSlot(hash);
    // nextSlotWithTag() may name the slot above one with the tag too.
    auto isSlotOfEntry = [&](uint32_t slot) { return hasTag(tags[slot], tag) && entryAt(kind, tags, indexBits, slot) == entry; };
    uint32_t slot = homeSlot;
    if (!isSlotOfEntry(slot)) {
        do {
            slot = nextSlotWithTag(tags, indexBits, tag, homeSlot, slot == homeSlot ? noSlot : slot);
            RELEASE_ASSERT(slot != noSlot);
        } while (!isSlotOfEntry(slot));
    }
    if (canFreeSlot(tags, slot))
        tags[slot] = 0;
}

uint32_t CompactTable::numberOfTakenSlots() const
{
    RELEASE_ASSERT(isHeapTable() && slotKind() != SlotKind::Tag);
    return static_cast<uint32_t>(std::ranges::count_if(std::span { at<uint8_t>(offsetOfIndex()), static_cast<size_t>(1) << indexBits() }, [](uint8_t tag) { return !!tag; }));
}

void CompactTable::fillIndex(uint32_t count)
{
    uint8_t* index = at<uint8_t>(offsetOfIndex());
    unsigned indexBits = this->indexBits();
    withSlotKind(slotKind(), [&](auto kind) ALWAYS_INLINE_LAMBDA {
        for (uint32_t entry = 0; entry < count; ++entry)
            insertIntoIndex<kind()>(index, indexBits, entry, hashOfStoredKey(heapKeyAt(entry)));
    });
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

CompactTable* CompactTable::tryCopyWithWidths(JSGlobalObject* globalObject, Width key, Width value, std::optional<uint32_t> capacity) const
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (m_alive >= maxCapacity) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        return nullptr;
    }
    ASSERT(!capacity || *capacity > m_alive);
    CompactTable* copy = tryCreate(globalObject, key, value, capacity.value_or(capacityAfterGrowth(m_alive, m_used)));
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
    copy->m_keysShiftAndFlags = m_keysShiftAndFlags & ~wasWalked;
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
        // Whoever stands in this table goes on at the same entry of the next.
        if (status == Status::Replaced)
            uncheckedDowncast<CompactTable>(next)->m_keysShiftAndFlags |= m_keysShiftAndFlags & wasWalked;
        m_status = status;
        *at<JSCell*>(offsetOfUsed()) = next;
        m_capacity = count;
    }
    vm.writeBarrier(this);
}

// The caller holds the lock of the cell. Whoever stands at an entry of this table comes to the same entry as by way of every table that is left out.
void CompactTable::skipObsoleteSuccessors()
{
    // An entry has at least four bytes behind the index, so no entry of this table has a number as large as this.
    uint32_t room = static_cast<uint32_t>((cellSize() - offsetOfIndex()) / sizeof(uint32_t));
    Vector<uint32_t, 32> merged;
    // The entries of such a table are not in the cell.
    if (m_keysShiftAndFlags & keysAreInImage)
        return;
    while (m_status == Status::Replaced) {
        // A stub replaces a table without the lock, and stores the successor last.
        WTF::loadLoadFence();
        auto* next = uncheckedDowncast<CompactTable>(successor());
        if (!next)
            return;
        Locker locker { next->cellLock() };
        if (next->m_status == Status::Live)
            return;
        WTF::loadLoadFence();
        if (!next->successor())
            return;
        if (next->m_status == Status::Cleared) {
            m_status = Status::Cleared;
            numberOfDeletedEntries() = 0;
            successor() = next->successor();
            return;
        }
        std::span<const uint32_t> own { deletedEntries(), numberOfDeletedEntries() };
        std::span<const uint32_t> later { next->deletedEntries(), next->numberOfDeletedEntries() };
        // Nothing is left of this table, or the next one lost nothing: the list stays as it is.
        if (own.size() == room || later.empty()) {
            successor() = next->successor();
            continue;
        }
        merged.shrink(0);
        size_t passed = 0;
        for (uint32_t entryOfNext : later) {
            uint64_t entry = static_cast<uint64_t>(entryOfNext) + passed;
            for (; passed < own.size() && own[passed] <= entry; ++passed, ++entry)
                merged.append(own[passed]);
            if (entry >= room)
                break;
            merged.append(static_cast<uint32_t>(entry));
        }
        merged.append(own.subspan(passed));
        RELEASE_ASSERT(merged.size() <= room);
        memcpySpan(std::span { deletedEntries(), merged.size() }, merged.span());
        numberOfDeletedEntries() = static_cast<uint32_t>(merged.size());
        successor() = next->successor();
    }
}

CompactTable* CompactTable::copyWithAbsentKey(VM& vm, JSValue key, unsigned hash, JSValue value) const
{
    uint32_t count = m_used;
    Width valueWidth = m_valueWidth;
    uint64_t plan = growthPlans()[firstGrowthPlanFor(valueWidth) + count / 2];
    uint32_t capacity = static_cast<uint32_t>(plan);
    unsigned indexBits = static_cast<uint8_t>(plan >> 32);
    uint8_t keysShift = keysShiftFor(indexBits, capacity);
    size_t keys = offsetOfIndex() + (static_cast<size_t>(2) << keysShift);
    size_t size = MarkedSpace::optimalSizeFor(keys + capacity * (static_cast<size_t>(Width::Narrow) + static_cast<size_t>(valueWidth)));
    CompactTable* larger = new (NotNull, allocateCell<CompactTable>(vm, size)) CompactTable(vm, structure());
    larger->m_capacity = capacity;
    larger->m_indexBits = indexBits;
    larger->m_keysShiftAndFlags = keysShift;
    larger->m_valueWidth = valueWidth;
    AOT_THRESHOLD(size_t, maxInlineWords, 8);
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
    const uint64_t* from = at<uint64_t>(offsetOfKeys());
    uint64_t* to = larger->at<uint64_t>(offsetOfIndex());
    bool keepsSlots = indexBits && indexBits == m_indexBits;
    if (keepsSlots)
        copyWords(to, at<uint64_t>(offsetOfIndex()), (keys - offsetOfIndex()) / sizeof(uint64_t));
    else if (indexBits)
        fillWords(to, 0, (keys - offsetOfIndex()) / sizeof(uint64_t));
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

// What canHold() says of a narrow column, as the stubs test it: every cell of a marked block lies in the cage, and no other cell is aligned like one.
static ALWAYS_INLINE bool narrowColumnHolds(JSValue value)
{
    static_assert((MarkedBlock::atomSize - 1) & PreciseAllocation::halfAlignment);
    uint64_t bits = JSValue::encode(value);
    return value.isCell() ? !(bits & (MarkedBlock::atomSize - 1)) : bits < minCell32;
}

ALWAYS_INLINE std::optional<unsigned> CompactTable::hashIfAbsent(JSValue key) const
{
    if (key.isString()) {
        if (tryFindQuickly(asString(key)).result != QuickLookup::Absent)
            return std::nullopt;
        return asString(key)->tryGetValueImpl()->existingHash();
    }
    if (!hasNarrowHeapKeys() || !narrowColumnHolds(key) || key.isHeapBigInt())
        return std::nullopt;
    unsigned hash = rapidHashMix64(JSValue::encode(key)) & hashMask;
    uint32_t narrowKey = encodeNarrow(key);
    const uint32_t* keys = at<uint32_t>(offsetOfKeys());
    auto found = findWithHash(hash, [&](uint32_t entry) ALWAYS_INLINE_LAMBDA {
        return keys[entry] == narrowKey;
    });
    return found ? std::nullopt : std::optional(hash);
}

// The usual addition: the table has room, its columns hold the key and the value, and the key is an object, a symbol or a string that is no rope, so it is found or missed without running anything.
ALWAYS_INLINE bool CompactTable::tryAddQuickly(JSGlobalObject* globalObject, JSValue key, JSValue value)
{
    if (!hasNarrowHeapKeys() || m_used == m_capacity || !key.isCell() || !narrowColumnHolds(key) || (m_valueWidth == Width::Narrow && !narrowColumnHolds(value)))
        return false;
    VM& vm = globalObject->vm();
    uintptr_t base = cageBaseOf(this);
    uint32_t narrowKey = MarkedBlockCage::encode(base, key.asCell());
    uint32_t* keys = at<uint32_t>(offsetOfKeys());
    unsigned hash;
    std::optional<uint32_t> found;
    if (key.isString()) {
        const StringImpl* impl = asString(key)->tryGetValueImpl();
        if (!impl)
            return false;
        hash = impl->hash();
        found = findWithHash(hash, [&](uint32_t entry) ALWAYS_INLINE_LAMBDA {
            uint32_t candidate = keys[entry];
            if (candidate == narrowKey)
                return true;
            const StringImpl* other = stringKeyOrNull(base, candidate);
            return other && (other == impl || (other->existingHash() == hash && equal(other, impl)));
        });
    } else {
        if (key.isHeapBigInt())
            return false;
        hash = rapidHashMix64(JSValue::encode(key)) & hashMask;
        found = findWithHash(hash, [&](uint32_t entry) ALWAYS_INLINE_LAMBDA {
            return keys[entry] == narrowKey;
        });
    }
    if (validatesRuntime(vm)) [[unlikely]] {
        DeferTerminationForAWhile deferTermination(vm);
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
        std::optional<uint32_t> expected = find(globalObject, key);
        scope.releaseAssertNoException();
        if (expected != found || hash != hashOfStoredKey(key) || !canHold(vm, Width::Narrow, key) || !canHold(vm, m_valueWidth, value))
            crashOnValidationFailure(vm, { ValidationFailureKind::FastPathResultDiffers, { }, { }, this });
    }
    uint32_t entry = found.value_or(m_used);
    switch (m_valueWidth) {
    case Width::None:
        break;
    case Width::Narrow:
        at<uint32_t>(offsetOfValues())[entry] = value.isCell() ? MarkedBlockCage::encode(base, value.asCell()) : static_cast<uint32_t>(JSValue::encode(value));
        break;
    case Width::Wide:
        at<EncodedJSValue>(offsetOfValues())[entry] = JSValue::encode(value);
        break;
    case Width::InImage:
        RELEASE_ASSERT_NOT_REACHED();
    }
    if (!found) {
        keys[entry] = narrowKey;
        addToIndex(entry, hash);
        WTF::storeStoreFence();
        m_used = entry + 1;
        ++m_alive;
    }
    vm.writeBarrier(this);
    return true;
}

// One hash serves the search for an equal key, whose StringImpl the substring then shares, and the StringImpl that is created otherwise.
ALWAYS_INLINE void CompactTable::resolveSubstringKey(JSRopeString* key) const
{
    StringImpl& base = *key->substringBase()->tryGetValueImpl();
    unsigned offset = key->substringOffset();
    unsigned length = key->length();
    auto resolve = [&]<typename CharacterType>(std::span<const CharacterType> characters) ALWAYS_INLINE_LAMBDA {
        unsigned hash = StringHasher::computeHashAndMaskTop8Bits(characters);
        if (Lookup lookup = tryFindCharactersQuickly(characters, hash); lookup.result == QuickLookup::Found) {
            StringImpl& equal = *asString(heapKeyAt(lookup.entry))->tryGetValueImpl();
            if (equal.is8Bit() || !key->is8Bit()) {
                key->resolveSubstringBySharing(equal);
                return;
            }
        }
        key->resolveSubstringBySharing(length == base.length() ? base : StringImpl::createSubstringSharingImpl(base, offset, length, hash).get());
    };
    if (base.is8Bit())
        resolve(base.span8().subspan(offset, length));
    else
        resolve(base.span16().subspan(offset, length));
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
    // A stored key is resolved, so a rope is resolved before the first search instead of between two.
    if (key.isString() && asString(key)->isRope()) {
        if (asString(key)->isSubstring())
            table->resolveSubstringKey(uncheckedDowncast<JSRopeString>(asString(key)));
        else {
            asString(key)->value(globalObject);
            RETURN_IF_EXCEPTION(scope, false);
        }
    }
    auto replaceWith = [&](CompactTable* replacement) {
        table->becomeObsolete(vm, Status::Replaced, replacement);
        setStorage(vm, owner, replacement);
        table = replacement;
    };

    auto tryAddWithoutSearch = [&] {
        if (table->tryAddQuickly(globalObject, key, value))
            return true;
        uint32_t count = table->m_used;
        if (count != table->m_capacity || count != table->m_alive || count >= maxCountWithGrowthPlan || !canHold(vm, table->m_valueWidth, value) || !canHold(vm, Width::Narrow, key))
            return false;
        std::optional<unsigned> hash = table->hashIfAbsent(key);
        if (!hash)
            return false;
        CompactTable* larger = table->copyWithAbsentKey(vm, key, *hash, value);
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
    };
    if (tryAddWithoutSearch())
        return true;

    auto entry = table->find(globalObject, key);
    RETURN_IF_EXCEPTION(scope, false);
    if (entry) {
        if (!canHold(vm, table->m_valueWidth, value)) [[unlikely]] {
            uint32_t deletedBefore = 0;
            for (uint32_t i = 0; i < *entry; ++i)
                deletedBefore += !table->heapKeyAt(i);
            CompactTable* wider = table->tryCopyWithWidths(globalObject, table->keyWidth(), Width::Wide, table->m_used < table->m_capacity ? std::optional { table->m_capacity } : std::nullopt);
            RETURN_IF_EXCEPTION(scope, false);
            replaceWith(wider);
            *entry -= deletedBefore;
        }
        table->setValue(*entry, value);
        vm.writeBarrier(table);
        return true;
    }

    unsigned hash = hashOfStoredKey(key);
    Width keyWidth = canHold(vm, table->keyWidth(), key) ? table->keyWidth() : Width::Wide;
    Width valueWidth = canHold(vm, table->m_valueWidth, value) ? table->m_valueWidth : Width::Wide;
    if (table->m_used == table->m_capacity || keyWidth != table->keyWidth() || valueWidth != table->m_valueWidth) {
        // A table that has room is only widened, and keeps the room.
        CompactTable* replacement = table->tryCopyWithWidths(globalObject, keyWidth, valueWidth, table->m_used < table->m_capacity ? std::optional { table->m_capacity } : std::nullopt);
        RETURN_IF_EXCEPTION(scope, false);
        replaceWith(replacement);
    }
    table->append(vm, key, hash, value);
    return true;
}

void CompactTable::reserveForAdditions(JSGlobalObject* globalObject, JSObject* owner, uint64_t remaining, uint64_t finished, uint64_t sizeAtEntry)
{
    VM& vm = globalObject->vm();
    if (!storageOf(owner) || !static_cast<JSOrderedHashTable<MapTraits>*>(owner)->hasCompactStorage())
        return;
    CompactTable* table = tableOf(owner);
    if (!table->isHeapTable() || table->m_used < table->m_capacity || !finished)
        return;
    if (remaining > runtimeLimitValue(vm, LimitID::MaxReservedCollectionCapacity)) {
        countLimitHit(vm, LimitID::MaxReservedCollectionCapacity);
        return;
    }

    // An addition that is skipped, or whose key is there, adds nothing.
    uint64_t alive = table->m_alive;
    uint64_t added = alive > sizeAtEntry ? alive - sizeAtEntry : 0;
    bool everyAdditionAddedAnEntry = added >= finished;
    uint64_t expected = everyAdditionAddedAnEntry ? remaining : (remaining * added + finished - 1) / finished;
    uint64_t capacity = std::min(alive + expected, alive * largestStepInLoop);
    if (!expected || capacity > maxCapacity || (!everyAdditionAddedAnEntry && capacity <= capacityAfterGrowth(table->m_alive, table->m_used)))
        return;

    DeferTerminationForAWhile deferTermination(vm);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    CompactTable* replacement = table->tryCopyWithWidths(globalObject, table->keyWidth(), table->m_valueWidth, static_cast<uint32_t>(capacity));
    if (scope.exception()) [[unlikely]] {
        scope.clearException();
        return;
    }
    table->becomeObsolete(vm, Status::Replaced, replacement);
    setStorage(vm, owner, replacement);
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
    table->removeFromIndex(*entry, hashOfStoredKey(table->heapKeyAt(*entry)));
    table->setKey(*entry, JSValue());
    table->setValue(*entry, JSValue());
    --table->m_alive;
    if (isMostlyDeleted(table->m_alive, table->m_used))
        shrink(globalObject, owner);
    return true;
}

// Whoever stands in the table goes on at the same entry of the smaller one, as behind a growth. Without memory for it the table stays.
void CompactTable::shrink(JSGlobalObject* globalObject, JSObject* owner)
{
    VM& vm = globalObject->vm();
    CompactTable* table = tableOf(owner);
    DeferTerminationForAWhile deferTermination(vm);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    CompactTable* replacement = table->tryCopyWithWidths(globalObject, table->keyWidth(), table->m_valueWidth, std::max(table->m_alive * 2, initialCapacity));
    if (scope.exception()) [[unlikely]] {
        scope.clearException();
        return;
    }
    table->becomeObsolete(vm, Status::Replaced, replacement);
    setStorage(vm, owner, replacement);
}

void CompactTable::removeAllEntries()
{
    ASSERT(isHeapTable());
    Locker locker { cellLock() };
    m_used = 0;
    m_alive = 0;
    if (slotKind() == SlotKind::Tag) {
        *at<uint64_t>(offsetOfIndex()) = 0;
        return;
    }
    zeroSpan(std::span { at<uint8_t>(offsetOfIndex()), offsetOfKeys() - offsetOfIndex() });
}

bool CompactTable::tryClearInPlace(JSObject* owner)
{
    CompactTable* table = tableOf(owner);
    if (!table || !table->isHeapTable() || table->m_keysShiftAndFlags & wasWalked || table->m_capacity > maxCapacityEmptiedByClear)
        return false;
    table->removeAllEntries();
    deletedPrefixOf(owner) = 0;
    return true;
}

void CompactTable::clear(JSGlobalObject* globalObject, JSObject* owner)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    CompactTable* table = tableOf(owner);
    // Nothing was added since the table was made, so whoever iterates over it stands in front of all that is added later.
    if (!table->m_used)
        return;
    // Nobody stands behind an entry, so the table is as good as a new one. A large one is let go: a collection that is cleared for good gives its memory back.
    if (tryClearInPlace(owner))
        return;
    // To whoever iterates, clearing is the deletion of every entry. A small table that has room for as many entries again stays, so a collection that is filled and cleared in turns makes fewer tables.
    if (table->isHeapTable() && table->m_capacity <= maxCapacityWithTags && table->m_used * 2 <= table->m_capacity) {
        for (uint32_t entry = 0; entry < table->m_used; ++entry) {
            table->setKey(entry, JSValue());
            table->setValue(entry, JSValue());
        }
        table->m_alive = 0;
        return;
    }
    // What is added next is most likely of the kind that was there.
    JSCell* fresh = table->isHeapTable() ? tryCreate(globalObject, table->keyWidth(), table->m_valueWidth, initialCapacity) : CompactStorage::tryCreate(globalObject, table->hasValues());
    RETURN_IF_EXCEPTION(scope, void());
    // Whoever stands in a cleared table goes on at the start of the table that the owner has by then.
    table->becomeObsolete(vm, Status::Cleared, owner);
    setStorage(vm, owner, fresh);
}

CompactStorage::Position CompactTable::next(VM&, JSCell* storage, uint32_t entry)
{
    for (;;) {
        if (!storage)
            return { };
        auto* table = uncheckedDowncast<CompactTable>(storage);
        switch (table->m_status) {
        case Status::Live:
            for (; entry < table->m_used; ++entry) {
                if (JSValue key = table->keyAt(entry)) {
                    table->m_keysShiftAndFlags |= wasWalked;
                    return { table, entry, key, table->valueAt(entry) };
                }
            }
            return { };
        case Status::Cleared:
        case Status::Replaced: {
            // The collector moves the successor of such a table on.
            Locker locker { table->cellLock() };
            if (table->m_status == Status::Cleared) {
                storage = tableOf(asObject(table->successor()));
                entry = 0;
                break;
            }
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

static ALWAYS_INLINE const StringImpl* stringImplOrNull(JSValue storedKey)
{
    return storedKey && storedKey.isString() ? asString(storedKey)->tryGetValueImpl() : nullptr;
}

template<typename CharacterType>
ALWAYS_INLINE std::optional<uint32_t> CompactTable::findCharacters(std::span<const CharacterType> characters) const
{
    unsigned hash = StringHasher::computeHashAndMaskTop8Bits(characters);
    if (Lookup lookup = tryFindCharactersQuickly(characters, hash); lookup.result != QuickLookup::Unknown)
        return lookup.result == QuickLookup::Found ? std::optional { lookup.entry } : std::nullopt;
    return findWithHash(hash, [&](uint32_t entry) ALWAYS_INLINE_LAMBDA {
        const StringImpl* impl = stringImplOrNull(heapKeyAt(entry));
        return impl && impl->existingHash() == hash && equalsCharacters(*impl, characters);
    });
}

template<typename CharacterType>
static std::optional<uint32_t> findCharactersInImage(const Image& image, const TableInImage& entries, std::span<const CharacterType> characters)
{
    return entries.find(image, StringView { characters }, StringHasher::computeHashAndMaskTop8Bits(characters));
}

std::optional<uint32_t> CompactTable::findInImage(JSGlobalObject* globalObject, JSString* key) const
{
    const TableInImage& entries = tableInImage();
    const Image& image = Image::current()->holderOf(&entries);
    if (const StringImpl* impl = key->tryGetValueImpl()) [[likely]]
        return entries.find(image, StringView { *impl }, impl->hash());
    if (key->isSubstring()) {
        auto characters = key->view(globalObject);
        return characters->is8Bit() ? findCharactersInImage(image, entries, characters->span8()) : findCharactersInImage(image, entries, characters->span16());
    }
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto string = key->value(globalObject);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    const StringImpl& impl = *string->impl();
    return entries.find(image, StringView { impl }, impl.hash());
}

std::optional<uint32_t> CompactTable::find(JSGlobalObject* globalObject, JSValue key) const
{
    key = normalizeMapKey(key);
    if (key.isString())
        return findString(globalObject, asString(key));
    if (isInImage())
        return std::nullopt;
    return findOtherKey(globalObject, key);
}

std::optional<uint32_t> CompactTable::findString(JSGlobalObject* globalObject, JSString* key) const
{
    if (isInImage())
        return findInImage(globalObject, key);
    if (key->isSubstring()) {
        auto characters = key->view(globalObject);
        auto entry = characters->is8Bit() ? findCharacters(characters->span8()) : findCharacters(characters->span16());
        if (entry) {
            StringImpl& impl = *const_cast<StringImpl*>(asString(heapKeyAt(*entry))->tryGetValueImpl());
            if (impl.is8Bit() || !key->is8Bit())
                uncheckedDowncast<JSRopeString>(key)->resolveSubstringBySharing(impl);
        }
        return entry;
    }

    if (Lookup lookup = tryFindQuickly(key); lookup.result != QuickLookup::Unknown)
        return lookup.result == QuickLookup::Found ? std::optional { lookup.entry } : std::nullopt;
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto string = key->value(globalObject);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    const StringImpl* impl = string->impl();
    unsigned hash = impl->hash();
    return findWithHash(hash, [&](uint32_t entry) {
        const StringImpl* other = stringImplOrNull(heapKeyAt(entry));
        return other && (other == impl || (other->hash() == hash && equal(other, impl)));
    });
}

std::optional<uint32_t> CompactTable::findOtherKey(JSGlobalObject*, JSValue normalizedKey) const
{
    ASSERT(normalizedKey && !normalizedKey.isString());
    if (normalizedKey.isNumber() && keyWidth() == Width::Narrow)
        return std::nullopt;
    JSBigInt* bigInt = normalizedKey.isHeapBigInt() ? normalizedKey.asHeapBigInt() : nullptr;
    return findWithHash(hashOfStoredKey(normalizedKey), [&](uint32_t entry) {
        JSValue candidate = heapKeyAt(entry);
        if (candidate == normalizedKey)
            return true;
        return bigInt && candidate && candidate.isHeapBigInt() && JSBigInt::equals(candidate.asHeapBigInt(), bigInt);
    });
}

ALWAYS_INLINE std::optional<uint32_t> CompactTable::findInline(std::span<const Latin1Character> characters) const
{
    if (isInImage())
        return findCharactersInImage(Image::current()->holderOf(&tableInImage()), tableInImage(), characters);
    return findCharacters(characters);
}

std::optional<uint32_t> CompactTable::find(std::span<const Latin1Character> characters) const
{
    return findInline(characters);
}

CompactTable* CompactTable::createInImage(VM& vm, State& state, const TableInImage& entries)
{
    CompactTable* table = new (NotNull, allocateCell<CompactTable>(vm, offsetOfIndex() + 2 * sizeof(void*))) CompactTable(vm, structureOf(vm));
    table->m_used = entries.count;
    table->m_alive = entries.count;
    table->m_capacity = entries.capacity | capacityIsInImage;
    table->m_indexBits = (entries.indexBits & numberOfIndexBits) | indexIsInImage;
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

// A text that the program has put together: `list.map(name => name + suffix)`. False: it is no text. It throws where there is no memory for the characters.
static NEVER_INLINE bool resolveIfRope(JSGlobalObject* globalObject, JSValue value)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (!value || !value.isString())
        return false;
    scope.release();
    asString(value)->value(globalObject);
    return true;
}

static NEVER_INLINE bool gatherDistinctStrings(JSGlobalObject* globalObject, uintptr_t cageBase, std::span<const WriteBarrier<Unknown>> strings, uint32_t* keys, uint64_t& tags, uint32_t& used)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    for (auto& string : strings) {
        JSValue value = string.get();
        if (!isResolvedString(value)) [[unlikely]] {
            bool isString = resolveIfRope(globalObject, value);
            RETURN_IF_EXCEPTION(scope, false);
            if (!isString)
                return false;
        }
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
    uint8_t* index = at<uint8_t>(offsetOfIndex());
    uint32_t* keys = at<uint32_t>(offsetOfKeys());
    uint32_t used = 0;
    for (auto& string : strings) {
        JSString* key = asString(string.get());
        const StringImpl* impl = key->tryGetValueImpl();
        unsigned hash = impl->existingHash();
        bool isPresent = !!probeIndex<kind>(index, indexBits(), used, hash, [&](uint32_t entry) ALWAYS_INLINE_LAMBDA {
            return holdsSameText(cageBase, keys[entry], impl, hash);
        });
        if (isPresent)
            continue;
        insertIntoIndex<kind>(index, indexBits(), used, hash);
        keys[used++] = MarkedBlockCage::encode(cageBase, key);
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
            if (!isResolvedString(value)) [[unlikely]] {
                bool isString = resolveIfRope(globalObject, value);
                RETURN_IF_EXCEPTION(scope, nullptr);
                if (!isString)
                    return nullptr;
            }
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
        if (i < length) [[unlikely]] {
            bool areStrings = gatherDistinctStrings(globalObject, cageBase, { strings + i, length - i }, keys, tags, used);
            RETURN_IF_EXCEPTION(scope, nullptr);
            if (!areStrings)
                return nullptr;
        }
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
            if (!isResolvedString(value)) [[unlikely]] {
                bool isString = resolveIfRope(globalObject, value);
                RETURN_IF_EXCEPTION(scope, nullptr);
                if (!isString)
                    return nullptr;
            }
            asString(value)->tryGetValueImpl()->hash();
        }
        table = tryCreate(globalObject, Width::Narrow, Width::None, length);
        RETURN_IF_EXCEPTION(scope, nullptr);
        uint32_t used = withSlotKind(table->slotKind(), [&](auto kind) -> uint32_t {
            if constexpr (kind() == SlotKind::Tag)
                RELEASE_ASSERT_NOT_REACHED();
            else
                return table->fillWithHashedStrings<kind()>(cageBase, values);
        });
        WTF::storeStoreFence();
        table->m_used = used;
        table->m_alive = used;
    }
    JSSet* set = JSSet::create(vm, globalObject->setStructure());
    set->m_storage.setWithoutWriteBarrier(table);
    vm.mutatorFence();
    return set;
}

void CompactTable::crashUnlessSameEntriesAs(VM& vm, JSObject* expectedOwner) const
{
    CompactTable* expected = tableOf(expectedOwner);
    bool isSame = expected->m_used == m_used && expected->m_alive == m_used;
    for (uint32_t entry = 0; isSame && entry < m_used; ++entry)
        isSame = expected->heapKeyAt(entry) == heapKeyAt(entry) && expected->valueAt(entry) == valueAt(entry);
    if (!isSame)
        crashOnValidationFailure(vm, { ValidationFailureKind::FastPathResultDiffers, { }, { }, this });
}

template<CompactTable::SlotKind kind>
uint32_t CompactTable::fillWithNumbers(std::span<const EncodedJSValue> numbers)
{
    uint8_t* index = at<uint8_t>(offsetOfIndex());
    EncodedJSValue* keys = at<EncodedJSValue>(offsetOfKeys());
    unsigned indexBits = this->indexBits();
    uint32_t used = 0;
    for (EncodedJSValue number : numbers) {
        unsigned hash = rapidHashMix64(number) & hashMask;
        bool isPresent = probeIndex<kind>(index, indexBits, used, hash, [&](uint32_t entry) ALWAYS_INLINE_LAMBDA {
            return keys[entry] == number;
        }).has_value();
        if (isPresent)
            continue;
        keys[used] = number;
        insertIntoIndex<kind>(index, indexBits, used, hash);
        ++used;
    }
    return used;
}

JSSet* CompactTable::tryCreateSetOfNumbers(JSGlobalObject* globalObject, JSArray* list)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    const Butterfly* butterfly = list->butterfly();
    unsigned length = butterfly->publicLength();
    if (!length || length > maxCapacity) [[unlikely]]
        return nullptr;

    Vector<EncodedJSValue, 32> numbers(length);
    switch (list->indexingType() & IndexingShapeMask) {
    case Int32Shape:
        for (unsigned i = 0; i < length; ++i) {
            JSValue element = butterfly->contiguousInt32().at(list, i).get();
            if (!element) [[unlikely]]
                return nullptr;
            numbers[i] = JSValue::encode(element);
        }
        break;
    case DoubleShape:
        for (unsigned i = 0; i < length; ++i) {
            double element = butterfly->contiguousDouble().at(list, i);
            if (element != element) [[unlikely]]
                return nullptr;
            numbers[i] = JSValue::encode(normalizeMapKey(JSValue(JSValue::EncodeAsDouble, element)));
        }
        break;
    default:
        return nullptr;
    }

    CompactTable* table = tryCreate(globalObject, Width::Wide, Width::None, length);
    RETURN_IF_EXCEPTION(scope, nullptr);
    uint32_t used = withSlotKind(table->slotKind(), [&](auto kind) {
        return table->fillWithNumbers<kind()>(numbers.span());
    });
    WTF::storeStoreFence();
    table->m_used = used;
    table->m_alive = used;
    if (length > maxCapacityWithShortSlots && used < length / 4) {
        table = table->tryCopyWithWidths(globalObject, Width::Wide, Width::None, used + 1);
        RETURN_IF_EXCEPTION(scope, nullptr);
    }

    JSSet* set = JSSet::create(vm, globalObject->setStructure());
    set->m_storage.setWithoutWriteBarrier(table);
    vm.mutatorFence();

    if (validatesRuntime(vm)) [[unlikely]] {
        DeferTerminationForAWhile deferTermination(vm);
        JSSet* expected = JSSet::create(vm, globalObject->setStructure());
        for (EncodedJSValue number : numbers) {
            expected->add(globalObject, JSValue::decode(number));
            scope.releaseAssertNoException();
        }
        table->crashUnlessSameEntriesAs(vm, expected);
    }
    return set;
}

// Reading the first two elements of such an array runs no code.
static ALWAYS_INLINE const WriteBarrier<Unknown>* elementsOfOrdinaryPair(JSGlobalObject* globalObject, JSValue value)
{
    JSArray* pair = value && value.isCell() ? dynamicDowncast<JSArray>(value.asCell()) : nullptr;
    if (!pair || (pair->indexingType() & IndexingShapeMask) != ContiguousShape || pair->butterfly()->publicLength() < 2 || !globalObject->isOriginalArrayStructure(pair->structure()))
        return nullptr;
    const WriteBarrier<Unknown>* elements = pair->butterfly()->contiguous().data();
    return elements[0] && elements[1] ? elements : nullptr;
}

template<CompactTable::SlotKind kind>
uint32_t CompactTable::fillWithPairsOfHashedStrings(uintptr_t cageBase, std::span<const WriteBarrier<Unknown>> pairs)
{
    uint8_t* index = at<uint8_t>(offsetOfIndex());
    uint32_t* keys = at<uint32_t>(offsetOfKeys());
    bool hasNarrowValues = m_valueWidth == Width::Narrow;
    uint32_t* narrowValues = at<uint32_t>(offsetOfValues());
    EncodedJSValue* wideValues = at<EncodedJSValue>(offsetOfValues());
    unsigned indexBits = this->indexBits();
    uint32_t used = 0;
    for (auto& pair : pairs) {
        const WriteBarrier<Unknown>* elements = asArray(pair.get())->butterfly()->contiguous().data();
        JSString* key = asString(elements[0].get());
        const StringImpl* impl = key->tryGetValueImpl();
        unsigned hash = impl->existingHash();
        std::optional<uint32_t> entry = probeIndex<kind>(index, indexBits, used, hash, [&](uint32_t candidate) ALWAYS_INLINE_LAMBDA {
            return holdsSameText(cageBase, keys[candidate], impl, hash);
        });
        if (!entry) {
            entry = used++;
            keys[*entry] = MarkedBlockCage::encode(cageBase, key);
            insertIntoIndex<kind>(index, indexBits, *entry, hash);
        }
        JSValue value = elements[1].get();
        if (hasNarrowValues)
            narrowValues[*entry] = value.isCell() ? MarkedBlockCage::encode(cageBase, value.asCell()) : static_cast<uint32_t>(JSValue::encode(value));
        else
            wideValues[*entry] = JSValue::encode(value);
    }
    return used;
}

JSMap* CompactTable::tryCreateMapOfStringKeys(JSGlobalObject* globalObject, JSArray* list)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    Instance& instance = *Instance::of(globalObject);
    std::span<const WriteBarrier<Unknown>> pairs { list->butterfly()->contiguous().data(), list->butterfly()->publicLength() };
    if (pairs.empty() || pairs.size() > maxCapacity || instance.options().useWideReferences()) [[unlikely]]
        return nullptr;

    Width valueWidth = Width::Narrow;
    for (auto& pair : pairs) {
        const WriteBarrier<Unknown>* elements = elementsOfOrdinaryPair(globalObject, pair.get());
        if (!elements || !isResolvedString(elements[0].get()) || !narrowColumnHolds(elements[0].get()))
            return nullptr;
        asString(elements[0].get())->tryGetValueImpl()->hash();
        if (!narrowColumnHolds(elements[1].get()))
            valueWidth = Width::Wide;
    }

    CompactTable* table = tryCreate(globalObject, Width::Narrow, valueWidth, pairs.size());
    RETURN_IF_EXCEPTION(scope, nullptr);
    uintptr_t cageBase = instance.cellCageBase;
    uint32_t used = withSlotKind(table->slotKind(), [&](auto kind) {
        return table->fillWithPairsOfHashedStrings<kind()>(cageBase, pairs);
    });
    WTF::storeStoreFence();
    table->m_used = used;
    table->m_alive = used;
    ensureStillAliveHere(list);

    JSMap* map = JSMap::create(vm, globalObject->mapStructure());
    map->m_storage.setWithoutWriteBarrier(table);
    vm.mutatorFence();

    if (validatesRuntime(vm)) [[unlikely]] {
        DeferTerminationForAWhile deferTermination(vm);
        JSMap* expected = JSMap::create(vm, globalObject->mapStructure());
        for (auto& pair : pairs) {
            const WriteBarrier<Unknown>* elements = asArray(pair.get())->butterfly()->contiguous().data();
            expected->set(globalObject, elements[0].get(), elements[1].get());
            scope.releaseAssertNoException();
        }
        table->crashUnlessSameEntriesAs(vm, expected);
        ensureStillAliveHere(list);
    }
    return map;
}

static ALWAYS_INLINE void resolveBySharing(JSRopeString* substring, StringImpl& impl)
{
    if (impl.is8Bit() || !substring->is8Bit())
        substring->resolveSubstringBySharing(impl);
}

static ALWAYS_INLINE CompactTable::QuickLookup tryFindSubstringQuickly(JSObject* collection, JSValue key, JSValue& value)
{
    using QuickLookup = CompactTable::QuickLookup;
    if (!key.isString() || !asString(key)->isSubstring())
        return QuickLookup::Unknown;
    CompactTable* table = CompactTable::tableOf(collection);
    if (!table)
        return QuickLookup::Absent;
    if (!table->isHeapTable())
        return QuickLookup::Unknown;
    auto* rope = uncheckedDowncast<JSRopeString>(asString(key));
    size_t length = rope->length();
    size_t offset = rope->substringOffset();
    const StringImpl* baseImpl = rope->substringBase()->tryGetValueImpl();
    ASSERT(baseImpl);
    auto find = [&]<typename CharacterType>(std::span<const CharacterType> text) ALWAYS_INLINE_LAMBDA {
        unsigned hash = StringHasher::computeHashAndMaskTop8Bits(text);
        auto lookup = table->tryFindCharactersQuickly(text, hash);
        if (lookup.result == QuickLookup::Found) {
            value = table->valueAt(lookup.entry);
            resolveBySharing(rope, *asString(table->heapKeyAt(lookup.entry))->tryGetValueImpl());
        }
        return lookup.result;
    };
    return baseImpl->is8Bit() ? find(baseImpl->span8().subspan(offset, length)) : find(baseImpl->span16().subspan(offset, length));
}

static CompactTable* heapTableOrNull(JSGlobalObject* globalObject, JSObject* collection)
{
    CompactTable* table = CompactTable::tableOf(collection);
    if (!table || !table->isInImage()) [[likely]]
        return table;
    DeferTerminationForAWhile deferTermination(globalObject->vm());
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(globalObject->vm());
    CompactTable* moved = CompactTable::tryMoveToHeap(globalObject, collection);
    scope.clearException();
    return moved ? moved : table;
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTMoveTableToHeap, JSCell*, (Instance* instance, JSCell* collection))
{
    AOT_OPERATION_BEGIN_WITHOUT_COLLECTION(instance, Entry::operationAOTMoveTableToHeap);
    heapTableOrNull(globalObject, asObject(collection));
    OPERATION_RETURN(scope, collection);
}

template<Entry entry>
static NEVER_INLINE OperationReturnType<EncodedJSValue> JIT_OPERATION_ATTRIBUTES findInCollectionGeneric(Instance* instance, JSCell* collection, EncodedJSValue lookupKey)
{
    AOT_OPERATION_BEGIN(instance, entry);
    CompactTable* table = heapTableOrNull(globalObject, asObject(collection));
    std::optional<uint32_t> found = table ? table->find(globalObject, JSValue::decode(lookupKey)) : std::nullopt;
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    if constexpr (entry == Entry::operationAOTMapGet)
        OPERATION_RETURN(scope, JSValue::encode(found ? table->valueAt(*found) : jsUndefined()));
    else if constexpr (entry == Entry::operationAOTMapFind)
        OPERATION_RETURN(scope, JSValue::encode(found ? table->valueAt(*found) : JSValue()));
    else
        OPERATION_RETURN(scope, JSValue::encode(jsBoolean(!!found)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTMapGet, EncodedJSValue, (Instance* instance, JSCell* map, EncodedJSValue lookupKey))
{
    JSValue value;
    switch (tryFindSubstringQuickly(uncheckedDowncast<JSMap>(map), JSValue::decode(lookupKey), value)) {
    case CompactTable::QuickLookup::Found:
        return { JSValue::encode(value), nullptr };
    case CompactTable::QuickLookup::Absent:
        return { JSValue::encode(jsUndefined()), nullptr };
    case CompactTable::QuickLookup::Unknown:
        break;
    }
    [[clang::musttail]] return findInCollectionGeneric<Entry::operationAOTMapGet>(instance, map, lookupKey);
}

JSC_DEFINE_JIT_OPERATION(operationAOTMapFind, EncodedJSValue, (Instance* instance, JSCell* map, EncodedJSValue lookupKey))
{
    JSValue value;
    switch (tryFindSubstringQuickly(uncheckedDowncast<JSMap>(map), JSValue::decode(lookupKey), value)) {
    case CompactTable::QuickLookup::Found:
        return { JSValue::encode(value), nullptr };
    case CompactTable::QuickLookup::Absent:
        return { JSValue::encode(JSValue()), nullptr };
    case CompactTable::QuickLookup::Unknown:
        break;
    }
    [[clang::musttail]] return findInCollectionGeneric<Entry::operationAOTMapFind>(instance, map, lookupKey);
}

JSC_DEFINE_JIT_OPERATION(operationAOTMapHas, EncodedJSValue, (Instance* instance, JSCell* map, EncodedJSValue lookupKey))
{
    JSValue value;
    if (auto lookup = tryFindSubstringQuickly(asObject(map), JSValue::decode(lookupKey), value); lookup != CompactTable::QuickLookup::Unknown)
        return { JSValue::encode(jsBoolean(lookup == CompactTable::QuickLookup::Found)), nullptr };
    [[clang::musttail]] return findInCollectionGeneric<Entry::operationAOTMapHas>(instance, map, lookupKey);
}

JSC_DEFINE_JIT_OPERATION(operationAOTSetHas, EncodedJSValue, (Instance* instance, JSCell* set, EncodedJSValue lookupKey))
{
    JSValue value;
    if (auto lookup = tryFindSubstringQuickly(asObject(set), JSValue::decode(lookupKey), value); lookup != CompactTable::QuickLookup::Unknown)
        return { JSValue::encode(jsBoolean(lookup == CompactTable::QuickLookup::Found)), nullptr };
    [[clang::musttail]] return findInCollectionGeneric<Entry::operationAOTSetHas>(instance, set, lookupKey);
}

template<Entry entry, typename Result>
static NEVER_INLINE OperationReturnType<Result> JIT_OPERATION_ATTRIBUTES findByKeyPartsGeneric(Instance* instance, JSCell* collection, uint32_t count, const EncodedJSValue* parts)
{
    AOT_OPERATION_BEGIN(instance, entry);
    JSString* key = StringBuilderStorage::concatenate(globalObject, *instance, unsafeMakeSpan(parts, count), StringBuilderStorage::StringUse::LikelyKey);
    OPERATION_RETURN_IF_EXCEPTION(scope, Result());
    CompactTable* table = CompactTable::tableOf(asObject(collection));
    std::optional<uint32_t> found = table ? table->find(globalObject, key) : std::nullopt;
    OPERATION_RETURN_IF_EXCEPTION(scope, Result());
    if constexpr (entry == Entry::operationAOTMapGetByKeyParts)
        OPERATION_RETURN(scope, JSValue::encode(found ? table->valueAt(*found) : jsUndefined()));
    else
        OPERATION_RETURN(scope, found.has_value());
}

JSC_DEFINE_JIT_OPERATION(operationAOTMapGetByKeyParts, EncodedJSValue, (Instance* instance, JSCell* map, uint32_t count, const EncodedJSValue* parts))
{
    ShortKeyCharacters key;
    if (tryWriteShortKey(unsafeMakeSpan(parts, count), key)) [[likely]] {
        CompactTable* table = CompactTable::tableOf(uncheckedDowncast<JSMap>(map));
        std::optional<uint32_t> entry = table ? table->findInline(key.span()) : std::nullopt;
        if (!entry)
            return { JSValue::encode(jsUndefined()), nullptr };
        if (table->isHeapTable()) [[likely]]
            return { JSValue::encode(table->valueAt(*entry)), nullptr };
    }
    [[clang::musttail]] return findByKeyPartsGeneric<Entry::operationAOTMapGetByKeyParts, EncodedJSValue>(instance, map, count, parts);
}

JSC_DEFINE_JIT_OPERATION(operationAOTHasByKeyParts, size_t, (Instance* instance, JSCell* collection, uint32_t count, const EncodedJSValue* parts))
{
    ShortKeyCharacters key;
    if (tryWriteShortKey(unsafeMakeSpan(parts, count), key)) [[likely]] {
        CompactTable* table = CompactTable::tableOf(asObject(collection));
        return { table && table->findInline(key.span()), nullptr };
    }
    [[clang::musttail]] return findByKeyPartsGeneric<Entry::operationAOTHasByKeyParts, size_t>(instance, collection, count, parts);
}

JSC_DEFINE_JIT_OPERATION(operationAOTMapSet, EncodedJSValue, (Instance* instance, JSCell* map, EncodedJSValue lookupKey, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTMapSet);
    instance->allocation.compactTableGrowthPlans = CompactTable::growthPlans();
    uncheckedDowncast<JSMap>(map)->set(globalObject, JSValue::decode(lookupKey), JSValue::decode(value));
    OPERATION_RETURN(scope, JSValue::encode(map));
}

JSC_DEFINE_JIT_OPERATION(operationAOTSetAdd, EncodedJSValue, (Instance* instance, JSCell* set, EncodedJSValue lookupKey))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTSetAdd);
    instance->allocation.compactTableGrowthPlans = CompactTable::growthPlans();
    uncheckedDowncast<JSSet>(set)->add(globalObject, JSValue::decode(lookupKey));
    OPERATION_RETURN(scope, JSValue::encode(set));
}

JSC_DEFINE_JIT_OPERATION(operationAOTWeakMapSet, EncodedJSValue, (Instance* instance, JSCell* map, JSCell* lookupKey, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTWeakMapSet);
    uncheckedDowncast<JSWeakMap>(map)->set(vm, lookupKey, JSValue::decode(value));
    OPERATION_RETURN(scope, JSValue::encode(map));
}

template<typename Visitor>
void CompactTable::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    Base::visitChildren(cell, visitor);
    visitFields(uncheckedDowncast<CompactTable>(cell), visitor);
}

DEFINE_VISIT_CHILDREN(CompactTable);

std::optional<Vector<uint8_t>> TableInImage::tryEncode(std::span<const Key> keys, std::span<const uint64_t> values)
{
    if (keys.empty() || keys.size() > CompactTable::maxCapacity || (!values.empty() && values.size() != keys.size()))
        return std::nullopt;
    TableInImage header { };
    header.count = keys.size();
    header.alive = header.count;
    header.capacity = roundUpToMultipleOf<2>(std::max<uint32_t>(header.count, 2));
    unsigned indexBits = CompactTable::indexBitsFor(header.capacity);
    header.indexBits = CompactTable::markedIndexBits(indexBits);
    header.keysShift = CompactTable::keysShiftFor(indexBits, header.capacity);
    header.hasValues = !values.empty();
    CompactTable::SlotKind slotKind = CompactTable::slotKindFor(header.capacity);
    size_t sizeOfIndex = static_cast<size_t>(2) << header.keysShift;
    size_t offsetOfStrings = sizeof(TableInImage) + sizeOfIndex;
    size_t offsetOfStringConstants = offsetOfStrings + sizeof(uint32_t) * header.capacity;
    size_t offsetOfValues = offsetOfStringConstants + sizeof(uint32_t) * header.capacity;
    size_t size = offsetOfValues + (header.hasValues ? sizeof(uint64_t) * header.capacity : 0);
    if (size > std::numeric_limits<uint32_t>::max())
        return std::nullopt;
    Vector<uint8_t> bytes(FillWith { }, size, static_cast<uint8_t>(0));
    memcpy(bytes.mutableSpan().data(), &header, sizeof(header));
    uint8_t* index = bytes.mutableSpan().data() + sizeof(TableInImage);
    uint32_t* strings = reinterpret_cast<uint32_t*>(bytes.mutableSpan().data() + offsetOfStrings);
    uint32_t* stringConstants = reinterpret_cast<uint32_t*>(bytes.mutableSpan().data() + offsetOfStringConstants);
    if (header.hasValues)
        memcpy(bytes.mutableSpan().data() + offsetOfValues, values.data(), values.size_bytes());
    for (uint32_t entry = 0; entry < header.count; ++entry) {
        RELEASE_ASSERT(keys[entry].hash <= CompactTable::hashMask);
        strings[entry] = keys[entry].string;
        stringConstants[entry] = keys[entry].stringConstant;
        CompactTable::insertIntoIndex(slotKind, index, indexBits, entry, keys[entry].hash);
    }
    return bytes;
}

} } // namespace JSC::AOT

namespace JSC {
namespace AOT::CompactStorage {

JSCell* tryCreate(JSGlobalObject* globalObject, bool hasValues)
{
    using Width = CompactTable::Width;
    RELEASE_ASSERT(!Options::useJIT());
    VM& vm = globalObject->vm();
    Width width = vm.aotState().image->header().options.useWideReferences() ? Width::Wide : Width::Narrow;
    CompactTable::ensureInitialTableAllocators(vm);
    return CompactTable::tryCreate(globalObject, width, hasValues ? width : Width::None, CompactTable::initialCapacity);
}

// A number needs a wide column. A table that starts with one is not replaced by its first entry.
JSCell* tryCreateForFirstEntry(JSGlobalObject* globalObject, bool hasValues, JSValue key, JSValue value)
{
    RELEASE_ASSERT(!Options::useJIT());
    CompactTable::ensureInitialTableAllocators(globalObject->vm());
    return CompactTable::tryCreateForFirstEntry(globalObject, hasValues, key, value);
}

JSCell* copy(JSGlobalObject* globalObject, JSCell* storage)
{
    return uncheckedDowncast<CompactTable>(storage)->tryCopy(globalObject);
}

bool has(JSGlobalObject* globalObject, JSCell* storage, JSValue key)
{
    return uncheckedDowncast<CompactTable>(storage)->find(globalObject, key).has_value();
}

JSValue find(JSGlobalObject* globalObject, JSCell* storage, JSValue key)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* table = uncheckedDowncast<CompactTable>(storage);
    std::optional<uint32_t> entry = table->find(globalObject, key);
    RETURN_IF_EXCEPTION(scope, { });
    if (!entry)
        return { };
    return table->hasValues() ? table->valueAt(*entry) : table->keyAt(*entry);
}

void add(JSGlobalObject* globalObject, JSObject* owner, JSCell* storage, JSValue key, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    ASSERT_UNUSED(storage, CompactTable::tableOf(owner) == storage);
    bool isAdded = CompactTable::add(globalObject, owner, key, value);
    RETURN_IF_EXCEPTION(scope, void());
    RELEASE_ASSERT(isAdded);
}

bool remove(JSGlobalObject* globalObject, JSObject* owner, JSCell* storage, JSValue key)
{
    ASSERT_UNUSED(storage, CompactTable::tableOf(owner) == storage);
    return CompactTable::remove(globalObject, owner, key);
}

void clear(JSGlobalObject* globalObject, JSObject* owner, JSCell* storage)
{
    ASSERT_UNUSED(storage, CompactTable::tableOf(owner) == storage);
    CompactTable::clear(globalObject, owner);
}

uint32_t aliveEntryCount(JSCell* storage)
{
    return uncheckedDowncast<CompactTable>(storage)->size();
}

Position transitAndNext(VM& vm, JSCell* storage, uint32_t entry)
{
    return CompactTable::next(vm, storage, entry);
}

void setIterationEntry(JSCell* storage, uint32_t entry)
{
    storage->vm().aotState().runtimeTable->setCompactTableIterationEntry(entry);
}

uint32_t iterationEntry(JSCell* storage)
{
    return storage->vm().aotState().runtimeTable->compactTableIterationEntry();
}

JSValue keyAt(JSCell* storage, uint32_t entry)
{
    return uncheckedDowncast<CompactTable>(storage)->keyAt(entry);
}

JSValue valueAt(JSCell* storage, uint32_t entry)
{
    return uncheckedDowncast<CompactTable>(storage)->valueAt(entry);
}

} // namespace AOT::CompactStorage
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
