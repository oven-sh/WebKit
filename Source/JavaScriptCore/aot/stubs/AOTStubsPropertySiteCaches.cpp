/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsPropertyProbes.h"

#if ENABLE(AOT)

#include "AOTCompiledStructure.h"
#include "AOTConvention.h"
#include "AOTPropertyCachesFront.h"
#include "AOTRuntimeTable.h"
#include "AOTStubsCalls.h"
#include "AOTStubsField.h"
#include "JSArrayBufferView.h"
#include "JSCell.h"
#include "JSObject.h"
#include "JSString.h"
#include <bit>

namespace JSC { namespace AOT {

static_assert(!Slot::offsetOfStructureID() && Slot::offsetOfPayload() == sizeof(uint64_t) && sizeof(Slot) == 2 * sizeof(uint64_t));
static_assert(sizeof(Slot) == InstanceLayout::sizeOfSiteCache && sizeof(SiteCacheState) == sizeof(uint16_t));
static_assert(operationArgumentGPR(4) == gprOf(StubRegister::A4));

static constexpr unsigned isOutOfLineBitInHeaderOfSite = Slot::flagsShiftInHeader + std::countr_zero(Slot::isOutOfLine);

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
struct TableStubsBehindSite {
    StubKey front;
    std::optional<StubKey> hashed;
};

Jump branchIfSiteIsFilled(StubAssembler& jit, GPRReg site)
{
    return jit.branchTest64(CCallHelpers::NonZero, Address(site));
}

void countSiteCacheMiss(StubAssembler& jit, GPRReg site, GPRReg scratch, JumpList& isDueForRefill)
{
    const Address states(instanceGPR, AOT_INSTANCE_OFFSET(propertyCaches.siteCacheStates));
    RELEASE_ASSERT(noOverlap(site, scratch, instanceGPR));
#if CPU(ARM64)
    const OwnedScratchRegister ownedScratch(jit);
    const GPRReg missesLeft = ownedScratch;
    jit.urshift64(site, TrustedImm32(SiteCacheState::slotAddressShift), scratch);
    jit.and64(TrustedImm32(SiteCacheState::offsetMask), scratch);
    jit.loadPtr(states, missesLeft);
    jit.add64(missesLeft, scratch);
    jit.load16(Address(scratch), missesLeft);
    jit.sub32(TrustedImm32(1), missesLeft);
    jit.store16(missesLeft, Address(scratch));
    isDueForRefill.append(jit.branchTest32(CCallHelpers::NonZero, missesLeft, TrustedImm32(UINT16_MAX + 1)));
#else
    static_assert(SiteCacheState::offsetMask == (UINT16_MAX & ~(alignof(Slot) - 1)) >> SiteCacheState::slotAddressShift);
    jit.zeroExtend16To32(site, scratch);
    jit.urshift32(TrustedImm32(SiteCacheState::slotAddressShift), scratch);
    jit.add64(states, scratch);
    jit.m_assembler.subw_im(1, 0, scratch);
    isDueForRefill.append(Jump(jit.m_assembler.jCC(X86Assembler::ConditionC)));
#endif
}

void countMissOfEmptySite(StubAssembler& jit, GPRReg site, GPRReg scratch, JumpList& isDueForFill)
{
    constexpr int32_t step = SiteCacheState::sampleStep;
    constexpr int32_t offsetOfCounter = AOT_INSTANCE_OFFSET(propertyCaches.missesOfEmptySites);
    RELEASE_ASSERT(noOverlap(site, scratch, instanceGPR));
#if CPU(ARM64)
    jit.load8(Address(instanceGPR, offsetOfCounter), scratch);
    jit.add32(TrustedImm32(step), scratch);
    jit.store8(scratch, Address(instanceGPR, offsetOfCounter));
    isDueForFill.append(jit.branchTest32(CCallHelpers::NonZero, scratch, TrustedImm32(UINT8_MAX + 1)));
#else
    jit.m_assembler.addb_im(step, offsetOfCounter, instanceGPR);
    isDueForFill.append(Jump(jit.m_assembler.jCC(X86Assembler::ConditionC)));
#endif
}

void refillSiteCache(StubAssembler& jit, GPRReg receiver, GPRReg identifierNumber, GPRReg site, SlotAccess access)
{
    jit.preservingCallerSavedRegisters(false, [&](unsigned) {
        const GPRReg A0 = jit.reuseScratch(StubRegister::A0);
        const GPRReg A1 = jit.reuseScratch(StubRegister::A1);
        const GPRReg A2 = jit.reuseScratch(StubRegister::A2);
        const GPRReg A3 = jit.reuseScratch(StubRegister::A3);
        const GPRReg A4 = jit.reuseScratch(StubRegister::A4);
        RELEASE_ASSERT(noOverlap(A3, identifierNumber, receiver) && A2 != receiver);
        jit.move(site, A3);
        jit.move(identifierNumber, A2);
        jit.move(receiver, A1);
        jit.move(instanceGPR, A0);
        jit.move(TrustedImm32(static_cast<int32_t>(access)), A4);
        jit.callOperation(Entry::operationAOTFillSiteCache, jit.reuseScratch(StubRegister::Operation));
    });
}

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
static void loadFromValueSlotAtSite(StubAssembler& jit, const SiteRegisters& registers, GPRReg result)
{
    const GPRReg header = registers.header;
    const GPRReg holder = registers.payload;
    RELEASE_ASSERT(noOverlap(result, header, holder) && noOverlap(registers.receiver, header, holder));
#if CPU(X86_64)
    jit.m_assembler.testq_rr(holder, holder);
    jit.m_assembler.cmovq_rr(X86Assembler::ConditionE, registers.receiver, holder);
    jit.m_assembler.btw_ir(isOutOfLineBitInHeaderOfSite, header);
    jit.m_assembler.cmovq_mr(X86Assembler::ConditionC, JSObject::butterflyOffset(), holder, holder);
#elif CPU(ARM64)
    jit.moveConditionallyTest64(CCallHelpers::Zero, holder, holder, registers.receiver, holder);
    jit.loadPtr(Address(holder, JSObject::butterflyOffset()), result);
    jit.m_assembler.tst<64>(header, LogicalImmediate::create64(1ull << isOutOfLineBitInHeaderOfSite));
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
        loadFromValueSlotAtSite(jit, registers, jit.result());
        jit.returnToCaller();
        isNotValue.link(&jit);
    }
    jumpToSlotHandlerOfKind(jit, table, registers.header, registers.scratch);
    hasOtherStructure.link(&jit);
}

// Whoever finds its site empty has asked the table of fields where the caller gives a list for it: it goes on behind that table.
static void emitSiteMiss(StubAssembler& jit, const SiteRegisters& registers, SlotAccess access, SiteReceiver receiver, const TableStubsBehindSite& stubs, JumpList* hasAskedFieldTable = nullptr)
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
    StubSignature signature = StubSignature::withGPRResult({ });
    signature.addOperand(gprOf(firstStubOperandRegister));
    signature.addOperand(gprOf(readSiteCacheRegister));
    signature.immediate = gprOf(slotNameRegister);
    return signature;
}

