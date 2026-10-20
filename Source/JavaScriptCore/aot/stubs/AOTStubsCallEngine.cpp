/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsCalls.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTStubFrames.h"
#include "AOTStubsCallFrames.h"
#include "AOTStubsExceptions.h"
#include "BaselineJITRegisters.h"
#include "CallFrame.h"
#include "FunctionRareData.h"
#include "InternalFunction.h"
#include "JSBoundFunction.h"
#include "NativeExecutable.h"
#include "StackAlignment.h"
#include "StackManager.h"

namespace JSC { namespace AOT {

static constexpr GPRReg sp = CCallHelpers::stackPointerRegister;
static constexpr GPRReg fp = GPRInfo::callFrameRegister;

struct EngineCallRegisters {
    GPRReg count;
    GPRReg frame;
    GPRReg word;
};

StubSignature signatureOfCallBoundTarget(StubKey) { return callConventionSignatureWithCount(); }
StubSignature signatureOfCallEngineCallee(StubKey) { return callConventionSignatureWithCount(); }

static Address softStackLimit()
{
    return Address(instanceGPR, AOT_INSTANCE_OFFSET(stackMirror) + StackManager::Mirror::offsetOfSoftStackLimit());
}

static void emitAlignedListSize(StubAssembler& jit, GPRReg count, unsigned extraRegisters, GPRReg size)
{
    static_assert(stackAlignmentRegisters() == 2);
    jit.add32(TrustedImm32(extraRegisters + 1), count, size);
    jit.and32(TrustedImm32(~1), size);
    jit.lshiftPtr(TrustedImm32(3), size);
}

static void emitCallBoundThis(StubAssembler& jit, const CallRegisters& registers, GPRReg count, JumpList& cannotUnpack)
{
    jit.load64(Address(calleeGPR, JSBoundFunction::offsetOfBoundThis()), registers.word);
    cannotUnpack.append(jit.branchIfNotCell(registers.word));
    cannotUnpack.append(branchIfNotFunctionType(jit, registers.word, registers.scratch));
    jit.move(registers.word, calleeGPR);
    Jump hasNoArguments = jit.branchTest32(CCallHelpers::Zero, count);
    jit.move(argumentGPR(0), thisGPR);
    for (unsigned i = 0; i + 1 < numberOfArgumentGPRs; ++i)
        jit.move(argumentGPR(i + 1), argumentGPR(i));
    jit.sub32(TrustedImm32(1), count);
    jit.jumpToStub(callCellWithCount());
    hasNoArguments.link(&jit);
    jit.move(TrustedImm64(JSValue::ValueUndefined), thisGPR);
    jit.jumpToStub(callCellWithCount());
}

static void emitPrependBoundArguments(StubAssembler& jit, const CallRegisters& registers, GPRReg count, JumpList& cannotUnpack)
{
    static_assert(JSBoundFunction::maxEmbeddedArgs < numberOfArgumentGPRs);
    cannotUnpack.append(jit.branch32(CCallHelpers::Above, registers.scratch, TrustedImm32(JSBoundFunction::maxEmbeddedArgs)));
    jit.add32(count, registers.scratch, registers.word);
    cannotUnpack.append(jit.branch32(CCallHelpers::Above, registers.word, TrustedImm32(numberOfArgumentGPRs)));
    jit.move(registers.word, count);
    JumpList areInPlace;
    for (unsigned bound = 1; bound <= JSBoundFunction::maxEmbeddedArgs; ++bound) {
        Jump hasMore;
        if (bound < JSBoundFunction::maxEmbeddedArgs)
            hasMore = jit.branch32(CCallHelpers::NotEqual, registers.scratch, TrustedImm32(bound));
        for (unsigned i = numberOfArgumentGPRs; i-- > bound;)
            jit.move(argumentGPR(i - bound), argumentGPR(i));
        for (unsigned i = 0; i < bound; ++i)
            jit.load64(Address(calleeGPR, JSBoundFunction::offsetOfBoundArgs() + i * sizeof(WriteBarrier<Unknown>)), argumentGPR(i));
        if (bound < JSBoundFunction::maxEmbeddedArgs) {
            areInPlace.append(jit.jump());
            hasMore.link(&jit);
        }
    }
    areInPlace.link(&jit);
}

void generateCallBoundTarget(StubAssembler& jit)
{
    const CallRegisters registers = CallRegisters::allocate(jit);
    const GPRReg count = jit.clobberOperand(0);
    JumpList cannotUnpack;
    cannotUnpack.append(jit.branch32(CCallHelpers::Above, count, TrustedImm32(numberOfArgumentGPRs)));
    cannotUnpack.append(jit.branchIfNotCell(calleeGPR));
    jit.load32(Address(calleeGPR, JSCell::structureIDOffset()), registers.word);
    cannotUnpack.append(jit.branch32(CCallHelpers::NotEqual, registers.word, Address(instanceGPR, AOT_INSTANCE_OFFSET(realm.boundFunctionStructureID))));
    jit.load32(Address(calleeGPR, JSBoundFunction::offsetOfBoundArgsLength()), registers.scratch);
    Jump hasBoundArguments = jit.branchTest32(CCallHelpers::NonZero, registers.scratch);
    jit.loadPtr(Address(calleeGPR, JSBoundFunction::offsetOfTargetFunction()), registers.word);
    Jump bindsCall = jit.branchPtr(CCallHelpers::Equal, registers.word, Address(instanceGPR, AOT_INSTANCE_OFFSET(realm.functionPrototypeCall)));
    jit.load64(Address(calleeGPR, JSBoundFunction::offsetOfBoundThis()), thisGPR);
    jit.move(registers.word, calleeGPR);
    jit.jumpToStub(callCellWithCount());

    bindsCall.link(&jit);
    emitCallBoundThis(jit, registers, count, cannotUnpack);

    hasBoundArguments.link(&jit);
    emitPrependBoundArguments(jit, registers, count, cannotUnpack);
    jit.load64(Address(calleeGPR, JSBoundFunction::offsetOfBoundThis()), thisGPR);
    jit.loadPtr(Address(calleeGPR, JSBoundFunction::offsetOfTargetFunction()), calleeGPR);
    jit.jumpToStub(callCellWithCount());

    jit.linkToStub(cannotUnpack, callEngineCallee());
}

static void emitEngineCallFrame(StubAssembler& jit, const EngineCallRegisters& registers, JumpList& doesNotFitOnStack)
{
    const auto [count, frame, word] = registers;
    emitAlignedListSize(jit, count, CallFrame::headerSizeInRegisters - CallerFrameAndPC::sizeInRegisters + 1, frame);
    jit.subPtr(sp, frame, frame);
    doesNotFitOnStack.append(jit.branchPtr(CCallHelpers::Above, softStackLimit(), frame));
    doesNotFitOnStack.append(jit.branchPtr(CCallHelpers::Above, frame, fp));
    jit.move(frame, sp);
    jit.store64(calleeGPR, outgoingFrameSlot(CallFrameSlot::callee));
    jit.add32(TrustedImm32(1), count, frame);
    jit.store64(frame, outgoingFrameSlot(CallFrameSlot::argumentCountIncludingThis));
    jit.store64(thisGPR, outgoingFrameSlot(CallFrameSlot::thisArgument));
    jit.addPtr(TrustedImm32(outgoingFrameSlot(CallFrameSlot::thisArgument, sizeof(Register)).offset), sp, frame);
    Jump none = jit.branchTest32(CCallHelpers::Zero, count);
    Label next = jit.label();
    jit.sub32(TrustedImm32(1), count);
    jit.load64(BaseIndex(fp, count, CCallHelpers::TimesEight, static_cast<int32_t>(offsetOfStackWordsFromFrame)), word);
    jit.store64(word, BaseIndex(frame, count, CCallHelpers::TimesEight));
    jit.branchTest32(CCallHelpers::NonZero, count).linkTo(next, &jit);
    none.link(&jit);
}

// CallLinkInfo::ownerForSlowPath() reads these two slots of the caller's frame. The spilled registers in them are already copied.
static void emitStoreCodeOwner(StubAssembler& jit, GPRReg scratch)
{
    static_assert(sizeof(Register) == sizeOfStackWord && numberOfArgumentGPRs >= 2);
    static_assert(offsetOfStackWordFromFrame(0) == static_cast<ptrdiff_t>(CallFrameSlot::codeBlock) * sizeOfStackWord && offsetOfStackWordFromFrame(1) == static_cast<ptrdiff_t>(CallFrameSlot::callee) * sizeOfStackWord);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(globalObject)), scratch);
    jit.storePtr(scratch, CCallHelpers::addressFor(CallFrameSlot::codeBlock));
    jit.store64(TrustedImm32(0), CCallHelpers::addressFor(CallFrameSlot::callee));
}

