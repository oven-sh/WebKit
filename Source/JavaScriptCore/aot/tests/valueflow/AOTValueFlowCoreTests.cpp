/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTNativeTest.h"
#include "AOTThreads.h"
#include "AOTValueFlowAnalysis.h"
#include "AOTValueFlowGraph.h"
#include "AOTValueFlowSolver.h"
#include "AOTValueSets.h"
#include "AOTVector.h"
#include <array>
#include <bit>
#include <wtf/HashMap.h>
#include <wtf/Threading.h>
#include <wtf/WeakRandom.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(!std::is_convertible_v<ValueFlowName, ValueFlowCellName>);
static_assert(!std::is_invocable_v<decltype(&ValueFlowRuleContext::read), ValueFlowRuleContext&, ClimbingSet, ValueFlowName>);
static_assert(std::is_invocable_v<decltype(&ValueFlowRuleContext::read), ValueFlowRuleContext&, ClimbingSet, ValueFlowCellName>);

std::span<const NativeTest> valueFlowCoreTests();

namespace {

// Not a BitVector: with assertions on, that compares two of them bit by bit as well, which took most of six minutes in a debug build.
class PlainSet {
public:
    explicit PlainSet(uint32_t sizeOfUniverse)
        : m_sizeOfUniverse(sizeOfUniverse)
        , m_words(FillWith { }, (sizeOfUniverse + 63) / 64, 0)
    {
    }

    bool contains(uint32_t member) const { return m_words[member / 64] >> member % 64 & 1; }

    bool add(uint32_t member)
    {
        bool isNew = !contains(member);
        m_words[member / 64] |= 1ull << member % 64;
        return isNew;
    }

    uint32_t size() const
    {
        uint32_t size = 0;
        for (uint64_t word : m_words)
            size += std::popcount(word);
        return size;
    }

    uint32_t sizeOfUniverse() const { return m_sizeOfUniverse; }
    uint64_t wordAt(uint32_t index) const { return index < m_words.size() ? m_words[index] : 0; }

    bool unite(const PlainSet& other, const PlainSet* mask = nullptr)
    {
        bool hasGrown = false;
        for (uint32_t member = 0; member < sizeOfUniverse(); ++member) {
            if (other.contains(member) && (!mask || mask->contains(member)))
                hasGrown |= add(member);
        }
        return hasGrown;
    }

    friend bool operator==(const PlainSet& first, const PlainSet& second) { return first.m_words == second.m_words; }

private:
    uint32_t m_sizeOfUniverse;
    Vector<uint64_t> m_words;
};

} // anonymous namespace

static bool holdsTheSame(const ValueSets& sets, ValueSetID set, const PlainSet& plain)
{
    if (sets.size(set) != plain.size())
        return false;
    for (uint32_t member = 0; member < plain.sizeOfUniverse(); ++member) {
        if (sets.contains(set, ValueFlowObjectID(member)) != plain.contains(member))
            return false;
    }
    uint32_t listed = 0;
    uint32_t before = 0;
    bool isAscending = true;
    sets.forEachMember(set, [&](ValueFlowObjectID member) {
        isAscending &= !listed || member.index() > before;
        isAscending &= plain.contains(member.index());
        before = member.index();
        ++listed;
    });
    return isAscending && listed == plain.size();
}

static std::pair<ValueSetID, PlainSet> makeRandomSet(ValueSetMaker& maker, WeakRandom& random, uint32_t sizeOfUniverse, uint32_t mostMembers)
{
    PlainSet plain(sizeOfUniverse);
    Vector<ValueFlowObjectID> members;
    for (uint32_t count = random.getUint32(mostMembers + 1); count; --count) {
        uint32_t member = random.getUint32(sizeOfUniverse);
        members.append(ValueFlowObjectID(member));
        plain.add(member);
    }
    return { maker.of(members.span()), plain };
}

static void valueSetsAgreeWithPlainSets()
{
    constexpr uint32_t sizeOfUniverse = 3 * ValueSets::membersPerBlock + 77;
    WeakRandom random(11);
    ValueSets sets(sizeOfUniverse);
    ValueSetMaker maker(sets);

    Vector<std::pair<ValueSetID, PlainSet>> pool;
    pool.append({ ValueSets::nothing, PlainSet(sizeOfUniverse) });
    PlainSet everything(sizeOfUniverse);
    for (uint32_t member = 0; member < sizeOfUniverse; ++member)
        everything.add(member);
    pool.append({ ValueSets::anything, everything });
    for (unsigned index = 0; index < 60; ++index)
        pool.append(makeRandomSet(maker, random, sizeOfUniverse, index % 3 ? 6 : 900));
    for (auto& [set, plain] : pool)
        AOT_TEST_CHECK(holdsTheSame(sets, set, plain));

    for (unsigned round = 0; round < 3000; ++round) {
        auto& [first, firstPlain] = pool[random.getUint32(pool.size())];
        auto& [second, secondPlain] = pool[random.getUint32(pool.size())];
        PlainSet expected(sizeOfUniverse);
        ValueSetID result;
        bool shareMember = false;
        bool isSubset = true;
        for (uint32_t member = 0; member < sizeOfUniverse; ++member) {
            shareMember |= firstPlain.contains(member) && secondPlain.contains(member);
            isSubset &= !firstPlain.contains(member) || secondPlain.contains(member);
        }
        AOT_TEST_CHECK(sets.meet(first, second) == shareMember);
        AOT_TEST_CHECK(sets.isSubset(first, second) == isSubset);
        switch (random.getUint32(3)) {
        case 0:
            result = maker.unite(first, second);
            expected.unite(firstPlain);
            expected.unite(secondPlain);
            break;
        case 1:
            result = maker.intersect(first, second);
            expected.unite(firstPlain, &secondPlain);
            break;
        default:
            result = ValueSets::complement(first);
            for (uint32_t member = 0; member < sizeOfUniverse; ++member) {
                if (!firstPlain.contains(member))
                    expected.add(member);
            }
            break;
        }
        AOT_TEST_CHECK(holdsTheSame(sets, result, expected));
        for (auto& [other, otherPlain] : pool) {
            if (ValueSets::isComplement(other) == ValueSets::isComplement(result))
                AOT_TEST_CHECK((other == result) == (otherPlain == expected));
            if (otherPlain == expected && ValueSets::isComplement(other) == ValueSets::isComplement(result))
                AOT_TEST_CHECK(sets.hashOfMembers(other) == sets.hashOfMembers(result));
        }
        if (pool.size() < 300)
            pool.append({ result, expected });
    }

    Vector<ValueSetID> many;
    PlainSet unionOfMany(sizeOfUniverse);
    for (auto& [set, plain] : pool) {
        if (ValueSets::isComplement(set) || many.size() == 40)
            continue;
        many.append(set);
        unionOfMany.unite(plain);
    }
    AOT_TEST_CHECK(holdsTheSame(sets, maker.uniteAll(many.span()), unionOfMany));

    ValueSetID single = maker.of(ValueFlowObjectID(4097u));
    AOT_TEST_CHECK(sets.onlyMember(single) == ValueFlowObjectID(4097u));
    AOT_TEST_CHECK(!sets.onlyMember(ValueSets::nothing));
    AOT_TEST_CHECK(!sets.onlyMember(maker.unite(single, maker.of(ValueFlowObjectID(1u)))));
}

static void valueSetsOfFewAndOfManyBlocksAgreeWithPlainSets()
{
    constexpr uint32_t sizeOfUniverse = 64 * ValueSets::membersPerBlock;
    WeakRandom random(23);
    ValueSets sets(sizeOfUniverse);
    ValueSetMaker maker(sets);

    Vector<std::pair<ValueSetID, PlainSet>> pool;
    for (unsigned index = 0; index < 24; ++index)
        pool.append(makeRandomSet(maker, random, sizeOfUniverse, index % 2 ? 3 : 500));
    for (unsigned round = 0; round < 200; ++round) {
        auto& [first, firstPlain] = pool[random.getUint32(pool.size())];
        auto& [second, secondPlain] = pool[random.getUint32(pool.size())];
        PlainSet united(sizeOfUniverse);
        united.unite(firstPlain);
        united.unite(secondPlain);
        PlainSet shared(sizeOfUniverse);
        shared.unite(firstPlain, &secondPlain);
        PlainSet left(sizeOfUniverse);
        for (uint32_t member = 0; member < sizeOfUniverse; ++member) {
            if (firstPlain.contains(member) && !secondPlain.contains(member))
                left.add(member);
        }
        AOT_TEST_CHECK(holdsTheSame(sets, maker.unite(first, second), united));
        AOT_TEST_CHECK(holdsTheSame(sets, maker.intersect(first, second), shared));
        AOT_TEST_CHECK(holdsTheSame(sets, maker.intersect(first, ValueSets::complement(second)), left));
    }
}

// A list names a block, or a group of the blocks at 64 places. Sets that lie within one group, that have one block in each of several, and that fill several.
static void valueSetsOfSeveralGroupsOfBlocksAgreeWithPlainSets()
{
    constexpr uint32_t sizeOfUniverse = 150 * ValueSets::membersPerBlock + 5;
    WeakRandom random(31);
    ValueSets sets(sizeOfUniverse);
    ValueSetMaker maker(sets);

    Vector<std::pair<ValueSetID, PlainSet>> pool;
    for (unsigned index = 0; index < 30; ++index) {
        switch (index % 5) {
        case 0:
            pool.append(makeRandomSet(maker, random, sizeOfUniverse, 2));
            break;
        case 1:
            pool.append(makeRandomSet(maker, random, sizeOfUniverse, 12));
            break;
        case 2:
            pool.append(makeRandomSet(maker, random, sizeOfUniverse, 700));
            break;
        case 3:
            pool.append(makeRandomSet(maker, random, 70 * ValueSets::membersPerBlock, 60));
            pool.last().second = [&] {
                PlainSet wide(sizeOfUniverse);
                for (uint32_t member = 0; member < 70 * ValueSets::membersPerBlock; ++member) {
                    if (pool.last().second.contains(member))
                        wide.add(member);
                }
                return wide;
            }();
            break;
        default: {
            uint32_t first = random.getUint32(sizeOfUniverse);
            uint32_t end = first + random.getUint32(sizeOfUniverse - first + 1);
            PlainSet plain(sizeOfUniverse);
            for (uint32_t member = first; member < end; ++member)
                plain.add(member);
            pool.append({ maker.ofRange(ValueFlowObjectID(first), ValueFlowObjectID(end)), plain });
            break;
        }
        }
    }
    for (auto& [set, plain] : pool)
        AOT_TEST_CHECK(holdsTheSame(sets, set, plain));

    for (unsigned round = 0; round < 80; ++round) {
        auto [first, firstPlain] = pool[random.getUint32(pool.size())];
        auto [second, secondPlain] = pool[random.getUint32(pool.size())];
        PlainSet united(sizeOfUniverse);
        united.unite(firstPlain);
        united.unite(secondPlain);
        PlainSet shared(sizeOfUniverse);
        shared.unite(firstPlain, &secondPlain);
        PlainSet left(sizeOfUniverse);
        bool isSubset = true;
        for (uint32_t member = 0; member < sizeOfUniverse; ++member) {
            if (firstPlain.contains(member) && !secondPlain.contains(member)) {
                left.add(member);
                isSubset = false;
            }
        }
        ValueSetID unitedSet = maker.unite(first, second);
        ValueSetID sharedSet = maker.intersect(first, second);
        ValueSetID leftSet = maker.intersect(first, ValueSets::complement(second));
        AOT_TEST_CHECK(holdsTheSame(sets, unitedSet, united));
        AOT_TEST_CHECK(holdsTheSame(sets, sharedSet, shared));
        AOT_TEST_CHECK(holdsTheSame(sets, leftSet, left));
        AOT_TEST_CHECK(sets.meet(first, second) == !!shared.size());
        AOT_TEST_CHECK(sets.isSubset(first, second) == isSubset);
        AOT_TEST_CHECK(sets.numberOfCommonMembers(first, second) == shared.size());
        // The same members have one id and one hash, whichever way they were made.
        AOT_TEST_CHECK(maker.unite(sharedSet, leftSet) == first);
        AOT_TEST_CHECK(maker.unite(leftSet, second) == unitedSet);
        Vector<ValueFlowObjectID> members;
        sets.forEachMember(unitedSet, [&](ValueFlowObjectID member) {
            members.append(member);
        });
        AOT_TEST_CHECK(maker.of(members.span()) == unitedSet);
        std::array<ValueSetID, 3> three { leftSet, sharedSet, second };
        AOT_TEST_CHECK(maker.uniteAll(three) == unitedSet);

        Vector<ValueSetID> parts;
        maker.appendParts(unitedSet, parts);
        if (!parts.isEmpty())
            AOT_TEST_CHECK(maker.uniteAll(parts.span()) == unitedSet);
        uint32_t listed = 0;
        uint64_t before = 0;
        bool isAscending = true;
        sets.forEachBlock(unitedSet, [&](uint64_t nameOfBlock) {
            isAscending &= !listed || nameOfBlock >> 32 > before >> 32;
            before = nameOfBlock;
            sets.forEachMemberOfBlock(nameOfBlock, [&](ValueFlowObjectID member) {
                isAscending &= united.contains(member.index()) && member.index() / ValueSets::membersPerBlock == nameOfBlock >> 32;
                ++listed;
            });
        });
        AOT_TEST_CHECK(isAscending && listed == united.size());
        if (pool.size() < 90)
            pool.append({ round % 3 ? unitedSet : round % 2 ? sharedSet : leftSet, round % 3 ? united : round % 2 ? shared : left });
    }
}

