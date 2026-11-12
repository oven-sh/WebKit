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
#include "AOTConvention.h"
#include "AOTProgramTables.h"
#include "AOTPropertyCachesFront.h"
#include "AOTRuntimeTable.h"
#include "AOTStaleAnswers.h"
#include "AOTStubProgramTables.h"
#include "AOTStubsBoxedCalls.h"
#include "AOTStubsCalls.h"
#include "AOTStubsField.h"
#include "AOTStubsValues.h"
#include "ClonedArguments.h"
#include "DirectArguments.h"
#include "GetterSetter.h"
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

struct TableStubsBehindSite {
    StubKey front;
    std::optional<StubKey> hashed;
};

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

void countMissOfPlace(StubAssembler& jit, GPRReg distance, GPRReg scratch, JumpList& isSample)
{
    constexpr int32_t step = SiteCacheState::sampleStep;
    RELEASE_ASSERT(noOverlap(distance, scratch, instanceGPR));
    static_assert(InstanceLayout::bytesReservedForMissesOfPlaces == size_t { 1 } << 16);
    jit.zeroExtend16To32(distance, scratch);
#if CPU(ARM64)
    const OwnedScratchRegister ownedScratch(jit);
    const GPRReg misses = ownedScratch;
    jit.addPtr(instanceGPR, scratch);
    jit.addPtr(TrustedImm32(static_cast<int32_t>(InstanceLayout::offsetOfMissesOfPlaces)), scratch);
    jit.load8(Address(scratch), misses);
    jit.add32(TrustedImm32(step), misses);
    jit.store8(misses, Address(scratch));
    isSample.append(jit.branchTest32(CCallHelpers::NonZero, misses, TrustedImm32(UINT8_MAX + 1)));
#else
    jit.m_assembler.addb_im(step, static_cast<int32_t>(InstanceLayout::offsetOfMissesOfPlaces), instanceGPR, scratch, 0);
    isSample.append(Jump(jit.m_assembler.jCC(X86Assembler::ConditionC)));
#endif
}

void goOnWhileSamplesOfPlaceAreLeftOut(StubAssembler& jit, GPRReg scratchOfCount, CCallHelpers::Label isCounted)
{
#if CPU(ARM64)
    const OwnedScratchRegister ownedScratch(jit);
    const GPRReg leftOut = ownedScratch;
    jit.load8(Address(scratchOfCount, 1), leftOut);
    Jump isDue = jit.branchTest32(CCallHelpers::Zero, leftOut);
    jit.sub32(TrustedImm32(1), leftOut);
    jit.store8(leftOut, Address(scratchOfCount, 1));
    jit.jump().linkTo(isCounted, &jit);
    isDue.link(&jit);
#else
    jit.m_assembler.subb_im(1, static_cast<int32_t>(InstanceLayout::offsetOfMissesOfPlaces) + 1, instanceGPR, scratchOfCount, 0);
    Jump(jit.m_assembler.jCC(X86Assembler::ConditionNC)).linkTo(isCounted, &jit);
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

static void emitSiteMiss(StubAssembler& jit, const SiteRegisters& registers, SlotAccess access, SiteReceiver receiver, const TableStubsBehindSite& stubs)
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

void generateGetByIdAtSite(StubAssembler& jit)
{
    static constexpr SlotHandlerTable table = SlotHandlerTable::ReadById;
    const StubKey tables = getByIdCheckingColumn();
    const SiteRegisters registers = allocateSiteRegisters(jit, 1);

    Jump isNotCell = jit.branchIfNotCell(registers.receiver);
    jit.bindEntry(getByIdOfObjectAtSite());
    const GPRReg holder = registers.payload;
#if CPU(X86_64)
    const GPRReg wordIndex = registers.header;
    auto branchOnFlags = [&](X86Assembler::Condition condition) { return Jump(jit.m_assembler.jCC(condition)); };
    jit.load64(Address(registers.site, Slot::offsetOfStructureID()), registers.header);
    Jump hasOtherStructure = jit.branch32(CCallHelpers::NotEqual, registers.header, Address(registers.receiver, JSCell::structureIDOffset()));
    jit.m_assembler.cmpb_im(Slot::isOwn, Slot::offsetOfFlags(), registers.site);
    Jump isNotOwnInlineValue = branchOnFlags(X86Assembler::ConditionNE);
    jit.urshift64(TrustedImm32(Slot::locationShiftInHeader), wordIndex);
    Label readsCellOfReceiver = jit.label();
    jit.load64(BaseIndex(registers.receiver, wordIndex, CCallHelpers::TimesEight), jit.result());
    jit.returnToCaller();

    isNotOwnInlineValue.link(&jit);
    Jump isOwnOrNeedsHandler = branchOnFlags(X86Assembler::ConditionA);
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
    Jump hasNoColumn = jit.branch32(CCallHelpers::Above, registers.identifierNumber, TrustedImm32(RowDisplacementTable::maxColumn));
    {
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
    }
    jit.jumpToStub(tables);

    hasNoColumn.link(&jit);
    countSiteCacheMiss(jit, registers.site, registers.scratch, isDue);
    jit.jumpToStub(tables);

    isEmpty.link(&jit);
    JumpList isNotInFieldTable;
    returnFieldByTable(jit, { registers.receiver, registers.identifierNumber, jit.allocateScratch(StubRegister::T14), registers.header, false, true }, isNotInFieldTable);
    isNotInFieldTable.link(&jit);
    countMissOfEmptySite(jit, registers.site, registers.scratch, isDue);
    jit.jumpToStub(tables);

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

StubSignature signatureOfGetLengthAtSite(StubKey)
{
    return signatureOf(getById());
}

StubSignature signatureOfLengthIs(StubKey)
{
    const StubSignature reads = signatureOf(getById());
    StubSignature signature = signatureWithFlagsResult({ reads.operands[0] }, CCallHelpers::Zero);
    signature.immediate = reads.immediate;
    return signature;
}

StubSignature signatureOfGetLengthRunningNoCode(StubKey)
{
    return signatureOf(getById());
}

enum class LengthMiss : uint8_t { ReadsProperty, GivesEmpty };

static void emitLengthAtSite(StubAssembler& jit, std::optional<int32_t> asked, LengthMiss miss = LengthMiss::ReadsProperty)
{
    static_assert(ArrayWithUndecided + 2 == ArrayWithInt32 && ArrayWithUndecided + 4 == ArrayWithDouble && ArrayWithUndecided + 6 == ArrayWithContiguous);
    constexpr int32_t bitsThatNoListWithPlainStorageHas = 0xff & ~(6 | CopyOnWrite | MayHaveIndexedAccessors);
    const GPRReg receiver = jit.operand(0);
    const GPRReg scratch = jit.allocateScratch(StubRegister::T11);
    const GPRReg key = jit.allocateScratch(StubRegister::T13);
    RELEASE_ASSERT(noOverlap(receiver, scratch, key, jit.immediate()));
    JumpList goesOn;

    auto answer = [&](GPRReg length) {
        if (asked) {
            jit.returnFlags32(CCallHelpers::Equal, length, TrustedImm32(*asked));
            return;
        }
        boxInt32(jit, length, jit.result());
        jit.returnToCaller();
    };

    goesOn.append(jit.branchIfNotCell(receiver));
    jit.load8(Address(receiver, JSCell::indexingTypeAndMiscOffset()), scratch);
    jit.sub32(TrustedImm32(ArrayWithUndecided), scratch);
    Jump hasOtherStorage = jit.branchTest32(CCallHelpers::NonZero, scratch, TrustedImm32(bitsThatNoListWithPlainStorageHas));
    jit.loadPtr(Address(receiver, JSObject::butterflyOffset()), scratch);
    if (asked) {
        jit.load32(Address(scratch, Butterfly::offsetOfPublicLength()), scratch);
        answer(scratch);
    } else {
        jit.load32(Address(scratch, Butterfly::offsetOfPublicLength()), jit.result());
        jit.or64(numberTag, jit.result());
        jit.returnToCaller();
    }

    hasOtherStorage.link(&jit);
    jit.load8(Address(receiver, JSCell::indexingTypeAndMiscOffset()), scratch);
    Jump isNotArray = jit.branchTest32(CCallHelpers::Zero, scratch, TrustedImm32(IsArray));
    goesOn.append(jit.branchTest32(CCallHelpers::Zero, scratch, TrustedImm32(IndexingShapeMask)));
    jit.loadPtr(Address(receiver, JSObject::butterflyOffset()), scratch);
    jit.load32(Address(scratch, Butterfly::offsetOfPublicLength()), scratch);
    goesOn.append(jit.branch32(CCallHelpers::LessThan, scratch, TrustedImm32(0)));
    answer(scratch);

    isNotArray.link(&jit);
    Jump isNoString = jit.branch8(CCallHelpers::NotEqual, Address(receiver, JSCell::typeInfoTypeOffset()), TrustedImm32(StringType));
    jit.loadPtr(Address(receiver, JSString::offsetOfValue()), scratch);
    Jump isRope = jit.branchIfRopeStringImpl(scratch);
    jit.load32(Address(scratch, StringImpl::lengthMemoryOffset()), scratch);
    answer(scratch);

    isRope.link(&jit);
    jit.load32(Address(receiver, JSRopeString::offsetOfLength()), scratch);
    answer(scratch);

    isNoString.link(&jit);
    jit.load8(Address(receiver, JSCell::typeInfoTypeOffset()), key);
    jit.sub32(TrustedImm32(FirstTypedArrayType), key);
    Jump isNoView = jit.branch32(CCallHelpers::AboveOrEqual, key, TrustedImm32(NumberOfTypedArrayTypesExcludingDataView));
    Jump isAsItsClassMakesIt = jit.branchTest8(CCallHelpers::NonZero, Address(receiver, JSCell::typeInfoFlagsOffset()), TrustedImm32(IsPristine));
    jit.load32(Address(receiver, JSCell::structureIDOffset()), key);
    goesOn.append(jit.branch32(CCallHelpers::NotEqual, key, Address(instanceGPR, Instance::offsetOfReceiverStructureID(Receiver::Buffer))));
    isAsItsClassMakesIt.link(&jit);
    goesOn.append(jit.branchTest8(CCallHelpers::NonZero, Address(instanceGPR, AOT_INSTANCE_OFFSET(builtins.someClassOfViewsWasWrittenAbove))));
    if (jit.options().checksWalk) {
        jit.preservingCallerSavedRegisters(false, [&](unsigned) {
            jit.move(receiver, jit.savedScratch(StubRegister::A1));
            jit.move(TrustedImm64(JSValue::encode(JSValue())), jit.savedScratch(StubRegister::A2));
            jit.move(instanceGPR, jit.savedScratch(StubRegister::A0));
            jit.move(TrustedImm32(static_cast<int32_t>(BoxedCallForm::TypedArrayLength)), jit.savedScratch(StubRegister::A3));
            jit.callOperation(Entry::operationAOTStopAtStaleMemberOfBuiltinClass, jit.savedScratch(StubRegister::Operation));
        });
    }
    goesOn.append(jit.branchTest8(CCallHelpers::NonZero, Address(receiver, JSArrayBufferView::offsetOfMode()), TrustedImm32(resizabilityAndAutoLengthMask)));
    jit.load64(Address(receiver, JSArrayBufferView::offsetOfLength()), key);
    goesOn.append(jit.branch64(CCallHelpers::Above, key, TrustedImm32(std::numeric_limits<int32_t>::max())));
    answer(key);

    isNoView.link(&jit);
    constexpr int32_t offsetOfStructureOfArgumentsAsMade = AOT_INSTANCE_OFFSET(allocation.inlineAllocations[static_cast<unsigned>(InlineAllocation::Arguments)].header);
    static_assert(!JSCell::structureIDOffset());
    Jump isNotUnmapped = jit.branch32(CCallHelpers::NotEqual, key, TrustedImm32(ClonedArgumentsType - FirstTypedArrayType));
    jit.load32(Address(receiver, JSCell::structureIDOffset()), key);
    goesOn.append(jit.branch32(CCallHelpers::NotEqual, key, Address(instanceGPR, offsetOfStructureOfArgumentsAsMade)));
    jit.loadPtr(Address(receiver, JSObject::butterflyOffset()), scratch);
    if (asked) {
        jit.load64(Address(scratch, offsetInButterfly(clonedArgumentsLengthPropertyOffset) * sizeof(EncodedJSValue)), scratch);
        goesOn.append(jit.branchIfNotInt32(scratch));
        answer(scratch);
    } else {
        jit.load64(Address(scratch, offsetInButterfly(clonedArgumentsLengthPropertyOffset) * sizeof(EncodedJSValue)), jit.result());
        jit.returnToCaller();
    }

    isNotUnmapped.link(&jit);
    Jump isNotMapped = jit.branch32(CCallHelpers::NotEqual, key, TrustedImm32(DirectArgumentsType - FirstTypedArrayType));
    goesOn.append(jit.branchTestPtr(CCallHelpers::NonZero, Address(receiver, DirectArguments::offsetOfMappedArguments())));
    jit.load32(Address(receiver, DirectArguments::offsetOfLength()), scratch);
    answer(scratch);

    isNotMapped.link(&jit);
    goesOn.append(jit.branch32(CCallHelpers::NotEqual, key, TrustedImm32(SlimClosureType - FirstTypedArrayType)));
    jit.load64(Address(receiver, offsetOfSlimClosureWord), key);
    extractBits(jit, key, ClosureWord::functionShift, ClosureWord::numberOfFunctionBits, key);
    loadAddressOfClosureFacts(jit, key, scratch);
    jit.load8(Address(scratch, OBJECT_OFFSETOF(ClosureFacts, length)), key);
    goesOn.append(jit.branch32(CCallHelpers::Equal, key, TrustedImm32(countsAreListed)));
    answer(key);

    goesOn.link(&jit);
    if (miss == LengthMiss::GivesEmpty) {
        jit.move(TrustedImm64(JSValue::encode(JSValue())), jit.result());
        jit.returnToCaller();
        return;
    }
    if (!asked) {
        jit.jumpToStub(getByIdCheckingColumn());
        return;
    }
    jit.emitFunctionPrologue();
    jit.callStub(getByIdCheckingColumn().withCallerFrame(CallerFrame::StubFrame));
    jit.bindReturnLabel(StubFrameKind::MethodRead);
    jit.emitFunctionEpilogue();
    const GPRReg found = signatureOf(getById()).firstResult.gpr();
    RELEASE_ASSERT(noOverlap(found, scratch));
    Jump isNoInt32 = jit.branchIfNotInt32(found);
    jit.returnFlags32(CCallHelpers::Equal, found, TrustedImm32(*asked));

    isNoInt32.link(&jit);
    jit.move(TrustedImm64(JSValue::encode(JSValue(JSValue::EncodeAsDouble, static_cast<double>(*asked)))), scratch);
    if (*asked) {
        jit.returnFlags64(CCallHelpers::Equal, found, scratch);
        return;
    }
    Jump isZero = jit.branch64(CCallHelpers::Equal, found, scratch);
    jit.move(TrustedImm64(JSValue::encode(JSValue(JSValue::EncodeAsDouble, -0.0))), scratch);
    jit.returnFlags64(CCallHelpers::Equal, found, scratch);

    isZero.link(&jit);
    jit.returnFlagsConstant(true);
}

void generateGetLengthAtSite(StubAssembler& jit) { emitLengthAtSite(jit, std::nullopt); }

void generateGetLengthRunningNoCode(StubAssembler& jit) { emitLengthAtSite(jit, std::nullopt, LengthMiss::GivesEmpty); }

void generateLengthIs(StubAssembler& jit) { emitLengthAtSite(jit, static_cast<int32_t>(jit.key().count())); }

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

    if (storeAtSiteComesBehindComparedStructure(jit.key())) {
        jit.load64(Address(registers.site, Slot::offsetOfStructureID()), registers.header);
        emitSiteMiss(jit, registers, accessFor(mode), SiteReceiver::IsCell, { putByIdBehindFieldTable(mode), putByIdHashed(mode) });
        return;
    }
    if (siteStoreChecksReplacedMembers(jit.key()))
        storeToReplacedMemberAndReturn(jit, registers.receiver, registers.identifierNumber, jit.operand(1), registers.payload, registers.structureID);
    jit.linkToStub(jit.branchIfNotCell(registers.receiver), slotExit(table, SlotExit::Miss));
    if (storeAtSiteAsksTableOfFields(jit.key())) {
        JumpList isNotInFieldTable;
        storeFieldByTable(jit, { registers.receiver, registers.identifierNumber, registers.structureID, registers.header, jit.key().count() == static_cast<unsigned>(NameAtSite::HasColumn), true }, jit.operand(1), registers.payload, operationFor(mode), isNotInFieldTable);
        isNotInFieldTable.link(&jit);
    }
    jit.load32(Address(registers.receiver, JSCell::structureIDOffset()), registers.structureID);
    emitSiteProbe(jit, registers, table);
    emitSiteMiss(jit, registers, accessFor(mode), SiteReceiver::IsCell, { putByIdBehindFieldTable(mode), putByIdHashed(mode) });
}

StubSignature signatureOfFindReplaceableValue(StubKey)
{
    StubSignature signature = StubSignature::withGPRResult({ });
    signature.addOperand(gprOf(firstStubOperandRegister));
    signature.addOperand(gprOf(readSiteCacheRegister));
    signature.immediate = gprOf(slotNameRegister);
    return signature;
}

void generateFindReplaceableValue(StubAssembler& jit)
{
    const SiteRegisters registers = allocateSiteRegisters(jit, 1);
    const GPRReg name = registers.identifierNumber;
    const GPRReg cell = registers.header;
    const GPRReg result = jit.result();
    JumpList isDue;
    JumpList isInNoCell;
    countSiteCacheMiss(jit, registers.site, registers.scratch, isDue);
    isInNoCell.append(jit.branch32(CCallHelpers::Above, name, TrustedImm32(RowDisplacementTable::maxColumn)));
    jit.load32(Address(registers.receiver, JSCell::structureIDOffset()), registers.structureID);
    loadFrontCell(jit, SlotAccess::Store, { registers.structureID, name, registers.payload, cell, registers.scratch });
    Jump isNotOwnInlineValue = branchUnlessOwnInlineValue(jit, name, cell, registers.scratch);
    jit.zeroExtend32ToWord(cell, cell);
    jit.getEffectiveAddress(BaseIndex(registers.receiver, cell, CCallHelpers::TimesEight), result);
    jit.returnToCaller();

    isNotOwnInlineValue.link(&jit);
    isInNoCell.append(branchIfCellNameDiffers(jit, name, cell, registers.scratch));
    extractBits(jit, cell, 8 * PackedSlot::offsetOfBiasedKindAndFlags(), 8, registers.scratch);
    static_assert(((static_cast<uint8_t>(Slot::Kind::Value) | PackedSlot::isOwn | PackedSlot::isOutOfLine) ^ PackedSlot::ownInlineValueBits) == PackedSlot::isOutOfLine);
    isInNoCell.append(jit.branch32(CCallHelpers::NotEqual, registers.scratch, TrustedImm32(PackedSlot::isOutOfLine)));
    jit.load8(Address(registers.receiver, JSCell::typeInfoTypeOffset()), registers.scratch);
    jit.sub32(TrustedImm32(typesWithMovingStorage.first), registers.scratch);
    isInNoCell.append(jit.branch32(CCallHelpers::BelowOrEqual, registers.scratch, TrustedImm32(typesWithMovingStorage.last - typesWithMovingStorage.first)));
    jit.signExtend32ToPtr(cell, cell);
    jit.loadPtr(Address(registers.receiver, JSObject::butterflyOffset()), registers.scratch);
    jit.getEffectiveAddress(BaseIndex(registers.scratch, cell, CCallHelpers::TimesEight), result);
    jit.returnToCaller();

    isDue.link(&jit);
    jit.store16(TrustedImm32(0), Address(registers.scratch));
    isInNoCell.link(&jit);
    jit.move(TrustedImm32(0), result);
    jit.returnToCaller();
}

static_assert(!OBJECT_OFFSETOF(SiteAnswers, first) && OBJECT_OFFSETOF(SiteAnswers, second) == sizeof(SiteAnswers::Answer) && !OBJECT_OFFSETOF(SiteAnswers::Answer, key) && OBJECT_OFFSETOF(SiteAnswers::Answer, operand) == sizeof(uint32_t));
static_assert(cell32Shift == CCallHelpers::TimesEight);

namespace {

class AnswersOfStub {
public:
    AnswersOfStub(StubAssembler& jit, GPRReg distance, GPRReg place)
        : m_place(place)
    {
        static_assert(InstanceLayout::sizeOfIndexOfSiteAnswers == sizeof(uint16_t));
        jit.load16(BaseIndex(instanceGPR, distance, CCallHelpers::TimesOne), place);
        locate(jit);
    }
    static AnswersOfStub atLoadedIndex(StubAssembler& jit, GPRReg place)
    {
        AnswersOfStub answers(place);
        answers.locate(jit);
        return answers;
    }
    static AnswersOfStub atLocatedPlace(GPRReg place) { return AnswersOfStub(place); }
#if CPU(X86_64)
    using Word = CCallHelpers::BaseIndex;
    Word word(ptrdiff_t offset) const { return Word(instanceGPR, m_place, CCallHelpers::TimesEight, static_cast<int32_t>(InstanceLayout::offsetOfSiteAnswers + offset)); }
#else
    using Word = CCallHelpers::Address;
    Word word(ptrdiff_t offset) const { return Word(m_place, static_cast<int32_t>(offset)); }
#endif

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
    explicit AnswersOfStub(GPRReg place)
        : m_place(place)
    {
    }
    void locate([[maybe_unused]] StubAssembler& jit) const
    {
#if !CPU(X86_64)
        jit.getEffectiveAddress(BaseIndex(instanceGPR, m_place, CCallHelpers::TimesEight), m_place);
        jit.addPtr(TrustedImm32(static_cast<int32_t>(InstanceLayout::offsetOfSiteAnswers)), m_place);
#endif
    }

    GPRReg m_place;
};

} // anonymous namespace

static constexpr ptrdiff_t offsetOfFirstAnswer = OBJECT_OFFSETOF(SiteAnswers, first);
static constexpr ptrdiff_t offsetOfSecondAnswer = OBJECT_OFFSETOF(SiteAnswers, second);

static Jump branchUnlessIndexIsOfWideAnswers(StubAssembler& jit, GPRReg index, GPRReg scratch)
{
    const Address firstWord(instanceGPR, AOT_INSTANCE_OFFSET(propertyCaches.firstWordOfWideAnswers));
#if CPU(X86_64)
    UNUSED_PARAM(scratch);
    return jit.branch32(CCallHelpers::Below, index, firstWord);
#else
    jit.load32(firstWord, scratch);
    return jit.branch32(CCallHelpers::Below, index, scratch);
#endif
}

static Jump branchUnlessAnswersAreWide(StubAssembler& jit, GPRReg distance, GPRReg index, GPRReg scratch)
{
    jit.load16(BaseIndex(instanceGPR, distance, CCallHelpers::TimesOne), index);
    return branchUnlessIndexIsOfWideAnswers(jit, index, scratch);
}

static Jump branchUnlessForm(StubAssembler& jit, SiteUse use, GPRReg bitsOfForm, SiteAnswers::Form form)
{
    return jit.branch32(CCallHelpers::NotEqual, bitsOfForm, TrustedImm32(static_cast<int32_t>(SiteAnswers::bitsOf(use, form))));
}

static void loadNameOfSiteOrMark(StubAssembler& jit, GPRReg distance, GPRReg name, GPRReg scratch)
{
    RELEASE_ASSERT(noOverlap(distance, scratch) && noOverlap(name, scratch));
    static_assert(sizeof(uint16_t) == InstanceLayout::sizeOfIndexOfSiteAnswers && RowDisplacementTable::maxColumn + 1 == InstanceLayout::nameOfSiteStandsBehind);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.namesOfSitesByDistance)), scratch);
    if (name == distance) {
        jit.addPtr(distance, scratch);
        jit.load16(Address(scratch), name);
        return;
    }
    jit.load16(BaseIndex(scratch, distance, CCallHelpers::TimesOne), name);
}

