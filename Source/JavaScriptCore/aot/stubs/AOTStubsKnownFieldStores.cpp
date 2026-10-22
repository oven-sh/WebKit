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

namespace JSC { namespace AOT {

// A store by name into a compiled object that has a field of the name. Whoever comes here knows the field: a site whose receiver has a proven layout, or a stub by name that found the field in the table of fields.
// So nothing is looked up, missed or filed: the immediate says how far behind the layout's data the record of the field lies, and the record says where the slot is and what it admits.
// A field that is not there yet is created here too, where the object itself says that it may be and nothing above the object has the name.
// Everything else goes on as a store by name behind the table of fields, under the name that the record has.
StubSignature signatureOfPutKnownField(StubKey)
{
    StubSignature signature;
    signature.addOperand(gprOf(firstStubOperandRegister));
    signature.addOperand(gprOf(storedValueByIdRegister));
    signature.immediate = gprOf(slotNameRegister);
    return signature;
}

StubSignature signatureOfCreateKnownField(StubKey key)
{
    return signatureOfPutKnownField(key);
}

static constexpr uint32_t offsetBitsWithoutSlotKey = SlotData::maxOffset & ~(outOfLineSlotKey - 1);
// The record's second word: the flags are its low half.
static constexpr ptrdiff_t offsetOfFlagsWord = OBJECT_OFFSETOF(FieldData, flags);
static_assert(offsetOfFlagsWord == sizeof(uint32_t) && OBJECT_OFFSETOF(FieldData, operandIndex) == offsetOfFlagsWord + sizeof(uint16_t) && FieldData::nameBits < 32);
static_assert(std::endian::native == std::endian::little);

static constexpr uint32_t hasGuardOfStoresInFirstWord = 1u << (FieldData::nameBits + 2);

static void loadNameOfField(StubAssembler& jit, GPRReg record, GPRReg name)
{
    jit.load32(Address(record), name);
    jit.and32(TrustedImm32((1 << FieldData::nameBits) - 1), name);
}

// Goes on where no object above the receiver has an accessor or a property that cannot be written under the name, so that a store creates the receiver's own property.
// Below the Object.prototype of its realm a flag of the Structure and the names of that object say so. Elsewhere the filter of the Structure does.
static void branchIfChainMayHaveName(StubAssembler& jit, GPRReg object, GPRReg structure, GPRReg name, GPRReg filter, GPRReg scratch, JumpList& mayHaveName)
{
    RELEASE_ASSERT(noOverlap(object, structure, name, filter, scratch, instanceGPR));
    mayHaveName.append(jit.branch32(CCallHelpers::Above, name, TrustedImm32(FieldTableEntry::maxName)));
    Jump isNotBelowObjectPrototype = jit.branchTest8(CCallHelpers::Zero, Address(structure, CompiledStructure::offsetOfFlags()), TrustedImm32(static_cast<uint8_t>(CompiledStructure::Flag::HasOnlyFieldsBelowObjectPrototype)));
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(propertyCaches.namesOfObjectPrototypes)), filter);
    jit.urshift32(name, TrustedImm32(6), scratch);
    jit.load64(BaseIndex(filter, scratch, CCallHelpers::TimesEight), scratch);
    // A shift takes the low six bits of its count.
    jit.urshift64(name, scratch);
    mayHaveName.append(jit.branchTest64(CCallHelpers::NonZero, scratch, TrustedImm32(1)));
    Jump isAbsent = jit.jump();

