/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsPropertyProbes.h"

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTClosureWord.h"
#include "AOTCompiledStructure.h"
#include "AOTLayoutData.h"
#include "AOTRuntimeRecords.h"
#include "AOTStubProgramTables.h"
#include "AOTStubsClosures.h"

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

// A field in the cell whose closure is not made yet. The record of the field says which function it is a closure of and whether it captures the object, and the stub that serves the reads by slot makes it.
// Falls through with every operand as it came where the operation has to look.
static void emitMakeClosureOfFieldAndReturn(StubAssembler& jit)
{
    constexpr unsigned fieldSizeShift = WTF::fastLog2(static_cast<unsigned>(sizeof(FieldData)));
    static_assert(sizeof(FieldData) == 1u << fieldSizeShift && !uncreatedDeferredClosure && numberOfSlimClosureKinds == 2);
    if (jit.options().useWideReferences || jit.key().callerFrame() == CallerFrame::Frameless)
        return;

    const GPRReg receiver = jit.operand(receiverOperand);
    const GPRReg header = jit.operand(headerOperand);
    const GPRReg payload = jit.operand(payloadOperand);
    const GPRReg word = jit.allocateScratch(StubRegister::T9);
    const GPRReg object = signatureOfMakeClosureOfField(makeClosureOfField(0, true)).operands[1].gpr();
    RELEASE_ASSERT(receiver == jit.result() && noOverlap(receiver, header, payload, word, object));
    RELEASE_ASSERT(signatureOfMakeClosureOfField(makeClosureOfField(0, true)).operands[0] == Reg(word));
    JumpList callsOperation;

    jit.urshift64(header, TrustedImm32(Slot::kindShiftInHeader), word);
    jit.and32(TrustedImm32(0xff | Slot::isOutOfLine << (Slot::flagsShiftInHeader - Slot::kindShiftInHeader)), word);
    callsOperation.append(jit.branch32(CCallHelpers::NotEqual, word, TrustedImm32(static_cast<int32_t>(Slot::Kind::Field))));
    jit.urshift32(payload, TrustedImm32(SlotData::offsetBits), word);
    callsOperation.append(jit.branch32(CCallHelpers::NotEqual, word, TrustedImm32(static_cast<int32_t>(SlotRepresentation::DeferredClosure))));
    jit.and32(TrustedImm32(SlotData::maxOffset), payload, word);
    callsOperation.append(jit.branchTest32(CCallHelpers::NonZero, BaseIndex(receiver, word, CCallHelpers::TimesOne)));

    jit.emitFunctionPrologue();
    jit.preserving({ header, payload, object }, [&] {
        const GPRReg layout = jit.clobberOperand(headerOperand);
        const GPRReg field = jit.clobberOperand(payloadOperand);
        const GPRReg count = jit.clobberOperand(receiverOperand);
        auto inField = [&](ptrdiff_t offset) {
            return Address(field, static_cast<int32_t>(sizeof(LayoutData) + offset));
        };
        JumpList cannot;
        JumpList isMade;

        jit.move(receiver, object);
        jit.zeroExtend32ToWord(payload, word);
        loadStructure(jit, object, layout);
        jit.loadPtr(Address(layout, CompiledStructure::offsetOfLayout()), layout);
        jit.load16(Address(layout, OBJECT_OFFSETOF(LayoutData, numberOfFields)), count);
        jit.move(layout, field);
        Label next = jit.label();
        cannot.append(jit.branchTest32(CCallHelpers::Zero, count));
        Jump isFound = jit.branch32(CCallHelpers::Equal, inField(OBJECT_OFFSETOF(FieldData, slot)), word);
        jit.addPtr(TrustedImm32(sizeof(FieldData)), field);
        jit.sub32(TrustedImm32(1), count);
        jit.jump().linkTo(next, &jit);

        isFound.link(&jit);
        const GPRReg flags = count;
        const GPRReg function = word;
        const GPRReg kind = layout;
        jit.load16(inField(OBJECT_OFFSETOF(FieldData, flags)), flags);
        cannot.append(jit.branchTest32(CCallHelpers::NonZero, flags, TrustedImm32(fieldIsOutOfLine | fieldIsAccessor | fieldDeferredClosureCapturesEnvironment)));
        jit.load16(Address(layout, OBJECT_OFFSETOF(LayoutData, numberOfFields)), function);
        jit.lshift32(TrustedImm32(fieldSizeShift), function);
        jit.addPtr(function, layout);
        jit.load16(inField(OBJECT_OFFSETOF(FieldData, operandIndex)), function);
        jit.load32(BaseIndex(layout, function, CCallHelpers::TimesFour, sizeof(LayoutData)), function);
        cannot.append(jit.branch32(CCallHelpers::Above, function, TrustedImm32(ClosureWord::functionMask)));
        loadKindOfClosureFunction(jit, function, kind);
        cannot.append(jit.branchTest32(CCallHelpers::Zero, kind, TrustedImm32(slimFunctionKindBit())));

        const GPRReg offset = field;
        jit.load32(inField(OBJECT_OFFSETOF(FieldData, slot)), offset);
        jit.and32(TrustedImm32(SlotData::maxOffset), offset);
        jit.lshift64(TrustedImm32(32), offset);
        jit.or64(offset, function);
        auto callStubFor = [&](unsigned slimClosureKind) {
            Jump capturesObject = jit.branchTest32(CCallHelpers::NonZero, flags, TrustedImm32(fieldDeferredClosureCapturesObject));
            jit.callStub(makeClosureOfField(slimClosureKind, false));
            isMade.append(jit.jump());
            capturesObject.link(&jit);
            jit.callStub(makeClosureOfField(slimClosureKind, true));
            isMade.append(jit.jump());
        };
        Jump isAsync = jit.branchTest32(CCallHelpers::NonZero, kind, TrustedImm32(asyncFunctionKindBit()));
        callStubFor(0);
        isAsync.link(&jit);
        callStubFor(1);

        cannot.link(&jit);
        jit.move(object, receiver);
        jit.move(TrustedImm32(0), word);
        Jump isLeftToOperation = jit.jump();
        isMade.link(&jit);
        jit.move(TrustedImm32(1), word);
        isLeftToOperation.link(&jit);
    });
    jit.emitFunctionEpilogue();
    callsOperation.append(jit.branchTest32(CCallHelpers::Zero, word));
    jit.returnToCaller();

    callsOperation.link(&jit);
}

void generateSlotCannot(StubAssembler& jit)
{
    const SlotFinisher finisher = static_cast<SlotFinisher>(jit.key().form());
    const FinisherCall finisherCall = finisherCallOf(finisher);
    if (finisher == SlotFinisher::ReadField)
        emitMakeClosureOfFieldAndReturn(jit);
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
