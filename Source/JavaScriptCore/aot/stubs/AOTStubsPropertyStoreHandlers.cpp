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
#include "AOTSlowPaths.h"
#include "AOTStubsBarriers.h"
#include "AOTStubsField.h"
#include "AOTStubsKeyed.h"
#include "AOTStubsPropertyAccessors.h"
#include "AOTStubsPropertyExtraProperties.h"
#include "Butterfly.h"
#include "IndexingType.h"
#include "JSFunction.h"
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

struct StoreSlotBody {
    StubAssembler& jit;
    SlotHandlerTable table;
    SlotHandlerTableTraits traits;
    CachedSlot slot;
    GPRReg receiver;
    GPRReg value;
    GPRReg name;
    GPRReg T10;
    GPRReg T11;
    GPRReg T14;
    JumpList miss;
    JumpList uncached;
};

static unsigned nameOperandOf(SlotForm form)
{
    return form == SlotForm::Slot ? payloadOperand + 1 : payloadOperand;
}

static StoreSlotBody createStoreSlotBody(StubAssembler& jit)
{
    const SlotHandlerTable table = handlerTableOf(jit.key());
    const SlotHandlerTableTraits traits = traitsOf(table);
    const SlotHandlerOperands operands = operandsOfSlotHandler(table);
    const GPRReg name = traits.origin == SlotOrigin::ByVal ? operands.keyCell : operands.identifierNumber;
    RELEASE_ASSERT(jit.operand(receiverOperand) == operands.receiver && jit.operand(headerOperand) == operands.header);
    RELEASE_ASSERT(traits.form == SlotForm::PackedSlot || jit.operand(payloadOperand) == operands.payload);
    RELEASE_ASSERT(jit.operand(nameOperandOf(traits.form)) == name);
    RELEASE_ASSERT(signatureOfWriteBarrier(writeBarrier()).operands[0] == Reg(operands.receiver));
    return {
        .jit = jit,
        .table = table,
        .traits = traits,
        .slot = CachedSlot(traits.form, slotKindOf(jit.key()), operands.header, operands.payload),
        .receiver = operands.receiver,
        .value = operands.value,
        .name = name,
        .T10 = jit.allocateScratch(StubRegister::T10),
        .T11 = jit.allocateScratch(StubRegister::T11),
        .T14 = jit.allocateScratch(StubRegister::T14),
        .miss = { },
        .uncached = { },
    };
}

static GPRReg clobberPayloadRegister(StoreSlotBody& body)
{
    if (body.traits.form == SlotForm::Slot)
        return body.jit.clobberOperand(payloadOperand);
    return body.jit.allocateScratch(slotPayloadRegister);
}

static GPRReg clobberNameRegister(StoreSlotBody& body)
{
    return body.jit.clobberOperand(nameOperandOf(body.traits.form));
}

