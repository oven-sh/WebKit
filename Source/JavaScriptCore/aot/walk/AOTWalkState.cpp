/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkState.h"

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include "AOTSlotPlan.h"
#include <bit>
#include <optional>
#include <wtf/HashFunctions.h>
#include <wtf/MathExtras.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr AbstractHeapSet heapsUnderElementsInWalkState { AbstractHeap::Elements, AbstractHeap::ArrayStorage, AbstractHeap::TypedArrayElements, AbstractHeap::TypedArrayStorage };
static constexpr AbstractHeapSet heapsUnderPlacesInWalkState = heapsUnderElementsInWalkState | AbstractHeapSet { AbstractHeap::ObjectFields, AbstractHeap::EnvironmentFields, AbstractHeap::ModuleVariables, AbstractHeap::MemberGuards };
static constexpr AbstractHeapSet heapsUnderElementsOutsideObjectsInWalkState = heapsUnderElementsInWalkState - perObjectHeaps;
static_assert(perObjectHeaps.containsAll(heapsUnderPlacesInWalkState - heapsUnderElementsOutsideObjectsInWalkState - AbstractHeapSet { AbstractHeap::ModuleVariables, AbstractHeap::MemberGuards }));
static_assert(perObjectHeaps.contains(AbstractHeap::ShapeWords));
static_assert(perObjectHeaps.contains(AbstractHeap::NamedProperties) && perObjectHeaps.contains(AbstractHeap::OrderWords) && perObjectHeaps.contains(AbstractHeap::PrototypeLinks));

OptionSet<FactOfCell> factsOfCellEndedBy(const LocationMasks& writes)
{
    OptionSet<FactOfCell> ended;
    if (writes.contains(AbstractHeap::ShapeWords))
        ended.add({ FactOfCell::NotFrozen, FactOfCell::OrderWordIsKnown, FactOfCell::IsLikelyAsAllocated, FactOfCell::IsLikelyAsConstructed, FactOfCell::BearsNoMark });
    if (writes.contains(AbstractHeap::NamedProperties))
        ended.add({ FactOfCell::NoFieldOutsideSlot, FactOfCell::NoExtraProperties, FactOfCell::OrderWordIsKnown, FactOfCell::IsLikelyAsAllocated, FactOfCell::IsLikelyAsConstructed, FactOfCell::BearsNoMark });
    if (writes.contains(AbstractHeap::OrderWords))
        ended.add({ FactOfCell::OrderWordIsKnown, FactOfCell::IsLikelyAsAllocated, FactOfCell::IsLikelyAsConstructed });
    if (writes.contains(AbstractHeap::PrototypeLinks))
        ended.add(FactOfCell::HasLayoutPrototype);
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
    static constexpr uint32_t headsOfPlaces = firstLayoutOfList + listsByLayout;
    static constexpr uint32_t entriesOfPlace = 2;
    static constexpr uint32_t entryByBit = 0;
    static constexpr uint32_t entryByLayout = 1;

    static constexpr uint32_t listOfShapes = 8 * sizeof(OptionSet<FactOfCell>::StorageType);
    static constexpr uint32_t listOfElements = listOfShapes + 1;
    static constexpr uint32_t listsOfHolders = listOfElements + 1;

    static uint32_t listOf(FactOfCell fact) { return std::countr_zero(static_cast<OptionSet<FactOfCell>::StorageType>(fact)); }

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
    void addHolder(uint32_t list, uint32_t holder) { add(holders, list, listsOfHolders + holder * listsOfHolders + list, holder); }
    void goThroughPlaces(uint32_t list, uint32_t whichEntry, NOESCAPE const Invocable<bool(uint32_t place)> auto& stays) { goThrough(places, list, headsOfPlaces + whichEntry, entriesOfPlace, stays); }
    void goThroughHolders(uint32_t list, NOESCAPE const Invocable<bool(uint32_t holder)> auto& stays) { goThrough(holders, list, listsOfHolders + list, listsOfHolders, stays); }
    void goThroughPlacesUnder(uint32_t firstList, uint64_t written, NOESCAPE const Invocable<bool(uint32_t place)> auto& stays)
    {
        if (!written)
            return;
        for (uint64_t bits = written; bits; bits &= bits - 1)
            goThroughPlaces(firstList + std::countr_zero(bits), entryByBit, stays);
        goThroughPlaces(firstList + Place::entireHeap, entryByBit, stays);
    }

    Table places;
    Table holders;
};