    isNotBelowObjectPrototype.link(&jit);
    const GPRReg structureID = structure;
    jit.load32(Address(object, JSCell::structureIDOffset()), structureID);
    jit.urshift32(structureID, TrustedImm32(StructureRowWords::granuleShift), scratch);
    jit.and32(TrustedImm32(AbsenceFilter::numberOfFilters - 1), scratch);
    jit.lshift32(TrustedImm32(AbsenceFilter::sizeShift), scratch);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(propertyCaches.absenceFilters)), filter);
    jit.add64(scratch, filter);
    mayHaveName.append(jit.branch32(CCallHelpers::NotEqual, Address(filter, OBJECT_OFFSETOF(AbsenceFilter, structureID)), structureID));
    jit.load32(Address(instanceGPR, AOT_INSTANCE_OFFSET(propertyCaches.absenceFilterGenerationForStores)), scratch);
    mayHaveName.append(jit.branch32(CCallHelpers::NotEqual, Address(filter, OBJECT_OFFSETOF(AbsenceFilter, generationForStores)), scratch));
    jit.urshift32(name, TrustedImm32(AbsenceFilter::bitsOfWordShift), scratch);
    jit.and32(TrustedImm32(AbsenceFilter::numberOfWordsForStores - 1), scratch);
    jit.load64(BaseIndex(filter, scratch, CCallHelpers::TimesEight, OBJECT_OFFSETOF(AbsenceFilter, namesThatStopStores)), scratch);
    jit.urshift64(name, scratch);
    mayHaveName.append(jit.branchTest64(CCallHelpers::NonZero, scratch, TrustedImm32(1)));
    // What has happened to a name since the filter was made.
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(propertyCaches.namesChangedOnChains)), filter);
    jit.urshift32(name, TrustedImm32(6), scratch);
    jit.load64(BaseIndex(filter, scratch, CCallHelpers::TimesEight), scratch);
    jit.urshift64(name, scratch);
    mayHaveName.append(jit.branchTest64(CCallHelpers::NonZero, scratch, TrustedImm32(1)));

    isAbsent.link(&jit);
}

// The guard of compiled code holds once the run time has looked, and it looks for a store by name that gets there. So one such store in 16 goes there.
static void leaveNowAndThenForGuardOfStores(StubAssembler& jit, GPRReg record, GPRReg scratch, JumpList& leaves)
{
    static_assert(sizeof(PropertyCacheWords::storesToGuardedFields) == sizeof(uint8_t));
    constexpr int32_t offsetOfCounter = AOT_INSTANCE_OFFSET(propertyCaches.storesToGuardedFields);
    constexpr int32_t step = (UINT8_MAX + 1) / 16;
    Jump hasNoGuard = jit.branchTest32(CCallHelpers::Zero, Address(record), TrustedImm32(hasGuardOfStoresInFirstWord));
#if CPU(ARM64)
    jit.load8(Address(instanceGPR, offsetOfCounter), scratch);
    jit.add32(TrustedImm32(step), scratch);
    jit.store8(scratch, Address(instanceGPR, offsetOfCounter));
    leaves.append(jit.branchTest32(CCallHelpers::NonZero, scratch, TrustedImm32(UINT8_MAX + 1)));
#else
    UNUSED_PARAM(scratch);
    jit.m_assembler.addb_im(step, offsetOfCounter, instanceGPR);
    leaves.append(Jump(jit.m_assembler.jCC(X86Assembler::ConditionC)));
#endif
    hasNoGuard.link(&jit);
}

