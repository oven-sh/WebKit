/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTMachineOrder.h"
#include "AOTMachineTestSupport.h"
#include "AOTMachineTestsOfPhis.h"
#include <array>
#include <limits>
#include <wtf/DataLog.h>

namespace JSC { namespace AOT { namespace Machine {

struct TestOfPhiTest {
    TestOfPhiTest()
        : out(testProcedure.out())
        , entry(out.m_block)
        , first(testProcedure.argumentGPR(0))
        , second(testProcedure.argumentGPR(1))
        , third(testProcedure.argumentGPR(2))
    {
    }

    Procedure& procedure() { return testProcedure.procedure(); }
    void returnConstant(LBasicBlock block, int64_t constant)
    {
        out.appendTo(block);
        out.ret(out.constInt64(constant));
    }
    const Order& compile()
    {
        compileOrFail(testProcedure);
        return procedure().order();
    }
    int64_t run(int64_t firstArgument, int64_t secondArgument = 0, int64_t thirdArgument = 0) { return testProcedure.call<int64_t>(firstArgument, secondArgument, thirdArgument); }

    TestProcedure testProcedure;
    Output& out;
    LBasicBlock entry;
    LValue first;
    LValue second;
    LValue third;
};

[[noreturn]] static void failTestOfPhiTest(const TestOfPhiTest& test, const char* file, int line, const char* condition)
{
    dataLogLn(test.testProcedure.printedProcedure(), test.testProcedure.printedOrder());
    failNativeTest(file, line, condition);
}

#define AOT_TEST_OF_PHI_CHECK(test, ...) do { \
        if (!(__VA_ARGS__)) [[unlikely]] \
            failTestOfPhiTest(test, __FILE__, __LINE__, #__VA_ARGS__); \
    } while (false)

static Block* targetInTestOfPhiTest(const Block* from, unsigned successorIndex)
{
    Block* target = from->successor(successorIndex).block();
    while (target->size() == 1 && target->last()->opcode() == Jump && target->phis().empty() && target->upsilons().empty())
        target = target->successor(0).block();
    return target;
}

enum class KindOfTestOfPhi : uint8_t {
    ComparesWithConstant,
    TestsBitsOfSum,
    IsReadAgain,
    ComparesWithValue,
    TestIsReadAgain,
    StandsBesideWhatIsReadBehindIt,
};

template<KindOfTestOfPhi kind>
static void testJoinThatComparesItsPhiIsPassedBy()
{
    TestOfPhiTest test;
    Output& out = test.out;
    LBasicBlock bringsConstant = out.newBlock();
    LBasicBlock bringsValue = out.newBlock();
    LBasicBlock join = out.newBlock();
    LBasicBlock holds = out.newBlock();
    LBasicBlock fails = out.newBlock();
    LValue held = out.phiIn(join, Int64);
    out.branch(test.first, normalEdge(bringsConstant), normalEdge(bringsValue));
    out.appendTo(bringsConstant);
    out.upsilon(out.constInt64(5), held);
    out.jump(join);
    out.appendTo(bringsValue);
    out.upsilon(test.second, held);
    out.jump(join);
    out.appendTo(join);
    LValue tested = out.equal(held, out.constInt64(5));
    if (kind == KindOfTestOfPhi::TestsBitsOfSum)
        tested = out.bitAnd(out.add(held, out.constInt64(3)), out.constInt64(8));
    else if (kind == KindOfTestOfPhi::ComparesWithValue)
        tested = out.equal(held, test.third);
    LValue sum = out.add(test.third, out.constInt64(5));
    out.branch(tested, normalEdge(holds), normalEdge(fails));
    out.appendTo(holds);
    if (kind == KindOfTestOfPhi::TestIsReadAgain)
        out.ret(out.add(out.zeroExtPtr(tested), 9));
    else
        out.ret(kind == KindOfTestOfPhi::StandsBesideWhatIsReadBehindIt ? sum : out.constInt64(10));
    out.appendTo(fails);
    out.ret(kind == KindOfTestOfPhi::IsReadAgain ? held : out.constInt64(20));
    const Order& order = test.compile();

    bool isPassedBy = kind == KindOfTestOfPhi::ComparesWithConstant || kind == KindOfTestOfPhi::TestsBitsOfSum;
    AOT_TEST_OF_PHI_CHECK(test, targetInTestOfPhiTest(test.entry, 0) == (isPassedBy ? holds : bringsConstant) && order.of(join).isReached());
    AOT_TEST_OF_PHI_CHECK(test, order.of(join).predecessors.size() == (isPassedBy ? 1u : 2u));
    for (int64_t value : { 0, 4, 5, 6, 13 }) {
        bool holdsOfValue = kind == KindOfTestOfPhi::TestsBitsOfSum ? (value + 3) & 8 : value == 5;
        int64_t whereItFails = kind == KindOfTestOfPhi::IsReadAgain ? value : 20;
        AOT_TEST_OF_PHI_CHECK(test, test.run(1, value, 5) == 10 && test.run(0, value, 5) == (holdsOfValue ? 10 : whereItFails));
    }
}

static void testValueOfTestWherePhiHolds()
{
    TestOfPhiTest test;
    Output& out = test.out;
    LBasicBlock join = out.newBlock();
    LValue wide = out.phiIn(join, Int64);
    out.upsilon(test.first, wide);
    out.jump(join);
    out.appendTo(join);
    LValue cut = out.castToInt32(wide);
    auto valueOf = [&](LValue tested, int64_t brought) {
        return valueOfTestWherePhiHolds(tested, wide, brought);
    };
    int64_t allOnesBelow = (int64_t { 1 } << 32) - 1;
    AOT_TEST_CHECK(valueOf(cut, allOnesBelow) == -1 && valueOf(out.zeroExtPtr(cut), allOnesBelow) == allOnesBelow && valueOf(out.signExt32To64(cut), allOnesBelow) == -1);
    AOT_TEST_CHECK(valueOf(out.add(cut, 1), std::numeric_limits<int32_t>::max()) == std::numeric_limits<int32_t>::min());
    AOT_TEST_CHECK(valueOf(out.lShr(cut, 28), -1) == 15 && valueOf(out.aShr(cut, 28), -1) == -1 && valueOf(out.shl(cut, 31), 3) == std::numeric_limits<int32_t>::min());
    AOT_TEST_CHECK(valueOf(out.above(cut, 5), -1) == 1 && valueOf(out.greaterThan(cut, 5), -1) == 0 && valueOf(out.below(wide, out.constInt64(-2)), -3) == 1);
    AOT_TEST_CHECK(valueOf(out.equal(out.bitAnd(wide, out.constInt64(~int64_t { 8 })), out.constInt64(2)), 10) == 1);
    AOT_TEST_CHECK(!valueOf(out.ctlz32(cut), 2) && !valueOf(out.add(wide, test.second), 2) && !valueOf(out.intToDouble(wide), 2));
    out.ret(wide);
}

static void testThirdArmPassesByJoinWhosePassingBlockHasBeenSkipped()
{
    constexpr int64_t upperHalf = int64_t { 0x5a5a } << 48;
    TestOfPhiTest test;
    Output& out = test.out;
    LBasicBlock boxes = out.newBlock();
    LBasicBlock doesNotBox = out.newBlock();
    LBasicBlock join = out.newBlock();
    LBasicBlock boxesNext = out.newBlock();
    LBasicBlock brings = out.newBlock();
    LBasicBlock passes = out.newBlock();
    LBasicBlock fails = out.newBlock();
    LBasicBlock behind = out.newBlock();
    LValue held = out.phiIn(join, Int64);
    LValue bound = out.constInt64(upperHalf);
    auto box = [&](LValue word) {
        return out.add(out.zeroExtPtr(word), bound);
    };
    out.branch(out.equal(test.first, out.constInt64(1)), normalEdge(boxes), normalEdge(doesNotBox));
    out.appendTo(boxes);
    out.upsilon(box(out.castToInt32(test.second)), held);
    out.jump(join);
    out.appendTo(doesNotBox);
    out.branch(out.equal(test.first, out.constInt64(2)), normalEdge(boxesNext), normalEdge(brings));
    out.appendTo(boxesNext);
    out.upsilon(box(out.add(out.castToInt32(test.second), 1)), held);
    out.jump(join);
    out.appendTo(brings);
    out.upsilon(test.third, held);
    out.jump(join);
    out.appendTo(join);
    out.branch(out.aboveOrEqual(held, bound), normalEdge(passes), normalEdge(fails));
    out.appendTo(passes);
    out.jump(behind);
    test.returnConstant(fails, 8);
    test.returnConstant(behind, 9);
    test.compile();

    AOT_TEST_OF_PHI_CHECK(test, test.run(1, 5) == 9 && test.run(2, 5) == 9 && test.run(0, 0, 7) == 8);
}

enum class KindOfWordInBox : uint8_t {
    IsAskedWhetherAtLeast,
    IsAskedWhetherBelow,
    ComesByTwoArms,
    IsIndexWhereItPasses,
    IsNotReadWhereItPasses,
    IsReadWholeWhereItPasses,
    IsReadBehindBothSides,
    FailsWhereWhatPassesMayFailToo,
    BoundHasLowerHalf,
    IsExtendedWithItsSign,
};

template<KindOfWordInBox kind>
static void testWordInBoxPassesByJoinThatAsksForBox()
{
    constexpr int64_t upperHalf = (int64_t { 0x5a5a } << 48) + (kind == KindOfWordInBox::BoundHasLowerHalf ? 1 : 0);
    static constexpr std::array<uint8_t, 16> bytes { 90, 91, 92, 93, 94, 95, 96, 97, 98, 99, 100, 101, 102, 103, 104, 105 };
    TestOfPhiTest test;
    Output& out = test.out;
    LBasicBlock boxes = out.newBlock();
    LBasicBlock doesNotBox = out.newBlock();
    LBasicBlock boxesNext = out.newBlock();
    LBasicBlock brings = out.newBlock();
    LBasicBlock join = out.newBlock();
    LBasicBlock passes = out.newBlock();
    LBasicBlock fails = out.newBlock();
    LBasicBlock isNotSeven = out.newBlock();
    LBasicBlock behind = out.newBlock();
    LValue held = out.phiIn(join, Int64);
    LValue answer = out.phiIn(behind, Int64);
    LValue bound = out.constInt64(upperHalf);
    auto box = [&](LValue word) {
        return out.add(kind == KindOfWordInBox::IsExtendedWithItsSign ? out.signExt32To64(word) : out.zeroExtPtr(word), bound);
    };
    out.branch(out.equal(test.first, out.constInt64(1)), normalEdge(boxes), normalEdge(doesNotBox));
    out.appendTo(boxes);
    out.upsilon(box(kind == KindOfWordInBox::IsIndexWhereItPasses ? out.bitAnd(out.castToInt32(test.second), out.constInt32(15)) : out.castToInt32(test.second)), held);
    out.jump(join);
    out.appendTo(doesNotBox);
    out.branch(out.equal(test.first, out.constInt64(2)), normalEdge(boxesNext), normalEdge(brings));
    out.appendTo(boxesNext);
    out.upsilon(kind == KindOfWordInBox::ComesByTwoArms ? box(out.add(out.castToInt32(test.second), 1)) : out.add(test.third, out.constInt64(0x100)), held);
    out.jump(join);
    out.appendTo(brings);
    out.upsilon(test.third, held);
    out.jump(join);

    out.appendTo(join);
    if (kind == KindOfWordInBox::IsAskedWhetherBelow)
        out.branch(out.below(held, bound), normalEdge(fails), normalEdge(passes));
    else
        out.branch(out.aboveOrEqual(held, bound), normalEdge(passes), normalEdge(fails));
    out.appendTo(passes);
    LValue whereItPasses = kind == KindOfWordInBox::IsNotReadWhereItPasses ? out.constInt64(77) : out.zeroExtPtr(out.castToInt32(held));
    if (kind == KindOfWordInBox::IsReadWholeWhereItPasses)
        whereItPasses = out.add(whereItPasses, held);
    if (kind == KindOfWordInBox::IsIndexWhereItPasses)
        whereItPasses = out.zeroExtPtr(out.load8ZeroExt32(out.baseIndex(test.testProcedure.heaps().argumentBuffer, out.constIntPtr(reinterpret_cast<intptr_t>(bytes.data())), whereItPasses, ScaleOne)));
    if (kind == KindOfWordInBox::FailsWhereWhatPassesMayFailToo) {
        out.branch(out.equal(whereItPasses, out.constInt64(7)), normalEdge(fails), normalEdge(isNotSeven));
        out.appendTo(isNotSeven);
    }
    out.upsilon(whereItPasses, answer);
    out.jump(behind);
    out.appendTo(fails);
    out.upsilon(out.add(held, out.constInt64(1)), answer);
    out.jump(behind);
    out.appendTo(behind);
    out.ret(kind == KindOfWordInBox::IsReadBehindBothSides ? out.add(answer, held) : answer);
    const Order& order = test.compile();

    bool isPassedBy = kind <= KindOfWordInBox::IsNotReadWhereItPasses;
    unsigned armsThatPassBy = !isPassedBy ? 0 : kind == KindOfWordInBox::ComesByTwoArms ? 2 : 1;
    AOT_TEST_OF_PHI_CHECK(test, boxes->successor(0).block() == (isPassedBy ? passes : join) && order.of(join).isReached());
    AOT_TEST_OF_PHI_CHECK(test, boxesNext->successor(0).block() == (armsThatPassBy == 2 ? passes : join));
    AOT_TEST_OF_PHI_CHECK(test, order.of(join).predecessors.size() == 3 - armsThatPassBy);
    AOT_TEST_OF_PHI_CHECK(test, passes->phis().size() == (isPassedBy && kind != KindOfWordInBox::IsNotReadWhereItPasses ? 1u : 0u));

    auto expected = [&](uint64_t word) -> int64_t {
        uint64_t result = word + 1;
        if (word >= static_cast<uint64_t>(upperHalf)) {
            uint64_t lowerHalf = static_cast<uint32_t>(word);
            if (kind != KindOfWordInBox::FailsWhereWhatPassesMayFailToo || lowerHalf != 7) {
                result = kind == KindOfWordInBox::IsNotReadWhereItPasses ? 77 : kind == KindOfWordInBox::IsIndexWhereItPasses ? bytes[lowerHalf] : lowerHalf;
                if (kind == KindOfWordInBox::IsReadWholeWhereItPasses)
                    result += word;
            }
        }
        if (kind == KindOfWordInBox::IsReadBehindBothSides)
            result += word;
        return static_cast<int64_t>(result);
    };
    auto boxOf = [&](int64_t word) {
        uint64_t extended = kind == KindOfWordInBox::IsExtendedWithItsSign ? static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(word))) : static_cast<uint32_t>(word);
        return extended + static_cast<uint64_t>(upperHalf);
    };
    for (int64_t word : { int64_t { 0 }, int64_t { 1 }, int64_t { 6 }, int64_t { 7 }, int64_t { -1 }, int64_t { -2 }, int64_t { std::numeric_limits<int32_t>::max() }, int64_t { std::numeric_limits<int32_t>::min() }, upperHalf + 5, (int64_t { 1 } << 32) + 7 }) {
        AOT_TEST_OF_PHI_CHECK(test, test.run(1, word, 0) == expected(boxOf(kind == KindOfWordInBox::IsIndexWhereItPasses ? word & 15 : word)));
        if (kind == KindOfWordInBox::ComesByTwoArms)
            AOT_TEST_OF_PHI_CHECK(test, test.run(2, word, 0) == expected(boxOf(static_cast<int32_t>(static_cast<uint32_t>(word) + 1))));
    }
    for (int64_t brought : { int64_t { 0 }, int64_t { 7 }, int64_t { -1 }, upperHalf - 1, upperHalf, upperHalf + 1, upperHalf + 7, upperHalf + 0xffffffff, upperHalf + (int64_t { 1 } << 32), upperHalf + (int64_t { 3 } << 32) + 7, std::numeric_limits<int64_t>::max(), std::numeric_limits<int64_t>::min() }) {
        if (kind == KindOfWordInBox::IsIndexWhereItPasses && static_cast<uint64_t>(brought) >= static_cast<uint64_t>(upperHalf) && static_cast<uint32_t>(brought) >= bytes.size())
            continue;
        AOT_TEST_OF_PHI_CHECK(test, test.run(0, 0, brought) == expected(static_cast<uint64_t>(brought)));
        if (kind != KindOfWordInBox::ComesByTwoArms && kind != KindOfWordInBox::IsIndexWhereItPasses)
            AOT_TEST_OF_PHI_CHECK(test, test.run(2, 0, brought) == expected(static_cast<uint64_t>(brought) + 0x100));
    }
}

static size_t bytesOfStoresInTestOfPhiTest(Type type, int64_t lower, int64_t upper, ptrdiff_t offsetOfUpper, unsigned numberOfPairs)
{
    TestOfPhiTest test;
    Output& out = test.out;
    const AbstractHeap& heap = test.testProcedure.heaps().argumentBuffer;
    ptrdiff_t size = sizeOf(type);
    for (unsigned pair = 0; pair < numberOfPairs; ++pair) {
        ptrdiff_t start = static_cast<ptrdiff_t>(pair) * 32;
        out.store(out.constantOfType(type, lower), out.address(heap, test.first, start));
        out.store(out.constantOfType(type, upper), out.address(heap, test.first, start + offsetOfUpper));
    }
    out.ret(out.int64Zero);
    test.compile();

    alignas(16) std::array<uint8_t, 256> bytes;
    bytes.fill(0xee);
    test.run(reinterpret_cast<int64_t>(bytes.data()));
    for (unsigned pair = 0; pair < numberOfPairs; ++pair) {
        for (ptrdiff_t place = 0; place < 32; ++place) {
            uint8_t expected = 0xee;
            if (place < size)
                expected = static_cast<uint8_t>(static_cast<uint64_t>(lower) >> (8 * place));
            else if (place >= offsetOfUpper && place < offsetOfUpper + size)
                expected = static_cast<uint8_t>(static_cast<uint64_t>(upper) >> (8 * (place - offsetOfUpper)));
            AOT_TEST_OF_PHI_CHECK(test, bytes[pair * 32 + place] == expected);
        }
    }
    return test.testProcedure.linkedCode().size();
}

enum class KindOfTwoBounds : uint8_t { BothAreSigned, LowerIsSignedAndNotBelowNought, LowerIsSignedAndBelowNought, LowerIsUnsignedAndUpperSigned };

template<KindOfTwoBounds kind>
static void testTwoBoundsOfOneValue()
{
    TestOfPhiTest test;
    Output& out = test.out;
    LBasicBlock asksUpperBound = out.newBlock();
    LBasicBlock inside = out.newBlock();
    LBasicBlock outside = out.newBlock();
    constexpr int32_t low = kind == KindOfTwoBounds::BothAreSigned || kind == KindOfTwoBounds::LowerIsSignedAndBelowNought ? -5 : 65;
    constexpr int32_t high = 90;
    constexpr bool lowerIsSigned = kind != KindOfTwoBounds::LowerIsUnsignedAndUpperSigned;
    constexpr bool upperIsSigned = kind == KindOfTwoBounds::BothAreSigned || kind == KindOfTwoBounds::LowerIsUnsignedAndUpperSigned;
    LValue value = out.castToInt32(test.first);
    out.branch(lowerIsSigned ? out.greaterThanOrEqual(value, out.constInt32(low)) : out.aboveOrEqual(value, out.constInt32(low)), normalEdge(asksUpperBound), normalEdge(outside));
    out.appendTo(asksUpperBound);
    out.branch(upperIsSigned ? out.lessThanOrEqual(value, out.constInt32(high)) : out.belowOrEqual(value, out.constInt32(high)), normalEdge(inside), normalEdge(outside));
    test.returnConstant(inside, 1);
    test.returnConstant(outside, 0);
    const Order& order = test.compile();

    bool areAskedAtOnce = kind == KindOfTwoBounds::BothAreSigned || kind == KindOfTwoBounds::LowerIsSignedAndNotBelowNought;
    AOT_TEST_OF_PHI_CHECK(test, order.of(asksUpperBound).isReached() == !areAskedAtOnce);
    AOT_TEST_OF_PHI_CHECK(test, value->useCount() == (areAskedAtOnce ? 1u : 2u));
    for (int32_t asSigned : { std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::min() + 1, -91, -90, -65, -6, -5, -4, -1, 0, 1, 64, 65, 66, 89, 90, 91, std::numeric_limits<int32_t>::max() - 1, std::numeric_limits<int32_t>::max() }) {
        uint32_t asUnsigned = static_cast<uint32_t>(asSigned);
        bool passesLower = lowerIsSigned ? asSigned >= low : asUnsigned >= static_cast<uint32_t>(low);
        bool passesUpper = upperIsSigned ? asSigned <= high : asUnsigned <= static_cast<uint32_t>(high);
        AOT_TEST_OF_PHI_CHECK(test, test.run(asSigned) == (passesLower && passesUpper ? 1 : 0));
    }
}

enum class KindOfArms : uint8_t { OnlyHandOn, HoldStores, HandOnDifferentValues };

template<KindOfArms kind>
static void testArmsOfTwoBounds()
{
    TestOfPhiTest test;
    Output& out = test.out;
    const AbstractHeap& heap = test.testProcedure.heaps().argumentBuffer;
    LBasicBlock below = out.newBlock();
    LBasicBlock asksUpperBound = out.newBlock();
    LBasicBlock above = out.newBlock();
    LBasicBlock inside = out.newBlock();
    LBasicBlock join = out.newBlock();
    LValue answer = out.phiIn(join, Int64);
    LValue value = out.castToInt32(test.first);
    auto arm = [&](LBasicBlock block, int32_t stored, ptrdiff_t offset, int64_t handedOn) {
        out.appendTo(block);
        if (kind == KindOfArms::HoldStores)
            out.store32(out.constInt32(stored), out.address(heap, test.second, offset));
        out.upsilon(out.constInt64(handedOn), answer);
        out.jump(join);
    };
    out.branch(out.lessThan(value, out.constInt32(3)), normalEdge(below), normalEdge(asksUpperBound));
    arm(below, 5, 0, 10);
    out.appendTo(asksUpperBound);
    out.branch(out.greaterThan(value, out.constInt32(9)), normalEdge(above), normalEdge(inside));
    arm(above, 7, 4, kind == KindOfArms::HandOnDifferentValues ? 11 : 10);
    out.appendTo(inside);
    out.upsilon(out.constInt64(20), answer);
    out.jump(join);
    out.appendTo(join);
    out.ret(answer);
    const Order& order = test.compile();

    AOT_TEST_OF_PHI_CHECK(test, order.of(asksUpperBound).isReached() == (kind != KindOfArms::OnlyHandOn));
    for (int32_t asked : { std::numeric_limits<int32_t>::min(), -1, 0, 2, 3, 4, 9, 10, 11, std::numeric_limits<int32_t>::max() }) {
        std::array<int32_t, 2> words { 0, 0 };
        int64_t expected = asked < 3 ? 10 : asked > 9 ? (kind == KindOfArms::HandOnDifferentValues ? 11 : 10) : 20;
        AOT_TEST_OF_PHI_CHECK(test, test.run(asked, reinterpret_cast<int64_t>(words.data())) == expected);
        AOT_TEST_OF_PHI_CHECK(test, words[0] == (kind == KindOfArms::HoldStores && asked < 3 ? 5 : 0));
        AOT_TEST_OF_PHI_CHECK(test, words[1] == (kind == KindOfArms::HoldStores && asked > 9 ? 7 : 0));
    }
}

static void testNeighboringStoresOfConstantsAreOneStore()
{
    size_t neighbors = bytesOfStoresInTestOfPhiTest(Int32, 7, 0, 4, 1);
    size_t apart = bytesOfStoresInTestOfPhiTest(Int32, 7, 0, 8, 1);
    size_t notOneImmediate = bytesOfStoresInTestOfPhiTest(Int32, 7, 1, 4, 1);
    size_t signExtended = bytesOfStoresInTestOfPhiTest(Int32, -7, -1, 4, 1);
    size_t emptyNeighbors = bytesOfStoresInTestOfPhiTest(Int64, 0, 0, 8, 1);
    size_t emptyApart = bytesOfStoresInTestOfPhiTest(Int64, 0, 0, 16, 1);
    size_t filledNeighbors = bytesOfStoresInTestOfPhiTest(Int64, 0, 3, 8, 1);
    bytesOfStoresInTestOfPhiTest(Int64, 0, 0, 8, 3);
    if (!isX86_64())
        return;
    AOT_TEST_CHECK(neighbors < apart && signExtended == neighbors && notOneImmediate == apart);
    AOT_TEST_CHECK(emptyNeighbors < emptyApart && filledNeighbors == emptyApart);
}

#undef AOT_TEST_OF_PHI_CHECK

} // namespace Machine

