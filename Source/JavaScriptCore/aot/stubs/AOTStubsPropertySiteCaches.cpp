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

struct TableStubs {
    StubKey front;
    StubKey hashed;
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
    jit.linkToStub(jit.branch32(CCallHelpers::Above, registers.identifierNumber, TrustedImm32(RowDisplacementTable::maxColumn)), stubs.hashed);
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

#if CPU(X86_64)
// A cell asks the site first: the StructureID is compared where it lies, and an empty site, which has the StructureID of nothing, is told from another Structure's only behind the mismatch.
static void generateGetByIdAtSiteAskingSiteFirst(StubAssembler& jit)
{
    static constexpr SlotHandlerTable table = SlotHandlerTable::ReadById;
    static constexpr TableStubs tableStubs { getById(), getByIdHashed() };
    const SiteRegisters registers = allocateSiteRegisters(jit, 1);

    Jump isNotCell = jit.branchIfNotCell(registers.receiver);
    jit.load64(Address(registers.site, Slot::offsetOfStructureID()), registers.header);
    Jump hasOtherStructure = jit.branch32(CCallHelpers::NotEqual, registers.header, Address(registers.receiver, JSCell::structureIDOffset()));
    jit.load64(Address(registers.site, Slot::offsetOfPayload()), registers.payload);
    Jump isNotValue = jit.branch8(CCallHelpers::NotEqual, Address(registers.site, Slot::offsetOfKind()), TrustedImm32(static_cast<int32_t>(Slot::Kind::Value)));
    loadFromValueSlot(jit, registers, jit.result());
    jit.returnToCaller();

    // A name that is asked for and is not there comes next in number: `if (options.signal)`, `payload.aborted`. It needs no handler.
    isNotValue.link(&jit);
    Jump isNotAbsent = jit.branch8(CCallHelpers::NotEqual, Address(registers.site, Slot::offsetOfKind()), TrustedImm32(static_cast<int32_t>(Slot::Kind::Absent)));
    jit.move(TrustedImm64(JSValue::ValueUndefined), jit.result());
    jit.returnToCaller();

    isNotAbsent.link(&jit);
    loadKindOfSlotAtSite(jit, registers, registers.scratch);
    jumpToSlotHandlerOfKind(jit, table, registers.header, registers.scratch);

    hasOtherStructure.link(&jit);
    JumpList isDue;
    Jump isEmpty = jit.branchTest64(CCallHelpers::Zero, registers.header);
    countSiteCacheMiss(jit, registers.site, registers.scratch, isDue);
    Label probeTables = jit.label();
    jit.linkToStub(jit.branch32(CCallHelpers::Above, registers.identifierNumber, TrustedImm32(RowDisplacementTable::maxColumn)), tableStubs.hashed);
    // Objects of several Structures come to this place. What the front table holds for this one is answered from here: the table of fields, which answers at an empty site, has its turn behind it.
    JumpList isNotInFront;
    jit.load32(Address(registers.receiver, JSCell::structureIDOffset()), registers.structureID);
    locateFrontCell(jit, SlotAccess::Read, registers.structureID, registers.identifierNumber, registers.scratch);
    returnByFrontCell(jit, registers.receiver, registers.identifierNumber, registers.scratch, registers.structureID, registers.header, isNotInFront);
    jumpToSlotHandler(jit, slotHandlerTable(SlotOperation::Read, SlotOrigin::ById, SlotForm::PackedSlot), registers.header, registers.scratch);
    isNotInFront.link(&jit);
    jit.jumpToStub(tableStubs.front);

    isEmpty.link(&jit);
    JumpList isNotInFieldTable;
    returnFieldByTable(jit, { registers.receiver, registers.identifierNumber, registers.structureID, registers.header, std::nullopt, false, true }, isNotInFieldTable);
    isNotInFieldTable.link(&jit);
    countMissOfEmptySite(jit, registers.site, registers.scratch, isDue);
    jit.linkToStub(jit.branch32(CCallHelpers::Above, registers.identifierNumber, TrustedImm32(RowDisplacementTable::maxColumn)), tableStubs.hashed);
    jit.jumpToStub(getByIdBehindFieldTable());

    isDue.link(&jit);
    refillSiteCache(jit, registers.receiver, registers.identifierNumber, registers.site, SlotAccess::Read);
    jit.jump().linkTo(probeTables, &jit);

    jit.addOutOfLinePart([isNotCell, registers](StubAssembler& jit) {
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
        emitSiteMiss(jit, registers, SlotAccess::Read, SiteReceiver::MayBePrimitive, tableStubs);
        linkToSlotExit(jit, hasNoStructure, table, SlotExit::Miss);
    });
}
#endif

void generateGetByIdAtSite(StubAssembler& jit)
{
#if CPU(X86_64)
    generateGetByIdAtSiteAskingSiteFirst(jit);
    return;
#endif
    static constexpr SlotHandlerTable table = SlotHandlerTable::ReadById;
    const SiteRegisters registers = allocateSiteRegisters(jit, 1);
    Box<JumpList> hasNoStructure = Box<JumpList>::create();

    loadStructureIDOfReceiver(jit, registers.receiver, registers.structureID, *hasNoStructure, [&] {
        JumpList isNotInFieldTable;
        isNotInFieldTable.append(branchIfSiteIsFilled(jit, registers.site));
        returnFieldByTable(jit, { registers.receiver, registers.identifierNumber, registers.structureID, registers.header, std::nullopt, false, true }, isNotInFieldTable);
        isNotInFieldTable.link(&jit);
    });
    emitSiteProbe(jit, registers, table);
    emitSiteMiss(jit, registers, SlotAccess::Read, SiteReceiver::MayBePrimitive, { getById(), getByIdHashed() });

    // loadStructureIDOfReceiver adds to the list in an out-of-line part, which runs after this function has returned.
    jit.addOutOfLinePart([hasNoStructure](StubAssembler& jit) {
        linkToSlotExit(jit, *hasNoStructure, table, SlotExit::Miss);
    });
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

void generatePutByIdAtSite(StubAssembler& jit)
{
    const PutMode mode = static_cast<PutMode>(jit.key().form());
    const SlotHandlerTable table = slotHandlerTable(operationFor(mode), SlotOrigin::ById, SlotForm::Slot);
    const SiteRegisters registers = allocateSiteRegisters(jit, 2);
    JumpList isNotInFieldTable;

    jit.linkToStub(jit.branchIfNotCell(registers.receiver), slotExit(table, SlotExit::Miss));
    isNotInFieldTable.append(branchIfSiteIsFilled(jit, registers.site));
    // What the table of fields answers is never filed. Its own forms are the shortest way of all. For a field that is stored by its record a filled site is the shorter way:
    // so one such store in 256 at the empty sites goes the way of the caches, and a site that comes again and again gets filled.
    JumpList isSampled;
    storeFieldByTable(jit, { registers.receiver, registers.identifierNumber, registers.structureID, registers.header, std::nullopt, false, true }, jit.operand(1), registers.payload, operationFor(mode), isNotInFieldTable, &isSampled);
    isNotInFieldTable.link(&jit);
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
