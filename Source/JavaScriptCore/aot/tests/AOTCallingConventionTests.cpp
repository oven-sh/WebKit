/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTConvention.h"
#include "AOTNativeTest.h"
#include "AOTRuntimeValidation.h"
#include "AOTSavedFrame.h"
#include "AOTStubBodyConvention.h"
#include "AOTStubCalls.h"
#include "AOTStubFrames.h"
#include "AOTStubTestSupport.h"
#include "AOTStubsCallFrames.h"
#include "AOTVector.h"
#include "B3ArgumentRegValue.h"
#include "B3BasicBlockInlines.h"
#include "B3Generate.h"
#include "B3PatchpointValue.h"
#include "B3Procedure.h"
#include "B3ValueInlines.h"
#include "LinkBuffer.h"
#include "StackAlignment.h"
#include <wtf/DataLog.h>

namespace JSC { namespace AOT {

static RegisterSet intersectionOf(RegisterSet left, const RegisterSet& right)
{
    left.filter(right);
    return left;
}

static RegisterSet differenceOf(RegisterSet left, const RegisterSet& right)
{
    left.exclude(right);
    return left;
}

static RegisterSet registersNoCodeAllocates()
{
    RegisterSet fixed = pinnedRegisters();
    fixed.merge(RegisterSet::stackRegisters());
    fixed.merge(RegisterSet::reservedHardwareRegisters());
    return fixed;
}

// What a function keeps for its caller by saving it in its frame, where unwinding finds it.
static RegisterSet registersKeptByCompiledFunctions()
{
    return differenceOf(differenceOf(RegisterSet::vmCalleeSaveRegisters(), unsavedCalleeSaves()), registersNoCodeAllocates());
}

static RegisterSet registersOf(std::span<const AdapterFrame::SavedRegister> savedRegisters)
{
    RegisterSet registers;
    for (const AdapterFrame::SavedRegister& saved : savedRegisters)
        registers.add(saved.reg, IgnoreVectors);
    return registers;
}

static void checkIsEmpty(const RegisterSet& registers, StubKey key, ASCIILiteral what)
{
    if (registers.isEmpty())
        return;
    PrintStream& out = WTF::dataFile();
    out.print("the stub "_s);
    dump(out, key);
    out.println(" "_s, what, " "_s, registers);
    AOT_TEST_CHECK(registers.isEmpty());
}

static void testEveryRegisterHasOneRole()
{
    RegisterSet all = RegisterSet::allScalarRegisters();
    RegisterSet fixed = intersectionOf(registersNoCodeAllocates(), all);

    RegisterSet clobberedByCalls = registersClobberedByCompiledCalls();
    RegisterSet kept = registersKeptByCompiledFunctions();
    AOT_TEST_CHECK(intersectionOf(clobberedByCalls, kept).isEmpty());
    AOT_TEST_CHECK(intersectionOf(clobberedByCalls, fixed).isEmpty());
    AOT_TEST_CHECK(RegisterSet(clobberedByCalls).merge(kept).merge(fixed) == all);

    RegisterSet clobberedByStubBodies = StubBodyConvention::registersClobberedByCalls();
    RegisterSet keptByStubBodies = StubBodyConvention::registersSavedByCallee();
    AOT_TEST_CHECK(intersectionOf(clobberedByStubBodies, keptByStubBodies).isEmpty());
    AOT_TEST_CHECK(intersectionOf(clobberedByStubBodies, fixed).isEmpty());
    AOT_TEST_CHECK(intersectionOf(keptByStubBodies, fixed).isEmpty());
    AOT_TEST_CHECK(RegisterSet(clobberedByStubBodies).merge(keptByStubBodies).merge(fixed) == all);

    AOT_TEST_CHECK(clobberedByCalls.subsumes(registersClobberedByOperations()));
    AOT_TEST_CHECK(registersClobberedByOperations().subsumes(registersClobberedByPreserveMostLeaves()));
    AOT_TEST_CHECK(RegisterSet(clobberedByCalls).merge(RegisterSet::macroClobberedGPRs()).subsumes(clobberedByStubBodies));
}

// No function saves such a register, so no convention may promise it to a caller.
static void testUnsavedCalleeSavesSurviveNoCall()
{
    RegisterSet unsaved = unsavedCalleeSaves();
    AOT_TEST_CHECK(RegisterSet::vmCalleeSaveRegisters().subsumes(unsaved));
    AOT_TEST_CHECK(registersClobberedByCompiledCalls().subsumes(unsaved));
    AOT_TEST_CHECK(StubBodyConvention::registersClobberedByCalls().subsumes(unsaved));
    AOT_TEST_CHECK(intersectionOf(StubBodyConvention::registersSavedByCallee(), unsaved).isEmpty());
    AOT_TEST_CHECK(intersectionOf(calleeSavesClobberedAtSuspensions(), unsaved).isEmpty());
    AOT_TEST_CHECK(intersectionOf(registersNoCodeAllocates(), unsaved).isEmpty());
    if constexpr (SavedFrame::preservedRegister != InvalidGPRReg)
        AOT_TEST_CHECK(!containsRegister(unsaved, SavedFrame::preservedRegister));
}

// A stub has no frame record, so unwinding past it restores nothing that it changed, and a stub body has these registers pinned.
static void testStubsKeepRegistersThatFunctionsSave()
{
    RegisterSet kept = registersKeptByCompiledFunctions();
    for (const StubOptions& options : stubOptionVariants()) {
        StubCatalog catalog(options, StubPolicy { });
        for (StubKey key : allStubKeys()) {
            const StubDescriptor& descriptor = descriptorOf(key.family());
            if (descriptor.linkage != StubLinkage::CalledByCode && descriptor.linkage != StubLinkage::FrontEnd)
                continue;
            if (neverReturns(key) || catalog.effectsOf(key).maySuspend)
                continue;
            checkIsEmpty(intersectionOf(catalog.summaryOf(key).clobbered, kept), key, "clobbers"_s);
        }
    }
}

static void testStubsKeepPinnedRegisters()
{
    RegisterSet pinned = pinnedRegisters();
    for (const StubOptions& options : stubOptionVariants()) {
        StubCatalog catalog(options, StubPolicy { });
        for (StubKey key : allStubKeys()) {
            const StubSummary& summary = catalog.summaryOf(key);
            if (!neverReturns(key))
                checkIsEmpty(intersectionOf(summary.clobbered, pinned), key, "clobbers"_s);
            RegisterSet written;
            for (Reg reg : { summary.signature.firstResult, summary.signature.secondResult }) {
                if (reg)
                    written.add(reg, IgnoreVectors);
            }
            if (summary.signature.immediate)
                written.add(*summary.signature.immediate, IgnoreVectors);
            checkIsEmpty(intersectionOf(written, pinned), key, "returns or takes an immediate in"_s);
        }
    }
}

// Native code expects every callee-saved register of the platform back. Compiled code gives back only what its functions save.
static void testAdaptersSaveWhatCompiledCodeDoesNotKeep()
{
    RegisterSet notKept = differenceOf(RegisterSet::vmCalleeSaveRegisters(), registersKeptByCompiledFunctions());
    notKept.exclude(RegisterSet::stackRegisters());
    AOT_TEST_CHECK(registersOf(AdapterFrame::savedRegisters).subsumes(notKept));

    // The engine's code keeps its callee-saved registers, and expects its constants in two of them.
    RegisterSet changedForEngine;
#if CPU(X86_64)
    changedForEngine = RegisterSet { GPRInfo::numberTagRegister, GPRInfo::notCellMaskRegister };
#endif
    AOT_TEST_CHECK(registersOf(EngineCalleeFrame::savedRegisters) == changedForEngine);
}

// A landing pad relies on the registers that no call clobbers. Unwinding restores the callee-saved registers of the VM and nothing else.
static void testUnwindingRestoresWhatLandingPadsKeep()
{
    RegisterSet keptAtLandingPads = differenceOf(RegisterSet::allScalarRegisters(), registersClobberedByCompiledCalls());
    keptAtLandingPads.exclude(RegisterSet::macroClobberedGPRs());
    keptAtLandingPads.exclude(RegisterSet::stackRegisters());
    keptAtLandingPads.exclude(RegisterSet::reservedHardwareRegisters());
    AOT_TEST_CHECK(RegisterSet::vmCalleeSaveRegisters().subsumes(keptAtLandingPads));
}

// A function that suspends may save only registers that the suspension stubs give back to the caller and take from whoever resumes it.
static void testSuspensionsCoverEveryRegisterThatFunctionsSave()
{
    RegisterSet handled = calleeSavesClobberedAtSuspensions();
    if constexpr (SavedFrame::preservedRegister != InvalidGPRReg)
        handled.add(SavedFrame::preservedRegister, IgnoreVectors);
    handled.merge(RegisterSet::fromIterable(pinnedFPRsInFrameSavingFunctions));
    AOT_TEST_CHECK(handled == registersKeptByCompiledFunctions());
}

#if OS(UNIX)

namespace {

enum class UseOfPinnedRegister : uint8_t { None, Read, ClobberedByPatch, OperandOfPatch };

} // anonymous namespace

static void compileWithPinnedRegister(GPRReg pinned, UseOfPinnedRegister use)
{
    B3::Policy policy;
    policy.air.registers.pinned.add(pinned, IgnoreVectors);
    B3::Procedure proc(policy);
    B3::BasicBlock* root = proc.addBlock();
    B3::Value* valueOfPinnedRegister = root->appendNew<B3::ArgumentRegValue>(proc, B3::Origin(), pinned);
    B3::Value* one = root->appendIntConstant(proc, B3::Origin(), B3::Int64, 1);
    B3::PatchpointValue* patchpoint = root->appendNew<B3::PatchpointValue>(proc, B3::Void, B3::Origin());
    switch (use) {
    case UseOfPinnedRegister::None:
        break;
    case UseOfPinnedRegister::Read:
        patchpoint->append(B3::ConstrainedValue(valueOfPinnedRegister, B3::ValueRep::SomeRegister));
        break;
    case UseOfPinnedRegister::ClobberedByPatch:
        patchpoint->clobberLate(RegisterSet { pinned });
        break;
    case UseOfPinnedRegister::OperandOfPatch:
        patchpoint->append(B3::ConstrainedValue(one, B3::ValueRep::reg(pinned)));
        break;
    }
    patchpoint->setGenerator([](CCallHelpers&, const B3::StackmapGenerationParams&) { });
    root->appendNew<B3::Value>(proc, B3::Return, B3::Origin());
    B3::prepareForGeneration(proc);
}

static void testBackEndRefusesUseOfPinnedRegister()
{
    GPRReg pinned = GPRInfo::regCS0;
    AOT_TEST_CHECK(runInChild([&] { compileWithPinnedRegister(pinned, UseOfPinnedRegister::None); }) == ChildOutcome::Finished);
    AOT_TEST_CHECK(runInChild([&] { compileWithPinnedRegister(pinned, UseOfPinnedRegister::Read); }) == ChildOutcome::Finished);
    AOT_TEST_CHECK(runInChild([&] { compileWithPinnedRegister(pinned, UseOfPinnedRegister::ClobberedByPatch); }) == ChildOutcome::Crashed);
    AOT_TEST_CHECK(runInChild([&] { compileWithPinnedRegister(pinned, UseOfPinnedRegister::OperandOfPatch); }) == ChildOutcome::Crashed);
}

#endif // OS(UNIX)

namespace {

struct ChangeDuringCall {
    Reg reg;
    bool movesStackPointer { false };
};

} // anonymous namespace

static constexpr uint32_t checkedCallee = 11;

// Runs the check of kept registers around code that stands for a call. Returns what the check would report as found, or zero.
static uint64_t runKeptRegisterCheck(const RegisterSet& clobbered, const ChangeDuringCall& change)
{
    Vector<Reg> savedRegisters;
    RegisterSet::vmCalleeSaveRegisters().forEach([&](Reg reg) {
        savedRegisters.append(reg);
    });
    auto slotOf = [](size_t index) {
        return CCallHelpers::Address(GPRInfo::callFrameRegister, -static_cast<int32_t>((index + 1) * sizeof(CPURegister)));
    };
    size_t bytesOfSaves = savedRegisters.size() * sizeof(CPURegister);
    size_t frameSize = WTF::roundUpToMultipleOf<stackAlignmentBytes()>(bytesOfSaves + KeptRegisterCheck::storageSize);
    KeptRegisterCheck check { clobbered, CCallHelpers::Address(GPRInfo::callFrameRegister, -static_cast<int32_t>(frameSize)), Typed::FunctionID(7u), 9, true };

    CCallHelpers jit;
    jit.emitFunctionPrologue();
    jit.subPtr(CCallHelpers::TrustedImm32(static_cast<int32_t>(frameSize)), CCallHelpers::stackPointerRegister);
    for (size_t i = 0; i < savedRegisters.size(); ++i) {
        if (savedRegisters[i].isGPR())
            jit.storePtr(savedRegisters[i].gpr(), slotOf(i));
        else
            jit.storeDouble(savedRegisters[i].fpr(), slotOf(i));
    }

    emitStoresOfKeptRegisters(jit, check);
    if (change.reg) {
        if (change.reg.isGPR())
            jit.add64(CCallHelpers::TrustedImm32(1), change.reg.gpr());
        else {
            jit.moveDoubleTo64(change.reg.fpr(), GPRInfo::returnValueGPR);
            jit.add64(CCallHelpers::TrustedImm32(1), GPRInfo::returnValueGPR);
            jit.move64ToDouble(GPRInfo::returnValueGPR, change.reg.fpr());
        }
    }
    if (change.movesStackPointer)
        jit.subPtr(CCallHelpers::TrustedImm32(stackAlignmentBytes()), CCallHelpers::stackPointerRegister);
    CCallHelpers::JumpList end;
    emitChecksOfKeptRegisters(jit, check, ValidationFailureKind::CallChangedKeptRegister, Typed::FunctionID(checkedCallee), [&] {
        jit.move(operationArgumentGPR(1), GPRInfo::returnValueGPR);
        end.append(jit.jump());
    });
    jit.move(CCallHelpers::TrustedImm32(0), GPRInfo::returnValueGPR);
    end.link(&jit);

    for (size_t i = 0; i < savedRegisters.size(); ++i) {
        if (savedRegisters[i].isGPR())
            jit.loadPtr(slotOf(i), savedRegisters[i].gpr());
        else
            jit.loadDouble(slotOf(i), savedRegisters[i].fpr());
    }
    jit.emitFunctionEpilogue();
    jit.ret();

    LinkBuffer linkBuffer(jit, nullptr);
    MacroAssemblerCodeRef<JSEntryPtrTag> code = FINALIZE_CODE(linkBuffer, JSEntryPtrTag, nullptr, "AOT check of kept registers");
    return std::bit_cast<uint64_t (SYSV_ABI *)()>(untagCFunctionPtr<JSEntryPtrTag>(code.code().taggedPtr()))();
}

static uint64_t reportOf(Reg reg)
{
    return JSValue::encode(jsNumber(ChangedRegister::encode(reg.index(), checkedCallee)));
}

static void testKeptRegisterCheckNamesTheChangedRegister()
{
    for (RegisterSet clobbered : { RegisterSet(registersClobberedByCompiledCalls()).merge(RegisterSet::macroClobberedGPRs()), StubBodyConvention::registersClobberedByCalls() }) {
        AOT_TEST_CHECK(!runKeptRegisterCheck(clobbered, { }));
        AOT_TEST_CHECK(runKeptRegisterCheck(clobbered, { { }, true }) == reportOf(CCallHelpers::stackPointerRegister));

        RegisterSet kept = differenceOf(RegisterSet::allScalarRegisters(), clobbered);
        kept.exclude(RegisterSet::stackRegisters());
        kept.exclude(RegisterSet::reservedHardwareRegisters());
        kept.exclude(RegisterSet::macroClobberedGPRs());
        kept.exclude(RegisterSet::macroClobberedFPRs());
#if CPU(ARM64)
        kept.remove(ARM64Registers::x18);
#endif
        AOT_TEST_CHECK(!kept.isEmpty());
        kept.forEach([&](Reg reg) {
            AOT_TEST_CHECK(runKeptRegisterCheck(clobbered, { reg }) == reportOf(reg));
        });

        RegisterSet free = differenceOf(clobbered, RegisterSet { GPRInfo::returnValueGPR });
        free.exclude(RegisterSet::macroClobberedGPRs());
        free.forEach([&](Reg reg) {
            AOT_TEST_CHECK(!runKeptRegisterCheck(clobbered, { reg }));
        });
    }
}

template<typename Functor>
static MacroAssemblerCodeRef<JSEntryPtrTag> compileOperationOnWord(const Functor& emit)
{
    CCallHelpers jit;
    jit.emitFunctionPrologue();
    jit.move(GPRInfo::argumentGPR0, GPRInfo::argumentGPR1);
    emit(jit, GPRInfo::argumentGPR1, GPRInfo::returnValueGPR);
    jit.emitFunctionEpilogue();
    jit.ret();
    LinkBuffer linkBuffer(jit, nullptr);
    return FINALIZE_CODE(linkBuffer, JSEntryPtrTag, nullptr, "AOT operation with an immediate");
}

static uint64_t callOperationOnWord(const MacroAssemblerCodeRef<JSEntryPtrTag>& code, int64_t operand)
{
    return std::bit_cast<uint64_t (SYSV_ABI *)(int64_t)>(untagCFunctionPtr<JSEntryPtrTag>(code.code().taggedPtr()))(operand);
}

static bool holds(CCallHelpers::RelationalCondition condition, int64_t left, int64_t right)
{
    switch (condition) {
    case CCallHelpers::Equal:
        return left == right;
    case CCallHelpers::NotEqual:
        return left != right;
    case CCallHelpers::LessThan:
        return left < right;
    case CCallHelpers::GreaterThanOrEqual:
        return left >= right;
    case CCallHelpers::Above:
        return static_cast<uint64_t>(left) > static_cast<uint64_t>(right);
    case CCallHelpers::Below:
        return static_cast<uint64_t>(left) < static_cast<uint64_t>(right);
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

// ARM64 has no instruction for most of these immediates and moves them into a register first. A 32-bit move clears the high half of the register.
static void testImmediatesOf64BitOperationsAreSignExtended()
{
    constexpr int32_t least = std::numeric_limits<int32_t>::min();
    constexpr int32_t most = std::numeric_limits<int32_t>::max();
    constexpr uint64_t overflowed = 0x0123456789abcdefull;
    for (int32_t immediate : { least, least + 1, least + 8, -0x12345, -0x1001, -1, 0, 1, 0x12345, most - 1, most }) {
        int64_t wide = immediate;
        CCallHelpers::TrustedImm32 imm(immediate);
        int64_t operands[] = { wide, static_cast<int64_t>(static_cast<uint32_t>(immediate)), wide - 1, wide + 1, 0, std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max() };

        for (auto condition : { CCallHelpers::Equal, CCallHelpers::NotEqual, CCallHelpers::LessThan, CCallHelpers::GreaterThanOrEqual, CCallHelpers::Above, CCallHelpers::Below }) {
            auto branch = compileOperationOnWord([&](CCallHelpers& jit, GPRReg operand, GPRReg result) {
                CCallHelpers::Jump taken = jit.branch64(condition, operand, imm);
                jit.move(CCallHelpers::TrustedImm32(0), result);
                CCallHelpers::Jump done = jit.jump();
                taken.link(&jit);
                jit.move(CCallHelpers::TrustedImm32(1), result);
                done.link(&jit);
            });
            auto select = compileOperationOnWord([&](CCallHelpers& jit, GPRReg operand, GPRReg result) {
                jit.move(CCallHelpers::TrustedImm32(1), GPRInfo::argumentGPR2);
                jit.move(CCallHelpers::TrustedImm32(0), GPRInfo::argumentGPR3);
                jit.moveConditionally64(condition, operand, imm, GPRInfo::argumentGPR2, GPRInfo::argumentGPR3, result);
            });
            auto selectDouble = compileOperationOnWord([&](CCallHelpers& jit, GPRReg operand, GPRReg result) {
                jit.move(CCallHelpers::TrustedImm32(1), GPRInfo::argumentGPR2);
                jit.move64ToDouble(GPRInfo::argumentGPR2, FPRInfo::fpRegT0);
                jit.moveZeroToDouble(FPRInfo::fpRegT1);
                jit.moveDoubleConditionally64(condition, operand, imm, FPRInfo::fpRegT0, FPRInfo::fpRegT1, FPRInfo::fpRegT2);
                jit.moveDoubleTo64(FPRInfo::fpRegT2, result);
            });
            for (int64_t operand : operands) {
                uint64_t expected = holds(condition, operand, wide);
                AOT_TEST_CHECK(callOperationOnWord(branch, operand) == expected);
                AOT_TEST_CHECK(callOperationOnWord(select, operand) == expected);
                AOT_TEST_CHECK(callOperationOnWord(selectDouble, operand) == expected);
            }
        }

        auto emitChecked = [&](bool adds) {
            return compileOperationOnWord([&](CCallHelpers& jit, GPRReg operand, GPRReg result) {
                CCallHelpers::Jump overflow = adds ? jit.branchAdd64(CCallHelpers::Overflow, imm, operand) : jit.branchSub64(CCallHelpers::Overflow, imm, operand);
                jit.move(operand, result);
                CCallHelpers::Jump done = jit.jump();
                overflow.link(&jit);
                jit.move(CCallHelpers::TrustedImm64(overflowed), result);
                done.link(&jit);
            });
        };
        auto sum = emitChecked(true);
        auto difference = emitChecked(false);
        for (int64_t operand : operands) {
            int64_t expected;
            AOT_TEST_CHECK(callOperationOnWord(sum, operand) == (__builtin_add_overflow(operand, wide, &expected) ? overflowed : static_cast<uint64_t>(expected)));
            AOT_TEST_CHECK(callOperationOnWord(difference, operand) == (__builtin_sub_overflow(operand, wide, &expected) ? overflowed : static_cast<uint64_t>(expected)));
        }

#if CPU(ARM64)
        auto shifted = compileOperationOnWord([&](CCallHelpers& jit, GPRReg operand, GPRReg result) {
            jit.lshift64(imm, operand, result);
        });
        for (int64_t amount : { 0, 1, 31, 32, 63 })
            AOT_TEST_CHECK(callOperationOnWord(shifted, amount) == static_cast<uint64_t>(wide) << amount);
#endif
    }
}

std::span<const NativeTest> callingConventionTests()
{
    static constexpr NativeTest tests[] = {
        { "calling-convention-every-register-has-one-role"_s, testEveryRegisterHasOneRole },
        { "calling-convention-unsaved-callee-saves-survive-no-call"_s, testUnsavedCalleeSavesSurviveNoCall },
        { "calling-convention-stubs-keep-registers-that-functions-save"_s, testStubsKeepRegistersThatFunctionsSave },
        { "calling-convention-stubs-keep-pinned-registers"_s, testStubsKeepPinnedRegisters },
        { "calling-convention-adapters-save-what-compiled-code-does-not-keep"_s, testAdaptersSaveWhatCompiledCodeDoesNotKeep },
        { "calling-convention-unwinding-restores-what-landing-pads-keep"_s, testUnwindingRestoresWhatLandingPadsKeep },
        { "calling-convention-suspensions-cover-every-register-that-functions-save"_s, testSuspensionsCoverEveryRegisterThatFunctionsSave },
        { "calling-convention-kept-register-check-names-the-changed-register"_s, testKeptRegisterCheckNamesTheChangedRegister },
        { "macro-assembler/a-32-bit-immediate-of-a-64-bit-operation-is-sign-extended"_s, testImmediatesOf64BitOperationsAreSignExtended },
#if OS(UNIX)
        { "calling-convention-back-end-refuses-use-of-pinned-register"_s, testBackEndRefusesUseOfPinnedRegister },
#endif
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
