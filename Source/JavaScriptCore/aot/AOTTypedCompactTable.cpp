/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedCompactTable.h"

#if ENABLE(AOT)

#include "AOTRuntime.h"
#include "AOTTypedObject.h"
#include "ArrayIteratorPrototype.h"
#include "IteratorOperations.h"
#include "JSArrayIteratorInlines.h"
#include "JSCInlines.h"
#include "JSMapInlines.h"
#include "JSMapIterator.h"
#include "JSRegExpStringIteratorInlines.h"
#include "JSSetInlines.h"
#include "JSSetIterator.h"
#include "JSStringIteratorInlines.h"
#include "MapIteratorPrototypeInlines.h"
#include "MarkedBlockCage.h"
#include "SetIteratorPrototypeInlines.h"
#include "TopExceptionScope.h"
#include <wtf/StackTrace.h>
#include <wtf/StringPrintStream.h>

namespace JSC { namespace AOT { namespace Typed {

const ClassInfo CompactTable::s_info = { "CompactTable"_s, nullptr, nullptr, nullptr, CREATE_METHOD_TABLE(CompactTable) };

static constexpr uint16_t freeShortSlot = UINT16_MAX;
static constexpr uint32_t freeLongSlot = UINT32_MAX;

static uint8_t shortTagOf(unsigned hash) { return hash >> CompactTable::shiftOfShortTag; }
static uint32_t longTagOf(unsigned hash) { return hash >> CompactTable::shiftOfLongTag; }
static_assert(StringImpl::s_flagCount == 8 && CompactTable::shiftOfShortTag + 8 == 24 && CompactTable::shiftOfLongTag + 32 - CompactTable::entryBitsOfLongSlot == 24);

static unsigned hashOf(JSString* key)
{
    return key->tryGetValueImpl()->hash();
}

Structure* CompactTable::createStructure(VM& vm)
{
    return Structure::create(vm, nullptr, jsNull(), TypeInfo(CellType, StructureFlags), info());
}

unsigned CompactTable::indexBitsFor(uint32_t capacity)
{
    if (capacity <= maxCapacityWithTags)
        return 0;
    static_assert((3u << maxIndexBitsWithShortSlots) / 4 <= maxCapacityWithShortSlots);
    unsigned bits = 4;
    while ((static_cast<uint64_t>(1) << bits) * 3 < static_cast<uint64_t>(capacity) * 4)
        ++bits;
    return bits;
}

size_t CompactTable::offsetOfKeys(uint32_t capacity)
{
    size_t index = maxCapacityWithTags;
    if (unsigned bits = indexBitsFor(capacity))
        index = (capacity <= maxCapacityWithShortSlots ? sizeof(uint16_t) : sizeof(uint32_t)) << bits;
    return offsetOfIndex() + index;
}

size_t CompactTable::allocationSize(uint32_t capacity, Width key, Width value)
{
    return offsetOfKeys(capacity) + static_cast<size_t>(capacity) * (static_cast<size_t>(key) + static_cast<size_t>(value));
}

CompactTable* CompactTable::create(VM& vm, Structure* structure, Width key, Width value, uint32_t leastCapacity)
{
    RELEASE_ASSERT(key != Width::None && leastCapacity <= maxCapacity);
    uint32_t capacity = roundUpToMultipleOf<2>(std::max<uint32_t>(leastCapacity, 2));
    size_t size = MarkedSpace::optimalSizeFor(allocationSize(capacity, key, value));
    while (capacity < maxCapacity && allocationSize(capacity + 2, key, value) <= size)
        capacity += 2;

    CompactTable* table = new (NotNull, allocateCell<CompactTable>(vm, size)) CompactTable(vm, structure);
    table->m_capacity = capacity;
    table->m_indexBits = indexBitsFor(capacity);
    table->m_keysShift = (table->hasTags() ? keysShiftWithTags : table->m_indexBits + !table->hasShortSlots()) | (key == Width::Wide ? keysAreWide : 0);
    table->m_valueWidth = value;
    size_t keys = table->offsetOfKeys();
    memsetSpan(std::span { table->at<uint8_t>(offsetOfIndex()), keys - offsetOfIndex() }, table->hasTags() ? 0 : 0xff);
    zeroSpan(std::span { table->at<uint8_t>(keys), size - keys });
    vm.mutatorFence();
    return table;
}

bool CompactTable::canHold(Width width, JSValue value)
{
    if (width != Width::Narrow || value.isUndefinedOrNull() || value.isBoolean())
        return true;
    return value.isCell() && MarkedBlockCage::contains(value.asCell()) && !(std::bit_cast<uintptr_t>(value.asCell()) & (MarkedBlock::atomSize - 1));
}

JSString* CompactTable::keyAt(uint32_t entry) const
{
    if (keyWidth() == Width::Wide)
        return at<JSString*>(offsetOfKeys())[entry];
    uint32_t index = at<uint32_t>(offsetOfKeys())[entry];
    return index ? uncheckedDowncast<JSString>(MarkedBlockCage::decode(index)) : nullptr;
}

void CompactTable::setKey(uint32_t entry, JSString* key)
{
    if (keyWidth() == Width::Wide)
        at<JSString*>(offsetOfKeys())[entry] = key;
    else
        at<uint32_t>(offsetOfKeys())[entry] = key ? MarkedBlockCage::encode(key) : absentCell32;
}

void CompactTable::setValue(uint32_t entry, JSValue value)
{
    switch (m_valueWidth) {
    case Width::None:
        return;
    case Width::Wide:
        at<EncodedJSValue>(offsetOfValues())[entry] = JSValue::encode(value);
        return;
    case Width::Narrow:
        at<uint32_t>(offsetOfValues())[entry] = value && value.isCell() ? MarkedBlockCage::encode(value.asCell()) : static_cast<uint32_t>(JSValue::encode(value));
        return;
    }
}

template<typename Matches>
std::optional<uint32_t> CompactTable::findWithHash(unsigned hash, NOESCAPE const Matches& matches) const
{
    ASSERT(isLive());
    if (hasTags()) {
        const uint8_t* tags = at<uint8_t>(offsetOfIndex());
        for (uint32_t entry = 0; entry < m_used; ++entry) {
            if (tags[entry] == shortTagOf(hash) && matches(entry))
                return entry;
        }
        return std::nullopt;
    }

    uint32_t mask = (1u << m_indexBits) - 1;
    if (hasShortSlots()) {
        const uint16_t* slots = at<uint16_t>(offsetOfIndex());
        for (uint32_t i = hash & mask;; i = (i + 1) & mask) {
            uint16_t slot = slots[i];
            if (slot == freeShortSlot)
                return std::nullopt;
            if (slot >> entryBitsOfShortSlot == shortTagOf(hash) && matches(slot & ((1 << entryBitsOfShortSlot) - 1)))
                return slot & ((1 << entryBitsOfShortSlot) - 1);
        }
    }
    const uint32_t* slots = at<uint32_t>(offsetOfIndex());
    for (uint32_t i = hash & mask;; i = (i + 1) & mask) {
        uint32_t slot = slots[i];
        if (slot == freeLongSlot)
            return std::nullopt;
        if (slot >> entryBitsOfLongSlot == longTagOf(hash) && matches(slot & ((1 << entryBitsOfLongSlot) - 1)))
            return slot & ((1 << entryBitsOfLongSlot) - 1);
    }
}

std::optional<uint32_t> CompactTable::find(JSGlobalObject* globalObject, JSValue value) const
{
    uint32_t found;
    switch (tryFindQuickly(value, found)) {
    case Quickly::Found:
        return found;
    case Quickly::Absent:
        return std::nullopt;
    case Quickly::Unknown:
        break;
    }
    JSString* key = asString(value);
    if (key->isSubstring()) {
        auto characters = key->view(globalObject);
        unsigned hash = characters->hash();
        auto entry = findWithHash(hash, [&](uint32_t entry) {
            JSString* candidate = keyAt(entry);
            return candidate && hashOf(candidate) == hash && StringView { *candidate->tryGetValueImpl() } == characters.data;
        });
        if (entry) {
            StringImpl& impl = *const_cast<StringImpl*>(keyAt(*entry)->tryGetValueImpl());
            if (impl.is8Bit() || !key->is8Bit())
                uncheckedDowncast<JSRopeString>(key)->resolveSubstringBySharing(impl);
        }
        return entry;
    }

    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto string = key->value(globalObject);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    const StringImpl* impl = string->impl();
    unsigned hash = impl->hash();
    return findWithHash(hash, [&](uint32_t entry) {
        JSString* candidate = keyAt(entry);
        if (!candidate)
            return false;
        if (candidate == key)
            return true;
        const StringImpl* other = candidate->tryGetValueImpl();
        return other == impl || (other->hash() == hash && equal(other, impl));
    });
}

std::optional<uint32_t> CompactTable::find(std::span<const Latin1Character> characters) const
{
    unsigned hash = StringHasher::computeHashAndMaskTop8Bits(characters);
    return findWithHash(hash, [&](uint32_t entry) {
        JSString* candidate = keyAt(entry);
        return candidate && hashOf(candidate) == hash && equal(candidate->tryGetValueImpl(), characters);
    });
}

void CompactTable::addToIndex(uint32_t entry, unsigned hash)
{
    if (hasTags()) {
        at<uint8_t>(offsetOfIndex())[entry] = shortTagOf(hash);
        return;
    }
    uint32_t mask = (1u << m_indexBits) - 1;
    uint32_t i = hash & mask;
    if (hasShortSlots()) {
        uint16_t* slots = at<uint16_t>(offsetOfIndex());
        while (slots[i] != freeShortSlot)
            i = (i + 1) & mask;
        slots[i] = shortTagOf(hash) << entryBitsOfShortSlot | entry;
        return;
    }
    uint32_t* slots = at<uint32_t>(offsetOfIndex());
    while (slots[i] != freeLongSlot)
        i = (i + 1) & mask;
    slots[i] = longTagOf(hash) << entryBitsOfLongSlot | entry;
}

void CompactTable::fillIndex(uint32_t count)
{
    auto forEachHash = [&](const auto& functor) {
        if (keyWidth() == Width::Wide) {
            JSString* const* keys = at<JSString*>(offsetOfKeys());
            for (uint32_t entry = 0; entry < count; ++entry)
                functor(entry, hashOf(keys[entry]));
            return;
        }
        const uint32_t* keys = at<uint32_t>(offsetOfKeys());
        for (uint32_t entry = 0; entry < count; ++entry)
            functor(entry, hashOf(uncheckedDowncast<JSString>(MarkedBlockCage::decode(keys[entry]))));
    };
    if (hasTags()) {
        uint8_t* tags = at<uint8_t>(offsetOfIndex());
        forEachHash([&](uint32_t entry, unsigned hash) {
            tags[entry] = shortTagOf(hash);
        });
        return;
    }
    uint32_t mask = (1u << m_indexBits) - 1;
    if (hasShortSlots()) {
        uint16_t* slots = at<uint16_t>(offsetOfIndex());
        forEachHash([&](uint32_t entry, unsigned hash) {
            uint32_t i = hash & mask;
            while (slots[i] != freeShortSlot)
                i = (i + 1) & mask;
            slots[i] = shortTagOf(hash) << entryBitsOfShortSlot | entry;
        });
        return;
    }
    uint32_t* slots = at<uint32_t>(offsetOfIndex());
    forEachHash([&](uint32_t entry, unsigned hash) {
        uint32_t i = hash & mask;
        while (slots[i] != freeLongSlot)
            i = (i + 1) & mask;
        slots[i] = longTagOf(hash) << entryBitsOfLongSlot | entry;
    });
}

void CompactTable::append(VM& vm, JSString* key, unsigned hash, JSValue value)
{
    RELEASE_ASSERT(m_used < m_capacity);
    uint32_t entry = m_used;
    setKey(entry, key);
    setValue(entry, value);
    addToIndex(entry, hash);
    // The collector goes up to `used`.
    WTF::storeStoreFence();
    m_used = entry + 1;
    ++m_alive;
    vm.writeBarrier(this);
}

CompactTable* CompactTable::copyWithRoom(VM& vm, Width value) const
{
    uint64_t alive = size();
    if (alive >= maxCapacity)
        return nullptr;
    uint64_t leastCapacity = std::min<uint64_t>(deleted() >= m_used / 2 ? alive + 1 : alive < 64 ? alive + alive / 2 + 1 : alive * 2, maxCapacity);
    CompactTable* copy = create(vm, structure(), keyWidth(), value, leastCapacity);
    // The collector goes up to `used`, which is zero until all is written.
    size_t keyWidth = static_cast<size_t>(this->keyWidth());
    size_t valueWidth = static_cast<size_t>(m_valueWidth);
    const uint8_t* keys = at<uint8_t>(offsetOfKeys());
    const uint8_t* values = at<uint8_t>(offsetOfValues());
    uint8_t* keysOfCopy = copy->at<uint8_t>(copy->offsetOfKeys());
    uint8_t* valuesOfCopy = copy->at<uint8_t>(copy->offsetOfValues());
    uint32_t count = 0;
    if (!deleted() && value == m_valueWidth) {
        count = m_used;
        memcpy(keysOfCopy, keys, count * keyWidth);
        memcpy(valuesOfCopy, values, count * valueWidth);
    } else {
        for (uint32_t entry = 0; entry < m_used; ++entry) {
            if (!keyAt(entry))
                continue;
            memcpy(keysOfCopy + count * keyWidth, keys + entry * keyWidth, keyWidth);
            if (value == m_valueWidth)
                memcpy(valuesOfCopy + count * valueWidth, values + entry * valueWidth, valueWidth);
            else
                copy->setValue(count, valueAt(entry));
            ++count;
        }
    }
    if (!deleted() && hasTags() && copy->hasTags())
        memcpy(copy->at<uint8_t>(offsetOfIndex()), at<uint8_t>(offsetOfIndex()), maxCapacityWithTags);
    else
        copy->fillIndex(count);
    WTF::storeStoreFence();
    copy->m_used = count;
    copy->m_alive = count;
    vm.writeBarrier(copy);
    return copy;
}

CompactTable* CompactTable::copy(VM& vm) const
{
    RELEASE_ASSERT(isLive());
    size_t used = allocationSize(m_capacity, keyWidth(), m_valueWidth);
    size_t size = MarkedSpace::optimalSizeFor(used);
    CompactTable* copy = new (NotNull, allocateCell<CompactTable>(vm, size)) CompactTable(vm, structure());
    copy->m_capacity = m_capacity;
    copy->m_indexBits = m_indexBits;
    copy->m_keysShift = m_keysShift;
    copy->m_valueWidth = m_valueWidth;
    memcpy(copy->at<uint8_t>(offsetOfIndex()), at<uint8_t>(offsetOfIndex()), used - offsetOfIndex());
    zeroSpan(std::span { copy->at<uint8_t>(used), size - used });
    WTF::storeStoreFence();
    copy->m_used = m_used;
    copy->m_alive = m_alive;
    vm.writeBarrier(copy);
    return copy;
}

void CompactTable::becomeObsolete(VM& vm, State state, JSCell* next)
{
    RELEASE_ASSERT(isLive() && state > State::Iterated);
    {
        Locker locker { cellLock() };
        uint32_t count = 0;
        if (state == State::Replaced && deleted()) {
            // What is written lies at or before what is read next.
            for (uint32_t entry = 0; entry < m_used; ++entry) {
                if (!keyAt(entry))
                    deletedEntries()[count++] = entry;
            }
        }
        m_state = state;
        successor() = next;
        numberOfDeletedEntries() = count;
    }
    vm.writeBarrier(this);
}

static Typed::State* stateOf(JSGlobalObject* globalObject)
{
    Instance* instance = globalObject->aotInstance();
    return instance ? instance->typed : nullptr;
}

template<typename Traits>
bool CompactTable::tryAdd(JSGlobalObject* globalObject, JSOrderedHashTable<Traits>* owner, JSValue key, JSValue value, CreateIfNeeded createIfNeeded, uint32_t leastCapacity)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    ASSERT(!owner->m_storage);
    if (!key.isString())
        return false;
    JSString* string = asString(key);
    Typed::State* state = stateOf(globalObject);
    auto* table = owner->m_compactTable ? uncheckedDowncast<CompactTable>(owner->m_compactTable.get()) : nullptr;
    auto replaceBy = [&](CompactTable* replacement, ASCIILiteral event) {
        if (state && state->isCounting()) [[unlikely]]
            state->countCompactTable(event, table->size());
        replacement->m_state = table->m_state;
        if (table->m_state != State::Live)
            table->becomeObsolete(vm, State::Replaced, replacement);
        owner->m_compactTable.set(vm, owner, replacement);
        table = replacement;
    };

