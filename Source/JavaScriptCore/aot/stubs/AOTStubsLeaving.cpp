/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsLeaving.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTPlanOfArrival.h"
#include "AOTRuntimeTable.h"
#include "AOTStubHelpers.h"
#include "AOTStubsBarriers.h"
#include "AOTStubsExceptions.h"
#include "RegisterSet.h"
#include "StackAlignment.h"

namespace JSC { namespace AOT {

static_assert(exists(leaveForSlowBody()) && exists(enterSlowBodiesAndComeBack()) && exists(selectResultOfConstruction()) && exists(goOnInSlowBodyOfCaller()));

StubSignature signatureOfLeaveForSlowBody(StubKey) { return StubSignature::withoutResult({ rowOfLeaveGPR }); }
StubSignature signatureOfEnterSlowBodiesAndComeBack(StubKey) { return StubSignature::withGPRResult({ rowOfLeaveGPR }); }

// The row says whether the fast frame goes or stays, and the operation reads it: the stub does the same for both.
static void emitLayFramesAndEnter(StubAssembler& jit)
{
    constexpr GPRReg sp = CCallHelpers::stackPointerRegister;
    constexpr GPRReg fp = GPRInfo::callFrameRegister;
    // No value ever lies in it, so it is free to the last.
    constexpr GPRReg plan = rowOfLeaveGPR;
    constexpr GPRReg cursor = gprOf(StubRegister::T9);
    constexpr GPRReg left = gprOf(StubRegister::T10);
    constexpr GPRReg address = gprOf(StubRegister::T11);
    constexpr GPRReg word = gprOf(StubRegister::A2);
    static_assert(noOverlap(plan, cursor, left, address, word, instanceGPR));

    jit.emitFunctionPrologue();
    jit.subPtr(TrustedImm32(RegistersLaidDownAtLeave::numberOfWords * sizeof(uint64_t)), sp);
    for (unsigned index = 0; index <= Reg::maxIndex(); ++index) {
        Reg reg = Reg::fromIndex(index);
        Address place(sp, index * sizeof(uint64_t));
        if (reg.isFPR())
            jit.storeDouble(reg.fpr(), place);
        else if (reg.gpr() != sp)
            jit.storePtr(reg.gpr(), place);
    }
    jit.move(instanceGPR, A0);
    jit.move(sp, A1);
    // The leaving code has pushed as many words as the row has frames.
    jit.move(sp, cursor);
    jit.andPtr(TrustedImm32(-static_cast<int32_t>(stackAlignmentBytes())), cursor);
    jit.move(cursor, sp);
    jit.callOperation(Entry::operationAOTLayOutSlowFrames, T11);
    Jump hasNoRoom = jit.branchTestPtr(CCallHelpers::Zero, GPRInfo::returnValueGPR);
    jit.move(GPRInfo::returnValueGPR, plan);

    // First, so that nothing that is written lies below the stack pointer.
    jit.loadPtr(Address(plan, OBJECT_OFFSETOF(PlanOfArrival, stackPointer)), cursor);
    jit.move(cursor, sp);
    jit.loadPtr(Address(plan, OBJECT_OFFSETOF(PlanOfArrival, numberOfStores)), left);
    jit.addPtr(TrustedImm32(sizeof(PlanOfArrival)), plan, cursor);
    Jump storesNothing = jit.branchTestPtr(CCallHelpers::Zero, left);
    Label storesNext = jit.label();
    jit.loadPtr(Address(cursor, OBJECT_OFFSETOF(StoreOfArrival, address)), address);
    jit.loadPtr(Address(cursor, OBJECT_OFFSETOF(StoreOfArrival, word)), word);
    jit.storePtr(word, Address(address));
    jit.addPtr(TrustedImm32(sizeof(StoreOfArrival)), cursor);
    jit.branchSubPtr(CCallHelpers::NonZero, TrustedImm32(1), left).linkTo(storesNext, &jit);
    storesNothing.link(&jit);

    // What a callee saves, and what a unit that is entered by a call takes its parameters in.
    RegisterSet loaded = RegisterSet::calleeSaveRegisters();
    loaded.merge(RegisterSet::fromIterable(allocatableGPRs));
#if CPU(ARM64)
    loaded.add(CCallHelpers::linkRegister, IgnoreVectors);
#endif
    loaded.forEach([&](Reg reg) {
        Address place(plan, OBJECT_OFFSETOF(PlanOfArrival, registers) + reg.index() * sizeof(uint64_t));
        if (reg.isFPR())
            jit.loadDouble(place, reg.fpr());
        else if (reg.gpr() != fp && reg.gpr() != plan)
            jit.loadPtr(place, reg.gpr());
    });
    jit.loadPtr(Address(plan, OBJECT_OFFSETOF(PlanOfArrival, framePointer)), fp);
    jit.loadPtr(Address(plan, OBJECT_OFFSETOF(PlanOfArrival, target)), plan);
    jit.farJump(plan, NoPtrTag);

    // The fast frame is whole, and the registers that a callee saves are as they came. The site that leaves throws like a call.
    hasNoRoom.link(&jit);
    jit.emitFunctionEpilogue();
    emitThrowStackOverflow(jit);
}

void generateLeaveForSlowBody(StubAssembler& jit)
{
    emitLayFramesAndEnter(jit);
}

void generateEnterSlowBodiesAndComeBack(StubAssembler& jit)
{
    emitLayFramesAndEnter(jit);
}

// A hardened build ends the process at a return address in a stub that follows no call.
static void emitCallThatNeverRuns(StubAssembler& jit)
{
    Label head = jit.label();
    jit.call(GPRInfo::returnValueGPR, NoPtrTag);
    RELEASE_ASSERT(CCallHelpers::differenceBetween(head, jit.label()) == bytesInFrontOfReturnIntoStubOfLeaving);
}

// `thisOfConstruction` may be the result register of nothing else.
static void emitSelectResultOfConstruction(StubAssembler& jit, GPRReg thisOfConstruction)
{
    constexpr GPRReg result = GPRInfo::returnValueGPR;
    JumpList isNotObject;
    isNotObject.append(jit.branchIfNotCell(result, DoNotHaveTagRegisters));
    isNotObject.append(jit.branchIfNotObject(result));
    Jump isObject = jit.jump();
    isNotObject.link(&jit);
    jit.move(thisOfConstruction, result);
    isObject.link(&jit);
}

StubSignature signatureOfSelectResultOfConstruction(StubKey) { return { }; }

// A return address in a stub that no return label is bound to stands for a frame that has a record and nothing that anybody asks for, so whoever unwinds or lists frames passes this one.
void generateSelectResultOfConstruction(StubAssembler& jit)
{
    constexpr GPRReg thisOfConstruction = gprOf(StubRegister::T9);
    emitCallThatNeverRuns(jit);
    jit.load64(Address(GPRInfo::callFrameRegister, -static_cast<int32_t>(slotOfThisInFrameThatSelectsResultOfConstruction * sizeof(uint64_t))), thisOfConstruction);
    emitSelectResultOfConstruction(jit, thisOfConstruction);
    jit.emitFunctionEpilogue();
    jit.returnToCaller();
}

StubSignature signatureOfGoOnInSlowBodyOfCaller(StubKey) { return { }; }

void generateGoOnInSlowBodyOfCaller(StubAssembler& jit)
{
    constexpr GPRReg fp = GPRInfo::callFrameRegister;
    constexpr GPRReg result = GPRInfo::returnValueGPR;
    constexpr GPRReg scratch = gprOf(StubRegister::T9);
    constexpr GPRReg cell = gprOf(StubRegister::T10);
    constexpr GPRReg left = gprOf(StubRegister::T11);
    constexpr GPRReg from = gprOf(StubRegister::A2);
    constexpr GPRReg to = gprOf(StubRegister::A3);
    static_assert(noOverlap(result, scratch, cell, left, from, to, rowOfLeaveGPR, instanceGPR));
    auto slot = [](unsigned number) {
        return Address(fp, -static_cast<int32_t>(number * sizeof(uint64_t)));
    };

    emitCallThatNeverRuns(jit);

    // As ConversionEmitter::box() boxes the form.
    constexpr FPRReg number = FPRInfo::returnValueFPR;
    constexpr FPRReg scratchFPR = FPRInfo::fpRegT1;
    static_assert(number != scratchFPR);
    auto isForm = [&](FormInLeaveRow form) {
        return jit.branch32(CCallHelpers::Equal, scratch, TrustedImm32(static_cast<int32_t>(form)));
    };
    static_assert(static_cast<unsigned>(FormInLeaveRow::JSValue) < static_cast<unsigned>(FormInLeaveRow::Cell) && static_cast<unsigned>(FormInLeaveRow::Cell) + 1 == static_cast<unsigned>(FormInLeaveRow::Int32));
    JumpList isBoxed;
    jit.load64(slot(FrameThatGoesOn::slotOfFormOfResult), scratch);
    isBoxed.append(jit.branch32(CCallHelpers::BelowOrEqual, scratch, TrustedImm32(static_cast<int32_t>(FormInLeaveRow::Cell))));
    Jump isInt32 = isForm(FormInLeaveRow::Int32);
    Jump isDouble = isForm(FormInLeaveRow::Double);
    Jump isInt64 = isForm(FormInLeaveRow::Int64);
    jit.and32(TrustedImm32(1), result);
    jit.or32(TrustedImm32(JSValue::ValueFalse), result);
    isBoxed.append(jit.jump());

    isInt64.link(&jit);
    jit.signExtend32ToPtr(result, cell);
    Jump fitsInt32 = jit.branch64(CCallHelpers::Equal, cell, result);
    jit.convertInt64ToDouble(result, number);
    Jump isNotWhole = jit.jump();

    isDouble.link(&jit);
    JumpList isNoInt32;
    jit.branchConvertDoubleToInt32(number, result, isNoInt32, scratchFPR, true);
    fitsInt32.link(&jit);
    isInt32.link(&jit);
    jit.zeroExtend32ToWord(result, result);
    jit.or64(TrustedImm64(JSValue::NumberTag), result);
    isBoxed.append(jit.jump());

    isNoInt32.link(&jit);
    isNotWhole.link(&jit);
    jit.purifyNaN(number, number);
    jit.boxDouble(number, result, DoNotHaveTagRegisters);
    isBoxed.link(&jit);

    jit.load64(slot(FrameThatGoesOn::slotOfThis), scratch);
    Jump isNoConstruction = jit.branchTest64(CCallHelpers::Zero, scratch);
    emitSelectResultOfConstruction(jit, scratch);
    isNoConstruction.link(&jit);

    jit.load64(slot(FrameThatGoesOn::slotOfOffsetOfWord), scratch);
    Jump dropsResult = jit.branchTest64(CCallHelpers::Zero, scratch);
    jit.load64(slot(FrameThatGoesOn::slotOfCell), cell);
    jit.store64(result, BaseIndex(cell, scratch, CCallHelpers::TimesOne));
    // The callee has run since the cell was made: it may be old by now, and what was returned new.
    checkStubOperands(writeBarrier(), { result });
    jit.move(cell, result);
    jit.callStub(writeBarrier());
    dropsResult.link(&jit);

    // The stack words of the head lie right above the address that this frame returns to.
    jit.load64(slot(FrameThatGoesOn::slotOfNumberOfStackWords), left);
    jit.addPtr(TrustedImm32(-static_cast<int32_t>(FrameThatGoesOn::firstSlotOfStackWords * sizeof(uint64_t))), fp, from);
    jit.addPtr(TrustedImm32(static_cast<int32_t>(offsetOfStackWordsFromFrame)), fp, to);
    Jump hasNoStackWords = jit.branchTest64(CCallHelpers::Zero, left);
    Label copiesNext = jit.label();
    jit.load64(Address(from), scratch);
    jit.store64(scratch, Address(to));
    jit.subPtr(TrustedImm32(sizeof(uint64_t)), from);
    jit.addPtr(TrustedImm32(sizeof(uint64_t)), to);
    jit.branchSubPtr(CCallHelpers::NonZero, TrustedImm32(1), left).linkTo(copiesNext, &jit);
    hasNoStackWords.link(&jit);

    for (unsigned index = 0; index < numberOfDirectArgumentGPRs; ++index)
        jit.load64(slot(FrameThatGoesOn::firstSlotOfRegisters + index), directArgumentGPR(index));
    jit.load64(slot(FrameThatGoesOn::slotOfHead), rowOfLeaveGPR);
    jit.emitFunctionEpilogue();
    jit.farJump(rowOfLeaveGPR, NoPtrTag);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
