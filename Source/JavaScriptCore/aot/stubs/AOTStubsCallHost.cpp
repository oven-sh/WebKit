/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsCallHost.h"

#if ENABLE(AOT)

#include "AOTConvention.h"
#include "AOTStubHelpers.h"
#include "AOTStubsArrays.h"
#include "AOTStubsBoxedCalls.h"
#include "AOTStubsCalls.h"
#include "FunctionRareData.h"
#include "InternalFunction.h"
#include "JSFunction.h"
#include "NativeExecutable.h"
#include "ProxyObject.h"

namespace JSC { namespace AOT {

StubSignature signatureOfCallHostFunction(StubKey) { return callConventionSignature({ }); }
StubSignature signatureOfCallHostFunctionWithoutBoxedCall(StubKey) { return callConventionSignature({ }); }
StubSignature signatureOfCallInternalFunction(StubKey) { return callConventionSignature({ }); }
StubSignature signatureOfCallProxy(StubKey) { return callConventionSignature({ }); }

static void emitLoadExecutable(StubAssembler& jit, GPRReg executable)
{
    jit.loadPtr(Address(calleeGPR, JSFunction::offsetOfExecutableOrRareData()), executable);
    Jump hasRareData = jit.branchTestPtr(CCallHelpers::NonZero, executable, TrustedImm32(JSFunction::rareDataTag));
    Label hasExecutable = jit.label();
    jit.addOutOfLinePart([hasRareData, hasExecutable, executable](StubAssembler& jit) {
        hasRareData.link(&jit);
        jit.loadPtr(Address(executable, FunctionRareData::offsetOfExecutable() - JSFunction::rareDataTag), executable);
        jit.jump().linkTo(hasExecutable, &jit);
    });
}

// The caller has put the operands where the native function takes its arguments.
StubSignature signatureOfCallDirectFunction(StubKey)
{
    StubSignature signature;
    signature.result = StubResult::GPR;
    signature.firstResult = GPRInfo::returnValueGPR;
    signature.immediate = gprOf(stubImmediateRegister);
    return signature;
}

static constexpr GPRReg directThisGPR = operationArgumentGPR(directThisArgumentIndex);
static constexpr GPRReg directCalleeGPR = operationArgumentGPR(directCalleeArgumentIndex);

// The native function is no operation: it stores no frame, and it reports an exception by the empty value.
void generateCallDirectFunction(StubAssembler& jit)
{
    const DirectFunctionCall form = static_cast<DirectFunctionCall>(jit.key().form());
    static_assert(noOverlap(gprOf(StubRegister::Operation), gprOf(stubImmediateRegister), operationArgumentGPR(0), operationArgumentGPR(1), operationArgumentGPR(2), operationArgumentGPR(3), operationArgumentGPR(4), operationArgumentGPR(5)));
    static_assert(noOverlap(gprOf(StubRegister::T11), gprOf(stubImmediateRegister), operationArgumentGPR(0), operationArgumentGPR(1), operationArgumentGPR(2), operationArgumentGPR(3), operationArgumentGPR(4)));

    if (form == DirectFunctionCall::WithHostFrame) {
        jit.emitFunctionPrologue();
        emitHostCallFrame(jit, { directCalleeGPR, directThisGPR, 0u, std::nullopt });
    }

    const GPRReg function = jit.reuseScratch(StubRegister::Operation);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(builtinBindings.directFunctions)), function);
    jit.loadPtr(CCallHelpers::BaseIndex(function, jit.immediate(), CCallHelpers::TimesOne), function);
    if (form == DirectFunctionCall::Leaf) {
        jit.farJump(function, HostFunctionPtrTag);
        return;
    }

    const GPRReg vm = jit.clobberRegister(stubImmediateRegister);
    if (form == DirectFunctionCall::WithFrame)
        jit.emitFunctionPrologue();
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(vm)), vm);
    GPRReg topCallFrame = form == DirectFunctionCall::WithHostFrame ? CCallHelpers::stackPointerRegister : GPRInfo::callFrameRegister;