static uint64_t mixAsTheSetsDo(uint64_t first, uint64_t second)
{
    uint64_t hash = (first + 0x9e3779b97f4a7c15ull) * 0xbf58476d1ce4e5b9ull ^ second * 0x94d049bb133111ebull;
    hash ^= hash >> 32;
    hash *= 0xd6e8feb86659fd93ull;
    return hash ^ hash >> 29;
}

static Vector<std::pair<uint32_t, uint64_t>> hashesOfBlocksOf(const PlainSet& plain)
{
    Vector<std::pair<uint32_t, uint64_t>> hashes;
    for (uint32_t place = 0; place * ValueSets::membersPerBlock < plain.sizeOfUniverse(); ++place) {
        uint64_t hash = 0;
        bool hasMember = false;
        for (uint32_t word = 0; word < ValueSets::membersPerBlock / 64; ++word) {
            uint64_t bits = plain.wordAt(place * (ValueSets::membersPerBlock / 64) + word);
            if (!bits)
                continue;
            hasMember = true;
            hash += mixAsTheSetsDo(word, bits);
        }
        if (hasMember)
            hashes.append({ place, hash });
    }
    return hashes;
}

static void valueSetsOfBothFormsAgreeWithPlainSets()
{
    constexpr uint32_t sizeOfUniverse = 70 * ValueSets::membersPerBlock + 9;
    WeakRandom random(47);
    ValueSets sets(sizeOfUniverse);
    ValueSetMaker maker(sets);

    auto makeSet = [&](uint32_t numberOfLowMembers, uint32_t numberOfOthers, uint32_t spread) {
        PlainSet plain(sizeOfUniverse);
        Vector<ValueFlowObjectID> members;
        auto add = [&](uint32_t member) {
            members.append(ValueFlowObjectID(member));
            plain.add(member);
        };
        for (; numberOfLowMembers; --numberOfLowMembers)
            add(random.getUint32(64));
        uint32_t first = 64 + random.getUint32(sizeOfUniverse - 64 - spread);
        for (; numberOfOthers; --numberOfOthers)
            add(first + random.getUint32(spread));
        return std::pair { maker.of(members.span()), plain };
    };

    Vector<std::pair<ValueSetID, PlainSet>> pool;
    constexpr uint32_t border = 26;
    for (unsigned index = 0; index < 120; ++index) {
        uint32_t spread = index % 4 == 0 ? 60 : index % 4 == 1 ? 3000 : index % 4 == 2 ? 40000 : sizeOfUniverse - 64;
        uint32_t numberOfOthers = index % 5 == 0 ? random.getUint32(4) : index % 5 == 4 ? 200 + random.getUint32(400) : border / 2 + random.getUint32(border);
        pool.append(makeSet(index % 3 ? random.getUint32(30) : 0, numberOfOthers, spread));
    }

    auto check = [&](ValueSetID set, const PlainSet& plain) {
        AOT_TEST_CHECK(holdsTheSame(sets, set, plain));
        Vector<ValueFlowObjectID> members;
        sets.forEachMember(set, [&](ValueFlowObjectID member) {
            members.append(member);
        });
        AOT_TEST_CHECK(maker.of(members.span()) == set);
        AOT_TEST_CHECK(maker.ofManyMembers(members.span()) == set);

        Vector<std::pair<uint32_t, uint64_t>> blocks = hashesOfBlocksOf(plain);
        uint64_t hash = 0;
        Vector<uint64_t> samples;
        for (auto [place, hashOfBlock] : blocks) {
            hash += mixAsTheSetsDo(place, hashOfBlock);
            uint64_t sample = (hashOfBlock ^ (place + 1) * 0xc2b2ae3d27d4eb4full) * 0x9e3779b97f4a7c15ull;
            samples.append(sample ^ sample >> 32);
        }
        AOT_TEST_CHECK(sets.hashOfMembers(set) == hash);
        std::ranges::sort(samples);
        std::array<uint64_t, 4> sample = sets.sampleOfBlocks(set);
        for (size_t index = 0; index < 4; ++index)
            AOT_TEST_CHECK(sample[index] == (index < samples.size() ? samples[index] : ValueSets::noBlock));

        uint32_t divisor = blocks.size() > 1 ? ValueSets::membersPerBlock : 64;
        Vector<uint32_t> firstOfPart;
        for (ValueFlowObjectID member : members) {
            if (firstOfPart.isEmpty() || firstOfPart.last() / divisor != member.index() / divisor)
                firstOfPart.append(member.index());
        }
        if (members.size() <= 16 || firstOfPart.size() == 1)
            firstOfPart.clear();
        Vector<ValueSetID> parts;
        maker.appendParts(set, parts);
        AOT_TEST_CHECK(parts.size() == firstOfPart.size());
        uint32_t numberInParts = 0;
        for (size_t index = 0; index < parts.size(); ++index) {
            bool liesInPart = true;
            sets.forEachMember(parts[index], [&](ValueFlowObjectID member) {
                liesInPart &= plain.contains(member.index()) && member.index() / divisor == firstOfPart[index] / divisor;
                ++numberInParts;
            });
            AOT_TEST_CHECK(liesInPart);
        }
        AOT_TEST_CHECK(parts.isEmpty() || numberInParts == members.size());

        uint32_t listed = 0;
        size_t numberOfBlocks = 0;
        bool liesInBlock = true;
        sets.forEachBlock(set, [&](uint64_t nameOfBlock) {
            liesInBlock &= numberOfBlocks < blocks.size() && blocks[numberOfBlocks].first == nameOfBlock >> 32;
            ++numberOfBlocks;
            sets.forEachMemberOfBlock(nameOfBlock, [&](ValueFlowObjectID member) {
                liesInBlock &= plain.contains(member.index()) && member.index() / ValueSets::membersPerBlock == nameOfBlock >> 32;
                ++listed;
            });
        });
        AOT_TEST_CHECK(liesInBlock && numberOfBlocks == blocks.size() && listed == members.size());
        uint64_t below64 = 0;
        for (ValueFlowObjectID member : members) {
            if (member.index() < 64)
                below64 |= 1ull << member.index();
        }
        AOT_TEST_CHECK(sets.membersBelow64(set) == below64);
        AOT_TEST_CHECK(sets.membersBelow64(ValueSets::complement(set)) == ~below64);
        AOT_TEST_CHECK(!!sets.onlyMember(set) == (members.size() == 1));
        if (members.size() == 1)
            AOT_TEST_CHECK(sets.onlyMember(set) == members[0]);
    };
    for (auto& [set, plain] : pool)
        check(set, plain);

    // Without optimization all the rounds take minutes, and a test that prints nothing for five is killed.
#ifdef NDEBUG
    constexpr unsigned numberOfRounds = 1500;
#else
    constexpr unsigned numberOfRounds = 300;
#endif
    for (unsigned round = 0; round < numberOfRounds; ++round) {
        auto [first, firstPlain] = pool[random.getUint32(pool.size())];
        auto [second, secondPlain] = pool[random.getUint32(pool.size())];
        auto [third, thirdPlain] = pool[random.getUint32(pool.size())];
        PlainSet united(sizeOfUniverse);
        PlainSet shared(sizeOfUniverse);
        PlainSet left(sizeOfUniverse);
        PlainSet unitedOfThree(sizeOfUniverse);
        bool isSubset = true;
        auto note = [&](uint32_t member) {
            bool inFirst = firstPlain.contains(member);
            bool inSecond = secondPlain.contains(member);
            if (inFirst || inSecond)
                united.add(member);
            if (inFirst && inSecond)
                shared.add(member);
            if (inFirst && !inSecond) {
                left.add(member);
                isSubset = false;
            }
            if (inFirst || inSecond || thirdPlain.contains(member))
                unitedOfThree.add(member);
        };
        for (ValueSetID set : { first, second, third }) {
            sets.forEachMember(set, [&](ValueFlowObjectID member) {
                note(member.index());
            });
        }
        ValueSetID unitedSet = maker.unite(first, second);
        ValueSetID sharedSet = maker.intersect(first, second);
        ValueSetID leftSet = maker.intersect(first, ValueSets::complement(second));
        check(unitedSet, united);
        check(sharedSet, shared);
        check(leftSet, left);
        std::array<ValueSetID, 3> three { first, second, third };
        check(maker.uniteAll(three), unitedOfThree);
        AOT_TEST_CHECK(sets.meet(first, second) == !!shared.size());
        AOT_TEST_CHECK(sets.isSubset(first, second) == isSubset);
        AOT_TEST_CHECK(sets.numberOfCommonMembers(first, second) == shared.size());
        AOT_TEST_CHECK(maker.unite(sharedSet, leftSet) == first);
        AOT_TEST_CHECK(maker.unite(leftSet, second) == unitedSet);
        AOT_TEST_CHECK(maker.intersect(unitedSet, ValueSets::complement(leftSet)) == second);
        if (pool.size() < 400)
            pool.append({ round % 3 ? leftSet : round % 2 ? sharedSet : unitedSet, round % 3 ? left : round % 2 ? shared : united });
    }
}

static void valueSetsAreTheSameFromEveryThread()
{
    constexpr uint32_t sizeOfUniverse = 130 * ValueSets::membersPerBlock;
    constexpr unsigned numberOfThreads = 8;
    constexpr unsigned numberOfSets = 4000;
    ParallelLoops loops(numberOfThreads);
    ValueSets sets(sizeOfUniverse);
    Vector<Vector<ValueSetID>> made(numberOfThreads);
    forEachIndexInParallel(loops, numberOfThreads, [&](unsigned thread) {
        ValueSetMaker maker(sets);
        WeakRandom random(5);
        ValueSetID growing = ValueSets::nothing;
        for (unsigned index = 0; index < numberOfSets; ++index) {
            growing = maker.unite(growing, maker.of(ValueFlowObjectID(random.getUint32(sizeOfUniverse))));
            made[thread].append(index % 7 ? growing : maker.intersect(growing, ValueSets::complement(maker.of(ValueFlowObjectID(random.getUint32(sizeOfUniverse))))));
        }
    });
    for (unsigned thread = 1; thread < numberOfThreads; ++thread)
        AOT_TEST_CHECK(made[thread] == made[0]);
    AOT_TEST_CHECK(sets.size(made[0].last()) > numberOfSets / 2);
}

// The solver finds a set that is like another by a few of its blocks. Threads make blocks in an order that differs from run to run, and what the solver builds must not.
static void sampleOfBlocksDependsOnTheMembersAlone()
{
    constexpr uint32_t sizeOfUniverse = 40 * ValueSets::membersPerBlock;
    constexpr unsigned numberOfSets = 50;
    auto membersOf = [&](unsigned seed) {
        WeakRandom random(seed + 1);
        Vector<ValueFlowObjectID> members;
        for (unsigned count = 0; count < 300; ++count)
            members.append(ValueFlowObjectID(random.getUint32(sizeOfUniverse)));
        return members;
    };

    ValueSets madeForwards(sizeOfUniverse);
    ValueSets madeBackwards(sizeOfUniverse);
    ValueSetMaker forwards(madeForwards);
    ValueSetMaker backwards(madeBackwards);
    Vector<ValueSetID> first(numberOfSets);
    Vector<ValueSetID> second(numberOfSets);
    for (unsigned index = 0; index < numberOfSets; ++index)
        first[index] = forwards.of(membersOf(index).span());
    for (unsigned index = numberOfSets; index--;)
        second[index] = backwards.of(membersOf(index).span());
    for (unsigned index = 0; index < numberOfSets; ++index) {
        AOT_TEST_CHECK(madeForwards.sampleOfBlocks(first[index]) == madeBackwards.sampleOfBlocks(second[index]));
        AOT_TEST_CHECK(madeForwards.sampleOfBlocks(first[index])[3] != ValueSets::noBlock);
    }
    AOT_TEST_CHECK(madeForwards.sampleOfBlocks(first[0]) != madeForwards.sampleOfBlocks(first[1]));
}

