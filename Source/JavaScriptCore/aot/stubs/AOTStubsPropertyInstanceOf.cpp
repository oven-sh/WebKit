/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsPropertyProbes.h"

#if ENABLE(AOT)

#include "AOTConvention.h"
#include "AOTInstance.h"
#include "AOTStubsCalls.h"
#include "AOTStubsConstants.h"
#include "AOTStubsGlobalBindings.h"
#include "AOTStubsValues.h"
#include "JSObject.h"
#include "JSTypeInfo.h"
#include "Structure.h"

namespace JSC { namespace AOT {

static void readConstructorOffGlobalNameAndGoOn(StubAssembler& jit, GlobalBindingScope scope)
{
    const GPRReg value = jit.operand(0);
    const GPRReg constructor = jit.clobberRegister(jit.immediate());
    const GPRReg number = jit.allocateScratch(StubRegister::T11);
    const GPRReg index = jit.allocateScratch(StubRegister::T12);
    const GPRReg record = index;
    const GPRReg global = jit.allocateScratch(StubRegister::T10);
    JumpList asksStubOfBinding;

    static_assert(sizeof(GlobalBinding) == 3 * sizeof(uint64_t) && !OBJECT_OFFSETOF(GlobalBinding, constant));
    jit.zeroExtend32ToWord(jit.immediate(), number);
    jit.getEffectiveAddress(BaseIndex(number, number, CCallHelpers::TimesTwo), index);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(runtime.bindings.globalBindings)), constructor);
    jit.load64(BaseIndex(constructor, index, CCallHelpers::TimesEight), constructor);
    Jump hasNoConstant = jit.branchTest64(CCallHelpers::Zero, constructor);
    jit.jumpToStub(instanceOf(InstanceOfOperand::Constructor));

    hasNoConstant.link(&jit);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(runtime.bindings.globalBindings)), constructor);
    jit.getEffectiveAddress(BaseIndex(constructor, index, CCallHelpers::TimesEight), record);
    loadValueAtLocationOfGlobalBinding(jit, scope == GlobalBindingScope::ScriptsAndGlobalObject, record, global, constructor, asksStubOfBinding);
    jit.jumpToStub(instanceOf(InstanceOfOperand::Constructor));

    const StubKey read = getGlobalBinding(scope);
    const StubSignature signatureOfRead = signatureOf(read);
    asksStubOfBinding.link(&jit);
    jit.emitFunctionPrologue();
    jit.preserving({ value }, [&] {
        jit.move(number, *signatureOfRead.immediate);
        jit.callStub(read);
        jit.move(signatureOfRead.firstResult.gpr(), constructor);
    });
    jit.emitFunctionEpilogue();
    jit.jumpToStub(instanceOf(InstanceOfOperand::Constructor));
}

