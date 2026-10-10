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
#include "IntlSegmentIterator.h"
#include "IntlSegments.h"
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
    table->m_keyWidth = key;
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
    return value.isCell() && MarkedBlockCage::contains(value.asCell()) && !(std::bit_cast<uintptr_t>(value.asCell()) & ((1 << cell32Shift) - 1));
}

JSString* CompactTable::keyAt(uint32_t entry) const
{
    if (m_keyWidth == Width::Wide)
        return at<JSString*>(offsetOfKeys())[entry];
    uint32_t index = at<uint32_t>(offsetOfKeys())[entry];
    return index ? uncheckedDowncast<JSString>(MarkedBlockCage::decode(index)) : nullptr;
}

void CompactTable::setKey(uint32_t entry, JSString* key)
{
    if (m_keyWidth == Width::Wide)
        at<JSString*>(offsetOfKeys())[entry] = key;
    else
        at<uint32_t>(offsetOfKeys())[entry] = key ? MarkedBlockCage::encode(key) : absentCell32;
}

JSValue CompactTable::valueAt(uint32_t entry) const
{
    switch (m_valueWidth) {
    case Width::None:
        return { };
    case Width::Wide:
        return JSValue::decode(at<EncodedJSValue>(offsetOfValues())[entry]);
    case Width::Narrow:
        break;
    }
    static_assert(absentCell32 == static_cast<uint32_t>(JSValue::ValueEmpty) && nullCell32 == JSValue::ValueNull && undefinedCell32 == JSValue::ValueUndefined);
    uint32_t index = at<uint32_t>(offsetOfValues())[entry];
    return index <= undefinedCell32 ? JSValue::decode(index) : JSValue(MarkedBlockCage::decode(index));
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
    if (!value.isString())
        return std::nullopt;
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
        if (m_keyWidth == Width::Wide) {
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
    vm.writeBarrier(this);
}

CompactTable* CompactTable::copyWithRoom(VM& vm, Width value) const
{
    uint64_t alive = size();
    if (alive >= maxCapacity)
        return nullptr;
    uint64_t leastCapacity = std::min<uint64_t>(m_deleted >= m_used / 2 ? alive + 1 : alive < 64 ? alive + alive / 2 + 1 : alive * 2, maxCapacity);
    CompactTable* copy = create(vm, structure(), m_keyWidth, value, leastCapacity);
    // The collector goes up to `used`, which is zero until all is written.
    size_t keyWidth = static_cast<size_t>(m_keyWidth);
    size_t valueWidth = static_cast<size_t>(m_valueWidth);
    const uint8_t* keys = at<uint8_t>(offsetOfKeys());
    const uint8_t* values = at<uint8_t>(offsetOfValues());
    uint8_t* keysOfCopy = copy->at<uint8_t>(copy->offsetOfKeys());
    uint8_t* valuesOfCopy = copy->at<uint8_t>(copy->offsetOfValues());
    uint32_t count = 0;
    if (!m_deleted && value == m_valueWidth) {
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
    if (!m_deleted && hasTags() && copy->hasTags())
        memcpy(copy->at<uint8_t>(offsetOfIndex()), at<uint8_t>(offsetOfIndex()), maxCapacityWithTags);
    else
        copy->fillIndex(count);
    WTF::storeStoreFence();
    copy->m_used = count;
    vm.writeBarrier(copy);
    return copy;
}

void CompactTable::becomeObsolete(VM& vm, State state, JSCell* next)
{
    RELEASE_ASSERT(isLive() && state > State::Iterated);
    {
        Locker locker { cellLock() };
        uint32_t count = 0;
        if (state == State::Replaced && m_deleted) {
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
bool CompactTable::tryAdd(JSGlobalObject* globalObject, JSOrderedHashTable<Traits>* owner, JSValue key, JSValue value)
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
    } else if (!state)
        return false;

    string->value(globalObject);
    RETURN_IF_EXCEPTION(scope, false);
    if (!table) {
        bool isNarrow = !state->data.usesWideReferences;
        // AOTStubs.cpp goes by this.
        Width keyWidth = isNarrow ? Width::Narrow : Width::Wide;
        Width valueWidth = !Traits::hasValueData ? Width::None : isNarrow && canHold(Width::Narrow, value) ? Width::Narrow : Width::Wide;
        table = create(vm, state->compactTableStructure(), keyWidth, valueWidth, initialCapacity);
        globalObject->aotInstance()->compactTableStructureID = table->structureID().bits();
        owner->m_compactTable.set(vm, owner, table);
        if (state->isCounting()) [[unlikely]]
            state->countCompactTable(makeString("made key "_s, static_cast<unsigned>(keyWidth), " value "_s, static_cast<unsigned>(valueWidth)), 0);
    }
    if (!canHold(table->m_keyWidth, string)) [[unlikely]]
        return false;
    bool holdsValue = canHold(table->m_valueWidth, value);
    if (table->m_used == table->m_capacity || !holdsValue) {
        CompactTable* larger = table->copyWithRoom(vm, holdsValue ? table->m_valueWidth : Width::Wide);
        if (!larger)
            return false;
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
    ++table->m_deleted;
    return true;
}

template<typename Traits>
void CompactTable::clear(VM& vm, JSOrderedHashTable<Traits>* owner)
{
    auto* table = uncheckedDowncast<CompactTable>(owner->m_compactTable.get());
    // The collector goes up to `used` as it has read it: what is there stays valid until it is written anew.
    if (table->m_state == State::Live && table->hasTags()) {
        table->m_used = 0;
        table->m_deleted = 0;
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
        StackTrace::captureStackTrace(5, 2)->forEachFrame([&](int, void*, const char* name) {
            callers.print(" ", name ? StringView::fromLatin1(name).left(60) : "?"_s);
        });
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
static constexpr int32_t iterationIsOfSegments = 512;
static constexpr int32_t iterationIsOfMatches = 128;

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

JSCellButterfly* CollectionIteration::tryOpen(JSGlobalObject* globalObject, JSValue iterable)
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
        // What `segmenter.segment(text)` makes, as it was made: nothing of its own, and the prototype it was given, which stays what it is, as does that of the iterator: see where JSGlobalObject
        // makes the two structures. The iterator is fresh, and nobody else gets hold of it. Where the Segmenter cuts simple graphemes itself the making of it asks ICU nothing and cannot fail.
        if (cell->classInfo() == IntlSegments::info()) {
            auto* segments = uncheckedDowncast<IntlSegments>(cell);
            if (cell->structure() != globalObject->segmentsStructure() || !segments->cutsSimpleGraphemesItself())
                return nullptr;
            auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
            cell = segments->createSegmentIterator(globalObject);
            scope.assertNoException();
            RELEASE_ASSERT(cell);
            mode = iterationGoesThroughIterator | iterationIsOfSegments;
            break;
        }
        auto* generator = dynamicDowncast<Generator>(cell);
        if (!generator || generator->realm() != globalObject || !generator->iteratesAsMade(globalObject))
            return nullptr;
        mode = iterationIsOfGenerator;
        break;
    }
    default:
        return nullptr;
    }
    static_assert(static_cast<int32_t>(IterationKind::Keys) <= iterationKindMask && static_cast<int32_t>(IterationKind::Values) <= iterationKindMask && static_cast<int32_t>(IterationKind::Entries) <= iterationKindMask);
    JSCellButterfly* record = JSCellButterfly::create(vm, CopyOnWriteArrayWithContiguous, 5);
    record->setIndex(vm, 0, cell);
    record->setIndex(vm, 1, jsNumber(mode));
    record->setIndex(vm, 2, jsUndefined());
    record->setIndex(vm, 3, jsNull());
    record->setIndex(vm, 4, jsNumber(0));
    return record;
}

// In place, however large the function around it grows. A record holds values, never doubles.
static ALWAYS_INLINE void storeWordOfRecord(VM& vm, JSCellButterfly* record, unsigned word, JSValue value)
{
    ASSERT(hasContiguous(record->indexingType()));
    record->toButterfly()->contiguous().atUnsafe(word).set(vm, record, value);
}

bool CollectionIteration::step(JSGlobalObject* globalObject, JSCellButterfly* record)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    int32_t mode = record->get(1).asInt32();
    // Compiled code steps through those by itself. They have two words.
    RELEASE_ASSERT(!(mode & isOfEntriesOfArrayByIndex));
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
    } else if (mode & iterationIsOfMatches) {
        // As JSRegExpStringIterator::next().
        auto* iterator = uncheckedDowncast<JSRegExpStringIterator>(cell);
        if (iterator->isDone())
            return false;
        value = iterator->nextImpl(globalObject);
        RETURN_IF_EXCEPTION(scope, false);
        if (value.isNull())
            return false;
    } else if (mode & iterationIsOfSegments) {
        JSObject* data = uncheckedDowncast<IntlSegmentIterator>(cell)->nextGrapheme(globalObject);
        RETURN_IF_EXCEPTION(scope, false);
        if (!data)
            return false;
        value = data;
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

void CollectionIteration::close(JSGlobalObject* globalObject, JSCellButterfly* record)
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
    unsigned size = owner->size();
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
        auto* table = uncheckedDowncast<CompactTable>(owner->m_compactTable.get());
        for (uint32_t entry = 0; entry < table->used() && !hasFailed; ++entry) {
            if (JSString* key = table->keyAt(entry))
                append(key, Traits::hasValueData ? table->valueAt(entry) : JSValue(key));
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
    for (uint32_t entry = 0; entry < used; ++entry) {
        if (JSString* key = table->keyAt(entry))
            visitor.appendUnbarriered(key);
        visitor.appendUnbarriered(table->valueAt(entry));
    }
}

DEFINE_VISIT_CHILDREN(CompactTable);

#define AOT_TYPED_INSTANTIATE_COMPACT_TABLE(Traits) \
    template bool CompactTable::tryAdd<Traits>(JSGlobalObject*, JSOrderedHashTable<Traits>*, JSValue, JSValue); \
    template bool CompactTable::remove<Traits>(JSGlobalObject*, JSOrderedHashTable<Traits>*, JSValue); \
    template void CompactTable::clear<Traits>(VM&, JSOrderedHashTable<Traits>*); \
    template void CompactTable::convert<Traits>(VM&, JSOrderedHashTable<Traits>*); \
    template CompactTable::Position CompactTable::next<Traits>(VM&, JSOrderedHashTable<Traits>*, JSCell*, uint32_t);
AOT_TYPED_INSTANTIATE_COMPACT_TABLE(MapTraits)
AOT_TYPED_INSTANTIATE_COMPACT_TABLE(SetTraits)
#undef AOT_TYPED_INSTANTIATE_COMPACT_TABLE

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
