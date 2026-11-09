/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTChildProcess.h"
#include "AOTMachineTestSupport.h"
#include <algorithm>
#include <array>
#include <limits>
#include <stdlib.h>
#include <wtf/DataLog.h>

#if OS(WINDOWS)
#include <windows.h>
#endif

namespace JSC { namespace AOT { namespace Machine {

struct NumberingTestFixture {
    using Memory = std::array<int64_t, 8>;

    explicit NumberingTestFixture(const ProcedureTraits& traits = { })
        : test(traits)
    {
        const HeapTree& tree = heaps.tree();
        AOT_TEST_CHECK(&test.procedure().heapTree() == &tree);
        AOT_TEST_CHECK(tree.parentOf(firstElement.index()) == everyElement.index() && tree.parentOf(secondElement.index()) == everyElement.index());
        AOT_TEST_CHECK(firstElement.index() != secondElement.index() && tree.parentOf(everyElement.index()) == arrays.index());
        AOT_TEST_CHECK(tree.isSameOrAbove(arrays.index(), cousin.index()) && !tree.overlap(everyElement.index(), cousin.index()));
        AOT_TEST_CHECK(!tree.overlap(arrays.index(), unrelated.index()));
        AOT_TEST_CHECK(tree.parentOf(immutableBelowMutable.index()) == mutableAboveImmutable.index());
        for (const AbstractHeap* heap : { &everything, &arrays, &everyElement, &firstElement, &secondElement, &cousin, &unrelated, &mutableAboveImmutable })
            AOT_TEST_CHECK(heap->mutability() == Mutability::Mutable);
        AOT_TEST_CHECK(immutable.mutability() == Mutability::Immutable && immutableBelowMutable.mutability() == Mutability::Immutable);
    }

    static constexpr ptrdiff_t offsetOfWord(unsigned index) { return static_cast<ptrdiff_t>(index * sizeof(int64_t)); }

    TypedPointer word(const AbstractHeap& heap, unsigned index = 0) { return out.address(heap, words, offsetOfWord(index)); }
    Operation* load(const AbstractHeap& heap, unsigned index = 0) { return out.load64(word(heap, index)); }
    void store(Operation* value, const AbstractHeap& heap, unsigned index = 0) { out.store64(value, out.address(heap, alias, offsetOfWord(index))); }

    PatchOperation* appendWriter(unsigned index)
    {
        int32_t offset = static_cast<int32_t>(offsetOfWord(index));
        PatchOperation* patch = out.patch(Void);
        patch->appendSomeRegister(alias);
        patch->appendSomeRegister(operand);
        patch->effects = Effects::none();
        patch->setRunsWithoutFrame(true);
        patch->setGenerator([offset](CCallHelpers& jit, const PatchParameters& params) {
            jit.store64(params[1].gpr(), CCallHelpers::Address(params[0].gpr(), offset));
            if (params.successorLabels().empty() || params.fallsThroughToSuccessor(0))
                return;
            CCallHelpers::Jump toSuccessor = jit.jump();
            MacroAssembler::Label* successor = params.successorLabels()[0];
            params.addLatePath([=](CCallHelpers& jit) {
                toSuccessor.linkTo(*successor, &jit);
            });
        });
        return patch;
    }

    PatchOperation* appendCall()
    {
        PatchOperation* patch = out.patch(Void);
        patch->setRunsWithoutFrame(true);
        patch->setShowsRegistersToCollector(true);
        patch->setGenerator([](CCallHelpers&, const PatchParameters&) { });
        return patch;
    }

    Operation* combine(Operation* ones, Operation* hundreds, Operation* tenThousands = nullptr)
    {
        Operation* result = out.add(ones, out.mul(hundreds, 100));
        return tenThousands ? out.add(result, out.mul(tenThousands, 10000)) : result;
    }

    int64_t run(Memory& memory, int64_t valueOfOperand) { return test.call<int64_t>(memory.data(), valueOfOperand, memory.data()); }

    TestProcedure test;
    Output& out { test.out() };
    const AOTHeaps& heaps { test.heaps() };
    Operation* words { test.argumentGPR(0) };
    Operation* operand { test.argumentGPR(1) };
    Operation* alias { test.argumentGPR(2) };

