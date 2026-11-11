/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTGrowthOfWork.h"
#include "AOTMachineOrder.h"
#include "AOTMachineTestSupport.h"
#include <algorithm>
#include <bit>
#include <initializer_list>
#include <utility>
#include <wtf/DataLog.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT { namespace Machine {

[[noreturn]] static void failCheckerTest(TestProcedure& test, const char* file, int line, const char* condition)
{
    if (const std::optional<Failure>& failure = test.procedure().failure())
        dataLogLn("reported: ", failure->kind, " at @", failure->operation);
    dataLogLn(test.printedProcedure(), test.printedAllocation(), test.printedFrame());
    failNativeTest(file, line, condition);
}

#define AOT_CHECKER_TEST_CHECK(test, ...) do { \
        if (!(__VA_ARGS__)) [[unlikely]] \
            failCheckerTest(test, __FILE__, __LINE__, #__VA_ARGS__); \
    } while (false)

static bool checkerReportedFailureAt(const Procedure& procedure, FailureKind kind, std::initializer_list<const Operation*> operations)
{
    const std::optional<Failure>& failure = procedure.failure();
    return failure && failure->kind == kind && failure->check.line && StringView::fromLatin1(failure->check.file).startsWith("AOTMachine"_s) && !StringView::fromLatin1(failure->check.function).startsWith("fail"_s) && std::ranges::any_of(operations, [&](const Operation* operation) {
        return failure->operation == operation->index() && failure->origin == operation->origin();
    });
}

static void prepareForCheckerTest(TestProcedure& test)
{
    prepareOrFail(test);
    Procedure& procedure = test.procedure();
    AOT_CHECKER_TEST_CHECK(test, checkAllocation(procedure) && checkFrame(procedure) && checkReferences(procedure) && !procedure.failure());
}

static void expectCheckerRefusal(TestProcedure& test, bool (*check)(Procedure&), FailureKind kind, std::initializer_list<const Operation*> operations)
{
    AOT_CHECKER_TEST_CHECK(test, !check(test.procedure()));
    AOT_CHECKER_TEST_CHECK(test, checkerReportedFailureAt(test.procedure(), kind, operations));
}

static Step& findExecuteStepForCheckerTest(TestProcedure& test, const Operation* operation)
{
    for (Step& step : test.procedure().allocationForTesting().stepsForTesting()) {
        if (step.kind == StepKind::Execute && step.operation == operation)
            return step;
    }
    failCheckerTest(test, __FILE__, __LINE__, "the operation has an Execute step");
}

static PatchAssignment& patchAssignmentForCheckerTest(TestProcedure& test, const PatchOperation* patch)
{
    uint32_t index = findExecuteStepForCheckerTest(test, patch).patchAssignment;
    return test.procedure().allocationForTesting().patchAssignmentForTesting(index);
}

static uint32_t findStepForCheckerTest(TestProcedure& test, const Block* block, StepKind kind, const Operation* value = nullptr)
{
    const Allocation& allocation = test.procedure().allocation();
    const BlockSteps& range = allocation.stepsOf(block);
    auto isWanted = [&](uint32_t index) {
        const Step& step = allocation.step(index);
        return step.kind == kind && (!value || step.operation == value);
    };
    for (uint32_t index = range.begin; index < range.end; ++index) {
        if (isWanted(index))
            return index;
    }
    for (uint32_t index = range.beginOfEdgeMoves; index < range.endOfEdgeMoves; ++index) {
        if (isWanted(index))
            return index;
    }
    failCheckerTest(test, __FILE__, __LINE__, "the block has a step of that kind");
}

static void dropStepForCheckerTest(TestProcedure& test, const Block* block, uint32_t index)
{
    Allocation& allocation = test.procedure().allocationForTesting();
    BlockSteps& range = allocation.stepsForTesting(block);
    bool isStepOfBlock = index >= range.begin && index < range.end;
    AOT_CHECKER_TEST_CHECK(test, isStepOfBlock || (index >= range.beginOfEdgeMoves && index < range.endOfEdgeMoves));
    uint32_t& end = isStepOfBlock ? range.end : range.endOfEdgeMoves;
    std::span<Step> steps = allocation.stepsForTesting();
    for (uint32_t next = index + 1; next < end; ++next)
        steps[next - 1] = steps[next];
    --end;
}

static const Operation* readerAfterStepForCheckerTest(TestProcedure& test, const Block* block, uint32_t index)
{
    const Allocation& allocation = test.procedure().allocation();
    uint32_t end = allocation.stepsOf(block).end;
    while (index < end && allocation.step(index).kind != StepKind::Execute)
        ++index;
    return index < end ? allocation.step(index).operation : block->last();
}

static RegisterMask callerSavedRegistersForCheckerTest(TestProcedure& test)
{
    RegisterFile file = registerFileFor(test.procedure().traits());
    return (file.allocatableIn(Bank::GP) | file.allocatableIn(Bank::FP)) & ~file.calleeSaved;
}

static PatchOperation* appendPatchForCheckerTest(Output& out, Type type, std::initializer_list<Operation*> operandsInRegisters, RegisterMask clobberedLate = 0)
{
    PatchOperation* patch = out.patch(type);
    for (Operation* operand : operandsInRegisters)
        patch->appendSomeRegister(operand);
    patch->clobberLate(clobberedLate);
    patch->setGenerator([](CCallHelpers&, const PatchParameters&) { });
    return patch;
}

static Block* buildPressureForCheckerTest(TestProcedure& test)
{
    Output& out = test.out();
    unsigned count = static_cast<unsigned>(std::popcount(registerFileFor(test.procedure().traits()).allocatableIn(Bank::GP))) + 4;
    Vector<Operation*> values = defineValues(test, test.argumentGPR(0), count, Int64);
    Operation* sum = values[0];
    for (Operation* value : values.span().subspan(1))
        sum = appendPatchForCheckerTest(out, Int64, { sum, value });
    out.ret(sum);
    return out.m_block;
}

struct CheckerTestDiamond {
    Operation* phi;
    Block* right;
};

static CheckerTestDiamond buildDiamondForCheckerTest(TestProcedure& test, bool rightHasUpsilon)
{
    Output& out = test.out();
    Operation* first = test.argumentGPR(0);
    Operation* second = test.argumentGPR(1);
    Block* left = out.newBlock();
    Block* right = out.newBlock();
    Block* join = out.newBlock();
    Operation* phi = out.phiIn(join, Int64);
    out.branch(out.testNonZero64(first, 1), normalEdge(left), normalEdge(right));

    out.appendTo(left);
    out.upsilon(first, phi);
    out.jump(normalEdge(join));

    out.appendTo(right);
    if (rightHasUpsilon)
        out.upsilon(second, phi);
    out.jump(normalEdge(join));

    out.appendTo(join);
    out.ret(out.sub(out.sub(phi, first), second));
    return { phi, right };
}

struct CheckerTestRareArm {
    Vector<Operation*> values;
    Block* slow;
    PatchOperation* coldReader;
};

