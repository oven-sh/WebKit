/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsPropertyProbes.h"

#if ENABLE(AOT)

#include "AOTPropertyCachesFront.h"
#include "AOTStubsBarriers.h"
#include "JSObject.h"

namespace JSC { namespace AOT {

static_assert(!PackedSlot::offsetOfPayload() && PackedSlot::offsetOfLocation() == sizeof(uint32_t) && sizeof(PackedSlot) == sizeof(uint64_t));

#if CPU(ARM64)
static constexpr unsigned locationShiftInCell = 8 * PackedSlot::offsetOfLocation();
static constexpr unsigned kindAndFlagsShiftInCell = 8 * PackedSlot::offsetOfBiasedKindAndFlags();
#endif

static PutMode modeOf(StubKey key)
{
    return static_cast<PutMode>(key.form());
}

static StubSignature signatureWith(std::initializer_list<StubRegister> operands)
{
    StubSignature signature;
    for (StubRegister operand : operands)
        signature.addOperand(gprOf(operand));
    signature.immediate = gprOf(slotNameRegister);
    return signature;
}

StubSignature signatureOfPutById(StubKey key)
{
    StubSignature signature = signatureWith({ firstStubOperandRegister, storedValueByIdRegister });
    signature.boundImmediates = modeOf(key) == PutMode::Define ? StubBoundImmediates::NamesDefined : StubBoundImmediates::NamesStored;
    return signature;
}

StubSignature signatureOfPutByIdAfterFrontProbe(StubKey)
{
    return signatureWith({ firstStubOperandRegister, storedValueByIdRegister, slotHeaderRegister });
}

StubSignature signatureOfPutByIdHashed(StubKey)
{
    return signatureWith({ firstStubOperandRegister, storedValueByIdRegister });
}

StubSignature signatureOfPutByIdBehindFieldTable(StubKey)
{
    return signatureWith({ firstStubOperandRegister, storedValueByIdRegister });
}

struct PutByIdBody {
    SlotHandlerTable handlerTable(SlotForm form) const { return slotHandlerTable(operationFor(mode), SlotOrigin::ById, form); }

