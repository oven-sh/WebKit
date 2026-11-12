/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkState.h"

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include "AOTInliningAcrossFiles.h"
#include "AOTSlotPlan.h"
#include <bit>
#include <optional>
#include <wtf/HashFunctions.h>
#include <wtf/MathExtras.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr AbstractHeapSet heapsUnderElementsInWalkState { AbstractHeap::Elements, AbstractHeap::ArrayStorage, AbstractHeap::TypedArrayElements, AbstractHeap::TypedArrayStorage };
static constexpr AbstractHeapSet heapsUnderElementsOutsideViewsInWalkState { AbstractHeap::Elements, AbstractHeap::ArrayStorage };
static constexpr AbstractHeapSet heapsUnderPlacesInWalkState = heapsUnderElementsInWalkState | AbstractHeapSet { AbstractHeap::ObjectFields, AbstractHeap::EnvironmentFields, AbstractHeap::ModuleVariables, AbstractHeap::MemberGuards };
static constexpr AbstractHeapSet heapsUnderElementsOutsideObjectsInWalkState = heapsUnderElementsInWalkState - perObjectHeaps;
static_assert(perObjectHeaps.containsAll(heapsUnderPlacesInWalkState - heapsUnderElementsOutsideObjectsInWalkState - AbstractHeapSet { AbstractHeap::ModuleVariables, AbstractHeap::MemberGuards }));
static_assert(perObjectHeaps.contains(AbstractHeap::ShapeWords));
static_assert(perObjectHeaps.contains(AbstractHeap::NamedProperties) && perObjectHeaps.contains(AbstractHeap::OrderWords) && perObjectHeaps.contains(AbstractHeap::PrototypeLinks));

OptionSet<CellFact> cellFactsInvalidatedBy(const LocationMasks& writes)
{
    OptionSet<CellFact> ended;
    if (writes.contains(AbstractHeap::ShapeWords))
        ended.add({ CellFact::NotFrozen, CellFact::IsExtensible, CellFact::OrderWordIsKnown, CellFact::IsLikelyAsAllocated, CellFact::IsLikelyAsConstructed, CellFact::EnumeratorIsCurrent, CellFact::BearsNoMark });
    if (writes.contains(AbstractHeap::NamedProperties))
        ended.add({ CellFact::NoFieldOutsideSlot, CellFact::NoExtraProperties, CellFact::OrderWordIsKnown, CellFact::IsLikelyAsAllocated, CellFact::IsLikelyAsConstructed, CellFact::EnumeratorIsCurrent, CellFact::BearsNoMark });
    if (writes.contains(AbstractHeap::OrderWords))
        ended.add({ CellFact::OrderWordIsKnown, CellFact::IsLikelyAsAllocated, CellFact::IsLikelyAsConstructed, CellFact::EnumeratorIsCurrent });
    if (writes.contains(AbstractHeap::PrototypeLinks))
        ended.add(CellFact::HasLayoutPrototype);
    return ended;
}

static unsigned hashOfHolderInWalkState(Reference object)
{
    return WTF::intHash(object.bits());
}

static unsigned hashOfPlaceInWalkState(uint32_t holder, unsigned kind, uint32_t first, uint32_t second)
{
    return WTF::pairIntHash(WTF::pairIntHash(holder, kind), WTF::pairIntHash(first, second));
}

uint32_t WalkState::Held::findInTable(const Table& table, unsigned hash, NOESCAPE const Invocable<bool(uint32_t)> auto& isSought)
{
    uint32_t mask = table.size() - 1;
    for (uint32_t entry = hash & mask;; entry = (entry + 1) & mask) {
        uint32_t position = table[entry];
        if (position == none || isSought(position))
            return position;
    }
}

void WalkState::Held::addToTable(Table& table, unsigned hash, uint32_t position)
{
    uint32_t mask = table.size() - 1;
    uint32_t entry = hash & mask;
    while (table[entry] != none)
        entry = (entry + 1) & mask;
    table.mutableAt(entry) = position;
}

struct WalkState::Held::Lists {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(Lists);

    static constexpr uint32_t notListed = none - 1;

    static constexpr uint32_t listsOfOneHeap = Place::entireHeap + 1;
    static constexpr uint32_t firstListOfVariables = 0;
    static constexpr uint32_t firstListOfSlots = firstListOfVariables + listsOfOneHeap;
    static constexpr uint32_t firstListOfEnvironmentFields = firstListOfSlots + listsOfOneHeap;
    static constexpr uint32_t listOfGuards = firstListOfEnvironmentFields + listsOfOneHeap;
    static constexpr uint32_t listsByLayout = 64;
    static constexpr uint32_t firstListByLayout = listOfGuards + 1;
    static constexpr uint32_t firstLayoutOfList = firstListByLayout + listsByLayout;
    static constexpr uint32_t severalLayouts = notListed;
    static constexpr uint32_t firstListByLayoutAlone = firstLayoutOfList + listsByLayout;
    static constexpr uint32_t firstLayoutOfListAlone = firstListByLayoutAlone + listsByLayout;
    static constexpr uint32_t firstListOfAnchors = firstLayoutOfListAlone + listsByLayout;
    static constexpr uint32_t headsOfPlaces = firstListOfAnchors + listsOfOneHeap;
    static constexpr uint32_t entriesOfPlace = 3;
    static constexpr uint32_t entryByBit = 0;
    static constexpr uint32_t entryByLayout = 1;
    static constexpr uint32_t entryInChain = 2;
    static constexpr uint32_t standsInChain = notListed - 1;

    static constexpr uint32_t listOfShapes = numberOfCellFacts;
    static constexpr uint32_t listOfElements = listOfShapes + 1;
    static constexpr uint32_t listsOfHolders = listOfElements + 1;
    static constexpr uint32_t listOfHoldersNotAsked = listsOfHolders;
    static constexpr uint32_t listOfWideHolders = listOfHoldersNotAsked + 1;
    static constexpr uint32_t headsOfHolders = listOfWideHolders + 1;
    static constexpr uint32_t entryByClass = listsOfHolders;
    static constexpr uint32_t entriesOfHolder = listsOfHolders + 1;
    static constexpr uint32_t wordsOfChainOfHolders = 3;
    static constexpr uint32_t wordOfLayout = 0;
    static constexpr uint32_t wordOfNewCells = 1;
    static constexpr uint32_t wordOfFirstHolder = 2;

    static uint32_t listOf(CellFact fact)
    {
        uint32_t list = std::countr_zero(static_cast<OptionSet<CellFact>::StorageType>(fact));
        RELEASE_ASSERT(list < numberOfCellFacts);
        return list;
    }

    static bool add(Table& table, uint32_t list, uint32_t entry, uint32_t record)
    {
        if (entry >= table.size())
            table.grow(notListed, entry + 1);
        else if (table[entry] != notListed)
            return false;
        uint32_t next = std::exchange(table.mutableAt(list), record);
        table.mutableAt(entry) = next;
        return true;
    }
    static void goThrough(Table& table, uint32_t list, uint32_t entryOfFirstRecord, uint32_t entriesOfRecord, NOESCAPE const Invocable<bool(uint32_t record)> auto& stays)
    {
        uint32_t entryOfPrevious = list;
        for (uint32_t record = table[list]; record != none;) {
            uint32_t entry = entryOfFirstRecord + record * entriesOfRecord;
            uint32_t next = table[entry];
            if (stays(record))
                entryOfPrevious = entry;
            else {
                table.mutableAt(entry) = notListed;
                table.mutableAt(entryOfPrevious) = next;
            }
            record = next;
        }
    }

    bool addPlace(uint32_t list, uint32_t whichEntry, uint32_t place) { return add(places, list, headsOfPlaces + place * entriesOfPlace + whichEntry, place); }
    static uint32_t entryOfHolder(uint32_t holder, uint32_t whichEntry) { return headsOfHolders + holder * entriesOfHolder + whichEntry; }
    void addHolderByClass(uint32_t list, uint32_t holder) { add(holders, list, entryOfHolder(holder, entryByClass), holder); }
    void addHolder(uint32_t list, uint32_t holder)
    {
        add(holders, list, entryOfHolder(holder, list), holder);
        addHolderByClass(listOfHoldersNotAsked, holder);
    }
    void goThroughPlaces(uint32_t list, uint32_t whichEntry, NOESCAPE const Invocable<bool(uint32_t place)> auto& stays) { goThrough(places, list, headsOfPlaces + whichEntry, entriesOfPlace, stays); }
    void goThroughHolders(uint32_t list, NOESCAPE const Invocable<bool(uint32_t holder)> auto& stays) { goThrough(holders, list, headsOfHolders + list, entriesOfHolder, stays); }
    void goThroughPlacesUnder(uint32_t firstList, uint64_t written, NOESCAPE const Invocable<bool(uint32_t place)> auto& stays)
    {
        if (!written)
            return;
        for (uint64_t bits = written; bits; bits &= bits - 1)
            goThroughPlaces(firstList + std::countr_zero(bits), entryByBit, stays);
        goThroughPlaces(firstList + Place::entireHeap, entryByBit, stays);
    }

    void goThroughHoldersByClass(uint32_t list, NOESCAPE const Invocable<bool(uint32_t holder)> auto& stays) { goThrough(holders, list, headsOfHolders + entryByClass, entriesOfHolder, stays); }

    static uint32_t entryOf(uint32_t place, uint32_t whichEntry) { return headsOfPlaces + place * entriesOfPlace + whichEntry; }
    static unsigned hashOfChain(uint32_t layout, uint32_t bit) { return WTF::pairIntHash(layout, bit); }

