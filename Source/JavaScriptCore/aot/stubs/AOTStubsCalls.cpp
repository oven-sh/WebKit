/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsCalls.h"

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTInstance.h"
#include "AOTStubsCallAdapters.h"
#include "AOTStubsCallFrames.h"
#include "AOTStubsCallHost.h"
#include "AOTStubsClosures.h"
#include "AOTStubsOperations.h"
#include "FunctionRareData.h"
#include "JSBoundFunction.h"
#include "JSFunction.h"
#include "NativeExecutable.h"

namespace JSC { namespace AOT {

static constexpr int64_t isNotCallableMask = static_cast<int64_t>(1ULL << ClosureWord::isNotCallableBit);

StubSignature callConventionSignature(std::initializer_list<StubRegister> operands)
{
    StubSignature signature;
    signature.usesCallConvention = true;
    for (StubRegister operand : operands)
        signature.addOperand(gprOf(operand));
    signature.result = StubResult::GPR;
    signature.firstResult = GPRInfo::returnValueGPR;
    return signature;
}

StubSignature callConventionSignatureWithCount()
{
    StubSignature signature = callConventionSignature({ });
    signature.addOperand(countGPR);
    return signature;
}

StubSignature signatureOfCallValue(StubKey) { return callConventionSignature({ }); }
StubSignature signatureOfCallValueWithoutThis(StubKey) { return callConventionSignature({ }); }
StubSignature signatureOfCallValueCell(StubKey) { return callConventionSignature({ }); }
StubSignature signatureOfCallValueNotClosure(StubKey) { return callConventionSignature({ closureWordRegister }); }
StubSignature signatureOfCallValueNamed(StubKey) { return callConventionSignature({ closureWordRegister }); }
StubSignature signatureOfCallValueWithCount(StubKey) { return callConventionSignatureWithCount(); }
StubSignature signatureOfCallCellWithCount(StubKey) { return callConventionSignatureWithCount(); }
StubSignature signatureOfConstructValue(StubKey) { return callConventionSignatureWithCount(); }

StubSignature signatureOfCallResolvedValue(StubKey)
{
    StubSignature signature = callConventionSignature({ });
    signature.addOperand(valueEntryGPR);
    return signature;
}

StubSignature signatureOfResolveCallValue(StubKey)
{
    StubSignature signature;
    signature.addOperand(calleeGPR);
    signature.result = StubResult::GPR;
    signature.firstResult = gprOf(closureWordRegister);
    return signature;
}

CallRegisters CallRegisters::allocate(StubAssembler& jit)
{
    const GPRReg word = jit.clobberRegister(StubRegister::T11);
    const GPRReg scratch = jit.clobberRegister(StubRegister::T13);
    auto allocateUnlessTaken = [&](StubRegister stubRegister) {
        const GPRReg reg = gprOf(stubRegister);
        return reg == word || reg == scratch ? reg : jit.allocateScratch(stubRegister);
    };
    const GPRReg target = allocateUnlessTaken(callTargetRegister);
    const GPRReg temporary = allocateUnlessTaken(callTemporaryRegister);
    return { word, scratch, target, temporary };
}

ClosureHeaderBranches branchIfNotPlainClosureHeader(StubAssembler& jit, GPRReg structureID, GPRReg scratch)
{
    static_assert(!JSCell::structureIDOffset() && sizeof(StructureID) == sizeof(uint32_t));
#if CPU(ARM64)
    jit.load32(Address(instanceGPR, AOT_INSTANCE_OFFSET(slimClosureHeaders[0])), scratch);
    Jump isSlim = jit.branch32(CCallHelpers::Equal, structureID, scratch);
    jit.load32(Address(instanceGPR, AOT_INSTANCE_OFFSET(closures.closureHeaders[0])), scratch);
    return { isSlim, jit.branch32(CCallHelpers::NotEqual, structureID, scratch) };
#else
    UNUSED_PARAM(scratch);
    Jump isSlim = jit.branch32(CCallHelpers::Equal, structureID, Address(instanceGPR, AOT_INSTANCE_OFFSET(slimClosureHeaders[0])));
    return { isSlim, jit.branch32(CCallHelpers::NotEqual, structureID, Address(instanceGPR, AOT_INSTANCE_OFFSET(closures.closureHeaders[0]))) };
#endif
}

void loadClosureWordOfThisRealm(StubAssembler& jit, GPRReg closure, const CallRegisters& registers, JumpList& isNotClosureOfThisRealm)
{
    ASSERT(noOverlap(closure, registers.word, registers.scratch));
    jit.load8(Address(closure, JSCell::typeInfoTypeOffset()), registers.scratch);
    Jump isFull = jit.branch32(CCallHelpers::Equal, registers.scratch, TrustedImm32(CompiledClosureType));
    jit.sub32(TrustedImm32(SlimClosureType), registers.scratch);
    isNotClosureOfThisRealm.append(jit.branch32(CCallHelpers::Above, registers.scratch, TrustedImm32(SlimClosureWithPropertiesType - SlimClosureType)));
    loadStructure(jit, closure, registers.scratch);
    isNotClosureOfThisRealm.append(branchIfNotOfThisRealm(jit, registers.scratch));
    loadSlimClosureWord(jit, closure, registers.word);
    Jump hasWord = jit.jump();

    isFull.link(&jit);
    loadStructure(jit, closure, registers.scratch);
    isNotClosureOfThisRealm.append(branchIfNotOfThisRealm(jit, registers.scratch));
    jit.load64(Address(closure, offsetOfClosureEntry), registers.word);
    hasWord.link(&jit);
}

void emitFillMissingArgumentRegisters(StubAssembler& jit, unsigned count)
{
    for (unsigned i = count; i < numberOfArgumentGPRs; ++i)
        jit.move(TrustedImm64(JSValue::ValueUndefined), argumentGPR(i));
}

void emitFillMissingArgumentRegisters(StubAssembler& jit, GPRReg count, GPRReg scratch)
{
    jit.move(TrustedImm64(JSValue::ValueUndefined), scratch);
    for (unsigned i = 0; i < numberOfArgumentGPRs; ++i)
        jit.moveConditionally32(CCallHelpers::Above, count, TrustedImm32(i), argumentGPR(i), scratch, argumentGPR(i));
}

void emitEnterClosure(StubAssembler& jit, const CallRegisters& registers, std::optional<unsigned> count, JumpList& isNotCallable)
{
    static_assert(ClosureWord::isNotCallableBit > ClosureWord::numberOfParametersShift && ClosureWord::takesNewTargetBit < ClosureWord::numberOfParametersShift && ClosureWord::functionShift + ClosureWord::numberOfFunctionBits <= ClosureWord::numberOfParametersShift);
    if (!count) {
        isNotCallable.append(jit.branchTest64(CCallHelpers::NonZero, registers.word, TrustedImm64(isNotCallableMask)));
        emitFillMissingArgumentRegisters(jit, countGPR, registers.scratch);
        toCodeAddress(jit, registers.word, registers.scratch, StackIsTested::No);
        jit.farJump(registers.word, JSEntryPtrTag);
        return;
    }

    jit.move(TrustedImm64(static_cast<int64_t>(*count + 1) << ClosureWord::numberOfParametersShift), registers.scratch);
    Jump notCommonCase = jit.branch64(CCallHelpers::AboveOrEqual, registers.word, registers.scratch);
    Label enter = jit.label();
    jit.move(TrustedImm32(*count), countGPR);
    toCodeAddress(jit, registers.word, registers.scratch, StackIsTested::No);
    jit.farJump(registers.word, JSEntryPtrTag);

    notCommonCase.link(&jit);
    isNotCallable.append(jit.branchTest64(CCallHelpers::NonZero, registers.word, TrustedImm64(isNotCallableMask)));
    emitFillMissingArgumentRegisters(jit, *count);
    jit.jump().linkTo(enter, &jit);
}

static Jump emitEnterClosureWithStructure(StubAssembler& jit, const CallRegisters& registers, unsigned count, JumpList& isNotCallable)
{
#if CPU(X86_64)
    constexpr unsigned topByteShift = 56;
    static_assert(ClosureWord::offsetUnit == 1 && ClosureWord::numberOfOffsetBits == 32 && ClosureWord::numberOfParametersShift >= topByteShift);
    Jump isNotSlim = jit.branch32(CCallHelpers::NotEqual, registers.word, Address(instanceGPR, AOT_INSTANCE_OFFSET(slimClosureHeaders[0])));
    Jump notCommonCase = jit.branch8(CCallHelpers::AboveOrEqual, Address(calleeGPR, offsetOfSlimClosureWord + topByteShift / 8), TrustedImm32((count + 1) << (ClosureWord::numberOfParametersShift - topByteShift)));
    jit.checkStackBeforeEntering();
    jit.move(TrustedImm32(count), countGPR);
    jit.load32(Address(calleeGPR, offsetOfSlimClosureWord), registers.word);
    jit.add64(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.code)), registers.word);
    jit.farJump(registers.word, JSEntryPtrTag);
    notCommonCase.link(&jit);
    jit.loadPtr(Address(calleeGPR, offsetOfSlimClosureWord), registers.word);
    emitEnterClosure(jit, registers, count, isNotCallable);
    isNotSlim.link(&jit);
    Jump isNeither = jit.branch32(CCallHelpers::NotEqual, registers.word, Address(instanceGPR, AOT_INSTANCE_OFFSET(closures.closureHeaders[0])));
    jit.loadPtr(Address(calleeGPR, offsetOfClosureEntry), registers.word);
    emitEnterClosure(jit, registers, count, isNotCallable);
    return isNeither;
#else
    auto [isSlim, isNeither] = branchIfNotPlainClosureHeader(jit, registers.word, registers.scratch);
    jit.loadPtr(Address(calleeGPR, offsetOfClosureEntry), registers.word);
    emitEnterClosure(jit, registers, count, isNotCallable);
    isSlim.link(&jit);
    jit.loadPtr(Address(calleeGPR, offsetOfSlimClosureWord), registers.word);
    emitEnterClosure(jit, registers, count, isNotCallable);
    return isNeither;
#endif
}