    AbstractHeap everything { rootHeapIndex };
    AbstractHeap arrays { heaps.elementsOfArrays };
    AbstractHeap everyElement { heaps.indexedContiguousProperties.atAnyIndex() };
    AbstractHeap firstElement { heaps.indexedContiguousProperties.at(0) };
    AbstractHeap secondElement { heaps.indexedContiguousProperties.at(1) };
    AbstractHeap cousin { heaps.indexedDoubleProperties.at(0) };
    AbstractHeap unrelated { heaps.JSPromise_slot };
    AbstractHeap immutable { heaps.JSCell_typeInfoType };
    AbstractHeap mutableAboveImmutable { heaps.JSRopeString_fiber1 };
    AbstractHeap immutableBelowMutable { heaps.JSRopeString_length };
};

using NumberingTestGroup = std::array<Operation*, 4>;

static constexpr NumberingTestFixture::Memory numberingTestMemory { 3, 4, 6, 7, 30, 40, 60, 70 };

static constexpr int64_t numberingTestAnswer(int64_t ones, int64_t hundreds, int64_t tenThousands = 0)
{
    return ones + 100 * hundreds + 10000 * tenThousands;
}

static bool areDistinctInNumberingTest(std::span<Operation* const> values)
{
    for (size_t index = 0; index < values.size(); ++index) {
        for (size_t earlier = 0; earlier < index; ++earlier) {
            if (values[index] == values[earlier])
                return false;
        }
    }
    return true;
}

static PatchOperation* appendNumberingTestPurePatch(NumberingTestFixture& fixture, const PureFunction& function, std::span<Operation* const> operands)
{
    int32_t id = static_cast<int32_t>(function.id);
    PatchOperation* patch = fixture.out.patch(Int64);
    for (Operation* operand : operands)
        patch->appendSomeRegister(operand);
    patch->resultConstraints = { ValueRep::SomeEarlyRegister };
    patch->effects = Effects::none();
    patch->pureFunction = function;
    patch->setRunsWithoutFrame(true);
    patch->setGenerator([id](CCallHelpers& jit, const PatchParameters& params) {
        jit.move(CCallHelpers::TrustedImm32(id), params[0].gpr());
        for (unsigned index = 1; index < params.size(); ++index)
            jit.add64(params[index].gpr(), params[0].gpr());
    });
    return patch;
}

static Operation* requestNumberingTestPurePatch(NumberingTestFixture& fixture, const PureFunction& function, std::span<Operation* const> operands)
{
    if (Operation* existing = fixture.out.findPurePatch(function, Int64, operands))
        return existing;
    PatchOperation* patch = appendNumberingTestPurePatch(fixture, function, operands);
    fixture.out.recordPurePatch(patch);
    return patch;
}

static NumberingTestGroup requestNumberingTestGroup(NumberingTestFixture& fixture, unsigned number)
{
    return {
        fixture.load(fixture.secondElement, number),
        fixture.load(fixture.immutable, 4 + number),
        fixture.out.mul(fixture.operand, fixture.out.add(fixture.operand, number + 1)),
        requestNumberingTestPurePatch(fixture, { 40 + number, true }, singleElementSpan(fixture.operand)),
    };
}

static Operation* sumOfNumberingTestGroup(Output& out, const NumberingTestGroup& group)
{
    return out.add(out.add(group[0], group[1]), out.add(group[2], group[3]));
}

static int64_t valueOfNumberingTestGroup(int64_t operand, unsigned number)
{
    return numberingTestMemory[number] + numberingTestMemory[4 + number] + operand * (operand + number + 1) + 40 + number + operand;
}

static bool standsInNumberingTestBlock(const NumberingTestGroup& group, const Block* block)
{
    return std::ranges::all_of(group, [&](const Operation* value) {
        return value->owner() == block;
    });
}

static void testMachineNumberingPatchWritesCountOnceItIsSetUp()
{
    {
        NumberingTestFixture fixture;
        Operation* before = fixture.load(fixture.firstElement);
        fixture.appendWriter(0);
        AOT_TEST_CHECK(fixture.load(fixture.firstElement) == before);
    }
    {
        NumberingTestFixture fixture;
        Operation* first = fixture.load(fixture.firstElement);
        Operation* second = fixture.load(fixture.secondElement, 1);
        Operation* third = fixture.load(fixture.cousin, 2);
        Operation* above = fixture.load(fixture.arrays, 3);
        Operation* elsewhere = fixture.load(fixture.unrelated, 4);
        const AbstractHeap* written[] = { &fixture.firstElement, &fixture.cousin };
        fixture.appendWriter(0)->setWrites(written);
        Operation* firstAgain = fixture.load(fixture.firstElement);
        AOT_TEST_CHECK(firstAgain != first && fixture.load(fixture.cousin, 2) != third && fixture.load(fixture.arrays, 3) != above);
        AOT_TEST_CHECK(fixture.load(fixture.secondElement, 1) == second && fixture.load(fixture.unrelated, 4) == elsewhere);
        fixture.out.ret(fixture.combine(first, firstAgain, second));

        compileOrFail(fixture.test);
        NumberingTestFixture::Memory memory = numberingTestMemory;
        AOT_TEST_CHECK(fixture.run(memory, 5) == numberingTestAnswer(3, 5, 4));
    }
    {
        NumberingTestFixture fixture;
        Operation* before = fixture.load(fixture.firstElement);
        PatchOperation* patch = fixture.appendWriter(0);
        patch->append(fixture.out.constInt64(77), ValueRep::WarmAny);
        patch->effects.writes = fixture.test.procedure().heapSet(fixture.firstElement);
        AOT_TEST_CHECK(fixture.load(fixture.firstElement) != before);
    }
    {
        NumberingTestFixture fixture;
        PureFunction function { 50, true };
        Operation* before = fixture.load(fixture.firstElement);
        PatchOperation* patch = appendNumberingTestPurePatch(fixture, function, singleElementSpan(fixture.operand));
        patch->effects.writes = fixture.test.procedure().heapSet(fixture.firstElement);
        fixture.out.recordPurePatch(patch);
        AOT_TEST_CHECK(fixture.load(fixture.firstElement) != before);
        AOT_TEST_CHECK(fixture.out.findPurePatch(function, Int64, singleElementSpan(fixture.operand)) == patch);
    }
}

static void testMachineNumberingAddressInFrameIsComputedAgain()
{
    for (bool savesFrameOnSuspend : { true, false }) {
        ProcedureTraits traits;
        traits.savesFrameOnSuspend = savesFrameOnSuspend;
        NumberingTestFixture fixture(traits);
        Output& out = fixture.out;
        Procedure& proc = fixture.test.procedure();
        Operation* slot = out.slotBase(proc.addStackSlot(16));
        Operation* otherSlot = out.slotBase(proc.addStackSlot(16));
        auto request = [&]() -> std::array<Operation*, 5> {
            return {
                out.add(slot, fixture.operand),
                out.sub(slot, fixture.operand),
                out.select(fixture.operand, slot, otherSlot),
                out.add(out.add(slot, 8), fixture.operand),
                out.add(out.framePointer(), fixture.operand),
            };
        };

        std::array<Operation*, 5> first = request();
        std::array<Operation*, 5> second = request();
        AOT_TEST_CHECK(areDistinctInNumberingTest(first));
        for (size_t index = 0; index < first.size(); ++index) {
            AOT_TEST_CHECK(first[index]->owner() == out.m_block && second[index]->owner() == out.m_block);
            AOT_TEST_CHECK((second[index] != first[index]) == savesFrameOnSuspend);
        }

        AOT_TEST_CHECK(out.add(slot, 8) == out.add(slot, 8) && out.add(slot, 8)->hasNoPosition());
        AOT_TEST_CHECK(out.add(fixture.words, fixture.operand) == out.add(fixture.words, fixture.operand));
        TypedPointer wordOfSlot = out.address(fixture.heaps.frameSlots, slot, 8);
        AOT_TEST_CHECK(out.load64(wordOfSlot) == out.load64(wordOfSlot));
    }
}

static void testMachineNumberingNestedForks()
{
    NumberingTestFixture fixture;
    Output& out = fixture.out;
    Block* left = out.newBlock();
    Block* innerLeft = out.newBlock();
    Block* innerRight = out.newBlock();
    Block* innerJoin = out.newBlock();
    Block* right = out.newBlock();
    Block* join = out.newBlock();
    Operation* isOdd = out.testNonZero64(fixture.operand, 1);
    Operation* hasSecondBit = out.testNonZero64(fixture.operand, 2);
    NumberingTestGroup atFork = requestNumberingTestGroup(fixture, 0);
    ScopeFork fork = out.forkScope();
    out.branch(isOdd, normalEdge(left), normalEdge(right));

    out.appendToArm(left, fork);
    NumberingTestGroup inLeft = requestNumberingTestGroup(fixture, 1);
    ScopeFork innerFork = out.forkScope();
    out.branch(hasSecondBit, normalEdge(innerLeft), normalEdge(innerRight));

    out.appendToArm(innerLeft, innerFork);
    AOT_TEST_CHECK(requestNumberingTestGroup(fixture, 0) == atFork && requestNumberingTestGroup(fixture, 1) == inLeft);
    AOT_TEST_CHECK(standsInNumberingTestBlock(requestNumberingTestGroup(fixture, 2), innerLeft));
    ValueFromBlock fromInnerLeft = out.anchor(out.constInt64(1000000));
    out.jump(innerJoin);

    out.appendToArm(innerRight, innerFork);
    AOT_TEST_CHECK(requestNumberingTestGroup(fixture, 0) == atFork && requestNumberingTestGroup(fixture, 1) == inLeft);
    AOT_TEST_CHECK(standsInNumberingTestBlock(requestNumberingTestGroup(fixture, 2), innerRight));
    ValueFromBlock fromInnerRight = out.anchor(out.constInt64(2000000));
    out.jump(innerJoin);

    out.appendToJoin(innerJoin, innerFork);
    AOT_TEST_CHECK(requestNumberingTestGroup(fixture, 0) == atFork && requestNumberingTestGroup(fixture, 1) == inLeft);
    NumberingTestGroup inInnerJoin = requestNumberingTestGroup(fixture, 2);
    AOT_TEST_CHECK(standsInNumberingTestBlock(inInnerJoin, innerJoin));
    Operation* fromInnerArm = out.phi(Int64, fromInnerLeft, fromInnerRight);
    ValueFromBlock fromLeft = out.anchor(out.add(fromInnerArm, out.add(sumOfNumberingTestGroup(out, inLeft), sumOfNumberingTestGroup(out, inInnerJoin))));
    out.jump(join);

    out.appendToArm(right, fork);
    AOT_TEST_CHECK(requestNumberingTestGroup(fixture, 0) == atFork);
    NumberingTestGroup inRight = requestNumberingTestGroup(fixture, 1);
    AOT_TEST_CHECK(standsInNumberingTestBlock(inRight, right) && standsInNumberingTestBlock(requestNumberingTestGroup(fixture, 2), right));
    ValueFromBlock fromRight = out.anchor(sumOfNumberingTestGroup(out, inRight));
    out.jump(join);

    out.appendToJoin(join, fork);
    AOT_TEST_CHECK(requestNumberingTestGroup(fixture, 0) == atFork);
    AOT_TEST_CHECK(standsInNumberingTestBlock(requestNumberingTestGroup(fixture, 1), join) && standsInNumberingTestBlock(requestNumberingTestGroup(fixture, 2), join));
    out.ret(out.add(out.phi(Int64, fromLeft, fromRight), sumOfNumberingTestGroup(out, atFork)));

    compileOrFail(fixture.test);
    for (int64_t operand : { 5, 6, 7 }) {
        NumberingTestFixture::Memory memory = numberingTestMemory;
        int64_t expected = valueOfNumberingTestGroup(operand, 0) + valueOfNumberingTestGroup(operand, 1);
        if (operand & 1)
            expected += valueOfNumberingTestGroup(operand, 2) + (operand & 2 ? 1000000 : 2000000);
        AOT_TEST_CHECK(fixture.run(memory, operand) == expected);
    }
}

static void testMachineNumberingArmThatNothingReaches()
{
    NumberingTestFixture fixture;
    Output& out = fixture.out;
    Block* entry = out.m_block;
    Block* taken = out.newBlock();
    Block* notTaken = out.newBlock();
    Block* join = out.newBlock();
    Operation* before = fixture.load(fixture.firstElement);
    ScopeFork fork = out.forkScope();
    out.branch(out.booleanTrue, normalEdge(taken), normalEdge(notTaken));
    AOT_TEST_CHECK(entry->last()->opcode() == Jump && !notTaken->numberOfIncomingEdges());

    out.appendToArm(taken, fork);
    AOT_TEST_CHECK(taken->hasOnlyOnePredecessor() && fixture.load(fixture.firstElement) == before);
    out.jump(join);
    out.appendToArm(notTaken, fork);
    AOT_TEST_CHECK(!notTaken->hasOnlyOnePredecessor());
    fixture.store(fixture.operand, fixture.firstElement);
    out.jump(join);
    out.appendToJoin(join, fork);
    out.ret(fixture.combine(before, fixture.load(fixture.firstElement)));

    compileOrFail(fixture.test);
    NumberingTestFixture::Memory memory = numberingTestMemory;
    AOT_TEST_CHECK(fixture.run(memory, 5) == numberingTestAnswer(3, 3) && memory[0] == 3);
}

static void testMachineNumberingSumIsFoundAgainWhereverItsBlockAloneLeads()
{
    NumberingTestFixture fixture;
    Output& out = fixture.out;
    Block* left = out.newBlock();
    Block* behindLeft = out.newBlock();
    Block* right = out.newBlock();
    Block* join = out.newBlock();
    Block* apart = out.newBlock();
    Operation* word = fixture.load(fixture.firstElement);
    Operation* atFork = out.mul(word, fixture.operand);
    AOT_TEST_CHECK(atFork->opcode() == Mul && out.findsAgain(atFork) && !out.findsAgain(word) && !out.findsAgain(fixture.operand));
    ScopeFork fork = out.forkScope();
    out.branch(fixture.operand, normalEdge(left), normalEdge(right));

    out.appendToArm(left, fork);
    Operation* inLeft = out.add(atFork, word);
    AOT_TEST_CHECK(inLeft->opcode() == Add && inLeft->owner() == left && out.findsAgain(atFork) && out.findsAgain(inLeft));
    out.jump(behindLeft);
    ScopeFork atEndOfLeft = out.forkScope();
    out.appendToSuccessor(behindLeft, atEndOfLeft);
    AOT_TEST_CHECK(out.m_block == behindLeft && out.findsAgain(atFork) && out.findsAgain(inLeft));
    fixture.store(inLeft, fixture.secondElement, 1);
    out.jump(join);

    out.appendToArm(right, fork);
    AOT_TEST_CHECK(out.findsAgain(atFork) && !out.findsAgain(inLeft));
    out.jump(join);

    out.appendToJoin(join, fork);
    AOT_TEST_CHECK(out.findsAgain(atFork) && !out.findsAgain(inLeft));
    out.jump(apart);

    out.appendTo(apart);
    AOT_TEST_CHECK(!out.findsAgain(atFork) && !out.findsAgain(inLeft));
    out.ret(atFork);

    compileOrFail(fixture.test);
    NumberingTestFixture::Memory memory = numberingTestMemory;
    AOT_TEST_CHECK(fixture.run(memory, 5) == 15 && memory[1] == 18);
}

static void testMachineNumberingScopesHoldWhileTableGrows()
{
    constexpr unsigned numberAtFork = 40;
    constexpr unsigned numberInBlock = 1000;
    NumberingTestFixture fixture;
    Output& out = fixture.out;
    Block* left = out.newBlock();
    Block* right = out.newBlock();
    Block* join = out.newBlock();
    auto request = [&](unsigned index) {
        return out.add(fixture.operand, index + 1);
    };

    Vector<Operation*> atFork;
    for (unsigned index = 0; index < numberAtFork; ++index)
        atFork.append(request(index));
    ScopeFork fork = out.forkScope();
    out.branch(fixture.operand, normalEdge(left), normalEdge(right));

    for (Block* block : { left, right, join }) {
        if (block == join)
            out.appendToJoin(block, fork);
        else
            out.appendToArm(block, fork);

        Vector<Operation*> inBlock;
        for (unsigned index = 0; index < numberInBlock; ++index) {
            inBlock.append(request(numberAtFork + index));
            AOT_TEST_CHECK(inBlock.last()->owner() == block);
        }
        for (unsigned index = 0; index < numberAtFork; ++index)
            AOT_TEST_CHECK(request(index) == atFork[index]);
        for (unsigned index = 0; index < numberInBlock; ++index)
            AOT_TEST_CHECK(request(numberAtFork + index) == inBlock[index]);

        if (block != join)
            out.jump(join);
    }
}

static void testMachineNumberingContinuationGoesOnInScope()
{
    NumberingTestFixture fixture;
    Output& out = fixture.out;
    Block* next = out.newBlock();
    Block* slow = out.newBlock();
    Operation* isOdd = out.testNonZero64(fixture.operand, 1);
    NumberingTestGroup first = requestNumberingTestGroup(fixture, 0);
    out.branch(isOdd, rareEdge(slow), normalEdge(next));

    out.appendToContinuation(next);
    AOT_TEST_CHECK(next->hasOnlyOnePredecessor());
    AOT_TEST_CHECK(requestNumberingTestGroup(fixture, 0) == first);
    fixture.store(fixture.operand, fixture.secondElement);
    Operation* behindStore = fixture.load(fixture.secondElement);
    AOT_TEST_CHECK(behindStore != first[0] && behindStore->owner() == next);
    out.ret(fixture.combine(first[0], behindStore));

    out.appendTo(slow);
    NumberingTestGroup inSlow = requestNumberingTestGroup(fixture, 0);
    AOT_TEST_CHECK(!slow->hasOnlyOnePredecessor() && standsInNumberingTestBlock(inSlow, slow));
    out.ret(sumOfNumberingTestGroup(out, inSlow));

    compileOrFail(fixture.test);
    NumberingTestFixture::Memory memory = numberingTestMemory;
    AOT_TEST_CHECK(fixture.run(memory, 5) == valueOfNumberingTestGroup(5, 0) && memory == numberingTestMemory);
    AOT_TEST_CHECK(fixture.run(memory, 6) == numberingTestAnswer(3, 6) && memory[0] == 6);
}

static void testMachineNumberingScopeComesBackWithItsBlock()
{
    NumberingTestFixture fixture;
    Output& out = fixture.out;
    Block* entry = out.m_block;
    Block* middle = out.newBlock();
    Block* last = out.newBlock();
    Operation* isOdd = out.testNonZero64(fixture.operand, 1);
    NumberingTestGroup inEntry = requestNumberingTestGroup(fixture, 0);

    out.appendTo(middle);
    NumberingTestGroup inMiddle = requestNumberingTestGroup(fixture, 0);
    AOT_TEST_CHECK(standsInNumberingTestBlock(inMiddle, middle));

    out.appendTo(last);
    NumberingTestGroup inLast = requestNumberingTestGroup(fixture, 0);
    NumberingTestGroup alsoInLast = requestNumberingTestGroup(fixture, 1);
    AOT_TEST_CHECK(standsInNumberingTestBlock(inLast, last) && standsInNumberingTestBlock(alsoInLast, last));
    out.ret(out.add(sumOfNumberingTestGroup(out, inLast), sumOfNumberingTestGroup(out, alsoInLast)));

    out.appendTo(middle);
    AOT_TEST_CHECK(requestNumberingTestGroup(fixture, 0) == inMiddle);
    NumberingTestGroup alsoInMiddle = requestNumberingTestGroup(fixture, 1);
    AOT_TEST_CHECK(standsInNumberingTestBlock(alsoInMiddle, middle));
    Operation* sumInMiddle = out.add(sumOfNumberingTestGroup(out, inMiddle), sumOfNumberingTestGroup(out, alsoInMiddle));
    out.ret(out.add(out.add(sumInMiddle, sumOfNumberingTestGroup(out, inEntry)), 1000000));

    out.appendTo(entry);
    AOT_TEST_CHECK(requestNumberingTestGroup(fixture, 0) == inEntry);
    AOT_TEST_CHECK(standsInNumberingTestBlock(requestNumberingTestGroup(fixture, 1), entry));
    out.branch(isOdd, normalEdge(middle), normalEdge(last));

    compileOrFail(fixture.test);
    NumberingTestFixture::Memory memory = numberingTestMemory;
    AOT_TEST_CHECK(fixture.run(memory, 5) == 2 * valueOfNumberingTestGroup(5, 0) + valueOfNumberingTestGroup(5, 1) + 1000000);
    AOT_TEST_CHECK(fixture.run(memory, 6) == valueOfNumberingTestGroup(6, 0) + valueOfNumberingTestGroup(6, 1));
}

static void testMachineNumberingSecondVisitSeesNothingOfOtherBlocks()
{
    NumberingTestFixture fixture;
    Output& out = fixture.out;
    Block* entry = out.m_block;
    Block* other = out.newBlock();
    NumberingTestGroup inEntry = requestNumberingTestGroup(fixture, 0);

    out.appendTo(other);
    AOT_TEST_CHECK(standsInNumberingTestBlock(requestNumberingTestGroup(fixture, 0), other));

    out.appendTo(entry);
    AOT_TEST_CHECK(requestNumberingTestGroup(fixture, 0) == inEntry);
    NumberingTestGroup laterInEntry = requestNumberingTestGroup(fixture, 1);
    AOT_TEST_CHECK(standsInNumberingTestBlock(laterInEntry, entry));

    out.appendTo(other);
    NumberingTestGroup first = requestNumberingTestGroup(fixture, 0);
    NumberingTestGroup second = requestNumberingTestGroup(fixture, 1);
    AOT_TEST_CHECK(standsInNumberingTestBlock(first, other) && standsInNumberingTestBlock(second, other));
    out.ret(out.add(sumOfNumberingTestGroup(out, first), sumOfNumberingTestGroup(out, second)));

    out.appendTo(entry);
    AOT_TEST_CHECK(requestNumberingTestGroup(fixture, 0) == inEntry && requestNumberingTestGroup(fixture, 1) == laterInEntry);
    out.jump(other);

    compileOrFail(fixture.test);
    NumberingTestFixture::Memory memory = numberingTestMemory;
    AOT_TEST_CHECK(fixture.run(memory, 5) == valueOfNumberingTestGroup(5, 0) + valueOfNumberingTestGroup(5, 1));
}

static void testMachineNumberingSuccessorReadsOwnCopyOfWhatIsFused()
{
    NumberingTestFixture fixture;
    Output& out = fixture.out;
    Block* next = out.newBlock();
    Block* isSame = out.newBlock();
    Block* isOther = out.newBlock();
    Operation* slot = out.slotBase(fixture.test.procedure().addStackSlot(8));
    auto requestAddress = [&] {
        return out.add(fixture.words, out.shl(out.zeroExtPtr(out.castToInt32(fixture.operand)), 3));
    };
    auto requestElement = [&] {
        return out.load64(out.address(fixture.everyElement, requestAddress()));
    };
    auto requestNarrow = [&] {
        return out.load32(fixture.word(fixture.secondElement, 1));
    };
    auto requestWordOfFrame = [&] {
        return out.load64(out.address(fixture.heaps.frameSlots, slot));
    };

    Operation* word = fixture.load(fixture.firstElement);
    Operation* address = requestAddress();
    Operation* element = requestElement();
    Operation* narrow = requestNarrow();
    Operation* wordOfFrame = requestWordOfFrame();
    Operation* test = out.equal(word, fixture.operand);
    Operation* product = out.mul(word, fixture.operand);
    out.store32(narrow, out.address(fixture.unrelated, fixture.alias, NumberingTestFixture::offsetOfWord(6)));
    AOT_TEST_CHECK(narrow->useCount() == 1);
    out.jump(next);
    ScopeFork atEnd = out.forkScope();

    out.appendToSuccessor(next, atEnd);
    AOT_TEST_CHECK(requestElement() == element && requestAddress() == address && requestNarrow() == narrow && requestWordOfFrame() == wordOfFrame);
    AOT_TEST_CHECK(out.equal(word, fixture.operand) == test && out.mul(word, fixture.operand) == product && next->isEmpty());

    Operation* neighbor = out.load64(out.address(fixture.cousin, requestAddress(), NumberingTestFixture::offsetOfWord(1)));
    Operation* ownAddress = neighbor->child(0);
    AOT_TEST_CHECK(neighbor->owner() == next && ownAddress != address && ownAddress->opcode() == Add && ownAddress->owner() == next);
    AOT_TEST_CHECK(ownAddress->child(0) == fixture.words && ownAddress->child(1) != address->child(1) && ownAddress->child(1)->opcode() == Shl && ownAddress->child(1)->owner() == next);
    AOT_TEST_CHECK(requestAddress() == ownAddress);

    Operation* widened = out.signExt32To64(requestNarrow());
    AOT_TEST_CHECK(widened->opcode() == SExt32 && widened->owner() == next && (widened->child(0) != narrow) == isX86_64());
    Operation* mixed = out.bitXor(requestWordOfFrame(), fixture.operand);
    AOT_TEST_CHECK(mixed->owner() == next && mixed->child(0) != wordOfFrame && mixed->child(1) != wordOfFrame);

    Operation* sum = out.add(out.mul(word, fixture.operand), widened);
    AOT_TEST_CHECK(sum->child(0) == product || sum->child(1) == product);
    out.branch(out.equal(word, fixture.operand), normalEdge(isSame), normalEdge(isOther));
    Operation* ownTest = next->last()->child(0);
    AOT_TEST_CHECK(ownTest != test && ownTest->opcode() == Equal && ownTest->owner() == next && (ownTest->child(0) == word || ownTest->child(1) == word));

    out.appendTo(isSame);
    out.ret(out.int64Zero);
    out.appendTo(isOther);
    out.ret(fixture.combine(element, neighbor, sum));

    compileOrFail(fixture.test);
    NumberingTestFixture::Memory memory = numberingTestMemory;
    AOT_TEST_CHECK(fixture.run(memory, 1) == numberingTestAnswer(4, 6, 3 + 4));
    AOT_TEST_CHECK(!fixture.run(memory, 3));
}

static void testMachineNumberingOwnCopyIsNotMadeOfLoadThatHasEnded()
{
    NumberingTestFixture fixture;
    Output& out = fixture.out;
    Block* next = out.newBlock();
    auto requestNarrow = [&] {
        return out.load32(fixture.word(fixture.firstElement));
    };
    Operation* narrow = requestNarrow();
    out.store32(narrow, out.address(fixture.unrelated, fixture.alias, NumberingTestFixture::offsetOfWord(6)));
    out.jump(next);
    ScopeFork atEnd = out.forkScope();

    out.appendToSuccessor(next, atEnd);
    Operation* found = requestNarrow();
    AOT_TEST_CHECK(found == narrow);
    fixture.store(fixture.operand, fixture.firstElement);
    Operation* widened = out.signExt32To64(found);
    AOT_TEST_CHECK(widened->child(0) == narrow);
    Operation* behindStore = out.signExt32To64(requestNarrow());
    AOT_TEST_CHECK(behindStore->child(0) != narrow && behindStore->child(0)->owner() == next);
    out.ret(fixture.combine(widened, behindStore));

    compileOrFail(fixture.test);
    NumberingTestFixture::Memory memory = numberingTestMemory;
    AOT_TEST_CHECK(fixture.run(memory, 5) == numberingTestAnswer(3, 5) && memory[0] == 5);
}

static void testMachineNumberingWordFilledOnceIsLoadedOnce()
{
    NumberingTestFixture fixture;
    Output& out = fixture.out;
    Block* next = out.newBlock();
    Block* isFilled = out.newBlock();
    Block* isEmpty = out.newBlock();
    Operation* slot = out.slotBase(fixture.test.procedure().addStackSlot(8));
    auto requestWord = [&] {
        return out.load64(out.address(fixture.heaps.AOTInstance_fieldsFilledOnce, slot));
    };
    out.store64(out.int64Zero, out.address(fixture.heaps.frameSlots, slot));
    Operation* word = requestWord();
    AOT_TEST_CHECK(word->has(OperationFlag::ReadsImmutableHeap) && word->has(OperationFlag::ReadsWordFilledOnce));
    out.store64(fixture.operand, out.address(fixture.heaps.frameSlots, slot));
    out.jump(next);
    ScopeFork atEnd = out.forkScope();

    out.appendToSuccessor(next, atEnd);
    Operation* found = requestWord();
    AOT_TEST_CHECK(found == word);
    out.branch(found, normalEdge(isFilled), normalEdge(isEmpty));
    AOT_TEST_CHECK(next->last()->child(0) == word);

    out.appendTo(isFilled);
    out.ret(found);
    out.appendTo(isEmpty);
    out.ret(out.constInt64(-1));

    compileOrFail(fixture.test);
    NumberingTestFixture::Memory memory = numberingTestMemory;
    AOT_TEST_CHECK(fixture.run(memory, 5) == -1);
}

static void testMachineNumberingLoadIsMadeAgainOnRequest()
{
    NumberingTestFixture fixture;
    Output& out = fixture.out;
    Operation* found = fixture.load(fixture.firstElement, 1);
    Operation* madeAgain = out.reload(found);
    AOT_TEST_CHECK(madeAgain != found && madeAgain->opcode() == Load && madeAgain->type() == Int64 && madeAgain->owner() == out.m_block);
    AOT_TEST_CHECK(madeAgain->child(0) == found->child(0) && madeAgain->offset() == found->offset() && madeAgain->heap() == found->heap());
    AOT_TEST_CHECK(fixture.load(fixture.firstElement, 1) == madeAgain);
    out.ret(fixture.combine(found, madeAgain));

    compileOrFail(fixture.test);
    NumberingTestFixture::Memory memory = numberingTestMemory;
    AOT_TEST_CHECK(fixture.run(memory, 5) == numberingTestAnswer(4, 4));
}

static void testMachineNumberingBlockThatLostAnEdgeStartsEmpty()
{
    for (bool firstEdgeIsTakenAway : { false, true }) {
        NumberingTestFixture fixture;
        Output& out = fixture.out;
        Block* entry = out.m_block;
        Block* other = out.newBlock();
        Block* next = out.newBlock();
        fixture.load(fixture.firstElement);
        out.jump(unlikelyEdge(next));
        ScopeFork atEnd = out.forkScope();
        AOT_TEST_CHECK(next->singlePredecessor() == entry && next->classOfSingleIncomingEdge() == EdgeClass::Unlikely);

        out.appendTo(other);
        out.jump(next);
        AOT_TEST_CHECK(next->numberOfIncomingEdges() == 2 && !next->singlePredecessor());
        (firstEdgeIsTakenAway ? entry : other)->clearSuccessors();
        AOT_TEST_CHECK(next->numberOfIncomingEdges() == 1 && next->singlePredecessor() == (firstEdgeIsTakenAway ? other : entry));
        AOT_TEST_CHECK(next->classOfSingleIncomingEdge() == (firstEdgeIsTakenAway ? EdgeClass::Normal : EdgeClass::Unlikely));

        out.appendToSuccessor(next, atEnd);
        AOT_TEST_CHECK(fixture.load(fixture.firstElement)->owner() == (firstEdgeIsTakenAway ? next : entry));
    }
}

static void testMachineNumberingSuccessorReadsOwnCopyOfTwoComparisons()
{
    NumberingTestFixture fixture;
    Output& out = fixture.out;
    Block* next = out.newBlock();
    Block* isNeither = out.newBlock();
    Block* isEitherAgain = out.newBlock();
    Operation* word = fixture.load(fixture.firstElement);
    auto requestTest = [&] {
        return out.bitOr(out.equal(word, fixture.operand), out.equal(word, 7));
    };
    Operation* test = requestTest();
    AOT_TEST_CHECK(test->opcode() == BitOr);
    out.branch(test, normalEdge(next), normalEdge(isNeither));
    ScopeFork atEnd = out.forkScope();

    out.appendToSuccessor(next, atEnd);
    AOT_TEST_CHECK(requestTest() == test && next->isEmpty());
    out.branch(requestTest(), normalEdge(isEitherAgain), normalEdge(isNeither));
    Operation* ownTest = next->last()->child(0);
    AOT_TEST_CHECK(ownTest != test && ownTest->opcode() == BitOr && ownTest->owner() == next);
    AOT_TEST_CHECK(ownTest->child(0)->opcode() == Equal && ownTest->child(0)->owner() == next && ownTest->child(1)->opcode() == Equal && ownTest->child(1)->owner() == next);
    AOT_TEST_CHECK(test->useCount() == 1 && test->child(0)->useCount() == 1 && test->child(1)->useCount() == 1);

    out.appendTo(isEitherAgain);
    out.ret(out.constInt64(1));
    out.appendTo(isNeither);
    out.ret(out.int64Zero);

    compileOrFail(fixture.test);
    NumberingTestFixture::Memory memory = numberingTestMemory;
    AOT_TEST_CHECK(fixture.run(memory, 3) == 1 && !fixture.run(memory, 5));
    memory[0] = 7;
    AOT_TEST_CHECK(fixture.run(memory, 5) == 1);
}

static void testMachineNumberingOwnCopyOfNarrowReferenceIsMarked()
{
    NumberingTestFixture fixture;
    Output& out = fixture.out;
    Block* next = out.newBlock();
    auto requestReference = [&] {
        Operation* reference = out.load32(fixture.word(fixture.firstElement));
        reference->markAsNarrowReference();
        return reference;
    };
    Operation* reference = requestReference();
    out.store32(reference, out.address(fixture.unrelated, fixture.alias, NumberingTestFixture::offsetOfWord(6)));
    out.jump(next);
    ScopeFork atEnd = out.forkScope();

    out.appendToSuccessor(next, atEnd);
    AOT_TEST_CHECK(requestReference() == reference);
    Operation* widened = out.zeroExtPtr(reference);
    AOT_TEST_CHECK(widened->opcode() == ZExt32 && widened->child(0)->isNarrowReference() && (widened->child(0) != reference) == isX86_64());
    out.ret(widened);

    compileOrFail(fixture.test);
    NumberingTestFixture::Memory memory = numberingTestMemory;
    AOT_TEST_CHECK(fixture.run(memory, 5) == 3);
}

static void testMachineNumberingReusedLoadIsVerified()
{
    for (auto [declaresWrite, crossesEdge] : { std::pair { false, true }, std::pair { true, true }, std::pair { false, false }, std::pair { true, false } }) {
        NumberingTestFixture fixture;
        Output& out = fixture.out;
        unsigned numberOfChecks = 0;
        out.verifyReusedLoads({ fixture.cousin.index() }, [&](Operation* readSameBits) {
            ++numberOfChecks;
            PatchOperation* check = out.check(out.isZero32(readSameBits));
            check->setGenerator([](CCallHelpers& jit, const PatchParameters& params) {
                jit.move(CCallHelpers::TrustedImm64(-1), GPRInfo::returnValueGPR);
                if (params.frameIsSetUp())
                    emitFrameTeardown(jit, params.frame());
                jit.ret();
            });
        });

        Operation* word = fixture.load(fixture.firstElement);
        Operation* exempt = fixture.load(fixture.cousin, 2);
        Operation* fixed = fixture.load(fixture.immutable, 4);
        fixture.appendWriter(0)->effects.writes = fixture.test.procedure().heapSet(declaresWrite ? fixture.firstElement : fixture.unrelated);
        if (crossesEdge) {
            Block* next = out.newBlock();
            out.jump(next);
            ScopeFork atEnd = out.forkScope();
            out.appendToSuccessor(next, atEnd);
        }
        AOT_TEST_CHECK(fixture.load(fixture.cousin, 2) == exempt && fixture.load(fixture.immutable, 4) == fixed && !numberOfChecks);
        Operation* found = fixture.load(fixture.firstElement);
        AOT_TEST_CHECK((found == word) == !declaresWrite && numberOfChecks == (declaresWrite ? 0u : 1u));
        out.ret(fixture.combine(found, exempt, fixed));

        compileOrFail(fixture.test);
        NumberingTestFixture::Memory memory = numberingTestMemory;
        AOT_TEST_CHECK(fixture.run(memory, 5) == (declaresWrite ? numberingTestAnswer(5, 6, 30) : -1));
        memory = numberingTestMemory;
        AOT_TEST_CHECK(fixture.run(memory, 3) == numberingTestAnswer(3, 6, 30));
    }
}

static void testMachineNumberingStoreOverLoadThatStandsStopsBuild()
{
    struct Case {
        bool isVerified;
        bool endsLoad;
        unsigned wordStored;
        bool stops;
    };
    for (auto [isVerified, endsLoad, wordStored, stops] : { Case { true, false, 0, true }, Case { true, true, 0, false }, Case { true, false, 1, false }, Case { false, false, 0, false } }) {
        NumberingTestFixture fixture;
        Output& out = fixture.out;
        if (isVerified)
            out.verifyReusedLoads({ fixture.cousin.index() }, [](Operation*) { });
        Operation* word = fixture.load(fixture.firstElement);
        out.store64(fixture.operand, out.address(endsLoad ? fixture.firstElement : fixture.unrelated, fixture.words, NumberingTestFixture::offsetOfWord(wordStored)));
        out.ret(word);
        const std::optional<Failure>& failure = fixture.test.procedure().failure();
        AOT_TEST_CHECK(stops ? failure && failure->kind == FailureKind::MalformedProcedure : !failure);
    }
}

static void testMachineNumberingStoreOverLoadThatStandsIsJudgedByWaysFromEntry()
{
    enum class Shape : uint8_t { DeadBlock, DeadThenDead, DeadForkAndJoin, RingThatNothingEnters, LiveThenLive, DeadThenDeadLedIntoLater, DeadBlockLedIntoLater };
    struct Case {
        Shape shape;
        bool stopsAtStore;
        bool stopsBeforeGeneration;
    };
    for (auto [shape, stopsAtStore, stopsBeforeGeneration] : {
        Case { Shape::DeadBlock, false, false },
        Case { Shape::DeadThenDead, false, false },
        Case { Shape::DeadForkAndJoin, false, false },
        Case { Shape::RingThatNothingEnters, false, false },
        Case { Shape::LiveThenLive, true, true },
        Case { Shape::DeadThenDeadLedIntoLater, false, true },
        Case { Shape::DeadBlockLedIntoLater, false, true },
    }) {
        NumberingTestFixture fixture;
        Output& out = fixture.out;
        out.verifyReusedLoads({ fixture.cousin.index() }, [](Operation*) { });
        Block* taken = out.newBlock();
        Block* first = out.newBlock();
        Block* end = out.newBlock();
        Operation* word = fixture.load(fixture.firstElement);
        ScopeFork fork = out.forkScope();
        bool firstIsLive = shape == Shape::LiveThenLive;
        out.branch(firstIsLive ? fixture.operand : out.booleanTrue, normalEdge(taken), normalEdge(first));
        AOT_TEST_CHECK(first->numberOfIncomingEdges() == firstIsLive);

        out.appendToArm(first, fork);
        Block* holder = first;
        if (shape == Shape::DeadThenDead || shape == Shape::LiveThenLive || shape == Shape::DeadThenDeadLedIntoLater) {
            holder = out.newBlock();
            out.jump(holder);
            out.appendToContinuation(holder);
            AOT_TEST_CHECK(holder->singlePredecessor() == first);
        } else if (shape == Shape::DeadForkAndJoin) {
            Block* left = out.newBlock();
            Block* right = out.newBlock();
            holder = out.newBlock();
            ScopeFork inner = out.forkScope();
            out.branch(fixture.operand, normalEdge(left), normalEdge(right));
            out.appendToArm(left, inner);
            out.jump(holder);
            out.appendToArm(right, inner);
            out.jump(holder);
            out.appendToJoin(holder, inner);
            AOT_TEST_CHECK(holder->numberOfIncomingEdges() == 2);
        }
        const std::optional<Failure>& failure = fixture.test.procedure().failure();
        AOT_TEST_CHECK(!failure);
        out.store64(fixture.operand, out.address(fixture.unrelated, fixture.words, NumberingTestFixture::offsetOfWord(0)));
        AOT_TEST_CHECK(stopsAtStore ? failure && failure->kind == FailureKind::MalformedProcedure : !failure);
        if (stopsAtStore)
            continue;
        out.jump(shape == Shape::RingThatNothingEnters ? first : end);
        AOT_TEST_CHECK(shape != Shape::RingThatNothingEnters || first->singlePredecessor() == first);

        out.appendToArm(taken, fork);
        bool isLedIntoLater = shape == Shape::DeadThenDeadLedIntoLater || shape == Shape::DeadBlockLedIntoLater;
        if (isLedIntoLater)
            out.branch(fixture.operand, normalEdge(end), rareEdge(first));
        else
            out.jump(end);
        out.appendTo(end);
        out.ret(word);
        AOT_TEST_CHECK(!failure);
        AOT_TEST_CHECK(fixture.test.prepare() == !stopsBeforeGeneration);
        AOT_TEST_CHECK(stopsBeforeGeneration ? failure && failure->kind == FailureKind::MalformedProcedure : !failure);
    }
}

static constexpr const char* numberingTestRequestVariable = "TESTAOT_ADD_SECOND_EDGE_ON_REQUEST";
static constexpr ASCIILiteral numberingTestOfSecondEdgeIntoContinuation = "machine/numbering-on-request/second-edge-into-continuation"_s;
static constexpr ASCIILiteral numberingTestOfSecondEdgeIntoJoin = "machine/numbering-on-request/second-edge-into-join"_s;
static constexpr ASCIILiteral lineAheadOfSecondEdgeInNumberingTest = "adds the second edge"_s;

static void addSecondEdgeInNumberingTestOnRequest(bool isContinuation)
{
    if (!getenv(numberingTestRequestVariable))
        return;

    NumberingTestFixture fixture;
    Output& out = fixture.out;
    Block* next = out.newBlock();
    Block* other = out.newBlock();
    out.branch(fixture.operand, normalEdge(next), rareEdge(other));
    if (isContinuation)
        out.appendToContinuation(next);
    else
        out.appendTo(next);
    out.ret(fixture.operand);
    out.appendTo(other);
    dataLogLn(lineAheadOfSecondEdgeInNumberingTest);
    out.jump(next);
}

static void testMachineNumberingAddsSecondEdgeIntoContinuationOnRequest()
{
    addSecondEdgeInNumberingTestOnRequest(true);
}

static void testMachineNumberingAddsSecondEdgeIntoJoinOnRequest()
{
    addSecondEdgeInNumberingTestOnRequest(false);
}

static void setNumberingTestRequest(bool isRequested)
{
#if OS(WINDOWS)
    SetEnvironmentVariableA(numberingTestRequestVariable, isRequested ? "1" : nullptr);
#else
    if (isRequested)
        setenv(numberingTestRequestVariable, "1", 1);
    else
        unsetenv(numberingTestRequestVariable);
#endif
}

static ChildProcessResult runNumberingTestOnRequest(ASCIILiteral test)
{
    std::array<String, 3> arguments { String(nativeSubcommandName), String(singleProcessFlag), String(test) };
    setNumberingTestRequest(true);
    ChildProcessResult result = ChildProcessLauncher(String(programPath()), { }).run(arguments);
    setNumberingTestRequest(false);
    return result;
}

static void testMachineNumberingSecondEdgeIntoContinuationAsserts()
{
    ChildProcessResult allowed = runNumberingTestOnRequest(numberingTestOfSecondEdgeIntoJoin);
    AOT_TEST_CHECK(allowed.exitedWith(0) && allowed.output.contains(lineAheadOfSecondEdgeInNumberingTest));
    ChildProcessResult refused = runNumberingTestOnRequest(numberingTestOfSecondEdgeIntoContinuation);
    AOT_TEST_CHECK(refused.outcome == ChildProcessOutcome::Crashed && refused.output.contains(lineAheadOfSecondEdgeInNumberingTest));
}

static void testMachineNumberingPurePatchesAreSharedByIDAndChildren()
{
    NumberingTestFixture fixture;
    Output& out = fixture.out;
    Vector<Operation*, 5> children;
    for (unsigned index = 0; index < 5; ++index)
        children.append(out.add(fixture.operand, index + 1));
    auto request = [&](uint64_t id, bool dependsOnChildren, std::span<Operation* const> operands) {
        return requestNumberingTestPurePatch(fixture, { id, dependsOnChildren }, operands);
    };

    Operation* withoutChildren = request(7, false, { });
    AOT_TEST_CHECK(request(7, false, { }) == withoutChildren && request(8, false, { }) != withoutChildren);
    Operation* whateverItsChild = request(9, false, children.span().first(1));
    AOT_TEST_CHECK(request(9, false, children.span().subspan(1, 1)) == whateverItsChild);

    Operation* ofAllChildren = nullptr;
    for (size_t count : { 1, 3, 5 }) {
        uint64_t id = 100 + 2 * count;
        std::span<Operation* const> operands = children.span().first(count);
        ofAllChildren = request(id, true, operands);
        AOT_TEST_CHECK(ofAllChildren->opcode() == Patch && ofAllChildren->numChildren() == count);
        AOT_TEST_CHECK(request(id, true, operands) == ofAllChildren);
        AOT_TEST_CHECK(request(id + 1, true, operands) != ofAllChildren);
        AOT_TEST_CHECK(request(id, true, operands.first(count - 1)) != ofAllChildren);

        Vector<Operation*, 5> otherOperands(operands);
        otherOperands.last() = fixture.operand;
        AOT_TEST_CHECK(request(id, true, otherOperands.span()) != ofAllChildren);
        if (count > 1) {
            Vector<Operation*, 5> reversedOperands(operands);
            std::ranges::reverse(reversedOperands.mutableSpan());
            AOT_TEST_CHECK(request(id, true, reversedOperands.span()) != ofAllChildren);
        }
    }

    fixture.store(fixture.operand, fixture.everything);
    fixture.appendCall();
    AOT_TEST_CHECK(request(7, false, { }) == withoutChildren && request(110, true, children.span()) == ofAllChildren);
    out.ret(fixture.combine(withoutChildren, ofAllChildren));

    compileOrFail(fixture.test);
    NumberingTestFixture::Memory memory = numberingTestMemory;
    AOT_TEST_CHECK(fixture.run(memory, 5) == numberingTestAnswer(7, 110 + 5 * 5 + 15));
}

static Operation* requestNumberingTestReader(NumberingTestFixture& fixture, const PureFunction& function, const AbstractHeap& heap, unsigned index)
{
    if (Operation* existing = fixture.out.findPurePatch(function, Int64, singleElementSpan(fixture.words)))
        return existing;
    int32_t offset = static_cast<int32_t>(NumberingTestFixture::offsetOfWord(index));
    PatchOperation* patch = fixture.out.patch(Int64);
    patch->appendSomeRegister(fixture.words);
    patch->effects = Effects::none();
    patch->effects.reads = fixture.test.procedure().heapSet(heap);
    patch->pureFunction = function;
    patch->setRunsWithoutFrame(true);
    patch->setGenerator([offset](CCallHelpers& jit, const PatchParameters& params) {
        jit.load64(CCallHelpers::Address(params[1].gpr(), offset), params[0].gpr());
    });
    fixture.out.recordPurePatch(patch);
    return patch;
}

static void testMachineNumberingPatchThatDependsOnMemoryIsSharedUntilWriteThere()
{
    constexpr PureFunction reader { 70, true, false, true };
    {
        NumberingTestFixture fixture;
        auto read = [&] {
            return requestNumberingTestReader(fixture, reader, fixture.firstElement, 0);
        };
        Operation* first = read();
        AOT_TEST_CHECK(first->opcode() == Patch && read() == first);
        fixture.store(fixture.operand, fixture.secondElement, 1);
        fixture.store(fixture.operand, fixture.cousin, 2);
        fixture.store(fixture.operand, fixture.unrelated, 4);
        AOT_TEST_CHECK(read() == first);

        fixture.store(fixture.operand, fixture.firstElement);
        Operation* second = read();
        AOT_TEST_CHECK(second != first && read() == second);

        fixture.store(fixture.out.add(fixture.operand, 1), fixture.everyElement);
        Operation* third = read();
        AOT_TEST_CHECK(third != second && third != first && read() == third);
        fixture.out.ret(fixture.combine(first, second, third));

        compileOrFail(fixture.test);
        NumberingTestFixture::Memory memory = numberingTestMemory;
        AOT_TEST_CHECK(fixture.run(memory, 5) == numberingTestAnswer(3, 5, 6));
    }
    {
        NumberingTestFixture fixture;
        auto read = [&] {
            return requestNumberingTestReader(fixture, reader, fixture.arrays, 0);
        };
        Operation* first = read();
        fixture.store(fixture.operand, fixture.cousin, 2);
        Operation* second = read();
        AOT_TEST_CHECK(second != first);
        fixture.appendWriter(0)->effects.writes = fixture.test.procedure().heapSet(fixture.firstElement);
        Operation* third = read();
        AOT_TEST_CHECK(third != second);
        fixture.appendCall();
        AOT_TEST_CHECK(read() != third);
    }
    {
        NumberingTestFixture fixture;
        Output& out = fixture.out;
        auto read = [&] {
            return requestNumberingTestReader(fixture, reader, fixture.firstElement, 0);
        };
        Block* writes = out.newBlock();
        Block* passes = out.newBlock();
        Block* join = out.newBlock();
        Operation* first = read();
        ScopeFork fork = out.forkScope();
        out.branch(out.notZero64(fixture.operand), normalEdge(passes), normalEdge(writes));
        out.appendToArm(passes, fork);
        AOT_TEST_CHECK(read() == first);
        out.jump(join);
        out.appendToArm(writes, fork);
        fixture.store(fixture.operand, fixture.firstElement);
        out.jump(join);
        out.appendToJoin(join, fork);
        AOT_TEST_CHECK(read() != first);
    }
    {
        constexpr PureFunction ofChildrenAlone { 71, true };
        NumberingTestFixture fixture;
        Operation* first = requestNumberingTestReader(fixture, ofChildrenAlone, fixture.firstElement, 0);
        fixture.store(fixture.operand, fixture.firstElement);
        AOT_TEST_CHECK(requestNumberingTestReader(fixture, ofChildrenAlone, fixture.firstElement, 0) == first);
    }
}

static Operation* appendNumberingTestWordReadOrMade(NumberingTestFixture& fixture)
{
    Output& out = fixture.out;
    Block* reads = out.newBlock();
    Block* makes = out.newBlock();
    Block* join = out.newBlock();
    Operation* word = fixture.load(fixture.firstElement);
    ScopeFork fork = out.forkScope();
    out.branch(out.notZero64(word), normalEdge(reads), rareEdge(makes));
    out.appendToArm(reads, fork);
    ValueFromBlock read = out.anchor(word);
    out.jump(join);
    out.appendToArm(makes, fork);
    ValueFromBlock made = out.anchor(fixture.operand);
    out.jump(join);
    out.appendToJoin(join, fork);
    return out.phi(Int64, read, made);
}

static void testMachineNumberingPhiStandsInPlaceOfPurePatch()
{
    constexpr PureFunction function { 60 };
    auto find = [&](NumberingTestFixture& fixture) {
        return fixture.out.findPurePatch(function, Int64, { });
    };
    {
        NumberingTestFixture fixture;
        Output& out = fixture.out;
        Operation* phi = appendNumberingTestWordReadOrMade(fixture);
        AOT_TEST_CHECK(phi->opcode() == Phi && !find(fixture));
        out.recordInPlaceOfPurePatch(function, phi);
        AOT_TEST_CHECK(find(fixture) == phi);
        AOT_TEST_CHECK(!out.findPurePatch({ 61 }, Int64, { }) && !out.findPurePatch(function, Int32, { }));
        AOT_TEST_CHECK(!out.findPurePatch({ 60, true }, Int64, singleElementSpan(fixture.operand)));

        fixture.store(fixture.operand, fixture.everything, 1);
        fixture.appendCall();
        Operation* found = find(fixture);
        AOT_TEST_CHECK(found == phi);
        out.ret(fixture.combine(phi, found));

        compileOrFail(fixture.test);
        NumberingTestFixture::Memory memory = numberingTestMemory;
        AOT_TEST_CHECK(fixture.run(memory, 5) == numberingTestAnswer(3, 3));
        memory[0] = 0;
        AOT_TEST_CHECK(fixture.run(memory, 5) == numberingTestAnswer(5, 5));
    }

    ProcedureTraits traits;
    traits.savesFrameOnSuspend = true;
    for (bool isSuspension : { true, false }) {
        NumberingTestFixture fixture(traits);
        Operation* phi = appendNumberingTestWordReadOrMade(fixture);
        fixture.out.recordInPlaceOfPurePatch(function, phi);
        PatchOperation* patch = fixture.appendCall();
        patch->effects = Effects::none();
        patch->setIsSuspension(isSuspension);
        AOT_TEST_CHECK(find(fixture) == (isSuspension ? nullptr : phi));
    }

    {
        NumberingTestFixture fixture;
        Output& out = fixture.out;
        Block* left = out.newBlock();
        Block* right = out.newBlock();
        Block* join = out.newBlock();
        ScopeFork fork = out.forkScope();
        out.branch(out.testNonZero64(fixture.operand, 1), normalEdge(left), normalEdge(right));
        out.appendToArm(left, fork);
        Operation* phi = appendNumberingTestWordReadOrMade(fixture);
        out.recordInPlaceOfPurePatch(function, phi);
        AOT_TEST_CHECK(find(fixture) == phi);
        out.jump(join);
        out.appendToArm(right, fork);
        AOT_TEST_CHECK(!find(fixture));
        out.jump(join);
        out.appendToJoin(join, fork);
        AOT_TEST_CHECK(!find(fixture));
    }
}

static void testMachineNumberingPhiStandsInPlaceOfReadOfMemory()
{
    constexpr PureFunction function { 62, true, false, true };
    auto find = [&](NumberingTestFixture& fixture, Operation* operand) {
        return fixture.out.findPurePatch(function, Int64, singleElementSpan(operand));
    };
    auto record = [&](NumberingTestFixture& fixture) {
        Operation* phi = appendNumberingTestWordReadOrMade(fixture);
        AOT_TEST_CHECK(phi->opcode() == Phi && !find(fixture, fixture.words));
        fixture.out.recordInPlaceOfReadOfMemory(function, phi, fixture.words, fixture.firstElement);
        AOT_TEST_CHECK(find(fixture, fixture.words) == phi);
        return phi;
    };
    {
        NumberingTestFixture fixture;
        Output& out = fixture.out;
        record(fixture);
        AOT_TEST_CHECK(!find(fixture, fixture.alias) && !find(fixture, fixture.operand));
        AOT_TEST_CHECK(!out.findPurePatch({ 63, true, false, true }, Int64, singleElementSpan(fixture.words)));
        AOT_TEST_CHECK(!out.findPurePatch(function, Int32, singleElementSpan(fixture.words)));
        AOT_TEST_CHECK(!out.findPurePatch({ 62 }, Int64, { }));
    }
    for (const AbstractHeap NumberingTestFixture::* written : { &NumberingTestFixture::secondElement, &NumberingTestFixture::cousin, &NumberingTestFixture::unrelated }) {
        NumberingTestFixture fixture;
        Operation* phi = record(fixture);
        fixture.store(fixture.operand, fixture.*written, 1);
        AOT_TEST_CHECK(find(fixture, fixture.words) == phi);
    }
    for (const AbstractHeap NumberingTestFixture::* written : { &NumberingTestFixture::firstElement, &NumberingTestFixture::everyElement, &NumberingTestFixture::arrays, &NumberingTestFixture::everything }) {
        NumberingTestFixture fixture;
        Output& out = fixture.out;
        Operation* phi = record(fixture);
        fixture.store(fixture.operand, fixture.*written);
        AOT_TEST_CHECK(!find(fixture, fixture.words));
        Operation* again = appendNumberingTestWordReadOrMade(fixture);
        AOT_TEST_CHECK(again != phi);
        out.ret(fixture.combine(phi, again));

        compileOrFail(fixture.test);
        NumberingTestFixture::Memory memory = numberingTestMemory;
        AOT_TEST_CHECK(fixture.run(memory, 5) == numberingTestAnswer(3, 5));
    }
    {
        NumberingTestFixture fixture;
        record(fixture);
        fixture.appendCall();
        AOT_TEST_CHECK(!find(fixture, fixture.words));
    }
    for (const AbstractHeap NumberingTestFixture::* written : { &NumberingTestFixture::firstElement, &NumberingTestFixture::secondElement, &NumberingTestFixture::cousin }) {
        NumberingTestFixture fixture;
        Operation* phi = appendNumberingTestWordReadOrMade(fixture);
        fixture.out.recordInPlaceOfReadOfMemory(function, phi, fixture.words, fixture.everyElement);
        AOT_TEST_CHECK(find(fixture, fixture.words) == phi);
        fixture.store(fixture.operand, fixture.*written, 1);
        AOT_TEST_CHECK(find(fixture, fixture.words) == (written == &NumberingTestFixture::cousin ? phi : nullptr));
    }
    for (bool writesInSuccessor : { false, true }) {
        NumberingTestFixture fixture;
        Output& out = fixture.out;
        Operation* phi = record(fixture);
        Block* taken = out.newBlock();
        Block* notTaken = out.newBlock();
        ScopeFork fork = out.forkScope();
        out.branch(out.testNonZero64(fixture.operand, 1), normalEdge(taken), normalEdge(notTaken));
        out.appendToSuccessor(taken, fork);
        AOT_TEST_CHECK(find(fixture, fixture.words) == phi);
        if (writesInSuccessor)
            fixture.store(fixture.operand, fixture.firstElement);
        AOT_TEST_CHECK(find(fixture, fixture.words) == (writesInSuccessor ? nullptr : phi));
        out.ret(phi);
        out.appendToSuccessor(notTaken, fork);
        AOT_TEST_CHECK(find(fixture, fixture.words) == (writesInSuccessor ? nullptr : phi));
        out.ret(phi);
    }
    auto recordPhiMadeWithoutLoads = [&](NumberingTestFixture& fixture) {
        Output& out = fixture.out;
        Block* left = out.newBlock();
        Block* right = out.newBlock();
        Block* join = out.newBlock();
        ScopeFork fork = out.forkScope();
        out.branch(out.testNonZero64(fixture.operand, 1), normalEdge(left), normalEdge(right));
        out.appendToArm(left, fork);
        ValueFromBlock first = out.anchor(fixture.operand);
        out.jump(join);
        out.appendToArm(right, fork);
        ValueFromBlock second = out.anchor(fixture.alias);
        out.jump(join);
        out.appendToJoin(join, fork);
        Operation* phi = out.phi(Int64, first, second);
        AOT_TEST_CHECK(phi->opcode() == Phi);
        out.recordInPlaceOfReadOfMemory(function, phi, fixture.words, fixture.firstElement);
        AOT_TEST_CHECK(find(fixture, fixture.words) == phi);
    };
    {
        NumberingTestFixture fixture;
        fixture.load(fixture.firstElement);
        PatchOperation* first = fixture.appendWriter(0);
        first->effects.writes = fixture.test.procedure().heapSet(fixture.firstElement);
        recordPhiMadeWithoutLoads(fixture);
        PatchOperation* second = fixture.appendWriter(0);
        second->effects.writes = first->effects.writes;
        AOT_TEST_CHECK(!find(fixture, fixture.words));
    }
    {
        NumberingTestFixture fixture;
        fixture.load(fixture.firstElement);
        fixture.appendCall();
        recordPhiMadeWithoutLoads(fixture);
        fixture.store(fixture.operand, fixture.firstElement);
        AOT_TEST_CHECK(!find(fixture, fixture.words));
    }
    ProcedureTraits traits;
    traits.savesFrameOnSuspend = true;
    for (bool isSuspension : { true, false }) {
        NumberingTestFixture fixture(traits);
        Operation* phi = record(fixture);
        PatchOperation* patch = fixture.appendCall();
        patch->effects = Effects::none();
        patch->setIsSuspension(isSuspension);
        AOT_TEST_CHECK(find(fixture, fixture.words) == (isSuspension ? nullptr : phi));
    }
    {
        NumberingTestFixture fixture;
        Output& out = fixture.out;
        Block* left = out.newBlock();
        Block* right = out.newBlock();
        Block* join = out.newBlock();
        ScopeFork fork = out.forkScope();
        out.branch(out.testNonZero64(fixture.operand, 1), normalEdge(left), normalEdge(right));
        out.appendToArm(left, fork);
        record(fixture);
        out.jump(join);
        out.appendToArm(right, fork);
        AOT_TEST_CHECK(!find(fixture, fixture.words));
        out.jump(join);
        out.appendToJoin(join, fork);
        AOT_TEST_CHECK(!find(fixture, fixture.words));
    }
}

static void testMachineNumberingTruncationOfConvertedInteger()
{
    NumberingTestFixture fixture;
    Output& out = fixture.out;
    PureFunction truncation { 12, true, true };
    Operation* integer = out.castToInt32(fixture.operand);
    Operation* number = out.intToDouble(integer);
    AOT_TEST_CHECK(number->opcode() == IToD);
    AOT_TEST_CHECK(out.findPurePatch(truncation, Int64, singleElementSpan(number)) == out.signExt32To64(integer));
    AOT_TEST_CHECK(!out.findPurePatch({ 12, true }, Int64, singleElementSpan(number)));

    Operation* bounded = out.zeroExtPtr(integer);
    Operation* boundedNumber = out.intToDouble(bounded);
    AOT_TEST_CHECK(out.findPurePatch(truncation, Int64, singleElementSpan(boundedNumber)) == bounded);

    Operation* wideNumber = out.intToDouble(fixture.operand);
    AOT_TEST_CHECK(!out.findPurePatch(truncation, Int64, singleElementSpan(wideNumber)));
}

static std::span<const NativeTest> listOfMachineNumberingTests()
{
    static constexpr NativeTest tests[] = {
        { "machine/numbering/patch-writes-count-once-it-is-set-up"_s, testMachineNumberingPatchWritesCountOnceItIsSetUp },
        { "machine/numbering/address-in-the-frame-is-computed-again"_s, testMachineNumberingAddressInFrameIsComputedAgain },
        { "machine/numbering/nested-forks"_s, testMachineNumberingNestedForks },
        { "machine/numbering/arm-that-nothing-reaches"_s, testMachineNumberingArmThatNothingReaches },
        { "machine/numbering/scopes-hold-while-the-table-grows"_s, testMachineNumberingScopesHoldWhileTableGrows },
        { "machine/numbering/continuation-goes-on-in-the-scope"_s, testMachineNumberingContinuationGoesOnInScope },
        { "machine/numbering/scope-comes-back-with-its-block"_s, testMachineNumberingScopeComesBackWithItsBlock },
        { "machine/numbering/second-visit-sees-nothing-of-other-blocks"_s, testMachineNumberingSecondVisitSeesNothingOfOtherBlocks },
        { "machine/numbering/successor-reads-its-own-copy-of-what-is-fused"_s, testMachineNumberingSuccessorReadsOwnCopyOfWhatIsFused },
        { "machine/numbering/own-copy-is-not-made-of-a-load-that-has-ended"_s, testMachineNumberingOwnCopyIsNotMadeOfLoadThatHasEnded },
        { "machine/numbering/word-filled-once"_s, testMachineNumberingWordFilledOnceIsLoadedOnce },
        { "machine/numbering/load-is-made-again-on-request"_s, testMachineNumberingLoadIsMadeAgainOnRequest },
        { "machine/numbering/block-that-lost-an-edge-starts-empty"_s, testMachineNumberingBlockThatLostAnEdgeStartsEmpty },
        { "machine/numbering/sum-is-found-again-wherever-its-block-alone-leads"_s, testMachineNumberingSumIsFoundAgainWhereverItsBlockAloneLeads },
        { "machine/numbering/successor-reads-its-own-copy-of-two-comparisons"_s, testMachineNumberingSuccessorReadsOwnCopyOfTwoComparisons },
        { "machine/numbering/own-copy-of-a-narrow-reference-is-marked"_s, testMachineNumberingOwnCopyOfNarrowReferenceIsMarked },
        { "machine/numbering/reused-load-is-verified"_s, testMachineNumberingReusedLoadIsVerified },
        { "machine/numbering/store-over-load-that-stands-stops-build"_s, testMachineNumberingStoreOverLoadThatStandsStopsBuild },
        { "machine/numbering/store-over-load-that-stands-is-judged-by-ways-from-entry"_s, testMachineNumberingStoreOverLoadThatStandsIsJudgedByWaysFromEntry },
        { "machine/numbering/second-edge-into-a-continuation-asserts"_s, testMachineNumberingSecondEdgeIntoContinuationAsserts },
        { numberingTestOfSecondEdgeIntoContinuation, testMachineNumberingAddsSecondEdgeIntoContinuationOnRequest },
        { numberingTestOfSecondEdgeIntoJoin, testMachineNumberingAddsSecondEdgeIntoJoinOnRequest },
        { "machine/numbering/pure-patches-are-shared-by-id-and-children"_s, testMachineNumberingPurePatchesAreSharedByIDAndChildren },
        { "machine/numbering/patch-that-depends-on-memory-is-shared-until-a-write-there"_s, testMachineNumberingPatchThatDependsOnMemoryIsSharedUntilWriteThere },
        { "machine/numbering/phi-stands-in-place-of-a-pure-patch"_s, testMachineNumberingPhiStandsInPlaceOfPurePatch },
        { "machine/numbering/phi-stands-in-place-of-a-read-of-memory"_s, testMachineNumberingPhiStandsInPlaceOfReadOfMemory },
        { "machine/numbering/truncation-of-a-converted-integer"_s, testMachineNumberingTruncationOfConvertedInteger },
    };
    return tests;
}

} // namespace Machine

std::span<const NativeTest> machineNumberingTests()
{
    return Machine::listOfMachineNumberingTests();
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