WalkState::Held::Held(const Held& other)
    : m_lists(other.m_lists ? new Lists(*other.m_lists) : nullptr)
    , m_recordsGoneThrough(other.m_recordsGoneThrough)
    , m_firstFreshHolderWithElements(other.m_firstFreshHolderWithElements)
    , m_numberOfHolders(other.m_numberOfHolders)
    , m_numberOfPlaces(other.m_numberOfPlaces)
    , m_numberOfShapes(other.m_numberOfShapes)
    , m_numberOfHoldersWithFactsOfCell(other.m_numberOfHoldersWithFactsOfCell)
    , m_numberOfFreshHolders(other.m_numberOfFreshHolders)
    , m_numberOfShapesReachedByEffects(other.m_numberOfShapesReachedByEffects)
    , m_numberOfHoldersWithFactsOfCellReachedByEffects(other.m_numberOfHoldersWithFactsOfCellReachedByEffects)
    , m_numberOfPlacesReachedByEffects(other.m_numberOfPlacesReachedByEffects)
    , m_numberOfElementsReachedByEffects(other.m_numberOfElementsReachedByEffects)
    , m_factsOfCellReachedByEffects(other.m_factsOfCellReachedByEffects)
    , m_kindsOfPlacesReachedByEffects(other.m_kindsOfPlacesReachedByEffects)
    , m_maskOfPlacesReachedByEffects(other.m_maskOfPlacesReachedByEffects)
    , m_filterOfHolders(other.m_filterOfHolders)
    , m_checkedNames(other.m_checkedNames)
    , m_bindingsPastDeadZone(other.m_bindingsPastDeadZone)
    , m_builtinsRead(other.m_builtinsRead)
    , m_tableOfHolders(other.m_tableOfHolders)
    , m_tableOfPlaces(other.m_tableOfPlaces)
    , m_holdersReachedByEffects(other.m_holdersReachedByEffects)
    , m_holders(other.m_holders)
    , m_places(other.m_places)
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

void WalkState::shareWhatIsHeldBy(const WalkState& other)
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
    m_marks.kinds &= programIsWhole;
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
    m_held->m_bindingsPastDeadZone = shared->m_bindingsPastDeadZone;
    m_held->m_builtinsRead = shared->m_builtinsRead;
}

