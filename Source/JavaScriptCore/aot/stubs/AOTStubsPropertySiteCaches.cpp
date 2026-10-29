/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsPropertyProbes.h"

#if ENABLE(AOT)

#include "AOTPropertyCachesFront.h"
#include "AOTRuntimeTable.h"
#include "JSCell.h"
#include "JSObject.h"
#include "JSString.h"
#include <bit>
#include <wtf/Box.h>

namespace JSC { namespace AOT {

static_assert(!Slot::offsetOfStructureID() && Slot::offsetOfPayload() == sizeof(uint64_t) && sizeof(Slot) == 2 * sizeof(uint64_t));
static_assert(operationArgumentGPR(4) == gprOf(StubRegister::A4) && operationArgumentGPR(5) == gprOf(StubRegister::A5));

static constexpr unsigned isOutOfLineBitInHeader = Slot::flagsShiftInHeader + std::countr_zero(Slot::isOutOfLine);

enum class SiteReceiver : bool { IsCell, MayBePrimitive };

struct SiteRegisters {
    GPRReg receiver;
    GPRReg site;
    GPRReg identifierNumber;
    GPRReg structureID;
    GPRReg header;
    GPRReg payload;
    GPRReg scratch;
};

// Where the caller of the stub has not said whether the front tables have a column for the name, the number of the name decides between the two.
struct TableStubs {
    StubKey front;
    std::optional<StubKey> hashed;
};

static SiteRegisters allocateSiteRegisters(StubAssembler& jit, unsigned siteOperandIndex)
{
    SiteRegisters registers;
    registers.receiver = jit.operand(0);
    registers.site = jit.operand(siteOperandIndex);
    registers.identifierNumber = jit.immediate();
    registers.structureID = jit.allocateScratch(StubRegister::T11);
    registers.header = jit.allocateScratch(slotHeaderRegister);
    registers.payload = jit.allocateScratch(slotPayloadRegister);
    registers.scratch = jit.allocateScratch(StubRegister::T10);
    return registers;
}

static void loadKindOfSlotAtSite(StubAssembler& jit, const SiteRegisters& registers, GPRReg kind)
{
#if CPU(ARM64)
    jit.extractUnsignedBitfield64(registers.header, TrustedImm32(Slot::kindShiftInHeader), TrustedImm32(8 * sizeof(Slot::Kind)), kind);
#else
    jit.load8(Address(registers.site, Slot::offsetOfKind()), kind);
#endif
}

// An own value has no holder. The result may be the register of the receiver.
static void loadFromValueSlot(StubAssembler& jit, const SiteRegisters& registers, GPRReg result)
{
    const GPRReg header = registers.header;
    const GPRReg holder = registers.payload;
    RELEASE_ASSERT(noOverlap(result, header, holder) && noOverlap(registers.receiver, header, holder));
#if CPU(X86_64)
    jit.m_assembler.testq_rr(holder, holder);
    jit.m_assembler.cmovq_rr(X86Assembler::ConditionE, registers.receiver, holder);
    jit.m_assembler.btw_ir(isOutOfLineBitInHeader, header);
    jit.m_assembler.cmovq_mr(X86Assembler::ConditionC, JSObject::butterflyOffset(), holder, holder);
#elif CPU(ARM64)
    jit.moveConditionallyTest64(CCallHelpers::Zero, holder, holder, registers.receiver, holder);
    jit.loadPtr(Address(holder, JSObject::butterflyOffset()), result);
    jit.m_assembler.tst<64>(header, LogicalImmediate::create64(1ull << isOutOfLineBitInHeader));
    jit.m_assembler.csel<64>(holder, result, holder, ARM64Assembler::ConditionNE);
#endif
    jit.rshift64(TrustedImm32(Slot::locationShiftInHeader), header);
    jit.load64(BaseIndex(holder, header, CCallHelpers::TimesEight), result);
}

// Falls through when the site is empty or has a slot of another Structure.
static void emitSiteProbe(StubAssembler& jit, const SiteRegisters& registers, SlotHandlerTable table)
{
    jit.loadPair64(registers.site, registers.header, registers.payload);
    Jump hasOtherStructure = jit.branch32(CCallHelpers::NotEqual, registers.header, registers.structureID);
    loadKindOfSlotAtSite(jit, registers, registers.scratch);
    if (traitsOf(table).operation == SlotOperation::Read) {
        Jump isNotValue = jit.branch32(CCallHelpers::NotEqual, registers.scratch, TrustedImm32(static_cast<int32_t>(Slot::Kind::Value)));
        loadFromValueSlot(jit, registers, jit.result());
        jit.returnToCaller();
        isNotValue.link(&jit);
    }
    jumpToSlotHandlerOfKind(jit, table, registers.header, registers.scratch);
    hasOtherStructure.link(&jit);
}

// Whoever finds its site empty has asked the table of fields where the caller gives a list for it: it goes on behind that table.
static void emitSiteMiss(StubAssembler& jit, const SiteRegisters& registers, SlotAccess access, SiteReceiver receiver, const TableStubs& stubs, JumpList* hasAskedFieldTable = nullptr)
{
    JumpList isDue;
    Jump isEmpty = jit.branchTest64(CCallHelpers::Zero, registers.header);
    countSiteCacheMiss(jit, registers.site, registers.scratch, isDue);
    Label probeTables = jit.label();
    if (stubs.hashed)
        jit.linkToStub(jit.branch32(CCallHelpers::Above, registers.identifierNumber, TrustedImm32(RowDisplacementTable::maxColumn)), *stubs.hashed);
    jit.jumpToStub(stubs.front);

    isEmpty.link(&jit);
    countMissOfEmptySite(jit, registers.site, registers.scratch, isDue);
    if (hasAskedFieldTable) {
        RELEASE_ASSERT(receiver == SiteReceiver::IsCell);
        hasAskedFieldTable->append(jit.jump());
    } else
        jit.jump().linkTo(probeTables, &jit);

    isDue.link(&jit);
    if (receiver == SiteReceiver::MayBePrimitive)
        jit.branchIfNotCell(registers.receiver).linkTo(probeTables, &jit);
    refillSiteCache(jit, registers.receiver, registers.identifierNumber, registers.site, access);
    jit.jump().linkTo(probeTables, &jit);
}

StubSignature signatureOfGetByIdAtSite(StubKey)
{
    StubSignature signature;
    signature.addOperand(gprOf(firstStubOperandRegister));
    signature.addOperand(gprOf(readSiteCacheRegister));
    signature.result = StubResult::GPR;
    signature.firstResult = gprOf(firstStubOperandRegister);
    signature.immediate = gprOf(slotNameRegister);
    return signature;
}

// A cell asks the site first: an empty site, which has the StructureID of nothing, is told from another Structure's only behind the mismatch.
void generateGetByIdAtSite(StubAssembler& jit)
{
    static constexpr SlotHandlerTable table = SlotHandlerTable::ReadById;
    const bool nameHasColumn = jit.key().family() == StubFamily::GetByIdAtSite;
    const StubKey tables = nameHasColumn ? getById() : getByIdHashed();
    const SiteRegisters registers = allocateSiteRegisters(jit, 1);

    Jump isNotCell = jit.branchIfNotCell(registers.receiver);
    if (nameHasColumn)
        jit.bindEntry(getByIdOfObjectAtSite());
    const GPRReg holder = registers.payload;
#if CPU(X86_64)
    const GPRReg wordIndex = registers.header;
    auto branchOnFlags = [&](X86Assembler::Condition condition) { return Jump(jit.m_assembler.jCC(condition)); };
    // Each word is compared where it lies.
    jit.load64(Address(registers.site, Slot::offsetOfStructureID()), registers.header);
    Jump hasOtherStructure = jit.branch32(CCallHelpers::NotEqual, registers.header, Address(registers.receiver, JSCell::structureIDOffset()));
    // Slot::needsHandler says what this one comparison tells.
    jit.m_assembler.cmpb_im(Slot::isOwn, Slot::offsetOfFlags(), registers.site);
    Jump isNotOwnInlineValue = branchOnFlags(X86Assembler::ConditionNE);
    jit.urshift64(TrustedImm32(Slot::locationShiftInHeader), wordIndex);
    Label readsCellOfReceiver = jit.label();
    jit.load64(BaseIndex(registers.receiver, wordIndex, CCallHelpers::TimesEight), jit.result());
    jit.returnToCaller();

    isNotOwnInlineValue.link(&jit);
    Jump isOwnOrNeedsHandler = branchOnFlags(X86Assembler::ConditionA);
    // A value never lies at index zero, and a name that is not there has no location: `if (options.signal)`, `payload.aborted`.
    jit.rshift64(TrustedImm32(Slot::locationShiftInHeader), wordIndex);
    Jump isAbsent = branchOnFlags(X86Assembler::ConditionE);
    jit.load64(Address(registers.site, Slot::offsetOfPayload()), holder);
    jit.m_assembler.cmovq_mr(X86Assembler::ConditionS, JSObject::butterflyOffset(), holder, holder);
    jit.load64(BaseIndex(holder, wordIndex, CCallHelpers::TimesEight), jit.result());
    jit.returnToCaller();

    isAbsent.link(&jit);
    jit.move(TrustedImm64(JSValue::ValueUndefined), jit.result());
    jit.returnToCaller();

    isOwnOrNeedsHandler.link(&jit);
    Jump isOwnValue = branchOnFlags(X86Assembler::ConditionG);
    jit.load64(Address(registers.site, Slot::offsetOfPayload()), registers.payload);
    loadKindOfSlotAtSite(jit, registers, registers.scratch);
    jumpToSlotHandlerOfKind(jit, table, registers.header, registers.scratch);

    isOwnValue.link(&jit);
    jit.rshift64(TrustedImm32(Slot::locationShiftInHeader), wordIndex);
    branchOnFlags(X86Assembler::ConditionNS).linkTo(readsCellOfReceiver, &jit);
    jit.loadPtr(Address(registers.receiver, JSObject::butterflyOffset()), holder);
    jit.load64(BaseIndex(holder, wordIndex, CCallHelpers::TimesEight), jit.result());
    jit.returnToCaller();
#else
    const GPRReg wordIndex = registers.scratch;
    auto branchOnFlag = [&](CCallHelpers::ResultCondition condition, uint8_t flag) { return jit.branchTest64(condition, registers.header, TrustedImm64(static_cast<int64_t>(flag) << Slot::flagsShiftInHeader)); };
    static_assert(!Slot::offsetOfStructureID() && Slot::offsetOfPayload() == sizeof(uint64_t));
    jit.loadPair64(registers.site, registers.header, registers.payload);
    jit.load32(Address(registers.receiver, JSCell::structureIDOffset()), registers.structureID);
    Jump hasOtherStructure = jit.branch32(CCallHelpers::NotEqual, registers.header, registers.structureID);
    Jump needsHandler = branchOnFlag(CCallHelpers::NonZero, Slot::needsHandler);
    jit.rshift64(registers.header, TrustedImm32(Slot::locationShiftInHeader), wordIndex);
    Jump isNotOwn = branchOnFlag(CCallHelpers::Zero, Slot::isOwn);
    Jump isOutOfLine = branchOnFlag(CCallHelpers::NonZero, Slot::isOutOfLine);
    jit.load64(BaseIndex(registers.receiver, wordIndex, CCallHelpers::TimesEight), jit.result());
    jit.returnToCaller();

    isOutOfLine.link(&jit);
    jit.loadPtr(Address(registers.receiver, JSObject::butterflyOffset()), holder);
    jit.load64(BaseIndex(holder, wordIndex, CCallHelpers::TimesEight), jit.result());
    jit.returnToCaller();

    isNotOwn.link(&jit);
    // A value never lies at index zero, and a name that is not there has no location: `if (options.signal)`, `payload.aborted`.
    Jump isAbsent = jit.branchTest64(CCallHelpers::Zero, wordIndex);
    Jump isInCellOfHolder = branchOnFlag(CCallHelpers::Zero, Slot::isOutOfLine);
    jit.loadPtr(Address(holder, JSObject::butterflyOffset()), holder);
    isInCellOfHolder.link(&jit);
    jit.load64(BaseIndex(holder, wordIndex, CCallHelpers::TimesEight), jit.result());
    jit.returnToCaller();

    isAbsent.link(&jit);
    jit.move(TrustedImm64(JSValue::ValueUndefined), jit.result());
    jit.returnToCaller();

    needsHandler.link(&jit);
    loadKindOfSlotAtSite(jit, registers, registers.scratch);
    jumpToSlotHandlerOfKind(jit, table, registers.header, registers.scratch);
#endif

    hasOtherStructure.link(&jit);
    JumpList isDue;
    Jump isEmpty = jit.branchTest64(CCallHelpers::Zero, registers.header);
    if (nameHasColumn) {
        // Objects of several Structures come to this place. What the front table holds for this one is answered from here: the table of fields, which answers at an empty site, has its turn behind it.
        // Structures often keep a name at one place. The front cell then only confirms the place that the site names, so the value does not wait for the tables, and the site stays as it is.
        JumpList isNotInFront;
#if CPU(X86_64)
        auto countMiss = [&] { countSiteCacheMiss(jit, registers.site, registers.header, isDue); };
        const ScopedLambda<void()> countMissAtSite = countMiss;
        jit.load32(Address(registers.receiver, JSCell::structureIDOffset()), registers.structureID);
        locateFrontCell(jit, SlotAccess::Read, registers.structureID, registers.identifierNumber, registers.scratch);
        returnByFrontCell(jit, registers.receiver, registers.identifierNumber, registers.scratch, registers.structureID, registers.header, isNotInFront, &countMissAtSite);
        jumpToSlotHandler(jit, slotHandlerTable(SlotOperation::Read, SlotOrigin::ById, SlotForm::PackedSlot), registers.header, registers.scratch);

        isNotInFront.link(&jit);
        countMiss();
#else
        const GPRReg cell = registers.header;
        const GPRReg placeAtSite = registers.payload;
        loadFrontCell(jit, SlotAccess::Read, { registers.structureID, registers.identifierNumber, registers.payload, cell, registers.scratch });
        Jump isNotOwnInlineInFront = branchUnlessOwnInlineValue(jit, registers.identifierNumber, cell, registers.scratch);
        jit.load16(Address(registers.site, Slot::offsetOfLocation()), placeAtSite);
        Jump liesElsewhere = jit.branch32(CCallHelpers::NotEqual, cell, placeAtSite);
        jit.load64(BaseIndex(registers.receiver, placeAtSite, CCallHelpers::TimesEight), jit.result());
        jit.returnToCaller();

        liesElsewhere.link(&jit);
        countSiteCacheMiss(jit, registers.site, registers.scratch, isDue);
        loadOwnInlineValue(jit, registers.receiver, cell, jit.result());
        jit.returnToCaller();

        isNotOwnInlineInFront.link(&jit);
        countSiteCacheMiss(jit, registers.site, registers.scratch, isDue);
        isNotInFront.append(branchIfCellNameDiffers(jit, registers.identifierNumber, cell, registers.scratch));
        jit.jumpToStub(getByIdAfterFrontProbe());

        isNotInFront.link(&jit);
#endif
    } else
        countSiteCacheMiss(jit, registers.site, registers.scratch, isDue);
    jit.jumpToStub(tables);

    isEmpty.link(&jit);
    JumpList isNotInFieldTable;
    returnFieldByTable(jit, { registers.receiver, registers.identifierNumber, registers.structureID, registers.header, std::nullopt, nameHasColumn, true }, isNotInFieldTable);
    isNotInFieldTable.link(&jit);
    countMissOfEmptySite(jit, registers.site, registers.scratch, isDue);
    jit.jumpToStub(nameHasColumn ? getByIdBehindFieldTable() : tables);

    isDue.link(&jit);
    refillSiteCache(jit, registers.receiver, registers.identifierNumber, registers.site, SlotAccess::Read);
    jit.jumpToStub(tables);

    jit.addOutOfLinePart([isNotCell, registers, tables](StubAssembler& jit) {
        JumpList hasNoStructure;
        isNotCell.link(&jit);
        Jump isNumber = jit.branchIfNumber(registers.receiver);
        hasNoStructure.append(jit.branchIfNotBoolean(registers.receiver, registers.structureID));
        jit.load32(Address(instanceGPR, AOT_INSTANCE_OFFSET(propertyCaches.booleanStructureID)), registers.structureID);
        Jump hasID = jit.jump();
        isNumber.link(&jit);
        jit.load32(Address(instanceGPR, AOT_INSTANCE_OFFSET(propertyCaches.numberStructureID)), registers.structureID);
        hasID.link(&jit);
        emitSiteProbe(jit, registers, table);
        emitSiteMiss(jit, registers, SlotAccess::Read, SiteReceiver::MayBePrimitive, { tables, std::nullopt });
        linkToSlotExit(jit, hasNoStructure, table, SlotExit::Miss);
    });
}

StubSignature signatureOfGetByIdOfObjectAtSite(StubKey key)
{
    return signatureOfGetByIdAtSite(key);
}

StubSignature signatureOfGetByIdAtSiteHashed(StubKey key)
{
    return signatureOfGetByIdAtSite(key);
}

void generateGetByIdAtSiteHashed(StubAssembler& jit)
{
    generateGetByIdAtSite(jit);
}

StubSignature signatureOfGetLengthAtSite(StubKey key)
{
    return signatureOfGetByIdAtSite(key);
}

// The length of an array and of a string is an own property that nothing redefines, so it is read without a look at the site. Of anything else the name is read as any other.
void generateGetLengthAtSite(StubAssembler& jit)
{
    const GPRReg receiver = jit.operand(0);
    const GPRReg scratch = jit.allocateScratch(StubRegister::T11);
    JumpList isNeither;

    isNeither.append(jit.branchIfNotCell(receiver));
    jit.load8(Address(receiver, JSCell::indexingTypeAndMiscOffset()), scratch);
    Jump isNotArray = jit.branchTest32(CCallHelpers::Zero, scratch, TrustedImm32(IsArray));
    isNeither.append(jit.branchTest32(CCallHelpers::Zero, scratch, TrustedImm32(IndexingShapeMask)));
    jit.loadPtr(Address(receiver, JSObject::butterflyOffset()), scratch);
    jit.load32(Address(scratch, Butterfly::offsetOfPublicLength()), scratch);
    isNeither.append(jit.branch32(CCallHelpers::LessThan, scratch, TrustedImm32(0)));
    boxInt32(jit, scratch, jit.result());
    jit.returnToCaller();

    isNotArray.link(&jit);
    isNeither.append(jit.branch8(CCallHelpers::NotEqual, Address(receiver, JSCell::typeInfoTypeOffset()), TrustedImm32(StringType)));
    jit.loadPtr(Address(receiver, JSString::offsetOfValue()), scratch);
    Jump isRope = jit.branchIfRopeStringImpl(scratch);
    jit.load32(Address(scratch, StringImpl::lengthMemoryOffset()), scratch);
    boxInt32(jit, scratch, jit.result());
    jit.returnToCaller();

    isRope.link(&jit);
    jit.load32(Address(receiver, JSRopeString::offsetOfLength()), scratch);
    boxInt32(jit, scratch, jit.result());
    jit.returnToCaller();

    isNeither.link(&jit);
    jit.jumpToStub(getByIdAtSite());
}

StubSignature signatureOfPutByIdAtSite(StubKey)
{
    StubSignature signature;
    signature.addOperand(gprOf(firstStubOperandRegister));
    signature.addOperand(gprOf(storedValueByIdRegister));
    signature.addOperand(gprOf(storeSiteCacheRegister));
    signature.immediate = gprOf(slotNameRegister);
    return signature;
}

StubSignature signatureOfPutByIdAtSiteBehindFieldTable(StubKey key) { return signatureOfPutByIdAtSite(key); }

void generatePutByIdAtSite(StubAssembler& jit)
{
    const PutMode mode = static_cast<PutMode>(jit.key().form());
    const SlotHandlerTable table = slotHandlerTable(operationFor(mode), SlotOrigin::ById, SlotForm::Slot);
    const SiteRegisters registers = allocateSiteRegisters(jit, 2);
    JumpList isNotInFieldTable;

    jit.linkToStub(jit.branchIfNotCell(registers.receiver), slotExit(table, SlotExit::Miss));
    isNotInFieldTable.append(branchIfSiteIsFilled(jit, registers.site));
    // What the table of fields answers is never filed.
    JumpList isSampled;
    storeFieldByTable(jit, { registers.receiver, registers.identifierNumber, registers.structureID, registers.header, std::nullopt, false, true }, jit.operand(1), registers.payload, operationFor(mode), isNotInFieldTable, &isSampled);
    isNotInFieldTable.link(&jit);
    jit.bindEntry(putByIdAtSiteBehindFieldTable(mode));
    jit.load32(Address(registers.receiver, JSCell::structureIDOffset()), registers.structureID);
    emitSiteProbe(jit, registers, table);
    JumpList hasAskedFieldTable;
    emitSiteMiss(jit, registers, accessFor(mode), SiteReceiver::IsCell, { putById(mode), putByIdHashed(mode) }, &hasAskedFieldTable);

    isSampled.link(&jit);
    refillSiteCache(jit, registers.receiver, registers.identifierNumber, registers.site, accessFor(mode));
    hasAskedFieldTable.link(&jit);
    jit.linkToStub(jit.branch32(CCallHelpers::Above, registers.identifierNumber, TrustedImm32(RowDisplacementTable::maxColumn)), putByIdHashed(mode));
    jit.jumpToStub(putByIdBehindFieldTable(mode));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