static void loadNameBehindMarkOfSite(StubAssembler& jit, GPRReg distance, GPRReg name, GPRReg scratch)
{
    if (name == distance)
        jit.load32(Address(scratch, sizeof(uint16_t)), name);
    else
        jit.load32(BaseIndex(scratch, distance, CCallHelpers::TimesOne, sizeof(uint16_t)), name);
}

static void loadNameOfSite(StubAssembler& jit, GPRReg distance, GPRReg name, GPRReg scratch)
{
    loadNameOfSiteOrMark(jit, distance, name, scratch);
    Jump standsThere = jit.branch32(CCallHelpers::BelowOrEqual, name, TrustedImm32(RowDisplacementTable::maxColumn));
    loadNameBehindMarkOfSite(jit, distance, name, scratch);
    standsThere.link(&jit);
}

static void fillAnswersAtSite(StubAssembler& jit, GPRReg receiver, GPRReg name, GPRReg distance, SiteUse use, uint32_t whenToFill = 0)
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
        jit.move(TrustedImm32(static_cast<int32_t>(static_cast<uint32_t>(use) | whenToFill)), A4);
        jit.callOperation(Entry::operationAOTFillSiteAnswers, jit.reuseScratch(StubRegister::Operation));
    });
}

static CCallHelpers::Jump countHitOfFurtherAnswer(StubAssembler& jit, [[maybe_unused]] GPRReg scratch)
{
    constexpr int32_t offsetOfCounter = AOT_INSTANCE_OFFSET(propertyCaches.hitsOfFurtherAnswers);
#if CPU(ARM64)
    jit.load8(Address(instanceGPR, offsetOfCounter), scratch);
    jit.add32(TrustedImm32(1), scratch);
    jit.store8(scratch, Address(instanceGPR, offsetOfCounter));
    return jit.branchTest32(CCallHelpers::NonZero, scratch, TrustedImm32(UINT8_MAX + 1));
#else
    jit.m_assembler.addb_im(1, offsetOfCounter, instanceGPR);
    return CCallHelpers::Jump(jit.m_assembler.jCC(X86Assembler::ConditionC));
#endif
}

static void countMissAtSite(StubAssembler& jit, GPRReg receiver, GPRReg distance, GPRReg name, GPRReg scratch, SiteUse use, uint32_t whatStubDoes = 0)
{
    RELEASE_ASSERT(noOverlap(receiver, distance, name, scratch, instanceGPR));
    JumpList isSample;
    countMissOfPlace(jit, distance, scratch, isSample);
    Label isCounted = jit.label();
    jit.addOutOfLinePart([=](StubAssembler& jit) mutable {
        isSample.link(&jit);
        goOnWhileSamplesOfPlaceAreLeftOut(jit, scratch, isCounted);
        loadNameOfSite(jit, distance, name, scratch);
        fillAnswersAtSite(jit, receiver, name, distance, use, whatStubDoes);
        jit.jump().linkTo(isCounted, &jit);
    });
}

StubSignature signatureOfReadAtSite(StubKey)
{
    StubSignature signature = StubSignature::withGPRResult({ gprOf(receiverAtSiteRegister) });
    signature.immediate = gprOf(shortStubImmediateRegister);
    return signature.fillingOperandInFront(0);
}

