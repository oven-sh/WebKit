/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsPropertyProbes.h"

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTCompiledStructure.h"
#include "AOTPropertyCachesFront.h"
#include "AOTStubsField.h"
#include "AOTStubsKeyed.h"
#include "AOTStubsPropertyAccessors.h"
#include "AOTStubsCalls.h"
#include "AOTStubsPropertyExtraProperties.h"
#include "Butterfly.h"
#include "JSArrayBufferView.h"
#include "JSObject.h"
#include "JSString.h"
#include "ProxyObject.h"
#include "VM.h"
#include <algorithm>
#include <limits>
#include <optional>
#include <wtf/text/StringImpl.h>

namespace JSC { namespace AOT {

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
    case Slot::Kind::Removal:
    case Slot::Kind::ExtraPropertyRemoval:
    case Slot::Kind::FieldRemoval:
    case Slot::Kind::NothingToRemove:
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
    case Slot::Kind::FieldOrInheritedField:
        return !isPacked && !isFreeName;
    case Slot::Kind::CustomGetter:
    case Slot::Kind::Field:
    case Slot::Kind::FieldGetter:
    case Slot::Kind::InheritedField:
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

void generateReadSlot(StubAssembler& jit)
{
    if (!isInHandlerTable(jit.key())) {
        jit.breakpoint();
        return;
    }
    const SlotHandlerTable table = handlerTableOf(jit.key());
    const SlotHandlerTableTraits traits = traitsOf(table);
    const SlotOrigin origin = traits.origin;
    const SlotHandlerOperands operands = operandsOfSlotHandler(table);
    RELEASE_ASSERT(jit.operand(receiverOperand) == operands.receiver && jit.result() == operands.receiver);
    RELEASE_ASSERT(jit.operand(headerOperand) == operands.header);
    RELEASE_ASSERT(traits.form == SlotForm::PackedSlot || jit.operand(payloadOperand) == operands.payload);
    const CachedSlot slot(traits.form, slotKindOf(jit.key()), operands.header, operands.payload);
    const GPRReg R0 = operands.receiver;
    const GPRReg T10 = jit.allocateScratch(StubRegister::T10);
    const GPRReg T11 = jit.allocateScratch(StubRegister::T11);
    const GPRReg T14 = jit.allocateScratch(StubRegister::T14);
    const ExtraPropertyRegisters extraPropertyRegisters { .object = R0, .extraProperties = T10, .wordIndex = T11, .scratch = T14 };
    JumpList miss;
    JumpList uncached;

    auto clobberPayloadRegister = [&] {
        return slot.form() == SlotForm::Slot ? jit.clobberOperand(payloadOperand) : jit.allocateScratch(slotPayloadRegister);
    };
    auto returnUndefined = [&] {
        jit.move(TrustedImm64(JSValue::ValueUndefined), R0);
        jit.returnToCaller();
    };
    auto returnInt32 = [&](GPRReg int32) {
        boxInt32(jit, int32, R0);
        jit.returnToCaller();
    };
    auto loadPropertyName = [&](GPRReg uid) {
        switch (origin) {
        case SlotOrigin::ById:
        case SlotOrigin::ByIdInStubFrame:
        case SlotOrigin::FreeName:
        case SlotOrigin::FreeNameForTypeOf:
            loadIdentifier(jit, operands.identifierNumber, uid, miss);
            return;
        case SlotOrigin::ByVal:
            loadNameOfKey(jit, operands.keyCell, uid, T11, miss);
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    };
    auto callGetter = [&](GPRReg getterSetter) {
        const StubSignature finisher = signatureOfSlotCannot(slotCannot(SlotFinisher::CallGetter, origin));
        RELEASE_ASSERT(finisher.operands[finisher.numberOfOperands - 1] == Reg(getterSetter));
        // Whoever jumps to a handler has written the two registers of the slot itself, and the number of the name is an immediate of the call: no caller keeps a value in them.
        RegisterSet lostToEveryCaller;
        for (GPRReg reg : { operands.header, operands.payload, operands.identifierNumber }) {
            if (reg != InvalidGPRReg)
                lostToEveryCaller.add(reg, IgnoreVectors);
        }
        jit.recordClobbers(lostToEveryCaller);
        JumpList cannot;
        callGetterAndReturn(jit, R0, getterSetter, cannot);
        linkToSlotCannot(jit, cannot, table, SlotFinisher::CallGetter, slot);
    };
    auto allocateFieldReadRegisters = [&](GPRReg object, GPRReg slotData) -> FieldReadRegisters {
        return {
            .field = { .object = object, .slotData = slotData, .extensionWordOffset = T11, .address = T11, .representation = T14 },
            .value = T10,
            .result = R0,
            .number = jit.allocateScratch(FPRInfo::fpRegT0),
            .numberScratch = jit.allocateScratch(FPRInfo::fpRegT1),
        };
    };
    // Goes on where the slot of the field is empty.
    auto useFieldAndReturn = [&](const FieldAddressRegisters& field, JumpList& isAbsent, auto useAddressAndReturn) {
        Jump isOutOfLine = slot.branchIfFlag(jit, Slot::isOutOfLine);
        computeFieldAddress(jit, field, false, isAbsent);
        Label hasAddress = jit.label();
        useAddressAndReturn();

        isOutOfLine.link(&jit);
        if (slot.kind() == Slot::Kind::FieldOrInherited)
            loadExtensionWordOffset(jit, R0, field.extensionWordOffset);
        else
            slot.loadHiddenWordOffset(jit, field.extensionWordOffset);
        computeFieldAddress(jit, field, true, isAbsent);
        jit.jump().linkTo(hasAddress, &jit);

        isAbsent.link(&jit);
    };
    // The value of a data property does not depend on the receiver, so the holder takes the receiver's place and the lookup tables answer for its own Structure.
    auto readFromHolder = [&] {
        slot.loadHolder(jit, R0);
        switch (origin) {
        case SlotOrigin::ById:
        case SlotOrigin::ByIdInStubFrame: {
            const CallerFrame callerFrame = callerFrameOf(origin);
            const StubKey frontProbe = getById().withCallerFrame(callerFrame);
            const StubSignature signature = signatureOfGetById(frontProbe);
            RELEASE_ASSERT(signature.operands[receiverOperand] == Reg(R0) && signature.immediate == operands.identifierNumber);
            if (slot.form() == SlotForm::Slot)
                jit.linkToStub(jit.branch32(CCallHelpers::Above, operands.identifierNumber, TrustedImm32(RowDisplacementTable::maxColumn)), getByIdHashed().withCallerFrame(callerFrame));
            jit.jumpToStub(frontProbe);
            return;
        }
        case SlotOrigin::ByVal: {
            // The probe by key clobbers like a call, and a handler does not.
            const GPRReg uid = T10;
            const GPRReg structureID = T11;
            const GPRReg header = jit.clobberOperand(headerOperand);
            const GPRReg payload = clobberPayloadRegister();
            loadNameOfKey(jit, operands.keyCell, uid, structureID, miss);
            jit.load32(Address(R0, JSCell::structureIDOffset()), structureID);
            findHashedSlot(jit, SlotAccess::Read, { .structureID = structureID, .uid = uid, .entry = T14, .header = header, .payload = payload, .scratch = payload }, miss);
            jumpToSlotHandler(jit, table, header, uid);
            return;
        }
        case SlotOrigin::FreeName:
        case SlotOrigin::FreeNameForTypeOf:
            break;
        }
        RELEASE_ASSERT_NOT_REACHED();
    };

    auto emitReadBySlot = [&] {
        if (!hasReadArm(traits, slot.kind())) {
            miss.append(jit.jump());
            return;
        }
        switch (slot.kind()) {
        case Slot::Kind::Empty: {
            // A cell of the front that is only reserved for its name has no slot, and a slot that does not pack is in the hashed table.
            const StubKey hashedProbe = getByIdHashed().withCallerFrame(callerFrameOf(origin));
            const StubSignature signature = signatureOfGetByIdHashed(hashedProbe);
            RELEASE_ASSERT(signature.operands[receiverOperand] == Reg(R0) && signature.immediate == operands.identifierNumber);
            jit.jumpToStub(hashedProbe);
            return;
        }
        case Slot::Kind::Value:
            loadValueBySlot(jit, slot, R0, T10, T11, R0);
            jit.returnToCaller();
            return;
        case Slot::Kind::FarValue: {
            const GPRReg butterfly = T10;
            const GPRReg wordIndex = T11;
            slot.loadFirstPayloadWord(jit, wordIndex);
            jit.signExtend32ToPtr(wordIndex, wordIndex);
            jit.loadPtr(Address(R0, JSObject::butterflyOffset()), butterfly);
            jit.load64(BaseIndex(butterfly, wordIndex, CCallHelpers::TimesEight), R0);
            jit.returnToCaller();
            return;
        }
        case Slot::Kind::Absent:
            if (origin == SlotOrigin::ByVal)
                rememberAbsenceOfKey(jit, operands.keyCell, slot.header(), T10, T11);
            returnUndefined();
            return;
        case Slot::Kind::Getter:
            loadValueBySlot(jit, slot, R0, T10, T11, T10);
            callGetter(T10);
            return;
        case Slot::Kind::CustomGetter: {
            const GPRReg accessorIndex = clobberPayloadRegister();
            const GPRReg uid = T10;
            slot.loadFirstPayloadWord(jit, accessorIndex);
            loadPropertyName(uid);
            callCustomGetterAndReturn(jit, R0, accessorIndex, uid, origin == SlotOrigin::ByVal ? std::optional<GPRReg>(operands.keyCell) : std::nullopt);
            return;
        }
        case Slot::Kind::Field:
        case Slot::Kind::FieldOrInheritedField:
        case Slot::Kind::FieldOrInherited: {
            const FieldReadRegisters registers = allocateFieldReadRegisters(R0, slot.registerOfFirstPayloadWord());
            FieldReadExits exits;
            useFieldAndReturn(registers.field, exits.isAbsent, [&] {
                readFieldAndReturn(jit, registers, exits);
            });
            if (slot.kind() == Slot::Kind::FieldOrInheritedField)
                readFromHolder();
            else if (slot.kind() == Slot::Kind::FieldOrInherited) {
                const GPRReg inherited = T10;
                loadInheritedValueBySlot(jit, slot, T10, T11, inherited);
                Jump isGetter = slot.branchIfFlag(jit, Slot::inheritsGetter);
                Jump isAbsentWhereEmpty = slot.branchUnlessFlag(jit, Slot::emptySlotMayBeUndefined);
                uncached.append(jit.branchIfNotUndefined(inherited));
                isAbsentWhereEmpty.link(&jit);
                jit.move(inherited, R0);
                jit.returnToCaller();

                isGetter.link(&jit);
                callGetter(inherited);
            } else {
                // No cell of the front table has room for the second flag.
                Jump isUndefinedEitherWay;
                if (slot.form() == SlotForm::Slot)
                    isUndefinedEitherWay = slot.branchIfFlag(jit, Slot::emptySlotMayBeUndefined);
                uncached.append(slot.branchUnlessFlag(jit, Slot::absentIsUndefined));
                if (isUndefinedEitherWay.isSet())
                    isUndefinedEitherWay.link(&jit);
                returnUndefined();
            }
            linkToSlotCannot(jit, exits.cannot, table, SlotFinisher::ReadField, slot);
            return;
        }
        case Slot::Kind::FieldGetter: {
            // The slot of an accessor field holds the GetterSetter. Many an object of the same Structure has it empty: the accessor is defined on some objects of the layout only.
            const FieldAddressRegisters field { .object = R0, .slotData = T10, .extensionWordOffset = T11, .address = T11, .representation = T14 };
            const GPRReg getterSetter = T10;
            JumpList isAbsent;
            slot.loadFirstPayloadWord(jit, field.slotData);
            useFieldAndReturn(field, isAbsent, [&] {
                loadGetterSetterInField(jit, field.address, field.representation, getterSetter, isAbsent);
                callGetter(getterSetter);
            });
            uncached.append(slot.branchUnlessFlag(jit, Slot::absentIsUndefined));
            returnUndefined();
            return;
        }
        case Slot::Kind::InheritedField:
            readFromHolder();
            return;
        case Slot::Kind::Variable: {
            const GPRReg statics = jit.clobberOperand(headerOperand);
            const FieldReadRegisters registers = allocateFieldReadRegisters(statics, T10);
            FieldReadExits exits;
            slot.loadFirstPayloadWord(jit, registers.field.slotData);
            loadStaticsBase(jit, statics, T11);
            computeFieldAddress(jit, registers.field, false, exits.isAbsent);
            readFieldAndReturn(jit, registers, exits);

            uncached.append(exits.isAbsent);
            uncached.append(exits.cannot);
            return;
        }
        case Slot::Kind::ExtraProperty:
            readExtraPropertyAndReturn(jit, slot, extraPropertyRegisters, R0, miss);
            return;
        case Slot::Kind::ExtraGetter:
            locateExtraProperty(jit, slot, extraPropertyRegisters, miss);
            jit.load64(BaseIndex(extraPropertyRegisters.extraProperties, extraPropertyRegisters.wordIndex, CCallHelpers::TimesEight), T10);
            callGetter(T10);
            return;
        case Slot::Kind::ArrayLength:
            jit.loadPtr(Address(R0, JSObject::butterflyOffset()), T10);
            jit.load32(Address(T10, Butterfly::offsetOfPublicLength()), T10);
            uncached.append(jit.branch32(CCallHelpers::LessThan, T10, TrustedImm32(0)));
            returnInt32(T10);
            return;
        case Slot::Kind::TypedArrayLength:
            uncached.append(jit.branchTest8(CCallHelpers::NonZero, Address(R0, JSArrayBufferView::offsetOfMode()), TrustedImm32(resizabilityAndAutoLengthMask)));
            jit.load64(Address(R0, JSArrayBufferView::offsetOfLength()), T10);
            uncached.append(jit.branch64(CCallHelpers::Above, T10, TrustedImm64(std::numeric_limits<int32_t>::max())));
            returnInt32(T10);
            return;
        case Slot::Kind::StringLength: {
            jit.loadPtr(Address(R0, JSString::offsetOfValue()), T11);
            Jump isRope = jit.branchIfRopeStringImpl(T11);
            jit.load32(Address(T11, StringImpl::lengthMemoryOffset()), T11);
            returnInt32(T11);

            isRope.link(&jit);
            jit.load32(Address(R0, JSRopeString::offsetOfLength()), T11);
            returnInt32(T11);
            return;
        }
        case Slot::Kind::ClosureLength:
            readClosureLengthAndReturn(jit, R0, miss);
            return;
        case Slot::Kind::ClosureName:
            readClosureNameAndReturn(jit, R0, miss);
            return;
        case Slot::Kind::Constant:
            slot.loadPayloadBits(jit, R0);
            jit.returnToCaller();
            return;
        case Slot::Kind::Uncacheable:
            if (jit.options().callsTrapsOfProxies && slot.form() == SlotForm::Slot && (origin == SlotOrigin::ById || origin == SlotOrigin::ByVal))
                jit.linkToStub(jit.branchTest64(CCallHelpers::NonZero, operands.payload), throughProxy(TrapOfProxy::Get));
            uncached.append(jit.jump());
            return;
        case Slot::Kind::Setter:
        case Slot::Kind::CustomSetter:
        case Slot::Kind::FieldCreation:
        case Slot::Kind::FieldOrInheritedSetter:
        case Slot::Kind::ExtraSetter:
        case Slot::Kind::ExtraPropertyAddition:
        case Slot::Kind::Transition:
        case Slot::Kind::Removal:
        case Slot::Kind::ExtraPropertyRemoval:
        case Slot::Kind::FieldRemoval:
        case Slot::Kind::NothingToRemove:
            break;
        }
        RELEASE_ASSERT_NOT_REACHED();
    };
    emitReadBySlot();
    linkToSlotExit(jit, miss, table, SlotExit::Miss);
    linkToSlotExit(jit, uncached, table, SlotExit::Uncached);
}

StubSignature signatureOfThroughProxy(StubKey key)
{
    if (static_cast<TrapOfProxy>(key.form()) == TrapOfProxy::Set)
        return StubSignature::withoutResult({ gprOf(firstStubOperandRegister), gprOf(slotPayloadRegister), gprOf(valueStoredThroughProxyRegister), gprOf(StubRegister::T14) });
    if (static_cast<TrapOfProxy>(key.form()) == TrapOfProxy::Get)
        return StubSignature::withGPRResult({ gprOf(firstStubOperandRegister), gprOf(slotPayloadRegister), gprOf(slotHeaderRegister) });
    return StubSignature::withGPRResult({ gprOf(firstStubOperandRegister), gprOf(slotPayloadRegister) });
}

// As ProxyObject.cpp performProxyGet(), performHasProperty() and performPut(). The trap is read off the handler and called as compiled code reads and calls.
void generateThroughProxy(StubAssembler& jit)
{
    static_assert(numberOfArgumentGPRs >= 4 && !(sizeof(ProxyAccessWords) % stackAlignmentBytes()));
    const TrapOfProxy trap = static_cast<TrapOfProxy>(jit.key().form());
    const bool isStore = trap == TrapOfProxy::Set;
    const GPRReg R0 = jit.operand(0);
    const GPRReg key = jit.operand(1);
    const GPRReg handler = isStore ? jit.clobberOperand(2) : jit.allocateScratch(valueStoredThroughProxyRegister);
    const GPRReg target = jit.allocateScratch(StubRegister::T11);
    const GPRReg scratch = isStore ? jit.clobberOperand(3) : jit.allocateScratch(StubRegister::T14);
    const GPRReg name = jit.clobberRegister(slotNameRegister);
    RELEASE_ASSERT(noOverlap(R0, key, handler, target, scratch, name) && (isStore || jit.result() == R0));
    struct Operations {
        unsigned numberOfArguments;
        Entry withoutFunction;
        Entry afterCall;
        Entry whole;
        uint32_t bitsOfTargetThatMayBind;
    };
    const Operations operations = [&]() -> Operations {
        switch (trap) {
        case TrapOfProxy::Get:
            return { 3, Entry::operationAOTGetWithProxyTrap, Entry::operationAOTValidateAnswerOfProxyTrap, Entry::operationAOTGetByVal, Structure::s_hasNonConfigurableReadOnlyOrGetterSetterPropertiesBits };
        case TrapOfProxy::Has:
            return { 2, Entry::operationAOTHasWithProxyTrap, Entry::operationAOTFinishHasThroughProxy, Entry::operationAOTInByValAndCache, Structure::s_didPreventExtensionsBits | Structure::s_hasNonConfigurablePropertiesBits };
        case TrapOfProxy::Set:
            return { 4, Entry::operationAOTSetWithProxyTrap, Entry::operationAOTFinishSetThroughProxy, Entry::operationAOTPutByVal, Structure::s_hasNonConfigurableReadOnlyOrGetterSetterPropertiesBits };
        }
        RELEASE_ASSERT_NOT_REACHED();
    }();
    auto inProxy = [&](ProxyObject::Field field) {
        return Address(R0, ProxyObject::offsetOfInternalField(static_cast<unsigned>(field)));
    };
    auto kept = [&](ptrdiff_t offset) {
        return Address(sp, static_cast<int32_t>(offset));
    };

    jit.checkStackBeforeEntering();
    jit.callAndReturnPreservingRegisters({ .callee = StubCallee::CompiledCode, .returnsValue = !isStore, .checksException = true }, [&] {
        JumpList isDone;
        JumpList isNoFunction;
        JumpList runTimeDoesAll;
        JumpList runTimeFinishes;
        auto callOperation = [&](Entry operation) {
            jit.move(instanceGPR, operationArgumentGPR(0));
            jit.callOperation(operation, jit.reuseScratch(StubRegister::Operation));
        };
        auto callWithWords = [&](Entry operation) {
            jit.move(R0, operationArgumentGPR(2));
            jit.move(sp, operationArgumentGPR(1));
            callOperation(operation);
        };

        jit.subPtr(TrustedImm32(sizeof(ProxyAccessWords)), sp);
        jit.storePtr(R0, kept(OBJECT_OFFSETOF(ProxyAccessWords, proxy)));
        jit.storePtr(key, kept(OBJECT_OFFSETOF(ProxyAccessWords, key)));
        if (isStore) {
            jit.store64(handler, kept(OBJECT_OFFSETOF(ProxyAccessWords, storedValue)));
            jit.store64(scratch, kept(OBJECT_OFFSETOF(ProxyAccessWords, storeIsStrictOrHeaderOfSlot)));
        } else if (trap == TrapOfProxy::Get)
            jit.store64(jit.operand(2), kept(OBJECT_OFFSETOF(ProxyAccessWords, storeIsStrictOrHeaderOfSlot)));
        jit.load64(inProxy(ProxyObject::Field::Handler), handler);
        jit.load64(inProxy(ProxyObject::Field::Target), target);
        jit.load32(Address(instanceGPR, AOT_INSTANCE_OFFSET(propertyCaches.numbersOfNamesOfTraps) + static_cast<int32_t>(static_cast<unsigned>(trap) * sizeof(uint32_t))), name);
        jit.storePtr(target, kept(OBJECT_OFFSETOF(ProxyAccessWords, target)));
        jit.storePtr(handler, kept(OBJECT_OFFSETOF(ProxyAccessWords, handler)));
        // It is revoked.
        runTimeDoesAll.append(jit.branchIfNotCell(handler));
        runTimeDoesAll.append(jit.branchTest32(CCallHelpers::Zero, name));
        jit.move(handler, R0);
        jit.callStub(getById().withCallerFrame(CallerFrame::StubFrame));
        isNoFunction.append(jit.branchIfNotCell(R0));
        isNoFunction.append(branchIfNotFunctionType(jit, R0, scratch));

        jit.move(R0, calleeGPR);
        jit.loadPtr(kept(OBJECT_OFFSETOF(ProxyAccessWords, handler)), thisGPR);
        jit.loadPtr(kept(OBJECT_OFFSETOF(ProxyAccessWords, target)), argumentGPR(0));
        jit.loadPtr(kept(OBJECT_OFFSETOF(ProxyAccessWords, key)), argumentGPR(1));
        if (isStore)
            jit.load64(kept(OBJECT_OFFSETOF(ProxyAccessWords, storedValue)), argumentGPR(2));
        if (trap != TrapOfProxy::Has)
            jit.loadPtr(kept(OBJECT_OFFSETOF(ProxyAccessWords, proxy)), argumentGPR(operations.numberOfArguments - 1));
        jit.callStub(callValueCell(operations.numberOfArguments));
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(vm)), scratch);
        isDone.append(jit.branchTestPtr(CCallHelpers::NonZero, Address(scratch, VM::exceptionOffset())));
        Jump saysYes;
        if (trap == TrapOfProxy::Has) {
            saysYes = jit.branch64(CCallHelpers::Equal, R0, TrustedImm64(JSValue::ValueTrue));
            runTimeFinishes.append(jit.branch64(CCallHelpers::NotEqual, R0, TrustedImm64(JSValue::ValueFalse)));
        } else if (isStore)
            runTimeFinishes.append(jit.branch64(CCallHelpers::NotEqual, R0, TrustedImm64(JSValue::ValueTrue)));
        // Only a target that is not extensible or has a property that is not configurable binds what the trap may answer.
        JumpList bindsNothing;
        jit.loadPtr(kept(OBJECT_OFFSETOF(ProxyAccessWords, target)), target);
        loadStructure(jit, target, scratch);
        bindsNothing.append(jit.branchTest32(CCallHelpers::Zero, Address(scratch, Structure::bitFieldOffset()), TrustedImm32(operations.bitsOfTargetThatMayBind)));
        // The Structure of a compiled object says yes for all of them. What was done to one object stands in the object, and what its fields are born with in the layout.
        runTimeFinishes.append(jit.branchIfNotType(target, CompiledObjectType));
        runTimeFinishes.append(jit.branchTest8(CCallHelpers::Zero, Address(scratch, CompiledStructure::offsetOfFlags()), TrustedImm32(static_cast<uint8_t>(CompiledStructure::Flag::EveryFieldIsBornConfigurable))));
        bindsNothing.append(jit.branchTest32(CCallHelpers::Zero, Address(target, offsetOfOrderWord), TrustedImm32(~orderAndCountMask)));
        runTimeFinishes.link(&jit);
        callWithWords(operations.afterCall);
        isDone.append(jit.jump());

        if (trap == TrapOfProxy::Has) {
            saysYes.link(&jit);
            jit.move(TrustedImm32(1), R0);
            isDone.append(jit.jump());
            bindsNothing.link(&jit);
            jit.move(TrustedImm32(0), R0);
            isDone.append(jit.jump());
        } else
            isDone.append(bindsNothing);

        isNoFunction.link(&jit);
        if (trap == TrapOfProxy::Get) {
            // Without a trap the target answers. A data property is the same for every receiver, so what is filed for the target says it.
            const StubSignature probe = signatureOfTryGetSuperById(tryGetSuperById());
            RELEASE_ASSERT(probe.immediate && probe.operands[0] == Reg(R0) && *probe.immediate != R0);
            const GPRReg column = *probe.immediate;
            JumpList tablesDoNotSay;
            tablesDoNotSay.append(jit.branchIfNotUndefined(R0));
            jit.load16(kept(OBJECT_OFFSETOF(ProxyAccessWords, storeIsStrictOrHeaderOfSlot) + Slot::offsetOfLocation()), column);
            tablesDoNotSay.append(jit.branchTest32(CCallHelpers::Zero, column));
            jit.loadPtr(kept(OBJECT_OFFSETOF(ProxyAccessWords, target)), R0);
            jit.callStub(tryGetSuperById());
            isDone.append(jit.branchTest64(CCallHelpers::NonZero, R0));
            jit.move(TrustedImm64(JSValue::ValueUndefined), R0);
            tablesDoNotSay.link(&jit);
        }
        callWithWords(operations.withoutFunction);
        isDone.append(jit.jump());

        runTimeDoesAll.link(&jit);
        jit.loadPtr(kept(OBJECT_OFFSETOF(ProxyAccessWords, proxy)), operationArgumentGPR(1));
        jit.loadPtr(kept(OBJECT_OFFSETOF(ProxyAccessWords, key)), operationArgumentGPR(2));
        if (isStore) {
            jit.load64(kept(OBJECT_OFFSETOF(ProxyAccessWords, storedValue)), operationArgumentGPR(3));
            jit.load32(kept(OBJECT_OFFSETOF(ProxyAccessWords, storeIsStrictOrHeaderOfSlot)), operationArgumentGPR(4));
        }
        callOperation(operations.whole);

        isDone.link(&jit);
        jit.addPtr(TrustedImm32(sizeof(ProxyAccessWords)), sp);
    });
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
    return { .saves = OperationCall::Saves::CallerSavedRegisters, .secondArgumentIn = second, .thirdArgumentIn = third, .fourthArgumentIn = fourth };
}

static void tryLeafLookup(StubAssembler& jit, Entry leafLookup, GPRReg second)
{
    const GPRReg receiver = jit.operand(receiverOperand);
    const GPRReg receiverAfterCall = jit.reuseScratch(StubRegister::T12);
    callLeafLookup(jit, leafLookup, second, std::nullopt, receiverAfterCall);
    Jump hasNoAnswer = jit.branchTest64(CCallHelpers::Zero, receiver);
    jit.returnToCaller();

    hasNoAnswer.link(&jit);
    jit.move(receiverAfterCall, receiver);
}

static void emitUncachedReadById(StubAssembler& jit, GPRReg receiver, GPRReg identifierNumber)
{
    const GPRReg uid = jit.allocateScratch(StubRegister::T10);
    JumpList needsOperation;
    needsOperation.append(jit.branchIfNotCell(receiver));
    needsOperation.append(jit.branchIfNotObject(receiver));
    // Its handler answers, and only the operation asks it.
    needsOperation.append(jit.branchIfType(receiver, ProxyObjectType));
    loadIdentifier(jit, identifierNumber, uid, needsOperation);
    // No slot says what the read finds, as where a getter lies in a field of an object above the receiver. The objects themselves say it.
    JumpList objectsDoNotSay;
    answerFromObjectsAndReturn(jit, ObjectLookupQuestion::Read, {
        .receiver = receiver,
        .name = identifierNumber,
        .uid = uid,
        .object = jit.allocateScratch(StubRegister::T12),
        .structure = jit.allocateScratch(StubRegister::T11),
        .size = jit.allocateScratch(StubRegister::T13),
        .vector = jit.allocateScratch(StubRegister::T14),
        .index = indexRegisterOfObjectLookup(jit),
    }, ObjectLookupName::IsGivenByNumber, objectsDoNotSay);
    objectsDoNotSay.link(&jit);
    loadIdentifier(jit, identifierNumber, uid, needsOperation);
    tryLeafLookup(jit, Entry::operationAOTGetGeneric, uid);

    needsOperation.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTGetById, createExitCall(receiver, identifierNumber));
}

