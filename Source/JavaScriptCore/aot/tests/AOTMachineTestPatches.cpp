/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachineTestSupport.h"

#if ENABLE(AOT)

#include "AOTMachineTarget.h"
#include "AllowMacroScratchRegisterUsage.h"
#include "JITOperationList.h"
#include "JITOperationValidation.h"
#include <algorithm>
#include <bit>
#include <mutex>
#include <wtf/DataLog.h>
#include <wtf/StdLibExtras.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT { namespace Machine {

static_assert(OBJECT_OFFSETOF(TestInstance, words) == sizeof(TestInstance::wordsBelow));

void TestInstance::fillPinnedConstants()
{
    constexpr int32_t bytesOfWord = sizeof(uint64_t);
    int32_t numberOfWordsBelow = static_cast<int32_t>(wordsBelow.size());
    for (uint64_t bits : bitsOfPinnedConstants()) {
        std::optional<PinnedConstant> pinned = findPinnedConstant(bits);
        AOT_TEST_CHECK(pinned);
        if (!pinned->offset)
            continue;

        int32_t word = *pinned->offset / bytesOfWord;
        AOT_TEST_CHECK(pinned->reg == instanceGPR && !(*pinned->offset % bytesOfWord));
        AOT_TEST_CHECK(word >= -numberOfWordsBelow && word < static_cast<int32_t>(words.size()));
        if (word < 0)
            wordsBelow[static_cast<size_t>(numberOfWordsBelow + word)] = bits;
        else
            words[static_cast<size_t>(word)] = bits;
    }
}

static TestInstance& sharedTestInstance()
{
    static TestInstance instance;
    static std::once_flag onceFlag;
    std::call_once(onceFlag, [] {
        instance.fillPinnedConstants();
    });
    return instance;
}

[[noreturn]] static void failWithTextOfTestProcedure(TestProcedure& test, const char* condition)
{
    if (const std::optional<Failure>& failure = test.procedure().failure()) {
        dataLog("The back end reports ", failure->kind);
        if (failure->operation != UINT32_MAX)
            dataLog(" at @", failure->operation);
        dataLogLn(", origin ", failure->origin.bits());
    }
    dataLogLn(test.printedProcedure(), test.printedAllocation());
    failNativeTest(__FILE__, __LINE__, condition);
}

void prepareOrFail(TestProcedure& test)
{
    if (!test.prepare())
        failWithTextOfTestProcedure(test, "the back end prepares the procedure");
}

void compileOrFail(TestProcedure& test, void* instance)
{
    if (!test.compile(instance ? instance : sharedTestInstance().pointer()))
        failWithTextOfTestProcedure(test, "the back end compiles the procedure");
}

static constexpr uint8_t numberOfWorkingRegistersOfTestPatch = 2;

struct TestPatchCode {
    TestPatchKind kind { TestPatchKind::Sum };
    RegisterMask earlyClobbers { 0 };
    RegisterMask lateClobbers { 0 };
    RegisterMask temporaries { 0 };
    int64_t callee { 0 };
    RegisterMask lostOnWayToLandingPad { 0 };
};

static uint64_t JIT_OPERATION_ATTRIBUTES passSumThroughCallOfTestPatch(uint64_t sum)
{
    volatile uint64_t wordsBelowCaller[64];
    for (volatile uint64_t& word : wordsBelowCaller)
        word = poison;
    return sum;
}

static int64_t entryOfCallOfTestPatch()
{
#if ENABLE(JIT_OPERATION_VALIDATION)
    JITOperationAnnotation annotation { std::bit_cast<void*>(&passSumThroughCallOfTestPatch), std::bit_cast<void*>(&passSumThroughCallOfTestPatch) };
    JITOperationList::populatePointersInEmbedder(&annotation, &annotation + 1);
#endif
    return static_cast<int64_t>(std::bit_cast<uintptr_t>(tagCFunctionPtr<void*, OperationPtrTag>(&passSumThroughCallOfTestPatch)));
}

