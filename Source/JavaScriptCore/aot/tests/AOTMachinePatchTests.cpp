/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTMachineTestSupport.h"
#include "AOTCellFormat.h"
#include "AOTConvention.h"
#include "AOTEntryEmitter.h"
#include "AllowMacroScratchRegisterUsage.h"
#include "JSCJSValueInlines.h"
#include <array>
#include <bit>

namespace JSC { namespace AOT { namespace Machine {

struct PatchTestObservation {
    unsigned runs { 0 };
    Vector<ValueRep, 8> reps;
    RegisterMask used { 0 };
    RegisterMask unavailable { 0 };
    RegisterMask scratch { 0 };
    bool frameIsSetUp { false };
};

static void recordPatchTestObservation(PatchTestObservation& observation, const PatchParameters& params)
{
    ++observation.runs;
    observation.reps.clear();
    for (const ValueRep& rep : params)
        observation.reps.append(rep);
    observation.used = params.usedRegistersMask();
    observation.unavailable = maskOf(params.unavailableRegisters());
    observation.scratch = 0;
    for (unsigned index = 0; index < params.value()->numGPScratchRegisters; ++index)
        observation.scratch |= maskOf(Reg(params.gpScratch(index)));
    for (unsigned index = 0; index < params.value()->numFPScratchRegisters; ++index)
        observation.scratch |= maskOf(Reg(params.fpScratch(index)));
    observation.frameIsSetUp = params.frameIsSetUp();
}

static RegisterMask registersAmongPatchTestReps(std::span<const ValueRep> reps)
{
    RegisterMask result = 0;
    for (const ValueRep& rep : reps) {
        if (rep.isReg())
            result |= maskOf(rep.reg());
    }
    return result;
}

static RegisterFile registerFileOfPatchTest(TestProcedure& test)
{
    return registerFileFor(test.procedure().traits());
}

static RegisterMask allocatableRegistersOfPatchTest(TestProcedure& test)
{
    RegisterFile file = registerFileOfPatchTest(test);
    return file.allocatableIn(Bank::GP) | file.allocatableIn(Bank::FP);
}

static RegisterMask callerSavedRegistersOfPatchTest(TestProcedure& test)
{
    return allocatableRegistersOfPatchTest(test) & ~registerFileOfPatchTest(test).calleeSaved;
}

static unsigned numberOfRegistersInPatchTest(TestProcedure& test, Bank bank)
{
    return static_cast<unsigned>(std::popcount(registerFileOfPatchTest(test).allocatableIn(bank)));
}

static RegisterMask firstArgumentRegistersForPatchTest()
{
    RegisterMask result = 0;
    for (unsigned index = 0; index < 4; ++index)
        result |= maskOf(Reg(GPRInfo::toArgumentRegister(index))) | maskOf(Reg(FPRInfo::toArgumentRegister(index)));
    return result;
}

static uint64_t patchTestInteger(int64_t seed, unsigned index) { return static_cast<uint64_t>(seed) + index + 1; }
static double patchTestNumber(double seed, unsigned index) { return seed + (index + 1.5); }

static uint64_t sumOfPatchTestIntegers(int64_t seed, unsigned count)
{
    uint64_t sum = 0;
    for (unsigned index = 0; index < count; ++index)
        sum += patchTestInteger(seed, index);
    return sum;
}

static uint64_t sumOfBitsOfPatchTestNumbers(double seed, unsigned count)
{
    uint64_t sum = 0;
    for (unsigned index = 0; index < count; ++index)
        sum += std::bit_cast<uint64_t>(patchTestNumber(seed, index));
    return sum;
}

static Vector<Operation*> definePatchTestValues(Output& out, Operation* seed, unsigned count)
{
    Vector<Operation*> values;
    for (unsigned index = 0; index < count; ++index) {
        Operation* addend = seed->type() == Double ? out.constDouble(index + 1.5) : out.constInt64(index + 1);
        values.append(out.opaque(out.add(seed, addend)));
    }
    return values;
}

static Operation* addUpPatchTestValues(Output& out, Operation* first, const Vector<Operation*>& values)
{
    Operation* sum = first;
    for (Operation* value : values)
        sum = out.add(sum, value);
    return sum;
}

static void emitPoisonInPatchTest(CCallHelpers& jit, RegisterMask registers)
{
    GPRReg source = InvalidGPRReg;
    forEachRegister(registers, [&](Reg reg) {
        if (!reg.isGPR())
            return;
        source = reg.gpr();
        jit.move(CCallHelpers::TrustedImm64(static_cast<int64_t>(poison)), source);
    });
    forEachRegister(registers, [&](Reg reg) {
        if (!reg.isFPR())
            return;
        if (source == InvalidGPRReg)
            jit.moveZeroToDouble(reg.fpr());
        else
            jit.move64ToDouble(source, reg.fpr());
    });
}

static void emitReturnInPatchTest(CCallHelpers& jit, const PatchParameters& params)
{
    if (params.frameIsSetUp())
        emitFrameTeardown(jit, params.frame());
    jit.ret();
}

static void emitJumpsToSuccessorsInPatchTest(CCallHelpers& jit, const PatchParameters& params, GPRReg tested, RegisterMask writtenOnTheWay)
{
    AOT_TEST_CHECK(params.successorLabels().size() == 2);
    MacroAssembler::Label* first = params.successorLabels()[0];
    MacroAssembler::Label* second = params.successorLabels()[1];
    CCallHelpers::Jump toSecond = jit.branchTest64(CCallHelpers::NonZero, tested, CCallHelpers::TrustedImm32(1));
    CCallHelpers::Jump toFirst;
    if (!params.fallsThroughToSuccessor(0))
        toFirst = jit.jump();
    params.addLatePath([=](CCallHelpers& jit) {
        toSecond.link(&jit);
        emitPoisonInPatchTest(jit, writtenOnTheWay);
        jit.jump().linkTo(*second, &jit);
        if (toFirst.isSet())
            toFirst.linkTo(*first, &jit);
    });
}

static uint32_t indexOfExecuteStepInPatchTest(const Allocation& allocation, const Operation* operation)
{
    std::span<const Step> steps = allocation.steps();
    uint32_t index = 0;
    while (index < steps.size() && (steps[index].kind != StepKind::Execute || steps[index].operation != operation))
        ++index;
    AOT_TEST_CHECK(index < steps.size());
    return index;
}

static PatchOperation* appendPatchTestIncrement(Output& out, Operation* operand, int32_t addend, PatchTestObservation& seen)
{
    PatchOperation* patch = out.patch(Int64);
    patch->appendSomeRegister(operand);
    patch->effects = Effects::none();
    patch->setGenerator([&seen, addend](CCallHelpers& jit, const PatchParameters& params) {
        recordPatchTestObservation(seen, params);
        jit.add64(CCallHelpers::TrustedImm32(addend), params[1].gpr(), params[0].gpr());
    });
    return patch;
}

static PatchOperation* appendPatchTestCallWithoutOperands(TestProcedure& test)
{
    RegisterMask clobbered = callerSavedRegistersOfPatchTest(test);
    PatchOperation* call = test.out().patch(Void);
    call->clobberLate(clobbered);
    call->setGenerator([clobbered](CCallHelpers& jit, const PatchParameters&) {
        emitPoisonInPatchTest(jit, clobbered);
    });
    return call;
}

static void testMachinePatchRegistersOfNumbers()
{
    TestProcedure test;
    Output& out = test.out();
    Operation* x = test.argumentFPR(0);
    Operation* y = test.argumentFPR(1);
    PatchTestObservation seen;

    PatchOperation* patch = out.patch(Double);
    patch->append(x, ValueRep::reg(FPRInfo::argumentFPR1));
    patch->append(y, ValueRep::reg(FPRInfo::argumentFPR0));
    patch->resultConstraints = { ValueRep::reg(FPRInfo::argumentFPR2) };
    patch->effects = Effects::none();
    patch->setGenerator([&seen](CCallHelpers& jit, const PatchParameters& params) {
        recordPatchTestObservation(seen, params);
        jit.moveDouble(params[1].fpr(), params[0].fpr());
        jit.addDouble(params[1].fpr(), params[0].fpr());
        jit.addDouble(params[2].fpr(), params[0].fpr());
    });
    out.ret(patch);

    compileOrFail(test);
    AOT_TEST_CHECK(seen.runs == 1 && seen.reps.size() == 3);
    AOT_TEST_CHECK(seen.reps[0] == ValueRep::reg(FPRInfo::argumentFPR2));
    AOT_TEST_CHECK(seen.reps[1] == ValueRep::reg(FPRInfo::argumentFPR1));
    AOT_TEST_CHECK(seen.reps[2] == ValueRep::reg(FPRInfo::argumentFPR0));
    AOT_TEST_CHECK(test.call<double>(10.5, 0.25) == 21.25);
}

static void runPatchTestOfOperandsAnywhere(ValueRep::Kind kind, bool isUnderPressure)
{
    constexpr int64_t a = (int64_t { 5 } << 32) + 1000;
    constexpr double x = 0.25;
    bool isLate = kind == ValueRep::LateColdAny;

    TestProcedure test;
    Output& out = test.out();
    Operation* integerArgument = test.argumentGPR(0);
    Operation* numberArgument = test.argumentFPR(0);
    unsigned numberOfIntegers = isUnderPressure ? numberOfRegistersInPatchTest(test, Bank::GP) + 4 : 3;
    unsigned numberOfNumbers = isUnderPressure ? numberOfRegistersInPatchTest(test, Bank::FP) + 4 : 3;
    Vector<Operation*> integers = definePatchTestValues(out, integerArgument, numberOfIntegers);
    Vector<Operation*> numbers = definePatchTestValues(out, numberArgument, numberOfNumbers);
    Operation* narrow = out.opaque(out.castToInt32(integerArgument));
    Operation* integerConstant = out.constInt64(77);
    Operation* numberConstant = out.constDouble(2.5);
    RegisterMask clobbered = isLate ? firstArgumentRegistersForPatchTest() : 0;
    PatchTestObservation seen;

    PatchOperation* patch = out.patch(Int64);
    for (Operation* value : integers)
        patch->append(value, kind);
    for (Operation* value : numbers)
        patch->append(value, kind);
    patch->append(integerConstant, kind);
    patch->append(numberConstant, kind);
    patch->append(narrow, kind);
    patch->clobberLate(clobbered);
    patch->numGPScratchRegisters = 2;
    patch->effects = Effects::none();
    patch->setGenerator([&seen, clobbered, isLate](CCallHelpers& jit, const PatchParameters& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        recordPatchTestObservation(seen, params);
        GPRReg sum = params.gpScratch(0);
        GPRReg word = params.gpScratch(1);
        if (isLate)
            emitPoisonInPatchTest(jit, clobbered | maskOf(params[0].reg()));
        jit.move(CCallHelpers::TrustedImm32(0), sum);
        for (unsigned index = 1; index < params.size(); ++index) {
            params[index].emitRestore(jit, word);
            if (index + 1 == params.size())
                jit.zeroExtend32ToWord(word, word);
            jit.add64(word, sum);
        }
        jit.move(sum, params[0].gpr());
    });
    out.ret(addUpPatchTestValues(out, patch, integers));

    compileOrFail(test);
    AOT_TEST_CHECK(seen.runs == 1 && seen.reps.size() == 1 + numberOfIntegers + numberOfNumbers + 3);
    std::span<const ValueRep> operands = seen.reps.span().subspan(1);
    unsigned numberInFrame = 0;
    for (const ValueRep& rep : operands) {
        AOT_TEST_CHECK(rep.isReg() || rep.isStack() || rep.isConstant());
        numberInFrame += rep.isStack();
    }
    AOT_TEST_CHECK(!isUnderPressure || numberInFrame >= 8);
    AOT_TEST_CHECK(!(registersAmongPatchTestReps(operands) & seen.scratch));
    if (isLate)
        AOT_TEST_CHECK(!(registersAmongPatchTestReps(operands) & (clobbered | maskOf(seen.reps[0].reg()))));

    const ValueRep& integerRep = operands[numberOfIntegers + numberOfNumbers];
    const ValueRep& numberRep = operands[numberOfIntegers + numberOfNumbers + 1];
    AOT_TEST_CHECK(kind == ValueRep::WarmAny || (integerRep.isConstant() && numberRep.isConstant()));
    AOT_TEST_CHECK(!integerRep.isConstant() || integerRep.value() == 77);
    AOT_TEST_CHECK(!numberRep.isConstant() || numberRep.value() == std::bit_cast<int64_t>(2.5));

    if (kind == ValueRep::ColdAny) {
        const Allocation& allocation = test.procedure().allocation();
        uint32_t end = indexOfExecuteStepInPatchTest(allocation, patch);
        for (uint32_t index = indexOfExecuteStepInPatchTest(allocation, narrow) + 1; index < end; ++index) {
            StepKind step = allocation.step(index).kind;
            AOT_TEST_CHECK(step != StepKind::Move && step != StepKind::Reload && step != StepKind::Materialize);
        }
    }

    uint64_t expected = 2 * sumOfPatchTestIntegers(a, numberOfIntegers) + sumOfBitsOfPatchTestNumbers(x, numberOfNumbers);
    expected += 77 + std::bit_cast<uint64_t>(2.5) + static_cast<uint32_t>(a);
    AOT_TEST_CHECK(test.call<uint64_t>(a, x) == expected);
}

static void testMachinePatchLateColdAnyUnderPressure() { runPatchTestOfOperandsAnywhere(ValueRep::LateColdAny, true); }

static void checkMachinePatchTupleWithMoreMembersThanRegisters(bool endsBlock)
{
    constexpr int64_t a = 1000;
    constexpr unsigned numberInRegisters = 4;
    ProcedureTraits traits;
    traits.hasHandlers = endsBlock;
    TestProcedure test(traits);
    Output& out = test.out();
    Procedure& proc = test.procedure();
    Operation* argument = test.argumentGPR(0);
    const unsigned numberInWords = numberOfRegistersInPatchTest(test, Bank::GP);
    const unsigned count = numberInRegisters + numberInWords;
    proc.requestCallArgAreaSizeInBytes(numberInWords * sizeof(uint64_t));
    unsigned runs = 0;

    Vector<Type> types;
    Vector<ValueRep> constraints;
    for (unsigned index = 0; index < count; ++index) {
        types.append(Int64);
        if (index < numberInRegisters)
            constraints.append(ValueRep::reg(GPRInfo::toArgumentRegister(index)));
        else
            constraints.append(ValueRep::stackArgument(static_cast<int32_t>((index - numberInRegisters) * sizeof(uint64_t))));
    }
    PatchOperation* patch = out.patch(proc.addTuple(types.span()));
    patch->appendSomeRegister(argument);
    patch->resultConstraints = constraints;
    patch->numGPScratchRegisters = 1;
    if (!endsBlock)
        patch->effects = Effects::none();
    patch->setGenerator([&runs, count, endsBlock](CCallHelpers& jit, const PatchParameters& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        ++runs;
        AOT_TEST_CHECK(params.size() == count + 1);
        GPRReg next = params.gpScratch(0);
        jit.move(params[count].gpr(), next);
        for (unsigned index = 0; index < count; ++index) {
            if (params[index].isGPR())
                jit.move(next, params[index].gpr());
            else
                jit.store64(next, CCallHelpers::Address(CCallHelpers::stackPointerRegister, params[index].offsetFromSP()));
            jit.add64(CCallHelpers::TrustedImm32(1), next);
        }
        if (endsBlock) {
            jit.sub64(CCallHelpers::TrustedImm32(static_cast<int32_t>(count)), next);
            emitJumpsToSuccessorsInPatchTest(jit, params, next, 0);
        }
    });
    if (endsBlock) {
        Block* continuation = out.newBlock();
        Block* landingPad = out.newBlock();
        out.appendSuccessor(normalEdge(continuation));
        out.appendSuccessor(rareEdge(landingPad));
        patch->effects.terminal = true;
        out.appendTo(landingPad);
        out.ret(argument);
        out.appendTo(continuation);
    }
    Vector<Operation*> members;
    for (unsigned index = 0; index < count; ++index)
        members.append(out.extract(patch, index));
    Operation* sum = argument;
    int64_t expected = a;
    for (unsigned index = 0; index < count; ++index) {
        sum = out.add(sum, out.mul(members[index], static_cast<int32_t>(index + 2)));
        expected += (a + index) * (index + 2);
    }
    out.ret(sum);

    compileOrFail(test);
    AOT_TEST_CHECK(runs == 1);
    AOT_TEST_CHECK(test.call<int64_t>(a) == expected);
    if (endsBlock)
        AOT_TEST_CHECK(test.call<int64_t>(a + 1) == a + 1);
}

static void testMachinePatchThatEndsBlockWithMoreMembersThanRegisters()
{
    checkMachinePatchTupleWithMoreMembersThanRegisters(true);
}

static void testMachinePatchClobbersOnSuccessor()
{
    constexpr int64_t a = 600;
    constexpr unsigned count = 6;
    ProcedureTraits traits;
    traits.hasHandlers = true;
    TestProcedure test(traits);
    Output& out = test.out();
    Operation* seed = test.argumentGPR(0);
    Operation* tested = test.argumentGPR(1);
    Vector<Operation*> values = definePatchTestValues(out, seed, count);
    Operation* thousand = out.constInt64(1000);
    Block* continuation = out.newBlock();
    Block* landingPad = out.newBlock();
    RegisterMask writtenOnTheWay = callerSavedRegistersOfPatchTest(test);
    PatchTestObservation seen;

    PatchOperation* patch = out.patch(Int64);
    patch->appendSomeRegister(tested);
    patch->resultConstraints = { ValueRep::SomeEarlyRegister };
    patch->setGenerator([&seen, writtenOnTheWay](CCallHelpers& jit, const PatchParameters& params) {
        recordPatchTestObservation(seen, params);
        jit.add64(CCallHelpers::TrustedImm32(3), params[1].gpr(), params[0].gpr());
        emitJumpsToSuccessorsInPatchTest(jit, params, params[1].gpr(), writtenOnTheWay);
    });
    out.appendSuccessor(normalEdge(continuation));
    out.appendSuccessor(rareEdge(landingPad));
    patch->clobberLateOnSuccessor(1, registerSetOf(writtenOnTheWay));
    patch->effects.terminal = true;
    AOT_TEST_CHECK(landingPad->isLandingPad() && !continuation->isLandingPad());
    AOT_TEST_CHECK(patch->lateClobberedOnSuccessor(1) == writtenOnTheWay && !patch->lateClobberedOnSuccessor(0));

    out.appendTo(continuation);
    appendPatchTestCallWithoutOperands(test);
    out.ret(addUpPatchTestValues(out, patch, values));
    out.appendTo(landingPad);
    out.ret(addUpPatchTestValues(out, thousand, values));

    compileOrFail(test);
    const Allocation& allocation = test.procedure().allocation();
    AOT_TEST_CHECK(seen.runs == 1 && seen.frameIsSetUp);
    AOT_TEST_CHECK(allocation.registersAtHeadOf(landingPad).empty());
    for (Operation* value : values)
        AOT_TEST_CHECK(allocation.isSpilled(value));
    AOT_TEST_CHECK(test.call<uint64_t>(a, int64_t { 20 }) == 23 + sumOfPatchTestIntegers(a, count));
    AOT_TEST_CHECK(test.call<uint64_t>(a, int64_t { 21 }) == 1000 + sumOfPatchTestIntegers(a, count));
}

static void testMachinePatchSuccessorLabelsAndFallThrough()
{
    TestProcedure test;
    Output& out = test.out();
    Operation* a = test.argumentGPR(0);
    Operation* two = out.constInt64(2);
    Operation* one = out.constInt64(1);
    Block* whenEven = out.newBlock();
    Block* whenOdd = out.newBlock();
    std::array<bool, 2> fallsThrough { false, false };

    PatchOperation* patch = out.patch(Void);
    patch->appendSomeRegister(a);
    patch->effects = Effects::none();
    patch->effects.terminal = true;
    patch->setGenerator([&fallsThrough](CCallHelpers& jit, const PatchParameters& params) {
        AOT_TEST_CHECK(params.size() == 1 && params[0].isGPR());
        fallsThrough[0] = params.fallsThroughToSuccessor(0);
        fallsThrough[1] = params.fallsThroughToSuccessor(1);
        emitJumpsToSuccessorsInPatchTest(jit, params, params[0].gpr(), 0);
    });
    out.appendSuccessor(normalEdge(whenEven));
    out.appendSuccessor(unlikelyEdge(whenOdd));
    out.appendTo(whenEven);
    out.ret(two);
    out.appendTo(whenOdd);
    out.ret(one);

    compileOrFail(test);
    const Order& order = test.procedure().order();
    std::span<Block* const> layout = order.blocksInLayout();
    uint32_t next = order.of(patch->owner()).layoutIndex + 1;
    AOT_TEST_CHECK(next < layout.size());
    for (unsigned index = 0; index < 2; ++index)
        AOT_TEST_CHECK(fallsThrough[index] == (patch->owner()->successor(index).block() == layout[next]));
    AOT_TEST_CHECK(fallsThrough[0] && !fallsThrough[1]);
    AOT_TEST_CHECK(test.call<int64_t>(int64_t { 10 }) == 2);
    AOT_TEST_CHECK(test.call<int64_t>(int64_t { 11 }) == 1);
}

static void testMachinePatchEntersJoinThroughBlocksWithoutCode()
{
    TestProcedure test;
    Output& out = test.out();
    Operation* a = test.argumentGPR(0);
    Block* arm = out.newBlock();
    Block* join = out.newBlock();
    MacroAssembler::Label* enteredFromFork = nullptr;
    MacroAssembler::Label* enteredFromArm = nullptr;

    PatchOperation* fork = out.patch(Void);
    fork->appendSomeRegister(a);
    fork->effects = Effects::none();
    fork->effects.terminal = true;
    fork->setGenerator([&enteredFromFork](CCallHelpers& jit, const PatchParameters& params) {
        enteredFromFork = params.successorLabels()[1];
        emitJumpsToSuccessorsInPatchTest(jit, params, params[0].gpr(), 0);
    });
    out.appendSuccessor(normalEdge(arm));
    out.appendSuccessor(normalEdge(join));

    out.appendTo(arm);
    PatchOperation* hop = out.patch(Void);
    hop->effects = Effects::none();
    hop->effects.terminal = true;
    hop->setGenerator([&enteredFromArm](CCallHelpers& jit, const PatchParameters& params) {
        enteredFromArm = params.successorLabels()[0];
        if (params.fallsThroughToSuccessor(0))
            return;
        CCallHelpers::Jump toSuccessor = jit.jump();
        MacroAssembler::Label* successor = params.successorLabels()[0];
        params.addLatePath([=](CCallHelpers& jit) {
            toSuccessor.linkTo(*successor, &jit);
        });
    });
    out.appendSuccessor(normalEdge(join));

    out.appendTo(join);
    out.ret(a);

    compileOrFail(test);
    Block* behindFork = fork->owner()->successor(1).block();
    Block* behindArm = hop->owner()->successor(0).block();
    AOT_TEST_CHECK(behindFork != join && behindArm != join && behindFork != behindArm);
    AOT_TEST_CHECK(behindFork->successor(0).block() == join && behindArm->successor(0).block() == join);
    AOT_TEST_CHECK(enteredFromFork && enteredFromFork == enteredFromArm);
    AOT_TEST_CHECK(test.call<int64_t>(int64_t { 10 }) == 10);
    AOT_TEST_CHECK(test.call<int64_t>(int64_t { 11 }) == 11);
}

static void testMachinePatchRareBlocksOfSameMovesStandOnce()
{
    TestProcedure test;
    Output& out = test.out();
    Operation* a = test.argumentGPR(0);
    Operation* b = test.argumentGPR(1);
    Block* next = out.newBlock();
    Block* usual = out.newBlock();
    Block* done = out.newBlock();

    Operation* early = out.opaque(out.add(a, out.constInt64(7)));
    ValueFromBlock fromEntry = out.anchor(early);
    out.branch(out.testNonZero64(a, 1), rareEdge(done), normalEdge(next));
    out.appendTo(next);
    ValueFromBlock fromNext = out.anchor(early);
    out.branch(out.testNonZero64(b, 1), rareEdge(done), normalEdge(usual));
    out.appendTo(usual);
    ValueFromBlock fromUsual = out.anchor(out.opaque(out.add(early, out.constInt64(100))));
    out.jump(done);
    out.appendTo(done);
    out.ret(out.add(out.phi(Int64, fromEntry, fromNext, fromUsual), out.add(early, early)));

    compileOrFail(test);
    const Allocation& allocation = test.procedure().allocation();
    Block* behindEntry = early->owner()->successor(0).block();
    Block* behindNext = next->successor(0).block();
    AOT_TEST_CHECK(behindEntry != done && behindNext != done && behindEntry != behindNext);
    AOT_TEST_CHECK(behindEntry->successor(0).block() == done && behindNext->successor(0).block() == done);
    for (Block* block : { behindEntry, behindNext })
        AOT_TEST_CHECK(allocation.stepsOf(block).beginOfEdgeMoves < allocation.stepsOf(block).endOfEdgeMoves);
    AOT_TEST_CHECK(allocation.numberOfOperationsAllocated() - test.procedure().numberOfOperationsEncoded() == behindNext->size());
    AOT_TEST_CHECK(test.call<int64_t>(int64_t { 10 }, int64_t { 20 }) == 117 + 34);
    AOT_TEST_CHECK(test.call<int64_t>(int64_t { 11 }, int64_t { 20 }) == 18 + 36);
    AOT_TEST_CHECK(test.call<int64_t>(int64_t { 10 }, int64_t { 21 }) == 17 + 34);
}

static void testMachinePatchRareBlocksThatMakeOtherConstantsStandApart()
{
    TestProcedure test;
    Output& out = test.out();
    Operation* a = test.argumentGPR(0);
    Operation* b = test.argumentGPR(1);
    Block* second = out.newBlock();
    Block* third = out.newBlock();
    Block* usual = out.newBlock();
    Block* done = out.newBlock();

    Operation* early = out.opaque(out.add(a, out.constInt64(7)));
    ValueFromBlock fromFirst = out.anchor(out.constInt64(0x1000000001));
    out.branch(out.testNonZero64(a, 1), rareEdge(done), normalEdge(second));
    out.appendTo(second);
    ValueFromBlock fromSecond = out.anchor(out.constInt64(0x2000000002));
    out.branch(out.testNonZero64(b, 1), rareEdge(done), normalEdge(third));
    out.appendTo(third);
    ValueFromBlock fromThird = out.anchor(out.constInt64(0x2000000002));
    out.branch(out.testNonZero64(b, 2), rareEdge(done), normalEdge(usual));
    out.appendTo(usual);
    ValueFromBlock fromUsual = out.anchor(out.opaque(out.add(early, out.constInt64(100))));
    out.jump(done);
    out.appendTo(done);
    out.ret(out.phi(Int64, fromFirst, fromSecond, fromThird, fromUsual));

    compileOrFail(test);
    const Allocation& allocation = test.procedure().allocation();
    Block* behindThird = third->successor(0).block();
    for (Block* block : { early->owner()->successor(0).block(), second->successor(0).block(), behindThird }) {
        AOT_TEST_CHECK(block != done && block->successor(0).block() == done);
        const BlockSteps& steps = allocation.stepsOf(block);
        AOT_TEST_CHECK(steps.endOfEdgeMoves == steps.beginOfEdgeMoves + 1 && allocation.step(steps.beginOfEdgeMoves).kind == StepKind::Materialize);
    }
    AOT_TEST_CHECK(allocation.numberOfOperationsAllocated() - test.procedure().numberOfOperationsEncoded() == behindThird->size());
    AOT_TEST_CHECK(test.call<int64_t>(int64_t { 10 }, int64_t { 4 }) == 117);
    AOT_TEST_CHECK(test.call<int64_t>(int64_t { 11 }, int64_t { 4 }) == 0x1000000001);
    AOT_TEST_CHECK(test.call<int64_t>(int64_t { 10 }, int64_t { 5 }) == 0x2000000002);
    AOT_TEST_CHECK(test.call<int64_t>(int64_t { 10 }, int64_t { 6 }) == 0x2000000002);
}

static void testMachinePatchCheckHandsOverOperands()
{
    constexpr double x = 6.5;
    TestProcedure test;
    Output& out = test.out();
    Operation* a = test.argumentGPR(0);
    Operation* b = test.argumentGPR(1);
    Operation* number = test.argumentFPR(0);
    Operation* sum = out.opaque(out.add(a, b));
    Operation* constant = out.constInt64(13);
    PatchTestObservation seen;

    PatchOperation* check = out.check(out.lessThan(a, b));
    check->append(sum, ValueRep::ColdAny);
    check->append(number, ValueRep::ColdAny);
    check->append(constant, ValueRep::ColdAny);
    check->setGenerator([&seen](CCallHelpers& jit, const PatchParameters& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        recordPatchTestObservation(seen, params);
        AOT_TEST_CHECK(params.size() == 3 && !params[1].isGPR() && params[2] == ValueRep::constant(13));
        params[0].emitRestore(jit, GPRInfo::returnValueGPR);
        params[1].emitRestore(jit, GPRInfo::argumentGPR1);
        jit.add64(GPRInfo::argumentGPR1, GPRInfo::returnValueGPR);
        jit.add64(CCallHelpers::TrustedImm32(static_cast<int32_t>(params[2].value())), GPRInfo::returnValueGPR);
        emitReturnInPatchTest(jit, params);
    });
    out.ret(sum);

    compileOrFail(test);
    AOT_TEST_CHECK(seen.runs == 1);
    AOT_TEST_CHECK(test.call<uint64_t>(int64_t { 9 }, int64_t { 4 }, x) == 13);
    AOT_TEST_CHECK(test.call<uint64_t>(int64_t { 4 }, int64_t { 9 }, x) == 13 + std::bit_cast<uint64_t>(x) + 13);
}

static unsigned countLatePathsOfPatchTestChecks(bool sharesLatePath, std::span<const int64_t> constants)
{
    TestProcedure test;
    Output& out = test.out();
    Operation* a = test.argumentGPR(0);
    unsigned numberOfPaths = 0;
    PatchGenerator* generator = test.procedure().createGenerator([&numberOfPaths](CCallHelpers& jit, const PatchParameters& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        ++numberOfPaths;
        AOT_TEST_CHECK(params.size() == 1 && params[0].isConstant());
        params[0].emitRestore(jit, GPRInfo::returnValueGPR);
        emitReturnInPatchTest(jit, params);
    });
    for (unsigned index = 0; index < constants.size(); ++index) {
        Operation* constant = out.constInt64(constants[index]);
        PatchOperation* check = out.check(out.equal(a, index));
        check->append(constant, ValueRep::ColdAny);
        check->setGenerator(generator);
        check->setSharesLatePath(sharesLatePath);
    }
    out.ret(out.constInt64(99));

    compileOrFail(test);
    for (unsigned index = 0; index < constants.size(); ++index)
        AOT_TEST_CHECK(test.call<int64_t>(static_cast<int64_t>(index)) == constants[index]);
    AOT_TEST_CHECK(test.call<int64_t>(static_cast<int64_t>(constants.size())) == 99);
    return numberOfPaths;
}

static void testMachinePatchChecksShareLatePaths()
{
    static constexpr int64_t alike[] = { 7, 7, 7 };
    static constexpr int64_t mixed[] = { 7, 8, 7 };
    AOT_TEST_CHECK(countLatePathsOfPatchTestChecks(true, alike) == 1);
    AOT_TEST_CHECK(countLatePathsOfPatchTestChecks(false, alike) == 3);
    AOT_TEST_CHECK(countLatePathsOfPatchTestChecks(true, mixed) == 2);
}

static void checkFrameOfPatchTestWithOnePatch(bool runsWithoutFrame, bool followsFrameSetup, FrameCoverage expected)
{
    TestProcedure test;
    Output& out = test.out();
    PatchTestObservation seen;
    PatchOperation* patch = appendPatchTestIncrement(out, test.argumentGPR(0), 1, seen);
    patch->setRunsWithoutFrame(runsWithoutFrame);
    patch->setFollowsFrameSetup(followsFrameSetup);
    out.ret(patch);

    compileOrFail(test);
    AOT_TEST_CHECK(test.procedure().frame().coverage() == expected);
    AOT_TEST_CHECK(seen.runs == 1 && seen.frameIsSetUp == (expected == FrameCoverage::WholeFunction));
    AOT_TEST_CHECK(test.call<int64_t>(int64_t { 41 }) == 42);
}

static void testMachinePatchFrameIsSetUp()
{
    checkFrameOfPatchTestWithOnePatch(true, false, FrameCoverage::None);
    checkFrameOfPatchTestWithOnePatch(false, false, FrameCoverage::WholeFunction);
    checkFrameOfPatchTestWithOnePatch(true, true, FrameCoverage::WholeFunction);
}

static void testMachinePatchFrameIsSetUpInRareRegion()
{
    TestProcedure test;
    Output& out = test.out();
    Operation* a = test.argumentGPR(0);
    Block* slow = out.newBlock();
    Block* done = out.newBlock();
    PatchTestObservation seenOutside;
    PatchTestObservation seenInside;

    PatchOperation* quick = appendPatchTestIncrement(out, a, 1, seenOutside);
    quick->setRunsWithoutFrame(true);
    ValueFromBlock fromEntry = out.anchor(quick);
    out.branch(out.testNonZero64(a, 1), rareEdge(slow), normalEdge(done));
    out.appendTo(slow);
    ValueFromBlock fromSlow = out.anchor(appendPatchTestIncrement(out, a, 100, seenInside));
    out.jump(done);
    out.appendTo(done);
    out.ret(out.phi(Int64, fromEntry, fromSlow));

    compileOrFail(test);
    AOT_TEST_CHECK(test.procedure().frame().coverage() == FrameCoverage::RareRegions);
    AOT_TEST_CHECK(seenOutside.runs == 1 && !seenOutside.frameIsSetUp);
    AOT_TEST_CHECK(seenInside.runs == 1 && seenInside.frameIsSetUp);
    AOT_TEST_CHECK(test.call<int64_t>(int64_t { 2 }) == 3);
    AOT_TEST_CHECK(test.call<int64_t>(int64_t { 3 }) == 103);
}

static void testMachinePatchThatWantsTooManyRegisters()
{
    TestProcedure test;
    Output& out = test.out();
    Vector<Operation*> values = definePatchTestValues(out, test.argumentGPR(0), numberOfRegistersInPatchTest(test, Bank::GP) + 1);

    PatchOperation* patch = out.patch(Void);
    for (Operation* value : values)
        patch->appendSomeRegister(value);
    patch->setGenerator([](CCallHelpers&, const PatchParameters&) { });
    out.retVoid();

    AOT_TEST_CHECK(!test.prepare());
    AOT_TEST_CHECK(test.procedure().failure() && test.procedure().failure()->kind == FailureKind::OutOfRegisters);
}

static bool fitsQuestionOfEntryInPatchTest(const Typed::QuestionOfEntry& question, uint64_t value)
{
    TestProcedure test;
    Output& out = test.out();
    Operation* asked = test.argumentGPR(0);
    PatchOperation* patch = out.patch(Int64);
    patch->append(asked, ValueRep::reg(question.value));
    RegisterSet clobbered = RegisterSet::macroClobberedGPRs();
    clobbered.add(valueEntryGPR, IgnoreVectors);
    patch->clobber(clobbered);
    patch->setGenerator([&question](CCallHelpers& jit, const PatchParameters& params) {
        CCallHelpers::JumpList misfits = Typed::emitQuestionOfEntryWrittenByHand(jit, question);
        jit.move(CCallHelpers::TrustedImm32(1), params[0].gpr());
        CCallHelpers::Jump done = jit.jump();
        misfits.link(&jit);
        jit.move(CCallHelpers::TrustedImm32(0), params[0].gpr());
        done.link(&jit);
    });
    out.ret(patch);
    compileOrFail(test);
    return test.call<int64_t>(value);
}

struct alignas(16) CellOfPatchTest {
    std::array<uint32_t, 4> words { };

