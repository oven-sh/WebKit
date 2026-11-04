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
// In ascending order, without repeats.
using IDSetMembers = std::vector<uint32_t>;

constexpr uint32_t idOfEmptyIDSet = ObjectSetID().bits();
constexpr uint32_t largestSmallIDSetSize = 257;
constexpr uint32_t largestTestedIDSetSize = 100000;
constexpr std::array<uint32_t, 11> testedIDSetSizes { 0, 1, 16, 17, 255, 256, largestSmallIDSetSize, membersPerSetChunk - 1, membersPerSetChunk, membersPerSetChunk + 1, largestTestedIDSetSize };
static_assert(IDSetTable::mostMembersOfList == 16 && membersPerSetChunk > largestSmallIDSetSize, "the sizes stand on both sides of each border");

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

// Answers whether it was a member.
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

// In the order in which the view lists them.
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

// Where the table has attached words, they are two that unite as sets do: which remainders by 32 the members have, and the largest member.
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

        // The second time the arena may answer from what it remembers.
        for (unsigned repetition = 0; repetition < 2; ++repetition) {
            AOT_TEST_CHECK(arena.isSubset(first.set, second.set) == inFirstAlone.empty() && arena.overlap(first.set, second.set) == !inBoth.empty());
            AOT_TEST_CHECK(arena.unionOf(second.set, first.set) == united && arena.intersectionOf(second.set, first.set) == intersection);
            AOT_TEST_CHECK(arena.differenceOf(first.set, second.set) == difference);
        }
    }

    // All at once, in another order with an empty set and a repeat among them, and one after the other: it is one set.
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

std::vector<std::pair<uint32_t, uint32_t>> chunksOfIDSetExcept(const IDSetView& view, uint32_t excludedChunkNumber)
{
    std::vector<std::pair<uint32_t, uint32_t>> chunks;
    for (const IDSetChunkEntry& entry : view.chunkEntries()) {
        if (entry.firstMemberDividedBySize != excludedChunkNumber)
            chunks.emplace_back(entry.firstMemberDividedBySize, entry.chunk);
    }
    return chunks;
}

std::vector<std::pair<uint32_t, uint32_t>> chunksOfIDSetThatOtherLacks(const IDSetView& view, const IDSetView& other)
{
    constexpr uint32_t numberOfNoChunk = UINT32_MAX;
    std::vector<std::pair<uint32_t, uint32_t>> chunks = chunksOfIDSetExcept(view, numberOfNoChunk);
    std::vector<std::pair<uint32_t, uint32_t>> chunksOfOther = chunksOfIDSetExcept(other, numberOfNoChunk);
    std::vector<std::pair<uint32_t, uint32_t>> lacking;
    std::ranges::set_difference(chunks, chunksOfOther, std::back_inserter(lacking));
    return lacking;
}

void checkIDSetsShareOtherChunks(IDSetArenaUnderTest& tested, IDSetModel& model, WeakRandom& random)
{
    IDSetArena& arena = tested.arena;
    uint32_t set = tested.make(model).set;
    AOT_TEST_CHECK(arena.viewOf(set).chunkEntries().size() >= model.size() / membersPerSetChunk);

    uint32_t last = *model.rbegin();
    IDSetMembers toggled { *model.begin(), last, last + 10 * membersPerSetChunk };
    for (unsigned count = 0; count < 6; ++count) {
        toggled.push_back(random.getUint32(last));
        toggled.push_back(*model.lower_bound(toggled.back()));
    }
    for (uint32_t member : toggled) {
        bool wasMember = toggleInIDSetModel(model, member);
        uint32_t changed = wasMember ? arena.without(set, member) : arena.with(set, member);
        tested.checkHolds(changed, membersOfIDSetModel(model));
        toggleInIDSetModel(model, member);
        uint32_t chunkNumber = member / membersPerSetChunk;
        AOT_TEST_CHECK(chunksOfIDSetExcept(arena.viewOf(changed), chunkNumber) == chunksOfIDSetExcept(arena.viewOf(set), chunkNumber));
    }
}

