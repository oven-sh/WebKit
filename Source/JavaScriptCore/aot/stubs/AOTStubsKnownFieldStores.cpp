/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsPropertyProbes.h"

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTCompiledStructure.h"
#include "AOTFieldTable.h"
#include "AOTInstance.h"
#include "AOTLayoutData.h"
#include "AOTObjectWords.h"
#include "AOTPropertyCachesFront.h"
#include "AOTStubHelpers.h"
#include "AOTStubsField.h"
#include "AOTStubsKeyed.h"
#include "AOTStubsPropertyExtraProperties.h"

namespace JSC { namespace AOT {

StubSignature signatureOfPutKnownField(StubKey)
{
    StubSignature signature = StubSignature::withoutResult({ gprOf(firstStubOperandRegister), gprOf(storedValueByIdRegister) });
    signature.immediate = gprOf(slotNameRegister);
    return signature;
}

StubSignature signatureOfCreateKnownField(StubKey key)
{
    return signatureOfPutKnownField(key);
}

static constexpr ptrdiff_t offsetOfFlagsWord = OBJECT_OFFSETOF(FieldData, flags);
static_assert(offsetOfFlagsWord == sizeof(uint32_t) && OBJECT_OFFSETOF(FieldData, typeNumber) == offsetOfFlagsWord + sizeof(uint16_t) && FieldData::nameBits < 32);
static_assert(std::endian::native == std::endian::little);

static void loadNameOfField(StubAssembler& jit, GPRReg record, GPRReg name)
{
    jit.load32(Address(record), name);
    jit.and32(TrustedImm32((1 << FieldData::nameBits) - 1), name);
}

static void branchIfChainMayStopStore(StubAssembler& jit, GPRReg object, GPRReg structure, GPRReg name, GPRReg filter, GPRReg scratch, JumpList& mayStopStore)
{
    RELEASE_ASSERT(noOverlap(object, structure, name, filter, scratch, instanceGPR));
    mayStopStore.append(jit.branch32(CCallHelpers::Above, name, TrustedImm32(FieldTableEntry::maxName)));
    Jump isNotBelowObjectPrototype = jit.branchTest8(CCallHelpers::Zero, Address(structure, CompiledStructure::offsetOfFlags()), TrustedImm32(static_cast<uint8_t>(CompiledStructure::Flag::HasOnlyFieldsBelowObjectPrototype)));
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(propertyCaches.objectPrototypeStoreStoppers)), filter);
    jit.urshift32(name, TrustedImm32(6), scratch);
    jit.load64(BaseIndex(filter, scratch, CCallHelpers::TimesEight), scratch);
    mayStopStore.append(branchTestBitAt(jit, CCallHelpers::NonZero, scratch, name));
    Jump isAbsent = jit.jump();

    isNotBelowObjectPrototype.link(&jit);
    const GPRReg structureID = structure;
    jit.load32(Address(object, JSCell::structureIDOffset()), structureID);
    jit.urshift32(structureID, TrustedImm32(StructureRowWords::granuleShift), scratch);
    jit.and32(TrustedImm32(AbsenceFilter::numberOfFilters - 1), scratch);
    jit.lshift32(TrustedImm32(AbsenceFilter::sizeShift), scratch);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(propertyCaches.absenceFilters)), filter);
    jit.add64(scratch, filter);
    mayStopStore.append(jit.branch32(CCallHelpers::NotEqual, Address(filter, OBJECT_OFFSETOF(AbsenceFilter, structureID)), structureID));
    jit.load32(Address(instanceGPR, AOT_INSTANCE_OFFSET(propertyCaches.absenceFilterGenerationForStores)), scratch);
    mayStopStore.append(jit.branch32(CCallHelpers::NotEqual, Address(filter, OBJECT_OFFSETOF(AbsenceFilter, generationForStores)), scratch));
    jit.urshift32(name, TrustedImm32(AbsenceFilter::bitsOfWordShift), scratch);
    jit.and32(TrustedImm32(AbsenceFilter::numberOfWordsForStores - 1), scratch);
    jit.load64(BaseIndex(filter, scratch, CCallHelpers::TimesEight, OBJECT_OFFSETOF(AbsenceFilter, namesThatStopStores)), scratch);
    mayStopStore.append(branchTestBitAt(jit, CCallHelpers::NonZero, scratch, name));
    jit.load32(Address(instanceGPR, AOT_INSTANCE_OFFSET(propertyCaches.changesUnderNamesOnChains)), scratch);
    Jump filterIsCurrent = jit.branch32(CCallHelpers::Equal, Address(filter, OBJECT_OFFSETOF(AbsenceFilter, changesUnderNames)), scratch);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(propertyCaches.namesChangedOnChains)), filter);
    jit.urshift32(name, TrustedImm32(6), scratch);
    jit.load64(BaseIndex(filter, scratch, CCallHelpers::TimesEight), scratch);
    mayStopStore.append(branchTestBitAt(jit, CCallHelpers::NonZero, scratch, name));

    filterIsCurrent.link(&jit);
    isAbsent.link(&jit);
}