std::span<const NativeTest> machineTestsOfPhisTests()
{
    using Machine::KindOfTestOfPhi;
    using Machine::KindOfWordInBox;
    static constexpr NativeTest tests[] = {
        { "machine-tests-of-phis/join-that-tests-bits-of-a-sum-of-its-phi-is-passed-by"_s, Machine::testJoinThatComparesItsPhiIsPassedBy<KindOfTestOfPhi::TestsBitsOfSum> },
        { "machine-tests-of-phis/value-of-a-test-where-the-phi-holds-a-constant"_s, Machine::testValueOfTestWherePhiHolds },
        { "machine-tests-of-phis/words-in-boxes-of-two-arms-pass-by-the-join"_s, Machine::testWordInBoxPassesByJoinThatAsksForBox<KindOfWordInBox::ComesByTwoArms> },
        { "machine-tests-of-phis/a-third-arm-passes-by-a-join-whose-passing-block-has-been-skipped"_s, Machine::testThirdArmPassesByJoinWhosePassingBlockHasBeenSkipped },
        { "machine-tests-of-phis/word-in-a-box-that-is-an-index-where-it-passes-passes-by-the-join"_s, Machine::testWordInBoxPassesByJoinThatAsksForBox<KindOfWordInBox::IsIndexWhereItPasses> },
        { "machine-tests-of-phis/word-in-a-box-that-nothing-reads-where-it-passes-passes-by-the-join"_s, Machine::testWordInBoxPassesByJoinThatAsksForBox<KindOfWordInBox::IsNotReadWhereItPasses> },
        { "machine-tests-of-phis/box-that-is-read-whole-where-it-passes-goes-through-the-join"_s, Machine::testWordInBoxPassesByJoinThatAsksForBox<KindOfWordInBox::IsReadWholeWhereItPasses> },
        { "machine-tests-of-phis/box-that-is-read-behind-both-sides-goes-through-the-join"_s, Machine::testWordInBoxPassesByJoinThatAsksForBox<KindOfWordInBox::IsReadBehindBothSides> },
        { "machine-tests-of-phis/box-that-is-read-where-what-passes-may-fail-too-goes-through-the-join"_s, Machine::testWordInBoxPassesByJoinThatAsksForBox<KindOfWordInBox::FailsWhereWhatPassesMayFailToo> },
        { "machine-tests-of-phis/word-added-to-a-bound-with-a-lower-half-goes-through-the-join"_s, Machine::testWordInBoxPassesByJoinThatAsksForBox<KindOfWordInBox::BoundHasLowerHalf> },
        { "machine-tests-of-phis/neighboring-stores-of-constants-are-one-store"_s, Machine::testNeighboringStoresOfConstantsAreOneStore },
        { "machine-tests-of-phis/two-signed-bounds-of-one-value-are-asked-at-once"_s, Machine::testTwoBoundsOfOneValue<Machine::KindOfTwoBounds::BothAreSigned> },
        { "machine-tests-of-phis/a-signed-lower-bound-not-below-nought-and-an-unsigned-upper-bound-are-asked-at-once"_s, Machine::testTwoBoundsOfOneValue<Machine::KindOfTwoBounds::LowerIsSignedAndNotBelowNought> },
        { "machine-tests-of-phis/a-signed-lower-bound-below-nought-and-an-unsigned-upper-bound-stay-two-tests"_s, Machine::testTwoBoundsOfOneValue<Machine::KindOfTwoBounds::LowerIsSignedAndBelowNought> },
        { "machine-tests-of-phis/an-unsigned-lower-and-a-signed-upper-bound-stay-two-tests"_s, Machine::testTwoBoundsOfOneValue<Machine::KindOfTwoBounds::LowerIsUnsignedAndUpperSigned> },
        { "machine-tests-of-phis/arms-of-two-bounds-that-only-hand-on-one-value-are-one-arm"_s, Machine::testArmsOfTwoBounds<Machine::KindOfArms::OnlyHandOn> },
        { "machine-tests-of-phis/arms-of-two-bounds-that-each-store-are-two-arms"_s, Machine::testArmsOfTwoBounds<Machine::KindOfArms::HoldStores> },
        { "machine-tests-of-phis/arms-of-two-bounds-that-hand-on-different-values-are-two-arms"_s, Machine::testArmsOfTwoBounds<Machine::KindOfArms::HandOnDifferentValues> },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