    uint32_t findAnchor(const auto& records, uint32_t layout, uint32_t bit) const
    {
        if (!numberOfAnchors)
            return none;
        return findInTable(anchors, hashOfChain(layout, bit), [&](uint32_t position) {
            return records[position].first == layout && records[position].bit == bit;
        });
    }
    void sortOut(const auto& records, uint32_t place)
    {
        if (entryOf(place, entryInChain) >= places.size())
            places.grow(notListed, entryOf(place, entryInChain) + 1);
        ASSERT(places[entryOf(place, entryByBit)] == notListed && places[entryOf(place, entryInChain)] == notListed);
        uint32_t layout = records[place].first;
        uint32_t bit = records[place].bit;
        uint32_t anchor = findAnchor(records, layout, bit);
        if (anchor != none) {
            places.mutableAt(entryOf(place, entryByBit)) = standsInChain;
            places.mutableAt(entryOf(place, entryInChain)) = std::exchange(places.mutableAt(entryOf(anchor, entryInChain)), place);
            return;
        }
        addPlace(firstListOfAnchors + bit, entryByBit, place);
        places.mutableAt(entryOf(place, entryInChain)) = none;
        ++numberOfAnchors;
        if (numberOfAnchors * 2 <= anchors.size()) {
            addToTable(anchors, hashOfChain(layout, bit), place);
            return;
        }
        anchors.fill(none, roundUpToPowerOfTwo(numberOfAnchors * 4));
        for (uint32_t list = 0; list < listsOfOneHeap; ++list) {
            for (uint32_t listed = places[firstListOfAnchors + list]; listed != none; listed = places[entryOf(listed, entryByBit)])
                addToTable(anchors, hashOfChain(records[listed].first, records[listed].bit), listed);
        }
    }
    void goThroughChain(uint32_t anchor, NOESCAPE const Invocable<bool(uint32_t place)> auto& stays)
    {
        stays(anchor);
        uint32_t entryOfPrevious = entryOf(anchor, entryInChain);
        for (uint32_t place = places[entryOfPrevious]; place != none;) {
            uint32_t entry = entryOf(place, entryInChain);
            uint32_t next = places[entry];
            if (stays(place))
                entryOfPrevious = entry;
            else {
                places.mutableAt(entry) = notListed;
                places.mutableAt(entryOf(place, entryByBit)) = notListed;
                places.mutableAt(entryOfPrevious) = next;
            }
            place = next;
        }
    }
    void goThroughChainsOf(uint32_t bit, NOESCAPE const Invocable<bool(uint32_t place)> auto& stays)
    {
        for (uint32_t anchor = places[firstListOfAnchors + bit]; anchor != none; anchor = places[entryOf(anchor, entryByBit)])
            goThroughChain(anchor, stays);
    }

    uint32_t findChainOfHolders(ClassOfHolder sought) const
    {
        if (!numberOfChainsOfHolders)
            return none;
        return findInTable(tableOfChainsOfHolders, hashOfChain(sought.layoutAlone.index(), sought.isNewCellItself), [&](uint32_t chain) {
            return chainsOfHolders[chain * wordsOfChainOfHolders + wordOfLayout] == sought.layoutAlone.index() && chainsOfHolders[chain * wordsOfChainOfHolders + wordOfNewCells] == sought.isNewCellItself;
        });
    }
    void addToChainOfHolders(ClassOfHolder classOfHolder, uint32_t holder)
    {
        ASSERT(holders[entryOfHolder(holder, entryByClass)] == notListed);
        uint32_t chain = findChainOfHolders(classOfHolder);
        if (chain == none) {
            chain = numberOfChainsOfHolders++;
            chainsOfHolders.grow(none, numberOfChainsOfHolders * wordsOfChainOfHolders);
            chainsOfHolders.mutableAt(chain * wordsOfChainOfHolders + wordOfLayout) = classOfHolder.layoutAlone.index();
            chainsOfHolders.mutableAt(chain * wordsOfChainOfHolders + wordOfNewCells) = classOfHolder.isNewCellItself;
            if (numberOfChainsOfHolders * 2 <= tableOfChainsOfHolders.size())
                addToTable(tableOfChainsOfHolders, hashOfChain(classOfHolder.layoutAlone.index(), classOfHolder.isNewCellItself), chain);
            else {
                tableOfChainsOfHolders.fill(none, roundUpToPowerOfTwo(numberOfChainsOfHolders * 4));
                for (uint32_t listed = 0; listed < numberOfChainsOfHolders; ++listed)
                    addToTable(tableOfChainsOfHolders, hashOfChain(chainsOfHolders[listed * wordsOfChainOfHolders + wordOfLayout], chainsOfHolders[listed * wordsOfChainOfHolders + wordOfNewCells]), listed);
            }
        }
        holders.mutableAt(entryOfHolder(holder, entryByClass)) = std::exchange(chainsOfHolders.mutableAt(chain * wordsOfChainOfHolders + wordOfFirstHolder), holder);
    }
    void goThroughChainOfHolders(uint32_t chain, NOESCAPE const Invocable<bool(uint32_t holder)> auto& stays)
    {
        uint32_t previous = none;
        for (uint32_t holder = chainsOfHolders[chain * wordsOfChainOfHolders + wordOfFirstHolder]; holder != none;) {
            uint32_t entry = entryOfHolder(holder, entryByClass);
            uint32_t next = holders[entry];
            if (stays(holder))
                previous = holder;
            else {
                holders.mutableAt(entry) = notListed;
                if (previous == none)
                    chainsOfHolders.mutableAt(chain * wordsOfChainOfHolders + wordOfFirstHolder) = next;
                else
                    holders.mutableAt(entryOfHolder(previous, entryByClass)) = next;
            }
            holder = next;
        }
    }

    Table places;
    Table holders;
    Table anchors;
    Table chainsOfHolders;
    Table tableOfChainsOfHolders;
    uint32_t numberOfAnchors { 0 };
    uint32_t numberOfChainsOfHolders { 0 };
};

WalkState::Held::Held(const Held& other)
    : m_lists(other.m_lists ? new Lists(*other.m_lists) : nullptr)
    , m_recordsGoneThrough(other.m_recordsGoneThrough)
    , m_firstFreshHolderWithElements(other.m_firstFreshHolderWithElements)
    , m_numberOfHolders(other.m_numberOfHolders)
    , m_numberOfPlaces(other.m_numberOfPlaces)
    , m_numberOfShapes(other.m_numberOfShapes)
    , m_numberOfFactHolders(other.m_numberOfFactHolders)
    , m_numberOfFreshHolders(other.m_numberOfFreshHolders)
    , m_numberOfShapesReachedByEffects(other.m_numberOfShapesReachedByEffects)
    , m_reachedFactHolderCount(other.m_reachedFactHolderCount)
    , m_numberOfPlacesReachedByEffects(other.m_numberOfPlacesReachedByEffects)
    , m_numberOfElementsReachedByEffects(other.m_numberOfElementsReachedByEffects)
    , m_cellFactsReachedByEffects(other.m_cellFactsReachedByEffects)
    , m_kindsOfPlacesReachedByEffects(other.m_kindsOfPlacesReachedByEffects)
    , m_maskOfPlacesReachedByEffects(other.m_maskOfPlacesReachedByEffects)
    , m_filterOfHolders(other.m_filterOfHolders)
    , m_checkedNames(other.m_checkedNames)
    , m_filterOfValuesWithCheckedNames(other.m_filterOfValuesWithCheckedNames)
    , m_bindingsPastDeadZone(other.m_bindingsPastDeadZone)
    , m_builtinsRead(other.m_builtinsRead)
    , m_tableOfHolders(other.m_tableOfHolders)
    , m_tableOfPlaces(other.m_tableOfPlaces)
    , m_holdersReachedByEffects(other.m_holdersReachedByEffects)
    , m_holders(other.m_holders)
    , m_places(other.m_places)
    , m_newestStoreIntoUnmadeRecord(other.m_newestStoreIntoUnmadeRecord)
{
}

void WalkState::takeHeldOfOwn()
{
    if (!m_held) {
        m_held = new Held;
        return;
    }
    if (m_held->m_holdersReachedByEffects.size() > holdersFoundWithoutTable && !m_held->m_lists) [[unlikely]] {
        m_held->m_recordsGoneThrough += m_held->m_holdersReachedByEffects.size();
        m_held->makeListsIfDue();
    }
    --m_held->numberOfOwners;
    m_held = new Held(*m_held);
}

ALWAYS_INLINE_INTO_OTHER_FILES void WalkState::shareWhatIsHeldBy(const WalkState& other)
{
    if (other.holdsNothing()) {
        if (m_held)
            dropWhatIsHeld();
        return;
    }
    ++other.m_held->numberOfOwners;
    release(m_held);
    m_held = other.m_held;
}

Reference WalkState::checkedNameHeldFrom(uint32_t place) const
{
    for (; place; place = m_program.numbers->checkedName(place - 1).older) {
        if (m_held->m_checkedNames.contains(place - 1))
            return m_program.numbers->checkedName(place - 1).name;
    }
    return { };
}

void WalkState::nameAnew(Reference value, Reference name)
{
    RELEASE_ASSERT(m_program.numbers);
    Held& held = heldToWrite();
    if (hasCheckedNames() && (held.m_filterOfValuesWithCheckedNames & bitOfValueWithCheckedName(value))) {
        for (uint32_t place = m_program.numbers->newestCheckedNameOf(value); place; place = m_program.numbers->checkedName(place - 1).older) {
            if (!held.m_checkedNames.contains(place - 1))
                continue;
            held.m_checkedNames.remove(place - 1);
            break;
        }
    }
    uint32_t number = m_program.numbers->addCheckedName(value, name);
    m_program.numbers->rememberNumberThatNames(name, number);
    held.m_checkedNames.add(number);
    held.m_filterOfValuesWithCheckedNames |= bitOfValueWithCheckedName(value);
    m_marks.kinds |= Kinds::checkedNames;
}

NEVER_INLINE void WalkState::takeMarksOfStateWithoutWholeProgram(WalkState& other)
{
    Marks marks = m_marks;
    m_program = other.m_program;
    m_marks = other.m_marks;
    noteWhetherProgramIsWhole();
    marks.kinds &= ~programIsWhole;
    other.m_marks = marks;
}

void WalkState::dropWhatIsHeld()
{
    m_marks.kinds &= keptWhereNothingIsHeld;
    if (m_held->numberOfOwners == 1) {
        m_held->clear();
        return;
    }
    --m_held->numberOfOwners;
    m_held = nullptr;
}

void WalkState::dropAllRecords()
{
    m_marks.kinds &= ~Kinds::records;
    if (m_held->numberOfOwners == 1) {
        m_held->dropAllRecords();
        return;
    }
    Held* shared = m_held;
    --shared->numberOfOwners;
    m_held = nullptr;
    if (shared->holdsNothingThatNoEffectEnds())
        return;
    m_held = new Held;
    m_held->m_checkedNames = shared->m_checkedNames;
    m_held->m_filterOfValuesWithCheckedNames = shared->m_filterOfValuesWithCheckedNames;
    m_held->m_bindingsPastDeadZone = shared->m_bindingsPastDeadZone;
    m_held->m_builtinsRead = shared->m_builtinsRead;
}

void WalkState::endWhatIsWritten(const Effects& effects, const MemoryLocation* writtenAlone)
{
    if (effects.mayRunOtherCode()) {
        endWhatIsNotFresh();
        return;
    }
    if (writesInPlace() || m_held->restsOnHeapsAmong(effects.writes)) {
        heldToWrite().endWhatIsWritten(m_program, effects, writtenAlone);
        noteKindsOfRecordsLeft();
    }
}

void WalkState::endWhatIsNotFresh()
{
    if (!m_held->m_numberOfFreshHolders) {
        dropAllRecords();
        return;
    }
    heldToWrite().endWhatIsNotFresh();
    noteKindsOfRecordsLeft();
}

uint8_t WalkState::Held::bitOf(const ProgramOfState& program, const MemoryLocation& location)
{
    if (!program.program)
        return Place::entireHeap;
    uint64_t mask = Typed::maskOf(*program.program, location);
    return std::has_single_bit(mask) ? static_cast<uint8_t>(std::countr_zero(mask)) : Place::entireHeap;
}

