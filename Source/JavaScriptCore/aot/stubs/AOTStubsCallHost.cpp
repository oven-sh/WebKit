/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsCallHost.h"

#if ENABLE(AOT)

#include "AOTBuiltinCells.h"
#include "AOTConvention.h"
#include "AOTStubHelpers.h"
#include "AOTStubsBoxedCalls.h"
#include "AOTStubsBoxedCallsOfLanguage.h"
#include "AOTStubsCalls.h"
#include "AOTStubsExceptions.h"
#include "AOTStubsPropertyKeys.h"
#include "FunctionRareData.h"
#include "InternalFunction.h"
#include "JSCustomGetterFunction.h"
#include "JSFunction.h"
#include "NativeExecutable.h"

namespace JSC { namespace AOT {

StubSignature signatureOfCallHostFunction(StubKey) { return callConventionSignature({ }); }
StubSignature signatureOfCallHostFunctionWithoutBoxedCall(StubKey) { return callConventionSignature({ }); }
StubSignature signatureOfCallHostFunctionThatMayBeUnfetched(StubKey) { return callConventionSignature({ }); }
StubSignature signatureOfCallHostFunctionCheckingResult(StubKey) { return callConventionSignature({ }); }
StubSignature signatureOfCallHostFunctionThatMayBeUnfetchedCheckingResult(StubKey) { return callConventionSignature({ }); }
StubSignature signatureOfCallInternalFunction(StubKey) { return callConventionSignature({ }); }
StubSignature signatureOfCallHostFunctionWithCount(StubKey) { return callConventionSignatureWithCount(); }
StubSignature signatureOfCallHostGetter(StubKey) { return callConventionSignature({ }); }

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

static void fetchFunctionLeftUnfetched(StubAssembler& jit, GPRReg scratch, unsigned count)
{
    Jump isFetched = jit.branch64(CCallHelpers::AboveOrEqual, calleeGPR, TrustedImm32(limitOfNumbersOfBuiltinCells));
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_PREFIX_OFFSET(builtinCells)), scratch);
    jit.load64(BaseIndex(scratch, calleeGPR, CCallHelpers::TimesEight), scratch);
    Jump isNotCreated = jit.branchTest64(CCallHelpers::Zero, scratch);
    jit.move(scratch, calleeGPR);
    isFetched.link(&jit);
    Label goesOn = jit.label();

    jit.addOutOfLinePart([=](StubAssembler& jit) {
        isNotCreated.link(&jit);
        Vector<GPRReg, 6> preserved { thisGPR };
        for (unsigned index = 0; index < count; ++index)
            preserved.append(argumentGPR(index));
        jit.emitFunctionPrologue();
        jit.preserving(preserved.span(), [&] {
            jit.move(calleeGPR, operationArgumentGPR(1));
            jit.move(instanceGPR, operationArgumentGPR(0));
            jit.callOperation(Entry::operationAOTBuiltinCell, jit.reuseScratch(StubRegister::Operation));
            jit.move(GPRInfo::returnValueGPR, calleeGPR);
        });
        jit.emitFunctionEpilogue();
        jit.jump().linkTo(goesOn, &jit);
    });
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
        jit.bindEntry(callHostFunctionThatMayBeUnfetched(count));
        fetchFunctionLeftUnfetched(jit, executable, count);
        jit.bindEntry(callHostFunctionWithoutBoxedCall(count));
    }
    emitLoadExecutable(jit, executable);
    hasNoBoxedCall.link(&jit);
    jit.emitFunctionPrologue();
    emitHostCallFrame(jit, { calleeGPR, thisGPR, count, std::nullopt });
    callNativeFunctionAndReturn(jit, CodeSpecializationKind::CodeForCall, registers.word, Address(calleeGPR, JSCallee::offsetOfScopeChain()), nativeFunction);
}

void generateCallHostFunctionThatMayBeUnfetchedCheckingResult(StubAssembler& jit)
{
    const unsigned count = jit.key().count();
    const HostResult result = static_cast<HostResult>(jit.key().form());
    const CallRegisters registers = CallRegisters::allocate(jit);
    const GPRReg executable = registers.scratch;
    fetchFunctionLeftUnfetched(jit, executable, count);
    jit.bindEntry(callHostFunctionCheckingResult(result, count));
    emitLoadExecutable(jit, executable);
    jit.emitFunctionPrologue();
    emitHostCallFrame(jit, { calleeGPR, thisGPR, count, std::nullopt });
    callNativeFunctionAndReturn(jit, CodeSpecializationKind::CodeForCall, registers.word, Address(calleeGPR, JSCallee::offsetOfScopeChain()), Address(executable, NativeExecutable::offsetOfNativeFunctionFor(CodeSpecializationKind::CodeForCall)), result);
}