void WalkState::endWhatIsWritten(const Effects& effects)
{
    if (effects.mayRunOtherCode()) {
        endWhatIsNotFresh();
        return;
    }
    if (writesInPlace() || m_held->restsOnHeapsAmong(effects.writes)) {
        heldToWrite().endWhatIsWritten(effects);
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

uint8_t WalkState::Held::bitOfSlot(const ProgramOfState& program, FieldReference field)
{
    if (!program.program || !field.layout)
        return Place::entireHeap;
    const Layout& layout = program.program->at(field.layout);
    PropertyKeyID key;
    if (field.field < layout.fields.size())
        key = layout.fields[field.field].key;
    else if (program.slots)
        key = program.slots->keyOf(*program.program, field);
    return key ? bitOf(program, MemoryLocation::objectField(key)) : Place::entireHeap;
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

ALWAYS_INLINE uint32_t WalkState::Held::findRecordOfHolder(Reference object) const
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

ALWAYS_INLINE uint32_t WalkState::Held::findHolder(Reference object) const
{
    uint32_t position = findRecordOfHolder(object);
    return position != none && !m_holders[position].hasEnded ? position : none;
}

ALWAYS_INLINE auto WalkState::Held::findRecordOfHolderThatHasNotEnded(Reference object) const -> const Holder*
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

uint32_t WalkState::Held::findOrAddHolder(Reference object)
{
    uint32_t position = findRecordOfHolder(object);
    if (position != none) {
        if (m_holders[position].hasEnded) {
            Holder& record = m_holders.mutableAt(position);
            ASSERT(record.firstPlace == none && record.firstElement == none && !record.shape && record.factsOfCell.isEmpty() && !record.isFresh);
            record.hasEnded = false;
            record.layoutsOfSlots = LayoutsOfSlots::None;
            record.kindsOfPlaces = 0;
            record.maskOfPlaces = 0;
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

ALWAYS_INLINE uint32_t WalkState::Held::findRecordOfPlace(uint32_t holder, PlaceKind kind, uint32_t first, uint32_t second) const
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

ALWAYS_INLINE uint32_t WalkState::Held::findPlace(uint32_t holder, PlaceKind kind, uint32_t first, uint32_t second) const
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

void WalkState::Held::endFactsOfCell(uint32_t holder, OptionSet<FactOfCell> ended)
{
    OptionSet<FactOfCell> known = m_holders[holder].factsOfCell;
    if (!known.containsAny(ended))
        return;
    Holder& record = m_holders.mutableAt(holder);
    record.factsOfCell = known - ended;
    if (!ended.containsAll(known))
        return;
    --m_numberOfHoldersWithFactsOfCell;
    if (!record.isFresh)
        --m_numberOfHoldersWithFactsOfCellReachedByEffects;
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
    endFactsOfCell(holder, m_holders[holder].factsOfCell);
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
}

void WalkState::Held::countAmongReachedByEffects(const Holder& holder)
{
    m_numberOfShapesReachedByEffects += !!holder.shape;
    m_numberOfHoldersWithFactsOfCellReachedByEffects += !holder.factsOfCell.isEmpty();
    m_numberOfPlacesReachedByEffects += holder.numberOfPlaces;
    m_numberOfElementsReachedByEffects += holder.numberOfElements;
    m_factsOfCellReachedByEffects.add(holder.factsOfCell);
    m_kindsOfPlacesReachedByEffects |= holder.kindsOfPlaces;
    m_maskOfPlacesReachedByEffects |= holder.maskOfPlaces;
}

void WalkState::Held::countNoLongerAmongReachedByEffects(const Holder& holder)
{
    m_numberOfShapesReachedByEffects -= !!holder.shape;
    m_numberOfHoldersWithFactsOfCellReachedByEffects -= !holder.factsOfCell.isEmpty();
    m_numberOfPlacesReachedByEffects -= holder.numberOfPlaces;
    m_numberOfElementsReachedByEffects -= holder.numberOfElements;
}

void WalkState::Held::listAsReachedByEffects(uint32_t holder)
{
    ASSERT(m_holders[holder].placeAmongReachedByEffects == none);
    m_holders.mutableAt(holder).placeAmongReachedByEffects = m_holdersReachedByEffects.size();
    m_holdersReachedByEffects.append(holder);
}

void WalkState::Held::takeOffListOfReachedByEffects(uint32_t holder)
{
    uint32_t place = std::exchange(m_holders.mutableAt(holder).placeAmongReachedByEffects, none);
    uint32_t last = m_holdersReachedByEffects.takeLast();
    if (last == holder)
        return;
    m_holdersReachedByEffects[place] = last;
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
    OptionSet<FactOfCell> facts;
    uint16_t kindsOfPlaces = 0;
    uint64_t maskOfPlaces = 0;
    for (size_t index = 0; index < m_holdersReachedByEffects.size();) {
        uint32_t holder = m_holdersReachedByEffects[index];
        functor(holder);
        const Holder& record = m_holders[holder];
        if (record.shape || !record.factsOfCell.isEmpty() || record.firstPlace != none || record.firstElement != none) {
            facts.add(record.factsOfCell);
            kindsOfPlaces |= record.kindsOfPlaces;
            maskOfPlaces |= record.maskOfPlaces;
            ++index;
        } else
            endHolder(holder);
    }
    m_factsOfCellReachedByEffects = facts;
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
    m_factsOfCellReachedByEffects = { };
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
        if (!holder.isFresh && !holder.shape && holder.factsOfCell.isEmpty() && holder.firstPlace == none && holder.firstElement == none)
            continue;
        m_holders.append(holder);
        m_filterOfHolders |= bitInFilterOfHolders(holder.object);
        if (!holder.isFresh) {
            listAsReachedByEffects(position);
            m_factsOfCellReachedByEffects.add(holder.factsOfCell);
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

void WalkState::Held::noteFactsOfCell(Reference object, OptionSet<FactOfCell> facts)
{
    ASSERT(object);
    if (facts.isEmpty())
        return;
    uint32_t holder = findOrAddHolder(object);
    OptionSet<FactOfCell> known = m_holders[holder].factsOfCell;
    if (known.containsAll(facts))
        return;
    Holder& record = m_holders.mutableAt(holder);
    record.factsOfCell = known | facts;
    if (known.isEmpty())
        ++m_numberOfHoldersWithFactsOfCell;
    if (record.isFresh)
        return;
    if (known.isEmpty())
        ++m_numberOfHoldersWithFactsOfCellReachedByEffects;
    m_factsOfCellReachedByEffects.add(facts);
    if (m_lists) [[unlikely]]
        listHolder(holder);
}

void WalkState::Held::endFactsOfCellOfHolderOf(Reference object, OptionSet<FactOfCell> ended)
{
    if (uint32_t holder = findHolder(object); holder != none)
        endFactsOfCell(holder, ended);
}

OptionSet<FactOfCell> WalkState::Held::factsOfCellOfHolderOf(Reference object) const
{
    const Holder* record = findRecordOfHolderThatHasNotEnded(object);
    return record ? record->factsOfCell : OptionSet<FactOfCell>();
}

void WalkState::Held::noteOrderWord(Reference object, std::optional<uint32_t> orderWord)
{
    ASSERT(object);
    if (!orderWord) {
        if (uint32_t holder = findHolder(object); holder != none)
            endFactsOfCell(holder, FactOfCell::OrderWordIsKnown);
        return;
    }
    noteFactsOfCell(object, { FactOfCell::NotFrozen, FactOfCell::NoFieldOutsideSlot, FactOfCell::NoExtraProperties, FactOfCell::OrderWordIsKnown });
    uint32_t holder = findHolder(object);
    if (m_holders[holder].orderWord != *orderWord)
        m_holders.mutableAt(holder).orderWord = *orderWord;
}

std::optional<uint32_t> WalkState::Held::orderWordOf(Reference object) const
{
    if (!m_numberOfHoldersWithFactsOfCell)
        return std::nullopt;
    const Holder* record = findRecordOfHolderThatHasNotEnded(object);
    if (!record || !record->factsOfCell.contains(FactOfCell::OrderWordIsKnown))
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

bool WalkState::Held::letLeaveIfFresh(Reference object)
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
        if (place.value && (place.kind == PlaceKind::GuardOfStores || place.kind == PlaceKind::GuardOfReads) && classOfGuardedSide(place.first) == classLayout)
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
    m_lists->holders.fill(none, Lists::listsOfHolders);
    m_lists->holders.grow(Lists::notListed, Lists::listsOfHolders + m_holders.size() * Lists::listsOfHolders);
    for (uint32_t holder : m_holdersReachedByEffects)
        listWhatHangsOn(holder);
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
    for (FactOfCell fact : record.factsOfCell)
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
    if (!record.hasEnded && !record.isFresh && !record.shape && record.factsOfCell.isEmpty() && !record.numberOfPlaces && !record.numberOfElements)
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
    auto goThroughPlaces = [&](uint32_t firstList, uint64_t written, const auto& mayEnd) {
        m_lists->goThroughPlacesUnder(firstList, written, [&](uint32_t position) {
            const Place& place = m_places[position];
            uint32_t holder = place.holder;
            if (!place.value || m_holders[holder].isFresh)
                return false;
            if (!(written & place.mask()) || !mayEnd(place))
                return true;
            endPlace(position);
            holdersThatLostSomething.append(holder);
            return false;
        });
    };
    auto holderIsWritten = [&](const Place& place) {
        return isWritten(place.holder);
    };

    if (ends.shapes) {
        goThroughHolders(Lists::listOfShapes, [](const Holder& record) {
            return !!record.shape;
        }, [&](uint32_t holder) {
            endShape(holder);
        });
    }
    for (FactOfCell fact : ends.facts) {
        goThroughHolders(Lists::listOf(fact), [&](const Holder& record) {
            return record.factsOfCell.contains(fact);
        }, [&](uint32_t holder) {
            endFactsOfCell(holder, ends.facts);
        });
    }
    goThroughPlaces(Lists::firstListOfSlots, ends.slots, holderIsWritten);
    goThroughPlaces(Lists::firstListOfEnvironmentFields, ends.environmentFields, holderIsWritten);
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
    if (ends.elements) {
        goThroughHolders(Lists::listOfElements, [](const Holder& record) {
            return !!record.numberOfElements;
        }, [&](uint32_t holder) {
            endPlacesOf(holder, ListOfPlaces::Elements, [](const Place&) {
                return true;
            });
        });
    }
    for (uint32_t holder : holdersThatLostSomething)
        endHolderIfNothingHangsOn(holder);
}

void WalkState::Held::invalidateAliasedSlotsByLists(uint32_t own, uint8_t bit, FieldReference field, NOESCAPE const ScopedLambda<bool(Reference other)>& mayBeSameObject)
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
        m_lists->goThroughPlaces(list, Lists::entryByBit, [&](uint32_t position) {
            return endIf(position, isSameField);
        });
    };
    if (bit == Place::entireHeap) {
        for (uint32_t list = 0; list < Lists::listsOfOneHeap; ++list)
            endSameFieldIn(Lists::firstListOfSlots + list);
    } else {
        endSameFieldIn(Lists::firstListOfSlots + bit);
        endSameFieldIn(Lists::firstListOfSlots + Place::entireHeap);
    }

    for (uint32_t byLayout = 0; byLayout < Lists::listsByLayout; ++byLayout) {
        if (m_lists->places[Lists::firstListByLayout + byLayout] == none || m_lists->places[Lists::firstLayoutOfList + byLayout] == layout)
            continue;
        bool someStay = false;
        uint32_t layoutOfList = none;
        m_lists->goThroughPlaces(Lists::firstListByLayout + byLayout, Lists::entryByLayout, [&](uint32_t position) {
            bool stays = endIf(position, [&](const Place& place) {
                return place.first != layout;
            });
            if (stays) {
                uint32_t layoutOfPlace = m_places[position].first;
                layoutOfList = !someStay || layoutOfList == layoutOfPlace ? layoutOfPlace : Lists::severalLayouts;
                someStay = true;
            }
            return stays;
        });
        if (m_lists->places[Lists::firstLayoutOfList + byLayout] != layoutOfList)
            m_lists->places.mutableAt(Lists::firstLayoutOfList + byLayout) = layoutOfList;
    }
    for (uint32_t holder : holdersThatLostSomething)
        endHolderIfNothingHangsOn(holder);
}

void WalkState::Held::invalidateAliasedSlots(const ProgramOfState& program, Reference object, FieldReference field, NOESCAPE const ScopedLambda<bool(Reference other)>& mayBeSameObject)
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
        invalidateAliasedSlotsByLists(own, bitOfSlot(program, field), field, mayBeSameObject);
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
        || (m_numberOfHoldersWithFactsOfCell && !factsOfCellEndedBy(writes).isEmpty());
}

void WalkState::Held::endWhatIsWritten(const Effects& effects)
{
    if (effects.mayRunOtherCode()) {
        endWhatIsNotFresh();
        return;
    }

    const LocationMasks& writes = effects.writes;
    bool endsShapes = writes.contains(AbstractHeap::ShapeWords) && m_numberOfShapes;
    bool endsPlaces = writes.containsAny(heapsUnderPlacesInWalkState) && m_numberOfPlaces;
    OptionSet<FactOfCell> factsEnded = m_numberOfHoldersWithFactsOfCell ? factsOfCellEndedBy(writes) : OptionSet<FactOfCell>();
    if (!endsShapes && !endsPlaces && factsEnded.isEmpty())
        return;

    uint64_t slotsWritten = writes.maskOf(AbstractHeap::ObjectFields);
    uint64_t variablesWritten = writes.maskOf(AbstractHeap::ModuleVariables);
    uint64_t environmentFieldsWritten = writes.maskOf(AbstractHeap::EnvironmentFields);
    uint64_t guardsWritten = writes.maskOf(AbstractHeap::MemberGuards);
    bool endsElements = endsPlaces && writes.containsAny(heapsUnderElementsInWalkState);
    bool reachesSomething = (endsShapes && m_numberOfShapesReachedByEffects)
        || (m_numberOfHoldersWithFactsOfCellReachedByEffects && m_factsOfCellReachedByEffects.containsAny(factsEnded))
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
        bool endsElementsOfHolder = endsElements && record.firstElement != none;
        bool endsFactsOfHolder = record.factsOfCell.containsAny(factsEnded);
        uint32_t numberOfPlaces = record.numberOfPlaces;
        if (endsShapes && record.shape)
            endShape(holder);
        if (endsFactsOfHolder)
            endFactsOfCell(holder, factsEnded);
        if (endsPlacesOfHolder) {
            noteRecordsGoneThrough(numberOfPlaces, placesFoundWithoutTable);
            uint16_t kindsLeft = 0;
            uint64_t maskLeft = 0;
            endPlacesOf(holder, ListOfPlaces::AllButElements, [&](const Place& place) {
                if (writtenOfKind(place.kind) & place.mask())
                    return true;
                kindsLeft |= 1 << static_cast<unsigned>(place.kind);
                maskLeft |= place.mask();
                return false;
            });
            Holder& changed = m_holders.mutableAt(holder);
            changed.kindsOfPlaces = kindsLeft;
            changed.maskOfPlaces = maskLeft;
        }
        if (endsElementsOfHolder)
            endPlacesOf(holder, ListOfPlaces::Elements, endsAlways);
    };
    if (reachesSomething && goesByLists()) [[unlikely]] {
        endByLists({ .shapes = endsShapes, .elements = endsElements, .facts = factsEnded, .slots = slotsWritten, .variables = variablesWritten,
            .environmentFields = environmentFieldsWritten, .guards = guardsWritten });
    } else if (reachesSomething)
        forEachHolderReachedByEffects(endWhatHangsOn);

    if (endsPlaces && writes.containsAny(heapsUnderElementsOutsideObjectsInWalkState)) {
        for (uint32_t holder = std::exchange(m_firstFreshHolderWithElements, none); holder != none;) {
            Holder& record = m_holders.mutableAt(holder);
            record.isListedAsFreshWithElements = false;
            uint32_t next = std::exchange(record.nextFreshWithElements, none);
            if (!record.hasEnded)
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
        endHolder(m_holdersReachedByEffects.last());
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
    m_numberOfHoldersWithFactsOfCell = 0;
    m_numberOfFreshHolders = 0;
    m_numberOfShapesReachedByEffects = 0;
    m_numberOfHoldersWithFactsOfCellReachedByEffects = 0;
    m_numberOfPlacesReachedByEffects = 0;
    m_numberOfElementsReachedByEffects = 0;
    m_factsOfCellReachedByEffects = { };
    m_kindsOfPlacesReachedByEffects = 0;
    m_maskOfPlacesReachedByEffects = 0;
    m_filterOfHolders = 0;
}

void WalkState::Held::clear()
{
    if (!holdsNoRecord())
        dropAllRecords();
    m_checkedNames.clear();
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
    if (variable.flags.containsAny({ VariableFlags::IsReplaceable, VariableFlags::HoldsMock, VariableFlags::OriginalMayRun }))
        return false;
    return loaderEntersBodyAtMostOnce(program.at(variable.module));
}

void SharedSetOfNumbers::keepMembersOfWhereSomeAreHeld(const SharedSetOfNumbers& other)
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
    if (m_marks.turnInWhichNothingWasDone != other.m_marks.turnInWhichNothingWasDone)
        m_marks.turnInWhichNothingWasDone = ((m_marks.turnInWhichNothingWasDone ^ other.m_marks.turnInWhichNothingWasDone) & ~placesWereWrittenInTurn) ? 0 : m_marks.turnInWhichNothingWasDone | placesWereWrittenInTurn;
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
            OptionSet<FactOfCell> factsKnownHereAlone = record.factsOfCell - same.factsOfCell;
            if (record.orderWord != same.orderWord)
                factsKnownHereAlone.add(FactOfCell::OrderWordIsKnown);
            if (record.shape != same.shape)
                endShape(holder);
            endFactsOfCell(holder, factsKnownHereAlone);
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