static CheckerTestRareArm buildRareArmForCheckerTest(TestProcedure& test)
{
    Output& out = test.out();
    Operation* selector = test.argumentGPR(0);
    CheckerTestRareArm arm { defineValues(test, test.argumentGPR(1), 2, Int64), out.newBlock(), nullptr };
    Block* join = out.newBlock();
    out.branch(out.testNonZero64(selector, 1), rareEdge(arm.slow), normalEdge(join));

    out.appendTo(arm.slow);
    appendPatchForCheckerTest(out, Void, { }, callerSavedRegistersForCheckerTest(test));
    arm.coldReader = appendPatchForCheckerTest(out, Void, { });
    arm.coldReader->append(arm.values[0], ValueRep::ColdAny);
    out.jump(normalEdge(join));

    out.appendTo(join);
    out.ret(out.sub(arm.values[0], arm.values[1]));
    return arm;
}

static void checkerRefusesReadThatNoDefinitionDominates()
{
    for (bool readsAcrossArms : { false, true }) {
        TestProcedure test;
        Output& out = test.out();
        Operation* argument = test.argumentGPR(0);
        Block* left = out.newBlock();
        Block* right = out.newBlock();
        out.branch(out.testNonZero64(argument, 1), normalEdge(left), normalEdge(right));

        out.appendTo(left);
        Operation* definedOnLeft = out.opaque(argument);
        out.ret(definedOnLeft);

        out.appendTo(right);
        Operation* reader = out.neg(readsAcrossArms ? definedOnLeft : argument);
        out.ret(reader);

        Procedure& procedure = test.procedure();
        AOT_TEST_CHECK(validate(procedure) != readsAcrossArms);
        AOT_TEST_CHECK(readsAcrossArms ? checkerReportedFailureAt(procedure, FailureKind::MalformedProcedure, { reader }) : !procedure.failure());
        if (readsAcrossArms) {
            StringPrintStream said;
            printFailure(said, procedure);
            AOT_TEST_CHECK(said.toString().startsWith("MalformedProcedure, by the check in "_s) && said.toString().contains(" at AOTMachineValidate.cpp:"_s));
        }
    }
}

static std::optional<uint64_t> checkerTestInstructionsToValidateRowOfGuards(unsigned numberOfGuards)
{
    TestProcedure test;
    Output& out = test.out();
    Operation* argument = test.argumentGPR(0);
    Block* shared = out.newBlock();
    for (unsigned number = 0; number < numberOfGuards; ++number) {
        Block* next = out.newBlock();
        out.branch(out.testNonZero64(argument, number + 1), normalEdge(shared), normalEdge(next));
        out.appendTo(next);
    }
    out.ret(out.neg(argument));
    out.appendTo(shared);
    out.ret(argument);

    bool isValid = false;
    std::optional<uint64_t> instructions = instructionsOfCurrentThreadIn([&] {
        isValid = validate(test.procedure());
    });
    AOT_TEST_CHECK(isValid);
    return instructions;
}

static void checkerWorkGrowsAsRowOfGuardsIntoOneBlockDoes()
{
    constexpr unsigned numberOfGuards = 5000;
    std::optional<uint64_t> smaller = checkerTestInstructionsToValidateRowOfGuards(numberOfGuards);
    std::optional<uint64_t> large = checkerTestInstructionsToValidateRowOfGuards(4 * numberOfGuards);
    AOT_TEST_CHECK(growsAsSizeDoes(smaller, large));
}

static void checkerRefusesEdgeWithoutUpsilonForPhi()
{
    for (bool rightHasUpsilon : { true, false }) {
        TestProcedure test;
        CheckerTestDiamond diamond = buildDiamondForCheckerTest(test, rightHasUpsilon);
        Procedure& procedure = test.procedure();
        AOT_TEST_CHECK(validate(procedure) == rightHasUpsilon);
        AOT_TEST_CHECK(rightHasUpsilon ? !procedure.failure() : checkerReportedFailureAt(procedure, FailureKind::MalformedProcedure, { diamond.right->last() }));
    }
}

static void checkerRefusesEdgeWithoutUpsilonForPhiBehindBlockPasses()
{
    for (bool rightHasUpsilon : { true, false }) {
        TestProcedure test;
        CheckerTestDiamond diamond = buildDiamondForCheckerTest(test, rightHasUpsilon);
        Procedure& procedure = test.procedure();
        AOT_TEST_CHECK(validateBehindBlockPasses(procedure) == rightHasUpsilon);
        AOT_TEST_CHECK(rightHasUpsilon ? !procedure.failure() : checkerReportedFailureAt(procedure, FailureKind::MalformedProcedure, { diamond.right->last() }));
    }
}

static void checkerLooksOnlyAtWhatBlockPassesLeave()
{
    for (bool asksAsOfEmittedOperations : { true, false }) {
        TestProcedure test;
        Output& out = test.out();
        Operation* argument = test.argumentGPR(0);
        Block* left = out.newBlock();
        Block* right = out.newBlock();
        Block* join = out.newBlock();
        Block* taken = out.newBlock();
        Block* notTaken = out.newBlock();
        Operation* phi = out.phiIn(join, Int32);
        out.branch(out.testNonZero64(argument, 1), normalEdge(left), normalEdge(right));

        out.appendTo(left);
        out.upsilon(out.constInt32(1), phi);
        out.jump(normalEdge(join));

        out.appendTo(right);
        out.upsilon(out.constInt32(0), phi);
        out.jump(normalEdge(join));

        out.appendTo(join);
        out.branch(phi, normalEdge(taken), normalEdge(notTaken));

        out.appendTo(taken);
        out.ret(argument);

        out.appendTo(notTaken);
        out.ret(out.neg(argument));

        Procedure& procedure = test.procedure();
        AOT_TEST_CHECK(validate(procedure));
        threadJumps(procedure);
        AOT_TEST_CHECK(!procedure.failure() && left->upsilons().empty() && right->upsilons().empty());
        AOT_TEST_CHECK(asksAsOfEmittedOperations ? !validate(procedure) : validateBehindBlockPasses(procedure));
    }
}

static void checkerRefusesStoreBehindStackSlot()
{
    for (bool reachesBehindSlot : { false, true }) {
        TestProcedure test;
        Output& out = test.out();
        Operation* argument = test.argumentGPR(0);
        Operation* slot = out.lockedStackSlot(3 * sizeof(uint64_t));
        Operation* store = out.store64(argument, out.address(test.heaps().frameSlots, slot, (reachesBehindSlot ? 3 : 2) * sizeof(uint64_t)));
        out.ret(argument);

        Procedure& procedure = test.procedure();
        AOT_TEST_CHECK(validate(procedure) != reachesBehindSlot);
        AOT_TEST_CHECK(reachesBehindSlot ? checkerReportedFailureAt(procedure, FailureKind::MalformedProcedure, { store }) : !procedure.failure());
    }
}

static void checkerRefusesChangedSourceRegister()
{
    TestProcedure test;
    Output& out = test.out();
    Operation* left = test.argumentGPR(0);
    Operation* right = test.argumentGPR(1);
    out.setOrigin(Origin(7));
    Operation* difference = out.sub(left, right);
    out.setOrigin(Origin());
    out.ret(difference);
    prepareForCheckerTest(test);

    Step& step = findExecuteStepForCheckerTest(test, difference);
    AOT_CHECKER_TEST_CHECK(test, step.sources[0].isRegister() && step.sources[1].isRegister() && step.sources[0] != step.sources[1]);
    step.sources[0] = step.sources[1];
    expectCheckerRefusal(test, checkAllocation, FailureKind::WrongValueInRegister, { difference });
    AOT_CHECKER_TEST_CHECK(test, test.procedure().failure()->origin == Origin(7));
}