// The engine's trampoline calls a native function where validation does not see it, so the run time calls it.
static void emitCallNativeFunctionByOperation(StubAssembler& jit, const EngineCallRegisters& registers)
{
    const GPRReg executable = registers.frame;
    JumpList isNative;
    JumpList isNotNative;
    isNotNative.append(jit.branchIfNotCell(calleeGPR));
    isNative.append(jit.branchIfType(calleeGPR, InternalFunctionType));
    isNotNative.append(jit.branchIfNotType(calleeGPR, JSFunctionType));
    jit.loadPtr(Address(calleeGPR, JSFunction::offsetOfExecutableOrRareData()), executable);
    Jump hasExecutable = jit.branchTestPtr(CCallHelpers::Zero, executable, TrustedImm32(JSFunction::rareDataTag));
    jit.loadPtr(Address(executable, FunctionRareData::offsetOfExecutable() - JSFunction::rareDataTag), executable);
    hasExecutable.link(&jit);
    isNotNative.append(jit.branchIfNotType(executable, NativeExecutableType));

    isNative.link(&jit);
    jit.shuffleRegisters<GPRReg, 3>({ calleeGPR, thisGPR, registers.count }, { operationArgumentGPR(1), operationArgumentGPR(2), operationArgumentGPR(3) });
    jit.addPtr(TrustedImm32(static_cast<int32_t>(offsetOfStackWordsFromFrame)), fp, operationArgumentGPR(4));
    jit.move(instanceGPR, operationArgumentGPR(0));
    jit.callOperation(Entry::operationAOTCallNativeFunction, jit.reuseScratch(StubRegister::Operation));
    jit.emitFunctionEpilogue();
    emitDropSpilledArgumentRegisters(jit);
    Jump threw = jit.branchTestPtr(CCallHelpers::NonZero, GPRInfo::returnValueGPR2);
    jit.returnToCaller();

    threw.link(&jit);
    jit.jumpToExceptionHandler();

    isNotNative.link(&jit);
}

