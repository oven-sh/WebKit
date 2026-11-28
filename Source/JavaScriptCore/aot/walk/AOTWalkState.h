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

class SharedSetOfNumbers {
public:
    bool isEmpty() const { return !m_size; }
    bool contains(uint32_t number) const
    {
        uint32_t word = number / bitsOfWord;
        return word < m_words.size() && (m_words[word] >> number % bitsOfWord & 1);
    }
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
    void remove(uint32_t number)
    {
        uint32_t word = number / bitsOfWord;
        uint64_t bit = 1ull << number % bitsOfWord;
        if (word >= m_words.size() || !(m_words[word] & bit))
            return;
        m_words.mutableAt(word) &= ~bit;
        --m_size;
    }
    void keepMembersOf(const SharedSetOfNumbers& other)
    {
        if (m_size)
            keepMembersOfSlowCase(other);
    }
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
    template<typename Functor> void forEachMemberNotIn(const SharedSetOfNumbers& other, const Functor& functor) const
    {
        if (!m_size)
            return;
        m_words.forEachStretchNotSharedWith(other.m_words, [&](uint32_t begin, uint32_t end) {
            for (uint32_t word = begin; word < end; ++word) {
                for (uint64_t bits = m_words[word] & ~(word < other.m_words.size() ? other.m_words[word] : 0); bits; bits &= bits - 1)
                    functor(word * bitsOfWord + static_cast<uint32_t>(std::countr_zero(bits)));
            }
        });
    }

private:
    static constexpr uint32_t bitsOfWord = 64;

    void keepMembersOfSlowCase(const SharedSetOfNumbers&);

    SharedArray<uint64_t, 2> m_words;
    uint32_t m_size { 0 };
};

class NumbersOfWalkStates {
public:
    struct CheckedName {
        Reference value;
        Reference name;
        uint32_t older { 0 };
    };

    void takeRoomOf(NumbersOfWalkStates& former)
    {
        takeRoom(m_checkedNames, former.m_checkedNames);
        takeRoom(m_newestCheckedNames, former.m_newestCheckedNames);
        takeRoom(m_numbersThatName, former.m_numbersThatName);
    }
    void rememberNoPlace() { m_remembersPlaces = false; }
    bool remembersPlaces() const { return m_remembersPlaces; }
    void clear()
    {
        m_checkedNames.shrink(0);
        m_newestCheckedNames.shrink(0);
        m_numbersThatName.shrink(0);
        if (!m_numbersOfBindings.isEmpty())
            m_numbersOfBindings.clear();
        if (!m_numbersOfBuiltins.isEmpty())
            m_numbersOfBuiltins.clear();
        if (!m_elementIndices.isEmpty())
            m_elementIndices.clear();
    }

    void setCode(const Code& code) { m_code = &code; }
    bool addElementIndex(Reference);
    std::span<const Reference> elementIndices(int64_t position) const LIFETIME_BOUND;

    Reference arrayKeepingElements;

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
    void rememberNumberThatNames(Reference name, uint32_t number)
    {
        uint32_t index = name.id().index();
        if (size_t size = m_numbersThatName.size(); size <= index) {
            m_numbersThatName.grow(index + 1);
            std::ranges::fill(m_numbersThatName.mutableSpan().subspan(size), 0);
        }
        m_numbersThatName[index] = number + 1;
    }
    uint32_t numberThatNames(Reference name) const { return name.id().index() < m_numbersThatName.size() ? m_numbersThatName[name.id().index()] : 0; }
    ALWAYS_INLINE uint32_t newestCheckedNameOf(Reference value) const
    {
        static_assert(!static_cast<uint32_t>(Reference::Kind::Value) && static_cast<uint32_t>(Reference::Kind::Constant) == 1 && Reference::kindShift == 30);
        uint32_t indexIfRecord = value.bits() & ~(1u << Reference::kindShift);
        return indexIfRecord < m_newestCheckedNames.size() ? m_newestCheckedNames[indexIfRecord] : 0;
    }
    const CheckedName& checkedName(uint32_t number) const { return m_checkedNames[number]; }

    uint32_t numberOf(VariableID variable) { return numberOfBinding(keyOf(variable)); }
    std::optional<uint32_t> findNumberOf(VariableID variable) const { return findNumberOfBinding(keyOf(variable)); }
    uint32_t numberOf(FunctionID functionWhoseClaimsAreAnswered) { return numberOfBinding(keyOf(functionWhoseClaimsAreAnswered)); }
    std::optional<uint32_t> findNumberOf(FunctionID functionWhoseClaimsAreAnswered) const { return findNumberOfBinding(keyOf(functionWhoseClaimsAreAnswered)); }
    uint32_t numberOf(Reference environment, FieldReference field) { return numberOfBinding(keyOf(environment, field)); }
    std::optional<uint32_t> findNumberOf(Reference environment, FieldReference field) const { return findNumberOfBinding(keyOf(environment, field)); }
    uint32_t numberOf(LayoutID classWhoseGuardsHaveBegun) { return numberOfBinding(keyOf(classWhoseGuardsHaveBegun)); }
    std::optional<uint32_t> findNumberOf(LayoutID classWhoseGuardsHaveBegun) const { return findNumberOfBinding(keyOf(classWhoseGuardsHaveBegun)); }

    uint32_t numberOf(BuiltinID builtin) { return m_numbersOfBuiltins.add(builtin.index() + 1, m_numbersOfBuiltins.size()).iterator->value; }
    std::optional<uint32_t> findNumberOf(BuiltinID builtin) const
    {
        auto found = m_numbersOfBuiltins.find(builtin.index() + 1);
        return found == m_numbersOfBuiltins.end() ? std::nullopt : std::optional(found->value);
    }

private:
    static uint64_t keyOf(VariableID variable) { return static_cast<uint64_t>(variable.index()) + 1; }
    static uint64_t keyOf(FunctionID function) { return 1ULL << 63 | function.index(); }
    static uint64_t keyOf(Reference environment, FieldReference field) { return (static_cast<uint64_t>(environment.id().index()) + 1) << 32 | field.field; }
    static uint64_t keyOf(LayoutID classLayout) { return 3ULL << 62 | classLayout.index(); }
    uint32_t numberOfBinding(uint64_t key) { return m_numbersOfBindings.add(key, m_numbersOfBindings.size()).iterator->value; }
    std::optional<uint32_t> findNumberOfBinding(uint64_t key) const
    {
        auto found = m_numbersOfBindings.find(key);
        return found == m_numbersOfBindings.end() ? std::nullopt : std::optional(found->value);
    }

    Vector<CheckedName> m_checkedNames;
    Vector<uint32_t> m_newestCheckedNames;
    Vector<uint32_t> m_numbersThatName;
    HashMap<uint64_t, uint32_t> m_numbersOfBindings;
    HashMap<uint32_t, uint32_t> m_numbersOfBuiltins;
    HashMap<uint64_t, Vector<Reference, 2>, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_elementIndices;
    const Code* m_code { nullptr };
    bool m_remembersPlaces { true };
};