static void generatePutKnownFieldThatTakesAnyValue(StubAssembler& jit)
{
    const PutMode mode = static_cast<PutMode>(jit.key().form());
    const GPRReg object = jit.operand(0);
    const GPRReg value = jit.operand(1);
    const GPRReg offsetOfRecord = jit.immediate();
    const GPRReg offset = jit.allocateScratch(StubRegister::T10);
    const GPRReg T11 = jit.allocateScratch(StubRegister::T11);
    const GPRReg record = jit.allocateScratch(slotHeaderRegister);
    const GPRReg structure = jit.allocateScratch(slotPayloadRegister);
    const GPRReg T14 = jit.allocateScratch(StubRegister::T14);
    const Address orderWord(object, offsetOfOrderWord);
    const Address flagsOfField(record, offsetOfFlagsWord);
    const BaseIndex slot(object, offset, CCallHelpers::TimesOne);
    constexpr uint32_t flagsOfUnusualFields = fieldAttributeFlags | fieldIsPrivate | fieldIsBrand | fieldIsOutOfLine | fieldMayExistWithoutStorage | fieldTypeIsLeftToRunTime;
    JumpList isLeftToStubForAnyField;

    loadStructure(jit, object, structure);
    jit.loadPtr(Address(structure, CompiledStructure::offsetOfLayout()), record);
    jit.addPtr(offsetOfRecord, record);
    isLeftToStubForAnyField.append(jit.branchTest32(CCallHelpers::NonZero, flagsOfField, TrustedImm32(flagsOfUnusualFields)));
    isLeftToStubForAnyField.append(jit.branchTest32(CCallHelpers::NonZero, orderWord, TrustedImm32(integrityBitsBlockingStore(operationFor(mode)))));
    jit.load32(Address(record, OBJECT_OFFSETOF(FieldData, slot)), offset);
    jit.and32(TrustedImm32(SlotData::maxOffset), offset);
    Jump isAbsent = jit.branchTest64(CCallHelpers::Zero, slot);
    jit.store64(value, slot);
    returnAfterStoring(jit, object, value);

    isAbsent.link(&jit);
    const KeyOrderRegisters keyOrder { .object = object, .slotKey = offset, .entry = T11, .words = T14, .scratch = structure };
    if (mode != PutMode::Define) {
        const GPRReg orderAfter = keyOrder.scratch;
        JumpList isNotRemembered;
        loadOrderAfterRememberedCreation(jit, keyOrder, isNotRemembered);
        jit.store64(value, slot);
        jit.store32(orderAfter, orderWord);
        returnAfterStoring(jit, object, value);
        isNotRemembered.link(&jit);
        loadStructure(jit, object, structure);
    }
    isLeftToStubForAnyField.append(jit.branchTest32(CCallHelpers::NonZero, orderWord, TrustedImm32(static_cast<int32_t>(orderWordIntegrityBits | orderWordHasFieldOutsideSlot))));
    isLeftToStubForAnyField.append(jit.branchTest32(CCallHelpers::NonZero, Address(structure, Structure::bitFieldOffset()), TrustedImm32(Structure::s_mayBePrototypeBits)));
    isLeftToStubForAnyField.append(jit.branchTest32(CCallHelpers::NonZero, offset, TrustedImm32(offsetBitsWithoutSlotKey)));
    JumpList goesByName;
    if (mode != PutMode::Define) {
        const GPRReg nameOfField = T14;
        const GPRReg filter = jit.allocateScratch(StubRegister::A2);
        loadNameOfField(jit, record, nameOfField);
        branchIfChainMayStopStore(jit, object, structure, nameOfField, filter, T11, goesByName);
    }
    jit.store64(value, slot);

    static_assert(slotKeyFor(8, false) == 8);
    const GPRReg layout = record;
    jit.subPtr(offsetOfRecord, layout);
    listCreatedFieldAndReturn(jit, keyOrder, value, FieldOfLayout { layout, offsetOfRecord }, mode == PutMode::Define ? CreationChecksChain::No : CreationChecksChain::Yes);

    if (mode != PutMode::Define) {
        const GPRReg name = offsetOfRecord;
        goesByName.link(&jit);
        loadNameOfField(jit, record, name);
        jit.linkToStub(jit.branch32(CCallHelpers::Above, name, TrustedImm32(RowDisplacementTable::maxColumn)), putByIdHashed(mode));
        jit.jumpToStub(putByIdBehindFieldTable(mode));
    }
    jit.linkToStub(isLeftToStubForAnyField, putKnownField(mode));
}