    if (table) {
        auto entry = table->find(globalObject, string);
        RETURN_IF_EXCEPTION(scope, false);
        if (entry) {
            if (!canHold(table->m_valueWidth, value)) [[unlikely]] {
                uint32_t before = 0;
                for (uint32_t i = 0; i < *entry; ++i)
                    before += !table->keyAt(i);
                CompactTable* wider = table->copyWithRoom(vm, Width::Wide);
                if (!wider)
                    return false;
                replaceBy(wider, "widened"_s);
                *entry -= before;
            }
            table->setValue(*entry, value);
            vm.writeBarrier(table);
            return true;
        }
    } else if (!state || createIfNeeded == CreateIfNeeded::No)
        return false;

    string->value(globalObject);
    RETURN_IF_EXCEPTION(scope, false);
    if (!table) {
        bool isNarrow = !state->data.usesWideReferences;
        // AOTStubs.cpp goes by this.
        Width keyWidth = isNarrow ? Width::Narrow : Width::Wide;
        Width valueWidth = !Traits::hasValueData ? Width::None : isNarrow && canHold(Width::Narrow, value) ? Width::Narrow : Width::Wide;
        table = create(vm, state->compactTableStructure(), keyWidth, valueWidth, leastCapacity);
        globalObject->aotInstance()->compactTableStructureID = table->structureID().bits();
        owner->m_compactTable.set(vm, owner, table);
        if (state->isCounting()) [[unlikely]]
            state->countCompactTable(makeString("made key "_s, static_cast<unsigned>(keyWidth), " value "_s, static_cast<unsigned>(valueWidth)), 0);
    }
    if (!canHold(table->keyWidth(), string)) [[unlikely]]
        return false;
    bool holdsValue = canHold(table->m_valueWidth, value);
    constexpr uint32_t maxEntriesOfKnownGrowth = 64;
    static std::atomic<uint32_t> capacitiesAndIndexBitsOfGrownTables[3][maxEntriesOfKnownGrowth / 2];
    std::atomic<uint32_t>* knownGrowth = nullptr;
    if (uint32_t count = table->m_used; count == table->m_capacity && holdsValue && count == table->m_alive && count < maxEntriesOfKnownGrowth && table->keyWidth() == Width::Narrow) {
        Width valueWidth = table->m_valueWidth;
        knownGrowth = &capacitiesAndIndexBitsOfGrownTables[static_cast<unsigned>(valueWidth) / 4][count / 2];
        if (uint32_t known = knownGrowth->load(std::memory_order_relaxed)) [[likely]] {
            uint32_t capacity = known & 0xffffff;
            unsigned indexBits = known >> 24;
            size_t keys = offsetOfIndex() + (indexBits ? sizeof(uint16_t) << indexBits : maxCapacityWithTags);
            size_t size = MarkedSpace::optimalSizeFor(keys + capacity * (static_cast<size_t>(Width::Narrow) + static_cast<size_t>(valueWidth)));
            CompactTable* larger = new (NotNull, allocateCell<CompactTable>(vm, size)) CompactTable(vm, table->structure());
            larger->m_capacity = capacity;
            larger->m_indexBits = indexBits;
            larger->m_keysShift = indexBits ? indexBits : keysShiftWithTags;
            larger->m_valueWidth = valueWidth;
            constexpr size_t maxWordsInLine = 8;
            auto copyWords = [](uint64_t*& to, const uint64_t* from, size_t words) ALWAYS_INLINE_LAMBDA {
                if (words > maxWordsInLine) {
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
                if (words > maxWordsInLine) {
                    memset(to, static_cast<int>(word & 0xff), words * sizeof(uint64_t));
                    to += words;
                    return;
                }
                for (size_t i = 0; i < words; ++i) {
                    *to++ = word;
                    WTF::compilerFence();
                }
            };
            unsigned hash = hashOf(string);
            uint64_t* to = larger->at<uint64_t>(offsetOfIndex());
            if (indexBits)
                fillWords(to, std::numeric_limits<uint64_t>::max(), (keys - offsetOfIndex()) / sizeof(uint64_t));
            else
                *to++ = *table->template at<uint64_t>(offsetOfIndex()) | static_cast<uint64_t>(shortTagOf(hash)) << (count * 8);
            uint32_t* keysOfLarger = reinterpret_cast<uint32_t*>(to);
            copyWords(to, table->template at<uint64_t>(table->offsetOfKeys()), count / 2);
            fillWords(to, 0, (capacity - count) / 2);
            size_t wordsOfValues = count * static_cast<size_t>(valueWidth) / sizeof(uint64_t);
            copyWords(to, table->template at<uint64_t>(table->offsetOfValues()), wordsOfValues);
            fillWords(to, 0, larger->at<uint64_t>(size) - to);
            vm.mutatorFence();
            keysOfLarger[count] = MarkedBlockCage::encode(string);
            larger->setValue(count, value);
            if (indexBits) {
                uint32_t mask = (1u << indexBits) - 1;
                uint16_t* slots = larger->at<uint16_t>(offsetOfIndex());
                for (uint32_t entry = 0; entry <= count; ++entry) {
                    unsigned hashOfEntry = entry == count ? hash : uncheckedDowncast<JSString>(MarkedBlockCage::decode(keysOfLarger[entry]))->tryGetValueImpl()->existingHash();
                    uint32_t i = hashOfEntry & mask;
                    while (slots[i] != freeShortSlot)
                        i = (i + 1) & mask;
                    slots[i] = shortTagOf(hashOfEntry) << entryBitsOfShortSlot | entry;
                }
            }
            WTF::storeStoreFence();
            larger->m_used = count + 1;
            larger->m_alive = count + 1;
            vm.writeBarrier(larger);
            if (Options::validateAOTTypedFacts()) [[unlikely]] {
                CompactTable* byTheOldWay = table->copyWithRoom(vm, valueWidth);
                RELEASE_ASSERT(byTheOldWay);
                byTheOldWay->append(vm, string, hash, value);
                RELEASE_ASSERT(!memcmp(byTheOldWay->at<uint8_t>(sizeof(JSCell)), larger->at<uint8_t>(sizeof(JSCell)), size - sizeof(JSCell)));
            }
            replaceBy(larger, "grown"_s);
            return true;
        }
    }
    if (table->m_used == table->m_capacity || !holdsValue) {
        CompactTable* larger = table->copyWithRoom(vm, holdsValue ? table->m_valueWidth : Width::Wide);
        if (!larger)
            return false;
        if (knownGrowth && larger->m_capacity <= maxCapacityWithShortSlots)
            knownGrowth->store(larger->m_capacity | static_cast<uint32_t>(larger->m_indexBits) << 24, std::memory_order_relaxed);
        replaceBy(larger, holdsValue ? "grown"_s : "widened"_s);
    }
    table->append(vm, string, hashOf(string), value);
    return true;
}

template<typename Traits>
bool CompactTable::remove(JSGlobalObject* globalObject, JSOrderedHashTable<Traits>* owner, JSValue key)
{
    auto* table = uncheckedDowncast<CompactTable>(owner->m_compactTable.get());
    auto entry = table->find(globalObject, key);
    if (!entry)
        return false;
    table->setKey(*entry, nullptr);
    table->setValue(*entry, JSValue());
    --table->m_alive;
    return true;
}

template<typename Traits>
void CompactTable::clear(VM& vm, JSOrderedHashTable<Traits>* owner)
{
    auto* table = uncheckedDowncast<CompactTable>(owner->m_compactTable.get());
    // The collector goes up to `used` as it has read it: what is there stays valid until it is written anew.
    if (table->m_state == State::Live && table->hasTags()) {
        table->m_used = 0;
        table->m_alive = 0;
        *table->template at<uint64_t>(offsetOfIndex()) = 0;
        return;
    }
    table->becomeObsolete(vm, State::Cleared, nullptr);
    owner->m_compactTable.clear();
}

template<typename Traits>
void CompactTable::convert(VM& vm, JSOrderedHashTable<Traits>* owner)
{
    using Helper = JSOrderedHashTableHelper<Traits>;
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    JSGlobalObject* globalObject = owner->realm();
    auto* table = uncheckedDowncast<CompactTable>(owner->m_compactTable.get());
    RELEASE_ASSERT(!owner->m_storage);
    if (Typed::State* state = stateOf(globalObject); state && state->isCounting()) [[unlikely]] {
        // Who wants the storage: behind this function and JSOrderedHashTable::convertCompactTable().
        StringPrintStream callers;
        auto trace = StackTrace::captureStackTrace(5, 2);
        for (void* returnAddress : trace->stack())
            callers.print(" ", RawPointer(returnAddress));
        state->countCompactTable(makeString("converted for"_s, callers.toString()), table->size());
    }

    typename Helper::TableSize capacity = Helper::InitialCapacity;
    while (Helper::dataCapacity(capacity) < table->size())
        capacity *= 2;
    auto* storage = Helper::tryCreate(globalObject, 0, 0, capacity);
    scope.releaseAssertNoException();
    owner->m_storage.set(vm, owner, storage);
    for (uint32_t entry = 0; entry < table->m_used; ++entry) {
        JSString* key = table->keyAt(entry);
        if (!key)
            continue;
        Helper::addNormalized(globalObject, owner, owner->storageRef(), key, table->valueAt(entry), hashOf(key));
        scope.releaseAssertNoException();
    }
    table->becomeObsolete(vm, State::Replaced, owner->m_storage.get());
    owner->m_compactTable.clear();
}

template<typename Traits>
CompactTable::Position CompactTable::next(VM& vm, JSOrderedHashTable<Traits>* owner, JSCell* table, uint32_t entry)
{
    using Helper = JSOrderedHashTableHelper<Traits>;
    for (;;) {
        if (!table) {
            table = owner->m_compactTable ? owner->m_compactTable.get() : owner->m_storage.get();
            entry = 0;
            if (!table)
                return { };
        }
        if (table->classInfo() != info()) {
            auto found = Helper::transitAndNext(vm, *uncheckedDowncast<JSCellButterfly>(table), entry);
            if (!found.storage)
                return { };
            return { found.storage, found.entry, found.key, found.value };
        }
        auto* compact = uncheckedDowncast<CompactTable>(table);
        switch (compact->m_state) {
        case State::Live:
        case State::Iterated:
            for (; entry < compact->m_used; ++entry) {
                if (JSString* key = compact->keyAt(entry))
                    return { compact, entry, key, compact->valueAt(entry) };
            }
            return { };
        case State::Cleared:
            table = nullptr;
            break;
        case State::Replaced: {
            uint32_t before = 0;
            while (before < compact->numberOfDeletedEntries() && compact->deletedEntries()[before] < entry)
                ++before;
            entry -= before;
            table = compact->successor();
            break;
        }
        }
    }
}

JSCell* CompactTable::copyOfPosition(VM& vm, JSCell* position)
{
    auto* record = uncheckedDowncast<JSCellButterfly>(position);
    JSCellButterfly* copy = JSCellButterfly::create(vm, CopyOnWriteArrayWithContiguous, 2);
    copy->setIndex(vm, 0, record->get(0));
    copy->setIndex(vm, 1, record->get(1));
    return copy;
}

// The record: 0 the collection, or the iterator; 1 the mode; 2 the value; 3 the table, or null; 4 the number of the entry to go on from.
static constexpr int32_t iterationKindMask = 3;
static constexpr int32_t iterationIsOfSet = 4;
static constexpr int32_t iterationGoesThroughIterator = 8;
static constexpr int32_t iterationIsOfGenerator = 16;
static constexpr int32_t iterationIsOfArray = 32;
static constexpr int32_t iterationIsOfString = 64;
static constexpr int32_t iterationIsOfMatches = 128;
static constexpr int32_t iterationIsOfTypedArray = 512;

static bool arrayIteratorProtocolIsFastAndNonObservable(VM& vm, JSArrayIterator* iterator)
{
    JSGlobalObject* globalObject = iterator->realm();
    if (!globalObject->isArrayPrototypeIteratorProtocolFastAndNonObservable() || iterator->getPrototypeDirect() != globalObject->arrayIteratorPrototype())
        return false;
    if (iterator->hasCustomProperties()) {
        if (iterator->getDirectOffset(vm, vm.propertyNames->next) != invalidOffset
            || iterator->getDirectOffset(vm, vm.propertyNames->returnKeyword) != invalidOffset
            || iterator->getDirectOffset(vm, vm.propertyNames->iteratorSymbol) != invalidOffset)
            return false;
    }
    // JSArrayIterator::next() is for arrays that keep their length where JSArray does.
    JSValue iterated = iterator->internalField(JSArrayIterator::Field::IteratedObject).get();
    return iterated.isCell() && iterated.asCell()->classInfo() == JSArray::info();
}

template<typename Functor>
static ALWAYS_INLINE auto tryOpenWith(JSGlobalObject* globalObject, JSValue iterable, const Functor& makeRecord) -> decltype(makeRecord(nullptr, 0))
{
    VM& vm = globalObject->vm();
    if (!iterable.isCell())
        return nullptr;
    // In this realm no prototype of an iterator can be given a `return` method.
    JSCell* cell = iterable.asCell();
    int32_t mode;
    switch (cell->type()) {
    case StringType:
        if (!globalObject->isStringPrototypeIteratorProtocolFastAndNonObservable())
            return nullptr;
        mode = iterationIsOfString;
        break;
    case JSMapType:
        if (uncheckedDowncast<JSMap>(cell)->realm() != globalObject || !uncheckedDowncast<JSMap>(cell)->isIteratorProtocolFastAndNonObservable())
            return nullptr;
        mode = static_cast<int32_t>(IterationKind::Entries);
        break;
    case JSSetType:
        if (uncheckedDowncast<JSSet>(cell)->realm() != globalObject || !uncheckedDowncast<JSSet>(cell)->isIteratorProtocolFastAndNonObservable())
            return nullptr;
        mode = static_cast<int32_t>(IterationKind::Values) | iterationIsOfSet;
        break;
    case JSMapIteratorType:
        if (uncheckedDowncast<JSMapIterator>(cell)->realm() != globalObject || !mapIteratorProtocolIsFastAndNonObservable(vm, uncheckedDowncast<JSMapIterator>(cell)))
            return nullptr;
        mode = iterationGoesThroughIterator;
        break;
    case JSSetIteratorType:
        if (uncheckedDowncast<JSSetIterator>(cell)->realm() != globalObject || !setIteratorProtocolIsFastAndNonObservable(vm, uncheckedDowncast<JSSetIterator>(cell)))
            return nullptr;
        mode = iterationGoesThroughIterator | iterationIsOfSet;
        break;
    case JSArrayIteratorType:
        if (uncheckedDowncast<JSArrayIterator>(cell)->realm() != globalObject || !arrayIteratorProtocolIsFastAndNonObservable(vm, uncheckedDowncast<JSArrayIterator>(cell)))
            return nullptr;
        mode = iterationGoesThroughIterator | iterationIsOfArray;
        break;
    // What matchAll() makes, as it was made: nothing of its own, and the prototype it was given, which stays what it is: JSGlobalObject::makeIntrinsicsImmutable().
    case JSRegExpStringIteratorType:
        if (cell->structure() != globalObject->regExpStringIteratorStructure())
            return nullptr;
        mode = iterationGoesThroughIterator | iterationIsOfMatches;
        break;
    case ObjectType: {
        auto* generator = dynamicDowncast<Generator>(cell);
        if (!generator || generator->realm() != globalObject || !generator->iteratesAsMade(globalObject))
            return nullptr;
        mode = iterationIsOfGenerator;
        break;
    }
    default: {
        if (!isTypedArrayType(cell->type()))
            return nullptr;
        auto* view = uncheckedDowncast<JSArrayBufferView>(cell);
        if (view->realm() != globalObject || !view->isIteratorProtocolFastAndNonObservable() || view->isOutOfBounds())
            return nullptr;
        mode = iterationIsOfMatches | iterationIsOfTypedArray;
        break;
    }
    }
    static_assert(static_cast<int32_t>(IterationKind::Keys) <= iterationKindMask && static_cast<int32_t>(IterationKind::Values) <= iterationKindMask && static_cast<int32_t>(IterationKind::Entries) <= iterationKindMask);
    return makeRecord(cell, mode);
}

template<typename Record>
static ALWAYS_INLINE Record* fill(VM& vm, Record* record, JSCell* cell, int32_t mode)
{
    record->setIndex(vm, 0, cell);
    record->setIndex(vm, 1, jsNumber(mode));
    record->setIndex(vm, 2, jsUndefined());
    record->setIndex(vm, 3, jsNull());
    record->setIndex(vm, 4, jsNumber(0));
    return record;
}

JSCellButterfly* CollectionIteration::tryOpen(JSGlobalObject* globalObject, JSValue iterable)
{
    VM& vm = globalObject->vm();
    return tryOpenWith(globalObject, iterable, [&](JSCell* cell, int32_t mode) {
        return fill(vm, JSCellButterfly::create(vm, CopyOnWriteArrayWithContiguous, numberOfWords), cell, mode);
    });
}

bool CollectionIteration::tryOpen(JSGlobalObject* globalObject, JSValue iterable, InFrame* record)
{
    return tryOpenWith(globalObject, iterable, [&](JSCell* cell, int32_t mode) {
        return fill(globalObject->vm(), record, cell, mode);
    });
}

// In place, however large the function around it grows. A record holds values, never doubles.
static ALWAYS_INLINE void storeWordOfRecord(VM& vm, JSCellButterfly* record, unsigned word, JSValue value)
{
    ASSERT(hasContiguous(record->indexingType()));
    record->toButterfly()->contiguous().atUnsafe(word).set(vm, record, value);
}

static ALWAYS_INLINE void storeWordOfRecord(VM& vm, CollectionIteration::InFrame* record, unsigned word, JSValue value)
{
    record->setIndex(vm, word, value);
}

template<typename Record>
static ALWAYS_INLINE bool stepThrough(JSGlobalObject* globalObject, Record* record)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    int32_t mode = record->get(1).asInt32();
    // Compiled code steps through those by itself. They have two words.
    RELEASE_ASSERT(!(mode & CollectionIteration::isOfEntriesOfArrayByIndex));
    JSCell* cell = record->get(0).asCell();
    JSValue value;
    if (mode & iterationIsOfGenerator) {
        bool isDone;
        value = uncheckedDowncast<Generator>(cell)->resume(globalObject, Generator::ResumeMode::Next, jsUndefined(), isDone);
        RETURN_IF_EXCEPTION(scope, false);
        if (isDone)
            return false;
    } else if (mode & iterationIsOfString) {
        auto [next, position] = JSStringIterator::advance(globalObject, vm, uncheckedDowncast<JSString>(cell), record->get(4).asInt32());
        RETURN_IF_EXCEPTION(scope, false);
        if (!next)
            return false;
        storeWordOfRecord(vm, record, 4, jsNumber(position));
        value = next;
    } else if (mode & iterationIsOfMatches && mode & iterationIsOfTypedArray) {
        auto* view = uncheckedDowncast<JSArrayBufferView>(cell);
        validateTypedArray(globalObject, view);
        RETURN_IF_EXCEPTION(scope, false);
        size_t index = static_cast<size_t>(record->get(4).asNumber());
        if (index >= view->length())
            return false;
        storeWordOfRecord(vm, record, 4, jsNumber(static_cast<double>(index + 1)));
        if (index <= std::numeric_limits<unsigned>::max() && view->canGetIndexQuicklyForTypedArray(static_cast<unsigned>(index)))
            value = view->getIndexQuicklyForTypedArray(static_cast<unsigned>(index));
        else {
            value = view->get(globalObject, static_cast<uint64_t>(index));
            RETURN_IF_EXCEPTION(scope, false);
        }
    } else if (mode & iterationIsOfMatches) {
        // As JSRegExpStringIterator::next().
        auto* iterator = uncheckedDowncast<JSRegExpStringIterator>(cell);
        if (iterator->isDone())
            return false;
        value = iterator->nextImpl(globalObject);
        RETURN_IF_EXCEPTION(scope, false);
        if (value.isNull())
            return false;
    } else if (mode & iterationGoesThroughIterator) {
        bool hasNext = mode & iterationIsOfArray ? uncheckedDowncast<JSArrayIterator>(cell)->next(globalObject, value)
            : mode & iterationIsOfSet ? uncheckedDowncast<JSSetIterator>(cell)->next(globalObject, value)
            : uncheckedDowncast<JSMapIterator>(cell)->next(globalObject, value);
        RETURN_IF_EXCEPTION(scope, false);
        if (!hasNext)
            return false;
    } else {
        JSCell* table = record->get(3).isCell() ? record->get(3).asCell() : nullptr;
        uint32_t entry = record->get(4).asUInt32();
        CompactTable::Position position = mode & iterationIsOfSet
            ? CompactTable::next<SetTraits>(vm, uncheckedDowncast<JSSet>(cell), table, entry)
            : CompactTable::next<MapTraits>(vm, uncheckedDowncast<JSMap>(cell), table, entry);
        if (!position.table)
            return false;
        if (auto* compact = dynamicDowncast<CompactTable>(position.table))
            compact->markAsIterated();
        storeWordOfRecord(vm, record, 3, position.table);
        storeWordOfRecord(vm, record, 4, jsNumber(position.entry + 1));
        value = position.key;
        if (static_cast<IterationKind>(mode & iterationKindMask) == IterationKind::Entries) {
            value = constructArrayPair(globalObject, position.key, position.value);
            RETURN_IF_EXCEPTION(scope, false);
        }
    }
    storeWordOfRecord(vm, record, 2, value);
    return true;
}

bool CollectionIteration::step(JSGlobalObject* globalObject, JSCellButterfly* record)
{
    return stepThrough(globalObject, record);
}

bool CollectionIteration::step(JSGlobalObject* globalObject, InFrame* record)
{
    return stepThrough(globalObject, record);
}

template<typename Record>
static ALWAYS_INLINE void closeRecord(JSGlobalObject* globalObject, Record* record)
{
    int32_t mode = record->get(1).asInt32();
    if (mode & iterationGoesThroughIterator) {
        if (JSObject* iterator = asObject(record->get(0)); iterator->hasCustomProperties())
            iteratorClose(globalObject, iterator);
        return;
    }
    if (!(mode & iterationIsOfGenerator))
        return;
    auto* generator = uncheckedDowncast<Generator>(record->get(0).asCell());
    // `next` was read when the loop was opened. `return` is looked up now: the body of the loop may have put another in its way.
    if (!generator->iteratesAsMade(globalObject)) {
        iteratorClose(globalObject, generator);
        return;
    }
    bool isDone;
    generator->resume(globalObject, Generator::ResumeMode::Return, jsUndefined(), isDone);
}

void CollectionIteration::close(JSGlobalObject* globalObject, JSCellButterfly* record)
{
    closeRecord(globalObject, record);
}

void CollectionIteration::close(JSGlobalObject* globalObject, InFrame* record)
{
    closeRecord(globalObject, record);
}

// Room for so many more elements, which are holes until they are stored. None: the array is left as it was.
static WriteBarrier<Unknown>* tryGrow(JSGlobalObject* globalObject, JSArray* array, unsigned count)
{
    VM& vm = globalObject->vm();
    unsigned length = array->length();
    // Beyond, an array that is mostly holes gets other storage.
    if (count >= MIN_SPARSE_ARRAY_INDEX - std::min(length, MIN_SPARSE_ARRAY_INDEX) || !array->tryMakeWritableContiguous(vm).data())
        return nullptr;
    if (!array->setLength(globalObject, length + count))
        return nullptr;
    RELEASE_ASSERT(hasContiguous(array->indexingType()));
    return array->butterfly()->contiguous().data() + length;
}

// From the first entry on. Nothing of the program runs meanwhile.
template<typename Traits>
static bool tryAppendEntries(JSGlobalObject* globalObject, JSArray* target, JSOrderedHashTable<Traits>* owner, IterationKind kind)
{
    using Helper = JSOrderedHashTableHelper<Traits>;
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* compactTable = owner->m_storage ? nullptr : static_cast<CompactTable*>(owner->m_compactTable.get());
    unsigned size = compactTable ? compactTable->size() : owner->size();
    if (!size)
        return true;
    WriteBarrier<Unknown>* next = tryGrow(globalObject, target, size);
    RETURN_IF_EXCEPTION(scope, true);
    if (!next)
        return false;
    bool hasFailed = false;
    auto append = [&](JSValue key, JSValue value) ALWAYS_INLINE_LAMBDA {
        if (kind != IterationKind::Entries) {
            (next++)->setWithoutWriteBarrier(kind == IterationKind::Keys ? key : value);
            return;
        }
        JSValue pair = constructArrayPair(globalObject, key, value);
        if (scope.exception()) [[unlikely]] {
            hasFailed = true;
            return;
        }
        (next++)->set(vm, target, pair);
    };
    if (owner->m_storage) {
        auto& storage = owner->storageRef();
        for (auto index = Helper::dataTableStartIndex(Helper::capacity(storage)); !hasFailed; index += Helper::EntrySize) {
            JSValue key = Helper::get(storage, index);
            if (!key)
                break;
            if (Helper::isDeleted(vm, key))
                continue;
            if constexpr (Traits::hasValueData)
                append(key, Helper::get(storage, index + 1));
            else
                append(key, key);
        }
    } else {
        auto* table = compactTable;
        if (!Traits::hasValueData && kind != IterationKind::Entries && table->keyWidth() == CompactTable::Width::Narrow) {
            uintptr_t base = MarkedBlockCage::base();
            auto keys = table->narrowKeys();
            if (keys.size() == size) {
#pragma clang loop vectorize(disable) interleave(disable)
                for (uint32_t index : keys)
                    (next++)->setWithoutWriteBarrier(JSValue(std::bit_cast<JSCell*>(base + (static_cast<uintptr_t>(index) << MarkedBlockCage::shift))));
            } else {
#pragma clang loop vectorize(disable) interleave(disable)
                for (uint32_t index : keys) {
                    if (index)
                        (next++)->setWithoutWriteBarrier(JSValue(std::bit_cast<JSCell*>(base + (static_cast<uintptr_t>(index) << MarkedBlockCage::shift))));
                }
            }
        } else {
            for (uint32_t entry = 0, used = table->used(); entry < used && !hasFailed; ++entry) {
                if (JSString* key = table->keyAt(entry))
                    append(key, Traits::hasValueData ? table->valueAt(entry) : JSValue(key));
            }
        }
    }
    vm.writeBarrier(target);
    return true;
}

template<typename Traits, typename Iterator>
static void appendRestOf(JSGlobalObject* globalObject, JSArray* target, Iterator* iterator)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    // One that has not taken a step is at the start of whatever the collection has.
    JSValue entry = iterator->internalField(Iterator::Field::Entry).get();
    JSCell* storage = iterator->tryGetStorage();
    auto* owner = iterator->iteratedObject();
    if (entry.isInt32() && !entry.asInt32() && (!storage || storage == owner->m_storage.get())) {
        bool isAppended = tryAppendEntries<Traits>(globalObject, target, owner, iterator->kind());
        RETURN_IF_EXCEPTION(scope, void());
        if (isAppended) {
            iterator->close(vm);
            return;
        }
    }
    JSValue value;
    while (iterator->next(globalObject, value)) {
        target->push(globalObject, value);
        RETURN_IF_EXCEPTION(scope, void());
    }
}

static bool tryAppendCodePoints(JSGlobalObject* globalObject, JSArray* target, JSString* string)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto view = string->view(globalObject);
    RETURN_IF_EXCEPTION(scope, true);
    unsigned length = view->length();
    if (view->is8Bit()) {
        WriteBarrier<Unknown>* next = tryGrow(globalObject, target, length);
        RETURN_IF_EXCEPTION(scope, true);
        if (!next)
            return false;
        for (auto character : view->span8())
            (next++)->setWithoutWriteBarrier(vm.smallStrings.singleCharacterString(character));
        vm.writeBarrier(target);
        return true;
    }
    auto characters = view->span16();
    for (unsigned i = 0; i < length;) {
        unsigned units = U16_IS_LEAD(characters[i]) && i + 1 < length && U16_IS_TRAIL(characters[i + 1]) ? 2 : 1;
        JSString* codePoint = jsSubstring(globalObject, vm, string, i, units);
        RETURN_IF_EXCEPTION(scope, true);
        target->push(globalObject, codePoint);
        RETURN_IF_EXCEPTION(scope, true);
        i += units;
    }
    return true;
}