enum class CellFact : uint16_t {
    NotFrozen = 1 << 0,
    NoFieldOutsideSlot = 1 << 1,
    HasLayoutPrototype = 1 << 2,
    OrderWordIsKnown = 1 << 3,
    IsLikelyAsAllocated = 1 << 4,
    IsLikelyAsConstructed = 1 << 5,
    NoExtraProperties = 1 << 6,
    BearsNoMark = 1 << 7,
    EnumeratorIsCurrent = 1 << 8,
    IsExtensible = 1 << 9,
};
inline constexpr uint32_t numberOfCellFacts = 10;
static_assert(static_cast<uint32_t>(CellFact::IsExtensible) == 1u << (numberOfCellFacts - 1));

OptionSet<CellFact> cellFactsInvalidatedBy(const LocationMasks& writes);

class WalkState {
public:
    WalkState() = default;
    WalkState(const WalkState& other)
        : m_held(other.holdsNothing() ? nullptr : other.m_held)
        , m_marks(other.m_marks)
        , m_program(other.m_program)
    {
        if (m_held)
            ++m_held->numberOfOwners;
        else
            m_marks.kinds &= keptWhereNothingIsHeld;
        if (!other.m_answers.isEmpty())
            m_answers = other.m_answers;
    }
    WalkState(WalkState&& other)
        : m_held(std::exchange(other.m_held, nullptr))
        , m_marks(other.m_marks)
        , m_program(other.m_program)
    {
        other.m_marks.kinds &= keptWhereNothingIsHeld;
        if (!other.m_answers.isEmpty())
            m_answers.swap(other.m_answers);
    }
    ALWAYS_INLINE ~WalkState()
    {
        if (m_held)
            letGoOfHeld();
    }

    WalkState& operator=(const WalkState& other)
    {
        if (this == &other)
            return *this;
        if (other.m_marks.kinds & programIsWhole) [[likely]]
            m_program.takeWhole(other.m_program);
        else
            m_program = other.m_program;
        if (m_held != other.m_held)
            shareWhatIsHeldBy(other);
        if (!m_answers.isEmpty() || !other.m_answers.isEmpty())
            m_answers = other.m_answers;
        m_marks = other.m_marks;
        if (m_held != other.m_held)
            m_marks.kinds &= keptWhereNothingIsHeld;
        if (!(m_marks.kinds & programIsWhole)) [[unlikely]]
            noteWhetherProgramIsWhole();
        return *this;
    }

    ALWAYS_INLINE WalkState& operator=(WalkState&& other)
    {
        static_assert(std::bit_cast<uint64_t>(Marks { 0, programIsWhole }) == programIsWholeAmongMarks);
        uint64_t marks = std::bit_cast<uint64_t>(m_marks);
        uint64_t marksOfOther = std::bit_cast<uint64_t>(other.m_marks);
        if (marksOfOther & programIsWholeAmongMarks) [[likely]] {
            m_program.takeWhole(other.m_program);
            m_marks = std::bit_cast<Marks>(marksOfOther);
            other.m_marks = std::bit_cast<Marks>(marks | programIsWholeAmongMarks);
        } else
            takeMarksOfStateWithoutWholeProgram(other);
        std::swap(m_held, other.m_held);
        if (!m_answers.isEmpty() || !other.m_answers.isEmpty())
            m_answers.swap(other.m_answers);
        return *this;
    }

    void setProgram(const Program& program)
    {
        m_program.program = &program;
        noteWhetherProgramIsWhole();
    }
    void setSlotPlan(const SlotPlan& slots)
    {
        m_program.slots = &slots;
        noteWhetherProgramIsWhole();
    }
    void setNumbers(NumbersOfWalkStates& numbers)
    {
        m_program.numbers = &numbers;
        noteWhetherProgramIsWhole();
    }
    void setAnswers(AnswersOfWalk& answers) { m_answers.setTable(answers); }
    KnownAnswers& answers() { return m_answers; }
    const KnownAnswers& answers() const { return m_answers; }

    void noteShape(Reference object, TypeID proven)
    {
        if (writesInPlace() || shapeOf(object) != proven) {
            heldToWrite().noteShape(object, proven);
            if (proven)
                m_marks.kinds |= Kinds::shapes;
        }
    }
    ALWAYS_INLINE TypeID shapeOf(Reference object) const { return mayHold(Kinds::shapes) && m_held->mayHaveRecordOf(object) ? m_held->shapeOfHolderOf(object) : TypeID(); }