// A cell asks the site first: an empty site, which has the StructureID of nothing, is told from another Structure's only behind the mismatch.
void generateGetByIdAtSite(StubAssembler& jit)
{
    static constexpr SlotHandlerTable table = SlotHandlerTable::ReadById;
    const bool nameHasColumn = jit.key().family() == StubFamily::GetByIdAtSite;
    const StubKey tables = nameHasColumn ? getById() : getByIdCheckingColumn();
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
        const GPRReg cell = registers.header;
        const GPRReg placeAtSite = registers.payload;
#if CPU(X86_64)
        jit.load32(Address(registers.receiver, JSCell::structureIDOffset()), registers.structureID);
#endif
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
    } else
        countSiteCacheMiss(jit, registers.site, registers.scratch, isDue);
    jit.jumpToStub(tables);

    isEmpty.link(&jit);
    JumpList isNotInFieldTable;
    // The probe shifts by its index, which x86-64 does by this register alone.
    returnFieldByTable(jit, { registers.receiver, registers.identifierNumber, jit.allocateScratch(StubRegister::T14), registers.header, nameHasColumn, true }, isNotInFieldTable);
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

StubSignature signatureOfGetLengthAtSite(StubKey)
{
    return signatureOf(getById());
}

StubSignature signatureOfGetLengthWithSite(StubKey key)
{
    return signatureOfGetByIdAtSite(key);
}

// The length of an array or a string is an own data property that is not configurable, so it is read without a lookup. Of anything else the name is read as any other.
static void returnLengthOrGoOnIn(StubAssembler& jit, StubKey readOfAnyName, const Invocable<void(JumpList& goesOn)> auto& emitForOtherCell)
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
    Jump isNoString = jit.branch8(CCallHelpers::NotEqual, Address(receiver, JSCell::typeInfoTypeOffset()), TrustedImm32(StringType));
    jit.loadPtr(Address(receiver, JSString::offsetOfValue()), scratch);
    Jump isRope = jit.branchIfRopeStringImpl(scratch);
    jit.load32(Address(scratch, StringImpl::lengthMemoryOffset()), scratch);
    boxInt32(jit, scratch, jit.result());
    jit.returnToCaller();

    isRope.link(&jit);
    jit.load32(Address(receiver, JSRopeString::offsetOfLength()), scratch);
    boxInt32(jit, scratch, jit.result());
    jit.returnToCaller();

    isNoString.link(&jit);
    emitForOtherCell(isNeither);

    isNeither.link(&jit);
    jit.jumpToStub(readOfAnyName);
}

void generateGetLengthAtSite(StubAssembler& jit)
{
    returnLengthOrGoOnIn(jit, getById(), [](JumpList&) { });
}

void generateGetLengthWithSite(StubAssembler& jit)
{
    returnLengthOrGoOnIn(jit, getByIdAtSite(), [](JumpList&) { });
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
    // What the table of fields answers is never filed.
    storeFieldByTable(jit, { registers.receiver, registers.identifierNumber, registers.structureID, registers.header, false, true }, jit.operand(1), registers.payload, operationFor(mode), isNotInFieldTable);
    isNotInFieldTable.link(&jit);
    jit.load32(Address(registers.receiver, JSCell::structureIDOffset()), registers.structureID);
    emitSiteProbe(jit, registers, table);
    JumpList hasAskedFieldTable;
    emitSiteMiss(jit, registers, accessFor(mode), SiteReceiver::IsCell, { putById(mode), putByIdHashed(mode) }, &hasAskedFieldTable);

    hasAskedFieldTable.link(&jit);
    jit.linkToStub(jit.branch32(CCallHelpers::Above, registers.identifierNumber, TrustedImm32(RowDisplacementTable::maxColumn)), putByIdHashed(mode));
    jit.jumpToStub(putByIdBehindFieldTable(mode));
}

// ---- Places with SiteAnswers

static_assert(!OBJECT_OFFSETOF(SiteAnswers, first) && OBJECT_OFFSETOF(SiteAnswers, second) == sizeof(SiteAnswers::Answer) && !OBJECT_OFFSETOF(SiteAnswers::Answer, key) && OBJECT_OFFSETOF(SiteAnswers::Answer, operand) == sizeof(uint32_t));
static_assert(cell32Shift == CCallHelpers::TimesEight);

namespace {

// A stub is handed the distance from the instance to the answers. x86-64 reaches a word of them by the two registers. ARM64 adds them up first.
class AnswersOfStub {
public:
    // `place` holds where the answers of the place lie from here on: on x86-64 their index, elsewhere their address. A place without answers finds SiteAnswers::forPlacesWithout().
    AnswersOfStub(StubAssembler& jit, GPRReg distance, GPRReg place)
        : m_place(place)
    {
        static_assert(InstanceLayout::sizeOfIndexOfSiteAnswers == sizeof(uint16_t));
        jit.load16(BaseIndex(instanceGPR, distance, CCallHelpers::TimesOne), place);
#if !CPU(X86_64)
        jit.getEffectiveAddress(BaseIndex(instanceGPR, place, CCallHelpers::TimesEight), place);
        jit.addPtr(TrustedImm32(static_cast<int32_t>(InstanceLayout::offsetOfSiteAnswers)), place);
#endif
    }
#if CPU(X86_64)
    using Word = CCallHelpers::BaseIndex;
    Word word(ptrdiff_t offset) const { return Word(instanceGPR, m_place, CCallHelpers::TimesEight, static_cast<int32_t>(InstanceLayout::offsetOfSiteAnswers + offset)); }
#else
    using Word = CCallHelpers::Address;
    Word word(ptrdiff_t offset) const { return Word(m_place, static_cast<int32_t>(offset)); }
#endif

    // Where the key of the answer is `key`, falls through with the operand of the answer in `operand`, which on x86-64 may be `key`. Elsewhere `keyOfAnswer` holds the key of the answer on ARM64.
    Jump loadOperandUnlessKeyDiffers(StubAssembler& jit, ptrdiff_t offsetOfAnswer, GPRReg key, GPRReg operand, [[maybe_unused]] GPRReg keyOfAnswer) const
    {
#if CPU(X86_64)
        Jump differs = jit.branch32(CCallHelpers::NotEqual, word(offsetOfAnswer), key);
        jit.load32(word(offsetOfAnswer + OBJECT_OFFSETOF(SiteAnswers::Answer, operand)), operand);
        return differs;
#else
        RELEASE_ASSERT(noOverlap(key, operand, keyOfAnswer));
        jit.loadPair32(m_place, TrustedImm32(static_cast<int32_t>(offsetOfAnswer)), keyOfAnswer, operand);
        return jit.branch32(CCallHelpers::NotEqual, key, keyOfAnswer);
#endif
    }

