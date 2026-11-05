/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCode.h"
#include "AOTEffects.h"
#include "AOTLayout.h"
#include "AOTSharedArray.h"
#include "AOTWalkKnownAnswers.h"
#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Typed {

class SlotPlan;

// A set of small numbers. A copy shares everything with the original, and a meet looks at the parts that the two do not share: each costs what was added since they parted.
class SharedSetOfNumbers {
public:
    bool isEmpty() const { return !m_size; }
    bool contains(uint32_t number) const
    {
        uint32_t word = number / bitsOfWord;
        return word < m_words.size() && (m_words[word] >> number % bitsOfWord & 1);
    }
    // False: it was a member.
    bool add(uint32_t number)
    {
        uint32_t word = number / bitsOfWord;
        uint64_t bit = 1ull << number % bitsOfWord;
        if (m_words.size() <= word)
            m_words.grow(0, word + 1);
        if (m_words[word] & bit)
            return false;
        m_words.mutableAt(word) |= bit;
        ++m_size;
        return true;
    }
    void keepMembersOf(const SharedSetOfNumbers&);
    void clear()
    {
        if (!m_words.isEmpty())
            m_words.clear();
        m_size = 0;
    }
    void swap(SharedSetOfNumbers& other)
    {
        m_words.swap(other.m_words);
        std::swap(m_size, other.m_size);
    }

private:
    static constexpr uint32_t bitsOfWord = 64;

    SharedArray<uint64_t, 2> m_words;
    uint32_t m_size { 0 };
};

// What no effect ends is a set that only grows along a path and is cut at joins. The body of a big module notes tens of thousands of members, and has as many joins.
// So the walk numbers the members as they are noted, and a state holds a SharedSetOfNumbers.
// The states of one walk share this. The walker empties it in front of each unit.
class NumbersOfWalkStates {
public:
    struct CheckedName {
        Reference value;
        Reference name;
        // Of the same value, noted on another path: its number and one more. 0: there is none.
        uint32_t older { 0 };
    };

    void clear()
    {
        m_checkedNames.shrink(0);
        m_newestCheckedNames.shrink(0);
        if (!m_numbersOfBindings.isEmpty())
            m_numbersOfBindings.clear();
        if (!m_numbersOfBuiltins.isEmpty())
            m_numbersOfBuiltins.clear();
    }

    uint32_t addCheckedName(Reference value, Reference name)
    {
        uint32_t index = value.id().index();
        if (size_t size = m_newestCheckedNames.size(); size <= index) {
            m_newestCheckedNames.grow(index + 1);
            std::ranges::fill(m_newestCheckedNames.mutableSpan().subspan(size), 0);
        }
        uint32_t number = m_checkedNames.size();
        m_checkedNames.append(CheckedName { value, name, m_newestCheckedNames[index] });
        m_newestCheckedNames[index] = number + 1;
        return number;
    }
    // Its number and one more. 0: none was ever noted.
    uint32_t newestCheckedNameOf(Reference value) const
    {
        if (!value.isRecord() || value.id().index() >= m_newestCheckedNames.size())
            return 0;
        return m_newestCheckedNames[value.id().index()];
    }
    const CheckedName& checkedName(uint32_t number) const { return m_checkedNames[number]; }

    // A binding is a variable, or a field of the environment that a record of Code names. Both are numbered in one row.
    uint32_t numberOf(VariableID variable) { return numberOfBinding(keyOf(variable)); }
    std::optional<uint32_t> findNumberOf(VariableID variable) const { return findNumberOfBinding(keyOf(variable)); }
    uint32_t numberOf(Reference environment, FieldReference field) { return numberOfBinding(keyOf(environment, field)); }
    std::optional<uint32_t> findNumberOf(Reference environment, FieldReference field) const { return findNumberOfBinding(keyOf(environment, field)); }

    uint32_t numberOf(BuiltinID builtin) { return m_numbersOfBuiltins.add(builtin.index() + 1, m_numbersOfBuiltins.size()).iterator->value; }
    std::optional<uint32_t> findNumberOf(BuiltinID builtin) const
    {
        auto found = m_numbersOfBuiltins.find(builtin.index() + 1);
        return found == m_numbersOfBuiltins.end() ? std::nullopt : std::optional(found->value);
    }

private:
    static uint64_t keyOf(VariableID variable) { return static_cast<uint64_t>(variable.index()) + 1; }
    // An environment has one layout, so the field's number says which field it is.
    static uint64_t keyOf(Reference environment, FieldReference field) { return (static_cast<uint64_t>(environment.id().index()) + 1) << 32 | field.field; }
    uint32_t numberOfBinding(uint64_t key) { return m_numbersOfBindings.add(key, m_numbersOfBindings.size()).iterator->value; }
    std::optional<uint32_t> findNumberOfBinding(uint64_t key) const
    {
        auto found = m_numbersOfBindings.find(key);
        return found == m_numbersOfBindings.end() ? std::nullopt : std::optional(found->value);
    }

