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

static uint8_t shortTagOf(unsigned hash) { return hash >> CompactTable::shortTagShift; }
static uint32_t longTagOf(unsigned hash) { return hash >> CompactTable::longTagShift; }
static_assert(StringImpl::s_flagCount == 8 && CompactTable::shortTagShift + 8 == 24 && CompactTable::longTagShift + 32 - CompactTable::longSlotEntryBits == 24);

static std::atomic<uint64_t> s_knownGrowths[3][CompactTable::maxKnownGrowthEntries / 2];

std::atomic<uint64_t>* CompactTable::knownGrowths()
{
    return &s_knownGrowths[0][0];
}

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

CompactTable* CompactTable::create(VM& vm, Structure* structure, Width key, Width value, uint32_t minCapacity)
{
    RELEASE_ASSERT(key != Width::None && minCapacity <= maxCapacity);
    uint32_t capacity = roundUpToMultipleOf<2>(std::max<uint32_t>(minCapacity, 2));
    size_t size = MarkedSpace::optimalSizeFor(allocationSize(capacity, key, value));
    while (capacity < maxCapacity && allocationSize(capacity + 2, key, value) <= size)
        capacity += 2;

    CompactTable* table = new (NotNull, allocateCell<CompactTable>(vm, size)) CompactTable(vm, structure);
    table->m_capacity = capacity;
    table->m_indexBits = indexBitsFor(capacity);
    table->m_keysShiftAndFlags = (table->hasTags() ? keysShiftWithTags : table->m_indexBits + !table->hasShortSlots()) | (key == Width::Wide ? keysAreWide : 0);
    table->m_valueWidth = value;
    size_t keys = table->offsetOfKeys();
    memsetSpan(std::span { table->at<uint8_t>(offsetOfIndex()), keys - offsetOfIndex() }, table->hasTags() ? 0 : 0xff);
    zeroSpan(std::span { table->at<uint8_t>(keys), size - keys });
    table->finishCreation(vm);
    return table;
}

CompactTable* CompactTable::createInImage(VM& vm, Structure* structure, const TableInImage& entries, Typed::State* state)
{
    CompactTable* table = new (NotNull, allocateCell<CompactTable>(vm, offsetOfIndex() + (entries.hasValues ? 2 : 1) * sizeof(void*))) CompactTable(vm, structure);
    table->m_used = entries.count;
    table->m_alive = entries.count;
    table->m_capacity = entries.capacity | capacityIsInImage;
    table->m_indexBits = entries.indexBits | indexIsInImage;
    table->m_keysShiftAndFlags = entries.keysShift | keysAreInImage;
    table->m_valueWidth = entries.hasValues ? Width::InImage : Width::None;
    *table->at<const TableInImage*>(offsetOfIndex()) = &entries;
    if (entries.hasValues)
        *table->at<Typed::State*>(offsetOfIndex() + sizeof(void*)) = state;
    table->finishCreation(vm);
    return table;
}

JSValue CompactTable::valueInImage(uint32_t entry) const
{
    uint64_t encoded = tableInImage().values()[entry];
    JSValue value = JSValue::decode(encoded);
    if (!value.isCell())
        return value;
    Typed::State& state = **at<Typed::State*>(offsetOfIndex() + sizeof(void*));
    VM& vm = *state.instance.vm;
    if (encoded & Data::encodedStringIsSmall)
        return encoded >> 3 ? vm.smallStrings.singleCharacterString(static_cast<unsigned char>(encoded >> 3)) : vm.smallStrings.emptyString();
    return state.string(vm, Data::decodeStringConstant(encoded));
}

Vector<uint8_t> TableInImage::encode(std::span<const std::pair<uint32_t, std::span<const uint8_t>>> keys, std::span<const uint64_t> values)
{
    RELEASE_ASSERT(!keys.empty() && keys.size() <= CompactTable::maxCapacity && (values.empty() || values.size() == keys.size()));
    TableInImage header { };
    header.count = keys.size();
    header.alive = header.count;
    header.capacity = roundUpToMultipleOf<2>(std::max<uint32_t>(header.count, 2));
    header.indexBits = CompactTable::indexBitsFor(header.capacity);
    bool hasShortSlots = header.capacity <= CompactTable::maxCapacityWithShortSlots;
    header.keysShift = header.indexBits ? header.indexBits + !hasShortSlots : CompactTable::keysShiftWithTags;
    header.hasValues = !values.empty();
    size_t sizeOfIndex = static_cast<size_t>(2) << header.keysShift;
    size_t offsetOfStrings = sizeof(TableInImage) + sizeOfIndex;
    size_t offsetOfValues = offsetOfStrings + sizeof(uint32_t) * header.capacity;
    Vector<uint8_t> bytes(FillWith { }, offsetOfValues + (header.hasValues ? sizeof(uint64_t) * header.capacity : 0), static_cast<uint8_t>(0));
    memcpy(bytes.mutableSpan().data(), &header, sizeof(header));
    uint8_t* index = bytes.mutableSpan().data() + sizeof(TableInImage);
    if (header.indexBits)
        memset(index, 0xff, sizeOfIndex);
    uint32_t* strings = reinterpret_cast<uint32_t*>(bytes.mutableSpan().data() + offsetOfStrings);
    memcpy(bytes.mutableSpan().data() + offsetOfValues, values.data(), values.size_bytes());
    uint32_t mask = (1u << header.indexBits) - 1;
    for (uint32_t entry = 0; entry < header.count; ++entry) {
        auto [string, text] = keys[entry];
        strings[entry] = string;
        unsigned hash = RapidHash::computeHashAndMaskTop8Bits<Latin1Character>(byteCast<Latin1Character>(text));
        if (!header.indexBits) {
            index[entry] = shortTagOf(hash);
            continue;
        }
        uint32_t i = hash & mask;
        if (hasShortSlots) {
            uint16_t* slots = reinterpret_cast<uint16_t*>(index);
            while (slots[i] != freeShortSlot)
                i = (i + 1) & mask;
            slots[i] = shortTagOf(hash) << CompactTable::shortSlotEntryBits | entry;
            continue;
        }
        uint32_t* slots = reinterpret_cast<uint32_t*>(index);
        while (slots[i] != freeLongSlot)
            i = (i + 1) & mask;
        slots[i] = longTagOf(hash) << CompactTable::longSlotEntryBits | entry;
    }
    return bytes;
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
    case Width::InImage:
        RELEASE_ASSERT_NOT_REACHED();
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
        constexpr uint64_t ones = 0x0101010101010101ull;
        uint64_t difference = *at<uint64_t>(offsetOfIndex()) ^ shortTagOf(hash) * ones;
        for (uint64_t flags = (difference - ones) & ~difference & ones << 7; flags; flags &= flags - 1) {
            uint32_t entry = WTF::ctz(flags) / 8;
            if (entry >= m_used)
                break;
            if (matches(entry))
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
            if (slot >> shortSlotEntryBits == shortTagOf(hash) && matches(slot & ((1 << shortSlotEntryBits) - 1)))
                return slot & ((1 << shortSlotEntryBits) - 1);
        }
    }
    const uint32_t* slots = at<uint32_t>(offsetOfIndex());
    for (uint32_t i = hash & mask;; i = (i + 1) & mask) {
        uint32_t slot = slots[i];
        if (slot == freeLongSlot)
            return std::nullopt;
        if (slot >> longSlotEntryBits == longTagOf(hash) && matches(slot & ((1 << longSlotEntryBits) - 1)))
            return slot & ((1 << longSlotEntryBits) - 1);
    }
}

