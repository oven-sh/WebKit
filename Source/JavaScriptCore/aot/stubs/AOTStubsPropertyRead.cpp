/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsPropertyProbes.h"

#if ENABLE(AOT)

#include "AOTPropertyCachesFront.h"
#include "AOTStubsField.h"
#include "JSObject.h"
#include <wtf/Box.h>

namespace JSC { namespace AOT {

static_assert(slotNameRegister == StubRegister::T9 && slotHeaderRegister == StubRegister::T12 && slotPayloadRegister == StubRegister::T13);
static_assert(!PackedSlot::offsetOfPayload() && PackedSlot::offsetOfLocation() == sizeof(uint32_t) && sizeof(PackedSlot) == sizeof(uint64_t));

static StubSignature signatureReturningValue(std::initializer_list<StubRegister> operands)
{
    StubSignature signature = StubSignature::withGPRResult({ });
    for (StubRegister operand : operands)
        signature.addOperand(gprOf(operand));
    signature.immediate = gprOf(slotNameRegister);
    return signature;
}

StubSignature signatureOfGetById(StubKey key)
{
    StubSignature signature = signatureReturningValue({ });
    signature.addOperand(key.operandRegister().value_or(gprOf(firstStubOperandRegister)));
    signature.boundImmediates = StubBoundImmediates::NamesRead;
    return signature;
}

StubSignature signatureOfGetByIdAfterFrontProbe(StubKey) { return signatureReturningValue({ firstStubOperandRegister, slotHeaderRegister }); }
StubSignature signatureOfGetByIdHashed(StubKey) { return signatureReturningValue({ firstStubOperandRegister }); }
StubSignature signatureOfInById(StubKey) { return signatureReturningValue({ firstStubOperandRegister }); }

struct ReadProbeRegisters {
    GPRReg R0;
    GPRReg T9;
    GPRReg T10;
    GPRReg T11;
    GPRReg T12;
    GPRReg T13;
    GPRReg T14;
};

static ReadProbeRegisters allocateReadProbeRegisters(StubAssembler& jit)
{
    return {
        .R0 = jit.operand(0),
        .T9 = jit.immediate(),
        .T10 = jit.allocateScratch(StubRegister::T10),
        .T11 = jit.allocateScratch(StubRegister::T11),
        .T12 = jit.allocateScratch(StubRegister::T12),
        .T13 = jit.allocateScratch(StubRegister::T13),
        .T14 = jit.allocateScratch(StubRegister::T14),
    };
}

static void returnOwnInlineValue(StubAssembler& jit, GPRReg receiver, GPRReg cell)
{
    loadOwnInlineValue(jit, receiver, cell, jit.result());
    jit.returnToCaller();
}

struct GetByIdBody {
    StubAssembler& jit;
    ReadProbeRegisters registers;
    CallerFrame callerFrame;
    SlotHandlerTable packedTable;
    SlotHandlerTable unpackedTable;
    Box<JumpList> miss;
};

static Jump emitFrontProbe(GetByIdBody& body)
{
    StubAssembler& jit = body.jit;
    const GPRReg R0 = body.registers.R0;
    const GPRReg name = body.registers.T9;
    const GPRReg T10 = body.registers.T10;
    const GPRReg T11 = body.registers.T11;
    const GPRReg cell = body.registers.T12;
    const GPRReg T13 = body.registers.T13;
    loadStructureIDOfReceiver(jit, R0, T11, *body.miss, [&] {
        JumpList isNotInFieldTable;
        returnFieldByTable(jit, { R0, name, T11, cell, std::nullopt, true, true }, isNotInFieldTable);
        isNotInFieldTable.link(&jit);
    });
    loadFrontCell(jit, SlotAccess::Read, { T11, name, T13, cell, T10 });
    Jump isNotOwnInlineValue = branchUnlessOwnInlineValue(jit, name, cell, T10);
    returnOwnInlineValue(jit, R0, cell);

    isNotOwnInlineValue.link(&jit);
    jit.bindEntry(getByIdAfterFrontProbe().withCallerFrame(body.callerFrame));
    Jump hasOtherName = branchIfCellNameDiffers(jit, name, cell, T10);
#if CPU(ARM64)
    Jump isNotValue = branchIfSlotKindDiffers(jit, { SlotForm::PackedSlot, cell }, Slot::Kind::Value, T10);
    loadValueBySlot(jit, CachedSlot(SlotForm::PackedSlot, Slot::Kind::Value, cell, InvalidGPRReg), R0, T13, T11, R0);
    jit.returnToCaller();

    isNotValue.link(&jit);
#endif
    jumpToSlotHandler(jit, body.packedTable, cell, T10);
    return hasOtherName;
}

static void emitHashedProbe(GetByIdBody& body, Jump hasOtherNameInFront)
{
    StubAssembler& jit = body.jit;
    const GPRReg R0 = body.registers.R0;
    const GPRReg name = body.registers.T9;
    const GPRReg uid = body.registers.T10;
    const GPRReg structureID = body.registers.T11;
    const GPRReg header = body.registers.T12;
    const GPRReg payload = body.registers.T13;
    const GPRReg entry = body.registers.T14;
    hasOtherNameInFront.link(&jit);
    jit.bindEntry(getByIdHashed().withCallerFrame(body.callerFrame));
    loadStructureIDOfReceiver(jit, R0, structureID, *body.miss);
    loadIdentifier(jit, name, uid, *body.miss);
    findHashedSlot(jit, SlotAccess::Read, { structureID, uid, entry, header, payload, payload }, *body.miss);
    jumpToSlotHandler(jit, body.unpackedTable, header, uid);
}

#if CPU(X86_64)
static void emitAlternateReceiverProbe(StubAssembler& jit)
{
    const CallerFrame callerFrame = jit.key().callerFrame();
    const GPRReg receiver = jit.operand(0);
    const GPRReg R0 = jit.result();
    const GPRReg name = jit.immediate();
    const GPRReg scratch = jit.allocateScratch(StubRegister::A1);
    const GPRReg T11 = jit.allocateScratch(StubRegister::T11);
    const GPRReg row = jit.allocateScratch(StubRegister::T13);
    const GPRReg T12 = receiver == gprOf(StubRegister::T12) ? jit.clobberOperand(0) : jit.allocateScratch(StubRegister::T12);
    const GPRReg structureID = T11;
    const GPRReg cell = T11;
    RELEASE_ASSERT(noOverlap(receiver, name, scratch, T11, row));
    Jump isNotCell = jit.branchIfNotCell(receiver);
    jit.load32(Address(receiver, JSCell::structureIDOffset()), structureID);
    loadFrontRow(jit, SlotAccess::Read, structureID, row, scratch);
    jit.load64(BaseIndex(row, name, CCallHelpers::TimesEight), cell);
    Jump isNotOwnInlineValue = branchUnlessOwnInlineValue(jit, name, cell, scratch);
    returnOwnInlineValue(jit, receiver, cell);

    isNotOwnInlineValue.link(&jit);
    jit.move(receiver, R0);
    jit.move(cell, T12);
    jit.jumpToStub(getByIdAfterFrontProbe().withCallerFrame(callerFrame));

    isNotCell.link(&jit);
    jit.move(receiver, R0);
    jit.jumpToStub(getById().withCallerFrame(callerFrame));
}
#endif

static void emitBoundNameProbe(StubAssembler& jit, uint32_t number)
{
    RELEASE_ASSERT(RowDisplacementTable::hasColumn(number));
    const CallerFrame callerFrame = jit.key().callerFrame();
    const GPRReg R0 = jit.operand(0);
    const GPRReg T9 = jit.reuseScratch(StubRegister::T9);
    const GPRReg T10 = jit.allocateScratch(StubRegister::T10);
    const GPRReg T11 = jit.allocateScratch(StubRegister::T11);
    const GPRReg T12 = jit.allocateScratch(StubRegister::T12);
    const GPRReg row = jit.allocateScratch(StubRegister::T13);
    auto inCell = [&](ptrdiff_t offset) { return Address(row, static_cast<int32_t>(number * sizeof(PackedSlot) + offset)); };
    JumpList isNotInFieldTable;
    returnFieldByTable(jit, { R0, InvalidGPRReg, T11, T12, number }, isNotInFieldTable);
    isNotInFieldTable.link(&jit);
#if CPU(X86_64)
    const uint32_t upperHalf = PackedSlot::upperHalfForOwnInlineValue(static_cast<uint16_t>(number));
    Jump isNotCell = jit.branchIfNotCell(R0);
    jit.load32(Address(R0, JSCell::structureIDOffset()), T11);
    loadFrontRow(jit, SlotAccess::Read, T11, row, T10);
    Jump isNotOwnInlineValue = jit.branch32(CCallHelpers::NotEqual, inCell(PackedSlot::offsetOfLocation()), TrustedImm32(static_cast<int32_t>(upperHalf)));
    jit.load32(inCell(PackedSlot::offsetOfPayload()), row);
    jit.load64(BaseIndex(R0, row, CCallHelpers::TimesEight), R0);
    jit.returnToCaller();

    isNotOwnInlineValue.link(&jit);
    jit.load64(inCell(0), T12);
    jit.move(TrustedImm32(number), T9);
    jit.jumpToStub(getByIdAfterFrontProbe().withCallerFrame(callerFrame));

    isNotCell.link(&jit);
    jit.move(TrustedImm32(number), T9);
    jit.jumpToStub(getById().withCallerFrame(callerFrame));
#else
    jit.move(TrustedImm32(number), T9);
    jit.linkToStub(jit.branchIfNotCell(R0), getById().withCallerFrame(callerFrame));
    jit.load32(Address(R0, JSCell::structureIDOffset()), T11);
    loadFrontRow(jit, SlotAccess::Read, T11, row, T10);
    jit.load64(inCell(0), T12);
    jit.linkToStub(branchUnlessOwnInlineValue(jit, T9, T12, T10), getByIdAfterFrontProbe().withCallerFrame(callerFrame));
    returnOwnInlineValue(jit, R0, T12);
#endif
}

static bool tryEmitSpecializedProbe(StubAssembler& jit)
{
    if (std::optional<uint32_t> number = jit.key().immediate()) {
        emitBoundNameProbe(jit, *number);
        return true;
    }
#if CPU(X86_64)
    if (jit.key().operandRegister()) {
        emitAlternateReceiverProbe(jit);
        return true;
    }
#endif
    return false;
}

void generateGetById(StubAssembler& jit)
{
    if (tryEmitSpecializedProbe(jit))
        return;
    const CallerFrame callerFrame = jit.key().callerFrame();
    const SlotOrigin origin = callerFrame == CallerFrame::StubFrame ? SlotOrigin::ByIdInStubFrame : SlotOrigin::ById;
    GetByIdBody body {
        jit,
        allocateReadProbeRegisters(jit),
        callerFrame,
        slotHandlerTable(SlotOperation::Read, origin, SlotForm::PackedSlot),
        slotHandlerTable(SlotOperation::Read, origin, SlotForm::Slot),
        Box<JumpList>::create(),
    };
    Jump hasOtherNameInFront = emitFrontProbe(body);
    emitHashedProbe(body, hasOtherNameInFront);
    // loadStructureIDOfReceiver adds to the list in an out-of-line part, which runs after this function has returned.
    jit.addOutOfLinePart([miss = body.miss, table = body.unpackedTable](StubAssembler& jit) {
        linkToSlotExit(jit, *miss, table, SlotExit::Miss);
    });
}

static void emitPresenceBySlot(StubAssembler& jit, PresenceQuestion question, const ProbedSlot& probed, const ReadProbeRegisters& registers, JumpList& isUnknown, JumpList& miss)
{
    const GPRReg R0 = registers.R0;
    Jump isNotField = branchIfSlotKindDiffers(jit, probed, Slot::Kind::Field, registers.T10);
    returnFieldPresence(jit, CachedSlot(probed.form, Slot::Kind::Field, probed.header, probed.payload), { R0, registers.T10, registers.T11, registers.T11, registers.T14 }, miss);

    isNotField.link(&jit);
    returnPresenceBySlot(jit, question, probed, R0, isUnknown);
}

static Entry cachingOperationFor(PresenceQuestion question)
{
    switch (question) {
    case PresenceQuestion::In:
        return Entry::operationAOTInByIdAndCache;
    case PresenceQuestion::HasOwn:
        return Entry::operationAOTHasOwnByIdAndCache;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void generateInById(StubAssembler& jit)
{
    const PresenceQuestion question = static_cast<PresenceQuestion>(jit.key().form());
    const ReadProbeRegisters registers = allocateReadProbeRegisters(jit);
    const GPRReg R0 = registers.R0;
    const GPRReg name = registers.T9;
    const GPRReg T10 = registers.T10;
    const GPRReg structureID = registers.T11;
    const GPRReg T12 = registers.T12;
    const GPRReg T13 = registers.T13;
    const GPRReg T14 = registers.T14;
    JumpList isNotInFront;
    JumpList miss;
    JumpList isNotInFieldTable;
    miss.append(jit.branchIfNotCell(R0));
    readFieldByTable(jit, { R0, name, T12, T13, std::nullopt, false, true }, T12, isNotInFieldTable, [&] {
        jit.move(TrustedImm32(1), R0);
        jit.returnToCaller();
    });
    isNotInFieldTable.link(&jit);
    miss.append(jit.branchIfNotObject(R0));
    jit.load32(Address(R0, JSCell::structureIDOffset()), structureID);
    isNotInFront.append(jit.branch32(CCallHelpers::Above, name, TrustedImm32(RowDisplacementTable::maxColumn)));
    loadFrontCell(jit, SlotAccess::Read, { structureID, name, T13, T12, T10 });
    isNotInFront.append(branchIfCellNameDiffers(jit, name, T12, T10));
    // A reserved cell has the name and no slot, and a slot that does not pack is in the hashed table.
    emitPresenceBySlot(jit, question, { SlotForm::PackedSlot, T12 }, registers, isNotInFront, miss);

    isNotInFront.link(&jit);
    loadIdentifier(jit, name, T10, miss);
    findHashedSlot(jit, SlotAccess::Read, { structureID, T10, T14, T12, T13, T13 }, miss);
    emitPresenceBySlot(jit, question, { SlotForm::Slot, T12, T13 }, registers, miss, miss);

    miss.link(&jit);
    jit.exitOnMiss(cachingOperationFor(question), { .saves = OperationCall::Saves::CallerSavedRegisters, .secondArgumentIn = R0, .thirdArgumentIn = name });
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