    // `difference` holds a word, and comes to hold the bits in which the key of the answer differs from it. Where there are none, falls through with the operand of the answer in `operand`.
    Jump loadOperandUnlessKeyDiffersKeepingDifference(StubAssembler& jit, ptrdiff_t offsetOfAnswer, GPRReg difference, GPRReg operand, [[maybe_unused]] GPRReg keyOfAnswer) const
    {
#if CPU(X86_64)
        jit.xor32(word(offsetOfAnswer), difference);
        Jump differs = Jump(jit.m_assembler.jCC(X86Assembler::ConditionNE));
        jit.load32(word(offsetOfAnswer + OBJECT_OFFSETOF(SiteAnswers::Answer, operand)), operand);
        return differs;
#else
        RELEASE_ASSERT(noOverlap(difference, operand, keyOfAnswer));
        jit.loadPair32(m_place, TrustedImm32(static_cast<int32_t>(offsetOfAnswer)), keyOfAnswer, operand);
        jit.xor32(keyOfAnswer, difference);
        return jit.branchTest32(CCallHelpers::NonZero, difference);
#endif
    }

    // The key of an answer by Structure less the StructureID of the cell. A StructureID has zeros where the form lies, so that is zero where the answer is of the Structure and of the form that the use meets most,
    // the bits of another form where it is of the Structure, and more than formMask of any other answer. Falls through at zero.
    Jump loadBitsOfFormUnlessMostCommon(StubAssembler& jit, ptrdiff_t offsetOfAnswer, GPRReg cell, GPRReg bitsOfForm, [[maybe_unused]] GPRReg operand, [[maybe_unused]] GPRReg keyOfAnswer) const
    {
#if CPU(X86_64)
        jit.load32(word(offsetOfAnswer), bitsOfForm);
        return jit.branchSub32(CCallHelpers::NonZero, Address(cell, JSCell::structureIDOffset()), bitsOfForm);
#else
        jit.load32(Address(cell, JSCell::structureIDOffset()), bitsOfForm);
        jit.loadPair32(m_place, TrustedImm32(static_cast<int32_t>(offsetOfAnswer)), keyOfAnswer, operand);
        return jit.branchSub32(CCallHelpers::NonZero, keyOfAnswer, bitsOfForm, bitsOfForm);
#endif
    }
    // The same for an answer that never has that form.
    void loadBitsOfForm(StubAssembler& jit, ptrdiff_t offsetOfAnswer, GPRReg cell, GPRReg bitsOfForm, [[maybe_unused]] GPRReg operand, [[maybe_unused]] GPRReg keyOfAnswer) const
    {
#if CPU(X86_64)
        jit.load32(word(offsetOfAnswer), bitsOfForm);
        jit.sub32(Address(cell, JSCell::structureIDOffset()), bitsOfForm);
#else
        jit.load32(Address(cell, JSCell::structureIDOffset()), bitsOfForm);
        jit.loadPair32(m_place, TrustedImm32(static_cast<int32_t>(offsetOfAnswer)), keyOfAnswer, operand);
        jit.sub32(keyOfAnswer, bitsOfForm, bitsOfForm);
#endif
    }
    // Behind either, on either side: the operand of that answer. On x86-64 `operand` may be any register; elsewhere it is the one that was handed there, which has it already.
    void loadOperand(StubAssembler& jit, [[maybe_unused]] ptrdiff_t offsetOfAnswer, [[maybe_unused]] GPRReg operand) const
    {
#if CPU(X86_64)
        jit.load32(word(offsetOfAnswer + OBJECT_OFFSETOF(SiteAnswers::Answer, operand)), operand);
#endif
    }
    void loadSignedOperand(StubAssembler& jit, [[maybe_unused]] ptrdiff_t offsetOfAnswer, GPRReg operand) const
    {
#if CPU(X86_64)
        jit.load32SignedExtendTo64(word(offsetOfAnswer + OBJECT_OFFSETOF(SiteAnswers::Answer, operand)), operand);
#else
        jit.signExtend32ToPtr(operand, operand);
#endif
    }

private:
    GPRReg m_place;
};

} // anonymous namespace

static constexpr ptrdiff_t offsetOfFirstAnswer = OBJECT_OFFSETOF(SiteAnswers, first);
static constexpr ptrdiff_t offsetOfSecondAnswer = OBJECT_OFFSETOF(SiteAnswers, second);

static Jump branchUnlessForm(StubAssembler& jit, SiteUse use, GPRReg bitsOfForm, SiteAnswers::Form form)
{
    return jit.branch32(CCallHelpers::NotEqual, bitsOfForm, TrustedImm32(static_cast<int32_t>(SiteAnswers::bitsOf(use, form))));
}

static constexpr bool nameHasColumn(NameAtSite name) { return name == NameAtSite::HasColumn; }

static void loadNameOfSite(StubAssembler& jit, GPRReg distance, GPRReg name, GPRReg scratch, NameAtSite nameAtSite)
{
    RELEASE_ASSERT(noOverlap(distance, scratch) && noOverlap(name, scratch));
    // The number of a name with a column has two bytes, as the index of a place has. A place of any other name has two numbers, and so four bytes at the same distance.
    static_assert(sizeof(uint16_t) == InstanceLayout::sizeOfIndexOfSiteAnswers && RowDisplacementTable::maxColumn <= UINT16_MAX);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.namesOfSitesByDistance)), scratch);
    if (nameHasColumn(nameAtSite))
        jit.load16(BaseIndex(scratch, distance, CCallHelpers::TimesOne), name);
    else
        jit.load32(BaseIndex(scratch, distance, CCallHelpers::TimesOne), name);
}

// Every register is as it was behind it. The receiver is a cell.
static void fillAnswersAtSite(StubAssembler& jit, GPRReg receiver, GPRReg name, GPRReg distance, SiteUse use)
{
    jit.preservingCallerSavedRegisters(false, [&](unsigned) {
        const GPRReg A0 = jit.reuseScratch(StubRegister::A0);
        const GPRReg A1 = jit.reuseScratch(StubRegister::A1);
        const GPRReg A2 = jit.reuseScratch(StubRegister::A2);
        const GPRReg A3 = jit.reuseScratch(StubRegister::A3);
        const GPRReg A4 = jit.reuseScratch(StubRegister::A4);
        RELEASE_ASSERT(noOverlap(A3, name, receiver) && A2 != receiver);
        jit.move(distance, A3);
        jit.move(name, A2);
        jit.move(receiver, A1);
        jit.move(instanceGPR, A0);
        jit.move(TrustedImm32(static_cast<int32_t>(use)), A4);
        jit.callOperation(Entry::operationAOTFillSiteAnswers, jit.reuseScratch(StubRegister::Operation));
    });
}