static void emitCallEngineCallee(StubAssembler& jit, unsigned count)
{
    jit.move(TrustedImm32(count), countGPR);
    jit.jumpToStub(callEngineCallee());
}

static void emitCallNotPlainClosure(StubAssembler& jit, const CallRegisters& registers, unsigned count, JumpList& isEngineCallee)
{
    jit.linkToStub(jit.branch32(CCallHelpers::Equal, registers.word, Address(instanceGPR, AOT_INSTANCE_OFFSET(realm.hostFunctionStructureID))), callHostFunction(count));

    JumpList isNotCompiled;
    loadClosureWordOfThisRealm(jit, calleeGPR, registers, isNotCompiled);
    emitEnterClosure(jit, registers, count, isEngineCallee);

    isNotCompiled.link(&jit);
    isEngineCallee.append(jit.branchIfNotType(calleeGPR, JSFunctionType));
    loadStructure(jit, calleeGPR, registers.word);
    static_assert(sizeof(StructureID) == sizeof(uint32_t) && StructureID::structureIDMask == UINT_MAX);
    JumpList isBoundFunction;
    isBoundFunction.append(jit.branch32(CCallHelpers::Equal, registers.word, Address(instanceGPR, AOT_INSTANCE_OFFSET(realm.boundFunctionStructureID))));
    jit.loadPtr(Address(registers.word, Structure::classInfoOffset()), registers.scratch);
    isBoundFunction.append(jit.branchPtr(CCallHelpers::Equal, registers.scratch, Address(instanceGPR, AOT_INSTANCE_OFFSET(realm.boundFunctionClassInfo))));
    if (jit.options().imageHasWebAssembly) {
        jit.loadRuntimePointer(Entry::WebAssemblyFunctionClassInfo, registers.word);
        isEngineCallee.append(jit.branchPtr(CCallHelpers::Equal, registers.scratch, registers.word));
    }
    jit.loadPtr(Address(calleeGPR, JSFunction::offsetOfExecutableOrRareData()), registers.scratch);
    Jump isTagged = jit.branchTestPtr(CCallHelpers::NonZero, registers.scratch, TrustedImm32(JSFunction::rareDataTag));
    jit.linkToStub(jit.branchIfType(registers.scratch, NativeExecutableType), callHostFunction(count));
    isEngineCallee.append(jit.jump());
    isTagged.link(&jit);
    jit.loadPtr(Address(registers.scratch, FunctionRareData::offsetOfExecutable() - JSFunction::rareDataTag), registers.scratch);
    jit.linkToStub(jit.branchIfType(registers.scratch, NativeExecutableType), callHostFunction(count));
    isEngineCallee.append(jit.jump());

    isBoundFunction.link(&jit);
    jit.move(TrustedImm32(count), countGPR);
    jit.jumpToStub(callBoundTarget());
}

