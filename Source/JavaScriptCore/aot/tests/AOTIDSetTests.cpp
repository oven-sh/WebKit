/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTChildProcess.h"
#include "AOTIDSets.h"
#include "AOTNativeTest.h"
#include "AOTObjects.h"
#include "AOTSharedAnswers.h"
#include "AOTSweepValueSets.h"
#include "AOTThreads.h"
#include "AOTTypeArena.h"
#include "AOTTypeRelations.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <compare>
#include <iterator>
#include <numeric>
#include <set>
#include <stdlib.h>
#include <string.h>
#include <utility>
#include <vector>
#include <wtf/DataLog.h>
#include <wtf/StdLibExtras.h>
#include <wtf/StringPrintStream.h>
#include <wtf/Threading.h>
#include <wtf/WeakRandom.h>

#if OS(WINDOWS)
#include <windows.h>
#endif

namespace JSC { namespace AOT { namespace Typed {

namespace {

using IDSetModel = std::set<uint32_t>;
using IDSetMembers = std::vector<uint32_t>;

constexpr uint32_t idOfEmptyIDSet = ObjectSetID().bits();
constexpr uint32_t largestSmallIDSetSize = 257;
constexpr uint32_t largestTestedIDSetSize = 100000;
constexpr std::array<uint32_t, 11> testedIDSetSizes { 0, 1, 16, 17, 255, 256, largestSmallIDSetSize, membersPerSetChunk - 1, membersPerSetChunk, membersPerSetChunk + 1, largestTestedIDSetSize };

enum class IDSetPattern : uint8_t { Dense, OneInEachChunk, Random, AtTheTop };
constexpr std::array<IDSetPattern, 4> testedIDSetPatterns { IDSetPattern::Dense, IDSetPattern::OneInEachChunk, IDSetPattern::Random, IDSetPattern::AtTheTop };

IDSetModel modelOfIDSet(IDSetPattern pattern, uint32_t size, WeakRandom& random)
{
    IDSetModel model;
    for (uint32_t index = 0; index < size; ++index) {
        switch (pattern) {
        case IDSetPattern::Dense:
            model.insert(200 + index);
            break;
        case IDSetPattern::OneInEachChunk:
            model.insert(index * (membersPerSetChunk + 1));
            break;
        case IDSetPattern::Random:
            while (!model.insert(random.getUint32(4 * size + 64)).second) { }
            break;
        case IDSetPattern::AtTheTop:
            model.insert(largestIDPosition - 3 * index);
            break;
        }
    }
    return model;
}

bool toggleInIDSetModel(IDSetModel& model, uint32_t member)
{
    if (model.erase(member))
        return true;
    model.insert(member);
    return false;
}

IDSetMembers membersOfIDSetModel(const IDSetModel& model)
{
    return IDSetMembers(model.begin(), model.end());
}

IDSetMembers combineIDSetMembers(IDSetOperation operation, const IDSetMembers& first, const IDSetMembers& second)
{
    IDSetMembers combined;
    switch (operation) {
    case IDSetOperation::Union:
        std::ranges::set_union(first, second, std::back_inserter(combined));
        break;
    case IDSetOperation::Intersection:
        std::ranges::set_intersection(first, second, std::back_inserter(combined));
        break;
    case IDSetOperation::Difference:
        std::ranges::set_difference(first, second, std::back_inserter(combined));
        break;
    }
    return combined;
}

IDSetMembers membersOfIDSetView(const IDSetView& view)
{
    IDSetMembers members;
    view.forEach([&](uint32_t member) {
        members.push_back(member);
    });
    return members;
}

IDSetMembers membersAddedToIDSet(const IDSetView& smaller, const IDSetView& larger)
{
    IDSetMembers members;
    forEachAddedMember(larger, smaller, [&](uint32_t member) {
        members.push_back(member);
    });
    return members;
}

struct IDSetWithMembers {
    uint32_t set { idOfEmptyIDSet };
    IDSetMembers members;
};

class IDSetArenaUnderTest final : private IDSetAnnotator {
public:
    static constexpr uint32_t numberOfAttachedWords = 2;

    IDSetArenaUnderTest(IDSetTable& table, ArenaKeeps keeps)
        : arena(table, keeps, table.numberOfAttachedWords() ? this : nullptr)
        , m_numberOfAttachedWords(table.numberOfAttachedWords())
    {
    }

    IDSetWithMembers make(const IDSetModel& model)
    {
        IDSetMembers members = membersOfIDSetModel(model);
        return { arena.makeFromSorted(members), WTF::move(members) };
    }

    std::vector<IDSetWithMembers> makePool(WeakRandom& random, uint32_t largestSize = largestTestedIDSetSize)
    {
        std::vector<IDSetWithMembers> pool;
        for (uint32_t size : testedIDSetSizes) {
            for (IDSetPattern pattern : testedIDSetPatterns) {
                if (size <= largestSize)
                    pool.push_back(make(modelOfIDSet(pattern, size, random)));
            }
        }
        return pool;
    }

    void checkHolds(uint32_t set, const IDSetMembers& expected)
    {
        IDSetView view = arena.viewOf(set);
        AOT_TEST_CHECK(view.size() == expected.size() && view.isEmpty() == expected.empty());
        AOT_TEST_CHECK(membersOfIDSetView(view) == expected);
        AOT_TEST_CHECK(arena.makeFromSorted(expected) == set);
        if (expected.size() < 2)
            return;
        AttachedWords words = wordsOfMembers(expected);
        AOT_TEST_CHECK(std::ranges::equal(view.attachedWords(), std::span(words).first(m_numberOfAttachedWords)));
    }

    void checkOperations(const IDSetWithMembers& first, const IDSetWithMembers& second)
    {
        IDSetMembers inBoth = combineIDSetMembers(IDSetOperation::Intersection, first.members, second.members);
        IDSetMembers inFirstAlone = combineIDSetMembers(IDSetOperation::Difference, first.members, second.members);
        uint32_t united = arena.unionOf(first.set, second.set);
        uint32_t intersection = arena.intersectionOf(first.set, second.set);
        uint32_t difference = arena.differenceOf(first.set, second.set);
        checkHolds(united, combineIDSetMembers(IDSetOperation::Union, first.members, second.members));
        checkHolds(intersection, inBoth);
        checkHolds(difference, inFirstAlone);

        IDSetView firstView = arena.viewOf(first.set);
        IDSetView secondView = arena.viewOf(second.set);
        AOT_TEST_CHECK(isSubset(firstView, secondView) == inFirstAlone.empty() && overlap(firstView, secondView) == !inBoth.empty());
        AOT_TEST_CHECK(membersAddedToIDSet(secondView, firstView) == inFirstAlone);
        AOT_TEST_CHECK(membersAddedToIDSet(secondView, arena.viewOf(united)) == inFirstAlone);

        for (unsigned repetition = 0; repetition < 2; ++repetition) {
            AOT_TEST_CHECK(arena.isSubset(first.set, second.set) == inFirstAlone.empty() && arena.overlap(first.set, second.set) == !inBoth.empty());
            AOT_TEST_CHECK(arena.unionOf(second.set, first.set) == united && arena.intersectionOf(second.set, first.set) == intersection);
            AOT_TEST_CHECK(arena.differenceOf(first.set, second.set) == difference);
        }
    }

    uint32_t checkUnionOfMany(const std::vector<const IDSetWithMembers*>& operands, WeakRandom& random)
    {
        std::vector<uint32_t> sets;
        IDSetMembers expected;
        for (const IDSetWithMembers* operand : operands) {
            sets.push_back(operand->set);
            expected = combineIDSetMembers(IDSetOperation::Union, expected, operand->members);
        }
        uint32_t united = arena.unionOf(sets);
        checkHolds(united, expected);

        sets.push_back(idOfEmptyIDSet);
        sets.push_back(sets.front());
        for (size_t index = sets.size(); index > 1; --index)
            std::swap(sets[index - 1], sets[random.getUint32(static_cast<unsigned>(index))]);
        AOT_TEST_CHECK(arena.unionOf(sets) == united);

        uint32_t unitedOneByOne = idOfEmptyIDSet;
        for (uint32_t set : sets)
            unitedOneByOne = arena.unionOf(unitedOneByOne, set);
        AOT_TEST_CHECK(unitedOneByOne == united);
        return united;
    }

    IDSetArena arena;
    unsigned numberOfUnionsAnnotated { 0 };
    uint64_t numberOfMembersAnnotated { 0 };

private:
    using AttachedWords = std::array<uint32_t, numberOfAttachedWords>;

    static AttachedWords wordsOfMember(uint32_t member) { return { 1u << member % 32, member }; }
    static AttachedWords uniteWords(AttachedWords first, AttachedWords second) { return { first[0] | second[0], std::max(first[1], second[1]) }; }