// The receiver is a cell for which the place has no answer. Falls through with every register but the two scratch registers as it was.
// The misses of all places are counted together, and one in a few is a sample: AnswersAtSites::isDueForFilling().
static void countMissAtSite(StubAssembler& jit, GPRReg receiver, GPRReg distance, GPRReg name, GPRReg scratch, SiteUse use, NameAtSite nameAtSite)
{
    RELEASE_ASSERT(noOverlap(receiver, distance, name, scratch, instanceGPR));
    JumpList isSample;
    countMissOfEmptySite(jit, distance, scratch, isSample);
    Label isCounted = jit.label();
    jit.addOutOfLinePart([=](StubAssembler& jit) mutable {
        isSample.link(&jit);
        loadNameOfSite(jit, distance, name, scratch, nameAtSite);
        fillAnswersAtSite(jit, receiver, name, distance, use);
        jit.jump().linkTo(isCounted, &jit);
    });
}

// The variant whose receiver lies where the others take the distance takes it in the usual register.
StubSignature signatureOfReadAtSite(StubKey key)
{
    GPRReg receiver = key.operandRegister().value_or(gprOf(firstStubOperandRegister));
    StubSignature signature = StubSignature::withGPRResult({ receiver });
    signature.immediate = gprOf(receiver == gprOf(shortStubImmediateRegister) ? stubImmediateRegister : shortStubImmediateRegister);
    return signature;
}

StubSignature signatureOfReadBehindSite(StubKey)
{
    StubSignature signature = StubSignature::withGPRResult({ gprOf(firstStubOperandRegister) });
    signature.immediate = gprOf(shortStubImmediateRegister);
    return signature;
}

StubSignature signatureOfReadLengthAtSite(StubKey key)
{
    return signatureOfReadBehindSite(key);
}