static void checkerRefusesDroppedReload()
{
    TestProcedure test;
    Block* block = buildPressureForCheckerTest(test);
    prepareForCheckerTest(test);

    uint32_t reload = findStepForCheckerTest(test, block, StepKind::Reload);
    const Operation* reloaded = test.procedure().allocation().step(reload).operation;
    const Operation* reader = readerAfterStepForCheckerTest(test, block, reload);
    AOT_CHECKER_TEST_CHECK(test, std::ranges::any_of(reader->children(), [&](const Operation* child) { return child == reloaded; }));
    dropStepForCheckerTest(test, block, reload);
    expectCheckerRefusal(test, checkAllocation, FailureKind::WrongValueInRegister, { reader });
}

static void checkerRefusesDroppedEdgeMove()
{
    TestProcedure test;
    CheckerTestDiamond diamond = buildDiamondForCheckerTest(test, true);
    prepareForCheckerTest(test);

    dropStepForCheckerTest(test, diamond.right, findStepForCheckerTest(test, diamond.right, StepKind::Move));
    expectCheckerRefusal(test, checkAllocation, FailureKind::WrongValueInRegister, { diamond.phi });
}

static void expectCheckerToRefuseSlotThatOneOfTwoEdgesDoesNotWrite(SpillIndex slotsInFront)
{
    TestProcedure test;
    Output& out = test.out();
    unsigned count = static_cast<unsigned>(std::popcount(registerFileFor(test.procedure().traits()).allocatableIn(Bank::GP))) + slotsInFront + 8;
    Operation* selector = test.argumentGPR(0);
    Operation* seed = test.argumentGPR(1);
    Block* left = out.newBlock();
    Block* right = out.newBlock();
    Block* join = out.newBlock();
    Vector<Operation*> phis;
    for (unsigned index = 0; index < count; ++index)
        phis.append(out.phiIn(join, Int64));
    out.branch(out.testNonZero64(selector, 1), normalEdge(left), normalEdge(right));
    for (Block* arm : { left, right }) {
        out.appendTo(arm);
        Vector<Operation*> values = defineValues(test, arm == left ? seed : selector, count, Int64);
        for (unsigned index = 0; index < count; ++index)
            out.upsilon(values[index], phis[index]);
        out.jump(normalEdge(join));
    }
    out.appendTo(join);
    Operation* sum = phis[0];
    for (Operation* phi : phis.span().subspan(1))
        sum = appendPatchForCheckerTest(out, Int64, { sum, phi });
    out.ret(sum);
    prepareForCheckerTest(test);

    const Allocation& allocation = test.procedure().allocation();
    AOT_CHECKER_TEST_CHECK(test, test.procedure().order().of(left).orderIndex < test.procedure().order().of(right).orderIndex);
    const BlockSteps& range = allocation.stepsOf(right);
    const Operation* leftOut = nullptr;
    uint32_t save = range.endOfEdgeMoves;
    for (const Operation* phi : phis) {
        SpillIndex spill = allocation.spillOf(phi);
        if (leftOut || spill == noSpill || spill < slotsInFront)
            continue;
        for (uint32_t index = range.beginOfEdgeMoves; index < range.endOfEdgeMoves; ++index) {
            const Step& step = allocation.step(index);
            if (step.kind != StepKind::Save || !step.destination.isSpill() || step.destination.spillIndex() != spill)
                continue;
            leftOut = phi;
            save = index;
        }
    }
    AOT_CHECKER_TEST_CHECK(test, leftOut);
    dropStepForCheckerTest(test, right, save);
    expectCheckerRefusal(test, checkAllocation, FailureKind::WrongValueInSpillSlot, { leftOut });
}

static void checkerRefusesSlotBeyondFirstThirtyTwoThatOneOfTwoEdgesDoesNotWrite()
{
    expectCheckerToRefuseSlotThatOneOfTwoEdgesDoesNotWrite(32);
}

static void checkerRefusesSlotBeyondFirst32768ThatOneOfTwoEdgesDoesNotWrite()
{
    expectCheckerToRefuseSlotThatOneOfTwoEdgesDoesNotWrite(32768);
}

static SpillFacts& findSlotBeyondFirstThirtyTwoReloadedInForCheckerTest(TestProcedure& test, const Block* block)
{
    const Allocation& allocation = test.procedure().allocation();
    const BlockSteps& range = allocation.stepsOf(block);
    for (uint32_t index = range.begin; index < range.end; ++index) {
        const Step& step = allocation.step(index);
        if (step.kind == StepKind::Reload && step.sources[0].isSpill() && step.sources[0].spillIndex() >= 32)
            return test.procedure().allocationForTesting().spillsForTesting()[step.sources[0].spillIndex()];
    }
    failCheckerTest(test, __FILE__, __LINE__, "the block reloads a slot beyond the first thirty-two");
}

static void expectCheckerToMissValueOfSlot(TestProcedure& test)
{
    AOT_CHECKER_TEST_CHECK(test, !checkAllocation(test.procedure()));
    const std::optional<Failure>& failure = test.procedure().failure();
    AOT_CHECKER_TEST_CHECK(test, failure && (failure->kind == FailureKind::WrongValueInRegister || failure->kind == FailureKind::WrongValueInSpillSlot));
}

static void checkerRefusesSlotBeyondFirstThirtyTwoWhoseGapLiesOverRareBlock()
{
    TestProcedure test;
    Output& out = test.out();
    unsigned count = static_cast<unsigned>(std::popcount(registerFileFor(test.procedure().traits()).allocatableIn(Bank::GP))) + 40;
    Operation* selector = test.argumentGPR(0);
    Vector<Operation*> values = defineValues(test, test.argumentGPR(1), count, Int64);
    Block* slow = out.newBlock();
    Block* join = out.newBlock();
    out.branch(out.testNonZero64(selector, 1), rareEdge(slow), normalEdge(join));
    out.appendTo(slow);
    appendPatchForCheckerTest(out, Void, { });
    out.jump(normalEdge(join));
    out.appendTo(join);
    Operation* sum = values[0];
    for (Operation* value : values.span().subspan(1))
        sum = appendPatchForCheckerTest(out, Int64, { sum, value });
    out.ret(sum);
    prepareForCheckerTest(test);

    Position headOfSlow = test.procedure().order().headOf(slow);
    Position headOfJoin = test.procedure().order().headOf(join);
    SpillFacts& facts = findSlotBeyondFirstThirtyTwoReloadedInForCheckerTest(test, join);
    AOT_CHECKER_TEST_CHECK(test, facts.begin < headOfSlow && headOfSlow < headOfJoin && headOfJoin <= facts.end && !facts.hasGap());
    facts.beginOfGap = headOfSlow;
    facts.endOfGap = headOfJoin - 1;
    expectCheckerToMissValueOfSlot(test);
}