    Vector<CheckedName> m_checkedNames;
    // By record of Code.
    Vector<uint32_t> m_newestCheckedNames;
    HashMap<uint64_t, uint32_t> m_numbersOfBindings;
    HashMap<uint32_t, uint32_t> m_numbersOfBuiltins;
};

// What may be known of the cell of one object beyond its shape. Unlike the shape word, none of it is written once.
enum class FactOfCell : uint8_t {
    // It is neither frozen nor sealed nor closed to new properties: orderWordIntegrityBits are clear. The tables declare whatever raises one of them as a write of ShapeWords.
    NotFrozen = 1 << 0,
    // Every property that the layout has a field for lies in its slot, so an empty slot says that the object has no such property: orderWordHasFieldOutsideSlot is clear.
    // Whatever moves a field out of its slot is declared as a write of NamedProperties.
    NoFieldOutsideSlot = 1 << 1,
    // It stands below the prototype object that its layout names. Whatever lays it below another writes PrototypeLinks.
    HasLayoutPrototype = 1 << 2,
    // WalkState::orderWordOf() has the whole order word, which has no flag: which keys the object lists, one behind the other, and that it has no property but those.
    // It ends with either of the first two facts, and with a write of OrderWords.
    OrderWordIsKnown = 1 << 3,
    // No fact, and no access leans on it: `this` of a function that `new` enters is likely to be as its allocation left it. It says where a test of the whole word pays, and ends where OrderWordIsKnown would.
    IsLikelyAsAllocated = 1 << 4,
    // No fact either: what `new` has handed back, and nothing has written an order word since, likely lists what its constructor creates and nothing else.
    IsLikelyAsConstructed = 1 << 5,
    // It has no property of its own but under the names that its layout has fields for, and no element: orderWordHasExtraProperties is clear. Whatever lays one there is declared as a write of NamedProperties.
    NoExtraProperties = 1 << 6,
    // shapeWordIsUnmarked is set: its birth is over, and every field of the birth lies filled in its slot. More than NoFieldOutsideSlot, which holds of a new object from its allocation on.
    // Whatever takes a field out of its slot or empties one is declared as a write of NamedProperties or of ShapeWords.
    BearsNoMark = 1 << 7,
};

// What holds at the present point of the walk beyond the proven type of each value, which Code has.
// An entry lives until an operation's declared effects write what it rests on. Nothing else ends one, and nothing brings one back.
// On an edge the state is copied. At a join only what is equal on every incoming edge is kept. At the head of a loop what the loop may write is dropped first.
// All but the answers lies behind one pointer: a state that holds nothing has none, a move is a word, and a copy shares it with the original until either writes.
class WalkState {
public:
    WalkState() = default;
    WalkState(const WalkState& other)
        : m_held(other.holdsNothing() ? nullptr : other.m_held)
        , m_program(other.m_program)
    {
        if (m_held)
            ++m_held->numberOfOwners;
        if (!other.m_answers.isEmpty())
            m_answers = other.m_answers;
        m_turnInWhichNothingWasDone = other.m_turnInWhichNothingWasDone;
    }
    WalkState(WalkState&& other)
        : m_held(std::exchange(other.m_held, nullptr))
        , m_program(other.m_program)
    {
        if (!other.m_answers.isEmpty())
            m_answers.swap(other.m_answers);
        m_turnInWhichNothingWasDone = other.m_turnInWhichNothingWasDone;
    }
    ~WalkState() { release(m_held); }

    WalkState& operator=(const WalkState& other)
    {
        if (this == &other)
            return *this;
        m_program = other.m_program;
        if (m_held != other.m_held)
            shareWhatIsHeldBy(other);
        if (!m_answers.isEmpty() || !other.m_answers.isEmpty())
            m_answers = other.m_answers;
        m_turnInWhichNothingWasDone = other.m_turnInWhichNothingWasDone;
        return *this;
    }

    // `other` is left with what this state held.
    WalkState& operator=(WalkState&& other)
    {
        m_program = other.m_program;
        std::swap(m_held, other.m_held);
        if (!m_answers.isEmpty() || !other.m_answers.isEmpty())
            m_answers.swap(other.m_answers);
        std::swap(m_turnInWhichNothingWasDone, other.m_turnInWhichNothingWasDone);
        return *this;
    }

    // With it a slot, a variable or a field of an environment rests on its own bit of its heap. Without it, on the whole heap.
    void setProgram(const Program& program) { m_program.program = &program; }
    // It has the keys of the fields that Link appended, which no record of the program lists.
    void setSlotPlan(const SlotPlan& slots) { m_program.slots = &slots; }
    // Without it nothing is noted of checked names and of dead zones.
    void setNumbers(NumbersOfWalkStates& numbers) { m_program.numbers = &numbers; }
    // Without it nothing is noted beyond the records of the state itself: walk/AOTWalkKnown.h.
    void setAnswers(AnswersOfWalk& answers) { m_answers.setTable(answers); }
    KnownAnswers& answers() { return m_answers; }
    const KnownAnswers& answers() const { return m_answers; }

