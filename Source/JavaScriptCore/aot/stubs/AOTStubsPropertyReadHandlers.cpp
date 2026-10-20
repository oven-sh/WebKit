/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsPropertyProbes.h"

#if ENABLE(AOT)

#include "AOTStubsField.h"
#include "AOTStubsKeyed.h"
#include "AOTStubsPropertyAccessors.h"
#include "AOTStubsPropertyExtraProperties.h"
#include "Butterfly.h"
#include "JSArrayBufferView.h"
#include "JSObject.h"
#include "JSString.h"
#include <algorithm>
#include <limits>
#include <optional>
#include <wtf/text/StringImpl.h>

namespace JSC { namespace AOT {

static constexpr unsigned receiverOperand = 0;
static constexpr unsigned headerOperand = 1;
static constexpr unsigned payloadOperand = 2;

static constexpr unsigned nameOperandOfExit = 1;
static constexpr unsigned siteOperandOfExit = 2;

static_assert(!JSValue::ValueEmpty);

// The row has a stub frame variant of every form, and only by-id reads have a table for it. Every handler is in every image.
static bool isInHandlerTable(StubKey handler)
{
    return std::ranges::any_of(allSlotHandlerTables, [&](SlotHandlerTable table) {
        return slotHandlerFor(table, slotKindOf(handler)) == handler;
    });
}

StubSignature signatureOfReadSlot(StubKey key)
{
    return signatureOfSlotHandler(handlerTableOf(isInHandlerTable(key) ? key : key.withCallerFrame(CallerFrame::HasFrame)));
}

struct ReadSlotBody {
    StubAssembler& jit;
    SlotHandlerTable table;
    SlotOrigin origin;
    SlotHandlerOperands operands;
    CachedSlot slot;
    GPRReg R0;
    GPRReg T10;
    GPRReg T11;
    GPRReg T14;
    JumpList miss;
    JumpList uncached;
};

static ReadSlotBody createReadSlotBody(StubAssembler& jit)
{
    const SlotHandlerTable table = handlerTableOf(jit.key());
    const SlotHandlerTableTraits traits = traitsOf(table);
    const SlotHandlerOperands operands = operandsOfSlotHandler(table);
    RELEASE_ASSERT(jit.operand(receiverOperand) == operands.receiver && jit.result() == operands.receiver);
    RELEASE_ASSERT(jit.operand(headerOperand) == operands.header);
    RELEASE_ASSERT(traits.form == SlotForm::PackedSlot || jit.operand(payloadOperand) == operands.payload);
    return {
        .jit = jit,
        .table = table,
        .origin = traits.origin,
        .operands = operands,
        .slot = CachedSlot(traits.form, slotKindOf(jit.key()), operands.header, operands.payload),
        .R0 = operands.receiver,
        .T10 = jit.allocateScratch(StubRegister::T10),
        .T11 = jit.allocateScratch(StubRegister::T11),
        .T14 = jit.allocateScratch(StubRegister::T14),
        .miss = { },
        .uncached = { },
    };
}

static GPRReg clobberPayloadRegister(ReadSlotBody& body)
{
    if (body.slot.form() == SlotForm::Slot)
        return body.jit.clobberOperand(payloadOperand);
    return body.jit.allocateScratch(slotPayloadRegister);
}

static void returnUndefined(ReadSlotBody& body)
{
    body.jit.move(TrustedImm64(JSValue::ValueUndefined), body.R0);
    body.jit.returnToCaller();
}

static void returnInt32(ReadSlotBody& body, GPRReg int32)
{
    boxInt32(body.jit, int32, body.R0);
    body.jit.returnToCaller();
}

static void loadPropertyName(ReadSlotBody& body, GPRReg uid)
{
    switch (body.origin) {
    case SlotOrigin::ById:
    case SlotOrigin::ByIdInStubFrame:
    case SlotOrigin::FreeName:
    case SlotOrigin::FreeNameForTypeOf:
        loadIdentifier(body.jit, body.operands.identifierNumber, uid, body.miss);
        return;
    case SlotOrigin::ByVal:
        loadNameOfKey(body.jit, body.operands.keyCell, uid, body.T11, body.miss);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void emitFarValueRead(ReadSlotBody& body)
{
    StubAssembler& jit = body.jit;
    const GPRReg butterfly = body.T10;
    const GPRReg wordIndex = body.T11;
    body.slot.loadFirstPayloadWord(jit, wordIndex);
    jit.signExtend32ToPtr(wordIndex, wordIndex);
    jit.loadPtr(Address(body.R0, JSObject::butterflyOffset()), butterfly);
    jit.load64(BaseIndex(butterfly, wordIndex, CCallHelpers::TimesEight), body.R0);
    jit.returnToCaller();
}

static void callGetter(ReadSlotBody& body, GPRReg getterSetter)
{
    const StubSignature finisher = signatureOfSlotCannot(slotCannot(SlotFinisher::CallGetter, body.origin));
    RELEASE_ASSERT(finisher.operands[finisher.numberOfOperands - 1] == Reg(getterSetter));
    JumpList cannot;
    callGetterAndReturn(body.jit, body.R0, getterSetter, cannot);
    linkToSlotCannot(body.jit, cannot, body.table, SlotFinisher::CallGetter, body.slot);
}

static void emitGetterCall(ReadSlotBody& body)
{
    const GPRReg getterSetter = body.T10;
    loadValueBySlot(body.jit, body.slot, body.R0, body.T10, body.T11, getterSetter);
    callGetter(body, getterSetter);
}

static void emitCustomGetterCall(ReadSlotBody& body)
{
    const GPRReg accessorIndex = clobberPayloadRegister(body);
    const GPRReg uid = body.T10;
    body.slot.loadFirstPayloadWord(body.jit, accessorIndex);
    loadPropertyName(body, uid);
    callCustomGetterAndReturn(body.jit, body.R0, accessorIndex, uid);
}

static FieldReadRegisters allocateFieldReadRegisters(ReadSlotBody& body, GPRReg object)
{
    return {
        .field = {
            .object = object,
            .slotData = body.T10,
            .extensionWordOffset = body.T11,
            .address = body.T11,
            .representation = body.T14,
        },
        .value = body.T10,
        .result = body.R0,
        .number = body.jit.allocateScratch(FPRInfo::fpRegT0),
        .numberScratch = body.jit.allocateScratch(FPRInfo::fpRegT1),
    };
}

static void emitFieldRead(ReadSlotBody& body)
{
    StubAssembler& jit = body.jit;
    const CachedSlot& slot = body.slot;
    const FieldReadRegisters registers = allocateFieldReadRegisters(body, body.R0);
    FieldReadExits exits;

    slot.loadFirstPayloadWord(jit, registers.field.slotData);
    Jump isOutOfLine = slot.branchIfFlag(jit, Slot::isOutOfLine);
    computeFieldAddress(jit, registers.field, false, exits.isAbsent);
    Label hasAddress = jit.label();
    readFieldAndReturn(jit, registers, exits);

    isOutOfLine.link(&jit);
    slot.loadHiddenWordOffset(jit, registers.field.extensionWordOffset);
    computeFieldAddress(jit, registers.field, true, exits.isAbsent);
    jit.jump().linkTo(hasAddress, &jit);

    exits.isAbsent.link(&jit);
    body.uncached.append(slot.branchUnlessFlag(jit, Slot::absentIsUndefined));
    returnUndefined(body);

    linkToSlotCannot(jit, exits.cannot, body.table, SlotFinisher::ReadField, slot);
}

static void emitFieldOrInheritedRead(ReadSlotBody& body)
{
    StubAssembler& jit = body.jit;
    const CachedSlot& slot = body.slot;
    const FieldReadRegisters registers = allocateFieldReadRegisters(body, body.R0);
    const GPRReg inherited = body.T10;
    FieldReadExits exits;

    slot.loadFirstPayloadWord(jit, registers.field.slotData);
    Jump isOutOfLine = slot.branchIfFlag(jit, Slot::isOutOfLine);
    computeFieldAddress(jit, registers.field, false, exits.isAbsent);
    Label hasAddress = jit.label();
    readFieldAndReturn(jit, registers, exits);

    isOutOfLine.link(&jit);
    loadExtensionWordOffset(jit, body.R0, registers.field.extensionWordOffset);
    computeFieldAddress(jit, registers.field, true, exits.isAbsent);
    jit.jump().linkTo(hasAddress, &jit);

    exits.isAbsent.link(&jit);
    loadInheritedValueBySlot(jit, slot, body.T10, body.T11, inherited);
    Jump isGetter = slot.branchIfFlag(jit, Slot::inheritsGetter);
    jit.move(inherited, body.R0);
    jit.returnToCaller();

    isGetter.link(&jit);
    callGetter(body, inherited);

    linkToSlotCannot(jit, exits.cannot, body.table, SlotFinisher::ReadField, slot);
}

static void emitVariableRead(ReadSlotBody& body)
{
    StubAssembler& jit = body.jit;
    const GPRReg statics = jit.clobberOperand(headerOperand);
    const FieldReadRegisters registers = allocateFieldReadRegisters(body, statics);
    FieldReadExits exits;

    body.slot.loadFirstPayloadWord(jit, registers.field.slotData);
    loadStaticsBase(jit, statics, body.T11);
    computeFieldAddress(jit, registers.field, false, exits.isAbsent);
    readFieldAndReturn(jit, registers, exits);

    body.uncached.append(exits.isAbsent);
    body.uncached.append(exits.cannot);
}

static ExtraPropertyRegisters extraPropertyRegistersOf(const ReadSlotBody& body)
{
    return { .object = body.R0, .extraProperties = body.T10, .wordIndex = body.T11, .scratch = body.T14 };
}

static void emitExtraGetterCall(ReadSlotBody& body)
{
    const ExtraPropertyRegisters registers = extraPropertyRegistersOf(body);
    const GPRReg getterSetter = body.T10;
    locateExtraProperty(body.jit, body.slot, registers, body.miss);
    body.jit.load64(BaseIndex(registers.extraProperties, registers.wordIndex, CCallHelpers::TimesEight), getterSetter);
    callGetter(body, getterSetter);
}

static void emitArrayLengthRead(ReadSlotBody& body)
{
    StubAssembler& jit = body.jit;
    const GPRReg length = body.T10;
    jit.loadPtr(Address(body.R0, JSObject::butterflyOffset()), length);
    jit.load32(Address(length, Butterfly::offsetOfPublicLength()), length);
    body.uncached.append(jit.branch32(CCallHelpers::LessThan, length, TrustedImm32(0)));
    returnInt32(body, length);
}

static void emitTypedArrayLengthRead(ReadSlotBody& body)
{
    StubAssembler& jit = body.jit;
    const GPRReg length = body.T10;
    body.uncached.append(jit.branchTest8(CCallHelpers::NonZero, Address(body.R0, JSArrayBufferView::offsetOfMode()), TrustedImm32(resizabilityAndAutoLengthMask)));
    jit.load64(Address(body.R0, JSArrayBufferView::offsetOfLength()), length);
    body.uncached.append(jit.branch64(CCallHelpers::Above, length, TrustedImm64(std::numeric_limits<int32_t>::max())));
    returnInt32(body, length);
}

static void emitStringLengthRead(ReadSlotBody& body)
{
    StubAssembler& jit = body.jit;
    const GPRReg length = body.T11;
    jit.loadPtr(Address(body.R0, JSString::offsetOfValue()), length);
    Jump isRope = jit.branchIfRopeStringImpl(length);
    jit.load32(Address(length, StringImpl::lengthMemoryOffset()), length);
    returnInt32(body, length);

    isRope.link(&jit);
    jit.load32(Address(body.R0, JSRopeString::offsetOfLength()), length);
    returnInt32(body, length);
}

// A cell of the front that is only reserved for its name has no slot, and a slot that does not pack is in the hashed table.
static void jumpToHashedProbe(ReadSlotBody& body)
{
    const StubKey hashedProbe = getByIdHashed().withCallerFrame(callerFrameOf(body.origin));
    const StubSignature signature = signatureOfGetByIdHashed(hashedProbe);
    RELEASE_ASSERT(signature.operands[receiverOperand] == Reg(body.R0) && signature.immediate == body.operands.identifierNumber);
    body.jit.jumpToStub(hashedProbe);
}

// No StructureID is compared at a free-name site, so its tables have an arm only for the kinds that are filed there.
static bool hasReadArm(SlotHandlerTableTraits traits, Slot::Kind kind)
{
    const bool isPacked = traits.form == SlotForm::PackedSlot;
    const bool isFreeName = traits.origin == SlotOrigin::FreeName || traits.origin == SlotOrigin::FreeNameForTypeOf;
    switch (kind) {
    case Slot::Kind::Setter:
    case Slot::Kind::CustomSetter:
    case Slot::Kind::FieldCreation:
    case Slot::Kind::FieldOrInheritedSetter:
    case Slot::Kind::ExtraSetter:
    case Slot::Kind::ExtraPropertyAddition:
    case Slot::Kind::Transition:
        return false;
    case Slot::Kind::Value:
    case Slot::Kind::Getter:
    case Slot::Kind::Uncacheable:
        return true;
    case Slot::Kind::Empty:
        return isPacked;
    case Slot::Kind::Constant:
        return !isPacked;
    case Slot::Kind::Absent:
        return traits.origin != SlotOrigin::FreeName;
    case Slot::Kind::FarValue:
    case Slot::Kind::FieldOrInherited:
        return !isPacked && !isFreeName;
    case Slot::Kind::CustomGetter:
    case Slot::Kind::Field:
    case Slot::Kind::Variable:
    case Slot::Kind::ExtraProperty:
    case Slot::Kind::ExtraGetter:
    case Slot::Kind::ArrayLength:
    case Slot::Kind::TypedArrayLength:
    case Slot::Kind::StringLength:
    case Slot::Kind::ClosureLength:
    case Slot::Kind::ClosureName:
        return !isFreeName;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void emitReadBySlot(ReadSlotBody& body)
{
    StubAssembler& jit = body.jit;
    if (!hasReadArm(traitsOf(body.table), body.slot.kind())) {
        body.miss.append(jit.jump());
        return;
    }
    switch (body.slot.kind()) {
    case Slot::Kind::Empty:
        jumpToHashedProbe(body);
        return;
    case Slot::Kind::Value:
        loadValueBySlot(jit, body.slot, body.R0, body.T10, body.T11, body.R0);
        jit.returnToCaller();
        return;
    case Slot::Kind::FarValue:
        emitFarValueRead(body);
        return;
    case Slot::Kind::Absent:
        returnUndefined(body);
        return;
    case Slot::Kind::Getter:
        emitGetterCall(body);
        return;
    case Slot::Kind::CustomGetter:
        emitCustomGetterCall(body);
        return;
    case Slot::Kind::Field:
        emitFieldRead(body);
        return;
    case Slot::Kind::FieldOrInherited:
        emitFieldOrInheritedRead(body);
        return;
    case Slot::Kind::Variable:
        emitVariableRead(body);
        return;
    case Slot::Kind::ExtraProperty:
        readExtraPropertyAndReturn(jit, body.slot, extraPropertyRegistersOf(body), body.R0, body.miss);
        return;
    case Slot::Kind::ExtraGetter:
        emitExtraGetterCall(body);
        return;
    case Slot::Kind::ArrayLength:
        emitArrayLengthRead(body);
        return;
    case Slot::Kind::TypedArrayLength:
        emitTypedArrayLengthRead(body);
        return;
    case Slot::Kind::StringLength:
        emitStringLengthRead(body);
        return;
    case Slot::Kind::ClosureLength:
        readClosureLengthAndReturn(jit, body.R0, body.miss);
        return;
    case Slot::Kind::ClosureName:
        readClosureNameAndReturn(jit, body.R0, body.miss);
        return;
    case Slot::Kind::Constant:
        body.slot.loadPayloadBits(jit, body.R0);
        jit.returnToCaller();
        return;
    case Slot::Kind::Uncacheable:
        body.uncached.append(jit.jump());
        return;
    case Slot::Kind::Setter:
    case Slot::Kind::CustomSetter:
    case Slot::Kind::FieldCreation:
    case Slot::Kind::FieldOrInheritedSetter:
    case Slot::Kind::ExtraSetter:
    case Slot::Kind::ExtraPropertyAddition:
    case Slot::Kind::Transition:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void generateReadSlot(StubAssembler& jit)
{
    if (!isInHandlerTable(jit.key())) {
        jit.breakpoint();
        return;
    }
    ReadSlotBody body = createReadSlotBody(jit);
    emitReadBySlot(body);
    linkToSlotExit(jit, body.miss, body.table, SlotExit::Miss);
    linkToSlotExit(jit, body.uncached, body.table, SlotExit::Uncached);
}

struct ReadSlotExitTraits {
    SlotHandlerTable table;
    SlotExit exit;
};

static ReadSlotExitTraits traitsOfReadSlotExit(StubKey key)
{
    for (SlotHandlerTable table : allSlotHandlerTables) {
        if (!isReadTable(table) || traitsOf(table).form != SlotForm::Slot)
            continue;
        for (SlotExit exit : { SlotExit::Miss, SlotExit::Uncached }) {
            if (slotExit(table, exit).form() == key.form())
                return { table, exit };
        }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

StubSignature signatureOfReadSlotExit(StubKey key)
{
    const SlotHandlerTable table = traitsOfReadSlotExit(key).table;
    const SlotHandlerOperands operands = operandsOfSlotHandler(table);
    const StubSignature handler = signatureOfSlotHandler(table);
    StubSignature signature = StubSignature::withGPRResult({ });
    for (unsigned index = 0; index < handler.numberOfOperands; ++index) {
        const Reg operand = handler.operands[index];
        if (operand != Reg(operands.header) && operand != Reg(operands.payload))
            signature.addOperand(operand);
    }
    return signature;
}

static OperationCall createExitCall(GPRReg second, GPRReg third, std::optional<GPRReg> fourth = std::nullopt)
{
    return {
        .saves = OperationCall::Saves::CallerSavedRegisters,
        .secondArgumentIn = second,
        .thirdArgumentIn = third,
        .fourthArgumentIn = fourth,
    };
}

static void tryLeafLookup(StubAssembler& jit, Entry leafLookup, GPRReg second)
{
    const GPRReg receiver = jit.operand(receiverOperand);
    const GPRReg receiverAfterCall = jit.allocateScratch(StubRegister::T12);
    callLeafLookup(jit, leafLookup, second, std::nullopt, receiverAfterCall);
    Jump hasNoAnswer = jit.branchTest64(CCallHelpers::Zero, receiver);
    jit.returnToCaller();

    hasNoAnswer.link(&jit);
    jit.move(receiverAfterCall, receiver);
}

static void emitMissById(StubAssembler& jit)
{
    const GPRReg noSite = jit.allocateScratch(StubRegister::T10);
    jit.move(CCallHelpers::TrustedImmPtr(nullptr), noSite);
    jit.exitOnMiss(Entry::operationAOTGetByIdAndCache, createExitCall(jit.operand(receiverOperand), jit.operand(nameOperandOfExit), noSite));
}

static void emitUncachedReadById(StubAssembler& jit)
{
    const GPRReg receiver = jit.operand(receiverOperand);
    const GPRReg identifierNumber = jit.operand(nameOperandOfExit);
    const GPRReg uid = jit.allocateScratch(StubRegister::T10);
    JumpList needsOperation;
    needsOperation.append(jit.branchIfNotCell(receiver));
    needsOperation.append(jit.branchIfNotObject(receiver));
    loadIdentifier(jit, identifierNumber, uid, needsOperation);
    tryLeafLookup(jit, Entry::operationAOTGetGeneric, uid);

    needsOperation.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTGetById, createExitCall(receiver, identifierNumber));
}

static void emitExitOfReadById(StubAssembler& jit, SlotExit exit)
{
    switch (exit) {
    case SlotExit::Miss:
        emitMissById(jit);
        return;
    case SlotExit::Uncached:
        emitUncachedReadById(jit);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void emitExitOfReadByVal(StubAssembler& jit, SlotExit exit)
{
    const GPRReg receiver = jit.operand(receiverOperand);
    const GPRReg keyCell = jit.operand(nameOperandOfExit);
    switch (exit) {
    case SlotExit::Miss:
        jit.exitOnMiss(Entry::operationAOTGetByValAndCache, createExitCall(receiver, keyCell));
        return;
    case SlotExit::Uncached:
        tryLeafLookup(jit, Entry::operationAOTGetByValGeneric, keyCell);
        jit.callOperationAndReturn(Entry::operationAOTGetByVal, createExitCall(receiver, keyCell));
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void emitExitOfFreeNameRead(StubAssembler& jit, Entry operation, SlotExit exit)
{
    const OperationCall call = createExitCall(jit.operand(nameOperandOfExit), jit.operand(siteOperandOfExit));
    switch (exit) {
    case SlotExit::Miss:
        jit.exitOnMiss(operation, call);
        return;
    case SlotExit::Uncached:
        jit.callOperationAndReturn(operation, call);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void generateReadSlotExit(StubAssembler& jit)
{
    const ReadSlotExitTraits traits = traitsOfReadSlotExit(jit.key());
    switch (traitsOf(traits.table).origin) {
    case SlotOrigin::ById:
    case SlotOrigin::ByIdInStubFrame:
        emitExitOfReadById(jit, traits.exit);
        return;
    case SlotOrigin::ByVal:
        emitExitOfReadByVal(jit, traits.exit);
        return;
    case SlotOrigin::FreeName:
        emitExitOfFreeNameRead(jit, Entry::operationAOTGetFreeName, traits.exit);
        return;
    case SlotOrigin::FreeNameForTypeOf:
        emitExitOfFreeNameRead(jit, Entry::operationAOTGetFreeNameForTypeOf, traits.exit);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