template<typename CharacterType>
ALWAYS_INLINE std::optional<uint32_t> CompactTable::findCharacters(std::span<const CharacterType> characters) const
{
    unsigned hash = RapidHash::computeHashAndMaskTop8Bits<CharacterType>(characters);
    return findWithHash(hash, [&](uint32_t entry) ALWAYS_INLINE_LAMBDA {
        JSString* candidate = keyAt(entry);
        if (!candidate)
            return false;
        const StringImpl& impl = *candidate->tryGetValueImpl();
        if (impl.existingHash() != hash)
            return false;
        return impl.is8Bit() == (sizeof(CharacterType) == 1) ? hasCharacters(impl, characters) : StringView { impl } == StringView { characters };
    });
}

std::optional<uint32_t> CompactTable::findInImage(JSGlobalObject* globalObject, JSString* key) const
{
    const TableInImage& entries = tableInImage();
    if (const StringImpl* impl = key->tryGetValueImpl()) [[likely]]
        return entries.find(*impl);
    auto findCharacters = [&]<typename CharacterType>(std::span<const CharacterType> characters) {
        return entries.findWithHash(RapidHash::computeHashAndMaskTop8Bits<CharacterType>(characters), [&](uint32_t entry) {
            return StringView { entries.textAt(entry) } == StringView { characters };
        });
    };
    if (key->isSubstring()) {
        auto characters = key->view(globalObject);
        return characters->is8Bit() ? findCharacters(characters->span8()) : findCharacters(characters->span16());
    }
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto string = key->value(globalObject);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    return entries.find(*string->impl());
}

std::optional<uint32_t> CompactTable::find(JSGlobalObject* globalObject, JSValue value) const
{
    if (!value.isString())
        return std::nullopt;
    JSString* key = asString(value);
    if (isInImage())
        return findInImage(globalObject, key);
    if (key->isSubstring()) {
        auto characters = key->view(globalObject);
        auto entry = characters->is8Bit() ? findCharacters(characters->span8()) : findCharacters(characters->span16());
        if (entry) {
            StringImpl& impl = *const_cast<StringImpl*>(keyAt(*entry)->tryGetValueImpl());
            if (impl.is8Bit() || !key->is8Bit())
                uncheckedDowncast<JSRopeString>(key)->resolveSubstringBySharing(impl);
        }
        return entry;
    }

    uint32_t found;
    switch (tryFindQuickly(value, found)) {
    case Quickly::Found:
        return found;
    case Quickly::Absent:
        return std::nullopt;
    case Quickly::Unknown:
        break;
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
    if (isInImage()) {
        const TableInImage& entries = tableInImage();
        return entries.findWithHash(RapidHash::computeHashAndMaskTop8Bits<Latin1Character>(characters), [&](uint32_t entry) {
            return StringView { entries.textAt(entry) } == StringView { characters };
        });
    }
    return findCharacters(characters);
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
        slots[i] = shortTagOf(hash) << shortSlotEntryBits | entry;
        return;
    }
    uint32_t* slots = at<uint32_t>(offsetOfIndex());
    while (slots[i] != freeLongSlot)
        i = (i + 1) & mask;
    slots[i] = longTagOf(hash) << longSlotEntryBits | entry;
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
            slots[i] = shortTagOf(hash) << shortSlotEntryBits | entry;
        });
        return;
    }
    uint32_t* slots = at<uint32_t>(offsetOfIndex());
    forEachHash([&](uint32_t entry, unsigned hash) {
        uint32_t i = hash & mask;
        while (slots[i] != freeLongSlot)
            i = (i + 1) & mask;
        slots[i] = longTagOf(hash) << longSlotEntryBits | entry;
    });
}

void CompactTable::append(VM& vm, JSString* key, unsigned hash, JSValue value)
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