void generateCallEngineCallee(StubAssembler& jit)
{
    static_assert(BaselineJITRegisters::Call::calleeGPR == gprOf(StubRegister::R0) && BaselineJITRegisters::Call::callLinkInfoGPR == gprOf(StubRegister::A2));
    const EngineCallRegisters registers { jit.clobberOperand(0), jit.allocateScratch(StubRegister::T11), jit.allocateScratch(StubRegister::T13) };
    JumpList doesNotFitOnStack;

    jit.recordClobbers(RegisterSet::macroClobberedGPRs());
    emitSpillArgumentRegisters(jit);
    jit.emitFunctionPrologue();
    if (jit.options().validates)
        emitCallNativeFunctionByOperation(jit, registers);
    if constexpr (!!EngineCalleeFrame::saveAreaSize)
        jit.subPtr(TrustedImm32(EngineCalleeFrame::saveAreaSize), sp);
    for (const AdapterFrame::SavedRegister& saved : EngineCalleeFrame::savedRegisters)
        jit.storePtr(saved.reg, Address(fp, static_cast<int32_t>(saved.offset)));
    emitEngineCallFrame(jit, registers, doesNotFitOnStack);
    emitStoreCodeOwner(jit, registers.word);
#if CPU(X86_64)
    jit.emitMaterializeTagCheckRegisters();
#endif
    jit.move(calleeGPR, jit.reuseScratch(StubRegister::R0));
    jit.loadRuntimePointer(Entry::CallLinkInfoForCall, jit.allocateScratch(StubRegister::A2));
    jit.callEngineThunk(Entry::VirtualCallTrampoline, registers.frame);
    jit.bindReturnLabel(StubFrameKind::EngineCallee);
    for (const AdapterFrame::SavedRegister& saved : EngineCalleeFrame::savedRegisters)
        jit.loadPtr(Address(fp, static_cast<int32_t>(saved.offset)), saved.reg);
    jit.emitFunctionEpilogue();
    emitDropSpilledArgumentRegisters(jit);
    jit.returnToCaller();

    doesNotFitOnStack.link(&jit);
    jit.emitFunctionEpilogue();
    emitDropSpilledArgumentRegisters(jit);
    emitThrowStackOverflow(jit);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