PropertyKeyID WalkState::Held::keyOfSlot(const ProgramOfState& program, FieldReference field)
{
    if (!program.program || !field.layout)
        return { };
    const Layout& layout = program.program->at(field.layout);
    if (field.field < layout.fields.size())
        return layout.fields[field.field].key;
    return program.slots ? program.slots->keyOf(*program.program, field) : PropertyKeyID();
}

ALWAYS_INLINE_INTO_OTHER_FILES uint8_t WalkState::Held::bitOfSlot(const ProgramOfState& program, FieldReference field)
{
    PropertyKeyID key = keyOfSlot(program, field);
    return key ? bitOf(program, MemoryLocation::objectField(key)) : Place::entireHeap;
}

bool WalkState::Held::liesBesideWhatIsWritten(const ProgramOfState& program, const Place& place, uint64_t written, const MemoryLocation* writtenAlone)
{
    if (!writtenAlone || written != place.mask())
        return false;
    switch (place.kind) {
    case PlaceKind::Slot: {
        PropertyKeyID key = writtenAlone->heap() == AbstractHeap::ObjectFields ? keyOfSlot(program, FieldReference { LayoutID(place.first), place.second }) : PropertyKeyID();
        if (!key || key == writtenAlone->key())
            return false;
        const TypeTable& types = program.program->types;
        return (types.hasStableNumber(key) && types.hasStableNumber(writtenAlone->key())) || types.compareByContent(key, writtenAlone->key()) != 0;
    }
    case PlaceKind::Variable:
        return writtenAlone->heap() == AbstractHeap::ModuleVariables && place.first != writtenAlone->variable().index();
    case PlaceKind::EnvironmentField:
        return writtenAlone->heap() == AbstractHeap::EnvironmentFields && (place.first != writtenAlone->environmentLayout().index() || place.second != writtenAlone->environmentFieldIndex());
    case PlaceKind::Element:
    case PlaceKind::GuardOfStores:
    case PlaceKind::GuardOfReads:
        break;
    }
    return false;
}

NEVER_INLINE uint32_t WalkState::Held::findRecordOfHolderInTable(Reference object) const
{
    return findInTable(m_tableOfHolders, hashOfHolderInWalkState(object), [&](uint32_t position) {
        return m_holders[position].object == object;
    });
}

NEVER_INLINE auto WalkState::Held::findRecordOfHolderThatHasNotEndedInTable(Reference object) const -> const Holder*
{
    const Holder* found = nullptr;
    findInTable(m_tableOfHolders, hashOfHolderInWalkState(object), [&](uint32_t position) {
        const Holder& record = m_holders[position];
        if (record.object != object)
            return false;
        found = &record;
        return true;
    });
    return found && !found->hasEnded ? found : nullptr;
}

ALWAYS_INLINE_INTO_OTHER_FILES uint32_t WalkState::Held::findRecordOfHolder(Reference object) const
{
    if (!mayHaveRecordOf(object))
        return none;
    if (!m_tableOfHolders.isEmpty())
        return findRecordOfHolderInTable(object);
    for (uint32_t position = m_holders.size(); position--;) {
        if (m_holders[position].object == object)
            return position;
    }
    return none;
}

ALWAYS_INLINE_INTO_OTHER_FILES uint32_t WalkState::Held::findHolder(Reference object) const
{
    uint32_t position = findRecordOfHolder(object);
    return position != none && !m_holders[position].hasEnded ? position : none;
}

ALWAYS_INLINE_INTO_OTHER_FILES auto WalkState::Held::findRecordOfHolderThatHasNotEnded(Reference object) const -> const Holder*
{
    if (!m_tableOfHolders.isEmpty())
        return findRecordOfHolderThatHasNotEndedInTable(object);
    for (uint32_t position = m_holders.size(); position--;) {
        if (const Holder& record = m_holders[position]; record.object == object)
            return record.hasEnded ? nullptr : &record;
    }
    return nullptr;
}

void WalkState::Held::fillTableOfHolders()
{
    m_tableOfHolders.fill(none, roundUpToPowerOfTwo(m_holders.size() * 4));
    for (uint32_t position = 0; position < m_holders.size(); ++position)
        addToTable(m_tableOfHolders, hashOfHolderInWalkState(m_holders[position].object), position);
}

void WalkState::Held::fillTableOfPlaces()
{
    m_tableOfPlaces.fill(none, roundUpToPowerOfTwo(m_places.size() * 4));
    for (uint32_t position = 0; position < m_places.size(); ++position) {
        const Place& place = m_places[position];
        addToTable(m_tableOfPlaces, hashOfPlaceInWalkState(place.holder, static_cast<unsigned>(place.kind), place.first, place.second), position);
    }
}

ALWAYS_INLINE_INTO_OTHER_FILES uint32_t WalkState::Held::findOrAddHolder(Reference object)
{
    uint32_t position = findRecordOfHolder(object);
    if (position != none) {
        if (m_holders[position].hasEnded) {
            Holder& record = m_holders.mutableAt(position);
            ASSERT(record.firstPlace == none && record.firstElement == none && !record.shape && record.cellFacts.isEmpty() && !record.isFresh);
            record.hasEnded = false;
            record.layoutsOfSlots = LayoutsOfSlots::None;
            record.kindsOfPlaces = 0;
            record.maskOfPlaces = 0;
            record.elementsLieOutsideViews = false;
            ++m_numberOfHolders;
            listAsReachedByEffects(position);
        }
        return position;
    }

    position = m_holders.size();
    m_holders.append(Holder { .object = object });
    m_filterOfHolders |= bitInFilterOfHolders(object);
    ++m_numberOfHolders;
    listAsReachedByEffects(position);
    if (m_holders.size() * 2 <= m_tableOfHolders.size())
        addToTable(m_tableOfHolders, hashOfHolderInWalkState(object), position);
    else if (m_holders.size() > holdersFoundWithoutTable)
        fillTableOfHolders();
    return position;
}

NEVER_INLINE uint32_t WalkState::Held::findRecordOfPlaceInTable(uint32_t holder, PlaceKind kind, uint32_t first, uint32_t second) const
{
    return findInTable(m_tableOfPlaces, hashOfPlaceInWalkState(holder, static_cast<unsigned>(kind), first, second), [&](uint32_t position) {
        const Place& place = m_places[position];
        return place.holder == holder && place.kind == kind && place.first == first && place.second == second;
    });
}

ALWAYS_INLINE_INTO_OTHER_FILES uint32_t WalkState::Held::findRecordOfPlace(uint32_t holder, PlaceKind kind, uint32_t first, uint32_t second) const
{
    if (!m_tableOfPlaces.isEmpty())
        return findRecordOfPlaceInTable(holder, kind, first, second);
    for (uint32_t position = m_places.size(); position--;) {
        const Place& place = m_places[position];
        if (place.holder == holder && place.kind == kind && place.first == first && place.second == second)
            return position;
    }
    return none;
}

ALWAYS_INLINE_INTO_OTHER_FILES uint32_t WalkState::Held::findPlace(uint32_t holder, PlaceKind kind, uint32_t first, uint32_t second) const
{
    uint32_t position = findRecordOfPlace(holder, kind, first, second);
    return position != none && m_places[position].value ? position : none;
}

Reference WalkState::Held::valueOfPlace(PlaceKind kind, Reference base, uint32_t first, uint32_t second) const
{
    if (!m_numberOfPlaces)
        return { };
    uint32_t holder = findHolder(base);
    uint32_t position = holder != none ? findPlace(holder, kind, first, second) : none;
    return position != none ? m_places[position].value : Reference();
}

void WalkState::Held::notePlace(PlaceKind kind, Reference base, uint32_t first, uint32_t second, Reference value, NOESCAPE const Invocable<uint8_t()> auto& bit)
{
    uint32_t holder = value ? findOrAddHolder(base) : findHolder(base);
    if (holder == none)
        return;
    uint32_t position = findRecordOfPlace(holder, kind, first, second);
    bool hasRecord = position != none;
    if (hasRecord && m_places[position].value) {
        if (!value)
            endPlace(position);
        else if (m_places[position].value != value)
            m_places.mutableAt(position).value = value;
        return;
    }
    if (!value)
        return;

    Holder& record = m_holders.mutableAt(holder);
    uint32_t& firstOfList = kind == PlaceKind::Element ? record.firstElement : record.firstPlace;
    uint64_t mask;
    if (hasRecord) {
        Place& place = m_places.mutableAt(position);
        place.value = value;
        if (!std::exchange(place.isListed, true))
            place.next = std::exchange(firstOfList, position);
        mask = place.mask();
    } else {
        position = m_places.size();
        Place place { kind, bit(), true, holder, first, second, value, firstOfList };
        m_places.append(place);
        firstOfList = position;
        mask = place.mask();
    }
    ++m_numberOfPlaces;
    if (kind != PlaceKind::Element) {
        record.kindsOfPlaces |= 1 << static_cast<unsigned>(kind);
        record.maskOfPlaces |= mask;
        ++record.numberOfPlaces;
        if (!record.isFresh) {
            ++m_numberOfPlacesReachedByEffects;
            m_kindsOfPlacesReachedByEffects |= 1 << static_cast<unsigned>(kind);
            m_maskOfPlacesReachedByEffects |= mask;
        }
    } else {
        ++record.numberOfElements;
        if (!record.isFresh)
            ++m_numberOfElementsReachedByEffects;
    }
    if (kind == PlaceKind::Slot) {
        if (record.layoutsOfSlots == LayoutsOfSlots::None) {
            record.layoutsOfSlots = LayoutsOfSlots::One;
            record.layoutOfSlots = first;
        } else if (record.layoutOfSlots != first)
            record.layoutsOfSlots = LayoutsOfSlots::Several;
    } else if (kind == PlaceKind::Element && record.isFresh)
        listAsFreshWithElements(holder);
    if (m_lists && !record.isFresh) [[unlikely]] {
        if (kind == PlaceKind::Element)
            m_lists->addHolder(Lists::listOfElements, holder);
        else
            listPlace(position);
    }

    if (hasRecord)
        return;
    if (m_places.size() * 2 <= m_tableOfPlaces.size())
        addToTable(m_tableOfPlaces, hashOfPlaceInWalkState(holder, static_cast<unsigned>(kind), first, second), position);
    else if (m_places.size() > placesFoundWithoutTable)
        fillTableOfPlaces();
}

void WalkState::Held::endPlace(uint32_t place)
{
    ASSERT(m_places[place].value);
    Place& ended = m_places.mutableAt(place);
    ended.value = { };
    --m_numberOfPlaces;
    Holder& holder = m_holders.mutableAt(ended.holder);
    bool isElement = ended.kind == PlaceKind::Element;
    --(isElement ? holder.numberOfElements : holder.numberOfPlaces);
    if (!holder.isFresh)
        --(isElement ? m_numberOfElementsReachedByEffects : m_numberOfPlacesReachedByEffects);
}