static void checkerRefusesSlotBeyondFirstThirtyTwoWhoseRangeEndsInFrontOfLatch()
{
    TestProcedure test;
    Output& out = test.out();
    unsigned count = static_cast<unsigned>(std::popcount(registerFileFor(test.procedure().traits()).allocatableIn(Bank::GP))) + 40;
    Operation* rounds = test.argumentGPR(0);
    Vector<Operation*> values = defineValues(test, test.argumentGPR(1), count, Int64);
    Block* head = out.newBlock();
    Block* latch = out.newBlock();
    Block* exit = out.newBlock();
    Operation* counter = out.phiIn(head, Int64);
    out.upsilon(rounds, counter);
    out.jump(normalEdge(head));
    out.appendTo(head);
    for (Operation* value : values)
        appendPatchForCheckerTest(out, Void, { value });
    out.branch(out.notZero64(counter), normalEdge(latch), normalEdge(exit));
    out.appendTo(latch);
    out.upsilon(out.sub(counter, 1), counter);
    out.jump(normalEdge(head));
    out.appendTo(exit);
    out.ret(counter);
    prepareForCheckerTest(test);

    Position headOfLatch = test.procedure().order().headOf(latch);
    SpillFacts& facts = findSlotBeyondFirstThirtyTwoReloadedInForCheckerTest(test, head);
    AOT_CHECKER_TEST_CHECK(test, test.procedure().order().headOf(head) < headOfLatch && headOfLatch <= facts.end && !facts.hasGap());
    facts.end = headOfLatch - 1;
    expectCheckerToMissValueOfSlot(test);
}

static std::optional<uint64_t> checkerTestInstructionsToCheckValuesHeldAcrossBlocks(unsigned count)
{
    TestProcedure test;
    Output& out = test.out();
    Operation* selector = test.argumentGPR(0);
    Vector<Operation*> values = defineValues(test, test.argumentGPR(1), count, Int64);
    for (unsigned number = 0; number < count; ++number) {
        Block* taken = out.newBlock();
        Block* next = out.newBlock();
        out.branch(out.testNonZero64(selector, number + 1), normalEdge(taken), normalEdge(next));
        out.appendTo(taken);
        appendPatchForCheckerTest(out, Void, { });
        out.jump(normalEdge(next));
        out.appendTo(next);
    }
    Operation* sum = values[0];
    for (Operation* value : values.span().subspan(1))
        sum = appendPatchForCheckerTest(out, Int64, { sum, value });
    out.ret(sum);
    prepareOrFail(test);

    bool findsNothing = false;
    std::optional<uint64_t> instructions = instructionsOfCurrentThreadIn([&] {
        findsNothing = checkAllocation(test.procedure());
    });
    AOT_CHECKER_TEST_CHECK(test, findsNothing);
    return instructions;
}

static void checkerWorkGrowsAsValuesHeldAcrossBlocksDo()
{
    constexpr unsigned count = 400;
    std::optional<uint64_t> smaller = checkerTestInstructionsToCheckValuesHeldAcrossBlocks(count);
    std::optional<uint64_t> large = checkerTestInstructionsToCheckValuesHeldAcrossBlocks(4 * count);
    AOT_TEST_CHECK(growsAsSizeDoes(smaller, large));
}

static void checkerRefusesExchangedMovesOfCycle()
{
    constexpr unsigned length = 3;
    TestProcedure test;
    Output& out = test.out();
    Operation* rounds = test.argumentGPR(0);
    Vector<Operation*> starts = defineValues(test, test.argumentGPR(1), length, Int64);
    Block* head = out.newBlock();
    Block* latch = out.newBlock();
    Block* exit = out.newBlock();
    Operation* counter = out.phiIn(head, Int64);
    out.upsilon(rounds, counter);
    Operation* phis[length];
    for (unsigned index = 0; index < length; ++index) {
        phis[index] = out.phiIn(head, Int64);
        out.upsilon(starts[index], phis[index]);
    }
    out.jump(normalEdge(head));

    out.appendTo(head);
    out.branch(out.notZero64(counter), normalEdge(latch), normalEdge(exit));

    out.appendTo(latch);
    for (unsigned index = 0; index < length; ++index)
        out.upsilon(phis[(index + 1) % length], phis[index]);
    out.upsilon(out.sub(counter, 1), counter);
    out.jump(normalEdge(head));

    out.appendTo(exit);
    out.ret(out.sub(out.sub(phis[0], phis[1]), phis[2]));
    prepareForCheckerTest(test);

    std::span<Step> steps = test.procedure().allocationForTesting().stepsForTesting();
    uint32_t end = test.procedure().allocation().stepsOf(latch).endOfEdgeMoves;
    uint32_t first = findStepForCheckerTest(test, latch, StepKind::Swap);
    uint32_t second = first + 1;
    while (second < end && steps[second].kind != StepKind::Swap)
        ++second;
    AOT_CHECKER_TEST_CHECK(test, second < end);
    std::swap(steps[first], steps[second]);
    expectCheckerRefusal(test, checkAllocation, FailureKind::WrongValueInRegister, { phis[0], phis[1], phis[2] });
}

static void checkerRefusesValueLeftInClobberedRegister()
{
    TestProcedure test;
    Output& out = test.out();
    Operation* kept = test.argumentGPR(0);
    Reg clobbered = kept->argumentReg();
    PatchOperation* patch = appendPatchForCheckerTest(out, Int64, { test.argumentGPR(1) }, maskOf(clobbered));
    Operation* difference = out.sub(patch, kept);
    out.ret(difference);
    prepareForCheckerTest(test);

    patchAssignmentForCheckerTest(test, patch).usedRegisters |= maskOf(clobbered);
    AOT_CHECKER_TEST_CHECK(test, checkAllocation(test.procedure()));

    Step& step = findExecuteStepForCheckerTest(test, difference);
    AOT_CHECKER_TEST_CHECK(test, step.sources[1].isRegister() && step.sources[1].reg() != clobbered);
    step.sources[1] = Location::reg(clobbered);
    expectCheckerRefusal(test, checkAllocation, FailureKind::WrongValueInRegister, { difference });
}

static void checkerRefusesLandingPadThatReadsCallerSavedRegister()
{
    ProcedureTraits traits;
    traits.hasHandlers = true;
    TestProcedure test(traits);
    Output& out = test.out();
    Operation* kept = test.argumentGPR(0);
    Operation* tested = test.argumentGPR(1);
    Block* continuation = out.newBlock();
    Block* landingPad = out.newBlock();
    RegisterMask callerSaved = callerSavedRegistersForCheckerTest(test);

    PatchOperation* patch = appendPatchForCheckerTest(out, Int64, { tested });
    out.appendSuccessor(normalEdge(continuation));
    out.appendSuccessor(rareEdge(landingPad));
    patch->clobberLateOnSuccessor(1, registerSetOf(callerSaved));
    patch->effects.terminal = true;

    out.appendTo(continuation);
    out.ret(patch);
    out.appendTo(landingPad);
    out.ret(kept);
    prepareForCheckerTest(test);

    Step& reload = test.procedure().allocationForTesting().stepsForTesting()[findStepForCheckerTest(test, landingPad, StepKind::Reload)];
    AOT_CHECKER_TEST_CHECK(test, landingPad->isLandingPad() && reload.operation == kept && (callerSaved & maskOf(kept->argumentReg())));
    reload.kind = StepKind::Move;
    reload.sources[0] = Location::reg(kept->argumentReg());
    expectCheckerRefusal(test, checkAllocation, FailureKind::WrongValueInRegister, { landingPad->last() });
}

