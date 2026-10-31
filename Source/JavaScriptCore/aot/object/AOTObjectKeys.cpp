/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTObject.h"

#if ENABLE(AOT)

#include "AOTClassPrototype.h"
#include "AOTHeapCacheRegistry.h"
#include "AOTInstance.h"
#include "AOTKeyOrderTable.h"
#include "AOTObjectInternal.h"
#include "AOTObjectModel.h"
#include "AOTObjectModelCaches.h"
#include "AOTPendingStars.h"
#include "AOTPropertyCaches.h"
#include "AOTState.h"
#include "AOTVector.h"
#include "DeferGC.h"
#include "JSArray.h"
#include "JSCInlines.h"
#include "JSCellButterfly.h"
#include "JSPropertyNameEnumerator.h"
#include "PropertyTable.h"
#include "StructureChain.h"
#include "VMInlines.h"
#include <algorithm>
#include <wtf/StdLibExtras.h>
#include <wtf/text/StringCommon.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

static bool isPresent(VM& vm, const Object& object, const FieldData& field)
{
    return object.isInSlot(field) || (object.hasExtraProperties() && livesInExtraProperties(vm, object, field));
}

static bool canPositionEveryKey(const LayoutData& layout, Structure* extraPropertiesStructure)
{
    return layout.numberOfFields + numberOfSlotsForMaxOffset(extraPropertiesStructure->maxOffset(), extraPropertiesStructure->inlineCapacity()) <= maxKeyOrderEntry;
}

std::span<const uint16_t> Object::keyOrder(State& state, KeyOrderStorage& storage) const
{
    const LayoutData& layout = this->layout();
    if (layout.keysAreNeverEnumerated) {
        VM& vm = this->vm();
        storage.shrink(0);
        for (const FieldData& field : layout.fields()) {
            if (isPresent(vm, *this, field))
                storage.append(layout.indexOf(field));
        }
        return storage.span();
    }
    uint32_t word = orderWord();
    if (isOrderStoredOutOfLine(word)) {
        std::span<const uint16_t> held = compiledStructure().outOfLineKeyOrder();
        return countOf(word) ? held.first(countOf(word)) : held;
    }
    return state.objects->keyOrders.entriesInOrder(layout, orderOf(word)).first(countOf(word));
}

bool Object::isInKeyOrder(State& stateAtHand, const FieldData& field) const
{
    State& state = stateAtHand.sharesRealm ? this->state() : stateAtHand;
    if (layout().keysAreNeverEnumerated)
        return isPresent(vm(), *this, field);
    KeyOrderStorage storage;
    auto keys = keyOrder(state, storage);
    uint16_t index = indexOf(field);
    if (index < keys.size() && keys[index] == index)
        return true;
    return WTF::find16(keys.data(), index, keys.size());
}

Vector<UniquedStringImpl*, 16> Object::extraKeyNames() const
{
    Vector<UniquedStringImpl*, 16> names;
    if (JSObject* extra = extraProperties()) {
        State& state = this->state();
        extra->structure()->forEachProperty(vm(), [&](const auto& property) {
            if (isExtraKey(state, *this, property.key()))
                names.append(property.key());
            return true;
        });
    }
    return names;
}

using CountsOfPositionedKeys = Vector<uint32_t, PlacesOfPositionedKeys::maxNumberOfCounts>;

static CountsOfPositionedKeys countPositionedExtraKeys(State& state, const Object& object)
{
    CountsOfPositionedKeys counts;
    const LayoutData& layout = object.layout();
    if (layout.keysAreNeverEnumerated || object.hasKeyOrderOfItsOwn())
        return counts;
    ExtraKeyCounter positioned(layout.numberOfFields);
    object.forEachKeyInOrder(state, [&](uint16_t key) {
        if (key >= layout.numberOfFields) {
            positioned.add(key);
            return;
        }
        if (positioned.count() && (counts.isEmpty() || counts.last() != positioned.count()))
            counts.append(positioned.count());
    });
    if (positioned.count() && (counts.isEmpty() || counts.last() != positioned.count()))
        counts.append(positioned.count());
    return counts;
}

static bool describes(const PlacesOfPositionedKeys& places, const Object& object, const PropertyTable& table, const CountsOfPositionedKeys& counts)
{
    return places.object == &object && places.table == &table && places.structureID == object.structureID().bits() && places.orderWord == object.orderWord()
        && places.numberOfRemovedEntries == table.numberOfRemovedEntries() && std::ranges::equal(std::span { places.counts }.first(places.numberOfCounts), counts);
}

static bool tryFindPlacesOfPositionedKeys(State& state, const Object& object, const PropertyTable& table, const CountsOfPositionedKeys& counts, PlacesOfPositionedKeys& places)
{
    places = { &object, &table, object.structureID().bits(), object.orderWord(), table.numberOfRemovedEntries(), static_cast<uint32_t>(counts.size()), { }, { }, 0, 0 };
    std::ranges::copy(counts, places.counts.begin());
    uint32_t numberOfKeys = 0;
    uint32_t next = 0;
    for (unsigned entry = 1, end = table.numberOfEntriesWithRemovedOnes(); entry <= end && next < counts.size(); ++entry) {
        UniquedStringImpl* key = table.keyOfEntry(entry);
        if (!key || !isExtraKey(state, object, key))
            continue;
        if (++numberOfKeys == counts[next])
            places.lastEntries[next++] = entry;
    }
    return next == counts.size();
}

static std::optional<uint32_t> findOrdinalByWalk(State& state, const Object& object, const PropertyTable& table, unsigned entry, uint32_t numberOfPositionedKeys)
{
    uint32_t ordinal = 0;
    for (unsigned earlier = 1; earlier < entry && ordinal < numberOfPositionedKeys; ++earlier) {
        UniquedStringImpl* key = table.keyOfEntry(earlier);
        ordinal += key && isExtraKey(state, object, key);
    }
    if (ordinal >= numberOfPositionedKeys)
        return std::nullopt;
    return ordinal;
}

bool Object::keyOrderCountsExtraKeys() const
{
    return !countPositionedExtraKeys(state(), *this).isEmpty();
}

static UniquedStringImpl* lastExtraKeyUpTo(State& state, const Object& object, const PropertyTable& table, unsigned entry)
{
    for (; entry; --entry) {
        UniquedStringImpl* key = table.keyOfEntry(entry);
        if (key && isExtraKey(state, object, key))
            return key;
    }
    return nullptr;
}

namespace {

class KeyOrderOfOneObject {
public:
    explicit KeyOrderOfOneObject(uint32_t numberOfFields)
        : m_numberOfFields(numberOfFields)
    {
    }