static void checkGraphAgainstPlainWorklist(unsigned numberOfThreads, unsigned seed)
{
    constexpr uint32_t sizeOfUniverse = 200;
    WeakRandom random(seed);
    ParallelLoops loops(numberOfThreads);
    ValueSets sets(sizeOfUniverse);
    Vector<std::unique_ptr<ValueSetMaker>> makers(numberOfThreads, [&](size_t) {
        return makeUnique<ValueSetMaker>(sets);
    });
    ValueFlowGraph graph(loops, makers.span());

    Vector<PlainSet> values;
    Vector<std::optional<PlainSet>> masks;
    Vector<bool> isConstant;
    Vector<ValueFlowEdge> allEdges;
    Vector<ValueFlowNodeID> moved;

    unsigned numberOfBatches = 1 + random.getUint32(6);
    for (unsigned batch = 0; batch < numberOfBatches; ++batch) {
        uint32_t numberOfNewNodes = 1 + random.getUint32(batch ? 12 : 70);
        ValueFlowNodeID first = graph.addNodes(numberOfNewNodes);
        for (uint32_t offset = 0; offset < numberOfNewNodes; ++offset) {
            ValueFlowNodeID node(first.index() + offset);
            values.append(PlainSet(sizeOfUniverse));
            masks.append(std::nullopt);
            isConstant.append(false);
            unsigned kind = random.getUint32(10);
            if (kind < 3) {
                auto [set, plain] = makeRandomSet(*makers[0], random, sizeOfUniverse, 4);
                graph.makeConstant(node, set);
                values.last() = plain;
                isConstant.last() = true;
            } else if (kind < 5) {
                auto [set, plain] = makeRandomSet(*makers[0], random, sizeOfUniverse, 120);
                bool isComplement = random.getUint32(2);
                graph.setMask(node, isComplement ? ValueSets::complement(set) : set);
                PlainSet mask(sizeOfUniverse);
                for (uint32_t member = 0; member < sizeOfUniverse; ++member) {
                    if (plain.contains(member) != isComplement)
                        mask.add(member);
                }
                masks.last() = mask;
            }
        }

        Vector<ValueFlowEdge> edges;
        for (uint32_t count = random.getUint32(batch ? 30 : 200); count; --count) {
            ValueFlowEdge edge { ValueFlowNodeID(random.getUint32(values.size())), ValueFlowNodeID(random.getUint32(values.size())) };
            if (isConstant[edge.to.index()])
                continue;
            edges.append(edge);
            allEdges.append(edge);
        }
        if (batch)
            graph.addEdges(edges.span());
        else
            graph.addInitialEdges(edges.span());
        Vector<PlainSet> before = values;
        graph.solve(moved);

        for (bool hasGrown = true; hasGrown;) {
            hasGrown = false;
            for (ValueFlowEdge edge : allEdges)
                hasGrown |= values[edge.to.index()].unite(values[edge.from.index()], masks[edge.to.index()] ? &*masks[edge.to.index()] : nullptr);
        }
        size_t nextMoved = 0;
        for (uint32_t node = 0; node < values.size(); ++node) {
            AOT_TEST_CHECK(holdsTheSame(sets, graph.valueOf(ValueFlowNodeID(node)), values[node]));
            bool hasMoved = nextMoved < moved.size() && moved[nextMoved].index() == node;
            nextMoved += hasMoved;
            AOT_TEST_CHECK(hasMoved == !(before[node] == values[node]));
        }
        AOT_TEST_CHECK(nextMoved == moved.size());
    }
}

static void valueFlowGraphFindsTheLeastSolution()
{
    for (unsigned seed = 1; seed <= 150; ++seed)
        checkGraphAgainstPlainWorklist(seed % 3 ? 1 : 4, seed);
}

namespace {

struct OutcomeOfSolve {
    Vector<uint32_t> moved;
    Vector<Vector<uint32_t>> membersOfNode;

    friend bool operator==(const OutcomeOfSolve&, const OutcomeOfSolve&) = default;
};

} // anonymous namespace

static Vector<OutcomeOfSolve> solveWithEdgesInOrderOf(unsigned seedOfGraph, unsigned seedOfOrder, unsigned numberOfThreads)
{
    constexpr uint32_t sizeOfUniverse = 200;
    WeakRandom random(seedOfGraph);
    WeakRandom order(seedOfOrder);
    ParallelLoops loops(numberOfThreads);
    ValueSets sets(sizeOfUniverse);
    Vector<std::unique_ptr<ValueSetMaker>> makers(numberOfThreads, [&](size_t) {
        return makeUnique<ValueSetMaker>(sets);
    });
    ValueFlowGraph graph(loops, makers.span());

    Vector<bool> isConstant;
    Vector<ValueFlowNodeID> moved;
    Vector<OutcomeOfSolve> outcomes;
    for (unsigned batch = 0; batch < 12; ++batch) {
        uint32_t numberOfNewNodes = 1 + random.getUint32(batch ? 12 : 70);
        ValueFlowNodeID first = graph.addNodes(numberOfNewNodes);
        for (uint32_t offset = 0; offset < numberOfNewNodes; ++offset) {
            ValueFlowNodeID node(first.index() + offset);
            unsigned kind = random.getUint32(10);
            isConstant.append(kind < 3);
            if (kind < 3)
                graph.makeConstant(node, makeRandomSet(*makers[0], random, sizeOfUniverse, 4).first);
            else if (kind < 5)
                graph.setMask(node, makeRandomSet(*makers[0], random, sizeOfUniverse, 120).first);
        }

        Vector<ValueFlowEdge> edges;
        for (uint32_t count = random.getUint32(batch ? 30 : 200); count; --count) {
            ValueFlowEdge edge { ValueFlowNodeID(random.getUint32(isConstant.size())), ValueFlowNodeID(random.getUint32(isConstant.size())) };
            if (!isConstant[edge.to.index()])
                edges.append(edge);
        }
        for (size_t left = edges.size(); left > 1; --left)
            std::swap(edges[left - 1], edges[order.getUint32(left)]);
        if (batch)
            graph.addEdges(edges.span());
        else
            graph.addInitialEdges(edges.span());
        graph.solve(moved);
        OutcomeOfSolve outcome;
        for (ValueFlowNodeID node : moved)
            outcome.moved.append(node.index());
        for (uint32_t node = 0; node < isConstant.size(); ++node) {
            Vector<uint32_t> members;
            sets.forEachMember(graph.valueOf(ValueFlowNodeID(node)), [&](ValueFlowObjectID member) {
                members.append(member.index());
            });
            outcome.membersOfNode.append(WTF::move(members));
        }
        outcomes.append(WTF::move(outcome));
    }
    return outcomes;
}

static void whatTheGraphHoldsDoesNotHangOnTheOrderOfTheEdges()
{
    for (unsigned seed = 1; seed <= 100; ++seed) {
        Vector<OutcomeOfSolve> outcomes = solveWithEdgesInOrderOf(seed, 1, 1);
        for (unsigned order = 2; order <= 5; ++order)
            AOT_TEST_CHECK(solveWithEdgesInOrderOf(seed, order, order % 2 ? 4 : 1) == outcomes);
    }
}

static void rowsOfNodesInsideOneShardAreWalkedInOneWave()
{
    constexpr uint32_t numberOfRows = 2048;
    constexpr uint32_t nodesOfRow = 8;
    for (unsigned numberOfThreads : { 1u, 4u }) {
        ParallelLoops loops(numberOfThreads);
        ValueSets sets(numberOfRows);
        Vector<std::unique_ptr<ValueSetMaker>> makers(numberOfThreads, [&](size_t) {
            return makeUnique<ValueSetMaker>(sets);
        });
        ValueFlowGraph graph(loops, makers.span());
        uint32_t numberOfRowsSolved = numberOfThreads == 1 ? 16 : numberOfRows;
        ValueFlowNodeID firstOfRows = graph.addNodes(numberOfRowsSolved * nodesOfRow);
        ValueFlowNodeID firstConstant = graph.addNodes(numberOfRowsSolved);
        Vector<ValueFlowEdge> edges;
        for (uint32_t row = 0; row < numberOfRowsSolved; ++row) {
            ValueFlowObjectID member(row);
            graph.makeConstant(ValueFlowNodeID(firstConstant.index() + row), makers[0]->of(member));
            uint32_t first = firstOfRows.index() + row * nodesOfRow;
            edges.append(ValueFlowEdge { ValueFlowNodeID(firstConstant.index() + row), ValueFlowNodeID(first) });
            for (uint32_t index = 1; index < nodesOfRow; ++index)
                edges.append(ValueFlowEdge { ValueFlowNodeID(first + index - 1), ValueFlowNodeID(first + index) });
        }
        graph.addInitialEdges(edges.span());
        Vector<ValueFlowNodeID> moved;
        graph.solve(moved);
        AOT_TEST_CHECK(moved.size() == numberOfRowsSolved * nodesOfRow);
        AOT_TEST_CHECK(graph.counters().waves == 1);
        AOT_TEST_CHECK(graph.counters().evaluations == numberOfRowsSolved * nodesOfRow);
        for (uint32_t row = 0; row < numberOfRowsSolved; ++row) {
            ValueFlowObjectID member(row);
            AOT_TEST_CHECK(graph.valueOf(ValueFlowNodeID(firstOfRows.index() + row * nodesOfRow + nodesOfRow - 1)) == makers[0]->of(member));
        }
    }
}