StubSignature signatureOfReadBehindSite(StubKey)
{
    StubSignature signature = StubSignature::withGPRResult({ gprOf(firstStubOperandRegister) });
    signature.immediate = gprOf(shortStubImmediateRegister);
    return signature;
}

StubSignature signatureOfReadTruthAtSite(StubKey key)
{
    const StubSignature reads = signatureOfReadAtSite(key);
    StubSignature signature = signatureWithFlagsResult({ reads.operands[0] }, signatureOf(valueIsTruthy()).trueCondition);
    signature.immediate = reads.immediate;
    return signature.fillingOperandInFront(0);
}

struct JumpsToTwoForms {
    Jump toLower;
    Jump toUpper;
};

static JumpsToTwoForms branchIfEitherOfTwoForms(StubAssembler& jit, GPRReg key, SiteUse use, SiteAnswers::Form lower, SiteAnswers::Form upper)
{
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
}

struct RegistersOfFieldThatDifferenceNames {
    GPRReg receiver;
    GPRReg difference;
    GPRReg operand;
    GPRReg key;
    GPRReg place;
    GPRReg extension;
    GPRReg value;
    GPRReg result;
};

struct WaysOutOfFieldThatDifferenceNames {
    JumpList& slotIsEmpty;
    JumpList& isNotAnswered;
    JumpList& isNotAsked;
};

static void readFieldThatDifferenceNames(StubAssembler& jit, const RegistersOfFieldThatDifferenceNames& registersAtSite, WaysOutOfFieldThatDifferenceNames ways, Jump isOwnNarrowReferenceByStructure, Jump isOwnInt32ByStructure, Jump isOwnFieldInExtensionByStructure)
{
    const GPRReg receiver = registersAtSite.receiver;
    const GPRReg difference = registersAtSite.difference;
    const GPRReg operand = registersAtSite.operand;
    const GPRReg key = registersAtSite.key;
    const GPRReg extension = registersAtSite.extension;
    const GPRReg value = registersAtSite.value;
    const GPRReg result = registersAtSite.result;
    const GPRReg laterGranules = operand;
    const GPRReg address = registersAtSite.place;
    const FieldReadRegisters typed {
        .field = { .object = receiver, .slotData = InvalidGPRReg, .extensionWordOffset = InvalidGPRReg, .address = address, .representation = operand },
        .value = key,
        .result = result,
        .number = jit.allocateScratch(FPRInfo::fpRegT0),
        .numberScratch = jit.allocateScratch(FPRInfo::fpRegT1),
    };
    FieldReadExits exits;
    auto readWhereDifferenceNames = [&](SlotRepresentation representation) {
        Jump isOfAnotherForm = jit.branch32(CCallHelpers::NotEqual, difference, TrustedImm32(static_cast<int32_t>(SiteAnswers::bitTurnedForSlotInCell(representation))));
        if (representation == SlotRepresentation::OptionalCell32 && isOwnNarrowReferenceByStructure.isSet())
            isOwnNarrowReferenceByStructure.link(&jit);
        if (representation == SlotRepresentation::OptionalInt32 && isOwnInt32ByStructure.isSet())
            isOwnInt32ByStructure.link(&jit);
        FieldReadRegisters registers = typed;
        if (representationWithCodeForRead(representation) <= representationsReadAsInt32.back())
            registers.slot = BaseIndex(receiver, operand, CCallHelpers::TimesOne);
        else
            jit.getEffectiveAddress(BaseIndex(receiver, operand, CCallHelpers::TimesOne), address);
        {
            AllowMacroScratchRegisterUsage operandIsFreeBehindTheLoad(jit);
            readFieldAndReturn(jit, registers, exits, representation);
        }
        isOfAnotherForm.link(&jit);
    };
    readWhereDifferenceNames(SlotRepresentation::OptionalInt32);
    readWhereDifferenceNames(SlotRepresentation::OptionalCell32);
    Jump isNotInExtension = jit.branch32(CCallHelpers::NotEqual, difference, TrustedImm32(static_cast<int32_t>(SiteAnswers::bitTurnedForFieldInExtension)));
    if (isOwnFieldInExtensionByStructure.isSet())
        isOwnFieldInExtensionByStructure.link(&jit);
    jit.load32(Address(receiver, offsetOfFirstField), extension);
    jit.getEffectiveAddress(BaseIndex(cellCageBaseGPR, extension, static_cast<CCallHelpers::Scale>(cell32Shift), offsetOfEmptyExtensionInCage), extension);
    jit.load64(Address(extension, offsetOfExtensionGranuleBits), key);
    jit.lshift64(laterGranules, key);
    ways.slotIsEmpty.append(jit.branchTest64(CCallHelpers::PositiveOrZero, key));
    jit.countPopulation64(key, key, CCallHelpers::fpTempRegister);
    jit.load64(BaseIndex(extension, key, CCallHelpers::TimesEight), value);
    ways.slotIsEmpty.append(jit.branchTest64(CCallHelpers::Zero, value));
    jit.move(value, result);
    jit.returnToCaller();

    isNotInExtension.link(&jit);
    for (SlotRepresentation representation : { SlotRepresentation::OptionalDouble, SlotRepresentation::OptionalBoolean, SlotRepresentation::OptionalInt64 })
        readWhereDifferenceNames(representation);
    ways.isNotAnswered.append(jit.jump());
    ways.slotIsEmpty.append(exits.isAbsent);
    ways.isNotAsked.append(exits.cannot);
}

static void returnFieldUnlessSlotIsEmpty(StubAssembler& jit, GPRReg receiver, GPRReg offset, [[maybe_unused]] GPRReg value, GPRReg result, JumpList& slotIsEmpty)
{
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
}

namespace {

struct RegistersOfAnswerAtSite {
    GPRReg receiver;
    GPRReg place;
    GPRReg key;
    GPRReg operand;

    friend constexpr bool operator==(const RegistersOfAnswerAtSite&, const RegistersOfAnswerAtSite&) = default;
};

struct ReadAtSiteInWork {
    StubAssembler& jit;
    bool asksTruth;
    NameAtSite name;
    SiteUse use;
    GPRReg receiver;
    GPRReg distance;
    GPRReg result;

    void goOnIn(StubKey next) const
    {
        if (!asksTruth) {
            jit.jumpToStub(next);
            return;
        }
        jit.emitFunctionPrologue();
        jit.callStub(next.withCallerFrame(CallerFrame::StubFrame));
        jit.bindReturnLabel(StubFrameKind::MethodRead);
        jit.emitFunctionEpilogue();
        jit.returnToCaller();
    }

    void returnUndefined() const
    {
        if (!asksTruth) {
            jit.move(TrustedImm64(JSValue::ValueUndefined), result);
            jit.returnToCaller();
            return;
        }
        RELEASE_ASSERT(signatureOf(valueIsTruthy()).trueCondition == CCallHelpers::NonZero);
        jit.move(TrustedImm32(0), result);
        jit.goOnAtEveryReturnIn(std::nullopt);
        jit.returnFlagsTest32(result, TrustedImm32(1));
        jit.goOnAtEveryReturnIn(valueIsTruthy());
    }

    void goOnBehindSite(WayBehindSite way) const
    {
        checkStubOperands(readBehindSite(way, name, use), { gprOf(firstStubOperandRegister) });
        jit.move(receiver, gprOf(firstStubOperandRegister));
        if (distance != gprOf(shortStubImmediateRegister))
            jit.move(distance, jit.clobberRegister(shortStubImmediateRegister));
        goOnIn(readBehindSite(way, name, use));
    }
};

} // anonymous namespace

static constexpr RegistersOfAnswerAtSite registersOfAnswerAtSite()
{
#if CPU(X86_64)
    return { gprOf(receiverAtSiteRegister), gprOf(StubRegister::T11), gprOf(StubRegister::T13), gprOf(StubRegister::T14) };
#else
    return { gprOf(receiverAtSiteRegister), gprOf(StubRegister::T14), gprOf(StubRegister::T11), gprOf(StubRegister::T13) };
#endif
}

static void jumpToHandlerOfAnswer(StubAssembler& jit, AnswerHandlerTable table, GPRReg bitsOfForm)
{
    const std::array handlers = answerHandlerKeys(table);
    const int32_t offsetOfHandlers = static_cast<int32_t>(Instance::offsetOfAnswerHandlers(table));
    RELEASE_ASSERT(noOverlap(bitsOfForm, registersOfAnswerAtSite().receiver, registersOfAnswerAtSite().place, registersOfAnswerAtSite().operand, jit.immediate(), instanceGPR));
    static_assert(sizeof(Instance) / sizeof(void*) <= 1 << 12);
#if CPU(ARM64)
    jit.add32(TrustedImm32(offsetOfHandlers / static_cast<int32_t>(sizeof(void*))), bitsOfForm);
    jit.jumpThroughHandlerTable(BaseIndex(instanceGPR, bitsOfForm, CCallHelpers::TimesEight), handlers);
#else
    jit.jumpThroughHandlerTable(BaseIndex(instanceGPR, bitsOfForm, CCallHelpers::TimesEight, offsetOfHandlers), handlers);
#endif
}

static Jump branchUnlessCompiledObject(StubAssembler& jit, GPRReg cell)
{
    return jit.branchTest8(CCallHelpers::Zero, Address(cell, offsetOfShapeWord + 3), TrustedImm32(compiledObjectShapeBit >> 24));
}

static void answerWhereSlotIsEmpty(const ReadAtSiteInWork& read, GPRReg place, GPRReg key, GPRReg operand, [[maybe_unused]] GPRReg keyOfAnswer, JumpList& slotIsEmpty, JumpList& findsNothing, JumpList& isNotAsked)
{
    StubAssembler& jit = read.jit;
    const GPRReg receiver = read.receiver;
    const GPRReg distance = read.distance;
    const GPRReg result = read.result;
    slotIsEmpty.link(&jit);
    isNotAsked.append(jit.branchTest32(CCallHelpers::NonZero, Address(receiver, offsetOfOrderWord), TrustedImm32(static_cast<int32_t>(orderWordHasFieldOutsideSlot))));
    loadStructure(jit, receiver, key);
    Jump isNotBelowObjectPrototype = jit.branchTest8(CCallHelpers::Zero, Address(key, CompiledStructure::offsetOfFlags()), TrustedImm32(static_cast<uint8_t>(CompiledStructure::Flag::HasOnlyFieldsBelowObjectPrototype)));
    loadNameOfSiteOrMark(jit, distance, key, place);
    if (read.name == NameAtSite::IsAnyName)
        isNotAsked.append(jit.branch32(CCallHelpers::Above, key, TrustedImm32(RowDisplacementTable::maxColumn)));
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(propertyCaches.namesOfObjectPrototypes)), place);
    jit.urshift32(key, TrustedImm32(6), operand);
    jit.load64(BaseIndex(place, operand, CCallHelpers::TimesEight), place);
    isNotAsked.append(branchTestBitAt(jit, CCallHelpers::NonZero, place, key));
    findsNothing.link(&jit);
    read.returnUndefined();

    JumpList isNotAnsweredAbove;
    isNotBelowObjectPrototype.link(&jit);
    isNotAnsweredAbove.append(branchUnlessAnswersAreWide(jit, distance, place, key));
    const AnswersOfStub wideAnswers = AnswersOfStub::atLoadedIndex(jit, place);
    jit.load32(Address(receiver, JSCell::structureIDOffset()), key);
#if CPU(X86_64)
    isNotAnsweredAbove.append(jit.branch32(CCallHelpers::NotEqual, wideAnswers.word(OBJECT_OFFSETOF(WideSiteAnswers, structureThatLeavesFieldOut)), key));
    jit.load32(wideAnswers.word(OBJECT_OFFSETOF(WideSiteAnswers, holderOrWordAbove)), operand);
#else
    static_assert(OBJECT_OFFSETOF(WideSiteAnswers, holderOrWordAbove) == OBJECT_OFFSETOF(WideSiteAnswers, structureThatLeavesFieldOut) + sizeof(uint32_t));
    jit.loadPair32(place, TrustedImm32(static_cast<int32_t>(OBJECT_OFFSETOF(WideSiteAnswers, structureThatLeavesFieldOut))), keyOfAnswer, operand);
    isNotAnsweredAbove.append(jit.branch32(CCallHelpers::NotEqual, key, keyOfAnswer));
#endif
    jit.load32SignedExtendTo64(wideAnswers.word(OBJECT_OFFSETOF(WideSiteAnswers, indexInStorageOfHolderAbove)), key);
    Jump standsInStorage = jit.branchTest64(CCallHelpers::NonZero, key);
    jit.load64(BaseIndex(cellCageBaseGPR, operand, CCallHelpers::TimesEight), operand);
    Label isLoaded = jit.label();
    isNotAnsweredAbove.append(jit.branchTest64(CCallHelpers::Zero, operand));
    jit.move(operand, result);
    jit.returnToCaller();

    standsInStorage.link(&jit);
    jit.loadPtr(BaseIndex(cellCageBaseGPR, operand, CCallHelpers::TimesEight, JSObject::butterflyOffset()), operand);
    jit.load64(BaseIndex(operand, key, CCallHelpers::TimesEight), operand);
    jit.jump().linkTo(isLoaded, &jit);

    isNotAnsweredAbove.link(&jit);
    read.goOnBehindSite(WayBehindSite::OfFieldLeftOut);
}

StubSignature signatureOfReadAnswerAtSite(StubKey key)
{
    constexpr RegistersOfAnswerAtSite registers = registersOfAnswerAtSite();
    StubSignature signature = readAnswerAtSiteFormOf(key).asksTruth
        ? signatureWithFlagsResult({ registers.receiver, registers.place, registers.key, registers.operand }, signatureOf(valueIsTruthy()).trueCondition)
        : StubSignature::withGPRResult({ registers.receiver, registers.place, registers.key, registers.operand });
    signature.immediate = gprOf(shortStubImmediateRegister);
    return signature;
}