    void listExtraKeysUpTo(UniquedStringImpl* keyOrNull)
    {
        if (keyOrNull)
            m_lastKeyInFront = keyOrNull;
    }

    void listField(uint16_t field)
    {
        if (m_lastKeyInFront && (m_namedKeys.isEmpty() || m_namedKeys.last() != m_lastKeyInFront)) {
            m_entries.append(static_cast<uint16_t>(m_numberOfFields + m_namedKeys.size()));
            m_namedKeys.append(m_lastKeyInFront);
        }
        m_lastKeyInFront = nullptr;
        m_entries.append(field);
    }

    std::span<const uint16_t> entries() const { return m_entries.span(); }
    std::span<const RefPtr<UniquedStringImpl>> namedKeys() const { return m_namedKeys.span(); }
    void moveTo(const CompiledStructure& structure) { structure.setKeyOrderOfOneObject(WTF::move(m_entries), WTF::move(m_namedKeys)); }

private:
    uint32_t m_numberOfFields;
    UniquedStringImpl* m_lastKeyInFront { nullptr };
    CompiledStructure::KeyOrderEntries m_entries;
    Vector<RefPtr<UniquedStringImpl>> m_namedKeys;
};

} // anonymous namespace

void Object::takeKeyOrderOfItsOwn(VM& vm, State& state)
{
    ASSERT(!hasKeyOrderOfItsOwn() && !layout().keysAreNeverEnumerated);
    forgetPlacesOfPositionedKeys(state);
    KeyOrderOfOneObject order(layout().numberOfFields);
    if (keyOrderCountsExtraKeys()) {
        UniquedStringImpl* lastKeyListed = nullptr;
        forEachKeyAsCreated(vm, state, *this, [&](uint16_t field) {
            order.listExtraKeysUpTo(lastKeyListed);
            order.listField(field);
        }, [&](const auto& entry) {
            lastKeyListed = entry.key();
        });
    } else {
        forEachFieldInKeyOrder(state, [&](uint16_t field) {
            order.listField(field);
        });
    }
    JSCell::setStructure(vm, state.objects->structures.withKeyOrderOfOneObject(vm, compiledStructure(), order.entries(), order.namedKeys()));
    setOrderAndCount(outOfLineOrderWord);
    count(instance(), RuntimeCounterID::KeyOrdersStoredOutOfLine);
}

bool Object::tryListFieldBehindNamedKey(State& state, const FieldData& field)
{
    bool namesKeys = hasKeyOrderOfItsOwn();
    JSObject* extra = extraProperties();
    if (!namesKeys && !(extra && extra->structure()->isDictionary()))
        return false;
    if (field.flags & fieldMayExistWithoutStorage && isInKeyOrder(state, field))
        return true;
    VM& vm = this->vm();
    UniquedStringImpl* lastKey = nullptr;
    if (extra) {
        const PropertyTable& table = *extra->structure()->propertyTableInOrderOfAddition(vm);
        lastKey = lastExtraKeyUpTo(state, *this, table, table.numberOfEntriesWithRemovedOnes());
    }
    if (!namesKeys) {
        if (!lastKey)
            return false;
        takeKeyOrderOfItsOwn(vm, state);
    }
    const CompiledStructure& structure = compiledStructure();
    std::span<const RefPtr<UniquedStringImpl>> namedKeys = structure.keysNamedByOrder();
    if (!namedKeys.empty() && namedKeys.back() == lastKey)
        lastKey = nullptr;
    structure.appendToKeyOrderOfOneObject(lastKey, static_cast<uint16_t>(layout().numberOfFields + namedKeys.size()), indexOf(field));
    return true;
}

static void rebuildKeyOrderOfOneObject(const Object& object, const Invocable<bool(uint16_t)> auto& keepsField, const Invocable<UniquedStringImpl*(size_t, UniquedStringImpl*)> auto& keyInPlaceOf)
{
    const CompiledStructure& structure = object.compiledStructure();
    uint32_t numberOfFields = object.layout().numberOfFields;
    std::span<const RefPtr<UniquedStringImpl>> namedKeys = structure.keysNamedByOrder();
    KeyOrderOfOneObject order(numberOfFields);
    for (uint16_t entry : structure.outOfLineKeyOrder()) {
        if (entry >= numberOfFields)
            order.listExtraKeysUpTo(keyInPlaceOf(entry - numberOfFields, namedKeys[entry - numberOfFields].get()));
        else if (keepsField(entry))
            order.listField(entry);
    }
    order.moveTo(structure);
}

static std::optional<uint32_t> extraKeyOrdinal(State& state, const Object& object, JSObject& extra, PropertyName name, const CountsOfPositionedKeys& counts, PlacesOfPositionedKeys& places)
{
    PropertyTable& table = *extra.structure()->propertyTableInOrderOfAddition(object.vm());
    unsigned entry = table.find(name.uid()).entryIndex;
    if (entry == PropertyTable::EmptyEntryIndex)
        return std::nullopt;
    bool isExtra = isExtraKey(state, object, name.uid());

    if (counts.size() > PlacesOfPositionedKeys::maxNumberOfCounts)
        return isExtra ? findOrdinalByWalk(state, object, table, entry, counts.last()) : std::nullopt;
    if (!describes(places, object, table, counts) && !tryFindPlacesOfPositionedKeys(state, object, table, counts, places)) {
        places = { };
        return isExtra ? findOrdinalByWalk(state, object, table, entry, counts.last()) : std::nullopt;
    }
    places.entryOfKeyAskedFor = entry;
    places.keyAskedForIsExtraKey = isExtra;
    if (!isExtra)
        return std::nullopt;
    for (uint32_t index = 0; index < counts.size(); ++index) {
        if (entry <= places.lastEntries[index])
            return index ? counts[index - 1] : 0;
    }
    return std::nullopt;
}

Object::PlaceOfExtraKey Object::placeOfExtraKey(PropertyName name)
{
    JSObject* extra = extraProperties();
    if (!extra)
        return { };
    // The orders of the image list fields alone.
    if (orderOf(orderWord()) < layout().numberOfOrders)
        return { };
    State& state = this->state();
    if (!hasKeyOrderOfItsOwn()) {
        PlacesOfPositionedKeys& places = state.objects->keyOrders.placesOfPositionedKeys(*this);
        if (places.object == this)
            places.entryOfKeyAskedFor = 0;
        CountsOfPositionedKeys counts = countPositionedExtraKeys(state, *this);
        if (counts.isEmpty())
            return { };
        if (!extra->structure()->isDictionary())
            return { extraKeyOrdinal(state, *this, *extra, name, counts, places), std::nullopt, nullptr, true };
        takeKeyOrderOfItsOwn(vm(), state);
    }
    std::span<const RefPtr<UniquedStringImpl>> namedKeys = compiledStructure().keysNamedByOrder();
    for (uint32_t index = 0; index < namedKeys.size(); ++index) {
        if (namedKeys[index] != name.uid())
            continue;
        PropertyTable& table = *extra->structure()->propertyTableInOrderOfAddition(vm());
        unsigned entry = table.find(name.uid()).entryIndex;
        ASSERT(entry != PropertyTable::EmptyEntryIndex);
        return { std::nullopt, index, lastExtraKeyUpTo(state, *this, table, entry - 1) };
    }
    return { };
}