void generateCallHostGetter(StubAssembler& jit)
{
    const CallRegisters registers = CallRegisters::allocate(jit);
    loadStructure(jit, calleeGPR, registers.word);
    jit.loadPtr(Address(registers.word, Structure::classInfoOffset()), registers.word);
    jit.loadRuntimePointer(Entry::CustomGetterClassInfo, registers.scratch);
    jit.linkToStub(jit.branchPtr(CCallHelpers::NotEqual, registers.word, registers.scratch), callHostFunctionWithoutBoxedCall(0));

    jit.emitFunctionPrologue();
    const GPRReg function = registers.word;
    const GPRReg realmArgument = jit.reuseScratch(StubRegister::A0);
    const GPRReg receiverArgument = jit.reuseScratch(StubRegister::A1);
    const GPRReg nameArgument = jit.reuseScratch(StubRegister::A2);
    RELEASE_ASSERT(noOverlap(calleeGPR, realmArgument, receiverArgument, nameArgument, function, registers.scratch) && noOverlap(thisGPR, realmArgument, nameArgument, function, registers.scratch));
    jit.loadPtr(Address(calleeGPR, JSCustomGetterFunction::offsetOfGetter()), function);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(vm)), registers.scratch);
    jit.storePtr(GPRInfo::callFrameRegister, Address(registers.scratch, VM::offsetOfTopCallFrame()));
    jit.move(thisGPR, receiverArgument);
    jit.loadPtr(Address(calleeGPR, JSCustomGetterFunction::offsetOfPropertyName()), nameArgument);
    jit.loadPtr(Address(calleeGPR, JSCallee::offsetOfScopeChain()), realmArgument);
    jit.call(function, CustomAccessorPtrTag);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(vm)), registers.scratch);
    Jump exception = jit.branchTestPtr(CCallHelpers::NonZero, Address(registers.scratch, VM::exceptionOffset()));
    jit.emitFunctionEpilogue();
    jit.pollBeforeReturningFromNativeCode();
    jit.returnToCaller();

    exception.link(&jit);
    jit.emitFunctionEpilogue();
    jit.jumpToExceptionHandler();
}

StubSignature signatureOfCallFunctionOfBuiltinAtSite(StubKey key) { return signatureOf(callMethodAtSite(key.count())); }

void generateCallFunctionOfBuiltinAtSite(StubAssembler& jit)
{
    const auto form = static_cast<BoxedCallForm>(jit.key().form());
    const unsigned count = jit.key().count();
    const GPRReg distance = jit.immediate();
    if (!acceptsArgumentCount(form, count)) {
        jit.jumpToStub(callMethodAtSite(count));
        return;
    }
    const ptrdiff_t words = AOT_INSTANCE_OFFSET(builtins.functionsAsBuilt) + jit.key().form() * sizeof(FunctionOfBuiltinAsBuilt);
    const Address receiver(instanceGPR, words + OBJECT_OFFSETOF(FunctionOfBuiltinAsBuilt, receiver));
    const Address function(instanceGPR, words + OBJECT_OFFSETOF(FunctionOfBuiltinAsBuilt, function));
    RELEASE_ASSERT(noOverlap(thisGPR, calleeGPR, distance, instanceGPR));

    Label asks = jit.label();
    Jump isOtherReceiver = jit.branch64(CCallHelpers::NotEqual, receiver, thisGPR);
    jit.load64(function, calleeGPR);
    jit.jumpToStub(callBoxedBuiltin(form, count));

    isOtherReceiver.link(&jit);
    Jump isNotAskedYet = jit.branchTest64(CCallHelpers::Zero, function);
    jit.jumpToStub(callMethodAtSite(count));

    isNotAskedYet.link(&jit);
    Vector<GPRReg, 8> preserved { thisGPR, distance };
    for (unsigned index = 0; index < count; ++index)
        preserved.append(argumentGPR(index));
    jit.emitFunctionPrologue();
    jit.preserving(preserved.span(), [&] {
        jit.move(TrustedImm32(static_cast<int32_t>(jit.key().form())), operationArgumentGPR(1));
        jit.move(instanceGPR, operationArgumentGPR(0));
        jit.callOperation(Entry::operationAOTFunctionOfBuiltinAsBuilt, jit.reuseScratch(StubRegister::Operation));
    });
    jit.emitFunctionEpilogue();
    jit.jump().linkTo(asks, &jit);
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
    if (kind == CodeSpecializationKind::CodeForCall) {
        emitCallOfBooleanConstructor(jit, scratch);
        emitCallOfNumberOrStringConstructor(jit, scratch);
    }

    jit.emitFunctionPrologue();
    emitHostCallFrame(jit, { calleeGPR, thisGPR, jit.key().count(), std::nullopt });
    callNativeFunctionAndReturn(jit, kind, scratch, Address(calleeGPR, InternalFunction::offsetOfGlobalObject()), Address(calleeGPR, InternalFunction::offsetOfNativeFunctionFor(kind)));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