void generateCallValue(StubAssembler& jit)
{
    const CallRegisters registers = CallRegisters::allocate(jit);
    const unsigned count = jit.key().count();
    constexpr unsigned sizeOfMove = isX86_64() ? 5 : 4;
    while ((jit.m_assembler.codeSize() + sizeOfMove) % 16)
        jit.nop();
    jit.bindEntry(callValueWithoutThis(count));
    jit.move(TrustedImm64(JSValue::ValueUndefined), thisGPR);
    RELEASE_ASSERT(!(jit.m_assembler.codeSize() % 16));
    jit.bindEntry(callValue(count));

    JumpList isEngineCallee;
    Jump isNotCell = jit.branchIfNotCell(calleeGPR);
    jit.bindEntry(callValueCell(count));
    jit.load32(Address(calleeGPR, JSCell::structureIDOffset()), registers.word);
    Jump isNeither = emitEnterClosureWithStructure(jit, registers, count, isEngineCallee);

    isNeither.link(&jit);
    jit.bindEntry(callValueNotClosure(count));
    emitCallNotPlainClosure(jit, registers, count, isEngineCallee);

    isEngineCallee.link(&jit);
    emitCallEngineCallee(jit, count);

    isNotCell.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTThrowUnlessCallable, { .returns = OperationCall::Returns::Void, .secondArgumentIn = calleeGPR });
}