void Object::didRemoveExtraKey(VM& vm, State& state, const PlaceOfExtraKey& place)
{
    if (place.indexAmongNamedKeys) {
        uint32_t index = *place.indexAmongNamedKeys;
        const CompiledStructure& structure = compiledStructure();
        if (place.extraKeyInFront && (!index || structure.keysNamedByOrder()[index - 1] != place.extraKeyInFront)) {
            structure.replaceKeyNamedByOrder(index, place.extraKeyInFront);
            return;
        }
        rebuildKeyOrderOfOneObject(*this, [](uint16_t) {
            return true;
        }, [&](size_t other, UniquedStringImpl* key) {
            return other == index ? nullptr : key;
        });
        return;
    }
    if (!place.orderCountsExtraKeys)
        return;
    if (place.ordinal)
        removeExtraKeyFromOrder(vm, state, *place.ordinal);
    didRemoveCountedExtraKey(state);
}

void Object::forgetPlacesOfPositionedKeys(State& state)
{
    PlacesOfPositionedKeys& places = state.objects->keyOrders.placesOfPositionedKeys(*this);
    if (places.object == this)
        places = { };
}

void Object::didRemoveCountedExtraKey(State& state)
{
    PlacesOfPositionedKeys& places = state.objects->keyOrders.placesOfPositionedKeys(*this);
    if (places.object != this)
        return;
    uint32_t removed = std::exchange(places.entryOfKeyAskedFor, 0);
    JSObject* extra = extraProperties();
    const PropertyTable* table = extra ? extra->structure()->propertyTableInOrderOfAddition(vm()) : nullptr;
    if (!removed || table != places.table || table->numberOfRemovedEntries() != places.numberOfRemovedEntries + 1) {
        places = { };
        return;
    }
    for (uint32_t index = 0; places.keyAskedForIsExtraKey && index < places.numberOfCounts; ++index) {
        if (removed > places.lastEntries[index])
            continue;
        uint32_t countInFront = index ? places.counts[index - 1] : 0;
        if (--places.counts[index] == countInFront) {
            places = { };
            return;
        }
        if (removed != places.lastEntries[index])
            continue;
        unsigned entry = removed - 1;
        auto isExtraKeyAt = [&](unsigned candidate) {
            UniquedStringImpl* key = table->keyOfEntry(candidate);
            return key && isExtraKey(state, *this, key);
        };
        while (entry && !isExtraKeyAt(entry))
            --entry;
        if (!entry) {
            places = { };
            return;
        }
        places.lastEntries[index] = entry;
    }
    places.numberOfRemovedEntries = table->numberOfRemovedEntries();
    places.structureID = structureID().bits();
    places.orderWord = orderWord();
}

std::optional<Object::InitialKeyOrder> Object::initialKeyOrder(VM& vm, std::span<const uint16_t> keys, JSObject* extra)
{
    State& state = this->state();
    const LayoutData& layout = this->layout();
    if (hasFlagsInOrderWord())
        return std::nullopt;
    StructureTable& structures = state.objects->structures;
    CompiledStructure* structure = const_cast<CompiledStructure*>(&compiledStructure());
    if (hasOutOfLineKeyOrder())
        structure = structures.withNumberedKeyOrder(vm, *structure);
    if (extra) {
        Structure* extraStructure = extra->structure();
        if (!layout.extraPropertiesSlot() || layout.impossibleMutations().contains(ObjectMutation::AddProperty) || !canPositionEveryKey(layout, extraStructure))
            return std::nullopt;
        bool areAllExtra = true;
        extraStructure->forEachProperty(vm, [&](const auto& property) {
            areAllExtra = !property.attributes() && !state.objects->layouts.field(vm, layout, property.key());
            return areAllExtra;
        });
        if (!areAllExtra)
            return std::nullopt;
        extraStructure->forEachProperty(vm, [&](const auto& property) {
            willAddExtraProperty(vm, property.key());
            return true;
        });
        CompiledStructure* existing = structures.existingWithExtraProperties(*structure, *extraStructure);
        structure = existing ? existing : structures.withExtraProperties(vm, *structure, nullptr);
    }
    if (layout.keysAreNeverEnumerated)
        return InitialKeyOrder { unorderedOrderWord, structure };
    if (auto orderAndCount = state.objects->keyOrders.orderStartingWith(layout, keys))
        return InitialKeyOrder { *orderAndCount, structure };
    count(instance(), RuntimeCounterID::KeyOrdersStoredOutOfLine);
    return InitialKeyOrder { outOfLineOrderWordFor(keys.size()), structures.withOutOfLineKeyOrder(vm, *structure, keys) };
}

void Object::setInitialKeyOrder(VM& vm, InitialKeyOrder order, JSObject* extra)
{
    setOrderAndCount(order.orderAndCount);
    if (extra) {
        setExtraProperties(vm, extra, order.structure);
        return;
    }
    if (order.structure != &compiledStructure())
        JSCell::setStructure(vm, order.structure);
}

static uint32_t countExtraKeys(State& state, const Object& object, JSObject* extra)
{
    const CompiledStructure& structure = object.compiledStructure();
    bool isFixedByStructure = structure.listsEveryExtraProperty() && structure.extraPropertiesStructure() == extra->structure();
    if (isFixedByStructure) {
        if (std::optional<uint16_t> known = structure.numberOfExtraKeys())
            return *known;
    }
    auto countByWalk = [&](bool mayHoldFields) {
        uint32_t count = 0;
        extra->structure()->forEachProperty(object.vm(), [&](const auto& property) {
            ASSERT(mayHoldFields || !object.fieldNamed(state, property.key()));
            count += mayHoldFields ? isExtraKey(state, object, property.key()) : !PropertyName(property.key()).isPrivateName();
            return true;
        });
        return count;
    };
    bool mayHoldFields = object.orderWord() & orderWordHasFieldOutsideSlot;
    uint32_t numberOfExtraKeys;
    if (Structure* extraStructure = extra->structure(); !mayHoldFields && extraStructure->isQuickPropertyAccessAllowedForEnumeration()) {
        numberOfExtraKeys = static_cast<uint32_t>(numberOfSlotsForMaxOffset(extraStructure->maxOffset(), extraStructure->inlineCapacity()));
        ASSERT(numberOfExtraKeys == countByWalk(false));
    } else
        numberOfExtraKeys = countByWalk(mayHoldFields);
    if (isFixedByStructure)
        structure.setNumberOfExtraKeys(numberOfExtraKeys);
    return numberOfExtraKeys;
}

