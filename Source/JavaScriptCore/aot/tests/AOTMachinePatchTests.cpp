/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTMachineTestSupport.h"
#include "AllowMacroScratchRegisterUsage.h"
#include <array>
#include <bit>

// No request goes to the builder between patch() or check() and the last word of the setup: the next request seals the Patch.

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

static double sumOfPatchTestNumbers(double first, double seed, unsigned count)
{
    double sum = first;
    for (unsigned index = 0; index < count; ++index)
        sum += patchTestNumber(seed, index);
    return sum;
}

static uint64_t sumOfBitsOfPatchTestNumbers(double seed, unsigned count)
{
    uint64_t sum = 0;
    for (unsigned index = 0; index < count; ++index)
        sum += std::bit_cast<uint64_t>(patchTestNumber(seed, index));
    return sum;
}

// patchTestInteger() or patchTestNumber() of the seed. Nothing can compute one again, so each needs a place of its own while it is live.
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

// The two operands lie in different registers. The destination may be either.
static void emitDifferenceInPatchTest(CCallHelpers& jit, GPRReg left, GPRReg right, GPRReg destination)
{
    if (destination == right) {
        jit.neg64(destination);
        jit.add64(left, destination);
        return;
    }
    jit.move(left, destination);
    jit.sub64(right, destination);
}

static void emitReturnInPatchTest(CCallHelpers& jit, const PatchParameters& params)
{
    if (params.frameIsSetUp())
        emitFrameTeardown(jit, params.frame());
    jit.ret();
}

// To the second successor where the low bit of the register is set, writing the registers on the way. To the first otherwise.
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

static unsigned numberOfStepsInPatchTest(const Allocation& allocation, StepKind kind)
{
    unsigned result = 0;
    for (const Step& step : allocation.steps())
        result += step.kind == kind;
    return result;
}

// The operand plus the addend.
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

// Writes every register that no callee saves.
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

static void testMachinePatchRegisterOperands()
{
    TestProcedure test;
    Output& out = test.out();
    Operation* a = test.argumentGPR(0);
    Operation* b = test.argumentGPR(1);
    PatchTestObservation seen;

    // Each is wanted where the other lies.
    PatchOperation* patch = out.patch(Int64);
    patch->append(a, ValueRep::reg(GPRInfo::argumentGPR1));
    patch->append(b, ValueRep::reg(GPRInfo::argumentGPR0));
    patch->effects = Effects::none();
    patch->setGenerator([&seen](CCallHelpers& jit, const PatchParameters& params) {
        recordPatchTestObservation(seen, params);
        emitDifferenceInPatchTest(jit, params[1].gpr(), params[2].gpr(), params[0].gpr());
    });
    out.ret(patch);

    compileOrFail(test);
    AOT_TEST_CHECK(seen.runs == 1 && seen.reps.size() == 3 && seen.reps[0].isGPR());
    AOT_TEST_CHECK(seen.reps[1] == ValueRep::reg(GPRInfo::argumentGPR1));
    AOT_TEST_CHECK(seen.reps[2] == ValueRep::reg(GPRInfo::argumentGPR0));
    AOT_TEST_CHECK(test.call<int64_t>(int64_t { 50 }, int64_t { 8 }) == 42);
}