// A field of a compiled object: where the answers of the place lie, the word of the shape, one comparison, the offset, a look whether the slot holds a value, and the value.
void generateReadAtSite(StubAssembler& jit)
{
    constexpr SiteUse use = SiteUse::Read;
    using Form = SiteAnswers::Form;
    const ReadAtSiteForm form = readAtSiteFormOf(jit.key());
    const GPRReg receiver = jit.operand(0);
    const GPRReg result = jit.result();
    const GPRReg distance = jit.immediate();
    // No receiver lies in the first two of these.
#if CPU(X86_64)
    const GPRReg place = jit.allocateScratch(StubRegister::T11);
    const GPRReg key = jit.allocateScratch(StubRegister::T13);
    // A shift takes its count in one register alone, and swaps where the count lies elsewhere.
    std::optional<OwnedScratchRegister> ownedScratch;
    if (receiver == gprOf(StubRegister::T14))
        ownedScratch.emplace(jit);
    const GPRReg operand = ownedScratch ? ownedScratch->gpr() : jit.allocateScratch(StubRegister::T14);
    const GPRReg keyOfAnswer = InvalidGPRReg;
    const GPRReg operandOfCommonForm = key;
    const GPRReg extension = place;
#else
    const GPRReg key = jit.allocateScratch(StubRegister::T11);
    const GPRReg operand = jit.allocateScratch(StubRegister::T13);
    const GPRReg keyOfAnswer = jit.allocateScratch(StubRegister::T12);
    const GPRReg place = jit.allocateScratch(StubRegister::T14);
    const GPRReg operandOfCommonForm = operand;
    const GPRReg extension = keyOfAnswer;
#endif
    const GPRReg laterGranules = operand;
    // It outlives the look at the Structure.
    const GPRReg difference = jit.allocateScratch(distance == gprOf(StubRegister::T15) ? StubRegister::T10 : StubRegister::T15);
    RELEASE_ASSERT(noOverlap(receiver, distance, place, key, operand, difference) && noOverlap(receiver, distance, key, laterGranules, extension));
    // The receiver outlives the load: an empty word sends it on.
    const GPRReg value = receiver == result ? operand : result;
    JumpList isNotAsked;
    JumpList isNotAnswered;
    const GPRReg storage = operandOfCommonForm == key ? operand : key;
    // The answer says where the slot lies, and this object holds nothing there.
    JumpList slotIsEmpty;
    auto returnFieldAt = [&](GPRReg offset) {
        const BaseIndex slot(receiver, offset, CCallHelpers::TimesOne);
#if CPU(X86_64)
        slotIsEmpty.append(jit.branchTest64(CCallHelpers::Zero, slot));
        jit.load64(slot, result);
#else
        jit.load64(slot, value);
        slotIsEmpty.append(jit.branchTest64(CCallHelpers::Zero, value));
        jit.move(value, result);
#endif
        jit.returnToCaller();
    };

    if (form.receiver == ReceiverAtSite::IsAnyValue)
        isNotAsked.append(jit.branchIfNotCell(receiver));
    const AnswersOfStub answers(jit, distance, place);
    // Only a compiled object has the word of a shape there, with compiledObjectShapeBit, and so has every key by shape: what another cell has there fits none.
    jit.load32(Address(receiver, offsetOfShapeWord), difference);
    Jump isNotInCell = answers.loadOperandUnlessKeyDiffersKeepingDifference(jit, offsetOfFirstAnswer, difference, operandOfCommonForm, keyOfAnswer);
    returnFieldAt(operandOfCommonForm);

    // THE ARMS STAND IN THE ORDER IN WHICH PROGRAMS MEET THEM. Behind a field in the cell that is a value of the receiver's own in its butterfly: one comparison finds the Structure and the form.
    isNotInCell.link(&jit);
    Jump hasOtherKey = answers.loadBitsOfFormUnlessMostCommon(jit, offsetOfSecondAnswer, receiver, key, operand, keyOfAnswer);
    answers.loadSignedOperand(jit, offsetOfSecondAnswer, operandOfCommonForm);
    jit.loadPtr(Address(receiver, JSObject::butterflyOffset()), storage);
    jit.load64(BaseIndex(storage, operandOfCommonForm, CCallHelpers::TimesEight), result);
    jit.returnToCaller();

    hasOtherKey.link(&jit);
    Jump isNotAnsweredByStructure = jit.branch32(CCallHelpers::Above, key, TrustedImm32(SiteAnswers::formMask));
    answers.loadOperand(jit, offsetOfSecondAnswer, operand);
    // Whatever comes here has none of the forms in front of `lower`.
    struct JumpsToTwoForms {
        Jump toLower;
        Jump toUpper;
    };
    auto branchIfEitherOf = [&](Form lower, Form upper) -> JumpsToTwoForms {
        const int32_t bitsOfUpper = static_cast<int32_t>(SiteAnswers::bitsOf(use, upper));
        RELEASE_ASSERT(static_cast<int32_t>(SiteAnswers::bitsOf(use, lower)) + 1 == bitsOfUpper);
#if CPU(X86_64)
        jit.m_assembler.cmpl_ir(bitsOfUpper, key);
        Jump toLower(jit.m_assembler.jCC(X86Assembler::ConditionB));
        return { toLower, Jump(jit.m_assembler.jCC(X86Assembler::ConditionE)) };
#else
        Jump toLower = jit.branch32(CCallHelpers::Below, key, TrustedImm32(bitsOfUpper));
        return { toLower, jit.branch32(CCallHelpers::Equal, key, TrustedImm32(bitsOfUpper)) };
#endif
    };
    static_assert(SiteAnswers::bitsOf(use, Form::ValueInStorageOfHolder) == 1);
    JumpsToTwoForms aboveInStorageOrInCell = branchIfEitherOf(Form::ValueInStorageOfHolder, Form::ValueInWordOfCage);
    JumpsToTwoForms ownFieldOrGetterAboveInStorage = branchIfEitherOf(Form::OwnFieldInCell, Form::GetterInStorageOfHolder);
    JumpsToTwoForms ownValueInCellOrExtraValueInStorage = branchIfEitherOf(Form::OwnValueInCell, Form::ExtraValueInStorage);
    JumpsToTwoForms extraValueInCellOrGetterAboveInCell = branchIfEitherOf(Form::ExtraValueInCell, Form::GetterInWordOfCage);
    JumpsToTwoForms ownGetter = branchIfEitherOf(Form::OwnGetterInStorage, Form::OwnGetterInCell);
    JumpsToTwoForms lengthOrOwnNarrowReference = branchIfEitherOf(Form::LengthOfTypedArray, Form::OwnNarrowReferenceInCell);
    Jump isOwnInt32 = jit.branch32(CCallHelpers::Equal, key, TrustedImm32(static_cast<int32_t>(SiteAnswers::bitsOf(use, Form::OwnInt32InCell))));
    isNotAnswered.append(jit.jump());

    aboveInStorageOrInCell.toLower.link(&jit);
    jit.loadPtr(BaseIndex(cellCageBaseGPR, operand, CCallHelpers::TimesEight, JSObject::butterflyOffset()), operand);
    jit.load32SignedExtendTo64(answers.word(offsetOfFirstAnswer + OBJECT_OFFSETOF(SiteAnswers::Answer, operand)), key);
    jit.load64(BaseIndex(operand, key, CCallHelpers::TimesEight), result);
    jit.returnToCaller();

    aboveInStorageOrInCell.toUpper.link(&jit);
    jit.load64(BaseIndex(cellCageBaseGPR, operand, CCallHelpers::TimesEight), result);
    jit.returnToCaller();

    ownFieldOrGetterAboveInStorage.toLower.link(&jit);
    returnFieldAt(operand);

    ownValueInCellOrExtraValueInStorage.toLower.link(&jit);
    jit.load64(BaseIndex(receiver, operand, CCallHelpers::TimesOne), result);
    jit.returnToCaller();

    auto loadExtraProperties = [&] {
        jit.zeroExtend8To32(operand, key);
        jit.load32(BaseIndex(receiver, key, CCallHelpers::TimesFour), key);
        isNotAsked.append(jit.branchTest32(CCallHelpers::Zero, key));
        jit.rshift32(TrustedImm32(SiteAnswers::indexShiftInOperandOfExtraValue), operand);
    };
    ownValueInCellOrExtraValueInStorage.toUpper.link(&jit);
    loadExtraProperties();
    jit.loadPtr(BaseIndex(cellCageBaseGPR, key, CCallHelpers::TimesEight, JSObject::butterflyOffset()), key);
    jit.signExtend32ToPtr(operand, operand);
    jit.load64(BaseIndex(key, operand, CCallHelpers::TimesEight), result);
    jit.returnToCaller();

    extraValueInCellOrGetterAboveInCell.toLower.link(&jit);
    loadExtraProperties();
    // A reference counts words of eight bytes from the base of the cage, and the value lies in the cell that it names.
    jit.add32(operand, key);
    jit.load64(BaseIndex(cellCageBaseGPR, key, CCallHelpers::TimesEight), result);
    jit.returnToCaller();

    lengthOrOwnNarrowReference.toLower.link(&jit);
    // A view that follows its buffer, and one of a buffer that may change its size, work their length out.
    isNotAsked.append(jit.branchTest8(CCallHelpers::NonZero, Address(receiver, JSArrayBufferView::offsetOfMode()), TrustedImm32(resizabilityAndAutoLengthMask)));
    jit.load64(Address(receiver, JSArrayBufferView::offsetOfLength()), operand);
    // The operand may be the scratch register of the assembler, through which a compare with 64 bits carries them.
    isNotAsked.append(jit.branch64(CCallHelpers::Above, operand, TrustedImm32(std::numeric_limits<int32_t>::max())));
    boxInt32(jit, operand, jit.result());
    jit.returnToCaller();

    // An accessor: the word that a value would lie in holds the GetterSetter, and its getter is called as behind the access table.
    const StubKey callOfGetter = callGetterOfAnswer(jit.key().callerFrame());
    const GPRReg receiverOfGetter = gprOf(firstStubOperandRegister);
    // The receiver has left its register by then.
    const GPRReg getterSetter = jit.clobberRegister(StubRegister::T10);
    checkStubOperands(callOfGetter, { receiverOfGetter, getterSetter });
    RELEASE_ASSERT(noOverlap(receiverOfGetter, getterSetter, key, operand, distance, place));
    auto callGetterIn = [&](BaseIndex word) {
        jit.load64(word, getterSetter);
        jit.jumpToStub(callOfGetter);
    };
    ownFieldOrGetterAboveInStorage.toUpper.link(&jit);
    jit.move(receiver, receiverOfGetter);
    jit.loadPtr(BaseIndex(cellCageBaseGPR, operand, CCallHelpers::TimesEight, JSObject::butterflyOffset()), operand);
    jit.load32SignedExtendTo64(answers.word(offsetOfFirstAnswer + OBJECT_OFFSETOF(SiteAnswers::Answer, operand)), key);
    callGetterIn(BaseIndex(operand, key, CCallHelpers::TimesEight));

    extraValueInCellOrGetterAboveInCell.toUpper.link(&jit);
    jit.move(receiver, receiverOfGetter);
    callGetterIn(BaseIndex(cellCageBaseGPR, operand, CCallHelpers::TimesEight));

    ownGetter.toLower.link(&jit);
    jit.move(receiver, receiverOfGetter);
    jit.loadPtr(Address(receiverOfGetter, JSObject::butterflyOffset()), key);
    jit.signExtend32ToPtr(operand, operand);
    callGetterIn(BaseIndex(key, operand, CCallHelpers::TimesEight));

    ownGetter.toUpper.link(&jit);
    jit.move(receiver, receiverOfGetter);
    callGetterIn(BaseIndex(receiverOfGetter, operand, CCallHelpers::TimesOne));

    // As readFieldInExtension(): the narrow word of an object without an Extension names a page of zeros, which holds no granule.
    // The bit of the granule goes to the top and the bits of later granules drop out, so what is left counts the granules up to this one.
    static_assert(offsetOfFirstExtensionSlot == extensionGranuleSize && extensionGranuleSize == sizeof(uint64_t) && maxExtensionGranules == 64);
    isNotAnsweredByStructure.link(&jit);
    // The one bit in which the key of the answer differs from the word of the shape says which other answer by shape it is. A key by no shape, and what another cell has there, differ in more.
    isNotAnswered.append(jit.branchTest32(CCallHelpers::NonZero, difference, TrustedImm32(static_cast<int32_t>(~SiteAnswers::bitsTurnedForAnswersByShape))));
    jit.load32(answers.word(offsetOfFirstAnswer + OBJECT_OFFSETOF(SiteAnswers::Answer, operand)), operand);
    Jump isNotInExtension = jit.branch32(CCallHelpers::NotEqual, difference, TrustedImm32(static_cast<int32_t>(SiteAnswers::bitTurnedForFieldInExtension)));
    jit.load32(Address(receiver, offsetOfFirstField), extension);
    jit.getEffectiveAddress(BaseIndex(cellCageBaseGPR, extension, static_cast<CCallHelpers::Scale>(cell32Shift), offsetOfEmptyExtensionInCage), extension);
    jit.load64(Address(extension, offsetOfExtensionGranuleBits), key);
    jit.lshift64(laterGranules, key);
    slotIsEmpty.append(jit.branchTest64(CCallHelpers::PositiveOrZero, key));
    jit.countPopulation64(key, key, CCallHelpers::fpTempRegister);
    jit.load64(BaseIndex(extension, key, CCallHelpers::TimesEight), value);
    slotIsEmpty.append(jit.branchTest64(CCallHelpers::Zero, value));
    jit.move(value, result);
    jit.returnToCaller();

    // A slot of the cell that holds no boxed value: as the stub of any field reads a slot of that form.
    isNotInExtension.link(&jit);
    {
        const GPRReg address = place;
        const FieldReadRegisters typed {
            .field = { .object = receiver, .slotData = InvalidGPRReg, .extensionWordOffset = InvalidGPRReg, .address = address, .representation = operand },
            .value = key,
            .result = result,
            .number = jit.allocateScratch(FPRInfo::fpRegT0),
            .numberScratch = jit.allocateScratch(FPRInfo::fpRegT1),
        };
        FieldReadExits exits;
        for (SlotRepresentation representation : { SlotRepresentation::OptionalCell32, SlotRepresentation::OptionalInt32, SlotRepresentation::OptionalDouble, SlotRepresentation::OptionalBoolean, SlotRepresentation::OptionalInt64 }) {
            Jump isOfAnotherForm = jit.branch32(CCallHelpers::NotEqual, difference, TrustedImm32(static_cast<int32_t>(SiteAnswers::bitTurnedForSlotInCell(representation))));
            // An answer by Structure of that form has left its operand in the same register.
            if (representation == SlotRepresentation::OptionalCell32)
                lengthOrOwnNarrowReference.toUpper.link(&jit);
            if (representation == SlotRepresentation::OptionalInt32)
                isOwnInt32.link(&jit);
            FieldReadRegisters registers = typed;
            if (representationWithCodeForRead(representation) <= representationsReadAsInt32.back())
                registers.slot = BaseIndex(receiver, operand, CCallHelpers::TimesOne);
            else
                jit.getEffectiveAddress(BaseIndex(receiver, operand, CCallHelpers::TimesOne), address);
            {
                // The operand is read by the load of the slot, or has been read by the line above, and by nothing behind.
                AllowMacroScratchRegisterUsage operandIsFreeBehindTheLoad(jit);
                readFieldAndReturn(jit, registers, exits, representation);
            }
            isOfAnotherForm.link(&jit);
        }
        isNotAnswered.append(jit.jump());
        slotIsEmpty.append(exits.isAbsent);
        isNotAsked.append(exits.cannot);
    }

    checkStubOperands(readBehindSite(WayBehindSite::AfterMiss, form.name), { gprOf(firstStubOperandRegister) });
    auto goOnBehindSite = [&](WayBehindSite way) {
        jit.move(receiver, gprOf(firstStubOperandRegister));
        if (distance != gprOf(shortStubImmediateRegister))
            jit.move(distance, jit.clobberRegister(shortStubImmediateRegister));
        jit.jumpToStub(readBehindSite(way, form.name));
    };
    isNotAnswered.link(&jit);
    goOnBehindSite(WayBehindSite::AfterMiss);

    // `options.signal`, where the object leaves the property out: undefined, by what getByIdOfEmptySlot() goes by. Every field that the object has lies in its slot, it has nothing but fields,
    // above it stands Object.prototype alone, and that has no such name now: each is asked of the object and of the table at every read, so nothing has to be taken back.
    slotIsEmpty.link(&jit);
    isNotAsked.append(jit.branchTest32(CCallHelpers::NonZero, Address(receiver, offsetOfOrderWord), TrustedImm32(static_cast<int32_t>(orderWordHasFieldOutsideSlot))));
    loadStructure(jit, receiver, key);
    Jump isNotBelowObjectPrototype = jit.branchTest8(CCallHelpers::Zero, Address(key, CompiledStructure::offsetOfFlags()), TrustedImm32(static_cast<uint8_t>(CompiledStructure::Flag::HasOnlyFieldsBelowObjectPrototype)));
    loadNameOfSite(jit, distance, key, place, form.name);
    if (!nameHasColumn(form.name))
        isNotAsked.append(jit.branch32(CCallHelpers::Above, key, TrustedImm32(FieldTableEntry::maxName)));
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(propertyCaches.namesOfObjectPrototypes)), place);
    jit.urshift32(key, TrustedImm32(6), operand);
    jit.load64(BaseIndex(place, operand, CCallHelpers::TimesEight), place);
    // A shift takes the low six bits of its count.
    jit.urshift64(key, place);
    isNotAsked.append(jit.branchTest64(CCallHelpers::NonZero, place, TrustedImm32(1)));
    jit.move(TrustedImm64(JSValue::ValueUndefined), result);
    jit.returnToCaller();

    if (!nameHasColumn(form.name))
        isNotAsked.append(isNotBelowObjectPrototype);
    isNotAsked.link(&jit);
    goOnBehindSite(WayBehindSite::WithoutMiss);

    // An instance of a class or of a constructor function. All that the tables in front would find is known here.
    if (nameHasColumn(form.name)) {
        isNotBelowObjectPrototype.link(&jit);
        goOnBehindSite(WayBehindSite::OfFieldLeftOut);
    }
}