// The caller knows more: the slot lies in the cell and holds any value as it is, and the type of the field is that of any value. So nothing is asked of the value.
// A field of any unusual kind and an object that may not be stored to go on in the stub for any field, with the operands as they came.
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
    constexpr uint32_t flagsOfUnusualFields = fieldAttributeFlags | fieldIsPrivate | fieldIsBrand | fieldIsOutOfLine | fieldMayExistWithoutStorage;
    JumpList isLeftToStubForAnyField;
    JumpList hasNoTransition;

    loadStructure(jit, object, structure);
    jit.loadPtr(Address(structure, CompiledStructure::offsetOfLayout()), record);
    jit.addPtr(offsetOfRecord, record);
    isLeftToStubForAnyField.append(jit.branchTest32(CCallHelpers::NonZero, flagsOfField, TrustedImm32(flagsOfUnusualFields)));
    isLeftToStubForAnyField.append(jit.branchTest32(CCallHelpers::NonZero, orderWord, TrustedImm32(integrityBitsBlockingStore(operationFor(mode)))));
    jit.load32(Address(record, OBJECT_OFFSETOF(FieldData, slot)), offset);
    jit.and32(TrustedImm32(SlotData::maxOffset), offset);
    Jump isAbsent = jit.branchTest64(CCallHelpers::Zero, slot);
    // Object::tryStore(): the one store of a field that is stored once creates it.
    isLeftToStubForAnyField.append(jit.branchTest32(CCallHelpers::NonZero, flagsOfField, TrustedImm32(fieldIsStoredOnce)));
    jit.store64(value, slot);
    returnAfterStoring(jit, object, value);

    // The field is created, as the stub for any field creates it.
    isAbsent.link(&jit);
    isLeftToStubForAnyField.append(jit.branchTest32(CCallHelpers::NonZero, orderWord, TrustedImm32(static_cast<int32_t>(orderWordIntegrityBits | orderWordHasFieldOutsideSlot))));
    isLeftToStubForAnyField.append(jit.branchTest32(CCallHelpers::NonZero, Address(structure, Structure::bitFieldOffset()), TrustedImm32(Structure::s_mayBePrototypeBits)));
    // The key order names a field by its slot key, which for a slot in the cell is its offset.
    isLeftToStubForAnyField.append(jit.branchTest32(CCallHelpers::NonZero, offset, TrustedImm32(offsetBitsWithoutSlotKey)));
    if (mode != PutMode::Define) {
        const GPRReg nameOfField = T14;
        const GPRReg filter = jit.allocateScratch(StubRegister::A2);
        leaveNowAndThenForGuardOfStores(jit, record, T11, isLeftToStubForAnyField);
        loadNameOfField(jit, record, nameOfField);
        branchIfChainMayHaveName(jit, object, structure, nameOfField, filter, T11, isLeftToStubForAnyField);
    }
    jit.store64(value, slot);

    static_assert(sizeof(FieldData) == 1 << 4 && slotKeyFor(8, false) == 8);
    const KeyOrderRegisters keyOrder {
        .object = object,
        .slotKey = offset,
        .entry = T11,
        .words = T14,
        .scratch = structure,
    };
    const GPRReg layout = record;
    const GPRReg fieldIndex = offsetOfRecord;
    jit.subPtr(offsetOfRecord, layout);
    jit.sub32(TrustedImm32(sizeof(LayoutData)), fieldIndex);
    jit.urshift32(TrustedImm32(4), fieldIndex);
    appendFieldToKeyOrder(jit, keyOrder, layout, fieldIndex, hasNoTransition);
    returnAfterStoring(jit, object, value);

    hasNoTransition.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTDidCreateFieldAt, OperationCall {
        .returns = OperationCall::Returns::Void,
        .saves = OperationCall::Saves::CallerSavedRegisters,
        .checksException = false,
        .secondArgumentIn = keyOrder.object,
        .thirdArgumentIn = keyOrder.slotKey,
    });

    jit.linkToStub(isLeftToStubForAnyField, putKnownField(mode));
}