    static AttachedWords wordsOfMembers(const IDSetMembers& members)
    {
        AttachedWords words { };
        for (uint32_t member : members)
            words = uniteWords(words, wordsOfMember(member));
        return words;
    }

    AttachedWords attachedWordsOf(uint32_t set) const
    {
        IDSetView view = arena.viewOf(set);
        if (view.size() == 1)
            return wordsOfMember(view.first());
        AOT_TEST_CHECK(view.size() > 1 && view.attachedWords().size() == numberOfAttachedWords);
        return { view.attachedWords()[0], view.attachedWords()[1] };
    }

    void annotate(const IDSetView& members, std::optional<IDSetOperation> operation, std::span<const uint32_t> operands, std::span<uint32_t> attachedWords) final
    {
        AOT_TEST_CHECK(members.size() > 1 && attachedWords.size() == numberOfAttachedWords);
        AttachedWords words { };
        if (operation == IDSetOperation::Union) {
            ++numberOfUnionsAnnotated;
            for (uint32_t operand : operands)
                words = uniteWords(words, attachedWordsOf(operand));
        } else {
            members.forEach([&](uint32_t member) {
                ++numberOfMembersAnnotated;
                words = uniteWords(words, wordsOfMember(member));
            });
        }
        std::ranges::copy(words, attachedWords.begin());
    }