static void checkerRefusesRareRegionThatDoesNotRestore()
{
    TestProcedure test;
    CheckerTestRareArm arm = buildRareArmForCheckerTest(test);
    prepareForCheckerTest(test);

    uint32_t reload = findStepForCheckerTest(test, arm.slow, StepKind::Reload);
    const Operation* restored = test.procedure().allocation().step(reload).operation;
    AOT_CHECKER_TEST_CHECK(test, test.procedure().order().of(arm.slow).isRare() && arm.values.contains(restored));
    dropStepForCheckerTest(test, arm.slow, reload);
    expectCheckerRefusal(test, checkAllocation, FailureKind::WrongValueInRegister, { restored });
}

static void checkerRefusesRareRegionThatDoesNotSave()
{
    TestProcedure test;
    CheckerTestRareArm arm = buildRareArmForCheckerTest(test);
    prepareForCheckerTest(test);

    dropStepForCheckerTest(test, arm.slow, findStepForCheckerTest(test, arm.slow, StepKind::Save, arm.values[0]));
    expectCheckerRefusal(test, checkAllocation, FailureKind::WrongValueInSpillSlot, { arm.coldReader });
}

static void checkerRefusesHeadOfFramedPartThatDoesNotSaveWhatIsReadBehindMeetingOfHeads()
{
    for (bool rareHeadLies : { true, false }) {
        TestProcedure test;
        Output& out = test.out();
        Operation* selector = test.argumentGPR(0);
        unsigned numberOfCalleeSaves = 0;
        forEachRegister(registerFileFor(test.procedure().traits()).calleeSavedIn(Bank::GP), [&](Reg) {
            ++numberOfCalleeSaves;
        });
        Vector<Operation*> values = defineValues(test, test.argumentGPR(1), numberOfCalleeSaves + 3, Int64);
        Block* slow = out.newBlock();
        Block* guard = out.newBlock();
        Block* returning = out.newBlock();
        Block* head = out.newBlock();
        Block* meeting = out.newBlock();
        out.branch(out.testNonZero64(selector, 2), rareEdge(slow), normalEdge(guard));
        out.appendTo(guard);
        out.branch(out.testNonZero64(selector, 1), normalEdge(returning), normalEdge(head));
        out.appendTo(returning);
        out.ret(selector);
        out.appendTo(head);
        appendPatchForCheckerTest(out, Void, { }, callerSavedRegistersForCheckerTest(test));
        out.jump(normalEdge(meeting));
        out.appendTo(slow);
        appendPatchForCheckerTest(out, Void, { }, callerSavedRegistersForCheckerTest(test));
        out.jump(normalEdge(meeting));
        out.appendTo(meeting);
        appendPatchForCheckerTest(out, Void, { }, callerSavedRegistersForCheckerTest(test));
        Operation* sum = values[0];
        for (size_t index = 1; index < values.size(); ++index)
            sum = out.add(sum, values[index]);
        out.ret(sum);
        prepareForCheckerTest(test);

        Allocation& allocation = test.procedure().allocationForTesting();
        AOT_CHECKER_TEST_CHECK(test, allocation.isHeadOfFramedPart(head) && allocation.isHeadOfFramedPart(slow) && allocation.isInFramedPart(meeting) && !allocation.isHeadOfFramedPart(meeting));
        bool hasLied = false;
        for (Allocation::HeadOfFramedPart& lying : allocation.headsOfFramedPartForTesting()) {
            if (lying.block != (rareHeadLies ? slow : head))
                continue;
            AOT_CHECKER_TEST_CHECK(test, lying.beginOfSteps < lying.endOfSteps && allocation.step(lying.endOfSteps - 1).kind == StepKind::Save);
            --lying.endOfSteps;
            hasLied = true;
        }
        AOT_CHECKER_TEST_CHECK(test, hasLied);
        AOT_CHECKER_TEST_CHECK(test, !checkAllocation(test.procedure()));
        AOT_CHECKER_TEST_CHECK(test, test.procedure().failure() && (test.procedure().failure()->kind == FailureKind::WrongValueInRegister || test.procedure().failure()->kind == FailureKind::WrongValueInSpillSlot));
    }
}

struct CheckerTestThreeHeads {
    Block* first { nullptr };
    Block* second { nullptr };
    Block* third { nullptr };
    uint32_t saveAtThirdOfWhatOthersSave { UINT32_MAX };
};

static std::optional<CheckerTestThreeHeads> buildHeadEnteredOutOfOtherPartsForCheckerTest(TestProcedure& test)
{
    Output& out = test.out();
    Operation* selector = test.argumentGPR(0);
    RegisterFile file = registerFileFor(test.procedure().traits());
    RegisterMask everyRegister = file.allocatableIn(Bank::GP) | file.allocatableIn(Bank::FP);
    Operation* value = defineValues(test, test.argumentGPR(1), 1, Int64)[0];
    auto appendCallAndReadInRegisterThatNoCallerCountsOn = [&] {
        appendPatchForCheckerTest(out, Void, { }, everyRegister);
        PatchOperation* reader = out.patch(Void);
        reader->append(value, ValueRep::reg(GPRInfo::argumentGPR2));
        reader->setGenerator([](CCallHelpers&, const PatchParameters&) { });
    };
    CheckerTestThreeHeads heads { out.newBlock(), out.newBlock(), out.newBlock() };
    Block* usual = out.newBlock();
    Block* other = out.newBlock();
    Block* returningFromUsual = out.newBlock();
    Block* returningFromOther = out.newBlock();
    out.branch(out.testNonZero64(selector, 1), unlikelyEdge(other), normalEdge(usual));
    out.appendTo(usual);
    out.branch(out.testNonZero64(selector, 2), normalEdge(returningFromUsual), normalEdge(heads.first));
    out.appendTo(returningFromUsual);
    out.ret(selector);
    out.appendTo(other);
    out.branch(out.testNonZero64(selector, 4), normalEdge(returningFromOther), normalEdge(heads.second));
    out.appendTo(returningFromOther);
    out.ret(selector);
    out.appendTo(heads.first);
    appendCallAndReadInRegisterThatNoCallerCountsOn();
    out.jump(normalEdge(heads.third));
    out.appendTo(heads.second);
    appendCallAndReadInRegisterThatNoCallerCountsOn();
    out.jump(normalEdge(heads.third));
    out.appendTo(heads.third);
    appendPatchForCheckerTest(out, Void, { }, everyRegister);
    out.ret(out.add(value, 1));
    prepareForCheckerTest(test);

    const Allocation& allocation = test.procedure().allocation();
    auto savesAtItsHead = [&](const Block* block, Location slot) {
        const Allocation::HeadOfFramedPart& head = allocation.headOfFramedPartAbove(block);
        for (uint32_t index = head.beginOfSteps; index < head.endOfSteps; ++index) {
            if (allocation.step(index).destination == slot)
                return true;
        }
        return false;
    };
    bool everyOneIsHead = !allocation.needsFrameAheadOfFramedPart() && allocation.headsOfFramedPart().size() == 3
        && allocation.isHeadOfFramedPart(heads.first) && allocation.isHeadOfFramedPart(heads.second) && allocation.isHeadOfFramedPart(heads.third);
    if (everyOneIsHead) {
        const Allocation::HeadOfFramedPart& third = allocation.headOfFramedPartAbove(heads.third);
        for (uint32_t index = third.beginOfSteps; index < third.endOfSteps && heads.saveAtThirdOfWhatOthersSave == UINT32_MAX; ++index) {
            Location slot = allocation.step(index).destination;
            if (allocation.step(index).kind == StepKind::Save && savesAtItsHead(heads.first, slot) && savesAtItsHead(heads.second, slot))
                heads.saveAtThirdOfWhatOthersSave = index;
        }
    }
    bool hasShape = heads.saveAtThirdOfWhatOthersSave != UINT32_MAX;
    AOT_CHECKER_TEST_CHECK(test, hasShape || !isX86_64());
    if (!hasShape) {
        dataLogLn("NOT RUN, the allocator makes no head here that is entered out of other parts and saves what their heads save");
        return std::nullopt;
    }
    AOT_CHECKER_TEST_CHECK(test, checkSavesAtHeads(test.procedure()) && !test.procedure().failure());
    return heads;
}