    // ---- An object has passed a test of its shape, or was made here: until something may change shapes, its type is `proven`, narrower than its value's own.
    void noteShape(Reference object, TypeID proven)
    {
        if (writesInPlace() || shapeOf(object) != proven)
            heldToWrite().noteShape(object, proven);
    }
    // Null: no more is known than the value's own type.
    TypeID shapeOf(Reference object) const { return m_held && m_held->m_numberOfShapes ? m_held->shapeOfHolderOf(object) : TypeID(); }

    // ---- An object was made here and nothing but this code has it yet. No call, no suspension and no store through another value can change it:
    // its shape and its places outlive all three ways below in which knowledge ends, until it leaves. Reads of it are answered from here, and if nothing else
    // comes to need it the sweep drops it with its stores. It is always built: nothing is ever made up for later.
    void noteFresh(Reference object)
    {
        if (writesInPlace() || !isFresh(object)) {
            heldToWrite().noteFresh(object);
            m_answers.holderIsFreshAgain(object);
        }
    }
    bool isFresh(Reference object) const { return m_held && m_held->m_numberOfFreshHolders && m_held->holderIsFresh(object); }
    // False: isFresh() says no to everything.
    bool holdsFreshObjects() const { return m_held && m_held->m_numberOfFreshHolders; }
    // It was stored, passed, returned, captured or thrown. From here on it is an object like any other.
    void markEscaped(Reference object)
    {
        if (m_held && m_held->m_numberOfFreshHolders && (writesInPlace() || m_held->holderIsFresh(object)) && heldToWrite().letLeaveIfFresh(object))
            m_answers.holderHasLeft(object);
    }

    // ---- A value has come through a check that throws for a part of it: where the walk stands, `name` is the same value under the narrower type. No effect ends that: a value is what it is.
    // Whoever notes one has followed the names to the last, so a state has at most one for a value.
    void noteCheckedName(Reference value, Reference name)
    {
        RELEASE_ASSERT(m_program.numbers && !checkedNameOf(value));
        heldToWrite().m_checkedNames.add(m_program.numbers->addCheckedName(value, name));
    }
    // Null: it has come through none.
    Reference checkedNameOf(Reference value) const
    {
        if (!hasCheckedNames())
            return { };
        for (uint32_t place = m_program.numbers->newestCheckedNameOf(value); place; place = m_program.numbers->checkedName(place - 1).older) {
            if (m_held->m_checkedNames.contains(place - 1))
                return m_program.numbers->checkedName(place - 1).name;
        }
        return { };
    }
    bool hasCheckedNames() const { return m_held && !m_held->m_checkedNames.isEmpty(); }

    // ---- A variable of a body that runs once was seen filled where the walk stands: by a store of what is no empty word, or by a check that a read of it has passed. No effect empties it again.
    void noteEndOfDeadZone(VariableID variable)
    {
        RELEASE_ASSERT(m_program.numbers);
        noteBindingPastDeadZone(m_program.numbers->numberOf(variable));
    }
    bool isPastDeadZone(VariableID variable) const
    {
        if (!m_held || m_held->m_bindingsPastDeadZone.isEmpty())
            return false;
        std::optional<uint32_t> number = m_program.numbers->findNumberOf(variable);
        return number && m_held->m_bindingsPastDeadZone.contains(*number);
    }
    // The same of a field of an environment that a record of this code names. An environment is made with its fields empty, each turn of a loop makes its own, and nothing empties a field again.
    void noteEndOfDeadZone(Reference environment, FieldReference field)
    {
        RELEASE_ASSERT(m_program.numbers);
        if (environment.isRecord())
            noteBindingPastDeadZone(m_program.numbers->numberOf(environment, field));
    }
    bool isPastDeadZone(Reference environment, FieldReference field) const
    {
        if (!m_held || m_held->m_bindingsPastDeadZone.isEmpty() || !environment.isRecord())
            return false;
        std::optional<uint32_t> number = m_program.numbers->findNumberOf(environment, field);
        return number && m_held->m_bindingsPastDeadZone.contains(*number);
    }

    // ---- The program has read the member of a built-in on every way to where the walk stands: InitializeBuiltin. Only the first read does anything, and no effect undoes it.
    void noteReadOfBuiltin(BuiltinID location)
    {
        RELEASE_ASSERT(m_program.numbers);
        uint32_t number = m_program.numbers->numberOf(location);
        if (!m_held || !m_held->m_builtinsRead.contains(number))
            heldToWrite().m_builtinsRead.add(number);
    }
    bool hasReadBuiltin(BuiltinID location) const
    {
        if (!m_held || m_held->m_builtinsRead.isEmpty())
            return false;
        std::optional<uint32_t> number = m_program.numbers->findNumberOf(location);
        return number && m_held->m_builtinsRead.contains(*number);
    }