    const uint32_t m_numberOfAttachedWords;
};

std::vector<IDSetMembers> contentsOfIDSetsToCompare()
{
    WeakRandom random(9);
    std::set<IDSetMembers> contents;
    for (uint32_t size : testedIDSetSizes) {
        for (IDSetPattern pattern : testedIDSetPatterns)
            contents.insert(membersOfIDSetModel(modelOfIDSet(pattern, size, random)));
    }
    for (uint32_t length : { 0u, 1u, 15u, 16u, 17u, 64u, 255u, 256u, 600u }) {
        IDSetMembers beginning(length);
        std::iota(beginning.begin(), beginning.end(), 0u);
        contents.insert(beginning);
        for (uint32_t distance : { 0u, 1u, 63u, 64u, 255u, 256u, 100000u }) {
            IDSetMembers longer = beginning;
            longer.push_back(length + distance);
            contents.insert(longer);
            longer.push_back(length + distance + membersPerSetChunk);
            contents.insert(WTF::move(longer));
        }
    }
    return std::vector<IDSetMembers>(contents.begin(), contents.end());
}

std::vector<uint32_t> makeIDSetsInOrder(IDSetArena& arena, const std::vector<IDSetMembers>& contents, bool makesLastFirst, unsigned stride)
{
    std::vector<uint32_t> sets(contents.size(), idOfEmptyIDSet);
    for (size_t step = 0; step < contents.size(); step += stride) {
        size_t index = makesLastFirst ? contents.size() - 1 - step : step;
        sets[index] = arena.makeFromSorted(contents[index]);
    }
    return sets;
}

std::vector<unsigned> ranksOfIDSetsByContent(const IDSetArena& arena, const std::vector<uint32_t>& sets)
{
    std::vector<IDSetView> views;
    for (uint32_t set : sets)
        views.push_back(arena.viewOf(set));

    std::vector<unsigned> ranks(sets.size(), 0);
    for (size_t first = 0; first < sets.size(); ++first) {
        for (size_t second = 0; second < sets.size(); ++second) {
            std::strong_ordering order = compareByContent(views[first], views[second]);
            AOT_TEST_CHECK((order == std::strong_ordering::equal) == (first == second));
            AOT_TEST_CHECK((order == std::strong_ordering::less) == (compareByContent(views[second], views[first]) == std::strong_ordering::greater));
            ranks[first] += order == std::strong_ordering::greater;
        }
    }
    std::vector<unsigned> ascendingRanks = ranks;
    std::ranges::sort(ascendingRanks);
    for (unsigned rank = 0; rank < ascendingRanks.size(); ++rank)
        AOT_TEST_CHECK(ascendingRanks[rank] == rank);
    return ranks;
}

} // anonymous namespace

template<IDSetForm form>
static void idSetsOfEverySizeHoldTheirMembers()
{
    IDSetTable table(form, 0);
    IDSetArenaUnderTest tested(table, ArenaKeeps::InProgram);
    WeakRandom random(1);
    for (const IDSetWithMembers& made : tested.makePool(random)) {
        const IDSetMembers& members = made.members;
        IDSetModel model(members.begin(), members.end());
        size_t size = members.size();
        tested.checkHolds(made.set, members);

        ObjectSetID id = ObjectSetID::fromBits(made.set);
        AOT_TEST_CHECK(!id == !size && holdsOneMember(id) == (size == 1) && liesInProgram(id) == (size > 1));
        AOT_TEST_CHECK(size != 1 || positionOf(id) == members[0]);

        IDSetMembers shuffled = members;
        shuffled.insert(shuffled.end(), members.begin(), members.begin() + size / 2);
        for (size_t index = shuffled.size(); index > 1; --index)
            std::swap(shuffled[index - 1], shuffled[random.getUint32(static_cast<unsigned>(index))]);
        AOT_TEST_CHECK(tested.arena.makeFromUnsorted(shuffled) == made.set);

        IDSetView view = table.viewOf(made.set);
        AOT_TEST_CHECK(view.isList() == (size <= IDSetTable::mostMembersOfList));
        AOT_TEST_CHECK(membersOfIDSetView(view) == members && (!view.isList() || std::ranges::equal(view.list(), members)));
        if constexpr (form == IDSetForm::ChunksWithLazyList)
            AOT_TEST_CHECK(std::ranges::equal(table.listOf(made.set), members) && table.listOf(made.set).data() == tested.arena.listOf(made.set).data());
        AOT_TEST_CHECK(!size || view.first() == members.front());
        for (uint32_t member : members) {
            AOT_TEST_CHECK(view.contains(member));
            for (uint32_t other : { member - 1, member + 1, member + membersPerSetChunk })
                AOT_TEST_CHECK(view.contains(other) == model.contains(other));
        }
        AOT_TEST_CHECK(view.allOf([&](uint32_t member) { return model.contains(member); }));
        AOT_TEST_CHECK(view.anyOf([&](uint32_t member) { return member == members.back(); }) == !!size);
        AOT_TEST_CHECK(view.allOf([&](uint32_t member) { return member != members.back(); }) == !size);
    }
}

template<IDSetForm form>
static void idSetOperationsAgreeWithModel()
{
    IDSetTable table(form, 0);
    IDSetArenaUnderTest tested(table, ArenaKeeps::InProgram);
    WeakRandom random(2);
    std::vector<IDSetWithMembers> pool = tested.makePool(random);
    for (const IDSetWithMembers& first : pool) {
        for (const IDSetWithMembers& second : pool)
            tested.checkOperations(first, second);
    }
}

template<IDSetForm form>
static void idSetsGrowAndShrinkByOneMember()
{
    IDSetTable table(form, 0);
    IDSetArenaUnderTest tested(table, ArenaKeeps::InProgram);
    IDSetArena& arena = tested.arena;
    WeakRandom random(3);
    IDSetModel model;
    uint32_t set = idOfEmptyIDSet;
    auto toggle = [&](uint32_t member) {
        bool wasMember = toggleInIDSetModel(model, member);
        AOT_TEST_CHECK((wasMember ? arena.with(set, member) : arena.without(set, member)) == set);
        set = wasMember ? arena.without(set, member) : arena.with(set, member);
        AOT_TEST_CHECK((wasMember ? arena.without(set, member) : arena.with(set, member)) == set);
        tested.checkHolds(set, membersOfIDSetModel(model));
    };

    constexpr uint32_t numberOfMembers = 300;
    for (uint32_t multiplier : { 37u, 71u }) {
        for (uint32_t step = 0; step < numberOfMembers; ++step)
            toggle(step * multiplier % numberOfMembers * 5);
    }
    AOT_TEST_CHECK(set == idOfEmptyIDSet);

    for (unsigned step = 0; step < 2000; ++step)
        toggle(random.getUint32(2 * IDSetTable::mostMembersOfList + 2) * 100);

    model = modelOfIDSet(IDSetPattern::Random, largestTestedIDSetSize, random);
    set = tested.make(model).set;
    for (unsigned step = 0; step < 16; ++step)
        toggle(random.getUint32(5 * largestTestedIDSetSize));
}

template<IDSetForm form>
static void idSetsAreFilteredAndReplaced()
{
    IDSetTable table(form, 0);
    IDSetArenaUnderTest tested(table, ArenaKeeps::InProgram);
    IDSetArena& arena = tested.arena;
    WeakRandom random(4);
    for (const IDSetWithMembers& made : tested.makePool(random)) {
        const IDSetMembers& members = made.members;
        auto checkFiltered = [&](const auto& keeps) {
            IDSetMembers kept;
            std::ranges::copy_if(members, std::back_inserter(kept), keeps);
            tested.checkHolds(arena.filtered(made.set, keeps), kept);
        };
        auto checkReplaced = [&](const auto& replacement) {
            IDSetModel expected;
            for (uint32_t member : members)
                expected.insert(replacement(member));
            tested.checkHolds(arena.replaced(made.set, replacement), membersOfIDSetModel(expected));
        };
        auto smallestMemberOfSetMadeInCallback = [&](uint32_t member) {
            return arena.viewOf(arena.makeFromUnsorted(std::array<uint32_t, 3> { member, member / 2, member / 3 })).first();
        };

        checkFiltered([](uint32_t) { return true; });
        checkFiltered([](uint32_t) { return false; });
        checkFiltered([](uint32_t member) { return !!(member % 2); });
        checkFiltered([](uint32_t member) { return !!(member % (membersPerSetChunk + 1)); });
        checkFiltered([&](uint32_t member) { return member == members.back(); });
        checkFiltered([&](uint32_t member) { return member != members[members.size() / 2]; });

        checkReplaced([](uint32_t member) { return member; });
        checkReplaced([](uint32_t member) { return member / 2; });
        checkReplaced([](uint32_t member) { return member ^ 1; });
        checkReplaced([](uint32_t member) { return member % 20; });
        checkReplaced([](uint32_t) { return 7u; });
        checkReplaced([](uint32_t member) { return largestIDPosition - member; });

        if (members.size() > largestSmallIDSetSize)
            continue;
        checkFiltered([&](uint32_t member) { return !!(smallestMemberOfSetMadeInCallback(member) % 3); });
        checkReplaced(smallestMemberOfSetMadeInCallback);
    }
}

template<IDSetForm form>
static void idSetsOfThreadLastUntilReset()
{
    constexpr uint32_t memberOutsideTable = 1000000;
    IDSetTable table(form, 0);
    WeakRandom random(6);
    std::vector<IDSetWithMembers> kept;
    {
        IDSetArenaUnderTest tested(table, ArenaKeeps::InProgram);
        kept = tested.makePool(random, largestSmallIDSetSize);
        tested.arena.reset();
        for (const IDSetWithMembers& inTable : kept)
            tested.checkHolds(inTable.set, inTable.members);
    }
    table.freeze();

    IDSetArenaUnderTest tested(table, ArenaKeeps::UntilReset);
    for (uint32_t round = 0; round < 5; ++round) {
        std::vector<IDSetWithMembers> made;
        std::vector<IDSetView> views;
        IDSetModel idsMade;
        for (uint32_t index = 200 + round; index--;) {
            IDSetModel model = modelOfIDSet(testedIDSetPatterns[index % testedIDSetPatterns.size()], 1 + index, random);
            model.insert(memberOutsideTable);
            made.push_back(tested.make(model));
            views.push_back(tested.arena.viewOf(made.back().set));
            AOT_TEST_CHECK(liesInThread(ObjectSetID::fromBits(made.back().set)) && idsMade.insert(made.back().set).second);
        }
        for (size_t index = 0; index < 8; ++index) {
            uint32_t madeAgain = idOfEmptyIDSet;
            views[index].forEach([&](uint32_t member) {
                madeAgain = tested.arena.with(madeAgain, member);
            });
            AOT_TEST_CHECK(madeAgain == made[index].set);
        }
        for (size_t index = 0; index < made.size(); ++index) {
            const IDSetWithMembers& inTable = kept[index % kept.size()];
            tested.checkOperations(made[index], made[(index + 1) % made.size()]);
            tested.checkOperations(made[index], inTable);
            tested.checkOperations(inTable, made[index]);
        }
        for (const IDSetWithMembers& first : kept) {
            for (const IDSetWithMembers& second : kept)
                tested.checkOperations(first, second);
        }
        for (size_t index = 0; index < made.size(); ++index)
            AOT_TEST_CHECK(membersOfIDSetView(views[index]) == made[index].members);

        tested.arena.reset();
        for (const IDSetWithMembers& inTable : kept)
            tested.checkHolds(inTable.set, inTable.members);
    }
}

template<IDSetForm form>
static void attachedWordsOfIDSetsFollowTheirMembers()
{
    IDSetTable table(form, IDSetArenaUnderTest::numberOfAttachedWords);
    WeakRandom random(7);
    for (ArenaKeeps keeps : { ArenaKeeps::InProgram, ArenaKeeps::UntilReset }) {
        IDSetArenaUnderTest tested(table, keeps);
        std::vector<IDSetWithMembers> pool = tested.makePool(random, largestSmallIDSetSize);
        IDSetWithMembers all;
        for (const IDSetWithMembers& first : pool) {
            for (const IDSetWithMembers& second : pool)
                tested.checkOperations(first, second);
            all = { tested.arena.unionOf(all.set, first.set), combineIDSetMembers(IDSetOperation::Union, all.members, first.members) };
            tested.checkHolds(all.set, all.members);
            uint32_t member = random.getUint32(largestTestedIDSetSize);
            all = { tested.arena.with(all.set, member), combineIDSetMembers(IDSetOperation::Union, all.members, { member }) };
            tested.checkHolds(all.set, all.members);
        }
        AOT_TEST_CHECK(tested.numberOfUnionsAnnotated);
        table.freeze();
    }
}

template<IDSetForm form>
static void eightThreadsGetTheSameIDSets()
{
    constexpr unsigned numberOfThreads = 8;
    WeakRandom random(8);
    std::vector<IDSetMembers> contents;
    for (uint32_t index = 0; index < 1200; ++index)
        contents.push_back(membersOfIDSetModel(modelOfIDSet(IDSetPattern::Random, 2 + index % 300, random)));

    for (uint32_t numberOfAttachedWords : { 0u, IDSetArenaUnderTest::numberOfAttachedWords }) {
        IDSetTable table(form, numberOfAttachedWords);
        std::array<std::vector<uint32_t>, numberOfThreads> setsOfThread;
        std::atomic<unsigned> numberOfThreadsAtStart { 0 };
        Vector<Ref<Thread>> threads;
        for (unsigned thread = 0; thread < numberOfThreads; ++thread) {
            threads.append(Thread::create("AOT id set test"_s, [&, thread] {
                IDSetArenaUnderTest tested(table, ArenaKeeps::InProgram);
                ++numberOfThreadsAtStart;
                while (numberOfThreadsAtStart.load() < numberOfThreads)
                    Thread::yield();
                std::vector<uint32_t>& sets = setsOfThread[thread];
                for (const IDSetMembers& members : contents)
                    sets.push_back(tested.arena.makeFromSorted(members));
                for (size_t index = 1; index < contents.size(); ++index)
                    sets.push_back(tested.arena.unionOf(sets[index - 1], sets[index]));
            }));
        }
        for (Ref<Thread>& thread : threads)
            thread->waitForCompletion();

        const std::vector<uint32_t>& sets = setsOfThread[0];
        for (const std::vector<uint32_t>& setsOfOther : setsOfThread)
            AOT_TEST_CHECK(setsOfOther == sets);
        IDSetArenaUnderTest tested(table, ArenaKeeps::InProgram);
        for (size_t index = 0; index < contents.size(); ++index) {
            tested.checkHolds(sets[index], contents[index]);
            if (index)
                tested.checkHolds(sets[contents.size() + index - 1], combineIDSetMembers(IDSetOperation::Union, contents[index - 1], contents[index]));
        }
    }
}

static void idSetsCompareByContent()
{
    std::vector<IDSetMembers> contents = contentsOfIDSetsToCompare();
    std::vector<unsigned> ranks;
    for (IDSetForm form : { IDSetForm::ChunksWithLazyList, IDSetForm::ListsAndChunks }) {
        for (bool makesLastFirst : { false, true }) {
            for (unsigned strideInTable : { 1u, 2u }) {
                IDSetTable table(form, 0);
                {
                    IDSetArena arena(table, ArenaKeeps::InProgram);
                    makeIDSetsInOrder(arena, contents, makesLastFirst, strideInTable);
                }
                table.freeze();
                IDSetArena arena(table, ArenaKeeps::UntilReset);
                std::vector<unsigned> ranksHere = ranksOfIDSetsByContent(arena, makeIDSetsInOrder(arena, contents, !makesLastFirst, 1));
                if (ranks.empty())
                    ranks = ranksHere;
                AOT_TEST_CHECK(ranksHere == ranks);
            }
        }
    }
}

template<IDSetForm form>
static void unionsOfManyIDSetsAgreeWithModel()
{
    IDSetTable table(form, IDSetArenaUnderTest::numberOfAttachedWords);
    WeakRandom random(10);
    for (ArenaKeeps keeps : { ArenaKeeps::InProgram, ArenaKeeps::UntilReset }) {
        IDSetArenaUnderTest tested(table, keeps);
        for (auto [largestSize, numberOfUnions] : { std::pair { IDSetTable::mostMembersOfList, 40u }, std::pair { membersPerSetChunk + 1, 160u }, std::pair { largestTestedIDSetSize, 10u } }) {
            std::vector<IDSetWithMembers> pool = tested.makePool(random, largestSize);
            for (unsigned count = 0; count < numberOfUnions; ++count) {
                std::vector<const IDSetWithMembers*> operands;
                for (unsigned numberOfOperands = 3 + random.getUint32(10); operands.size() < numberOfOperands;)
                    operands.push_back(&pool[random.getUint32(static_cast<unsigned>(pool.size()))]);
                tested.checkUnionOfMany(operands, random);
            }
        }
        AOT_TEST_CHECK(tested.numberOfUnionsAnnotated);
        table.freeze();
    }
}

namespace {

struct IDSetsForAnnotatorThatComesBack {
    std::array<uint32_t, 3> lists;
    std::array<uint32_t, 2> setsWithChunks;
};

struct WayBackIntoIDSetArena {
    ASCIILiteral name;
    bool stopsTheBuild;
    void (*comeBack)(IDSetArena&, const IDSetsForAnnotatorThatComesBack&);
};

constexpr WayBackIntoIDSetArena waysBackIntoIDSetArena[] = {
    { "reads"_s, false, [](IDSetArena& arena, const IDSetsForAnnotatorThatComesBack& sets) {
        AOT_TEST_CHECK(arena.viewOf(sets.lists[0]).size() == 2 && !arena.isSubset(sets.lists[0], sets.lists[1]) && arena.overlap(sets.setsWithChunks[0], sets.setsWithChunks[1]));
    } },
    { "makes-a-list"_s, false, [](IDSetArena& arena, const IDSetsForAnnotatorThatComesBack&) {
        arena.makeFromSorted(std::array { 70u, 71u });
    } },
    { "makes-a-set-with-chunks"_s, true, [](IDSetArena& arena, const IDSetsForAnnotatorThatComesBack&) {
        std::array<uint32_t, IDSetTable::mostMembersOfList + 1> members;
        std::iota(members.begin(), members.end(), 70u);
        arena.makeFromSorted(members);
    } },
    { "sorts-members"_s, true, [](IDSetArena& arena, const IDSetsForAnnotatorThatComesBack&) {
        arena.makeFromUnsorted(std::array { 71u, 70u });
    } },
    { "unites-two-lists"_s, true, [](IDSetArena& arena, const IDSetsForAnnotatorThatComesBack& sets) {
        arena.unionOf(sets.lists[0], sets.lists[1]);
    } },
    { "unites-three-lists"_s, true, [](IDSetArena& arena, const IDSetsForAnnotatorThatComesBack& sets) {
        arena.unionOf(sets.lists);
    } },
    { "unites-sets-with-chunks"_s, true, [](IDSetArena& arena, const IDSetsForAnnotatorThatComesBack& sets) {
        arena.unionOf(sets.setsWithChunks[0], sets.setsWithChunks[1]);
    } },
    { "subtracts-sets-with-chunks"_s, true, [](IDSetArena& arena, const IDSetsForAnnotatorThatComesBack& sets) {
        arena.differenceOf(sets.setsWithChunks[0], sets.setsWithChunks[1]);
    } },
    { "filters"_s, true, [](IDSetArena& arena, const IDSetsForAnnotatorThatComesBack& sets) {
        arena.filtered(sets.lists[0], [](uint32_t) { return true; });
    } },
    { "replaces"_s, true, [](IDSetArena& arena, const IDSetsForAnnotatorThatComesBack& sets) {
        arena.replaced(sets.lists[0], [](uint32_t member) { return member; });
    } },
    { "resets"_s, true, [](IDSetArena& arena, const IDSetsForAnnotatorThatComesBack&) {
        arena.reset();
    } },
};

constexpr const char* wayBackIntoIDSetArenaVariable = "TESTAOT_WAY_BACK_INTO_ID_SET_ARENA";
constexpr ASCIILiteral idSetTestOfAnnotatorOnRequest = "id-sets/on-request/annotator-comes-back"_s;
constexpr ASCIILiteral lineAheadOfWayBackIntoIDSetArena = "the annotator comes back"_s;
constexpr ASCIILiteral lineBehindWayBackIntoIDSetArena = "the annotator has come back"_s;

class IDSetAnnotatorThatComesBack final : private IDSetAnnotator {
public:
    IDSetAnnotatorThatComesBack(IDSetTable& table, const WayBackIntoIDSetArena& way)
        : arena(table, ArenaKeeps::InProgram, this)
        , m_way(way)
    {
    }