    explicit CellOfPatchTest(uint32_t shapeWord)
    {
        static_assert(offsetOfShapeWord == 3 * sizeof(uint32_t));
        words[3] = shapeWord;
    }
    uint64_t bits() const { return reinterpret_cast<uintptr_t>(this); }
};

static void testMachinePatchQuestionOfEntryAsksForOneShape()
{
    for (uint32_t shape : { 1u, 4095u, 4096u, 4097u, 70001u, maxNumberOfShapes - 2 }) {
        for (bool mayBearMark : { false, true }) {
            Typed::QuestionOfEntry question { .value = thisGPR, .passingPrimitives = Typed::Primitive::Undefined, .shapeMayBearMark = mayBearMark, .shapeWord = shapeWordOf(shape) };
            AOT_TEST_CHECK(fitsQuestionOfEntryInPatchTest(question, CellOfPatchTest(shapeWordOf(shape)).bits()));
            AOT_TEST_CHECK(fitsQuestionOfEntryInPatchTest(question, CellOfPatchTest(shapeWordOf(shape) & ~shapeWordIsUnmarked).bits()) == mayBearMark);
            AOT_TEST_CHECK(!fitsQuestionOfEntryInPatchTest(question, CellOfPatchTest(shapeWordOf(shape + 1)).bits()));
            AOT_TEST_CHECK(!fitsQuestionOfEntryInPatchTest(question, CellOfPatchTest(shapeWordOf(shape + 1) & ~shapeWordIsUnmarked).bits()));
            AOT_TEST_CHECK(!fitsQuestionOfEntryInPatchTest(question, CellOfPatchTest(0).bits()));
            AOT_TEST_CHECK(fitsQuestionOfEntryInPatchTest(question, JSValue::encode(jsUndefined())));
            AOT_TEST_CHECK(!fitsQuestionOfEntryInPatchTest(question, JSValue::encode(jsNull())));
            AOT_TEST_CHECK(!fitsQuestionOfEntryInPatchTest(question, JSValue::encode(jsNumber(7))));
        }
    }
}

static void testMachinePatchQuestionOfEntryAsksForWords()
{
    using namespace Typed;
    struct Word {
        uint16_t primitive;
        JSValue value;
    };
    const Word words[] = {
        { Primitive::Undefined, jsUndefined() }, { Primitive::Null, jsNull() }, { Primitive::False, jsBoolean(false) }, { Primitive::True, jsBoolean(true) },
        { Primitive::Number, jsNumber(0) }, { Primitive::Number, jsNumber(-1) }, { Primitive::Number, jsNumber(2147483647) }, { Primitive::Number, jsNumber(2147483648.0) },
        { Primitive::Number, jsNumber(-0.0) }, { Primitive::Number, jsNumber(0.5) }, { Primitive::Number, jsNumber(9007199254740992.0) }, { Primitive::Number, jsNaN() },
    };
    for (uint16_t passing : { uint16_t { 0 }, Primitive::Undefined, Primitive::Null, Primitive::UndefinedOrNull, Primitive::False, Primitive::True, Primitive::Boolean, Primitive::Number,
        static_cast<uint16_t>(Primitive::Number | Primitive::Undefined), static_cast<uint16_t>(Primitive::Boolean | Primitive::UndefinedOrNull), static_cast<uint16_t>(Primitive::Number | Primitive::Boolean | Primitive::Null) }) {
        for (bool asksForShapeToo : { false, true }) {
            QuestionOfEntry question { .value = argumentGPR(1), .passingPrimitives = passing };
            if (asksForShapeToo)
                question.shapeWord = shapeWordOf(4097);
            for (const Word& word : words)
                AOT_TEST_CHECK(fitsQuestionOfEntryInPatchTest(question, JSValue::encode(word.value)) == !!(passing & word.primitive));
            AOT_TEST_CHECK(fitsQuestionOfEntryInPatchTest(question, CellOfPatchTest(shapeWordOf(4097)).bits()) == asksForShapeToo);
            AOT_TEST_CHECK(!fitsQuestionOfEntryInPatchTest(question, CellOfPatchTest(shapeWordOf(4098)).bits()));
        }
    }
}

static std::span<const NativeTest> listOfMachinePatchTests()
{
    static constexpr NativeTest tests[] = {
        { "machine/patch/registers-of-numbers"_s, testMachinePatchRegistersOfNumbers },
        { "machine/patch/late-cold-any-under-pressure"_s, testMachinePatchLateColdAnyUnderPressure },
        { "machine/patch/patch-that-ends-block-with-more-members-than-registers"_s, testMachinePatchThatEndsBlockWithMoreMembersThanRegisters },
        { "machine/patch/clobbers-on-a-successor"_s, testMachinePatchClobbersOnSuccessor },
        { "machine/patch/successor-labels-and-fall-through"_s, testMachinePatchSuccessorLabelsAndFallThrough },
        { "machine/patch/enters-join-through-blocks-without-code"_s, testMachinePatchEntersJoinThroughBlocksWithoutCode },
        { "machine/patch/rare-blocks-of-the-same-moves-stand-once"_s, testMachinePatchRareBlocksOfSameMovesStandOnce },
        { "machine/patch/rare-blocks-that-make-other-constants-stand-apart"_s, testMachinePatchRareBlocksThatMakeOtherConstantsStandApart },
        { "machine/patch/check-hands-over-operands"_s, testMachinePatchCheckHandsOverOperands },
        { "machine/patch/checks-share-late-paths"_s, testMachinePatchChecksShareLatePaths },
        { "machine/patch/frame-is-set-up"_s, testMachinePatchFrameIsSetUp },
        { "machine/patch/frame-is-set-up-in-rare-region"_s, testMachinePatchFrameIsSetUpInRareRegion },
        { "machine/patch/wants-too-many-registers"_s, testMachinePatchThatWantsTooManyRegisters },
        { "machine/patch/question-of-an-entry-written-by-hand-asks-for-one-shape"_s, testMachinePatchQuestionOfEntryAsksForOneShape },
        { "machine/patch/question-of-an-entry-written-by-hand-asks-for-words"_s, testMachinePatchQuestionOfEntryAsksForWords },
    };
    return tests;
}

} // namespace Machine

std::span<const NativeTest> machinePatchTests()
{
    return Machine::listOfMachinePatchTests();
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