bool CollectionIteration::tryAppendTo(JSGlobalObject* globalObject, JSArray* target, JSValue iterable)
{
    VM& vm = globalObject->vm();
    if (!iterable.isCell())
        return false;
    JSCell* cell = iterable.asCell();
    switch (cell->type()) {
    case StringType:
        return globalObject->isStringPrototypeIteratorProtocolFastAndNonObservable() && tryAppendCodePoints(globalObject, target, asString(cell));
    case JSMapType: {
        auto* map = uncheckedDowncast<JSMap>(cell);
        return map->realm() == globalObject && map->isIteratorProtocolFastAndNonObservable() && tryAppendEntries<MapTraits>(globalObject, target, map, IterationKind::Entries);
    }
    case JSSetType: {
        auto* set = uncheckedDowncast<JSSet>(cell);
        return set->realm() == globalObject && set->isIteratorProtocolFastAndNonObservable() && tryAppendEntries<SetTraits>(globalObject, target, set, IterationKind::Values);
    }
    case JSMapIteratorType: {
        auto* iterator = uncheckedDowncast<JSMapIterator>(cell);
        if (iterator->realm() != globalObject || !mapIteratorProtocolIsFastAndNonObservable(vm, iterator))
            return false;
        appendRestOf<MapTraits>(globalObject, target, iterator);
        return true;
    }
    case JSSetIteratorType: {
        auto* iterator = uncheckedDowncast<JSSetIterator>(cell);
        if (iterator->realm() != globalObject || !setIteratorProtocolIsFastAndNonObservable(vm, iterator))
            return false;
        appendRestOf<SetTraits>(globalObject, target, iterator);
        return true;
    }
    default:
        return false;
    }
}