    // ---- The cell of an object was made here, or has passed a test of its order word: until something may write what the fact rests on.
    void noteFactsOfCell(Reference object, OptionSet<FactOfCell> facts)
    {
        if (writesInPlace() || !factsOfCell(object).containsAll(facts))
            heldToWrite().noteFactsOfCell(object, facts);
    }
    OptionSet<FactOfCell> factsOfCell(Reference object) const { return m_held && m_held->m_numberOfHoldersWithFactsOfCell ? m_held->factsOfCellOfHolderOf(object) : OptionSet<FactOfCell>(); }
    // With FactOfCell::OrderWordIsKnown, NotFrozen and NoFieldOutsideSlot. Nothing: the word is known no longer.
    void noteOrderWord(Reference object, std::optional<uint32_t> orderWord)
    {
        bool isNoted = orderWord ? orderWordOf(object) == orderWord && factsOfCell(object).containsAll({ FactOfCell::NotFrozen, FactOfCell::NoFieldOutsideSlot }) : !factsOfCell(object).contains(FactOfCell::OrderWordIsKnown);
        if (!isNoted)
            heldToWrite().noteOrderWord(object, orderWord);
    }
    std::optional<uint32_t> orderWordOf(Reference object) const { return m_held ? m_held->orderWordOf(object) : std::nullopt; }

    // ---- A test of the guards of a class has passed: a store under the key into an object that stands below the prototype object of the class meets neither a setter nor a property that is read only.
    // Until something may write MemberGuards.
    void noteStoresPassAbove(LayoutID classLayout, PropertyKeyID key, Reference test)
    {
        if (writesInPlace() || !noteChangesNothing(valueOfPlace(PlaceKind::GuardOfStores, Reference(), classLayout.index(), key.bits()), test))
            heldToWrite().noteStoresPassAbove(classLayout, key, test);
    }
    bool storesPassAbove(LayoutID classLayout, PropertyKeyID key) const { return !!valueOfPlace(PlaceKind::GuardOfStores, Reference(), classLayout.index(), key.bits()); }
    // The same of the guard that reads ask: under the key stands above such an object what the records of the classes declare there, as declared, and nothing where they declare nothing; and no object of the class hides it.
    void noteGuardOfReadsHolds(LayoutID classLayout, PropertyKeyID key, Reference test)
    {
        if (writesInPlace() || !noteChangesNothing(valueOfPlace(PlaceKind::GuardOfReads, Reference(), classLayout.index(), key.bits()), test))
            heldToWrite().noteGuardOfReadsHolds(classLayout, key, test);
    }
    bool guardOfReadsHolds(LayoutID classLayout, PropertyKeyID key) const { return !!valueOfPlace(PlaceKind::GuardOfReads, Reference(), classLayout.index(), key.bits()); }

    // ---- What a place was last seen to hold, by a store or by a read.
    void noteSlot(Reference object, FieldReference field, Reference value)
    {
        if (writesInPlace() || !noteChangesNothing(slot(object, field), value))
            heldToWrite().noteSlot(m_program, object, field, value);
    }
    void noteVariable(VariableID variable, Reference value)
    {
        if (writesInPlace() || !noteChangesNothing(this->variable(variable), value))
            heldToWrite().noteVariable(m_program, variable, value);
    }
    void noteEnvironmentField(Reference environment, FieldReference field, Reference value)
    {
        if (writesInPlace() || !noteChangesNothing(environmentField(environment, field), value))
            heldToWrite().noteEnvironmentField(m_program, environment, field, value);
    }
    void noteElement(Reference object, Reference index, Reference value)
    {
        if (writesInPlace() || !noteChangesNothing(element(object, index), value))
            heldToWrite().noteElement(object, index, value);
    }
    // Null: not known.
    Reference slot(Reference object, FieldReference field) const { return valueOfPlace(PlaceKind::Slot, object, field.layout.index(), field.field); }
    Reference variable(VariableID variable) const { return valueOfPlace(PlaceKind::Variable, Reference(), variable.index(), 0); }
    Reference environmentField(Reference environment, FieldReference field) const { return valueOfPlace(PlaceKind::EnvironmentField, environment, field.layout.index(), field.field); }
    Reference element(Reference object, Reference index) const { return valueOfPlace(PlaceKind::Element, object, index.bits(), 0); }

    // A store to a slot of one object ends what is known of the same field of every other object that may be the same one: mayBeSameObject() is asked of the two types.
    void invalidateAliasedSlots(Reference object, FieldReference field, NOESCAPE const ScopedLambda<bool(Reference other)>& mayBeSameObject)
    {
        if (m_held && m_held->m_numberOfPlaces)
            heldToWrite().invalidateAliasedSlots(m_program, object, field, mayBeSameObject);
    }

