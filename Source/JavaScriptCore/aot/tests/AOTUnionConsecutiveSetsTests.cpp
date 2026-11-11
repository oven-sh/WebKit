/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTFacts.h"
#include "AOTGrowthOfWork.h"
#include "AOTIRParser.h"
#include "AOTNativeTest.h"
#include "AOTPlan.h"
#include "AOTProgramFile.h"
#include "AOTTestHarness.h"
#include "AOTUnionConsecutiveSets.h"
#include <algorithm>
#include <initializer_list>
#include <wtf/text/StringBuilder.h>

namespace JSC { namespace AOT {

std::span<const NativeTest> unionConsecutiveSetsTests();

using namespace Typed;

static Vector<uint32_t> orderByNumberForConsecutiveSetsTests(const ConsecutiveSets& sets, uint32_t numberOfLeaves)
{
    Vector<uint64_t> keys(numberOfLeaves, [](size_t leaf) {
        return static_cast<uint64_t>(leaf);
    });
    return sets.order(keys.span());
}

static bool standSideBySideForConsecutiveSetsTests(const Vector<uint32_t>& order, std::initializer_list<uint32_t> leaves)
{
    size_t first = order.size();
    size_t last = 0;
    for (uint32_t leaf : leaves) {
        size_t position = order.find(leaf);
        first = std::min(first, position);
        last = std::max(last, position);
    }
    return last - first + 1 == leaves.size();
}

static bool tryKeepTogetherForConsecutiveSetsTests(ConsecutiveSets& sets, std::initializer_list<uint32_t> leaves)
{
    Vector<uint32_t> list(leaves);
    return sets.tryKeepTogether(list.span());
}

static void testOrderOfKeysWhereNothingIsKeptTogether()
{
    ConsecutiveSets sets(5);
    Vector<uint64_t> keys { 40, 10, 30, 0, 20 };
    Vector<uint32_t> order = sets.order(keys.span());
    AOT_TEST_CHECK((order == Vector<uint32_t> { 3, 1, 4, 2, 0 }));
    AOT_TEST_CHECK(ConsecutiveSets(0).order({ }).isEmpty());
}

static void testNestedAndSeparateSetsAreAllKeptTogether()
{
    ConsecutiveSets sets(8);
    AOT_TEST_CHECK(tryKeepTogetherForConsecutiveSetsTests(sets, { 0, 7 }));
    AOT_TEST_CHECK(tryKeepTogetherForConsecutiveSetsTests(sets, { 0, 3, 7 }));
    AOT_TEST_CHECK(tryKeepTogetherForConsecutiveSetsTests(sets, { 1, 5 }));
    AOT_TEST_CHECK(tryKeepTogetherForConsecutiveSetsTests(sets, { 0, 1, 3, 5, 7 }));
    Vector<uint32_t> order = orderByNumberForConsecutiveSetsTests(sets, 8);
    AOT_TEST_CHECK(order.size() == 8);
    AOT_TEST_CHECK(standSideBySideForConsecutiveSetsTests(order, { 0, 7 }));
    AOT_TEST_CHECK(standSideBySideForConsecutiveSetsTests(order, { 0, 3, 7 }));
    AOT_TEST_CHECK(standSideBySideForConsecutiveSetsTests(order, { 1, 5 }));
    AOT_TEST_CHECK(standSideBySideForConsecutiveSetsTests(order, { 0, 1, 3, 5, 7 }));
    AOT_TEST_CHECK(!order[0]);
}

static void testSetThatNoOrderKeepsTogetherIsRefusedAndChangesNothing()
{
    ConsecutiveSets sets(5);
    AOT_TEST_CHECK(tryKeepTogetherForConsecutiveSetsTests(sets, { 0, 1 }));
    AOT_TEST_CHECK(tryKeepTogetherForConsecutiveSetsTests(sets, { 1, 2 }));
    AOT_TEST_CHECK(tryKeepTogetherForConsecutiveSetsTests(sets, { 2, 3 }));
    Vector<uint32_t> before = orderByNumberForConsecutiveSetsTests(sets, 5);
    AOT_TEST_CHECK(!tryKeepTogetherForConsecutiveSetsTests(sets, { 0, 2 }));
    AOT_TEST_CHECK(!tryKeepTogetherForConsecutiveSetsTests(sets, { 0, 3 }));
    AOT_TEST_CHECK(!tryKeepTogetherForConsecutiveSetsTests(sets, { 1, 4 }));
    AOT_TEST_CHECK(orderByNumberForConsecutiveSetsTests(sets, 5) == before);
    AOT_TEST_CHECK(tryKeepTogetherForConsecutiveSetsTests(sets, { 3, 4 }));
    AOT_TEST_CHECK(standSideBySideForConsecutiveSetsTests(orderByNumberForConsecutiveSetsTests(sets, 5), { 3, 4 }));
}

static void testNestedSetsMatchLeafSets()
{
    uint32_t state = 12345;
    auto random = [&](uint32_t limit) {
        state = state * 1664525 + 1013904223;
        return (state >> 8) % limit;
    };
    for (unsigned round = 0; round < 200; ++round) {
        uint32_t numberOfLeaves = 2 + random(40);
        Vector<Vector<uint32_t>> groups;
        for (uint32_t leaf = 0; leaf < numberOfLeaves; ++leaf)
            groups.append(Vector<uint32_t> { leaf });
        Vector<Vector<uint32_t>> nested;
        while (groups.size() > 1 && random(8)) {
            Vector<uint32_t> united;
            for (unsigned count = 2 + random(3); count && !groups.isEmpty(); --count) {
                size_t taken = random(groups.size());
                united.appendVector(groups[taken]);
                groups.removeAt(taken);
            }
            nested.append(united);
            groups.append(united);
        }
        std::ranges::stable_sort(nested, { }, &Vector<uint32_t>::size);
        ConsecutiveSets leafByLeaf(numberOfLeaves);
        ConsecutiveSets byNodes(numberOfLeaves);
        for (const Vector<uint32_t>& set : nested) {
            AOT_TEST_CHECK(leafByLeaf.tryKeepTogether(set.span()));
            AOT_TEST_CHECK(byNodes.tryKeepNestedSetTogether(set.span()));
        }
        for (unsigned count = 0; count < 12; ++count) {
            Vector<uint32_t> set;
            for (uint32_t leaf = 0; leaf < numberOfLeaves; ++leaf) {
                if (!random(4))
                    set.append(leaf);
            }
            AOT_TEST_CHECK(leafByLeaf.tryKeepTogether(set.span()) == byNodes.tryKeepTogether(set.span()));
        }
        AOT_TEST_CHECK(orderByNumberForConsecutiveSetsTests(leafByLeaf, numberOfLeaves) == orderByNumberForConsecutiveSetsTests(byNodes, numberOfLeaves));
    }
    ConsecutiveSets sets(4);
    Vector<uint32_t> first { 0, 1 };
    Vector<uint32_t> overlapping { 1, 2 };
    AOT_TEST_CHECK(sets.tryKeepNestedSetTogether(first.span()));
    AOT_TEST_CHECK(!sets.tryKeepNestedSetTogether(overlapping.span()));
    AOT_TEST_CHECK(sets.tryKeepTogether(overlapping.span()));
}

static void testLeafThatIsNamedTwiceCountsOnce()
{
    ConsecutiveSets sets(4);
    AOT_TEST_CHECK(tryKeepTogetherForConsecutiveSetsTests(sets, { 2, 2 }));
    AOT_TEST_CHECK(tryKeepTogetherForConsecutiveSetsTests(sets, { 0, 3, 0, 3 }));
    AOT_TEST_CHECK(standSideBySideForConsecutiveSetsTests(orderByNumberForConsecutiveSetsTests(sets, 4), { 0, 3 }));
}

static void keepManySetsTogetherForConsecutiveSetsTests(uint32_t numberOfLeaves)
{
    ConsecutiveSets sets(numberOfLeaves);
    Vector<uint32_t> leaves;
    for (uint32_t size : { 8u, 64u }) {
        for (uint32_t first = 0; first + size <= numberOfLeaves; first += size) {
            leaves.shrink(0);
            for (uint32_t leaf = first; leaf < first + size; ++leaf)
                leaves.append(leaf);
            AOT_TEST_CHECK(sets.tryKeepTogether(leaves.span()));
        }
    }
    for (uint32_t border = 8; border < numberOfLeaves; border += 8) {
        leaves.shrink(0);
        leaves.append(border - 1);
        leaves.append(border);
        sets.tryKeepTogether(leaves.span());
    }
    AOT_TEST_CHECK(orderByNumberForConsecutiveSetsTests(sets, numberOfLeaves).size() == numberOfLeaves);
}

static void testTreeGrowsAsItsLeavesDo()
{
    constexpr uint32_t numberOfLeaves = 1 << 15;
    InstructionCounts::begin();
    std::optional<uint64_t> smaller = instructionsOfAllThreadsIn([&] {
        keepManySetsTogetherForConsecutiveSetsTests(numberOfLeaves);
    });
    std::optional<uint64_t> large = instructionsOfAllThreadsIn([&] {
        keepManySetsTogetherForConsecutiveSetsTests(4 * numberOfLeaves);
    });
    AOT_TEST_CHECK(growsAsSizeDoes(smaller, large));
}

static String manyUnionsForConsecutiveSetsTests(unsigned numberOfUnions)
{
    StringBuilder text;
    unsigned unionOfField = 0;
    auto field = [&](ASCIILiteral key) {
        text.append("{key=\""_s, key, unionOfField, "\" declared={type=anything} flags=[IsRequired]}"_s);
    };
    for (unsigned number = 0; number < numberOfUnions; ++number) {
        unionOfField = number;
        unsigned first = 4 * number;
        text.append("layout L"_s, first, " module=m0 fields=["_s);
        field("a"_s);
        text.append(", "_s);
        field("kind"_s);
        text.append("] ownFieldCount=2\nlayout L"_s, first + 1, " module=m0 fields=["_s);
        field("kind"_s);
        text.append(", "_s);
        field("b"_s);
        text.append("] ownFieldCount=2\nlayout L"_s, first + 2, " module=m0 fields=["_s);
        field("x"_s);
        text.append(", "_s);
        field("parent"_s);
        text.append(", "_s);
        field("y"_s);
        text.append("] ownFieldCount=3\nlayout L"_s, first + 3, " module=m0 fields=["_s);
        field("p"_s);
        text.append(", "_s);
        field("q"_s);
        text.append("] ownFieldCount=2\n"_s);
    }
    text.append("module m0 path=\"main.js\" displayPath=\"main.js\" body=f0\nentry module=m0\n"_s);
    text.append("function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {\n  b0():\n    v0 = Integer integer=1\n"_s);
    unsigned value = 1;
    for (unsigned number = 0; number < numberOfUnions; ++number) {
        unsigned closure = value++;
        text.append("    v"_s, closure, " = NewClosure function=f"_s, number + 1, "\n"_s);
        for (unsigned member = 0; member < 4; ++member) {
            bool hasThreeFields = member == 2;
            text.append("    v"_s, value, " = NewObject -, v0, v0"_s, hasThreeFields ? ", v0"_s : ""_s, " layout=L"_s, 4 * number + member, " fields=[0, 1"_s, hasThreeFields ? ", 2]\n"_s : "]\n"_s);
            text.append("    v"_s, value + 1, " = FinishObject v"_s, value, "\n"_s);
            text.append("    v"_s, value + 2, " = Call v"_s, closure, ", -, v"_s, value + 1, "\n"_s);
            value += 3;
        }
    }
    text.append("    Return v0\n}\n"_s);
    for (unsigned number = 0; number < numberOfUnions; ++number) {
        unsigned first = 4 * number;
        StringBuilder type;
        type.append("L"_s, first, "|L"_s, first + 1, "|L"_s, first + 2, "|L"_s, first + 3);
        String written = type.toString();
        text.append("function f"_s, number + 1, " module=m0 length=1 thisType={type=anything} parameters=[{type="_s, written, " flags=[IsExpected]}] results=[{type=anything}] {\n"_s);
        text.append("  b0(v0: "_s, written, "):\n    v1 = PropGet v0 key=\"kind"_s, number, "\"\n    v2 = PropGet v0 key=\"parent"_s, number, "\"\n    v3 = Add v1, v2\n    Return v3\n}\n"_s);
    }
    return text.toString();
}

static std::optional<uint64_t> instructionsOfPlanForConsecutiveSetsTests(const String& text)
{
    std::optional<uint64_t> instructions;
    runOnBuildThread([&] {
        BuildContext build(makeOptionsOfTestedProgram());
        std::unique_ptr<Program> parsed = parseProgram(withStandInLibrary(text), build);
        AOT_TEST_CHECK(parsed);
        StagedBuild stages(WTF::move(parsed), build);
        AOT_TEST_CHECK(stages.facts());
        instructions = instructionsOfAllThreadsIn([&] {
            AOT_TEST_CHECK(stages.plan());
        });
    });
    return instructions;
}

static void testPlanGrowsAsUnionsDo()
{
    constexpr unsigned numberOfUnions = 400;
    InstructionCounts::begin();
    std::optional<uint64_t> smaller = instructionsOfPlanForConsecutiveSetsTests(manyUnionsForConsecutiveSetsTests(numberOfUnions));
    std::optional<uint64_t> large = instructionsOfPlanForConsecutiveSetsTests(manyUnionsForConsecutiveSetsTests(4 * numberOfUnions));
    AOT_TEST_CHECK(growsAsSizeDoes(smaller, large));
}

static void appendModuleThatHandsOutFirstFunctionForConsecutiveSetsTests(StringBuilder& text)
{
    text.append("module m0 path=\"main.js\" displayPath=\"main.js\" body=f0\nentry module=m0\n"_s);
    text.append("function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {\n  b0():\n    v0 = Builtin builtin=GlobalThis\n    v1 = PropGet v0 key=\"sink\" [isFreeName]\n    v2 = NewClosure function=f1\n    v3 = Call v1, -, v2\n    Return v3\n}\n"_s);
    text.append("function f1 name=\"asked\" module=m0 length=1 thisType={type=anything} parameters=[{type=anything}] results=[{type=anything}] {\n  b0(v0: anything):\n"_s);
}

static String chainOfComparisonsForConsecutiveSetsTests(unsigned numberOfLinks)
{
    StringBuilder text;
    appendModuleThatHandsOutFirstFunctionForConsecutiveSetsTests(text);
    for (unsigned link = 0; link < numberOfLinks; ++link) {
        if (link)
            text.append("  b"_s, link, "():\n"_s);
        text.append("    v"_s, 2 * link + 1, " = Integer integer="_s, link + 1, "\n    v"_s, 2 * link + 2, " = StrictEq v0, v"_s, 2 * link + 1, "\n"_s);
        text.append("    Branch v"_s, 2 * link + 2, " -> taken b"_s, numberOfLinks + 1, "(), notTaken b"_s, link + 1, "()\n"_s);
    }
    text.append("  b"_s, numberOfLinks, "():\n    v"_s, 2 * numberOfLinks + 1, " = Integer integer=20\n    Return v"_s, 2 * numberOfLinks + 1, "\n"_s);
    text.append("  b"_s, numberOfLinks + 1, "():\n    v"_s, 2 * numberOfLinks + 2, " = Integer integer=10\n    Return v"_s, 2 * numberOfLinks + 2, "\n}\n"_s);
    return text.toString();
}

static String rowOfAlternativesForConsecutiveSetsTests(unsigned numberOfAlternatives)
{
    StringBuilder text;
    appendModuleThatHandsOutFirstFunctionForConsecutiveSetsTests(text);
    text.append("    v1 = Integer integer=1\n    v2 = StrictEq v0, v1\n    v3 = ToBoolean v2\n"_s);
    unsigned truth = 3;
    unsigned value = 4;
    for (unsigned alternative = 1; alternative < numberOfAlternatives; ++alternative) {
        unsigned block = 4 * alternative - 3;
        text.append("    Branch v"_s, truth, " -> taken b"_s, block, "(| 0), notTaken b"_s, block + 1, "(| 0)\n"_s);
        text.append("  b"_s, block, "(v"_s, value, "):\n    Jump -> next b"_s, block + 3, "(v"_s, value, ")\n"_s);
        text.append("  b"_s, block + 1, "(v"_s, value + 1, "):\n    Jump -> next b"_s, block + 2, "()\n"_s);
        text.append("  b"_s, block + 2, "():\n    v"_s, value + 2, " = Integer integer="_s, alternative + 1, "\n    v"_s, value + 3, " = StrictEq v0, v"_s, value + 2, "\n    Jump -> next b"_s, block + 3, "(v"_s, value + 3, ")\n"_s);
        text.append("  b"_s, block + 3, "(v"_s, value + 4, "):\n"_s);
        if (alternative + 1 == numberOfAlternatives) {
            text.append("    Return v"_s, value + 4, "\n}\n"_s);
            break;
        }
        text.append("    v"_s, value + 5, " = ToBoolean v"_s, value + 4, "\n"_s);
        truth = value + 5;
        value += 6;
    }
    return text.toString();
}

static std::optional<uint64_t> instructionsOfWalkForConsecutiveSetsTests(const String& text)
{
    std::optional<uint64_t> instructions;
    runOnBuildThread([&] {
        BuildContext build(makeOptionsOfTestedProgram());
        std::unique_ptr<Program> parsed = parseProgram(withStandInLibrary(text), build);
        AOT_TEST_CHECK(parsed);
        StagedBuild stages(WTF::move(parsed), build);
        CodeUnit unit { FunctionID(1u), CodeRole::Body };
        AOT_TEST_CHECK(stages.code(unit));
        instructions = instructionsOfAllThreadsIn([&] {
            AOT_TEST_CHECK(stages.code(unit));
        });
    });
    return instructions;
}

static void testWalkGrowsAsLinksOfChainDo()
{
    constexpr unsigned numberOfLinks = 1500;
    InstructionCounts::begin();
    std::optional<uint64_t> smaller = instructionsOfWalkForConsecutiveSetsTests(chainOfComparisonsForConsecutiveSetsTests(numberOfLinks));
    std::optional<uint64_t> large = instructionsOfWalkForConsecutiveSetsTests(chainOfComparisonsForConsecutiveSetsTests(4 * numberOfLinks));
    AOT_TEST_CHECK(growsAsSizeDoes(smaller, large));
}

static void testWalkGrowsAsAlternativesOfRowDo()
{
    constexpr unsigned numberOfAlternatives = 1500;
    InstructionCounts::begin();
    std::optional<uint64_t> smaller = instructionsOfWalkForConsecutiveSetsTests(rowOfAlternativesForConsecutiveSetsTests(numberOfAlternatives));
    std::optional<uint64_t> large = instructionsOfWalkForConsecutiveSetsTests(rowOfAlternativesForConsecutiveSetsTests(4 * numberOfAlternatives));
    AOT_TEST_CHECK(growsAsSizeDoes(smaller, large));
}

std::span<const NativeTest> unionConsecutiveSetsTests()
{
    static constexpr NativeTest tests[] {
        { "union-consecutive-sets/the-order-of-the-keys-where-nothing-is-kept-together"_s, testOrderOfKeysWhereNothingIsKeptTogether },
        { "union-consecutive-sets/nested-and-separate-sets-are-all-kept-together"_s, testNestedAndSeparateSetsAreAllKeptTogether },
        { "union-consecutive-sets/a-set-that-no-order-keeps-together-is-refused-and-changes-nothing"_s, testSetThatNoOrderKeepsTogetherIsRefusedAndChangesNothing },
        { "union-consecutive-sets/nested-sets-match-leaf-sets"_s, testNestedSetsMatchLeafSets },
        { "union-consecutive-sets/a-leaf-that-is-named-twice-counts-once"_s, testLeafThatIsNamedTwiceCountsOnce },
        { "union-consecutive-sets/the-tree-grows-as-its-leaves-do"_s, testTreeGrowsAsItsLeavesDo },
        { "union-consecutive-sets/the-plan-grows-as-the-unions-do"_s, testPlanGrowsAsUnionsDo },
        { "union-consecutive-sets/the-walk-grows-as-the-links-of-a-chain-do"_s, testWalkGrowsAsLinksOfChainDo },
        { "union-consecutive-sets/the-walk-grows-as-the-alternatives-of-a-row-do"_s, testWalkGrowsAsAlternativesOfRowDo },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