CompactTable* CompactTable::copyWithCapacity(VM& vm, Width value) const
{
    uint64_t alive = size();
    if (alive >= maxCapacity)
        return nullptr;
    uint64_t minCapacity = std::min<uint64_t>(deleted() >= m_used / 2 ? alive + 1 : alive < 64 ? alive + alive / 2 + 1 : alive * 2, maxCapacity);
    CompactTable* copy = create(vm, structure(), keyWidth(), value, minCapacity);
    size_t keyWidth = static_cast<size_t>(this->keyWidth());
    size_t valueWidth = static_cast<size_t>(m_valueWidth);
    const uint8_t* keys = at<uint8_t>(offsetOfKeys());
    const uint8_t* values = at<uint8_t>(offsetOfValues());
    uint8_t* copyKeys = copy->at<uint8_t>(copy->offsetOfKeys());
    uint8_t* copyValues = copy->at<uint8_t>(copy->offsetOfValues());
    uint32_t count = 0;
    if (!deleted() && value == m_valueWidth) {
        count = m_used;
        memcpy(copyKeys, keys, count * keyWidth);
        memcpy(copyValues, values, count * valueWidth);
    } else {
        for (uint32_t entry = 0; entry < m_used; ++entry) {
            if (!keyAt(entry))
                continue;
            memcpy(copyKeys + count * keyWidth, keys + entry * keyWidth, keyWidth);
            if (value == m_valueWidth)
                memcpy(copyValues + count * valueWidth, values + entry * valueWidth, valueWidth);
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
    if (isInImage())
        return createInImage(vm, structure(), tableInImage(), m_valueWidth == Width::InImage ? *at<Typed::State*>(offsetOfIndex() + sizeof(void*)) : nullptr);
    size_t used = allocationSize(m_capacity, keyWidth(), m_valueWidth);
    size_t size = MarkedSpace::optimalSizeFor(used);
    CompactTable* copy = new (NotNull, allocateCell<CompactTable>(vm, size)) CompactTable(vm, structure());
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

void CompactTable::becomeObsolete(VM& vm, State state, JSCell* next)
{
    RELEASE_ASSERT(isLive() && state > State::Iterated);
    {
        Locker locker { cellLock() };
        uint32_t count = 0;
        if (state == State::Replaced && deleted()) {
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

static Structure* structureFor(JSGlobalObject* globalObject, Typed::State* state)
{
    Structure* structure = state->compactTableStructure();
    if (!globalObject->aotInstance()->compactTableStructureID) {
        for (Instance* instance : globalObject->vm().m_aotInstances) {
            if (instance->globalObject == globalObject)
                instance->compactTableStructureID = structure->id().bits();
        }
    }
    return structure;
}

JSSet* CompactTable::createSetInImage(JSGlobalObject* globalObject, const TableInImage& entries)
{
    VM& vm = globalObject->vm();
    Typed::State* state = stateOf(globalObject);
    if (state->isCounting()) [[unlikely]]
        state->countCompactTable("made in the image"_s, entries.count);
    if (Options::useAOTOperationCounters()) [[unlikely]] {
        runtimeTable(vm).countOperation("CompactTable in the image", "made");
        runtimeTable(vm).countOperation("CompactTable in the image", "entries of those made", entries.count);
    }
    JSSet* set = JSSet::create(vm, globalObject->setStructure());
    set->m_compactTable.set(vm, set, createInImage(vm, structureFor(globalObject, state), entries, state));
    return set;
}

JSMap* CompactTable::createMapInImage(JSGlobalObject* globalObject, const TableInImage& entries)
{
    VM& vm = globalObject->vm();
    Typed::State* state = stateOf(globalObject);
    if (state->isCounting()) [[unlikely]]
        state->countCompactTable("made in the image"_s, entries.count);
    if (Options::useAOTOperationCounters()) [[unlikely]] {
        runtimeTable(vm).countOperation("CompactTable in the image", "made");
        runtimeTable(vm).countOperation("CompactTable in the image", "entries of those made", entries.count);
    }
    JSMap* map = JSMap::create(vm, globalObject->mapStructure());
    map->m_compactTable.set(vm, map, createInImage(vm, structureFor(globalObject, state), entries, state));
    return map;
}

template<typename Traits>
CompactTable* CompactTable::moveToHeap(VM& vm, JSOrderedHashTable<Traits>* owner)
{
    auto* table = uncheckedDowncast<CompactTable>(owner->m_compactTable.get());
    RELEASE_ASSERT(table->isInImage() && table->isLive());
    const TableInImage& entries = table->tableInImage();
    Typed::State* state = stateOf(owner->realm());
    RELEASE_ASSERT(state && !state->data.usesWideReferences);
    if (state->isCounting()) [[unlikely]]
        state->countCompactTable("moved from the image to the heap"_s, entries.count);
    if (Options::useAOTOperationCounters()) [[unlikely]] {
        runtimeTable(vm).countOperation("CompactTable in the image", "moved to the heap");
        runtimeTable(vm).countOperation("CompactTable in the image", "entries of those moved to the heap", entries.count);
    }
    DeferGCForAWhile deferGC(vm);
    Width valueWidth = Width::None;
    if constexpr (Traits::hasValueData) {
        valueWidth = Width::Narrow;
        for (uint32_t entry = 0; entry < entries.count; ++entry) {
            if (!canHold(Width::Narrow, table->valueInImage(entry)))
                valueWidth = Width::Wide;
        }
    }
    CompactTable* copy = create(vm, table->structure(), Width::Narrow, valueWidth, entries.count);
    for (uint32_t entry = 0; entry < entries.count; ++entry) {
        auto text = entries.textAt(entry);
        JSString* key = text.empty() ? vm.smallStrings.emptyString() : text.size() == 1 ? vm.smallStrings.singleCharacterString(text[0]) : state->string(vm, entries.strings()[entry]);
        RELEASE_ASSERT(canHold(Width::Narrow, key));
        copy->append(vm, key, hashOf(key), Traits::hasValueData ? table->valueInImage(entry) : JSValue());
    }
    copy->m_state = table->m_state;
    table->becomeObsolete(vm, State::Replaced, copy);
    owner->m_compactTable.set(vm, owner, copy);
    return copy;
}

template<typename Traits>
bool CompactTable::tryAdd(JSGlobalObject* globalObject, JSOrderedHashTable<Traits>* owner, JSValue key, JSValue value, CreateIfNeeded createIfNeeded, uint32_t minCapacity)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    ASSERT(!owner->m_storage);
    if (!key.isString())
        return false;
    JSString* string = asString(key);
    Typed::State* state = stateOf(globalObject);
    auto* table = owner->m_compactTable ? uncheckedDowncast<CompactTable>(owner->m_compactTable.get()) : nullptr;
    if (table && table->isInImage()) {
        if constexpr (!Traits::hasValueData) {
            auto entry = table->findInImage(globalObject, string);
            RETURN_IF_EXCEPTION(scope, false);
            if (entry)
                return true;
        }
        table = moveToHeap(vm, owner);
    }
    auto replaceBy = [&](CompactTable* replacement, ASCIILiteral event) {
        if (state && state->isCounting()) [[unlikely]]
            state->countCompactTable(event, table->size());
        replacement->m_state = table->m_state;
        if (table->m_state != State::Live)
            table->becomeObsolete(vm, State::Replaced, replacement);
        owner->m_compactTable.set(vm, owner, replacement);
        table = replacement;
    };

    uint32_t found;
    if (uint32_t count = table ? table->m_used : 0; count && count == table->m_capacity && count == table->m_alive && count < maxKnownGrowthEntries
        && table->tryFindQuickly(key, found) == Quickly::Absent && canHold(table->m_valueWidth, value) && canHold(Width::Narrow, string) && !(state && state->isCounting())) {
        Width valueWidth = table->m_valueWidth;
        if (uint64_t known = s_knownGrowths[static_cast<unsigned>(valueWidth) / 4][count / 2].load(std::memory_order_relaxed)) [[likely]] {
            uint32_t capacity = static_cast<uint32_t>(known);
            unsigned indexBits = known >> 32 & 0xff;
            size_t keys = offsetOfIndex() + (indexBits ? sizeof(uint16_t) << indexBits : maxCapacityWithTags);
            size_t size = MarkedSpace::optimalSizeFor(keys + capacity * (static_cast<size_t>(Width::Narrow) + static_cast<size_t>(valueWidth)));
            CompactTable* larger = new (NotNull, allocateCell<CompactTable>(vm, size)) CompactTable(vm, table->structure());
            larger->m_capacity = capacity;
            larger->m_indexBits = indexBits;
            larger->m_keysShiftAndFlags = indexBits ? indexBits : keysShiftWithTags;
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
            unsigned hash = string->tryGetValueImpl()->existingHash();
            const uint64_t* from = table->template at<uint64_t>(table->offsetOfKeys());
            uint64_t* to = larger->at<uint64_t>(offsetOfIndex());
            if (indexBits)
                fillWords(to, std::numeric_limits<uint64_t>::max(), (keys - offsetOfIndex()) / sizeof(uint64_t));
            else
                *to++ = *table->template at<uint64_t>(offsetOfIndex()) | static_cast<uint64_t>(shortTagOf(hash)) << (count * 8);
            uint32_t* largerKeys = reinterpret_cast<uint32_t*>(to);
            copyWords(to, from, count / 2);
            fillWords(to, 0, (capacity - count) / 2);
            uint64_t* largerValues = to;
            copyWords(to, from + count / 2, count * static_cast<size_t>(valueWidth) / sizeof(uint64_t));
            fillWords(to, 0, larger->at<uint64_t>(size) - to);
            larger->finishCreation(vm);
            largerKeys[count] = static_cast<uint32_t>(std::bit_cast<uintptr_t>(string) >> MarkedBlockCage::shift);
            if (valueWidth == Width::Wide)
                largerValues[count] = JSValue::encode(value);
            else if (valueWidth == Width::Narrow)
                reinterpret_cast<uint32_t*>(largerValues)[count] = value.isCell() ? static_cast<uint32_t>(std::bit_cast<uintptr_t>(value.asCell()) >> MarkedBlockCage::shift) : static_cast<uint32_t>(JSValue::encode(value));
            if (indexBits) {
                uint32_t mask = (1u << indexBits) - 1;
                uint16_t* slots = larger->at<uint16_t>(offsetOfIndex());
                for (uint32_t entry = 0; entry <= count; ++entry) {
                    unsigned entryHash = entry == count ? hash : uncheckedDowncast<JSString>(MarkedBlockCage::decode(largerKeys[entry]))->tryGetValueImpl()->existingHash();
                    uint32_t i = entryHash & mask;
                    while (slots[i] != freeShortSlot)
                        i = (i + 1) & mask;
                    slots[i] = shortTagOf(entryHash) << shortSlotEntryBits | entry;
                }
            }
            WTF::storeStoreFence();
            larger->m_used = count + 1;
            larger->m_alive = count + 1;
            vm.writeBarrier(larger);
            if (Options::validateAOTTypedFacts()) [[unlikely]] {
                CompactTable* fallbackCopy = table->copyWithCapacity(vm, valueWidth);
                RELEASE_ASSERT(fallbackCopy);
                fallbackCopy->append(vm, string, hash, value);
                RELEASE_ASSERT(!memcmp(fallbackCopy->at<uint8_t>(sizeof(JSCell)), larger->at<uint8_t>(sizeof(JSCell)), size - sizeof(JSCell)));
            }
            larger->m_state = table->m_state;
            if (table->m_state != State::Live)
                table->becomeObsolete(vm, State::Replaced, larger);
            owner->m_compactTable.set(vm, owner, larger);
            return true;
        }
    }

    if (table) {
        auto entry = table->find(globalObject, string);
        RETURN_IF_EXCEPTION(scope, false);
        if (entry) {
            if (!canHold(table->m_valueWidth, value)) [[unlikely]] {
                uint32_t before = 0;
                for (uint32_t i = 0; i < *entry; ++i)
                    before += !table->keyAt(i);
                CompactTable* wider = table->copyWithCapacity(vm, Width::Wide);
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
        Width keyWidth = isNarrow ? Width::Narrow : Width::Wide;
        Width valueWidth = !Traits::hasValueData ? Width::None : isNarrow && canHold(Width::Narrow, value) ? Width::Narrow : Width::Wide;
        table = create(vm, structureFor(globalObject, state), keyWidth, valueWidth, minCapacity);
        owner->m_compactTable.set(vm, owner, table);
        if (state->isCounting()) [[unlikely]]
            state->countCompactTable(makeString("made key "_s, static_cast<unsigned>(keyWidth), " value "_s, static_cast<unsigned>(valueWidth)), 0);
    }
    if (!canHold(table->keyWidth(), string)) [[unlikely]]
        return false;
    bool canStoreValue = canHold(table->m_valueWidth, value);
    std::atomic<uint64_t>* knownGrowth = nullptr;
    if (uint32_t count = table->m_used; count == table->m_capacity && canStoreValue && count == table->m_alive && count < maxKnownGrowthEntries && table->keyWidth() == Width::Narrow)
        knownGrowth = &s_knownGrowths[static_cast<unsigned>(table->m_valueWidth) / 4][count / 2];
    if (table->m_used == table->m_capacity || !canStoreValue) {
        CompactTable* larger = table->copyWithCapacity(vm, canStoreValue ? table->m_valueWidth : Width::Wide);
        if (!larger)
            return false;
        if (knownGrowth && larger->m_capacity <= maxCapacityWithShortSlots)
            knownGrowth->store(larger->m_capacity | static_cast<uint64_t>(larger->m_indexBits) << 32 | static_cast<uint64_t>(larger->m_keysShiftAndFlags) << 40 | static_cast<uint64_t>(larger->m_valueWidth) << 48
                | static_cast<uint64_t>(MarkedSpace::sizeClassToIndex(MarkedSpace::optimalSizeFor(allocationSize(larger->m_capacity, Width::Narrow, larger->m_valueWidth)))) << knownGrowthSizeStepShift, std::memory_order_relaxed);
        replaceBy(larger, canStoreValue ? "grown"_s : "widened"_s);
    }
    table->append(vm, string, hashOf(string), value);
    return true;
}

static ALWAYS_INLINE uint32_t narrowed(const JSString* key)
{
    return static_cast<uint32_t>(std::bit_cast<uintptr_t>(key) >> MarkedBlockCage::shift);
}

static ALWAYS_INLINE bool holdsSameText(uint32_t narrowKey, const StringImpl* impl, unsigned hash)
{
    const StringImpl* other = uncheckedDowncast<JSString>(MarkedBlockCage::decode(narrowKey))->tryGetValueImpl();
    return other == impl || (other->existingHash() == hash && equal(other, impl));
}

static ALWAYS_INLINE bool isResolvedString(JSValue value)
{
    return value && value.isString() && !asString(value)->isRope();
}

static constexpr uint64_t onesInBytes = 0x0101010101010101ull;

static NEVER_INLINE bool gatherDistinctStrings(std::span<const WriteBarrier<Unknown>> strings, uint32_t* keys, uint64_t& tags, uint32_t& used)
{
    for (auto& string : strings) {
        JSValue value = string.get();
        if (!isResolvedString(value))
            return false;
        const StringImpl* impl = asString(value)->tryGetValueImpl();
        unsigned hash = impl->hash();
        uint64_t tag = hash >> CompactTable::shortTagShift;
        uint64_t difference = tags ^ tag * onesInBytes;
        bool isPresent = false;
        for (uint64_t candidates = (difference - onesInBytes) & ~difference & onesInBytes << 7 & ((1ull << (used * 8)) - 1); candidates && !isPresent; candidates &= candidates - 1)
            isPresent = holdsSameText(keys[WTF::ctz(candidates) / 8], impl, hash);
        if (isPresent)
            continue;
        keys[used] = narrowed(asString(value));
        tags |= tag << (used * 8);
        ++used;
    }
    return true;
}

template<typename Slot>
uint32_t CompactTable::fillWithHashedStrings(std::span<const WriteBarrier<Unknown>> strings)
{
    constexpr bool isShort = sizeof(Slot) == sizeof(uint16_t);
    constexpr unsigned entryBits = isShort ? shortSlotEntryBits : longSlotEntryBits;
    constexpr unsigned tagShift = isShort ? shortTagShift : longTagShift;
    constexpr Slot freeSlot = isShort ? freeShortSlot : freeLongSlot;
    Slot* slots = at<Slot>(offsetOfIndex());
    uint32_t* keys = at<uint32_t>(offsetOfKeys());
    uint32_t mask = (1u << m_indexBits) - 1;
    uint32_t used = 0;
    for (auto& string : strings) {
        JSString* key = asString(string.get());
        const StringImpl* impl = key->tryGetValueImpl();
        unsigned hash = impl->existingHash();
        uint32_t tag = hash >> tagShift << entryBits;
        for (uint32_t i = hash & mask;; i = (i + 1) & mask) {
            uint32_t slot = slots[i];
            if (slot == freeSlot) {
                slots[i] = tag | used;
                keys[used++] = narrowed(key);
                break;
            }
            if (uint32_t entry = slot ^ tag; !(entry >> entryBits) && holdsSameText(keys[entry], impl, hash))
                break;
        }
    }
    return used;
}

OperationReturnType<JSCell*> JIT_OPERATION_ATTRIBUTES CompactTable::newSetOfStringsQuickly(Instance* instance, EncodedJSValue encodedList)
{
    JSValue list = JSValue::decode(encodedList);
    if (!list.isCell() || list.asCell()->type() != ArrayType || (list.asCell()->indexingType() & IndexingShapeMask) != ContiguousShape)
        [[clang::musttail]] return newSetOfStringsGeneric(instance, encodedList);
    Typed::State& state = *instance->typed;
    if (!instance->compactTableStructureID || state.data.usesWideReferences || state.isCounting()) [[unlikely]]
        [[clang::musttail]] return newSetOfStringsGeneric(instance, encodedList);
    VM& vm = *instance->vm;
    const Butterfly* butterfly = uncheckedDowncast<JSArray>(list.asCell())->butterfly();
    const WriteBarrier<Unknown>* strings = butterfly->contiguous().data();
    size_t length = butterfly->publicLength();

    CompactTable* table;
    if (length <= maxCapacityWithTags) {
        if (!length)
            [[clang::musttail]] return newSetOfStringsGeneric(instance, encodedList);
        std::array<uint32_t, maxCapacityWithTags> storage { };
        uint32_t* keys = storage.data();
        uint64_t tags = 0;
        uint64_t flagsOfUsed = 0;
        size_t i = 0;
        for (; i < length; ++i) {
            JSValue value = strings[i].get();
            if (!isResolvedString(value))
                [[clang::musttail]] return newSetOfStringsGeneric(instance, encodedList);
            const StringImpl* impl = asString(value)->tryGetValueImpl();
            if (!impl->hasHash()) [[unlikely]]
                break;
            uint64_t tag = impl->existingHash() >> shortTagShift;
            uint64_t difference = tags ^ tag * onesInBytes;
            if ((difference - onesInBytes) & ~difference & flagsOfUsed) [[unlikely]]
                break;
            keys[i] = narrowed(asString(value));
            tags |= tag << (i * 8);
            flagsOfUsed = flagsOfUsed << 8 | 0x80;
        }
        uint32_t used = i;
        if (i < length && !gatherDistinctStrings({ strings + i, length - i }, keys, tags, used)) [[unlikely]]
            [[clang::musttail]] return newSetOfStringsGeneric(instance, encodedList);
        static_assert(initialCapacity * 2 == maxCapacityWithTags && sizeof(storage) == 4 * sizeof(uint64_t) && !(initialSize(Width::None) % MarkedSpace::sizeStep) && !(initialCapacity * sizeof(uint32_t) % MarkedSpace::sizeStep));
        bool isInitialSize = used <= initialCapacity;
        size_t size = isInitialSize ? initialSize(Width::None) : initialSize(Width::None) + initialCapacity * sizeof(uint32_t);
        table = new (NotNull, tryAllocateCellHelper<CompactTable, AllocationFailureMode::Assert>(vm, size, nullptr)) CompactTable(vm, std::bit_cast<StructureID>(instance->compactTableStructureID).decode());
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
        if (length > maxCapacity)
            [[clang::musttail]] return newSetOfStringsGeneric(instance, encodedList);
        for (size_t i = 0; i < length; ++i) {
            JSValue value = strings[i].get();
            if (!isResolvedString(value))
                [[clang::musttail]] return newSetOfStringsGeneric(instance, encodedList);
            asString(value)->tryGetValueImpl()->hash();
        }
        table = create(vm, std::bit_cast<StructureID>(instance->compactTableStructureID).decode(), Width::Narrow, Width::None, length);
        uint32_t used = table->hasShortSlots() ? table->fillWithHashedStrings<uint16_t>({ strings, length }) : table->fillWithHashedStrings<uint32_t>({ strings, length });
        WTF::storeStoreFence();
        table->m_used = used;
        table->m_alive = used;
    }
    JSSet* set = JSSet::create(vm, instance->globalObject->setStructure());
    set->m_compactTable.setWithoutWriteBarrier(table);
    vm.mutatorFence();
    ensureStillAliveHere(list);
    return { set, nullptr };
}

template<typename Traits>
bool CompactTable::tryAddAbsent(JSGlobalObject* globalObject, JSOrderedHashTable<Traits>* owner, JSString* key, JSValue value)
{
    auto* table = uncheckedDowncast<CompactTable>(owner->m_compactTable.get());
    const StringImpl* impl = key->tryGetValueImpl();
    if (impl && !table->isInImage() && table->m_used < table->m_capacity && canHold(table->m_valueWidth, value) && canHold(table->keyWidth(), key)) [[likely]] {
        table->append(globalObject->vm(), key, impl->hash(), value);
        return true;
    }
    return tryAdd(globalObject, owner, key, value);
}

template<typename Traits>
bool CompactTable::remove(JSGlobalObject* globalObject, JSOrderedHashTable<Traits>* owner, JSValue key)
{
    auto* table = uncheckedDowncast<CompactTable>(owner->m_compactTable.get());
    auto entry = table->find(globalObject, key);
    if (!entry)
        return false;
    if (table->isInImage())
        table = moveToHeap(globalObject->vm(), owner);
    table->setKey(*entry, nullptr);
    table->setValue(*entry, JSValue());
    --table->m_alive;
    return true;
}

template<typename Traits>
void CompactTable::clear(VM& vm, JSOrderedHashTable<Traits>* owner)
{
    auto* table = uncheckedDowncast<CompactTable>(owner->m_compactTable.get());
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
    auto* table = heapTableOf(owner);
    RELEASE_ASSERT(!owner->m_storage);
    if (Typed::State* state = stateOf(globalObject); state && state->isCounting()) [[unlikely]] {
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
            if (compact->isInImage()) {
                table = moveToHeap(vm, owner);
                break;
            }
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

JSCell* CompactTable::copyPosition(VM& vm, JSCell* position)
{
    auto* record = uncheckedDowncast<JSCellButterfly>(position);
    JSCellButterfly* copy = JSCellButterfly::create(vm, CopyOnWriteArrayWithContiguous, 2);
    copy->setIndex(vm, 0, record->get(0));
    copy->setIndex(vm, 1, record->get(1));
    return copy;
}

static constexpr int32_t iterationKindMask = CollectionIteration::iterationKindMask;
static constexpr int32_t iteratesSet = CollectionIteration::iteratesSet;
static constexpr int32_t iteratesThroughIterator = CollectionIteration::iteratesThroughIterator;
static constexpr int32_t iteratesGenerator = CollectionIteration::iteratesGenerator;
static constexpr int32_t iteratesArray = CollectionIteration::iteratesArray;
static constexpr int32_t iteratesString = 64;
static constexpr int32_t iteratesMatches = 128;
static constexpr int32_t iteratesTypedArray = 512;
static_assert(CollectionIteration::mayBeSteppedQuickly == 1024 && CollectionIteration::hasFastStepAfterStart == 2048 && CollectionIteration::iteratesArrayEntriesByIndex == 256);

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
    JSValue iterated = iterator->internalField(JSArrayIterator::Field::IteratedObject).get();
    return iterated.isCell() && iterated.asCell()->classInfo() == JSArray::info();
}

template<typename Functor>
static ALWAYS_INLINE auto tryOpenWith(JSGlobalObject* globalObject, JSValue iterable, const Functor& makeRecord) -> decltype(makeRecord(nullptr, 0))
{
    VM& vm = globalObject->vm();
    if (!iterable.isCell())
        return nullptr;
    JSCell* cell = iterable.asCell();
    int32_t mode;
    switch (cell->type()) {
    case StringType:
        if (!globalObject->isStringPrototypeIteratorProtocolFastAndNonObservable())
            return nullptr;
        mode = iteratesString;
        break;
    case JSMapType:
        if (uncheckedDowncast<JSMap>(cell)->realm() != globalObject || !uncheckedDowncast<JSMap>(cell)->isIteratorProtocolFastAndNonObservable())
            return nullptr;
        mode = CollectionIteration::modeOfMap;
        break;
    case JSSetType:
        if (uncheckedDowncast<JSSet>(cell)->realm() != globalObject || !uncheckedDowncast<JSSet>(cell)->isIteratorProtocolFastAndNonObservable())
            return nullptr;
        mode = CollectionIteration::modeOfSet;
        break;
    case JSMapIteratorType:
        if (uncheckedDowncast<JSMapIterator>(cell)->realm() != globalObject || !mapIteratorProtocolIsFastAndNonObservable(vm, uncheckedDowncast<JSMapIterator>(cell)))
            return nullptr;
        mode = iteratesThroughIterator | (uncheckedDowncast<JSMapIterator>(cell)->kind() != IterationKind::Entries ? CollectionIteration::hasFastStepAfterStart : 0);
        break;
    case JSSetIteratorType:
        if (uncheckedDowncast<JSSetIterator>(cell)->realm() != globalObject || !setIteratorProtocolIsFastAndNonObservable(vm, uncheckedDowncast<JSSetIterator>(cell)))
            return nullptr;
        mode = iteratesThroughIterator | iteratesSet | (uncheckedDowncast<JSSetIterator>(cell)->kind() != IterationKind::Entries ? CollectionIteration::hasFastStepAfterStart : 0);
        break;
    case JSArrayIteratorType:
        if (uncheckedDowncast<JSArrayIterator>(cell)->realm() != globalObject || !arrayIteratorProtocolIsFastAndNonObservable(vm, uncheckedDowncast<JSArrayIterator>(cell)))
            return nullptr;
        mode = iteratesThroughIterator | iteratesArray;
        break;
    case JSRegExpStringIteratorType:
        if (cell->structure() != globalObject->regExpStringIteratorStructure())
            return nullptr;
        mode = iteratesThroughIterator | iteratesMatches;
        break;
    case ObjectType: {
        auto* generator = dynamicDowncast<Generator>(cell);
        if (!generator || generator->realm() != globalObject || !generator->hasUnmodifiedIteratorProtocol(globalObject))
            return nullptr;
        mode = iteratesGenerator;
        break;
    }
    default: {
        if (!isTypedArrayType(cell->type()))
            return nullptr;
        auto* view = uncheckedDowncast<JSArrayBufferView>(cell);
        if (view->realm() != globalObject || !view->isIteratorProtocolFastAndNonObservable() || view->isOutOfBounds())
            return nullptr;
        mode = iteratesMatches | iteratesTypedArray;
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

bool CollectionIteration::tryOpen(JSGlobalObject* globalObject, JSValue iterable, StackRecord* record)
{
    return tryOpenWith(globalObject, iterable, [&](JSCell* cell, int32_t mode) {
        return fill(globalObject->vm(), record, cell, mode);
    });
}

static ALWAYS_INLINE void storeRecordWord(VM& vm, JSCellButterfly* record, unsigned word, JSValue value)
{
    ASSERT(hasContiguous(record->indexingType()));
    record->toButterfly()->contiguous().atUnsafe(word).set(vm, record, value);
}

static ALWAYS_INLINE void storeRecordWord(VM& vm, CollectionIteration::StackRecord* record, unsigned word, JSValue value)
{
    record->setIndex(vm, word, value);
}

template<typename Record>
static ALWAYS_INLINE bool stepThrough(JSGlobalObject* globalObject, Record* record)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    int32_t mode = record->get(1).asInt32();
    RELEASE_ASSERT(!(mode & CollectionIteration::iteratesArrayEntriesByIndex));
    JSCell* cell = record->get(0).asCell();
    JSValue value;
    if (mode & iteratesGenerator) {
        bool isDone;
        value = uncheckedDowncast<Generator>(cell)->resume(globalObject, Generator::ResumeMode::Next, jsUndefined(), isDone);
        RETURN_IF_EXCEPTION(scope, false);
        if (isDone)
            return false;
    } else if (mode & iteratesString) {
        auto [next, position] = JSStringIterator::advance(globalObject, vm, uncheckedDowncast<JSString>(cell), record->get(4).asInt32());
        RETURN_IF_EXCEPTION(scope, false);
        if (!next)
            return false;
        storeRecordWord(vm, record, 4, jsNumber(position));
        value = next;
    } else if (mode & iteratesMatches && mode & iteratesTypedArray) {
        auto* view = uncheckedDowncast<JSArrayBufferView>(cell);
        validateTypedArray(globalObject, view);
        RETURN_IF_EXCEPTION(scope, false);
        size_t index = static_cast<size_t>(record->get(4).asNumber());
        if (index >= view->length())
            return false;
        storeRecordWord(vm, record, 4, jsNumber(static_cast<double>(index + 1)));
        if (index <= std::numeric_limits<unsigned>::max() && view->canGetIndexQuicklyForTypedArray(static_cast<unsigned>(index)))
            value = view->getIndexQuicklyForTypedArray(static_cast<unsigned>(index));
        else {
            value = view->get(globalObject, static_cast<uint64_t>(index));
            RETURN_IF_EXCEPTION(scope, false);
        }
    } else if (mode & iteratesMatches) {
        auto* iterator = uncheckedDowncast<JSRegExpStringIterator>(cell);
        if (iterator->isDone())
            return false;
        value = iterator->nextImpl(globalObject);
        RETURN_IF_EXCEPTION(scope, false);
        if (value.isNull())
            return false;
    } else if (mode & iteratesThroughIterator) {
        bool hasNext = mode & iteratesArray ? uncheckedDowncast<JSArrayIterator>(cell)->next(globalObject, value)
            : mode & iteratesSet ? uncheckedDowncast<JSSetIterator>(cell)->next(globalObject, value)
            : uncheckedDowncast<JSMapIterator>(cell)->next(globalObject, value);
        RETURN_IF_EXCEPTION(scope, false);
        if (!hasNext)
            return false;
        if (mode & CollectionIteration::hasFastStepAfterStart)
            storeRecordWord(vm, record, 1, jsNumber(mode ^ (CollectionIteration::hasFastStepAfterStart | CollectionIteration::mayBeSteppedQuickly)));
    } else if (mode & iteratesArray) {
        auto* array = uncheckedDowncast<JSArray>(cell);
        uint32_t index = static_cast<uint32_t>(record->get(4).asNumber());
        if (index >= array->length())
            return false;
        storeRecordWord(vm, record, 4, jsNumber(index + 1));
        value = jsNumber(index);
        if (static_cast<IterationKind>(mode & iterationKindMask) == IterationKind::Entries) {
            JSValue element = array->getIndex(globalObject, index);
            RETURN_IF_EXCEPTION(scope, false);
            if constexpr (std::is_same_v<Record, CollectionIteration::StackRecord>) {
                if (mode & CollectionIteration::takesPairsApart) {
                    storeRecordWord(vm, record, CollectionIteration::firstPartWord, value);
                    storeRecordWord(vm, record, CollectionIteration::firstPartWord + 1, element);
                    return true;
                }
            }
            value = constructArrayPair(globalObject, value, element);
            RETURN_IF_EXCEPTION(scope, false);
        }
    } else {
        JSCell* table = record->get(3).isCell() ? record->get(3).asCell() : nullptr;
        uint32_t entry = record->get(4).asUInt32();
        CompactTable::Position position = mode & iteratesSet
            ? CompactTable::next<SetTraits>(vm, uncheckedDowncast<JSSet>(cell), table, entry)
            : CompactTable::next<MapTraits>(vm, uncheckedDowncast<JSMap>(cell), table, entry);
        if (!position.table)
            return false;
        if (auto* compact = dynamicDowncast<CompactTable>(position.table))
            compact->markAsIterated();
        storeRecordWord(vm, record, 3, position.table);
        storeRecordWord(vm, record, 4, jsNumber(position.entry + 1));
        IterationKind kind = static_cast<IterationKind>(mode & iterationKindMask);
        JSValue second = mode & iteratesSet ? position.key : position.value;
        value = kind == IterationKind::Values ? second : position.key;
        if (kind == IterationKind::Entries) {
            if constexpr (std::is_same_v<Record, CollectionIteration::StackRecord>) {
                if (mode & CollectionIteration::takesPairsApart) {
                    storeRecordWord(vm, record, CollectionIteration::firstPartWord, position.key);
                    storeRecordWord(vm, record, CollectionIteration::firstPartWord + 1, second);
                    return true;
                }
            }
            value = constructArrayPair(globalObject, position.key, second);
            RETURN_IF_EXCEPTION(scope, false);
        } else if (mode & CollectionIteration::hasFastStepAfterStart)
            storeRecordWord(vm, record, 1, jsNumber(mode ^ (CollectionIteration::hasFastStepAfterStart | CollectionIteration::mayBeSteppedQuickly)));
    }
    storeRecordWord(vm, record, 2, value);
    return true;
}

bool CollectionIteration::step(JSGlobalObject* globalObject, JSCellButterfly* record)
{
    return stepThrough(globalObject, record);
}

bool CollectionIteration::step(JSGlobalObject* globalObject, StackRecord* record)
{
    return stepThrough(globalObject, record);
}

static ALWAYS_INLINE uint32_t nextEntryWithKey(CompactTable* compact, uint32_t number, JSString*& key)
{
    for (uint32_t used = compact->used(); number < used && !(key = compact->keyAt(number)); ++number) { }
    return number;
}

using IterationStep = OperationReturnType<size_t>;

static NEVER_INLINE IterationStep JIT_OPERATION_ATTRIBUTES stepThroughSetQuickly(Instance* instance, EncodedJSValue* words)
{
    JSValue table = JSValue::decode(words[3]);
    if (!table.isCell())
        [[clang::musttail]] return stepIterationInFrameGeneric(instance, words);
    uint32_t entry = JSValue::decode(words[4]).asUInt32();
    if (table.asCell()->classInfo() != CompactTable::info()) {
        auto found = JSOrderedHashTableHelper<SetTraits>::transitAndNext(*instance->vm, *uncheckedDowncast<JSCellButterfly>(table.asCell()), entry);
        if (!found.storage)
            return { false, nullptr };
        words[2] = JSValue::encode(found.key);
        words[3] = JSValue::encode(found.storage);
        words[4] = JSValue::encode(jsNumber(found.entry + 1));
        return { true, nullptr };
    }
    auto* compact = uncheckedDowncast<CompactTable>(table.asCell());
    if (!compact->isLive() || compact->isInImage())
        [[clang::musttail]] return stepIterationInFrameGeneric(instance, words);
    JSString* key = nullptr;
    entry = nextEntryWithKey(compact, entry, key);
    if (!key)
        return { false, nullptr };
    compact->markAsIterated();
    words[2] = JSValue::encode(key);
    words[4] = JSValue::encode(jsNumber(entry + 1));
    return { true, nullptr };
}

template<typename Iterator, typename ValueOf>
static ALWAYS_INLINE unsigned stepThroughIteratorQuickly(VM& vm, EncodedJSValue* words, Iterator* iterator, bool returnsTableValue, const ValueOf& valueOf)
{
    using Field = typename Iterator::Field;
    if (iterator->tryGetStorage()) {
        JSValue value = valueOf(iterator->nextWithAdvance(vm));
        if (!value)
            return 0;
        words[2] = JSValue::encode(value);
        return 1;
    }
    JSValue at = iterator->internalField(Field::Entry).get();
    if (!at.isCell())
        return 2;
    auto* position = uncheckedDowncast<JSCellButterfly>(at.asCell());
    JSValue table = position->get(0);
    if (!table.isCell() || table.asCell()->classInfo() != CompactTable::info())
        return 2;
    auto* compact = uncheckedDowncast<CompactTable>(table.asCell());
    if (!compact->isLive() || compact->isInImage())
        return 2;
    JSString* key = nullptr;
    uint32_t entry = nextEntryWithKey(compact, position->get(1).asUInt32(), key);
    if (!key) {
        iterator->close(vm);
        return 0;
    }
    compact->markAsIterated();
    position->toButterfly()->contiguous().atUnsafe(1).setWithoutWriteBarrier(jsNumber(entry + 1));
    words[2] = JSValue::encode(returnsTableValue ? compact->valueAt(entry) : JSValue(key));
    return 1;
}

static NEVER_INLINE IterationStep JIT_OPERATION_ATTRIBUTES stepThroughSetIteratorQuickly(Instance* instance, EncodedJSValue* words)
{
    unsigned outcome = stepThroughIteratorQuickly(*instance->vm, words, uncheckedDowncast<JSSetIterator>(JSValue::decode(words[0]).asCell()), false, [](JSValue key) ALWAYS_INLINE_LAMBDA {
        return key;
    });
    if (outcome == 2)
        [[clang::musttail]] return stepIterationInFrameGeneric(instance, words);
    return { outcome, nullptr };
}

static NEVER_INLINE IterationStep JIT_OPERATION_ATTRIBUTES stepThroughMapIteratorQuickly(Instance* instance, EncodedJSValue* words)
{
    auto* iterator = uncheckedDowncast<JSMapIterator>(JSValue::decode(words[0]).asCell());
    bool returnsValue = iterator->kind() == IterationKind::Values;
    unsigned outcome = stepThroughIteratorQuickly(*instance->vm, words, iterator, returnsValue, [&](const JSMapIterator::NextResult& result) ALWAYS_INLINE_LAMBDA {
        return returnsValue && result.key ? result.value : result.key;
    });
    if (outcome == 2)
        [[clang::musttail]] return stepIterationInFrameGeneric(instance, words);
    return { outcome, nullptr };
}

IterationStep JIT_OPERATION_ATTRIBUTES CollectionIteration::stepQuickly(Instance* instance, EncodedJSValue* words)
{
    JSValue encodedMode = JSValue::decode(words[1]);
    if (!encodedMode.isInt32()) [[unlikely]]
        [[clang::musttail]] return stepIterationInFrameGeneric(instance, words);
    int32_t mode = encodedMode.asInt32();
    if (!(mode & iteratesThroughIterator))
        [[clang::musttail]] return stepThroughSetQuickly(instance, words);
    if (mode & iteratesSet)
        [[clang::musttail]] return stepThroughSetIteratorQuickly(instance, words);
    [[clang::musttail]] return stepThroughMapIteratorQuickly(instance, words);
}

template<typename Record>
static ALWAYS_INLINE void closeRecord(JSGlobalObject* globalObject, Record* record)
{
    int32_t mode = record->get(1).asInt32();
    if (mode & iteratesThroughIterator) {
        if (JSObject* iterator = asObject(record->get(0)); iterator->hasCustomProperties())
            iteratorClose(globalObject, iterator);
        return;
    }
    if (!(mode & iteratesGenerator))
        return;
    auto* generator = uncheckedDowncast<Generator>(record->get(0).asCell());
    if (!generator->hasUnmodifiedIteratorProtocol(globalObject)) {
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

void CollectionIteration::close(JSGlobalObject* globalObject, StackRecord* record)
{
    closeRecord(globalObject, record);
}

static WriteBarrier<Unknown>* tryGrow(JSGlobalObject* globalObject, JSArray* array, unsigned count)
{
    VM& vm = globalObject->vm();
    unsigned length = array->length();
    if (count >= MIN_SPARSE_ARRAY_INDEX - std::min(length, MIN_SPARSE_ARRAY_INDEX) || !array->tryMakeWritableContiguous(vm).data())
        return nullptr;
    if (!array->setLength(globalObject, length + count))
        return nullptr;
    RELEASE_ASSERT(hasContiguous(array->indexingType()));
    return array->butterfly()->contiguous().data() + length;
}

template<typename Traits>
static bool tryAppendEntries(JSGlobalObject* globalObject, JSArray* target, JSOrderedHashTable<Traits>* owner, IterationKind kind)
{
    using Helper = JSOrderedHashTableHelper<Traits>;
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* compactTable = owner->m_storage || !owner->m_compactTable ? nullptr : CompactTable::heapTableOf(owner);
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
    while (true) {
        bool hasNext = iterator->next(globalObject, value);
        RETURN_IF_EXCEPTION(scope, void());
        if (!hasNext)
            return;
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
    if (table->isInImage())
        return;
    uint32_t used = table->m_used;
    WTF::loadLoadFence();
    table->visitEntries(visitor, 0, used);
}

DEFINE_VISIT_CHILDREN(CompactTable);

#define AOT_TYPED_INSTANTIATE_COMPACT_TABLE(Traits) \
    template bool CompactTable::tryAdd<Traits>(JSGlobalObject*, JSOrderedHashTable<Traits>*, JSValue, JSValue, CreateIfNeeded, uint32_t); \
    template bool CompactTable::tryAddAbsent<Traits>(JSGlobalObject*, JSOrderedHashTable<Traits>*, JSString*, JSValue); \
    template bool CompactTable::remove<Traits>(JSGlobalObject*, JSOrderedHashTable<Traits>*, JSValue); \
    template void CompactTable::clear<Traits>(VM&, JSOrderedHashTable<Traits>*); \
    template void CompactTable::convert<Traits>(VM&, JSOrderedHashTable<Traits>*); \
    template CompactTable* CompactTable::moveToHeap<Traits>(VM&, JSOrderedHashTable<Traits>*); \
    template CompactTable::Position CompactTable::next<Traits>(VM&, JSOrderedHashTable<Traits>*, JSCell*, uint32_t);
AOT_TYPED_INSTANTIATE_COMPACT_TABLE(MapTraits)
AOT_TYPED_INSTANTIATE_COMPACT_TABLE(SetTraits)
#undef AOT_TYPED_INSTANTIATE_COMPACT_TABLE

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