static void checkerOfSavesAtHeadsRefusesHeadEnteredOutOfOtherPartsThatDoesNotSave()
{
    TestProcedure test;
    std::optional<CheckerTestThreeHeads> heads = buildHeadEnteredOutOfOtherPartsForCheckerTest(test);
    if (!heads)
        return;

    Allocation& allocation = test.procedure().allocationForTesting();
    bool hasLied = false;
    for (Allocation::HeadOfFramedPart& lying : allocation.headsOfFramedPartForTesting()) {
        if (lying.block != heads->third)
            continue;
        std::swap(allocation.stepsForTesting()[heads->saveAtThirdOfWhatOthersSave], allocation.stepsForTesting()[lying.endOfSteps - 1]);
        --lying.endOfSteps;
        hasLied = true;
    }
    AOT_CHECKER_TEST_CHECK(test, hasLied);
    AOT_CHECKER_TEST_CHECK(test, !checkSavesAtHeads(test.procedure()));
    AOT_CHECKER_TEST_CHECK(test, test.procedure().failure() && test.procedure().failure()->kind == FailureKind::WrongValueInSpillSlot);
}

static void checkerRefusesTwoLiveSpillsInOneSlot()
{
    TestProcedure test;
    buildPressureForCheckerTest(test);
    prepareForCheckerTest(test);

    Procedure& procedure = test.procedure();
    std::span<const SpillFacts> spills = procedure.allocation().spills();
    AOT_CHECKER_TEST_CHECK(test, spills.size() >= 2 && spills[0].begin <= spills[1].end && spills[1].begin <= spills[0].end);
    Frame& frame = procedure.frameForTesting();
    AOT_CHECKER_TEST_CHECK(test, frame.offsetFromFPOf(0) != frame.offsetFromFPOf(1));
    frame.setOffsetFromFPForTesting(1, frame.offsetFromFPOf(0));
    expectCheckerRefusal(test, checkFrame, FailureKind::WrongValueInSpillSlot, { spills[0].value, spills[1].value });
}

static void checkerRefusesPatchWithoutTheFrameItNeeds()
{
    TestProcedure test;
    Output& out = test.out();
    PatchOperation* patch = appendPatchForCheckerTest(out, Int64, { test.argumentGPR(0) });
    patch->setRunsWithoutFrame(true);
    patch->setShowsRegistersToCollector(true);
    out.ret(patch);
    prepareForCheckerTest(test);
    AOT_CHECKER_TEST_CHECK(test, !test.procedure().frame().hasFrame());

    patch->setRunsWithoutFrame(false);
    expectCheckerRefusal(test, checkFrame, FailureKind::MissingFrameSetup, { patch });
}

static void checkerFindsOwnerOfStorageThatLandingPadReads()
{
    ProcedureTraits traits;
    traits.hasHandlers = true;
    TestProcedure test(traits);
    Output& out = test.out();
    AbstractHeap storageOfOwner(test.heaps().argumentBuffer.index(), 0, OptionSet<HeapTrait> { HeapTrait::HoldsInnerPointer });
    Operation* owner = out.opaque(test.argumentGPR(0));
    Operation* storage = out.loadPtr(out.address(storageOfOwner, owner));
    Block* continuation = out.newBlock();
    Block* landingPad = out.newBlock();
    RegisterMask callerSaved = callerSavedRegistersForCheckerTest(test);

    PatchOperation* patch = appendPatchForCheckerTest(out, Void, { test.argumentGPR(1) });
    out.appendSuccessor(normalEdge(continuation));
    out.appendSuccessor(rareEdge(landingPad));
    patch->clobberLateOnSuccessor(1, registerSetOf(callerSaved));
    patch->effects.terminal = true;

    out.appendTo(continuation);
    out.ret(owner);
    out.appendTo(landingPad);
    PatchOperation* safepoint = appendPatchForCheckerTest(out, Void, { }, callerSaved);
    out.ret(out.load64(out.address(test.heaps().argumentBuffer, storage, 2 * sizeof(uint64_t))));
    AOT_CHECKER_TEST_CHECK(test, safepoint->effects.mayCollect && landingPad->isLandingPad() && storage->has(OperationFlag::IsInnerPointer));
    prepareForCheckerTest(test);

    const Order& order = test.procedure().order();
    AOT_CHECKER_TEST_CHECK(test, order.headOf(landingPad) < order.headOf(continuation) && test.procedure().allocation().isSpilled(owner));
}

static void checkerRefusesInnerPointerWhoseOwnerPatchOverwrites()
{
    TestProcedure test;
    Output& out = test.out();
    AbstractHeap storageOfOwner(test.heaps().argumentBuffer.index(), 0, OptionSet<HeapTrait> { HeapTrait::HoldsInnerPointer });
    Operation* owner = out.opaque(test.argumentGPR(0));
    Operation* storage = out.loadPtr(out.address(storageOfOwner, owner));
    PatchOperation* safepoint = appendPatchForCheckerTest(out, Void, { });
    out.ret(out.load64(out.address(test.heaps().argumentBuffer, storage, 2 * sizeof(uint64_t))));
    AOT_CHECKER_TEST_CHECK(test, safepoint->effects.mayCollect && storage->has(OperationFlag::IsInnerPointer));
    prepareForCheckerTest(test);

    const Allocation& allocation = test.procedure().allocation();
    const Reg registerOfOwner = allocation.registerAtDefinitionOf(owner);
    PatchAssignment& assignment = patchAssignmentForCheckerTest(test, safepoint);
    AOT_CHECKER_TEST_CHECK(test, test.procedure().liveness().of(owner).has(ValueFlag::IsKeptForCollector) && !allocation.isSpilled(owner));
    AOT_CHECKER_TEST_CHECK(test, registerOfOwner.isSet() && (assignment.usedRegisters & maskOf(registerOfOwner)));

    assignment.usedRegisters &= ~maskOf(registerOfOwner);
    expectCheckerRefusal(test, checkReferences, FailureKind::InnerPointerWithoutOwner, { storage });
}