void generateCallValueNamed(StubAssembler& jit)
{
    const CallRegisters registers = CallRegisters::allocate(jit);
    const unsigned count = jit.key().count();
    JumpList isEngineCallee;
    Jump notCell = jit.branchIfNotCell(calleeGPR);
    jit.load32(Address(calleeGPR, JSCell::structureIDOffset()), registers.word);
    Jump isNeither = emitEnterClosureWithStructure(jit, registers, count, isEngineCallee);
    jit.linkToStub(isNeither, callValueNotClosure(count));
    isEngineCallee.link(&jit);
    emitCallEngineCallee(jit, count);
    notCell.link(&jit);
    jit.move(registers.word, throwWordGPR);
    jit.jumpToStub(throwAtSite(ThrowSiteSource::WordInRegister, CallerFrame::HasFrame));
}

void generateCallValueWithCount(StubAssembler& jit)
{
    const CallRegisters registers = CallRegisters::allocate(jit);
    JumpList isNotClosure;
    isNotClosure.append(jit.branchIfNotCell(calleeGPR));
    jit.bindEntry(callCellWithCount());
    jit.load32(Address(calleeGPR, JSCell::structureIDOffset()), registers.word);
    auto [isSlim, isNeither] = branchIfNotPlainClosureHeader(jit, registers.word, registers.scratch);
    jit.load64(Address(calleeGPR, offsetOfClosureEntry), registers.word);
    emitEnterClosure(jit, registers, std::nullopt, isNotClosure);

    isSlim.link(&jit);
    jit.load64(Address(calleeGPR, offsetOfSlimClosureWord), registers.word);
    emitEnterClosure(jit, registers, std::nullopt, isNotClosure);

    isNeither.link(&jit);
    loadClosureWordOfThisRealm(jit, calleeGPR, registers, isNotClosure);
    emitEnterClosure(jit, registers, std::nullopt, isNotClosure);

    isNotClosure.link(&jit);
    jit.jumpToStub(callBoundTarget());
}

static void emitSpillArgumentsAndPushFrame(StubAssembler& jit)
{
    jit.recordClobbers(RegisterSet::macroClobberedGPRs());
    emitSpillArgumentRegisters(jit);
    jit.emitFunctionPrologue();
}