    // ---- Whether a turn of a loop may still begin again: AOTWalkLoopsBuiltTwice.cpp.
    // On every way to here the turn has run nothing since the head but what may be run again unseen: Effects::mayBeRunAgainUnseen(). A state that nobody has told so says no.
    // THE STATE NAMES THE TURN: the number of the typed copy whose head it has come from (Walker::WayIntoClosedCopy::numberOfTurn). A typed loop inside a typed loop has a number of its own, so what leaves it, by whatever road,
    // says nothing of the turn of the loop around.
    bool nothingWasDoneInTurn(uint32_t numberOfTurn) const { return numberOfTurn && m_turnInWhichNothingWasDone == numberOfTurn; }
    void noteHeadOfTurn(uint32_t numberOfTurn) { m_turnInWhichNothingWasDone = numberOfTurn; }
    void noteSomethingWasDoneInTurn() { m_turnInWhichNothingWasDone = 0; }
    void noteWhatOperationDoes(const Effects& effects)
    {
        if (m_turnInWhichNothingWasDone && !effects.mayBeRunAgainUnseen())
            m_turnInWhichNothingWasDone = 0;
    }

    // ---- The three ways in which knowledge ends.
    void invalidate(const Effects& effects)
    {
        noteWhatOperationDoes(effects);
        // What only reads ends nothing but what a collection ends.
        if (!effects.writes.isEmpty() || effects.mayRunOtherCode()) {
            if (!holdsNoRecord())
                endWhatIsWritten(effects);
            m_answers.endWhatIsWritten(effects, *this);
        }
    }
    // AOTWalkKnownWriters.cpp. Of an operation that runs no other code. Whatever hangs on a holder and rests on a heap of perObjectHeaps ends only where `mayBeWritten` says so of the holder, a guard only where `mayEndGuard` says so of its class.
    void invalidate(const Effects&, NOESCAPE const ScopedLambda<bool(Reference holder)>& mayBeWritten, NOESCAPE const ScopedLambda<bool(LayoutID classOfGuard)>& mayEndGuard);
    // A call of code that nobody has seen, a suspension.
    void invalidateHeap()
    {
        m_turnInWhichNothingWasDone = 0;
        if (!holdsNoRecord())
            endWhatIsNotFresh();
        m_answers.endWhatIsNotFresh(*this);
    }
    void clear()
    {
        m_turnInWhichNothingWasDone = 0;
        if (m_held)
            dropWhatIsHeld();
        m_answers.clear();
    }

    // Keeps what `other` holds too, equal. An object stays fresh if it is fresh in both. The state of a join is the first incoming edge's, met with each of the others.
    void meetWith(const WalkState& other);
    // AOTWalkKnownHandlers.cpp. Of the objects that are not fresh: each place and element whose value the lambda names is known no longer.
    void endPlacesHolding(NOESCAPE const ScopedLambda<bool(Reference value)>& isEnded);

    // That nothing was done is something that the state knows: whoever passes over a meet with an empty state may not pass over this.
    bool isEmpty() const { return !m_turnInWhichNothingWasDone && (!m_held || m_held->isEmpty()) && m_answers.isEmpty(); }

private:
    // WHAT EACH STEP COSTS. A question and a note: the same whatever the state holds. A mark of freshness set or taken away: the same however much is known of the object.
    // Whatever ends knowledge goes through the holders that are not fresh, and never looks at what hangs on a fresh one. WHERE MUCH IS KNOWN IT GOES THROUGH WHAT MAY END AND THROUGH NOTHING ELSE: Held::Lists.
    // A copy: nothing, and the first write behind it as many holders as are not fresh. A meet: as much as either state has written since the two were one.
    static constexpr uint32_t none = UINT32_MAX;
    // A list of no more records is searched one by one. A longer one has a table.
    static constexpr size_t holdersFoundWithoutTable = 8;
    static constexpr size_t placesFoundWithoutTable = 16;
    // A state that has gone through more records one by one, in rows too long to be searched without a table, makes Held::Lists: by then it has spent what they cost.
    static constexpr uint32_t recordsGoneThroughWithoutLists = 4096;
    // To list a record costs about as much as to go through it so many times.
    static constexpr uint32_t timesGoneThroughForOneListing = 16;

    enum class PlaceKind : uint8_t { Slot, Variable, EnvironmentField, Element, GuardOfStores, GuardOfReads };
    enum class LayoutsOfSlots : uint8_t { None, One, Several };