    void noteFresh(Reference object)
    {
        if (writesInPlace() || !isFresh(object)) {
            heldToWrite().noteFresh(object);
            m_marks.kinds |= Kinds::freshObjects;
            m_answers.holderIsFreshAgain(object);
        }
    }
    bool isFresh(Reference object) const { return mayHold(Kinds::freshObjects) && m_held->mayHaveRecordOf(object) && m_held->holderIsFresh(object); }
    bool holdsFreshObjects() const { return mayHold(Kinds::freshObjects) && m_held->m_numberOfFreshHolders; }
    bool mayHoldFreshObjectsFrom(uint32_t firstRecord) const { return holdsFreshObjects() && m_held->m_endOfFreshObjects > firstRecord; }
    void noteStoreIntoUnmadeRecord(uint32_t number) { heldToWrite().m_newestStoreIntoUnmadeRecord = number; }
    uint32_t newestStoreIntoUnmadeRecord() const { return m_held ? m_held->m_newestStoreIntoUnmadeRecord : 0; }
    void forEachFreshObject(NOESCAPE const Invocable<void(Reference)> auto& functor) const
    {
        if (!holdsFreshObjects())
            return;
        for (uint32_t holder = 0; holder < m_held->m_holders.size(); ++holder) {
            if (m_held->m_holders[holder].isFresh && !m_held->m_holders[holder].hasEnded)
                functor(m_held->m_holders[holder].object);
        }
    }
    void forEachFieldNotSharedWithAll(std::span<const WalkState* const> ways, NOESCAPE const Invocable<void(Reference holder, FieldReference, Reference value, bool isSlot)> auto& functor) const
    {
        if (!mayHold(Kinds::places) || !m_held->m_numberOfPlaces)
            return;
        Vector<uint32_t, 16> positions;
        for (const WalkState* way : ways) {
            if (way->m_held == m_held)
                continue;
            if (!way->m_held)
                return;
            m_held->m_places.forEachStretchNotSharedWith(way->m_held->m_places, [&](uint32_t begin, uint32_t end) {
                for (uint32_t position = begin; position < end; ++position) {
                    const Place& place = m_held->m_places[position];
                    if (place.value && (place.kind == PlaceKind::EnvironmentField || place.kind == PlaceKind::Slot))
                        positions.append(position);
                }
            });
        }
        if (ways.size() > 2) {
            std::ranges::sort(positions);
            positions.shrink(std::ranges::unique(positions).begin() - positions.begin());
        }
        for (uint32_t position : positions) {
            const Place& place = m_held->m_places[position];
            const Holder& holder = m_held->m_holders[place.holder];
            if (!holder.hasEnded && holder.object)
                functor(holder.object, FieldReference { LayoutID(place.first), place.second }, place.value, place.kind == PlaceKind::Slot);
        }
    }
    void forEachHolderOfElementsNotSharedWithAll(std::span<const WalkState* const> ways, NOESCAPE const Invocable<void(Reference holder)> auto& functor) const
    {
        if (!mayHold(Kinds::places) || !m_held->m_numberOfPlaces)
            return;
        Vector<uint32_t, 16> holders;
        for (const WalkState* way : ways) {
            if (way->m_held == m_held)
                continue;
            if (!way->m_held)
                return;
            m_held->m_places.forEachStretchNotSharedWith(way->m_held->m_places, [&](uint32_t begin, uint32_t end) {
                for (uint32_t position = begin; position < end; ++position) {
                    const Place& place = m_held->m_places[position];
                    if (place.value && place.kind == PlaceKind::Element && !place.second)
                        holders.append(place.holder);
                }
            });
        }
        std::ranges::sort(holders);
        holders.shrink(std::ranges::unique(holders).begin() - holders.begin());
        for (uint32_t holder : holders) {
            if (!m_held->m_holders[holder].hasEnded && m_held->m_holders[holder].object)
                functor(m_held->m_holders[holder].object);
        }
    }
    void forEachBeliefOfFreshHolderLackingIn(const WalkState& arriving, NOESCAPE const Invocable<void(Reference holder, std::optional<FieldReference> fieldOfScope, Reference believed)> auto& functor) const
    {
        if (!holdsFreshObjects())
            return;
        auto isFreshHolder = [&](const Holder& holder) {
            return !holder.hasEnded && holder.isFresh && holder.object;
        };
        auto holdCells = [&](uint32_t begin, uint32_t end) {
            for (uint32_t number = begin; number < end; ++number) {
                if (!isFreshHolder(m_held->m_holders[number]))
                    continue;
                Reference object = m_held->m_holders[number].object;
                if (!arriving.isFresh(object) || (shapeOf(object) && shapeOf(object) != arriving.shapeOf(object)) || !arriving.cellFacts(object).containsAll(cellFacts(object)) || (cellFacts(object).contains(CellFact::OrderWordIsKnown) && orderWordOf(object) != arriving.orderWordOf(object)))
                    functor(object, std::nullopt, Reference());
            }
        };
        auto holdPlaces = [&](uint32_t begin, uint32_t end) {
            for (uint32_t position = begin; position < end; ++position) {
                const Place& place = m_held->m_places[position];
                if (!place.value || place.holder == none || !isFreshHolder(m_held->m_holders[place.holder]))
                    continue;
                Reference object = m_held->m_holders[place.holder].object;
                if (!arriving.isFresh(object) || arriving.valueOfPlace(place.kind, object, place.first, place.second) == place.value)
                    continue;
                functor(object, place.kind == PlaceKind::EnvironmentField ? std::optional(FieldReference { LayoutID(place.first), place.second }) : std::nullopt, place.value);
            }
        };
        if (!arriving.m_held)
            holdCells(0, m_held->m_holders.size());
        else if (arriving.m_held != m_held) {
            m_held->m_holders.forEachStretchNotSharedWith(arriving.m_held->m_holders, holdCells);
            if (mayHold(Kinds::places) && m_held->m_numberOfPlaces)
                m_held->m_places.forEachStretchNotSharedWith(arriving.m_held->m_places, holdPlaces);
        }
        m_answers.forEachHolderOfAnswerAboutFreshHolderLackingIn(arriving.m_answers, [&](Reference holder) {
            functor(holder, std::nullopt, holder);
        });
    }
    void forEachValueHeldByAnotherSince(ValueID oldest, NOESCAPE const Invocable<void(Reference)> auto& functor) const
    {
        if (!m_held)
            return;
        for (uint32_t position = 0; position < m_held->m_places.size();) {
            if (!(position % Held::placesOfNewestValue) && m_held->holdsOnlyValuesBefore(oldest, position / Held::placesOfNewestValue)) {
                position += Held::placesOfNewestValue;
                continue;
            }
            const Place& place = m_held->m_places[position++];
            if (place.value && place.value != m_held->m_holders[place.holder].object)
                functor(place.value);
        }
    }
    void forEachValueHeldByAnother(NOESCAPE const Invocable<void(Reference)> auto& functor) const
    {
        if (!m_held)
            return;
        for (uint32_t position = 0; position < m_held->m_places.size(); ++position) {
            const Place& place = m_held->m_places[position];
            if (place.value && place.value != m_held->m_holders[place.holder].object)
                functor(place.value);
        }
    }
    void markEscaped(Reference object)
    {
        if (mayHold(Kinds::freshObjects) && m_held->mayHaveRecordOf(object) && (writesInPlace() || m_held->holderIsFresh(object)) && heldToWrite().letLeaveIfFresh(object)) {
            if (!m_held->m_numberOfFreshHolders)
                m_marks.kinds &= ~Kinds::freshObjects;
            m_answers.holderHasLeft(object);
        }
    }
    Vector<Reference, 8> listFreshObjects() const;

    void noteCheckedName(Reference value, Reference name)
    {
        RELEASE_ASSERT(m_program.numbers && !checkedNameOf(value));
        Held& held = heldToWrite();
        held.m_checkedNames.add(m_program.numbers->addCheckedName(value, name));
        held.m_filterOfValuesWithCheckedNames |= bitOfValueWithCheckedName(value);
        m_marks.kinds |= Kinds::checkedNames;
    }
    ALWAYS_INLINE Reference checkedNameOf(Reference value) const
    {
        if (!hasCheckedNames())
            return { };
        uint32_t newest = m_program.numbers->newestCheckedNameOf(value);
        return newest && (m_held->m_filterOfValuesWithCheckedNames & bitOfValueWithCheckedName(value)) ? checkedNameHeldFrom(newest) : Reference();
    }
    static uint64_t bitOfValueWithCheckedName(Reference value) { return 1ull << (value.bits() * 0x9e3779b1u >> 26); }
    template<typename Functor> void forEachValueWithCheckedNameThatIsNotHeldBy(const WalkState& other, const Functor& functor) const
    {
        if (!hasCheckedNames() || !other.hasCheckedNames())
            return;
        m_held->m_checkedNames.forEachMemberNotIn(other.m_held->m_checkedNames, [&](uint32_t number) {
            functor(m_program.numbers->checkedName(number).value);
        });
    }
    bool hasCheckedNames() const { return mayHold(Kinds::checkedNames); }
    Reference valueNamedAnewBy(Reference name) const
    {
        uint32_t number = hasCheckedNames() && name.isValue() ? m_program.numbers->numberThatNames(name) : 0;
        if (!number || !m_held->m_checkedNames.contains(number - 1) || m_program.numbers->checkedName(number - 1).name != name)
            return { };
        return m_program.numbers->checkedName(number - 1).value;
    }
    void nameAnew(Reference value, Reference name);

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
    void noteThatClaimsAreAnswered(FunctionID function)
    {
        RELEASE_ASSERT(m_program.numbers);
        noteBindingPastDeadZone(m_program.numbers->numberOf(function));
    }
    bool claimsAreAnswered(FunctionID function) const
    {
        if (!m_held || m_held->m_bindingsPastDeadZone.isEmpty())
            return false;
        std::optional<uint32_t> number = m_program.numbers->findNumberOf(function);
        return number && m_held->m_bindingsPastDeadZone.contains(*number);
    }
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