std::optional<uint32_t> Object::numberOfExtraKeysByStructure() const
{
    if (!hasExtraProperties())
        return 0;
    return compiledStructure().numberOfExtraKeys();
}

using AddedEntries = Vector<uint16_t, 4>;

static AddedEntries entriesPositioningExtraKeys(State& state, const Object& object, std::optional<uint32_t> numberOfKeysOfPendingStars)
{
    AddedEntries entries;
    JSObject* extra = numberOfKeysOfPendingStars ? nullptr : object.extraProperties();
    if (!extra && !numberOfKeysOfPendingStars)
        return entries;
    const LayoutData& layout = object.layout();
    uint32_t numberOfExtraKeys = extra ? countExtraKeys(state, object, extra) : *numberOfKeysOfPendingStars;
    if (!numberOfExtraKeys)
        return entries;
    if (!object.hasOutOfLineKeyOrder() && state.objects->keyOrders.positionsExtraKeys(layout, object.orderWord() & orderAndCountMask, numberOfExtraKeys))
        return entries;
    ExtraKeyCounter positioned(layout.numberOfFields);
    Object::KeyOrderStorage storage;
    positioned.addAll(object.keyOrder(state, storage));
    positioned.appendEntriesCountingTo(numberOfExtraKeys, entries);
    return entries;
}

static void storeKeyOrderOutOfLine(VM& vm, State& state, Object& object, std::span<const uint16_t> keys, StructureTable::HeldKeyOrder heldKeyOrder = StructureTable::HeldKeyOrder::IsFoundByOthers)
{
    object.JSCell::setStructure(vm, state.objects->structures.withOutOfLineKeyOrder(vm, object.compiledStructure(), keys, heldKeyOrder));
    object.setOrderAndCount(outOfLineOrderWordFor(keys.size()));
    count(object.instance(), RuntimeCounterID::KeyOrdersStoredOutOfLine);
}

static bool tryStepInOutOfLineKeyOrder(Object& object, std::span<const uint16_t> added)
{
    uint32_t count = countOf(object.orderWord());
    if (!object.hasOutOfLineKeyOrder() || !count || count + added.size() > maxFieldsInOrder)
        return false;
    const CompiledStructure& structure = object.compiledStructure();
    for (uint16_t entry : added) {
        std::span<const uint16_t> held = structure.outOfLineKeyOrder();
        if (count < held.size() ? held[count] != entry : !structure.tryAppendToOutOfLineKeyOrder(entry))
            return false;
        ++count;
    }
    object.setOrderAndCount(outOfLineOrderWordFor(count));
    return true;
}

static ALWAYS_INLINE bool trySearchForHeldKeyOrder(VM& vm, State& state, Object& object, std::span<const uint16_t> keys)
{
    KeyOrderTable& keyOrders = state.objects->keyOrders;
    const LayoutData& layout = object.layout();
    bool leavesNumberedOrder = !object.hasOutOfLineKeyOrder();
    if (leavesNumberedOrder && !keyOrders.shouldSearchForHeldOrder(layout))
        return false;
    bool leavesOrderThatItFound = !leavesNumberedOrder && object.compiledStructure().keyOrderWasFoundAgain();
    storeKeyOrderOutOfLine(vm, state, object, keys);
    bool didFind = object.compiledStructure().keyOrderWasFoundAgain();
    if (leavesNumberedOrder && didFind)
        keyOrders.didFindHeldOrder(layout);
    else if (leavesOrderThatItFound && !didFind)
        keyOrders.didMakeHeldOrderAfterFindingOne(layout);
    return true;
}

// For an order that has no number. No order with a number starts with it, and the table takes no other order once it has refused one, so what is longer has no number either.
static void appendEntriesToUnnumberedKeyOrder(VM& vm, State& state, Object& object, std::span<const uint16_t> added)
{
    if (tryStepInOutOfLineKeyOrder(object, added))
        return;
    Object::KeyOrderStorage storage;
    Vector<uint16_t, 32> entries(object.keyOrder(state, storage));
    entries.append(added);
    if (!trySearchForHeldKeyOrder(vm, state, object, entries.span()))
        storeKeyOrderOutOfLine(vm, state, object, entries.span(), StructureTable::HeldKeyOrder::IsFoundByNobody);
}

void Object::willCreateField(State&, const FieldData& field)
{
    if (field.isOutOfLine())
        ensureStorageFor(vm(), field);
}

void Object::didCreateField(State& stateAtHand, const FieldData& field, bool withStorage)
{
    State& state = stateAtHand.sharesRealm ? this->state() : stateAtHand;
    if (withStorage)
        willCreateField(state, field);
    const LayoutData& layout = this->layout();
    if (layout.isClass())
        state.objects->classes.didShadowMember(layout, state.objects->layouts.nameOf(vm(), field));
    if (layout.keysAreNeverEnumerated)
        return;
    KeyOrderTable& keyOrders = state.objects->keyOrders;
    if (std::optional<uint32_t> after = keyOrders.stepInsideOrderOfImage(layout, orderWord(), indexOf(field))) {
        setOrderAndCount(*after);
        return;
    }
    uint32_t before = orderWord() & orderAndCountMask;
    std::optional<uint32_t> numberOfExtraKeys = numberOfExtraKeysByStructure();
    if (numberOfExtraKeys && !hasOutOfLineKeyOrder()) {
        if (std::optional<uint32_t> after = keyOrders.publishedTransition(layout, before, field, *numberOfExtraKeys)) {
            setOrderAndCount(*after);
            return;
        }
    }
    if (!hasFlagsInOrderWord() && !hasOutOfLineKeyOrder() && !(field.flags & fieldMayExistWithoutStorage)) {
        if (std::optional<uint32_t> after = keyOrders.tryAppendToAddedOrder(layout, before, field)) {
            setOrderAndCount(*after);
            return;
        }
    }
    // The order needs the number of the keys in front of the field, so stars go on waiting.
    std::optional<uint32_t> numberOfKeysOfStars = numberOfKeysOfPendingStars(vm(), *this);
    if (!numberOfKeysOfStars && tryListFieldBehindNamedKey(state, field))
        return;
    AddedEntries added = entriesPositioningExtraKeys(state, *this, numberOfKeysOfStars);
    numberOfExtraKeys = numberOfExtraKeysByStructure();
    if (field.flags & fieldMayExistWithoutStorage && isInKeyOrder(state, field)) {
        if (!hasOutOfLineKeyOrder() && numberOfExtraKeys)
            keyOrders.publishTransition(layout, before, field, before, *numberOfExtraKeys);
        return;
    }
    added.append(indexOf(field));
    if (!hasOutOfLineKeyOrder()) {
        std::optional<uint32_t> after = before;
        bool publishesWholeStep = numberOfExtraKeys && (*numberOfExtraKeys || added.size() > 1);
        for (uint16_t entry : added) {
            if (after)
                after = keyOrders.orderAfterCreating(layout, *after, entry, publishesWholeStep ? KeyOrderTable::Step::IsNotPublished : KeyOrderTable::Step::IsPublished);
        }
        if (after) {
            if (publishesWholeStep)
                keyOrders.publishTransition(layout, before, field, *after, *numberOfExtraKeys);
            setOrderAndCount(*after);
            return;
        }
    }
    appendEntriesToUnnumberedKeyOrder(vm(), state, *this, added.span());
}