void generateReadAnswerAtSite(StubAssembler& jit)
{
    using Form = SiteAnswers::Form;
    const bool asksTruth = readAnswerAtSiteFormOf(jit.key()).asksTruth;
    const SiteUse use = readAnswerAtSiteFormOf(jit.key()).use;
    const unsigned number = jit.key().count();
    const bool indexLiesBesideForm = number > SiteAnswers::formMask;
    const Form form = indexLiesBesideForm ? WideSiteAnswers::formsOfFurtherAnswersWithIndex[number - SiteAnswers::formMask - 1] : SiteAnswers::formOf(use, number);
    const GPRReg receiver = jit.operand(0);
    const GPRReg place = jit.clobberOperand(1);
    const GPRReg key = jit.clobberOperand(2);
    const GPRReg operand = jit.clobberOperand(3);
    const GPRReg distance = jit.immediate();
    const GPRReg result = asksTruth ? jit.clobberRegister(signatureOf(valueIsTruthy()).operands[0].gpr()) : jit.result();
    if (asksTruth)
        jit.goOnAtEveryReturnIn(valueIsTruthy());
#if CPU(X86_64)
    const GPRReg keyOfAnswer = InvalidGPRReg;
#else
    const GPRReg keyOfAnswer = jit.allocateScratch(StubRegister::T12);
#endif
    const GPRReg difference = jit.allocateScratch(distance == gprOf(StubRegister::T15) ? StubRegister::T10 : StubRegister::T15);
    RELEASE_ASSERT(noOverlap(receiver, distance, place, key, operand, difference));
    const GPRReg value = receiver == result ? operand : result;
    const ReadAtSiteInWork read { jit, asksTruth, NameAtSite::IsAnyName, use, receiver, distance, result };
    const AnswersOfStub answers = AnswersOfStub::atLocatedPlace(place);
    JumpList isNotAsked;
    JumpList slotIsEmpty;
    JumpList slotOfAnyObjectIsEmpty;
    JumpList isSampledHit;

    const StubKey callOfGetter = callGetterOfAnswer(jit.key().callerFrame());
    const GPRReg receiverOfGetter = gprOf(firstStubOperandRegister);
    auto callGetterIn = [&](BaseIndex word) {
        const GPRReg getterSetter = jit.clobberRegister(StubRegister::T10);
        checkStubOperands(callOfGetter, { receiverOfGetter, getterSetter });
        RELEASE_ASSERT(noOverlap(receiverOfGetter, getterSetter, key, operand, distance, place) && receiverOfGetter != difference);
        jit.load64(word, getterSetter);
        branchUnlessAccessorPair(jit, getterSetter, isNotAsked);
        read.goOnIn(callOfGetter);
    };
    auto loadStorageOfHolderAndIndex = [&]() -> GPRReg {
        if (!indexLiesBesideForm) {
            jit.loadPtr(BaseIndex(cellCageBaseGPR, operand, CCallHelpers::TimesEight, JSObject::butterflyOffset()), operand);
            jit.load32SignedExtendTo64(answers.word(offsetOfFirstAnswer + OBJECT_OFFSETOF(SiteAnswers::Answer, operand)), key);
            return key;
        }
        static_assert(std::endian::native == std::endian::little && sizeof(WideSiteAnswers::FormOfFurtherAnswer) == sizeof(uint16_t) && !OBJECT_OFFSETOF(WideSiteAnswers::FormOfFurtherAnswer, bits));
        isSampledHit.append(countHitOfFurtherAnswer(jit, difference));
        jit.move(key, difference);
        jit.lshift64(TrustedImm32(48), difference);
        jit.rshift64(TrustedImm32(56), difference);
        jit.loadPtr(BaseIndex(cellCageBaseGPR, operand, CCallHelpers::TimesEight, JSObject::butterflyOffset()), operand);
        return difference;
    };
    auto loadExtraProperties = [&] {
        jit.zeroExtend8To32(operand, key);
        jit.load32(BaseIndex(receiver, key, CCallHelpers::TimesFour), key);
        isNotAsked.append(jit.branchTest32(CCallHelpers::Zero, key));
        jit.rshift32(TrustedImm32(SiteAnswers::indexShiftInOperandOfExtraValue), operand);
    };
    auto readTypedSlot = [&](SlotRepresentation representation) {
        FieldReadRegisters registers {
            .field = { .object = receiver, .slotData = InvalidGPRReg, .extensionWordOffset = InvalidGPRReg, .address = place, .representation = operand },
            .value = key,
            .result = result,
            .number = jit.allocateScratch(FPRInfo::fpRegT0),
            .numberScratch = jit.allocateScratch(FPRInfo::fpRegT1),
        };
        FieldReadExits exits;
        if (representationWithCodeForRead(representation) <= representationsReadAsInt32.back())
            registers.slot = BaseIndex(receiver, operand, CCallHelpers::TimesOne);
        else
            jit.getEffectiveAddress(BaseIndex(receiver, operand, CCallHelpers::TimesOne), place);
        {
            AllowMacroScratchRegisterUsage operandIsFreeBehindTheLoad(jit);
            readFieldAndReturn(jit, registers, exits, representation);
        }
        slotIsEmpty.append(exits.isAbsent);
        isNotAsked.append(exits.cannot);
    };

    switch (form) {
    case Form::ValueInStorageOfHolder: {
        const GPRReg index = loadStorageOfHolderAndIndex();
        jit.load64(BaseIndex(operand, index, CCallHelpers::TimesEight), result);
        jit.returnToCaller();
        break;
    }
    case Form::ValueInWordOfCage:
        jit.load64(BaseIndex(cellCageBaseGPR, operand, CCallHelpers::TimesEight), result);
        jit.returnToCaller();
        break;
    case Form::OwnFieldInCell:
        returnFieldUnlessSlotIsEmpty(jit, receiver, operand, value, result, use == SiteUse::ReadUnderNameOfField ? slotOfAnyObjectIsEmpty : slotIsEmpty);
        break;
    case Form::OwnValueInCell:
        jit.load64(BaseIndex(receiver, operand, CCallHelpers::TimesOne), result);
        jit.returnToCaller();
        break;
    case Form::ExtraValueInStorage:
        loadExtraProperties();
        jit.loadPtr(BaseIndex(cellCageBaseGPR, key, CCallHelpers::TimesEight, JSObject::butterflyOffset()), key);
        jit.signExtend32ToPtr(operand, operand);
        jit.load64(BaseIndex(key, operand, CCallHelpers::TimesEight), result);
        jit.returnToCaller();
        break;
    case Form::ExtraValueInCell:
        loadExtraProperties();
        jit.add32(operand, key);
        jit.load64(BaseIndex(cellCageBaseGPR, key, CCallHelpers::TimesEight), result);
        jit.returnToCaller();
        break;
    case Form::LengthOfTypedArray:
        isNotAsked.append(jit.branchTest8(CCallHelpers::NonZero, Address(receiver, JSArrayBufferView::offsetOfMode()), TrustedImm32(resizabilityAndAutoLengthMask)));
        jit.load64(Address(receiver, JSArrayBufferView::offsetOfLength()), operand);
        isNotAsked.append(jit.branch64(CCallHelpers::Above, operand, TrustedImm32(std::numeric_limits<int32_t>::max())));
        boxInt32(jit, operand, result);
        jit.returnToCaller();
        break;
    case Form::GetterInStorageOfHolder: {
        const GPRReg index = loadStorageOfHolderAndIndex();
        jit.move(receiver, receiverOfGetter);
        callGetterIn(BaseIndex(operand, index, CCallHelpers::TimesEight));
        break;
    }
    case Form::GetterInWordOfCage:
        jit.move(receiver, receiverOfGetter);
        callGetterIn(BaseIndex(cellCageBaseGPR, operand, CCallHelpers::TimesEight));
        break;
    case Form::OwnGetterInStorage:
        jit.move(receiver, receiverOfGetter);
        jit.loadPtr(Address(receiverOfGetter, JSObject::butterflyOffset()), key);
        jit.signExtend32ToPtr(operand, operand);
        callGetterIn(BaseIndex(key, operand, CCallHelpers::TimesEight));
        break;
    case Form::OwnGetterInCell:
        jit.move(receiver, receiverOfGetter);
        callGetterIn(BaseIndex(receiverOfGetter, operand, CCallHelpers::TimesOne));
        break;
    case Form::OwnNarrowReferenceInCell:
        readTypedSlot(SlotRepresentation::OptionalCell32);
        break;
    case Form::OwnInt32InCell:
        readTypedSlot(SlotRepresentation::OptionalInt32);
        break;
    case Form::OwnValueInStorage:
        if (use == SiteUse::ReadUnderNameOfField) {
            jit.loadPtr(Address(receiver, JSObject::butterflyOffset()), key);
            jit.signExtend32ToPtr(operand, operand);
            jit.load64(BaseIndex(key, operand, CCallHelpers::TimesEight), result);
            jit.returnToCaller();
            break;
        }
        read.goOnBehindSite(WayBehindSite::AfterMiss);
        break;
    case Form::Nothing: {
        const GPRReg object = gprOf(firstStubOperandRegister);
        const GPRReg name = distance == gprOf(slotNameRegister) ? distance : jit.clobberRegister(slotNameRegister);
        const GPRReg cell = distance == gprOf(slotHeaderRegister) ? distance : jit.clobberRegister(slotHeaderRegister);
        RELEASE_ASSERT(noOverlap(object, name, cell, place, key, operand, instanceGPR) && noOverlap(receiver, name, place));
        checkStubOperands(getByIdAfterFrontProbe(), { object, cell });
        loadNameOfSiteOrMark(jit, distance, name, place);
        jit.move(receiver, object);
        jit.load32(Address(object, JSCell::structureIDOffset()), key);
        loadFrontCell(jit, SlotAccess::Read, { key, name, operand, cell, place });
        read.goOnIn(getByIdAfterFrontProbe());
        break;
    }
    case Form::OwnFieldInExtension: {
#if CPU(X86_64)
        const GPRReg extension = place;
#else
        const GPRReg extension = keyOfAnswer;
#endif
        jit.load32(Address(receiver, offsetOfFirstField), extension);
        jit.getEffectiveAddress(BaseIndex(cellCageBaseGPR, extension, static_cast<CCallHelpers::Scale>(cell32Shift), offsetOfEmptyExtensionInCage), extension);
        jit.load64(Address(extension, offsetOfExtensionGranuleBits), key);
        jit.lshift64(operand, key);
        slotIsEmpty.append(jit.branchTest64(CCallHelpers::PositiveOrZero, key));
        jit.countPopulation64(key, key, CCallHelpers::fpTempRegister);
        jit.load64(BaseIndex(extension, key, CCallHelpers::TimesEight), value);
        slotIsEmpty.append(jit.branchTest64(CCallHelpers::Zero, value));
        jit.move(value, result);
        jit.returnToCaller();
        break;
    }
    }

    if (!slotOfAnyObjectIsEmpty.empty()) {
        slotOfAnyObjectIsEmpty.link(&jit);
        isNotAsked.append(branchUnlessCompiledObject(jit, receiver));
    }
    if (!slotIsEmpty.empty() || !slotOfAnyObjectIsEmpty.empty()) {
        JumpList findsNothing;
        answerWhereSlotIsEmpty(read, place, key, operand, keyOfAnswer, slotIsEmpty, findsNothing, isNotAsked);
    }
    if (!isNotAsked.empty()) {
        isNotAsked.link(&jit);
        read.goOnBehindSite(WayBehindSite::WithoutMiss);
    }
    if (!isSampledHit.empty()) {
        isSampledHit.link(&jit);
        read.goOnBehindSite(WayBehindSite::AfterHitOfFurtherAnswer);
    }
}