void generatePutKnownField(StubAssembler& jit)
{
    if (jit.key().count()) {
        generatePutKnownFieldThatTakesAnyValue(jit);
        return;
    }
    const PutMode mode = static_cast<PutMode>(jit.key().form());
    const GPRReg object = jit.operand(0);
    const GPRReg value = jit.operand(1);
    const GPRReg name = jit.immediate();
    const GPRReg T10 = jit.allocateScratch(StubRegister::T10);
    const GPRReg T11 = jit.allocateScratch(StubRegister::T11);
    const GPRReg record = jit.allocateScratch(slotHeaderRegister);
    const GPRReg structure = jit.allocateScratch(slotPayloadRegister);
    const GPRReg T14 = jit.allocateScratch(StubRegister::T14);
    const FieldStoreRegisters registers {
        .field = {
            .object = object,
            .slotData = T10,
            .extensionWordOffset = T11,
            .address = T11,
            .representation = name,
        },
        .value = value,
        .type = T14,
        .scratch = T10,
        .secondScratch = jit.allocateScratch(StubRegister::A2),
        .number = jit.allocateScratch(FPRInfo::fpRegT0),
    };
    const Address orderWord(object, offsetOfOrderWord);
    // A plain data property. What the layout gives another form is left to the run time.
    constexpr uint32_t flagsLeftToRunTime = fieldAttributeFlags | fieldIsPrivate | fieldIsBrand;
    JumpList goesByName;
    FieldData fieldWithGuard { };
    fieldWithGuard.hasGuardOfStores = 1;
    using WordsOfField = std::array<uint32_t, sizeof(FieldData) / sizeof(uint32_t)>;
    RELEASE_ASSERT(std::bit_cast<WordsOfField>(fieldWithGuard)[0] == hasGuardOfStoresInFirstWord);

    loadStructure(jit, object, structure);
    jit.loadPtr(Address(structure, CompiledStructure::offsetOfLayout()), record);
    jit.addPtr(name, record);
    goesByName.append(jit.branchTest32(CCallHelpers::NonZero, Address(record, offsetOfFlagsWord), TrustedImm32(flagsLeftToRunTime)));
    goesByName.append(jit.branchTest32(CCallHelpers::NonZero, orderWord, TrustedImm32(integrityBitsBlockingStore(operationFor(mode)))));
    Label findStorage = jit.label();
    jit.load32(Address(record, OBJECT_OFFSETOF(FieldData, slot)), registers.field.slotData);
    jit.load32(Address(record, OBJECT_OFFSETOF(FieldData, type)), registers.type);

    JumpList hasNoStorage;
    FieldStoreExits exits;
    Jump isOutOfLine = jit.branchTest32(CCallHelpers::NonZero, Address(record, offsetOfFlagsWord), TrustedImm32(fieldIsOutOfLine));
    computeFieldAddress(jit, registers.field, false, hasNoStorage);
    RELEASE_ASSERT(hasNoStorage.empty());
    Label hasAddress = jit.label();
    // Object::tryStore(): the one store of a field that is stored once creates it. A store that finds the slot filled is left to the run time, which takes the property out of its slot.
    Jump isNotStoredOnce = jit.branchTest32(CCallHelpers::Zero, Address(record, offsetOfFlagsWord), TrustedImm32(fieldIsStoredOnce));
    JumpList slotIsEmpty;
    branchOnFieldPresence(jit, registers.field, registers.scratch, goesByName, slotIsEmpty);
    slotIsEmpty.link(&jit);
    isNotStoredOnce.link(&jit);
    storeField(jit, registers, FieldStoreEnd::ReturnAfterBarrier, FieldTypeTest::IsEmitted, nullptr, exits);
    goesByName.append(exits.wasAbsentAndIsNotAccepted);
    goesByName.append(exits.cannot);

    isOutOfLine.link(&jit);
    jit.load32(Address(structure, CompiledStructure::offsetOfExtensionSlot()), registers.field.extensionWordOffset);
    jit.and32(TrustedImm32(SlotData::maxOffset), registers.field.extensionWordOffset);
    computeFieldAddress(jit, registers.field, true, hasNoStorage);
    jit.jump().linkTo(hasAddress, &jit);

    const KeyOrderRegisters keyOrder {
        .object = object,
        .slotKey = T10,
        .entry = T11,
        .words = T14,
        .scratch = name,
    };
    const OperationCall didCreateFieldCall {
        .returns = OperationCall::Returns::Void,
        .saves = OperationCall::Saves::CallerSavedRegisters,
        .checksException = false,
        .secondArgumentIn = keyOrder.object,
        .thirdArgumentIn = keyOrder.slotKey,
    };
    FieldStoreExits creationExits;
    JumpList stored;
    JumpList hasNoTransition;
    JumpList cannotAppend;

    // A field that is about to be created has no granule yet. With one it is a field that has storage and is absent.
    hasNoStorage.link(&jit);
    goesByName.append(jit.branchTest32(CCallHelpers::NonZero, orderWord, TrustedImm32(static_cast<int32_t>(orderWordIntegrityBits | orderWordHasFieldOutsideSlot))));
    // Where the key order alone says that the field exists, a field without a granule reads as undefined, so that value asks for none: the key is listed and nothing is stored.
    // The tables have a slot that does so for every field but one that is stored once.
    Jump alwaysNeedsStorage = jit.branchTest32(CCallHelpers::Zero, Address(record, offsetOfFlagsWord), TrustedImm32(fieldMayExistWithoutStorage));
    Jump isNotUndefined = jit.branch64(CCallHelpers::NotEqual, value, TrustedImm32(JSValue::ValueUndefined));
    goesByName.append(jit.branchTest32(CCallHelpers::Zero, Address(record, offsetOfFlagsWord), TrustedImm32(fieldIsStoredOnce)));
    Jump needsNoStorage = jit.jump();
    isNotUndefined.link(&jit);
    alwaysNeedsStorage.link(&jit);
    const GPRReg granule = registers.field.representation;
    const GPRReg granuleBit = registers.secondScratch;
    const GPRReg extension = registers.field.address;
    jit.and32(TrustedImm32(SlotData::maxOffset), registers.field.slotData, granule);
    jit.urshift32(TrustedImm32(3), granule);
    jit.sub32(TrustedImm32(1), granule);
    jit.move(TrustedImm32(1), granuleBit);
    jit.lshift64(granule, granuleBit);
    static_assert(granuleOf(offsetOfFirstExtensionSlot) == offsetOfFirstExtensionSlot / 8 - 1 && extensionGranuleSize == 8);
    jit.load32(Address(structure, CompiledStructure::offsetOfExtensionSlot()), registers.field.extensionWordOffset);
    jit.and32(TrustedImm32(SlotData::maxOffset), registers.field.extensionWordOffset);
    if (jit.options().useWideReferences) {
        jit.load64(BaseIndex(object, registers.field.extensionWordOffset, CCallHelpers::TimesOne), extension);
        cannotAppend.append(jit.branchTest64(CCallHelpers::Zero, extension));
    } else {
        jit.load32(BaseIndex(object, registers.field.extensionWordOffset, CCallHelpers::TimesOne), extension);
        cannotAppend.append(jit.branchTest32(CCallHelpers::Zero, extension));
        decodeCell32(jit, extension, extension);
        jit.addPtr(TrustedImm32(offsetOfEmptyExtensionInCage), extension);
    }
    appendGranule(jit, extension, granuleBit, granule, jit.scratchRegister(), cannotAppend);
    jit.jump().linkTo(findStorage, &jit);

    cannotAppend.link(&jit);
    jit.preservingCallerSavedRegisters(false, [&](unsigned) {
        const GPRReg A0 = jit.reuseScratch(StubRegister::A0);
        const GPRReg A1 = jit.reuseScratch(StubRegister::A1);
        const GPRReg A2 = jit.reuseScratch(StubRegister::A2);
        RELEASE_ASSERT(object != A2 && noOverlap(instanceGPR, A1, A2));
        jit.move(granuleBit, A2);
        jit.move(object, A1);
        jit.move(instanceGPR, A0);
        jit.callOperation(Entry::operationAOTEnsureExtensionGranules, jit.reuseScratch(StubRegister::Operation));
    });
    jit.jump().linkTo(findStorage, &jit);

    // While no field lies outside its slot an empty slot says that the object has no such property. What is cached about the objects below a prototype relies on the run time's report of a field that the prototype gets.
    auto leaveUnlessFieldIsCreatedHere = [&] {
        goesByName.append(jit.branchTest32(CCallHelpers::NonZero, orderWord, TrustedImm32(static_cast<int32_t>(orderWordIntegrityBits | orderWordHasFieldOutsideSlot))));
        goesByName.append(jit.branchTest32(CCallHelpers::NonZero, Address(structure, Structure::bitFieldOffset()), TrustedImm32(Structure::s_mayBePrototypeBits)));
        // The key order names a field by its slot key.
        goesByName.append(jit.branchTest32(CCallHelpers::NonZero, Address(record, OBJECT_OFFSETOF(FieldData, slot)), TrustedImm32(offsetBitsWithoutSlotKey)));
        if (mode != PutMode::Define) {
            // The guard of compiled code holds once the run time has looked, and it looks for a store by name that gets there. So one such store in 16 goes there: code that comes again and again gets its guard, and code that runs a few times pays for no look.
            static_assert(sizeof(PropertyCacheWords::storesToGuardedFields) == sizeof(uint8_t));
            constexpr int32_t offsetOfCounter = AOT_INSTANCE_OFFSET(propertyCaches.storesToGuardedFields);
            constexpr int32_t step = (UINT8_MAX + 1) / 16;
            Jump hasNoGuard = jit.branchTest32(CCallHelpers::Zero, Address(record), TrustedImm32(hasGuardOfStoresInFirstWord));
#if CPU(ARM64)
            jit.load8(Address(instanceGPR, offsetOfCounter), registers.scratch);
            jit.add32(TrustedImm32(step), registers.scratch);
            jit.store8(registers.scratch, Address(instanceGPR, offsetOfCounter));
            goesByName.append(jit.branchTest32(CCallHelpers::NonZero, registers.scratch, TrustedImm32(UINT8_MAX + 1)));
#else
            jit.m_assembler.addb_im(step, offsetOfCounter, instanceGPR);
            goesByName.append(Jump(jit.m_assembler.jCC(X86Assembler::ConditionC)));
#endif
            hasNoGuard.link(&jit);
            // The type has passed, and the store below takes its register for a scratch register.
            const GPRReg nameOfField = registers.type;
            loadNameOfField(jit, record, nameOfField);
            branchIfChainMayHaveName(jit, object, structure, nameOfField, registers.secondScratch, registers.scratch, goesByName);
        }
    };

    // RuntimeLayoutTable::accept() and Object::store(): the type has to take undefined, and for a field whose raw value the mark of the cell answers for the run time sets the mark.
    needsNoStorage.link(&jit);
    FieldData fieldWithRawValue { };
    fieldWithRawValue.holdsRawValueFromBirth = 1;
    goesByName.append(jit.branchTest32(CCallHelpers::NonZero, Address(record), TrustedImm32(static_cast<int32_t>(std::bit_cast<WordsOfField>(fieldWithRawValue)[0]))));
    branchUnlessTypeAccepts(jit, { value, registers.type, registers.field.representation, registers.scratch, registers.secondScratch }, goesByName, goesByName);
    leaveUnlessFieldIsCreatedHere();
    stored.append(jit.jump());

    exits.wasAbsent.link(&jit);
    Label createsField = jit.label();
    leaveUnlessFieldIsCreatedHere();
    storeField(jit, registers, FieldStoreEnd::JoinList, FieldTypeTest::HasPassed, &stored, creationExits);
    RELEASE_ASSERT(creationExits.wasAbsent.empty() && creationExits.wasAbsentAndIsNotAccepted.empty());
    goesByName.append(creationExits.cannot);

    stored.link(&jit);
    jit.load32(Address(record, OBJECT_OFFSETOF(FieldData, slot)), keyOrder.slotKey);
    jit.and32(TrustedImm32(SlotData::maxOffset), keyOrder.slotKey);
    Jump isInCell = jit.branchTest32(CCallHelpers::Zero, Address(record, offsetOfFlagsWord), TrustedImm32(fieldIsOutOfLine));
    jit.or32(TrustedImm32(outOfLineSlotKey), keyOrder.slotKey);
    isInCell.link(&jit);
    appendFieldToKeyOrder(jit, keyOrder, hasNoTransition);
    returnAfterStoring(jit, object, value);

    hasNoTransition.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTDidCreateFieldAt, didCreateFieldCall);

    // The caller has seen that the slot, which lies in the cell, is empty, and that the field takes the value: its type does and its slot does.
    jit.bindEntry(createKnownField(mode));
    loadStructure(jit, object, structure);
    jit.loadPtr(Address(structure, CompiledStructure::offsetOfLayout()), record);
    jit.addPtr(name, record);
    goesByName.append(jit.branchTest32(CCallHelpers::NonZero, Address(record, offsetOfFlagsWord), TrustedImm32(flagsLeftToRunTime | fieldIsOutOfLine | fieldIsStoredOnce)));
    jit.load32(Address(record, OBJECT_OFFSETOF(FieldData, slot)), registers.field.slotData);
    computeFieldAddress(jit, registers.field, false, hasNoStorage);
    jit.jump().linkTo(createsField, &jit);

    goesByName.link(&jit);
    loadNameOfField(jit, record, name);
    jit.linkToStub(jit.branch32(CCallHelpers::Above, name, TrustedImm32(RowDisplacementTable::maxColumn)), putByIdHashed(mode));
    jit.jumpToStub(putByIdBehindFieldTable(mode));
}

// A store by name in code that runs once. What a table remembers pays where the access comes again, so only the table of fields is asked, which is there from the start.
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
    storeFieldByTable(jit, { jit.operand(0), jit.immediate(), index, entries, std::nullopt, true, false }, jit.operand(1), scratch, operationFor(mode), isNotInFieldTable);
    linkToSlotExit(jit, isNotInFieldTable, table, SlotExit::Uncached);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