static void checkerRefusesNarrowReferenceWithoutPointer()
{
    for (bool isMarkedBeforeAllocation : { true, false }) {
        TestProcedure test;
        Output& out = test.out();
        Operation* reference = out.opaque(out.castToInt32(test.argumentGPR(0)));
        if (isMarkedBeforeAllocation)
            reference->markAsNarrowReference();
        PatchOperation* safepoint = appendPatchForCheckerTest(out, Void, { });
        AOT_TEST_CHECK(safepoint->effects.mayCollect);
        out.ret(out.zeroExtPtr(reference));
        prepareForCheckerTest(test);

        bool hasPointer = std::ranges::any_of(test.procedure().allocation().spills(), [&](const SpillFacts& spill) {
            return spill.isPointerBesideNarrowReference && spill.value == reference;
        });
        AOT_CHECKER_TEST_CHECK(test, hasPointer == isMarkedBeforeAllocation);
        if (isMarkedBeforeAllocation)
            continue;

        reference->markAsNarrowReference();
        expectCheckerRefusal(test, checkReferences, FailureKind::NarrowReferenceWithoutPointer, { reference, safepoint });
    }
}

enum class DamageToPointerKeptInRareBlock : uint8_t {
    IsNotKept,
    IsKeptBehindSafepoint,
    IsKeptInBlockInFrontAlone,
};

static void checkerRefusesRareSafepointOutsideFramedPartWithoutPointerKeptAheadOfItInItsBlock()
{
    for (DamageToPointerKeptInRareBlock damage : { DamageToPointerKeptInRareBlock::IsNotKept, DamageToPointerKeptInRareBlock::IsKeptBehindSafepoint, DamageToPointerKeptInRareBlock::IsKeptInBlockInFrontAlone }) {
        TestProcedure test;
        Output& out = test.out();
        RegisterMask callerSaved = registerFileFor(test.procedure().traits()).allocatableIn(Bank::GP) & ~registerFileFor(test.procedure().traits()).calleeSaved;
        Operation* argument = test.argumentGPR(0);
        Operation* reference = out.opaque(out.castToInt32(argument));
        reference->markAsNarrowReference();
        Block* slow = out.newBlock();
        Block* slowOn = out.newBlock();
        Block* guard = out.newBlock();
        Block* returning = out.newBlock();
        Block* calling = out.newBlock();
        out.branch(out.testNonZero64(argument, 2), rareEdge(slow), normalEdge(guard));
        out.appendTo(slow);
        PatchOperation* first = appendPatchForCheckerTest(out, Void, { }, callerSaved);
        out.branch(out.testNonZero64(out.zeroExtPtr(reference), 8), normalEdge(slowOn), normalEdge(guard));
        out.appendTo(slowOn);
        PatchOperation* second = appendPatchForCheckerTest(out, Void, { }, callerSaved);
        out.jump(guard);
        out.appendTo(guard);
        out.branch(out.testNonZero64(out.zeroExtPtr(reference), 1), normalEdge(returning), normalEdge(calling));
        out.appendTo(returning);
        out.ret(out.constInt64(5));
        out.appendTo(calling);
        appendPatchForCheckerTest(out, Void, { }, callerSaved);
        out.ret(out.zeroExtPtr(reference));
        prepareForCheckerTest(test);

        Procedure& procedure = test.procedure();
        const Allocation& allocation = procedure.allocation();
        AOT_CHECKER_TEST_CHECK(test, first->effects.mayCollect && procedure.frame().coverage() == FrameCoverage::FramedPart && procedure.order().of(slow).isRare() && procedure.order().of(slowOn).isRare());
        AOT_CHECKER_TEST_CHECK(test, !allocation.isInFramedPart(slow) && !allocation.isInFramedPart(slowOn));
        const Block* damaged = damage == DamageToPointerKeptInRareBlock::IsKeptInBlockInFrontAlone ? slowOn : slow;
        const PatchOperation* safepoint = damaged == slow ? first : second;
        std::span<Step> steps = procedure.allocationForTesting().stepsForTesting();
        const BlockSteps& range = allocation.stepsOf(damaged);
        uint32_t keeps = range.end;
        uint32_t executes = range.end;
        for (uint32_t index = range.begin; index < range.end; ++index) {
            if (steps[index].kind == StepKind::KeepPointer && steps[index].operation == reference)
                keeps = index;
            if (steps[index].kind == StepKind::Execute && steps[index].operation == safepoint)
                executes = index;
        }
        AOT_CHECKER_TEST_CHECK(test, keeps < executes && executes < range.end);
        if (damage == DamageToPointerKeptInRareBlock::IsKeptBehindSafepoint)
            std::swap(steps[keeps], steps[executes]);
        else
            steps[keeps].operation = nullptr;
        expectCheckerRefusal(test, checkReferences, FailureKind::NarrowReferenceWithoutPointer, { reference });
    }
}

static void checkerRefusesFrameAddressReadLateBySuspension()
{
    ProcedureTraits traits;
    traits.savesFrameOnSuspend = true;
    TestProcedure test(traits);
    Output& out = test.out();
    Operation* offset = test.argumentGPR(0);
    Operation* address = out.add(out.lockedStackSlot(2 * sizeof(uint64_t)), offset);
    AOT_TEST_CHECK(!address->hasNoPosition());
    PatchOperation* patch = appendPatchForCheckerTest(out, Void, { });
    patch->append(address, ValueRep::LateColdAny);
    out.ret(offset);
    prepareForCheckerTest(test);

    patch->setIsSuspension(true);
    expectCheckerRefusal(test, checkReferences, FailureKind::FrameAddressAcrossSuspension, { address });
}

static void checkerRefusesShiftedNarrowReferenceWithoutPointer()
{
    for (bool isMarkedBeforeAllocation : { true, false }) {
        TestProcedure test;
        Output& out = test.out();
        Operation* reference = out.opaque(out.castToInt32(test.argumentGPR(0)));
        if (isMarkedBeforeAllocation)
            reference->markAsNarrowReference();
        Operation* widened = out.zeroExtPtr(reference);
        Operation* shifted = out.shl(widened, 3);
        PatchOperation* safepoint = appendPatchForCheckerTest(out, Void, { shifted });
        AOT_TEST_CHECK(safepoint->effects.mayCollect);
        out.ret(shifted);
        prepareForCheckerTest(test);

        bool hasPointer = std::ranges::any_of(test.procedure().allocation().spills(), [&](const SpillFacts& spill) {
            return spill.isPointerBesideNarrowReference && spill.value == reference;
        });
        AOT_CHECKER_TEST_CHECK(test, hasPointer == isMarkedBeforeAllocation);
        if (isMarkedBeforeAllocation)
            continue;

        reference->markAsNarrowReference();
        expectCheckerRefusal(test, checkReferences, FailureKind::NarrowReferenceWithoutPointer, { widened, shifted });
    }
}

struct SumTestedByOverflowForCheckerTest {
    Operation* left;
    Operation* lowHalf;
    Operation* branch;
    Operation* constant;
};