void WalkState::Held::endPlacesOf(uint32_t holder, ListOfPlaces list, NOESCAPE const Invocable<bool(const Place&)> auto& ends)
{
    bool isListOfElements = list == ListOfPlaces::Elements;
    uint32_t previous = none;
    for (uint32_t position = isListOfElements ? m_holders[holder].firstElement : m_holders[holder].firstPlace; position != none;) {
        const Place& place = m_places[position];
        uint32_t next = place.next;
        bool hasEnded = !place.value;
        if (!hasEnded && ends(place)) {
            endPlace(position);
            hasEnded = true;
        }
        if (!hasEnded) {
            previous = position;
            position = next;
            continue;
        }
        m_places.mutableAt(position).isListed = false;
        if (previous != none)
            m_places.mutableAt(previous).next = next;
        else if (isListOfElements)
            m_holders.mutableAt(holder).firstElement = next;
        else
            m_holders.mutableAt(holder).firstPlace = next;
        position = next;
    }
}

void WalkState::Held::endSlotsUnderOtherLayouts(uint32_t holder, uint32_t layout)
{
    const Holder& record = m_holders[holder];
    if (record.layoutsOfSlots == LayoutsOfSlots::None || (record.layoutsOfSlots == LayoutsOfSlots::One && record.layoutOfSlots == layout))
        return;
    endPlacesOf(holder, ListOfPlaces::AllButElements, [&](const Place& place) {
        return place.kind == PlaceKind::Slot && place.first != layout;
    });
    Holder& changed = m_holders.mutableAt(holder);
    changed.layoutsOfSlots = LayoutsOfSlots::One;
    changed.layoutOfSlots = layout;
}

void WalkState::Held::endShape(uint32_t holder)
{
    if (!m_holders[holder].shape)
        return;
    Holder& record = m_holders.mutableAt(holder);
    record.shape = { };
    --m_numberOfShapes;
    if (!record.isFresh)
        --m_numberOfShapesReachedByEffects;
}

void WalkState::Held::invalidateCellFacts(uint32_t holder, OptionSet<CellFact> ended)
{
    OptionSet<CellFact> known = m_holders[holder].cellFacts;
    if (!known.containsAny(ended))
        return;
    Holder& record = m_holders.mutableAt(holder);
    record.cellFacts = known - ended;
    if (!ended.containsAll(known))
        return;
    --m_numberOfFactHolders;
    if (!record.isFresh)
        --m_reachedFactHolderCount;
}

void WalkState::Held::endHolder(uint32_t holder)
{
    ASSERT(!m_holders[holder].hasEnded);
    auto endsAlways = [](const Place&) {
        return true;
    };
    endPlacesOf(holder, ListOfPlaces::AllButElements, endsAlways);
    endPlacesOf(holder, ListOfPlaces::Elements, endsAlways);
    endShape(holder);
    invalidateCellFacts(holder, m_holders[holder].cellFacts);
    if (m_holders[holder].isFresh)
        --m_numberOfFreshHolders;
    else
        takeOffListOfReachedByEffects(holder);
    Holder& record = m_holders.mutableAt(holder);
    record.isFresh = false;
    record.hasEnded = true;
    --m_numberOfHolders;
}

void WalkState::Held::letLeave(uint32_t holder)
{
    ASSERT(m_holders[holder].isFresh);
    m_holders.mutableAt(holder).isFresh = false;
    --m_numberOfFreshHolders;
    listAsReachedByEffects(holder);
    countAmongReachedByEffects(m_holders[holder]);
    if (m_lists) [[unlikely]]
        listWhatHangsOn(holder);
    endHolderIfNothingHangsOn(holder);
}

void WalkState::Held::countAmongReachedByEffects(const Holder& holder)
{
    m_numberOfShapesReachedByEffects += !!holder.shape;
    m_reachedFactHolderCount += !holder.cellFacts.isEmpty();
    m_numberOfPlacesReachedByEffects += holder.numberOfPlaces;
    m_numberOfElementsReachedByEffects += holder.numberOfElements;
    m_cellFactsReachedByEffects.add(holder.cellFacts);
    m_kindsOfPlacesReachedByEffects |= holder.kindsOfPlaces;
    m_maskOfPlacesReachedByEffects |= holder.maskOfPlaces;
}

void WalkState::Held::countNoLongerAmongReachedByEffects(const Holder& holder)
{
    m_numberOfShapesReachedByEffects -= !!holder.shape;
    m_reachedFactHolderCount -= !holder.cellFacts.isEmpty();
    m_numberOfPlacesReachedByEffects -= holder.numberOfPlaces;
    m_numberOfElementsReachedByEffects -= holder.numberOfElements;
}

ALWAYS_INLINE_INTO_OTHER_FILES void WalkState::Held::listAsReachedByEffects(uint32_t holder)
{
    ASSERT(m_holders[holder].placeAmongReachedByEffects == none);
    m_holders.mutableAt(holder).placeAmongReachedByEffects = m_holdersReachedByEffects.size();
    m_holdersReachedByEffects.append(holder);
}

ALWAYS_INLINE_INTO_OTHER_FILES void WalkState::Held::takeOffListOfReachedByEffects(uint32_t holder)
{
    uint32_t place = std::exchange(m_holders.mutableAt(holder).placeAmongReachedByEffects, none);
    uint32_t numberLeft = m_holdersReachedByEffects.size() - 1;
    uint32_t last = m_holdersReachedByEffects[numberLeft];
    m_holdersReachedByEffects.shrink(numberLeft);
    if (last == holder)
        return;
    m_holdersReachedByEffects.mutableAt(place) = last;
    m_holders.mutableAt(last).placeAmongReachedByEffects = place;
}

void WalkState::Held::listAsFreshWithElements(uint32_t holder)
{
    if (m_holders[holder].isListedAsFreshWithElements)
        return;
    Holder& record = m_holders.mutableAt(holder);
    record.isListedAsFreshWithElements = true;
    record.nextFreshWithElements = std::exchange(m_firstFreshHolderWithElements, holder);
}

void WalkState::Held::forEachHolderReachedByEffects(NOESCAPE const Invocable<void(uint32_t holder)> auto& functor)
{
    OptionSet<CellFact> facts;
    uint16_t kindsOfPlaces = 0;
    uint64_t maskOfPlaces = 0;
    for (uint32_t index = 0; index < m_holdersReachedByEffects.size();) {
        uint32_t holder = m_holdersReachedByEffects[index];
        functor(holder);
        const Holder& record = m_holders[holder];
        if (record.shape || !record.cellFacts.isEmpty() || record.firstPlace != none || record.firstElement != none) {
            facts.add(record.cellFacts);
            kindsOfPlaces |= record.kindsOfPlaces;
            maskOfPlaces |= record.maskOfPlaces;
            ++index;
        } else
            endHolder(holder);
    }
    m_cellFactsReachedByEffects = facts;
    m_kindsOfPlacesReachedByEffects = kindsOfPlaces;
    m_maskOfPlacesReachedByEffects = maskOfPlaces;
}

void WalkState::Held::dropManyEndedRecords()
{
    if (!m_numberOfHolders)
        dropAllRecords();
    else
        dropEndedRecords();
}

void WalkState::Held::dropEndedRecords()
{
    bool hadLists = m_lists;
    if (hadLists) [[unlikely]]
        dropLists();
    auto holders = std::exchange(m_holders, { });
    auto places = std::exchange(m_places, { });
    m_holdersReachedByEffects.shrink(0);
    m_firstFreshHolderWithElements = none;
    m_filterOfHolders = 0;
    m_cellFactsReachedByEffects = { };
    m_kindsOfPlacesReachedByEffects = 0;
    m_maskOfPlacesReachedByEffects = 0;

    auto keepPlacesOf = [&](uint32_t first, uint32_t holder) {
        uint32_t firstKept = none;
        uint32_t lastKept = none;
        for (uint32_t position = first; position != none; position = places[position].next) {
            Place place = places[position];
            if (!place.value)
                continue;
            uint32_t kept = m_places.size();
            place.holder = holder;
            place.next = none;
            m_places.append(place);
            if (lastKept == none)
                firstKept = kept;
            else
                m_places.mutableAt(lastKept).next = kept;
            lastKept = kept;
        }
        return firstKept;
    };
    for (uint32_t former = 0; former < holders.size(); ++former) {
        Holder holder = holders[former];
        if (holder.hasEnded)
            continue;
        uint32_t position = m_holders.size();
        holder.firstPlace = keepPlacesOf(holder.firstPlace, position);
        holder.firstElement = keepPlacesOf(holder.firstElement, position);
        holder.placeAmongReachedByEffects = none;
        holder.isListedAsFreshWithElements = false;
        holder.nextFreshWithElements = none;
        if (holder.firstPlace == none) {
            holder.layoutsOfSlots = LayoutsOfSlots::None;
            holder.kindsOfPlaces = 0;
            holder.maskOfPlaces = 0;
        }
        if (!holder.isFresh && !holder.shape && holder.cellFacts.isEmpty() && holder.firstPlace == none && holder.firstElement == none)
            continue;
        m_holders.append(holder);
        m_filterOfHolders |= bitInFilterOfHolders(holder.object);
        if (!holder.isFresh) {
            listAsReachedByEffects(position);
            m_cellFactsReachedByEffects.add(holder.cellFacts);
            m_kindsOfPlacesReachedByEffects |= holder.kindsOfPlaces;
            m_maskOfPlacesReachedByEffects |= holder.maskOfPlaces;
        } else if (holder.firstElement != none)
            listAsFreshWithElements(position);
    }
    m_numberOfHolders = m_holders.size();
    RELEASE_ASSERT(m_numberOfPlaces == m_places.size());

    m_tableOfHolders.clear();
    m_tableOfPlaces.clear();
    if (m_holders.size() > holdersFoundWithoutTable)
        fillTableOfHolders();
    if (m_places.size() > placesFoundWithoutTable)
        fillTableOfPlaces();
    if (hadLists) [[unlikely]]
        makeLists();
}

void WalkState::Held::noteShape(Reference object, TypeID proven)
{
    ASSERT(object);
    uint32_t holder = proven ? findOrAddHolder(object) : findHolder(object);
    if (holder == none)
        return;
    if (!proven) {
        endShape(holder);
        return;
    }
    if (m_holders[holder].shape == proven)
        return;
    Holder& record = m_holders.mutableAt(holder);
    if (!record.shape) {
        ++m_numberOfShapes;
        if (!record.isFresh)
            ++m_numberOfShapesReachedByEffects;
    }
    record.shape = proven;
    if (m_lists && !record.isFresh) [[unlikely]]
        m_lists->addHolder(Lists::listOfShapes, holder);
}

TypeID WalkState::Held::shapeOfHolderOf(Reference object) const
{
    const Holder* record = findRecordOfHolderThatHasNotEnded(object);
    return record ? record->shape : TypeID();
}

