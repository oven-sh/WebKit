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

static constexpr unsigned kindAndFlagsShiftInCell = 8 * PackedSlot::offsetOfBiasedKindAndFlags();

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

StubSignature signatureOfPutById(StubKey)
{
    return signatureWith({ firstStubOperandRegister, storedValueByIdRegister });
}

StubSignature signatureOfPutByIdAfterFrontProbe(StubKey)
{
    return signatureWith({ firstStubOperandRegister, storedValueByIdRegister, slotHeaderRegister });
}

StubSignature signatureOfPutByIdHashed(StubKey)
{
    return signatureWith({ firstStubOperandRegister, storedValueByIdRegister });
}

StubSignature signatureOfPutByIdOfSymbolOfRealm(StubKey)
{
    return signatureWith({ firstStubOperandRegister, storedValueByIdRegister });
}

StubSignature signatureOfPutByIdBehindFieldTable(StubKey)
{
    return signatureWith({ firstStubOperandRegister, storedValueByIdRegister });
}

static constexpr int32_t biasedKindAndFlags(Slot::Kind kind, uint8_t packedFlags)
{
    return (static_cast<uint8_t>(kind) | packedFlags) ^ PackedSlot::ownInlineValueBits;
}

void generatePutById(StubAssembler& jit)
{
    const PutMode mode = modeOf(jit.key());
    const GPRReg R0 = jit.operand(0);
    const GPRReg A1 = jit.operand(1);
    const GPRReg T9 = jit.reuseScratch(slotNameRegister);
    const GPRReg T10 = jit.allocateScratch(StubRegister::T10);
    const GPRReg T11 = jit.allocateScratch(StubRegister::T11);
    const GPRReg T12 = jit.allocateScratch(slotHeaderRegister);
    const GPRReg T13 = jit.allocateScratch(slotPayloadRegister);
    const GPRReg T14 = jit.allocateScratch(StubRegister::T14);
    const GPRReg structureID = T11;
    const GPRReg cell = T12;
    const GPRReg row = T13;
    JumpList miss;

    auto handlerTable = [&](SlotForm form) {
        return slotHandlerTable(operationFor(mode), SlotOrigin::ById, form);
    };
    auto storeAndReturn = [&](BaseIndex location) {
        jit.store64(A1, location);
        jit.linkToStub(jit.branchIfCell(A1), writeBarrier());
        jit.returnToCaller();
    };
    auto storeInlineByCell = [&] {
#if CPU(ARM64)
        storeAndReturn(BaseIndex(R0, cell, CCallHelpers::TimesEight, 0, CCallHelpers::Extend::ZExt32));
#else
        jit.zeroExtend32ToWord(cell, cell);
        storeAndReturn(BaseIndex(R0, cell, CCallHelpers::TimesEight));
#endif
    };
#if CPU(ARM64)
    auto emitStoresWithoutHandler = [&] {
        const GPRReg kindAndFlags = T10;
        const GPRReg wordIndex = T11;
        const GPRReg butterfly = T13;
        extractBits(jit, cell, kindAndFlagsShiftInCell, 8, kindAndFlags);
        Jump isNotReplacedInStorage = jit.branch32(CCallHelpers::NotEqual, kindAndFlags, TrustedImm32(biasedKindAndFlags(Slot::Kind::Value, PackedSlot::isOwn | PackedSlot::isOutOfLine)));
        jit.loadPtr(Address(R0, JSObject::butterflyOffset()), butterfly);
        storeAndReturn(BaseIndex(butterfly, cell, CCallHelpers::TimesEight, 0, CCallHelpers::Extend::SExt32));

        isNotReplacedInStorage.link(&jit);
        Jump isNotAddedInline = jit.branch32(CCallHelpers::NotEqual, kindAndFlags, TrustedImm32(biasedKindAndFlags(Slot::Kind::Transition, PackedSlot::isOwn)));
        extractBits(jit, cell, locationShiftInCell, 8, wordIndex);
        jit.store64(A1, BaseIndex(R0, wordIndex, CCallHelpers::TimesEight));
        jit.store32(cell, Address(R0, JSCell::structureIDOffset()));
        jit.jumpToStub(writeBarrier());

        isNotAddedInline.link(&jit);
    };
#endif

    const GPRReg number = T9;
    JumpList isNotInFieldTable;
    Label start = jit.label();
    miss.append(jit.branchIfNotCell(R0));
    storeFieldByTable(jit, { R0, number, structureID, cell, true, true }, A1, row, operationFor(mode), isNotInFieldTable);
    isNotInFieldTable.link(&jit);
    Jump hasNoFrontColumn = jit.branch32(CCallHelpers::Above, number, TrustedImm32(RowDisplacementTable::maxColumn));
    jit.bindEntry(putByIdBehindFieldTable(mode));
    jit.load32(Address(R0, JSCell::structureIDOffset()), structureID);
    loadFrontCell(jit, accessFor(mode), { structureID, number, row, cell, T10 });
    Jump isNotOwnInlineValue = branchUnlessOwnInlineValue(jit, number, cell, T10);
    storeInlineByCell();

    isNotOwnInlineValue.link(&jit);
    jit.bindEntry(putByIdAfterFrontProbe(mode));
    Jump hasOtherName = branchIfCellNameDiffers(jit, number, cell, T10);
#if CPU(ARM64)
    emitStoresWithoutHandler();
#endif
    jumpToSlotHandler(jit, handlerTable(SlotForm::PackedSlot), cell, T10);

    const GPRReg uid = T10;
    const GPRReg header = T12;
    const GPRReg payload = T13;
    const GPRReg entry = T14;
    jit.bindEntry(putByIdHashed(mode));
    jit.branch32(CCallHelpers::BelowOrEqual, number, TrustedImm32(RowDisplacementTable::maxColumn)).linkTo(start, &jit);
    miss.append(jit.branchIfNotCell(R0));
    hasOtherName.link(&jit);
    hasNoFrontColumn.link(&jit);
    jit.load32(Address(R0, JSCell::structureIDOffset()), structureID);
    loadIdentifier(jit, number, uid, miss);
    JumpList isInNoTable;
    findHashedSlot(jit, accessFor(mode), { structureID, uid, entry, header, payload, payload }, isInNoTable);
    jumpToSlotHandler(jit, handlerTable(SlotForm::Slot), header, uid);

    isInNoTable.link(&jit);
    countMissOfPair(jit, structureID, uid, header, miss);
    jit.xor64(TrustedImm32(static_cast<int32_t>(accessFor(mode))), uid);
    replaceOwnValueAndReturn(jit, operationFor(mode), {
        .receiver = R0,
        .name = number,
        .uid = uid,
        .object = header,
        .structure = structureID,
        .size = payload,
        .vector = entry,
        .index = IndexRegisterOfObjectLookup(jit),
    }, ObjectLookupName::IsGivenByNumber, A1, miss);

    jit.bindEntry(putByIdOfSymbolOfRealm(mode));
    if (jit.options().hasOneInstance)
        jit.breakpoint();
    else {
        JumpList looksUpByUID;
        miss.append(jit.branchIfNotCell(R0));
        loadStructure(jit, R0, T10);
        looksUpByUID.append(branchIfNotOfThisRealm(jit, T10));
        storeFieldByTable(jit, { R0, number, structureID, cell, false, true }, A1, row, operationFor(mode), looksUpByUID);
        looksUpByUID.link(&jit);
        jit.load32(Address(R0, JSCell::structureIDOffset()), structureID);
        loadProgramSymbol(jit, number, uid, miss);
        findHashedSlot(jit, accessFor(mode), { structureID, uid, entry, header, payload, payload }, miss);
        jumpToSlotHandler(jit, handlerTable(SlotForm::Slot), header, uid);
    }
    linkToSlotExit(jit, miss, handlerTable(SlotForm::Slot), SlotExit::Miss);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
