/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsPropertyProbes.h"

#if ENABLE(AOT)

#include "AOTStubsAccessTable.h"
#include "AOTStubsPropertyAccessors.h"
#include "GetterSetter.h"

namespace JSC { namespace AOT {

static constexpr StubRegister getterSetterRegister = StubRegister::T10;
static constexpr unsigned firstOperandOfFinisher = 3;

static_assert(noOverlap(operationArgumentGPR(4), gprOf(firstStubOperandRegister), gprOf(slotHeaderRegister), gprOf(slotPayloadRegister), gprOf(storedValueByIdRegister)));
static_assert(noOverlap(gprOf(getterSetterRegister), gprOf(firstStubOperandRegister), gprOf(slotHeaderRegister), gprOf(slotPayloadRegister), gprOf(storedValueByIdRegister)));

struct FinisherCall {
    Entry entry;
    bool returnsValue;
    bool takesGetterSetter;
    bool takesValue;
};

static constexpr FinisherCall finisherCallOf(SlotFinisher finisher)
{
    switch (finisher) {
    case SlotFinisher::ReadField:
        return { Entry::operationAOTReadField, true, false, false };
    case SlotFinisher::CallGetter:
        return { Entry::operationAOTCallGetterOfSlot, true, true, false };
    case SlotFinisher::StoreField:
        return { Entry::operationAOTStoreField, false, false, true };
    case SlotFinisher::CreateField:
        return { Entry::operationAOTCreateField, false, false, true };
    case SlotFinisher::CallSetterStrict:
    case SlotFinisher::CallSetterSloppy:
        return { Entry::operationAOTCallSetterOfSlot, false, true, true };
    case SlotFinisher::GrowStorage:
        return { Entry::operationAOTAddPropertyBySlot, false, false, true };
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

StubSignature signatureOfSlotCannot(StubKey key)
{
    const FinisherCall finisherCall = finisherCallOf(static_cast<SlotFinisher>(key.form()));
    StubSignature signature = StubSignature::withoutResult({ gprOf(firstStubOperandRegister), gprOf(slotHeaderRegister), gprOf(slotPayloadRegister) });
    if (finisherCall.takesGetterSetter)
        signature.addOperand(gprOf(getterSetterRegister));
    if (finisherCall.takesValue)
        signature.addOperand(gprOf(storedValueByIdRegister));
    if (finisherCall.returnsValue) {
        signature.result = StubResult::GPR;
        signature.firstResult = GPRInfo::returnValueGPR;
    }
    return signature;
}

StubSignature signatureOfCallGetterOfAnswer(StubKey)
{
    return StubSignature::withGPRResult({ gprOf(firstStubOperandRegister), gprOf(getterSetterRegister) });
}

void generateCallGetterOfAnswer(StubAssembler& jit)
{
    const SlotOrigin origin = jit.key().callerFrame() == CallerFrame::StubFrame ? SlotOrigin::ByIdInStubFrame : SlotOrigin::ById;
    const StubKey finisher = slotCannot(SlotFinisher::CallGetter, origin);
    RELEASE_ASSERT(signatureOfSlotCannot(finisher).operands[firstOperandOfFinisher] == Reg(jit.operand(1)) && signatureOfSlotCannot(finisher).operands[0] == Reg(jit.operand(0)));
    jit.recordClobbers(RegisterSet { gprOf(slotHeaderRegister), gprOf(slotPayloadRegister), gprOf(slotNameRegister) });
    JumpList cannot;
    callGetterAndReturn(jit, jit.operand(0), jit.clobberOperand(1), cannot);
    jit.linkToStub(cannot, finisher);
}

StubSignature signatureOfGetterByAccessTable(StubKey)
{
    StubSignature signature = StubSignature::withGPRResult({ gprOf(firstStubOperandRegister) });
    signature.immediate = gprOf(stubImmediateRegister);
    return signature;
}

void generateGetterByAccessTable(StubAssembler& jit)
{
    const GPRReg object = jit.operand(0);
    const GPRReg name = jit.immediate();
    const GPRReg scratch = jit.allocateScratch(StubRegister::T10);
    const GPRReg key = jit.allocateScratch(StubRegister::T11);
    const GPRReg place = jit.allocateScratch(StubRegister::T12);
    const GPRReg answer = jit.allocateScratch(StubRegister::T14);
    constexpr int32_t bitsOfForm = AccessTable::getterIsInOwnStorage | AccessTable::answerHasForm;
    const AccessTableRegisters registers { object, name, key, place };
    JumpList findsNone;
    locatePlaceInAccessTable(jit, PartOfAccessTable::Reads, registers);
    loadWordBehindKeyInAccessTable(jit, registers, OBJECT_OFFSETOF(AccessTable::Read, keyOfOtherAnswer), answer, scratch, findsNone);
    jit.and32(TrustedImm32(bitsOfForm), answer, key);
    findsNone.append(jit.branch32(CCallHelpers::NotEqual, key, TrustedImm32(bitsOfForm)));
    loadValueInOwnStorageByAnswer(jit, object, answer, place, answer);
    branchUnlessAccessorPair(jit, answer, findsNone);
    jit.loadPtr(Address(answer, GetterSetter::offsetOfGetter()), jit.result());
    jit.returnToCaller();

    findsNone.link(&jit);
    jit.move(TrustedImm64(JSValue::ValueUndefined), jit.result());
    jit.returnToCaller();
}

static void setUpSetterCall(StubAssembler& jit, PutMode mode, OperationCall& call)
{
    call.thirdArgumentIn = jit.operand(firstOperandOfFinisher);
    call.fourthArgumentIn = jit.operand(firstOperandOfFinisher + 1);
    call.constantArguments[0] = OperationCall::ConstantArgument { operationArgumentGPR(4), static_cast<uint32_t>(mode) };
}

void generateSlotCannot(StubAssembler& jit)
{
    const SlotFinisher finisher = static_cast<SlotFinisher>(jit.key().form());
    const FinisherCall finisherCall = finisherCallOf(finisher);
    OperationCall call {
        .returns = finisherCall.returnsValue ? OperationCall::Returns::Value : OperationCall::Returns::Void,
        .saves = OperationCall::Saves::CallerSavedRegisters,
        .checksException = effectsOf(finisherCall.entry).mayThrow,
        .secondArgumentIn = jit.operand(receiverOperand),
    };
    switch (finisher) {
    case SlotFinisher::ReadField:
        call.thirdArgumentIn = jit.operand(headerOperand);
        call.fourthArgumentIn = jit.operand(payloadOperand);
        break;
    case SlotFinisher::CallGetter:
        call.thirdArgumentIn = jit.operand(firstOperandOfFinisher);
        break;
    case SlotFinisher::CallSetterStrict:
        setUpSetterCall(jit, PutMode::Strict, call);
        break;
    case SlotFinisher::CallSetterSloppy:
        setUpSetterCall(jit, PutMode::Sloppy, call);
        break;
    case SlotFinisher::StoreField:
    case SlotFinisher::CreateField:
    case SlotFinisher::GrowStorage:
        call.thirdArgumentIn = jit.operand(headerOperand);
        call.fourthArgumentIn = jit.operand(payloadOperand);
        call.fifthArgumentIn = jit.operand(firstOperandOfFinisher);
        break;
    }
    jit.exitOnCannot(finisherCall.entry, call);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