StubSignature signatureOfReadAtSiteOfNarrowField(StubKey key)
{
    return signatureOfReadAtSite(key);
}

// The key of such an answer is the word of the shape with the bit of the form turned. Any other receiver, any other answer and a slot that holds a mark go on as at every place.
void generateReadAtSiteOfNarrowField(StubAssembler& jit)
{
    const ReadAtSiteForm form = readAtSiteFormOf(jit.key());
    const SlotRepresentation representation = narrowFieldsAtSites[jit.key().count()];
    const GPRReg receiver = jit.operand(0);
    const GPRReg distance = jit.immediate();
#if CPU(X86_64)
    const GPRReg place = jit.allocateScratch(StubRegister::T11);
    const GPRReg key = jit.allocateScratch(StubRegister::T13);
    std::optional<OwnedScratchRegister> ownedScratch;
    if (receiver == gprOf(StubRegister::T14))
        ownedScratch.emplace(jit);
    const GPRReg offset = ownedScratch ? ownedScratch->gpr() : jit.allocateScratch(StubRegister::T14);
    const GPRReg keyOfAnswer = InvalidGPRReg;
#else
    const GPRReg key = jit.allocateScratch(StubRegister::T11);
    const GPRReg offset = jit.allocateScratch(StubRegister::T13);
    const GPRReg keyOfAnswer = jit.allocateScratch(StubRegister::T12);
    const GPRReg place = jit.allocateScratch(StubRegister::T14);
#endif
    RELEASE_ASSERT(noOverlap(receiver, distance, place, key, offset));
    auto readAsAtEveryPlace = [&](ReceiverAtSite receiverIs) {
        StubKey stub = readAtSite(receiverIs, form.name);
        jit.jumpToStub(jit.key().operandRegister() ? stub.withOperandRegister(*jit.key().operandRegister()) : stub);
    };

    Jump isNotCell;
    if (form.receiver == ReceiverAtSite::IsAnyValue)
        isNotCell = jit.branchIfNotCell(receiver);
    const AnswersOfStub answers(jit, distance, place);
    jit.load32(Address(receiver, offsetOfShapeWord), key);
    jit.xor32(TrustedImm32(static_cast<int32_t>(SiteAnswers::bitTurnedForSlotInCell(representation))), key);
    JumpList goesOn;
    Jump hasOtherKey = answers.loadOperandUnlessKeyDiffers(jit, offsetOfFirstAnswer, key, offset, keyOfAnswer);
    Label readsSlot = jit.label();
    FieldReadRegisters typed {
        .field = { .object = receiver, .slotData = InvalidGPRReg, .extensionWordOffset = InvalidGPRReg, .address = place, .representation = offset },
        .value = key,
        .result = jit.result(),
        .number = jit.allocateScratch(FPRInfo::fpRegT0),
        .numberScratch = jit.allocateScratch(FPRInfo::fpRegT1),
    };
    if (representationWithCodeForRead(representation) <= representationsReadAsInt32.back())
        typed.slot = BaseIndex(receiver, offset, CCallHelpers::TimesOne);
    else
        jit.getEffectiveAddress(BaseIndex(receiver, offset, CCallHelpers::TimesOne), place);
    FieldReadExits exits;
    {
        // The offset is read by the load of the slot, or has been read by the line above, and by nothing behind.
        AllowMacroScratchRegisterUsage offsetIsFreeBehindTheLoad(jit);
        readFieldAndReturn(jit, typed, exits, representation);
    }

    // Objects of two layouts come in turns: the second layout is remembered by the Structure of its object.
    hasOtherKey.link(&jit);
    if (std::optional<SiteAnswers::Form> byStructure = SiteAnswers::formByStructureOfSlotInCell(representation)) {
        answers.loadBitsOfForm(jit, offsetOfSecondAnswer, receiver, key, offset, keyOfAnswer);
        goesOn.append(jit.branch32(CCallHelpers::NotEqual, key, TrustedImm32(static_cast<int32_t>(SiteAnswers::bitsOf(SiteUse::Read, *byStructure)))));
        answers.loadOperand(jit, offsetOfSecondAnswer, offset);
        jit.jump().linkTo(readsSlot, &jit);
    }
    goesOn.append(exits.isAbsent);
    goesOn.append(exits.cannot);
    goesOn.link(&jit);
    readAsAtEveryPlace(ReceiverAtSite::IsCell);
    if (isNotCell.isSet()) {
        isNotCell.link(&jit);
        readAsAtEveryPlace(ReceiverAtSite::IsAnyValue);
    }
}