    void recordCellFacts(Reference object, OptionSet<CellFact> facts)
    {
        if (writesInPlace() || !cellFacts(object).containsAll(facts)) {
            heldToWrite().recordCellFacts(object, facts);
            m_marks.kinds |= Kinds::cellFacts;
        }
    }
    OptionSet<CellFact> cellFacts(Reference object) const { return mayHold(Kinds::cellFacts) && m_held->mayHaveRecordOf(object) ? m_held->cellFactsOfHolder(object) : OptionSet<CellFact>(); }
    void invalidateCellFacts(Reference object, OptionSet<CellFact> facts)
    {
        if (cellFacts(object).containsAny(facts))
            heldToWrite().invalidateCellFactsOfHolder(object, facts);
    }
    void noteOrderWord(Reference object, std::optional<uint32_t> orderWord)
    {
        bool isNoted = orderWord ? orderWordOf(object) == orderWord && cellFacts(object).containsAll({ CellFact::NotFrozen, CellFact::IsExtensible, CellFact::NoFieldOutsideSlot }) : !cellFacts(object).contains(CellFact::OrderWordIsKnown);
        if (!isNoted) {
            heldToWrite().noteOrderWord(object, orderWord);
            m_marks.kinds |= Kinds::cellFacts;
        }
    }
    std::optional<uint32_t> orderWordOf(Reference object) const { return mayHold(Kinds::cellFacts) && m_held->mayHaveRecordOf(object) ? m_held->orderWordOf(object) : std::nullopt; }

    enum class AskerOfGuardOfReads : uint8_t { EveryRead, ReadBehindLookAtObject, AssignedMember, PrototypeLink };
    static uint32_t numberOfGuardedSide(LayoutID classLayout, ClassSide side, AskerOfGuardOfReads asker = AskerOfGuardOfReads::EveryRead) { return classLayout.index() << 3 | static_cast<uint32_t>(asker) << 1 | static_cast<uint32_t>(side); }
    static LayoutID classOfGuardedSide(uint32_t number) { return LayoutID(number >> 3); }
    static AskerOfGuardOfReads askerOfGuardedSide(uint32_t number) { return static_cast<AskerOfGuardOfReads>(number >> 1 & 3); }
    void noteStoresPassAbove(LayoutID classLayout, PropertyKeyID key, Reference test)
    {
        if (writesInPlace() || !noteChangesNothing(valueOfPlace(PlaceKind::GuardOfStores, Reference(), numberOfGuardedSide(classLayout, ClassSide::Prototype), key.bits()), test)) {
            heldToWrite().noteStoresPassAbove(classLayout, key, test);
            m_marks.kinds |= Kinds::places;
        }
    }
    bool storesPassAbove(LayoutID classLayout, PropertyKeyID key) const { return !!valueOfPlace(PlaceKind::GuardOfStores, Reference(), numberOfGuardedSide(classLayout, ClassSide::Prototype), key.bits()); }
    void noteGuardOfReadsHolds(LayoutID classLayout, PropertyKeyID key, Reference test, ClassSide side = ClassSide::Prototype, AskerOfGuardOfReads asker = AskerOfGuardOfReads::EveryRead)
    {
        if (writesInPlace() || !noteChangesNothing(valueOfPlace(PlaceKind::GuardOfReads, Reference(), numberOfGuardedSide(classLayout, side, asker), key.bits()), test)) {
            heldToWrite().noteGuardOfReadsHolds(classLayout, key, test, side, asker);
            m_marks.kinds |= Kinds::places;
        }
    }
    bool guardOfReadsHolds(LayoutID classLayout, PropertyKeyID key, ClassSide side = ClassSide::Prototype) const { return !!valueOfPlace(PlaceKind::GuardOfReads, Reference(), numberOfGuardedSide(classLayout, side), key.bits()); }
    bool memberAboveIsUnchanged(LayoutID classLayout, PropertyKeyID key) const { return guardOfReadsHolds(classLayout, key) || !!valueOfPlace(PlaceKind::GuardOfReads, Reference(), numberOfGuardedSide(classLayout, ClassSide::Prototype, AskerOfGuardOfReads::ReadBehindLookAtObject), key.bits()); }
    Reference assignedMemberTest(LayoutID classLayout, PropertyKeyID key) const { return valueOfPlace(PlaceKind::GuardOfReads, Reference(), numberOfGuardedSide(classLayout, ClassSide::Prototype, AskerOfGuardOfReads::AssignedMember), key.bits()); }
    bool isMemberAssigned(LayoutID classLayout, PropertyKeyID key) const { return !!assignedMemberTest(classLayout, key); }
    bool isPrototypeLinkUnchanged(LayoutID classLayout, ClassSide side) const { return !!valueOfPlace(PlaceKind::GuardOfReads, Reference(), numberOfGuardedSide(classLayout, side, AskerOfGuardOfReads::PrototypeLink), PropertyKeyID().bits()); }
    bool mayHoldGuards() const { return mayHoldPlaceOf(Reference()) && m_held->mayHoldGuards(); }
    bool someGuardHoldsOf(LayoutID classLayout) const { return mayHoldPlaceOf(Reference()) && m_held->someGuardHoldsOf(classLayout); }
    void noteThatGuardsHaveBegun(LayoutID classLayout)
    {
        RELEASE_ASSERT(m_program.numbers);
        noteBindingPastDeadZone(m_program.numbers->numberOf(classLayout));
    }
    bool guardsHaveBegun(LayoutID classLayout) const
    {
        if (!m_held || m_held->m_bindingsPastDeadZone.isEmpty())
            return false;
        std::optional<uint32_t> number = m_program.numbers->findNumberOf(classLayout);
        return number && m_held->m_bindingsPastDeadZone.contains(*number);
    }