void WalkState::Held::recordCellFacts(Reference object, OptionSet<CellFact> facts)
{
    ASSERT(object);
    if (facts.isEmpty())
        return;
    uint32_t holder = findOrAddHolder(object);
    OptionSet<CellFact> known = m_holders[holder].cellFacts;
    if (known.containsAll(facts))
        return;
    Holder& record = m_holders.mutableAt(holder);
    record.cellFacts = known | facts;
    if (known.isEmpty())
        ++m_numberOfFactHolders;
    if (record.isFresh)
        return;
    if (known.isEmpty())
        ++m_reachedFactHolderCount;
    m_cellFactsReachedByEffects.add(facts);
    if (m_lists) [[unlikely]]
        listHolder(holder);
}

void WalkState::Held::invalidateCellFactsOfHolder(Reference object, OptionSet<CellFact> ended)
{
    if (uint32_t holder = findHolder(object); holder != none)
        invalidateCellFacts(holder, ended);
}

OptionSet<CellFact> WalkState::Held::cellFactsOfHolder(Reference object) const
{
    const Holder* record = findRecordOfHolderThatHasNotEnded(object);
    return record ? record->cellFacts : OptionSet<CellFact>();
}

void WalkState::Held::noteOrderWord(Reference object, std::optional<uint32_t> orderWord)
{
    ASSERT(object);
    if (!orderWord) {
        if (uint32_t holder = findHolder(object); holder != none)
            invalidateCellFacts(holder, CellFact::OrderWordIsKnown);
        return;
    }
    recordCellFacts(object, { CellFact::NotFrozen, CellFact::IsExtensible, CellFact::NoFieldOutsideSlot, CellFact::NoExtraProperties, CellFact::OrderWordIsKnown });
    uint32_t holder = findHolder(object);
    if (m_holders[holder].orderWord != *orderWord)
        m_holders.mutableAt(holder).orderWord = *orderWord;
}

std::optional<uint32_t> WalkState::Held::orderWordOf(Reference object) const
{
    if (!m_numberOfFactHolders)
        return std::nullopt;
    const Holder* record = findRecordOfHolderThatHasNotEnded(object);
    if (!record || !record->cellFacts.contains(CellFact::OrderWordIsKnown))
        return std::nullopt;
    return record->orderWord;
}

void WalkState::Held::noteFresh(Reference object)
{
    ASSERT(object);
    uint32_t holder = findOrAddHolder(object);
    if (m_holders[holder].isFresh)
        return;
    takeOffListOfReachedByEffects(holder);
    countNoLongerAmongReachedByEffects(m_holders[holder]);
    m_holders.mutableAt(holder).isFresh = true;
    ++m_numberOfFreshHolders;
    if (m_holders[holder].firstElement != none)
        listAsFreshWithElements(holder);
}

bool WalkState::Held::holderIsFresh(Reference object) const
{
    const Holder* record = findRecordOfHolderThatHasNotEnded(object);
    return record && record->isFresh;
}

Vector<WalkState::SlotOfObject, 8> WalkState::listSlotsOf(Reference object) const
{
    Vector<SlotOfObject, 8> slots;
    if (!mayHoldPlaceOf(object))
        return slots;
    uint32_t holder = m_held->findHolder(object);
    if (holder == none)
        return slots;
    for (uint32_t position = m_held->m_holders[holder].firstPlace; position != none; position = m_held->m_places[position].next) {
        const Place& place = m_held->m_places[position];
        if (place.value && place.kind == PlaceKind::Slot)
            slots.append({ FieldReference { LayoutID(place.first), place.second }, place.value });
    }
    return slots;
}

Vector<WalkState::ElementOfObject, 8> WalkState::listElementsOf(Reference object) const
{
    Vector<ElementOfObject, 8> elements;
    if (!mayHoldPlaceOf(object))
        return elements;
    uint32_t holder = m_held->findHolder(object);
    if (holder == none)
        return elements;
    for (uint32_t position = m_held->m_holders[holder].firstElement; position != none; position = m_held->m_places[position].next) {
        const Place& place = m_held->m_places[position];
        if (place.value && place.kind == PlaceKind::Element && !place.second)
            elements.append({ Reference::fromBits(place.first), place.value });
    }
    return elements;
}

Vector<Reference, 8> WalkState::listFreshObjects() const
{
    Vector<Reference, 8> fresh;
    if (!holdsFreshObjects())
        return fresh;
    for (uint32_t holder = 0; holder < m_held->m_holders.size(); ++holder) {
        if (m_held->m_holders[holder].isFresh)
            fresh.append(m_held->m_holders[holder].object);
    }
    return fresh;
}

ALWAYS_INLINE_INTO_OTHER_FILES bool WalkState::Held::letLeaveIfFresh(Reference object)
{
    uint32_t holder = findHolder(object);
    if (holder == none || !m_holders[holder].isFresh)
        return false;
    letLeave(holder);
    return true;
}

void WalkState::Held::noteSlot(const ProgramOfState& program, Reference object, FieldReference field, Reference value)
{
    ASSERT(object);
    uint32_t layout = field.layout.index();
    if (m_numberOfFreshHolders) {
        uint32_t holder = findHolder(object);
        if (holder != none && m_holders[holder].isFresh && (!value || valueOfPlace(PlaceKind::Slot, object, layout, field.field) != value))
            endSlotsUnderOtherLayouts(holder, layout);
    }
    notePlace(PlaceKind::Slot, object, layout, field.field, value, [&] {
        return bitOfSlot(program, field);
    });
    dropEndedRecordsIfMany();
}

void WalkState::Held::noteVariable(const ProgramOfState& program, VariableID variable, Reference value)
{
    notePlace(PlaceKind::Variable, Reference(), variable.index(), 0, value, [&] {
        return bitOf(program, MemoryLocation::moduleVariable(variable));
    });
    dropEndedRecordsIfMany();
}

void WalkState::Held::noteEnvironmentField(const ProgramOfState& program, Reference environment, FieldReference field, Reference value)
{
    ASSERT(environment);
    notePlace(PlaceKind::EnvironmentField, environment, field.layout.index(), field.field, value, [&] {
        return bitOf(program, MemoryLocation::environmentField(field.layout, field.field));
    });
    dropEndedRecordsIfMany();
}

void WalkState::Held::noteElement(Reference object, Reference index, Reference value)
{
    ASSERT(object && index);
    if (m_numberOfFreshHolders) {
        uint32_t holder = findHolder(object);
        if (holder != none && m_holders[holder].isFresh && (!value || valueOfPlace(PlaceKind::Element, object, index.bits(), 0) != value)) {
            endPlacesOf(holder, ListOfPlaces::Elements, [&](const Place& place) {
                return place.first != index.bits();
            });
        }
    }
    notePlace(PlaceKind::Element, object, index.bits(), 0, value, [] {
        return Place::entireHeap;
    });
    dropEndedRecordsIfMany();
}

void WalkState::Held::noteElementWithNaNForHole(Reference object, Reference index, Reference value)
{
    ASSERT(object && index && value);
    notePlace(PlaceKind::Element, object, index.bits(), 1, value, [] {
        return Place::entireHeap;
    });
    dropEndedRecordsIfMany();
}

void WalkState::Held::noteElementsLieOutsideViews(Reference object)
{
    uint32_t holder = findHolder(object);
    if (holder != none && !m_holders[holder].elementsLieOutsideViews)
        m_holders.mutableAt(holder).elementsLieOutsideViews = true;
}

void WalkState::Held::noteStoresPassAbove(LayoutID classLayout, PropertyKeyID key, Reference test)
{
    notePlace(PlaceKind::GuardOfStores, Reference(), numberOfGuardedSide(classLayout, ClassSide::Prototype), key.bits(), test, [] {
        return Place::entireHeap;
    });
    dropEndedRecordsIfMany();
}

void WalkState::Held::noteGuardOfReadsHolds(LayoutID classLayout, PropertyKeyID key, Reference test, ClassSide side, AskerOfGuardOfReads asker)
{
    notePlace(PlaceKind::GuardOfReads, Reference(), numberOfGuardedSide(classLayout, side, asker), key.bits(), test, [] {
        return Place::entireHeap;
    });
    dropEndedRecordsIfMany();
}

bool WalkState::Held::mayHoldGuards() const
{
    uint32_t holder = findHolder(Reference());
    constexpr uint16_t kindsOfGuards = 1 << static_cast<unsigned>(PlaceKind::GuardOfStores) | 1 << static_cast<unsigned>(PlaceKind::GuardOfReads);
    return holder != none && (m_holders[holder].kindsOfPlaces & kindsOfGuards);
}

bool WalkState::Held::someGuardHoldsOf(LayoutID classLayout) const
{
    if (!mayHoldGuards())
        return false;
    for (uint32_t position = m_holders[findHolder(Reference())].firstPlace; position != none; position = m_places[position].next) {
        const Place& place = m_places[position];
        if (place.value && (place.kind == PlaceKind::GuardOfStores || place.kind == PlaceKind::GuardOfReads) && classOfGuardedSide(place.first) == classLayout && askerOfGuardedSide(place.first) != AskerOfGuardOfReads::PrototypeLink)
            return true;
    }
    return false;
}

void WalkState::Held::makeLists()
{
    ASSERT(!m_lists);
    m_lists = new Lists;
    m_lists->places.fill(none, Lists::headsOfPlaces);
    m_lists->places.grow(Lists::notListed, Lists::headsOfPlaces + m_places.size() * Lists::entriesOfPlace);
    m_lists->holders.fill(none, Lists::headsOfHolders);
    m_lists->holders.grow(Lists::notListed, Lists::headsOfHolders + m_holders.size() * Lists::entriesOfHolder);
    for (uint32_t index = 0; index < m_holdersReachedByEffects.size(); ++index)
        listWhatHangsOn(m_holdersReachedByEffects[index]);
}

void WalkState::Held::dropLists()
{
    delete std::exchange(m_lists, nullptr);
}

void WalkState::Held::listPlace(uint32_t position)
{
    const Place& place = m_places[position];
    if (place.kind == PlaceKind::Variable) {
        m_lists->addPlace(Lists::firstListOfVariables + place.bit, Lists::entryByBit, position);
        return;
    }
    if (place.kind == PlaceKind::EnvironmentField) {
        m_lists->addPlace(Lists::firstListOfEnvironmentFields + place.bit, Lists::entryByBit, position);
        return;
    }
    if (place.kind == PlaceKind::GuardOfStores || place.kind == PlaceKind::GuardOfReads) {
        m_lists->addPlace(Lists::listOfGuards, Lists::entryByBit, position);
        return;
    }
    if (place.kind != PlaceKind::Slot)
        return;
    m_lists->addPlace(Lists::firstListOfSlots + place.bit, Lists::entryByBit, position);
    uint32_t byLayout = place.first % Lists::listsByLayout;
    bool wasEmpty = m_lists->places[Lists::firstListByLayout + byLayout] == none;
    if (!m_lists->addPlace(Lists::firstListByLayout + byLayout, Lists::entryByLayout, position))
        return;
    uint32_t layoutOfList = m_lists->places[Lists::firstLayoutOfList + byLayout];
    if (wasEmpty || layoutOfList != place.first)
        m_lists->places.mutableAt(Lists::firstLayoutOfList + byLayout) = wasEmpty ? place.first : Lists::severalLayouts;
}