static SumTestedByOverflowForCheckerTest buildSumTestedByOverflowForCheckerTest(TestProcedure& test)
{
    Output& out = test.out();
    Operation* left = out.opaque(out.castToInt32(test.argumentGPR(0)));
    Operation* right = out.opaque(out.castToInt32(test.argumentGPR(1)));
    Operation* sum = out.add(out.signExt32To64(left), out.signExt32To64(right));
    Operation* lowHalf = out.castToInt32(sum);
    Operation* constant = out.constInt32(7);
    Block* tests = out.m_block;
    Block* passes = out.newBlock();
    Block* fails = out.newBlock();
    out.branch(out.equal(out.signExt32To64(lowHalf), sum), normalEdge(passes), rareEdge(fails));
    out.appendTo(passes);
    out.ret(out.zeroExtPtr(lowHalf));
    out.appendTo(fails);
    out.ret(out.zeroExtPtr(left));
    prepareForCheckerTest(test);
    AOT_CHECKER_TEST_CHECK(test, test.procedure().liveness().usesOverflowFlag(tests->last()) && test.procedure().liveness().usesOverflowFlag(sum));
    return { left, lowHalf, tests->last(), constant };
}

static void checkerRefusesOperandThatIsNotTakenBack()
{
    TestProcedure test;
    SumTestedByOverflowForCheckerTest built = buildSumTestedByOverflowForCheckerTest(test);
    Step& branch = findExecuteStepForCheckerTest(test, built.branch);
    AOT_CHECKER_TEST_CHECK(test, branch.other == built.left);
    branch.other = nullptr;
    expectCheckerRefusal(test, checkAllocation, FailureKind::WrongValueInRegister, { built.left });
}

static void checkerRefusesStepThatMayWriteFlagsAheadOfBranchOnOverflow()
{
    TestProcedure test;
    SumTestedByOverflowForCheckerTest built = buildSumTestedByOverflowForCheckerTest(test);
    Step& betweenSumAndBranch = findExecuteStepForCheckerTest(test, built.lowHalf);
    betweenSumAndBranch.kind = StepKind::Materialize;
    betweenSumAndBranch.operation = built.constant;
    expectCheckerRefusal(test, checkAllocation, FailureKind::WrongValueInRegister, { built.constant });
}

#undef AOT_CHECKER_TEST_CHECK

} // namespace Machine

std::span<const NativeTest> machineCheckerTests()
{
    static constexpr NativeTest tests[] = {
        { "machine-checker/read-that-no-definition-dominates"_s, Machine::checkerRefusesReadThatNoDefinitionDominates },
        { "machine-checker/work-grows-as-a-row-of-guards-into-one-block-does"_s, Machine::checkerWorkGrowsAsRowOfGuardsIntoOneBlockDoes },
        { "machine-checker/edge-without-an-upsilon-for-a-phi"_s, Machine::checkerRefusesEdgeWithoutUpsilonForPhi },
        { "machine-checker/edge-without-an-upsilon-for-a-phi-behind-the-block-passes"_s, Machine::checkerRefusesEdgeWithoutUpsilonForPhiBehindBlockPasses },
        { "machine-checker/only-what-the-block-passes-leave-is-looked-at"_s, Machine::checkerLooksOnlyAtWhatBlockPassesLeave },
        { "machine-checker/store-behind-a-stack-slot"_s, Machine::checkerRefusesStoreBehindStackSlot },
        { "machine-checker/changed-source-register"_s, Machine::checkerRefusesChangedSourceRegister },
        { "machine-checker/dropped-reload"_s, Machine::checkerRefusesDroppedReload },
        { "machine-checker/dropped-edge-move"_s, Machine::checkerRefusesDroppedEdgeMove },
        { "machine-checker/a-slot-beyond-the-first-thirty-two-that-one-of-two-edges-does-not-write"_s, Machine::checkerRefusesSlotBeyondFirstThirtyTwoThatOneOfTwoEdgesDoesNotWrite },
        { "machine-checker/a-slot-beyond-the-first-32768-that-one-of-two-edges-does-not-write"_s, Machine::checkerRefusesSlotBeyondFirst32768ThatOneOfTwoEdgesDoesNotWrite },
        { "machine-checker/a-slot-beyond-the-first-thirty-two-whose-range-ends-in-front-of-the-latch"_s, Machine::checkerRefusesSlotBeyondFirstThirtyTwoWhoseRangeEndsInFrontOfLatch },
        { "machine-checker/a-slot-beyond-the-first-thirty-two-whose-gap-lies-over-a-rare-block"_s, Machine::checkerRefusesSlotBeyondFirstThirtyTwoWhoseGapLiesOverRareBlock },
        { "machine-checker/work-grows-as-values-held-across-blocks-do"_s, Machine::checkerWorkGrowsAsValuesHeldAcrossBlocksDo },
        { "machine-checker/exchanged-moves-of-a-cycle"_s, Machine::checkerRefusesExchangedMovesOfCycle },
        { "machine-checker/value-left-in-a-clobbered-register"_s, Machine::checkerRefusesValueLeftInClobberedRegister },
        { "machine-checker/landing-pad-reads-a-caller-saved-register"_s, Machine::checkerRefusesLandingPadThatReadsCallerSavedRegister },
        { "machine-checker/rare-region-does-not-restore"_s, Machine::checkerRefusesRareRegionThatDoesNotRestore },
        { "machine-checker/rare-region-does-not-save"_s, Machine::checkerRefusesRareRegionThatDoesNotSave },
        { "machine-checker/a-head-of-the-framed-part-does-not-save-what-is-read-behind-the-meeting-of-heads"_s, Machine::checkerRefusesHeadOfFramedPartThatDoesNotSaveWhatIsReadBehindMeetingOfHeads },
        { "machine-checker/a-head-entered-out-of-other-parts-that-does-not-save-is-refused-where-saves-at-heads-alone-are-checked"_s, Machine::checkerOfSavesAtHeadsRefusesHeadEnteredOutOfOtherPartsThatDoesNotSave },
        { "machine-checker/two-live-spills-in-one-slot"_s, Machine::checkerRefusesTwoLiveSpillsInOneSlot },
        { "machine-checker/patch-without-the-frame-it-needs"_s, Machine::checkerRefusesPatchWithoutTheFrameItNeeds },
        { "machine-checker/owner-of-storage-that-a-landing-pad-reads"_s, Machine::checkerFindsOwnerOfStorageThatLandingPadReads },
        { "machine-checker/an-inner-pointer-whose-owner-the-patch-overwrites"_s, Machine::checkerRefusesInnerPointerWhoseOwnerPatchOverwrites },
        { "machine-checker/narrow-reference-without-its-pointer"_s, Machine::checkerRefusesNarrowReferenceWithoutPointer },
        { "machine-checker/rare-safepoint-outside-the-framed-part-without-the-pointer-kept-ahead-of-it-in-its-block"_s, Machine::checkerRefusesRareSafepointOutsideFramedPartWithoutPointerKeptAheadOfItInItsBlock },
        { "machine-checker/shifted-narrow-reference-without-its-pointer"_s, Machine::checkerRefusesShiftedNarrowReferenceWithoutPointer },
        { "machine-checker/frame-address-read-late-by-a-suspension"_s, Machine::checkerRefusesFrameAddressReadLateBySuspension },
        { "machine-checker/operand-that-is-not-taken-back"_s, Machine::checkerRefusesOperandThatIsNotTakenBack },
        { "machine-checker/step-that-may-write-the-flags-ahead-of-a-branch-on-overflow"_s, Machine::checkerRefusesStepThatMayWriteFlagsAheadOfBranchOnOverflow },
    };
    return tests;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