void generateReadSlotExit(StubAssembler& jit)
{
    const ReadSlotExitTraits traits = traitsOfReadSlotExit(jit.key());
    const SlotOrigin origin = traitsOf(traits.table).origin;
    const GPRReg receiver = jit.operand(receiverOperand);
    const GPRReg name = jit.operand(nameOperandOfExit);
    switch (origin) {
    case SlotOrigin::ById:
    case SlotOrigin::ByIdInStubFrame:
        switch (traits.exit) {
        case SlotExit::Miss: {
            const GPRReg noSite = jit.allocateScratch(StubRegister::T10);
            jit.move(CCallHelpers::TrustedImmPtr(nullptr), noSite);
            jit.exitOnMiss(Entry::operationAOTGetByIdAndCache, createExitCall(receiver, name, noSite));
            return;
        }
        case SlotExit::Uncached:
            emitUncachedReadById(jit, receiver, name);
            return;
        }
        break;
    case SlotOrigin::ByVal:
        switch (traits.exit) {
        case SlotExit::Miss:
            jit.exitOnMiss(Entry::operationAOTGetByValAndCache, createExitCall(receiver, name));
            return;
        case SlotExit::Uncached:
            tryLeafLookup(jit, Entry::operationAOTGetByValGeneric, name);
            jit.callOperationAndReturn(Entry::operationAOTGetByVal, createExitCall(receiver, name));
            return;
        }
        break;
    case SlotOrigin::FreeName:
    case SlotOrigin::FreeNameForTypeOf: {
        const Entry operation = origin == SlotOrigin::FreeName ? Entry::operationAOTGetFreeName : Entry::operationAOTGetFreeNameForTypeOf;
        const OperationCall call = createExitCall(name, jit.operand(siteOperandOfExit));
        switch (traits.exit) {
        case SlotExit::Miss:
            jit.exitOnMiss(operation, call);
            return;
        case SlotExit::Uncached:
            jit.callOperationAndReturn(operation, call);
            return;
        }
        break;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
