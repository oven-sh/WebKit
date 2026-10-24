/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsPropertyProbes.h"

#if ENABLE(AOT)

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
    unsigned reasonArgument;
};

static constexpr FinisherCall finisherCallOf(SlotFinisher finisher)
{
    switch (finisher) {
    case SlotFinisher::ReadField:
        return { Entry::operationAOTReadField, true, false, false, 4 };
    case SlotFinisher::CallGetter:
        return { Entry::operationAOTCallGetterOfSlot, true, true, false, 3 };
    case SlotFinisher::StoreField:
        return { Entry::operationAOTStoreField, false, false, true, 5 };
    case SlotFinisher::CreateField:
        return { Entry::operationAOTCreateField, false, false, true, 5 };
    case SlotFinisher::CallSetterStrict:
    case SlotFinisher::CallSetterSloppy:
        return { Entry::operationAOTCallSetterOfSlot, false, true, true, 5 };
    case SlotFinisher::GrowStorage:
        return { Entry::operationAOTAddPropertyBySlot, false, false, true, 5 };
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

static void setUpSetterCall(StubAssembler& jit, PutMode mode, OperationCall& call)
{
    // On x86-64 the GetterSetter arrives in the register of the fifth argument. The constants are set after the registers are moved.
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
    jit.exitOnCannot(finisherCall.entry, reasonOf(finisher), operationArgumentGPR(finisherCall.reasonArgument), call);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