static GPRReg allocateSpareRegister(StoreSlotBody& body)
{
    switch (body.traits.origin) {
    case SlotOrigin::ById:
        return body.jit.allocateScratch(StubRegister::A2);
    case SlotOrigin::ByVal:
        return body.jit.allocateScratch(StubRegister::T9);
    case SlotOrigin::ByIdInStubFrame:
    case SlotOrigin::FreeName:
    case SlotOrigin::FreeNameForTypeOf:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void loadPropertyName(StoreSlotBody& body, GPRReg uid)
{
    switch (body.traits.origin) {
    case SlotOrigin::ById:
    case SlotOrigin::FreeName:
        loadIdentifier(body.jit, body.name, uid, body.miss);
        return;
    case SlotOrigin::ByVal:
        loadNameOfKey(body.jit, body.name, uid, body.T11, body.miss);
        return;
    case SlotOrigin::ByIdInStubFrame:
    case SlotOrigin::FreeNameForTypeOf:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void storeInStorageOf(StoreSlotBody& body, GPRReg owner, GPRReg wordIndex)
{
    StubAssembler& jit = body.jit;
    const GPRReg storage = body.T14;
    RELEASE_ASSERT(noOverlap(owner, wordIndex, storage, body.value));
    jit.move(owner, storage);
    Jump isInline = body.slot.branchUnlessFlag(jit, Slot::isOutOfLine);
    jit.loadPtr(Address(owner, JSObject::butterflyOffset()), storage);
    isInline.link(&jit);
    jit.store64(body.value, BaseIndex(storage, wordIndex, CCallHelpers::TimesEight));
}

static void emitValueStore(StoreSlotBody& body)
{
    StubAssembler& jit = body.jit;
    const GPRReg wordIndex = body.T11;
    if (body.traits.origin != SlotOrigin::FreeName) {
        body.slot.loadOwnWordIndex(jit, wordIndex);
        storeInStorageOf(body, body.receiver, wordIndex);
        returnAfterStoring(jit, body.receiver, body.value);
        return;
    }
    const GPRReg holder = body.T10;
    body.slot.loadHolder(jit, holder);
    body.slot.loadInheritedWordIndex(jit, wordIndex);
    storeInStorageOf(body, holder, wordIndex);
    returnAfterStoring(jit, holder, body.value);
}

static void emitFarValueStore(StoreSlotBody& body)
{
    StubAssembler& jit = body.jit;
    const GPRReg butterfly = body.T10;
    const GPRReg wordIndex = body.T11;
    body.slot.loadOwnWordIndex(jit, wordIndex);
    jit.loadPtr(Address(body.receiver, JSObject::butterflyOffset()), butterfly);
    jit.store64(body.value, BaseIndex(butterfly, wordIndex, CCallHelpers::TimesEight));
    returnAfterStoring(jit, body.receiver, body.value);
}

static SlotFinisher setterFinisherOf(SlotOperation operation)
{
    switch (operation) {
    case SlotOperation::StoreStrict:
        return SlotFinisher::CallSetterStrict;
    case SlotOperation::StoreSloppy:
        return SlotFinisher::CallSetterSloppy;
    case SlotOperation::Read:
    case SlotOperation::Define:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void callSetter(StoreSlotBody& body, GPRReg getterSetter)
{
    const SlotFinisher finisher = setterFinisherOf(body.traits.operation);
    const StubSignature finisherSignature = signatureOfSlotCannot(slotCannot(finisher, body.traits.origin));
    RELEASE_ASSERT(finisherSignature.operands[finisherSignature.numberOfOperands - 2] == Reg(getterSetter));
    JumpList cannot;
    callSetterAndReturn(body.jit, body.receiver, body.value, getterSetter, cannot);
    linkToSlotCannot(body.jit, cannot, body.table, finisher, body.slot);
}

static void emitSetterCall(StoreSlotBody& body)
{
    const GPRReg getterSetter = body.T10;
    loadValueBySlot(body.jit, body.slot, body.receiver, body.T10, body.T11, getterSetter);
    callSetter(body, getterSetter);
}

static void emitCustomSetterCall(StoreSlotBody& body)
{
    const GPRReg accessorIndex = clobberPayloadRegister(body);
    const GPRReg uid = body.T14;
    loadPropertyName(body, uid);
    body.slot.loadFirstPayloadWord(body.jit, accessorIndex);
    callCustomSetterAndReturn(body.jit, body.receiver, body.value, accessorIndex, uid);
}

struct FieldStoreArm {
    FieldStoreRegisters registers;
    FPRReg savedName;
    JumpList uncached;
};

static FieldStoreArm createFieldStoreArm(StoreSlotBody& body, GPRReg object)
{
    StubAssembler& jit = body.jit;
    return {
        .registers = {
            .field = {
                .object = object,
                .slotData = body.T10,
                .extensionWordOffset = body.T11,
                .address = body.T11,
                .representation = clobberNameRegister(body),
            },
            .value = body.value,
            .type = body.T14,
            .scratch = body.T10,
            .secondScratch = allocateSpareRegister(body),
            .number = jit.allocateScratch(FPRInfo::fpRegT0),
        },
        .savedName = jit.allocateScratch(FPRInfo::fpRegT1),
        .uncached = { },
    };
}

// storeField takes every general-purpose scratch register of x86-64, the name's among them, and the Uncached exit takes the name.
static void saveName(StoreSlotBody& body, const FieldStoreArm& arm)
{
    body.jit.move64ToDouble(body.name, arm.savedName);
}

static void linkToUncachedExit(StoreSlotBody& body, FieldStoreArm& arm)
{
    StubAssembler& jit = body.jit;
    if (arm.uncached.empty())
        return;
    arm.uncached.link(&jit);
    jit.moveDoubleTo64(arm.savedName, body.name);
    body.uncached.append(jit.jump());
}

static void emitFieldCreation(StoreSlotBody& body, FieldStoreArm& arm, FieldStoreExits& storeExits, JumpList& hasNoStorage, Label findStorage)
{
    StubAssembler& jit = body.jit;
    const CachedSlot& slot = body.slot;
    const FieldStoreRegisters& r = arm.registers;
    const Address orderWord(body.receiver, offsetOfOrderWord);
    const TrustedImm32 integrityBits(static_cast<int32_t>(orderWordIntegrityBits));
    const KeyOrderRegisters keyOrder {
        .object = body.receiver,
        .slotKey = r.scratch,
        .entry = r.field.address,
        .words = r.type,
        .scratch = r.field.representation,
    };
    const OperationCall didCreateFieldCall {
        .returns = OperationCall::Returns::Void,
        .saves = OperationCall::Saves::CallerSavedRegisters,
        .checksException = false,
        .secondArgumentIn = keyOrder.object,
        .thirdArgumentIn = keyOrder.slotKey,
    };
    FieldStoreExits exits;
    JumpList stored;
    JumpList hasNoTransition;

    // A field that is about to be created has no granule yet. With one it is a field that has storage and is absent.
    hasNoStorage.link(&jit);
    arm.uncached.append(jit.branchTest32(CCallHelpers::NonZero, orderWord, integrityBits));
    // Where the key order alone says that the field exists, a field without a granule reads as undefined, so that value asks for none.
    Jump alwaysNeedsStorage = slot.branchUnlessFlag(jit, Slot::undefinedNeedsNoStorage);
    Jump needsNoStorage = jit.branch64(CCallHelpers::Equal, body.value, TrustedImm32(JSValue::ValueUndefined));
    alwaysNeedsStorage.link(&jit);
    const GPRReg granule = r.field.representation;
    const GPRReg granuleBit = r.secondScratch;
    const GPRReg extension = r.field.address;
    JumpList cannotAppend;
    jit.and32(TrustedImm32(SlotData::maxOffset), r.field.slotData, granule);
    jit.urshift32(TrustedImm32(3), granule);
    jit.sub32(TrustedImm32(1), granule);
    jit.move(TrustedImm32(1), granuleBit);
    jit.lshift64(granule, granuleBit);
    static_assert(granuleOf(offsetOfFirstExtensionSlot) == offsetOfFirstExtensionSlot / 8 - 1 && extensionGranuleSize == 8);
    slot.loadHiddenWordOffset(jit, r.field.extensionWordOffset);
    if (jit.options().useWideReferences) {
        jit.load64(BaseIndex(body.receiver, r.field.extensionWordOffset, CCallHelpers::TimesOne), extension);
        cannotAppend.append(jit.branchTest64(CCallHelpers::Zero, extension));
    } else {
        jit.load32(BaseIndex(body.receiver, r.field.extensionWordOffset, CCallHelpers::TimesOne), extension);
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
        RELEASE_ASSERT(body.receiver != A2 && noOverlap(instanceGPR, A1, A2));
        jit.move(granuleBit, A2);
        jit.move(body.receiver, A1);
        jit.move(instanceGPR, A0);
        jit.callOperation(Entry::operationAOTEnsureExtensionGranules, jit.reuseScratch(StubRegister::Operation));
    });
    jit.jump().linkTo(findStorage, &jit);

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
    appendFieldToKeyOrder(jit, keyOrder, hasNoTransition);
    returnAfterStoring(jit, body.receiver, body.value);

    // The slot key says where the slot is. The storage is there by now, or the field needs none.
    hasNoTransition.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTDidCreateFieldAt, didCreateFieldCall);
}

static void emitFieldStore(StoreSlotBody& body)
{
    StubAssembler& jit = body.jit;
    const CachedSlot& slot = body.slot;
    const bool createsField = slot.kind() == Slot::Kind::FieldCreation;
    FieldStoreArm arm = createFieldStoreArm(body, body.receiver);
    const FieldStoreRegisters& r = arm.registers;
    FieldStoreExits exits;
    JumpList hasNoStorage;

    body.uncached.append(jit.branchTest32(CCallHelpers::NonZero, Address(body.receiver, offsetOfOrderWord), TrustedImm32(integrityBitsBlockingStore(body.traits.operation))));
    saveName(body, arm);
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
        const Address orderWord(body.receiver, offsetOfOrderWord);
        Jump remembersNothing = slot.branchUnlessFlag(jit, Slot::remembersOrder);
        Jump isAbsent = jit.branchTest64(CCallHelpers::Zero, valueInSlot);
        jit.store64(body.value, valueInSlot);
        returnAfterStoring(jit, body.receiver, body.value);

        isAbsent.link(&jit);
        exits.wasAbsent.append(jit.branch32(CCallHelpers::NotEqual, orderWord, orderWordBefore));
        jit.store64(body.value, valueInSlot);
        slot.loadHiddenWordOffset(jit, orderAfter);
        jit.add32(TrustedImm32(1 << orderWordCountShift), orderWordBefore);
        jit.and32(TrustedImm32(static_cast<int32_t>(~orderWordOrderMask)), orderWordBefore);
        jit.or32(orderAfter, orderWordBefore);
        jit.store32(orderWordBefore, orderWord);
        returnAfterStoring(jit, body.receiver, body.value);
        remembersNothing.link(&jit);
    }
    Label hasAddress = jit.label();
    storeField(jit, r, FieldStoreEnd::ReturnAfterBarrier, FieldTypeTest::IsEmitted, nullptr, exits);

    isOutOfLine.link(&jit);
    slot.loadHiddenWordOffset(jit, r.field.extensionWordOffset);
    computeFieldAddress(jit, r.field, true, hasNoStorage);
    jit.jump().linkTo(hasAddress, &jit);

    if (createsField)
        emitFieldCreation(body, arm, exits, hasNoStorage, findStorage);
    else {
        arm.uncached.append(exits.wasAbsent);
        arm.uncached.append(exits.wasAbsentAndIsNotAccepted);
        arm.uncached.append(hasNoStorage);
    }
    linkToSlotCannot(jit, exits.cannot, body.table, createsField ? SlotFinisher::CreateField : SlotFinisher::StoreField, slot);
    linkToUncachedExit(body, arm);
}

// The record has no room for the type of the field, so a store to a filled slot is left to the run time.
static void emitFieldOrInheritedSetterCall(StoreSlotBody& body)
{
    StubAssembler& jit = body.jit;
    const FieldAddressRegisters field {
        .object = body.receiver,
        .slotData = body.T10,
        .extensionWordOffset = body.T11,
        .address = body.T11,
        .representation = body.T14,
    };
    const GPRReg getterSetter = body.T10;
    JumpList isAbsent;

    body.slot.loadFirstPayloadWord(jit, field.slotData);
    Jump isOutOfLine = body.slot.branchIfFlag(jit, Slot::isOutOfLine);
    computeFieldAddress(jit, field, false, isAbsent);
    Label hasAddress = jit.label();
    branchOnFieldPresence(jit, field, body.T10, body.uncached, isAbsent);

    isOutOfLine.link(&jit);
    loadExtensionWordOffset(jit, body.receiver, field.extensionWordOffset);
    computeFieldAddress(jit, field, true, isAbsent);
    jit.jump().linkTo(hasAddress, &jit);

    isAbsent.link(&jit);
    loadInheritedValueBySlot(jit, body.slot, body.T10, body.T11, getterSetter);
    callSetter(body, getterSetter);
}

static void emitVariableStore(StoreSlotBody& body)
{
    StubAssembler& jit = body.jit;
    const CachedSlot& slot = body.slot;
    const GPRReg statics = jit.clobberOperand(headerOperand);
    FieldStoreArm arm = createFieldStoreArm(body, statics);
    const FieldStoreRegisters& r = arm.registers;
    const GPRReg roundedSizeOfStatics = r.scratch;
    const GPRReg offsetInStatics = r.field.address;
    FieldStoreExits exits;
    JumpList hasNoStorage;
    JumpList isPresent;
    JumpList stored;

    saveName(body, arm);
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

    linkToUncachedExit(body, arm);
}

static ExtraPropertyRegisters extraPropertyRegistersOf(const StoreSlotBody& body)
{
    return { .object = body.receiver, .extraProperties = body.T10, .wordIndex = body.T11, .scratch = body.T14 };
}

static void finishExtraPropertyStore(StoreSlotBody& body, GPRReg owner, bool valueMayNeedBarrier)
{
    if (valueMayNeedBarrier)
        returnAfterStoring(body.jit, owner, body.value);
    else
        body.jit.returnToCaller();
}

static void emitExtraPropertyStore(StoreSlotBody& body)
{
    auto finish = [&](GPRReg owner, bool valueMayNeedBarrier) {
        finishExtraPropertyStore(body, owner, valueMayNeedBarrier);
    };
    storeExtraProperty(body.jit, body.slot, extraPropertyRegistersOf(body), body.value, finish, body.miss);
}

static void emitExtraSetterCall(StoreSlotBody& body)
{
    const ExtraPropertyRegisters registers = extraPropertyRegistersOf(body);
    const GPRReg getterSetter = body.T10;
    locateExtraProperty(body.jit, body.slot, registers, body.miss);
    body.jit.load64(BaseIndex(registers.extraProperties, registers.wordIndex, CCallHelpers::TimesEight), getterSetter);
    callSetter(body, getterSetter);
}

static void emitExtraPropertyAddition(StoreSlotBody& body)
{
    StubAssembler& jit = body.jit;
    const ExtraPropertyRegisters registers = extraPropertyRegistersOf(body);
    ExtraPropertyAdditionExits exits;
    JumpList cannotGrow;
    auto finish = [&](GPRReg owner, bool valueMayNeedBarrier) {
        finishExtraPropertyStore(body, owner, valueMayNeedBarrier);
    };
    addExtraProperty(jit, body.slot, registers, body.value, finish, exits);
    body.uncached.append(exits.isNotExtensible);

    // The growth takes every general-purpose scratch register of x86-64, the header's among them, and the finisher takes the header.
    exits.growsStorage.link(&jit);
    const GPRReg header = jit.clobberOperand(headerOperand);
    const FPRReg savedHeader = jit.allocateScratch(FPRInfo::fpRegT0);
    jit.move64ToDouble(header, savedHeader);
    addExtraPropertyInNewStorage(jit, body.slot, { .access = registers, .oldCapacity = allocateSpareRegister(body), .storage = clobberNameRegister(body), .scratch = header }, body.value, finish, cannotGrow);

    cannotGrow.link(&jit);
    jit.moveDoubleTo64(savedHeader, header);
    exits.cannot.append(jit.jump());
    linkToSlotCannot(jit, exits.cannot, body.table, SlotFinisher::GrowStorage, body.slot);
}

static void emitAdditionInStorage(StoreSlotBody& body)
{
    StubAssembler& jit = body.jit;
    const GPRReg newStructureID = body.T10;
    const GPRReg wordIndex = body.T11;
    body.slot.loadFirstPayloadWord(jit, newStructureID);
    body.slot.loadOwnWordIndex(jit, wordIndex);
    storeInStorageOf(body, body.receiver, wordIndex);
    jit.store32(newStructureID, Address(body.receiver, JSCell::structureIDOffset()));
    jit.jumpToStub(writeBarrier());
}

static Address closureExecutable(FunctionTables::ExecutableKind kind)
{
    return Address(instanceGPR, static_cast<int32_t>(AOT_INSTANCE_OFFSET(closures.executables) + static_cast<unsigned>(kind) * sizeof(JSCell*)));
}

// Functions of every kind share a Structure, and only these kinds have an ordinary [[Set]].
static void branchUnlessOrdinaryFunction(StoreSlotBody& body, GPRReg executable)
{
    StubAssembler& jit = body.jit;
    Jump isNotFunction = jit.branchIfNotType(body.receiver, jsFunctionTypes);
    jit.loadPtr(Address(body.receiver, JSFunction::offsetOfExecutableOrRareData()), executable);
    Jump isPlainFunction = jit.branchPtr(CCallHelpers::Equal, executable, closureExecutable(FunctionTables::ExecutableKind::Constructible));
    Jump isArrowFunction = jit.branchPtr(CCallHelpers::Equal, executable, closureExecutable(FunctionTables::ExecutableKind::NotConstructible));
    body.uncached.append(jit.branchPtr(CCallHelpers::NotEqual, executable, Address(instanceGPR, AOT_INSTANCE_OFFSET(realm.boundFunctionExecutable))));
    isArrowFunction.link(&jit);
    isPlainFunction.link(&jit);
    isNotFunction.link(&jit);
}

static void emitAdditionInNewStorage(StoreSlotBody& body)
{
    StubAssembler& jit = body.jit;
    const GPRReg newStructureID = body.T10;
    const GPRReg oldCapacity = body.T11;
    const GPRReg newCapacity = body.T14;
    const Address butterfly(body.receiver, JSObject::butterflyOffset());
    JumpList cannot;

    // No packed slot has the flag.
    Jump hasIndexingHeader;
    if (body.traits.form == SlotForm::Slot)
        hasIndexingHeader = body.slot.branchIfFlag(jit, Slot::storageHasIndexingHeader);
    body.slot.loadSecondPayloadWord(jit, newCapacity);
    Jump isFirstStorage = jit.branch32(CCallHelpers::Equal, newCapacity, TrustedImm32(initialOutOfLineCapacity));
    cannot.append(jit.branchTestPtr(CCallHelpers::Zero, butterfly));
    jit.urshift32(newCapacity, TrustedImm32(1), oldCapacity);
    Jump hasCapacities = jit.jump();

    isFirstStorage.link(&jit);
    cannot.append(jit.branchTestPtr(CCallHelpers::NonZero, butterfly));
    branchUnlessOrdinaryFunction(body, newStructureID);
    jit.move(TrustedImm32(0), oldCapacity);

    hasCapacities.link(&jit);
    body.slot.loadFirstPayloadWord(jit, newStructureID);
    growOutOfLineStorage(jit, {
        .object = body.receiver,
        .newStructureID = newStructureID,
        .oldCapacity = oldCapacity,
        .newCapacity = newCapacity,
        .storage = clobberNameRegister(body),
        .scratch = allocateSpareRegister(body),
        .addedValue = body.value,
    }, cannot);
    jit.jumpToStub(writeBarrier());

    // An array that was born with room for its first named properties has the new storage already: the room lies in front of its indexing header.
    if (hasIndexingHeader.isSet()) {
        const GPRReg distanceToButterfly = oldCapacity;
        hasIndexingHeader.link(&jit);
        body.slot.loadSecondPayloadWord(jit, newCapacity);
        cannot.append(jit.branch32(CCallHelpers::NotEqual, newCapacity, TrustedImm32(initialOutOfLineCapacity)));
        jit.loadPtr(butterfly, distanceToButterfly);
        jit.subPtr(body.receiver, distanceToButterfly);
        cannot.append(jit.branch64(CCallHelpers::NotEqual, distanceToButterfly, TrustedImm32(static_cast<int32_t>(sizeOfArrayWithStorage(0, propertyRoomOfArray)))));
        emitAdditionInStorage(body);
    }

    linkToSlotCannot(jit, cannot, body.table, SlotFinisher::GrowStorage, body.slot);
}

static void emitTransition(StoreSlotBody& body)
{
    Jump growsStorage = body.slot.branchIfFlag(body.jit, Slot::growsStorage);
    emitAdditionInStorage(body);

    growsStorage.link(&body.jit);
    emitAdditionInNewStorage(body);
}

static void emitArrayLengthStore(StoreSlotBody& body)
{
    constexpr unsigned maxElementsClearedInStub = 64;
    StubAssembler& jit = body.jit;
    JumpList& cannot = body.uncached;
    const GPRReg array = body.receiver;
    const GPRReg newLength = body.T10;
    const GPRReg butterfly = body.T11;
    const GPRReg length = jit.clobberOperand(headerOperand);
    const GPRReg hole = body.T14;

    cannot.append(jit.branchIfNotInt32(body.value));
    jit.zeroExtend32ToWord(body.value, newLength);
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

static void emitStoreBySlot(StoreSlotBody& body)
{
    if (!hasStoreArm(body.traits, body.slot.kind())) {
        body.miss.append(body.jit.jump());
        return;
    }
    switch (body.slot.kind()) {
    case Slot::Kind::Value:
        emitValueStore(body);
        return;
    case Slot::Kind::FarValue:
        emitFarValueStore(body);
        return;
    case Slot::Kind::Setter:
        emitSetterCall(body);
        return;
    case Slot::Kind::CustomSetter:
        emitCustomSetterCall(body);
        return;
    case Slot::Kind::Field:
    case Slot::Kind::FieldCreation:
        emitFieldStore(body);
        return;
    case Slot::Kind::FieldOrInheritedSetter:
        emitFieldOrInheritedSetterCall(body);
        return;
    case Slot::Kind::Variable:
        emitVariableStore(body);
        return;
    case Slot::Kind::ExtraProperty:
        emitExtraPropertyStore(body);
        return;
    case Slot::Kind::ExtraSetter:
        emitExtraSetterCall(body);
        return;
    case Slot::Kind::ExtraPropertyAddition:
        emitExtraPropertyAddition(body);
        return;
    case Slot::Kind::Transition:
        emitTransition(body);
        return;
    case Slot::Kind::ArrayLength:
        emitArrayLengthStore(body);
        return;
    case Slot::Kind::Uncacheable:
        body.uncached.append(body.jit.jump());
        return;
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
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void generateStoreSlot(StubAssembler& jit)
{
    StoreSlotBody body = createStoreSlotBody(jit);
    emitStoreBySlot(body);
    linkToSlotExit(jit, body.miss, body.table, SlotExit::Miss);
    linkToSlotExit(jit, body.uncached, body.table, SlotExit::Uncached);
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

static Entry uncachedOperationById(PutMode mode)
{
    switch (mode) {
    case PutMode::Strict:
        return Entry::operationAOTPutById;
    case PutMode::Sloppy:
        return Entry::operationAOTPutByIdSloppy;
    case PutMode::Define:
        return Entry::operationAOTDefineById;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void emitStoreByIdExit(StubAssembler& jit, PutMode mode, SlotExit exit)
{
    const GPRReg receiver = jit.operand(receiverOperand);
    const GPRReg identifierNumber = jit.operand(exitNameOperand);
    const GPRReg value = jit.operand(exitValueOperand);
    switch (exit) {
    case SlotExit::Miss: {
        OperationCall call = createExitCall(receiver, value, identifierNumber);
        const OperationCall::ConstantArgument noSite { operationArgumentGPR(4), 0 };
        const OperationCall::ConstantArgument putMode { operationArgumentGPR(5), static_cast<uint32_t>(mode) };
        call.constantArguments = { noSite, putMode };
        jit.exitOnMiss(Entry::operationAOTPutByIdAndCache, call);
        return;
    }
    case SlotExit::Uncached:
        jit.callOperationAndReturn(uncachedOperationById(mode), createExitCall(receiver, identifierNumber, value));
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Entry cachingOperationByVal(PutMode mode)
{
    switch (mode) {
    case PutMode::Strict:
        return Entry::operationAOTPutByValAndCache;
    case PutMode::Sloppy:
        return Entry::operationAOTPutByValAndCacheSloppy;
    case PutMode::Define:
        return Entry::operationAOTDefineField;
    }
    RELEASE_ASSERT_NOT_REACHED();
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
    switch (exit) {
    case SlotExit::Miss:
        jit.exitOnMiss(cachingOperationByVal(mode), call);
        return;
    case SlotExit::Uncached:
        if (mode == PutMode::Define) {
            jit.callOperationAndReturn(Entry::operationAOTDefineField, call);
            return;
        }
        tryStoreByLeafLookup(jit, keyCell, value);
        jit.move(TrustedImm32(mode == PutMode::Strict ? 1 : 0), jit.allocateScratch(StubRegister::A4));
        jit.callOperationAndReturn(Entry::operationAOTPutByVal, call);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Entry freeNameOperation(PutMode mode)
{
    switch (mode) {
    case PutMode::Strict:
        return Entry::operationAOTSetFreeName;
    case PutMode::Sloppy:
        return Entry::operationAOTSetFreeNameSloppy;
    case PutMode::Define:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void emitFreeNameStoreExit(StubAssembler& jit, PutMode mode, SlotExit exit)
{
    const OperationCall call = createExitCall(jit.operand(exitNameOperand), jit.operand(exitSiteOperand), jit.operand(exitValueOperand));
    switch (exit) {
    case SlotExit::Miss:
        jit.exitOnMiss(freeNameOperation(mode), call);
        return;
    case SlotExit::Uncached:
        jit.callOperationAndReturn(freeNameOperation(mode), call);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
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