void generateReadBehindSite(StubAssembler& jit)
{
    const ReadBehindSiteForm form = readBehindSiteFormOf(jit.key());
    const GPRReg receiver = jit.operand(0);
    const GPRReg distance = jit.immediate();
    const GPRReg scratch = jit.allocateScratch(StubRegister::T11);
    const GPRReg name = jit.allocateScratch(StubRegister::T13);
    checkStubOperands(getByIdCheckingColumn(), { receiver });
    checkStubOperands(getById(), { receiver });
    checkStubOperands(getByIdBehindFieldTable(), { receiver });
    checkStubOperands(getByIdOfFieldLeftOut(), { receiver });
    if (form.way == WayBehindSite::AfterMiss)
        countMissAtSite(jit, receiver, distance, name, scratch, SiteUse::Read, form.name);
    loadNameOfSite(jit, distance, distance == gprOf(slotNameRegister) ? distance : jit.allocateScratch(slotNameRegister), scratch, form.name);
    if (!nameHasColumn(form.name))
        jit.jumpToStub(getByIdCheckingColumn());
    else
        jit.jumpToStub(form.way == WayBehindSite::AfterMiss ? getByIdBehindFieldTable() : form.way == WayBehindSite::OfFieldLeftOut ? getByIdOfFieldLeftOut() : getById());
}

void generateReadLengthAtSite(StubAssembler& jit)
{
    // Of a typed array the name is a getter of the engine, far above the receiver. The place remembers of a Structure that this is what a lookup finds; it is asked here, in front of the arms of other names.
    returnLengthOrGoOnIn(jit, readAtSite(ReceiverAtSite::IsAnyValue, static_cast<NameAtSite>(jit.key().form())), [&](JumpList& goesOn) {
        const GPRReg receiver = jit.operand(0);
        const GPRReg distance = jit.immediate();
        const GPRReg place = jit.reuseScratch(StubRegister::T11);
        const GPRReg key = jit.allocateScratch(StubRegister::T13);
        RELEASE_ASSERT(noOverlap(receiver, distance, place, key));
        const AnswersOfStub answers(jit, distance, place);
        jit.load32(Address(receiver, JSCell::structureIDOffset()), key);
        jit.or32(TrustedImm32(static_cast<int32_t>(SiteAnswers::bitsOf(SiteUse::Read, SiteAnswers::Form::LengthOfTypedArray))), key);
        goesOn.append(jit.branch32(CCallHelpers::NotEqual, answers.word(offsetOfSecondAnswer), key));
        // A view that follows its buffer, and one of a buffer that may change its size, work their length out.
        goesOn.append(jit.branchTest8(CCallHelpers::NonZero, Address(receiver, JSArrayBufferView::offsetOfMode()), TrustedImm32(resizabilityAndAutoLengthMask)));
        jit.load64(Address(receiver, JSArrayBufferView::offsetOfLength()), key);
        goesOn.append(jit.branch64(CCallHelpers::Above, key, TrustedImm32(std::numeric_limits<int32_t>::max())));
        boxInt32(jit, key, jit.result());
        jit.returnToCaller();
    });
}