ALWAYS_INLINE bool Object::trySetNumberedKeyOrder(VM& vm, State& state, std::span<const uint16_t> keys)
{
    const LayoutData& layout = this->layout();
    ASSERT(!layout.keysAreNeverEnumerated && !hasKeyOrderOfItsOwn());
    auto numbered = state.objects->keyOrders.orderStartingWith(layout, keys);
    if (!numbered)
        return false;
    bool wasOutOfLine = hasOutOfLineKeyOrder();
    setOrderAndCount(*numbered);
    if (wasOutOfLine)
        JSCell::setStructure(vm, state.objects->structures.withNumberedKeyOrder(vm, compiledStructure()));
    return true;
}

void Object::setKeyOrder(VM& vm, State& state, std::span<const uint16_t> keys)
{
    if (!trySetNumberedKeyOrder(vm, state, keys))
        storeKeyOrderOutOfLine(vm, state, *this, keys);
}

void Object::removeExtraKeyFromOrder(VM& vm, State& state, uint32_t ordinal)
{
    const LayoutData& layout = this->layout();
    if (layout.keysAreNeverEnumerated)
        return;
    uint32_t numberOfFields = layout.numberOfFields;
    ExtraKeyCounter positionedBefore(numberOfFields);
    ExtraKeyCounter positionedAfter(numberOfFields);
    bool isChanged = false;
    Vector<uint16_t, 16> keys;
    uint32_t countInFrontOfNextField = 0;
    forEachKeyInOrder(state, [&](uint16_t key) {
        if (key < numberOfFields) {
            positionedAfter.appendEntriesCountingTo(countInFrontOfNextField, keys);
            keys.append(key);
            return;
        }
        positionedBefore.add(key);
        bool countsRemovedKey = positionedBefore.count() > ordinal;
        isChanged |= countsRemovedKey;
        countInFrontOfNextField = positionedBefore.count() - countsRemovedKey;
    });
    if (isChanged)
        setKeyOrder(vm, state, keys.span());
}

void removeFieldFromKeyOrder(VM& vm, State& state, Object& object, const FieldData& field)
{
    if (object.layout().keysAreNeverEnumerated)
        return;
    object.forgetPlacesOfPositionedKeys(state);
    uint16_t index = object.indexOf(field);
    auto removeFromOrderOfItsOwn = [&] {
        rebuildKeyOrderOfOneObject(object, [&](uint16_t other) {
            return other != index;
        }, [](size_t, UniquedStringImpl* key) {
            return key;
        });
    };
    if (object.hasOutOfLineKeyOrder() && !object.hasKeyOrderOfItsOwn() && !object.compiledStructure().keyOrderWasFoundAgain() && !hasPendingStars(object))
        object.takeKeyOrderOfItsOwn(vm, state);
    if (object.hasKeyOrderOfItsOwn()) {
        removeFromOrderOfItsOwn();
        return;
    }
    uint32_t numberOfFields = object.layout().numberOfFields;
    auto countsExtraKeys = [&](uint16_t key) {
        return key >= numberOfFields;
    };
    Vector<uint16_t, 16> remaining;
    object.forEachKeyInOrder(state, [&](uint16_t key) {
        if (key == index)
            return;
        if (countsExtraKeys(key) && !remaining.isEmpty() && countsExtraKeys(remaining.last()) && remaining.last() != maxKeyOrderEntry)
            remaining.last() = std::max(remaining.last(), key);
        else
            remaining.append(key);
    });
    while (!remaining.isEmpty() && countsExtraKeys(remaining.last()))
        remaining.removeLast();
    if (object.trySetNumberedKeyOrder(vm, state, remaining.span()) || trySearchForHeldKeyOrder(vm, state, object, remaining.span()))
        return;
    if (hasPendingStars(object)) {
        storeKeyOrderOutOfLine(vm, state, object, remaining.span());
        return;
    }
    object.takeKeyOrderOfItsOwn(vm, state);
    removeFromOrderOfItsOwn();
}

static bool hasNumberedKeyOrder(const Object& object)
{
    return !object.hasOutOfLineKeyOrder() && !object.layout().keysAreNeverEnumerated;
}

static bool holdsValue(const Object& object, const FieldData& listedField)
{
    SlotRepresentation representation = listedField.slot.representation();
    if (representation == SlotRepresentation::DeferredClosure)
        return object.isInSlot(listedField);
    const void* slot = object.slotOf(listedField);
    if (slot && !slotHoldsAbsent(representation, slot))
        return true;
    return listedField.flags & fieldMayExistWithoutStorage && !livesInExtraProperties(object.vm(), object, listedField);
}

std::optional<std::span<const FieldAsProperty>> Object::fieldsInKeyOrderSlow() const
{
    validateKeyListing(vm(), *this);
    if (hasExtraProperties() || !hasNumberedKeyOrder(*this))
        return std::nullopt;
    State& state = this->state();
    uint32_t orderAndCount = orderWord() & orderAndCountMask;
    auto fields = state.objects->keyOrders.propertiesInOrder(*state.instance.vm, layout(), orderAndCount);
    if (fields)
        compiledStructure().setListedFields({ orderAndCount, static_cast<uint32_t>(fields->size()), fields->data() });
    return fields;
}