static void masksTreatPlaceholdersByTheirKind()
{
    ParallelLoops loops(1);
    ValueSets sets(16);
    Vector<std::unique_ptr<ValueSetMaker>> makers(1, [&](size_t) {
        return makeUnique<ValueSetMaker>(sets);
    });
    ValueSetMaker& maker = *makers[0];
    auto setOf = [&](std::initializer_list<uint32_t> members) {
        Vector<ValueFlowObjectID> list;
        for (uint32_t member : members)
            list.append(ValueFlowObjectID(member));
        return maker.of(list.span());
    };

    // Member 0 is no object, 1 is the untracked object, 2 the unknown object. 3 to 7 are objects of the program, 8 and above objects of other kinds.
    ValueFlowPlaceholders placeholders { ValueFlowObjectID(1u), ValueFlowObjectID(2u), setOf({ 3, 4, 5, 6, 7 }), ValueSets::nothing };
    ValueSetID ofType = setOf({ 0, 1, 2, 3, 4 });
    ValueSetID family = setOf({ 3, 4 });
    auto pass = [&](ValueSetID value, ValueSetID mask, ValueSetID familyObjects, ValueFlowMaskKind kind) {
        return maker.pass(value, mask, familyObjects, kind, placeholders);
    };

    AOT_TEST_CHECK(pass(setOf({ 0, 1, 2, 3, 5 }), ofType, family, ValueFlowMaskKind::Plain) == setOf({ 0, 1, 2, 3 }));

    AOT_TEST_CHECK(pass(setOf({ 0, 3, 5 }), ofType, family, ValueFlowMaskKind::Checked) == setOf({ 0, 3 }));
    AOT_TEST_CHECK(pass(setOf({ 1, 5 }), ofType, family, ValueFlowMaskKind::Checked) == setOf({ 3, 4 }));
    AOT_TEST_CHECK(pass(setOf({ 2, 9 }), ofType, family, ValueFlowMaskKind::Checked) == setOf({ 3, 4 }));
    AOT_TEST_CHECK(pass(setOf({ 1, 2 }), setOf({ 0, 3 }), family, ValueFlowMaskKind::Checked) == setOf({ 3 }));
    AOT_TEST_CHECK(pass(setOf({ 1, 2, 5, 9 }), setOf({ 1, 2, 8, 9 }), { }, ValueFlowMaskKind::Checked) == setOf({ 1, 2, 9 }));
    AOT_TEST_CHECK(pass(setOf({ 0, 1, 2 }), setOf({ 0 }), { }, ValueFlowMaskKind::Checked) == setOf({ 0 }));

    AOT_TEST_CHECK(pass(setOf({ 0, 5, 9, 10 }), ofType, family, ValueFlowMaskKind::Declared) == setOf({ 0, 5, 9, 10 }));
    AOT_TEST_CHECK(pass(setOf({ 1, 5 }), ofType, family, ValueFlowMaskKind::Declared) == setOf({ 3, 4, 5 }));
    AOT_TEST_CHECK(pass(setOf({ 2, 5 }), ofType, family, ValueFlowMaskKind::Declared) == setOf({ 2, 5 }));
    AOT_TEST_CHECK(pass(setOf({ 1, 2 }), ofType, family, ValueFlowMaskKind::Declared) == setOf({ 2, 3, 4 }));
    AOT_TEST_CHECK(pass(setOf({ 1, 2, 5 }), setOf({ 0 }), { }, ValueFlowMaskKind::Declared) == setOf({ 1, 2, 5 }));

    AOT_TEST_CHECK(pass(setOf({ 1, 5 }), ValueSets::anything, family, ValueFlowMaskKind::Checked) == setOf({ 1, 5 }));
    AOT_TEST_CHECK(maker.pass(setOf({ 1, 5 }), ofType, family, ValueFlowMaskKind::Checked, ValueFlowPlaceholders()) == setOf({ 1 }));
    AOT_TEST_CHECK(maker.pass(setOf({ 1, 5 }), ofType, family, ValueFlowMaskKind::Declared, ValueFlowPlaceholders()) == setOf({ 1, 5 }));

    // A placeholder is replaced only by the family objects that it covers: the graph passes nothing, the rules pass what has escaped and what is lost.
    auto passCovered = [&](ValueSetID value, ValueFlowMaskKind kind, ValueSetID byUntracked, ValueSetID byUnknown) {
        return maker.pass(value, ofType, family, kind, placeholders, ValueFlowCoveredObjects { byUntracked, byUnknown });
    };
    AOT_TEST_CHECK(passCovered(setOf({ 1, 5 }), ValueFlowMaskKind::Checked, setOf({ 4, 6 }), ValueSets::nothing) == setOf({ 4 }));
    AOT_TEST_CHECK(passCovered(setOf({ 2, 3 }), ValueFlowMaskKind::Checked, ValueSets::nothing, setOf({ 4 })) == setOf({ 3, 4 }));
    AOT_TEST_CHECK(passCovered(setOf({ 1, 2 }), ValueFlowMaskKind::Checked, setOf({ 3 }), setOf({ 4 })) == setOf({ 3, 4 }));
    AOT_TEST_CHECK(passCovered(setOf({ 2 }), ValueFlowMaskKind::Checked, setOf({ 3 }), setOf({ 4 })) == setOf({ 4 }));
    AOT_TEST_CHECK(passCovered(setOf({ 1, 2, 3 }), ValueFlowMaskKind::Checked, ValueSets::nothing, ValueSets::nothing) == setOf({ 3 }));
    for (ValueFlowMaskKind kind : { ValueFlowMaskKind::Declared, ValueFlowMaskKind::Expected }) {
        AOT_TEST_CHECK(passCovered(setOf({ 1, 5 }), kind, setOf({ 4, 6 }), ValueSets::anything) == setOf({ 4, 5 }));
        AOT_TEST_CHECK(passCovered(setOf({ 1, 5 }), kind, ValueSets::nothing, ValueSets::anything) == setOf({ 5 }));
        AOT_TEST_CHECK(passCovered(setOf({ 1, 2, 5 }), kind, setOf({ 3 }), ValueSets::nothing) == setOf({ 2, 3, 5 }));
    }

    // It makes no difference in how many parts a value arrives.
    for (ValueFlowMaskKind kind : { ValueFlowMaskKind::Plain, ValueFlowMaskKind::Checked, ValueFlowMaskKind::Declared, ValueFlowMaskKind::Expected })
        AOT_TEST_CHECK(maker.unite(pass(setOf({ 1 }), ofType, family, kind), pass(setOf({ 2, 5 }), ofType, family, kind)) == pass(setOf({ 1, 2, 5 }), ofType, family, kind));

    // followed -> declared <-> copy, followed -> checked, followed -> plain. The placeholders come with edges that are added after the first solve.
    ValueFlowGraph graph(loops, makers.span());
    graph.setPlaceholders(placeholders);
    ValueFlowNodeID followed = graph.addNodes(7);
    ValueFlowNodeID untracked(followed.index() + 1);
    ValueFlowNodeID unknown(followed.index() + 2);
    ValueFlowNodeID declared(followed.index() + 3);
    ValueFlowNodeID copy(followed.index() + 4);
    ValueFlowNodeID checked(followed.index() + 5);
    ValueFlowNodeID plain(followed.index() + 6);
    graph.makeConstant(followed, setOf({ 3, 5 }));
    graph.makeConstant(untracked, setOf({ 1 }));
    graph.makeConstant(unknown, setOf({ 2 }));
    graph.setMask(declared, ofType, ValueFlowMaskKind::Declared, family);
    graph.setMask(checked, ofType, ValueFlowMaskKind::Checked, family);
    graph.setMask(plain, ofType);
    AOT_TEST_CHECK(graph.maskOf(declared) == ofType && graph.maskKindOf(declared) == ValueFlowMaskKind::Declared && graph.familyObjectsOf(declared) == family);
    AOT_TEST_CHECK(graph.maskOf(plain) == ofType && graph.maskKindOf(plain) == ValueFlowMaskKind::Plain && !graph.familyObjectsOf(plain));
    AOT_TEST_CHECK(graph.maskOf(copy) == ValueSets::anything);
    ValueFlowEdge edges[] = { { followed, declared }, { declared, copy }, { copy, declared }, { followed, checked }, { followed, plain } };
    graph.addInitialEdges(edges);
    Vector<ValueFlowNodeID> moved;
    graph.solve(moved);
    AOT_TEST_CHECK(graph.valueOf(declared) == setOf({ 3, 5 }));
    AOT_TEST_CHECK(graph.valueOf(copy) == setOf({ 3, 5 }));
    AOT_TEST_CHECK(graph.valueOf(checked) == setOf({ 3 }));
    AOT_TEST_CHECK(graph.valueOf(plain) == setOf({ 3 }));

    ValueFlowEdge late[] = { { untracked, declared }, { untracked, checked }, { untracked, plain } };
    AOT_TEST_CHECK(graph.addEdges(late) == 3);
    graph.solve(moved);
    AOT_TEST_CHECK(graph.valueOf(declared) == setOf({ 3, 4, 5 }));
    AOT_TEST_CHECK(graph.valueOf(copy) == setOf({ 3, 4, 5 }));
    AOT_TEST_CHECK(graph.valueOf(checked) == setOf({ 3, 4 }));
    AOT_TEST_CHECK(graph.valueOf(plain) == setOf({ 1, 3 }));

    ValueFlowEdge later[] = { { unknown, declared }, { unknown, checked } };
    AOT_TEST_CHECK(graph.addEdges(later) == 2);
    graph.solve(moved);
    AOT_TEST_CHECK(graph.valueOf(declared) == setOf({ 2, 3, 4, 5 }));
    AOT_TEST_CHECK(graph.valueOf(copy) == setOf({ 2, 3, 4, 5 }));
    AOT_TEST_CHECK(graph.valueOf(checked) == setOf({ 3, 4 }));
}

static void nodesThatPlaceholdersCoverAreRetired()
{
    ParallelLoops loops(1);
    ValueSets sets(16);
    Vector<std::unique_ptr<ValueSetMaker>> makers(1, [&](size_t) {
        return makeUnique<ValueSetMaker>(sets);
    });
    ValueSetMaker& maker = *makers[0];
    auto setOf = [&](std::initializer_list<uint32_t> members) {
        Vector<ValueFlowObjectID> list;
        for (uint32_t member : members)
            list.append(ValueFlowObjectID(member));
        return maker.of(list.span());
    };

    // Member 0 is no object, 1 is the untracked object, 2 the unknown object, 3 to 9 are objects.
    ValueFlowObjectID untrackedObject(1u);
    ValueFlowObjectID unknownObject(2u);
    ValueFlowGraph graph(loops, makers.span());
    ValueFlowNodeID escaped = graph.addNodes(14);
    auto at = [&](uint32_t offset) {
        return ValueFlowNodeID(escaped.index() + offset);
    };
    ValueFlowNodeID lost = at(1);
    ValueFlowNodeID object3 = at(2);
    ValueFlowNodeID unknownValue = at(3);
    ValueFlowNodeID handedOut = at(4);
    ValueFlowNodeID readerOfHandedOut = at(5);
    ValueFlowNodeID object4 = at(6);
    ValueFlowNodeID object5 = at(7);
    ValueFlowNodeID untrackedAlone = at(8);
    ValueFlowNodeID givenUp = at(9);
    ValueFlowNodeID readerOfGivenUp = at(10);
    ValueFlowNodeID object6 = at(11);
    ValueFlowNodeID primitive = at(12);
    ValueFlowNodeID unknownAlone = at(13);
    graph.retireNodesCoveredBy(unknownObject, escaped, setOf({ 0, 2 }), ValueSets::anything);
    graph.retireNodesCoveredBy(untrackedObject, lost, setOf({ 1 }), setOf({ 3, 4, 5, 6, 7, 8, 9 }));
    graph.makeConstant(object3, setOf({ 3 }));
    graph.makeConstant(unknownValue, setOf({ 0, 2 }));
    graph.makeConstant(object4, setOf({ 4 }));
    graph.makeConstant(object5, setOf({ 5 }));
    graph.makeConstant(untrackedAlone, setOf({ 1 }));
    graph.makeConstant(object6, setOf({ 6 }));
    graph.makeConstant(primitive, setOf({ 0 }));
    graph.makeConstant(unknownAlone, setOf({ 2 }));

    ValueFlowEdge edges[] = {
        { object3, handedOut }, { unknownValue, handedOut }, { handedOut, escaped }, { handedOut, readerOfHandedOut },
        { object5, givenUp }, { untrackedAlone, givenUp }, { givenUp, lost }, { givenUp, readerOfGivenUp },
    };
    graph.addInitialEdges(edges);
    Vector<ValueFlowNodeID> moved;
    graph.solve(moved);
    AOT_TEST_CHECK(graph.valueOf(handedOut) == setOf({ 0, 2, 3 }));
    AOT_TEST_CHECK(graph.valueOf(escaped) == setOf({ 0, 2, 3 }));
    AOT_TEST_CHECK(graph.valueOf(givenUp) == setOf({ 1, 5 }));
    AOT_TEST_CHECK(graph.valueOf(lost) == setOf({ 1, 5 }));
    AOT_TEST_CHECK(graph.isRetired(handedOut) && graph.isRetired(givenUp));
    AOT_TEST_CHECK(!graph.isRetired(escaped) && !graph.isRetired(lost) && !graph.isRetired(readerOfHandedOut) && !graph.isRetired(readerOfGivenUp) && !graph.isRetired(object3));
    AOT_TEST_CHECK(!graph.isAtRest(handedOut) && !graph.isAtRest(givenUp));
    AOT_TEST_CHECK(graph.flowsIntoNodeCoveredBy(unknownObject, object3) && !graph.flowsIntoNodeCoveredBy(unknownObject, readerOfHandedOut) && !graph.flowsIntoNodeCoveredBy(unknownObject, givenUp));
    AOT_TEST_CHECK(graph.flowsIntoNodeCoveredBy(untrackedObject, object5) && !graph.flowsIntoNodeCoveredBy(untrackedObject, handedOut));

    // Whatever comes later goes straight to the covered node. The node that holds the whole unknown value takes none of it; the one that holds the untracked object takes what is no object that flow follows.
    ValueFlowEdge late[] = { { object4, handedOut }, { object6, givenUp }, { primitive, givenUp }, { unknownAlone, givenUp } };
    AOT_TEST_CHECK(graph.addEdges(late) == 4);
    graph.solve(moved);
    AOT_TEST_CHECK(graph.valueOf(handedOut) == setOf({ 0, 2, 3 }));
    AOT_TEST_CHECK(graph.valueOf(readerOfHandedOut) == setOf({ 0, 2, 3 }));
    AOT_TEST_CHECK(graph.valueOf(escaped) == setOf({ 0, 2, 3, 4 }));
    AOT_TEST_CHECK(graph.valueOf(givenUp) == setOf({ 0, 1, 2, 5 }));
    AOT_TEST_CHECK(graph.valueOf(readerOfGivenUp) == setOf({ 0, 1, 2, 5 }));
    AOT_TEST_CHECK(graph.valueOf(lost) == setOf({ 0, 1, 2, 5, 6 }));
    AOT_TEST_CHECK(graph.hasAdded(ValueFlowEdge { object4, escaped }) && graph.hasAdded(ValueFlowEdge { object6, lost }));
}