static void emitReadAtSite(StubAssembler& jit, bool asksTruth, bool asksShapeFirst)
{
    const ReadAtSiteForm form = readAtSiteFormOf(jit.key());
    const GPRReg receiver = jit.operand(0);
    const GPRReg result = asksTruth ? jit.clobberRegister(signatureOf(valueIsTruthy()).operands[0].gpr()) : jit.result();
    if (asksTruth)
        jit.goOnAtEveryReturnIn(valueIsTruthy());
    const GPRReg distance = jit.immediate();
#if CPU(X86_64)
    const GPRReg place = jit.allocateScratch(StubRegister::T11);
    const GPRReg key = jit.allocateScratch(StubRegister::T13);
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
    const GPRReg difference = jit.allocateScratch(distance == gprOf(StubRegister::T15) ? StubRegister::T10 : StubRegister::T15);
    RELEASE_ASSERT(noOverlap(receiver, distance, place, key, operand, difference) && noOverlap(receiver, distance, key, laterGranules, extension));
    RELEASE_ASSERT((registersOfAnswerAtSite() == RegistersOfAnswerAtSite { receiver, place, key, operand }) && distance == gprOf(shortStubImmediateRegister));
    const GPRReg value = receiver == result ? operand : result;
    const SiteUse use = form.use;
    RELEASE_ASSERT(use == SiteUse::Read || !asksShapeFirst);
    const bool asksShape = asksShapeFirst || (use == SiteUse::ReadUnderNameOfField && !form.asksNoShapeUnderNameOfField);
    const ReadAtSiteInWork read { jit, asksTruth, form.name, use, receiver, distance, result };
    JumpList isNotAsked;
    JumpList isNotAnswered;
    const GPRReg storage = operandOfCommonForm == key ? operand : key;
    JumpList slotIsEmpty;
    JumpList slotOfAnyObjectIsEmpty;

    if (form.receiver == ReceiverAtSite::IsAnyValue)
        isNotAsked.append(jit.branchIfNotCell(receiver));
    const AnswersOfStub answers(jit, distance, place);
    auto askAnswerByShape = [&] {
        jit.load32(Address(receiver, offsetOfShapeWord), difference);
        Jump isNotInCell = answers.loadOperandUnlessKeyDiffersKeepingDifference(jit, offsetOfFirstAnswer, difference, operandOfCommonForm, keyOfAnswer);
        returnFieldUnlessSlotIsEmpty(jit, receiver, operandOfCommonForm, value, result, slotIsEmpty);
        isNotInCell.link(&jit);
    };
    if (asksShapeFirst)
        askAnswerByShape();
    Jump hasOtherKey = answers.loadBitsOfFormUnlessMostCommon(jit, offsetOfSecondAnswer, receiver, key, operand, keyOfAnswer);
    if (use == SiteUse::ReadUnderNameOfField) {
        answers.loadOperand(jit, offsetOfSecondAnswer, operandOfCommonForm);
        returnFieldUnlessSlotIsEmpty(jit, receiver, operandOfCommonForm, value, result, slotOfAnyObjectIsEmpty);
    } else {
        answers.loadSignedOperand(jit, offsetOfSecondAnswer, operandOfCommonForm);
        Jump liesInStorage = jit.branchTest64(CCallHelpers::Signed, operandOfCommonForm);
        jit.load64(BaseIndex(receiver, operandOfCommonForm, CCallHelpers::TimesEight), result);
        jit.returnToCaller();

        liesInStorage.link(&jit);
        jit.loadPtr(Address(receiver, JSObject::butterflyOffset()), storage);
        jit.load64(BaseIndex(storage, operandOfCommonForm, CCallHelpers::TimesEight), result);
        jit.returnToCaller();
    }

    hasOtherKey.link(&jit);
    Jump isNotAnsweredByStructure = jit.branch32(CCallHelpers::Above, key, TrustedImm32(SiteAnswers::formMask));
    answers.loadOperand(jit, offsetOfSecondAnswer, operand);
    jumpToHandlerOfAnswer(jit, answerHandlerTable(asksTruth, false, use), key);

    static_assert(offsetOfFirstExtensionSlot == extensionGranuleSize && extensionGranuleSize == sizeof(uint64_t) && maxExtensionGranules == 64);
    static_assert(!(WideSiteAnswers::numberOfStructuresThatLackName % 2) && WideSiteAnswers::numberOfFurtherAnswers == 2);
    JumpList findsNothing;
    std::array<Jump, WideSiteAnswers::numberOfFurtherAnswers> hasFurtherAnswer;
    auto askWideAnswers = [&] {
#if CPU(X86_64)
        Jump hasNoMoreAnswers = branchUnlessIndexIsOfWideAnswers(jit, place, InvalidGPRReg);
#else
        Jump hasNoMoreAnswers = branchUnlessAnswersAreWide(jit, distance, key, operand);
#endif
        jit.load32(Address(receiver, JSCell::structureIDOffset()), key);
#if CPU(X86_64)
        for (unsigned index = 0; index < hasFurtherAnswer.size(); ++index)
            hasFurtherAnswer[index] = jit.branch32(CCallHelpers::Equal, answers.word(OBJECT_OFFSETOF(WideSiteAnswers, structuresOfFurtherAnswers) + index * sizeof(uint32_t)), key);
#else
        jit.loadPair32(place, TrustedImm32(static_cast<int32_t>(OBJECT_OFFSETOF(WideSiteAnswers, structuresOfFurtherAnswers))), keyOfAnswer, operand);
        hasFurtherAnswer[0] = jit.branch32(CCallHelpers::Equal, key, keyOfAnswer);
        hasFurtherAnswer[1] = jit.branch32(CCallHelpers::Equal, key, operand);
#endif
        for (unsigned index = 0; index < WideSiteAnswers::numberOfStructuresThatLackName; index += 2) {
            const ptrdiff_t offset = OBJECT_OFFSETOF(WideSiteAnswers, structuresThatLackName) + index * sizeof(uint32_t);
#if CPU(X86_64)
            findsNothing.append(jit.branch32(CCallHelpers::Equal, answers.word(offset), key));
            findsNothing.append(jit.branch32(CCallHelpers::Equal, answers.word(offset + sizeof(uint32_t)), key));
#else
            jit.loadPair32(place, TrustedImm32(static_cast<int32_t>(offset)), keyOfAnswer, operand);
            findsNothing.append(jit.branch32(CCallHelpers::Equal, key, keyOfAnswer));
            findsNothing.append(jit.branch32(CCallHelpers::Equal, key, operand));
#endif
        }
        hasNoMoreAnswers.link(&jit);
    };
    isNotAnsweredByStructure.link(&jit);
    if (!asksShapeFirst) {
        askWideAnswers();
        if (asksShape)
            askAnswerByShape();
    }
    if (asksShape) {
        isNotAnswered.append(jit.branchTest32(CCallHelpers::NonZero, difference, TrustedImm32(static_cast<int32_t>(~SiteAnswers::bitsTurnedForAnswersByShape))));
        jit.load32(answers.word(offsetOfFirstAnswer + OBJECT_OFFSETOF(SiteAnswers::Answer, operand)), operand);
        readFieldThatDifferenceNames(jit, { receiver, difference, operand, key, place, extension, value, result }, { slotIsEmpty, isNotAnswered, isNotAsked }, Jump(), Jump(), Jump());
    }

    isNotAnswered.link(&jit);
    if (asksShapeFirst)
        askWideAnswers();
    read.goOnBehindSite(WayBehindSite::AfterMiss);

    JumpList hasFormAndOperand;
    for (unsigned index = hasFurtherAnswer.size(); index--;) {
        hasFurtherAnswer[index].link(&jit);
        jit.load16(answers.word(OBJECT_OFFSETOF(WideSiteAnswers, formsOfFurtherAnswersHeld) + index * sizeof(WideSiteAnswers::FormOfFurtherAnswer)), key);
        jit.load32(answers.word(OBJECT_OFFSETOF(WideSiteAnswers, operandsOfFurtherAnswers) + index * sizeof(uint32_t)), operand);
        if (index)
            hasFormAndOperand.append(jit.jump());
    }
    hasFormAndOperand.link(&jit);
#if CPU(ARM64)
    jit.and32(TrustedImm32(SiteAnswers::formMask), key, difference);
#elif defined(BUN_HARDENED)
    jit.zeroExtend8To32(key, difference);
    jit.and32(TrustedImm32(SiteAnswers::formMask), difference);
#else
    jit.zeroExtend8To32(key, difference);
#endif
    jumpToHandlerOfAnswer(jit, answerHandlerTable(asksTruth, true, use), difference);

    if (!slotOfAnyObjectIsEmpty.empty()) {
        slotOfAnyObjectIsEmpty.link(&jit);
        isNotAsked.append(branchUnlessCompiledObject(jit, receiver));
    }
    if (asksShape || use == SiteUse::ReadUnderNameOfField)
        answerWhereSlotIsEmpty(read, place, key, operand, keyOfAnswer, slotIsEmpty, findsNothing, isNotAsked);
    else {
        RELEASE_ASSERT(slotIsEmpty.empty() && slotOfAnyObjectIsEmpty.empty());
        findsNothing.link(&jit);
        read.returnUndefined();
    }

    isNotAsked.link(&jit);
    read.goOnBehindSite(WayBehindSite::WithoutMiss);
}

void generateReadAtSite(StubAssembler& jit)
{
    emitReadAtSite(jit, false, true);
}

void generateReadTruthAtSite(StubAssembler& jit)
{
    emitReadAtSite(jit, true, true);
}

StubSignature signatureOfReadTruthAtSiteAskingStructureFirst(StubKey key)
{
    return signatureOfReadTruthAtSite(key);
}

void generateReadTruthAtSiteAskingStructureFirst(StubAssembler& jit)
{
    emitReadAtSite(jit, true, false);
}

StubSignature signatureOfReadAtSiteAskingStructureFirst(StubKey key)
{
    return signatureOfReadAtSite(key);
}

void generateReadAtSiteAskingStructureFirst(StubAssembler& jit)
{
    emitReadAtSite(jit, false, false);
}

StubSignature signatureOfReadLengthAtSite(StubKey key)
{
    return signatureOfReadAtSite(key);
}

void generateReadLengthAtSite(StubAssembler& jit)
{
    static_assert(ArrayWithUndecided + 2 == ArrayWithInt32 && ArrayWithUndecided + 4 == ArrayWithDouble && ArrayWithUndecided + 6 == ArrayWithContiguous);
    const GPRReg receiver = jit.operand(0);
    const GPRReg scratch = jit.allocateScratch(StubRegister::T13);
    RELEASE_ASSERT(noOverlap(receiver, scratch, jit.immediate(), jit.result()));
    auto answer = [&](Address length) {
        jit.load32(length, jit.result());
        jit.or64(numberTag, jit.result());
        jit.returnToCaller();
    };
    auto goOn = [&](ReceiverAtSite receiverIs) {
        jit.jumpToStub(jit.key().form() ? readAtSiteAskingStructureFirst(receiverIs) : readAtSite(receiverIs));
    };

    Jump isNotCell = jit.branchIfNotCell(receiver);
    Jump isNoContiguousList = jit.branch8(CCallHelpers::NotEqual, Address(receiver, JSCell::indexingTypeAndMiscOffset()), TrustedImm32(ArrayWithContiguous));
    Label hasPlainStorage = jit.label();
    jit.loadPtr(Address(receiver, JSObject::butterflyOffset()), scratch);
    answer(Address(scratch, Butterfly::offsetOfPublicLength()));

    isNoContiguousList.link(&jit);
    Jump isNoText = jit.branch8(CCallHelpers::NotEqual, Address(receiver, JSCell::typeInfoTypeOffset()), TrustedImm32(StringType));
    jit.loadPtr(Address(receiver, JSString::offsetOfValue()), scratch);
    Jump isRope = jit.branchIfRopeStringImpl(scratch);
    answer(Address(scratch, StringImpl::lengthMemoryOffset()));

    isRope.link(&jit);
    answer(Address(receiver, JSRopeString::offsetOfLength()));

    isNoText.link(&jit);
    jit.load8(Address(receiver, JSCell::indexingTypeAndMiscOffset()), scratch);
    jit.sub32(TrustedImm32(ArrayWithUndecided), scratch);
    jit.branchTest32(CCallHelpers::Zero, scratch, TrustedImm32(0xff & ~(6 | CopyOnWrite | MayHaveIndexedAccessors))).linkTo(hasPlainStorage, &jit);
    goOn(ReceiverAtSite::IsCell);

    isNotCell.link(&jit);
    goOn(ReceiverAtSite::IsAnyValue);
}

StubSignature signatureOfReadAtSiteUnlessAsMade(StubKey key)
{
    return signatureOfReadAtSite(key);
}

void generateReadAtSiteUnlessAsMade(StubAssembler& jit)
{
    const ReadAtSiteForm form = readAtSiteFormOf(jit.key());
    StubKey reads = readAtSite(form.receiver, form.name);
    if (form.receiver != ReceiverAtSite::IsCell) {
        jit.jumpToStub(reads);
        return;
    }
    Jump isNotAsMade = branchIfNotPristine(jit, jit.operand(0));
    jit.move(TrustedImm64(JSValue::encode(JSValue())), jit.result());
    jit.returnToCaller();
    jit.linkToStub(isNotAsMade, reads);
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
    checkStubOperands(getByIdOfCell(), { receiver });
    checkStubOperands(getByIdOfFieldLeftOut(), { receiver });
    if (form.way == WayBehindSite::AfterHitOfFurtherAnswer) {
        jit.move(TrustedImm32(0), name);
        fillAnswersAtSite(jit, receiver, name, distance, form.use, visitorHitFurtherAnswer);
    } else if (form.way != WayBehindSite::WithoutMiss)
        countMissAtSite(jit, receiver, distance, name, scratch, form.use, stubOfSiteFollowsFurtherAnswers | stubJumpsToArmOfAnswer);
    const GPRReg nameOfSite = distance == gprOf(slotNameRegister) ? distance : jit.allocateScratch(slotNameRegister);
    loadNameOfSiteOrMark(jit, distance, nameOfSite, scratch);
    Jump standsBehind;
    if (form.name == NameAtSite::IsAnyName)
        standsBehind = jit.branch32(CCallHelpers::Above, nameOfSite, TrustedImm32(RowDisplacementTable::maxColumn));
    const StubKey reads = form.way == WayBehindSite::OfFieldLeftOut ? getByIdOfFieldLeftOut() : form.way == WayBehindSite::AfterMiss || form.way == WayBehindSite::AfterHitOfFurtherAnswer ? getByIdOfCell() : getById();
    jit.jumpToStub(reads.withCallerFrame(jit.key().callerFrame()));
    if (form.name == NameAtSite::HasColumn)
        return;
    standsBehind.link(&jit);
    loadNameBehindMarkOfSite(jit, distance, nameOfSite, scratch);
    jit.jumpToStub(getByIdHashed().withCallerFrame(jit.key().callerFrame()));
}

StubSignature signatureOfCallMethodAtSite(StubKey key)
{
    return signatureOf(callMethodById(key.count()));
}

StubSignature signatureOfCallMethodAtSiteLending(StubKey key) { return signatureOfCallMethodAtSite(key); }