static void testMachinePatchRegisterOperandsDisplaceLiveValues()
{
    TestProcedure test;
    Output& out = test.out();
    Operation* a = test.argumentGPR(0);
    Operation* b = test.argumentGPR(1);
    Operation* constant = out.constInt64(77);
    PatchTestObservation seen;

    // One value is wanted in two registers, and the first of them holds a value that is read again.
    PatchOperation* patch = out.patch(Int64);
    patch->append(b, ValueRep::reg(GPRInfo::argumentGPR0));
    patch->append(b, ValueRep::reg(GPRInfo::argumentGPR2));
    patch->append(constant, ValueRep::reg(GPRInfo::argumentGPR3));
    patch->resultConstraints = { ValueRep::SomeEarlyRegister };
    patch->effects = Effects::none();
    patch->setGenerator([&seen](CCallHelpers& jit, const PatchParameters& params) {
        recordPatchTestObservation(seen, params);
        jit.add64(params[1].gpr(), params[2].gpr(), params[0].gpr());
        jit.add64(params[3].gpr(), params[0].gpr());
    });
    out.ret(out.add(patch, a));

    compileOrFail(test);
    AOT_TEST_CHECK(seen.runs == 1 && seen.reps.size() == 4);
    AOT_TEST_CHECK(seen.reps[1] == ValueRep::reg(GPRInfo::argumentGPR0));
    AOT_TEST_CHECK(seen.reps[2] == ValueRep::reg(GPRInfo::argumentGPR2));
    AOT_TEST_CHECK(seen.reps[3] == ValueRep::reg(GPRInfo::argumentGPR3));
    AOT_TEST_CHECK(test.call<int64_t>(int64_t { 1000 }, int64_t { 5 }) == 1087);
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

static void testMachinePatchRegisterResultDisplacesLiveValue()
{
    TestProcedure test;
    Output& out = test.out();
    Operation* a = test.argumentGPR(0);
    Operation* b = test.argumentGPR(1);
    PatchTestObservation seen;

    PatchOperation* patch = appendPatchTestIncrement(out, a, 1, seen);
    patch->resultConstraints = { ValueRep::reg(GPRInfo::argumentGPR1) };
    out.ret(out.add(patch, b));

    compileOrFail(test);
    AOT_TEST_CHECK(seen.runs == 1 && seen.reps[0] == ValueRep::reg(GPRInfo::argumentGPR1));
    AOT_TEST_CHECK(test.call<int64_t>(int64_t { 40 }, int64_t { 500 }) == 541);
}

static void testMachinePatchSomeRegisterUnderPressure()
{
    constexpr int64_t a = 9000;
    TestProcedure test;
    Output& out = test.out();
    unsigned count = numberOfRegistersInPatchTest(test, Bank::GP) + 4;
    Vector<Operation*> values = definePatchTestValues(out, test.argumentGPR(0), count);
    PatchTestObservation seen;

    PatchOperation* patch = out.patch(Int64);
    patch->appendSomeRegister(values[0]);
    patch->appendSomeRegister(values[1]);
    patch->appendSomeRegister(values[count - 1]);
    patch->resultConstraints = { ValueRep::SomeEarlyRegister };
    patch->effects = Effects::none();
    patch->setGenerator([&seen](CCallHelpers& jit, const PatchParameters& params) {
        recordPatchTestObservation(seen, params);
        jit.add64(params[1].gpr(), params[2].gpr(), params[0].gpr());
        jit.add64(params[3].gpr(), params[0].gpr());
    });
    out.ret(addUpPatchTestValues(out, patch, values));

    compileOrFail(test);
    AOT_TEST_CHECK(seen.runs == 1 && seen.reps.size() == 4);
    for (const ValueRep& rep : seen.reps)
        AOT_TEST_CHECK(rep.isGPR());
    RegisterMask registers = registersAmongPatchTestReps(seen.reps.span());
    AOT_TEST_CHECK(std::popcount(registers) == 4 && !(registers & ~allocatableRegistersOfPatchTest(test)));
    uint64_t expected = patchTestInteger(a, 0) + patchTestInteger(a, 1) + patchTestInteger(a, count - 1) + sumOfPatchTestIntegers(a, count);
    AOT_TEST_CHECK(test.call<uint64_t>(a) == expected);
}

static void testMachinePatchSomeEarlyRegister()
{
    TestProcedure test;
    Output& out = test.out();
    Operation* a = test.argumentGPR(0);
    Operation* b = test.argumentGPR(1);
    PatchTestObservation seen;

    PatchOperation* patch = out.patch(Int64);
    patch->appendSomeRegister(a);
    patch->appendSomeRegister(b);
    patch->resultConstraints = { ValueRep::SomeEarlyRegister };
    patch->effects = Effects::none();
    patch->setGenerator([&seen](CCallHelpers& jit, const PatchParameters& params) {
        recordPatchTestObservation(seen, params);
        jit.move(CCallHelpers::TrustedImm32(3), params[0].gpr());
        jit.add64(params[1].gpr(), params[0].gpr());
        jit.add64(params[2].gpr(), params[0].gpr());
    });
    out.ret(patch);

    compileOrFail(test);
    AOT_TEST_CHECK(seen.runs == 1 && seen.reps.size() == 3 && seen.reps[0].isGPR());
    AOT_TEST_CHECK(!(maskOf(seen.reps[0].reg()) & registersAmongPatchTestReps(seen.reps.span().subspan(1))));
    AOT_TEST_CHECK(test.call<int64_t>(int64_t { 10 }, int64_t { 200 }) == 213);
}

static void testMachinePatchSomeLateRegister()
{
    TestProcedure test;
    Output& out = test.out();
    Operation* a = test.argumentGPR(0);
    Operation* b = test.argumentGPR(1);
    RegisterMask clobbered = firstArgumentRegistersForPatchTest() | maskOf(Reg(GPRInfo::returnValueGPR));
    PatchTestObservation seen;

    PatchOperation* patch = out.patch(Int64);
    patch->append(a, ValueRep::SomeLateRegister);
    patch->append(b, ValueRep::SomeLateRegister);
    patch->clobberLate(clobbered);
    patch->numGPScratchRegisters = 1;
    patch->effects = Effects::none();
    patch->setGenerator([&seen, clobbered](CCallHelpers& jit, const PatchParameters& params) {
        recordPatchTestObservation(seen, params);
        emitPoisonInPatchTest(jit, clobbered | seen.scratch | maskOf(params[0].reg()));
        emitDifferenceInPatchTest(jit, params[1].gpr(), params[2].gpr(), params[0].gpr());
    });
    out.ret(patch);

    compileOrFail(test);
    AOT_TEST_CHECK(seen.runs == 1 && seen.reps.size() == 3 && seen.reps[0].isGPR() && seen.reps[1].isGPR() && seen.reps[2].isGPR());
    AOT_TEST_CHECK(!(maskOf(seen.reps[0].reg()) & clobbered));
    AOT_TEST_CHECK(!(registersAmongPatchTestReps(seen.reps.span().subspan(1)) & (clobbered | seen.scratch | maskOf(seen.reps[0].reg()))));
    AOT_TEST_CHECK(test.call<int64_t>(int64_t { 50 }, int64_t { 8 }) == 42);
}

// Under pressure every register holds an operand, so the scratch registers and the result are had only by answering some operands with their places in the frame.
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

static void testMachinePatchWarmAny() { runPatchTestOfOperandsAnywhere(ValueRep::WarmAny, false); }
static void testMachinePatchColdAny() { runPatchTestOfOperandsAnywhere(ValueRep::ColdAny, false); }
static void testMachinePatchLateColdAny() { runPatchTestOfOperandsAnywhere(ValueRep::LateColdAny, false); }
static void testMachinePatchWarmAnyUnderPressure() { runPatchTestOfOperandsAnywhere(ValueRep::WarmAny, true); }
static void testMachinePatchColdAnyUnderPressure() { runPatchTestOfOperandsAnywhere(ValueRep::ColdAny, true); }
static void testMachinePatchLateColdAnyUnderPressure() { runPatchTestOfOperandsAnywhere(ValueRep::LateColdAny, true); }

static void testMachinePatchStackArgumentOperands()
{
    constexpr int64_t a = 4000;
    constexpr double x = 6.5;
    TestProcedure test;
    Output& out = test.out();
    Operation* integerArgument = test.argumentGPR(0);
    Operation* numberArgument = test.argumentFPR(0);
    unsigned count = numberOfRegistersInPatchTest(test, Bank::GP) + 2;
    Vector<Operation*> values = definePatchTestValues(out, integerArgument, count);
    Operation* constant = out.constInt64(77);
    test.procedure().requestCallArgAreaSizeInBytes(4 * sizeof(uint64_t));
    PatchTestObservation seen;

    PatchOperation* patch = out.patch(Int64);
    patch->append(integerArgument, ValueRep::stackArgument(0));
    patch->append(numberArgument, ValueRep::stackArgument(8));
    patch->append(constant, ValueRep::stackArgument(16));
    patch->append(values[0], ValueRep::stackArgument(24));
    patch->numGPScratchRegisters = 1;
    patch->effects = Effects::none();
    patch->setGenerator([&seen](CCallHelpers& jit, const PatchParameters& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        recordPatchTestObservation(seen, params);
        AOT_TEST_CHECK(params.size() == 5);
        jit.move(CCallHelpers::TrustedImm32(0), params[0].gpr());
        for (unsigned index = 1; index < params.size(); ++index) {
            AOT_TEST_CHECK(params[index] == ValueRep::stackArgument(static_cast<int32_t>(8 * (index - 1))));
            jit.load64(CCallHelpers::Address(CCallHelpers::stackPointerRegister, params[index].offsetFromSP()), params.gpScratch(0));
            jit.add64(params[0].gpr(), params[0].gpr());
            jit.add64(params.gpScratch(0), params[0].gpr());
        }
    });
    out.ret(addUpPatchTestValues(out, patch, values));

    compileOrFail(test);
    AOT_TEST_CHECK(seen.runs == 1 && seen.frameIsSetUp);
    uint64_t expected = 8 * static_cast<uint64_t>(a) + 4 * std::bit_cast<uint64_t>(x) + 2 * 77 + patchTestInteger(a, 0) + sumOfPatchTestIntegers(a, count);
    AOT_TEST_CHECK(test.call<uint64_t>(a, x) == expected);
}

static void testMachinePatchStackArgumentResults()
{
    TestProcedure test;
    Output& out = test.out();
    Operation* a = test.argumentGPR(0);
    test.procedure().requestCallArgAreaSizeInBytes(2 * sizeof(uint64_t));
    unsigned runs = 0;

    auto appendIncrementInOutgoingWord = [&](int32_t addend) {
        PatchOperation* patch = out.patch(Int64);
        patch->appendSomeRegister(a);
        patch->resultConstraints = { ValueRep::stackArgument(8) };
        patch->numGPScratchRegisters = 1;
        patch->effects = Effects::none();
        patch->setGenerator([&runs, addend](CCallHelpers& jit, const PatchParameters& params) {
            AllowMacroScratchRegisterUsage allowScratch(jit);
            ++runs;
            AOT_TEST_CHECK(params.size() == 2 && params[0] == ValueRep::stackArgument(8) && params[1].isGPR());
            jit.add64(CCallHelpers::TrustedImm32(addend), params[1].gpr(), params.gpScratch(0));
            jit.store64(params.gpScratch(0), CCallHelpers::Address(CCallHelpers::stackPointerRegister, params[0].offsetFromSP()));
        });
        return patch;
    };

    // The second writes the word that the first has left its result in.
    Operation* first = appendIncrementInOutgoingWord(1);
    Operation* second = appendIncrementInOutgoingWord(2);
    out.ret(out.add(out.mul(first, 10), second));

    compileOrFail(test);
    AOT_TEST_CHECK(runs == 2);
    AOT_TEST_CHECK(test.call<int64_t>(int64_t { 100 }) == 1112);
}

static void testMachinePatchTupleResults()
{
    constexpr int64_t a = 700;
    constexpr int64_t b = 30;
    TestProcedure test;
    Output& out = test.out();
    Procedure& proc = test.procedure();
    Operation* left = test.argumentGPR(0);
    Operation* right = test.argumentGPR(1);
    proc.requestCallArgAreaSizeInBytes(2 * sizeof(uint64_t));
    PatchTestObservation seen;

    // Nothing extracts the last member.
    PatchOperation* patch = out.patch(proc.addTuple({ Int64, Double, Int64, Int64, Int64 }));
    patch->appendSomeRegister(left);
    patch->appendSomeRegister(right);
    patch->resultConstraints = { ValueRep::reg(GPRInfo::argumentGPR0), ValueRep::SomeRegister, ValueRep::SomeEarlyRegister, ValueRep::stackArgument(8), ValueRep::SomeRegister };
    patch->numGPScratchRegisters = 2;
    patch->effects = Effects::none();
    patch->setGenerator([&seen](CCallHelpers& jit, const PatchParameters& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        recordPatchTestObservation(seen, params);
        AOT_TEST_CHECK(params.size() == 7);
        GPRReg copyOfLeft = params.gpScratch(0);
        GPRReg copyOfRight = params.gpScratch(1);
        jit.move(params[5].gpr(), copyOfLeft);
        jit.move(params[6].gpr(), copyOfRight);
        jit.add64(copyOfLeft, copyOfRight, params[0].gpr());
        jit.convertInt64ToDouble(copyOfLeft, params[1].fpr());
        emitDifferenceInPatchTest(jit, copyOfLeft, copyOfRight, params[2].gpr());
        jit.store64(copyOfRight, CCallHelpers::Address(CCallHelpers::stackPointerRegister, params[3].offsetFromSP()));
        jit.move(CCallHelpers::TrustedImm64(static_cast<int64_t>(poison)), params[4].gpr());
    });
    // The Extracts stand right behind their Patch, one for a member.
    Operation* sum = out.extract(patch, 0);
    Operation* number = out.extract(patch, 1);
    Operation* difference = out.extract(patch, 2);
    Operation* word = out.extract(patch, 3);
    sum = out.add(out.add(sum, out.mul(difference, 3)), out.add(out.mul(word, 5), out.bitCast(number, Int64)));
    out.ret(out.add(sum, left));

    compileOrFail(test);
    AOT_TEST_CHECK(seen.runs == 1);
    AOT_TEST_CHECK(seen.reps[0] == ValueRep::reg(GPRInfo::argumentGPR0) && seen.reps[1].isFPR() && seen.reps[2].isGPR() && seen.reps[4].isGPR());
    AOT_TEST_CHECK(seen.reps[3] == ValueRep::stackArgument(8));
    AOT_TEST_CHECK(std::popcount(registersAmongPatchTestReps(seen.reps.span().first(5))) == 4);
    AOT_TEST_CHECK(!(maskOf(seen.reps[2].reg()) & registersAmongPatchTestReps(seen.reps.span().subspan(5))));
    uint64_t expected = (a + b) + 3 * (a - b) + 5 * b + std::bit_cast<uint64_t>(static_cast<double>(a)) + a;
    AOT_TEST_CHECK(test.call<uint64_t>(a, b) == expected);
}

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
        // Member number n is the argument plus n.
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

static void testMachinePatchTupleWithMoreMembersThanRegisters()
{
    checkMachinePatchTupleWithMoreMembersThanRegisters(false);
}

static void testMachinePatchThatEndsBlockWithMoreMembersThanRegisters()
{
    checkMachinePatchTupleWithMoreMembersThanRegisters(true);
}

static void testMachinePatchScratchRegistersOfBothBanks()
{
    constexpr int64_t a = 300;
    constexpr double x = 0.75;
    TestProcedure test;
    Output& out = test.out();
    Operation* integerArgument = test.argumentGPR(0);
    Operation* numberArgument = test.argumentFPR(0);
    unsigned numberOfIntegers = numberOfRegistersInPatchTest(test, Bank::GP);
    unsigned numberOfNumbers = numberOfRegistersInPatchTest(test, Bank::FP);
    Vector<Operation*> integers = definePatchTestValues(out, integerArgument, numberOfIntegers);
    Vector<Operation*> numbers = definePatchTestValues(out, numberArgument, numberOfNumbers);
    PatchTestObservation seen;

    PatchOperation* patch = out.patch(Int64);
    patch->appendSomeRegister(integers[0]);
    patch->appendSomeRegister(numbers[0]);
    patch->numGPScratchRegisters = 3;
    patch->numFPScratchRegisters = 3;
    patch->effects = Effects::none();
    patch->setGenerator([&seen](CCallHelpers& jit, const PatchParameters& params) {
        recordPatchTestObservation(seen, params);
        jit.moveDoubleTo64(params[2].fpr(), params.gpScratch(0));
        jit.add64(params[1].gpr(), params.gpScratch(0));
        jit.move(params.gpScratch(0), params[0].gpr());
        emitPoisonInPatchTest(jit, seen.scratch);
    });
    Operation* sumOfNumbers = addUpPatchTestValues(out, numberArgument, numbers);
    out.ret(out.add(addUpPatchTestValues(out, patch, integers), out.bitCast(sumOfNumbers, Int64)));

    compileOrFail(test);
    RegisterFile file = registerFileOfPatchTest(test);
    AOT_TEST_CHECK(seen.runs == 1 && seen.reps.size() == 3);
    AOT_TEST_CHECK(std::popcount(seen.scratch & file.allocatableIn(Bank::GP)) == 3 && std::popcount(seen.scratch & file.allocatableIn(Bank::FP)) == 3);
    AOT_TEST_CHECK(!(seen.scratch & (registersAmongPatchTestReps(seen.reps.span()) | seen.used | seen.unavailable)));
    uint64_t expected = patchTestInteger(a, 0) + std::bit_cast<uint64_t>(patchTestNumber(x, 0)) + sumOfPatchTestIntegers(a, numberOfIntegers);
    expected += std::bit_cast<uint64_t>(sumOfPatchTestNumbers(x, x, numberOfNumbers));
    AOT_TEST_CHECK(test.call<uint64_t>(a, x) == expected);
}

static void runPatchTestOfClobbers(bool isEarly)
{
    constexpr int64_t a = 50;
    constexpr int64_t b = 8;
    constexpr double x = 3.5;
    TestProcedure test;
    Output& out = test.out();
    Operation* left = test.argumentGPR(0);
    Operation* right = test.argumentGPR(1);
    Operation* number = test.argumentFPR(0);
    RegisterMask clobbered = maskOf(Reg(GPRInfo::argumentGPR0)) | maskOf(Reg(GPRInfo::argumentGPR1)) | maskOf(Reg(FPRInfo::argumentFPR0));
    PatchTestObservation seen;

    PatchOperation* patch = out.patch(Int64);
    patch->appendSomeRegister(left);
    patch->appendSomeRegister(right);
    if (isEarly)
        patch->clobberEarly(clobbered);
    else
        patch->clobberLate(clobbered);
    patch->effects = Effects::none();
    patch->setGenerator([&seen, clobbered, isEarly](CCallHelpers& jit, const PatchParameters& params) {
        recordPatchTestObservation(seen, params);
        if (isEarly)
            emitPoisonInPatchTest(jit, clobbered);
        emitDifferenceInPatchTest(jit, params[1].gpr(), params[2].gpr(), params[0].gpr());
        if (!isEarly)
            emitPoisonInPatchTest(jit, clobbered);
    });
    AOT_TEST_CHECK(patch->earlyClobberedMask() == (isEarly ? clobbered : 0) && patch->lateClobberedMask() == (isEarly ? 0 : clobbered));
    out.ret(out.add(out.add(patch, left), out.add(out.shl(right, 1), out.bitCast(number, Int64))));

    compileOrFail(test);
    AOT_TEST_CHECK(seen.runs == 1 && seen.reps.size() == 3);
    AOT_TEST_CHECK(!(registersAmongPatchTestReps(isEarly ? seen.reps.span().subspan(1) : seen.reps.span().first(1)) & clobbered));
    AOT_TEST_CHECK(!(seen.used & clobbered & ~maskOf(seen.reps[0].reg())));
    AOT_TEST_CHECK(test.call<uint64_t>(a, b, x) == (a - b) + a + 2 * b + std::bit_cast<uint64_t>(x));
}

static void testMachinePatchEarlyClobbers() { runPatchTestOfClobbers(true); }
static void testMachinePatchLateClobbers() { runPatchTestOfClobbers(false); }

static void testMachinePatchThatClobbersLikeCall()
{
    constexpr int64_t a = 50;
    constexpr int64_t b = 8;
    constexpr double x = 3.5;
    TestProcedure test;
    Output& out = test.out();
    RegisterFile file = registerFileOfPatchTest(test);
    Operation* left = test.argumentGPR(0);
    Operation* right = test.argumentGPR(1);
    Operation* number = test.argumentFPR(0);
    unsigned numberOfIntegers = static_cast<unsigned>(std::popcount(file.calleeSavedIn(Bank::GP))) + 3;
    unsigned numberOfNumbers = static_cast<unsigned>(std::popcount(file.calleeSavedIn(Bank::FP))) + 3;
    Vector<Operation*> integers = definePatchTestValues(out, left, numberOfIntegers);
    Vector<Operation*> numbers = definePatchTestValues(out, number, numberOfNumbers);
    RegisterMask clobbered = callerSavedRegistersOfPatchTest(test);
    PatchTestObservation seen;

    PatchOperation* patch = out.patch(Int64);
    patch->append(left, ValueRep::reg(GPRInfo::argumentGPR1));
    patch->append(right, ValueRep::reg(GPRInfo::argumentGPR0));
    patch->append(number, ValueRep::reg(FPRInfo::argumentFPR1));
    patch->resultConstraints = { ValueRep::reg(GPRInfo::returnValueGPR) };
    patch->clobberLate(clobbered);
    patch->clobber(RegisterSet::macroClobberedGPRs());
    patch->setGenerator([&seen, clobbered](CCallHelpers& jit, const PatchParameters& params) {
        recordPatchTestObservation(seen, params);
        emitDifferenceInPatchTest(jit, GPRInfo::argumentGPR1, GPRInfo::argumentGPR0, GPRInfo::returnValueGPR);
        jit.moveDoubleTo64(FPRInfo::argumentFPR1, GPRInfo::argumentGPR2);
        jit.add64(GPRInfo::argumentGPR2, GPRInfo::returnValueGPR);
        emitPoisonInPatchTest(jit, clobbered & ~maskOf(Reg(GPRInfo::returnValueGPR)));
    });
    Operation* sumOfNumbers = addUpPatchTestValues(out, number, numbers);
    out.ret(out.add(addUpPatchTestValues(out, patch, integers), out.bitCast(sumOfNumbers, Int64)));

    compileOrFail(test);
    AOT_TEST_CHECK(seen.runs == 1 && seen.frameIsSetUp && seen.reps[0] == ValueRep::reg(GPRInfo::returnValueGPR));
    AOT_TEST_CHECK(!(seen.used & clobbered & ~maskOf(Reg(GPRInfo::returnValueGPR))));
    uint64_t expected = (a - b) + std::bit_cast<uint64_t>(x) + sumOfPatchTestIntegers(a, numberOfIntegers);
    expected += std::bit_cast<uint64_t>(sumOfPatchTestNumbers(x, x, numberOfNumbers));
    AOT_TEST_CHECK(test.call<uint64_t>(a, b, x) == expected);
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

    // The result, which is defined where the continuation begins, has to outlive a call there.
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

static void testMachinePatchUsedAndUnavailableRegisters()
{
    constexpr int64_t a = 300;
    constexpr int64_t b = 40;
    constexpr double x = 0.75;
    constexpr unsigned count = 3;
    TestProcedure test;
    Output& out = test.out();
    Operation* number = test.argumentFPR(0);
    Operation* dying = test.argumentGPR(1);
    Vector<Operation*> integers = definePatchTestValues(out, test.argumentGPR(0), count);
    Vector<Operation*> numbers = definePatchTestValues(out, number, count);
    RegisterMask allocatable = allocatableRegistersOfPatchTest(test);
    PatchTestObservation seen;

    // Whatever is not unavailable is the generator's to write, once it has read its operands.
    PatchOperation* patch = out.patch(Int64);
    patch->appendSomeRegister(dying);
    patch->numGPScratchRegisters = 1;
    patch->effects = Effects::none();
    patch->setGenerator([&seen, allocatable](CCallHelpers& jit, const PatchParameters& params) {
        recordPatchTestObservation(seen, params);
        jit.add64(CCallHelpers::TrustedImm32(1), params[1].gpr(), params[0].gpr());
        emitPoisonInPatchTest(jit, allocatable & ~seen.unavailable);
    });
    Operation* sumOfNumbers = addUpPatchTestValues(out, number, numbers);
    out.ret(out.add(addUpPatchTestValues(out, patch, integers), out.bitCast(sumOfNumbers, Int64)));

    compileOrFail(test);
    RegisterMask result = maskOf(seen.reps[0].reg());
    RegisterMask calleeSavesLeftAlone = registerFileOfPatchTest(test).calleeSaved & ~test.procedure().frame().calleeSavesMask();
    AOT_TEST_CHECK(seen.runs == 1 && (seen.used & result));
    AOT_TEST_CHECK(!(seen.used & (seen.scratch | (maskOf(seen.reps[1].reg()) & ~result))));
    AOT_TEST_CHECK(!(seen.used & ~allocatable));
    AOT_TEST_CHECK(!((seen.used | pinnedRegisters() | calleeSavesLeftAlone) & ~seen.unavailable));
    AOT_TEST_CHECK(!(seen.unavailable & seen.scratch));
    uint64_t expected = (b + 1) + sumOfPatchTestIntegers(a, count) + std::bit_cast<uint64_t>(sumOfPatchTestNumbers(x, x, count));
    AOT_TEST_CHECK(test.call<uint64_t>(a, b, x) == expected);
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

// Each edge from a Patch to the join has a block of its own, and the allocation has nothing to move in either. Such a block has no code: whoever jumps to it jumps to the join.
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

static void testMachinePatchLatePaths()
{
    TestProcedure test;
    Output& out = test.out();
    Operation* a = test.argumentGPR(0);
    Operation* b = test.argumentGPR(1);
    Vector<int32_t, 4> events;

    auto appendPatchWithLatePath = [&](Operation* operand, int32_t number) {
        PatchOperation* patch = out.patch(Int64);
        patch->appendSomeRegister(operand);
        patch->resultConstraints = { ValueRep::SomeEarlyRegister };
        patch->effects = Effects::none();
        patch->setGenerator([&events, number](CCallHelpers& jit, const PatchParameters& params) {
            events.append(number);
            GPRReg result = params[0].gpr();
            jit.move(params[1].gpr(), result);
            CCallHelpers::Jump isOdd = jit.branchTest64(CCallHelpers::NonZero, params[1].gpr(), CCallHelpers::TrustedImm32(1));
            CCallHelpers::Label done = jit.label();
            params.addLatePath([&events, number, result, isOdd, done](CCallHelpers& jit) {
                events.append(10 + number);
                isOdd.link(&jit);
                jit.add64(CCallHelpers::TrustedImm32(100 * number), result);
                jit.jump().linkTo(done, &jit);
            });
        });
        return patch;
    };

    Operation* first = appendPatchWithLatePath(a, 1);
    Operation* second = appendPatchWithLatePath(b, 2);
    out.ret(out.add(first, second));

    compileOrFail(test);
    AOT_TEST_CHECK(events.size() == 4 && events[0] == 1 && events[1] == 2 && events[2] == 11 && events[3] == 12);
    AOT_TEST_CHECK(test.call<int64_t>(int64_t { 2 }, int64_t { 4 }) == 6);
    AOT_TEST_CHECK(test.call<int64_t>(int64_t { 3 }, int64_t { 4 }) == 107);
    AOT_TEST_CHECK(test.call<int64_t>(int64_t { 2 }, int64_t { 5 }) == 207);
    AOT_TEST_CHECK(test.call<int64_t>(int64_t { 3 }, int64_t { 5 }) == 308);
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

// One Check for each constant. It fails where the argument is its index, and its late path returns the constant. Answers how many late paths were written.
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

static void testMachinePatchChecksOfConstants()
{
    TestProcedure test;
    Output& out = test.out();
    unsigned runsOfCheckThatPasses = 0;
    unsigned runsOfCheckThatFails = 0;

    PatchOperation* passes = out.check(out.int32Zero);
    passes->setGenerator([&runsOfCheckThatPasses](CCallHelpers& jit, const PatchParameters&) {
        ++runsOfCheckThatPasses;
        jit.breakpoint();
    });
    AOT_TEST_CHECK(passes->hasNoPosition());

    PatchOperation* fails = out.check(out.int32One);
    fails->setGenerator([&runsOfCheckThatFails](CCallHelpers& jit, const PatchParameters& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        ++runsOfCheckThatFails;
        AOT_TEST_CHECK(!params.size());
        jit.move(CCallHelpers::TrustedImm32(5), GPRInfo::returnValueGPR);
        emitReturnInPatchTest(jit, params);
    });
    AOT_TEST_CHECK(!fails->hasNoPosition());
    out.ret(out.constInt64(6));

    compileOrFail(test);
    AOT_TEST_CHECK(!runsOfCheckThatPasses && runsOfCheckThatFails == 1);
    AOT_TEST_CHECK(test.call<int64_t>() == 5);
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

static PatchOperation* appendPatchTestReleaseHint(Output& out, Operation* released, PatchTestObservation& seen, RegisterMask& cleared)
{
    PatchOperation* hint = out.patch(Void);
    hint->append(released, ValueRep::ColdAny);
    hint->effects = Effects::none();
    hint->effects.writesLocalState = true;
    hint->setReleasesDeadValues(true);
    hint->setGenerator([&seen, &cleared](CCallHelpers& jit, const PatchParameters& params) {
        recordPatchTestObservation(seen, params);
        cleared = maskOf(params.value()->registersToRelease()) & ~params.usedRegistersMask();
        forEachRegister(cleared, [&](Reg reg) {
            jit.move(CCallHelpers::TrustedImm32(0), reg.gpr());
        });
    });
    return hint;
}

static void testMachinePatchReleasesValueInRegister()
{
    TestProcedure test;
    Output& out = test.out();
    Operation* cell = out.opaque(out.add(test.argumentGPR(0), 5));
    PatchTestObservation seen;
    RegisterMask cleared = 0;
    appendPatchTestReleaseHint(out, cell, seen, cleared);

    PatchOperation* reader = out.patch(Int64);
    reader->effects = Effects::none();
    reader->setGenerator([&cleared](CCallHelpers& jit, const PatchParameters& params) {
        AOT_TEST_CHECK(cleared);
        jit.move(Reg::fromIndex(static_cast<unsigned>(std::countr_zero(cleared))).gpr(), params[0].gpr());
    });
    out.ret(reader);

    compileOrFail(test);
    AOT_TEST_CHECK(seen.runs == 1 && seen.reps.size() == 1 && seen.reps[0].isGPR() && (cleared & maskOf(seen.reps[0].reg())));
    AOT_TEST_CHECK(!test.call<int64_t>(int64_t { 37 }));
}

static void testMachinePatchReleasesValueInFrame()
{
    constexpr int64_t a = 800;
    TestProcedure test;
    Output& out = test.out();
    Operation* seed = test.argumentGPR(0);
    unsigned count = numberOfRegistersInPatchTest(test, Bank::GP) + 2;

    // Of everything that is live while the values are defined, the cell is read last, so it is the first to lose its register.
    Operation* cell = out.opaque(out.add(seed, 5));
    Operation* sum = addUpPatchTestValues(out, seed, definePatchTestValues(out, seed, count));
    PatchTestObservation seen;
    RegisterMask cleared = 0;
    appendPatchTestReleaseHint(out, cell, seen, cleared);

    PatchOperation* reader = out.patch(Int64);
    reader->effects = Effects::none();
    reader->setGenerator([&seen](CCallHelpers& jit, const PatchParameters& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        AOT_TEST_CHECK(seen.runs == 1 && seen.reps.size() == 1 && seen.reps[0].isStack() && params.frameIsSetUp());
        jit.load64(CCallHelpers::Address(GPRInfo::callFrameRegister, seen.reps[0].offsetFromFP()), params[0].gpr());
    });
    out.ret(out.add(sum, reader));

    compileOrFail(test);
    const Allocation& allocation = test.procedure().allocation();
    AOT_TEST_CHECK(allocation.isSpilled(cell));
    AOT_TEST_CHECK(seen.reps[0] == ValueRep::stack(test.procedure().frame().offsetFromFPOf(allocation.spillOf(cell))));
    AOT_TEST_CHECK(numberOfStepsInPatchTest(allocation, StepKind::ClearSpill) == 1);
    for (const Step& step : allocation.steps())
        AOT_TEST_CHECK(step.kind != StepKind::ClearSpill || step.destination == Location::spill(allocation.spillOf(cell)));
    AOT_TEST_CHECK(test.call<uint64_t>(a) == a + sumOfPatchTestIntegers(a, count));
}

static void testMachinePatchReleasesNothingThatIsReadAgain()
{
    TestProcedure test;
    Output& out = test.out();
    Operation* cell = out.opaque(out.add(test.argumentGPR(0), 5));
    PatchTestObservation seen;
    RegisterMask cleared = 0;
    appendPatchTestReleaseHint(out, cell, seen, cleared);
    out.ret(cell);

    compileOrFail(test);
    AOT_TEST_CHECK(seen.runs == 1 && !cleared);
    AOT_TEST_CHECK(!numberOfStepsInPatchTest(test.procedure().allocation(), StepKind::ClearSpill));
    AOT_TEST_CHECK(test.call<int64_t>(int64_t { 37 }) == 42);
}

// The cell waits across a call in a register that callees save. An edge hands it to a Phi that lies in another, and nothing takes the first again.
static void testMachinePatchReleasesRegisterOfAnotherName()
{
    TestProcedure test;
    Output& out = test.out();
    RegisterMask saved = registerFileOfPatchTest(test).calleeSaved & registerFileOfPatchTest(test).allocatableIn(Bank::GP);
    AOT_TEST_CHECK(std::popcount(saved) >= 2);
    Reg first = Reg::fromIndex(static_cast<unsigned>(std::countr_zero(saved)));
    Reg second = Reg::fromIndex(static_cast<unsigned>(std::countr_zero(saved & (saved - 1))));
    auto appendReader = [&](Operation* read, ValueRep rep, PatchTestObservation& seen) {
        PatchOperation* reader = out.patch(Void);
        reader->append(read, rep);
        reader->effects = Effects::none();
        reader->effects.writesLocalState = true;
        reader->setGenerator([&seen](CCallHelpers&, const PatchParameters& params) {
            recordPatchTestObservation(seen, params);
        });
    };

    Block* made = out.newBlock();
    Block* waited = out.newBlock();
    Block* joined = out.newBlock();
    Operation* part = out.opaque(out.add(test.argumentGPR(0), 1));
    Operation* cell = out.opaque(out.add(test.argumentGPR(0), 5));
    PatchOperation* call = appendPatchTestCallWithoutOperands(test);
    call->append(part, ValueRep::reg(first));
    call->append(cell, ValueRep::reg(second));
    out.branch(out.testNonZero64(part, 1), normalEdge(made), normalEdge(waited));

    // The cell keeps its register while the other value of the Phi is given one.
    out.appendTo(made);
    PatchTestObservation seenBesideOther;
    ValueFromBlock fromMade = out.anchor(out.opaque(out.add(part, 100)));
    appendReader(cell, ValueRep::ColdAny, seenBesideOther);
    out.jump(joined);

    out.appendTo(waited);
    PatchTestObservation seenWaiting;
    appendReader(cell, ValueRep::SomeRegister, seenWaiting);
    ValueFromBlock fromWaited = out.anchor(cell);
    out.jump(joined);

    out.appendTo(joined);
    Operation* released = out.phi(Int64, fromMade, fromWaited);
    appendPatchTestCallWithoutOperands(test)->append(released, ValueRep::reg(first));
    PatchTestObservation seen;
    RegisterMask cleared = 0;
    appendPatchTestReleaseHint(out, released, seen, cleared);
    PatchOperation* reader = out.patch(Int64);
    reader->effects = Effects::none();
    reader->setGenerator([&seenWaiting](CCallHelpers& jit, const PatchParameters& params) {
        AOT_TEST_CHECK(seenWaiting.runs == 1 && seenWaiting.reps.size() == 1 && seenWaiting.reps[0].isGPR());
        jit.move(seenWaiting.reps[0].gpr(), params[0].gpr());
    });
    out.ret(reader);

    compileOrFail(test);
    AOT_TEST_CHECK(maskOf(seenWaiting.reps[0].reg()) & saved);
    AOT_TEST_CHECK(seen.runs == 1 && seen.reps.size() == 1 && seen.reps[0].isGPR() && seen.reps[0].reg() != seenWaiting.reps[0].reg());
    AOT_TEST_CHECK((cleared & maskOf(seen.reps[0].reg())) && (cleared & maskOf(seenWaiting.reps[0].reg())));
    AOT_TEST_CHECK(!test.call<int64_t>(int64_t { 36 }));
    AOT_TEST_CHECK(!test.call<int64_t>(int64_t { 37 }));
}

// The cell is spilled. One arm hands it to a Phi and releases that, the other reads it further on in the order, and no way leads from the first to the second.
// Where it is read behind both arms as well, the spill stays.
static void testMachinePatchClearsSpillThatAnotherArmReads()
{
    for (bool isReadBehindArms : { false, true }) {
        TestProcedure test;
        Output& out = test.out();
        Block* releasing = out.newBlock();
        Block* made = out.newBlock();
        Block* waited = out.newBlock();
        Block* joined = out.newBlock();
        Block* reading = out.newBlock();
        Block* done = out.newBlock();

        Operation* a = test.argumentGPR(0);
        Operation* cell = out.opaque(out.add(a, 5));
        PatchOperation* call = out.patch(Void);
        call->clobberLate(allocatableRegistersOfPatchTest(test));
        call->setGenerator([](CCallHelpers&, const PatchParameters&) { });
        out.branch(out.testNonZero64(a, 2), normalEdge(releasing), normalEdge(reading));

        out.appendTo(releasing);
        out.branch(out.testNonZero64(a, 1), normalEdge(made), normalEdge(waited));
        out.appendTo(made);
        ValueFromBlock fromMade = out.anchor(out.opaque(out.add(a, 100)));
        out.jump(joined);
        out.appendTo(waited);
        ValueFromBlock fromWaited = out.anchor(cell);
        out.jump(joined);

        out.appendTo(joined);
        PatchTestObservation seen;
        RegisterMask cleared = 0;
        PatchOperation* hint = appendPatchTestReleaseHint(out, out.phi(Int64, fromMade, fromWaited), seen, cleared);
        PatchOperation* reader = out.patch(Int64);
        reader->effects = Effects::none();
        reader->setGenerator([cell](CCallHelpers& jit, const PatchParameters& params) {
            AllowMacroScratchRegisterUsage allowScratch(jit);
            jit.load64(CCallHelpers::Address(GPRInfo::callFrameRegister, params.frame().offsetFromFPOf(params.proc().allocation().spillOf(cell))), params[0].gpr());
        });
        ValueFromBlock fromJoined = out.anchor(reader);
        out.jump(done);

        out.appendTo(reading);
        ValueFromBlock fromReading = out.anchor(out.add(cell, 1));
        out.jump(done);

        out.appendTo(done);
        Operation* result = out.phi(Int64, fromJoined, fromReading);
        out.ret(isReadBehindArms ? out.add(result, cell) : result);

        compileOrFail(test);
        Procedure& proc = test.procedure();
        const Allocation& allocation = proc.allocation();
        AOT_TEST_CHECK(allocation.isSpilled(cell) && proc.liveness().of(cell).lastUse > proc.order().latePositionOf(hint));
        AOT_TEST_CHECK(numberOfStepsInPatchTest(allocation, StepKind::ClearSpill) == (isReadBehindArms ? 0u : 1u));
        for (const Step& step : allocation.steps())
            AOT_TEST_CHECK(step.kind != StepKind::ClearSpill || step.destination == Location::spill(allocation.spillOf(cell)));
        AOT_TEST_CHECK(test.call<int64_t>(int64_t { 2 }) == (isReadBehindArms ? 14 : 0));
        AOT_TEST_CHECK(test.call<int64_t>(int64_t { 3 }) == (isReadBehindArms ? 16 : 0));
        AOT_TEST_CHECK(test.call<int64_t>(int64_t { 0 }) == (isReadBehindArms ? 11 : 6));
    }
}

// The same where the cell waits in a register that callees save and has no spill.
static void testMachinePatchReleasesRegisterThatAnotherArmReads()
{
    for (bool isReadBehindArms : { false, true }) {
        TestProcedure test;
        Output& out = test.out();
        RegisterMask saved = registerFileOfPatchTest(test).calleeSaved & registerFileOfPatchTest(test).allocatableIn(Bank::GP);
        AOT_TEST_CHECK(saved);
        Block* releasing = out.newBlock();
        Block* made = out.newBlock();
        Block* waited = out.newBlock();
        Block* joined = out.newBlock();
        Block* reading = out.newBlock();
        Block* done = out.newBlock();

        Operation* a = test.argumentGPR(0);
        Operation* cell = out.opaque(out.add(a, 5));
        appendPatchTestCallWithoutOperands(test)->append(cell, ValueRep::reg(Reg::fromIndex(static_cast<unsigned>(std::countr_zero(saved)))));
        out.branch(out.testNonZero64(a, 2), normalEdge(releasing), normalEdge(reading));

        out.appendTo(releasing);
        out.branch(out.testNonZero64(a, 1), normalEdge(made), normalEdge(waited));
        out.appendTo(made);
        ValueFromBlock fromMade = out.anchor(out.opaque(out.add(a, 100)));
        out.jump(joined);
        out.appendTo(waited);
        PatchTestObservation seenWaiting;
        PatchOperation* witness = out.patch(Void);
        witness->append(cell, ValueRep::SomeRegister);
        witness->effects = Effects::none();
        witness->effects.writesLocalState = true;
        witness->setGenerator([&seenWaiting](CCallHelpers&, const PatchParameters& params) {
            recordPatchTestObservation(seenWaiting, params);
        });
        ValueFromBlock fromWaited = out.anchor(cell);
        out.jump(joined);

        out.appendTo(joined);
        PatchTestObservation seen;
        RegisterMask cleared = 0;
        PatchOperation* hint = appendPatchTestReleaseHint(out, out.phi(Int64, fromMade, fromWaited), seen, cleared);
        PatchOperation* reader = out.patch(Int64);
        reader->effects = Effects::none();
        reader->setGenerator([&seenWaiting](CCallHelpers& jit, const PatchParameters& params) {
            AOT_TEST_CHECK(seenWaiting.runs == 1 && seenWaiting.reps.size() == 1 && seenWaiting.reps[0].isGPR());
            jit.move(seenWaiting.reps[0].gpr(), params[0].gpr());
        });
        ValueFromBlock fromJoined = out.anchor(reader);
        out.jump(done);

        out.appendTo(reading);
        ValueFromBlock fromReading = out.anchor(out.add(cell, 1));
        out.jump(done);

        out.appendTo(done);
        Operation* result = out.phi(Int64, fromJoined, fromReading);
        out.ret(isReadBehindArms ? out.add(result, cell) : result);

        compileOrFail(test);
        Procedure& proc = test.procedure();
        AOT_TEST_CHECK(!proc.allocation().isSpilled(cell) && proc.liveness().of(cell).lastUse > proc.order().latePositionOf(hint));
        AOT_TEST_CHECK(maskOf(seenWaiting.reps[0].reg()) & saved);
        AOT_TEST_CHECK(seen.runs == 1 && seen.reps.size() == 1 && seen.reps[0] != seenWaiting.reps[0]);
        AOT_TEST_CHECK(!(cleared & maskOf(seenWaiting.reps[0].reg())) == isReadBehindArms);
        AOT_TEST_CHECK(test.call<int64_t>(int64_t { 2 }) == (isReadBehindArms ? 14 : 0));
        AOT_TEST_CHECK(test.call<int64_t>(int64_t { 3 }) == (isReadBehindArms ? 16 : 0));
        AOT_TEST_CHECK(test.call<int64_t>(int64_t { 0 }) == (isReadBehindArms ? 11 : 6));
    }
}

static Location placeOfOnlyOperandInPatchTest(const Allocation& allocation, const PatchOperation* patch)
{
    const PatchAssignment& assignment = allocation.patchAssignment(allocation.step(indexOfExecuteStepInPatchTest(allocation, patch)).patchAssignment);
    AOT_TEST_CHECK(assignment.locations.size() == 1);
    return assignment.locations[0];
}

static void testMachinePatchThatOnlyKeepsChildrenAlive()
{
    TestProcedure test;
    Output& out = test.out();
    Operation* a = test.argumentGPR(0);
    Operation* kept = out.opaque(out.add(a, 7));
    Operation* unread = out.opaque(out.add(a, 8));
    appendPatchTestCallWithoutOperands(test);

    PatchOperation* keeper = out.patch(Void);
    keeper->append(kept, ValueRep::ColdAny);
    keeper->effects = Effects::none();
    keeper->effects.writesLocalState = true;
    keeper->setRunsWithoutFrame(true);
    keeper->onlyKeepsChildrenAlive = true;
    keeper->setGenerator([](CCallHelpers&, const PatchParameters&) { });

    PatchOperation* reader = out.patch(Int64);
    reader->effects = Effects::none();
    reader->setGenerator([keeper](CCallHelpers& jit, const PatchParameters& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        Location place = placeOfOnlyOperandInPatchTest(params.proc().allocation(), keeper);
        AOT_TEST_CHECK(place.isRegister() || place.isSpill());
        if (place.isRegister())
            jit.move(place.gpr(), params[0].gpr());
        else
            jit.load64(CCallHelpers::Address(GPRInfo::callFrameRegister, params.frame().offsetFromFPOf(place.spillIndex())), params[0].gpr());
    });
    out.ret(reader);

    compileOrFail(test);
    Procedure& proc = test.procedure();
    const ValueFacts& facts = proc.liveness().of(kept);
    AOT_TEST_CHECK(proc.liveness().isDropped(unread) && !proc.liveness().isDropped(kept));
    AOT_TEST_CHECK(facts.lastUse != noPosition && facts.lastUse >= proc.order().earlyPositionOf(keeper));
    Location place = placeOfOnlyOperandInPatchTest(proc.allocation(), keeper);
    AOT_TEST_CHECK(place.isSpill() || (maskOf(place.reg()) & registerFileOfPatchTest(test).calleeSaved));
    AOT_TEST_CHECK(test.call<int64_t>(int64_t { 35 }) == 42);
}

static void testMachinePatchGeneratorThatWrapsAnother()
{
    TestProcedure test;
    Output& out = test.out();
    PatchTestObservation seen;
    Vector<int32_t, 4> addendsInOrder;

    PatchOperation* patch = appendPatchTestIncrement(out, test.argumentGPR(0), 1, seen);
    for (int32_t addend : { 10, 100 }) {
        PatchGenerator* inner = patch->generator();
        patch->setGenerator([&addendsInOrder, inner, addend](CCallHelpers& jit, const PatchParameters& params) {
            inner->run(jit, params);
            addendsInOrder.append(addend);
            jit.add64(CCallHelpers::TrustedImm32(addend), params[0].gpr());
        });
    }
    out.ret(patch);

    compileOrFail(test);
    AOT_TEST_CHECK(seen.runs == 1 && addendsInOrder.size() == 2 && addendsInOrder[0] == 10 && addendsInOrder[1] == 100);
    AOT_TEST_CHECK(test.call<int64_t>(int64_t { 1 }) == 112);
}

static void testMachinePatchWithoutEffectsThatNothingReadsIsDropped()
{
    TestProcedure test;
    Output& out = test.out();
    Operation* a = test.argumentGPR(0);
    PatchTestObservation seen;
    PatchOperation* patch = appendPatchTestIncrement(out, a, 1, seen);
    out.ret(a);

    compileOrFail(test);
    AOT_TEST_CHECK(!seen.runs && test.procedure().liveness().isDropped(patch));
    AOT_TEST_CHECK(test.procedure().frame().coverage() == FrameCoverage::None);
    AOT_TEST_CHECK(test.call<int64_t>(int64_t { 4 }) == 4);
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

static std::span<const NativeTest> listOfMachinePatchTests()
{
    static constexpr NativeTest tests[] = {
        { "machine/patch/register-operands"_s, testMachinePatchRegisterOperands },
        { "machine/patch/register-operands-displace-live-values"_s, testMachinePatchRegisterOperandsDisplaceLiveValues },
        { "machine/patch/registers-of-numbers"_s, testMachinePatchRegistersOfNumbers },
        { "machine/patch/register-result-displaces-live-value"_s, testMachinePatchRegisterResultDisplacesLiveValue },
        { "machine/patch/some-register-under-pressure"_s, testMachinePatchSomeRegisterUnderPressure },
        { "machine/patch/some-early-register"_s, testMachinePatchSomeEarlyRegister },
        { "machine/patch/some-late-register"_s, testMachinePatchSomeLateRegister },
        { "machine/patch/warm-any"_s, testMachinePatchWarmAny },
        { "machine/patch/cold-any"_s, testMachinePatchColdAny },
        { "machine/patch/late-cold-any"_s, testMachinePatchLateColdAny },
        { "machine/patch/warm-any-under-pressure"_s, testMachinePatchWarmAnyUnderPressure },
        { "machine/patch/cold-any-under-pressure"_s, testMachinePatchColdAnyUnderPressure },
        { "machine/patch/late-cold-any-under-pressure"_s, testMachinePatchLateColdAnyUnderPressure },
        { "machine/patch/stack-argument-operands"_s, testMachinePatchStackArgumentOperands },
        { "machine/patch/stack-argument-results"_s, testMachinePatchStackArgumentResults },
        { "machine/patch/tuple-results"_s, testMachinePatchTupleResults },
        { "machine/patch/tuple-with-more-members-than-registers"_s, testMachinePatchTupleWithMoreMembersThanRegisters },
        { "machine/patch/patch-that-ends-block-with-more-members-than-registers"_s, testMachinePatchThatEndsBlockWithMoreMembersThanRegisters },
        { "machine/patch/scratch-registers-of-both-banks"_s, testMachinePatchScratchRegistersOfBothBanks },
        { "machine/patch/early-clobbers"_s, testMachinePatchEarlyClobbers },
        { "machine/patch/late-clobbers"_s, testMachinePatchLateClobbers },
        { "machine/patch/clobbers-like-a-call"_s, testMachinePatchThatClobbersLikeCall },
        { "machine/patch/clobbers-on-a-successor"_s, testMachinePatchClobbersOnSuccessor },
        { "machine/patch/used-and-unavailable-registers"_s, testMachinePatchUsedAndUnavailableRegisters },
        { "machine/patch/successor-labels-and-fall-through"_s, testMachinePatchSuccessorLabelsAndFallThrough },
        { "machine/patch/enters-join-through-blocks-without-code"_s, testMachinePatchEntersJoinThroughBlocksWithoutCode },
        { "machine/patch/late-paths"_s, testMachinePatchLatePaths },
        { "machine/patch/check-hands-over-operands"_s, testMachinePatchCheckHandsOverOperands },
        { "machine/patch/checks-share-late-paths"_s, testMachinePatchChecksShareLatePaths },
        { "machine/patch/checks-of-constants"_s, testMachinePatchChecksOfConstants },
        { "machine/patch/frame-is-set-up"_s, testMachinePatchFrameIsSetUp },
        { "machine/patch/frame-is-set-up-in-rare-region"_s, testMachinePatchFrameIsSetUpInRareRegion },
        { "machine/patch/releases-value-in-register"_s, testMachinePatchReleasesValueInRegister },
        { "machine/patch/releases-value-in-frame"_s, testMachinePatchReleasesValueInFrame },
        { "machine/patch/releases-nothing-that-is-read-again"_s, testMachinePatchReleasesNothingThatIsReadAgain },
        { "machine/patch/releases-register-of-another-name"_s, testMachinePatchReleasesRegisterOfAnotherName },
        { "machine/patch/clears-spill-that-another-arm-reads"_s, testMachinePatchClearsSpillThatAnotherArmReads },
        { "machine/patch/releases-register-that-another-arm-reads"_s, testMachinePatchReleasesRegisterThatAnotherArmReads },
        { "machine/patch/only-keeps-children-alive"_s, testMachinePatchThatOnlyKeepsChildrenAlive },
        { "machine/patch/generator-that-wraps-another"_s, testMachinePatchGeneratorThatWrapsAnother },
        { "machine/patch/without-effects-and-unread-is-dropped"_s, testMachinePatchWithoutEffectsThatNothingReadsIsDropped },
        { "machine/patch/wants-too-many-registers"_s, testMachinePatchThatWantsTooManyRegisters },
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