static void theCallerMayConnectWhatLeadsIntoRetiredNodes()
{
    constexpr uint32_t numberOfSources = 70000;
    for (unsigned width : { 1u, 4u }) {
        ParallelLoops loops(width);
        ValueSets sets(16);
        Vector<std::unique_ptr<ValueSetMaker>> makers(width, [&](size_t) {
            return makeUnique<ValueSetMaker>(sets);
        });
        ValueSetMaker& maker = *makers[0];
        auto setOf = [&](std::initializer_list<uint32_t> members) {
            Vector<ValueFlowObjectID> list;
            for (uint32_t member : members)
                list.append(ValueFlowObjectID(member));
            return maker.of(list.span());
        };
        for (ValueFlowGraph::RetiredNodes retiredNodes : { ValueFlowGraph::RetiredNodes::AreFoundAndConnectedInSolve, ValueFlowGraph::RetiredNodes::AreFoundAndConnectedByCaller }) {
            ValueFlowGraph graph(loops, makers.span());
            ValueFlowNodeID escaped = graph.addNodes(4 + numberOfSources);
            auto at = [&](uint32_t offset) {
                return ValueFlowNodeID(escaped.index() + offset);
            };
            ValueFlowNodeID unknownValue = at(1);
            ValueFlowNodeID handedOut = at(2);
            ValueFlowNodeID late = at(3);
            graph.retireNodesCoveredBy(ValueFlowObjectID(2u), escaped, setOf({ 0, 2 }), ValueSets::anything);
            graph.makeConstant(unknownValue, setOf({ 0, 2 }));
            graph.makeConstant(late, setOf({ 4 }));
            Vector<ValueFlowEdge> edges { { unknownValue, handedOut }, { handedOut, escaped }, { late, at(4) } };
            for (uint32_t source = 0; source < numberOfSources; ++source)
                edges.append(ValueFlowEdge { at(4 + source), handedOut });
            graph.addInitialEdges(edges.span());
            Vector<ValueFlowNodeID> moved;
            graph.solve(moved, retiredNodes);
            bool isLeftToCaller = retiredNodes == ValueFlowGraph::RetiredNodes::AreFoundAndConnectedByCaller;
            AOT_TEST_CHECK(graph.isRetired(handedOut) == !isLeftToCaller);
            AOT_TEST_CHECK(graph.hasAdded(ValueFlowEdge { at(4 + numberOfSources - 1), escaped }) == !isLeftToCaller);
            if (isLeftToCaller)
                graph.retireNodesAmong(moved.span());
            AOT_TEST_CHECK(graph.isRetired(handedOut));
            AOT_TEST_CHECK(!graph.hasNodesJustRetired());
            AOT_TEST_CHECK(graph.hasAdded(ValueFlowEdge { at(4), escaped }) && graph.hasAdded(ValueFlowEdge { at(4 + numberOfSources - 1), escaped }));
            size_t numberOfEdgesBefore = graph.numberOfEdges();
            graph.solve(moved, retiredNodes);
            AOT_TEST_CHECK(graph.numberOfEdges() >= numberOfEdgesBefore + numberOfSources);
            AOT_TEST_CHECK(graph.valueOf(escaped) == setOf({ 0, 2, 4 }));
            AOT_TEST_CHECK(graph.valueOf(handedOut) == setOf({ 0, 2, 4 }));
        }
    }
}

static void whatACoveredNodeHoldsIsNotSentToItAgain()
{
    ParallelLoops loops(1);
    ValueSets sets(16);
    Vector<std::unique_ptr<ValueSetMaker>> makers(1, [&](size_t) {
        return makeUnique<ValueSetMaker>(sets);
    });
    ValueSetMaker& maker = *makers[0];
    auto setOf = [&](std::initializer_list<uint32_t> members) {
        Vector<ValueFlowObjectID> list;
        for (uint32_t member : members)
            list.append(ValueFlowObjectID(member));
        return maker.of(list.span());
    };

    // Member 0 is no object, 2 is the unknown object, 3 and 4 are objects.
    ValueFlowGraph graph(loops, makers.span());
    ValueFlowNodeID escaped = graph.addNodes(6);
    auto at = [&](uint32_t offset) {
        return ValueFlowNodeID(escaped.index() + offset);
    };
    ValueFlowNodeID reader = at(1);
    ValueFlowNodeID object3 = at(2);
    ValueFlowNodeID object3Again = at(3);
    ValueFlowNodeID passingOn = at(4);
    ValueFlowNodeID objects3And4 = at(5);
    graph.retireNodesCoveredBy(ValueFlowObjectID(2u), escaped, setOf({ 0, 2 }), ValueSets::anything);
    graph.makeConstant(object3, setOf({ 3 }));
    graph.makeConstant(object3Again, setOf({ 3 }));
    graph.makeConstant(objects3And4, setOf({ 3, 4 }));

    ValueFlowEdge edges[] = { { object3, escaped }, { escaped, reader } };
    graph.addInitialEdges(edges);
    Vector<ValueFlowNodeID> moved;
    graph.solve(moved);
    AOT_TEST_CHECK(graph.valueOf(escaped) == setOf({ 3 }) && graph.valueOf(reader) == setOf({ 3 }));

    // A new edge that brings what is there wakes nobody.
    uint64_t evaluations = graph.counters().evaluations;
    uint64_t waves = graph.counters().waves;
    ValueFlowEdge bringingTheSame { object3Again, escaped };
    AOT_TEST_CHECK(graph.addEdges(singleElementSpan(bringingTheSame)) == 1);
    graph.solve(moved);
    AOT_TEST_CHECK(moved.isEmpty());
    AOT_TEST_CHECK(graph.counters().evaluations == evaluations && graph.counters().waves == waves);
    AOT_TEST_CHECK(graph.valueOf(escaped) == setOf({ 3 }));

    // Nor does a node that moves and has no more to send: it alone is evaluated.
    ValueFlowEdge throughAnotherNode[] = { { object3Again, passingOn }, { passingOn, escaped } };
    AOT_TEST_CHECK(graph.addEdges(throughAnotherNode) == 2);
    graph.solve(moved);
    AOT_TEST_CHECK(moved.size() == 1 && moved[0] == passingOn);
    AOT_TEST_CHECK(graph.counters().evaluations == evaluations + 1);

    // Whatever is new arrives, and goes on from there.
    ValueFlowEdge bringingMore { objects3And4, passingOn };
    AOT_TEST_CHECK(graph.addEdges(singleElementSpan(bringingMore)) == 1);
    graph.solve(moved);
    AOT_TEST_CHECK(graph.valueOf(passingOn) == setOf({ 3, 4 }) && graph.valueOf(escaped) == setOf({ 3, 4 }) && graph.valueOf(reader) == setOf({ 3, 4 }));
    AOT_TEST_CHECK(moved.size() == 3);
}

static void valueFlowGraphSolvesLongChainsAndWideLevelsOnThreads()
{
    constexpr uint32_t width = 3000;
    constexpr uint32_t depth = 40;
    ParallelLoops loops(8);
    ValueSets sets(width + 1);
    Vector<std::unique_ptr<ValueSetMaker>> makers(8, [&](size_t) {
        return makeUnique<ValueSetMaker>(sets);
    });
    ValueFlowGraph graph(loops, makers.span());
    ValueFlowNodeID constants = graph.addNodes(width);
    ValueFlowNodeID grid = graph.addNodes(width * depth);
    Vector<ValueFlowEdge> edges;
    for (uint32_t column = 0; column < width; ++column) {
        graph.makeConstant(ValueFlowNodeID(constants.index() + column), makers[0]->of(ValueFlowObjectID(column)));
        edges.append(ValueFlowEdge { ValueFlowNodeID(constants.index() + column), ValueFlowNodeID(grid.index() + column) });
        for (uint32_t row = 1; row < depth; ++row) {
            edges.append(ValueFlowEdge { ValueFlowNodeID(grid.index() + (row - 1) * width + column), ValueFlowNodeID(grid.index() + row * width + column) });
            edges.append(ValueFlowEdge { ValueFlowNodeID(grid.index() + (row - 1) * width + (column + 1) % width), ValueFlowNodeID(grid.index() + row * width + column) });
        }
    }
    graph.addInitialEdges(edges.span());
    Vector<ValueFlowNodeID> moved;
    graph.solve(moved);
    AOT_TEST_CHECK(moved.size() == width * depth);
    for (uint32_t row = 0; row < depth; ++row) {
        for (uint32_t column = 0; column < width; column += 97) {
            ValueSetID value = graph.valueOf(ValueFlowNodeID(grid.index() + row * width + column));
            AOT_TEST_CHECK(sets.size(value) == row + 1);
            AOT_TEST_CHECK(sets.contains(value, ValueFlowObjectID((column + row) % width)));
        }
    }

    // An edge from the last row back to the first closes a cycle through everything.
    ValueFlowEdge back { ValueFlowNodeID(grid.index() + (depth - 1) * width), ValueFlowNodeID(grid.index()) };
    AOT_TEST_CHECK(graph.addEdges(singleElementSpan(back)) == 1);
    AOT_TEST_CHECK(!graph.addEdges(singleElementSpan(back)));
    graph.solve(moved);
    AOT_TEST_CHECK(sets.size(graph.valueOf(ValueFlowNodeID(grid.index()))) == depth);
}

// A small language for the rounds: objects have properties, the first objects are closures of functions with one parameter and one result.
namespace {

enum class ToyOperation : uint32_t { Allocate, Load, Store, Call, TakeStoredInto, TakeFunctions, LoadEveryProperty, StoreUnderUnknownKey };

struct ToyProgram {
    uint32_t numberOfFunctions { 0 };
    uint32_t numberOfObjects { 0 };
    uint32_t numberOfKeys { 0 };
    uint32_t nodesPerFunction { 0 };
    Vector<ValueFlowEdge> edges;
    Vector<ValueFlowStatement> statements;
    Vector<ValueFlowOperand> operands;
    Vector<ValueFlowBodyRange> ranges;
    Vector<uint32_t> payloads;

    ValueFlowNodeID node(uint32_t function, uint32_t index) const { return ValueFlowNodeID(function * nodesPerFunction + index); }
    ValueFlowNodeID parameter(uint32_t function) const { return node(function, 0); }
    ValueFlowNodeID result(uint32_t function) const { return node(function, 1); }
    uint32_t numberOfNodes() const { return (numberOfFunctions + 1) * nodesPerFunction; }
};

const ToyProgram* toyProgram;
ValueSetID toyFunctions;

} // anonymous namespace

static void runToyRule(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    const ToyProgram& program = *toyProgram;
    if (statement.kind == ValueFlowStatementKind::CallGroup) {
        ValueFlowCallGroup group = context.group(context.setOfKey(), context.callKindOfKey());
        context.forEachMember(context.setOfKey(), [&](ValueFlowObjectID callee) {
            if (callee.index() >= program.numberOfFunctions)
                return;
            context.reach(FunctionID(callee.index()));
            context.connect(group.argument(0), program.parameter(callee.index()));
            context.connect(program.result(callee.index()), group.result);
        });
        return;
    }

    std::span<const ValueFlowOperand> operands = context.operandsOf(statement);
    uint32_t payload = program.payloads[context.statement().index()];
    switch (static_cast<ToyOperation>(statement.site.index())) {
    case ToyOperation::Allocate:
        context.add(statement.result, context.maker().of(ValueFlowObjectID(payload)));
        return;
    case ToyOperation::Load:
        context.connect(context.readWhatWasWritten(context.valueOf(operands[0]), ValueFlowName::property(PropertyKeyID(payload))), statement.result);
        return;
    case ToyOperation::LoadEveryProperty:
        context.connect(context.readWhatWasWritten(context.valueOf(operands[0]), ValueFlowName { ValueFlowNameKind::EveryProperty, 0 }), statement.result);
        return;
    case ToyOperation::StoreUnderUnknownKey:
        context.connect(operands[1], context.written(context.valueOf(operands[0]), ValueFlowName { ValueFlowNameKind::UnknownKey, 0 }));
        return;
    case ToyOperation::TakeStoredInto:
        // The operand's objects that something has been stored into.
        context.add(statement.result, context.markedAmong(context.valueOf(operands[0]), ObjectMutation::StoreAfterCreation));
        return;
    case ToyOperation::TakeFunctions:
        context.add(statement.result, context.valueWithin(ClimbingSet(toyFunctions), operands[0].node));
        return;
    case ToyOperation::Store:
        context.connect(operands[1], context.written(context.valueOf(operands[0]), ValueFlowName::property(PropertyKeyID(payload))));
        context.mark(context.valueOf(operands[0]), ObjectMutation::StoreAfterCreation);
        return;
    case ToyOperation::Call: {
        ValueFlowCallGroup group = context.group(context.valueOf(operands[0]), ValueFlowCallKind::Call);
        context.connect(operands[1], group.argument(0));
        context.connect(group.result, statement.result);
        return;
    }
    }
}

