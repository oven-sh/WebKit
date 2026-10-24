/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTObject.h"

#if ENABLE(AOT)

#include "AOTKeyOrderTable.h"
#include "AOTObjectInternal.h"
#include "AOTObjectModel.h"
#include "AOTState.h"
#include "AOTVector.h"
#include "JSCInlines.h"
#include "PropertyTable.h"
#include <algorithm>
#include <wtf/StdLibExtras.h>
#include <wtf/text/StringCommon.h>

namespace JSC { namespace AOT {

static bool isPresent(VM& vm, const Object& object, const FieldData& field)
{
    return object.isInSlot(field) || (object.hasExtraProperties() && livesInExtraProperties(vm, object, field));
}

static bool isExtraKey(State& state, const Object& object, UniquedStringImpl* name)
{
    return state.objects->layouts.hasPositionAmongKeys(object.layout(), name) && !object.fieldNamed(state, name);
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
    if (hasOutOfLineKeyOrder())
        return compiledStructure().outOfLineKeyOrder();
    uint32_t word = orderWord();
    return state.objects->keyOrders.entriesInOrder(layout, orderOf(word)).first(countOf(word));
}

bool Object::isInKeyOrder(State& state, const FieldData& field) const
{
    if (layout().keysAreNeverEnumerated)
        return isPresent(vm(), *this, field);
    KeyOrderStorage storage;
    auto keys = keyOrder(state, storage);
    uint16_t index = indexOf(field);
    if (index < keys.size() && keys[index] == index)
        return true;
    return WTF::find16(keys.data(), index, keys.size());
}

bool Object::hasPositionAmongKeys(UniquedStringImpl* name) const
{
    return state().objects->layouts.hasPositionAmongKeys(layout(), name);
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

// How many extra keys stand in front of each place of the order at which a field follows extra keys, ascending. An object that is used as a dictionary positions none.
static CountsOfPositionedKeys countPositionedExtraKeys(State& state, const Object& object)
{
    CountsOfPositionedKeys counts;
    const LayoutData& layout = object.layout();
    if (layout.keysAreNeverEnumerated)
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
    // No order is written with entries that no field follows. One that has them all the same loses them with the first key that they count.
    if (positioned.count() && (counts.isEmpty() || counts.last() != positioned.count()))
        counts.append(positioned.count());
    return counts;
}

static bool describes(const PlacesOfPositionedKeys& places, const Object& object, const PropertyTable& table, const CountsOfPositionedKeys& counts)
{
    return places.object == &object && places.table == &table && places.structureID == object.structureID().bits() && places.orderWord == object.orderWord()
        && places.numberOfRemovedEntries == table.numberOfRemovedEntries() && std::ranges::equal(std::span { places.counts }.first(places.numberOfCounts), counts);
}

// One walk over the keys that are positioned. Whoever removes key after key pays for it once: didRemoveExtraKey() keeps the places right.
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

std::optional<uint32_t> Object::extraKeyOrdinal(PropertyName name) const
{
    JSObject* extra = extraProperties();
    if (!extra)
        return std::nullopt;
    State& state = this->state();
    PlacesOfPositionedKeys& places = state.objects->keyOrders.placesOfPositionedKeys(*this);
    if (places.object == this)
        places.entryOfKeyAskedFor = 0;
    CountsOfPositionedKeys counts = countPositionedExtraKeys(state, *this);
    if (counts.isEmpty())
        return std::nullopt;
    PropertyTable& table = *extra->structure()->propertyTableInOrderOfAddition(vm());
    unsigned entry = table.find(name.uid()).entryIndex;
    if (entry == PropertyTable::EmptyEntryIndex)
        return std::nullopt;
    bool isExtra = isExtraKey(state, *this, name.uid());

    // After so many fields that were created behind extra keys nothing is remembered.
    if (counts.size() > PlacesOfPositionedKeys::maxNumberOfCounts)
        return isExtra ? findOrdinalByWalk(state, *this, table, entry, counts.last()) : std::nullopt;
    if (!describes(places, *this, table, counts) && !tryFindPlacesOfPositionedKeys(state, *this, table, counts, places)) {
        places = { };
        return isExtra ? findOrdinalByWalk(state, *this, table, entry, counts.last()) : std::nullopt;
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

void Object::forgetPlacesOfPositionedKeys(State& state)
{
    PlacesOfPositionedKeys& places = state.objects->keyOrders.placesOfPositionedKeys(*this);
    if (places.object == this)
        places = { };
}

void Object::didRemoveExtraKey(State& state)
{
    PlacesOfPositionedKeys& places = state.objects->keyOrders.placesOfPositionedKeys(*this);
    if (places.object != this)
        return;
    uint32_t removed = std::exchange(places.entryOfKeyAskedFor, 0);
    JSObject* extra = extraProperties();
    const PropertyTable* table = extra ? extra->structure()->propertyTableInOrderOfAddition(vm()) : nullptr;
    // The table is another one, or it was rehashed when it lost the key.
    if (!removed || table != places.table || table->numberOfRemovedEntries() != places.numberOfRemovedEntries + 1) {
        places = { };
        return;
    }
    for (uint32_t index = 0; places.keyAskedForIsExtraKey && index < places.numberOfCounts; ++index) {
        if (removed > places.lastEntries[index])
            continue;
        uint32_t countInFront = index ? places.counts[index - 1] : 0;
        // The entry counted this key alone on top of the one in front of it, and is gone from the order.
        if (--places.counts[index] == countInFront) {
            places = { };
            return;
        }
        if (removed != places.lastEntries[index])
            continue;
        // The entry still counts a key, which stands in front of this one.
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
    return InitialKeyOrder { outOfLineOrderWord, structures.withOutOfLineKeyOrder(vm, *structure, keys) };
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
        const RuntimeLayoutTable& layouts = state.objects->layouts;
        const LayoutData& layout = object.layout();
        extra->structure()->forEachProperty(object.vm(), [&](const auto& property) {
            ASSERT(mayHoldFields || !object.fieldNamed(state, property.key()));
            count += mayHoldFields ? isExtraKey(state, object, property.key()) : layouts.hasPositionAmongKeys(layout, property.key());
            return true;
        });
        return count;
    };
    // While no field lies outside its slot, no extra property has the name of a field, and nobody has to look each of them up.
    bool mayHoldFields = object.orderWord() & orderWordHasFieldOutsideSlot;
    uint32_t numberOfExtraKeys;
    // A Structure allows quick access for enumeration until it gets a symbol or a property that is not enumerable, or loses a property.
    if (Structure* extraStructure = extra->structure(); !mayHoldFields && extraStructure->isQuickPropertyAccessAllowedForEnumeration()) {
        numberOfExtraKeys = static_cast<uint32_t>(numberOfSlotsForMaxOffset(extraStructure->maxOffset(), extraStructure->inlineCapacity()));
        ASSERT(numberOfExtraKeys == countByWalk(false));
    } else
        numberOfExtraKeys = countByWalk(mayHoldFields);
    if (isFixedByStructure)
        structure.setNumberOfExtraKeys(numberOfExtraKeys);
    return numberOfExtraKeys;
}

// Nothing where only a walk over the extra properties can tell. countExtraKeys() leaves the number with a Structure that fixes it.
std::optional<uint32_t> Object::numberOfExtraKeysByStructure() const
{
    if (!hasExtraProperties())
        return 0;
    return compiledStructure().numberOfExtraKeys();
}

using AddedEntries = Vector<uint16_t, 4>;

static AddedEntries entriesPositioningExtraKeys(State& state, const Object& object)
{
    AddedEntries entries;
    JSObject* extra = object.extraProperties();
    if (!extra)
        return entries;
    const LayoutData& layout = object.layout();
    uint32_t numberOfExtraKeys = countExtraKeys(state, object, extra);
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

static void appendEntries(VM& vm, State& state, Object& object, std::span<const uint16_t> added)
{
    Object::KeyOrderStorage storage;
    Vector<uint16_t, 32> entries(object.keyOrder(state, storage));
    entries.append(added);
    object.setKeyOrder(vm, state, entries.span());
}

void Object::willCreateField(State&, const FieldData& field)
{
    if (field.isOutOfLine())
        ensureStorageFor(vm(), field);
}

void Object::didCreateField(State& state, const FieldData& field, bool withStorage)
{
    if (withStorage)
        willCreateField(state, field);
    const LayoutData& layout = this->layout();
    if (layout.keysAreNeverEnumerated)
        return;
    KeyOrderTable& keyOrders = state.objects->keyOrders;
    uint32_t before = orderWord() & orderAndCountMask;
    std::optional<uint32_t> numberOfExtraKeys = numberOfExtraKeysByStructure();
    if (numberOfExtraKeys && !hasOutOfLineKeyOrder()) {
        if (std::optional<uint32_t> after = keyOrders.publishedTransition(layout, before, field, *numberOfExtraKeys)) {
            setOrderAndCount(*after);
            return;
        }
    }
    AddedEntries added = entriesPositioningExtraKeys(state, *this);
    numberOfExtraKeys = numberOfExtraKeysByStructure();
    if (field.flags & fieldMayExistWithoutStorage && isInKeyOrder(state, field)) {
        if (!hasOutOfLineKeyOrder() && numberOfExtraKeys)
            keyOrders.publishTransition(layout, before, field, before, *numberOfExtraKeys);
        return;
    }
    added.append(indexOf(field));
    if (!hasOutOfLineKeyOrder()) {
        std::optional<uint32_t> after = before;
        // The one step of an object without extra keys is published by whoever makes it. No such object ever stands where one with extra keys does.
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
    appendEntries(vm(), state, *this, added.span());
}

void Object::appendField(State& state, const FieldData& field)
{
    willCreateField(state, field);
    const LayoutData& layout = this->layout();
    if (layout.keysAreNeverEnumerated)
        return;
    uint16_t index = indexOf(field);
    if (!hasOutOfLineKeyOrder()) {
        if (auto orderAndCount = state.objects->keyOrders.orderAfterCreating(layout, orderWord() & orderAndCountMask, index)) {
            setOrderAndCount(*orderAndCount);
            return;
        }
    }
    appendEntries(vm(), state, *this, singleElementSpan(index));
}

void Object::setKeyOrder(VM& vm, State& state, std::span<const uint16_t> keys)
{
    const LayoutData& layout = this->layout();
    ASSERT(!layout.keysAreNeverEnumerated);
    StructureTable& structures = state.objects->structures;
    if (auto numbered = state.objects->keyOrders.orderStartingWith(layout, keys)) {
        bool wasOutOfLine = hasOutOfLineKeyOrder();
        setOrderAndCount(*numbered);
        if (wasOutOfLine)
            JSCell::setStructure(vm, structures.withNumberedKeyOrder(vm, compiledStructure()));
        return;
    }
    JSCell::setStructure(vm, structures.withOutOfLineKeyOrder(vm, compiledStructure(), keys));
    setOrderAndCount(outOfLineOrderWord);
    count(instance(), RuntimeCounterID::KeyOrdersStoredOutOfLine);
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
    // Of the entries in a row that count extra keys only the sum matters, and only where a field follows. Written anew each time, the row stays as short as its sum allows.
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
    uint32_t numberOfFields = object.layout().numberOfFields;
    auto countsExtraKeys = [&](uint16_t key) {
        return key >= numberOfFields;
    };
    Vector<uint16_t, 16> remaining;
    object.forEachKeyInOrder(state, [&](uint16_t key) {
        if (key == index)
            return;
        // Of two entries in a row that count extra keys the larger says all that the other says. Only on top of a full one does the next one count.
        if (countsExtraKeys(key) && !remaining.isEmpty() && countsExtraKeys(remaining.last()) && remaining.last() != maxKeyOrderEntry)
            remaining.last() = std::max(remaining.last(), key);
        else
            remaining.append(key);
    });
    // The extra keys that no field follows stand at the end without an entry. So an order does not grow by an entry each time a field goes and comes again behind one more key.
    while (!remaining.isEmpty() && countsExtraKeys(remaining.last()))
        remaining.removeLast();
    object.setKeyOrder(vm, state, remaining.span());
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