    void noteSlot(Reference object, FieldReference field, Reference value)
    {
        if (!remembersPlaces())
            return;
        if (writesInPlace() || !noteChangesNothing(slot(object, field), value)) {
            heldToWrite().noteSlot(m_program, object, field, value);
            m_marks.kinds |= Kinds::places;
        }
    }
    void noteVariable(VariableID variable, Reference value)
    {
        if (!remembersPlaces())
            return;
        if (writesInPlace() || !noteChangesNothing(this->variable(variable), value)) {
            heldToWrite().noteVariable(m_program, variable, value);
            m_marks.kinds |= Kinds::places;
        }
    }
    void noteEnvironmentField(Reference environment, FieldReference field, Reference value)
    {
        if (!remembersPlaces())
            return;
        if (writesInPlace() || !noteChangesNothing(environmentField(environment, field), value)) {
            heldToWrite().noteEnvironmentField(m_program, environment, field, value);
            m_marks.kinds |= Kinds::places;
        }
    }
    void noteElement(Reference object, Reference index, Reference value)
    {
        if (!remembersPlaces())
            return;
        if (writesInPlace() || !noteChangesNothing(element(object, index), value)) {
            heldToWrite().noteElement(object, index, value, value && m_program.numbers && m_program.numbers->addElementIndex(index));
            m_marks.kinds |= Kinds::places;
        }
    }
    void endElementAt(Reference object, int64_t position)
    {
        for (Reference index : m_program.numbers->elementIndices(position))
            noteElement(object, index, { });
    }
    Reference elementAt(Reference object, int64_t position) const
    {
        for (Reference index : m_program.numbers->elementIndices(position)) {
            if (Reference held = element(object, index))
                return held;
        }
        return { };
    }
    void noteElementWithNaNForHole(Reference object, Reference index, Reference value)
    {
        if (!remembersPlaces())
            return;
        if (writesInPlace() || !noteChangesNothing(elementWithNaNForHole(object, index), value)) {
            heldToWrite().noteElementWithNaNForHole(object, index, value);
            m_marks.kinds |= Kinds::places;
        }
    }
    void noteElementsLieOutsideViews(Reference object)
    {
        if (mayHoldPlaceOf(object))
            heldToWrite().noteElementsLieOutsideViews(object);
    }
    bool remembersPlaces() const { return !m_program.numbers || m_program.numbers->remembersPlaces(); }
    Reference slot(Reference object, FieldReference field) const { return valueOfPlace(PlaceKind::Slot, object, field.layout.index(), field.field); }
    Reference variable(VariableID variable) const { return valueOfPlace(PlaceKind::Variable, Reference(), variable.index(), 0); }
    Reference environmentField(Reference environment, FieldReference field) const { return valueOfPlace(PlaceKind::EnvironmentField, environment, field.layout.index(), field.field); }
    Reference element(Reference object, Reference index) const { return valueOfPlace(PlaceKind::Element, object, index.bits(), 0); }
    Reference elementWithNaNForHole(Reference object, Reference index) const { return valueOfPlace(PlaceKind::Element, object, index.bits(), 1); }
    struct SlotOfObject {
        FieldReference field;
        Reference value;
    };
    Vector<SlotOfObject, 8> listSlotsOf(Reference object) const;
    struct ElementOfObject {
        Reference index;
        Reference value;
    };
    Vector<ElementOfObject, 8> listElementsOf(Reference object) const;
    bool mayHoldPlaceOf(Reference base) const { return mayHold(Kinds::places) && m_held->m_numberOfPlaces && m_held->mayHaveRecordOf(base); }
    bool holdsPlacesThatEffectsReach() const { return mayHold(Kinds::places) && m_held->m_numberOfPlacesReachedByEffects; }

    void invalidateAliasedSlots(Reference object, FieldReference field, NOESCAPE const ScopedLambda<bool(Reference other)>& mayBeSameObject, NOESCAPE const ScopedLambda<bool(Reference, LayoutID)>& isOfLayoutAlone)
    {
        if (m_held && m_held->m_numberOfPlaces)
            heldToWrite().invalidateAliasedSlots(m_program, object, field, mayBeSameObject, isOfLayoutAlone);
    }

    static constexpr uint32_t irreversibleWritesBit = 1u << 31;
    bool isEffectFreeSince(uint32_t restartPoint) const { return restartPoint && m_marks.restartMark == restartPoint; }
    static constexpr uint32_t undoableStoresBit = 1u << 30;
    static constexpr uint32_t restartPointOf(uint32_t mark) { return mark & ~(irreversibleWritesBit | undoableStoresBit); }
    bool hasOnlyKnownEffectsSince(uint32_t restartPoint) const { return restartPoint && restartPointOf(m_marks.restartMark) == restartPoint; }
    bool canRestartFrom(uint32_t restartPoint) const { return hasOnlyKnownEffectsSince(restartPoint) && !(m_marks.restartMark & irreversibleWritesBit); }
    uint32_t newestUndoableStore() const { return m_marks.restartMark & undoableStoresBit ? (m_marks.kinds & newestUndoableStoreMask) >> newestUndoableStoreShift : 0; }
    void recordUndoableStore(uint32_t restartPoint, uint32_t store)
    {
        RELEASE_ASSERT(restartPoint < undoableStoresBit && store <= newestUndoableStoreMask >> newestUndoableStoreShift);
        m_marks.restartMark = undoableStoresBit | restartPoint;
        m_marks.kinds = (m_marks.kinds & ~newestUndoableStoreMask) | store << newestUndoableStoreShift;
    }
    void setRestartPoint(uint32_t restartPoint) { m_marks.restartMark = restartPoint; }
    void clearRestartMark() { m_marks.restartMark = 0; }
    uint32_t restartMark() const { return m_marks.restartMark; }
    void forbidRestart()
    {
        if (m_marks.restartMark)
            m_marks.restartMark = restartPointOf(m_marks.restartMark) | irreversibleWritesBit;
    }
    void setRestartMarkForTesting(uint32_t mark) { m_marks.restartMark = mark; }
    void restoreRestartMark(uint32_t mark) { m_marks.restartMark = mark; }
    bool isInlineeEffectFree(uint32_t inlineeRestartPoint) const { return m_marks.restartMark && m_marks.restartMark <= inlineeRestartPoint; }
    void setInlineeRestartPoint(uint32_t inlineeRestartPoint)
    {
        if (!m_marks.restartMark || (m_marks.restartMark & irreversibleWritesBit))
            m_marks.restartMark = inlineeRestartPoint;
    }
    void restoreDisplacedRestartMark(uint32_t inlineeRestartPoint, uint32_t displacedMark)
    {
        if (!(m_marks.restartMark & undoableStoresBit) && (m_marks.restartMark & ~irreversibleWritesBit) == inlineeRestartPoint)
            m_marks.restartMark = displacedMark;
    }
    void updateRestartMark(const Effects& effects)
    {
        constexpr AbstractHeapSet heapsWrittenUnseen = unobservableWriteHeaps | AbstractHeapSet { AbstractHeap::ArrayCapacity };
        if (m_marks.restartMark && (effects.mayRunOtherCode() || !effects.writes.containsOnly(heapsWrittenUnseen)))
            m_marks.restartMark = effects.mayRunOtherCode() ? 0 : restartPointOf(m_marks.restartMark) | irreversibleWritesBit;
    }