static void makeToyFunctions(ValueSets& sets, const ToyProgram& program)
{
    ValueSetMaker maker(sets);
    toyFunctions = maker.ofRange(ValueFlowObjectID(0u), ValueFlowObjectID(program.numberOfFunctions));
}

static ToyProgram makeToyProgram(WeakRandom& random)
{
    ToyProgram program;
    program.numberOfFunctions = 2 + random.getUint32(12);
    program.numberOfObjects = program.numberOfFunctions + 1 + random.getUint32(20);
    program.numberOfKeys = 1 + random.getUint32(4);
    program.nodesPerFunction = 4 + random.getUint32(8);
    for (uint32_t body = 0; body <= program.numberOfFunctions; ++body) {
        auto anyNode = [&] {
            return program.node(body, random.getUint32(program.nodesPerFunction));
        };
        for (uint32_t count = random.getUint32(program.nodesPerFunction); count; --count)
            program.edges.append(ValueFlowEdge { anyNode(), anyNode() });

        ValueFlowBodyRange range;
        range.statement = program.statements.size();
        auto addStatement = [&](ToyOperation operation, uint32_t payload, unsigned numberOfOperands) {
            ValueFlowStatement statement;
            statement.function = FunctionID(body);
            statement.site = ValueID(static_cast<uint32_t>(operation));
            statement.firstOperand = program.operands.size();
            statement.operandCount = numberOfOperands;
            statement.result = anyNode();
            for (unsigned operand = 0; operand < numberOfOperands; ++operand)
                program.operands.append(ValueFlowOperand { anyNode(), { }, { } });
            program.statements.append(statement);
            program.payloads.append(payload);
        };
        for (uint32_t count = random.getUint32(4); count; --count)
            addStatement(ToyOperation::Load, random.getUint32(program.numberOfKeys), 1);
        if (!random.getUint32(3))
            addStatement(ToyOperation::LoadEveryProperty, 0, 1);
        if (!random.getUint32(3))
            addStatement(ToyOperation::TakeStoredInto, 0, 1);
        if (!random.getUint32(3))
            addStatement(ToyOperation::TakeFunctions, 0, 1);
        range.statementLeavingBody = program.statements.size();
        for (uint32_t count = 1 + random.getUint32(6); count; --count) {
            switch (random.getUint32(4)) {
            case 0:
                addStatement(ToyOperation::Allocate, random.getUint32(program.numberOfObjects), 0);
                break;
            case 2:
                addStatement(random.getUint32(3) ? ToyOperation::Allocate : ToyOperation::StoreUnderUnknownKey, random.getUint32(program.numberOfObjects), 2);
                break;
            case 1:
                addStatement(ToyOperation::Store, random.getUint32(program.numberOfKeys), 2);
                break;
            default:
                addStatement(ToyOperation::Call, 0, 2);
                break;
            }
        }
        program.ranges.append(range);
    }
    ValueFlowBodyRange end;
    end.statement = end.statementLeavingBody = program.statements.size();
    program.ranges.append(end);
    return program;
}

namespace {

struct ToySolution {
    Vector<PlainSet> values;
    Vector<bool> isReached;
    Vector<bool> isStoredInto;
};

} // anonymous namespace

// One set per (object, key), one step at a time, until nothing moves.
static ToySolution solveToyProgramObjectByObject(const ToyProgram& program)
{
    ToySolution solution;
    solution.values.fill(PlainSet(program.numberOfObjects), program.numberOfNodes());
    solution.isReached.fill(false, program.numberOfFunctions + 1);
    solution.isReached[program.numberOfFunctions] = true;
    solution.isStoredInto.fill(false, program.numberOfObjects);
    Vector<PlainSet> fields;
    fields.fill(PlainSet(program.numberOfObjects), program.numberOfObjects * program.numberOfKeys);
    Vector<PlainSet> storedUnderUnknownKey;
    storedUnderUnknownKey.fill(PlainSet(program.numberOfObjects), program.numberOfObjects);

    auto& values = solution.values;
    for (bool hasMoved = true; hasMoved;) {
        hasMoved = false;
        for (ValueFlowEdge edge : program.edges)
            hasMoved |= values[edge.to.index()].unite(values[edge.from.index()]);
        for (uint32_t body = 0; body <= program.numberOfFunctions; ++body) {
            uint32_t end = solution.isReached[body] ? program.ranges[body + 1].statement : program.ranges[body].statementLeavingBody;
            for (uint32_t index = program.ranges[body].statement; index < end; ++index) {
                const ValueFlowStatement& statement = program.statements[index];
                auto operand = [&](unsigned number) -> PlainSet& {
                    return values[program.operands[statement.firstOperand + number].node.index()];
                };
                PlainSet& result = values[statement.result.index()];
                uint32_t payload = program.payloads[index];
                switch (static_cast<ToyOperation>(statement.site.index())) {
                case ToyOperation::Allocate:
                    hasMoved |= result.add(payload);
                    break;
                case ToyOperation::Load:
                    for (uint32_t object = 0; object < program.numberOfObjects; ++object) {
                        if (!operand(0).contains(object))
                            continue;
                        hasMoved |= result.unite(fields[object * program.numberOfKeys + payload]);
                        hasMoved |= result.unite(storedUnderUnknownKey[object]);
                    }
                    break;
                case ToyOperation::LoadEveryProperty:
                    for (uint32_t object = 0; object < program.numberOfObjects; ++object) {
                        if (!operand(0).contains(object))
                            continue;
                        for (uint32_t key = 0; key < program.numberOfKeys; ++key)
                            hasMoved |= result.unite(fields[object * program.numberOfKeys + key]);
                        hasMoved |= result.unite(storedUnderUnknownKey[object]);
                    }
                    break;
                case ToyOperation::StoreUnderUnknownKey:
                    for (uint32_t object = 0; object < program.numberOfObjects; ++object) {
                        if (operand(0).contains(object))
                            hasMoved |= storedUnderUnknownKey[object].unite(operand(1));
                    }
                    break;
                case ToyOperation::TakeStoredInto:
                    for (uint32_t object = 0; object < program.numberOfObjects; ++object) {
                        if (operand(0).contains(object) && solution.isStoredInto[object])
                            hasMoved |= result.add(object);
                    }
                    break;
                case ToyOperation::TakeFunctions:
                    for (uint32_t object = 0; object < program.numberOfFunctions; ++object) {
                        if (operand(0).contains(object))
                            hasMoved |= result.add(object);
                    }
                    break;
                case ToyOperation::Store:
                    for (uint32_t object = 0; object < program.numberOfObjects; ++object) {
                        if (!operand(0).contains(object))
                            continue;
                        hasMoved |= fields[object * program.numberOfKeys + payload].unite(operand(1));
                        hasMoved |= !std::exchange(solution.isStoredInto[object], true);
                    }
                    break;
                case ToyOperation::Call:
                    for (uint32_t callee = 0; callee < program.numberOfFunctions; ++callee) {
                        if (!operand(0).contains(callee))
                            continue;
                        hasMoved |= !std::exchange(solution.isReached[callee], true);
                        hasMoved |= values[program.parameter(callee).index()].unite(operand(1));
                        hasMoved |= result.unite(values[program.result(callee).index()]);
                    }
                    break;
                }
            }
        }
    }
    return solution;
}

static void valueFlowRoundsAgreeWithAnAnalysisObjectByObject()
{
    for (unsigned seed = 1; seed <= 120; ++seed) {
        WeakRandom random(seed);
        ToyProgram program = makeToyProgram(random);
        toyProgram = &program;
        ToySolution expected = solveToyProgramObjectByObject(program);

        ValueFlowSolverInput input;
        input.numberOfNodes = program.numberOfNodes();
        input.edges = program.edges.span();
        input.statements = program.statements.span();
        input.operands = program.operands.span();
        input.ranges = program.ranges.span();
        input.remembersWhoLoggedEachEdge = true;

        struct Way {
            unsigned numberOfThreads;
            ValueFlowSolver::Rounds rounds;
        };
        for (Way way : { Way { 1, ValueFlowSolver::Rounds::Due }, Way { 4, ValueFlowSolver::Rounds::Due }, Way { 3, ValueFlowSolver::Rounds::Full } }) {
            ParallelLoops loops(way.numberOfThreads);
            ValueSets sets(program.numberOfObjects);
            makeToyFunctions(sets, program);
            ValueFlowSolver solver(input, sets, runToyRule, loops, way.rounds);
            solver.solve();
            for (uint32_t node = 0; node < program.numberOfNodes(); ++node)
                AOT_TEST_CHECK(holdsTheSame(sets, solver.possibleValuesOf(ValueFlowNodeID(node)), expected.values[node]));
            for (uint32_t function = 0; function < program.numberOfFunctions; ++function)
                AOT_TEST_CHECK(solver.isReached(FunctionID(function)) == expected.isReached[function]);
            for (uint32_t object = 0; object < program.numberOfObjects; ++object)
                AOT_TEST_CHECK(solver.marksOf(ValueFlowObjectID(object)).contains(ObjectMutation::StoreAfterCreation) == expected.isStoredInto[object]);
            AOT_TEST_CHECK(solver.counters().fullRounds >= 1);

            // Every function is reached by a call group that holds its closure, and every member of a set can be followed back to where it was made.
            for (uint32_t function = 0; function < program.numberOfFunctions; ++function) {
                if (!solver.isReached(FunctionID(function)))
                    continue;
                ValueFlowStatementID witness = solver.reachedBy(FunctionID(function)).statement;
                AOT_TEST_CHECK(witness && solver.statementAt(witness).kind == ValueFlowStatementKind::CallGroup);
                AOT_TEST_CHECK(sets.contains(solver.keyOf(witness).set, ValueFlowObjectID(function)));
            }
            for (uint32_t node = 0; node < program.numberOfNodes(); node += 3) {
                sets.forEachMember(solver.possibleValuesOf(ValueFlowNodeID(node)), [&](ValueFlowObjectID member) {
                    Vector<ValueFlowNodeID> way = solver.explain(ValueFlowNodeID(node), member);
                    AOT_TEST_CHECK(way.size() >= 2 && way[0] == ValueFlowNodeID(node));
                    for (ValueFlowNodeID step : way)
                        AOT_TEST_CHECK(sets.contains(solver.possibleValuesOf(step), member));
                    const ValueFlowNodeKey* key = solver.keyOfMadeNode(way.last());
                    AOT_TEST_CHECK(key && key->kind == ValueFlowMadeNodeKind::Constant);
                    ValueFlowStatementID allocation = solver.statementThatLogged(ValueFlowEdge { way.last(), way[way.size() - 2] });
                    AOT_TEST_CHECK(allocation && !solver.isMadeByRule(allocation));
                });
            }
            uint64_t runs = 0;
            uint64_t edgesLogged = 0;
            solver.forEachKindOfStatementRun([&](ValueFlowStatementKind, Opcode, const ValueFlowSolver::RunsAndEdges& counted) {
                runs += counted.runs;
                edgesLogged += counted.edgesLogged;
            });
            AOT_TEST_CHECK(runs == solver.counters().statementRuns && edgesLogged == solver.counters().edgesLogged);
        }
    }
    toyProgram = nullptr;
}