static void askAgainWhatSiteAnswers(StubAssembler& jit, GPRReg receiver, GPRReg answer, GPRReg distance, KindOfAnswer kind, StubThatAnswers stub)
{
    if (!jit.options().checksWalk)
        return;
    jit.preservingCallerSavedRegisters(false, [&](unsigned) {
        const GPRReg A0 = jit.savedScratch(StubRegister::A0);
        const GPRReg A1 = jit.savedScratch(StubRegister::A1);
        const GPRReg A2 = jit.savedScratch(StubRegister::A2);
        const GPRReg A3 = jit.savedScratch(StubRegister::A3);
        const GPRReg A4 = jit.savedScratch(StubRegister::A4);
        jit.subPtr(TrustedImm32(4 * sizeof(CPURegister)), sp);
        jit.storePair64(receiver, answer, sp, TrustedImm32(0));
        jit.storePtr(distance, Address(sp, 2 * sizeof(CPURegister)));
        jit.loadPair64(sp, TrustedImm32(0), A1, A2);
        jit.loadPtr(Address(sp, 2 * sizeof(CPURegister)), A3);
        jit.addPtr(TrustedImm32(4 * sizeof(CPURegister)), sp);
        loadNameOfSite(jit, A3, A3, A4);
        jit.move(instanceGPR, A0);
        jit.move(TrustedImm32(static_cast<int32_t>(kindAndStubOfAnswer(kind, stub))), A4);
        jit.callOperation(Entry::operationAOTStopAtStaleAnswer, jit.savedScratch(StubRegister::Operation));
    });
}

StubSignature signatureOfReadAtSiteRunningNoCode(StubKey key) { return signatureOfReadAtSite(key); }

void generateReadAtSiteRunningNoCode(StubAssembler& jit)
{
    constexpr SiteUse use = SiteUse::Read;
    using Form = SiteAnswers::Form;
    const ReadAtSiteRunningNoCodeForm form = readAtSiteRunningNoCodeFormOf(jit.key());
    const GPRReg receiver = jit.operand(0);
    const GPRReg result = jit.result();
    const GPRReg distance = jit.immediate();
#if CPU(X86_64)
    const GPRReg place = jit.allocateScratch(StubRegister::T11);
    const GPRReg key = jit.allocateScratch(StubRegister::T13);
    std::optional<OwnedScratchRegister> ownedScratch;
    if (receiver == gprOf(StubRegister::T14))
        ownedScratch.emplace(jit);
    const GPRReg operand = ownedScratch ? ownedScratch->gpr() : jit.allocateScratch(StubRegister::T14);
    const GPRReg keyOfAnswer = InvalidGPRReg;
    const GPRReg operandOfCommonForm = key;
    const GPRReg extension = place;
    const GPRReg scratchOfMiss = place;
    const GPRReg nameOfMiss = key;
    const GPRReg cellOfFront = place;
#else
    const GPRReg key = jit.allocateScratch(StubRegister::T11);
    const GPRReg operand = jit.allocateScratch(StubRegister::T13);
    const GPRReg keyOfAnswer = jit.allocateScratch(StubRegister::T12);
    const GPRReg place = jit.allocateScratch(StubRegister::T14);
    const GPRReg operandOfCommonForm = operand;
    const GPRReg extension = keyOfAnswer;
    const GPRReg scratchOfMiss = key;
    const GPRReg nameOfMiss = operand;
    const GPRReg cellOfFront = keyOfAnswer;
#endif
    const GPRReg difference = jit.allocateScratch(distance == gprOf(StubRegister::T15) ? StubRegister::T10 : StubRegister::T15);
    const GPRReg storage = operandOfCommonForm == key ? operand : key;
    RELEASE_ASSERT(noOverlap(receiver, result, distance, place, key, operand, difference) && noOverlap(receiver, distance, key, operand, extension) && result == gprOf(firstStubOperandRegister));
    RELEASE_ASSERT(noOverlap(receiver, result, distance, key, operand, difference, cellOfFront));
    JumpList isNoHit;
    JumpList isMiss;
    JumpList isSampledHit;
    auto askFrontOfTables = [&] {
        loadNameOfSiteOrMark(jit, distance, key, operand);
        Jump hasNoColumn = jit.branch32(CCallHelpers::Above, key, TrustedImm32(RowDisplacementTable::maxColumn));
        jit.load32(Address(receiver, JSCell::structureIDOffset()), difference);
        loadFrontCell(jit, SlotAccess::Read, { difference, key, operand, cellOfFront, result });
        Jump isNotOwnInlineValue = branchUnlessOwnInlineValue(jit, key, cellOfFront, result);
        loadOwnInlineValue(jit, receiver, cellOfFront, result);
        jit.returnToCaller();

        isNotOwnInlineValue.link(&jit);
        Jump hasOtherName = branchIfCellNameDiffers(jit, key, cellOfFront, result);
        extractBits(jit, cellOfFront, 8 * PackedSlot::offsetOfBiasedKindAndFlags(), 8, result);
        static_assert(((static_cast<uint8_t>(Slot::Kind::Value) | PackedSlot::isOwn | PackedSlot::isOutOfLine) ^ PackedSlot::ownInlineValueBits) == PackedSlot::isOutOfLine);
        Jump isNoOwnValueInStorage = jit.branch32(CCallHelpers::NotEqual, result, TrustedImm32(PackedSlot::isOutOfLine));
        jit.signExtend32ToPtr(cellOfFront, cellOfFront);
        jit.loadPtr(Address(receiver, JSObject::butterflyOffset()), result);
        jit.load64(BaseIndex(result, cellOfFront, CCallHelpers::TimesEight), result);
        jit.returnToCaller();

        hasNoColumn.link(&jit);
        hasOtherName.link(&jit);
        isNoOwnValueInStorage.link(&jit);
    };
    auto branchIfOfForm = [&](Form asked) {
        return jit.branch32(CCallHelpers::Equal, key, TrustedImm32(static_cast<int32_t>(SiteAnswers::bitsOf(use, asked))));
    };

    if (form.receiver == ReceiverAtSite::IsAnyValue)
        isNoHit.append(jit.branchIfNotCell(receiver));
    Label asksSite = jit.label();
    const AnswersOfStub answers(jit, distance, place);
    auto askAnswerByShape = [&] {
        jit.load32(Address(receiver, offsetOfShapeWord), difference);
        Jump isNotInCell = answers.loadOperandUnlessKeyDiffersKeepingDifference(jit, offsetOfFirstAnswer, difference, operandOfCommonForm, keyOfAnswer);
        returnFieldUnlessSlotIsEmpty(jit, receiver, operandOfCommonForm, result, result, isNoHit);
        isNotInCell.link(&jit);
    };
    if (form.asksShapeFirst)
        askAnswerByShape();
    Jump hasOtherKey = answers.loadBitsOfFormUnlessMostCommon(jit, offsetOfSecondAnswer, receiver, key, operand, keyOfAnswer);
    answers.loadSignedOperand(jit, offsetOfSecondAnswer, operandOfCommonForm);
    Jump liesInStorage = jit.branchTest64(CCallHelpers::Signed, operandOfCommonForm);
    jit.load64(BaseIndex(receiver, operandOfCommonForm, CCallHelpers::TimesEight), result);
    jit.returnToCaller();

    liesInStorage.link(&jit);
    jit.loadPtr(Address(receiver, JSObject::butterflyOffset()), storage);
    jit.load64(BaseIndex(storage, operandOfCommonForm, CCallHelpers::TimesEight), result);
    jit.returnToCaller();

    hasOtherKey.link(&jit);
    Jump isNotAnsweredByStructure = jit.branch32(CCallHelpers::Above, key, TrustedImm32(SiteAnswers::formMask));
    answers.loadOperand(jit, offsetOfSecondAnswer, operand);
    static_assert(SiteAnswers::bitsOf(use, Form::ValueInStorageOfHolder) == 1);
    JumpsToTwoForms aboveInStorageOrInCell = branchIfEitherOfTwoForms(jit, key, use, Form::ValueInStorageOfHolder, Form::ValueInWordOfCage);
    Jump isOwnField = branchIfOfForm(Form::OwnFieldInCell);
    Jump isOwnValueInCell = branchIfOfForm(Form::OwnValueInCell);
    Jump isOwnNarrowReference = branchIfOfForm(Form::OwnNarrowReferenceInCell);
    Jump isOwnInt32 = branchIfOfForm(Form::OwnInt32InCell);
    Jump isExtraValueInStorageOfNarrowRecord = branchIfOfForm(Form::ExtraValueInStorage);
    Jump isExtraValueInCellOfNarrowRecord = branchIfOfForm(Form::ExtraValueInCell);
    Jump isOwnFieldInExtension = branchIfOfForm(Form::OwnFieldInExtension);
    isNoHit.append(jit.jump());

    aboveInStorageOrInCell.toLower.link(&jit);
    jit.loadPtr(BaseIndex(cellCageBaseGPR, operand, CCallHelpers::TimesEight, JSObject::butterflyOffset()), operand);
    jit.load32SignedExtendTo64(answers.word(offsetOfFirstAnswer + OBJECT_OFFSETOF(SiteAnswers::Answer, operand)), key);
    jit.load64(BaseIndex(operand, key, CCallHelpers::TimesEight), result);
    jit.returnToCaller();

    aboveInStorageOrInCell.toUpper.link(&jit);
    Label readsWordOfCage = jit.label();
    jit.load64(BaseIndex(cellCageBaseGPR, operand, CCallHelpers::TimesEight), result);
    jit.returnToCaller();

    isOwnField.link(&jit);
    Label readsOwnField = jit.label();
    returnFieldUnlessSlotIsEmpty(jit, receiver, operand, result, result, isNoHit);

    isOwnValueInCell.link(&jit);
    Label readsOwnValueInCell = jit.label();
    jit.load64(BaseIndex(receiver, operand, CCallHelpers::TimesOne), result);
    jit.returnToCaller();

    isNotAnsweredByStructure.link(&jit);
    if (!form.asksShapeFirst)
        askAnswerByShape();
    isMiss.append(jit.branchTest32(CCallHelpers::NonZero, difference, TrustedImm32(static_cast<int32_t>(~SiteAnswers::bitsTurnedForAnswersByShape))));
    jit.load32(answers.word(offsetOfFirstAnswer + OBJECT_OFFSETOF(SiteAnswers::Answer, operand)), operand);
    readFieldThatDifferenceNames(jit, { receiver, difference, operand, key, place, extension, result, result }, { isNoHit, isMiss, isNoHit }, isOwnNarrowReference, isOwnInt32, isOwnFieldInExtension);

    isMiss.link(&jit);
    static_assert(WideSiteAnswers::numberOfFurtherAnswers == 2 && !(WideSiteAnswers::numberOfStructuresThatLackName % 2));
#if CPU(X86_64)
    Jump hasNoMoreAnswers = branchUnlessIndexIsOfWideAnswers(jit, place, InvalidGPRReg);
#else
    Jump hasNoMoreAnswers = branchUnlessAnswersAreWide(jit, distance, key, operand);
#endif
    jit.load32(Address(receiver, JSCell::structureIDOffset()), key);
    std::array<Jump, WideSiteAnswers::numberOfFurtherAnswers> hasFurtherAnswer;
    JumpList lacksName;
#if CPU(X86_64)
    for (unsigned index = 0; index < hasFurtherAnswer.size(); ++index)
        hasFurtherAnswer[index] = jit.branch32(CCallHelpers::Equal, answers.word(OBJECT_OFFSETOF(WideSiteAnswers, structuresOfFurtherAnswers) + index * sizeof(uint32_t)), key);
    for (unsigned index = 0; index < WideSiteAnswers::numberOfStructuresThatLackName; ++index)
        lacksName.append(jit.branch32(CCallHelpers::Equal, answers.word(OBJECT_OFFSETOF(WideSiteAnswers, structuresThatLackName) + index * sizeof(uint32_t)), key));
#else
    jit.loadPair32(place, TrustedImm32(static_cast<int32_t>(OBJECT_OFFSETOF(WideSiteAnswers, structuresOfFurtherAnswers))), keyOfAnswer, operand);
    hasFurtherAnswer[0] = jit.branch32(CCallHelpers::Equal, key, keyOfAnswer);
    hasFurtherAnswer[1] = jit.branch32(CCallHelpers::Equal, key, operand);
    for (unsigned index = 0; index < WideSiteAnswers::numberOfStructuresThatLackName; index += 2) {
        jit.loadPair32(place, TrustedImm32(static_cast<int32_t>(OBJECT_OFFSETOF(WideSiteAnswers, structuresThatLackName) + index * sizeof(uint32_t))), keyOfAnswer, operand);
        lacksName.append(jit.branch32(CCallHelpers::Equal, key, keyOfAnswer));
        lacksName.append(jit.branch32(CCallHelpers::Equal, key, operand));
    }
#endif
    Jump isMissOfWideRecord = jit.jump();

    lacksName.link(&jit);
    jit.move(TrustedImm64(JSValue::ValueUndefined), result);
    jit.returnToCaller();

    JumpList hasFormAndOperand;
    for (unsigned index = hasFurtherAnswer.size(); index--;) {
        hasFurtherAnswer[index].link(&jit);
        jit.load16(answers.word(OBJECT_OFFSETOF(WideSiteAnswers, formsOfFurtherAnswersHeld) + index * sizeof(WideSiteAnswers::FormOfFurtherAnswer)), key);
        jit.load32(answers.word(OBJECT_OFFSETOF(WideSiteAnswers, operandsOfFurtherAnswers) + index * sizeof(uint32_t)), operand);
        if (index)
            hasFormAndOperand.append(jit.jump());
    }
    hasFormAndOperand.link(&jit);
    static_assert(WideSiteAnswers::formsOfFurtherAnswers == std::array { Form::ExtraValueInCell, Form::OwnFieldInCell, Form::OwnValueInCell, Form::ExtraValueInStorage, Form::ValueInWordOfCage });
    branchIfOfForm(Form::OwnFieldInCell).linkTo(readsOwnField, &jit);
    branchIfOfForm(Form::OwnValueInCell).linkTo(readsOwnValueInCell, &jit);
    branchIfOfForm(Form::ValueInWordOfCage).linkTo(readsWordOfCage, &jit);
    Jump isExtraValueInStorage = branchIfOfForm(Form::ExtraValueInStorage);
    Jump isExtraValueInCell = branchIfOfForm(Form::ExtraValueInCell);
    {
        Jump hasNoIndex = jit.branch32(CCallHelpers::BelowOrEqual, key, TrustedImm32(std::numeric_limits<uint8_t>::max()));
        jit.move(key, difference);
        jit.lshift64(TrustedImm32(48), difference);
        jit.rshift64(TrustedImm32(56), difference);
        jit.zeroExtend8To32(key, key);
        isNoHit.append(jit.branch32(CCallHelpers::NotEqual, key, TrustedImm32(static_cast<int32_t>(SiteAnswers::bitsOf(use, Form::ValueInStorageOfHolder)))));
        isSampledHit.append(countHitOfFurtherAnswer(jit, key));
        jit.loadPtr(BaseIndex(cellCageBaseGPR, operand, CCallHelpers::TimesEight, JSObject::butterflyOffset()), operand);
        jit.load64(BaseIndex(operand, difference, CCallHelpers::TimesEight), result);
        jit.returnToCaller();
        hasNoIndex.link(&jit);
    }
    isNoHit.append(jit.jump());
    isExtraValueInCell.link(&jit);
    isExtraValueInCellOfNarrowRecord.link(&jit);
    auto loadExtraProperties = [&] {
        jit.zeroExtend8To32(operand, key);
        jit.load32(BaseIndex(receiver, key, CCallHelpers::TimesFour), key);
        isNoHit.append(jit.branchTest32(CCallHelpers::Zero, key));
        jit.rshift32(TrustedImm32(SiteAnswers::indexShiftInOperandOfExtraValue), operand);
    };
    loadExtraProperties();
    jit.add32(operand, key);
    jit.load64(BaseIndex(cellCageBaseGPR, key, CCallHelpers::TimesEight), result);
    jit.returnToCaller();

    isExtraValueInStorage.link(&jit);
    isExtraValueInStorageOfNarrowRecord.link(&jit);
    loadExtraProperties();
    jit.loadPtr(BaseIndex(cellCageBaseGPR, key, CCallHelpers::TimesEight, JSObject::butterflyOffset()), key);
    jit.signExtend32ToPtr(operand, operand);
    jit.load64(BaseIndex(key, operand, CCallHelpers::TimesEight), result);
    jit.returnToCaller();

    if (!isSampledHit.empty()) {
        isSampledHit.link(&jit);
        jit.move(receiver, result);
        jit.move(TrustedImm32(0), nameOfMiss);
        fillAnswersAtSite(jit, result, nameOfMiss, distance, use, visitorHitFurtherAnswer);
        jit.jump().linkTo(asksSite, &jit);
    }

    hasNoMoreAnswers.link(&jit);
    isMissOfWideRecord.link(&jit);
    jit.move(receiver, result);
    isNoHit.append(jit.branchIfNotObject(result));
    jit.load16(BaseIndex(instanceGPR, distance, CCallHelpers::TimesOne), scratchOfMiss);
    Jump hasAnswers = jit.branchTest32(CCallHelpers::NonZero, scratchOfMiss);
    loadNameOfSite(jit, distance, nameOfMiss, scratchOfMiss);
    fillAnswersAtSite(jit, result, nameOfMiss, distance, use, tablesAreFiledForSiteWithoutAnswers);
    jit.load16(BaseIndex(instanceGPR, distance, CCallHelpers::TimesOne), scratchOfMiss);
    jit.branchTest32(CCallHelpers::NonZero, scratchOfMiss).linkTo(asksSite, &jit);

    hasAnswers.link(&jit);
    Jump hasAnswersOfItsOwn = jit.branch32(CCallHelpers::NotEqual, scratchOfMiss, TrustedImm32(indexOfSiteAnswersThatHoldNothing));
    countMissAtSite(jit, result, distance, key, operand, use, stubOfSiteFollowsFurtherAnswers);
    askFrontOfTables();
    isNoHit.append(jit.jump());

    hasAnswersOfItsOwn.link(&jit);
    askFrontOfTables();
    jit.move(receiver, result);
    countMissAtSite(jit, result, distance, key, operand, use, stubOfSiteFollowsFurtherAnswers);
    isNoHit.link(&jit);
    jit.move(TrustedImm32(0), result);
    jit.returnToCaller();
}