template<typename Visitor>
ALWAYS_INLINE void CompactTable::visitEntries(Visitor& visitor, uint32_t first, uint32_t last) const
{
    if (keyWidth() == Width::Narrow && m_valueWidth != Width::Wide) [[likely]] {
        uintptr_t base = MarkedBlockCage::base();
        auto decode = [&](uint32_t index) ALWAYS_INLINE_LAMBDA {
            return std::bit_cast<JSCell*>(base + (static_cast<uintptr_t>(index) << MarkedBlockCage::shift));
        };
        const uint32_t* keys = at<uint32_t>(offsetOfKeys());
        if (m_valueWidth == Width::None) {
            for (uint32_t entry = first; entry < last; ++entry) {
                if (uint32_t key = keys[entry])
                    visitor.appendUnbarriered(decode(key));
            }
            return;
        }
        const uint32_t* values = at<uint32_t>(offsetOfValues());
        for (uint32_t entry = first; entry < last; ++entry) {
            if (uint32_t key = keys[entry])
                visitor.appendUnbarriered(decode(key));
            if (uint32_t value = values[entry]; value > undefinedCell32)
                visitor.appendUnbarriered(decode(value));
        }
        return;
    }
    for (uint32_t entry = first; entry < last; ++entry) {
        if (JSString* key = keyAt(entry))
            visitor.appendUnbarriered(key);
        visitor.appendUnbarriered(valueAt(entry));
    }
}

