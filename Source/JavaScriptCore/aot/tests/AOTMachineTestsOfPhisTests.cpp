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

// No constant here is one that findPinnedConstant() knows: the tests run without an instance of their own.

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

// ---- T1

enum class KindOfTestOfPhi : uint8_t {
    ComparesWithConstant,
    TestsBitsOfSum,
    IsReadAgain,
    ComparesWithValue,
    TestIsReadAgain,
    StandsBesideWhatIsReadBehindIt,
};

// const held = c ? 5 : second; if (held === 5) ..
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

// ---- S1: what the encoder writes. The bytes of two procedures that differ in the one thing are held against each other.

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

static void testNeighboringStoresOfConstantsAreOneStore()
{
    size_t neighbors = bytesOfStoresInTestOfPhiTest(Int32, 7, 0, 4, 1);
    size_t apart = bytesOfStoresInTestOfPhiTest(Int32, 7, 0, 8, 1);
    size_t notOneImmediate = bytesOfStoresInTestOfPhiTest(Int32, 7, 1, 4, 1);
    size_t signExtended = bytesOfStoresInTestOfPhiTest(Int32, -7, -1, 4, 1);
    size_t emptyNeighbors = bytesOfStoresInTestOfPhiTest(Int64, 0, 0, 8, 1);
    size_t emptyApart = bytesOfStoresInTestOfPhiTest(Int64, 0, 0, 16, 1);
    size_t filledNeighbors = bytesOfStoresInTestOfPhiTest(Int64, 0, 3, 8, 1);
    // The second pair finds the register cleared.
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
    static constexpr NativeTest tests[] = {
        { "machine-tests-of-phis/join-that-compares-its-phi-with-a-constant-is-passed-by"_s, Machine::testJoinThatComparesItsPhiIsPassedBy<KindOfTestOfPhi::ComparesWithConstant> },
        { "machine-tests-of-phis/join-that-tests-bits-of-a-sum-of-its-phi-is-passed-by"_s, Machine::testJoinThatComparesItsPhiIsPassedBy<KindOfTestOfPhi::TestsBitsOfSum> },
        { "machine-tests-of-phis/join-whose-phi-is-read-again-stays"_s, Machine::testJoinThatComparesItsPhiIsPassedBy<KindOfTestOfPhi::IsReadAgain> },
        { "machine-tests-of-phis/join-that-compares-its-phi-with-a-value-stays"_s, Machine::testJoinThatComparesItsPhiIsPassedBy<KindOfTestOfPhi::ComparesWithValue> },
        { "machine-tests-of-phis/join-whose-test-is-read-again-stays"_s, Machine::testJoinThatComparesItsPhiIsPassedBy<KindOfTestOfPhi::TestIsReadAgain> },
        { "machine-tests-of-phis/join-that-computes-what-is-read-behind-it-stays"_s, Machine::testJoinThatComparesItsPhiIsPassedBy<KindOfTestOfPhi::StandsBesideWhatIsReadBehindIt> },
        { "machine-tests-of-phis/value-of-a-test-where-the-phi-holds-a-constant"_s, Machine::testValueOfTestWherePhiHolds },
        { "machine-tests-of-phis/neighboring-stores-of-constants-are-one-store"_s, Machine::testNeighboringStoresOfConstantsAreOneStore },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
