/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTObject.h"

#if ENABLE(AOT)

#include "AOTClassPrototype.h"
#include "AOTHeapCacheRegistry.h"
#include "AOTInliningAcrossFiles.h"
#include "AOTInstance.h"
#include "AOTKeyOrderTable.h"
#include "AOTObjectInternal.h"
#include "AOTObjectModel.h"
#include "AOTObjectModelCaches.h"
#include "AOTPendingStars.h"
#include "AOTPropertyCaches.h"
#include "AOTRuntimeReporter.h"
#include "AOTState.h"
#include "AOTVector.h"
#include "DeferGC.h"
#include "JSArray.h"
#include "JSCInlines.h"
#include "JSCellButterfly.h"
#include "JSPropertyNameEnumeratorInlines.h"
#include "PropertyTable.h"
#include "StructureChain.h"
#include "VMInlines.h"
#include <algorithm>
#include <wtf/HashFunctions.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/StringCommon.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

std::span<const uint16_t> Object::keyOrder(State& state) const
{
    const LayoutData& layout = this->layout();
    uint32_t word = orderWord();
    if (isOrderStoredOutOfLine(word)) {
        std::span<const uint16_t> held = compiledStructure().outOfLineKeyOrder();
        RELEASE_ASSERT(countOf(word) <= held.size());
        return countOf(word) ? held.first(countOf(word)) : held;
    }
    return state.objects->keyOrders.entriesListedBy(layout, word);
}

bool Object::isInKeyOrder(State& state, const FieldData& field) const
{
    auto keys = keyOrder(state);
    uint16_t index = indexOf(field);
    if (index < keys.size() && keys[index] == index)
        return true;
    return WTF::find16(keys.data(), index, keys.size());
}

static uint32_t numberOfExtraKeysCountedByOrder(State& state, const Object& object)
{
    const LayoutData& layout = object.layout();
    if (object.hasKeyOrderOfItsOwn())
        return 0;
    ExtraKeyCounter positioned(layout.numberOfFields);
    positioned.addAll(object.keyOrder(state));
    return positioned.count();
}

bool Object::keyOrderCountsExtraKeys() const
{
    return numberOfExtraKeysCountedByOrder(state(), *this);
}

using CountsOfPositionedKeys = Vector<uint32_t, PlacesOfPositionedKeys::maxNumberOfCounts>;

static CountsOfPositionedKeys countPositionedExtraKeys(State& state, const Object& object)
{
    CountsOfPositionedKeys counts;
    const LayoutData& layout = object.layout();
    if (object.hasKeyOrderOfItsOwn())
        return counts;
    ExtraKeyCounter positioned(layout.numberOfFields);
    auto noteCount = [&] {
        if (positioned.count() && (counts.isEmpty() || counts.last() != positioned.count()))
            counts.append(positioned.count());
    };
    object.forEachKeyInOrder(state, [&](uint16_t key) {
        if (key >= layout.numberOfFields)
            positioned.add(key);
        else
            noteCount();
    });
    noteCount();
    return counts;
}

static bool arePlacesOfPositionedKeysOf(const PlacesOfPositionedKeys& places, const Object& object, const PropertyTable& table, const CountsOfPositionedKeys& counts)
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

static std::optional<uint32_t> findOrdinalOfExtraKeyByWalk(State& state, const Object& object, const PropertyTable& table, unsigned entry, uint32_t numberOfCountedKeys)
{
    uint32_t ordinal = 0;
    for (unsigned earlier = 1; earlier < entry && ordinal < numberOfCountedKeys; ++earlier) {
        UniquedStringImpl* key = table.keyOfEntry(earlier);
        ordinal += key && isExtraKey(state, object, key);
    }
    if (ordinal >= numberOfCountedKeys)
        return std::nullopt;
    return ordinal;
}