static Entry constructingOperationOf(ConstructSyntax syntax)
{
    switch (syntax) {
    case ConstructSyntax::NewExpression:
        return Entry::operationAOTConstruct;
    case ConstructSyntax::SuperCall:
        return Entry::operationAOTSuperConstruct;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void emitConstructByOperation(StubAssembler& jit, ConstructSyntax syntax)
{
    constexpr GPRReg fp = GPRInfo::callFrameRegister;
    static_assert(noOverlap(countGPR, operationArgumentGPR(1), operationArgumentGPR(2)));
    jit.move(calleeGPR, operationArgumentGPR(1));
    jit.load64(Address(fp, offsetOfStackWordFromFrame(0)), operationArgumentGPR(2));
    jit.move(countGPR, operationArgumentGPR(3));
    jit.sub32(TrustedImm32(1), operationArgumentGPR(3));
    jit.addPtr(TrustedImm32(static_cast<int32_t>(offsetOfStackWordFromFrame(1))), fp, operationArgumentGPR(4));
    jit.move(instanceGPR, operationArgumentGPR(0));
    jit.callOperation(constructingOperationOf(syntax), jit.reuseScratch(StubRegister::Operation));
    jit.emitFunctionEpilogue();
    emitDropSpilledArgumentRegisters(jit);
    Jump threw = jit.branchTestPtr(CCallHelpers::NonZero, GPRInfo::returnValueGPR2);
    jit.returnToCaller();

    threw.link(&jit);
    jit.jumpToExceptionHandler();
}

void generateConstructValue(StubAssembler& jit)
{
    const CallRegisters registers = CallRegisters::allocate(jit);
    JumpList isNotClosureOfThisRealm;
    isNotClosureOfThisRealm.append(jit.branchIfNotCell(calleeGPR));
    loadClosureWordOfThisRealm(jit, calleeGPR, registers, isNotClosureOfThisRealm);
    Jump isCallable = jit.branchTest64(CCallHelpers::Zero, registers.word, TrustedImm64(isNotCallableMask));

    JumpList never;
    jit.and64(TrustedImm64(~isNotCallableMask), registers.word);
    emitEnterClosure(jit, registers, std::nullopt, never);
    never.link(&jit);
    jit.breakpoint();

    isCallable.link(&jit);
    JumpList isNotPlainFunction;
    emitSpillArgumentsAndPushFrame(jit);
    emitConstructWithPlainFunction(jit, registers.word, isNotPlainFunction);

    isNotClosureOfThisRealm.link(&jit);
    emitSpillArgumentsAndPushFrame(jit);
    isNotPlainFunction.link(&jit);
    emitConstructByOperation(jit, static_cast<ConstructSyntax>(jit.key().form()));
}

void generateResolveCallValue(StubAssembler& jit)
{
    const unsigned count = jit.key().count();
    const GPRReg word = jit.result();
    const GPRReg scratch = jit.allocateScratch(StubRegister::T13);
    JumpList isResolvedAtEachCall;
    isResolvedAtEachCall.append(jit.branchIfNotCell(calleeGPR));
    jit.load32(Address(calleeGPR, JSCell::structureIDOffset()), word);
    auto [isSlim, isNeither] = branchIfNotPlainClosureHeader(jit, word, scratch);
    jit.loadPtr(Address(calleeGPR, offsetOfClosureEntry), word);
    Jump hasWord = jit.jump();
    isSlim.link(&jit);
    jit.loadPtr(Address(calleeGPR, offsetOfSlimClosureWord), word);
    hasWord.link(&jit);
    jit.move(TrustedImm64(static_cast<int64_t>(count + 1) << ClosureWord::numberOfParametersShift), scratch);
    isResolvedAtEachCall.append(jit.branch64(CCallHelpers::AboveOrEqual, word, scratch));
    isResolvedAtEachCall.append(jit.branchPtr(CCallHelpers::Below, CCallHelpers::stackPointerRegister, Address(instanceGPR, AOT_INSTANCE_OFFSET(stackMirror) + StackManager::Mirror::offsetOfTrapAwareSoftStackLimit())));
    toCodeAddress(jit, word, scratch, StackIsTested::Yes);
    jit.returnToCaller();

    isNeither.link(&jit);
    isResolvedAtEachCall.append(jit.branch32(CCallHelpers::NotEqual, word, Address(instanceGPR, AOT_INSTANCE_OFFSET(realm.hostFunctionStructureID))));
    jit.returnStubAddress(callHostFunction(count));

    isResolvedAtEachCall.link(&jit);
    jit.returnStubAddress(callValue(count));
}

void generateCallResolvedValue(StubAssembler& jit)
{
    jit.farJump(jit.operand(0), JSEntryPtrTag);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
