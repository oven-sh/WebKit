/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsPropertyProbes.h"

#if ENABLE(AOT)

#include "AOTArrayAllocationProfile.h"
#include "AOTCellFormat.h"
#include "AOTFunctionTables.h"
#include "AOTInstance.h"
#include "AOTObjectWords.h"
#include "AOTRuntimeTable.h"
#include "AOTStubsBarriers.h"
#include "AOTStubsField.h"
#include "AOTStubsKeyed.h"
#include "AOTStubsPropertyAccessors.h"
#include "AOTStubsPropertyExtraProperties.h"
#include "AOTThreshold.h"
#include "Butterfly.h"
#include "IndexingType.h"
#include "JSFunction.h"
#include "JSGlobalObject.h"
#include "JSGlobalProxy.h"
#include "JSObject.h"
#include "JSType.h"
#include "PureNaN.h"
#include <bit>
#include <wtf/MathExtras.h>

namespace JSC { namespace AOT {

static constexpr unsigned exitNameOperand = 1;
static constexpr unsigned exitValueOperand = 2;
static constexpr unsigned exitSiteOperand = 3;

static_assert(hasOneBitSet(outOfLineSlotKey) && outOfLineSlotKey <= SlotData::maxOffset);
static_assert(slotKeyFor(outOfLineSlotKey - 1, false) != noSlotKey && slotKeyFor(outOfLineSlotKey, false) == noSlotKey);
static_assert(slotKeyFor(1, true) == (1 | outOfLineSlotKey));
static_assert(operationArgumentGPR(4) == gprOf(StubRegister::A4) && operationArgumentGPR(5) == gprOf(StubRegister::A5));
static_assert(storedValueByIdRegister != StubRegister::A2 && slotNameRegister == StubRegister::T9);

StubSignature signatureOfStoreSlot(StubKey key)
{
    return signatureOfSlotHandler(handlerTableOf(key));
}

// storeField takes every general-purpose scratch register of x86-64, the name's among them, and the Uncached exit takes the name.
struct FieldStoreArm {
    FieldStoreRegisters registers;
    FPRReg savedName;
    JumpList uncached;
};

static Address closureExecutable(FunctionTables::ExecutableKind kind)
{
    return Address(instanceGPR, static_cast<int32_t>(AOT_INSTANCE_OFFSET(closures.executables) + static_cast<unsigned>(kind) * sizeof(JSCell*)));
}

// No StructureID is compared at a free-name site, so its tables have an arm only for the kinds that are filed there.
static bool hasStoreArm(SlotHandlerTableTraits traits, Slot::Kind kind)
{
    const bool isDefinition = traits.operation == SlotOperation::Define;
    const bool isPacked = traits.form == SlotForm::PackedSlot;
    const bool isFreeName = traits.origin == SlotOrigin::FreeName;
    switch (kind) {
    case Slot::Kind::Empty:
    case Slot::Kind::Absent:
    case Slot::Kind::Getter:
    case Slot::Kind::CustomGetter:
    case Slot::Kind::FieldGetter:
    case Slot::Kind::FieldOrInherited:
    case Slot::Kind::InheritedField:
    case Slot::Kind::FieldOrInheritedField:
    case Slot::Kind::ExtraGetter:
    case Slot::Kind::TypedArrayLength:
    case Slot::Kind::StringLength:
    case Slot::Kind::ClosureLength:
    case Slot::Kind::ClosureName:
    case Slot::Kind::Constant:
    case Slot::Kind::Removal:
    case Slot::Kind::ExtraPropertyRemoval:
    case Slot::Kind::FieldRemoval:
    case Slot::Kind::NothingToRemove:
        return false;
    case Slot::Kind::Value:
    case Slot::Kind::Uncacheable:
        return true;
    case Slot::Kind::Setter:
        return !isDefinition;
    case Slot::Kind::CustomSetter:
    case Slot::Kind::ExtraSetter:
    case Slot::Kind::ArrayLength:
        return !isDefinition && !isFreeName;
    case Slot::Kind::ExtraProperty:
    case Slot::Kind::Transition:
        return !isFreeName;
    case Slot::Kind::FarValue:
    case Slot::Kind::Field:
    case Slot::Kind::FieldCreation:
    case Slot::Kind::ExtraPropertyAddition:
        return !isPacked && !isFreeName;
    case Slot::Kind::FieldOrInheritedSetter:
    case Slot::Kind::Variable:
        return !isDefinition && !isPacked && !isFreeName;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void generateStoreSlot(StubAssembler& jit)
{
    const SlotHandlerTable table = handlerTableOf(jit.key());
    const SlotHandlerTableTraits traits = traitsOf(table);
    const SlotOrigin origin = traits.origin;
    const SlotHandlerOperands operands = operandsOfSlotHandler(table);
    const unsigned nameOperand = traits.form == SlotForm::Slot ? payloadOperand + 1 : payloadOperand;
    const GPRReg receiver = operands.receiver;
    const GPRReg value = operands.value;
    const GPRReg name = origin == SlotOrigin::ByVal ? operands.keyCell : operands.identifierNumber;
    RELEASE_ASSERT(jit.operand(receiverOperand) == receiver && jit.operand(headerOperand) == operands.header);
    RELEASE_ASSERT(traits.form == SlotForm::PackedSlot || jit.operand(payloadOperand) == operands.payload);
    RELEASE_ASSERT(jit.operand(nameOperand) == name);
    RELEASE_ASSERT(signatureOfWriteBarrier(writeBarrier()).operands[0] == Reg(receiver));
    const CachedSlot slot(traits.form, slotKindOf(jit.key()), operands.header, operands.payload);
    const GPRReg T10 = jit.allocateScratch(StubRegister::T10);
    const GPRReg T11 = jit.allocateScratch(StubRegister::T11);
    const GPRReg T14 = jit.allocateScratch(StubRegister::T14);
    const ExtraPropertyRegisters extraPropertyRegisters { .object = receiver, .extraProperties = T10, .wordIndex = T11, .scratch = T14 };
    JumpList miss;
    JumpList uncached;

    auto clobberNameRegister = [&] {
        return jit.clobberOperand(nameOperand);
    };
    auto allocateSpareRegister = [&] {
        RELEASE_ASSERT(origin == SlotOrigin::ById || origin == SlotOrigin::ByVal);
        return jit.allocateScratch(origin == SlotOrigin::ById ? StubRegister::A2 : StubRegister::T9);
    };
    auto storeInStorageOf = [&](GPRReg owner, GPRReg wordIndex) {
        const GPRReg storage = T14;
        RELEASE_ASSERT(noOverlap(owner, wordIndex, storage, value));
        jit.move(owner, storage);
        Jump isInline = slot.branchUnlessFlag(jit, Slot::isOutOfLine);
        jit.loadPtr(Address(owner, JSObject::butterflyOffset()), storage);
        isInline.link(&jit);
        jit.store64(value, BaseIndex(storage, wordIndex, CCallHelpers::TimesEight));
    };
    auto callSetter = [&](GPRReg getterSetter) {
        const SlotOperation operation = traits.operation;
        RELEASE_ASSERT(operation == SlotOperation::StoreStrict || operation == SlotOperation::StoreSloppy);
        const SlotFinisher finisher = operation == SlotOperation::StoreStrict ? SlotFinisher::CallSetterStrict : SlotFinisher::CallSetterSloppy;
        const StubSignature finisherSignature = signatureOfSlotCannot(slotCannot(finisher, origin));
        RELEASE_ASSERT(finisherSignature.operands[finisherSignature.numberOfOperands - 2] == Reg(getterSetter));
        JumpList cannot;
        callSetterAndReturn(jit, receiver, value, getterSetter, cannot);
        linkToSlotCannot(jit, cannot, table, finisher, slot);
    };
    auto createFieldStoreArm = [&](GPRReg object) -> FieldStoreArm {
        return {
            .registers = {
                .field = { .object = object, .slotData = T10, .extensionWordOffset = T11, .address = T11, .representation = clobberNameRegister() },
                .value = value,
                .type = T14,
                .scratch = T10,
                .secondScratch = allocateSpareRegister(),
                .number = jit.allocateScratch(FPRInfo::fpRegT0),
            },
            .savedName = jit.allocateScratch(FPRInfo::fpRegT1),
            .uncached = { },
        };
    };
    auto linkToUncachedExit = [&](FieldStoreArm& arm) {
        if (arm.uncached.empty())
            return;
        arm.uncached.link(&jit);
        jit.moveDoubleTo64(arm.savedName, name);
        uncached.append(jit.jump());
    };
    auto emitFieldCreation = [&](FieldStoreArm& arm, FieldStoreExits& storeExits, JumpList& hasNoStorage, Label findStorage) {
        const FieldStoreRegisters& r = arm.registers;
        const Address orderWord(receiver, offsetOfOrderWord);
        const TrustedImm32 integrityBits(static_cast<int32_t>(orderWordIntegrityBits));
        const KeyOrderRegisters keyOrder { .object = receiver, .slotKey = r.scratch, .entry = r.field.address, .words = r.type, .scratch = r.field.representation };
        FieldStoreExits exits;
        JumpList stored;

        // A field that is about to be created has no granule yet. With one it is a field that has storage and is absent.
        hasNoStorage.link(&jit);
        arm.uncached.append(jit.branchTest32(CCallHelpers::NonZero, orderWord, integrityBits));
        // Where the key order alone says that the field exists, a field without a granule reads as undefined, so that value asks for none.
        Jump alwaysNeedsStorage = slot.branchUnlessFlag(jit, Slot::undefinedNeedsNoStorage);
        Jump needsNoStorage = jit.branch64(CCallHelpers::Equal, value, TrustedImm32(JSValue::ValueUndefined));
        alwaysNeedsStorage.link(&jit);
        createGranuleOfField(jit, r, [&] {
            slot.loadHiddenWordOffset(jit, r.field.extensionWordOffset);
        }, findStorage);

        needsNoStorage.link(&jit);
        arm.uncached.append(jit.branchTest32(CCallHelpers::NonZero, r.field.slotData, TrustedImm32(offsetBitsWithoutSlotKey)));
        stored.append(jit.jump());

        storeExits.wasAbsentAndIsNotAccepted.link(&jit);
        arm.uncached.append(jit.branchTest32(CCallHelpers::NonZero, orderWord, integrityBits));
        storeExits.cannot.append(jit.jump());

        storeExits.wasAbsent.link(&jit);
        arm.uncached.append(jit.branchTest32(CCallHelpers::NonZero, orderWord, integrityBits));
        // The key order names a field by its slot key.
        slot.loadFirstPayloadWord(jit, keyOrder.slotKey);
        arm.uncached.append(jit.branchTest32(CCallHelpers::NonZero, keyOrder.slotKey, TrustedImm32(offsetBitsWithoutSlotKey)));
        storeField(jit, r, FieldStoreEnd::JoinList, FieldTypeTest::HasPassed, &stored, exits);
        RELEASE_ASSERT(exits.wasAbsent.empty() && exits.wasAbsentAndIsNotAccepted.empty());
        storeExits.cannot.append(exits.cannot);

        stored.link(&jit);
        slot.loadFirstPayloadWord(jit, keyOrder.slotKey);
        jit.and32(TrustedImm32(SlotData::maxOffset), keyOrder.slotKey);
        Jump isInline = slot.branchUnlessFlag(jit, Slot::isOutOfLine);
        jit.or32(TrustedImm32(outOfLineSlotKey), keyOrder.slotKey);
        isInline.link(&jit);
        listCreatedFieldAndReturn(jit, keyOrder, value);
    };
    auto finishExtraPropertyStore = [&](GPRReg owner, bool valueMayNeedBarrier) {
        if (valueMayNeedBarrier)
            returnAfterStoring(jit, owner, value);
        else
            jit.returnToCaller();
    };
    auto emitAdditionInStorage = [&] {
        const GPRReg newStructureID = T10;
        const GPRReg wordIndex = T11;
        slot.loadFirstPayloadWord(jit, newStructureID);
        slot.loadOwnWordIndex(jit, wordIndex);
        storeInStorageOf(receiver, wordIndex);
        jit.store32(newStructureID, Address(receiver, JSCell::structureIDOffset()));
        jit.jumpToStub(writeBarrier());
    };
    // Functions of every kind share a Structure, and only these kinds have an ordinary [[Set]].
    auto branchUnlessOrdinaryFunction = [&](GPRReg executable) {
        Jump isNotFunction = jit.branchIfNotType(receiver, jsFunctionTypes);
        jit.loadPtr(Address(receiver, JSFunction::offsetOfExecutableOrRareData()), executable);
        Jump isPlainFunction = jit.branchPtr(CCallHelpers::Equal, executable, closureExecutable(FunctionTables::ExecutableKind::Constructible));
        Jump isArrowFunction = jit.branchPtr(CCallHelpers::Equal, executable, closureExecutable(FunctionTables::ExecutableKind::NotConstructible));
        uncached.append(jit.branchPtr(CCallHelpers::NotEqual, executable, Address(instanceGPR, AOT_INSTANCE_OFFSET(realm.boundFunctionExecutable))));
        isArrowFunction.link(&jit);
        isPlainFunction.link(&jit);
        isNotFunction.link(&jit);
    };

    // A kind that has no arm in this table misses.
    switch (hasStoreArm(traits, slot.kind()) ? slot.kind() : Slot::Kind::Empty) {
    case Slot::Kind::Value: {
        const bool isInherited = origin == SlotOrigin::FreeName;
        const GPRReg owner = isInherited ? T10 : receiver;
        const GPRReg wordIndex = T11;
        if (isInherited) {
            slot.loadHolder(jit, owner);
            slot.loadInheritedWordIndex(jit, wordIndex);
        } else
            slot.loadOwnWordIndex(jit, wordIndex);
        storeInStorageOf(owner, wordIndex);
        returnAfterStoring(jit, owner, value);
        break;
    }
    case Slot::Kind::FarValue: {
        const GPRReg butterfly = T10;
        const GPRReg wordIndex = T11;
        slot.loadOwnWordIndex(jit, wordIndex);
        jit.loadPtr(Address(receiver, JSObject::butterflyOffset()), butterfly);
        jit.store64(value, BaseIndex(butterfly, wordIndex, CCallHelpers::TimesEight));
        returnAfterStoring(jit, receiver, value);
        break;
    }
    case Slot::Kind::Setter:
        loadValueBySlot(jit, slot, receiver, T10, T11, T10);
        callSetter(T10);
        break;
    case Slot::Kind::CustomSetter: {
        const GPRReg accessorIndex = traits.form == SlotForm::Slot ? jit.clobberOperand(payloadOperand) : jit.allocateScratch(slotPayloadRegister);
        const GPRReg uid = T14;
        if (origin == SlotOrigin::ByVal)
            loadNameOfKey(jit, name, uid, T11, miss);
        else {
            RELEASE_ASSERT(origin == SlotOrigin::ById || origin == SlotOrigin::FreeName);
            loadIdentifier(jit, name, uid, miss);
        }
        slot.loadFirstPayloadWord(jit, accessorIndex);
        callCustomSetterAndReturn(jit, receiver, value, accessorIndex, uid, origin == SlotOrigin::ByVal ? std::optional<GPRReg>(name) : std::nullopt);
        break;
    }
    case Slot::Kind::Field:
    case Slot::Kind::FieldCreation: {
        const bool createsField = slot.kind() == Slot::Kind::FieldCreation;
        FieldStoreArm arm = createFieldStoreArm(receiver);
        const FieldStoreRegisters& r = arm.registers;
        FieldStoreExits exits;
        JumpList hasNoStorage;

        uncached.append(jit.branchTest32(CCallHelpers::NonZero, Address(receiver, offsetOfOrderWord), TrustedImm32(integrityBitsBlockingStore(traits.operation))));
        jit.move64ToDouble(name, arm.savedName);
        Label findStorage = jit.label();
        slot.loadFirstPayloadWord(jit, r.field.slotData);
        slot.loadSecondPayloadWord(jit, r.type);
        Jump isOutOfLine = slot.branchIfFlag(jit, Slot::isOutOfLine);
        computeFieldAddress(jit, r.field, false, hasNoStorage);
        // Slot::remembersOrder: nothing is asked of the value.
        if (createsField && slot.form() == SlotForm::Slot) {
            const GPRReg orderWordBefore = r.type;
            const GPRReg orderAfter = r.scratch;
            const Address valueInSlot(r.field.address);
            const Address orderWord(receiver, offsetOfOrderWord);
            Jump remembersNothing = slot.branchUnlessFlag(jit, Slot::remembersOrder);
            Jump isAbsent = jit.branchTest64(CCallHelpers::Zero, valueInSlot);
            jit.store64(value, valueInSlot);
            returnAfterStoring(jit, receiver, value);

            isAbsent.link(&jit);
            exits.wasAbsent.append(jit.branch32(CCallHelpers::NotEqual, orderWord, orderWordBefore));
            jit.store64(value, valueInSlot);
            slot.loadHiddenWordOffset(jit, orderAfter);
            jit.add32(TrustedImm32(1 << orderWordCountShift), orderWordBefore);
            jit.and32(TrustedImm32(static_cast<int32_t>(~orderWordOrderMask)), orderWordBefore);
            jit.or32(orderAfter, orderWordBefore);
            jit.store32(orderWordBefore, orderWord);
            returnAfterStoring(jit, receiver, value);
            remembersNothing.link(&jit);
        }
        Label hasAddress = jit.label();
        storeField(jit, r, FieldStoreEnd::ReturnAfterBarrier, FieldTypeTest::IsEmittedForSlot, nullptr, exits);

        isOutOfLine.link(&jit);
        slot.loadHiddenWordOffset(jit, r.field.extensionWordOffset);
        computeFieldAddress(jit, r.field, true, hasNoStorage);
        jit.jump().linkTo(hasAddress, &jit);

        if (createsField)
            emitFieldCreation(arm, exits, hasNoStorage, findStorage);
        else {
            arm.uncached.append(exits.wasAbsent);
            arm.uncached.append(exits.wasAbsentAndIsNotAccepted);
            arm.uncached.append(hasNoStorage);
        }
        linkToSlotCannot(jit, exits.cannot, table, createsField ? SlotFinisher::CreateField : SlotFinisher::StoreField, slot);
        linkToUncachedExit(arm);
        break;
    }
    // The record has no room for the type of the field, so a store to a filled slot is left to the run time.
    case Slot::Kind::FieldOrInheritedSetter: {
        const FieldAddressRegisters field { .object = receiver, .slotData = T10, .extensionWordOffset = T11, .address = T11, .representation = T14 };
        const GPRReg getterSetter = T10;
        JumpList isAbsent;

        slot.loadFirstPayloadWord(jit, field.slotData);
        Jump isOutOfLine = slot.branchIfFlag(jit, Slot::isOutOfLine);
        computeFieldAddress(jit, field, false, isAbsent);
        Label hasAddress = jit.label();
        branchOnFieldPresence(jit, field, T10, uncached, isAbsent);

        isOutOfLine.link(&jit);
        loadExtensionWordOffset(jit, receiver, field.extensionWordOffset);
        computeFieldAddress(jit, field, true, isAbsent);
        jit.jump().linkTo(hasAddress, &jit);

        isAbsent.link(&jit);
        loadInheritedValueBySlot(jit, slot, T10, T11, getterSetter);
        callSetter(getterSetter);
        break;
    }
    case Slot::Kind::Variable: {
        const GPRReg statics = jit.clobberOperand(headerOperand);
        FieldStoreArm arm = createFieldStoreArm(statics);
        const FieldStoreRegisters& r = arm.registers;
        const GPRReg roundedSizeOfStatics = r.scratch;
        const GPRReg offsetInStatics = r.field.address;
        FieldStoreExits exits;
        JumpList hasNoStorage;
        JumpList isPresent;
        JumpList stored;

        jit.move64ToDouble(name, arm.savedName);
        loadStaticsBase(jit, statics, roundedSizeOfStatics);
        slot.loadFirstPayloadWord(jit, r.field.slotData);
        slot.loadSecondPayloadWord(jit, r.type);
        computeFieldAddress(jit, r.field, false, hasNoStorage);
        RELEASE_ASSERT(hasNoStorage.empty());
        branchOnFieldPresence(jit, r.field, r.scratch, isPresent, arm.uncached);

        isPresent.link(&jit);
        storeField(jit, r, FieldStoreEnd::JoinList, FieldTypeTest::IsEmitted, &stored, exits);
        RELEASE_ASSERT(exits.wasAbsent.empty() && exits.wasAbsentAndIsNotAccepted.empty());
        arm.uncached.append(exits.cannot);

        stored.link(&jit);
        loadStaticsBase(jit, statics, roundedSizeOfStatics);
        slot.loadFirstPayloadWord(jit, offsetInStatics);
        jit.and32(TrustedImm32(SlotData::maxOffset), offsetInStatics);
        markStaticsCard(jit, statics, roundedSizeOfStatics, offsetInStatics);
        jit.returnToCaller();

        linkToUncachedExit(arm);
        break;
    }
    case Slot::Kind::ExtraProperty:
        if (origin == SlotOrigin::ByVal && accessOf(traits.operation) == SlotAccess::Store) {
            jit.load32(Address(receiver, JSCell::structureIDOffset()), T11);
            rememberPlaceOfExtraProperty(jit, SlotAccess::Store, slot, receiver, operands.keyCell, T10, T11);
        }
        storeExtraProperty(jit, slot, extraPropertyRegisters, value, finishExtraPropertyStore, miss);
        break;
    case Slot::Kind::ExtraSetter:
        locateExtraProperty(jit, slot, extraPropertyRegisters, miss);
        jit.load64(BaseIndex(extraPropertyRegisters.extraProperties, extraPropertyRegisters.wordIndex, CCallHelpers::TimesEight), T10);
        callSetter(T10);
        break;
    case Slot::Kind::ExtraPropertyAddition: {
        ExtraPropertyAdditionExits exits;
        JumpList cannotGrow;
        addExtraProperty(jit, slot, extraPropertyRegisters, value, finishExtraPropertyStore, exits);
        uncached.append(exits.isNotExtensible);

        // The growth takes every general-purpose scratch register of x86-64, the header's among them, and the finisher takes the header.
        exits.growsStorage.link(&jit);
        const GPRReg header = jit.clobberOperand(headerOperand);
        const FPRReg savedHeader = jit.allocateScratch(FPRInfo::fpRegT0);
        jit.move64ToDouble(header, savedHeader);
        addExtraPropertyInNewStorage(jit, slot, {
            .access = extraPropertyRegisters,
            .oldCapacity = allocateSpareRegister(),
            .storage = clobberNameRegister(),
            .scratch = header,
        }, value, finishExtraPropertyStore, cannotGrow);

        cannotGrow.link(&jit);
        jit.moveDoubleTo64(savedHeader, header);
        exits.cannot.append(jit.jump());
        linkToSlotCannot(jit, exits.cannot, table, SlotFinisher::GrowStorage, slot);
        break;
    }
    case Slot::Kind::Transition: {
        Jump growsStorage = slot.branchIfFlag(jit, Slot::growsStorage);
        emitAdditionInStorage();

        growsStorage.link(&jit);
        const GPRReg newStructureID = T10;
        const GPRReg oldCapacity = T11;
        const GPRReg newCapacity = T14;
        const Address butterfly(receiver, JSObject::butterflyOffset());
        JumpList cannot;

        // No packed slot has the flag.
        Jump hasIndexingHeader;
        if (traits.form == SlotForm::Slot)
            hasIndexingHeader = slot.branchIfFlag(jit, Slot::storageHasIndexingHeader);
        slot.loadSecondPayloadWord(jit, newCapacity);
        Jump isFirstStorage = jit.branch32(CCallHelpers::Equal, newCapacity, TrustedImm32(initialOutOfLineCapacity));
        cannot.append(jit.branchTestPtr(CCallHelpers::Zero, butterfly));
        jit.urshift32(newCapacity, TrustedImm32(1), oldCapacity);
        Jump hasCapacities = jit.jump();

        isFirstStorage.link(&jit);
        cannot.append(jit.branchTestPtr(CCallHelpers::NonZero, butterfly));
        branchUnlessOrdinaryFunction(newStructureID);
        jit.move(TrustedImm32(0), oldCapacity);

        hasCapacities.link(&jit);
        slot.loadFirstPayloadWord(jit, newStructureID);
        growOutOfLineStorage(jit, {
            .object = receiver,
            .newStructureID = newStructureID,
            .oldCapacity = oldCapacity,
            .newCapacity = newCapacity,
            .storage = clobberNameRegister(),
            .scratch = allocateSpareRegister(),
            .addedValue = value,
        }, cannot);
        jit.jumpToStub(writeBarrier());

        // An array that was born with room for its first named properties has the new storage already: the room lies in front of its indexing header.
        if (hasIndexingHeader.isSet()) {
            const GPRReg distanceToButterfly = oldCapacity;
            hasIndexingHeader.link(&jit);
            slot.loadSecondPayloadWord(jit, newCapacity);
            cannot.append(jit.branch32(CCallHelpers::NotEqual, newCapacity, TrustedImm32(initialOutOfLineCapacity)));
            jit.loadPtr(butterfly, distanceToButterfly);
            jit.subPtr(receiver, distanceToButterfly);
            cannot.append(jit.branch64(CCallHelpers::NotEqual, distanceToButterfly, TrustedImm32(static_cast<int32_t>(sizeOfArrayWithStorage(0, propertyRoomOfArray)))));
            emitAdditionInStorage();
        }

        linkToSlotCannot(jit, cannot, table, SlotFinisher::GrowStorage, slot);
        break;
    }
    case Slot::Kind::ArrayLength: {
        AOT_THRESHOLD(unsigned, maxElementsClearedInStub, 64);
        JumpList& cannot = uncached;
        const GPRReg array = receiver;
        const GPRReg newLength = T10;
        const GPRReg butterfly = T11;
        const GPRReg length = jit.clobberOperand(headerOperand);
        const GPRReg hole = T14;

        cannot.append(jit.branchIfNotInt32(value));
        jit.zeroExtend32ToWord(value, newLength);
        cannot.append(jit.branch32(CCallHelpers::LessThan, newLength, TrustedImm32(0)));
        jit.loadPtr(Address(array, JSObject::butterflyOffset()), butterfly);
        jit.load32(Address(butterfly, Butterfly::offsetOfPublicLength()), length);
        Jump isShorter = jit.branch32(CCallHelpers::Below, newLength, length);
        cannot.append(jit.branch32(CCallHelpers::Above, newLength, Address(butterfly, Butterfly::offsetOfVectorLength())));
        jit.store32(newLength, Address(butterfly, Butterfly::offsetOfPublicLength()));
        jit.returnToCaller();

        isShorter.link(&jit);
        jit.sub32(length, newLength, hole);
        cannot.append(jit.branch32(CCallHelpers::Above, hole, TrustedImm32(maxElementsClearedInStub)));
        jit.load8(Address(array, JSCell::indexingTypeAndMiscOffset()), hole);
        jit.and32(TrustedImm32(IndexingShapeMask), hole);
        Jump hasDoubles = jit.branch32(CCallHelpers::Equal, hole, TrustedImm32(DoubleShape));
        jit.move(TrustedImm32(0), hole);
        Jump hasHole = jit.jump();
        hasDoubles.link(&jit);
        jit.move(TrustedImm64(std::bit_cast<int64_t>(PNaN)), hole);
        hasHole.link(&jit);
        Label clears = jit.label();
        jit.sub32(TrustedImm32(1), length);
        jit.store64(hole, BaseIndex(butterfly, length, CCallHelpers::TimesEight));
        jit.branch32(CCallHelpers::Above, length, newLength).linkTo(clears, &jit);
        jit.store32(newLength, Address(butterfly, Butterfly::offsetOfPublicLength()));
        jit.returnToCaller();
        break;
    }
    case Slot::Kind::Uncacheable:
        if (jit.options().callsTrapsOfProxies.predicted() && slot.form() == SlotForm::Slot && traits.operation != SlotOperation::Define && (origin == SlotOrigin::ById || origin == SlotOrigin::ByVal)) {
            static_assert(valueStoredThroughProxyRegister == StubRegister::T10);
            uncached.append(jit.branchTest64(CCallHelpers::Zero, operands.payload));
            jit.move(value, T10);
            jit.move(TrustedImm32(traits.operation == SlotOperation::StoreStrict), T14);
            jit.jumpToStub(throughProxy(TrapOfProxy::Set));
            break;
        }
        uncached.append(jit.jump());
        break;
    case Slot::Kind::Empty:
    case Slot::Kind::Absent:
    case Slot::Kind::Getter:
    case Slot::Kind::CustomGetter:
    case Slot::Kind::FieldGetter:
    case Slot::Kind::FieldOrInherited:
    case Slot::Kind::InheritedField:
    case Slot::Kind::FieldOrInheritedField:
    case Slot::Kind::ExtraGetter:
    case Slot::Kind::TypedArrayLength:
    case Slot::Kind::StringLength:
    case Slot::Kind::ClosureLength:
    case Slot::Kind::ClosureName:
    case Slot::Kind::Constant:
    case Slot::Kind::Removal:
    case Slot::Kind::ExtraPropertyRemoval:
    case Slot::Kind::FieldRemoval:
    case Slot::Kind::NothingToRemove:
        RELEASE_ASSERT(!hasStoreArm(traits, slot.kind()));
        miss.append(jit.jump());
        break;
    }
    linkToSlotExit(jit, miss, table, SlotExit::Miss);
    linkToSlotExit(jit, uncached, table, SlotExit::Uncached);
}

struct StoreSlotExitTraits {
    SlotHandlerTable table;
    SlotExit exit;
};

static StoreSlotExitTraits traitsOfStoreSlotExit(StubKey key)
{
    for (SlotHandlerTable table : allSlotHandlerTables) {
        if (isReadTable(table) || traitsOf(table).form != SlotForm::Slot)
            continue;
        for (SlotExit exit : { SlotExit::Miss, SlotExit::Uncached }) {
            if (slotExit(table, exit) == key)
                return { table, exit };
        }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

StubSignature signatureOfStoreSlotExit(StubKey key)
{
    const SlotHandlerTable table = traitsOfStoreSlotExit(key).table;
    const SlotHandlerOperands operands = operandsOfSlotHandler(table);
    const StubSignature handler = signatureOfSlotHandler(table);
    StubSignature signature;
    for (unsigned index = 0; index < handler.numberOfOperands; ++index) {
        const Reg operand = handler.operands[index];
        if (operand != Reg(operands.header) && operand != Reg(operands.payload))
            signature.addOperand(operand);
    }
    return signature;
}

static OperationCall createExitCall(GPRReg second, GPRReg third, GPRReg fourth)
{
    return {
        .returns = OperationCall::Returns::Void,
        .saves = OperationCall::Saves::CallerSavedRegisters,
        .secondArgumentIn = second,
        .thirdArgumentIn = third,
        .fourthArgumentIn = fourth,
    };
}

static_assert(!static_cast<size_t>(PutMode::Strict) && static_cast<size_t>(PutMode::Sloppy) == 1 && static_cast<size_t>(PutMode::Define) == 2);
static constexpr Entry uncachedOperationsById[] = { Entry::operationAOTPutById, Entry::operationAOTPutByIdSloppy, Entry::operationAOTDefineById };
static constexpr Entry cachingOperationsByVal[] = { Entry::operationAOTPutByValAndCache, Entry::operationAOTPutByValAndCacheSloppy, Entry::operationAOTDefineField };
static constexpr Entry freeNameOperations[] = { Entry::operationAOTSetFreeName, Entry::operationAOTSetFreeNameSloppy };

template<size_t numberOfModes>
static Entry entryFor(const Entry (&byMode)[numberOfModes], PutMode mode)
{
    RELEASE_ASSERT(static_cast<size_t>(mode) < numberOfModes);
    return byMode[static_cast<size_t>(mode)];
}

// `root.shelf = value` where root turns out to be globalThis. Nothing is filed about the proxy, so its stores come here. What they replace is a property of the global object,
// and tryReplaceGlobalProperty() files where that lies under the Structure of the global object, which changes when the property is deleted or stops being a writable value.
// Goes on, with the three operands as they were, where the receiver is something else or nothing is filed.
static void storeToGlobalObjectThroughItsProxyAndReturn(StubAssembler& jit, GPRReg receiver, GPRReg identifierNumber, GPRReg value)
{
    const GPRReg uid = jit.allocateScratch(StubRegister::T10);
    const GPRReg structureID = jit.allocateScratch(StubRegister::T11);
    const GPRReg header = jit.allocateScratch(slotHeaderRegister);
    const GPRReg payload = jit.allocateScratch(slotPayloadRegister);
    const GPRReg entry = jit.allocateScratch(StubRegister::T14);
    const GPRReg globalObject = entry;
    const GPRReg kind = uid;
    const GPRReg wordIndex = uid;
    const GPRReg storage = structureID;
    RELEASE_ASSERT(noOverlap(receiver, identifierNumber, value, uid, structureID, header, payload, entry, instanceGPR));
    JumpList isLeftToOperation;

    isLeftToOperation.append(jit.branchIfNotCell(receiver));
    isLeftToOperation.append(jit.branch8(CCallHelpers::NotEqual, Address(receiver, JSCell::typeInfoTypeOffset()), TrustedImm32(GlobalProxyType)));
    jit.loadPtr(Address(receiver, JSGlobalProxy::targetOffset()), globalObject);
    isLeftToOperation.append(jit.branchPtr(CCallHelpers::NotEqual, globalObject, Address(instanceGPR, AOT_INSTANCE_OFFSET(globalObject))));
    isLeftToOperation.append(jit.branchPtr(CCallHelpers::NotEqual, Address(globalObject, JSGlobalObject::offsetOfGlobalThis()), receiver));
    jit.load32(Address(globalObject, JSCell::structureIDOffset()), structureID);
    loadIdentifier(jit, identifierNumber, uid, isLeftToOperation);
    findHashedSlot(jit, SlotAccess::Store, { structureID, uid, entry, header, payload, payload, kind }, isLeftToOperation);

    const CachedSlot nearValue(SlotForm::Slot, Slot::Kind::Value, header, payload);
    const CachedSlot farValue(SlotForm::Slot, Slot::Kind::FarValue, header, payload);
    isLeftToOperation.append(nearValue.branchUnlessFlag(jit, Slot::isOwn));
    Jump isFar = jit.branch32(CCallHelpers::Equal, kind, TrustedImm32(static_cast<int32_t>(Slot::Kind::FarValue)));
    isLeftToOperation.append(jit.branch32(CCallHelpers::NotEqual, kind, TrustedImm32(static_cast<int32_t>(Slot::Kind::Value))));
    jit.loadPtr(Address(receiver, JSGlobalProxy::targetOffset()), globalObject);
    nearValue.loadOwnWordIndex(jit, wordIndex);
    jit.move(globalObject, storage);
    Jump isInline = nearValue.branchUnlessFlag(jit, Slot::isOutOfLine);
    Label isOutOfLine = jit.label();
    jit.loadPtr(Address(globalObject, JSObject::butterflyOffset()), storage);
    isInline.link(&jit);
    jit.store64(value, BaseIndex(storage, wordIndex, CCallHelpers::TimesEight));
    returnAfterStoring(jit, globalObject, value);

    isFar.link(&jit);
    jit.loadPtr(Address(receiver, JSGlobalProxy::targetOffset()), globalObject);
    farValue.loadOwnWordIndex(jit, wordIndex);
    jit.jump().linkTo(isOutOfLine, &jit);

    isLeftToOperation.link(&jit);
}

static void emitStoreByIdExit(StubAssembler& jit, PutMode mode, SlotExit exit)
{
    const GPRReg receiver = jit.operand(receiverOperand);
    const GPRReg identifierNumber = jit.operand(exitNameOperand);
    const GPRReg value = jit.operand(exitValueOperand);
    if (exit == SlotExit::Uncached) {
        if (mode != PutMode::Define)
            storeToGlobalObjectThroughItsProxyAndReturn(jit, receiver, identifierNumber, value);
        jit.callOperationAndReturn(entryFor(uncachedOperationsById, mode), createExitCall(receiver, identifierNumber, value));
        return;
    }
    OperationCall call = createExitCall(receiver, value, identifierNumber);
    const OperationCall::ConstantArgument noSite { operationArgumentGPR(4), 0 };
    const OperationCall::ConstantArgument putMode { operationArgumentGPR(5), static_cast<uint32_t>(mode) };
    call.constantArguments = { noSite, putMode };
    jit.exitOnMiss(Entry::operationAOTPutByIdAndCache, call);
}

static void tryStoreByLeafLookup(StubAssembler& jit, GPRReg keyCell, GPRReg value)
{
    const GPRReg receiver = jit.operand(receiverOperand);
    const GPRReg receiverAfterCall = jit.allocateScratch(StubRegister::T12);
    callLeafLookup(jit, Entry::operationAOTPutByValGeneric, keyCell, value, receiverAfterCall);
    Jump didNotStore = jit.branch32(CCallHelpers::NotEqual, receiver, TrustedImm32(static_cast<int32_t>(LeafAnswer::Yes)));
    jit.returnToCaller();

    didNotStore.link(&jit);
    jit.move(receiverAfterCall, receiver);
}

static void emitStoreByValExit(StubAssembler& jit, PutMode mode, SlotExit exit)
{
    const GPRReg keyCell = jit.operand(exitNameOperand);
    const GPRReg value = jit.operand(exitValueOperand);
    const OperationCall call = createExitCall(jit.operand(receiverOperand), keyCell, value);
    if (exit == SlotExit::Miss) {
        jit.exitOnMiss(entryFor(cachingOperationsByVal, mode), call);
        return;
    }
    if (mode == PutMode::Define) {
        jit.callOperationAndReturn(Entry::operationAOTDefineField, call);
        return;
    }
    tryStoreByLeafLookup(jit, keyCell, value);
    jit.move(TrustedImm32(mode == PutMode::Strict ? 1 : 0), jit.allocateScratch(StubRegister::A4));
    jit.callOperationAndReturn(Entry::operationAOTPutByVal, call);
}

static void emitFreeNameStoreExit(StubAssembler& jit, PutMode mode, SlotExit exit)
{
    const OperationCall call = createExitCall(jit.operand(exitNameOperand), jit.operand(exitSiteOperand), jit.operand(exitValueOperand));
    if (exit == SlotExit::Miss)
        jit.exitOnMiss(entryFor(freeNameOperations, mode), call);
    else
        jit.callOperationAndReturn(entryFor(freeNameOperations, mode), call);
}

void generateStoreSlotExit(StubAssembler& jit)
{
    const StoreSlotExitTraits exitTraits = traitsOfStoreSlotExit(jit.key());
    const SlotHandlerTableTraits traits = traitsOf(exitTraits.table);
    const PutMode mode = putModeOf(traits.operation);
    switch (traits.origin) {
    case SlotOrigin::ById:
        emitStoreByIdExit(jit, mode, exitTraits.exit);
        return;
    case SlotOrigin::ByVal:
        emitStoreByValExit(jit, mode, exitTraits.exit);
        return;
    case SlotOrigin::FreeName:
        emitFreeNameStoreExit(jit, mode, exitTraits.exit);
        return;
    case SlotOrigin::ByIdInStubFrame:
    case SlotOrigin::FreeNameForTypeOf:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