    // Whatever something is known of: an object, an environment, or no object at all, which the variables hang on.
    struct Holder {
        Reference object;
        TypeID shape;
        // Through Place::next. The elements have a list of their own. A list may still hold places that have ended.
        uint32_t firstPlace { none };
        uint32_t firstElement { none };
        // What the slots in the list lie under, and some that have ended.
        uint32_t layoutOfSlots { 0 };
        LayoutsOfSlots layoutsOfSlots { LayoutsOfSlots::None };
        // Of the places in the first list, and of some that have ended: a bit for each kind, and all their masks together.
        uint16_t kindsOfPlaces { 0 };
        OptionSet<FactOfCell> factsOfCell;
        // With FactOfCell::OrderWordIsKnown.
        uint32_t orderWord { 0 };
        bool isFresh { false };
        bool hasEnded { false };
        bool isListedAsFreshWithElements { false };
        // In m_holdersReachedByEffects.
        uint32_t placeAmongReachedByEffects { none };
        // The list that begins with m_firstFreshHolderWithElements.
        uint32_t nextFreshWithElements { none };
        // Of each list: the places that have not ended.
        uint32_t numberOfPlaces { 0 };
        uint32_t numberOfElements { 0 };
        uint64_t maskOfPlaces { 0 };
    };
    struct Place {
        static constexpr uint8_t entireHeap = 64;

        uint64_t mask() const { return bit < entireHeap ? uint64_t { 1 } << bit : LocationMasks::entireHeap; }

        PlaceKind kind;
        // Within the heap of its kind.
        uint8_t bit { entireHeap };
        // It stands in a list of its holder.
        bool isListed { true };
        // In m_holders.
        uint32_t holder { none };
        // Slot, EnvironmentField: layout and field. Variable: the id. Element: the bits of the index's Reference. GuardOfStores, GuardOfReads: class and key.
        uint32_t first { 0 };
        uint32_t second { 0 };
        // Null: the place has ended.
        Reference value;
        uint32_t next { none };
    };
    // A state that has the program keeps it when one that never had it is assigned to it.
    struct ProgramOfState {
        ProgramOfState() = default;
        ProgramOfState(const ProgramOfState&) = default;
        ProgramOfState& operator=(const ProgramOfState& other)
        {
            if (other.program)
                program = other.program;
            if (other.slots)
                slots = other.slots;
            if (other.numbers)
                numbers = other.numbers;
            return *this;
        }

        const Program* program { nullptr };
        const SlotPlan* slots { nullptr };
        NumbersOfWalkStates* numbers { nullptr };
    };

    // Whoever writes it is its one owner: heldToWrite().
    struct Held {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(Held);

        Held() = default;
        Held(const Held&);
        Held& operator=(const Held&) = delete;
        ~Held()
        {
            if (m_lists) [[unlikely]]
                dropLists();
        }

        static uint8_t bitOf(const ProgramOfState&, const MemoryLocation&);
        static uint8_t bitOfSlot(const ProgramOfState&, FieldReference);

        void noteShape(Reference object, TypeID proven);
        void noteFresh(Reference object);
        void noteFactsOfCell(Reference object, OptionSet<FactOfCell>);
        void noteOrderWord(Reference object, std::optional<uint32_t>);
        std::optional<uint32_t> orderWordOf(Reference object) const;
        void noteStoresPassAbove(LayoutID classLayout, PropertyKeyID, Reference test);
        void noteGuardOfReadsHolds(LayoutID classLayout, PropertyKeyID, Reference test);
        void noteSlot(const ProgramOfState&, Reference object, FieldReference, Reference value);
        void noteVariable(const ProgramOfState&, VariableID, Reference value);
        void noteEnvironmentField(const ProgramOfState&, Reference environment, FieldReference, Reference value);
        void noteElement(Reference object, Reference index, Reference value);
        void invalidateAliasedSlots(const ProgramOfState&, Reference object, FieldReference, NOESCAPE const ScopedLambda<bool(Reference other)>& mayBeSameObject);
        void meetWith(const Held&);
        // AOTWalkKnownHandlers.cpp
        void endPlacesHolding(NOESCAPE const ScopedLambda<bool(Reference value)>& isEnded);
        // AOTWalkKnownWriters.cpp
        void endWhatIsWritten(const Effects&, NOESCAPE const ScopedLambda<bool(Reference holder)>& mayBeWritten, NOESCAPE const ScopedLambda<bool(LayoutID classOfGuard)>& mayEndGuard);

        TypeID shapeOfHolderOf(Reference) const;
        OptionSet<FactOfCell> factsOfCellOfHolderOf(Reference) const;
        bool holderIsFresh(Reference) const;
        // True: it was fresh.
        bool letLeaveIfFresh(Reference);
        // False: endWhatIsWritten() would leave everything as it is.
        bool restsOnHeapsAmong(const LocationMasks& writes) const;
        void endWhatIsWritten(const Effects&);
        void endWhatIsNotFresh();
        void dropAllRecords();
        void clear();