template<typename Visitor>
void CompactTable::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* table = uncheckedDowncast<CompactTable>(cell);
    Base::visitChildren(cell, visitor);
    Locker locker { table->cellLock() };
    if (!table->isLive()) {
        if (JSCell* next = table->successor())
            visitor.appendUnbarriered(next);
        return;
    }
    uint32_t used = table->m_used;
    WTF::loadLoadFence();
    table->visitEntries(visitor, 0, used);
}

DEFINE_VISIT_CHILDREN(CompactTable);

#define AOT_TYPED_INSTANTIATE_COMPACT_TABLE(Traits) \
    template bool CompactTable::tryAdd<Traits>(JSGlobalObject*, JSOrderedHashTable<Traits>*, JSValue, JSValue, CreateIfNeeded, uint32_t); \
    template bool CompactTable::remove<Traits>(JSGlobalObject*, JSOrderedHashTable<Traits>*, JSValue); \
    template void CompactTable::clear<Traits>(VM&, JSOrderedHashTable<Traits>*); \
    template void CompactTable::convert<Traits>(VM&, JSOrderedHashTable<Traits>*); \
    template CompactTable::Position CompactTable::next<Traits>(VM&, JSOrderedHashTable<Traits>*, JSCell*, uint32_t);
AOT_TYPED_INSTANTIATE_COMPACT_TABLE(MapTraits)
AOT_TYPED_INSTANTIATE_COMPACT_TABLE(SetTraits)
#undef AOT_TYPED_INSTANTIATE_COMPACT_TABLE

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
