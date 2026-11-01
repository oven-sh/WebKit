/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsOperations.h"

#if ENABLE(AOT)

#include "AOTConvention.h"
#include "AOTInstance.h"
#include "AOTState.h"
#include "AOTStubCatalog.h"
#include "AOTStubHelpers.h"
#include "AOTStubRegisters.h"
#include "AOTStubsExceptions.h"
#include "Exception.h"
#include <initializer_list>
#include <iterator>

namespace JSC { namespace AOT {

static constexpr OperationCall operationCallOf(const OperationCallShape& shape)
{
    return { .returns = shape.returns, .firstArgument = shape.firstArgument, .saves = shape.saves, .checksException = shape.checksException, .pollsBeforeReturning = shape.pollsBeforeReturning };
}

static constexpr bool savesAtLeast(OperationCall::Saves saves, OperationCall::Saves requested)
{
    return saves == requested || saves == OperationCall::Saves::CallerSavedRegisters;
}

static consteval bool factoriesRoundTrip()
{
    for (uint8_t form = 0; form < std::size(operationCallShapes); ++form) {
        const OperationCallShape& shape = operationCallShapes[form];
        if ((shape.checksException ? callOperation(shape.returns, shape.firstArgument, shape.pollsBeforeReturning) : callOperationThatMayFailToAllocate(shape.returns)).form() != form)
            return false;
    }
    for (uint8_t form = 0; form < std::size(preservingCallShapes); ++form) {
        if (callOperationPreservingRegisters(operationCallOf(preservingCallShapes[form])).form() != form)
            return false;
    }
    return true;
}

static consteval bool hasExpectedShape(const OperationCallShape& requested)
{
    std::optional<StubKey> key = tryCallOperationPreservingRegisters(operationCallOf(requested));
    bool isRejected = (requested.firstArgument == OperationCall::FirstArgument::None && requested.checksException)
        || (requested.returns == OperationCall::Returns::Double && requested.saves == OperationCall::Saves::CallerSavedGPRs)
        || requested.returns == OperationCall::Returns::Pair;
    if (isRejected || !key)
        return isRejected && !key;
    const OperationCallShape& shape = preservingCallShapes[key->form()];
    return shape.firstArgument == requested.firstArgument
        && (shape.returns == requested.returns || requested.returns == OperationCall::Returns::Void)
        && savesAtLeast(shape.saves, requested.saves)
        && (shape.checksException || !requested.checksException);
}

static consteval bool shapesCoverPreservingCalls()
{
    for (auto returns : { OperationCall::Returns::Value, OperationCall::Returns::Void, OperationCall::Returns::Double, OperationCall::Returns::Pair }) {
        for (auto firstArgument : { OperationCall::FirstArgument::Instance, OperationCall::FirstArgument::None }) {
            for (auto saves : { OperationCall::Saves::CallerSavedGPRs, OperationCall::Saves::CallerSavedRegisters }) {
                for (bool checksException : { false, true }) {
                    if (!hasExpectedShape({ returns, firstArgument, saves, checksException }))
                        return false;
                }
            }
        }
    }
    return true;
}

static_assert(factoriesRoundTrip());
static_assert(shapesCoverPreservingCalls());
static_assert(!tryCallOperationPreservingRegisters({ .saves = OperationCall::Saves::CallerSavedRegisters, .fourthArgumentIn = GPRInfo::returnValueGPR }));
static_assert(!tryCallOperationPreservingRegisters({ .saves = OperationCall::Saves::CallerSavedRegisters, .valueDestination = GPRInfo::returnValueGPR }));
static_assert(!tryCallOperationPreservingRegisters({ }));
static_assert(callOperationFor(Entry::operationAOTAllocateSlow, OperationCall::Returns::Value, OperationCall::FirstArgument::Instance) == callOperationThatMayFailToAllocate(OperationCall::Returns::Value));
static_assert(callOperationFor(Entry::operationAOTBarrierSlow, OperationCall::Returns::Void, OperationCall::FirstArgument::Instance).family() == StubFamily::TailCallOperation);
static_assert(callOperationFor(Entry::operationAOTThrow, OperationCall::Returns::Void, OperationCall::FirstArgument::Instance).family() == StubFamily::CallOperation);

RegisterSet registersClobberedByPreservingCall(const StubCatalog& stubs, OperationCall call)
{
    RegisterSet clobbered;
    for (OperationCall::Saves saves : { OperationCall::Saves::CallerSavedRegisters, OperationCall::Saves::CallerSavedGPRs }) {
        call.saves = saves;
        if (std::optional<StubKey> key = tryCallOperationPreservingRegisters(call))
            clobbered.merge(stubs.summaryOf(*key).clobbered);
    }
    return clobbered;
}

static StubSignature operationCallSignature(OperationCall::Returns returns, GPRReg immediate)
{
    StubSignature signature;
    signature.immediate = immediate;
    switch (returns) {
    case OperationCall::Returns::Value:
        signature.result = StubResult::GPR;
        signature.firstResult = GPRInfo::returnValueGPR;
        break;
    case OperationCall::Returns::Void:
        break;
    case OperationCall::Returns::Double:
        signature.result = StubResult::FPR;
        signature.firstResult = FPRInfo::returnValueFPR;
        break;
    case OperationCall::Returns::Pair:
        signature.result = StubResult::TwoGPRs;
        signature.firstResult = GPRInfo::returnValueGPR;
        signature.secondResult = GPRInfo::returnValueGPR2;
        break;
    }
    return signature;
}

static std::optional<OperationCallShape> tryGetShape(StubKey key)
{
    if (key.family() == StubFamily::CallOperation)
        return operationCallShapes[key.form()];
    if (key.family() == StubFamily::CallOperationPreservingRegisters)
        return preservingCallShapes[key.form()];
    return std::nullopt;
}

#if CPU(ARM64)
static void emitFramelessCallerFrame(StubAssembler& jit)
{
    jit.pushPair(GPRInfo::callFrameRegister, faultCallerLinkGPR);
    jit.move(CCallHelpers::stackPointerRegister, GPRInfo::callFrameRegister);
}
#endif

StubSignature signatureOfCallOperation(StubKey key)
{
    return operationCallSignature(operationCallShapes[key.form()].returns, gprOf(stubImmediateRegister));
}

void generateCallOperation(StubAssembler& jit)
{
    jit.callOperationAndReturn(jit.immediate(), operationCallOf(operationCallShapes[jit.key().form()]));
}

StubSignature signatureOfTailCallOperation(StubKey)
{
    StubSignature signature;
    signature.immediate = gprOf(stubImmediateRegister);
    return signature;
}

void generateTailCallOperation(StubAssembler& jit)
{
    jit.tailCallOperation(jit.immediate(), static_cast<OperationCall::FirstArgument>(jit.key().form()));
}

StubSignature signatureOfCallOperationPreservingRegisters(StubKey key)
{
    StubSignature signature = operationCallSignature(preservingCallShapes[key.form()].returns, preservingCallImmediateGPR);
    if (signature.result == StubResult::GPR)
        signature.firstResult = resultGPROfPreservingCall();
    return signature;
}

void generateCallOperationPreservingRegisters(StubAssembler& jit)
{
    OperationCall call = operationCallOf(preservingCallShapes[jit.key().form()]);
    if (call.returns == OperationCall::Returns::Value && resultGPROfPreservingCall() != GPRInfo::returnValueGPR)
        call.valueDestination = resultGPROfPreservingCall();
    if (call.firstArgument == OperationCall::FirstArgument::Instance) {
        std::span<const GPRReg> registers = registersThatPreservingCallsTakeArgumentsIn();
        call.secondArgumentIn = registers[0];
        call.thirdArgumentIn = registers[1];
        if (registers.size() > 2) {
            call.fourthArgumentIn = registers[2];
            call.fifthArgumentIn = registers[3];
        }
    }
    jit.callOperationAndReturn(jit.immediate(), call);
}

static_assert(MacroAssembler::numberOfRegisters() <= ThrowSiteWord::numberOfRegistersPerBank && MacroAssembler::numberOfFPRegisters() <= ThrowSiteWord::numberOfRegistersPerBank);

static void storeRegisterBanks(StubAssembler& jit)
{
    constexpr GPRReg sp = CCallHelpers::stackPointerRegister;
    constexpr unsigned numberOfRegistersPerBank = ThrowSiteWord::numberOfRegistersPerBank;
    jit.subPtr(TrustedImm32(2 * numberOfRegistersPerBank * 8), sp);
#if CPU(ARM64)
    for (unsigned i = 0; i < 30; i += 2) {
        if (i == 18)
            jit.store64(ARM64Registers::x19, Address(sp, 19 * 8));
        else
            jit.storePair64(static_cast<GPRReg>(ARM64Registers::x0 + i), static_cast<GPRReg>(ARM64Registers::x0 + i + 1), sp, TrustedImm32(i * 8));
    }
    for (unsigned i = 0; i < 32; i += 2)
        jit.storePairDouble(static_cast<FPRReg>(ARM64Registers::q0 + i), static_cast<FPRReg>(ARM64Registers::q0 + i + 1), Address(sp, (numberOfRegistersPerBank + i) * 8));
#else
    for (unsigned i = 0; i < 16; ++i)
        jit.store64(static_cast<GPRReg>(X86Registers::eax + i), Address(sp, i * 8));
    for (unsigned i = 0; i < 16; ++i)
        jit.storeDouble(static_cast<FPRReg>(X86Registers::xmm0 + i), Address(sp, (numberOfRegistersPerBank + i) * 8));
#endif
}

static void loadRegisterBanks(StubAssembler& jit)
{
    constexpr GPRReg sp = CCallHelpers::stackPointerRegister;
    constexpr unsigned numberOfRegistersPerBank = ThrowSiteWord::numberOfRegistersPerBank;
#if CPU(ARM64)
    for (unsigned i = 0; i < 30; i += 2) {
        if (i == 18)
            jit.load64(Address(sp, 19 * 8), ARM64Registers::x19);
        else
            jit.loadPair64(sp, TrustedImm32(i * 8), static_cast<GPRReg>(ARM64Registers::x0 + i), static_cast<GPRReg>(ARM64Registers::x0 + i + 1));
    }
    for (unsigned i = 0; i < 32; i += 2)
        jit.loadPairDouble(Address(sp, (numberOfRegistersPerBank + i) * 8), static_cast<FPRReg>(ARM64Registers::q0 + i), static_cast<FPRReg>(ARM64Registers::q0 + i + 1));
#else
    for (unsigned i = 0; i < 16; ++i) {
        GPRReg reg = static_cast<GPRReg>(X86Registers::eax + i);
        if (reg != X86Registers::esp && reg != X86Registers::ebp)
            jit.load64(Address(sp, i * 8), reg);
    }
    for (unsigned i = 0; i < 16; ++i)
        jit.loadDouble(Address(sp, (numberOfRegistersPerBank + i) * 8), static_cast<FPRReg>(X86Registers::xmm0 + i));
#endif
}

static void dumpRegistersAndThrow(StubAssembler& jit, ThrowSiteSource source)
{
    constexpr GPRReg site = operationArgumentGPR(2);
    const bool isFrameless = jit.key().callerFrame() == CallerFrame::Frameless;
    const bool isFault = source == ThrowSiteSource::FaultSite;
    RELEASE_ASSERT(!isFrameless || isFault);
#if CPU(ARM64)
    if (isFrameless)
        emitFramelessCallerFrame(jit);
    jit.emitFunctionPrologue();
    storeRegisterBanks(jit);
    jit.move(CCallHelpers::linkRegister, site);
#else
    if (isFrameless)
        jit.emitFunctionPrologue();
    if (isFault)
        jit.push(CCallHelpers::s_scratchRegister);
    jit.emitFunctionPrologue();
    storeRegisterBanks(jit);
    jit.loadPtr(Address(GPRInfo::callFrameRegister, sizeof(void*)), site);
#endif
    auto callThrowOperation = [&] {
        jit.move(CCallHelpers::stackPointerRegister, operationArgumentGPR(1));
        jit.move(instanceGPR, operationArgumentGPR(0));
        jit.move(TrustedImm32(static_cast<uint32_t>(source)), operationArgumentGPR(3));
        jit.callOperation(Entry::operationAOTThrowAtSite, jit.allocateScratch(StubRegister::T9));
        // The operation makes the Error. What throws it comes behind.
        jit.bindReturnLabel(StubFrameKind::FirstOfTwoReads);
    };
    if (!isFrameless) {
        jit.withSavedRegisters(RegisterSet::registersToSaveForCCall(RegisterSet::allScalarRegisters()), callThrowOperation);
        loadRegisterBanks(jit);
        jit.emitFunctionEpilogue();
        jit.jumpToStub(handleException(true));
        return;
    }
    callThrowOperation();
    jit.emitFunctionEpilogue();
    jit.jumpToExceptionHandler();
}

StubSignature signatureOfThrowAtSite(StubKey key)
{
    StubSignature signature;
    if (static_cast<ThrowSiteSource>(key.form()) == ThrowSiteSource::WordInRegister)
        signature.addOperand(throwWordGPR);
    return signature;
}

void generateThrowAtSite(StubAssembler& jit)
{
    dumpRegistersAndThrow(jit, static_cast<ThrowSiteSource>(jit.key().form()));
}

StubSignature signatureOfThrowAtFault(StubKey)
{
    return { };
}

void generateThrowAtFault(StubAssembler& jit)
{
    dumpRegistersAndThrow(jit, ThrowSiteSource::FaultSite);
}

static GPRReg registerOfPreludeAnswer(StubAssembler& jit)
{
    return jit.key().family() == StubFamily::CallOperationPreservingRegisters ? jit.result() : jit.reuseScratch(StubRegister::R0);
}

enum class CaughtWord : uint8_t { Value, Exception, ValueAndException };

// The word holds what compiled code threw: the value, or the Exception that a cleanup throws on.
static PreludeExit emitCatchPrelude(StubAssembler& jit, CaughtWord wanted)
{
    if (jit.key().family() != StubFamily::TailCallOperation && jit.signature().result != StubResult::GPR)
        return PreludeExit::CallsOperation;
    const GPRReg value = registerOfPreludeAnswer(jit);
    Address thrownValue(instanceGPR, AOT_INSTANCE_OFFSET(runtime.exceptions.thrownValue));
    jit.load64(thrownValue, value);
    JumpList callsOperation;
    callsOperation.append(jit.branchTest64(CCallHelpers::Zero, value));
    switch (wanted) {
    case CaughtWord::Value:
        jit.store64(TrustedImm32(0), thrownValue);
        jit.returnToCaller();
        break;
    case CaughtWord::Exception:
        callsOperation.append(jit.branchIfNotCell(value));
        callsOperation.append(jit.branchIfNotType(value, CellType));
        jit.store64(TrustedImm32(0), thrownValue);
        jit.returnToCaller();
        break;
    case CaughtWord::ValueAndException: {
        const GPRReg exception = jit.clobberRegister(GPRInfo::returnValueGPR2);
        jit.store64(TrustedImm32(0), thrownValue);
        jit.move(TrustedImm32(0), exception);
        Jump isNotCell = jit.branchIfNotCell(value);
        Jump isException = jit.branchIfType(value, CellType);
        isNotCell.link(&jit);
        jit.returnToCaller();
        isException.link(&jit);
        jit.move(value, exception);
        jit.load64(Address(exception, Exception::valueOffset()), value);
        jit.returnToCaller();
        break;
    }
    }
    callsOperation.link(&jit);
    return PreludeExit::CallsOperation;
}

PreludeExit emitCatchPrelude(StubAssembler& jit)
{
    return emitCatchPrelude(jit, CaughtWord::Value);
}

PreludeExit emitCatchExceptionPrelude(StubAssembler& jit)
{
    return emitCatchPrelude(jit, CaughtWord::Exception);
}

PreludeExit emitCatchValueAndExceptionPrelude(StubAssembler& jit)
{
    return emitCatchPrelude(jit, CaughtWord::ValueAndException);
}

PreludeExit emitAddressOfThrowPrelude(StubAssembler& jit)
{
    if (jit.key().family() != StubFamily::TailCallOperation && jit.signature().result != StubResult::GPR)
        return PreludeExit::CallsOperation;
    jit.loadCallerReturnAddress(0, registerOfPreludeAnswer(jit));
    jit.returnToCaller();
    // Like every head of an operation this one ends in the call, so that what is known of the call holds for the head.
    return PreludeExit::CallsOperation;
}

static PreludeExit emitThrowPrelude(StubAssembler& jit, bool endsCleanup)
{
    std::optional<OperationCallShape> shape = tryGetShape(jit.key());
    if (!shape || shape->returns != OperationCall::Returns::Void || shape->firstArgument != OperationCall::FirstArgument::Instance || !shape->checksException)
        return PreludeExit::CallsOperation;
    jit.store64(operationArgumentGPR(1), Address(instanceGPR, AOT_INSTANCE_OFFSET(runtime.exceptions.thrownValue)));
    if (endsCleanup) {
        const GPRReg returnAddress = jit.allocateScratch(StubRegister::T9);
        const GPRReg state = jit.allocateScratch(StubRegister::T10);
        jit.loadCallerReturnAddress(0, returnAddress);
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(state)), state);
        jit.storePtr(returnAddress, Address(state, OBJECT_OFFSETOF(State, returnAddressOfThrowAfterCleanup)));
        jit.store64(operationArgumentGPR(2), Address(state, OBJECT_OFFSETOF(State, keptByCleanup)));
    }
    jit.jumpToExceptionHandler();
    return PreludeExit::Returns;
}

PreludeExit emitThrowPrelude(StubAssembler& jit)
{
    return emitThrowPrelude(jit, false);
}

PreludeExit emitThrowAfterCleanupPrelude(StubAssembler& jit)
{
    return emitThrowPrelude(jit, true);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