void WalkState::Held::listHolder(uint32_t holder)
{
    const Holder& record = m_holders[holder];
    for (CellFact fact : record.cellFacts)
        m_lists->addHolder(Lists::listOf(fact), holder);
    if (record.shape)
        m_lists->addHolder(Lists::listOfShapes, holder);
    if (record.numberOfElements)
        m_lists->addHolder(Lists::listOfElements, holder);
}

void WalkState::Held::listWhatHangsOn(uint32_t holder)
{
    listHolder(holder);
    for (uint32_t position = m_holders[holder].firstPlace; position != none; position = m_places[position].next) {
        if (m_places[position].value)
            listPlace(position);
    }
}

void WalkState::Held::endHolderIfNothingHangsOn(uint32_t holder)
{
    const Holder& record = m_holders[holder];
    if (!record.hasEnded && !record.isFresh && !record.shape && record.cellFacts.isEmpty() && !record.numberOfPlaces && !record.numberOfElements)
        endHolder(holder);
}

void WalkState::Held::endByLists(const WhatEnds& ends)
{
    Vector<uint32_t, 16> holdersThatLostSomething;
    uint32_t lastHolderAskedAbout = none;
    bool lastAnswer = false;
    auto isWritten = [&](uint32_t holder) {
        if (!ends.mayBeWritten)
            return true;
        if (holder != lastHolderAskedAbout) {
            lastHolderAskedAbout = holder;
            Reference object = m_holders[holder].object;
            lastAnswer = !object || (*ends.mayBeWritten)(object);
        }
        return lastAnswer;
    };
    auto goThroughHolders = [&](uint32_t list, const auto& has, const auto& end) {
        m_lists->goThroughHolders(list, [&](uint32_t holder) {
            const Holder& record = m_holders[holder];
            if (record.hasEnded || record.isFresh || !has(record))
                return false;
            if (!isWritten(holder))
                return true;
            end(holder);
            holdersThatLostSomething.append(holder);
            return false;
        });
    };
    auto staysBehindWrite = [&](uint64_t written, const auto& mayEnd) {
        return [&, written](uint32_t position) {
            const Place& place = m_places[position];
            uint32_t holder = place.holder;
            if (!place.value || m_holders[holder].isFresh)
                return false;
            if (!(written & place.mask()) || !mayEnd(place))
                return true;
            endPlace(position);
            holdersThatLostSomething.append(holder);
            return false;
        };
    };
    auto goThroughPlaces = [&](uint32_t firstList, uint64_t written, const auto& mayEnd) {
        m_lists->goThroughPlacesUnder(firstList, written, staysBehindWrite(written, mayEnd));
    };
    auto holderIsWritten = [&](const Place& place) {
        return isWritten(place.holder);
    };
    auto isWrittenAmong = [&](uint64_t written) {
        return [&, written](const Place& place) {
            return isWritten(place.holder) && !(ends.writtenAlone && liesBesideWhatIsWritten(*ends.program, place, written, ends.writtenAlone));
        };
    };

    const WriterOfOneObject* writer = ends.writerOfOneObject;
    if (writer && (ends.shapes || ends.elements || !ends.facts.isEmpty())) {
        auto hasWhatListsOfHoldersHold = [&](uint32_t holder) {
            const Holder& record = m_holders[holder];
            return !record.hasEnded && !record.isFresh && (record.shape || !record.cellFacts.isEmpty() || record.numberOfElements);
        };
        Vector<uint32_t, 16> holdersToAsk;
        m_lists->goThroughHoldersByClass(Lists::listOfHoldersNotAsked, [&](uint32_t holder) {
            if (hasWhatListsOfHoldersHold(holder))
                holdersToAsk.append(holder);
            return false;
        });
        for (uint32_t holder : holdersToAsk) {
            Reference object = m_holders[holder].object;
            ClassOfHolder classOfHolder = object ? writer->classOf(object) : ClassOfHolder();
            if (classOfHolder.layoutAlone)
                m_lists->addToChainOfHolders(classOfHolder, holder);
            else
                m_lists->addHolderByClass(Lists::listOfWideHolders, holder);
        }
        auto staysBehindWriter = [&](uint32_t holder) {
            if (!hasWhatListsOfHoldersHold(holder))
                return false;
            if (!isWritten(holder))
                return true;
            bool lostSomething = false;
            if (ends.shapes && m_holders[holder].shape) {
                endShape(holder);
                lostSomething = true;
            }
            if (m_holders[holder].cellFacts.containsAny(ends.facts)) {
                invalidateCellFacts(holder, ends.facts);
                lostSomething = true;
            }
            if (ends.elements && m_holders[holder].numberOfElements) {
                endPlacesOf(holder, ListOfPlaces::Elements, [](const Place&) {
                    return true;
                });
                lostSomething = true;
            }
            if (lostSomething)
                holdersThatLostSomething.append(holder);
            return hasWhatListsOfHoldersHold(holder);
        };
        auto goThroughChainOf = [&](bool newCellsThemselves) {
            if (uint32_t chain = m_lists->findChainOfHolders({ writer->classOfRoot.layoutAlone, newCellsThemselves }); chain != none)
                m_lists->goThroughChainOfHolders(chain, staysBehindWriter);
        };
        m_lists->goThroughHoldersByClass(Lists::listOfWideHolders, staysBehindWriter);
        goThroughChainOf(false);
        if (!writer->classOfRoot.isNewCellItself)
            goThroughChainOf(true);
        else if (uint32_t own = findHolder(writer->root); own != none)
            staysBehindWriter(own);
    }
    if (ends.shapes && !writer) {
        goThroughHolders(Lists::listOfShapes, [](const Holder& record) {
            return !!record.shape;
        }, [&](uint32_t holder) {
            endShape(holder);
        });
    }
    for (CellFact fact : writer ? OptionSet<CellFact>() : ends.facts) {
        goThroughHolders(Lists::listOf(fact), [&](const Holder& record) {
            return record.cellFacts.contains(fact);
        }, [&](uint32_t holder) {
            invalidateCellFacts(holder, ends.facts);
        });
    }
    if (ends.slots) {
        bool writesOneLayout = writer;
        Vector<uint32_t, 16> foundToBeOfLayoutAlone;
        auto slotIsWritten = isWrittenAmong(ends.slots);
        auto slotStays = staysBehindWrite(ends.slots, slotIsWritten);
        auto goThroughSlotsOf = [&](uint32_t list) {
            m_lists->goThroughPlaces(Lists::firstListOfSlots + list, Lists::entryByBit, [&](uint32_t position) {
                if (!slotStays(position))
                    return false;
                if (!writesOneLayout || !writer->isOfLayoutAlone(m_holders[m_places[position].holder].object, LayoutID(m_places[position].first)))
                    return true;
                foundToBeOfLayoutAlone.append(position);
                return false;
            });
            if (!writesOneLayout)
                m_lists->goThroughChainsOf(list, slotStays);
            else if (uint32_t anchor = m_lists->findAnchor(m_places, writer->classOfRoot.layoutAlone.index(), list); anchor != none)
                m_lists->goThroughChain(anchor, slotStays);
        };
        for (uint64_t bits = ends.slots; bits; bits &= bits - 1)
            goThroughSlotsOf(std::countr_zero(bits));
        goThroughSlotsOf(Place::entireHeap);
        for (uint32_t position : foundToBeOfLayoutAlone)
            m_lists->sortOut(m_places, position);
    }
    goThroughPlaces(Lists::firstListOfEnvironmentFields, ends.environmentFields, holderIsWritten);
    if (ends.variableWrittenAlone) {
        uint32_t holder = findHolder(Reference());
        uint32_t written = holder != none ? findPlace(holder, PlaceKind::Variable, ends.variableWrittenAlone.index(), 0) : none;
        if (written != none) {
            endPlace(written);
            holdersThatLostSomething.append(holder);
        }
        m_lists->goThroughPlaces(Lists::firstListOfVariables + Place::entireHeap, Lists::entryByBit, staysBehindWrite(ends.variables, holderIsWritten));
    } else
        goThroughPlaces(Lists::firstListOfVariables, ends.variables, holderIsWritten);
    if (ends.guards) {
        m_lists->goThroughPlaces(Lists::listOfGuards, Lists::entryByBit, [&](uint32_t position) {
            const Place& place = m_places[position];
            if (!place.value)
                return false;
            if (!(ends.guards & place.mask()) || (ends.mayEndGuard && !(*ends.mayEndGuard)(classOfGuardedSide(place.first), PropertyKeyID::fromBits(place.second))))
                return true;
            uint32_t holder = place.holder;
            endPlace(position);
            holdersThatLostSomething.append(holder);
            return false;
        });
    }
    if (ends.elements && !writer) {
        m_lists->goThroughHolders(Lists::listOfElements, [&](uint32_t holder) {
            const Holder& record = m_holders[holder];
            if (record.hasEnded || record.isFresh || !record.numberOfElements)
                return false;
            if (!isWritten(holder) || (ends.sparesElementsOutsideViews && record.elementsLieOutsideViews))
                return true;
            endPlacesOf(holder, ListOfPlaces::Elements, [](const Place&) {
                return true;
            });
            holdersThatLostSomething.append(holder);
            return false;
        });
    }
    for (uint32_t holder : holdersThatLostSomething)
        endHolderIfNothingHangsOn(holder);
}