    IDSetArena arena;
    IDSetsForAnnotatorThatComesBack sets { };
    bool comesBackNextTime { false };

private:
    void annotate(const IDSetView& members, std::optional<IDSetOperation>, std::span<const uint32_t>, std::span<uint32_t> attachedWords) final
    {
        attachedWords[0] = members.size();
        if (!std::exchange(comesBackNextTime, false))
            return;
        dataLogLn(lineAheadOfWayBackIntoIDSetArena);
        m_way.comeBack(arena, sets);
        dataLogLn(lineBehindWayBackIntoIDSetArena);
    }

    const WayBackIntoIDSetArena& m_way;
};

} // anonymous namespace

static void annotatorComesBackIntoIDSetArenaOnRequest()
{
    const char* requested = getenv(wayBackIntoIDSetArenaVariable);
    if (!requested)
        return;
    const WayBackIntoIDSetArena* way = std::ranges::find_if(waysBackIntoIDSetArena, [&](const WayBackIntoIDSetArena& candidate) {
        return !strcmp(candidate.name.characters(), requested);
    });
    AOT_TEST_CHECK(way != std::end(waysBackIntoIDSetArena));

    IDSetTable table(IDSetForm::ListsAndChunks, 1);
    IDSetAnnotatorThatComesBack annotator(table, *way);
    IDSetArena& arena = annotator.arena;
    for (uint32_t index = 0; index < annotator.sets.lists.size(); ++index)
        annotator.sets.lists[index] = arena.makeFromSorted(std::array { 10 + index, 11 + index });
    for (uint32_t index = 0; index < annotator.sets.setsWithChunks.size(); ++index) {
        std::array<uint32_t, 3 * IDSetTable::mostMembersOfList> members;
        std::iota(members.begin(), members.end(), 100 + index * 2 * IDSetTable::mostMembersOfList);
        annotator.sets.setsWithChunks[index] = arena.makeFromSorted(members);
    }

    uint32_t first = arena.makeFromSorted(std::array { 1u, 2u, 3u });
    uint32_t second = arena.makeFromSorted(std::array { 3u, 4u });
    annotator.comesBackNextTime = true;
    IDSetView united = arena.viewOf(arena.unionOf(first, second));
    IDSetMembers expected { 1, 2, 3, 4 };
    AOT_TEST_CHECK(!annotator.comesBackNextTime && membersOfIDSetView(united) == expected && united.attachedWords()[0] == expected.size());
}

static void unionsOfIDSetsWithEarlierStatesAgreeWithModel()
{
    constexpr uint32_t endOfMembers = 40 * membersPerSetChunk;
    constexpr unsigned numberOfStates = 12;
    IDSetTable table(IDSetForm::ListsAndChunks, IDSetArenaUnderTest::numberOfAttachedWords);
    WeakRandom random(12);
    for (ArenaKeeps keeps : { ArenaKeeps::InProgram, ArenaKeeps::UntilReset }) {
        IDSetArenaUnderTest tested(table, keeps);
        IDSetArena& arena = tested.arena;

        std::vector<IDSetWithMembers> states;
        IDSetModel source;
        for (unsigned state = 0; state < numberOfStates; ++state) {
            for (unsigned count = 0; count < 3 + 40 * state; ++count)
                source.insert((random.getUint32(3) * 13 + state) % 40 * membersPerSetChunk + random.getUint32(membersPerSetChunk));
            states.push_back(tested.make(source));
        }
        AOT_TEST_CHECK(arena.viewOf(states.front().set).isList() && !arena.viewOf(states.back().set).isList());

        auto unitedMembers = [](const IDSetMembers& first, const IDSetMembers& second) {
            return combineIDSetMembers(IDSetOperation::Union, first, second);
        };
        IDSetModel ownMembersOfTarget;
        for (unsigned count = 0; count < 3000; ++count)
            ownMembersOfTarget.insert(random.getUint32(endOfMembers));
        IDSetWithMembers own = tested.make(ownMembersOfTarget);

        for (const IDSetWithMembers* start : { &own, &states[0] }) {
            IDSetWithMembers target { arena.unionOf(start->set, states[0].set), unitedMembers(start->members, states[0].members) };
            for (unsigned state = 1; state < numberOfStates; ++state) {
                target.members = unitedMembers(target.members, states[state].members);
                target.set = arena.unionOf(std::array { target.set, states[state].set }, std::array { idOfEmptyIDSet, states[state - 1].set });
                tested.checkHolds(target.set, target.members);
            }
            arena.forgetEarlierStates();
        }

        for (unsigned first = 0; first + 3 < numberOfStates; ++first) {
            uint32_t target = arena.unionOf(own.set, states[first].set);
            uint32_t united = arena.unionOf(std::array { states[first + 3].set, target, states[first + 1].set, states[first + 2].set }, std::array { states[first + 2].set, idOfEmptyIDSet, states[first].set, states[first + 1].set });
            tested.checkHolds(united, unitedMembers(own.members, states[first + 3].members));
        }
        arena.forgetEarlierStates();

        for (unsigned state = 1; state + 1 < numberOfStates; ++state) {
            uint32_t united = arena.unionOf(std::array { own.set, states[state + 1].set, states[state].set }, std::array { idOfEmptyIDSet, states[state].set, idOfEmptyIDSet });
            tested.checkHolds(united, unitedMembers(own.members, states[state + 1].members));
            united = arena.unionOf(std::array { states[state].set, own.set, states[state].set }, std::array { states[state - 1].set, idOfEmptyIDSet, idOfEmptyIDSet });
            tested.checkHolds(united, unitedMembers(own.members, states[state].members));
        }
        arena.forgetEarlierStates();
        table.freeze();
    }
}

static void unionOfIDSetsWithEarlierStateRemembersOnlyWhatLiesInside()
{
    auto modelWithChunks = [](uint32_t firstChunk, uint32_t endOfChunks) {
        IDSetModel model;
        for (uint32_t chunk = firstChunk; chunk < endOfChunks; ++chunk) {
            for (uint32_t index = 0; index < 40; ++index)
                model.insert(chunk * membersPerSetChunk + 3 * index);
        }
        return model;
    };

    for (ArenaKeeps keeps : { ArenaKeeps::InProgram, ArenaKeeps::UntilReset }) {
        IDSetTable table(IDSetForm::ListsAndChunks, 0);
        if (keeps == ArenaKeeps::UntilReset)
            table.freeze();
        IDSetArenaUnderTest tested(table, keeps);
        IDSetArena& arena = tested.arena;

        IDSetWithMembers largest = tested.make(modelWithChunks(1, 9));
        IDSetWithMembers earlierState = tested.make(modelWithChunks(0, 1));
        IDSetWithMembers grown = tested.make(modelWithChunks(0, 2));
        IDSetWithMembers all = tested.make(modelWithChunks(0, 9));
        for (unsigned repetition = 0; repetition < 2; ++repetition) {
            AOT_TEST_CHECK(arena.unionOf(std::array { earlierState.set, largest.set, grown.set }, std::array { idOfEmptyIDSet, idOfEmptyIDSet, earlierState.set }) == all.set);
            AOT_TEST_CHECK(arena.unionOf(largest.set, grown.set) == all.set);
            AOT_TEST_CHECK(arena.unionOf(std::array { largest.set, grown.set, tested.make(modelWithChunks(3, 4)).set }) == all.set);
        }
    }
}

static void typedIDSetsAnswerLikeUntypedOnes()
{
    IDSetTable table(IDSetForm::ChunksWithLazyList, 0);
    table.reserveMembers(64);
    LayoutSets sets(table, ArenaKeeps::InProgram);
    LayoutSetID none;
    LayoutSetID five = LayoutSets::of(LayoutID(5u));
    LayoutSetID nine = LayoutSets::of(LayoutID(9u));
    LayoutSetID both = sets.with(five, LayoutID(9u));

    AOT_TEST_CHECK(both == sets.ofSorted(std::array { LayoutID(5u), LayoutID(9u) }) && both == sets.ofUnsorted(std::array { LayoutID(9u), LayoutID(5u), LayoutID(9u) }));
    AOT_TEST_CHECK(sets.with(none, LayoutID(5u)) == five && sets.without(both, LayoutID(5u)) == nine && !sets.without(nine, LayoutID(9u)) && !sets.without(none, LayoutID(9u)));
    AOT_TEST_CHECK(!sets.sizeOf(none) && sets.sizeOf(five) == 1 && sets.sizeOf(both) == 2);
    AOT_TEST_CHECK(!sets.contains(none, LayoutID(5u)) && sets.contains(five, LayoutID(5u)) && !sets.contains(five, LayoutID(9u)) && sets.contains(both, LayoutID(9u)) && !sets.contains(both, LayoutID(7u)));
    AOT_TEST_CHECK(!sets.onlyMemberOf(none) && sets.onlyMemberOf(nine) == LayoutID(9u) && !sets.onlyMemberOf(both));
    AOT_TEST_CHECK(!sets.unionOf(none, none) && sets.unionOf(none, both) == both && sets.unionOf(five, none) == five && sets.unionOf(nine, five) == both && sets.unionOf(both, five) == both);
    AOT_TEST_CHECK(sets.intersectionOf(both, both) == both && sets.intersectionOf(both, nine) == nine && !sets.intersectionOf(five, nine) && !sets.intersectionOf(none, both) && !sets.intersectionOf(both, none));
    AOT_TEST_CHECK(sets.differenceOf(both, five) == nine && sets.differenceOf(both, none) == both && !sets.differenceOf(both, both) && !sets.differenceOf(none, both) && !sets.differenceOf(five, both));
    AOT_TEST_CHECK(sets.isSubset(none, none) && sets.isSubset(none, five) && !sets.isSubset(five, none) && sets.isSubset(five, both) && !sets.isSubset(both, five) && sets.isSubset(both, both));
    AOT_TEST_CHECK(!sets.overlap(none, none) && !sets.overlap(both, none) && !sets.overlap(five, nine) && sets.overlap(nine, both) && sets.overlap(both, both));

    AOT_TEST_CHECK(sets.filtered(both, [](LayoutID layout) { return layout == LayoutID(9u); }) == nine);
    AOT_TEST_CHECK(sets.replaced(both, [](LayoutID layout) { return LayoutID(layout.index() + 4); }) == sets.with(nine, LayoutID(13u)));
    IDSetMembers added;
    sets.forEachAdded(both, five, [&](LayoutID layout) {
        added.push_back(layout.index());
    });
    AOT_TEST_CHECK(added == IDSetMembers { 9 });

    LayoutSets::View view = sets.at(both);
    AOT_TEST_CHECK(view.size() == 2 && view.first() == LayoutID(5u) && !view.onlyMember() && view.contains(LayoutID(9u)) && !view.contains(LayoutID()));
    AOT_TEST_CHECK(std::ranges::equal(view.list(), std::array { LayoutID(5u), LayoutID(9u) }));
    AOT_TEST_CHECK(sets.at(none).isEmpty() && sets.at(nine).onlyMember() == LayoutID(9u));
    LayoutSetID thousand = LayoutSets::of(LayoutID(1000u));
    LayoutSets::View viewOfNine = sets.at(nine);
    LayoutSets::View viewOfThousand = sets.at(thousand);
    table.reserveMembers(2000);
    AOT_TEST_CHECK(sets.at(nine).list().data() == viewOfNine.list().data() && sets.at(thousand).list().data() == viewOfThousand.list().data());
    AOT_TEST_CHECK(std::ranges::equal(viewOfThousand.list(), std::array { LayoutID(1000u) }));
    AOT_TEST_CHECK(sets.listOf(none).empty() && sets.listOf(nine).data() == viewOfNine.list().data() && std::ranges::equal(sets.listOf(both), view.list()));

    std::vector<LayoutID> wide;
    for (uint32_t index = 0; index < 3 * IDSetTable::mostMembersOfList; ++index)
        wide.push_back(LayoutID(index * 977));
    LayoutSetID wideInTable = sets.ofSorted(wide);
    table.freeze();
    LayoutSets setsOfThread(table, ArenaKeeps::UntilReset);
    LayoutSetID wideInThread = setsOfThread.with(wideInTable, LayoutID(5u));
    AOT_TEST_CHECK(!setsOfThread.at(wideInTable).isList() && !setsOfThread.at(wideInThread).isList() && liesInThread(wideInThread));
    AOT_TEST_CHECK(std::ranges::equal(setsOfThread.listOf(wideInTable), wide) && setsOfThread.listOf(wideInTable).data() == sets.listOf(wideInTable).data());
    wide.insert(wide.begin() + 1, LayoutID(5u));
    AOT_TEST_CHECK(std::ranges::equal(setsOfThread.listOf(wideInThread), wide) && std::ranges::equal(setsOfThread.listOf(wideInThread), wide));
}

namespace {

} // anonymous namespace

static void unionsOfTypesWithEarlierStatesAreTheUnions()
{
    TypeTable table(BuiltinTable::singleton());
    table.setObjects(makeUniqueWithoutFastMallocCheck<ObjectTable>(BuiltinTable::singleton(), std::span<const FunctionInOrderOfObjects>()));
    TypeArena told(table, ArenaKeeps::InProgram);
    TypeArena untold(table, ArenaKeeps::InProgram);

    auto typeOfStrings = [&](uint32_t end, uint32_t stride, uint16_t primitives) {
        std::vector<StringID> strings;
        for (uint32_t string = 0; string < end; string += stride)
            strings.push_back(StringID(string));
        TypeParts parts;
        parts.primitives = primitives;
        parts.stringLiterals = told.stringSets().ofSorted(strings);
        return told.intern(parts);
    };
    std::vector<TypeID> states;
    for (uint32_t state = 0; state < 8; ++state)
        states.push_back(typeOfStrings(70 + state * 3 * membersPerSetChunk, 7, state % 2 ? Primitive::Boolean : Primitive::Null));
    AOT_TEST_CHECK(told.at(told.at(states.front()).stringLiterals).isList() && !told.at(told.at(states.back()).stringLiterals).isList());

    TypeID target = untold.unionOf(typeOfStrings(30 * membersPerSetChunk, 11, Primitive::Undefined), states[0]);
    for (size_t state = 1; state < states.size(); ++state) {
        TypeID united = told.unionOf(std::array { target, states[state] }, std::array { TypeID(), states[state - 1] });
        AOT_TEST_CHECK(united == untold.unionOf(target, states[state]));
        AOT_TEST_CHECK(united == told.unionOf(std::array { states[state], target, states[state] }, std::array { states[state - 1], TypeID(), TypeID() }));
        target = united;
    }
    told.forgetEarlierStates();

    TypeID start = untold.unionOf(typeOfStrings(30 * membersPerSetChunk, 13, Primitive::Undefined), states[1]);
    TypeID severalAtOnce = told.unionOf(std::array { states[4], states[2], start, states[3] }, std::array { states[3], states[1], TypeID(), states[2] });
    AOT_TEST_CHECK(severalAtOnce == untold.unionOf(start, states[4]));

    TypeID withAnyString = told.unionOf(std::array { start, TypeID(TypeTable::string), states[2] }, std::array { TypeID(), TypeID(), states[1] });
    AOT_TEST_CHECK(withAnyString == untold.unionOf(untold.unionOf(start, TypeTable::string), states[2]) && !told.at(withAnyString).stringLiterals);
    told.forgetEarlierStates();
}

static void rememberedAnswersDoNotShareNumbers()
{
    TypeTable table(BuiltinTable::singleton());
    table.setObjects(makeUniqueWithoutFastMallocCheck<ObjectTable>(BuiltinTable::singleton(), std::span<const FunctionInOrderOfObjects>()));
    table.freeze();
    TypeArena types(table, ArenaKeeps::UntilReset);

    constexpr auto records = static_cast<uint8_t>(AnswerKeptInArena::RecordsThatFileWrites);
    constexpr auto mention = static_cast<uint8_t>(AnswerKeptInArena::UnitMayMentionFunctions);
    const uint32_t word = TypeID(TypeTable::string).bits();
    TypeArena::SetOfRound round { word, false };
    types.rememberRound(records, 2, 0, round, 0, { 5, 2 });
    AOT_TEST_CHECK(!types.rememberedAnswer(mention, word, 0));
    types.rememberAnswer(mention, word, 0, 1);
    std::optional<TypeArena::WordsOfRound> kept = types.rememberedRound(records, 2, round, 0);
    AOT_TEST_CHECK(kept && (*kept)[0] == 5 && (*kept)[1] == 2);
    AOT_TEST_CHECK(types.rememberedAnswer(mention, word, 0) == 1u);
}

static void freezingTypeTableFreesOutgrownSlots()
{
    constexpr uint32_t numberMade = 64 * InternedRecords<Type>::numberOfShards;
    auto isSmallerAndFindsAllOnceFrozen = [](const auto& make) {
        TypeTable table(BuiltinTable::singleton());
        table.setObjects(makeUniqueWithoutFastMallocCheck<ObjectTable>(BuiltinTable::singleton(), std::span<const FunctionInOrderOfObjects>()));
        std::vector<uint32_t> made;
        {
            TypeArena types(table, ArenaKeeps::InProgram);
            for (uint32_t number = 0; number < numberMade; ++number)
                made.push_back(make(types, number).bits());
        }
        size_t sizeWhileItGrew = table.sizeInBytes();
        table.freeze();
        size_t sizeOfWhatFreezingMakes = table.sharedAnswers() ? table.sharedAnswers()->sizeInBytes() : 0;
        if (table.sizeInBytes() - sizeOfWhatFreezingMakes >= sizeWhileItGrew)
            return false;

        TypeArena types(table, ArenaKeeps::UntilReset);
        for (uint32_t number = 0; number < numberMade; ++number) {
            if (make(types, number).bits() != made[number])
                return false;
        }
        return true;
    };

    AOT_TEST_CHECK(isSmallerAndFindsAllOnceFrozen([](TypeArena& types, uint32_t number) {
        TypeID type = types.integer({ 0, 100000 + static_cast<int64_t>(number) });
        AOT_TEST_CHECK(liesInProgram(type));
        return type;
    }));
    AOT_TEST_CHECK(isSmallerAndFindsAllOnceFrozen([](TypeArena& types, uint32_t number) {
        return types.textKey(StringID(number));
    }));
    AOT_TEST_CHECK(isSmallerAndFindsAllOnceFrozen([](TypeArena& types, uint32_t number) {
        ClassPart part;
        part.builtinClass = BuiltinClassID(0u);
        part.writtenContents[0] = types.integer(Range::one(number));
        AOT_TEST_CHECK(isItsOwnValue(part.writtenContents[0]));
        ClassPartsID parts = types.internClassParts(singleElementSpan(part));
        AOT_TEST_CHECK(liesInProgram(parts));
        return parts;
    }));
}

static void idListMemoKeepsWhatIsAskedFor()
{
    std::vector<uint32_t> list;
    auto listOf = [&](uint32_t number) -> std::span<const uint32_t> {
        list.clear();
        for (uint32_t index = 0; index < 3 + number % 70; ++index)
            list.push_back(number * 131 + index * index);
        return list;
    };
    auto find = [&](IDListMemo& memo, uint32_t number) {
        std::optional<uint32_t> answer = memo.find(listOf(number));
        return answer ? answer : memo.find(listOf(number));
    };

    constexpr uint32_t numberAskedAgain = IDListMemo::leastIDsInGeneration / 128;
    IDListMemo memo;
    for (uint32_t number = 0; number < numberAskedAgain; ++number) {
        AOT_TEST_CHECK(!find(memo, number));
        memo.add(listOf(number), number + 1);
    }
    for (uint32_t number = 0; number < numberAskedAgain; ++number) {
        AOT_TEST_CHECK(find(memo, number) == number + 1);
        AOT_TEST_CHECK(!memo.find(listOf(number).first(2)));
    }

    uint32_t next = numberAskedAgain;
    for (unsigned round = 0; round < 40; ++round) {
        for (uint32_t number = 0; number < numberAskedAgain; ++number) {
            AOT_TEST_CHECK(find(memo, number) == number + 1);
            memo.add(listOf(next), next + 1);
            ++next;
        }
    }
    AOT_TEST_CHECK(!find(memo, numberAskedAgain));

    memo.clear();
    AOT_TEST_CHECK(!find(memo, 0) && !memo.sizeInBytes());

    IDListMemo memoThatKeepsEverything;
    memoThatKeepsEverything.keepEverything();
    for (uint32_t number = 0; number < 64 * numberAskedAgain; ++number)
        memoThatKeepsEverything.add(listOf(number), number + 1);
    for (uint32_t number = 0; number < 64 * numberAskedAgain; ++number)
        AOT_TEST_CHECK(find(memoThatKeepsEverything, number) == number + 1);
}

namespace {

class NumberKeptInSharedAnswers final : public SharedAnswers::Answer {
public:
    explicit NumberKeptInSharedAnswers(uint32_t number)
        : number(number)
    {
    }

