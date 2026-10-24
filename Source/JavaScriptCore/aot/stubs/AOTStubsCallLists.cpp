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
#include "JSArray.h"
#include "StackAlignment.h"
#include "StackManager.h"
#include <array>

namespace JSC { namespace AOT {

struct ArrayCallRegisters {
    CallRegisters call;
    GPRReg list;
    GPRReg mode;
    GPRReg element;
    FPRReg number;

    GPRReg preservedList() const { return call.word; }
    GPRReg storage() const { return call.scratch; }
};

struct ListCopyRegisters {
    GPRReg count;
    GPRReg source;
    GPRReg destination;
    GPRReg index;
    GPRReg temporary;
};

static void emitAlignedListSize(StubAssembler& jit, GPRReg count, unsigned extraRegisters, GPRReg size)
{
    static_assert(stackAlignmentRegisters() == 2);
    jit.add32(TrustedImm32(extraRegisters + 1), count, size);
    jit.and32(TrustedImm32(~1), size);
    jit.lshiftPtr(TrustedImm32(3), size);
}

StubSignature signatureOfCallWithArray(StubKey)
{
    static_assert(gprOf(StubRegister::A0) == argumentGPR(0));
    return callConventionSignature({ StubRegister::A0 });
}

StubSignature signatureOfCallWithList(StubKey)
{
    return StubSignature::withGPRResult({ calleeGPR, thisGPR, argumentGPR(0), argumentGPR(1) });
}

StubSignature signatureOfTailCallWithList(StubKey)
{
    return StubSignature::withGPRResult({ calleeGPR, thisGPR, argumentGPR(0), argumentGPR(1), argumentGPR(2) });
}

StubSignature signatureOfSuperConstructWithList(StubKey)
{
    return StubSignature::withGPRResult({ calleeGPR, argumentGPR(0), argumentGPR(1), argumentGPR(2) });
}

StubSignature signatureOfCallFromArgumentListFrame(StubKey)
{
    StubSignature signature = callConventionSignatureWithCount();
    signature.addOperand(gprOf(callTargetRegister));
    return signature;
}

StubSignature signatureOfReturnFromArgumentListFrame(StubKey) { return { }; }

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
    jit.loadStubAddress(callValueWithCount(), jit.clobberRegister(callTargetRegister));
    jit.jumpToStub(callFromArgumentListFrame());
}

static void emitCopyFromLastWord(StubAssembler& jit, const ListCopyRegisters& copy)
{
    jit.move(copy.count, copy.index);
    Label previous = jit.label();
    jit.sub32(TrustedImm32(1), copy.index);
    jit.load64(BaseIndex(copy.source, copy.index, CCallHelpers::TimesEight), copy.temporary);
    jit.store64(copy.temporary, BaseIndex(copy.destination, copy.index, CCallHelpers::TimesEight));
    jit.branchTest32(CCallHelpers::NonZero, copy.index).linkTo(previous, &jit);
}

void emitPushArgumentListFrame(StubAssembler& jit, const ArgumentList& list, const CallRegisters& registers, JumpList& doesNotFitOnStack)
{
    constexpr GPRReg index = argumentGPR(0);
    constexpr GPRReg temporary = argumentGPR(1);
    const GPRReg words = registers.word;
    ASSERT(noOverlap(list.count, list.words, words, index, temporary));
    emitAlignedListSize(jit, list.count, 0, words);
    jit.subPtr(sp, words, temporary);
    jit.subPtr(TrustedImm32(sizeof(CallerFrameAndPC)), temporary);
    doesNotFitOnStack.append(jit.branchPtr(CCallHelpers::Above, temporary, fp));
    doesNotFitOnStack.append(jit.branchPtr(CCallHelpers::Below, temporary, softStackLimit()));
    jit.emitFunctionPrologue();
    jit.subPtr(sp, words, words);
    jit.move(words, sp);
    emitCopyFromLastWord(jit, { list.count, list.words, words, index, temporary });
    emitPopArgumentRegisters(jit, words);
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
    JumpList doesNotFitOnStack;
    emitPushArgumentListFrame(jit, list, registers, doesNotFitOnStack);
    emitCallValueFromArgumentListFrame(jit);

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

static void emitCopyListBelowFrame(StubAssembler& jit, const ListCopyRegisters& copy)
{
    JumpList didCopy;
    didCopy.append(jit.branchTest32(CCallHelpers::Zero, copy.count));
    Jump movesDown = jit.branchPtr(CCallHelpers::Above, copy.source, copy.destination);
    emitCopyFromLastWord(jit, copy);
    didCopy.append(jit.jump());
    movesDown.link(&jit);
    jit.move(TrustedImm32(0), copy.index);
    Label next = jit.label();
    jit.load64(BaseIndex(copy.source, copy.index, CCallHelpers::TimesEight), copy.temporary);
    jit.store64(copy.temporary, BaseIndex(copy.destination, copy.index, CCallHelpers::TimesEight));
    jit.add32(TrustedImm32(1), copy.index);
    jit.branch32(CCallHelpers::Below, copy.index, copy.count).linkTo(next, &jit);
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
    emitAlignedListSize(jit, count, 0, destination);
    jit.subPtr(frame, destination, destination);
    doesNotFitOnStack.append(jit.branchPtr(CCallHelpers::Above, destination, frame));
    // A signal handler may run on this stack, so nothing is stored below sp.
    Jump isAboveStackPointer = jit.branchPtr(CCallHelpers::BelowOrEqual, sp, destination);
    doesNotFitOnStack.append(jit.branchPtr(CCallHelpers::Below, destination, softStackLimit()));
    jit.move(destination, sp);
    isAboveStackPointer.link(&jit);
    emitCopyListBelowFrame(jit, { count, list, destination, countGPR, bytesAboveFrameRecord });
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

static void emitCallWithElementsInRegisters(StubAssembler& jit, const ArrayCallRegisters& registers, Jump hasDoubleShape, JumpList& genericPath)
{
    std::array<JumpList, numberOfArgumentGPRs> isPastLength;
    for (unsigned i = 0; i < numberOfArgumentGPRs; ++i) {
        isPastLength[i].append(jit.branch32(CCallHelpers::BelowOrEqual, countGPR, TrustedImm32(i)));
        jit.load64(Address(registers.storage(), i * sizeof(EncodedJSValue)), argumentGPR(i));
        genericPath.append(jit.branchTest64(CCallHelpers::Zero, argumentGPR(i)));
    }
    jit.jumpToStub(callValueWithCount());

    hasDoubleShape.link(&jit);
    for (unsigned i = 0; i < numberOfArgumentGPRs; ++i) {
        isPastLength[i].append(jit.branch32(CCallHelpers::BelowOrEqual, countGPR, TrustedImm32(i)));
        jit.loadDouble(Address(registers.storage(), i * sizeof(double)), registers.number);
        boxDouble(jit, PureDouble::branchIfNaN(jit, registers.number, genericPath), argumentGPR(i));
    }
    jit.jumpToStub(callValueWithCount());

    for (unsigned i = 0; i < numberOfArgumentGPRs; ++i) {
        isPastLength[i].link(&jit);
        jit.move(TrustedImm64(JSValue::ValueUndefined), argumentGPR(i));
    }
    jit.jumpToStub(callValueWithCount());
}

static void emitCopyArrayToListFrame(StubAssembler& jit, const ArrayCallRegisters& registers, bool hasDoubleShape, JumpList& genericPath)
{
    GPRReg storage = registers.storage();
    GPRReg frameList = registers.preservedList();
    GPRReg index = registers.mode;
    GPRReg element = registers.element;
    jit.move(countGPR, index);
    Label testPrevious = jit.label();
    jit.sub32(TrustedImm32(1), index);
    if (hasDoubleShape) {
        jit.loadDouble(BaseIndex(storage, index, CCallHelpers::TimesEight), registers.number);
        genericPath.append(jit.branchIfNaN(registers.number));
    } else {
        jit.load64(BaseIndex(storage, index, CCallHelpers::TimesEight), element);
        genericPath.append(jit.branchTest64(CCallHelpers::Zero, element));
    }
    jit.branchTest32(CCallHelpers::NonZero, index).linkTo(testPrevious, &jit);
    emitAlignedListSize(jit, countGPR, 0, index);
    jit.subPtr(sp, index, element);
    genericPath.append(jit.branchPtr(CCallHelpers::Above, element, fp));
    genericPath.append(jit.branchPtr(CCallHelpers::Below, element, softStackLimit()));
    emitDropArgumentListFrames(jit, element);
    jit.emitFunctionPrologue();
    jit.subPtr(sp, index, frameList);
    jit.move(frameList, sp);
    jit.move(countGPR, index);
    Label copyPrevious = jit.label();
    jit.sub32(TrustedImm32(1), index);
    jit.load64(BaseIndex(storage, index, CCallHelpers::TimesEight), element);
    if (hasDoubleShape)
        boxDoubleBits(jit, PureDoubleBits::loadedAgainBehindTestForNaN(element));
    jit.store64(element, BaseIndex(frameList, index, CCallHelpers::TimesEight));
    jit.branchTest32(CCallHelpers::NonZero, index).linkTo(copyPrevious, &jit);
    emitPopArgumentRegisters(jit, frameList);
    emitCallValueFromArgumentListFrame(jit);
}

static void emitCallWithLongArray(StubAssembler& jit, const ArrayCallRegisters& registers, JumpList& genericPath)
{
    genericPath.append(jit.branch32(CCallHelpers::Above, countGPR, TrustedImm32(maxArrayLengthCopiedToStack)));
    jit.and32(TrustedImm32(IndexingShapeMask), registers.mode);
    Jump hasDoubles = jit.branch32(CCallHelpers::Equal, registers.mode, TrustedImm32(DoubleShape));
    Jump isContiguous = jit.branch32(CCallHelpers::Equal, registers.mode, TrustedImm32(ContiguousShape));
    genericPath.append(jit.branch32(CCallHelpers::NotEqual, registers.mode, TrustedImm32(Int32Shape)));
    isContiguous.link(&jit);
    emitCopyArrayToListFrame(jit, registers, false, genericPath);
    hasDoubles.link(&jit);
    emitCopyArrayToListFrame(jit, registers, true, genericPath);
}

static void emitGenericArrayCall(StubAssembler& jit, const ArrayCallRegisters& registers)
{
    ASSERT(noOverlap(operationArgumentGPR(1), operationArgumentGPR(2), operationArgumentGPR(3), calleeGPR, thisGPR, registers.preservedList()));
    jit.move(calleeGPR, operationArgumentGPR(1));
    jit.move(thisGPR, operationArgumentGPR(2));
    jit.move(registers.preservedList(), operationArgumentGPR(3));
    jit.callOperationAndReturn(Entry::operationAOTApply, { });
}

void generateCallWithArray(StubAssembler& jit)
{
    CallRegisters call = CallRegisters::allocate(jit);
    ArrayCallRegisters registers { call, jit.clobberOperand(0), jit.allocateScratch(StubRegister::A1), jit.allocateScratch(StubRegister::A2), jit.allocateScratch(FPRInfo::fpRegT0) };
    JumpList genericPath;
    jit.move(registers.list, registers.preservedList());
    jit.load8(Address(registers.list, JSCell::indexingTypeAndMiscOffset()), registers.mode);
    genericPath.append(branchIfNotPristine(jit, registers.list));
    jit.loadPtr(Address(registers.list, JSObject::butterflyOffset()), registers.storage());
    jit.load32(Address(registers.storage(), Butterfly::offsetOfPublicLength()), countGPR);
    Jump exceedsArgumentRegisters = jit.branch32(CCallHelpers::Above, countGPR, TrustedImm32(numberOfArgumentGPRs));
    jit.and32(TrustedImm32(IndexingShapeMask), registers.mode);
    Jump hasDoubleShape = jit.branch32(CCallHelpers::Equal, registers.mode, TrustedImm32(DoubleShape));
    Jump isContiguous = jit.branch32(CCallHelpers::Equal, registers.mode, TrustedImm32(ContiguousShape));
    Jump hasInt32Shape = jit.branch32(CCallHelpers::Equal, registers.mode, TrustedImm32(Int32Shape));
    genericPath.append(jit.branch32(CCallHelpers::NotEqual, registers.mode, TrustedImm32(UndecidedShape)));
    genericPath.append(jit.branchTest32(CCallHelpers::NonZero, countGPR));
    hasInt32Shape.link(&jit);
    isContiguous.link(&jit);
    emitCallWithElementsInRegisters(jit, registers, hasDoubleShape, genericPath);
    exceedsArgumentRegisters.link(&jit);
    emitCallWithLongArray(jit, registers, genericPath);
    genericPath.link(&jit);
    emitGenericArrayCall(jit, registers);
}

// The frame kind has one return address, so every stub calls from here: a return that is paired with a call is predicted.
void generateCallFromArgumentListFrame(StubAssembler& jit)
{
    jit.call(gprOf(callTargetRegister), JSEntryPtrTag);
    jit.bindEntry(returnFromArgumentListFrame());
    jit.bindReturnLabel(StubFrameKind::ArgumentList);
    jit.emitFunctionEpilogue();
    jit.returnToCaller();
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