    StubAssembler& jit;
    PutMode mode;
    GPRReg R0;
    GPRReg A1;
    GPRReg T9;
    GPRReg T10;
    GPRReg T11;
    GPRReg T12;
    GPRReg T13;
    GPRReg T14;
    JumpList miss;
};

static PutByIdBody createPutByIdBody(StubAssembler& jit)
{
    return {
        .jit = jit,
        .mode = modeOf(jit.key()),
        .R0 = jit.operand(0),
        .A1 = jit.operand(1),
        .T9 = jit.reuseScratch(slotNameRegister),
        .T10 = jit.allocateScratch(StubRegister::T10),
        .T11 = jit.allocateScratch(StubRegister::T11),
        .T12 = jit.allocateScratch(slotHeaderRegister),
        .T13 = jit.allocateScratch(slotPayloadRegister),
        .T14 = jit.allocateScratch(StubRegister::T14),
        .miss = { },
    };
}

static void storeAndReturn(PutByIdBody& body, BaseIndex location)
{
    StubAssembler& jit = body.jit;
    const GPRReg A1 = body.A1;
    jit.store64(A1, location);
    jit.linkToStub(jit.branchIfCell(A1), writeBarrier());
    jit.returnToCaller();
}

static void storeInlineByCell(PutByIdBody& body)
{
    const GPRReg R0 = body.R0;
    const GPRReg cell = body.T12;
#if CPU(ARM64)
    storeAndReturn(body, BaseIndex(R0, cell, CCallHelpers::TimesEight, 0, CCallHelpers::Extend::ZExt32));
#else
    body.jit.zeroExtend32ToWord(cell, cell);
    storeAndReturn(body, BaseIndex(R0, cell, CCallHelpers::TimesEight));
#endif
}

static void emitPutByBoundName(PutByIdBody& body, uint32_t number)
{
    StubAssembler& jit = body.jit;
    const GPRReg R0 = body.R0;
    const GPRReg T9 = body.T9;
    const GPRReg T10 = body.T10;
    const GPRReg structureID = body.T11;
    const GPRReg cell = body.T12;
    const GPRReg row = body.T13;
    RELEASE_ASSERT(RowDisplacementTable::hasColumn(number));
    const TrustedImm32 upperHalfOfOwnInlineValue(static_cast<int32_t>(PackedSlot::upperHalfForOwnInlineValue(static_cast<uint16_t>(number))));
    auto inCell = [&](ptrdiff_t offset) { return Address(row, static_cast<int32_t>(number * sizeof(PackedSlot) + offset)); };
    JumpList isNotInFieldTable;
    Jump isNotCell = jit.branchIfNotCell(R0);
    storeFieldByTable(jit, { R0, InvalidGPRReg, structureID, cell, number, true, true }, body.A1, row, operationFor(body.mode), isNotInFieldTable);
    isNotInFieldTable.link(&jit);
    jit.load32(Address(R0, JSCell::structureIDOffset()), structureID);
    loadFrontRow(jit, accessFor(body.mode), structureID, row, T10);
#if CPU(X86_64)
    const GPRReg wordIndex = cell;
    Jump isNotOwnInlineValue = jit.branch32(CCallHelpers::NotEqual, inCell(PackedSlot::offsetOfLocation()), upperHalfOfOwnInlineValue);
    jit.load32(inCell(PackedSlot::offsetOfPayload()), wordIndex);
    storeAndReturn(body, BaseIndex(R0, wordIndex, CCallHelpers::TimesEight));

    isNotOwnInlineValue.link(&jit);
    jit.load64(inCell(0), cell);
#else
    const GPRReg upperHalf = T10;
    jit.load64(inCell(0), cell);
    jit.urshift64(cell, TrustedImm32(locationShiftInCell), upperHalf);
    Jump isNotOwnInlineValue = jit.branch32(CCallHelpers::NotEqual, upperHalf, upperHalfOfOwnInlineValue);
    storeInlineByCell(body);

    isNotOwnInlineValue.link(&jit);
#endif
    jit.move(TrustedImm32(number), T9);
    jit.jumpToStub(putByIdAfterFrontProbe(body.mode));

    isNotCell.link(&jit);
    jit.move(TrustedImm32(number), T9);
    jit.jumpToStub(slotExit(body.handlerTable(SlotForm::Slot), SlotExit::Miss));
}

#if CPU(ARM64)
static constexpr int32_t biasedKindAndFlags(Slot::Kind kind, uint8_t packedFlags)
{
    return (static_cast<uint8_t>(kind) | packedFlags) ^ PackedSlot::ownInlineValueBits;
}

static void emitStoresWithoutHandler(PutByIdBody& body)
{
    StubAssembler& jit = body.jit;
    const GPRReg R0 = body.R0;
    const GPRReg A1 = body.A1;
    const GPRReg kindAndFlags = body.T10;
    const GPRReg wordIndex = body.T11;
    const GPRReg cell = body.T12;
    const GPRReg butterfly = body.T13;
    extractBits(jit, cell, kindAndFlagsShiftInCell, 8, kindAndFlags);
    Jump isNotReplacedInStorage = jit.branch32(CCallHelpers::NotEqual, kindAndFlags, TrustedImm32(biasedKindAndFlags(Slot::Kind::Value, PackedSlot::isOwn | PackedSlot::isOutOfLine)));
    jit.loadPtr(Address(R0, JSObject::butterflyOffset()), butterfly);
    storeAndReturn(body, BaseIndex(butterfly, cell, CCallHelpers::TimesEight, 0, CCallHelpers::Extend::SExt32));

    isNotReplacedInStorage.link(&jit);
    Jump isNotAddedInline = jit.branch32(CCallHelpers::NotEqual, kindAndFlags, TrustedImm32(biasedKindAndFlags(Slot::Kind::Transition, PackedSlot::isOwn)));
    extractBits(jit, cell, locationShiftInCell, 8, wordIndex);
    jit.store64(A1, BaseIndex(R0, wordIndex, CCallHelpers::TimesEight));
    jit.store32(cell, Address(R0, JSCell::structureIDOffset()));
    jit.jumpToStub(writeBarrier());

    isNotAddedInline.link(&jit);
}
#endif

static Jump emitFrontProbe(PutByIdBody& body)
{
    StubAssembler& jit = body.jit;
    const GPRReg R0 = body.R0;
    const GPRReg number = body.T9;
    const GPRReg T10 = body.T10;
    const GPRReg structureID = body.T11;
    const GPRReg cell = body.T12;
    const GPRReg row = body.T13;
    JumpList isNotInFieldTable;
    body.miss.append(jit.branchIfNotCell(R0));
    storeFieldByTable(jit, { R0, number, structureID, cell, std::nullopt, true, true }, body.A1, row, operationFor(body.mode), isNotInFieldTable);
    isNotInFieldTable.link(&jit);
    // The receiver is a cell.
    jit.bindEntry(putByIdBehindFieldTable(body.mode));
    jit.load32(Address(R0, JSCell::structureIDOffset()), structureID);
    loadFrontCell(jit, accessFor(body.mode), { structureID, number, row, cell, T10 });
    Jump isNotOwnInlineValue = branchUnlessOwnInlineValue(jit, number, cell, T10);
    storeInlineByCell(body);

    isNotOwnInlineValue.link(&jit);
    jit.bindEntry(putByIdAfterFrontProbe(body.mode));
    Jump hasOtherName = branchIfCellNameDiffers(jit, number, cell, T10);
#if CPU(ARM64)
    emitStoresWithoutHandler(body);
#endif
    jumpToSlotHandler(jit, body.handlerTable(SlotForm::PackedSlot), cell, T10);
    return hasOtherName;
}

static void emitHashedProbe(PutByIdBody& body, Jump hasOtherNameInFront)
{
    StubAssembler& jit = body.jit;
    const GPRReg R0 = body.R0;
    const GPRReg number = body.T9;
    const GPRReg uid = body.T10;
    const GPRReg structureID = body.T11;
    const GPRReg header = body.T12;
    const GPRReg payload = body.T13;
    const GPRReg entry = body.T14;
    jit.bindEntry(putByIdHashed(body.mode));
    body.miss.append(jit.branchIfNotCell(R0));
    hasOtherNameInFront.link(&jit);
    jit.load32(Address(R0, JSCell::structureIDOffset()), structureID);
    loadIdentifier(jit, number, uid, body.miss);
#if CPU(X86_64)
    JumpList isInNoTable;
    findHashedSlot(jit, accessFor(body.mode), { structureID, uid, entry, header, payload, payload }, isInNoTable);
    jumpToSlotHandler(jit, body.handlerTable(SlotForm::Slot), header, uid);

    // No table has the Structure and the name. A value that the object itself has, plain and writable, is replaced where it lies, and nothing is filed:
    // so a pair that comes again and again leaves now and then for the operation that files. The search has put the access into the low bits of the name, which tells the pair from that of a read.
    isInNoTable.link(&jit);
    countMissOfPair(jit, structureID, uid, header, body.miss);
    jit.xor64(TrustedImm32(static_cast<int32_t>(accessFor(body.mode))), uid);
    replaceOwnValueAndReturn(jit, operationFor(body.mode), {
        .receiver = R0,
        .name = number,
        .uid = uid,
        .object = header,
        .structure = structureID,
        .size = payload,
        .vector = entry,
        .index = jit.scratchRegister(),
    }, ObjectLookupName::IsGivenByNumber, body.A1, body.miss);
#else
    findHashedSlot(jit, accessFor(body.mode), { structureID, uid, entry, header, payload, payload }, body.miss);
    jumpToSlotHandler(jit, body.handlerTable(SlotForm::Slot), header, uid);
#endif
}

void generatePutById(StubAssembler& jit)
{
    PutByIdBody body = createPutByIdBody(jit);
    if (std::optional<uint32_t> number = jit.key().immediate()) {
        emitPutByBoundName(body, *number);
        return;
    }
    Jump hasOtherNameInFront = emitFrontProbe(body);
    emitHashedProbe(body, hasOtherNameInFront);
    linkToSlotExit(jit, body.miss, body.handlerTable(SlotForm::Slot), SlotExit::Miss);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