static void generatePutKnownFieldAcceptingAnyNumber(StubAssembler& jit, SlotRepresentation representation)
{
    static_assert(traitsOf(SlotRepresentation::OptionalDouble).absentBits == absentDouble && !traitsOf(SlotRepresentation::Double).hasAbsent);
    const PutMode mode = static_cast<PutMode>(jit.key().form());
    const GPRReg object = jit.operand(0);
    const GPRReg value = jit.operand(1);
    const GPRReg offsetOfRecord = jit.immediate();
    const GPRReg offset = jit.allocateScratch(StubRegister::T10);
    const GPRReg scratch = jit.allocateScratch(StubRegister::T11);
    const GPRReg record = jit.allocateScratch(slotHeaderRegister);
    const GPRReg structure = jit.allocateScratch(slotPayloadRegister);
    const GPRReg orderAfter = structure;
    const FPRReg number = CCallHelpers::fpTempRegister;
    const Address orderWord(object, offsetOfOrderWord);
    const BaseIndex slot(object, offset, CCallHelpers::TimesOne);
    constexpr uint32_t flagsOfUnusualFields = fieldAttributeFlags | fieldIsPrivate | fieldIsBrand | fieldIsOutOfLine | fieldMayExistWithoutStorage | fieldTypeIsLeftToRunTime;
    JumpList isLeftToStubForAnyField;

    isLeftToStubForAnyField.append(jit.branchIfNotNumber(value));
    loadStructure(jit, object, structure);
    jit.loadPtr(Address(structure, CompiledStructure::offsetOfLayout()), record);
    jit.addPtr(offsetOfRecord, record);
    isLeftToStubForAnyField.append(jit.branchTest32(CCallHelpers::NonZero, Address(record, offsetOfFlagsWord), TrustedImm32(flagsOfUnusualFields)));
    isLeftToStubForAnyField.append(jit.branchTest32(CCallHelpers::NonZero, orderWord, TrustedImm32(integrityBitsBlockingStore(operationFor(mode)))));
    jit.load32(Address(record, OBJECT_OFFSETOF(FieldData, slot)), offset);
    jit.and32(TrustedImm32(SlotData::maxOffset), offset);
    auto storeNumberAndReturn = [&](bool listsField) {
        auto finish = [&] {
            if (listsField)
                jit.store32(orderAfter, orderWord);
            jit.returnToCaller();
        };
        if (representation == SlotRepresentation::JSValue) {
            jit.store64(value, slot);
            finish();
            return;
        }
        Jump isNotInt32 = jit.branchIfNotInt32(value);
        jit.convertInt32ToDouble(value, number);
        jit.storeDouble(number, slot);
        finish();

        isNotInt32.link(&jit);
        unboxDouble(jit, value, scratch, number);
        jit.storeDouble(number, slot);
        finish();
    };
    Jump isAbsent;
    if (representation == SlotRepresentation::JSValue)
        isAbsent = jit.branchTest64(CCallHelpers::Zero, slot);
    else {
        jit.load64(slot, scratch);
        isAbsent = jit.branch64(CCallHelpers::Equal, scratch, TrustedImm64(static_cast<int64_t>(absentDouble)));
    }
    storeNumberAndReturn(false);

    isAbsent.link(&jit);
    if (mode == PutMode::Define)
        isLeftToStubForAnyField.append(jit.jump());
    else {
        loadOrderAfterRememberedCreation(jit, { .object = object, .slotKey = offset, .entry = scratch, .words = jit.allocateScratch(StubRegister::T14), .scratch = orderAfter }, isLeftToStubForAnyField);
        storeNumberAndReturn(true);
    }

    jit.linkToStub(isLeftToStubForAnyField, putKnownField(mode));
}

