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

// Each value is one Operation, so two requests were numbered alike exactly when they return the same pointer. Most procedures are also compiled and run, so a load wrongly shared across a write shows up in the result.
// The builder reads a Patch's effects when it seals the Patch. Most later requests seal it; a request for a constant does not. Tests finish setting up a Patch before they make any other request, except the tests about sealing.

namespace JSC { namespace AOT { namespace Machine {

// The procedure takes a pointer to eight words, an integer and the same pointer again, and returns an integer.
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
    // Stores go through the second pointer, so only the heaps tell the builder whether a store can change what a load read.
    void store(Operation* value, const AbstractHeap& heap, unsigned index = 0) { out.store64(value, out.address(heap, alias, offsetOfWord(index))); }

    // Stores the operand to a word and, if it ends its block, continues to its first successor. Its effects start empty: the caller sets what it writes.
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

    // Has the effects of a call and emits no code.
    PatchOperation* appendCall()
    {
        PatchOperation* patch = out.patch(Void);
        patch->setRunsWithoutFrame(true);
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
    AbstractHeap cousin { heaps.indexedInt32Properties.at(0) };
    AbstractHeap unrelated { heaps.JSPromise_slot };
    AbstractHeap immutable { heaps.JSCell_typeInfoType };
    AbstractHeap mutableAboveImmutable { heaps.JSRopeString_fiber1 };
    AbstractHeap immutableBelowMutable { heaps.JSRopeString_length };
};

using NumberingTestHeap = AbstractHeap NumberingTestFixture::*;
using NumberingTestGroup = std::array<Operation*, 4>;

static constexpr NumberingTestFixture::Memory numberingTestMemory { 3, 4, 6, 7, 30, 40, 60, 70 };

// What NumberingTestFixture::combine() computes.
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

// Computes its id plus the sum of its operands. The caller records it.
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

// A load from a mutable heap, a load from an immutable heap, a pure operation and a pure Patch. Different numbers, all below four, ask for different values.
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

static void testMachineNumberingLoadIsSharedWithinBlock()
{
    NumberingTestFixture fixture;
    Output& out = fixture.out;
    auto request = [&]() -> std::array<Operation*, 8> {
        return {
            fixture.load(fixture.firstElement),
            fixture.load(fixture.firstElement, 1),
            fixture.load(fixture.everyElement),
            out.load64(out.address(fixture.firstElement, fixture.alias)),
            out.loadDouble(fixture.word(fixture.firstElement)),
            out.load32(fixture.word(fixture.firstElement)),
            out.load8ZeroExt32(fixture.word(fixture.firstElement)),
            out.load8SignExt32(fixture.word(fixture.firstElement)),
        };
    };

    std::array<Operation*, 8> loads = request();
    AOT_TEST_CHECK(areDistinctInNumberingTest(loads));
    for (Operation* load : loads)
        AOT_TEST_CHECK(isLoad(load->opcode()) && load->owner() == out.m_block);

    unsigned size = out.m_block->size();
    out.setOrigin(Origin(7));
    AOT_TEST_CHECK(request() == loads && out.m_block->size() == size);
    AOT_TEST_CHECK(out.load64(out.address(fixture.firstElement, out.add(fixture.words, 8))) == loads[1]);
    out.ret(fixture.combine(loads[0], loads[1]));

    compileOrFail(fixture.test);
    NumberingTestFixture::Memory memory = numberingTestMemory;
    AOT_TEST_CHECK(fixture.run(memory, 5) == numberingTestAnswer(3, 4));
}

static void testMachineNumberingTrappingLoadIsNeverShared()
{
    NumberingTestFixture fixture;
    Output& out = fixture.out;
    TypedPointer byte = fixture.word(fixture.firstElement);
    Operation* plain = out.load8ZeroExt32(byte);
    Operation* trapping = out.load8ZeroExt32Trapping(byte);
    AOT_TEST_CHECK(trapping != plain && trapping->traps() && !plain->traps());
    AOT_TEST_CHECK(out.load8ZeroExt32Trapping(byte) != trapping);
    AOT_TEST_CHECK(out.load8ZeroExt32(byte) == plain);
}

struct NumberingTestStoreCase {
    NumberingTestHeap read;
    NumberingTestHeap written;
    bool endsLoad;
};

static void testMachineNumberingStoreEndsLoadsOfOverlappingHeaps()
{
    using Fixture = NumberingTestFixture;
    const NumberingTestStoreCase cases[] = {
        { &Fixture::firstElement, &Fixture::firstElement, true },
        { &Fixture::firstElement, &Fixture::everyElement, true },
        { &Fixture::firstElement, &Fixture::arrays, true },
        { &Fixture::firstElement, &Fixture::everything, true },
        { &Fixture::everyElement, &Fixture::firstElement, true },
        { &Fixture::arrays, &Fixture::firstElement, true },
        { &Fixture::firstElement, &Fixture::secondElement, false },
        { &Fixture::firstElement, &Fixture::cousin, false },
        { &Fixture::everyElement, &Fixture::cousin, false },
        { &Fixture::firstElement, &Fixture::unrelated, false },
    };
    for (const NumberingTestStoreCase& testCase : cases) {
        Fixture fixture;
        // Accesses through heaps that do not overlap must touch different words.
        unsigned wordWritten = testCase.endsLoad ? 0 : 1;
        Operation* before = fixture.load(fixture.*testCase.read);
        fixture.store(fixture.operand, fixture.*testCase.written, wordWritten);
        Operation* after = fixture.load(fixture.*testCase.read);
        AOT_TEST_CHECK((after != before) == testCase.endsLoad);
        fixture.out.ret(fixture.combine(before, after));

        compileOrFail(fixture.test);
        Fixture::Memory memory = numberingTestMemory;
        AOT_TEST_CHECK(fixture.run(memory, 5) == numberingTestAnswer(3, testCase.endsLoad ? 5 : 3));
        AOT_TEST_CHECK(memory[wordWritten] == 5);
    }

    Fixture fixture;
    Output& out = fixture.out;
    Operation* before = fixture.load(fixture.firstElement);
    out.store32As8(out.castToInt32(fixture.operand), out.address(fixture.firstElement, fixture.alias));
    Operation* after = fixture.load(fixture.firstElement);
    AOT_TEST_CHECK(after != before);
    out.ret(fixture.combine(before, after));

    compileOrFail(fixture.test);
    Fixture::Memory memory = numberingTestMemory;
    memory[0] = 0x300;
    AOT_TEST_CHECK(fixture.run(memory, 5) == numberingTestAnswer(0x300, 0x305));
}

static void testMachineNumberingPureOperationsOutliveWrites()
{
    NumberingTestFixture fixture;
    Output& out = fixture.out;
    Operation* sum = out.add(fixture.operand, fixture.words);
    Operation* difference = out.sub(fixture.operand, fixture.words);
    Operation* comparison = out.lessThan(fixture.operand, fixture.words);
    Operation* choice = out.select(comparison, fixture.operand, fixture.words);
    Operation* narrowed = out.castToInt32(fixture.operand);
    Operation* load = fixture.load(fixture.firstElement);
    auto expectNewLoad = [&] {
        Operation* next = fixture.load(fixture.firstElement);
        AOT_TEST_CHECK(next != load);
        load = next;
    };

    fixture.store(fixture.operand, fixture.everything, 1);
    expectNewLoad();
    fixture.appendCall();
    expectNewLoad();
    // A fence writes no heap.
    out.fence();
    AOT_TEST_CHECK(fixture.load(fixture.firstElement) == load);

    AOT_TEST_CHECK(out.add(fixture.operand, fixture.words) == sum && out.add(fixture.words, fixture.operand) == sum);
    AOT_TEST_CHECK(out.sub(fixture.operand, fixture.words) == difference && out.sub(fixture.words, fixture.operand) != difference);
    AOT_TEST_CHECK(out.lessThan(fixture.operand, fixture.words) == comparison && out.select(comparison, fixture.operand, fixture.words) == choice);
    AOT_TEST_CHECK(out.castToInt32(fixture.operand) == narrowed);
    AOT_TEST_CHECK(out.add(fixture.operand, fixture.alias) != sum);
    AOT_TEST_CHECK(out.opaque(sum) != out.opaque(sum));
    out.ret(fixture.combine(load, out.sub(sum, fixture.words)));

    compileOrFail(fixture.test);
    NumberingTestFixture::Memory memory = numberingTestMemory;
    AOT_TEST_CHECK(fixture.run(memory, 5) == numberingTestAnswer(3, 5) && memory[1] == 5);
}

static void testMachineNumberingImmutableLoadsOutliveWrites()
{
    NumberingTestFixture fixture;
    Operation* fixed = fixture.load(fixture.immutable);
    Operation* fixedBelowMutable = fixture.load(fixture.immutableBelowMutable, 1);
    Operation* changing = fixture.load(fixture.firstElement, 2);
    AOT_TEST_CHECK(fixed->has(OperationFlag::ReadsImmutableHeap) && fixedBelowMutable->has(OperationFlag::ReadsImmutableHeap) && !changing->has(OperationFlag::ReadsImmutableHeap));
    auto expectOnlyImmutableLoads = [&] {
        AOT_TEST_CHECK(fixture.load(fixture.immutable) == fixed && fixture.load(fixture.immutableBelowMutable, 1) == fixedBelowMutable);
        Operation* next = fixture.load(fixture.firstElement, 2);
        AOT_TEST_CHECK(next != changing);
        changing = next;
    };

    fixture.store(fixture.operand, fixture.everything, 3);
    expectOnlyImmutableLoads();
    fixture.appendWriter(3)->effects.writes = HeapSet::top();
    expectOnlyImmutableLoads();
    fixture.appendCall();
    expectOnlyImmutableLoads();

    fixture.store(fixture.operand, fixture.immutable, 3);
    fixture.store(fixture.operand, fixture.mutableAboveImmutable, 3);
    AOT_TEST_CHECK(fixture.load(fixture.immutable) == fixed && fixture.load(fixture.immutableBelowMutable, 1) == fixedBelowMutable);
    // The same word read through a mutable heap is a different value.
    Operation* readAsMutable = fixture.load(fixture.mutableAboveImmutable, 1);
    AOT_TEST_CHECK(readAsMutable != fixedBelowMutable && !readAsMutable->has(OperationFlag::ReadsImmutableHeap));
    fixture.out.ret(fixture.combine(fixed, fixedBelowMutable, changing));

    compileOrFail(fixture.test);
    NumberingTestFixture::Memory memory = numberingTestMemory;
    AOT_TEST_CHECK(fixture.run(memory, 5) == numberingTestAnswer(3, 4, 6) && memory[3] == 5);
}

static void testMachineNumberingPatchWritesCountOnceItIsSetUp()
{
    {
        // A new Patch has the effects of a call. They are not read until the Patch is sealed, so the emitter can still replace them.
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
        // Emitters make constants while they set up a Patch. A constant does not seal it, so effects set afterward still count.
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

static void testMachineNumberingPatchThatEndsItsBlockIsSealedAheadOfContinuation()
{
    NumberingTestFixture fixture;
    Output& out = fixture.out;
    Block* next = out.newBlock();
    Block* other = out.newBlock();
    Operation* before = fixture.load(fixture.firstElement);
    Operation* untouched = fixture.load(fixture.secondElement, 1);

    PatchOperation* patch = fixture.appendWriter(0);
    patch->effects.writes = fixture.test.procedure().heapSet(fixture.firstElement);
    patch->effects.terminal = true;
    out.appendSuccessor(normalEdge(next));
    out.appendSuccessor(rareEdge(other));

    out.appendToContinuation(next);
    Operation* after = fixture.load(fixture.firstElement);
    AOT_TEST_CHECK(after != before && fixture.load(fixture.secondElement, 1) == untouched);
    out.ret(fixture.combine(before, after, untouched));
    out.appendTo(other);
    out.ret(out.int64Zero);

    compileOrFail(fixture.test);
    NumberingTestFixture::Memory memory = numberingTestMemory;
    AOT_TEST_CHECK(fixture.run(memory, 5) == numberingTestAnswer(3, 5, 4));
}

static void testMachineNumberingSuspensionEndsEveryLoad()
{
    ProcedureTraits traits;
    traits.savesFrameOnSuspend = true;
    for (bool isSuspension : { true, false }) {
        NumberingTestFixture fixture(traits);
        Output& out = fixture.out;
        const AbstractHeap& slots = fixture.heaps.frameSlots;
        AOT_TEST_CHECK(slots.mutability() == Mutability::Mutable);
        Operation* slot = out.slotBase(fixture.test.procedure().addStackSlot(8));
        auto request = [&]() -> std::array<Operation*, 3> {
            return {
                fixture.load(fixture.firstElement),
                fixture.load(fixture.unrelated, 1),
                out.load64(out.address(slots, slot)),
            };
        };

        std::array<Operation*, 3> before = request();
        Operation* fixed = fixture.load(fixture.immutable, 2);
        AOT_TEST_CHECK(areDistinctInNumberingTest(before) && request() == before);

        // The effects name no heap. Only the flag says that other code runs here and that the frame comes back without its dead slots.
        PatchOperation* patch = fixture.appendCall();
        patch->effects = Effects::none();
        patch->setIsSuspension(isSuspension);

        std::array<Operation*, 3> after = request();
        for (size_t index = 0; index < before.size(); ++index)
            AOT_TEST_CHECK((after[index] != before[index]) == isSuspension);
        AOT_TEST_CHECK(fixture.load(fixture.immutable, 2) == fixed);
    }
}

static void testMachineNumberingOperationsWithoutPositionAreSharedByAllBlocks()
{
    NumberingTestFixture fixture;
    Output& out = fixture.out;
    Procedure& proc = fixture.test.procedure();
    StackSlot* slot = proc.addStackSlot(16);
    auto request = [&]() -> std::array<Operation*, 11> {
        return {
            out.constInt64(0),
            out.constInt32(0),
            out.constFloat(0.0f),
            out.constDouble(0.0),
            out.constDouble(-0.0),
            out.constDouble(std::numeric_limits<double>::quiet_NaN()),
            out.constInt64(77),
            out.framePointer(),
            out.slotBase(slot),
            out.add(out.slotBase(slot), 8),
            out.argumentRegister(instanceGPR),
        };
    };

    std::array<Operation*, 11> operations = request();
    AOT_TEST_CHECK(areDistinctInNumberingTest(operations));
    for (Operation* operation : operations)
        AOT_TEST_CHECK(operation->hasNoPosition());

    out.appendTo(out.newBlock());
    AOT_TEST_CHECK(request() == operations && out.m_block->isEmpty());
    AOT_TEST_CHECK(out.slotBase(proc.addStackSlot(16)) != out.slotBase(slot));
    AOT_TEST_CHECK(out.add(out.add(out.framePointer(), 16), 8) == out.add(out.framePointer(), 24));
}

// A saved frame goes on at another address, and the scope goes on across a suspension. So what has a position and is computed from an address in the frame is not found again.
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

static void testMachineNumberingArmsAndJoin()
{
    NumberingTestFixture fixture;
    Output& out = fixture.out;
    Block* left = out.newBlock();
    Block* right = out.newBlock();
    Block* join = out.newBlock();
    Operation* isOdd = out.testNonZero64(fixture.operand, 1);
    NumberingTestGroup atFork = requestNumberingTestGroup(fixture, 0);
    ScopeFork fork = out.forkScope();
    out.branch(isOdd, normalEdge(left), normalEdge(right));

    out.appendToArm(left, fork);
    AOT_TEST_CHECK(left->hasOnlyOnePredecessor());
    AOT_TEST_CHECK(requestNumberingTestGroup(fixture, 0) == atFork);
    NumberingTestGroup inLeft = requestNumberingTestGroup(fixture, 1);
    AOT_TEST_CHECK(standsInNumberingTestBlock(inLeft, left) && requestNumberingTestGroup(fixture, 1) == inLeft);
    ValueFromBlock fromLeft = out.anchor(sumOfNumberingTestGroup(out, inLeft));
    out.jump(join);

    out.appendToArm(right, fork);
    AOT_TEST_CHECK(right->hasOnlyOnePredecessor());
    AOT_TEST_CHECK(requestNumberingTestGroup(fixture, 0) == atFork);
    NumberingTestGroup inRight = requestNumberingTestGroup(fixture, 1);
    AOT_TEST_CHECK(standsInNumberingTestBlock(inRight, right));
    ValueFromBlock fromRight = out.anchor(out.add(sumOfNumberingTestGroup(out, inRight), 1000000));
    out.jump(join);

    out.appendToJoin(join, fork);
    AOT_TEST_CHECK(!join->hasOnlyOnePredecessor());
    AOT_TEST_CHECK(requestNumberingTestGroup(fixture, 0) == atFork);
    NumberingTestGroup inJoin = requestNumberingTestGroup(fixture, 1);
    AOT_TEST_CHECK(standsInNumberingTestBlock(inJoin, join));
    Operation* fromArm = out.phi(Int64, fromLeft, fromRight);
    out.ret(out.add(fromArm, out.add(sumOfNumberingTestGroup(out, atFork), sumOfNumberingTestGroup(out, inJoin))));

    compileOrFail(fixture.test);
    for (int64_t operand : { 5, 6 }) {
        NumberingTestFixture::Memory memory = numberingTestMemory;
        int64_t expected = valueOfNumberingTestGroup(operand, 0) + 2 * valueOfNumberingTestGroup(operand, 1) + (operand & 1 ? 0 : 1000000);
        AOT_TEST_CHECK(fixture.run(memory, operand) == expected);
    }
}

static void checkNumberingTestWriteInOneArm(NumberingTestHeap written, bool isStruck, bool writesInFirstArm, bool writesThroughPatch)
{
    NumberingTestFixture fixture;
    Output& out = fixture.out;
    Block* left = out.newBlock();
    Block* right = out.newBlock();
    Block* join = out.newBlock();
    // Accesses through heaps that do not overlap must touch different words.
    unsigned wordWritten = isStruck ? 0 : 1;
    Operation* before = fixture.load(fixture.firstElement);
    ScopeFork fork = out.forkScope();
    out.branch(fixture.operand, normalEdge(left), normalEdge(right));
    for (Block* arm : { left, right }) {
        out.appendToArm(arm, fork);
        if ((arm == left) == writesInFirstArm) {
            if (writesThroughPatch)
                fixture.appendWriter(wordWritten)->effects.writes = fixture.test.procedure().heapSet(fixture.*written);
            else
                fixture.store(fixture.operand, fixture.*written, wordWritten);
        }
        out.jump(join);
    }
    out.appendToJoin(join, fork);
    Operation* after = fixture.load(fixture.firstElement);
    AOT_TEST_CHECK((after != before) == isStruck);
    out.ret(fixture.combine(before, after));

    compileOrFail(fixture.test);
    for (int64_t operand : { 5, 0 }) {
        NumberingTestFixture::Memory memory = numberingTestMemory;
        bool writes = !!operand == writesInFirstArm;
        AOT_TEST_CHECK(fixture.run(memory, operand) == numberingTestAnswer(3, writes && isStruck ? operand : 3));
        AOT_TEST_CHECK(memory[wordWritten] == (writes ? operand : numberingTestMemory[wordWritten]));
    }
}

static void testMachineNumberingWriteInOneArmIsStruckAtJoin()
{
    for (bool writesInFirstArm : { true, false }) {
        for (bool writesThroughPatch : { false, true }) {
            checkNumberingTestWriteInOneArm(&NumberingTestFixture::firstElement, true, writesInFirstArm, writesThroughPatch);
            checkNumberingTestWriteInOneArm(&NumberingTestFixture::everyElement, true, writesInFirstArm, writesThroughPatch);
            checkNumberingTestWriteInOneArm(&NumberingTestFixture::secondElement, false, writesInFirstArm, writesThroughPatch);
        }
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

static void testMachineNumberingBlockEnteredByAppendToStartsEmpty()
{
    NumberingTestFixture fixture;
    Output& out = fixture.out;
    Block* next = out.newBlock();
    NumberingTestGroup first = requestNumberingTestGroup(fixture, 0);
    out.jump(next);

    out.appendTo(next);
    AOT_TEST_CHECK(!next->hasOnlyOnePredecessor());
    NumberingTestGroup second = requestNumberingTestGroup(fixture, 0);
    AOT_TEST_CHECK(standsInNumberingTestBlock(second, next) && requestNumberingTestGroup(fixture, 0) == second);
    out.ret(out.add(sumOfNumberingTestGroup(out, first), sumOfNumberingTestGroup(out, second)));

    compileOrFail(fixture.test);
    NumberingTestFixture::Memory memory = numberingTestMemory;
    AOT_TEST_CHECK(fixture.run(memory, 5) == 2 * valueOfNumberingTestGroup(5, 0));
}

static void testMachineNumberingLoopHeadStartsEmpty()
{
    NumberingTestFixture fixture;
    Output& out = fixture.out;
    Block* loop = out.newBlock();
    Block* done = out.newBlock();
    Operation* sum = out.phiIn(loop, Int64);
    Operation* before = fixture.load(fixture.firstElement);
    out.upsilon(out.int64Zero, sum);
    out.jump(loop);

    out.appendTo(loop);
    Operation* inLoop = fixture.load(fixture.firstElement);
    AOT_TEST_CHECK(inLoop != before && inLoop->owner() == loop);
    Operation* next = out.add(sum, inLoop);
    fixture.store(next, fixture.firstElement);
    out.upsilon(next, sum);
    out.branch(out.lessThan(next, fixture.operand), normalEdge(loop), normalEdge(done));

    out.appendTo(done);
    Operation* after = fixture.load(fixture.firstElement);
    AOT_TEST_CHECK(after->owner() == done);
    out.ret(fixture.combine(before, after));

    compileOrFail(fixture.test);
    NumberingTestFixture::Memory memory = numberingTestMemory;
    AOT_TEST_CHECK(fixture.run(memory, 7) == numberingTestAnswer(3, 12) && memory[0] == 12);
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

static void testMachineNumberingScopeOfArmComesBack()
{
    NumberingTestFixture fixture;
    Output& out = fixture.out;
    Block* left = out.newBlock();
    Block* slow = out.newBlock();
    Block* right = out.newBlock();
    Block* join = out.newBlock();
    Operation* isOdd = out.testNonZero64(fixture.operand, 1);
    Operation* hasSecondBit = out.testNonZero64(fixture.operand, 2);
    NumberingTestGroup atFork = requestNumberingTestGroup(fixture, 0);
    ScopeFork fork = out.forkScope();
    out.branch(isOdd, normalEdge(left), normalEdge(right));

    out.appendToArm(left, fork);
    NumberingTestGroup inLeft = requestNumberingTestGroup(fixture, 1);

    out.appendTo(slow);
    NumberingTestGroup inSlow = requestNumberingTestGroup(fixture, 0);
    AOT_TEST_CHECK(standsInNumberingTestBlock(inSlow, slow) && standsInNumberingTestBlock(requestNumberingTestGroup(fixture, 1), slow));
    out.ret(out.add(sumOfNumberingTestGroup(out, inSlow), 3000000));

    out.appendTo(left);
    AOT_TEST_CHECK(requestNumberingTestGroup(fixture, 0) == atFork && requestNumberingTestGroup(fixture, 1) == inLeft);
    ValueFromBlock fromLeft = out.anchor(sumOfNumberingTestGroup(out, inLeft));
    out.branch(hasSecondBit, rareEdge(slow), normalEdge(join));

    out.appendToArm(right, fork);
    AOT_TEST_CHECK(requestNumberingTestGroup(fixture, 0) == atFork);
    NumberingTestGroup inRight = requestNumberingTestGroup(fixture, 1);
    AOT_TEST_CHECK(standsInNumberingTestBlock(inRight, right));
    ValueFromBlock fromRight = out.anchor(out.add(sumOfNumberingTestGroup(out, inRight), 1000000));
    out.jump(join);

    out.appendToJoin(join, fork);
    AOT_TEST_CHECK(requestNumberingTestGroup(fixture, 0) == atFork && standsInNumberingTestBlock(requestNumberingTestGroup(fixture, 1), join));
    out.ret(out.add(out.phi(Int64, fromLeft, fromRight), sumOfNumberingTestGroup(out, atFork)));

    compileOrFail(fixture.test);
    NumberingTestFixture::Memory memory = numberingTestMemory;
    AOT_TEST_CHECK(fixture.run(memory, 5) == valueOfNumberingTestGroup(5, 0) + valueOfNumberingTestGroup(5, 1));
    AOT_TEST_CHECK(fixture.run(memory, 6) == valueOfNumberingTestGroup(6, 0) + valueOfNumberingTestGroup(6, 1) + 1000000);
    AOT_TEST_CHECK(fixture.run(memory, 7) == valueOfNumberingTestGroup(7, 0) + 3000000);
}

// Between two blocks of Code, CodeGenerator takes the fork behind the terminator of the first and hands it to each block that the first alone leads to.
static void testMachineNumberingSuccessorFindsWhatTestHasLoaded()
{
    NumberingTestFixture fixture;
    Output& out = fixture.out;
    Block* filled = out.newBlock();
    Block* empty = out.newBlock();
    Operation* tested = fixture.load(fixture.firstElement);
    Operation* fixed = fixture.load(fixture.immutable, 4);
    Operation* product = out.mul(tested, fixture.operand);
    out.branch(out.notZero64(tested), normalEdge(filled), rareEdge(empty));
    ScopeFork atEnd = out.forkScope();

    out.appendToSuccessor(empty, atEnd);
    Operation* inEmpty = fixture.load(fixture.firstElement);
    AOT_TEST_CHECK(!empty->hasOnlyOnePredecessor() && inEmpty->owner() == empty);
    out.ret(out.add(inEmpty, 1000000));

    out.appendToSuccessor(filled, atEnd);
    AOT_TEST_CHECK(filled->hasOnlyOnePredecessor());
    AOT_TEST_CHECK(fixture.load(fixture.firstElement) == tested && fixture.load(fixture.immutable, 4) == fixed && out.mul(tested, fixture.operand) == product);
    fixture.store(fixture.operand, fixture.firstElement);
    Operation* behindStore = fixture.load(fixture.firstElement);
    AOT_TEST_CHECK(behindStore != tested && behindStore->owner() == filled);
    out.ret(fixture.combine(tested, behindStore, product));

    compileOrFail(fixture.test);
    NumberingTestFixture::Memory memory = numberingTestMemory;
    AOT_TEST_CHECK(fixture.run(memory, 5) == numberingTestAnswer(3, 5, 15) && memory[0] == 5);
    memory[0] = 0;
    AOT_TEST_CHECK(fixture.run(memory, 5) == 1000000 && !memory[0]);
}

// The scan fuses an address into its access and a comparison into its Branch where nothing else reads them. So a request behind the border finds them, and an operation that is appended there reads copies of its own.
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

    // Each request finds what it asks for, so a chain that ends in a load costs nothing the second time.
    out.appendToSuccessor(next, atEnd);
    AOT_TEST_CHECK(requestElement() == element && requestAddress() == address && requestNarrow() == narrow && requestWordOfFrame() == wordOfFrame);
    AOT_TEST_CHECK(out.equal(word, fixture.operand) == test && out.mul(word, fixture.operand) == product && next->isEmpty());

    Operation* neighbor = out.load64(out.address(fixture.cousin, requestAddress(), NumberingTestFixture::offsetOfWord(1)));
    Operation* ownAddress = neighbor->child(0);
    AOT_TEST_CHECK(neighbor->owner() == next && ownAddress != address && ownAddress->opcode() == Add && ownAddress->owner() == next);
    AOT_TEST_CHECK(ownAddress->child(0) == fixture.words && ownAddress->child(1) != address->child(1) && ownAddress->child(1)->opcode() == Shl && ownAddress->child(1)->owner() == next);
    AOT_TEST_CHECK(requestAddress() == ownAddress);

    // Where an instruction reads an operand from memory, a narrow word that one operation reads is part of that operation.
    Operation* widened = out.signExt32To64(requestNarrow());
    AOT_TEST_CHECK(widened->opcode() == SExt32 && widened->owner() == next && (widened->child(0) != narrow) == isX86_64());
    Operation* mixed = out.bitXor(requestWordOfFrame(), fixture.operand);
    AOT_TEST_CHECK(mixed->owner() == next && mixed->child(0) != wordOfFrame && mixed->child(1) != wordOfFrame);

    // A word of 64 bits and a product take a register wherever they stand.
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

// An emitter asks for a word, writes it, and then reads what it asked for.
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

// A word that is filled once: zero at the first load, filled by the time a second one would run. Whoever tests it and hands it on hands on what it tested.
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

// The same of a narrow word through a value, which each reader would otherwise take from memory on x86-64.
static void testMachineNumberingNarrowWordFilledOnceIsLoadedOnce()
{
    NumberingTestFixture fixture;
    Output& out = fixture.out;
    Block* next = out.newBlock();
    Block* isFilled = out.newBlock();
    Block* isEmpty = out.newBlock();
    auto requestWord = [&] {
        return out.load32(fixture.word(fixture.heaps.AOTInstance_fieldsFilledOnce));
    };
    Operation* word = requestWord();
    fixture.store(fixture.operand, fixture.unrelated);
    out.jump(next);
    ScopeFork atEnd = out.forkScope();

    out.appendToSuccessor(next, atEnd);
    Operation* found = requestWord();
    AOT_TEST_CHECK(found == word && !word->useCount());
    out.branch(out.notZero32(found), normalEdge(isFilled), normalEdge(isEmpty));
    AOT_TEST_CHECK(next->last()->child(0) == word);

    out.appendTo(isFilled);
    out.ret(out.zeroExtPtr(found));
    out.appendTo(isEmpty);
    out.ret(out.constInt64(-1));

    compileOrFail(fixture.test);
    NumberingTestFixture::Memory memory = numberingTestMemory;
    memory[0] = 0;
    AOT_TEST_CHECK(fixture.run(memory, 5) == -1 && memory[0] == 5);
}

static void testMachineNumberingSuccessorWithTwoPredecessorsStartsEmpty()
{
    NumberingTestFixture fixture;
    Output& out = fixture.out;
    Block* other = out.newBlock();
    Block* next = out.newBlock();
    Operation* tested = fixture.load(fixture.firstElement);
    out.branch(fixture.operand, normalEdge(next), normalEdge(other));
    ScopeFork atEnd = out.forkScope();

    out.appendToSuccessor(other, atEnd);
    AOT_TEST_CHECK(other->hasOnlyOnePredecessor() && fixture.load(fixture.firstElement) == tested);
    out.jump(next);

    out.appendToSuccessor(next, atEnd);
    Operation* inNext = fixture.load(fixture.firstElement);
    AOT_TEST_CHECK(!next->hasOnlyOnePredecessor() && inNext->owner() == next);
    out.ret(fixture.combine(tested, inNext));

    compileOrFail(fixture.test);
    for (int64_t operand : { 5, 0 }) {
        NumberingTestFixture::Memory memory = numberingTestMemory;
        AOT_TEST_CHECK(fixture.run(memory, operand) == numberingTestAnswer(3, 3));
    }
}

static void testMachineNumberingWriteInOneSuccessorEndsLoadsForTheNext()
{
    NumberingTestFixture fixture;
    Output& out = fixture.out;
    Block* left = out.newBlock();
    Block* right = out.newBlock();
    Operation* tested = fixture.load(fixture.firstElement);
    Operation* fixed = fixture.load(fixture.immutable, 4);
    out.branch(fixture.operand, normalEdge(left), normalEdge(right));
    ScopeFork atEnd = out.forkScope();

    out.appendToSuccessor(left, atEnd);
    fixture.store(fixture.operand, fixture.firstElement);
    out.ret(fixture.load(fixture.firstElement));

    out.appendToSuccessor(right, atEnd);
    Operation* inRight = fixture.load(fixture.firstElement);
    AOT_TEST_CHECK(right->hasOnlyOnePredecessor() && inRight->owner() == right && fixture.load(fixture.immutable, 4) == fixed);
    out.ret(fixture.combine(tested, inRight, fixed));

    compileOrFail(fixture.test);
    NumberingTestFixture::Memory memory = numberingTestMemory;
    AOT_TEST_CHECK(fixture.run(memory, 0) == numberingTestAnswer(3, 3, 30) && memory == numberingTestMemory);
    AOT_TEST_CHECK(fixture.run(memory, 5) == 5 && memory[0] == 5);
}

// The emitters come back to a block that they left ahead of the fork, so what the fork counts goes and other entries take its places.
static void testMachineNumberingSuccessorStartsEmptyOnceScopeHasGone()
{
    NumberingTestFixture fixture;
    Output& out = fixture.out;
    Block* entry = out.m_block;
    Block* first = out.newBlock();
    Block* next = out.newBlock();
    NumberingTestGroup inEntry = requestNumberingTestGroup(fixture, 0);

    out.appendTo(first);
    Operation* tested = fixture.load(fixture.firstElement);
    out.jump(next);
    ScopeFork atEnd = out.forkScope();

    out.appendTo(entry);
    AOT_TEST_CHECK(requestNumberingTestGroup(fixture, 0) == inEntry);
    NumberingTestGroup laterInEntry = requestNumberingTestGroup(fixture, 1);
    out.jump(first);

    out.appendToSuccessor(next, atEnd);
    Operation* inNext = fixture.load(fixture.firstElement);
    AOT_TEST_CHECK(!next->hasOnlyOnePredecessor() && inNext->owner() == next);
    AOT_TEST_CHECK(standsInNumberingTestBlock(requestNumberingTestGroup(fixture, 1), next));
    out.ret(out.add(fixture.combine(tested, inNext), out.mul(sumOfNumberingTestGroup(out, laterInEntry), 1000000)));

    compileOrFail(fixture.test);
    NumberingTestFixture::Memory memory = numberingTestMemory;
    AOT_TEST_CHECK(fixture.run(memory, 5) == numberingTestAnswer(3, 3) + 1000000 * valueOfNumberingTestGroup(5, 1));
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

static void testMachineNumberingTargetOfSwitchFindsWhatHeadHasLoaded()
{
    NumberingTestFixture fixture;
    Output& out = fixture.out;
    Block* first = out.newBlock();
    Block* second = out.newBlock();
    Block* ofTwoCases = out.newBlock();
    Block* otherwise = out.newBlock();
    Operation* loaded = fixture.load(fixture.firstElement);
    SwitchCase cases[] = { { 1, normalEdge(first) }, { 2, normalEdge(second) }, { 3, normalEdge(ofTwoCases) }, { 4, normalEdge(ofTwoCases) } };
    out.switchOn(fixture.operand, cases, rareEdge(otherwise));
    ScopeFork atEnd = out.forkScope();

    for (Block* target : { first, second }) {
        out.appendToSuccessor(target, atEnd);
        AOT_TEST_CHECK(target->hasOnlyOnePredecessor() && fixture.load(fixture.firstElement) == loaded);
        out.ret(out.add(loaded, target == first ? 100 : 200));
    }
    for (Block* target : { ofTwoCases, otherwise }) {
        out.appendToSuccessor(target, atEnd);
        Operation* inTarget = fixture.load(fixture.firstElement);
        AOT_TEST_CHECK(!target->hasOnlyOnePredecessor() && inTarget->owner() == target);
        out.ret(out.add(inTarget, target == ofTwoCases ? 300 : 400));
    }

    compileOrFail(fixture.test);
    NumberingTestFixture::Memory memory = numberingTestMemory;
    AOT_TEST_CHECK(fixture.run(memory, 1) == 103 && fixture.run(memory, 2) == 203 && fixture.run(memory, 3) == 303 && fixture.run(memory, 4) == 303 && fixture.run(memory, 9) == 403);
}

// Of two edges one is taken away. The count says one, and nothing says which of the two is left.
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
        AOT_TEST_CHECK(next->numberOfIncomingEdges() == 1 && !next->singlePredecessor());

        out.appendToSuccessor(next, atEnd);
        AOT_TEST_CHECK(!next->hasOnlyOnePredecessor() && fixture.load(fixture.firstElement)->owner() == next);
    }
}

// A Branch on a BitOr of two comparisons compares twice and jumps behind each, as long as nothing else reads the three.
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

// The emitters mark the operation that their request found. The collector's rules go by the mark of the operation that is read.
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

// Under --aot-validate. A Patch writes a word and says nothing of it, so the block behind it finds what the word held.
static void testMachineNumberingInheritedLoadIsVerified()
{
    for (bool declaresWrite : { false, true }) {
        NumberingTestFixture fixture;
        Output& out = fixture.out;
        unsigned numberOfChecks = 0;
        out.verifyInheritedLoads(fixture.cousin.index(), [&](Operation* readSameBits) {
            ++numberOfChecks;
            PatchOperation* check = out.check(out.isZero32(readSameBits));
            check->setGenerator([](CCallHelpers& jit, const PatchParameters& params) {
                jit.move(CCallHelpers::TrustedImm64(-1), GPRInfo::returnValueGPR);
                if (params.frameIsSetUp())
                    emitFrameTeardown(jit, params.frame());
                jit.ret();
            });
        });

        Block* next = out.newBlock();
        Operation* word = fixture.load(fixture.firstElement);
        Operation* exempt = fixture.load(fixture.cousin, 2);
        Operation* fixed = fixture.load(fixture.immutable, 4);
        fixture.appendWriter(0)->effects.writes = fixture.test.procedure().heapSet(declaresWrite ? fixture.firstElement : fixture.unrelated);
        out.jump(next);
        ScopeFork atEnd = out.forkScope();

        out.appendToSuccessor(next, atEnd);
        AOT_TEST_CHECK(fixture.load(fixture.cousin, 2) == exempt && fixture.load(fixture.immutable, 4) == fixed && !numberOfChecks);
        Operation* found = fixture.load(fixture.firstElement);
        AOT_TEST_CHECK((found == word) == !declaresWrite && numberOfChecks == (declaresWrite ? 0u : 1u));
        out.ret(fixture.combine(found, exempt, fixed));

        compileOrFail(fixture.test);
        NumberingTestFixture::Memory memory = numberingTestMemory;
        AOT_TEST_CHECK(fixture.run(memory, 5) == (declaresWrite ? numberingTestAnswer(5, 6, 30) : -1));
        // The Patch stores what the word held.
        memory = numberingTestMemory;
        AOT_TEST_CHECK(fixture.run(memory, 3) == numberingTestAnswer(3, 6, 30));
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

// Reads a word, and says that it reads the heap.
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
        // A write below what it reads, a Patch that writes there and a call end it as well.
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
        // A write in one arm ends it at the join, and the arm that does not write still sees it.
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
        // The control: one that depends on its children alone outlives the write, whatever it says that it reads.
        constexpr PureFunction ofChildrenAlone { 71, true };
        NumberingTestFixture fixture;
        Operation* first = requestNumberingTestReader(fixture, ofChildrenAlone, fixture.firstElement, 0);
        fixture.store(fixture.operand, fixture.firstElement);
        AOT_TEST_CHECK(requestNumberingTestReader(fixture, ofChildrenAlone, fixture.firstElement, 0) == first);
    }
}

// One arm has read the first word, the other takes the operand in its place.
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

    // Above 2^53 a double may differ from the integer it was converted from.
    Operation* wideNumber = out.intToDouble(fixture.operand);
    AOT_TEST_CHECK(!out.findPurePatch(truncation, Int64, singleElementSpan(wideNumber)));
}

static std::span<const NativeTest> listOfMachineNumberingTests()
{
    static constexpr NativeTest tests[] = {
        { "machine/numbering/load-is-shared-within-a-block"_s, testMachineNumberingLoadIsSharedWithinBlock },
        { "machine/numbering/trapping-load-is-never-shared"_s, testMachineNumberingTrappingLoadIsNeverShared },
        { "machine/numbering/store-ends-loads-of-overlapping-heaps"_s, testMachineNumberingStoreEndsLoadsOfOverlappingHeaps },
        { "machine/numbering/pure-operations-outlive-writes"_s, testMachineNumberingPureOperationsOutliveWrites },
        { "machine/numbering/immutable-loads-outlive-writes"_s, testMachineNumberingImmutableLoadsOutliveWrites },
        { "machine/numbering/patch-writes-count-once-it-is-set-up"_s, testMachineNumberingPatchWritesCountOnceItIsSetUp },
        { "machine/numbering/patch-that-ends-its-block-is-sealed-ahead-of-the-continuation"_s, testMachineNumberingPatchThatEndsItsBlockIsSealedAheadOfContinuation },
        { "machine/numbering/suspension-ends-every-load"_s, testMachineNumberingSuspensionEndsEveryLoad },
        { "machine/numbering/operations-without-position-are-shared-by-all-blocks"_s, testMachineNumberingOperationsWithoutPositionAreSharedByAllBlocks },
        { "machine/numbering/address-in-the-frame-is-computed-again"_s, testMachineNumberingAddressInFrameIsComputedAgain },
        { "machine/numbering/arms-and-join"_s, testMachineNumberingArmsAndJoin },
        { "machine/numbering/write-in-one-arm-is-struck-at-the-join"_s, testMachineNumberingWriteInOneArmIsStruckAtJoin },
        { "machine/numbering/nested-forks"_s, testMachineNumberingNestedForks },
        { "machine/numbering/arm-that-nothing-reaches"_s, testMachineNumberingArmThatNothingReaches },
        { "machine/numbering/scopes-hold-while-the-table-grows"_s, testMachineNumberingScopesHoldWhileTableGrows },
        { "machine/numbering/continuation-goes-on-in-the-scope"_s, testMachineNumberingContinuationGoesOnInScope },
        { "machine/numbering/block-entered-by-append-to-starts-empty"_s, testMachineNumberingBlockEnteredByAppendToStartsEmpty },
        { "machine/numbering/loop-head-starts-empty"_s, testMachineNumberingLoopHeadStartsEmpty },
        { "machine/numbering/scope-comes-back-with-its-block"_s, testMachineNumberingScopeComesBackWithItsBlock },
        { "machine/numbering/second-visit-sees-nothing-of-other-blocks"_s, testMachineNumberingSecondVisitSeesNothingOfOtherBlocks },
        { "machine/numbering/scope-of-an-arm-comes-back"_s, testMachineNumberingScopeOfArmComesBack },
        { "machine/numbering/successor-finds-what-the-test-has-loaded"_s, testMachineNumberingSuccessorFindsWhatTestHasLoaded },
        { "machine/numbering/successor-reads-its-own-copy-of-what-is-fused"_s, testMachineNumberingSuccessorReadsOwnCopyOfWhatIsFused },
        { "machine/numbering/own-copy-is-not-made-of-a-load-that-has-ended"_s, testMachineNumberingOwnCopyIsNotMadeOfLoadThatHasEnded },
        { "machine/numbering/word-filled-once"_s, testMachineNumberingWordFilledOnceIsLoadedOnce },
        { "machine/numbering/narrow-word-filled-once"_s, testMachineNumberingNarrowWordFilledOnceIsLoadedOnce },
        { "machine/numbering/successor-with-two-predecessors-starts-empty"_s, testMachineNumberingSuccessorWithTwoPredecessorsStartsEmpty },
        { "machine/numbering/write-in-one-successor-ends-loads-for-the-next"_s, testMachineNumberingWriteInOneSuccessorEndsLoadsForTheNext },
        { "machine/numbering/successor-starts-empty-once-the-scope-has-gone"_s, testMachineNumberingSuccessorStartsEmptyOnceScopeHasGone },
        { "machine/numbering/load-is-made-again-on-request"_s, testMachineNumberingLoadIsMadeAgainOnRequest },
        { "machine/numbering/target-of-a-switch-finds-what-the-head-has-loaded"_s, testMachineNumberingTargetOfSwitchFindsWhatHeadHasLoaded },
        { "machine/numbering/block-that-lost-an-edge-starts-empty"_s, testMachineNumberingBlockThatLostAnEdgeStartsEmpty },
        { "machine/numbering/successor-reads-its-own-copy-of-two-comparisons"_s, testMachineNumberingSuccessorReadsOwnCopyOfTwoComparisons },
        { "machine/numbering/own-copy-of-a-narrow-reference-is-marked"_s, testMachineNumberingOwnCopyOfNarrowReferenceIsMarked },
        { "machine/numbering/inherited-load-is-verified"_s, testMachineNumberingInheritedLoadIsVerified },
        { "machine/numbering/second-edge-into-a-continuation-asserts"_s, testMachineNumberingSecondEdgeIntoContinuationAsserts },
        { numberingTestOfSecondEdgeIntoContinuation, testMachineNumberingAddsSecondEdgeIntoContinuationOnRequest },
        { numberingTestOfSecondEdgeIntoJoin, testMachineNumberingAddsSecondEdgeIntoJoinOnRequest },
        { "machine/numbering/pure-patches-are-shared-by-id-and-children"_s, testMachineNumberingPurePatchesAreSharedByIDAndChildren },
        { "machine/numbering/patch-that-depends-on-memory-is-shared-until-a-write-there"_s, testMachineNumberingPatchThatDependsOnMemoryIsSharedUntilWriteThere },
        { "machine/numbering/phi-stands-in-place-of-a-pure-patch"_s, testMachineNumberingPhiStandsInPlaceOfPurePatch },
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