static RegisterMask registersTestPatchMayPoison(const RegisterFile& file)
{
    RegisterMask kept = pinnedRegisters() | maskOf(Reg(scratchGPR)) | maskOf(Reg(scratchFPR));
    if constexpr (secondScratchGPR != InvalidGPRReg)
        kept |= maskOf(Reg(secondScratchGPR));
    return (file.allocatableIn(Bank::GP) | file.allocatableIn(Bank::FP)) & ~kept;
}

static void emitPoisonOfTestPatch(CCallHelpers& jit, RegisterMask registers, GPRReg holder)
{
    jit.move(CCallHelpers::TrustedImm64(static_cast<int64_t>(poison)), holder);
    forEachRegister(registers, [&](Reg reg) {
        if (reg.isFPR())
            jit.move64ToDouble(holder, reg.fpr());
        else if (reg.gpr() != holder)
            jit.move(holder, reg.gpr());
    });
}

static GPRReg takeTemporaryOfTestPatch(RegisterMask& registers)
{
    AOT_TEST_CHECK(registers);
    Reg reg = Reg::fromIndex(static_cast<unsigned>(std::countr_zero(registers)));
    registers &= registers - 1;
    return reg.gpr();
}

static void addOperandsOfTestPatch(CCallHelpers& jit, const PatchParameters& parameters, bool areReadLate, GPRReg sum, GPRReg word)
{
    const PatchOperation* patch = parameters.value();
    unsigned numberOfResults = parameters.size() - patch->numberOfOperands();
    for (unsigned index = 0; index < patch->numberOfOperands(); ++index) {
        if (patch->constraintOf(index).isLate() != areReadLate)
            continue;
        parameters[numberOfResults + index].emitRestore(jit, word);
        if (widthOf(patch->child(index)->type()) == Width32)
            jit.zeroExtend32ToWord(word, word);
        jit.add64(word, sum);
    }
}