static std::optional<uint32_t> ordinalOfCountedExtraKey(State& state, const Object& object, JSObject& extra, PropertyName name, const CountsOfPositionedKeys& counts, PlacesOfPositionedKeys& places)
{
    PropertyTable& table = *extra.structure()->propertyTableInOrderOfAddition(object.vm());
    unsigned entry = table.find(name.uid()).entryIndex;
    if (entry == PropertyTable::EmptyEntryIndex)
        return std::nullopt;
    bool isExtra = isExtraKey(state, object, name.uid());
    if (counts.size() > PlacesOfPositionedKeys::maxNumberOfCounts)
        return isExtra ? findOrdinalOfExtraKeyByWalk(state, object, table, entry, counts.last()) : std::nullopt;
    if (!arePlacesOfPositionedKeysOf(places, object, table, counts) && !tryFindPlacesOfPositionedKeys(state, object, table, counts, places)) {
        places = { };
        return isExtra ? findOrdinalOfExtraKeyByWalk(state, object, table, entry, counts.last()) : std::nullopt;
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
    ASSERT(!hasKeyOrderOfItsOwn());
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
    countEvent(vm, RuntimeCounterID::KeyOrdersStoredOutOfLine);
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

bool Object::tryListFieldBehindNamedKey(State& state, const FieldData& field)
{
    bool namesKeys = hasKeyOrderOfItsOwn();
    JSObject* extra = extraProperties();
    if (!namesKeys && !(extra && extra->structure()->isDictionary()))
        return false;
    if (field.flags & fieldMayExistWithoutStorage && isInKeyOrder(state, field))
        return true;
    VM& vm = this->vm();
    RefPtr<UniquedStringImpl> lastKey;
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
    structure.appendToKeyOrderOfOneObject(lastKey.get(), static_cast<uint16_t>(layout().numberOfFields + namedKeys.size()), indexOf(field));
    return true;
}

Object::PlaceOfExtraKey Object::placeOfExtraKey(PropertyName name)
{
    JSObject* extra = extraProperties();
    if (!extra)
        return { };
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
            return { ordinalOfCountedExtraKey(state, *this, *extra, name, counts, places), std::nullopt, nullptr, true };
        takeKeyOrderOfItsOwn(vm(), state);
    }
    std::span<const RefPtr<UniquedStringImpl>> namedKeys = compiledStructure().keysNamedByOrder();
    for (uint32_t index = 0; index < namedKeys.size(); ++index) {
        if (namedKeys[index] != name.uid())
            continue;
        PropertyTable& table = *extra->structure()->propertyTableInOrderOfAddition(vm());
        unsigned entry = table.find(name.uid()).entryIndex;
        RELEASE_ASSERT(entry != PropertyTable::EmptyEntryIndex);
        return { std::nullopt, index, lastExtraKeyUpTo(state, *this, table, entry - 1) };
    }
    return { };
}

void Object::didRemoveExtraKey(VM& vm, State& state, const PlaceOfExtraKey& place)
{
    if (place.indexAmongNamedKeys) {
        uint32_t removed = *place.indexAmongNamedKeys;
        const CompiledStructure& structure = compiledStructure();
        if (place.extraKeyInFront && (!removed || structure.keysNamedByOrder()[removed - 1] != place.extraKeyInFront)) {
            structure.replaceKeyNamedByOrder(removed, place.extraKeyInFront);
            return;
        }
        rebuildKeyOrderOfOneObject(*this, [](uint16_t) {
            return true;
        }, [&](size_t index, UniquedStringImpl* key) {
            return index == removed ? nullptr : key;
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
    auto isExtraKeyAt = [&](unsigned entry) {
        UniquedStringImpl* key = table->keyOfEntry(entry);
        return key && isExtraKey(state, *this, key);
    };
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

static void storeKeyOrderOutOfLine(VM& vm, State& state, Object& object, std::span<const uint16_t> keys, StructureTable::HeldKeyOrder heldKeyOrder = StructureTable::HeldKeyOrder::IsFoundByOthers)
{
    object.JSCell::setStructure(vm, state.objects->structures.withOutOfLineKeyOrder(vm, object.compiledStructure(), keys, heldKeyOrder));
    object.setOrderAndCount(outOfLineOrderWordFor(keys.size()));
    countEvent(vm, RuntimeCounterID::KeyOrdersStoredOutOfLine);
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

static void appendEntriesToUnnumberedKeyOrder(VM& vm, State& state, Object& object, std::span<const uint16_t> added)
{
    if (tryStepInOutOfLineKeyOrder(object, added))
        return;
    Vector<uint16_t, 32> entries(object.keyOrder(state));
    entries.append(added);
    if (!trySearchForHeldKeyOrder(vm, state, object, entries.span()))
        storeKeyOrderOutOfLine(vm, state, object, entries.span(), StructureTable::HeldKeyOrder::IsFoundByNobody);
}

static ALWAYS_INLINE bool sharesStepsOfKeyOrder(const Object& object)
{
    return !object.hasFlagsInOrderWord() && !object.hasKeyOrderOfItsOwn();
}

static bool tryRepeatStepOfKeyOrder(VM& vm, State& state, Object& object, uint16_t field, StructureTable::KindOfStep kind)
{
    const StructureTable::StepOfKeyOrder* step = state.objects->structures.findStepOfKeyOrder(object.structureID(), object.orderWord() & orderAndCountMask, field, kind);
    if (!step)
        return false;
    uint32_t orderAndCountAfter = step->orderAndCountAfter;
    if (step->after != object.structureID()) {
        auto* after = uncheckedDowncast<CompiledStructure>(step->after.decode());
        if (after->hasOutOfLineKeyOrder())
            after->didFindKeyOrderAgain();
        object.JSCell::setStructure(vm, after);
    }
    object.setOrderAndCount(orderAndCountAfter);
    return true;
}

static void rememberStepOfKeyOrder(State& state, const Object& object, StructureID before, uint32_t orderAndCountBefore, uint16_t field, StructureTable::KindOfStep kind)
{
    if (sharesStepsOfKeyOrder(object))
        state.objects->structures.rememberStepOfKeyOrder(before, orderAndCountBefore, field, kind, object.structureID(), object.orderWord() & orderAndCountMask);
}

bool Object::tryRepeatCreationAtSlot(State& state, uint32_t slotKey)
{
    return sharesStepsOfKeyOrder(*this) && tryRepeatStepOfKeyOrder(vm(), state, *this, static_cast<uint16_t>(slotKey), StructureTable::KindOfStep::CreatesAtSlot);
}

void Object::rememberCreationAtSlot(State& state, StructureID before, uint32_t orderAndCountBefore, uint32_t slotKey) const
{
    rememberStepOfKeyOrder(state, *this, before, orderAndCountBefore, static_cast<uint16_t>(slotKey), StructureTable::KindOfStep::CreatesAtSlot);
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

using EntriesAddedToKeyOrder = Vector<uint16_t, 4>;

static EntriesAddedToKeyOrder entriesCountingExtraKeys(State& state, const Object& object, std::optional<uint32_t> numberOfKeysOfStars)
{
    EntriesAddedToKeyOrder entries;
    JSObject* extra = numberOfKeysOfStars ? nullptr : object.extraProperties();
    if (!extra && !numberOfKeysOfStars)
        return entries;
    const LayoutData& layout = object.layout();
    uint32_t numberOfExtraKeys = extra ? countExtraKeys(state, object, extra) : *numberOfKeysOfStars;
    if (!numberOfExtraKeys)
        return entries;
    if (!object.hasOutOfLineKeyOrder() && state.objects->keyOrders.positionsExtraKeys(layout, object.orderWord() & orderAndCountMask, numberOfExtraKeys))
        return entries;
    ExtraKeyCounter positioned(layout.numberOfFields);
    positioned.addAll(object.keyOrder(state));
    positioned.appendEntriesCountingTo(numberOfExtraKeys, entries);
    return entries;
}

static std::optional<uint32_t> numberOfExtraKeysByStructures(State& state, const Object& object)
{
    if (std::optional<uint32_t> number = object.numberOfExtraKeysByStructure())
        return number;
    JSObject* extra = object.orderWord() & orderWordHasFieldOutsideSlot ? nullptr : extraPropertiesIfCreated(object);
    Structure* structure = extra ? extra->structure() : nullptr;
    if (!structure || structure->isDictionary() || !structure->isQuickPropertyAccessAllowedForEnumeration())
        return std::nullopt;
    return countExtraKeys(state, object, extra);
}

void Object::willCreateField(State&, const FieldData& field)
{
    if (field.isOutOfLine())
        ensureStorageFor(vm(), field);
}

void Object::didCreateField(State& state, const FieldData& field, bool withStorage)
{
    if (isListedOnlyByKeyOrder(field.slot.representation()) && isInKeyOrder(state, field)) [[unlikely]]
        return;
    if (withStorage)
        willCreateField(state, field);
    const LayoutData& layout = this->layout();
    if (layout.isClass())
        state.objects->classes.didShadowMember(layout, field);
    KeyOrderTable& keyOrders = state.objects->keyOrders;
    uint16_t index = indexOf(field);
    uint32_t before = orderWord() & orderAndCountMask;
    std::optional<uint32_t> numberOfExtraKeys = numberOfExtraKeysByStructures(state, *this);
    bool isKnownNotToBeListed = !(field.flags & fieldMayExistWithoutStorage);
    if (numberOfExtraKeys && !hasOutOfLineKeyOrder()) {
        std::optional<uint32_t> after = *numberOfExtraKeys ? std::nullopt : keyOrders.stepInsideOrderOfImage(layout, before, index);
        if (!after)
            after = keyOrders.publishedTransition(layout, before, field, *numberOfExtraKeys);
        if (!after && isKnownNotToBeListed)
            after = keyOrders.tryAppendToAddedOrder(layout, before, field, *numberOfExtraKeys);
        if (after) {
            setOrderAndCount(*after);
            return;
        }
    }
    bool sharesStep = isKnownNotToBeListed && sharesStepsOfKeyOrder(*this);
    StructureID structureBefore = structureID();
    if (sharesStep && (tryStepInOutOfLineKeyOrder(*this, singleElementSpan(index)) || tryRepeatStepOfKeyOrder(vm(), state, *this, index, StructureTable::KindOfStep::Creates)))
        return;
    std::optional<uint32_t> numberOfKeysOfStars = numberOfKeysOfPendingStars(vm(), *this);
    if (!numberOfKeysOfStars && tryListFieldBehindNamedKey(state, field))
        return;
    EntriesAddedToKeyOrder added = entriesCountingExtraKeys(state, *this, numberOfKeysOfStars);
    numberOfExtraKeys = numberOfExtraKeysByStructure();
    if (field.flags & fieldMayExistWithoutStorage && isInKeyOrder(state, field)) {
        if (!hasOutOfLineKeyOrder() && numberOfExtraKeys)
            keyOrders.publishTransition(layout, before, field, before, *numberOfExtraKeys);
        return;
    }
    added.append(index);
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
    if (sharesStep)
        rememberStepOfKeyOrder(state, *this, structureBefore, before, index, StructureTable::KindOfStep::Creates);
}

ALWAYS_INLINE_INTO_OTHER_FILES bool Object::trySetNumberedKeyOrder(VM& vm, State& state, std::span<const uint16_t> keys)
{
    const LayoutData& layout = this->layout();
    ASSERT(!hasKeyOrderOfItsOwn());
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
    uint32_t numberOfFields = layout().numberOfFields;
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
    object.forgetPlacesOfPositionedKeys(state);
    uint16_t index = object.indexOf(field);
    bool sharesStep = sharesStepsOfKeyOrder(object);
    if (sharesStep && tryRepeatStepOfKeyOrder(vm, state, object, index, StructureTable::KindOfStep::Removes))
        return;
    StructureID structureBefore = object.structureID();
    uint32_t orderAndCountBefore = object.orderWord() & orderAndCountMask;
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
    if (object.trySetNumberedKeyOrder(vm, state, remaining.span()) || trySearchForHeldKeyOrder(vm, state, object, remaining.span())) {
        if (sharesStep)
            rememberStepOfKeyOrder(state, object, structureBefore, orderAndCountBefore, index, StructureTable::KindOfStep::Removes);
        return;
    }
    if (hasPendingStars(object)) {
        storeKeyOrderOutOfLine(vm, state, object, remaining.span());
        return;
    }
    object.takeKeyOrderOfItsOwn(vm, state);
    removeFromOrderOfItsOwn();
}

static bool hasNumberedKeyOrder(const Object& object)
{
    return !object.hasOutOfLineKeyOrder();
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

NEVER_INLINE void writeJSONKeys(VM& vm, CachedPropertyList& list)
{
    constexpr size_t sizeAroundName = 4;
    size_t size = FieldAsProperty::jsonKeyBlockSize;
    bool areWrittenAsTheyAre = true;
    for (const PropertyListEntry& property : list.properties) {
        list.hasToJSON |= property.name == vm.propertyNames->toJSON.impl();
        areWrittenAsTheyAre = areWrittenAsTheyAre && property.name->is8Bit() && std::ranges::all_of(property.name->span8(), [](Latin1Character character) {
            return character >= 0x20 && character < 0x7F && character != '"' && character != '\\';
        });
        size += property.name->length() + sizeAroundName;
    }
    if (!areWrittenAsTheyAre || size > UINT32_MAX)
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
        *end++ = static_cast<uint32_t>(next - start);
    }
}

CachedPropertyList* findCachedPropertyList(const Object& object, JSObject* extra)
{
    if (!hasNumberedKeyOrder(object))
        return nullptr;
    return object.state().objects->caches->propertyLists.find(extra->structureID(), object.shapeWord(), object.orderWord() & orderAndCountMask);
}

CachedPropertyList* cachedPropertyList(VM& vm, const Object& object)
{
    JSObject* extra = object.extraProperties();
    if (!extra || !hasNumberedKeyOrder(object))
        return nullptr;
    State& state = object.state();
    const LayoutData& layout = object.layout();
    PropertyListCache& propertyLists = state.objects->caches->propertyLists;
    Structure* structure = extra->structure();
    uint32_t shapeWord = object.shapeWord();
    uint32_t orderAndCount = object.orderWord() & orderAndCountMask;
    if (CachedPropertyList* found = propertyLists.find(structure->id(), shapeWord, orderAndCount)) {
        for (const PropertyListEntry& property : found->properties) {
            if (property.field && !holdsValue(object, *property.field))
                return nullptr;
        }
        return found;
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
    std::span<const uint16_t> keyOrder = object.keyOrder(state);
    Vector<PropertyListEntry> properties;
    properties.reserveInitialCapacity(keyOrder.size() + extraKeys.size());
    bool hasAllFields = true;
    size_t nextExtraKey = Object::forEachPositionedKey(keyOrder, layout.numberOfFields, extraKeys.size(), [&](uint16_t index) {
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
    countEvent(vm, RuntimeCounterID::PropertyListsCreated);
    return &propertyLists.add({ structure->id(), shapeWord, orderAndCount, WTF::move(properties) });
}

const Vector<PropertyListEntry>* Object::fieldsAndExtraPropertiesInKeyOrder(VM& vm) const
{
    CachedPropertyList* list = cachedPropertyList(vm, *this);
    return list ? &list->properties : nullptr;
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
    CachedPropertyList* list = cachedPropertyList(vm, *this);
    if (!list || list->keys)
        return list ? list->keys : nullptr;
    JSCellButterfly* keys = JSCellButterfly::tryCreate(vm, CopyOnWriteArrayWithContiguous, static_cast<unsigned>(list->properties.size()));
    if (!keys)
        return nullptr;
    for (unsigned index = 0; index < list->properties.size(); ++index)
        keys->setIndex(vm, index, jsOwnedString(vm, String(list->properties[index].name)));
    list->keys = keys;
    return keys;
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
    for (unsigned index = 0; index < length; ++index)
        values.at(list, index).setWithoutWriteBarrier(keys->get(index));
    vm.mutatorFence();
    return list;
}

static bool enumerableNamesFollowFromStructureAndOrderWord(VM&, const Object&);

enum class HiddenKeys : bool { Refused, LeftOut };

static JSCellButterfly* keysOfObjectWithExtraProperties(VM& vm, const Object& object, HiddenKeys hiddenKeys)
{
    State& state = object.state();
    uint32_t orderWord = object.orderWord();
    uint32_t structureID = object.structureID().bits();
    static_assert(MarkedBlock::atomSize >= 4);
    uint32_t structureWord = hiddenKeys == HiddenKeys::LeftOut ? structureEnumerableKeysWord(structureID) : keyListWordOfStructure(structureID);
    KeyListCacheEntry& cacheEntry = state.instance.objects.keyLists[indexOfKeyList(structureWord, orderWord)];
    if (cacheEntry.shapeWord == structureWord && cacheEntry.orderWord == orderWord)
        return uncheckedDowncast<JSCellButterfly>(cacheEntry.list);
    if (hasPendingStars(object) || !enumerableNamesFollowFromStructureAndOrderWord(vm, object))
        return nullptr;
    if (JSCellButterfly* list = object.keysWithExtraProperties(vm)) {
        cacheEntry = { orderWord, structureWord, list };
        return list;
    }
    Vector<UniquedStringImpl*, 32> names;
    forEachOwnKey(vm, state, object, KeysListed::Strings, DontEnumPropertiesMode::Exclude, [&](UniquedStringImpl* name) {
        names.append(name);
    });
    if (hiddenKeys == HiddenKeys::Refused) {
        size_t numberOfOwnKeys = 0;
        forEachOwnKey(vm, state, object, KeysListed::StringsAndSymbols, DontEnumPropertiesMode::Include, [&](UniquedStringImpl*) {
            ++numberOfOwnKeys;
        });
        if (numberOfOwnKeys != names.size())
            return nullptr;
    }
    DeferGC deferGC(vm);
    JSCellButterfly* list = JSCellButterfly::tryCreate(vm, CopyOnWriteArrayWithContiguous, static_cast<unsigned>(names.size()));
    if (!list)
        return nullptr;
    for (unsigned index = 0; index < names.size(); ++index)
        list->setIndex(vm, index, jsOwnedString(vm, String(names[index])));
    cacheEntry = { orderWord, structureWord, list };
    return list;
}

static NEVER_INLINE JSCellButterfly* keysOfSharedOrderWithoutNumber(VM& vm, const Object& object, KeyListKind kind)
{
    if (object.hasExtraProperties() || object.hasKeyOrderOfItsOwn())
        return nullptr;
    State& state = object.state();
    StructureTable& structures = state.objects->structures;
    uint32_t count = countOf(object.orderWord());
    if (JSCellButterfly* known = structures.findKeysOfOrderWithoutNumber(object.structureID(), count, kind))
        return known;
    JSCellButterfly* list = state.objects->keyOrders.tryCreateKeyList(vm, object.layout(), object.keyOrder(state), kind);
    if (list)
        structures.rememberKeysOfOrderWithoutNumber(object.structureID(), count, kind, list);
    return list;
}

JSCellButterfly* Object::keys(VM& vm, KeyListKind kind) const
{
    if (!hasNumberedKeyOrder(*this))
        return keysOfSharedOrderWithoutNumber(vm, *this, kind);
    if (hasExtraProperties())
        return kind == KeyListKind::Symbols ? nullptr : keysOfObjectWithExtraProperties(vm, *this, HiddenKeys::Refused);
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

static bool enumerableNamesFollowFromStructureAndOrderWord(VM& vm, const Object& object)
{
    if (!object.keysDependOnlyOnStructureAndOrderWord())
        return false;
    if (!object.hasFlagsInOrderWord() && !object.bearsMark()) [[likely]]
        return true;
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

JSCellButterfly* Object::enumerableKeys(VM& vm) const
{
    if (!hasNumberedKeyOrder(*this) || !hasExtraProperties())
        return keys(vm);
    return keysOfObjectWithExtraProperties(vm, *this, HiddenKeys::LeftOut);
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

static CachedPropertyList* listOfKeysFixedByStructure(VM& vm, const Object& object)
{
    JSObject* extra = object.extraProperties();
    if (!extra || !object.keysDependOnlyOnStructureAndOrderWord() || hasIndexedProperties(extra->indexingType()))
        return nullptr;
    return cachedPropertyList(vm, object);
}

static JSPropertyNameEnumerator* cachedEnumeratorOfObjectWithExtraProperties(VM& vm, const Object& object, StructureChain* prototypeChain)
{
    DeferGC deferGC(vm);
    CachedPropertyList* list = listOfKeysFixedByStructure(vm, object);
    if (!list || !list->enumerator || list->prototypeChain != prototypeChain)
        return nullptr;
    if (list->enumerator != vm.emptyPropertyNameEnumerator() && list->enumerator->cachedStructureID() != object.structureID())
        return nullptr;
    return list->enumerator;
}

static void cacheEnumeratorOfObjectWithExtraProperties(VM& vm, const Object& object, JSPropertyNameEnumerator* enumerator, StructureChain* prototypeChain)
{
    DeferGC deferGC(vm);
    CachedPropertyList* list = listOfKeysFixedByStructure(vm, object);
    if (!list)
        return;
    list->enumerator = enumerator;
    list->prototypeChain = prototypeChain;
}

static ALWAYS_INLINE JSObject* prototypeOrNull(const JSObject* object)
{
    JSValue prototype = object->getPrototypeDirect();
    return prototype.isCell() ? asObject(prototype.asCell()) : nullptr;
}

static bool describePrototypes(VM& vm, JSObject* base, EnumeratorBelowCompiledObjects& described)
{
    bool isBelowCompiledObject = false;
    for (JSObject* prototype = prototypeOrNull(base); prototype; prototype = prototypeOrNull(prototype)) {
        Object* compiled = compiledObjectOrNull(prototype);
        Structure* structure = prototype->structure();
        bool namesAreFixed = compiled ? enumerableNamesFollowFromStructureAndOrderWord(vm, *compiled) : structure->canCacheOwnPropertyNames() && !structure->hasPolyProto();
        if (!namesAreFixed || described.numberOfPrototypes == EnumeratorBelowCompiledObjects::mostPrototypes)
            return false;
        isBelowCompiledObject |= !!compiled;
        described.prototypes[described.numberOfPrototypes++] = { structure->id().bits(), compiled ? compiled->orderWord() : 0 };
    }
    return isBelowCompiledObject;
}

static ALWAYS_INLINE bool hasDescribedPrototypes(VM& vm, JSObject* base, const EnumeratorBelowCompiledObjects& described)
{
    uint32_t place = 0;
    for (JSObject* prototype = prototypeOrNull(base); prototype; prototype = prototypeOrNull(prototype), ++place) {
        Object* compiled = compiledObjectOrNull(prototype);
        if (place == described.numberOfPrototypes || described.prototypes[place] != EnumeratorBelowCompiledObjects::Prototype { prototype->structureID().bits(), compiled ? compiled->orderWord() : 0 })
            return false;
        if (compiled && !enumerableNamesFollowFromStructureAndOrderWord(vm, *compiled))
            return false;
    }
    return place == described.numberOfPrototypes;
}

static bool describesNames(VM& vm, JSObject* base, const EnumeratorBelowCompiledObjects& described)
{
    if (described.structureID != base->structureID().bits())
        return false;
    Object* compiled = compiledObjectOrNull(base);
    if (compiled && (described.orderWord != compiled->orderWord() || !enumerableNamesFollowFromStructureAndOrderWord(vm, *compiled)))
        return false;
    return hasDescribedPrototypes(vm, base, described);
}

static bool isEveryNameEnumerable(JSGlobalObject* globalObject, JSObject* base, JSPropertyNameEnumerator* enumerator)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    for (uint32_t index = 0; index < enumerator->sizeOfPropertyNames(); ++index) {
        JSString* name = enumerator->propertyNameAtIndex(index);
        if (!name)
            return false;
        auto identifier = name->toAtomString(globalObject);
        RETURN_IF_EXCEPTION(scope, false);
        bool isEnumerable = base->hasEnumerableProperty(globalObject, identifier.data);
        RETURN_IF_EXCEPTION(scope, false);
        if (!isEnumerable)
            return false;
    }
    return true;
}

static ALWAYS_INLINE bool hasDescribedNames(JSObject* object, uint32_t structureID, uint32_t orderWord)
{
    if (object->structureID().bits() != structureID)
        return false;
    Object* compiled = compiledObjectOrNull(object);
    if (!compiled)
        return true;
    JSObject* extra = compiled->extraProperties();
    return compiled->orderWord() == orderWord && (!extra || compiled->compiledStructure().extraPropertiesStructureID() == extra->structureID());
}

bool Object::areEnumeratedNamesFixed(JSGlobalObject* globalObject, JSObject* base, const JSPropertyNameEnumerator* enumerator)
{
    Instance* instance = Instance::of(globalObject);
    if (!instance || !instance->state || !instance->state->objects)
        return false;
    const EnumeratorBelowCompiledObjects& described = instance->state->objects->keyOrders.enumeratorBelowCompiledObjects(base->structureID().bits());
    if (described.enumerator != enumerator || !described.everyNameIsEnumerable || !hasDescribedNames(base, described.structureID, described.orderWord))
        return false;
    uint32_t place = 0;
    for (JSObject* prototype = prototypeOrNull(base); prototype; prototype = prototypeOrNull(prototype), ++place) {
        if (place == described.numberOfPrototypes || !hasDescribedNames(prototype, described.prototypes[place].structureID, described.prototypes[place].orderWord))
            return false;
    }
    return place == described.numberOfPrototypes;
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
    bool isCacheable = ownNamesAreFixed && (hasExtraProperties || !compiledStructure().hasOtherPrototype() || getPrototypeDirect().isNull());
    StructureChain* prototypeChainBefore = nullptr;
    if (isCacheable) {
        prototypeChainBefore = structureBefore->prototypeChain(vm, globalObject, this);
        if (JSPropertyNameEnumerator* cached = hasExtraProperties ? cachedEnumeratorOfObjectWithExtraProperties(vm, *this, prototypeChainBefore) : keyOrders.enumerator(layout, orderWordBefore, prototypeChainBefore)) {
            keyOrders.publishEnumerator(*this, cached);
            return cached;
        }
    }
    EnumeratorBelowCompiledObjects& place = keyOrders.enumeratorBelowCompiledObjects(structureBefore->id().bits());
    if (ownNamesAreFixed && describesNames(vm, this, place)) {
        countEvent(vm, RuntimeCounterID::CompiledObjectEnumeratorsFound);
        return place.enumerator;
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

    bool isCachedByChain = false;
    bool sawPolyProto;
    if (isCacheable && isUnchanged && normalizePrototypeChain(globalObject, this, sawPolyProto) != InvalidPrototypeChain) {
        if (structureBefore->prototypeChain(vm, globalObject, this) == prototypeChainBefore && canCacheEnumeratorFor(prototypeChainBefore)) {
            if (!hasExtraProperties)
                keyOrders.setEnumerator(vm, layout, orderWordBefore, enumerator, prototypeChainBefore);
            else
                cacheEnumeratorOfObjectWithExtraProperties(vm, *this, enumerator, prototypeChainBefore);
            keyOrders.publishEnumerator(*this, enumerator);
            isCachedByChain = true;
        }
    }
    EnumeratorBelowCompiledObjects described { structureBefore->id().bits(), orderWordBefore, 0, false, enumerator, { } };
    if (!isCachedByChain && ownNamesAreFixed && isUnchanged && structure() == structureBefore && orderWord() == orderWordBefore && describePrototypes(vm, this, described)) {
        described.everyNameIsEnumerable = isEveryNameEnumerable(globalObject, this, enumerator);
        RETURN_IF_EXCEPTION(scope, nullptr);
        place = described;
    }
    return enumerator;
}

JSPropertyNameEnumerator* Object::enumeratorOfFinalObject(JSGlobalObject* globalObject, JSObject* base)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    ASSERT(base->type() == FinalObjectType);
    Structure* structure = base->structure();
    Instance* instance = Instance::of(globalObject);
    if (!instance || !instance->state || !instance->state->objects || structure->hasPolyProto() || !structure->canCacheOwnPropertyNames())
        RELEASE_AND_RETURN(scope, JSC::propertyNameEnumerator(globalObject, base));
    EnumeratorBelowCompiledObjects& place = instance->state->objects->keyOrders.enumeratorBelowCompiledObjects(structure->id().bits());
    if (describesNames(vm, base, place)) {
        countEvent(vm, RuntimeCounterID::CompiledObjectEnumeratorsFound);
        return place.enumerator;
    }
    JSPropertyNameEnumerator* enumerator = JSC::propertyNameEnumerator(globalObject, base);
    RETURN_IF_EXCEPTION(scope, nullptr);
    EnumeratorBelowCompiledObjects described { structure->id().bits(), 0, 0, false, enumerator, { } };
    if (base->structure() == structure && describePrototypes(vm, base, described)) {
        described.everyNameIsEnumerable = isEveryNameEnumerable(globalObject, base, enumerator);
        RETURN_IF_EXCEPTION(scope, nullptr);
        place = described;
    }
    return enumerator;
}

PropertyListCache::PropertyListCache(State& state)
    : m_entries(state.tableArena)
    , m_ownKeyLists(state.tableArena)
{
    state.caches->add(CacheKind::ClearedEachCollection, "property lists"_s, *this);
}

CachedPropertyList& PropertyListCache::add(CachedPropertyList&& list)
{
    if (!m_entries)
        m_entries.make();
    m_writes.recordAll();
    CachedPropertyList& place = (*m_entries)[placeOf(list.extraPropertiesStructureID, list.shapeWord, list.orderAndCount)];
    place = WTF::move(list);
    return place;
}

bool PropertyListCache::isSecondWalk(StructureID extraPropertiesStructureID, uint32_t shapeWord, uint32_t orderAndCount)
{
    if (m_walksWithoutList.testAndSet(WTF::intHash(static_cast<uint64_t>(extraPropertiesStructureID.bits()) << 32 | (shapeWord ^ std::rotl(orderAndCount, 16))) % numberOfWalksRemembered))
        return true;
    if (++m_numberOfWalksWithoutList > numberOfEntries) {
        m_walksWithoutList.clearAll();
        m_numberOfWalksWithoutList = 0;
    }
    return false;
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

static bool ownKeysFollowFromStructureAndOrderWord(const Object& object)
{
    JSObject* extra = object.extraProperties();
    if (!extra)
        return !object.hasExtraProperties() && object.keysDependOnlyOnStructureAndOrderWord();
    return object.keysDependOnlyOnStructureAndOrderWord() && object.compiledStructure().extraPropertiesStructureID() == extra->structureID();
}

JSCellButterfly* findKeptOwnKeys(const Object& object, bool listsSymbols)
{
    if (!ownKeysFollowFromStructureAndOrderWord(object))
        return nullptr;
    const KeptOwnKeys& kept = object.state().objects->caches->propertyLists.ownKeysFor(object.structureID(), object.orderWord(), listsSymbols);
    return kept.isFor(object.structureID(), object.orderWord(), listsSymbols) ? kept.keys : nullptr;
}

void didListOwnKeys(VM& vm, const Object& object, bool listsSymbols, JSArray* list)
{
    if (!ownKeysFollowFromStructureAndOrderWord(object))
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
            for (CachedPropertyList& list : *m_entries)
                list = { };
        }
        if (m_ownKeyLists)
            m_ownKeyLists->fill({ });
    });
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
