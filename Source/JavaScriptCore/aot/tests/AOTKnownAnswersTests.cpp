/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTNativeTest.h"
#include "AOTTypeTable.h"
#include "AOTWalkState.h"
#include <array>
#include <optional>
#include <wtf/WeakRandom.h>

namespace JSC { namespace AOT {

std::span<const NativeTest> knownAnswersTests();

using namespace Typed;

namespace {

struct AnswerOfKnownAnswersTests {
    friend bool operator==(const AnswerOfKnownAnswersTests&, const AnswerOfKnownAnswersTests&) = default;

    uint32_t key { 0 };
    uint64_t noted { 0 };
    Reference holder;
    AbstractHeapSet restsOn;
    bool endsAtSuspension { false };
};

struct PlainTableOfKnownAnswersTests {
    uint32_t numberOf(const AnswerOfKnownAnswersTests& answer)
    {
        for (size_t number = answers.size(); number--;) {
            if (answers[number].key != answer.key)
                continue;
            if (answers[number] == answer)
                return number;
            if (!answer.restsOn.isEmpty())
                break;
        }
        answers.append(answer);
        return answers.size() - 1;
    }

    Vector<AnswerOfKnownAnswersTests> answers;
};

struct PlainListOfKnownAnswersTests {
    const AnswerOfKnownAnswersTests* find(const PlainTableOfKnownAnswersTests& table, uint32_t key) const
    {
        for (uint32_t number : held) {
            if (table.answers[number].key == key)
                return &table.answers[number];
        }
        return nullptr;
    }

    void end(const PlainTableOfKnownAnswersTests& table, uint32_t key)
    {
        held.removeAllMatching([&](uint32_t number) {
            return table.answers[number].key == key;
        });
    }

    void note(PlainTableOfKnownAnswersTests& table, const AnswerOfKnownAnswersTests& answer)
    {
        end(table, answer.key);
        held.append(table.numberOf(answer));
    }

    void meetWith(const PlainListOfKnownAnswersTests& other)
    {
        held.removeAllMatching([&](uint32_t number) {
            return !other.held.contains(number);
        });
    }

    void endWhatIsWritten(const PlainTableOfKnownAnswersTests& table, std::optional<AbstractHeapSet> written, const WalkState& stateAskedWhatIsFresh)
    {
        held.removeAllMatching([&](uint32_t number) {
            const AnswerOfKnownAnswersTests& answer = table.answers[number];
            if (!written && answer.endsAtSuspension)
                return true;
            AbstractHeapSet reached = written ? answer.restsOn & *written : answer.restsOn;
            if (reached.isEmpty())
                return false;
            return !perObjectHeaps.containsAll(reached) || !answer.holder || !stateAskedWhatIsFresh.isFresh(answer.holder);
        });
    }