static void whatATurnMakesAndReachesRunsInThatTurn()
{
    ToyProgram program;
    program.numberOfFunctions = 3;
    program.numberOfObjects = 5;
    program.numberOfKeys = 1;
    program.nodesPerFunction = 4;
    uint32_t host = program.numberOfFunctions;
    auto addStatement = [&](uint32_t body, ToyOperation operation, uint32_t payload, ValueFlowNodeID result, std::initializer_list<ValueFlowNodeID> operands) {
        ValueFlowStatement statement;
        statement.function = FunctionID(body);
        statement.site = ValueID(static_cast<uint32_t>(operation));
        statement.firstOperand = program.operands.size();
        statement.operandCount = operands.size();
        statement.result = result;
        for (ValueFlowNodeID operand : operands)
            program.operands.append(ValueFlowOperand { operand, { }, { } });
        program.statements.append(statement);
        program.payloads.append(payload);
    };
    for (uint32_t body = 0; body <= host; ++body) {
        ValueFlowBodyRange range;
        range.statement = range.statementLeavingBody = program.statements.size();
        program.ranges.append(range);
        if (body == host) {
            for (uint32_t function = 0; function < program.numberOfFunctions; ++function)
                addStatement(body, ToyOperation::Allocate, function, program.node(host, function), { });
            addStatement(body, ToyOperation::Call, 0, program.node(host, 3), { program.node(host, 0), program.node(host, 3) });
        } else if (body + 1 < program.numberOfFunctions) {
            program.edges.append(ValueFlowEdge { program.node(host, body + 1), program.node(body, 2) });
            addStatement(body, ToyOperation::Call, 0, program.result(body), { program.node(body, 2), program.parameter(body) });
        } else
            addStatement(body, ToyOperation::Allocate, 4, program.result(body), { });
    }
    ValueFlowBodyRange end;
    end.statement = end.statementLeavingBody = program.statements.size();
    program.ranges.append(end);

    toyProgram = &program;
    ValueFlowSolverInput input;
    input.numberOfNodes = program.numberOfNodes();
    input.edges = program.edges.span();
    input.statements = program.statements.span();
    input.operands = program.operands.span();
    input.ranges = program.ranges.span();
    for (unsigned numberOfThreads : { 1u, 4u }) {
        ParallelLoops loops(numberOfThreads);
        ValueSets sets(program.numberOfObjects);
        makeToyFunctions(sets, program);
        ValueFlowSolver solver(input, sets, runToyRule, loops);
        solver.solve();
        for (uint32_t function = 0; function < program.numberOfFunctions; ++function)
            AOT_TEST_CHECK(solver.isReached(FunctionID(function)));
        AOT_TEST_CHECK(sets.contains(solver.possibleValuesOf(program.node(host, 3)), ValueFlowObjectID(4u)));
        for (const ValueFlowSolver::Round& round : solver.rounds())
            AOT_TEST_CHECK(round.turns <= 1);
    }
    toyProgram = nullptr;
}

static void valueFlowSolverFindsObjectsOutsideDeclaredTypes()
{
    ParallelLoops loops(1);
    ValueSets sets(16);
    ValueSetMaker maker(sets);
    auto setOf = [&](std::initializer_list<uint32_t> members) {
        Vector<ValueFlowObjectID> list;
        for (uint32_t member : members)
            list.append(ValueFlowObjectID(member));
        return maker.of(list.span());
    };

    // Member 0 is no object, 1 is the untracked object, 2 the unknown object, 3 to 7 are objects of the program. The type lists 3 and 4.
    ValueSetID ofType = setOf({ 0, 1, 2, 3, 4 });
    ValueSetID family = setOf({ 3, 4 });
    ValueFlowNodeID withStranger(0u);
    ValueFlowNodeID withPlaceholders(1u);
    ValueFlowNodeID takingStranger(2u);
    ValueFlowNodeID takingPlaceholders(3u);
    ValueFlowEdge edges[] = { { withStranger, takingStranger }, { withPlaceholders, takingPlaceholders } };
    ValueFlowNodeSet seeds[] = { { withStranger, setOf({ 0, 3, 5 }) }, { withPlaceholders, setOf({ 1, 2, 3 }) } };
    ValueFlowNodeMask masks[] = { { takingStranger, ofType, ValueFlowMaskKind::Declared, family }, { takingPlaceholders, ofType, ValueFlowMaskKind::Declared, family } };
    ValueFlowBodyRange ranges[2];
    ranges[1].seedOnceReached = 2;

    ValueFlowSolverInput input;
    input.placeholders = { ValueFlowObjectID(1u), ValueFlowObjectID(2u), setOf({ 3, 4, 5, 6, 7 }), ValueSets::nothing };
    input.numberOfNodes = 4;
    input.edges = edges;
    input.masks = masks;
    input.seedsOnceReached = seeds;
    input.ranges = ranges;

    ValueFlowSolver solver(input, sets, [](const ValueFlowStatement&, ValueFlowRuleContext&) { }, loops);
    solver.solve();
    AOT_TEST_CHECK(solver.possibleValuesOf(takingStranger) == setOf({ 0, 3, 5 }));
    AOT_TEST_CHECK(solver.possibleValuesOf(takingPlaceholders) == setOf({ 2, 3, 4 }));
    AOT_TEST_CHECK(solver.holdsObjectOutsideFamily(takingStranger) && !solver.holdsObjectOutsideFamily(takingPlaceholders));
    AOT_TEST_CHECK(!solver.holdsObjectOutsideFamily(withStranger));
    AOT_TEST_CHECK(solver.counters().nodesHoldingObjectsOutsideFamily == 1);
}

static void whatAShorterWayOfARuleLeavesOutIsFound()
{
    ValueFlowStatement statements[2];
    statements[0].function = statements[1].function = FunctionID(0u);
    statements[0].result = ValueFlowNodeID(0u);
    statements[1].result = ValueFlowNodeID(1u);
    ValueFlowBodyRange ranges[2];
    ranges[1].statement = ranges[1].statementLeavingBody = 2;

    ValueFlowSolverInput input;
    input.numberOfNodes = 2;
    input.statements = statements;
    input.ranges = ranges;

    ValueFlowRule addsTheSameOnBothWays = [](const ValueFlowStatement& statement, ValueFlowRuleContext& context) {
        context.add(statement.result, context.maker().of(ValueFlowObjectID(1u)));
    };
    ValueFlowRule leavesOutTheFirstResult = [](const ValueFlowStatement& statement, ValueFlowRuleContext& context) {
        if (statement.result.index() || context.usesGeneralRulesOnly())
            context.add(statement.result, context.maker().of(ValueFlowObjectID(1u)));
    };
    for (unsigned numberOfThreads : { 1u, 4u }) {
        ParallelLoops loops(numberOfThreads);
        {
            ValueSets sets(4);
            ValueFlowSolver solver(input, sets, addsTheSameOnBothWays, loops, ValueFlowSolver::Rounds::Due);
            solver.solve();
            AOT_TEST_CHECK(!solver.addedByGeneralRules().isAnything());
            AOT_TEST_CHECK(!solver.numberOfRunsThatWereNotDueAndAdded());
        }
        {
            ValueSets sets(4);
            ValueFlowSolver solver(input, sets, leavesOutTheFirstResult, loops, ValueFlowSolver::Rounds::Due);
            solver.solve();
            AOT_TEST_CHECK(solver.addedByGeneralRules().nodesThatHoldMore == 1);
            AOT_TEST_CHECK(solver.addedByGeneralRules().firstNodesThatHoldMore.size() == 1 && solver.addedByGeneralRules().firstNodesThatHoldMore[0] == statements[0].result);
            AOT_TEST_CHECK(!solver.numberOfRunsThatWereNotDueAndAdded());
        }
        {
            ValueSets sets(4);
            ValueFlowSolver solver(input, sets, leavesOutTheFirstResult, loops, ValueFlowSolver::Rounds::DueWithoutCheck);
            solver.solve();
            AOT_TEST_CHECK(!solver.addedByGeneralRules().isAnything());
            AOT_TEST_CHECK(solver.possibleValuesOf(statements[0].result) == ValueSets::nothing);
        }
    }
}

static std::atomic<unsigned> runsOfStatementThatAsksLast;

static void whatIsAskedLastIsAnsweredWhenNothingElseIsDue()
{
    static constexpr uint32_t numberOfObjects = 12;
    ValueFlowStatement statements[2];
    statements[0].function = statements[1].function = FunctionID(0u);
    statements[0].result = ValueFlowNodeID(0u);
    statements[1].result = ValueFlowNodeID(1u);
    ValueFlowBodyRange ranges[2];
    ranges[1].statement = ranges[1].statementLeavingBody = 2;

    ValueFlowSolverInput input;
    input.numberOfNodes = 2;
    input.statements = statements;
    input.ranges = ranges;

    // The first statement gives its node one more member a round. The second takes all that the node holds.
    ValueFlowRule rule = [](const ValueFlowStatement& statement, ValueFlowRuleContext& context) {
        ValueFlowNodeID growing(0u);
        if (statement.result == growing) {
            uint32_t held = 0;
            context.forEachMember(context.valueOf(growing), [&](ValueFlowObjectID) {
                ++held;
            });
            if (held < numberOfObjects)
                context.add(growing, context.maker().of(ValueFlowObjectID(held)));
            return;
        }
        ++runsOfStatementThatAsksLast;
        context.add(statement.result, context.valueWithinAskedLast(ClimbingSet(context.maker().ofRange(ValueFlowObjectID(0u), ValueFlowObjectID(numberOfObjects))), growing));
    };
    for (unsigned numberOfThreads : { 1u, 4u }) {
        ParallelLoops loops(numberOfThreads);
        ValueSets sets(numberOfObjects);
        runsOfStatementThatAsksLast = 0;
        ValueFlowSolver solver(input, sets, rule, loops, ValueFlowSolver::Rounds::DueWithoutCheck);
        solver.solve();
        AOT_TEST_CHECK(sets.size(solver.possibleValuesOf(statements[0].result)) == numberOfObjects);
        AOT_TEST_CHECK(solver.possibleValuesOf(statements[1].result) == solver.possibleValuesOf(statements[0].result));
        AOT_TEST_CHECK(solver.rounds().size() > numberOfObjects);
        AOT_TEST_CHECK(runsOfStatementThatAsksLast == 2);
    }
}

static std::atomic<uint32_t> firstOfLowestObjects;

// The graph keeps the sixty-four lowest objects of a set as bits of one word, and finds the set of a word in a table by that word.
static void aNodeMayHoldAllOfTheLowestObjects()
{
    static constexpr uint32_t numberOfLowestObjects = 64;
    ValueFlowStatement statement;
    statement.function = FunctionID(0u);
    statement.result = ValueFlowNodeID(0u);
    ValueFlowBodyRange ranges[2];
    ranges[1].statement = ranges[1].statementLeavingBody = 1;

    ValueFlowSolverInput input;
    input.numberOfNodes = 1;
    input.statements = std::span(&statement, 1);
    input.ranges = ranges;

    ValueFlowRule rule = [](const ValueFlowStatement& statement, ValueFlowRuleContext& context) {
        uint32_t next = firstOfLowestObjects;
        context.forEachMember(context.valueOf(statement.result), [&](ValueFlowObjectID) {
            ++next;
        });
        if (next < numberOfLowestObjects)
            context.add(statement.result, context.maker().of(ValueFlowObjectID(next)));
    };
    for (uint32_t first : { 0u, 1u }) {
        ParallelLoops loops(1);
        ValueSets sets(numberOfLowestObjects);
        firstOfLowestObjects = first;
        ValueFlowSolver solver(input, sets, rule, loops, ValueFlowSolver::Rounds::DueWithoutCheck);
        solver.solve();
        AOT_TEST_CHECK(sets.size(solver.possibleValuesOf(statement.result)) == numberOfLowestObjects - first);
    }
}

static std::atomic<unsigned> runsOfReaderOfGrowingSet;
static std::atomic<unsigned> connectionsOfReaderOfGrowingSet;