    ALWAYS_INLINE void invalidate(const Effects& effects, const MemoryLocation* writtenAlone = nullptr)
    {
        updateRestartMark(effects);
        if (!effects.writes.isEmpty() || effects.mayRunOtherCode()) {
            if (!holdsNoRecord())
                endWhatIsWritten(effects, writtenAlone);
            m_answers.endWhatIsWritten(effects, *this);
        }
    }
    struct ClassOfHolder {
        LayoutID layoutAlone;
        bool isNewCellItself { false };
    };
    struct WriterOfOneObject {
        Reference root;
        ClassOfHolder classOfRoot;
        const ScopedLambda<bool(Reference, LayoutID)>& isOfLayoutAlone;
        const ScopedLambda<ClassOfHolder(Reference holder)>& classOf;
    };
    struct FreshCellWriter {
        std::span<const Reference> cells;
        const ScopedLambda<Reference(Reference holder)>& rootOf;
    };
    void invalidate(const Effects&, NOESCAPE const ScopedLambda<bool(Reference holder)>& mayBeWritten, NOESCAPE const ScopedLambda<bool(LayoutID classOfGuard, PropertyKeyID nameOfGuard)>& mayEndGuard, const MemoryLocation* writtenAlone = nullptr, const WriterOfOneObject* = nullptr, const FreshCellWriter* = nullptr);
    void invalidateHeap()
    {
        clearRestartMark();
        if (!holdsNoRecord())
            endWhatIsNotFresh();
        m_answers.endWhatIsNotFresh(*this);
    }
    void clear()
    {
        m_marks = { 0, m_marks.kinds & programIsWhole };
        if (m_held)
            dropWhatIsHeld();
        m_answers.clear();
    }
    void freeWhatIsHeld()
    {
        m_marks = { 0, m_marks.kinds & programIsWhole };
        release(std::exchange(m_held, nullptr));
        m_answers.clear();
    }
    void forgetAllButFreshObjects()
    {
        if (!holdsFreshObjects()) {
            clear();
            return;
        }
        clearRestartMark();
        endWhatIsNotFresh();
        Held& held = heldToWrite();
        held.m_checkedNames.clear();
        held.m_filterOfValuesWithCheckedNames = 0;
        held.m_builtinsRead.clear();
        m_marks.kinds &= ~Kinds::checkedNames;
        m_answers.clear();
    }

    struct RecordsLastMet;
    void meetWith(const WalkState& other, RecordsLastMet* = nullptr);

    bool isEmpty() const { return !m_marks.restartMark && (!m_held || m_held->isEmpty()) && m_answers.isEmpty(); }

private:
    static constexpr uint32_t none = UINT32_MAX;
    static constexpr size_t holdersFoundWithoutTable = 8;
    static constexpr size_t placesFoundWithoutTable = 16;
    static constexpr size_t placesThatLieInHeld = 9;
    static constexpr uint32_t recordsGoneThroughWithoutLists = 4096;
    static constexpr uint32_t timesGoneThroughForOneListing = 16;

    struct Kinds {
        static constexpr uint32_t checkedNames = 1u << 0;
        static constexpr uint32_t shapes = 1u << 1;
        static constexpr uint32_t freshObjects = 1u << 2;
        static constexpr uint32_t places = 1u << 3;
        static constexpr uint32_t cellFacts = 1u << 4;
        static constexpr uint32_t records = shapes | freshObjects | places | cellFacts;
    };
    static constexpr uint32_t programIsWhole = 1u << 31;
    static constexpr uint32_t newestUndoableStoreShift = 8;
    static constexpr uint32_t newestUndoableStoreMask = (programIsWhole - 1) & ~((1u << newestUndoableStoreShift) - 1);
    static constexpr uint32_t keptWhereNothingIsHeld = programIsWhole | newestUndoableStoreMask;
    static constexpr uint64_t programIsWholeAmongMarks = static_cast<uint64_t>(programIsWhole) << 32;

    enum class PlaceKind : uint8_t { Slot, Variable, EnvironmentField, Element, GuardOfStores, GuardOfReads };
    enum class LayoutsOfSlots : uint8_t { None, One, Several };

    struct Holder {
        Reference object;
        TypeID shape;
        uint32_t firstPlace { none };
        uint32_t firstElement { none };
        uint32_t layoutOfSlots { 0 };
        uint32_t orderWord { 0 };
        uint32_t placeAmongReachedByEffects { none };
        uint32_t nextFreshWithElements { none };
        uint32_t numberOfPlaces { 0 };
        uint32_t numberOfElements { 0 };
        uint64_t maskOfPlaces { 0 };
        uint8_t kindsOfPlaces { 0 };
        LayoutsOfSlots layoutsOfSlots { LayoutsOfSlots::None };
        OptionSet<CellFact> cellFacts;
        bool isFresh { false };
        bool hasEnded { false };
        bool isListedAsFreshWithElements { false };
        bool elementsLieOutsideViews : 1 { false };
        bool hasElementsUnderOtherKeys : 1 { false };
    };
    static_assert(sizeof(Holder) == 56);
    static_assert(static_cast<unsigned>(PlaceKind::GuardOfReads) < 8 * sizeof(Holder::kindsOfPlaces));
    struct Place {
        static constexpr uint8_t entireHeap = 64;

        uint64_t mask() const { return bit < entireHeap ? uint64_t { 1 } << bit : LocationMasks::entireHeap; }

        PlaceKind kind;
        uint8_t bit { entireHeap };
        bool isListed { true };
        bool isUnderConstant { false };
        uint32_t holder { none };
        uint32_t first { 0 };
        uint32_t second { 0 };
        Reference value;
        uint32_t next { none };
    };
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
        void takeWhole(const ProgramOfState& other)
        {
            program = other.program;
            slots = other.slots;
            numbers = other.numbers;
        }
        bool isWhole() const { return program && slots && numbers; }

        const Program* program { nullptr };
        const SlotPlan* slots { nullptr };
        NumbersOfWalkStates* numbers { nullptr };
    };

public:
    struct RecordsLastMet {
        SharedArray<Holder, holdersFoundWithoutTable> holders;
        SharedArray<Place, placesThatLieInHeld> places;
        uint32_t numberOfOwnHolders { 0 };
        uint32_t numberOfOwnPlaces { 0 };
        bool lieAsOwn { false };
    };