bool Object::forEachFieldAsProperty(const ScopedLambda<bool(UniquedStringImpl&, JSValue)>& functor, SymbolKeys symbolKeys) const
{
    if (hasExtraProperties())
        return false;
    State& state = this->state();
    VM& vm = *state.instance.vm;
    validateKeyListing(vm, *this);
    std::optional<std::span<const FieldAsProperty>> properties;
    if (symbolKeys == SymbolKeys::Skip || !layout().hasFieldsWithSymbolKeys)
        properties = fieldsInKeyOrder();
    else if (hasNumberedKeyOrder(*this))
        properties = state.objects->keyOrders.propertiesInOrder(vm, layout(), orderWord() & orderAndCountMask, symbolKeys);
    if (properties) {
        for (auto& property : *properties) {
            JSValue value = readListedField(property);
            if (value && !functor(*property.name, value))
                break;
        }
        return true;
    }
    RuntimeLayoutTable& layouts = state.objects->layouts;
    std::span<const FieldData> fields = layout().fields();
    bool hasOnlyPlainFields = true;
    forEachFieldInKeyOrder(state, [&](uint16_t index) {
        hasOnlyPlainFields &= !fields[index].attributes();
    });
    if (!hasOnlyPlainFields)
        return false;
    bool shouldContinue = true;
    forEachFieldInKeyOrder(state, [&](uint16_t index) {
        const FieldData& field = fields[index];
        if (!shouldContinue || !(layouts.hasStringKey(field) || (symbolKeys == SymbolKeys::Include && layouts.hasSymbolKey(field))))
            return;
        if (JSValue value = read(field))
            shouldContinue = functor(*layouts.nameOf(vm, field), value);
    });
    return true;
}

static NEVER_INLINE void writeJSONKeys(VM& vm, CachedPropertyList& list)
{
    size_t size = FieldAsProperty::jsonKeyBlockSize;
    bool areWrittenAsTheyAre = true;
    for (const PropertyListEntry& property : list.properties) {
        list.hasToJSON |= property.name == vm.propertyNames->toJSON.impl();
        areWrittenAsTheyAre = areWrittenAsTheyAre && property.name->is8Bit() && std::ranges::all_of(property.name->span8(), [](Latin1Character character) {
            return character >= 0x20 && character < 0x7F && character != '"' && character != '\\';
        });
        size += property.name->length() + 4;
    }
    if (!areWrittenAsTheyAre)
        return;
    list.jsonKeys.fill(0, size);
    list.endsOfJSONKeys.fill(0, list.properties.size());
    Latin1Character* next = list.jsonKeys.mutableSpan().data();
    uint32_t* end = list.endsOfJSONKeys.mutableSpan().data();
    const Latin1Character* start = next;
    for (const PropertyListEntry& property : list.properties) {
        *next++ = ',';
        *next++ = '"';
        memcpy(next, property.name->span8().data(), property.name->length());
        next += property.name->length();
        *next++ = '"';
        *next++ = ':';
        *end++ = next - start;
    }
}

CachedPropertyList* findCachedPropertyList(VM& vm, const Object& object, JSObject* extra)
{
    validateKeyListing(vm, object);
    if (!hasNumberedKeyOrder(object))
        return nullptr;
    StructureID structureID = extra->structureID();
    uint32_t shapeWord = object.shapeWord();
    uint32_t orderAndCount = object.orderWord() & orderAndCountMask;
    CachedPropertyList& entry = object.state().objects->caches->propertyLists.entryFor(structureID, shapeWord, orderAndCount);
    return entry.extraPropertiesStructureID == structureID && entry.shapeWord == shapeWord && entry.orderAndCount == orderAndCount ? &entry : nullptr;
}

CachedPropertyList* cachedPropertyList(VM& vm, const Object& object)
{
    validateKeyListing(vm, object);
    JSObject* extra = object.extraProperties();
    if (!extra || !hasNumberedKeyOrder(object))
        return nullptr;
    State& state = object.state();
    const LayoutData& layout = object.layout();
    PropertyListCache& propertyLists = state.objects->caches->propertyLists;
    Structure* structure = extra->structure();
    uint32_t shapeWord = object.shapeWord();
    uint32_t orderAndCount = object.orderWord() & orderAndCountMask;
    auto& cachedEntry = propertyLists.entryFor(structure->id(), shapeWord, orderAndCount);
    if (cachedEntry.extraPropertiesStructureID == structure->id() && cachedEntry.shapeWord == shapeWord && cachedEntry.orderAndCount == orderAndCount) {
        for (auto& property : cachedEntry.properties) {
            if (property.field && !holdsValue(object, *property.field))
                return nullptr;
        }
        return &cachedEntry;
    }
    if (extra->type() != FinalObjectType || structure->isDictionary() || structure->hasNonEnumerableProperties() || !structure->canPerformFastPropertyEnumeration() || structure->hasNonReifiedStaticProperties())
        return nullptr;
    RuntimeLayoutTable& layouts = state.objects->layouts;
    std::span<const FieldData> fields = layout.fields();
    Vector<PropertyListEntry, 16> extraKeys;
    bool hasSymbolKey = false;
    structure->forEachProperty(vm, [&](const auto& entry) {
        hasSymbolKey = entry.key()->isSymbol();
        if (!hasSymbolKey && !object.fieldNamed(state, entry.key()))
            extraKeys.append(PropertyListEntry { entry.key(), nullptr, entry.offset() });
        return !hasSymbolKey;
    });
    if (hasSymbolKey)
        return nullptr;
    Vector<PropertyListEntry> properties;
    bool hasAllFields = true;
    Object::KeyOrderStorage storage;
    size_t nextExtraKey = Object::forEachPositionedKey(object.keyOrder(state, storage), layout.numberOfFields, extraKeys.size(), [&](uint16_t index) {
        const FieldData& field = fields[index];
        if (!layouts.hasStringKey(field)) {
            hasAllFields &= !layouts.hasSymbolKey(field);
            return;
        }
        UniquedStringImpl* name = layouts.nameOf(vm, field);
        if (holdsValue(object, field)) {
            hasAllFields &= !field.attributes();
            properties.append(PropertyListEntry { name, &field, invalidOffset });
            return;
        }
        PropertyOffset offset = structure->get(vm, name);
        hasAllFields &= isValidOffset(offset);
        properties.append(PropertyListEntry { name, nullptr, offset });
    }, [&](size_t ordinal) {
        properties.append(extraKeys[ordinal]);
    });
    if (!hasAllFields)
        return nullptr;
    for (; nextExtraKey < extraKeys.size(); ++nextExtraKey)
        properties.append(extraKeys[nextExtraKey]);
    auto& newEntry = propertyLists.entryFor(structure->id(), shapeWord, orderAndCount);
    newEntry = { structure->id(), shapeWord, orderAndCount, nullptr, WTF::move(properties) };
    writeJSONKeys(vm, newEntry);
    return &newEntry;
}