    size_t sizeInBytes() const final { return sizeof(NumberKeptInSharedAnswers); }
    friend bool operator==(const NumberKeptInSharedAnswers& first, const NumberKeptInSharedAnswers& second) { return first.number == second.number; }

    const uint32_t number;
};

} // anonymous namespace

static IDSetMembers listForSharedAnswers(uint32_t number)
{
    IDSetMembers list;
    for (uint32_t index = 0; index < 1 + number % 90; ++index)
        list.push_back(number * 131 + index * index);
    return list;
}

static void eightThreadsGetOneNumberForEachSharedList()
{
    constexpr unsigned numberOfThreads = 8;
    constexpr uint32_t numberOfLists = 20000;
    constexpr uint32_t steps[numberOfThreads] = { 1, 3, 7, 11, 13, 17, 19, 19999 };

    SharedAnswers answers;
    std::array<std::vector<uint32_t>, numberOfThreads> numbersOfThread;
    std::array<bool, numberOfThreads> everyListWasReadBack { };
    std::atomic<unsigned> numberOfThreadsAtStart { 0 };
    Vector<Ref<Thread>> threads;
    for (unsigned thread = 0; thread < numberOfThreads; ++thread) {
        threads.append(Thread::create("AOT shared answers test"_s, [&, thread] {
            std::vector<uint32_t>& numbers = numbersOfThread[thread];
            numbers.resize(numberOfLists);
            bool wasReadBack = true;
            ++numberOfThreadsAtStart;
            while (numberOfThreadsAtStart.load() < numberOfThreads)
                Thread::yield();
            for (uint32_t turn = 0; turn < numberOfLists; ++turn) {
                uint32_t index = static_cast<uint32_t>((static_cast<uint64_t>(turn) * steps[thread] + thread * 2500) % numberOfLists);
                IDSetMembers list = listForSharedAnswers(index);
                numbers[index] = answers.internList(list);
                wasReadBack &= std::ranges::equal(answers.listAt(numbers[index]), list);
            }
            everyListWasReadBack[thread] = wasReadBack;
        }));
    }
    for (Ref<Thread>& thread : threads)
        thread->waitForCompletion();

    for (unsigned thread = 0; thread < numberOfThreads; ++thread) {
        AOT_TEST_CHECK(everyListWasReadBack[thread]);
        AOT_TEST_CHECK(numbersOfThread[thread] == numbersOfThread[0]);
    }
    std::set<uint32_t> distinct(numbersOfThread[0].begin(), numbersOfThread[0].end());
    AOT_TEST_CHECK(distinct.size() == numberOfLists);
}

static void sharedAnswersKeepFirstAnswerOfEightThreads()
{
    constexpr unsigned numberOfThreads = 8;
    constexpr uint32_t numberOfQuestions = 5000;
    auto questionAt = [](uint32_t index) {
        return SharedAnswers::Question { static_cast<SharedQuestion>(index % 3), index, index * 7, index % 5, index % 11, ~index };
    };

    SharedAnswers answers;
    AOT_TEST_CHECK(!answers.find(questionAt(0)));
    std::array<std::vector<const SharedAnswers::Answer*>, numberOfThreads> answersOfThread;
    std::atomic<unsigned> numberOfThreadsAtStart { 0 };
    Vector<Ref<Thread>> threads;
    for (unsigned thread = 0; thread < numberOfThreads; ++thread) {
        threads.append(Thread::create("AOT shared answers test"_s, [&, thread] {
            ++numberOfThreadsAtStart;
            while (numberOfThreadsAtStart.load() < numberOfThreads)
                Thread::yield();
            for (uint32_t index = 0; index < numberOfQuestions; ++index) {
                const SharedAnswers::Answer* answer = answers.find(questionAt(index));
                if (!answer)
                    answer = &answers.add(questionAt(index), makeUniqueWithoutFastMallocCheck<NumberKeptInSharedAnswers>(index));
                answersOfThread[thread].push_back(answer);
            }
        }));
    }
    for (Ref<Thread>& thread : threads)
        thread->waitForCompletion();

    for (const std::vector<const SharedAnswers::Answer*>& answersOfOther : answersOfThread)
        AOT_TEST_CHECK(answersOfOther == answersOfThread[0]);
    for (uint32_t index = 0; index < numberOfQuestions; ++index) {
        AOT_TEST_CHECK(answers.find(questionAt(index)) == answersOfThread[0][index]);
        AOT_TEST_CHECK(static_cast<const NumberKeptInSharedAnswers&>(*answersOfThread[0][index]).number == index);
    }
    SharedAnswers::Question question = questionAt(1);
    for (uint32_t SharedAnswers::Question::* word : { &SharedAnswers::Question::first, &SharedAnswers::Question::second, &SharedAnswers::Question::third, &SharedAnswers::Question::fourth, &SharedAnswers::Question::fifth }) {
        SharedAnswers::Question other = question;
        other.*word += 100000;
        AOT_TEST_CHECK(!answers.find(other));
    }
    SharedAnswers::Question ofAnotherAsker = question;
    ofAnotherAsker.kind = static_cast<SharedQuestion>(numberOfSharedQuestions - 1);
    AOT_TEST_CHECK(!answers.find(ofAnotherAsker));
}

namespace {

class ProgramForCutsOfIDSets {
    WTF_MAKE_NONCOPYABLE(ProgramForCutsOfIDSets);
public:
    static constexpr std::array<ObjectKind, 5> kinds { ObjectKind::Closure, ObjectKind::CoroutineResult, ObjectKind::RestArray, ObjectKind::FunctionPrototype, ObjectKind::ConstructedObject };
    static constexpr uint32_t numberOfFunctionsInSet = largestTestedIDSetSize / kinds.size();
    static constexpr uint32_t numberOfFunctionsOutsideSet = 20;
    static constexpr uint32_t numberOfFunctionsInShortSet = membersPerSetChunk / kinds.size();