void generatePutKnownField(StubAssembler& jit)
{
    const PutMode mode = static_cast<PutMode>(jit.key().form());
    const unsigned whatCallerKnows = jit.key().count();
    if (whatCallerKnows == putKnownFieldAcceptingAnyNumber(mode).count()) {
        generatePutKnownFieldAcceptingAnyNumber(jit, SlotRepresentation::Double);
        return;
    }
    if (whatCallerKnows == putKnownFieldKeepingNumbersAsValues(mode).count()) {
        generatePutKnownFieldAcceptingAnyNumber(jit, SlotRepresentation::JSValue);
        return;
    }
    if (whatCallerKnows == putKnownFieldThatTakesAnyValue(mode).count()) {
        generatePutKnownFieldThatTakesAnyValue(jit);
        return;
    }
    const GPRReg object = jit.operand(0);
    const GPRReg value = jit.operand(1);
    const GPRReg name = jit.immediate();
    const GPRReg T10 = jit.allocateScratch(StubRegister::T10);
    const GPRReg T11 = jit.allocateScratch(StubRegister::T11);
    const GPRReg record = jit.allocateScratch(slotHeaderRegister);
    const GPRReg structure = jit.allocateScratch(slotPayloadRegister);
    const GPRReg T14 = jit.allocateScratch(StubRegister::T14);
    const FieldStoreRegisters registers {
        .field = { .object = object, .slotData = T10, .extensionWordOffset = T11, .address = T11, .representation = name },
        .value = value,
        .type = T14,
        .scratch = T10,
        .secondScratch = jit.allocateScratch(StubRegister::A2),
        .number = jit.allocateScratch(FPRInfo::fpRegT0),
    };
    const Address orderWord(object, offsetOfOrderWord);
    constexpr uint32_t flagsLeftToRunTime = fieldAttributeFlags | fieldIsPrivate | fieldIsBrand | fieldTypeIsLeftToRunTime;
    JumpList goesByName;
    using WordsOfField = std::array<uint32_t, sizeof(FieldData) / sizeof(uint32_t)>;

    loadStructure(jit, object, structure);
    jit.loadPtr(Address(structure, CompiledStructure::offsetOfLayout()), record);
    jit.addPtr(name, record);
    goesByName.append(jit.branchTest32(CCallHelpers::NonZero, Address(record, offsetOfFlagsWord), TrustedImm32(flagsLeftToRunTime)));
    goesByName.append(jit.branchTest32(CCallHelpers::NonZero, orderWord, TrustedImm32(integrityBitsBlockingStore(operationFor(mode)))));
    jit.load32(Address(record, OBJECT_OFFSETOF(FieldData, slot)), registers.field.slotData);
    loadTypeOfFieldRecord(jit, record, 0, registers.type);

    JumpList hasNoStorage;
    WaysToFieldInExtension ways;
    FieldStoreExits exits;
    Jump isOutOfLine = jit.branchTest32(CCallHelpers::NonZero, Address(record, offsetOfFlagsWord), TrustedImm32(fieldIsOutOfLine));
    computeFieldAddress(jit, registers.field, false, hasNoStorage);
    RELEASE_ASSERT(hasNoStorage.empty());
    Label hasAddress = jit.label();
    auto emitRememberingOfKindAs = [&](uint32_t bits) {
        JumpList isNotRemembered;
        const GPRReg nameOfField = registers.field.representation;
        const GPRReg index = structure;
        isNotRemembered.append(jit.branchTest32(CCallHelpers::Zero, registers.type));
        isNotRemembered.append(jit.branchTest32(CCallHelpers::NonZero, Address(record, offsetOfFlagsWord), TrustedImm32(fieldIsOutOfLine)));
        loadNameOfField(jit, record, nameOfField);
        loadIndexInFieldTable(jit, object, nameOfField, index, record, isNotRemembered);
        jit.or32(TrustedImm32(static_cast<int32_t>(bits)), index);
        rememberAcceptedKindOfValue(jit, value, registers.type, index, registers.field.address, registers.scratch, record);
        isNotRemembered.link(&jit);
    };
    auto emitRememberingOfKind = [&](GPRReg) {
        emitRememberingOfKindAs(AcceptedKindOfValue::isEntryOfFieldTable);
    };
    const ScopedLambda<void(GPRReg)> rememberKind(emitRememberingOfKind);
    storeField(jit, registers, FieldStoreEnd::ReturnAfterBarrier, FieldTypeTest::IsEmitted, nullptr, exits, &rememberKind);
    JumpList isRefusedBySlot;
    isRefusedBySlot.append(exits.cannot);

    isOutOfLine.link(&jit);
    jit.load32(Address(structure, CompiledStructure::offsetOfExtensionSlot()), registers.field.extensionWordOffset);
    jit.and32(TrustedImm32(SlotData::maxOffset), registers.field.extensionWordOffset);
    computeFieldAddress(jit, registers.field, true, hasNoStorage, &ways);
    jit.jump().linkTo(hasAddress, &jit);

    const KeyOrderRegisters keyOrder { .object = object, .slotKey = T10, .entry = T11, .words = T14, .scratch = name };
    FieldStoreExits creationExits;
    JumpList stored;

    hasNoStorage.link(&jit);
    goesByName.append(jit.branchTest32(CCallHelpers::NonZero, orderWord, TrustedImm32(static_cast<int32_t>(orderWordIntegrityBits | orderWordHasFieldOutsideSlot))));
    Jump alwaysNeedsStorage = jit.branchTest32(CCallHelpers::Zero, Address(record, offsetOfFlagsWord), TrustedImm32(fieldMayExistWithoutStorage));
    Jump needsNoStorage = jit.branch64(CCallHelpers::Equal, value, TrustedImm32(JSValue::ValueUndefined));
    alwaysNeedsStorage.link(&jit);
    Jump createsGranule = jit.jump();
    auto reloadSlotBefore = [&](Label way) {
        Label reloads = jit.label();
        jit.load32(Address(record, OBJECT_OFFSETOF(FieldData, slot)), registers.field.slotData);
        jit.jump().linkTo(way, &jit);
        return reloads;
    };
    const WaysToFieldInExtension reloadingWays { reloadSlotBefore(ways.fromExtension), reloadSlotBefore(ways.fromGranule) };
    createsGranule.link(&jit);
    createGranuleOfField(jit, registers, reloadingWays, FirstExtension::IsAllocatedInLine);

    auto leaveUnlessFieldIsCreatedHere = [&](JumpList& isFixedOrSomeFieldHasLeft) {
        isFixedOrSomeFieldHasLeft.append(jit.branchTest32(CCallHelpers::NonZero, orderWord, TrustedImm32(static_cast<int32_t>(orderWordIntegrityBits | orderWordHasFieldOutsideSlot))));
        goesByName.append(jit.branchTest32(CCallHelpers::NonZero, Address(structure, Structure::bitFieldOffset()), TrustedImm32(Structure::s_mayBePrototypeBits)));
        goesByName.append(jit.branchTest32(CCallHelpers::NonZero, Address(record, OBJECT_OFFSETOF(FieldData, slot)), TrustedImm32(offsetBitsWithoutSlotKey)));
        if (mode != PutMode::Define) {
            const GPRReg nameOfField = registers.type;
            loadNameOfField(jit, record, nameOfField);
            branchIfChainMayStopStore(jit, object, structure, nameOfField, registers.secondScratch, registers.scratch, goesByName);
        }
    };

    needsNoStorage.link(&jit);
    FieldData fieldWithRawValue { };
    fieldWithRawValue.holdsRawValueFromBirth = 1;
    goesByName.append(jit.branchTest32(CCallHelpers::NonZero, Address(record), TrustedImm32(static_cast<int32_t>(std::bit_cast<WordsOfField>(fieldWithRawValue)[0]))));
    branchUnlessTypeAccepts(jit, { value, registers.type, registers.field.representation, registers.scratch, registers.secondScratch, object }, goesByName, goesByName);
    leaveUnlessFieldIsCreatedHere(goesByName);
    const GPRReg orderAfter = structure;
    const TrustedImm32 isNotRemembered(0);
    jit.move(isNotRemembered, orderAfter);
    stored.append(jit.jump());

    auto loadSlotKeyInCell = [&](GPRReg slotKey) {
        jit.load32(Address(record, OBJECT_OFFSETOF(FieldData, slot)), slotKey);
        jit.and32(TrustedImm32(SlotData::maxOffset), slotKey);
    };
    JumpList mayComeBack;
    JumpList hasOrderAfter;
    exits.wasAbsent.link(&jit);
    Label createsField = jit.label();
    if (mode != PutMode::Define) {
        JumpList asksChain;
        const KeyOrderRegisters creation { .object = object, .slotKey = T10, .entry = registers.secondScratch, .words = registers.type, .scratch = orderAfter };
        RELEASE_ASSERT(noOverlap(creation.slotKey, creation.entry, creation.words, creation.scratch, record, value, registers.field.address, registers.field.representation));
        loadSlotKeyInCell(creation.slotKey);
        Jump isInCell = jit.branchTest32(CCallHelpers::Zero, Address(record, offsetOfFlagsWord), TrustedImm32(fieldIsOutOfLine));
        jit.or32(TrustedImm32(outOfLineSlotKey), creation.slotKey);
        isInCell.link(&jit);
        loadOrderAfterRememberedCreation(jit, creation, asksChain);
        hasOrderAfter.append(jit.jump());
        asksChain.link(&jit);
        loadStructure(jit, object, structure);
    }
    leaveUnlessFieldIsCreatedHere(mayComeBack);
    Label storesInEmptySlot = jit.label();
    jit.move(isNotRemembered, orderAfter);
    hasOrderAfter.link(&jit);
    storeField(jit, registers, FieldStoreEnd::JoinList, FieldTypeTest::HasPassed, &stored, creationExits);
    RELEASE_ASSERT(creationExits.wasAbsent.empty() && creationExits.wasAbsentAndIsNotAccepted.empty());
    goesByName.append(creationExits.cannot);

    stored.link(&jit);
    Jump isRemembered = jit.branchTest32(CCallHelpers::NonZero, orderAfter);
    loadSlotKeyInCell(keyOrder.slotKey);
    Jump isInCell = jit.branchTest32(CCallHelpers::Zero, Address(record, offsetOfFlagsWord), TrustedImm32(fieldIsOutOfLine | fieldRecordIsCopyInFieldReturn));
    Jump hasComeBack = jit.branchTest32(CCallHelpers::NonZero, Address(record, offsetOfFlagsWord), TrustedImm32(fieldRecordIsCopyInFieldReturn));
    jit.or32(TrustedImm32(outOfLineSlotKey), keyOrder.slotKey);
    isInCell.link(&jit);
    listCreatedFieldAndReturn(jit, keyOrder, value, std::nullopt, mode == PutMode::Define ? CreationChecksChain::No : CreationChecksChain::Yes);

    isRemembered.link(&jit);
    jit.store32(orderAfter, orderWord);
    FieldData fieldWithMarkers { };
    fieldWithMarkers.slotHoldsUndefinedAndNull = 1;
    Jump isNoMarker = jit.branchIfNotOther(value, registers.scratch);
    Jump mayBeMarker = jit.branchTest32(CCallHelpers::NonZero, Address(record), TrustedImm32(static_cast<int32_t>(std::bit_cast<WordsOfField>(fieldWithMarkers)[0])));
    isNoMarker.link(&jit);
    loadTypeOfFieldRecord(jit, record, 0, registers.type);
    emitRememberingOfKind(registers.type);
    mayBeMarker.link(&jit);
    returnAfterStoring(jit, object, value);

    mayComeBack.link(&jit);
    {
        const GPRReg back = structure;
        const GPRReg nameOfField = registers.type;
        const GPRReg scratch = registers.scratch;
        const GPRReg extra = registers.secondScratch;
        auto member = [&](ptrdiff_t offset) {
            return Address(back, static_cast<int32_t>(offset));
        };
        RELEASE_ASSERT(noOverlap(object, value, record, back, nameOfField, scratch, extra, registers.field.address, registers.field.representation, instanceGPR));
        loadNameOfField(jit, record, nameOfField);
        jit.load32(Address(object, JSCell::structureIDOffset()), scratch);
        jit.urshift32(scratch, TrustedImm32(StructureRowWords::granuleShift), back);
        jit.add32(nameOfField, back);
        jit.and32(TrustedImm32(FieldReturn::numberOfPlaces - 1), back);
        jit.lshift32(TrustedImm32(FieldReturn::sizeShift), back);
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(propertyCaches.fieldReturns)), extra);
        goesByName.append(jit.branchTestPtr(CCallHelpers::Zero, extra));
        jit.addPtr(extra, back);
        goesByName.append(jit.branch32(CCallHelpers::NotEqual, member(OBJECT_OFFSETOF(FieldReturn, structureBefore)), scratch));
        goesByName.append(jit.branch32(CCallHelpers::NotEqual, member(OBJECT_OFFSETOF(FieldReturn, name)), nameOfField));
        jit.load32(orderWord, scratch);
        goesByName.append(jit.branch32(CCallHelpers::NotEqual, member(OBJECT_OFFSETOF(FieldReturn, orderWord)), scratch));
        goesByName.append(branchIfBarrierIsNeeded(jit, object, scratch));
        goesByName.append(jit.branch32(CCallHelpers::Equal, scratch, TrustedImm32(frameCellState)));
        jit.load16(member(OBJECT_OFFSETOF(FieldReturn, offsetOfExtraPropertiesWord)), extra);
        loadExtraProperties(jit, object, extra, extra, goesByName);
        jit.load32(member(OBJECT_OFFSETOF(FieldReturn, extraPropertiesBefore)), scratch);
        goesByName.append(jit.branch32(CCallHelpers::NotEqual, Address(extra, JSCell::structureIDOffset()), scratch));
        goesByName.append(branchIfBarrierIsNeeded(jit, extra, scratch));
        jit.move(back, record);
        jit.jump().linkTo(storesInEmptySlot, &jit);
    }

    hasComeBack.link(&jit);
    {
        const GPRReg back = record;
        const GPRReg extra = T10;
        const GPRReg scratch = T14;
        auto member = [&](ptrdiff_t offset) {
            return Address(back, static_cast<int32_t>(offset));
        };
        JumpList hasNone;
        jit.load16(member(OBJECT_OFFSETOF(FieldReturn, offsetOfExtraPropertiesWord)), extra);
        loadExtraProperties(jit, object, extra, extra, hasNone);
        jit.load32(member(OBJECT_OFFSETOF(FieldReturn, offset)), scratch);
        jit.store64(TrustedImm32(0), BaseIndex(extra, scratch, CCallHelpers::TimesEight, JSObject::offsetOfInlineStorage()));
        jit.load32(member(OBJECT_OFFSETOF(FieldReturn, extraPropertiesAfter)), scratch);
        jit.store32(scratch, Address(extra, JSCell::structureIDOffset()));
        jit.load32(member(OBJECT_OFFSETOF(FieldReturn, structureAfter)), scratch);
        jit.store32(scratch, Address(object, JSCell::structureIDOffset()));
        returnAfterStoring(jit, object, value);
        hasNone.link(&jit);
        jit.breakpoint();
    }

    jit.bindEntry(createKnownField(mode));
    loadStructure(jit, object, structure);
    jit.loadPtr(Address(structure, CompiledStructure::offsetOfLayout()), record);
    jit.addPtr(name, record);
    goesByName.append(jit.branchTest32(CCallHelpers::NonZero, Address(record, offsetOfFlagsWord), TrustedImm32(flagsLeftToRunTime | fieldIsOutOfLine)));
    jit.load32(Address(record, OBJECT_OFFSETOF(FieldData, slot)), registers.field.slotData);
    computeFieldAddress(jit, registers.field, false, hasNoStorage);
    jit.jump().linkTo(createsField, &jit);

    exits.wasAbsentAndIsNotAccepted.link(&jit);
    isRefusedBySlot.append(jit.branchTest32(CCallHelpers::Zero, orderWord, TrustedImm32(static_cast<int32_t>(orderWordHasFieldOutsideSlot))));
    isRefusedBySlot.append(jit.branchIfNotCell(value));
    loadNameOfField(jit, record, name);
    jit.linkToStub(jit.branch32(CCallHelpers::Above, name, TrustedImm32(RowDisplacementTable::maxColumn)), putByIdHashed(mode));
    loadTypeOfFieldRecord(jit, record, 0, registers.type);
    emitRememberingOfKindAs(AcceptedKindOfValue::isEntryOfFieldTable | AcceptedKindOfValue::isRefused);
    jit.jumpToStub(putValueThatSlotRefuses(mode));

    isRefusedBySlot.link(&jit);
    loadNameOfField(jit, record, name);
    jit.linkToStub(jit.branch32(CCallHelpers::Above, name, TrustedImm32(RowDisplacementTable::maxColumn)), putByIdHashed(mode));
    jit.jumpToStub(putValueThatSlotRefuses(mode));

    goesByName.link(&jit);
    loadNameOfField(jit, record, name);
    jit.linkToStub(jit.branch32(CCallHelpers::Above, name, TrustedImm32(RowDisplacementTable::maxColumn)), putByIdHashed(mode));
    jit.jumpToStub(putByIdBehindFieldTable(mode));
}

StubSignature signatureOfPutByIdOnce(StubKey key)
{
    return signatureOfPutKnownField(key);
}

void generatePutByIdOnce(StubAssembler& jit)
{
    const PutMode mode = static_cast<PutMode>(jit.key().form());
    const SlotHandlerTable table = slotHandlerTable(operationFor(mode), SlotOrigin::ById, SlotForm::Slot);
    const GPRReg index = jit.allocateScratch(StubRegister::T11);
    const GPRReg entries = jit.allocateScratch(slotHeaderRegister);
    const GPRReg scratch = jit.allocateScratch(slotPayloadRegister);
    JumpList isNotInFieldTable;
    storeFieldByTable(jit, { jit.operand(0), jit.immediate(), index, entries, false, false }, jit.operand(1), scratch, operationFor(mode), isNotInFieldTable);
    linkToSlotExit(jit, isNotInFieldTable, table, SlotExit::Uncached);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