StubSignature signatureOfCallMethodAtSite(StubKey key)
{
    return signatureOf(callMethodById(key.count()));
}

// `object.name(a, b)`: the Structure of `this`, one comparison, the callee out of its word, and on as a call of a value.
void generateCallMethodAtSite(StubAssembler& jit)
{
    constexpr SiteUse use = SiteUse::Call;
    using Form = SiteAnswers::Form;
    const unsigned count = jit.key().count();
    const CallRegisters registers = CallRegisters::allocate(jit);
    const GPRReg distance = jit.immediate();
    const GPRReg key = registers.word;
#if CPU(X86_64)
    const GPRReg place = registers.scratch;
    const OwnedScratchRegister ownedScratch(jit);
    const GPRReg operand = ownedScratch;
    const GPRReg keyOfAnswer = InvalidGPRReg;
    const GPRReg operandOfCommonForm = calleeGPR;
    const GPRReg index = operand;
#else
    const GPRReg operand = registers.scratch;
    const GPRReg keyOfAnswer = jit.allocateScratch(StubRegister::T14);
    const GPRReg place = jit.allocateScratch(StubRegister::T15);
    const GPRReg operandOfCommonForm = operand;
    const GPRReg index = keyOfAnswer;
#endif
    RELEASE_ASSERT(noOverlap(thisGPR, calleeGPR, distance, key, operand, place, instanceGPR));
    JumpList isNotAsked;

    isNotAsked.append(jit.branchIfNotCell(thisGPR));
    const AnswersOfStub answers(jit, distance, place);
    // A method in the butterfly of an object above the receiver is what places meet most: one comparison finds the Structure and the form. Only the first answer has that form: AnswersAtSites::fill().
    Jump hasOtherKey = answers.loadBitsOfFormUnlessMostCommon(jit, offsetOfFirstAnswer, thisGPR, calleeGPR, operand, keyOfAnswer);
    answers.loadOperand(jit, offsetOfFirstAnswer, operandOfCommonForm);
    jit.loadPtr(BaseIndex(cellCageBaseGPR, operandOfCommonForm, CCallHelpers::TimesEight, JSObject::butterflyOffset()), operandOfCommonForm);
    jit.load32SignedExtendTo64(answers.word(offsetOfSecondAnswer + OBJECT_OFFSETOF(SiteAnswers::Answer, operand)), index);
    jit.load64(BaseIndex(operandOfCommonForm, index, CCallHelpers::TimesEight), calleeGPR);
    jit.jumpToStub(callValue(count));

    // calleeGPR holds the bits of the form.
    auto callValueOfOtherForm = [&](ptrdiff_t offsetOfAnswer) {
        JumpList differs;
        differs.append(jit.branch32(CCallHelpers::Above, calleeGPR, TrustedImm32(SiteAnswers::formMask)));
        answers.loadOperand(jit, offsetOfAnswer, operand);
        // Each object has its own value, so only the place of the value is known.
        Jump isNotInCell = branchUnlessForm(jit, use, calleeGPR, Form::OwnFieldInCell);
        jit.load64(BaseIndex(thisGPR, operand, CCallHelpers::TimesOne), calleeGPR);
        isNotAsked.append(jit.branchTest64(CCallHelpers::Zero, calleeGPR));
        jit.jumpToStub(callValue(count));

        isNotInCell.link(&jit);
        Jump isNotAbove = branchUnlessForm(jit, use, calleeGPR, Form::ValueInWordOfCage);
        jit.load64(BaseIndex(cellCageBaseGPR, operand, CCallHelpers::TimesEight), calleeGPR);
        jit.jumpToStub(callValue(count));

        // A value of the receiver's own in its butterfly.
        isNotAbove.link(&jit);
        Jump isNotInStorage = branchUnlessForm(jit, use, calleeGPR, Form::OwnValueInStorage);
        jit.loadPtr(Address(thisGPR, JSObject::butterflyOffset()), calleeGPR);
        jit.signExtend32ToPtr(operand, operand);
        jit.load64(BaseIndex(calleeGPR, operand, CCallHelpers::TimesEight), calleeGPR);
        jit.jumpToStub(callValue(count));

        // Among the extra properties of a compiled object, found as a place that reads finds it.
        // The run time files no other form for a call. SiteAnswers::forPlacesWithout() has the form Nothing.
        isNotInStorage.link(&jit);
        auto loadExtraProperties = [&] {
            jit.zeroExtend8To32(operand, calleeGPR);
            jit.load32(BaseIndex(thisGPR, calleeGPR, CCallHelpers::TimesFour), calleeGPR);
            isNotAsked.append(jit.branchTest32(CCallHelpers::Zero, calleeGPR));
            jit.rshift32(TrustedImm32(SiteAnswers::indexShiftInOperandOfExtraValue), operand);
        };
        Jump isNotExtraValueInStorage = branchUnlessForm(jit, use, calleeGPR, Form::ExtraValueInStorage);
        loadExtraProperties();
        jit.loadPtr(BaseIndex(cellCageBaseGPR, calleeGPR, CCallHelpers::TimesEight, JSObject::butterflyOffset()), calleeGPR);
        jit.signExtend32ToPtr(operand, operand);
        jit.load64(BaseIndex(calleeGPR, operand, CCallHelpers::TimesEight), calleeGPR);
        jit.jumpToStub(callValue(count));

        isNotExtraValueInStorage.link(&jit);
        differs.append(branchUnlessForm(jit, use, calleeGPR, Form::ExtraValueInCell));
        loadExtraProperties();
        jit.add32(operand, calleeGPR);
        jit.load64(BaseIndex(cellCageBaseGPR, calleeGPR, CCallHelpers::TimesEight), calleeGPR);
        jit.jumpToStub(callValue(count));
        differs.link(&jit);
    };
    hasOtherKey.link(&jit);
    callValueOfOtherForm(offsetOfFirstAnswer);
    // No second answer has the form that places meet most, and no arm is of it.
    answers.loadBitsOfForm(jit, offsetOfSecondAnswer, thisGPR, calleeGPR, operand, keyOfAnswer);
    callValueOfOtherForm(offsetOfSecondAnswer);

    // The code asks for such a place only under a name with a column.
    countMissAtSite(jit, thisGPR, distance, key, place, use, NameAtSite::HasColumn);
    isNotAsked.link(&jit);
    loadNameOfSite(jit, distance, distance, key, NameAtSite::HasColumn);
    jit.jumpToStub(callMethodById(count));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
