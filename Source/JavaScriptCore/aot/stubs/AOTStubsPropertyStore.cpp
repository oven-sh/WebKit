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

#if CPU(ARM64)
static constexpr int32_t biasedKindAndFlags(Slot::Kind kind, uint8_t packedFlags)
{
    return (static_cast<uint8_t>(kind) | packedFlags) ^ PackedSlot::ownInlineValueBits;
}
#endif

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

    if (std::optional<uint32_t> boundNumber = jit.key().immediate()) {
        const uint32_t number = *boundNumber;
        RELEASE_ASSERT(RowDisplacementTable::hasColumn(number));
        const TrustedImm32 upperHalfOfOwnInlineValue(static_cast<int32_t>(PackedSlot::upperHalfForOwnInlineValue(static_cast<uint16_t>(number))));
        auto inCell = [&](ptrdiff_t offset) { return Address(row, static_cast<int32_t>(number * sizeof(PackedSlot) + offset)); };
        JumpList isNotInFieldTable;
        Jump isNotCell = jit.branchIfNotCell(R0);
        storeFieldByTable(jit, { R0, InvalidGPRReg, structureID, cell, number, true, true }, A1, row, operationFor(mode), isNotInFieldTable);
        isNotInFieldTable.link(&jit);
        jit.load32(Address(R0, JSCell::structureIDOffset()), structureID);
        loadFrontRow(jit, accessFor(mode), structureID, row, T10);
#if CPU(X86_64)
        const GPRReg wordIndex = cell;
        Jump isNotOwnInlineValue = jit.branch32(CCallHelpers::NotEqual, inCell(PackedSlot::offsetOfLocation()), upperHalfOfOwnInlineValue);
        jit.load32(inCell(PackedSlot::offsetOfPayload()), wordIndex);
        storeAndReturn(BaseIndex(R0, wordIndex, CCallHelpers::TimesEight));

        isNotOwnInlineValue.link(&jit);
        jit.load64(inCell(0), cell);
#else
        const GPRReg upperHalf = T10;
        jit.load64(inCell(0), cell);
        jit.urshift64(cell, TrustedImm32(locationShiftInCell), upperHalf);
        Jump isNotOwnInlineValue = jit.branch32(CCallHelpers::NotEqual, upperHalf, upperHalfOfOwnInlineValue);
        storeInlineByCell();

        isNotOwnInlineValue.link(&jit);
#endif
        jit.move(TrustedImm32(number), T9);
        jit.jumpToStub(putByIdAfterFrontProbe(mode));

        isNotCell.link(&jit);
        jit.move(TrustedImm32(number), T9);
        jit.jumpToStub(slotExit(handlerTable(SlotForm::Slot), SlotExit::Miss));
        return;
    }

    const GPRReg number = T9;
    JumpList isNotInFieldTable;
    miss.append(jit.branchIfNotCell(R0));
    storeFieldByTable(jit, { R0, number, structureID, cell, std::nullopt, true, true }, A1, row, operationFor(mode), isNotInFieldTable);
    isNotInFieldTable.link(&jit);
    // The table of fields takes names that have no column in the front table.
    Jump hasNoFrontColumn = jit.branch32(CCallHelpers::Above, number, TrustedImm32(RowDisplacementTable::maxColumn));
    // The receiver is a cell.
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
    miss.append(jit.branchIfNotCell(R0));
    hasOtherName.link(&jit);
    hasNoFrontColumn.link(&jit);
    jit.load32(Address(R0, JSCell::structureIDOffset()), structureID);
    loadIdentifier(jit, number, uid, miss);
    JumpList isInNoTable;
    findHashedSlot(jit, accessFor(mode), { structureID, uid, entry, header, payload, payload }, isInNoTable);
    jumpToSlotHandler(jit, handlerTable(SlotForm::Slot), header, uid);

    // No table has the Structure and the name.
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
        .index = indexRegisterOfObjectLookup(jit),
    }, ObjectLookupName::IsGivenByNumber, A1, miss);
    linkToSlotExit(jit, miss, handlerTable(SlotForm::Slot), SlotExit::Miss);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