void WalkState::Held::invalidateAliasedSlotsByLists(uint32_t own, uint8_t bit, FieldReference field, bool storesIntoLayoutAlone, NOESCAPE const ScopedLambda<bool(Reference other)>& mayBeSameObject, NOESCAPE const ScopedLambda<bool(Reference, LayoutID)>& isOfLayoutAlone)
{
    uint32_t layout = field.layout.index();
    Vector<uint32_t, 16> holdersThatLostSomething;
    uint32_t lastHolderAskedAbout = none;
    bool lastAnswer = false;
    auto mayBeSame = [&](uint32_t holder) {
        if (holder != lastHolderAskedAbout) {
            lastHolderAskedAbout = holder;
            lastAnswer = mayBeSameObject(m_holders[holder].object);
        }
        return lastAnswer;
    };
    auto endIf = [&](uint32_t position, const auto& isMeant) {
        const Place& place = m_places[position];
        uint32_t holder = place.holder;
        if (!place.value || m_holders[holder].isFresh)
            return false;
        if (holder == own || !isMeant(place) || !mayBeSame(holder))
            return true;
        endPlace(position);
        holdersThatLostSomething.append(holder);
        return false;
    };

    auto isSameField = [&](const Place& place) {
        return place.first == layout && place.second == field.field;
    };
    auto endSameFieldIn = [&](uint32_t list) {
        auto staysBesideStore = [&](uint32_t position) {
            return endIf(position, isSameField);
        };
        m_lists->goThroughPlaces(Lists::firstListOfSlots + list, Lists::entryByBit, staysBesideStore);
        if (uint32_t anchor = m_lists->findAnchor(m_places, layout, list); anchor != none)
            m_lists->goThroughChain(anchor, staysBesideStore);
    };
    if (bit == Place::entireHeap) {
        for (uint32_t list = 0; list < Lists::listsOfOneHeap; ++list)
            endSameFieldIn(list);
    } else {
        endSameFieldIn(bit);
        endSameFieldIn(Place::entireHeap);
    }

    Vector<uint32_t, 16> foundToBeOfLayoutAlone;
    auto goThroughListsByLayout = [&](uint32_t firstList, uint32_t firstLayout, bool sortsOut) {
        for (uint32_t byLayout = 0; byLayout < Lists::listsByLayout; ++byLayout) {
            if (m_lists->places[firstList + byLayout] == none || m_lists->places[firstLayout + byLayout] == layout)
                continue;
            bool someStay = false;
            uint32_t layoutOfList = none;
            m_lists->goThroughPlaces(firstList + byLayout, Lists::entryByLayout, [&](uint32_t position) {
                bool stays = endIf(position, [&](const Place& place) {
                    return place.first != layout;
                });
                if (stays && sortsOut && isOfLayoutAlone(m_holders[m_places[position].holder].object, LayoutID(m_places[position].first))) {
                    foundToBeOfLayoutAlone.append(position);
                    return false;
                }
                if (stays) {
                    uint32_t layoutOfPlace = m_places[position].first;
                    layoutOfList = !someStay || layoutOfList == layoutOfPlace ? layoutOfPlace : Lists::severalLayouts;
                    someStay = true;
                }
                return stays;
            });
            if (m_lists->places[firstLayout + byLayout] != layoutOfList)
                m_lists->places.mutableAt(firstLayout + byLayout) = layoutOfList;
        }
    };
    goThroughListsByLayout(Lists::firstListByLayout, Lists::firstLayoutOfList, true);
    if (!storesIntoLayoutAlone)
        goThroughListsByLayout(Lists::firstListByLayoutAlone, Lists::firstLayoutOfListAlone, false);
    for (uint32_t position : foundToBeOfLayoutAlone) {
        uint32_t layoutOfPlace = m_places[position].first;
        uint32_t byLayout = layoutOfPlace % Lists::listsByLayout;
        bool wasEmpty = m_lists->places[Lists::firstListByLayoutAlone + byLayout] == none;
        m_lists->addPlace(Lists::firstListByLayoutAlone + byLayout, Lists::entryByLayout, position);
        if (wasEmpty || m_lists->places[Lists::firstLayoutOfListAlone + byLayout] != layoutOfPlace)
            m_lists->places.mutableAt(Lists::firstLayoutOfListAlone + byLayout) = wasEmpty ? layoutOfPlace : Lists::severalLayouts;
    }
    for (uint32_t holder : holdersThatLostSomething)
        endHolderIfNothingHangsOn(holder);
}

void WalkState::Held::invalidateAliasedSlots(const ProgramOfState& program, Reference object, FieldReference field, NOESCAPE const ScopedLambda<bool(Reference other)>& mayBeSameObject, NOESCAPE const ScopedLambda<bool(Reference, LayoutID)>& isOfLayoutAlone)
{
    if (!m_numberOfPlaces)
        return;
    uint32_t layout = field.layout.index();
    uint32_t own = findHolder(object);
    if (own != none)
        endSlotsUnderOtherLayouts(own, layout);
    if (own != none && m_holders[own].isFresh)
        return;
    if (!m_numberOfPlacesReachedByEffects || !(m_kindsOfPlacesReachedByEffects >> static_cast<unsigned>(PlaceKind::Slot) & 1)) {
        dropEndedRecordsIfMany();
        return;
    }
    if (goesByLists()) [[unlikely]] {
        invalidateAliasedSlotsByLists(own, bitOfSlot(program, field), field, isOfLayoutAlone(object, field.layout), mayBeSameObject, isOfLayoutAlone);
        dropEndedRecordsIfMany();
        return;
    }

    forEachHolderReachedByEffects([&](uint32_t holder) {
        const Holder& record = m_holders[holder];
        if (holder == own || record.layoutsOfSlots == LayoutsOfSlots::None)
            return;
        Reference other = record.object;
        if (record.layoutsOfSlots == LayoutsOfSlots::One && record.layoutOfSlots == layout) {
            uint32_t sameField = findPlace(holder, PlaceKind::Slot, layout, field.field);
            if (sameField != none && mayBeSameObject(other))
                endPlace(sameField);
            return;
        }
        std::optional<bool> mayBeSame;
        endPlacesOf(holder, ListOfPlaces::AllButElements, [&](const Place& place) {
            if (place.kind != PlaceKind::Slot || (place.first == layout && place.second != field.field))
                return false;
            if (!mayBeSame)
                mayBeSame = mayBeSameObject(other);
            return *mayBeSame;
        });
    });
    dropEndedRecordsIfMany();
}

uint64_t WalkState::Held::writtenUnderPlacesReachedByEffects(const LocationMasks& writes) const
{
    auto hasKind = [&](PlaceKind kind) {
        return m_kindsOfPlacesReachedByEffects >> static_cast<unsigned>(kind) & 1;
    };
    uint64_t written = 0;
    if (hasKind(PlaceKind::Slot))
        written |= writes.maskOf(AbstractHeap::ObjectFields);
    if (hasKind(PlaceKind::Variable))
        written |= writes.maskOf(AbstractHeap::ModuleVariables);
    if (hasKind(PlaceKind::EnvironmentField))
        written |= writes.maskOf(AbstractHeap::EnvironmentFields);
    if (hasKind(PlaceKind::GuardOfStores) || hasKind(PlaceKind::GuardOfReads))
        written |= writes.maskOf(AbstractHeap::MemberGuards);
    return written;
}

bool WalkState::Held::restsOnHeapsAmong(const LocationMasks& writes) const
{
    return (writes.contains(AbstractHeap::ShapeWords) && m_numberOfShapes)
        || (writes.containsAny(heapsUnderPlacesInWalkState) && m_numberOfPlaces)
        || (m_numberOfFactHolders && !cellFactsInvalidatedBy(writes).isEmpty());
}

void WalkState::Held::endWhatIsWritten(const ProgramOfState& program, const Effects& effects, const MemoryLocation* writtenAlone)
{
    if (effects.mayRunOtherCode()) {
        endWhatIsNotFresh();
        return;
    }

    const LocationMasks& writes = effects.writes;
    bool endsShapes = writes.contains(AbstractHeap::ShapeWords) && m_numberOfShapes;
    bool endsPlaces = writes.containsAny(heapsUnderPlacesInWalkState) && m_numberOfPlaces;
    OptionSet<CellFact> factsEnded = m_numberOfFactHolders ? cellFactsInvalidatedBy(writes) : OptionSet<CellFact>();
    if (!endsShapes && !endsPlaces && factsEnded.isEmpty())
        return;

    uint64_t slotsWritten = writes.maskOf(AbstractHeap::ObjectFields);
    uint64_t variablesWritten = writes.maskOf(AbstractHeap::ModuleVariables);
    uint64_t environmentFieldsWritten = writes.maskOf(AbstractHeap::EnvironmentFields);
    uint64_t guardsWritten = writes.maskOf(AbstractHeap::MemberGuards);
    bool endsElements = endsPlaces && writes.containsAny(heapsUnderElementsInWalkState);
    bool reachesSomething = (endsShapes && m_numberOfShapesReachedByEffects)
        || (m_reachedFactHolderCount && m_cellFactsReachedByEffects.containsAny(factsEnded))
        || (m_numberOfPlacesReachedByEffects && (writtenUnderPlacesReachedByEffects(writes) & m_maskOfPlacesReachedByEffects))
        || (endsElements && m_numberOfElementsReachedByEffects);
    auto writtenOfKind = [&](PlaceKind kind) {
        switch (kind) {
        case PlaceKind::Slot:
            return slotsWritten;
        case PlaceKind::Variable:
            return variablesWritten;
        case PlaceKind::EnvironmentField:
            return environmentFieldsWritten;
        case PlaceKind::GuardOfStores:
        case PlaceKind::GuardOfReads:
            return guardsWritten;
        case PlaceKind::Element:
            break;
        }
        RELEASE_ASSERT_NOT_REACHED();
    };
    auto endsAlways = [](const Place&) {
        return true;
    };
    auto endWhatHangsOn = [&](uint32_t holder) {
        const Holder& record = m_holders[holder];
        uint64_t written = 0;
        for (PlaceKind kind : { PlaceKind::Slot, PlaceKind::Variable, PlaceKind::EnvironmentField, PlaceKind::GuardOfStores, PlaceKind::GuardOfReads }) {
            if (record.kindsOfPlaces >> static_cast<unsigned>(kind) & 1)
                written |= writtenOfKind(kind);
        }
        bool endsPlacesOfHolder = written & record.maskOfPlaces;
        bool endsElementsOfHolder = endsElements && record.firstElement != none && (!record.elementsLieOutsideViews || writes.containsAny(heapsUnderElementsOutsideViewsInWalkState));
        bool endsFactsOfHolder = record.cellFacts.containsAny(factsEnded);
        uint32_t numberOfPlaces = record.numberOfPlaces;
        if (endsShapes && record.shape)
            endShape(holder);
        if (endsFactsOfHolder)
            invalidateCellFacts(holder, factsEnded);
        if (endsPlacesOfHolder) {
            noteRecordsGoneThrough(numberOfPlaces, placesFoundWithoutTable);
            uint8_t remainingKinds = 0;
            uint64_t remainingMask = 0;
            endPlacesOf(holder, ListOfPlaces::AllButElements, [&](const Place& place) {
                if ((writtenOfKind(place.kind) & place.mask()) && !liesBesideWhatIsWritten(program, place, writtenOfKind(place.kind), writtenAlone))
                    return true;
                remainingKinds |= 1 << static_cast<unsigned>(place.kind);
                remainingMask |= place.mask();
                return false;
            });
            Holder& changed = m_holders.mutableAt(holder);
            changed.kindsOfPlaces = remainingKinds;
            changed.maskOfPlaces = remainingMask;
        }
        if (endsElementsOfHolder)
            endPlacesOf(holder, ListOfPlaces::Elements, endsAlways);
    };
    if (reachesSomething && goesByLists()) [[unlikely]] {
        bool namesOneVariable = writtenAlone && writtenAlone->heap() == AbstractHeap::ModuleVariables && variablesWritten == Typed::maskOf(*program.program, *writtenAlone);
        endByLists({ .shapes = endsShapes, .elements = endsElements, .sparesElementsOutsideViews = !writes.containsAny(heapsUnderElementsOutsideViewsInWalkState), .facts = factsEnded, .slots = slotsWritten, .variables = variablesWritten,
            .environmentFields = environmentFieldsWritten, .guards = guardsWritten, .variableWrittenAlone = namesOneVariable ? writtenAlone->variable() : VariableID(), .program = &program, .writtenAlone = writtenAlone });
    } else if (reachesSomething)
        forEachHolderReachedByEffects(endWhatHangsOn);

    if (endsPlaces && writes.containsAny(heapsUnderElementsOutsideObjectsInWalkState)) {
        for (uint32_t holder = std::exchange(m_firstFreshHolderWithElements, none); holder != none;) {
            Holder& record = m_holders.mutableAt(holder);
            record.isListedAsFreshWithElements = false;
            uint32_t next = std::exchange(record.nextFreshWithElements, none);
            if (!record.hasEnded && !record.elementsLieOutsideViews)
                endPlacesOf(holder, ListOfPlaces::Elements, endsAlways);
            holder = next;
        }
    }
    dropEndedRecordsIfMany();
}