    ProgramForCutsOfIDSets()
        : table(BuiltinTable::singleton())
    {
        std::vector<FunctionInOrderOfObjects> functions(numberOfFunctionsInSet + numberOfFunctionsOutsideSet);
        table.setObjects(makeUniqueWithoutFastMallocCheck<ObjectTable>(BuiltinTable::singleton(), std::span<const FunctionInOrderOfObjects>(functions)));
        {
            TypeArena types(table, ArenaKeeps::InProgram);
            all = objectsOfFunctions(types, 0, numberOfFunctionsInSet);
            shortSet = objectsOfFunctions(types, 0, numberOfFunctionsInShortSet);
        }
        table.freeze();
        AOT_TEST_CHECK(liesInProgram(all) && liesInProgram(shortSet));
    }

    ObjectID objectOf(ObjectKind kind, uint32_t function) const { return ObjectID(table.objects().firstOf(kind) + function); }

    ObjectSetID objectsOfFunctions(TypeArena& types, uint32_t begin, uint32_t end) const
    {
        std::vector<ObjectID> members;
        for (ObjectKind kind : kinds) {
            for (uint32_t function = begin; function < end; ++function)
                members.push_back(objectOf(kind, function));
        }
        return types.objectSets().ofSorted(members);
    }

    ObjectSetID allWithout(TypeArena& types, std::initializer_list<ObjectID> cutAway) const
    {
        ObjectSets& sets = types.objectSets();
        ObjectSetID cut = sets.differenceOf(all, sets.ofUnsorted(std::span<const ObjectID>(cutAway.begin(), cutAway.size())));
        AOT_TEST_CHECK(liesInThread(cut) && sets.sizeOf(cut) == largestTestedIDSetSize - cutAway.size() && sets.setOfProgramBehind(cut) == all);
        return cut;
    }