JSValue Object::readPropertyListEntry(const PropertyListEntry& property, JSObject* extra) const
{
    if (!property.field)
        return extra->getDirect(property.offsetInExtraProperties);
    JSValue value = readInline(*property.field);
    return value ? value : read(*property.field);
}

bool Object::keysDependOnlyOnStructureAndOrderWord() const
{
    return hasNumberedKeyOrder(*this) && (!hasExtraProperties() || compiledStructure().listsEveryExtraProperty());
}

JSCellButterfly* Object::keysWithExtraProperties(VM& vm) const
{
    DeferGC deferGC(vm);
    auto* listing = cachedPropertyList(vm, *this);
    if (!listing || listing->keys)
        return listing ? listing->keys : nullptr;
    JSCellButterfly* list = JSCellButterfly::tryCreate(vm, CopyOnWriteArrayWithContiguous, listing->properties.size());
    if (!list)
        return nullptr;
    for (unsigned i = 0; i < listing->properties.size(); ++i)
        list->setIndex(vm, i, jsOwnedString(vm, listing->properties[i].name));
    listing->keys = list;
    return list;
}

JSArray* Object::tryCreateKeysArrayWithExtraProperties(JSGlobalObject* globalObject) const
{
    VM& vm = globalObject->vm();
    DeferGC deferGC(vm);
    JSCellButterfly* keys = keysWithExtraProperties(vm);
    if (!keys)
        return nullptr;
    if (keysDependOnlyOnStructureAndOrderWord())
        return JSArray::createWithButterfly(vm, nullptr, globalObject->originalArrayStructureForIndexingType(keys->indexingMode()), keys->toButterfly());
    unsigned length = keys->length();
    JSArray* list = JSArray::tryCreate(vm, globalObject->arrayStructureForIndexingTypeDuringAllocation(ArrayWithContiguous), length);
    if (!list)
        return nullptr;
    auto values = list->butterfly()->contiguous();
    for (unsigned i = 0; i < length; ++i)
        values.at(list, i).setWithoutWriteBarrier(keys->get(i));
    return list;
}

const Vector<PropertyListEntry>* Object::fieldsAndExtraPropertiesInKeyOrder(VM& vm) const
{
    auto* listing = cachedPropertyList(vm, *this);
    return listing ? &listing->properties : nullptr;
}

JSCellButterfly* Object::keys(VM& vm, KeyListKind kind) const
{
    validateKeyListing(vm, *this);
    if (hasExtraProperties() || !hasNumberedKeyOrder(*this))
        return nullptr;
    State& state = this->state();
    if (!hasKeyListsInTable(kind, state.tables.numberOfShapes))
        return state.objects->keyOrders.keysInOrder(vm, layout(), orderWord(), kind);
    uint32_t shapeWord = keyListWordOf(this->shapeWord(), kind);
    auto& cacheEntry = state.instance.objects.keyLists[indexOfKeyList(shapeWord, orderWord())];
    if (cacheEntry.shapeWord == shapeWord && cacheEntry.orderWord == orderWord())
        return uncheckedDowncast<JSCellButterfly>(cacheEntry.list);
    JSCellButterfly* list = state.objects->keyOrders.keysInOrder(vm, layout(), orderWord(), kind);
    if (list)
        cacheEntry = { orderWord(), shapeWord, list };
    return list;
}

static CachedPropertyList* listOfKeysFixedByStructure(VM& vm, const Object& object)
{
    JSObject* extra = object.extraProperties();
    if (!extra || !object.keysDependOnlyOnStructureAndOrderWord() || hasIndexedProperties(extra->indexingType()))
        return nullptr;
    return cachedPropertyList(vm, object);
}

static JSPropertyNameEnumerator* cachedEnumeratorOfObjectWithExtraProperties(VM& vm, const Object& object, StructureChain* prototypeChain)
{
    CachedPropertyList* listing = listOfKeysFixedByStructure(vm, object);
    if (!listing || !listing->enumerator || listing->prototypeChain != prototypeChain || listing->enumerator->cachedStructureID() != object.structureID())
        return nullptr;
    return listing->enumerator;
}

// Which names a loop finds on the object itself, and in which order: the attributes of a field in its slot are those of the layout, and the Structure of the extra properties has those of every other.
static bool enumerableNamesFollowFromStructureAndOrderWord(VM& vm, const Object& object)
{
    if (!object.keysDependOnlyOnStructureAndOrderWord())
        return false;
    JSObject* extra = object.extraProperties();
    if (extra && (object.compiledStructure().extraPropertiesStructureID() != extra->structureID() || !extra->structure()->canCacheOwnPropertyNames()))
        return false;
    std::span<const FieldData> fields = object.layout().fields();
    bool holdsEveryListedField = true;
    object.forEachFieldInKeyOrder(object.state(), [&](uint16_t index) {
        holdsEveryListedField &= holdsValue(object, fields[index]) || (extra && livesInExtraProperties(vm, object, fields[index]));
    });
    return holdsEveryListedField;
}

static bool holdsValueUnderEveryOwnName(const Object& object)
{
    JSObject* extra = object.extraProperties();
    if (extra && extra->structure()->hasAnyKindOfGetterSetterProperties())
        return false;
    std::span<const FieldData> fields = object.layout().fields();
    bool hasAccessor = false;
    object.forEachFieldInKeyOrder(object.state(), [&](uint16_t index) {
        hasAccessor |= fields[index].isAccessor();
    });
    return !hasAccessor;
}

static bool canCacheEnumeratorFor(StructureChain* prototypeChain)
{
    for (StructureID* current = prototypeChain->head(); *current; ++current) {
        Structure* structure = current->decode();
        if (!structure->canCacheOwnPropertyNames() && structure->classInfoForCells() != ClassPrototype::info())
            return false;
    }
    return true;
}

