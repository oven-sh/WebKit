/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsCallLists.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTStubFrames.h"
#include "AOTStubsExceptions.h"
#include "Butterfly.h"
#include "CallFrame.h"
#include "Interpreter.h"
#include "JSArray.h"
#include "StackAlignment.h"
#include "StackManager.h"
#include <array>

namespace JSC { namespace AOT {

void emitAlignedSizeOfWords(StubAssembler& jit, GPRReg count, int32_t extraWords, GPRReg size)
{
    static_assert(stackAlignmentRegisters() == 2);
    jit.add32(TrustedImm32(extraWords + 1), count, size);
    jit.and32(TrustedImm32(~1), size);
    jit.lshiftPtr(TrustedImm32(3), size);
}

static_assert(gprOf(StubRegister::A0) == argumentGPR(0));
StubSignature signatureOfCallWithArray(StubKey) { return callConventionSignature({ StubRegister::A0 }); }
StubSignature signatureOfCallWithList(StubKey) { return StubSignature::withGPRResult({ calleeGPR, thisGPR, argumentGPR(0), argumentGPR(1) }); }
StubSignature signatureOfTailCallWithList(StubKey) { return StubSignature::withGPRResult({ calleeGPR, thisGPR, argumentGPR(0), argumentGPR(1), argumentGPR(2) }); }
StubSignature signatureOfSuperConstructWithList(StubKey) { return StubSignature::withGPRResult({ calleeGPR, argumentGPR(0), argumentGPR(1), argumentGPR(2) }); }
StubSignature signatureOfSuperConstructWithArray(StubKey) { return StubSignature::withGPRResult({ calleeGPR, argumentGPR(0), argumentGPR(1) }); }
StubSignature signatureOfReturnFromArgumentListFrame(StubKey) { return { }; }

StubSignature signatureOfCallFromArgumentListFrame(StubKey)
{
    StubSignature signature = callConventionSignatureWithCount();
    signature.addOperand(valueEntryGPR);
    return signature;
}

void emitLoadArgumentRegistersFromList(StubAssembler& jit, const ArgumentList& list, unsigned firstRegister)
{
    std::array<Jump, numberOfArgumentGPRs> noMoreArguments;
    for (unsigned i = firstRegister; i < numberOfArgumentGPRs; ++i) {
        ASSERT(noOverlap(argumentGPR(i), list.count, list.words));
        noMoreArguments[i] = jit.branch32(CCallHelpers::BelowOrEqual, list.count, TrustedImm32(i - firstRegister));
        jit.load64(Address(list.words, (i - firstRegister) * sizeof(Register)), argumentGPR(i));
    }
    Jump didLoadEveryRegister = jit.jump();
    for (unsigned i = firstRegister; i < numberOfArgumentGPRs; ++i) {
        noMoreArguments[i].link(&jit);
        jit.move(TrustedImm64(JSValue::ValueUndefined), argumentGPR(i));
    }
    didLoadEveryRegister.link(&jit);
}

static void emitPopArgumentRegisters(StubAssembler& jit, GPRReg words)
{
    for (unsigned i = 0; i < numberOfArgumentGPRs; ++i) {
        ASSERT(words != argumentGPR(i));
        jit.load64(Address(words, i * sizeof(Register)), argumentGPR(i));
    }
    jit.addPtr(TrustedImm32(sizeOfSpilledArgumentRegisters), words, sp);
}

static void emitCallValueFromArgumentListFrame(StubAssembler& jit)
{
    jit.loadStubAddress(callValueWithCount(), valueEntryGPR);
    jit.jumpToStub(callFromArgumentListFrame());
}

void emitCopyFromLastWord(StubAssembler& jit, Address source, Address destination, GPRReg index, GPRReg word)
{
    Label previous = jit.label();
#if CPU(X86_64)
    constexpr int32_t offsetOfPreviousWord = -static_cast<int32_t>(sizeof(Register));
    jit.load64(source.withOffset(offsetOfPreviousWord).indexedBy(index, CCallHelpers::TimesEight), word);
    jit.store64(word, destination.withOffset(offsetOfPreviousWord).indexedBy(index, CCallHelpers::TimesEight));
    jit.branchSub32(CCallHelpers::NonZero, TrustedImm32(1), index).linkTo(previous, &jit);
#else
    jit.sub32(TrustedImm32(1), index);
    jit.load64(source.indexedBy(index, CCallHelpers::TimesEight), word);
    jit.store64(word, destination.indexedBy(index, CCallHelpers::TimesEight));
    jit.branchTest32(CCallHelpers::NonZero, index).linkTo(previous, &jit);
#endif
}

void emitCopyFromFirstWord(StubAssembler& jit, GPRReg source, GPRReg destination, GPRReg count, GPRReg index, GPRReg word)
{
    jit.move(TrustedImm32(0), index);
    Label next = jit.label();
    jit.load64(BaseIndex(source, index, CCallHelpers::TimesEight), word);
    jit.store64(word, BaseIndex(destination, index, CCallHelpers::TimesEight));
    jit.add32(TrustedImm32(1), index);
    jit.branch32(CCallHelpers::Below, index, count).linkTo(next, &jit);
}

static void branchIfNotCompiledCallee(StubAssembler& jit, GPRReg scratch, JumpList& isNotCompiledCallee)
{
    static_assert(isCompiledCalleeType(CompiledClosureType) && SlimClosureWithPropertiesType == SlimClosureType + 1);
    isNotCompiledCallee.append(jit.branchIfNotCell(calleeGPR));
    jit.load8(Address(calleeGPR, JSCell::typeInfoTypeOffset()), scratch);
    Jump isClosure = jit.branch32(CCallHelpers::Equal, scratch, TrustedImm32(CompiledClosureType));
    jit.sub32(TrustedImm32(SlimClosureType), scratch);
    isNotCompiledCallee.append(jit.branch32(CCallHelpers::Above, scratch, TrustedImm32(SlimClosureWithPropertiesType - SlimClosureType)));
    isClosure.link(&jit);
}

void generateCallWithList(StubAssembler& jit)
{
    const CallRegisters registers = CallRegisters::allocate(jit);
    const ArgumentList list { countGPR, registers.scratch };
    jit.zeroExtend32ToWord(jit.clobberOperand(2), list.count);
    jit.move(jit.clobberOperand(3), list.words);
    Jump exceedsArgumentRegisters = jit.branch32(CCallHelpers::Above, list.count, TrustedImm32(numberOfArgumentGPRs));
    emitLoadArgumentRegistersFromList(jit, list);
    jit.jumpToStub(callValueWithCount());

    exceedsArgumentRegisters.link(&jit);
    constexpr GPRReg index = argumentGPR(0);
    constexpr GPRReg temporary = argumentGPR(1);
    const GPRReg words = registers.word;
    ASSERT(noOverlap(list.count, list.words, words, index, temporary));
    JumpList doesNotFitOnStack;
    Jump isLong = jit.branch32(CCallHelpers::Above, list.count, TrustedImm32(maxListLengthCopiedTwice));
    Label usesListFrame = jit.label();
    emitAlignedSizeOfWords(jit, list.count, 0, words);
    jit.subPtr(sp, words, temporary);
    jit.subPtr(TrustedImm32(sizeof(CallerFrameAndPC)), temporary);
    doesNotFitOnStack.append(jit.branchPtr(CCallHelpers::Below, temporary, softStackLimit()));
    jit.emitFunctionPrologue();
    jit.subPtr(sp, words, words);
    jit.move(words, sp);
    jit.move(list.count, index);
    emitCopyFromLastWord(jit, Address(list.words), Address(words), index, temporary);
    emitPopArgumentRegisters(jit, words);
    emitCallValueFromArgumentListFrame(jit);

    isLong.link(&jit);
    JumpList isEngineCallee;
    doesNotFitOnStack.append(jit.branch32(CCallHelpers::Above, list.count, TrustedImm32(maxArguments)));
    branchIfNotCompiledCallee(jit, temporary, isEngineCallee);
    jit.jump().linkTo(usesListFrame, &jit);

    isEngineCallee.link(&jit);
    jit.move(calleeGPR, registers.word);
    jit.callOperationAndReturn(Entry::operationAOTCallEngineCallee, { .secondArgumentIn = registers.word, .thirdArgumentIn = thisGPR, .fourthArgumentIn = list.count, .fifthArgumentIn = list.words });

    doesNotFitOnStack.link(&jit);
    emitThrowStackOverflow(jit);
}

void generateSuperConstructWithList(StubAssembler& jit)
{
    constexpr unsigned numberOfRegistersForList = numberOfArgumentGPRs - 1;
    const CallRegisters registers = CallRegisters::allocate(jit);
    const GPRReg newTarget = jit.operand(1);
    const ArgumentList list { jit.clobberRegister(countGPR), registers.scratch };
    jit.zeroExtend32ToWord(jit.clobberOperand(2), list.count);
    jit.move(jit.clobberOperand(3), list.words);
    Jump exceedsArgumentRegisters = jit.branch32(CCallHelpers::Above, list.count, TrustedImm32(numberOfRegistersForList));
    emitLoadArgumentRegistersFromList(jit, list, 1);
    jit.add32(TrustedImm32(1), countGPR);
    jit.move(TrustedImm64(JSValue::ValueUndefined), jit.clobberRegister(thisGPR));
    jit.jumpToStub(constructValue(ConstructSyntax::SuperCall));

    exceedsArgumentRegisters.link(&jit);
    jit.move(calleeGPR, registers.word);
    jit.callOperationAndReturn(Entry::operationAOTSuperConstruct, { .secondArgumentIn = registers.word, .thirdArgumentIn = newTarget, .fourthArgumentIn = list.count, .fifthArgumentIn = list.words });
}

// super(...list). The elements of a short list that has every element go into the registers behind new.target.
void generateSuperConstructWithArray(StubAssembler& jit)
{
    const CallRegisters registers = CallRegisters::allocate(jit);
    const GPRReg newTarget = jit.operand(1);
    const GPRReg list = jit.clobberOperand(2);
    const GPRReg preservedList = registers.word;
    const GPRReg storage = registers.scratch;
    const GPRReg mode = jit.allocateScratch(StubRegister::A2);
    const FPRReg number = jit.allocateScratch(FPRInfo::fpRegT0);
    jit.clobberRegister(countGPR);
    ASSERT(newTarget == argumentGPR(0) && list == argumentGPR(1));
    ASSERT(noOverlap(calleeGPR, countGPR, newTarget, list, preservedList, storage, mode));
    JumpList genericPath;

    jit.move(list, preservedList);
    jit.load8(Address(list, JSCell::indexingTypeAndMiscOffset()), mode);
    genericPath.append(branchIfNotPristine(jit, list));
    jit.loadPtr(Address(list, JSObject::butterflyOffset()), storage);
    jit.load32(Address(storage, Butterfly::offsetOfPublicLength()), countGPR);
    genericPath.append(jit.branch32(CCallHelpers::AboveOrEqual, countGPR, TrustedImm32(numberOfArgumentGPRs)));
    jit.and32(TrustedImm32(IndexingShapeMask), mode);
    Jump hasDoubleShape = jit.branch32(CCallHelpers::Equal, mode, TrustedImm32(DoubleShape));
    Jump isContiguous = jit.branch32(CCallHelpers::Equal, mode, TrustedImm32(ContiguousShape));
    Jump hasInt32Shape = jit.branch32(CCallHelpers::Equal, mode, TrustedImm32(Int32Shape));
    genericPath.append(jit.branch32(CCallHelpers::NotEqual, mode, TrustedImm32(UndecidedShape)));
    genericPath.append(jit.branchTest32(CCallHelpers::NonZero, countGPR));
    hasInt32Shape.link(&jit);
    isContiguous.link(&jit);

    JumpList didLoadRegisters;
    std::array<JumpList, numberOfArgumentGPRs> isPastLength;
    for (unsigned i = 1; i < numberOfArgumentGPRs; ++i) {
        isPastLength[i].append(jit.branch32(CCallHelpers::Below, countGPR, TrustedImm32(i)));
        jit.load64(Address(storage, (i - 1) * sizeof(EncodedJSValue)), argumentGPR(i));
        genericPath.append(jit.branchTest64(CCallHelpers::Zero, argumentGPR(i)));
    }
    didLoadRegisters.append(jit.jump());

    hasDoubleShape.link(&jit);
    for (unsigned i = 1; i < numberOfArgumentGPRs; ++i) {
        isPastLength[i].append(jit.branch32(CCallHelpers::Below, countGPR, TrustedImm32(i)));
        jit.loadDouble(Address(storage, (i - 1) * sizeof(double)), number);
        boxDouble(jit, PureDouble::branchIfNaN(jit, number, genericPath), argumentGPR(i));
    }
    didLoadRegisters.append(jit.jump());

    for (unsigned i = 1; i < numberOfArgumentGPRs; ++i) {
        isPastLength[i].link(&jit);
        jit.move(TrustedImm64(JSValue::ValueUndefined), argumentGPR(i));
    }
    didLoadRegisters.link(&jit);
    jit.add32(TrustedImm32(1), countGPR);
    jit.move(TrustedImm64(JSValue::ValueUndefined), jit.clobberRegister(thisGPR));
    jit.jumpToStub(constructValue(ConstructSyntax::SuperCall));

    genericPath.link(&jit);
    jit.move(calleeGPR, storage);
    jit.callOperationAndReturn(Entry::operationAOTSuperConstructList, { .secondArgumentIn = storage, .thirdArgumentIn = newTarget, .fourthArgumentIn = preservedList });
}

static void emitCopyListBelowFrame(StubAssembler& jit, GPRReg count, GPRReg source, GPRReg destination, GPRReg index, GPRReg word)
{
    JumpList didCopy;
    didCopy.append(jit.branchTest32(CCallHelpers::Zero, count));
    Jump movesDown = jit.branchPtr(CCallHelpers::Above, source, destination);
    jit.zeroExtend32ToWord(count, index);
    emitCopyFromLastWord(jit, Address(source), Address(destination), index, word);
    didCopy.append(jit.jump());
    movesDown.link(&jit);
    emitCopyFromFirstWord(jit, source, destination, count, index, word);
    didCopy.link(&jit);
}

void generateTailCallWithList(StubAssembler& jit)
{
    const CallRegisters registers = CallRegisters::allocate(jit);
    const GPRReg count = jit.clobberOperand(2);
    const GPRReg list = jit.clobberOperand(3);
    const GPRReg bytesAboveFrameRecord = jit.clobberOperand(4);
    const GPRReg frame = registers.scratch;
    const GPRReg destination = registers.word;
    const GPRReg callerFrame = jit.allocateScratch(StubRegister::T14);
#if CPU(X86_64)
    constexpr GPRReg returnPC = CCallHelpers::s_scratchRegister;
    jit.recordClobbers(RegisterSet::macroClobberedGPRs());
#else
    const GPRReg returnPC = registers.target;
#endif
    ASSERT(noOverlap(calleeGPR, thisGPR, countGPR, count, list, bytesAboveFrameRecord, frame, destination, callerFrame, returnPC));

    jit.move(fp, frame);
    Label nextFrame = jit.label();
    jit.loadPtr(Address(frame, CallFrame::returnPCOffset()), returnPC);
    Jump isNotOnListFrame = jit.branchPtr(CCallHelpers::NotEqual, returnPC, Address(instanceGPR, AOT_INSTANCE_OFFSET(stubs) + StubWords::offsetOfReturnAddress(StubFrameKind::ArgumentList)));
    jit.loadPtr(Address(frame, CallFrame::callerFrameOffset()), frame);
    jit.move(TrustedImm32(0), bytesAboveFrameRecord);
    jit.jump().linkTo(nextFrame, &jit);
    isNotOnListFrame.link(&jit);
    jit.loadPtr(Address(frame, CallFrame::callerFrameOffset()), callerFrame);
    jit.addPtr(bytesAboveFrameRecord, frame);

    JumpList doesNotFitOnStack;
    emitAlignedSizeOfWords(jit, count, 0, destination);
    jit.subPtr(frame, destination, destination);
    doesNotFitOnStack.append(jit.branchPtr(CCallHelpers::Above, destination, frame));
    // A signal handler may run on this stack, so nothing is stored below sp.
    Jump isAboveStackPointer = jit.branchPtr(CCallHelpers::BelowOrEqual, sp, destination);
    doesNotFitOnStack.append(jit.branchPtr(CCallHelpers::Below, destination, softStackLimit()));
    jit.move(destination, sp);
    isAboveStackPointer.link(&jit);
    emitCopyListBelowFrame(jit, count, list, destination, countGPR, bytesAboveFrameRecord);
    jit.storePtr(callerFrame, Address(frame, CallFrame::callerFrameOffset()));
    jit.storePtr(returnPC, Address(frame, CallFrame::returnPCOffset()));
    jit.move(frame, fp);
    jit.zeroExtend32ToWord(count, countGPR);

    Jump exceedsArgumentRegisters = jit.branch32(CCallHelpers::Above, countGPR, TrustedImm32(numberOfArgumentGPRs));
    emitLoadArgumentRegistersFromList(jit, { countGPR, destination });
    jit.emitFunctionEpilogue();
    jit.jumpToStub(callValueWithCount());

    exceedsArgumentRegisters.link(&jit);
    emitPopArgumentRegisters(jit, destination);
    emitCallValueFromArgumentListFrame(jit);

    doesNotFitOnStack.link(&jit);
    jit.emitFunctionEpilogue();
    emitThrowStackOverflow(jit);
}

void generateCallWithArray(StubAssembler& jit)
{
    const CallRegisters registers = CallRegisters::allocate(jit);
    const GPRReg list = jit.clobberOperand(0);
    const GPRReg mode = jit.allocateScratch(StubRegister::A1);
    const GPRReg element = jit.allocateScratch(StubRegister::A2);
    const FPRReg number = jit.allocateScratch(FPRInfo::fpRegT0);
    const GPRReg preservedList = registers.word;
    const GPRReg storage = registers.scratch;
    JumpList genericPath;

    jit.move(list, preservedList);
    jit.load8(Address(list, JSCell::indexingTypeAndMiscOffset()), mode);
    genericPath.append(branchIfNotPristine(jit, list));
    jit.loadPtr(Address(list, JSObject::butterflyOffset()), storage);
    jit.load32(Address(storage, Butterfly::offsetOfPublicLength()), countGPR);
    Jump exceedsArgumentRegisters = jit.branch32(CCallHelpers::Above, countGPR, TrustedImm32(numberOfArgumentGPRs));
    jit.and32(TrustedImm32(IndexingShapeMask), mode);
    Jump hasDoubleShape = jit.branch32(CCallHelpers::Equal, mode, TrustedImm32(DoubleShape));
    Jump isContiguous = jit.branch32(CCallHelpers::Equal, mode, TrustedImm32(ContiguousShape));
    Jump hasInt32Shape = jit.branch32(CCallHelpers::Equal, mode, TrustedImm32(Int32Shape));
    genericPath.append(jit.branch32(CCallHelpers::NotEqual, mode, TrustedImm32(UndecidedShape)));
    genericPath.append(jit.branchTest32(CCallHelpers::NonZero, countGPR));
    hasInt32Shape.link(&jit);
    isContiguous.link(&jit);

    std::array<JumpList, numberOfArgumentGPRs> isPastLength;
    for (unsigned i = 0; i < numberOfArgumentGPRs; ++i) {
        isPastLength[i].append(jit.branch32(CCallHelpers::BelowOrEqual, countGPR, TrustedImm32(i)));
        jit.load64(Address(storage, i * sizeof(EncodedJSValue)), argumentGPR(i));
        genericPath.append(jit.branchTest64(CCallHelpers::Zero, argumentGPR(i)));
    }
    jit.jumpToStub(callValueWithCount());

    hasDoubleShape.link(&jit);
    for (unsigned i = 0; i < numberOfArgumentGPRs; ++i) {
        isPastLength[i].append(jit.branch32(CCallHelpers::BelowOrEqual, countGPR, TrustedImm32(i)));
        jit.loadDouble(Address(storage, i * sizeof(double)), number);
        boxDouble(jit, PureDouble::branchIfNaN(jit, number, genericPath), argumentGPR(i));
    }
    jit.jumpToStub(callValueWithCount());

    for (unsigned i = 0; i < numberOfArgumentGPRs; ++i) {
        isPastLength[i].link(&jit);
        jit.move(TrustedImm64(JSValue::ValueUndefined), argumentGPR(i));
    }
    jit.jumpToStub(callValueWithCount());

    auto copyToListFrameAndCall = [&](bool holdsDoubles) {
        const GPRReg frameList = preservedList;
        const GPRReg index = mode;
        jit.move(countGPR, index);
        Label testPrevious = jit.label();
        jit.sub32(TrustedImm32(1), index);
        if (holdsDoubles) {
            jit.loadDouble(BaseIndex(storage, index, CCallHelpers::TimesEight), number);
            genericPath.append(jit.branchIfNaN(number));
        } else {
            jit.load64(BaseIndex(storage, index, CCallHelpers::TimesEight), element);
            genericPath.append(jit.branchTest64(CCallHelpers::Zero, element));
        }
        jit.branchTest32(CCallHelpers::NonZero, index).linkTo(testPrevious, &jit);
        emitDropArgumentListFrames(jit, element);
        emitAlignedSizeOfWords(jit, countGPR, 0, index);
        jit.subPtr(sp, index, element);
        genericPath.append(jit.branchPtr(CCallHelpers::Below, element, softStackLimit()));
        jit.emitFunctionPrologue();
        jit.subPtr(sp, index, frameList);
        jit.move(frameList, sp);
        jit.move(countGPR, index);
        Label copyPrevious = jit.label();
        jit.sub32(TrustedImm32(1), index);
        jit.load64(BaseIndex(storage, index, CCallHelpers::TimesEight), element);
        if (holdsDoubles)
            boxDoubleBits(jit, PureDoubleBits::loadedAgainBehindTestForNaN(element));
        jit.store64(element, BaseIndex(frameList, index, CCallHelpers::TimesEight));
        jit.branchTest32(CCallHelpers::NonZero, index).linkTo(copyPrevious, &jit);
        emitPopArgumentRegisters(jit, frameList);
        emitCallValueFromArgumentListFrame(jit);
    };
    exceedsArgumentRegisters.link(&jit);
    Jump isLong = jit.branch32(CCallHelpers::Above, countGPR, TrustedImm32(maxListLengthCopiedTwice));
    Label usesListFrame = jit.label();
    jit.and32(TrustedImm32(IndexingShapeMask), mode);
    Jump longListHoldsDoubles = jit.branch32(CCallHelpers::Equal, mode, TrustedImm32(DoubleShape));
    Jump longListIsContiguous = jit.branch32(CCallHelpers::Equal, mode, TrustedImm32(ContiguousShape));
    genericPath.append(jit.branch32(CCallHelpers::NotEqual, mode, TrustedImm32(Int32Shape)));
    longListIsContiguous.link(&jit);
    copyToListFrameAndCall(false);
    longListHoldsDoubles.link(&jit);
    copyToListFrameAndCall(true);

    isLong.link(&jit);
    JumpList isEngineCallee;
    genericPath.append(jit.branch32(CCallHelpers::Above, countGPR, TrustedImm32(maxArguments)));
    branchIfNotCompiledCallee(jit, element, isEngineCallee);
    jit.jump().linkTo(usesListFrame, &jit);

    // A tail call out of a function that the interpreter entered. The callee's frame replaces the adapter's, which a call from C++ would keep on the stack.
    isEngineCallee.link(&jit);
    for (StubFrameKind kind : { StubFrameKind::Adapter, StubFrameKind::ArgumentList }) {
        Address returnAddressOfKind(instanceGPR, AOT_INSTANCE_OFFSET(stubs) + StubWords::offsetOfReturnAddress(kind));
#if CPU(ARM64)
        jit.loadPtr(returnAddressOfKind, element);
        jit.branchPtr(CCallHelpers::Equal, CCallHelpers::linkRegister, element).linkTo(usesListFrame, &jit);
#else
        jit.loadPtr(Address(sp), element);
        jit.branchPtr(CCallHelpers::Equal, element, returnAddressOfKind).linkTo(usesListFrame, &jit);
#endif
    }

    genericPath.link(&jit);
    emitDropArgumentListFrames(jit, element);
    ASSERT(noOverlap(operationArgumentGPR(1), operationArgumentGPR(2), operationArgumentGPR(3), calleeGPR, thisGPR, preservedList));
    jit.move(calleeGPR, operationArgumentGPR(1));
    jit.move(thisGPR, operationArgumentGPR(2));
    jit.move(preservedList, operationArgumentGPR(3));
    jit.callOperationAndReturn(Entry::operationAOTApply, { });
}

// The frame kind has one return address, so every stub calls from here: a return that is paired with a call is predicted.
void generateCallFromArgumentListFrame(StubAssembler& jit)
{
    jit.call(valueEntryGPR, JSEntryPtrTag);
    jit.bindEntry(returnFromArgumentListFrame());
    jit.bindReturnLabel(StubFrameKind::ArgumentList);
    jit.emitFunctionEpilogue();
    jit.returnToCaller();
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
