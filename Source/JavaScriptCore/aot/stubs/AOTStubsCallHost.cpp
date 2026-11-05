/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsCallHost.h"

#if ENABLE(AOT)

#include "AOTConvention.h"
#include "AOTStubHelpers.h"
#include "AOTStubsBoxedCalls.h"
#include "AOTStubsBoxedCallsOfLanguage.h"
#include "AOTStubsCalls.h"
#include "AOTStubsExceptions.h"
#include "FunctionRareData.h"
#include "InternalFunction.h"
#include "JSFunction.h"
#include "NativeExecutable.h"

namespace JSC { namespace AOT {

StubSignature signatureOfCallHostFunction(StubKey) { return callConventionSignature({ }); }
StubSignature signatureOfCallHostFunctionWithoutBoxedCall(StubKey) { return callConventionSignature({ }); }
StubSignature signatureOfCallInternalFunction(StubKey) { return callConventionSignature({ }); }
StubSignature signatureOfCallHostFunctionWithCount(StubKey) { return callConventionSignatureWithCount(); }

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

StubSignature signatureOfCallDirectFunction(StubKey key)
{
    StubSignature signature;
    signature.result = StubResult::GPR;
    signature.firstResult = GPRInfo::returnValueGPR;
    if (static_cast<DirectFunctionCall>(key.form()) == DirectFunctionCall::LeafReturningNumber) {
        signature.result = StubResult::FPR;
        signature.firstResult = FPRInfo::returnValueFPR;
    }
    signature.immediate = gprOf(stubImmediateRegister);
    return signature;
}

static constexpr GPRReg directThisGPR = operationArgumentGPR(directThisArgumentIndex);
static constexpr GPRReg directCalleeGPR = operationArgumentGPR(directCalleeArgumentIndex);

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
    if (form == DirectFunctionCall::Leaf || form == DirectFunctionCall::LeafReturningNumber) {
        jit.farJump(function, HostFunctionPtrTag);
        return;
    }

    const GPRReg vm = jit.clobberRegister(stubImmediateRegister);
    if (form == DirectFunctionCall::WithFrame)
        jit.emitFunctionPrologue();
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(vm)), vm);
    GPRReg topCallFrame = form == DirectFunctionCall::WithHostFrame ? CCallHelpers::stackPointerRegister : GPRInfo::callFrameRegister;
#if CPU(ARM64)
    if (form == DirectFunctionCall::WithHostFrame) {
        static_assert(noOverlap(gprOf(StubRegister::T10), gprOf(StubRegister::Operation), gprOf(stubImmediateRegister), operationArgumentGPR(0), operationArgumentGPR(1), operationArgumentGPR(2), operationArgumentGPR(3), operationArgumentGPR(4), operationArgumentGPR(5)));
        topCallFrame = jit.allocateScratch(StubRegister::T10);
        jit.move(CCallHelpers::stackPointerRegister, topCallFrame);
    }
#endif
    jit.storePtr(topCallFrame, Address(vm, VM::offsetOfTopCallFrame()));
    jit.call(function, HostFunctionPtrTag);
    jit.emitFunctionEpilogue();
    Jump exception = jit.branchTestPtr(CCallHelpers::Zero, GPRInfo::returnValueGPR);
    jit.pollBeforeReturningFromNativeCode();
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
        emitBoxedCallDispatch(jit, registers, count, hasNoBoxedCall);
        jit.bindEntry(callHostFunctionWithoutBoxedCall(count));
    }
    emitLoadExecutable(jit, executable);
    hasNoBoxedCall.link(&jit);
    jit.emitFunctionPrologue();
    emitHostCallFrame(jit, { calleeGPR, thisGPR, count, std::nullopt });
    callNativeFunctionAndReturn(jit, CodeSpecializationKind::CodeForCall, registers.word, Address(calleeGPR, JSCallee::offsetOfScopeChain()), nativeFunction);
}

void generateCallHostFunctionWithCount(StubAssembler& jit)
{
    const CallRegisters registers = CallRegisters::allocate(jit);
    const GPRReg count = jit.clobberOperand(0);
    const GPRReg executable = registers.scratch;
    const Address nativeFunction(executable, NativeExecutable::offsetOfNativeFunctionFor(CodeSpecializationKind::CodeForCall));
    JumpList doesNotFitOnStack;

    jit.emitFunctionPrologue();
    emitHostCallFrameOfCallWithCount(jit, calleeGPR, thisGPR, count, doesNotFitOnStack);
    emitLoadExecutable(jit, executable);
    callNativeFunctionAndReturn(jit, CodeSpecializationKind::CodeForCall, registers.word, Address(calleeGPR, JSCallee::offsetOfScopeChain()), nativeFunction);

    doesNotFitOnStack.link(&jit);
    jit.emitFunctionEpilogue();
    emitThrowStackOverflow(jit);
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
    if (kind == CodeSpecializationKind::CodeForCall)
        emitCallOfBooleanConstructor(jit, scratch);

    jit.emitFunctionPrologue();
    emitHostCallFrame(jit, { calleeGPR, thisGPR, jit.key().count(), std::nullopt });
    callNativeFunctionAndReturn(jit, kind, scratch, Address(calleeGPR, InternalFunction::offsetOfGlobalObject()), Address(calleeGPR, InternalFunction::offsetOfNativeFunctionFor(kind)));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