        // The record of the object or of the place, which may have ended. There is one at most that anybody finds: whatever is noted again takes its old record.
        uint32_t findRecordOfHolder(Reference) const;
        uint32_t findRecordOfPlace(uint32_t holder, PlaceKind, uint32_t first, uint32_t second) const;
        uint32_t findHolder(Reference) const;
        uint32_t findOrAddHolder(Reference);
        uint32_t findPlace(uint32_t holder, PlaceKind, uint32_t first, uint32_t second) const;
        Reference valueOfPlace(PlaceKind, Reference base, uint32_t first, uint32_t second) const;
        // No value: the place is no longer known. `bit` is asked where the place is new.
        void notePlace(PlaceKind, Reference base, uint32_t first, uint32_t second, Reference value, NOESCAPE const Invocable<uint8_t()> auto& bit);
        enum class ListOfPlaces : uint8_t { AllButElements, Elements };
        void endPlace(uint32_t place);
        void endPlacesOf(uint32_t holder, ListOfPlaces, NOESCAPE const Invocable<bool(const Place&)> auto& ends);
        void endSlotsUnderOtherLayouts(uint32_t holder, uint32_t layout);
        void endShape(uint32_t holder);
        void endFactsOfCell(uint32_t holder, OptionSet<FactOfCell>);
        void endHolder(uint32_t holder);
        void letLeave(uint32_t holder);
        void listAsReachedByEffects(uint32_t holder);
        void takeOffListOfReachedByEffects(uint32_t holder);
        void listAsFreshWithElements(uint32_t holder);
        // The holder comes among those that effects reach, or goes from among them, with all that hangs on it.
        void countAmongReachedByEffects(const Holder&);
        void countNoLongerAmongReachedByEffects(const Holder&);
        uint64_t writtenUnderPlacesReachedByEffects(const LocationMasks& writes) const;
        // Of the holders that are not fresh. One that nothing hangs on any more ends.
        void forEachHolderReachedByEffects(NOESCAPE const Invocable<void(uint32_t holder)> auto&);
        void fillTableOfHolders();
        void fillTableOfPlaces();

        // ---- Where much is known
        struct Lists;
        struct WhatEnds {
            bool shapes { false };
            bool elements { false };
            OptionSet<FactOfCell> facts;
            // The bits that are written in the heap of each kind of place.
            uint64_t slots { 0 };
            uint64_t variables { 0 };
            uint64_t environmentFields { 0 };
            uint64_t guards { 0 };
            // Null: whatever rests on what is written ends.
            const ScopedLambda<bool(Reference holder)>* mayBeWritten { nullptr };
            const ScopedLambda<bool(LayoutID classOfGuard)>* mayEndGuard { nullptr };
        };
        // Asked by whoever is about to go through the holders that effects reach. A kind of place that another file has added since has no list.
        bool goesByLists()
        {
            if (!m_lists) {
                noteRecordsGoneThrough(m_holdersReachedByEffects.size(), holdersFoundWithoutTable);
                if (!makeListsIfDue()) [[likely]]
                    return false;
            }
            constexpr uint16_t kindsWithLists = 1 << static_cast<unsigned>(PlaceKind::Slot) | 1 << static_cast<unsigned>(PlaceKind::Variable) | 1 << static_cast<unsigned>(PlaceKind::EnvironmentField)
                | 1 << static_cast<unsigned>(PlaceKind::GuardOfStores) | 1 << static_cast<unsigned>(PlaceKind::GuardOfReads);
            return !(m_kindsOfPlacesReachedByEffects & ~kindsWithLists);
        }
        void noteRecordsGoneThrough(size_t records, size_t foundWithoutTable)
        {
            if (records > foundWithoutTable) [[unlikely]]
                m_recordsGoneThrough += records;
        }
        // THE LISTS ARE NO PART OF WHAT A STATE HOLDS: they are made in a state that others share too, so that a row of branches that each copy it, write the copy and drop it make them once.
        bool makeListsIfDue()
        {
            if (m_recordsGoneThrough <= recordsGoneThroughWithoutLists) [[likely]]
                return false;
            if (m_lists || m_recordsGoneThrough <= static_cast<uint64_t>(timesGoneThroughForOneListing) * (m_holdersReachedByEffects.size() + m_numberOfPlacesReachedByEffects))
                return false;
            makeLists();
            return true;
        }
        void endByLists(const WhatEnds&);
        void invalidateAliasedSlotsByLists(uint32_t own, uint8_t bit, FieldReference, NOESCAPE const ScopedLambda<bool(Reference other)>& mayBeSameObject);
        void makeLists();
        void dropLists();
        void listPlace(uint32_t place);
        void listHolder(uint32_t holder);
        void listWhatHangsOn(uint32_t holder);
        void endHolderIfNothingHangsOn(uint32_t holder);
        bool hasManyEndedRecords() const;
        void dropEndedRecordsIfMany();
        void dropEndedRecords();

        using Table = SharedArray<uint32_t, 0, 8>;
        static uint32_t findInTable(const Table&, unsigned hash, NOESCAPE const Invocable<bool(uint32_t)> auto& isSought);
        static void addToTable(Table&, unsigned hash, uint32_t position);

        // Without a holder there is no other record either.
        bool holdsNoRecord() const { return m_holders.isEmpty(); }
        bool holdsNothingThatNoEffectEnds() const { return m_checkedNames.isEmpty() && m_bindingsPastDeadZone.isEmpty() && m_builtinsRead.isEmpty(); }
        bool isEmpty() const { return !m_numberOfShapes && !m_numberOfPlaces && !m_numberOfFreshHolders && !m_numberOfHoldersWithFactsOfCell && holdsNothingThatNoEffectEnds(); }