std::vector<IDSetMembers> contentsOfIDSetsToCompare()
{
    WeakRandom random(9);
    std::set<IDSetMembers> contents;
    for (uint32_t size : testedIDSetSizes) {
        for (IDSetPattern pattern : testedIDSetPatterns)
            contents.insert(membersOfIDSetModel(modelOfIDSet(pattern, size, random)));
    }
    // Sets that begin alike. One ends there, the others go on in the same word, the same chunk, the next chunk or far beyond.
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

// No two of the sets are equal. Answers how many of them come in front of each.
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
    // Of any two sets one comes first. Such a relation has no cycle exactly where no two sets have the same number in front of them.
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
        // Asked twice, it is one list.
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

    // Through every size up to 300 and back, in two orders: neither multiplier has a factor in common with 300.
    constexpr uint32_t numberOfMembers = 300;
    for (uint32_t multiplier : { 37u, 71u }) {
        for (uint32_t step = 0; step < numberOfMembers; ++step)
            toggle(step * multiplier % numberOfMembers * 5);
    }
    AOT_TEST_CHECK(set == idOfEmptyIDSet);

    // About half of 34 numbers are members, so the set keeps changing between the two forms.
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

static void idSetsThatDifferInOneMemberShareOtherChunks()
{
    IDSetTable table(IDSetForm::ListsAndChunks, 0);
    WeakRandom random(5);
    std::array<IDSetModel, 3> models {
        modelOfIDSet(IDSetPattern::Dense, largestTestedIDSetSize, random),
        modelOfIDSet(IDSetPattern::OneInEachChunk, largestTestedIDSetSize, random),
        modelOfIDSet(IDSetPattern::Random, largestTestedIDSetSize, random),
    };
    for (ArenaKeeps keeps : { ArenaKeeps::InProgram, ArenaKeeps::UntilReset }) {
        IDSetArenaUnderTest tested(table, keeps);
        for (IDSetModel& model : models)
            checkIDSetsShareOtherChunks(tested, model, random);
        table.freeze();
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

    // Each round makes one set more than the round before, last index first. So a position in the arena names another set each round, and a set that comes again has another position.
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
        // The arena grows while one of its own records is being walked.
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
            // With a stride of two, every other set lies in the thread.
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
        // Lists alone, lists beside sets with chunks, numbers of chunks that lie close together and far apart, numbers that the largest set lacks.
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

// What meets at a hub of the solver: many wide sets that hold nearly the same. Under one number a few chunks come again and again, and most of them bring nothing.
static void unionsOfIDSetsThatHoldNearlyTheSame()
{
    constexpr uint32_t endOfMembers = 12 * membersPerSetChunk;
    constexpr unsigned numberOfSets = 48;
    IDSetTable table(IDSetForm::ListsAndChunks, 0);
    WeakRandom random(11);
    for (ArenaKeeps keeps : { ArenaKeeps::InProgram, ArenaKeeps::UntilReset }) {
        IDSetArenaUnderTest tested(table, keeps);
        IDSetModel all;
        for (uint32_t member = 0; member < endOfMembers; ++member) {
            if (random.getUint32(2))
                all.insert(member);
        }

        // Each lacks up to five members, so it shares every other chunk with the rest.
        std::vector<IDSetWithMembers> sets;
        for (unsigned index = 0; index < numberOfSets; ++index) {
            IDSetModel model = all;
            for (unsigned count = 0; count <= index % 5; ++count)
                model.erase(*model.lower_bound(random.getUint32(endOfMembers - membersPerSetChunk)));
            sets.push_back(tested.make(model));
        }
        std::vector<const IDSetWithMembers*> operands;
        for (const IDSetWithMembers& set : sets)
            operands.push_back(&set);
        uint32_t united = tested.checkUnionOfMany(operands, random);

        // A set that holds all the others is the answer itself, also the second time, when the arena may remember that they lie inside it.
        IDSetWithMembers holdsAll { united, membersOfIDSetView(tested.arena.viewOf(united)) };
        operands.insert(operands.begin() + numberOfSets / 2, &holdsAll);
        for (unsigned repetition = 0; repetition < 2; ++repetition)
            AOT_TEST_CHECK(tested.checkUnionOfMany(operands, random) == united);

        // One member more, from a list and from a wide set, under a number that all have and under one that none has.
        uint32_t leastMemberThatAllLack = 0;
        while (all.contains(leastMemberThatAllLack))
            ++leastMemberThatAllLack;
        AOT_TEST_CHECK(leastMemberThatAllLack < membersPerSetChunk);
        for (uint32_t member : { leastMemberThatAllLack, endOfMembers + 5 * membersPerSetChunk }) {
            IDSetWithMembers one = tested.make(IDSetModel { member });
            IDSetModel modelOfWide(sets[0].members.begin(), sets[0].members.end());
            modelOfWide.insert(member);
            IDSetWithMembers wide = tested.make(modelOfWide);
            for (const IDSetWithMembers* added : { &one, &wide }) {
                operands.push_back(added);
                uint32_t withMember = tested.checkUnionOfMany(operands, random);
                AOT_TEST_CHECK(withMember == tested.arena.with(united, member));
                operands.pop_back();
            }
        }
        table.freeze();
    }
}

// Two sets bring the same chunk and nothing else, so the one that comes second brings nothing new. It still does not lie inside the largest set, and the arena must not remember that it does.
static void unionOfManyIDSetsRemembersOnlyWhatLiesInside()
{
    constexpr uint32_t memberThatLargestLacks = membersPerSetChunk + 1;
    auto modelWithChunks = [](uint32_t numberOfChunks) {
        IDSetModel model;
        for (uint32_t chunk = 0; chunk < numberOfChunks; ++chunk) {
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

        uint32_t largest = tested.make(modelWithChunks(4)).set;
        uint32_t inside = tested.make(modelWithChunks(3)).set;
        // Under the number that gains it has a chunk of its own, which adds nothing.
        IDSetModel modelWithOwnChunk = modelWithChunks(3);
        modelWithOwnChunk.erase(membersPerSetChunk + 3);
        uint32_t insideWithOwnChunk = tested.make(modelWithOwnChunk).set;
        std::array<uint32_t, 2> bringSameChunk;
        for (uint32_t index = 0; index < bringSameChunk.size(); ++index) {
            IDSetModel model = modelWithChunks(2 + index);
            model.insert(memberThatLargestLacks);
            bringSameChunk[index] = tested.make(model).set;
        }
        IDSetModel modelOfUnion = modelWithChunks(4);
        modelOfUnion.insert(memberThatLargestLacks);
        uint32_t expected = tested.make(modelOfUnion).set;

        // A build with assertions withholds one hit in sixteen, never two in a row.
        auto rememberedUnionWithLargest = [&](uint32_t set) {
            auto find = [&] {
                return arena.lastingMemo().find(static_cast<uint32_t>(IDSetOperation::Union), std::min(set, largest), std::max(set, largest));
            };
            std::optional<uint32_t> answer = find();
            return answer ? answer : find();
        };

        for (unsigned repetition = 0; repetition < 2; ++repetition) {
            AOT_TEST_CHECK(arena.unionOf(std::array { largest, inside, insideWithOwnChunk, bringSameChunk[0], bringSameChunk[1] }) == expected);
            // The unions of two below remember by themselves.
            if (keeps == ArenaKeeps::InProgram && !repetition) {
                AOT_TEST_CHECK(rememberedUnionWithLargest(inside) == largest && rememberedUnionWithLargest(insideWithOwnChunk) == largest);
                AOT_TEST_CHECK(rememberedUnionWithLargest(bringSameChunk[0]) != largest && rememberedUnionWithLargest(bringSameChunk[1]) != largest);
            }
            AOT_TEST_CHECK(arena.unionOf(largest, bringSameChunk[0]) == expected && arena.unionOf(largest, bringSameChunk[1]) == expected);
            AOT_TEST_CHECK(arena.unionOf(largest, inside) == largest && arena.unionOf(largest, insideWithOwnChunk) == largest);
            AOT_TEST_CHECK(arena.unionOf(std::array { largest, inside, insideWithOwnChunk }) == largest);
            AOT_TEST_CHECK(arena.unionOf(std::array { largest, inside, bringSameChunk[1] }) == expected && arena.unionOf(std::array { largest, inside, bringSameChunk[0] }) == expected);
        }
    }
}

// The same where ids follow the order of making. So the two sets stand next to each other among the operands, whichever way those are sorted, and the walk drops every chunk that the second of them brings.
// Whoever takes "it added no candidate" for "it lies inside the largest set" remembers that, and the next union with the largest set lacks members. Nothing else is remembered in front of that union.
static void unionOfIDSetsThatBringSameChunksHoldsTheirMembers()
{
    constexpr std::array<uint32_t, 2> membersThatLargestLacks { membersPerSetChunk + 1, 3 * membersPerSetChunk + 2 };
    auto modelWithChunks = [](uint32_t numberOfChunks) {
        IDSetModel model;
        for (uint32_t chunk = 0; chunk < numberOfChunks; ++chunk) {
            for (uint32_t index = 0; index < 40; ++index)
                model.insert(chunk * membersPerSetChunk + 3 * index);
        }
        return model;
    };

    IDSetTable table(IDSetForm::ListsAndChunks, 0);
    table.freeze();
    IDSetArenaUnderTest tested(table, ArenaKeeps::UntilReset);
    IDSetArena& arena = tested.arena;

    for (bool asksForUnionOfTwoFirst : { true, false }) {
        for (size_t askedFirst = 0; askedFirst < 2; ++askedFirst) {
            IDSetWithMembers largest = tested.make(modelWithChunks(6));
            std::array<IDSetWithMembers, 2> bringSameChunks;
            for (uint32_t index = 0; index < bringSameChunks.size(); ++index) {
                IDSetModel model = modelWithChunks(4 + index);
                model.insert(membersThatLargestLacks.begin(), membersThatLargestLacks.end());
                bringSameChunks[index] = tested.make(model);
            }
            uint32_t oneMemberOfLargest = tested.make(IDSetModel { 0 }).set;
            IDSetMembers expected = combineIDSetMembers(IDSetOperation::Union, largest.members, IDSetMembers(membersThatLargestLacks.begin(), membersThatLargestLacks.end()));

            IDSetView viewOfLargest = arena.viewOf(largest.set);
            std::vector<std::pair<uint32_t, uint32_t>> brought = chunksOfIDSetThatOtherLacks(arena.viewOf(bringSameChunks[0].set), viewOfLargest);
            AOT_TEST_CHECK(liesInThread(ObjectSetID::fromBits(largest.set)) && bringSameChunks[0].set == largest.set + 1 && bringSameChunks[1].set == largest.set + 2);
            AOT_TEST_CHECK(brought.size() == membersThatLargestLacks.size() && brought == chunksOfIDSetThatOtherLacks(arena.viewOf(bringSameChunks[1].set), viewOfLargest));
            for (const std::pair<uint32_t, uint32_t>& chunk : brought)
                AOT_TEST_CHECK(std::ranges::binary_search(viewOfLargest.chunkEntries(), chunk.first, { }, &IDSetChunkEntry::firstMemberDividedBySize));

            tested.checkHolds(arena.unionOf(std::array { largest.set, bringSameChunks[0].set, bringSameChunks[1].set }), expected);
            for (size_t index : { askedFirst, 1 - askedFirst }) {
                for (bool isUnionOfTwo : { asksForUnionOfTwoFirst, !asksForUnionOfTwoFirst }) {
                    // A list is not looked up, so beside one the set alone is asked about.
                    uint32_t united = isUnionOfTwo ? arena.unionOf(largest.set, bringSameChunks[index].set) : arena.unionOf(std::array { largest.set, oneMemberOfLargest, bringSameChunks[index].set });
                    tested.checkHolds(united, expected);
                }
            }
            arena.reset();
        }
    }
}

static void scratchOfIDSetsIsCountedWithMemosOfTypeArena()
{
    constexpr uint32_t numberOfChunks = 64;
    TypeTable table(BuiltinTable::singleton());
    table.setObjects(makeUniqueWithoutFastMallocCheck<ObjectTable>(BuiltinTable::singleton(), std::span<const FunctionInOrderOfObjects>()));
    TypeArena types(table, ArenaKeeps::InProgram);
    LayoutSets& sets = types.layoutSets();

    // No two share a chunk and none lies inside another. So the union lays out every chunk of two of them and a new chunk under every number, and it remembers nothing.
    std::array<LayoutSetID, 3> operands;
    for (uint32_t index = 0; index < operands.size(); ++index) {
        std::vector<LayoutID> members;
        for (uint32_t chunk = 0; chunk < numberOfChunks; ++chunk)
            members.push_back(LayoutID(chunk * membersPerSetChunk + index));
        operands[index] = sets.ofSorted(members);
    }
    size_t sizeOfMemos = types.sizeOfMemosInBytes();
    size_t sizeOfScratch = sets.untyped().sizeOfScratchInBytes();
    AOT_TEST_CHECK(sets.sizeOf(sets.unionOf(operands)) == operands.size() * numberOfChunks);
    size_t gained = sets.untyped().sizeOfScratchInBytes() - sizeOfScratch;
    AOT_TEST_CHECK(gained >= 2 * numberOfChunks * sizeof(IDSetChunkEntry) + numberOfChunks * sizeof(IDSetChunk));
    AOT_TEST_CHECK(types.sizeOfMemosInBytes() == sizeOfMemos + gained);
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

void setWayBackIntoIDSetArena(const char* name)
{
#if OS(WINDOWS)
    SetEnvironmentVariableA(wayBackIntoIDSetArenaVariable, name);
#else
    if (name)
        setenv(wayBackIntoIDSetArenaVariable, name, 1);
    else
        unsetenv(wayBackIntoIDSetArenaVariable);
#endif
}

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
    // What one has and the other lacks is too much for a list.
    for (uint32_t index = 0; index < annotator.sets.setsWithChunks.size(); ++index) {
        std::array<uint32_t, 3 * IDSetTable::mostMembersOfList> members;
        std::iota(members.begin(), members.end(), 100 + index * 2 * IDSetTable::mostMembersOfList);
        annotator.sets.setsWithChunks[index] = arena.makeFromSorted(members);
    }

    // The members of a union of two lists lie in scratch while they are annotated.
    uint32_t first = arena.makeFromSorted(std::array { 1u, 2u, 3u });
    uint32_t second = arena.makeFromSorted(std::array { 3u, 4u });
    annotator.comesBackNextTime = true;
    IDSetView united = arena.viewOf(arena.unionOf(first, second));
    IDSetMembers expected { 1, 2, 3, 4 };
    AOT_TEST_CHECK(!annotator.comesBackNextTime && membersOfIDSetView(united) == expected && united.attachedWords()[0] == expected.size());
}

static void layingOutInScratchWhileIDSetIsInternedStopsTheBuild()
{
    for (const WayBackIntoIDSetArena& way : waysBackIntoIDSetArena) {
        std::array<String, 3> arguments { String(nativeSubcommandName), String(singleProcessFlag), String(idSetTestOfAnnotatorOnRequest) };
        setWayBackIntoIDSetArena(way.name.characters());
        ChildProcessResult result = ChildProcessLauncher(String(programPath()), { }).run(arguments);
        setWayBackIntoIDSetArena(nullptr);
        AOT_TEST_CHECK(result.output.contains(lineAheadOfWayBackIntoIDSetArena));
        if (way.stopsTheBuild)
            AOT_TEST_CHECK(result.outcome == ChildProcessOutcome::Crashed && !result.output.contains(lineBehindWayBackIntoIDSetArena));
        else
            AOT_TEST_CHECK(result.exitedWith(0) && result.output.contains(lineBehindWayBackIntoIDSetArena));
    }
}

// What comes over an edge of the solver: a source that grows, handed over whole beside what it was when it last sent.
static void unionsOfIDSetsWithEarlierStatesAgreeWithModel()
{
    constexpr uint32_t endOfMembers = 40 * membersPerSetChunk;
    constexpr unsigned numberOfStates = 12;
    IDSetTable table(IDSetForm::ListsAndChunks, IDSetArenaUnderTest::numberOfAttachedWords);
    WeakRandom random(12);
    for (ArenaKeeps keeps : { ArenaKeeps::InProgram, ArenaKeeps::UntilReset }) {
        IDSetArenaUnderTest tested(table, keeps);
        IDSetArena& arena = tested.arena;

        // The first states are lists. Each later one gains members under a few numbers, so it shares the other chunks with the one in front of it.
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

        // One state after the other, at a target with members of its own and at one that holds what the source held.
        for (const IDSetWithMembers* start : { &own, &states[0] }) {
            IDSetWithMembers target { arena.unionOf(start->set, states[0].set), unitedMembers(start->members, states[0].members) };
            for (unsigned state = 1; state < numberOfStates; ++state) {
                target.members = unitedMembers(target.members, states[state].members);
                target.set = arena.unionOf(std::array { target.set, states[state].set }, std::array { idOfEmptyIDSet, states[state - 1].set });
                tested.checkHolds(target.set, target.members);
            }
            arena.forgetEarlierStates();
        }

        // Several at once. The target holds the first state. Each later one is held by the set that brings it.
        for (unsigned first = 0; first + 3 < numberOfStates; ++first) {
            uint32_t target = arena.unionOf(own.set, states[first].set);
            uint32_t united = arena.unionOf(std::array { states[first + 3].set, target, states[first + 1].set, states[first + 2].set }, std::array { states[first + 2].set, idOfEmptyIDSet, states[first].set, states[first + 1].set });
            tested.checkHolds(united, unitedMembers(own.members, states[first + 3].members));
        }
        arena.forgetEarlierStates();

        // The target holds nothing of the source. A set that comes without an earlier state brings it, also where the same set comes once more with one.
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

// A set with an earlier state brings nothing to the largest set in the chunks that are looked at. In those that are passed over it holds what the largest set lacks, and the arena must not remember that it lies inside.
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

// What was made in front of keepAllSetsMadeSoFar() and what is marked stays as it is, under its id. The rest is gone, and an equal set gets another id.
static void idSetsThatNothingNamesAreCollected()
{
    IDSetTable table(IDSetForm::ListsAndChunks, IDSetArenaUnderTest::numberOfAttachedWords);
    IDSetArenaUnderTest tested(table, ArenaKeeps::InProgram);
    IDSetArena& arena = tested.arena;
    WeakRandom random(13);
    auto collect = [&](const std::vector<IDSetWithMembers>& named) {
        table.beginCollection();
        table.markAsNamed(idOfEmptyIDSet);
        for (const IDSetWithMembers& set : named)
            table.markAsNamed(set.set);
        for (uint32_t shard = 0; shard < IDSetTable::numberOfShards; ++shard)
            table.collectShard(shard);
        arena.lastingMemo().clear();
    };
    auto checkAllHold = [&](const std::vector<IDSetWithMembers>& sets) {
        for (const IDSetWithMembers& set : sets)
            tested.checkHolds(set.set, set.members);
    };

    std::vector<IDSetWithMembers> madeBefore = tested.makePool(random);
    table.keepAllSetsMadeSoFar();
    IDSetTable::Contents contentsBefore = table.contents();
    collect({ });
    checkAllHold(madeBefore);
    AOT_TEST_CHECK(table.contents().numberOfSets == contentsBefore.numberOfSets);

    // Lists, sets whose entries lie in their records, and sets whose entries lie outside.
    auto makeSets = [&](uint32_t offset) {
        std::vector<IDSetWithMembers> sets;
        for (uint32_t numberOfChunks : { 1u, 2u, 15u, 16u, 17u, 300u }) {
            for (uint32_t membersInChunk : { 1u, 2u, 40u }) {
                IDSetModel model;
                for (uint32_t chunk = 0; chunk < numberOfChunks; ++chunk) {
                    for (uint32_t index = 0; index < membersInChunk; ++index)
                        model.insert(chunk * membersPerSetChunk + 5 * index + offset);
                }
                if (model.size() > 1)
                    sets.push_back(tested.make(model));
            }
        }
        return sets;
    };
    std::vector<IDSetWithMembers> named = makeSets(1);
    IDSetTable::Contents contentsWithNamed = table.contents();
    std::vector<IDSetWithMembers> notNamed = makeSets(2);
    AOT_TEST_CHECK(table.contents().numberOfSets == contentsWithNamed.numberOfSets + notNamed.size());

    collect(named);
    AOT_TEST_CHECK(table.contents().numberOfSets == contentsWithNamed.numberOfSets);
    checkAllHold(madeBefore);
    checkAllHold(named);

    std::vector<IDSetWithMembers> madeAgain = makeSets(2);
    AOT_TEST_CHECK(madeAgain.size() == notNamed.size());
    for (size_t index = 0; index < madeAgain.size(); ++index)
        AOT_TEST_CHECK(madeAgain[index].set != notNamed[index].set);
    checkAllHold(madeAgain);

    // What stays is united as before.
    for (size_t index = 0; index < named.size(); ++index)
        tested.checkOperations(named[index], madeAgain[index]);
    collect(madeAgain);
    checkAllHold(madeBefore);
    checkAllHold(madeAgain);
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
    // The list of one member lies in the table, not in the view, whether or not the member is reserved. Reserving a second time moves neither list.
    LayoutSetID thousand = LayoutSets::of(LayoutID(1000u));
    LayoutSets::View viewOfNine = sets.at(nine);
    LayoutSets::View viewOfThousand = sets.at(thousand);
    table.reserveMembers(2000);
    AOT_TEST_CHECK(sets.at(nine).list().data() == viewOfNine.list().data() && sets.at(thousand).list().data() == viewOfThousand.list().data());
    AOT_TEST_CHECK(std::ranges::equal(viewOfThousand.list(), std::array { LayoutID(1000u) }));
    AOT_TEST_CHECK(sets.listOf(none).empty() && sets.listOf(nine).data() == viewOfNine.list().data() && std::ranges::equal(sets.listOf(both), view.list()));

    // A wide set has chunks, and a list once somebody asks: of the table also when it is frozen, of an arena until reset().
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

// How many of the lists of words begin where the list in front of them in their shard ends.
class AdjacentWordListCounter {
public:
    void note(uint32_t set, std::span<const uint32_t> words)
    {
        const uint32_t*& endOfLast = m_endOfLastInShard[(set & idPositionMask) >> (idHomeShift - std::countr_zero(IDSetTable::numberOfShards))];
        m_count += words.data() == endOfLast;
        endOfLast = words.data() + words.size();
    }

    uint32_t count() const { return m_count; }

private:
    std::array<const uint32_t*, IDSetTable::numberOfShards> m_endOfLastInShard { };
    uint32_t m_count { 0 };
};

} // anonymous namespace

// The table frees them block by block. Were each an allocation of its own, its destructor would have to go over every set to find them.
static void wordsOutsideRecordsOfIDSetsLieInBlocks()
{
    constexpr uint32_t numberOfSets = 32 * IDSetTable::numberOfShards;
    constexpr uint32_t size = IDSetTable::mostMembersOfList + 1;
    static_assert(numberOfSets <= membersPerSetChunk);
    static_assert(size * sizeof(uint32_t) % 16 && size * sizeof(IDSetChunkEntry) % 16, "an allocation begins at a multiple of sixteen bytes, so none begins where one of these ends");

    IDSetTable tableWithLists(IDSetForm::ChunksWithLazyList, 0);
    IDSetArena arenaWithLists(tableWithLists, ArenaKeeps::InProgram);
    AdjacentWordListCounter lazyLists;
    for (uint32_t number = 0; number < numberOfSets; ++number) {
        IDSetMembers members(size);
        std::iota(members.begin(), members.end(), number * size);
        uint32_t set = arenaWithLists.makeFromSorted(members);
        AOT_TEST_CHECK(std::ranges::equal(tableWithLists.listOf(set), members));
        lazyLists.note(set, tableWithLists.listOf(set));
    }
    AOT_TEST_CHECK(lazyLists.count() > numberOfSets / 2);

    // One member in each of so many chunks that the entries lie outside the record.
    IDSetTable table(IDSetForm::ListsAndChunks, 0);
    IDSetArena arena(table, ArenaKeeps::InProgram);
    AdjacentWordListCounter entries;
    for (uint32_t number = 0; number < numberOfSets; ++number) {
        IDSetMembers members;
        for (uint32_t chunk = 0; chunk < size; ++chunk)
            members.push_back(chunk * membersPerSetChunk + number);
        uint32_t set = arena.makeFromSorted(members);
        std::span<const IDSetChunkEntry> entriesOfSet = arena.viewOf(set).chunkEntries();
        AOT_TEST_CHECK(entriesOfSet.size() == size && membersOfIDSetView(arena.viewOf(set)) == members);
        entries.note(set, unsafeMakeSpan(reinterpret_cast<const uint32_t*>(entriesOfSet.data()), 2 * entriesOfSet.size()));
    }
    AOT_TEST_CHECK(entries.count() > numberOfSets / 2);
}

static void freezingIDSetTableFreesOutgrownSlots()
{
    constexpr uint32_t numberOfSets = 64 * IDSetTable::numberOfShards;
    auto membersOf = [](uint32_t number) {
        return std::array { number, number + numberOfSets };
    };

    IDSetTable table(IDSetForm::ListsAndChunks, 0);
    std::vector<uint32_t> sets;
    {
        IDSetArena arena(table, ArenaKeeps::InProgram);
        for (uint32_t number = 0; number < numberOfSets; ++number)
            sets.push_back(arena.makeFromSorted(membersOf(number)));
    }
    size_t sizeWhileItGrew = table.sizeInBytes();
    table.freeze();
    AOT_TEST_CHECK(table.sizeInBytes() < sizeWhileItGrew);

    IDSetArena arena(table, ArenaKeeps::UntilReset);
    for (uint32_t number = 0; number < numberOfSets; ++number)
        AOT_TEST_CHECK(arena.makeFromSorted(membersOf(number)) == sets[number]);
}

// Types, keys and parts of classes each have an index of their own, so each grows in a table of its own: what another index frees hides nothing.
// One arena is told what each value held before and the other is not. They share a table, so equal answers have equal ids, and neither sees what the other remembers.
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
    // The first is a list. Each later one holds the one in front of it and goes on under further numbers.
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

    // Any string swallows the literals, whatever is said of them.
    TypeID withAnyString = told.unionOf(std::array { start, TypeID(TypeTable::string), states[2] }, std::array { TypeID(), TypeID(), states[1] });
    AOT_TEST_CHECK(withAnyString == untold.unionOf(untold.unionOf(start, TypeTable::string), states[2]) && !told.at(withAnyString).stringLiterals);
    told.forgetEarlierStates();
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

// Some texts have their keys when the rest get theirs, in an order that is not that of the texts, with keys of another kind between them.
static void keysOfTextsAreNumberedByTextOnAnyNumberOfThreads()
{
    constexpr uint32_t numberOfStrings = 5 * indicesInRangeOfLoop + 77;
    auto numbersOfKeys = [](const auto& addKeysOfOtherTexts) {
        TypeTable table(BuiltinTable::singleton());
        table.setObjects(makeUniqueWithoutFastMallocCheck<ObjectTable>(BuiltinTable::singleton(), std::span<const FunctionInOrderOfObjects>()));
        {
            TypeArena types(table, ArenaKeeps::InProgram);
            for (uint32_t text = numberOfStrings; text--;) {
                if (!(text % 7))
                    types.textKey(StringID(text));
                if (!(text % 1000))
                    types.registeredSymbolKey(StringID(text));
            }
            // A whole range that lacks nothing.
            for (uint32_t text = 2 * indicesInRangeOfLoop; text < 3 * indicesInRangeOfLoop; ++text)
                types.textKey(StringID(text));
        }
        addKeysOfOtherTexts(table);

        std::vector<uint32_t> numbers;
        for (uint32_t text = 0; text < numberOfStrings; ++text) {
            std::optional<PropertyKeyID> key = table.find(PropertyKey { .kind = PropertyKeyKind::Text, .text = StringID(text) });
            AOT_TEST_CHECK(key && table.at(*key).text == StringID(text) && table.at(*key).kind == PropertyKeyKind::Text);
            numbers.push_back(key->index());
        }
        numbers.push_back(table.numberOfPropertyKeys());
        return numbers;
    };

    std::vector<uint32_t> oneByOne = numbersOfKeys([](TypeTable& table) {
        TypeArena types(table, ArenaKeeps::InProgram);
        for (uint32_t text = 0; text < numberOfStrings; ++text)
            types.textKey(StringID(text));
    });
    for (unsigned numberOfThreads : { 1u, 3u, 8u }) {
        AOT_TEST_CHECK(oneByOne == numbersOfKeys([&](TypeTable& table) {
            ParallelLoops loops(numberOfThreads);
            table.addKeysOfTexts(numberOfStrings, loops);
            // Nothing is left to add.
            table.addKeysOfTexts(numberOfStrings, loops);
        }));
    }
}

static void idPairMemoKeepsWhatIsAskedFor()
{
    constexpr uint32_t numberAskedAgain = IDPairMemo::leastAnswersInGeneration;
    auto answerTo = [](uint32_t pair) {
        return pair * 1000003 + 7;
    };
    auto add = [&](IDPairMemo& memo, uint32_t pair) {
        memo.add(pair % 3, pair, pair / 2, answerTo(pair));
    };
    // A build with assertions withholds one hit in sixteen, never two in a row.
    auto find = [&](IDPairMemo& memo, uint32_t pair) {
        std::optional<uint32_t> answer = memo.find(pair % 3, pair, pair / 2);
        return answer ? answer : memo.find(pair % 3, pair, pair / 2);
    };

    IDPairMemo memo;
    AOT_TEST_CHECK(!memo.find(0, 1, 2) && !memo.sizeInBytes());
    for (uint32_t pair = 0; pair < numberAskedAgain; ++pair) {
        AOT_TEST_CHECK(!find(memo, pair));
        add(memo, pair);
    }
    for (uint32_t pair = 0; pair < numberAskedAgain; ++pair) {
        AOT_TEST_CHECK(find(memo, pair) == answerTo(pair));
        AOT_TEST_CHECK(!memo.find(pair % 3 + 3, pair, pair / 2) && !memo.find(pair % 3, pair / 2, pair + 1));
    }

    // What is asked for again stays, beside as many answers that nobody asks for again.
    uint32_t next = numberAskedAgain;
    for (unsigned round = 0; round < 12; ++round) {
        for (uint32_t pair = 0; pair < numberAskedAgain; ++pair) {
            AOT_TEST_CHECK(find(memo, pair) == answerTo(pair));
            add(memo, next++);
        }
    }
    AOT_TEST_CHECK(!find(memo, numberAskedAgain));

    // What nobody asks for again takes no room for long.
    size_t sizeWithWhatIsAskedAgain = memo.sizeInBytes();
    for (unsigned count = 0; count < 64 * numberAskedAgain; ++count)
        add(memo, next++);
    AOT_TEST_CHECK(memo.sizeInBytes() < sizeWithWhatIsAskedAgain && find(memo, next - 1) == answerTo(next - 1) && !find(memo, 0));

    memo.clear();
    AOT_TEST_CHECK(!memo.find(0, 0, 0) && !memo.sizeInBytes());

    IDPairMemo memoThatKeepsEverything;
    memoThatKeepsEverything.keepEverything();
    for (uint32_t pair = 0; pair < 8 * numberAskedAgain; ++pair)
        add(memoThatKeepsEverything, pair);
    for (uint32_t pair = 0; pair < 8 * numberAskedAgain; ++pair)
        AOT_TEST_CHECK(find(memoThatKeepsEverything, pair) == answerTo(pair));
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

    // Fewer ids than a generation holds.
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

static void sharedAnswersGiveEqualListsOneNumber()
{
    constexpr uint32_t numberOfLists = 20000;
    SharedAnswers answers;
    std::vector<uint32_t> numbers;
    for (uint32_t index = 0; index < numberOfLists; ++index)
        numbers.push_back(answers.internList(listForSharedAnswers(index)));

    std::set<uint32_t> distinct(numbers.begin(), numbers.end());
    AOT_TEST_CHECK(distinct.size() == numberOfLists);
    for (uint32_t index = 0; index < numberOfLists; ++index) {
        IDSetMembers list = listForSharedAnswers(index);
        AOT_TEST_CHECK(numbers[index] < 1u << 30 && !liesInThread(LayoutSetID::fromBits(numbers[index])));
        AOT_TEST_CHECK(answers.internList(list) == numbers[index]);
        AOT_TEST_CHECK(std::ranges::equal(answers.listAt(numbers[index]), list));
    }
    // A list is no prefix of another, and an order is not a set.
    AOT_TEST_CHECK(answers.internList(IDSetMembers { 1, 2 }) != answers.internList(IDSetMembers { 1, 2, 0 }));
    AOT_TEST_CHECK(answers.internList(IDSetMembers { 1, 2 }) != answers.internList(IDSetMembers { 2, 1 }));
}

// The first look compares one list of the hash outside the lock. Where that is another list, the search goes on under the lock.
static void sharedAnswersTellListsWithOneHashApart()
{
    constexpr uint32_t numberOfLists = 300;
    constexpr uint64_t hashes[] = { 0, 0x123456789abcdef0ull, UINT64_MAX };
    SharedAnswers answers;
    std::vector<uint32_t> numbers;
    for (uint32_t index = 0; index < numberOfLists; ++index)
        numbers.push_back(answers.internListWithHash(listForSharedAnswers(index), hashes[index % 3]));

    std::set<uint32_t> distinct(numbers.begin(), numbers.end());
    AOT_TEST_CHECK(distinct.size() == numberOfLists);
    for (uint32_t index = numberOfLists; index--;) {
        IDSetMembers list = listForSharedAnswers(index);
        AOT_TEST_CHECK(answers.internListWithHash(list, hashes[index % 3]) == numbers[index]);
        AOT_TEST_CHECK(std::ranges::equal(answers.listAt(numbers[index]), list));
    }
}

// A thread compares the words of a list outside the lock while the others add lists to the same shard, which moves the records of the lists and not their words.
static void eightThreadsGetOneNumberForEachSharedList()
{
    constexpr unsigned numberOfThreads = 8;
    constexpr uint32_t numberOfLists = 20000;
    // Each is coprime to the number of lists, so each thread comes to every list, in an order of its own.
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
    // Each word tells two questions apart.
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

static void sharedAnswersWorkOutOnceAndCountWhatTheyHold()
{
    SharedAnswers answers;
    size_t sizeWhileEmpty = answers.sizeInBytes();
    SharedAnswers::Question question { SharedQuestion::StateOfFieldArm, 1, 2, 3, 4, 5 };
    unsigned timesWorkedOut = 0;
    auto workOut = [&] {
        ++timesWorkedOut;
        return makeUniqueWithoutFastMallocCheck<NumberKeptInSharedAnswers>(7u);
    };
    const NumberKeptInSharedAnswers& first = answers.findOrAdd<NumberKeptInSharedAnswers>(question, workOut);
    AOT_TEST_CHECK(first.number == 7 && timesWorkedOut == 1);
    const NumberKeptInSharedAnswers& second = answers.findOrAdd<NumberKeptInSharedAnswers>(question, workOut);
    AOT_TEST_CHECK(&second == &first);
    AOT_TEST_CHECK(timesWorkedOut == (SharedAnswers::worksFoundAnswersOutAgain ? 2u : 1u));
    AOT_TEST_CHECK(answers.sizeInBytes() >= sizeWhileEmpty + sizeof(NumberKeptInSharedAnswers));

    answers.internList(IDSetMembers { 4, 5, 6 });
    StringPrintStream counts;
    answers.dumpCounts(counts);
    String text = counts.toString();
    AOT_TEST_CHECK(text.contains("| lists 1 | words of lists 3\n"_s));
    AOT_TEST_CHECK(text.contains("| StateOfFieldArm | asked 2 | found 1 | answers 1 | bytes "_s));
    AOT_TEST_CHECK(text.contains("| PartitionOfMembers | asked 0 | found 0 | answers 0 | bytes 0\n"_s));
}

// What SharedAnswers cannot keep because a word of the question or the answer lies in the thread: TypeArena::keepAnswerUntilReset().
static void arenaKeepsAnswersUnderListsOfWordsUntilReset()
{
    TypeTable table(BuiltinTable::singleton());
    table.setObjects(makeUniqueWithoutFastMallocCheck<ObjectTable>(BuiltinTable::singleton(), std::span<const FunctionInOrderOfObjects>()));
    table.freeze();
    TypeArena types(table, ArenaKeeps::UntilReset);

    constexpr uint32_t numberOfQuestions = 300;
    auto questionAt = [](uint32_t number) {
        return std::array<uint32_t, 4> { 1, 0xc0000000u + number, UINT32_MAX, number % 3 };
    };
    for (unsigned turn = 0; turn < 2; ++turn) {
        for (uint32_t number = 0; number < numberOfQuestions; ++number) {
            AOT_TEST_CHECK(!types.answerKeptUntilReset(questionAt(number)));
            const SharedAnswers::Answer& kept = types.keepAnswerUntilReset(questionAt(number), makeUniqueWithoutFastMallocCheck<NumberKeptInSharedAnswers>(number + turn));
            AOT_TEST_CHECK(types.answerKeptUntilReset(questionAt(number)) == &kept);
        }
        for (uint32_t number = 0; number < numberOfQuestions; ++number) {
            const SharedAnswers::Answer* found = types.answerKeptUntilReset(questionAt(number));
            AOT_TEST_CHECK(found && static_cast<const NumberKeptInSharedAnswers&>(*found).number == number + turn);
        }
        // A list that begins like another, and one that goes on behind it, are other questions.
        std::array<uint32_t, 4> whole = questionAt(7);
        std::array<uint32_t, 5> longer { whole[0], whole[1], whole[2], whole[3], 0 };
        AOT_TEST_CHECK(!types.answerKeptUntilReset(std::span<const uint32_t>(whole).first(3)));
        AOT_TEST_CHECK(!types.answerKeptUntilReset(longer));
        types.reset();
    }
    AOT_TEST_CHECK(!types.answerKeptUntilReset(questionAt(7)));
}

namespace {

// The five objects of each function, none with a layout. `all` is a set of the program that holds those of the first twenty thousand functions. Those of the last twenty are members of no set of the program.
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

    // Of the functions from `begin` up to and not including `end`.
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

// filterMembersOncePerBuild() with a test that goes by a trait and counts the members that it is asked about.
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

    // What is picked is what asking each member picks. A build that works found answers out again asks more members than `numberAsked`.
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

static void cutOfProgramSetAsksNoMember()
{
    ProgramForCutsOfIDSets program;
    TypeArena types(program.table, ArenaKeeps::UntilReset);
    ObjectSets& sets = types.objectSets();
    AOT_TEST_CHECK(sets.sizeOf(program.all) == largestTestedIDSetSize && sets.setOfProgramBehind(program.all) == program.all);

    ObjectsPickedOutOfCutsByTrait closures(types, ObjectTrait::IsClosure);
    closures.checkPicks(program.all, largestTestedIDSetSize);
    closures.checkPicks(program.all, 0);

    auto cutIn = [&](TypeArena& arena) {
        return program.allWithout(arena, { program.objectOf(ObjectKind::Closure, 7), program.objectOf(ObjectKind::FunctionPrototype, 7), program.objectOf(ObjectKind::ConstructedObject, ProgramForCutsOfIDSets::numberOfFunctionsInSet - 1) });
    };
    ObjectSetID cut = cutIn(types);
    closures.checkPicks(cut, 0);
    AOT_TEST_CHECK(sets.sizeOf(closures.pick(cut)) == ProgramForCutsOfIDSets::numberOfFunctionsInSet - 1);

    // Another thread finds what the first has left.
    TypeArena typesOfOtherThread(program.table, ArenaKeeps::UntilReset);
    ObjectsPickedOutOfCutsByTrait closuresOfOtherThread(typesOfOtherThread, ObjectTrait::IsClosure);
    closuresOfOtherThread.checkPicks(cutIn(typesOfOtherThread), 0);
}

// Nobody has asked about the set of the program. Its members are asked, once for the build, and the answer serves it and whatever is cut out of it.
static void cutOfProgramSetAnswersForWholeSet()
{
    ProgramForCutsOfIDSets program;
    TypeArena types(program.table, ArenaKeeps::UntilReset);
    // Four of five pass, so what is kept is the list of those that do not.
    ObjectsPickedOutOfCutsByTrait others(types, ObjectTrait::IsOtherObject);
    others.checkPicks(program.allWithout(types, { program.objectOf(ObjectKind::Closure, 1), program.objectOf(ObjectKind::RestArray, 1) }), largestTestedIDSetSize);
    others.checkPicks(program.all, 0);
    others.checkPicks(program.allWithout(types, { program.objectOf(ObjectKind::Closure, 2), program.objectOf(ObjectKind::CoroutineResult, 9000) }), 0);

    // No object passes.
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

    // A set that was made member by member came of no other set.
    ObjectSetID ofFirstFunctions = program.objectsOfFunctions(types, 0, ProgramForCutsOfIDSets::numberOfFunctionsInSet / 2);
    AOT_TEST_CHECK(liesInThread(ofFirstFunctions) && !sets.setOfProgramBehind(ofFirstFunctions));
    ObjectSetID kept = sets.intersectionOf(cutAgain, ofFirstFunctions);
    AOT_TEST_CHECK(kept != ofFirstFunctions && sets.setOfProgramBehind(kept) == program.all);
    closures.checkPicks(kept, 0);

    // What one test has picked is asked about by the next.
    ObjectSetID picked = closures.pick(cutAgain);
    AOT_TEST_CHECK(liesInThread(picked) && sets.setOfProgramBehind(picked) == program.all);
    ObjectsPickedOutOfCutsByTrait callables(types, ObjectTrait::IsCallable);
    callables.checkPicks(program.all, largestTestedIDSetSize);
    callables.checkPicks(picked, 0);
    AOT_TEST_CHECK(callables.pick(picked) == picked);

    // Nobody has asked about the set of the program, and this is less than half of it: its own members are asked, and no answer is left for the build.
    ObjectsPickedOutOfCutsByTrait withoutShape(types, ObjectTrait::HasNoShape);
    withoutShape.checkPicks(picked, sets.sizeOf(picked));
    withoutShape.checkPicks(program.all, largestTestedIDSetSize);
    withoutShape.checkPicks(picked, 0);
}

static void joinOfCutsOfProgramSetAsksNoMember()
{
    ProgramForCutsOfIDSets program;
    TypeArena types(program.table, ArenaKeeps::UntilReset);
    ObjectSets& sets = types.objectSets();
    ObjectsPickedOutOfCutsByTrait closures(types, ObjectTrait::IsClosure);
    closures.checkPicks(program.all, largestTestedIDSetSize);

    auto closure = [&](uint32_t function) {
        return program.objectOf(ObjectKind::Closure, function);
    };
    std::array<ObjectSetID, 3> cuts { program.allWithout(types, { closure(1), closure(2), closure(5000) }), program.allWithout(types, { closure(2), closure(5000), closure(6000) }), program.allWithout(types, { closure(5000), closure(19000) }) };

    ObjectSetID joined = sets.unionOf(cuts[0], cuts[1]);
    AOT_TEST_CHECK(joined == program.allWithout(types, { closure(2), closure(5000) }));
    closures.checkPicks(joined, 0);

    ObjectSetID joinedOfThree = sets.unionOf(cuts);
    AOT_TEST_CHECK(joinedOfThree == program.allWithout(types, { closure(5000) }));
    closures.checkPicks(joinedOfThree, 0);

    // In an arena that has made neither result before.
    TypeArena typesOfOtherThread(program.table, ArenaKeeps::UntilReset);
    ObjectSets& setsOfOtherThread = typesOfOtherThread.objectSets();
    ObjectSetID joinedThere = setsOfOtherThread.unionOf(program.allWithout(typesOfOtherThread, { closure(1), closure(2) }), program.allWithout(typesOfOtherThread, { closure(2), closure(3) }));
    AOT_TEST_CHECK(liesInThread(joinedThere) && setsOfOtherThread.sizeOf(joinedThere) == largestTestedIDSetSize - 1 && setsOfOtherThread.setOfProgramBehind(joinedThere) == program.all);
    std::array<ObjectSetID, 3> cutsThere { program.allWithout(typesOfOtherThread, { closure(4), closure(5) }), program.allWithout(typesOfOtherThread, { closure(5), closure(6) }), program.allWithout(typesOfOtherThread, { closure(5), closure(7) }) };
    ObjectSetID joinedOfThreeThere = setsOfOtherThread.unionOf(cutsThere);
    AOT_TEST_CHECK(liesInThread(joinedOfThreeThere) && setsOfOtherThread.sizeOf(joinedOfThreeThere) == largestTestedIDSetSize - 1 && setsOfOtherThread.setOfProgramBehind(joinedOfThreeThere) == program.all);
    ObjectsPickedOutOfCutsByTrait closuresOfOtherThread(typesOfOtherThread, ObjectTrait::IsClosure);
    closuresOfOtherThread.checkPicks(joinedThere, 0);
    closuresOfOtherThread.checkPicks(joinedOfThreeThere, 0);
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

static void setUnderChunkAsksEachMember()
{
    ProgramForCutsOfIDSets program;
    TypeArena types(program.table, ArenaKeeps::UntilReset);
    ObjectSets& sets = types.objectSets();
    ObjectsPickedOutOfCutsByTrait closures(types, ObjectTrait::IsClosure);
    closures.checkPicks(program.all, largestTestedIDSetSize);

    AOT_TEST_CHECK(sets.sizeOf(program.shortSet) <= membersPerSetChunk && !sets.setOfProgramBehind(program.shortSet));
    ObjectSetID fewLeft = sets.filtered(program.all, [](ObjectID object) {
        return object.index() % 64 == 1;
    });
    AOT_TEST_CHECK(liesInThread(fewLeft) && sets.sizeOf(fewLeft) > IDSetTable::mostMembersOfList && sets.sizeOf(fewLeft) <= membersPerSetChunk && !sets.setOfProgramBehind(fewLeft));
    for (unsigned repetition = 0; repetition < 2; ++repetition) {
        closures.checkPicks(program.shortSet, sets.sizeOf(program.shortSet));
        closures.checkPicks(fewLeft, sets.sizeOf(fewLeft));
    }
}

static void cutOfProgramSetIsForgottenAtReset()
{
    ProgramForCutsOfIDSets program;
    TypeArena types(program.table, ArenaKeeps::UntilReset);
    ObjectSets& sets = types.objectSets();
    ObjectsPickedOutOfCutsByTrait closures(types, ObjectTrait::IsClosure);
    closures.checkPicks(program.all, largestTestedIDSetSize);

    for (unsigned round = 0; round < 3; ++round) {
        for (uint32_t function = 0; function < 40; ++function)
            closures.checkPicks(program.allWithout(types, { program.objectOf(ObjectKind::Closure, function * 500) }), 0);
        types.reset();
        // The places of the arena hold other sets now, which came of no set.
        for (uint32_t begin = 1; begin < 4; ++begin) {
            ObjectSetID made = program.objectsOfFunctions(types, begin, ProgramForCutsOfIDSets::numberOfFunctionsInSet + 1);
            AOT_TEST_CHECK(liesInThread(made) && !sets.setOfProgramBehind(made));
            closures.checkPicks(made, sets.sizeOf(made));
        }
        types.reset();
    }
}

static void cutOfProgramSetAnnotatesChangedChunksAlone()
{
    IDSetTable table(IDSetForm::ListsAndChunks, IDSetArenaUnderTest::numberOfAttachedWords);
    WeakRandom random(11);
    IDSetWithMembers whole;
    {
        IDSetArenaUnderTest tested(table, ArenaKeeps::InProgram);
        whole = tested.make(modelOfIDSet(IDSetPattern::Dense, largestTestedIDSetSize, random));
        AOT_TEST_CHECK(!tested.numberOfUnionsAnnotated && tested.numberOfMembersAnnotated == whole.members.size());
    }
    table.freeze();

    IDSetArenaUnderTest tested(table, ArenaKeeps::UntilReset);
    auto checkIsCut = [&](uint32_t cut, const IDSetMembers& cutAway) {
        AOT_TEST_CHECK(liesInThread(ObjectSetID::fromBits(cut)));
        IDSetWithMembers made { cut, combineIDSetMembers(IDSetOperation::Difference, whole.members, cutAway) };
        tested.checkHolds(made.set, made.members);
        return made;
    };
    uint32_t inFirstChunk = whole.members[10];
    uint32_t alsoInFirstChunk = whole.members[20];
    uint32_t inTheMiddle = whole.members[whole.members.size() / 2];
    AOT_TEST_CHECK(inFirstChunk / membersPerSetChunk == alsoInFirstChunk / membersPerSetChunk && inFirstChunk / membersPerSetChunk != inTheMiddle / membersPerSetChunk);

    for (unsigned round = 0; round < 2; ++round) {
        // Behind a reset the members of each chunk are read once.
        tested.numberOfMembersAnnotated = 0;
        IDSetWithMembers first = checkIsCut(tested.arena.without(whole.set, inFirstChunk), { inFirstChunk });
        AOT_TEST_CHECK(tested.numberOfMembersAnnotated == first.members.size());

        // The chunk that this cut changes, and the first chunk as the whole set has it.
        tested.numberOfMembersAnnotated = 0;
        IDSetWithMembers second = checkIsCut(tested.arena.without(whole.set, inTheMiddle), { inTheMiddle });
        AOT_TEST_CHECK(tested.numberOfMembersAnnotated && tested.numberOfMembersAnnotated < 2 * membersPerSetChunk);

        tested.numberOfMembersAnnotated = 0;
        checkIsCut(tested.arena.filtered(second.set, [&](uint32_t member) {
            return member != alsoInFirstChunk;
        }), { alsoInFirstChunk, inTheMiddle });
        AOT_TEST_CHECK(tested.numberOfMembersAnnotated && tested.numberOfMembersAnnotated < membersPerSetChunk);

        // Both of its changed chunks have been read.
        tested.numberOfMembersAnnotated = 0;
        checkIsCut(tested.arena.intersectionOf(first.set, second.set), { inFirstChunk, inTheMiddle });
        AOT_TEST_CHECK(!tested.numberOfMembersAnnotated);

        tested.checkOperations(first, second);
        tested.arena.reset();
    }

    // A long set with few members in a chunk is read as ever.
    unsigned numberOfUnionsAnnotated = tested.numberOfUnionsAnnotated;
    tested.numberOfMembersAnnotated = 0;
    IDSetWithMembers sparse = tested.make(modelOfIDSet(IDSetPattern::OneInEachChunk, largestTestedIDSetSize, random));
    AOT_TEST_CHECK(tested.numberOfMembersAnnotated == sparse.members.size() && tested.numberOfUnionsAnnotated == numberOfUnionsAnnotated);
    tested.checkHolds(sparse.set, sparse.members);

    IDSetWithMembers scattered = tested.make(modelOfIDSet(IDSetPattern::Random, largestTestedIDSetSize, random));
    AOT_TEST_CHECK(tested.numberOfUnionsAnnotated > numberOfUnionsAnnotated);
    tested.checkHolds(scattered.set, scattered.members);
}

static bool sweepValueSetHolds(const SweepValueSets& sets, SweepValueSets::Set set, const IDSetModel& model)
{
    IDSetMembers members;
    sets.forEachMember(set, [&](uint32_t member) {
        members.push_back(member);
    });
    return std::ranges::equal(members, model) && std::ranges::all_of(model, [&](uint32_t member) {
        return sets.contains(set, member);
    });
}

// One set is changed step by step, as the sweep changes the live set on its way through a block. Now and then it is kept, as the set at the head of a block is.
// Whatever becomes of the sets that are made out of it, a kept set holds what it held.
static void sweepValueSetsAgreeWithModel()
{
    for (uint32_t range : { 60u, 700u, 150000u, UINT32_MAX }) {
        WeakRandom random(range);
        SweepValueSets sets;
        std::vector<std::pair<SweepValueSets::Set, IDSetModel>> kept;
        SweepValueSets::Set set = SweepValueSets::empty;
        IDSetModel model;
        for (unsigned step = 0; step < 6000; ++step) {
            uint32_t member = random.getUint32(range);
            switch (random.getUint32(10)) {
            case 0:
            case 1:
            case 2:
            case 3:
                set = sets.with(set, member);
                model.insert(member);
                break;
            case 4:
                set = sets.without(set, member);
                model.erase(member);
                break;
            case 5:
            case 6:
            case 7: {
                auto found = model.lower_bound(member);
                if (found == model.end())
                    break;
                member = *found;
                set = sets.without(set, member);
                model.erase(found);
                break;
            }
            case 8:
                kept.push_back({ set, model });
                sets.keepSetsMadeSoFar();
                break;
            default: {
                if (kept.empty())
                    break;
                const auto& [other, membersOfOther] = kept[random.getUint32(static_cast<uint32_t>(kept.size()))];
                set = sets.unite(set, other);
                model.insert(membersOfOther.begin(), membersOfOther.end());
                break;
            }
            }
            AOT_TEST_CHECK(sets.contains(set, member) == !!model.count(member));
            if (!(step % 300))
                AOT_TEST_CHECK(sweepValueSetHolds(sets, set, model));
        }
        AOT_TEST_CHECK(sweepValueSetHolds(sets, set, model));
        for (const auto& [keptSet, membersOfKeptSet] : kept)
            AOT_TEST_CHECK(sweepValueSetHolds(sets, keptSet, membersOfKeptSet));
    }
}

static void sweepValueSetsShareWhatTheyHaveInCommon()
{
    constexpr uint32_t mostNodesOnOneWayDown = 32;
    SweepValueSets sets;
    SweepValueSets::Set large = SweepValueSets::empty;
    for (uint32_t member = 0; member < 100000; member += 7)
        large = sets.with(large, member);
    sets.keepSetsMadeSoFar();

    // What changes nothing makes nothing.
    uint32_t numberOfNodes = sets.numberOfNodes();
    AOT_TEST_CHECK(sets.with(large, 700) == large && sets.without(large, 701) == large);
    AOT_TEST_CHECK(sets.unite(large, large) == large && sets.unite(large, SweepValueSets::empty) == large && sets.unite(SweepValueSets::empty, large) == large);
    AOT_TEST_CHECK(sets.numberOfNodes() == numberOfNodes);

    // One member more costs the way down to it, however many the set holds, and leaves the set as it was.
    SweepValueSets::Set withOneMore = sets.with(large, 701);
    sets.keepSetsMadeSoFar();
    SweepValueSets::Set withAnother = sets.with(large, 99999);
    sets.keepSetsMadeSoFar();
    AOT_TEST_CHECK(sets.numberOfNodes() - numberOfNodes <= 2 * mostNodesOnOneWayDown);
    AOT_TEST_CHECK(!sets.contains(large, 701) && sets.contains(withOneMore, 701) && !sets.contains(withOneMore, 99999) && sets.contains(withAnother, 99999));

    // Two sets that were made out of one are united where they differ.
    numberOfNodes = sets.numberOfNodes();
    SweepValueSets::Set united = sets.unite(withOneMore, withAnother);
    AOT_TEST_CHECK(sets.numberOfNodes() - numberOfNodes <= 2 * mostNodesOnOneWayDown);
    AOT_TEST_CHECK(sets.contains(united, 701) && sets.contains(united, 99999) && sets.contains(united, 700) && !sets.contains(united, 702));
    numberOfNodes = sets.numberOfNodes();
    AOT_TEST_CHECK(sets.unite(united, withOneMore) == united && sets.unite(united, large) == united && sets.numberOfNodes() == numberOfNodes);

    // Until it is kept a set is changed in place.
    sets.keepSetsMadeSoFar();
    SweepValueSets::Set changing = sets.with(united, 702);
    numberOfNodes = sets.numberOfNodes();
    changing = sets.with(changing, 703);
    changing = sets.without(changing, 702);
    AOT_TEST_CHECK(sets.numberOfNodes() == numberOfNodes && sets.contains(changing, 703) && !sets.contains(changing, 702) && !sets.contains(united, 703));
}

std::span<const NativeTest> idSetTests()
{
#define AOT_ID_SET_TEST_OF_BOTH_FORMS(name, function) \
    { "id-sets/" name "-lazy-lists"_s, function<IDSetForm::ChunksWithLazyList> }, \
    { "id-sets/" name "-chunks"_s, function<IDSetForm::ListsAndChunks> },
    static constexpr NativeTest tests[] = {
        { "id-sets/sweep-value-sets-agree-with-model"_s, sweepValueSetsAgreeWithModel },
        { "id-sets/sweep-value-sets-share-what-they-have-in-common"_s, sweepValueSetsShareWhatTheyHaveInCommon },
        { "id-sets/cut-of-a-program-set-asks-no-member"_s, cutOfProgramSetAsksNoMember },
        { "id-sets/cut-of-a-program-set-answers-for-the-whole-set"_s, cutOfProgramSetAnswersForWholeSet },
        { "id-sets/cut-of-a-program-set-cut-again"_s, cutOfCutOfProgramSetAsksNoMember },
        { "id-sets/cut-of-a-program-set-joined"_s, joinOfCutsOfProgramSetAsksNoMember },
        { "id-sets/cut-of-a-program-set-with-members-from-elsewhere"_s, cutOfProgramSetAsksMembersFromElsewhere },
        { "id-sets/cut-of-a-program-set-under-a-chunk-takes-the-plain-way"_s, setUnderChunkAsksEachMember },
        { "id-sets/cut-of-a-program-set-is-forgotten-at-reset"_s, cutOfProgramSetIsForgottenAtReset },
        { "id-sets/cut-of-a-program-set-annotates-changed-chunks-alone"_s, cutOfProgramSetAnnotatesChangedChunksAlone },
        AOT_ID_SET_TEST_OF_BOTH_FORMS("sizes", idSetsOfEverySizeHoldTheirMembers)
        AOT_ID_SET_TEST_OF_BOTH_FORMS("operations", idSetOperationsAgreeWithModel)
        AOT_ID_SET_TEST_OF_BOTH_FORMS("with-and-without", idSetsGrowAndShrinkByOneMember)
        AOT_ID_SET_TEST_OF_BOTH_FORMS("filtered-and-replaced", idSetsAreFilteredAndReplaced)
        AOT_ID_SET_TEST_OF_BOTH_FORMS("until-reset", idSetsOfThreadLastUntilReset)
        AOT_ID_SET_TEST_OF_BOTH_FORMS("attached-words", attachedWordsOfIDSetsFollowTheirMembers)
        AOT_ID_SET_TEST_OF_BOTH_FORMS("eight-threads", eightThreadsGetTheSameIDSets)
        AOT_ID_SET_TEST_OF_BOTH_FORMS("unions-of-many", unionsOfManyIDSetsAgreeWithModel)
        { "id-sets/union-hub"_s, unionsOfIDSetsThatHoldNearlyTheSame },
        { "id-sets/union-memo"_s, unionOfManyIDSetsRemembersOnlyWhatLiesInside },
        { "id-sets/union-memo-of-sets-that-bring-the-same-chunks"_s, unionOfIDSetsThatBringSameChunksHoldsTheirMembers },
        { "id-sets/scratch-is-counted-with-memos"_s, scratchOfIDSetsIsCountedWithMemosOfTypeArena },
        { idSetTestOfAnnotatorOnRequest, annotatorComesBackIntoIDSetArenaOnRequest },
        { "id-sets/laying-out-in-scratch-while-a-set-is-interned-stops-the-build"_s, layingOutInScratchWhileIDSetIsInternedStopsTheBuild },
        { "id-sets/unions-with-earlier-states"_s, unionsOfIDSetsWithEarlierStatesAgreeWithModel },
        { "id-sets/union-with-earlier-state-memo"_s, unionOfIDSetsWithEarlierStateRemembersOnlyWhatLiesInside },
        { "id-sets/unions-of-types-with-earlier-states"_s, unionsOfTypesWithEarlierStatesAreTheUnions },
        { "id-sets/sets-that-nothing-names-are-collected"_s, idSetsThatNothingNamesAreCollected },
        { "id-sets/shared-chunks"_s, idSetsThatDifferInOneMemberShareOtherChunks },
        { "id-sets/compare-by-content"_s, idSetsCompareByContent },
        { "id-sets/typed-sets"_s, typedIDSetsAnswerLikeUntypedOnes },
        { "id-sets/words-outside-records-lie-in-blocks"_s, wordsOutsideRecordsOfIDSetsLieInBlocks },
        { "id-sets/freezing-frees-outgrown-slots"_s, freezingIDSetTableFreesOutgrownSlots },
        { "id-sets/freezing-type-table-frees-outgrown-slots"_s, freezingTypeTableFreesOutgrownSlots },
        { "id-sets/keys-of-texts-are-numbered-by-text-on-any-number-of-threads"_s, keysOfTextsAreNumberedByTextOnAnyNumberOfThreads },
        { "id-sets/pair-memo"_s, idPairMemoKeepsWhatIsAskedFor },
        { "id-sets/list-memo"_s, idListMemoKeepsWhatIsAskedFor },
        { "id-sets/shared-answers-number-lists"_s, sharedAnswersGiveEqualListsOneNumber },
        { "id-sets/shared-answers-tell-lists-with-one-hash-apart"_s, sharedAnswersTellListsWithOneHashApart },
        { "id-sets/shared-answers-eight-threads-number-lists"_s, eightThreadsGetOneNumberForEachSharedList },
        { "id-sets/shared-answers-keep-the-first"_s, sharedAnswersKeepFirstAnswerOfEightThreads },
        { "id-sets/shared-answers-work-out-once-and-count"_s, sharedAnswersWorkOutOnceAndCountWhatTheyHold },
        { "id-sets/arena-keeps-answers-under-lists-of-words-until-reset"_s, arenaKeepsAnswersUnderListsOfWordsUntilReset },
    };
#undef AOT_ID_SET_TEST_OF_BOTH_FORMS
    return tests;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