private:
    struct Held {
        static void* operator new(size_t) { return ThreadBlockCache<Held, KindOfBlocks::HeldByWalkState>::allocate(); }
        static void operator delete(void* held) { ThreadBlockCache<Held, KindOfBlocks::HeldByWalkState>::free(held); }

        Held() = default;
        Held(const Held&);
        Held& operator=(const Held&) = delete;
        ~Held()
        {
            if (m_lists) [[unlikely]]
                dropLists();
        }

        static uint8_t bitOf(const ProgramOfState&, const MemoryLocation&);
        static PropertyKeyID keyOfSlot(const ProgramOfState&, FieldReference);
        static uint8_t bitOfSlot(const ProgramOfState&, FieldReference);
        static bool liesBesideWhatIsWritten(const ProgramOfState&, const Place&, uint64_t written, const MemoryLocation* writtenAlone);

        void noteShape(Reference object, TypeID proven);
        void noteFresh(Reference object);
        void recordCellFacts(Reference object, OptionSet<CellFact>);
        void invalidateCellFactsOfHolder(Reference object, OptionSet<CellFact>);
        void noteOrderWord(Reference object, std::optional<uint32_t>);
        std::optional<uint32_t> orderWordOf(Reference object) const;
        void noteStoresPassAbove(LayoutID classLayout, PropertyKeyID, Reference test);
        void noteGuardOfReadsHolds(LayoutID classLayout, PropertyKeyID, Reference test, ClassSide, AskerOfGuardOfReads);
        bool mayHoldGuards() const;
        bool someGuardHoldsOf(LayoutID classLayout) const;
        void noteSlot(const ProgramOfState&, Reference object, FieldReference, Reference value);
        void noteVariable(const ProgramOfState&, VariableID, Reference value);
        void noteEnvironmentField(const ProgramOfState&, Reference environment, FieldReference, Reference value);
        void noteElement(Reference object, Reference index, Reference value, bool isUnderConstant);
        void noteElementWithNaNForHole(Reference object, Reference index, Reference value);
        void noteElementsLieOutsideViews(Reference object);
        void invalidateAliasedSlots(const ProgramOfState&, Reference object, FieldReference, NOESCAPE const ScopedLambda<bool(Reference other)>& mayBeSameObject, NOESCAPE const ScopedLambda<bool(Reference, LayoutID)>& isOfLayoutAlone);
        void meetWith(const Held&, RecordsLastMet*);
        void endWhatIsWritten(const ProgramOfState&, const Effects&, NOESCAPE const ScopedLambda<bool(Reference holder)>& mayBeWritten, NOESCAPE const ScopedLambda<bool(LayoutID classOfGuard, PropertyKeyID nameOfGuard)>& mayEndGuard, const MemoryLocation* writtenAlone, const WriterOfOneObject*, const FreshCellWriter*);

        bool mayHaveRecordOf(Reference object) const { return m_filterOfHolders & bitInFilterOfHolders(object); }
        TypeID shapeOfHolderOf(Reference) const;
        OptionSet<CellFact> cellFactsOfHolder(Reference) const;
        bool holderIsFresh(Reference) const;
        bool letLeaveIfFresh(Reference);
        bool restsOnHeapsAmong(const LocationMasks& writes) const;
        void endWhatIsWritten(const ProgramOfState&, const Effects&, const MemoryLocation* writtenAlone);
        void endWhatIsNotFresh();
        void dropAllRecords();
        void clear();

        inline uint32_t findRecordOfHolder(Reference) const;
        inline uint32_t findRecordOfPlace(uint32_t holder, PlaceKind, uint32_t first, uint32_t second) const;
        inline uint32_t findHolder(Reference) const;
        inline const Holder* findRecordOfHolderThatHasNotEnded(Reference) const;
        uint32_t findRecordOfHolderInTable(Reference) const;
        const Holder* findRecordOfHolderThatHasNotEndedInTable(Reference) const;
        uint32_t findRecordOfPlaceInTable(uint32_t holder, PlaceKind, uint32_t first, uint32_t second) const;
        uint32_t findOrAddHolder(Reference);
        inline uint32_t findPlace(uint32_t holder, PlaceKind, uint32_t first, uint32_t second) const;
        Reference valueOfPlace(PlaceKind, Reference base, uint32_t first, uint32_t second) const;
        void notePlace(PlaceKind, Reference base, uint32_t first, uint32_t second, Reference value, NOESCAPE const Invocable<uint8_t()> auto& bit, bool isUnderConstant = false);
        void endElementsUnderOtherKeys(uint32_t holder);
        void endElementsOf(const ProgramOfState&, uint32_t holder);
        enum class ListOfPlaces : uint8_t { AllButElements, Elements };
        void endPlace(uint32_t place);
        void endPlacesOf(uint32_t holder, ListOfPlaces, NOESCAPE const Invocable<bool(const Place&)> auto& ends);
        void endSlotsUnderOtherLayouts(uint32_t holder, uint32_t layout);
        void endShape(uint32_t holder);
        void invalidateCellFacts(uint32_t holder, OptionSet<CellFact>);
        void endHolder(uint32_t holder);
        void letLeave(uint32_t holder);
        void listAsReachedByEffects(uint32_t holder);
        void takeOffListOfReachedByEffects(uint32_t holder);
        void listAsFreshWithElements(uint32_t holder);
        void countAmongReachedByEffects(const Holder&);
        void countNoLongerAmongReachedByEffects(const Holder&);
        uint64_t writtenUnderPlacesReachedByEffects(const LocationMasks& writes) const;
        void forEachHolderReachedByEffects(NOESCAPE const Invocable<void(uint32_t holder)> auto&);
        void fillTableOfHolders();
        void fillTableOfPlaces();

        struct Lists;
        struct WhatEnds {
            bool shapes { false };
            bool elements { false };
            bool sparesElementsOutsideViews { false };
            OptionSet<CellFact> facts;
            uint64_t slots { 0 };
            uint64_t variables { 0 };
            uint64_t environmentFields { 0 };
            uint64_t guards { 0 };
            const ScopedLambda<bool(Reference holder)>* mayBeWritten { nullptr };
            const ScopedLambda<bool(LayoutID classOfGuard, PropertyKeyID nameOfGuard)>* mayEndGuard { nullptr };
            VariableID variableWrittenAlone;
            const WriterOfOneObject* writerOfOneObject { nullptr };
            const FreshCellWriter* freshCellWriter { nullptr };
            const ProgramOfState* program { nullptr };
            const MemoryLocation* writtenAlone { nullptr };
        };
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
        void invalidateAliasedSlotsByLists(uint32_t own, uint8_t bit, FieldReference, bool storesIntoLayoutAlone, NOESCAPE const ScopedLambda<bool(Reference other)>& mayBeSameObject, NOESCAPE const ScopedLambda<bool(Reference, LayoutID)>& isOfLayoutAlone);
        void makeLists();
        void dropLists();
        void listPlace(uint32_t place);
        void listHolder(uint32_t holder);
        void listWhatHangsOn(uint32_t holder);
        void endHolderIfNothingHangsOn(uint32_t holder);
        bool hasManyEndedRecords() const
        {
            if (!m_numberOfHolders)
                return !holdsNoRecord();
            size_t numberLeft = m_numberOfPlaces + m_numberOfHolders;
            return m_places.size() + m_holders.size() - numberLeft > numberLeft + placesFoundWithoutTable + holdersFoundWithoutTable;
        }
        void dropEndedRecordsIfMany()
        {
            if (hasManyEndedRecords()) [[unlikely]]
                dropManyEndedRecords();
        }
        void dropManyEndedRecords();
        void dropEndedRecords();

        using Table = SharedArray<uint32_t, 0, 8>;
        static constexpr uint32_t placesOfNewestValue = 64;
        static constexpr uint32_t placesWithoutNewestValues = 256;
        void noteNewestValue(uint32_t position, Reference value)
        {
            if (m_places.size() > placesWithoutNewestValues) [[unlikely]]
                noteNewestValueSlowCase(position, value);
        }
        void noteNewestValueSlowCase(uint32_t position, Reference value);
        bool holdsOnlyValuesBefore(ValueID oldest, uint32_t group) const { return group < m_newestValues.size() && m_newestValues[group] < oldest.index(); }
        static uint32_t findInTable(const Table&, unsigned hash, NOESCAPE const Invocable<bool(uint32_t)> auto& isSought);
        static void addToTable(Table&, unsigned hash, uint32_t position);

        bool holdsNoRecord() const { return m_holders.isEmpty(); }
        bool holdsNothingThatNoEffectEnds() const { return m_checkedNames.isEmpty() && m_bindingsPastDeadZone.isEmpty() && m_builtinsRead.isEmpty(); }
        bool isEmpty() const { return !m_numberOfShapes && !m_numberOfPlaces && !m_numberOfFreshHolders && !m_numberOfFactHolders && holdsNothingThatNoEffectEnds(); }

        static uint64_t bitInFilterOfHolders(Reference object) { return uint64_t { 1 } << (object.bits() & 63); }

        uint32_t numberOfOwners { 1 };
        Lists* m_lists { nullptr };
        uint32_t m_recordsGoneThrough { 0 };
        uint32_t m_firstFreshHolderWithElements { none };
        uint32_t m_numberOfHolders { 0 };
        uint32_t m_numberOfPlaces { 0 };
        uint32_t m_numberOfShapes { 0 };
        uint32_t m_numberOfFactHolders { 0 };
        uint32_t m_numberOfFreshHolders { 0 };
        uint32_t m_endOfFreshObjects { 0 };
        uint32_t m_numberOfShapesReachedByEffects { 0 };
        uint32_t m_reachedFactHolderCount { 0 };
        uint32_t m_numberOfPlacesReachedByEffects { 0 };
        uint32_t m_numberOfElementsReachedByEffects { 0 };
        OptionSet<CellFact> m_cellFactsReachedByEffects;
        uint16_t m_kindsOfPlacesReachedByEffects { 0 };
        uint64_t m_maskOfPlacesReachedByEffects { 0 };
        uint64_t m_filterOfHolders { 0 };
        SharedSetOfNumbers m_checkedNames;
        uint64_t m_filterOfValuesWithCheckedNames { 0 };
        SharedSetOfNumbers m_bindingsPastDeadZone;
        SharedSetOfNumbers m_builtinsRead;
        Table m_tableOfHolders;
        Table m_tableOfPlaces;
        SharedArray<uint32_t, holdersFoundWithoutTable> m_holdersReachedByEffects;
        SharedArray<Holder, holdersFoundWithoutTable> m_holders;
        SharedArray<Place, placesThatLieInHeld> m_places;
        Table m_newestValues;
        uint32_t m_newestStoreIntoUnmadeRecord { 0 };
    };

    NEVER_INLINE void letGoOfHeld() { release(m_held); }
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
    void endWhatIsWritten(const Effects&, const MemoryLocation* writtenAlone);
    void endWhatIsNotFresh();
    void noteBindingPastDeadZone(uint32_t number)
    {
        if (!m_held || !m_held->m_bindingsPastDeadZone.contains(number))
            heldToWrite().m_bindingsPastDeadZone.add(number);
    }
    Reference checkedNameHeldFrom(uint32_t place) const;
    Reference valueOfPlace(PlaceKind kind, Reference base, uint32_t first, uint32_t second) const { return mayHold(Kinds::places) && m_held->m_numberOfPlaces && m_held->mayHaveRecordOf(base) ? m_held->valueOfPlace(kind, base, first, second) : Reference(); }
    bool writesInPlace() const { return m_held && m_held->numberOfOwners == 1; }
    bool noteChangesNothing(Reference known, Reference value) const
    {
        if (!m_held)
            return !value;
        return value && known == value && !m_held->hasManyEndedRecords();
    }

    bool holdsNoRecord() const { return !m_held || m_held->holdsNoRecord(); }
    bool holdsNothing() const { return !m_held || (m_held->holdsNoRecord() && m_held->holdsNothingThatNoEffectEnds()); }

    ALWAYS_INLINE bool mayHold(uint32_t kind) const
    {
        ASSERT(marksFitWhatIsHeld());
        return m_marks.kinds & kind;
    }
    bool marksFitWhatIsHeld() const
    {
        if (!!(m_marks.kinds & programIsWhole) != m_program.isWhole())
            return false;
        if (!m_held)
            return !(m_marks.kinds & ~keptWhereNothingIsHeld);
        return !!(m_marks.kinds & Kinds::checkedNames) == !m_held->m_checkedNames.isEmpty()
            && (m_marks.kinds & Kinds::shapes || !m_held->m_numberOfShapes)
            && (m_marks.kinds & Kinds::freshObjects || !m_held->m_numberOfFreshHolders)
            && (m_marks.kinds & Kinds::places || !m_held->m_numberOfPlaces)
            && (m_marks.kinds & Kinds::cellFacts || !m_held->m_numberOfFactHolders);
    }
    void noteKindsOfRecordsLeft()
    {
        uint32_t kinds = m_marks.kinds & ~Kinds::records;
        if (m_held) {
            if (m_held->m_numberOfShapes)
                kinds |= Kinds::shapes;
            if (m_held->m_numberOfFreshHolders)
                kinds |= Kinds::freshObjects;
            if (m_held->m_numberOfPlaces)
                kinds |= Kinds::places;
            if (m_held->m_numberOfFactHolders)
                kinds |= Kinds::cellFacts;
        }
        m_marks.kinds = kinds;
    }
    void noteWhetherProgramIsWhole()
    {
        if (m_program.isWhole())
            m_marks.kinds |= programIsWhole;
        else
            m_marks.kinds &= ~programIsWhole;
    }
    void takeMarksOfStateWithoutWholeProgram(WalkState&);

    struct Marks {
        uint32_t restartMark { 0 };
        uint32_t kinds { 0 };
    };

    Held* m_held { nullptr };
    KnownAnswers m_answers;
    Marks m_marks;
    ProgramOfState m_program;
};

bool leavesDeadZoneForGood(const Program&, VariableID);
bool leavesDeadZoneForGood(const Program&, FieldReference);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