StubSignature signatureOfFindsNameAtSite(StubKey key) { return signatureOfReadAtSite(key); }

void generateFindsNameAtSite(StubAssembler& jit)
{
    constexpr SiteUse use = SiteUse::Read;
    using Form = SiteAnswers::Form;
    const FindsNameAtSiteForm form = findsNameAtSiteFormOf(jit.key());
    const bool asksForOwn = form.question == PresenceQuestion::HasOwn;
    const GPRReg receiver = jit.operand(0);
    const GPRReg result = jit.result();
    const GPRReg distance = jit.immediate();
#if CPU(X86_64)
    const GPRReg place = jit.allocateScratch(StubRegister::T11);
    const GPRReg key = jit.allocateScratch(StubRegister::T13);
    std::optional<OwnedScratchRegister> ownedScratch;
    if (receiver == gprOf(StubRegister::T14))
        ownedScratch.emplace(jit);
    const GPRReg operand = ownedScratch ? ownedScratch->gpr() : jit.allocateScratch(StubRegister::T14);
    const GPRReg keyOfAnswer = InvalidGPRReg;
    const GPRReg operandOfCommonForm = key;
    const GPRReg scratchOfMiss = place;
    const GPRReg nameOfMiss = key;
#else
    const GPRReg key = jit.allocateScratch(StubRegister::T11);
    const GPRReg operand = jit.allocateScratch(StubRegister::T13);
    const GPRReg keyOfAnswer = jit.allocateScratch(StubRegister::T12);
    const GPRReg place = jit.allocateScratch(StubRegister::T14);
    const GPRReg operandOfCommonForm = operand;
    const GPRReg scratchOfMiss = key;
    const GPRReg nameOfMiss = operand;
#endif
    const GPRReg difference = jit.allocateScratch(distance == gprOf(StubRegister::T15) ? StubRegister::T10 : StubRegister::T15);
    const GPRReg nameOfGeneralStub = distance == gprOf(slotNameRegister) || difference == gprOf(slotNameRegister) ? gprOf(slotNameRegister) : jit.allocateScratch(slotNameRegister);
    RELEASE_ASSERT(noOverlap(receiver, result, distance, place, key, operand, difference) && result == gprOf(firstStubOperandRegister));
    checkStubOperands(inById(form.question), { result });
    auto answer = [&](bool isThere) {
        jit.move(TrustedImm32(isThere), result);
        jit.returnToCaller();
    };
    auto askGeneralStub = [&] {
        loadNameOfSite(jit, distance, nameOfGeneralStub, scratchOfMiss);
        jit.jumpToStub(inById(form.question));
    };
    auto branchIfOfForm = [&](Form asked) {
        return jit.branch32(CCallHelpers::Equal, key, TrustedImm32(static_cast<int32_t>(SiteAnswers::bitsOf(use, asked))));
    };
    auto branchIfSlotIsEmpty = [&](GPRReg offset) {
        const BaseIndex slot(receiver, offset, CCallHelpers::TimesOne);
#if CPU(X86_64)
        return jit.branchTest64(CCallHelpers::Zero, slot);
#else
        jit.load64(slot, result);
        return jit.branchTest64(CCallHelpers::Zero, result);
#endif
    };
    JumpList isNotAsked;
    JumpList isThere;
    JumpList isNotThere;
    JumpList& isAbove = asksForOwn ? isNotThere : isThere;

    isNotAsked.append(jit.branchIfNotCell(receiver));
    const AnswersOfStub answers(jit, distance, place);
    auto askAnswerByShape = [&] {
        jit.load32(Address(receiver, offsetOfShapeWord), difference);
        Jump isNotInCell = answers.loadOperandUnlessKeyDiffersKeepingDifference(jit, offsetOfFirstAnswer, difference, operandOfCommonForm, keyOfAnswer);
        isNotAsked.append(branchIfSlotIsEmpty(operandOfCommonForm));
        answer(true);
        isNotInCell.link(&jit);
    };
    if (form.asksShapeFirst)
        askAnswerByShape();
    Jump hasOtherKey = answers.loadBitsOfFormUnlessMostCommon(jit, offsetOfSecondAnswer, receiver, key, operand, keyOfAnswer);
    answer(true);

    hasOtherKey.link(&jit);
    Jump isNotAnsweredByStructure = jit.branch32(CCallHelpers::Above, key, TrustedImm32(SiteAnswers::formMask));
    answers.loadOperand(jit, offsetOfSecondAnswer, operand);
    Label asksFormAndOperand = jit.label();
    JumpsToTwoForms aboveInStorageOrInCell = branchIfEitherOfTwoForms(jit, key, use, Form::ValueInStorageOfHolder, Form::ValueInWordOfCage);
    isAbove.append(aboveInStorageOrInCell.toLower);
    JumpsToTwoForms ownFieldOrExtraValueInCell = branchIfEitherOfTwoForms(jit, key, use, Form::OwnFieldInCell, Form::ExtraValueInCell);
    isNotAsked.append(ownFieldOrExtraValueInCell.toUpper);
    for (Form own : { Form::OwnValueInCell, Form::OwnGetterInStorage })
        isThere.append(branchIfOfForm(own));
    Jump isNoGetterInCell = jit.branch32(CCallHelpers::NotEqual, key, TrustedImm32(static_cast<int32_t>(SiteAnswers::bitsOf(use, Form::OwnGetterInCell))));
    isNotAsked.append(branchIfSlotIsEmpty(operand));
    isThere.append(jit.jump());
    isNoGetterInCell.link(&jit);
    for (Form above : { Form::GetterInStorageOfHolder, Form::GetterInWordOfCage })
        isAbove.append(branchIfOfForm(above));
    isNotAsked.append(jit.jump());

    aboveInStorageOrInCell.toUpper.link(&jit);
    if (!asksForOwn)
        isThere.append(jit.branch32(CCallHelpers::NotEqual, operand, Address(instanceGPR, AOT_INSTANCE_OFFSET(propertyCaches.wordOfCageThatHoldsUndefined))));
    isNotThere.link(&jit);
    Label answersNo = jit.label();
    answer(false);

    ownFieldOrExtraValueInCell.toLower.link(&jit);
    isNotAsked.append(branchIfSlotIsEmpty(operand));
    isThere.link(&jit);
    answer(true);

    isNotAnsweredByStructure.link(&jit);
    if (!form.asksShapeFirst)
        askAnswerByShape();
    isNotAsked.append(jit.branchTest32(CCallHelpers::Zero, difference, TrustedImm32(static_cast<int32_t>(~SiteAnswers::bitsTurnedForAnswersByShape))));
#if CPU(X86_64)
    Jump hasNoMoreAnswers = branchUnlessIndexIsOfWideAnswers(jit, place, InvalidGPRReg);
#else
    Jump hasNoMoreAnswers = branchUnlessAnswersAreWide(jit, distance, key, operand);
#endif
    jit.load32(Address(receiver, JSCell::structureIDOffset()), key);
    std::array<Jump, WideSiteAnswers::numberOfFurtherAnswers> hasFurtherAnswer;
    for (unsigned index = 0; index < hasFurtherAnswer.size(); ++index)
        hasFurtherAnswer[index] = jit.branch32(CCallHelpers::Equal, answers.word(OBJECT_OFFSETOF(WideSiteAnswers, structuresOfFurtherAnswers) + index * sizeof(uint32_t)), key);
    for (unsigned index = 0; index < WideSiteAnswers::numberOfStructuresThatLackName; ++index)
        jit.branch32(CCallHelpers::Equal, answers.word(OBJECT_OFFSETOF(WideSiteAnswers, structuresThatLackName) + index * sizeof(uint32_t)), key).linkTo(answersNo, &jit);
    hasNoMoreAnswers.link(&jit);
    jit.move(receiver, result);
    Jump isNoObject = jit.branchIfNotObject(result);
    countMissAtSite(jit, result, distance, nameOfMiss, scratchOfMiss, use, stubAsksForPresence);
    isNoObject.link(&jit);
    askGeneralStub();

    for (unsigned index = 0; index < hasFurtherAnswer.size(); ++index) {
        hasFurtherAnswer[index].link(&jit);
        static_assert(!OBJECT_OFFSETOF(WideSiteAnswers::FormOfFurtherAnswer, bits) && sizeof(WideSiteAnswers::FormOfFurtherAnswer::bits) == 1);
        jit.load8(answers.word(OBJECT_OFFSETOF(WideSiteAnswers, formsOfFurtherAnswersHeld) + index * sizeof(WideSiteAnswers::FormOfFurtherAnswer)), key);
        jit.load32(answers.word(OBJECT_OFFSETOF(WideSiteAnswers, operandsOfFurtherAnswers) + index * sizeof(uint32_t)), operand);
        jit.jump().linkTo(asksFormAndOperand, &jit);
    }

    isNotAsked.link(&jit);
    jit.move(receiver, result);
    askGeneralStub();
}

StubSignature signatureOfFindsNoExtraPropertyAtSite(StubKey)
{
    StubSignature signature = signatureWithFlagsResult({ gprOf(firstStubOperandRegister) }, CCallHelpers::Zero);
    signature.immediate = gprOf(stubImmediateRegister);
    return signature;
}