static void moveSumToResultOfTestPatch(CCallHelpers& jit, GPRReg sum, const ValueRep& result, Type type)
{
    switch (type.kind()) {
    case Int32:
    case Int64:
        jit.move(sum, result.gpr());
        return;
    case Float:
        jit.move32ToFloat(sum, result.fpr());
        return;
    case Double:
        jit.move64ToDouble(sum, result.fpr());
        return;
    case Void:
    case Tuple:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void generateTestPatch(CCallHelpers& jit, const PatchParameters& parameters, const TestPatchCode& code)
{
    AllowMacroScratchRegisterUsage allowScratch(jit);
    const PatchOperation* patch = parameters.value();
    bool hasResult = patch->type() != Void;
    AOT_TEST_CHECK(parameters.size() == patch->numberOfOperands() + hasResult);

    RegisterMask scratch = 0;
    for (unsigned index = 0; index < patch->numGPScratchRegisters; ++index)
        scratch |= maskOf(Reg(parameters.gpScratch(index)));
    for (unsigned index = 0; index < patch->numFPScratchRegisters; ++index)
        scratch |= maskOf(Reg(parameters.fpScratch(index)));
    RegisterMask result = hasResult ? maskOf(parameters[0].reg()) : 0;

    GPRReg sum = InvalidGPRReg;
    GPRReg word = InvalidGPRReg;
    if (code.kind == TestPatchKind::SumThroughCall) {
        RegisterMask temporaries = code.temporaries;
        for (const ValueRep& rep : parameters) {
            if (rep.isReg())
                temporaries &= ~maskOf(rep.reg());
        }
        sum = takeTemporaryOfTestPatch(temporaries);
        word = takeTemporaryOfTestPatch(temporaries);
    } else {
        sum = parameters.gpScratch(0);
        word = parameters.gpScratch(1);
    }

    emitPoisonOfTestPatch(jit, code.earlyClobbers, word);
    jit.move(CCallHelpers::TrustedImm32(0), sum);
    addOperandsOfTestPatch(jit, parameters, false, sum, word);

    if (code.kind == TestPatchKind::SumThroughCall) {
        AOT_TEST_CHECK(parameters.frameIsSetUp());
        jit.move(sum, GPRInfo::argumentGPR0);
        jit.move(CCallHelpers::TrustedImm64(code.callee), word);
        jit.call(word, OperationPtrTag);
        sum = GPRInfo::returnValueGPR;
    }

    emitPoisonOfTestPatch(jit, (code.lateClobbers | scratch | result) & ~maskOf(Reg(sum)), word);
    addOperandsOfTestPatch(jit, parameters, true, sum, word);

    CCallHelpers::Jump toLandingPad;
    if (code.kind == TestPatchKind::SumOrThrow)
        toLandingPad = jit.branchTest64(CCallHelpers::NonZero, sum, CCallHelpers::TrustedImm32(1));

    if (hasResult)
        moveSumToResultOfTestPatch(jit, sum, parameters[0], patch->type());
    emitPoisonOfTestPatch(jit, (maskOf(Reg(sum)) | maskOf(Reg(word))) & ~result, word);

    if (code.kind != TestPatchKind::SumOrThrow)
        return;

    AOT_TEST_CHECK(parameters.successorLabels().size() == 2);
    MacroAssembler::Label* continuation = parameters.successorLabels()[0];
    MacroAssembler::Label* landingPad = parameters.successorLabels()[1];
    CCallHelpers::Jump toContinuation;
    if (!parameters.fallsThroughToSuccessor(0))
        toContinuation = jit.jump();
    parameters.addLatePath([toLandingPad, toContinuation, continuation, landingPad, lost = code.lostOnWayToLandingPad, holder = word](CCallHelpers& jit) {
        toLandingPad.link(&jit);
        emitPoisonOfTestPatch(jit, lost, holder);
        jit.jump().linkTo(*landingPad, &jit);
        if (toContinuation.isSet())
            toContinuation.linkTo(*continuation, &jit);
    });
}

PatchOperation* appendTestPatch(TestProcedure& test, const TestPatch& testPatch)
{
    Procedure& procedure = test.procedure();
    Output& out = test.out();
    RegisterFile file = registerFileFor(procedure.traits());
    RegisterMask poisonable = registersTestPatchMayPoison(file);
    bool endsBlock = testPatch.kind == TestPatchKind::SumOrThrow;
    AOT_TEST_CHECK(!testPatch.result.isTuple());
    AOT_TEST_CHECK(!!testPatch.normalSuccessor == endsBlock && !!testPatch.landingPad == endsBlock);

    bool namesOutgoingWords = false;
    for (const ConstrainedValue& operand : testPatch.operands) {
        if (!operand.rep().isStackArgument())
            continue;
        AOT_TEST_CHECK(operand.rep().offsetFromSP() >= 0);
        namesOutgoingWords = true;
        procedure.requestCallArgAreaSizeInBytes(static_cast<unsigned>(operand.rep().offsetFromSP()) + static_cast<unsigned>(sizeof(uint64_t)));
    }

    TestPatchCode code;
    code.kind = testPatch.kind;
    code.earlyClobbers = testPatch.earlyClobbers & poisonable;
    code.lateClobbers = testPatch.lateClobbers & poisonable;

    PatchOperation* patch = out.patch(testPatch.result);
    for (const ConstrainedValue& operand : testPatch.operands)
        patch->append(operand);
    patch->clobberEarly(testPatch.earlyClobbers);
    patch->clobberLate(testPatch.lateClobbers);
    patch->numGPScratchRegisters = testPatch.numberOfGPScratchRegisters;
    patch->numFPScratchRegisters = testPatch.numberOfFPScratchRegisters;

    switch (testPatch.kind) {
    case TestPatchKind::Sum:
        patch->effects = Effects::none();
        patch->effects.writesLocalState = true;
        patch->setRunsWithoutFrame(!namesOutgoingWords);
        patch->numGPScratchRegisters = std::max(patch->numGPScratchRegisters, numberOfWorkingRegistersOfTestPatch);
        break;
    case TestPatchKind::SumThroughCall: {
        RegisterMask writtenByCallee = maskOf(registersClobberedByOperations());
        patch->effects = Effects::forCall();
        patch->setRunsWithoutFrame(false);
        patch->clobberLate(writtenByCallee);
        if (testPatch.result != Void)
            patch->resultConstraints = { ValueRep::reg(testPatch.result.isFloat() ? Reg(FPRInfo::returnValueFPR) : Reg(GPRInfo::returnValueGPR)) };
        code.lateClobbers |= writtenByCallee & poisonable;
        code.temporaries = writtenByCallee & file.allocatableIn(Bank::GP) & ~(maskOf(Reg(GPRInfo::argumentGPR0)) | maskOf(Reg(GPRInfo::returnValueGPR)));
        code.callee = entryOfCallOfTestPatch();
        break;
    }
    case TestPatchKind::SumOrThrow:
        AOT_TEST_CHECK(procedure.traits().hasHandlers);
        patch->effects = Effects::forCall();
        patch->setRunsWithoutFrame(false);
        patch->numGPScratchRegisters = std::max(patch->numGPScratchRegisters, numberOfWorkingRegistersOfTestPatch);
        code.lostOnWayToLandingPad = poisonable & ~file.calleeSaved;
        out.appendSuccessor(testPatch.normalSuccessor);
        out.appendSuccessor(testPatch.landingPad);
        patch->clobberLateOnSuccessor(1, registerSetOf(code.lostOnWayToLandingPad));
        patch->effects.terminal = true;
        break;
    }

    patch->setGenerator([code](CCallHelpers& jit, const PatchParameters& parameters) {
        generateTestPatch(jit, parameters, code);
    });
    return patch;
}

static Operation* seedOfTestValues(Output& out, Operation* seed, Type type)
{
    if (seed->type() == type)
        return seed;
    switch (type.kind()) {
    case Int32:
        AOT_TEST_CHECK(seed->type() == Int64);
        return out.castToInt32(seed);
    case Int64:
        AOT_TEST_CHECK(seed->type() == Int32);
        return out.zeroExtPtr(seed);
    case Float:
        return out.doubleToFloat(seedOfTestValues(out, seed, Double));
    case Double:
        return seed->type() == Float ? out.floatToDouble(seed) : out.intToDouble(seed);
    case Void:
    case Tuple:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Operation* addendOfTestValue(Output& out, Type type, unsigned index)
{
    switch (type.kind()) {
    case Int32:
    case Int64:
        return out.constantOfType(type, index + 1);
    case Float:
        return out.constFloat(static_cast<float>(index) + 1.5f);
    case Double:
        return out.constDouble(index + 1.5);
    case Void:
    case Tuple:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Vector<Operation*> defineValues(TestProcedure& test, Operation* seed, unsigned count, Type type)
{
    Output& out = test.out();
    Operation* start = seedOfTestValues(out, seed, type);
    Vector<Operation*> values;
    values.reserveInitialCapacity(count);
    for (unsigned index = 0; index < count; ++index)
        values.append(out.opaque(out.add(start, addendOfTestValue(out, type, index))));
    return values;
}

Operation* sumOf(TestProcedure& test, std::span<Operation* const> values)
{
    Vector<ConstrainedValue> operands;
    operands.reserveInitialCapacity(values.size());
    for (Operation* value : values)
        operands.append(ConstrainedValue(value, ValueRep::WarmAny));
    TestPatch sum;
    sum.operands = operands.span();
    return appendTestPatch(test, sum);
}

} } } // namespace JSC::AOT::Machine

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