        static uint64_t bitInFilterOfHolders(Reference object) { return uint64_t { 1 } << (object.bits() & 63); }

        uint32_t numberOfOwners { 1 };
        // Null: going through the records one by one has cost little so far.
        Lists* m_lists { nullptr };
        // By this state, those that it comes from and those that were copied from it, since it last held nothing. It counts no further where there are lists.
        uint32_t m_recordsGoneThrough { 0 };
        // Through Holder::nextFreshWithElements: every fresh holder with an element, and some others.
        uint32_t m_firstFreshHolderWithElements { none };
        // Of the records that have not ended.
        uint32_t m_numberOfHolders { 0 };
        uint32_t m_numberOfPlaces { 0 };
        uint32_t m_numberOfShapes { 0 };
        uint32_t m_numberOfHoldersWithFactsOfCell { 0 };
        uint32_t m_numberOfFreshHolders { 0 };
        // Of what hangs on the holders that are not fresh. Whatever ends knowledge asks these first: where none of it can end, it goes through no holder, however many there are.
        uint32_t m_numberOfShapesReachedByEffects { 0 };
        uint32_t m_numberOfHoldersWithFactsOfCellReachedByEffects { 0 };
        uint32_t m_numberOfPlacesReachedByEffects { 0 };
        uint32_t m_numberOfElementsReachedByEffects { 0 };
        // Holder::factsOfCell, kindsOfPlaces and maskOfPlaces of all of them together, and of some that have ended. Whoever goes through them all makes the three anew.
        OptionSet<FactOfCell> m_factsOfCellReachedByEffects;
        uint16_t m_kindsOfPlacesReachedByEffects { 0 };
        uint64_t m_maskOfPlacesReachedByEffects { 0 };
        // bitInFilterOfHolders() of every record in m_holders: most of the values that are asked about have none.
        uint64_t m_filterOfHolders { 0 };
        // By the numbers of NumbersOfWalkStates.
        SharedSetOfNumbers m_checkedNames;
        SharedSetOfNumbers m_bindingsPastDeadZone;
        SharedSetOfNumbers m_builtinsRead;
        // Empty, or a power of two of entries, at most half of them taken, each found from the hash of its record on.
        Table m_tableOfHolders;
        Table m_tableOfPlaces;
        // The holders that are not fresh and have not ended.
        Vector<uint32_t, holdersFoundWithoutTable> m_holdersReachedByEffects;
        // Records stay where they are, and stand for what they stood for, until dropEndedRecords(), which makes both lists anew.
        // So where two states share a part of a list, each position that a record of that part names means the same in both.
        SharedArray<Holder, holdersFoundWithoutTable> m_holders;
        SharedArray<Place, placesFoundWithoutTable> m_places;
    };

    static void release(Held* held)
    {
        if (held && !--held->numberOfOwners)
            delete held;
    }
    Held& heldToWrite()
    {
        if (!m_held || m_held->numberOfOwners > 1) [[unlikely]]
            takeHeldOfOwn();
        return *m_held;
    }
    void takeHeldOfOwn();
    void shareWhatIsHeldBy(const WalkState&);
    void dropWhatIsHeld();
    void dropAllRecords();
    void endWhatIsWritten(const Effects&);
    void endWhatIsNotFresh();
    void noteBindingPastDeadZone(uint32_t number)
    {
        if (!m_held || !m_held->m_bindingsPastDeadZone.contains(number))
            heldToWrite().m_bindingsPastDeadZone.add(number);
    }
    Reference valueOfPlace(PlaceKind kind, Reference base, uint32_t first, uint32_t second) const { return m_held ? m_held->valueOfPlace(kind, base, first, second) : Reference(); }
    bool writesInPlace() const { return m_held && m_held->numberOfOwners == 1; }
    // `known` is what the state holds for the place. Asked so that a state which shares what it holds goes on sharing it.
    bool noteChangesNothing(Reference known, Reference value) const
    {
        if (!m_held)
            return !value;
        return value && known == value && !m_held->hasManyEndedRecords();
    }

    // Most states hold nothing at all, and whatever is done to such a state asks this first.
    bool holdsNoRecord() const { return !m_held || m_held->holdsNoRecord(); }
    bool holdsNothing() const { return !m_held || (m_held->holdsNoRecord() && m_held->holdsNothingThatNoEffectEnds()); }

    Held* m_held { nullptr };
    KnownAnswers m_answers;
    // Zero: something was done, or nobody has said.
    uint32_t m_turnInWhichNothingWasDone { 0 };
    ProgramOfState m_program;
};

// The variable starts empty and is stored once, and nothing empties it again: no second run of the body of its module, no mock, no run time that owns the word.
bool leavesDeadZoneForGood(const Program&, VariableID);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