void generateInstanceOf(StubAssembler& jit)
{
    const auto operand = static_cast<InstanceOfOperand>(jit.key().form());
    if (operand == InstanceOfOperand::NameSeenByScripts || operand == InstanceOfOperand::NameOfGlobalObject) {
        readConstructorOffGlobalNameAndGoOn(jit, operand == InstanceOfOperand::NameSeenByScripts ? GlobalBindingScope::ScriptsAndGlobalObject : GlobalBindingScope::GlobalObject);
        return;
    }
    const bool readsPrototypeOutOfCell = operand == InstanceOfOperand::PrototypeThatIsBuiltin;
    const bool givesWayAtTraps = operand == InstanceOfOperand::PrototypeWithoutTraps;
    const bool isGivenPrototype = operand == InstanceOfOperand::Prototype || givesWayAtTraps;
    const GPRReg prototype = readsPrototypeOutOfCell ? jit.clobberRegister(jit.immediate()) : isGivenPrototype ? jit.operand(1) : jit.clobberOperand(1);
    const GPRReg constructor = prototype;
    const GPRReg current = jit.allocateScratch(StubRegister::T11);
    const GPRReg next = jit.allocateScratch(StubRegister::T12);
    const GPRReg value = jit.operand(0);
    JumpList isInstance;
    JumpList notInstance;
    JumpList slowCases;
    JumpList startsOverWithPrototype;
    JumpList cellIsNotMade;

    auto readPrototypeOutOfCell = [&] {
        jit.zeroExtend32ToWord(jit.immediate(), current);
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_PREFIX_OFFSET(builtinCells)), prototype);
        jit.load64(BaseIndex(prototype, current, CCallHelpers::TimesEight), prototype);
        cellIsNotMade.append(jit.branchTest64(CCallHelpers::Zero, prototype));
    };
    auto returnBoolean = [&](bool result) {
        jit.move(TrustedImm32(result), jit.result());
        jit.returnToCaller();
    };
    auto followPrototypeChainAndReturn = [&] {
        struct Link {
            Jump hasPolyProto;
            Label hasPrototype;
            Jump goesOn;
        };
        auto followLink = [&](GPRReg object, bool standsAheadOfPrototype) {
            Link link;
            (standsAheadOfPrototype ? startsOverWithPrototype : slowCases).append(jit.branchTest8(CCallHelpers::NonZero, Address(object, JSObject::typeInfoFlagsOffset()), TrustedImm32(OverridesGetPrototype)));
            loadStructure(jit, object, next);
            jit.load64(Address(next, Structure::prototypeOffset()), next);
            link.hasPolyProto = jit.branchTest64(CCallHelpers::Zero, next);
            link.hasPrototype = jit.label();
            if (standsAheadOfPrototype) {
                Jump liesBelowAnotherObject = jit.branch64(CCallHelpers::NotEqual, next, Address(instanceGPR, AOT_INSTANCE_OFFSET(realm.objectPrototype)));
                returnBoolean(false);
                liesBelowAnotherObject.link(&jit);
                readPrototypeOutOfCell();
                isInstance.append(jit.branch64(CCallHelpers::Equal, next, prototype));
                return link;
            }
            isInstance.append(jit.branch64(CCallHelpers::Equal, next, prototype));
            link.goesOn = jit.branch64(CCallHelpers::NotEqual, next, Address(instanceGPR, AOT_INSTANCE_OFFSET(realm.objectPrototype)));
            returnBoolean(false);
            return link;
        };
        auto readPolyProto = [&](Link& link, GPRReg object) {
            link.hasPolyProto.link(&jit);
            jit.load64(Address(object, offsetRelativeToBase(knownPolyProtoOffset)), next);
            jit.jump().linkTo(link.hasPrototype, &jit);
        };
        Link first = followLink(value, readsPrototypeOutOfCell);
        if (!readsPrototypeOutOfCell)
            first.goesOn.link(&jit);
        Label nextLink = jit.label();
        notInstance.append(jit.branchIfNotCell(next));
        jit.move(next, current);
        Link further = followLink(current, false);
        further.goesOn.linkTo(nextLink, &jit);
        readPolyProto(first, value);
        readPolyProto(further, current);

        notInstance.link(&jit);
        returnBoolean(false);
        isInstance.link(&jit);
        returnBoolean(true);
        slowCases.link(&jit);
        if (givesWayAtTraps) {
            jit.move(TrustedImm32(answerOfChainWalkAtTrap), jit.result());
            jit.returnToCaller();
        } else
            jit.callOperationAndReturn(Entry::operationAOTDefaultHasInstance, { .secondArgumentIn = value, .thirdArgumentIn = prototype });
        if (!readsPrototypeOutOfCell)
            return;

        startsOverWithPrototype.link(&jit);
        readPrototypeOutOfCell();
        jit.jumpToStub(instanceOf(InstanceOfOperand::Prototype));

        const StubSignature signatureOfMaker = signatureOf(builtinCell());
        cellIsNotMade.link(&jit);
        jit.emitFunctionPrologue();
        jit.preserving({ value }, [&] {
            jit.move(current, *signatureOfMaker.immediate);
            jit.callStub(builtinCell());
            jit.move(signatureOfMaker.firstResult.gpr(), prototype);
        });
        jit.emitFunctionEpilogue();
        jit.jumpToStub(instanceOf(InstanceOfOperand::Prototype));
    };

    if (isGivenPrototype || readsPrototypeOutOfCell) {
        notInstance.append(jit.branchIfNotCell(value));
        notInstance.append(jit.branchIfNotObject(value));
        followPrototypeChainAndReturn();
        return;
    }

    const GPRReg method = jit.allocateScratch(StubRegister::T10);
    const GPRReg payload = jit.allocateScratch(slotPayloadRegister);
    const GPRReg place = jit.allocateScratch(StubRegister::A2);
    const HashedProbeRegisters probe { current, method, jit.allocateScratch(StubRegister::T14), next, payload, payload };
    const CachedSlot valueSlot(SlotForm::Slot, Slot::Kind::Value, probe.header, probe.payload);
    const BaseIndex headerAtPlace(instanceGPR, place, CCallHelpers::TimesEight, AOT_INSTANCE_OFFSET(propertyCaches.headersOfPrototypeSlotsOfOrdinaryConstructors));
    JumpList miss;

    struct SlotRead {
        GPRReg result;
        Jump isNotValue;
        Label isDone;
    };
    auto readFromConstructor = [&](GPRReg result, bool remembersOwnSlot) {
        SlotRead read { result, { }, { } };
        jit.load32(Address(constructor, JSCell::structureIDOffset()), probe.structureID);
        findHashedSlot(jit, SlotAccess::Read, probe, miss);
        read.isNotValue = branchIfSlotKindDiffers(jit, { SlotForm::Slot, probe.header, probe.payload }, Slot::Kind::Value, probe.uid);
        loadValueBySlot(jit, valueSlot, constructor, probe.structureID, probe.entry, result);
        if (remembersOwnSlot) {
            Jump isInherited = valueSlot.branchUnlessFlag(jit, Slot::isOwn);
            jit.store64(probe.header, headerAtPlace);
            isInherited.link(&jit);
        }
        read.isDone = jit.label();
        return read;
    };
    auto emitFarValueRead = [&](const SlotRead& read) {
        const GPRReg storage = probe.structureID;
        const GPRReg wordIndex = probe.entry;
        read.isNotValue.link(&jit);
        miss.append(branchIfSlotKindDiffers(jit, { SlotForm::Slot, probe.header, probe.payload }, Slot::Kind::FarValue, probe.uid));
        CachedSlot(SlotForm::Slot, Slot::Kind::FarValue, probe.header, probe.payload).loadFirstPayloadWord(jit, wordIndex);
        jit.signExtend32ToPtr(wordIndex, wordIndex);
        jit.loadPtr(Address(constructor, JSObject::butterflyOffset()), storage);
        jit.load64(BaseIndex(storage, wordIndex, CCallHelpers::TimesEight), read.result);
        jit.jump().linkTo(read.isDone, &jit);
    };

    miss.append(jit.branchIfNotCell(constructor));
    jit.load32(Address(constructor, JSCell::structureIDOffset()), probe.structureID);
    jit.mul32(TrustedImm32(static_cast<int32_t>(HashedSlot::structureMultiplier)), probe.structureID, place);
    jit.urshift32(TrustedImm32(32 - PlaceOfOrdinaryConstructor::numberOfPlacesLog2), place);
    jit.load64(headerAtPlace, probe.header);
    Jump isNotRemembered = jit.branch32(CCallHelpers::NotEqual, probe.header, probe.structureID);
    notInstance.append(jit.branchIfNotCell(value));
    notInstance.append(jit.branchIfNotObject(value));
    loadOwnValueBySlot(jit, valueSlot, constructor, probe.structureID, probe.header, prototype);
    Label hasReadPrototype = jit.label();
    slowCases.append(jit.branchIfNotCell(prototype));
    slowCases.append(jit.branchIfNotObject(prototype));
    followPrototypeChainAndReturn();

    isNotRemembered.link(&jit);
    miss.append(jit.branchIfNotObject(constructor));
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(propertyCaches.hasInstanceUID)), probe.uid);
    SlotRead methodRead = readFromConstructor(method, false);
    Jump isNotDefaultMethod = jit.branchPtr(CCallHelpers::NotEqual, method, Address(instanceGPR, AOT_INSTANCE_OFFSET(propertyCaches.defaultHasInstanceFunction)));
    miss.append(jit.branchTest8(CCallHelpers::Zero, Address(constructor, JSCell::typeInfoFlagsOffset()), TrustedImm32(ImplementsDefaultHasInstance)));
    Jump methodCannotChange = jit.branchPtr(CCallHelpers::Equal, probe.payload, Address(instanceGPR, AOT_INSTANCE_OFFSET(propertyCaches.holderOfDefaultHasInstanceFunction)));
    jit.move(TrustedImm32(PlaceOfOrdinaryConstructor::placeThatIsNeverRead), place);
    methodCannotChange.link(&jit);

    JumpList isNoObject;
    isNoObject.append(jit.branchIfNotCell(value));
    isNoObject.append(jit.branchIfNotObject(value));
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(propertyCaches.prototypeUID)), probe.uid);
    SlotRead prototypeRead = readFromConstructor(prototype, true);
    jit.jump().linkTo(hasReadPrototype, &jit);
    isNoObject.link(&jit);
    returnBoolean(false);

    isNotDefaultMethod.link(&jit);
    const GPRReg argument = jit.reuseScratch(StubRegister::A0);
    RELEASE_ASSERT(method == calleeGPR && argument == argumentGPR(0));
    miss.append(jit.branchIfNotCell(method));
    miss.append(branchIfNotFunctionType(jit, method, current));
    jit.emitFunctionPrologue();
    jit.recordClobbers(RegisterSet { thisGPR });
    // In this order: on x86-64 thisGPR is the register of value.
    jit.move(value, argument);
    jit.move(constructor, thisGPR);
    jit.callStub(callValue(1));
    jit.emitFunctionEpilogue();
    jit.jumpToStub(toBoolean());

    emitFarValueRead(methodRead);
    emitFarValueRead(prototypeRead);

    miss.link(&jit);
    jit.exitOnMiss(Entry::operationAOTInstanceOfAndCache, { .secondArgumentIn = value, .thirdArgumentIn = constructor });
}

StubSignature signatureOfInstanceOf(StubKey key)
{
    if (static_cast<InstanceOfOperand>(key.form()) <= InstanceOfOperand::PrototypeWithoutTraps)
        return StubSignature::withGPRResult({ gprOf(StubRegister::R0), gprOf(StubRegister::A1) });
    StubSignature signature = StubSignature::withGPRResult({ gprOf(StubRegister::R0) });
    signature.immediate = gprOf(StubRegister::A1);
    return signature;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