JSPropertyNameEnumerator* Object::propertyNameEnumerator(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    State& state = this->state();
    KeyOrderTable& keyOrders = state.objects->keyOrders;
    const LayoutData& layout = this->layout();
    if (JSObject* extra = extraProperties(); extra && !compiledStructure().extraPropertiesStructureID() && state.propertyCaches->shouldFile(extra->structureID(), nullptr, SlotAccess::Read))
        ensureStructureForExtraProperties(vm);
    Structure* structureBefore = WTF::opaque(structure());
    uint32_t orderWordBefore = orderWord();

    bool hasExtraProperties = this->hasExtraProperties();
    bool ownNamesAreFixed = enumerableNamesFollowFromStructureAndOrderWord(vm, *this);
    bool isCacheable = ownNamesAreFixed && (hasExtraProperties || !compiledStructure().hasOtherPrototype());
    StructureChain* prototypeChainBefore = nullptr;
    if (isCacheable) {
        prototypeChainBefore = structureBefore->prototypeChain(vm, globalObject, this);
        if (JSPropertyNameEnumerator* cached = hasExtraProperties ? cachedEnumeratorOfObjectWithExtraProperties(vm, *this, prototypeChainBefore) : keyOrders.enumerator(layout, orderWordBefore, prototypeChainBefore)) {
            keyOrders.publishEnumerator(*this, cached);
            return cached;
        }
    }

    uint32_t indexedLength = 0;
    uint32_t numberOfStructureProperties = 0;
    uint32_t numberOfOwnNames = 0;
    PropertyNameArrayBuilder names(vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
    getEnumerablePropertyNames(globalObject, this, names, indexedLength, numberOfStructureProperties, &numberOfOwnNames);
    RETURN_IF_EXCEPTION(scope, nullptr);
    ASSERT(!indexedLength && !numberOfStructureProperties);

    bool isUnchanged = structure() == structureBefore && orderWord() == orderWordBefore;
    constexpr uint32_t orderWordMask = orderAndCountMask | orderWordHasExtraProperties;
    JSPropertyNameEnumerator* enumerator = nullptr;
    if (!names.size())
        enumerator = vm.emptyPropertyNameEnumerator();
    else if (ownNamesAreFixed && isUnchanged)
        enumerator = JSPropertyNameEnumerator::tryCreate(vm, structureBefore, 0, holdsValueUnderEveryOwnName(*this) ? numberOfOwnNames : 0, WTF::move(names), orderWordBefore & orderWordMask, orderWordMask, numberOfOwnNames);
    else
        enumerator = JSPropertyNameEnumerator::tryCreate(vm, structure(), 0, 0, WTF::move(names));
    if (!enumerator) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        return nullptr;
    }

    bool sawPolyProto;
    if (isCacheable && isUnchanged && normalizePrototypeChain(globalObject, this, sawPolyProto) != InvalidPrototypeChain) {
        if (structureBefore->prototypeChain(vm, globalObject, this) == prototypeChainBefore && canCacheEnumeratorFor(prototypeChainBefore)) {
            if (!hasExtraProperties)
                keyOrders.setEnumerator(vm, layout, orderWordBefore, enumerator, prototypeChainBefore);
            else if (CachedPropertyList* listing = listOfKeysFixedByStructure(vm, *this)) {
                listing->enumerator = enumerator;
                listing->prototypeChain = prototypeChainBefore;
            }
            keyOrders.publishEnumerator(*this, enumerator);
        }
    }
    return enumerator;
}

PropertyListCache::PropertyListCache(State& state)
    : m_state(state)
    , m_entries(state.tableArena)
    , m_ownKeyLists(state.tableArena)
{
    state.caches->add(CacheKind::ClearedEachCollection, "property lists"_s, *this);
}

CachedPropertyList& PropertyListCache::entryFor(StructureID extraPropertiesStructureID, uint32_t shapeWord, uint32_t orderAndCount)
{
    if (!m_entries)
        m_entries.make();
    m_writes.recordAll();
    return (*m_entries)[((extraPropertiesStructureID.bits() >> 4) + shapeWord + orderAndCount * 31) % numberOfEntries];
}

KeptOwnKeys& PropertyListCache::ownKeysFor(StructureID structureID, uint32_t orderWord, bool listsSymbols)
{
    constexpr unsigned listsPerSet = 4;
    static_assert(!(numberOfOwnKeyLists % listsPerSet));
    if (!m_ownKeyLists)
        m_ownKeyLists.make();
    m_writes.recordAll();
    auto set = std::span { *m_ownKeyLists }.subspan(WTF::pairIntHash(structureID.bits(), orderWord) % (numberOfOwnKeyLists / listsPerSet) * listsPerSet, listsPerSet);
    KeptOwnKeys* unused = nullptr;
    KeptOwnKeys* withoutList = nullptr;
    for (KeptOwnKeys& kept : set) {
        if (kept.isFor(structureID, orderWord, listsSymbols))
            return kept;
        if (!kept.structureID)
            unused = &kept;
        else if (!kept.keys)
            withoutList = &kept;
    }
    if (unused)
        return *unused;
    return withoutList ? *withoutList : set[m_numberOfOwnKeyListsReplaced++ % listsPerSet];
}

JSCellButterfly* findKeptOwnKeys(const Object& object, bool listsSymbols)
{
    if (!object.keysDependOnlyOnStructureAndOrderWord())
        return nullptr;
    const KeptOwnKeys& kept = object.state().objects->caches->propertyLists.ownKeysFor(object.structureID(), object.orderWord(), listsSymbols);
    return kept.isFor(object.structureID(), object.orderWord(), listsSymbols) ? kept.keys : nullptr;
}

void didListOwnKeys(VM& vm, const Object& object, bool listsSymbols, JSArray* list)
{
    if (!object.keysDependOnlyOnStructureAndOrderWord())
        return;
    DeferGC deferGC(vm);
    KeptOwnKeys& kept = object.state().objects->caches->propertyLists.ownKeysFor(object.structureID(), object.orderWord(), listsSymbols);
    if (!kept.isFor(object.structureID(), object.orderWord(), listsSymbols)) {
        kept = { object.structureID(), object.orderWord(), listsSymbols, nullptr };
        return;
    }
    unsigned length = list->length();
    JSCellButterfly* keys = JSCellButterfly::tryCreate(vm, CopyOnWriteArrayWithContiguous, length);
    if (!keys)
        return;
    auto elements = list->butterfly()->contiguous();
    for (unsigned index = 0; index < length; ++index)
        keys->setIndex(vm, index, elements.at(list, index).get());
    kept.keys = keys;
}

void PropertyListCache::clear()
{
    m_writes.forEachRegionToClear([&](size_t, size_t) {
        if (m_entries) {
            for (auto& listing : *m_entries)
                listing = { };
        }
        if (m_ownKeyLists)
            m_ownKeyLists->fill({ });
        for (auto& cacheEntry : std::span { m_state.instance.objects.keyLists, numberOfKeyLists }) {
            if (cacheEntry.orderWord & orderWordHasExtraProperties)
                cacheEntry = { };
        }
    });
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