// The statements that cost most are handed to the threads first. They were picked for that by being due, and a statement that waits is still due:
// so the one kind of statement that it pays most to let wait ran in every round.
static void aStatementThatMakesManyConnectionsWaitsLikeAnyOtherThatRunsOften()
{
    static constexpr uint32_t numberOfObjects = 256;
    static constexpr uint32_t growing = 0;
    static constexpr uint32_t taken = 1;
    static constexpr uint32_t handedOn = 2;
    static constexpr uint32_t firstOfRow = 3;
    Vector<ValueFlowStatement> statements(numberOfObjects + 1);
    for (uint32_t index = 0; index < statements.size(); ++index) {
        statements[index].function = FunctionID(0u);
        statements[index].result = ValueFlowNodeID(index < numberOfObjects ? firstOfRow + index : taken);
    }
    ValueFlowBodyRange ranges[2];
    ranges[1].statement = ranges[1].statementLeavingBody = statements.size();

    ValueFlowSolverInput input;
    input.numberOfNodes = firstOfRow + numberOfObjects;
    input.statements = statements.span();
    input.ranges = ranges;

    // Each statement of the row runs when the one in front of it has, a round later, and gives the set one more member: so something else is due in every round. The last statement takes all that the set holds.
    ValueFlowRule rule = [](const ValueFlowStatement& statement, ValueFlowRuleContext& context) {
        if (statement.result.index() >= firstOfRow) {
            uint32_t position = statement.result.index() - firstOfRow;
            if (position && !context.meet(context.valueOf(ValueFlowNodeID(statement.result.index() - 1)), ValueSets::anything))
                return;
            context.add(statement.result, context.maker().of(ValueFlowObjectID(position)));
            context.add(ValueFlowNodeID(growing), context.maker().of(ValueFlowObjectID(position)));
            return;
        }
        ++runsOfReaderOfGrowingSet;
        context.add(statement.result, context.valueOf(ValueFlowNodeID(growing)));
        for (unsigned connection = 0; connection < connectionsOfReaderOfGrowingSet; ++connection)
            context.connect(ValueFlowNodeID(taken), ValueFlowNodeID(handedOn));
    };
    for (unsigned numberOfThreads : { 1u, 4u }) {
        unsigned runsWithFewConnections = 0;
        for (unsigned connections : { 1u, 5000u }) {
            ParallelLoops loops(numberOfThreads);
            ValueSets sets(numberOfObjects);
            runsOfReaderOfGrowingSet = 0;
            connectionsOfReaderOfGrowingSet = connections;
            ValueFlowSolver solver(input, sets, rule, loops, ValueFlowSolver::Rounds::DueWithoutCheck);
            solver.solve();
            AOT_TEST_CHECK(sets.size(solver.possibleValuesOf(ValueFlowNodeID(growing))) == numberOfObjects);
            AOT_TEST_CHECK(solver.possibleValuesOf(ValueFlowNodeID(handedOn)) == solver.possibleValuesOf(ValueFlowNodeID(growing)));
            AOT_TEST_CHECK(solver.rounds().size() >= numberOfObjects);
            AOT_TEST_CHECK(runsOfReaderOfGrowingSet < numberOfObjects / 4);
            if (connections == 1)
                runsWithFewConnections = runsOfReaderOfGrowingSet;
            else
                AOT_TEST_CHECK(runsOfReaderOfGrowingSet == runsWithFewConnections);
        }
    }
}

static void numbersByKeyAgreeWithAHashMap()
{
    // The second kind of key is picked as a shard of watches picks its sets: by the high bits of a product with the golden ratio.
    for (bool arePickedByHighBitsOfProduct : { false, true }) {
        WeakRandom random(7);
        ValueFlowNumbersByKey numbers;
        HashMap<uint64_t, uint32_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> expected;
        AOT_TEST_CHECK(!numbers.find(0));
        uint32_t next = 0;
        while (expected.size() < 50000) {
            uint32_t low = arePickedByHighBitsOfProduct ? next++ : random.getUint32();
            if (arePickedByHighBitsOfProduct && low * 0x9E3779B1u >> 28 != 5)
                continue;
            uint64_t key = static_cast<uint64_t>(arePickedByHighBitsOfProduct ? 3 : random.getUint32(1000)) << 32 | low;
            uint32_t number = expected.size();
            auto added = numbers.add(key, number);
            auto addedToExpected = expected.add(key, number);
            AOT_TEST_CHECK(added.isNew == addedToExpected.isNewEntry);
            AOT_TEST_CHECK(added.number == addedToExpected.iterator->value);
            if (!(number % 3)) {
                added.number += 7;
                addedToExpected.iterator->value += 7;
            }
        }
        for (auto [key, number] : expected) {
            const uint32_t* found = numbers.find(key);
            AOT_TEST_CHECK(found && *found == number);
            AOT_TEST_CHECK(!!numbers.find(key + 1) == expected.contains(key + 1));
        }
        AOT_TEST_CHECK(numbers.bytesInUse() == expected.size() * 12);
        AOT_TEST_CHECK(numbers.memoryInBytes() <= numbers.bytesInUse() * 3 + 12);
        AOT_TEST_CHECK(numbers.longestRunOfSlotsInUse() < 256);
    }
}

// A loop on many threads borrows a maker at each of its steps. A thread takes what it gave back, whoever else takes one in between, so the threads do not meet at the pool.
static void aMakerThatIsGivenBackWaitsForItsThread()
{
    ValueSets sets(64);
    ValueSetMakerPool pool(sets);
    ValueSetMaker& mine = pool.take();
    pool.giveBack(mine);
    // Of two threads made one behind the other at most one is taken for this one.
    std::array<ValueSetMaker*, 2> takenByOthers { };
    for (ValueSetMaker*& taken : takenByOthers) {
        Thread::create("a borrower"_s, [&] {
            taken = &pool.take();
            pool.giveBack(*taken);
        })->waitForCompletion();
    }
    AOT_TEST_CHECK(takenByOthers[0] != &mine || takenByOthers[1] != &mine);
    AOT_TEST_CHECK(&pool.take() == &mine);
    ValueSetMaker& second = pool.take();
    AOT_TEST_CHECK(&second != &mine);
    pool.giveBack(second);
    pool.giveBack(mine);
    AOT_TEST_CHECK(&pool.take() == &second);
    AOT_TEST_CHECK(&pool.take() == &mine);
}

static void numbersTakenOutOfListsAreGoneAndTheOthersStay()
{
    // One number is in the name of the list, up to sixteen stand side by side, more are dealt out over eight lists, and those again.
    for (uint32_t size : { 1u, 2u, 3u, 4u, 5u, 6u, 7u, 12u, 13u, 16u, 17u, 40u, 300u, 5000u }) {
        for (uint32_t step : { 1u, 2u, 3u, 7u }) {
            ValueFlowNumberLists lists;
            uint32_t list = 0;
            uint32_t other = 0;
            auto numberAt = [](uint32_t index) { return index * 2654435761u >> 3; };
            for (uint32_t index = 0; index < size; ++index) {
                AOT_TEST_CHECK(lists.add(list, numberAt(index)));
                AOT_TEST_CHECK(lists.add(other, numberAt(index) ^ 1));
            }
            AOT_TEST_CHECK(!lists.remove(list, numberAt(size)));
            uint32_t left = size;
            for (uint32_t index = 0; index < size; index += step, --left) {
                AOT_TEST_CHECK(lists.remove(list, numberAt(index)));
                AOT_TEST_CHECK(!lists.remove(list, numberAt(index)));
                AOT_TEST_CHECK(!lists.contains(list, numberAt(index)));
            }
            uint32_t seen = 0;
            lists.forEach(list, [&](uint32_t) { ++seen; });
            AOT_TEST_CHECK(seen == left && lists.numberOfNumbers() == left + size);
            for (uint32_t index = 0; index < size; ++index) {
                AOT_TEST_CHECK(lists.contains(list, numberAt(index)) == !!(index % step));
                AOT_TEST_CHECK(lists.contains(other, numberAt(index) ^ 1));
            }
            // What was given back is taken again.
            size_t memory = lists.memoryInBytes();
            for (uint32_t index = 0; index < size; index += step)
                AOT_TEST_CHECK(lists.add(list, numberAt(index)));
            AOT_TEST_CHECK(lists.numberOfNumbers() == 2 * size && lists.memoryInBytes() <= memory + 64 * sizeof(uint32_t));
            for (uint32_t index = 0; index < size; ++index)
                AOT_TEST_CHECK(lists.remove(list, numberAt(index)));
            AOT_TEST_CHECK(size > 16 || !list);
            AOT_TEST_CHECK(lists.numberOfNumbers() == size);
        }
    }
}

static void anEdgeTakenOutOfTheGraphCarriesNoMore()
{
    ParallelLoops loops(1);
    ValueSets sets(16);
    Vector<std::unique_ptr<ValueSetMaker>> makers(1, [&](size_t) {
        return makeUnique<ValueSetMaker>(sets);
    });
    auto setOf = [&](std::initializer_list<uint32_t> members) {
        Vector<ValueFlowObjectID> list;
        for (uint32_t member : members)
            list.append(ValueFlowObjectID(member));
        return makers[0]->of(list.span());
    };
    // early -> source, late -> source, source -> old, source -> current
    ValueFlowGraph graph(loops, makers.span());
    ValueFlowNodeID early = graph.addNodes(5);
    ValueFlowNodeID late(early.index() + 1);
    ValueFlowNodeID source(early.index() + 2);
    ValueFlowNodeID old(early.index() + 3);
    ValueFlowNodeID current(early.index() + 4);
    graph.makeConstant(early, setOf({ 3 }));
    graph.makeConstant(late, setOf({ 5 }));
    Vector<ValueFlowNodeID> moved;
    ValueFlowEdge first[] = { { early, source }, { source, old }, { source, current } };
    AOT_TEST_CHECK(graph.addEdges(first) == 3);
    graph.solve(moved);
    AOT_TEST_CHECK(graph.valueOf(old) == setOf({ 3 }) && graph.valueOf(current) == setOf({ 3 }));

    ValueFlowEdge removed[] = { { source, old } };
    ValueFlowEdge listedTwice[] = { removed[0], removed[0] };
    graph.removeEdges(listedTwice);
    AOT_TEST_CHECK(!graph.hasAdded(removed[0]) && graph.counters().edgesRemoved == 1);
    bool isPredecessor = false;
    graph.forEachPredecessor(old, [&](ValueFlowNodeID predecessor) {
        isPredecessor |= predecessor == source;
    });
    AOT_TEST_CHECK(!isPredecessor);
    ValueFlowEdge second[] = { { late, source } };
    AOT_TEST_CHECK(graph.addEdges(second) == 1);
    graph.solve(moved);
    AOT_TEST_CHECK(graph.valueOf(old) == setOf({ 3 }) && graph.valueOf(current) == setOf({ 3, 5 }));

    // Whoever wants it again adds it as a new edge, and it brings all that its source holds.
    AOT_TEST_CHECK(graph.addEdges(removed) == 1);
    graph.solve(moved);
    AOT_TEST_CHECK(graph.valueOf(old) == setOf({ 3, 5 }));
}

#define FOR_EACH_AOT_VALUE_FLOW_CORE_TEST(v) \
    v(numbersTakenOutOfListsAreGoneAndTheOthersStay) v(anEdgeTakenOutOfTheGraphCarriesNoMore) \
    v(valueSetsAgreeWithPlainSets) v(valueSetsOfFewAndOfManyBlocksAgreeWithPlainSets) v(valueSetsOfSeveralGroupsOfBlocksAgreeWithPlainSets) v(valueSetsOfBothFormsAgreeWithPlainSets) v(valueSetsAreTheSameFromEveryThread) v(sampleOfBlocksDependsOnTheMembersAlone) v(valueFlowGraphFindsTheLeastSolution) v(whatTheGraphHoldsDoesNotHangOnTheOrderOfTheEdges) v(rowsOfNodesInsideOneShardAreWalkedInOneWave) \
    v(masksTreatPlaceholdersByTheirKind) v(nodesThatPlaceholdersCoverAreRetired) v(theCallerMayConnectWhatLeadsIntoRetiredNodes) v(whatACoveredNodeHoldsIsNotSentToItAgain) \
    v(valueFlowGraphSolvesLongChainsAndWideLevelsOnThreads) v(valueFlowRoundsAgreeWithAnAnalysisObjectByObject) v(whatATurnMakesAndReachesRunsInThatTurn) \
    v(valueFlowSolverFindsObjectsOutsideDeclaredTypes) v(whatAShorterWayOfARuleLeavesOutIsFound) v(whatIsAskedLastIsAnsweredWhenNothingElseIsDue) v(aNodeMayHoldAllOfTheLowestObjects) v(aStatementThatMakesManyConnectionsWaitsLikeAnyOtherThatRunsOften) v(numbersByKeyAgreeWithAHashMap) v(aMakerThatIsGivenBackWaitsForItsThread)

std::span<const NativeTest> valueFlowCoreTests()
{
#define AOT_VALUE_FLOW_CORE_TEST_ENTRY(name) { #name ""_s, name },
    static constexpr NativeTest tests[] = { FOR_EACH_AOT_VALUE_FLOW_CORE_TEST(AOT_VALUE_FLOW_CORE_TEST_ENTRY) };
#undef AOT_VALUE_FLOW_CORE_TEST_ENTRY
    return tests;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