void generateFindsNoExtraPropertyAtSite(StubAssembler& jit)
{
    constexpr SiteUse use = SiteUse::AbsenceAmongExtraProperties;
    static_assert(!SiteAnswers::bitsOf(use, SiteAnswers::Form::Nothing));
    const GPRReg object = jit.operand(0);
    const GPRReg distance = jit.immediate();
    const GPRReg place = jit.allocateScratch(StubRegister::T11);
    const GPRReg structureID = jit.allocateScratch(StubRegister::T13);
    RELEASE_ASSERT(noOverlap(object, distance, place, structureID));
    const AnswersOfStub answers(jit, distance, place);
    jit.load32(Address(object, JSCell::structureIDOffset()), structureID);
    JumpList isRemembered;
    isRemembered.append(jit.branch32(CCallHelpers::Equal, answers.word(offsetOfFirstAnswer), structureID));
    isRemembered.append(jit.branch32(CCallHelpers::Equal, answers.word(offsetOfSecondAnswer), structureID));
#if CPU(X86_64)
    Jump hasNoMoreAnswers = branchUnlessIndexIsOfWideAnswers(jit, place, InvalidGPRReg);
#else
    Jump hasNoMoreAnswers = branchUnlessAnswersAreWide(jit, distance, jit.allocateScratch(StubRegister::T12), jit.allocateScratch(StubRegister::T14));
#endif
    for (unsigned index = 0; index < WideSiteAnswers::numberOfStructuresThatLackName; ++index)
        isRemembered.append(jit.branch32(CCallHelpers::Equal, answers.word(OBJECT_OFFSETOF(WideSiteAnswers, structuresThatLackName) + index * sizeof(uint32_t)), structureID));
    hasNoMoreAnswers.link(&jit);
    countMissAtSite(jit, object, distance, structureID, place, use);
    jit.returnFlagsConstant(false);

    isRemembered.link(&jit);
    askAgainWhatSiteAnswers(jit, object, object, distance, KindOfAnswer::AbsentFromObjectItself, StubThatAnswers::FindsNoExtraPropertyAtSite);
    jit.returnFlagsConstant(true);
}

static void emitCallMethodAtSite(StubAssembler& jit, unsigned lentArguments, NameAtSite name)
{
    static_assert(descriptorOf(StubFamily::CallMethodAtSiteLending).numberOfForms == 2 * numberOfSetsOfLentArguments && numberOfSetsLentAtSite == numberOfSetsOfLentArguments);
    constexpr SiteUse use = SiteUse::Call;
    using Form = SiteAnswers::Form;
    const unsigned count = jit.key().count();
    const StubKey callsValue = callValueLending(count, lentArguments);
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
    JumpList mayBeGetter;
    auto callAnswer = [&] {
        askAgainWhatSiteAnswers(jit, thisGPR, calleeGPR, distance, KindOfAnswer::Value, StubThatAnswers::CallMethodAtSite);
        jit.jumpToStub(callsValue);
    };
    auto loadExtraProperties = [&](JumpList& hasNone) {
        jit.zeroExtend8To32(operand, calleeGPR);
        jit.load32(BaseIndex(thisGPR, calleeGPR, CCallHelpers::TimesFour), calleeGPR);
        hasNone.append(jit.branchTest32(CCallHelpers::Zero, calleeGPR));
        jit.rshift32(TrustedImm32(SiteAnswers::indexShiftInOperandOfExtraValue), operand);
    };

    isNotAsked.append(jit.branchIfNotCell(thisGPR));
    const AnswersOfStub answers(jit, distance, place);
    Jump hasOtherKey = answers.loadBitsOfFormUnlessMostCommon(jit, offsetOfFirstAnswer, thisGPR, calleeGPR, operand, keyOfAnswer);
    answers.loadOperand(jit, offsetOfFirstAnswer, operandOfCommonForm);
    jit.loadPtr(BaseIndex(cellCageBaseGPR, operandOfCommonForm, CCallHelpers::TimesEight, JSObject::butterflyOffset()), operandOfCommonForm);
    jit.load32SignedExtendTo64(answers.word(offsetOfSecondAnswer + OBJECT_OFFSETOF(SiteAnswers::Answer, operand)), index);
    jit.load64(BaseIndex(operandOfCommonForm, index, CCallHelpers::TimesEight), calleeGPR);
    callAnswer();

    auto callValueOfOtherForm = [&](ptrdiff_t offsetOfAnswer) {
        JumpList differs;
        differs.append(jit.branch32(CCallHelpers::Above, calleeGPR, TrustedImm32(SiteAnswers::formMask)));
        answers.loadOperand(jit, offsetOfAnswer, operand);
        Jump isNotInCell = branchUnlessForm(jit, use, calleeGPR, Form::OwnFieldInCell);
        jit.load64(BaseIndex(thisGPR, operand, CCallHelpers::TimesOne), calleeGPR);
        isNotAsked.append(jit.branchTest64(CCallHelpers::Zero, calleeGPR));
        callAnswer();

        isNotInCell.link(&jit);
        Jump isNotAbove = branchUnlessForm(jit, use, calleeGPR, Form::ValueInWordOfCage);
        jit.load64(BaseIndex(cellCageBaseGPR, operand, CCallHelpers::TimesEight), calleeGPR);
        callAnswer();

        isNotAbove.link(&jit);
        Jump isNotInStorage = branchUnlessForm(jit, use, calleeGPR, Form::OwnValueInStorage);
        jit.loadPtr(Address(thisGPR, JSObject::butterflyOffset()), calleeGPR);
        jit.signExtend32ToPtr(operand, operand);
        jit.load64(BaseIndex(calleeGPR, operand, CCallHelpers::TimesEight), calleeGPR);
        callAnswer();

        isNotInStorage.link(&jit);
        Jump isNotExtraValueInStorage = branchUnlessForm(jit, use, calleeGPR, Form::ExtraValueInStorage);
        loadExtraProperties(isNotAsked);
        jit.loadPtr(BaseIndex(cellCageBaseGPR, calleeGPR, CCallHelpers::TimesEight, JSObject::butterflyOffset()), calleeGPR);
        jit.signExtend32ToPtr(operand, operand);
        jit.load64(BaseIndex(calleeGPR, operand, CCallHelpers::TimesEight), calleeGPR);
        callAnswer();

        isNotExtraValueInStorage.link(&jit);
        Jump isNotExtraValueInCell = branchUnlessForm(jit, use, calleeGPR, Form::ExtraValueInCell);
        loadExtraProperties(isNotAsked);
        jit.add32(operand, calleeGPR);
        jit.load64(BaseIndex(cellCageBaseGPR, calleeGPR, CCallHelpers::TimesEight), calleeGPR);
        callAnswer();

        isNotExtraValueInCell.link(&jit);
        mayBeGetter.append(branchUnlessForm(jit, use, calleeGPR, Form::OwnFieldInExtension));
        jit.load32(Address(thisGPR, offsetOfFirstField), calleeGPR);
        jit.getEffectiveAddress(BaseIndex(cellCageBaseGPR, calleeGPR, static_cast<CCallHelpers::Scale>(cell32Shift), offsetOfEmptyExtensionInCage), calleeGPR);
        jit.load64(Address(calleeGPR, offsetOfExtensionGranuleBits), key);
        jit.lshift64(operand, key);
        isNotAsked.append(jit.branchTest64(CCallHelpers::PositiveOrZero, key));
        jit.countPopulation64(key, key, CCallHelpers::fpTempRegister);
        jit.load64(BaseIndex(calleeGPR, key, CCallHelpers::TimesEight), calleeGPR);
        isNotAsked.append(jit.branchTest64(CCallHelpers::Zero, calleeGPR));
        callAnswer();
        differs.link(&jit);
    };
    hasOtherKey.link(&jit);
    callValueOfOtherForm(offsetOfFirstAnswer);
    answers.loadBitsOfForm(jit, offsetOfSecondAnswer, thisGPR, calleeGPR, operand, keyOfAnswer);
    Jump secondHasOtherForm = jit.branchTest32(CCallHelpers::NonZero, calleeGPR);
    jit.load32(answers.word(offsetOfFirstAnswer + OBJECT_OFFSETOF(SiteAnswers::Answer, operand)), operandOfCommonForm);
    jit.loadPtr(BaseIndex(cellCageBaseGPR, operandOfCommonForm, CCallHelpers::TimesEight, JSObject::butterflyOffset()), operandOfCommonForm);
    jit.load32SignedExtendTo64(answers.word(offsetOfSecondAnswer + OBJECT_OFFSETOF(SiteAnswers::Answer, operand)), index);
    jit.load64(BaseIndex(operandOfCommonForm, index, CCallHelpers::TimesEight), calleeGPR);
    callAnswer();
    secondHasOtherForm.link(&jit);
    callValueOfOtherForm(offsetOfSecondAnswer);

    Label isMissed = jit.label();
    countMissAtSite(jit, thisGPR, distance, key, place, use);
    isNotAsked.link(&jit);
    Label goesByName = jit.label();
    if (name == NameAtSite::HasColumn) {
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.namesOfSitesByDistance)), key);
        jit.load16(BaseIndex(key, distance, CCallHelpers::TimesOne), distance);
    } else
        loadNameOfSite(jit, distance, distance, key);
    jit.jumpToStub(callMethodById(count, lentArguments));

    JumpList callsGetter;
    JumpList findsNoGetter;
    mayBeGetter.link(&jit);
    Jump isNotOwnInCell = branchUnlessForm(jit, use, calleeGPR, Form::OwnGetterInCell);
    jit.load64(BaseIndex(thisGPR, operand, CCallHelpers::TimesOne), calleeGPR);
    findsNoGetter.append(jit.branchTest64(CCallHelpers::Zero, calleeGPR));
    callsGetter.append(jit.jump());

    isNotOwnInCell.link(&jit);
    Jump isNotExtraInCell = branchUnlessForm(jit, use, calleeGPR, Form::ExtraGetterInCellAtCall);
    loadExtraProperties(findsNoGetter);
    jit.add32(operand, calleeGPR);
    jit.load64(BaseIndex(cellCageBaseGPR, calleeGPR, CCallHelpers::TimesEight), calleeGPR);
    callsGetter.append(jit.jump());

    isNotExtraInCell.link(&jit);
    Jump isNotOwnInStorage = branchUnlessForm(jit, use, calleeGPR, Form::OwnGetterInStorage);
    jit.loadPtr(Address(thisGPR, JSObject::butterflyOffset()), calleeGPR);
    jit.signExtend32ToPtr(operand, operand);
    jit.load64(BaseIndex(calleeGPR, operand, CCallHelpers::TimesEight), calleeGPR);
    callsGetter.append(jit.jump());

    isNotOwnInStorage.link(&jit);
    Jump isNotExtraInStorage = branchUnlessForm(jit, use, calleeGPR, Form::ExtraGetterInStorageAtCall);
    loadExtraProperties(findsNoGetter);
    jit.loadPtr(BaseIndex(cellCageBaseGPR, calleeGPR, CCallHelpers::TimesEight, JSObject::butterflyOffset()), calleeGPR);
    jit.signExtend32ToPtr(operand, operand);
    jit.load64(BaseIndex(calleeGPR, operand, CCallHelpers::TimesEight), calleeGPR);
    callsGetter.append(jit.jump());

    isNotExtraInStorage.link(&jit);
    Jump isNotInWordAbove = branchUnlessForm(jit, use, calleeGPR, Form::GetterInWordOfCage);
    jit.load64(BaseIndex(cellCageBaseGPR, operand, CCallHelpers::TimesEight), calleeGPR);
    callsGetter.append(jit.jump());

    isNotInWordAbove.link(&jit);
    branchUnlessForm(jit, use, calleeGPR, Form::GetterInStorageOfHolder).linkTo(isMissed, &jit);
    jit.loadPtr(BaseIndex(cellCageBaseGPR, operand, CCallHelpers::TimesEight, JSObject::butterflyOffset()), calleeGPR);
    jit.load32SignedExtendTo64(answers.word(offsetOfSecondAnswer + OBJECT_OFFSETOF(SiteAnswers::Answer, operand)), operand);
    jit.load64(BaseIndex(calleeGPR, operand, CCallHelpers::TimesEight), calleeGPR);

    callsGetter.link(&jit);
    branchUnlessAccessorPair(jit, calleeGPR, findsNoGetter);
    findsNoGetter.linkTo(goesByName, &jit);
    if (jit.options().checksWalk) {
        jit.loadPtr(Address(calleeGPR, GetterSetter::offsetOfGetter()), key);
        askAgainWhatSiteAnswers(jit, thisGPR, key, distance, KindOfAnswer::Accessor, StubThatAnswers::CachedGetterByName);
    }
    const StubKey callOfGetter = callGetterOfAnswer(CallerFrame::StubFrame);
    Vector<GPRReg, 1 + numberOfArgumentGPRs> kept;
    kept.append(thisGPR);
    for (unsigned index = 0; index < count; ++index)
        kept.append(argumentGPR(index));
    jit.emitFunctionPrologue();
    jit.preserving(std::span<const GPRReg>(kept.span()), [&] {
        jit.move(thisGPR, gprOf(firstStubOperandRegister));
        checkStubOperands(callOfGetter, { gprOf(firstStubOperandRegister), calleeGPR });
        jit.callStub(callOfGetter);
        jit.bindReturnLabel(StubFrameKind::MethodRead);
        jit.move(GPRInfo::returnValueGPR, calleeGPR);
    });
    jit.emitFunctionEpilogue();
    jit.jumpToStub(callsValue);
}

void generateCallMethodAtSite(StubAssembler& jit)
{
    emitCallMethodAtSite(jit, 0, static_cast<NameAtSite>(jit.key().form()));
}

void generateCallMethodAtSiteLending(StubAssembler& jit)
{
    emitCallMethodAtSite(jit, jit.key().form() % numberOfSetsOfLentArguments + 1, static_cast<NameAtSite>(jit.key().form() / numberOfSetsOfLentArguments));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