    Vector<uint32_t> held;
};

} // anonymous namespace

static Reference valueOfKnownAnswersTests(uint32_t number)
{
    return Reference::value(ValueID(number));
}

static Effects effectsThatWriteInKnownAnswersTests(AbstractHeapSet written)
{
    Effects effects = Effects::none();
    effects.writes = LocationMasks(written);
    return effects;
}

static void noteInKnownAnswersTests(WalkState& state, const AnswerOfKnownAnswersTests& answer, KnownAnswer::Family family = KnownAnswer::Family::Note, bool mayBeNumbered = true)
{
    KnownAnswer noted;
    noted.noted = answer.noted;
    noted.holder = answer.holder;
    noted.family = family;
    noted.endsAtSuspension = answer.endsAtSuspension;
    std::array<uint32_t, 1> key { answer.key };
    state.answers().note(key, noted, LocationMasks(answer.restsOn), mayBeNumbered, noted.holder && state.isFresh(noted.holder));
}

static std::optional<uint64_t> findInKnownAnswersTests(const WalkState& state, uint32_t key)
{
    std::array<uint32_t, 1> words { key };
    const KnownAnswer* answer = state.answers().find(words);
    return answer ? std::optional<uint64_t>(answer->noted) : std::nullopt;
}

static void testSetsAnswerAsPlainListsDo()
{
    constexpr size_t numberOfSets = 4;
    for (uint32_t seed = 1; seed <= 6; ++seed) {
        WeakRandom random(seed);
        std::array<SetOfAnswersForGood, numberOfSets> sets;
        std::array<ListOfAnswersUntilEnded, numberOfSets> lists;
        std::array<Vector<uint32_t>, numberOfSets> plain;
        uint32_t nextNumber = 0;
        uint32_t stride = seed % 2 ? 1 : 977;
        for (uint32_t step = 0; step < 6000; ++step) {
            size_t place = random.getUint32(numberOfSets);
            size_t other = random.getUint32(numberOfSets);
            switch (random.getUint32(13)) {
            case 0:
            case 1:
            case 2:
            case 3:
            case 4: {
                uint32_t number = nextNumber;
                nextNumber += 1 + random.getUint32(stride);
                bool wasAbsent = !plain[place].contains(number);
                AOT_TEST_CHECK(sets[place].add(number) == wasAbsent && lists[place].add(number) == wasAbsent);
                plain[place].append(number);
                break;
            }
            case 5: {
                if (!nextNumber)
                    break;
                uint32_t number = random.getUint32(nextNumber);
                bool wasAbsent = !plain[place].contains(number);
                AOT_TEST_CHECK(sets[place].add(number) == wasAbsent && lists[place].add(number) == wasAbsent);
                if (wasAbsent)
                    plain[place].append(number);
                break;
            }
            case 12: {
                if (nextNumber < 64)
                    break;
                uint32_t number = 32 + random.getUint32(nextNumber - 32);
                for (uint32_t fall = random.getUint32(24); fall && number; --fall) {
                    number -= std::min(number, 1 + random.getUint32(3));
                    bool wasAbsent = !plain[place].contains(number);
                    AOT_TEST_CHECK(sets[place].add(number) == wasAbsent && lists[place].add(number) == wasAbsent);
                    if (wasAbsent)
                        plain[place].append(number);
                }
                break;
            }
            case 6:
            case 7: {
                if (plain[place].isEmpty())
                    break;
                uint32_t number = random.getUint32(4) ? plain[place][random.getUint32(plain[place].size())] : random.getUint32(nextNumber);
                bool wasMember = plain[place].contains(number);
                AOT_TEST_CHECK(sets[place].remove(number) == wasMember && lists[place].remove(number) == wasMember);
                plain[place].removeAll(number);
                break;
            }
            case 8:
                sets[place] = sets[other];
                lists[place] = lists[other];
                plain[place] = plain[other];
                break;
            case 9:
            case 10: {
                if (place == other)
                    break;
                sets[place].keepMembersOf(sets[other]);
                lists[place].keepMembersOf(lists[other]);
                plain[place].removeAllMatching([&](uint32_t number) {
                    return !plain[other].contains(number);
                });
                break;
            }
            case 11:
                if (random.getUint32(8))
                    break;
                sets[place].clear();
                lists[place].clear();
                plain[place].clear();
                break;
            }
            if (step % 50)
                continue;
            for (size_t index = 0; index < numberOfSets; ++index) {
                AOT_TEST_CHECK(sets[index].size() == plain[index].size() && lists[index].size() == plain[index].size());
                AOT_TEST_CHECK(sets[index].isEmpty() == plain[index].isEmpty() && lists[index].isEmpty() == plain[index].isEmpty());
                for (uint32_t number : plain[index])
                    AOT_TEST_CHECK(sets[index].contains(number) && lists[index].contains(number));
                uint32_t numberSeen = 0;
                uint32_t last = 0;
                lists[index].forEach([&](uint32_t number) {
                    AOT_TEST_CHECK(plain[index].contains(number) && (!numberSeen || last < number));
                    last = number;
                    ++numberSeen;
                });
                AOT_TEST_CHECK(numberSeen == plain[index].size());
                for (uint32_t trial = 0; trial < 20 && nextNumber; ++trial) {
                    uint32_t number = random.getUint32(nextNumber + 100);
                    AOT_TEST_CHECK(sets[index].contains(number) == plain[index].contains(number) && lists[index].contains(number) == plain[index].contains(number));
                }
            }
        }
    }
}

static void testAnswersAsPlainListsDo()
{
    constexpr size_t numberOfStates = 4;
    constexpr uint32_t numberOfKeys = 24;
    constexpr uint32_t numberOfHolders = 6;
    static constexpr AbstractHeapSet heapsRestedOn[] {
        { }, { }, { AbstractHeap::ArrayStorage }, { AbstractHeap::ArrayStorage, AbstractHeap::Elements }, { AbstractHeap::ObjectFields }, { AbstractHeap::TypedArrayStorage }, { AbstractHeap::NamedProperties, AbstractHeap::ModuleVariables }
    };
    static constexpr AbstractHeapSet heapsWritten[] {
        { AbstractHeap::ArrayStorage }, { AbstractHeap::Elements }, { AbstractHeap::ObjectFields }, { AbstractHeap::TypedArrayStorage }, { AbstractHeap::ModuleVariables }, { AbstractHeap::NamedProperties, AbstractHeap::ArrayStorage }, { AbstractHeap::AllocatorState }
    };
    for (uint32_t seed = 1; seed <= 8; ++seed) {
        WeakRandom random(seed);
        AnswersOfWalk table;
        PlainTableOfKnownAnswersTests plainTable;
        std::array<WalkState, numberOfStates> states;
        std::array<PlainListOfKnownAnswersTests, numberOfStates> lists;
        states[0].setAnswers(table);
        for (size_t index = 1; index < numberOfStates; ++index)
            states[index].setAnswers(table);

        for (uint32_t step = 0; step < 8000; ++step) {
            size_t place = random.getUint32(numberOfStates);
            size_t other = random.getUint32(numberOfStates);
            switch (random.getUint32(16)) {
            case 0:
            case 1:
            case 2:
            case 3:
            case 4:
            case 5: {
                AnswerOfKnownAnswersTests answer;
                answer.key = random.getUint32(numberOfKeys);
                answer.noted = random.getUint32(3);
                answer.restsOn = heapsRestedOn[answer.key % std::size(heapsRestedOn)];
                answer.holder = answer.key % 5 ? valueOfKnownAnswersTests(answer.key % numberOfHolders) : Reference();
                answer.endsAtSuspension = !(answer.key % 7);
                noteInKnownAnswersTests(states[place], answer);
                lists[place].note(plainTable, answer);
                break;
            }
            case 6: {
                uint32_t key = random.getUint32(numberOfKeys);
                std::array<uint32_t, 1> words { key };
                states[place].answers().end(words);
                lists[place].end(plainTable, key);
                break;
            }
            case 7:
                states[place] = states[other];
                lists[place] = lists[other];
                break;
            case 8:
                if (place == other)
                    break;
                states[place] = WTF::move(states[other]);
                std::swap(lists[place], lists[other]);
                break;
            case 9:
            case 10:
                if (place == other)
                    break;
                states[place].meetWith(states[other]);
                lists[place].meetWith(lists[other]);
                break;
            case 11:
            case 12: {
                AbstractHeapSet written = heapsWritten[random.getUint32(std::size(heapsWritten))];
                states[place].invalidate(effectsThatWriteInKnownAnswersTests(written));
                lists[place].endWhatIsWritten(plainTable, written, states[place]);
                break;
            }
            case 13:
                if (random.getUint32(3))
                    break;
                states[place].invalidateHeap();
                lists[place].endWhatIsWritten(plainTable, std::nullopt, states[place]);
                break;
            case 14: {
                Reference holder = valueOfKnownAnswersTests(random.getUint32(numberOfHolders));
                if (random.getUint32(2))
                    states[place].noteFresh(holder);
                else
                    states[place].markEscaped(holder);
                break;
            }
            case 15:
                if (random.getUint32(6))
                    break;
                states[place].clear();
                lists[place].held.clear();
                break;
            }
            for (size_t index = 0; index < numberOfStates; ++index) {
                AOT_TEST_CHECK(states[index].answers().size() == lists[index].held.size());
                for (uint32_t key = 0; key < numberOfKeys; ++key) {
                    const AnswerOfKnownAnswersTests* held = lists[index].find(plainTable, key);
                    AOT_TEST_CHECK(findInKnownAnswersTests(states[index], key) == (held ? std::optional<uint64_t>(held->noted) : std::nullopt));
                }
            }
        }
    }
}

static void testWhatEveryWayHasNotedHoldsBehindJoin()
{
    AnswersOfWalk table;
    WalkState state;
    state.setAnswers(table);
    noteInKnownAnswersTests(state, { 1, 10, { }, { }, false });
    WalkState taken = state;
    WalkState notTaken = WTF::move(state);
    noteInKnownAnswersTests(taken, { 2, 20, valueOfKnownAnswersTests(0), { AbstractHeap::ArrayStorage }, false });
    noteInKnownAnswersTests(taken, { 3, 30, { }, { }, false });
    noteInKnownAnswersTests(taken, { 4, 40, { }, { }, false });
    noteInKnownAnswersTests(notTaken, { 5, 50, { }, { }, false });
    noteInKnownAnswersTests(notTaken, { 2, 20, valueOfKnownAnswersTests(0), { AbstractHeap::ArrayStorage }, false });
    noteInKnownAnswersTests(notTaken, { 3, 31, { }, { }, false });
    taken.meetWith(notTaken);
    AOT_TEST_CHECK(findInKnownAnswersTests(taken, 1) == std::optional<uint64_t>(10));
    AOT_TEST_CHECK(findInKnownAnswersTests(taken, 2) == std::optional<uint64_t>(20));
    AOT_TEST_CHECK(!findInKnownAnswersTests(taken, 3) && !findInKnownAnswersTests(taken, 4) && !findInKnownAnswersTests(taken, 5));

    state = WalkState();
    noteInKnownAnswersTests(state, { 6, 60, { }, { }, false });
    AOT_TEST_CHECK(findInKnownAnswersTests(state, 6) == std::optional<uint64_t>(60));
    WalkState moved = WTF::move(state);
    noteInKnownAnswersTests(state, { 7, 70, { }, { }, false });
    AOT_TEST_CHECK(findInKnownAnswersTests(moved, 6) == std::optional<uint64_t>(60) && !findInKnownAnswersTests(moved, 7));
    AOT_TEST_CHECK(findInKnownAnswersTests(state, 7) == std::optional<uint64_t>(70) && !findInKnownAnswersTests(state, 6));
}

static void testSuspensions()
{
    AnswersOfWalk table;
    WalkState state;
    state.setAnswers(table);
    Reference fresh = valueOfKnownAnswersTests(0);
    state.noteFresh(fresh);
    noteInKnownAnswersTests(state, { 1, 1, { }, { }, false });
    noteInKnownAnswersTests(state, { 2, 2, { }, { }, true });
    noteInKnownAnswersTests(state, { 3, 3, fresh, { AbstractHeap::ArrayStorage }, true });
    noteInKnownAnswersTests(state, { 4, 4, fresh, { AbstractHeap::ArrayStorage }, false });

    state.invalidate(effectsThatWriteInKnownAnswersTests({ AbstractHeap::ArrayStorage, AbstractHeap::AllocatorState }));
    state.invalidate(Effects::unknownCode());
    AOT_TEST_CHECK(findInKnownAnswersTests(state, 1) && findInKnownAnswersTests(state, 2) && findInKnownAnswersTests(state, 3) && findInKnownAnswersTests(state, 4));

    WalkState other = state;
    Effects suspends = Effects::unknownCode();
    suspends.maySuspend = true;
    state.invalidate(suspends);
    AOT_TEST_CHECK(findInKnownAnswersTests(state, 1) && !findInKnownAnswersTests(state, 2) && !findInKnownAnswersTests(state, 3) && findInKnownAnswersTests(state, 4));
    other.invalidateHeap();
    AOT_TEST_CHECK(findInKnownAnswersTests(other, 1) && !findInKnownAnswersTests(other, 2) && !findInKnownAnswersTests(other, 3) && findInKnownAnswersTests(other, 4));
}

static void testWriterEndsWhatItMayWrite()
{
    AnswersOfWalk table;
    NumbersOfWalkStates numbers;
    WalkState state;
    state.setAnswers(table);
    state.setNumbers(numbers);
    Reference written = valueOfKnownAnswersTests(1);
    Reference other = valueOfKnownAnswersTests(2);
    Reference held = valueOfKnownAnswersTests(3);
    FieldReference field { LayoutID(0u), 0 };
    PropertyKeyID key(0u);
    for (Reference holder : { written, other }) {
        state.noteShape(holder, TypeTable::object);
        state.noteSlot(holder, field, held);
        state.noteElement(holder, held, held);
        state.recordCellFacts(holder, CellFact::NotFrozen);
    }
    state.noteVariable(VariableID(0u), held);
    state.noteGuardOfReadsHolds(LayoutID(1u), key, held);
    state.noteGuardOfReadsHolds(LayoutID(2u), key, held);
    state.noteThatGuardsHaveBegun(LayoutID(1u));
    noteInKnownAnswersTests(state, { 1, 1, written, { AbstractHeap::ObjectFields }, false });
    noteInKnownAnswersTests(state, { 2, 2, other, { AbstractHeap::ObjectFields }, false });
    noteInKnownAnswersTests(state, { 3, 3, other, { AbstractHeap::ModuleVariables }, false });
    noteInKnownAnswersTests(state, { 4, 4, { }, { AbstractHeap::ObjectFields }, false });

    uint32_t numberOfQuestionsAboutOther = 0;
    auto mayBeWritten = [&](Reference holder) {
        numberOfQuestionsAboutOther += holder == other;
        return holder == written;
    };
    auto mayEndGuard = [&](LayoutID classOfGuard, PropertyKeyID) {
        return classOfGuard == LayoutID(1u);
    };
    state.invalidate(effectsThatWriteInKnownAnswersTests({ AbstractHeap::ObjectFields, AbstractHeap::ShapeWords, AbstractHeap::Elements, AbstractHeap::MemberGuards }), mayBeWritten, mayEndGuard);
    AOT_TEST_CHECK(!state.shapeOf(written) && !state.slot(written, field) && !state.element(written, held) && state.cellFacts(written).isEmpty());
    AOT_TEST_CHECK(state.shapeOf(other) && state.slot(other, field) == held && state.element(other, held) == held && state.cellFacts(other).contains(CellFact::NotFrozen));
    AOT_TEST_CHECK(state.variable(VariableID(0u)) == held);
    AOT_TEST_CHECK(!state.guardOfReadsHolds(LayoutID(1u), key) && state.guardOfReadsHolds(LayoutID(2u), key));
    AOT_TEST_CHECK(!state.someGuardHoldsOf(LayoutID(1u)) && state.someGuardHoldsOf(LayoutID(2u)));
    AOT_TEST_CHECK(!findInKnownAnswersTests(state, 1) && findInKnownAnswersTests(state, 2) && findInKnownAnswersTests(state, 3));
    AOT_TEST_CHECK(!findInKnownAnswersTests(state, 4));
    AOT_TEST_CHECK(numberOfQuestionsAboutOther == 2);

    state.invalidate(effectsThatWriteInKnownAnswersTests({ AbstractHeap::ModuleVariables }), mayBeWritten, mayEndGuard);
    AOT_TEST_CHECK(!state.variable(VariableID(0u)) && !findInKnownAnswersTests(state, 3) && findInKnownAnswersTests(state, 2));

    state.invalidate(effectsThatWriteInKnownAnswersTests({ AbstractHeap::TypedArrayElements }), mayBeWritten, mayEndGuard);
    AOT_TEST_CHECK(!state.element(other, held) && state.slot(other, field) == held);

    state.invalidate(Effects::unknownCode(), mayBeWritten, mayEndGuard);
    AOT_TEST_CHECK(!state.slot(other, field) && !state.guardOfReadsHolds(LayoutID(2u), key) && !findInKnownAnswersTests(state, 2));
    AOT_TEST_CHECK(state.guardsHaveBegun(LayoutID(1u)) && !state.guardsHaveBegun(LayoutID(2u)));
}

static void testNumberFarAboveOldOneCostsNoMore()
{
    constexpr uint32_t numberOfLoops = 200000;
    SetOfAnswersForGood old;
    old.add(3);
    SharedSetOfNumbers oldNumbers;
    oldNumbers.add(3);
    for (uint32_t loop = 1; loop <= numberOfLoops; ++loop) {
        uint32_t number = 4096 * loop + loop % 64;
        SetOfAnswersForGood set = old;
        AOT_TEST_CHECK(set.add(number) && !set.add(number) && set.size() == 2);
        AOT_TEST_CHECK(set.contains(3) && set.contains(number) && !set.contains(number - 1) && !set.contains(number / 2 + 64) && !set.contains(number + 1));
        AOT_TEST_CHECK(set.add(number - 4000) && set.contains(number - 4000) && set.contains(number) && set.size() == 3);
        set.keepMembersOf(old);
        AOT_TEST_CHECK(set.size() == 1 && set.contains(3) && !set.contains(number));

        SharedSetOfNumbers numbers = oldNumbers;
        numbers.add(number);
        AOT_TEST_CHECK(numbers.contains(3) && numbers.contains(number) && !numbers.contains(number - 1));
        numbers.keepMembersOf(oldNumbers);
        AOT_TEST_CHECK(numbers.contains(3) && !numbers.contains(number));
    }
    AOT_TEST_CHECK(old.size() == 1 && old.contains(3));
}

static void testNumbersThatFallCostNoMoreEach()
{
    constexpr uint32_t numberOfNumbers = 400000;
    ListOfAnswersUntilEnded list;
    for (uint32_t above = 0; above < 8; ++above)
        AOT_TEST_CHECK(list.add(3 * numberOfNumbers + above));
    AOT_TEST_CHECK(list.add(0));
    for (uint32_t number = numberOfNumbers; number; --number)
        AOT_TEST_CHECK(list.add(3 * number - 1) && !list.add(3 * number - 1) && list.contains(3 * number - 1) && !list.contains(3 * number - 2));
    AOT_TEST_CHECK(list.size() == numberOfNumbers + 9 && list.numberOfPlaces() <= 2 * list.size() + 16);
    uint32_t numberSeen = 0;
    uint32_t last = 0;
    list.forEach([&](uint32_t number) {
        AOT_TEST_CHECK(!numberSeen || last < number);
        last = number;
        ++numberSeen;
    });
    AOT_TEST_CHECK(numberSeen == list.size() && list.contains(0) && !list.contains(1) && list.contains(2) && !list.contains(3));
    for (uint32_t number = 1; number <= numberOfNumbers; ++number)
        AOT_TEST_CHECK(list.remove(3 * number - 1) && !list.contains(3 * number - 1));
    AOT_TEST_CHECK(list.size() == 9 && list.contains(0) && list.contains(3 * numberOfNumbers + 7));
}

static void testStoresIntoFreshObjectsCostNoMoreEach()
{
    constexpr uint32_t numberOfStatements = 200000;
    AnswersOfWalk table;
    WalkState state;
    state.setAnswers(table);
    Effects store = effectsThatWriteInKnownAnswersTests({ AbstractHeap::ObjectFields });
    for (uint32_t statement = 0; statement < numberOfStatements; ++statement) {
        noteInKnownAnswersTests(state, { 2 * statement, statement, valueOfKnownAnswersTests(2 * statement), { AbstractHeap::ObjectFields }, false });
        Reference fresh = valueOfKnownAnswersTests(2 * statement + 1);
        noteInKnownAnswersTests(state, { 2 * statement + 1, statement, fresh, { AbstractHeap::ObjectFields }, false });
        state.answers().endWhatIsWrittenIn(fresh, store);
        AOT_TEST_CHECK(findInKnownAnswersTests(state, 2 * statement) == std::optional<uint64_t>(statement) && !findInKnownAnswersTests(state, 2 * statement + 1));
    }
    AOT_TEST_CHECK(state.answers().size() == numberOfStatements);
}

std::span<const NativeTest> knownAnswersTests()
{
    static constexpr NativeTest tests[] {
        { "known-answers/sets-answer-as-plain-lists-do"_s, testSetsAnswerAsPlainListsDo },
        { "known-answers/answers-as-plain-lists-do"_s, testAnswersAsPlainListsDo },
        { "known-answers/what-every-way-has-noted-holds-behind-a-join"_s, testWhatEveryWayHasNotedHoldsBehindJoin },
        { "known-answers/suspensions"_s, testSuspensions },
        { "known-answers/a-writer-ends-what-it-may-write"_s, testWriterEndsWhatItMayWrite },
        { "known-answers/a-number-far-above-an-old-one-costs-no-more"_s, testNumberFarAboveOldOneCostsNoMore },
        { "known-answers/numbers-that-fall-cost-no-more-each"_s, testNumbersThatFallCostNoMoreEach },
        { "known-answers/stores-into-fresh-objects-cost-no-more-each"_s, testStoresIntoFreshObjectsCostNoMoreEach },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