    TypeTable table;
    ObjectSetID all;
    ObjectSetID shortSet;
};

class ObjectsPickedOutOfCutsByTrait {
public:
    ObjectsPickedOutOfCutsByTrait(TypeArena& types, ObjectTrait trait)
        : m_types(types)
        , m_trait(trait)
    {
    }

    ObjectSetID askEachMemberOf(ObjectSetID set)
    {
        return m_types.objectSets().filtered(set, [&](ObjectID object) {
            ++m_numberOfMembersAsked;
            return m_types.at(object).has(m_trait);
        });
    }

    ObjectSetID pick(ObjectSetID set)
    {
        return filterMembersOncePerBuild(m_types, SharedQuestion::NamedObjectsOfUnionPartKind, set, { 1000, static_cast<uint32_t>(m_trait), 0, 0 }, scopedLambda<ObjectSetID(ObjectSetID)>([&](ObjectSetID asked) {
            return askEachMemberOf(asked);
        }));
    }

    void checkPicks(ObjectSetID set, uint64_t numberAsked)
    {
        m_numberOfMembersAsked = 0;
        ObjectSetID picked = pick(set);
        if constexpr (!SharedAnswers::worksFoundAnswersOutAgain)
            AOT_TEST_CHECK(m_numberOfMembersAsked == numberAsked);
        AOT_TEST_CHECK(picked == askEachMemberOf(set));
    }

private:
    TypeArena& m_types;
    const ObjectTrait m_trait;
    uint64_t m_numberOfMembersAsked { 0 };
};

} // anonymous namespace

static void cutOfProgramSetAnswersForWholeSet()
{
    ProgramForCutsOfIDSets program;
    TypeArena types(program.table, ArenaKeeps::UntilReset);
    ObjectsPickedOutOfCutsByTrait others(types, ObjectTrait::IsOtherObject);
    others.checkPicks(program.allWithout(types, { program.objectOf(ObjectKind::Closure, 1), program.objectOf(ObjectKind::RestArray, 1) }), largestTestedIDSetSize);
    others.checkPicks(program.all, 0);
    others.checkPicks(program.allWithout(types, { program.objectOf(ObjectKind::Closure, 2), program.objectOf(ObjectKind::CoroutineResult, 9000) }), 0);

    ObjectsPickedOutOfCutsByTrait merged(types, ObjectTrait::IsMerged);
    ObjectSetID cut = program.allWithout(types, { program.objectOf(ObjectKind::RestArray, 3) });
    merged.checkPicks(cut, largestTestedIDSetSize);
    merged.checkPicks(cut, 0);
    AOT_TEST_CHECK(!merged.pick(cut));
}

static void cutOfCutOfProgramSetAsksNoMember()
{
    ProgramForCutsOfIDSets program;
    TypeArena types(program.table, ArenaKeeps::UntilReset);
    ObjectSets& sets = types.objectSets();
    ObjectsPickedOutOfCutsByTrait closures(types, ObjectTrait::IsClosure);
    closures.checkPicks(program.all, largestTestedIDSetSize);

    ObjectSetID cut = program.allWithout(types, { program.objectOf(ObjectKind::Closure, 7), program.objectOf(ObjectKind::RestArray, 8), program.objectOf(ObjectKind::ConstructedObject, 9) });
    ObjectSetID cutAgain = sets.without(cut, program.objectOf(ObjectKind::Closure, 4000));
    AOT_TEST_CHECK(liesInThread(cutAgain) && sets.sizeOf(cutAgain) == largestTestedIDSetSize - 4 && sets.setOfProgramBehind(cutAgain) == program.all);
    closures.checkPicks(cutAgain, 0);

    ObjectSetID everySecond = sets.filtered(cutAgain, [](ObjectID object) {
        return !(object.index() % 2);
    });
    AOT_TEST_CHECK(liesInThread(everySecond) && sets.setOfProgramBehind(everySecond) == program.all);
    closures.checkPicks(everySecond, 0);

    ObjectSetID ofFirstFunctions = program.objectsOfFunctions(types, 0, ProgramForCutsOfIDSets::numberOfFunctionsInSet / 2);
    AOT_TEST_CHECK(liesInThread(ofFirstFunctions) && !sets.setOfProgramBehind(ofFirstFunctions));
    ObjectSetID kept = sets.intersectionOf(cutAgain, ofFirstFunctions);
    AOT_TEST_CHECK(kept != ofFirstFunctions && sets.setOfProgramBehind(kept) == program.all);
    closures.checkPicks(kept, 0);

    ObjectSetID picked = closures.pick(cutAgain);
    AOT_TEST_CHECK(liesInThread(picked) && sets.setOfProgramBehind(picked) == program.all);
    ObjectsPickedOutOfCutsByTrait callables(types, ObjectTrait::IsCallable);
    callables.checkPicks(program.all, largestTestedIDSetSize);
    callables.checkPicks(picked, 0);
    AOT_TEST_CHECK(callables.pick(picked) == picked);

    ObjectsPickedOutOfCutsByTrait withoutShape(types, ObjectTrait::HasNoShape);
    withoutShape.checkPicks(picked, sets.sizeOf(picked));
    withoutShape.checkPicks(program.all, largestTestedIDSetSize);
    withoutShape.checkPicks(picked, 0);
}

static void cutOfProgramSetAsksMembersFromElsewhere()
{
    ProgramForCutsOfIDSets program;
    TypeArena types(program.table, ArenaKeeps::UntilReset);
    ObjectSets& sets = types.objectSets();
    ObjectsPickedOutOfCutsByTrait closures(types, ObjectTrait::IsClosure);
    closures.checkPicks(program.all, largestTestedIDSetSize);

    constexpr uint32_t endOfFunctions = ProgramForCutsOfIDSets::numberOfFunctionsInSet + ProgramForCutsOfIDSets::numberOfFunctionsOutsideSet;
    ObjectSetID fromElsewhere = program.objectsOfFunctions(types, ProgramForCutsOfIDSets::numberOfFunctionsInSet, endOfFunctions);
    ObjectSetID joined = sets.unionOf(program.allWithout(types, { program.objectOf(ObjectKind::Closure, 7) }), fromElsewhere);
    AOT_TEST_CHECK(sets.sizeOf(joined) == largestTestedIDSetSize - 1 + sets.sizeOf(fromElsewhere) && sets.setOfProgramBehind(joined) == program.all);
    closures.checkPicks(joined, sets.sizeOf(fromElsewhere));
    AOT_TEST_CHECK(sets.sizeOf(closures.pick(joined)) == endOfFunctions - 1);
}

std::span<const NativeTest> idSetTests()
{
#define AOT_ID_SET_TEST_OF_BOTH_FORMS(name, function) \
    { "id-sets/" name "-lazy-lists"_s, function<IDSetForm::ChunksWithLazyList> }, \
    { "id-sets/" name "-chunks"_s, function<IDSetForm::ListsAndChunks> },
    static constexpr NativeTest tests[] = {
        { "id-sets/cut-of-a-program-set-answers-for-the-whole-set"_s, cutOfProgramSetAnswersForWholeSet },
        { "id-sets/cut-of-a-program-set-cut-again"_s, cutOfCutOfProgramSetAsksNoMember },
        { "id-sets/cut-of-a-program-set-with-members-from-elsewhere"_s, cutOfProgramSetAsksMembersFromElsewhere },
        AOT_ID_SET_TEST_OF_BOTH_FORMS("sizes", idSetsOfEverySizeHoldTheirMembers)
        AOT_ID_SET_TEST_OF_BOTH_FORMS("operations", idSetOperationsAgreeWithModel)
        AOT_ID_SET_TEST_OF_BOTH_FORMS("with-and-without", idSetsGrowAndShrinkByOneMember)
        AOT_ID_SET_TEST_OF_BOTH_FORMS("filtered-and-replaced", idSetsAreFilteredAndReplaced)
        AOT_ID_SET_TEST_OF_BOTH_FORMS("until-reset", idSetsOfThreadLastUntilReset)
        AOT_ID_SET_TEST_OF_BOTH_FORMS("attached-words", attachedWordsOfIDSetsFollowTheirMembers)
        AOT_ID_SET_TEST_OF_BOTH_FORMS("eight-threads", eightThreadsGetTheSameIDSets)
        AOT_ID_SET_TEST_OF_BOTH_FORMS("unions-of-many", unionsOfManyIDSetsAgreeWithModel)
        { idSetTestOfAnnotatorOnRequest, annotatorComesBackIntoIDSetArenaOnRequest },
        { "id-sets/unions-with-earlier-states"_s, unionsOfIDSetsWithEarlierStatesAgreeWithModel },
        { "id-sets/union-with-earlier-state-memo"_s, unionOfIDSetsWithEarlierStateRemembersOnlyWhatLiesInside },
        { "id-sets/unions-of-types-with-earlier-states"_s, unionsOfTypesWithEarlierStatesAreTheUnions },
        { "id-sets/compare-by-content"_s, idSetsCompareByContent },
        { "id-sets/typed-sets"_s, typedIDSetsAnswerLikeUntypedOnes },
        { "id-sets/remembered-answers-do-not-share-numbers"_s, rememberedAnswersDoNotShareNumbers },
        { "id-sets/freezing-type-table-frees-outgrown-slots"_s, freezingTypeTableFreesOutgrownSlots },
        { "id-sets/list-memo"_s, idListMemoKeepsWhatIsAskedFor },
        { "id-sets/shared-answers-eight-threads-number-lists"_s, eightThreadsGetOneNumberForEachSharedList },
        { "id-sets/shared-answers-keep-the-first"_s, sharedAnswersKeepFirstAnswerOfEightThreads },
    };
#undef AOT_ID_SET_TEST_OF_BOTH_FORMS
    return tests;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