#if CPU(ARM64)
    // In a store the number of sp names the zero register.
    if (form == DirectFunctionCall::WithHostFrame) {
        static_assert(noOverlap(gprOf(StubRegister::T10), gprOf(StubRegister::Operation), gprOf(stubImmediateRegister), operationArgumentGPR(0), operationArgumentGPR(1), operationArgumentGPR(2), operationArgumentGPR(3), operationArgumentGPR(4), operationArgumentGPR(5)));
        topCallFrame = jit.allocateScratch(StubRegister::T10);
        jit.move(CCallHelpers::stackPointerRegister, topCallFrame);
    }
#endif
    jit.storePtr(topCallFrame, Address(vm, VM::offsetOfTopCallFrame()));
    jit.call(function, HostFunctionPtrTag);
    if (jit.options().validates) {
        RELEASE_ASSERT(vm != GPRInfo::returnValueGPR);
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(vm)), vm);
        Jump isEmpty = jit.branchTestPtr(CCallHelpers::Zero, GPRInfo::returnValueGPR);
        Jump answered = jit.branchTestPtr(CCallHelpers::Zero, Address(vm, VM::exceptionOffset()));
        jit.breakpoint();
        isEmpty.link(&jit);
        Jump threw = jit.branchTestPtr(CCallHelpers::NonZero, Address(vm, VM::exceptionOffset()));
        jit.breakpoint();
        answered.link(&jit);
        threw.link(&jit);
    }
    jit.emitFunctionEpilogue();
    Jump exception = jit.branchTestPtr(CCallHelpers::Zero, GPRInfo::returnValueGPR);
    jit.returnToCaller();

    exception.link(&jit);
    jit.jumpToExceptionHandler();
}

void generateCallHostFunction(StubAssembler& jit)
{
    const unsigned count = jit.key().count();
    const CallRegisters registers = CallRegisters::allocate(jit);
    const GPRReg executable = registers.scratch;
    const Address nativeFunction(executable, NativeExecutable::offsetOfNativeFunctionFor(CodeSpecializationKind::CodeForCall));

    JumpList hasNoBoxedCall;
    if (exists(callHostFunctionWithoutBoxedCall(count))) {
        emitLoadExecutable(jit, executable);
        // Under validation every call reaches the function, where it is compared with the entry of the built-in.
        if (!jit.options().validates)
            emitBoxedCallDispatch(jit, registers, count, hasNoBoxedCall);
        jit.bindEntry(callHostFunctionWithoutBoxedCall(count));
    }
    emitLoadExecutable(jit, executable);
    hasNoBoxedCall.link(&jit);
    jit.emitFunctionPrologue();
    emitHostCallFrame(jit, { calleeGPR, thisGPR, count, std::nullopt });
    callNativeFunctionAndReturn(jit, CodeSpecializationKind::CodeForCall, registers.word, Address(calleeGPR, JSCallee::offsetOfScopeChain()), nativeFunction);
}