void WalkState::Held::endWhatIsNotFresh()
{
    if (!m_numberOfFreshHolders) {
        dropAllRecords();
        return;
    }
    while (!m_holdersReachedByEffects.isEmpty())
        endHolder(m_holdersReachedByEffects[m_holdersReachedByEffects.size() - 1]);
    dropEndedRecordsIfMany();
}

void WalkState::Held::dropAllRecords()
{
    if (m_lists) [[unlikely]]
        dropLists();
    m_recordsGoneThrough = 0;
    m_holders.clear();
    m_places.clear();
    m_holdersReachedByEffects.shrink(0);
    m_firstFreshHolderWithElements = none;
    if (!m_tableOfHolders.isEmpty())
        m_tableOfHolders.clear();
    if (!m_tableOfPlaces.isEmpty())
        m_tableOfPlaces.clear();
    m_numberOfHolders = 0;
    m_numberOfPlaces = 0;
    m_numberOfShapes = 0;
    m_numberOfFactHolders = 0;
    m_numberOfFreshHolders = 0;
    m_numberOfShapesReachedByEffects = 0;
    m_reachedFactHolderCount = 0;
    m_numberOfPlacesReachedByEffects = 0;
    m_numberOfElementsReachedByEffects = 0;
    m_cellFactsReachedByEffects = { };
    m_kindsOfPlacesReachedByEffects = 0;
    m_maskOfPlacesReachedByEffects = 0;
    m_filterOfHolders = 0;
}

void WalkState::Held::clear()
{
    if (!holdsNoRecord())
        dropAllRecords();
    m_checkedNames.clear();
    m_filterOfValuesWithCheckedNames = 0;
    m_bindingsPastDeadZone.clear();
    m_builtinsRead.clear();
}

bool leavesDeadZoneForGood(const Program& program, VariableID id)
{
    if (id.index() >= program.variables.size())
        return false;
    const Variable& variable = program.at(id);
    if (variable.initialization != VariableInitialization::Uninitialized || variable.owner || !variable.module)
        return false;
    if (variable.flags.containsAny({ VariableFlags::IsReplaceable, VariableFlags::HoldsMock, VariableFlags::OriginalMayRun, VariableFlags::HoldsInstance }))
        return false;
    return loaderEntersBodyAtMostOnce(program.at(variable.module));
}

bool leavesDeadZoneForGood(const Program& program, FieldReference field)
{
    return program.isInstanceVariable(field) && !fieldAt(program, field).flags.contains(FieldFlags::IsReplaceable);
}

void SharedSetOfNumbers::keepMembersOfSlowCase(const SharedSetOfNumbers& other)
{
    if (!other.m_size) {
        clear();
        return;
    }
    if (other.m_words.size() < m_words.size()) {
        SharedSetOfNumbers shorter = other;
        shorter.keepMembersOf(*this);
        swap(shorter);
        return;
    }

    struct Stretch {
        uint32_t begin;
        uint32_t end;
    };
    Vector<Stretch, 8> stretches;
    m_words.forEachStretchNotSharedWith(other.m_words, [&](uint32_t begin, uint32_t end) {
        stretches.append({ begin, end });
    });
    for (Stretch stretch : stretches) {
        for (uint32_t word = stretch.begin; word < stretch.end; ++word) {
            uint64_t held = m_words[word];
            uint64_t kept = word < other.m_words.size() ? held & other.m_words[word] : 0;
            if (kept == held)
                continue;
            m_words.mutableAt(word) = kept;
            m_size -= std::popcount(held ^ kept);
        }
    }
}

void WalkState::meetWith(const WalkState& other, RecordsLastMet* last)
{
    if (&other == this)
        return;
    m_answers.meetWith(other.m_answers);
    if (uint32_t newest = newestUndoableStore(); m_marks.restartMark != other.m_marks.restartMark || newest != other.newestUndoableStore()) {
        bool areBothUndoable = !((m_marks.restartMark | other.m_marks.restartMark) & irreversibleWritesBit) && newest == other.newestUndoableStore();
        m_marks.kinds = (m_marks.kinds & ~newestUndoableStoreMask) | newest << newestUndoableStoreShift;
        m_marks.restartMark = restartPointOf(m_marks.restartMark) != restartPointOf(other.m_marks.restartMark) ? 0
            : areBothUndoable ? m_marks.restartMark | other.m_marks.restartMark
            : restartPointOf(m_marks.restartMark) | irreversibleWritesBit;
    }
    if (m_held == other.m_held || holdsNothing())
        return;
    if (other.holdsNothing()) {
        dropWhatIsHeld();
        return;
    }
    heldToWrite().meetWith(*other.m_held, last);
    noteKindsOfRecordsLeft();
    if (m_held->m_checkedNames.isEmpty())
        m_marks.kinds &= ~Kinds::checkedNames;
}

void WalkState::Held::meetWith(const Held& other, RecordsLastMet* last)
{
    m_checkedNames.keepMembersOf(other.m_checkedNames);
    m_filterOfValuesWithCheckedNames = m_checkedNames.isEmpty() ? 0 : m_filterOfValuesWithCheckedNames & other.m_filterOfValuesWithCheckedNames;
    m_bindingsPastDeadZone.keepMembersOf(other.m_bindingsPastDeadZone);
    m_builtinsRead.keepMembersOf(other.m_builtinsRead);
    if (holdsNoRecord())
        return;
    if (other.holdsNoRecord()) {
        dropAllRecords();
        return;
    }
    struct Stretch {
        uint32_t begin;
        uint32_t end;
    };
    Vector<Stretch, 8> stretches;
    auto noteStretch = [&](uint32_t begin, uint32_t end) {
        stretches.append({ begin, end });
    };
    bool goesByWhatHasChanged = last && last->lieAsOwn
        && last->numberOfOwnHolders == m_holders.size() && last->numberOfOwnPlaces == m_places.size()
        && last->holders.size() <= other.m_holders.size() && last->places.size() <= other.m_places.size();
    bool lieAsOwn = true;
    auto noteStretchAmong = [&](uint32_t numberOfOwn) {
        return [&stretches, numberOfOwn](uint32_t begin, uint32_t end) {
            if (begin < numberOfOwn)
                stretches.append({ begin, std::min(end, numberOfOwn) });
        };
    };
    auto findHolderInOther = [&](uint32_t holder, Reference object) {
        if (holder < other.m_holders.size()) {
            if (const Holder& same = other.m_holders[holder]; same.object == object)
                return same.hasEnded ? none : holder;
        }
        return other.findHolder(object);
    };

    if (goesByWhatHasChanged)
        other.m_places.forEachStretchNotSharedWith(last->places, noteStretchAmong(m_places.size()));
    else
        m_places.forEachStretchNotSharedWith(other.m_places, noteStretch);
    for (Stretch stretch : stretches) {
        for (uint32_t position = stretch.begin; position < stretch.end; ++position) {
            const Place& place = m_places[position];
            if (!place.value)
                continue;
            uint32_t holderInOther = findHolderInOther(place.holder, m_holders[place.holder].object);
            uint32_t placeInOther = none;
            if (holderInOther != none) {
                const Place* same = position < other.m_places.size() ? &other.m_places[position] : nullptr;
                if (same && same->holder == holderInOther && same->kind == place.kind && same->first == place.first && same->second == place.second)
                    placeInOther = same->value ? position : none;
                else
                    placeInOther = other.findPlace(holderInOther, place.kind, place.first, place.second);
            }
            if (placeInOther == none || other.m_places[placeInOther].value != place.value)
                endPlace(position);
            else if (placeInOther != position || holderInOther != place.holder)
                lieAsOwn = false;
        }
    }

    stretches.shrink(0);
    if (goesByWhatHasChanged)
        other.m_holders.forEachStretchNotSharedWith(last->holders, noteStretchAmong(m_holders.size()));
    else
        m_holders.forEachStretchNotSharedWith(other.m_holders, noteStretch);
    for (Stretch stretch : stretches) {
        for (uint32_t holder = stretch.begin; holder < stretch.end; ++holder) {
            const Holder& record = m_holders[holder];
            if (record.hasEnded)
                continue;
            uint32_t holderInOther = findHolderInOther(holder, record.object);
            if (holderInOther == none) {
                endHolder(holder);
                continue;
            }
            if (holderInOther != holder)
                lieAsOwn = false;
            const Holder& same = other.m_holders[holderInOther];
            bool leaves = record.isFresh && !same.isFresh;
            OptionSet<CellFact> factsKnownHereAlone = record.cellFacts - same.cellFacts;
            if (record.orderWord != same.orderWord)
                factsKnownHereAlone.add(CellFact::OrderWordIsKnown);
            if (record.shape != same.shape)
                endShape(holder);
            invalidateCellFacts(holder, factsKnownHereAlone);
            if (record.elementsLieOutsideViews && !same.elementsLieOutsideViews) {
                m_holders.mutableAt(holder).elementsLieOutsideViews = false;
                if (m_holders[holder].isFresh && m_holders[holder].firstElement != none)
                    listAsFreshWithElements(holder);
            }
            if (leaves)
                letLeave(holder);
        }
    }
    if (!last)
        dropEndedRecordsIfMany();
    else if ((last->lieAsOwn = lieAsOwn && other.m_places.liesInTree())) {
        last->holders = other.m_holders;
        last->places = other.m_places;
        last->numberOfOwnHolders = m_holders.size();
        last->numberOfOwnPlaces = m_places.size();
    }
    if (other.m_recordsGoneThrough | m_recordsGoneThrough) [[unlikely]] {
        m_recordsGoneThrough = std::max(m_recordsGoneThrough, other.m_recordsGoneThrough);
        makeListsIfDue();
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