static CodeSpecializationKind specializationKindOf(InternalFunctionEntry entry)
{
    switch (entry) {
    case InternalFunctionEntry::Call:
        return CodeSpecializationKind::CodeForCall;
    case InternalFunctionEntry::Construct:
        return CodeSpecializationKind::CodeForConstruct;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void generateCallInternalFunction(StubAssembler& jit)
{
    const CodeSpecializationKind kind = specializationKindOf(static_cast<InternalFunctionEntry>(jit.key().form()));
    const GPRReg scratch = jit.allocateScratch(StubRegister::T11);

    jit.emitFunctionPrologue();
    emitHostCallFrame(jit, { calleeGPR, thisGPR, jit.key().count(), std::nullopt });
    callNativeFunctionAndReturn(jit, kind, scratch, Address(calleeGPR, InternalFunction::offsetOfGlobalObject()), Address(calleeGPR, InternalFunction::offsetOfNativeFunctionFor(kind)));
}

// As ProxyObject.cpp performProxyCall(), in the frame that it would have, which a stack trace lists.
void generateCallProxy(StubAssembler& jit)
{
    static_assert(numberOfArgumentGPRs >= 3);
    constexpr GPRReg function = GPRInfo::returnValueGPR;
    constexpr GPRReg list = GPRInfo::returnValueGPR2;
    const unsigned count = jit.key().count();
    const bool makesList = count <= maxArrayElementsInRegisters;
    auto inFrame = [&](CallFrameSlot slot, unsigned index = 0) {
        return Address(fp, (static_cast<int>(slot) + static_cast<int>(index)) * static_cast<int>(sizeof(Register)));
    };
    auto loadCallFromFrame = [&] {
        jit.load64(inFrame(CallFrameSlot::thisArgument), thisGPR);
        for (unsigned i = 0; i < count; ++i)
            jit.load64(inFrame(CallFrameSlot::firstArgument, i), argumentGPR(i));
    };
    auto leaveFrames = [&] {
        jit.loadPtr(Address(fp, CallFrame::callerFrameOffset()), fp);
        jit.emitFunctionEpilogue();
    };

    jit.emitFunctionPrologue();
    emitHostCallFrame(jit, { calleeGPR, thisGPR, count, std::nullopt });
    jit.move(sp, fp);
    jit.move(sp, operationArgumentGPR(1));
    jit.move(TrustedImm32(!makesList), operationArgumentGPR(2));
    jit.move(instanceGPR, operationArgumentGPR(0));
    jit.callOperation(Entry::operationAOTPrepareProxyCall, jit.reuseScratch(StubRegister::Operation));
    jit.bindReturnLabel(StubFrameKind::InHostCallFrame);
    Jump isEngines = jit.branchTestPtr(CCallHelpers::Zero, function);
    Jump hasNoTrap = jit.branchTestPtr(CCallHelpers::Zero, list);

    if (makesList) {
        jit.subPtr(TrustedImm32(stackAlignmentBytes()), sp);
        jit.storePtr(function, Address(sp));
        for (unsigned i = 0; i < count; ++i)
            jit.load64(inFrame(CallFrameSlot::firstArgument, i), operationArgumentGPR(1 + i));
        jit.callStub(arrayLiteral(count));
        jit.bindReturnLabel(StubFrameKind::InHostCallFrame);
        jit.move(GPRInfo::returnValueGPR, argumentGPR(2));
        jit.loadPtr(Address(sp), calleeGPR);
        jit.addPtr(TrustedImm32(stackAlignmentBytes()), sp);
    } else
        jit.shuffleRegisters<GPRReg, 2>({ function, list }, { calleeGPR, argumentGPR(2) });
    jit.loadPtr(inFrame(CallFrameSlot::callee), argumentGPR(0));
    jit.load64(Address(argumentGPR(0), ProxyObject::offsetOfInternalField(static_cast<unsigned>(ProxyObject::Field::Handler))), thisGPR);
    jit.load64(Address(argumentGPR(0), ProxyObject::offsetOfInternalField(static_cast<unsigned>(ProxyObject::Field::Target))), argumentGPR(0));
    jit.load64(inFrame(CallFrameSlot::thisArgument), argumentGPR(1));
    jit.callStub(callValueCell(3));
    jit.bindReturnLabel(StubFrameKind::InHostCallFrame);
    leaveFrames();
    jit.returnToCaller();

    hasNoTrap.link(&jit);
    jit.move(function, calleeGPR);
    loadCallFromFrame();
    jit.callStub(callValueCell(count));
    jit.bindReturnLabel(StubFrameKind::InHostCallFrame);
    leaveFrames();
    jit.returnToCaller();

    isEngines.link(&jit);
    jit.loadPtr(inFrame(CallFrameSlot::callee), calleeGPR);
    loadCallFromFrame();
    leaveFrames();
    jit.move(TrustedImm32(count), countGPR);
    jit.jumpToStub(callEngineCallee());
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
