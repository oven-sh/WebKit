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
#include "AOTStubsValues.h"
#include "JSObject.h"
#include "JSTypeInfo.h"
#include "Structure.h"

namespace JSC { namespace AOT {

void generateInstanceOf(StubAssembler& jit)
{
    const bool isGivenPrototype = static_cast<InstanceOfOperand>(jit.key().form()) == InstanceOfOperand::Prototype;
    // The prototype of a constructor is read into the register of the constructor.
    const GPRReg prototype = isGivenPrototype ? jit.operand(1) : jit.clobberOperand(1);
    const GPRReg constructor = prototype;
    const GPRReg current = jit.allocateScratch(StubRegister::T11);
    const GPRReg next = jit.allocateScratch(StubRegister::T12);
    const GPRReg value = jit.operand(0);
    JumpList isInstance;
    JumpList notInstance;
    JumpList slowCases;

    auto returnBoolean = [&](bool result) {
        jit.move(TrustedImm32(result), jit.result());
        jit.returnToCaller();
    };
    // value and prototype are intact on slowCases.
    auto followPrototypeChainAndReturn = [&] {
        struct Link {
            Jump hasPolyProto;
            Label hasPrototype;
            Jump goesOn;
        };
        auto followLink = [&](GPRReg object) {
            Link link;
            slowCases.append(jit.branchTest8(CCallHelpers::NonZero, Address(object, JSObject::typeInfoFlagsOffset()), TrustedImm32(OverridesGetPrototype)));
            loadStructure(jit, object, next);
            jit.load64(Address(next, Structure::prototypeOffset()), next);
            link.hasPolyProto = jit.branchTest64(CCallHelpers::Zero, next);
            link.hasPrototype = jit.label();
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
        Link first = followLink(value);
        first.goesOn.link(&jit);
        Label nextLink = jit.label();
        notInstance.append(jit.branchIfNotCell(next));
        jit.move(next, current);
        Link further = followLink(current);
        further.goesOn.linkTo(nextLink, &jit);
        readPolyProto(first, value);
        readPolyProto(further, current);

        notInstance.link(&jit);
        returnBoolean(false);
        isInstance.link(&jit);
        returnBoolean(true);
        slowCases.link(&jit);
        jit.callOperationAndReturn(Entry::operationAOTDefaultHasInstance, { .secondArgumentIn = value, .thirdArgumentIn = prototype });
    };

    if (isGivenPrototype) {
        notInstance.append(jit.branchIfNotCell(value));
        notInstance.append(jit.branchIfNotObject(value));
        followPrototypeChainAndReturn();
        return;
    }

    const GPRReg method = jit.allocateScratch(StubRegister::T10);
    const GPRReg payload = jit.allocateScratch(slotPayloadRegister);
    const HashedProbeRegisters probe { current, method, jit.allocateScratch(StubRegister::T14), next, payload, payload };
    JumpList miss;

    struct SlotRead {
        GPRReg result;
        Jump isNotValue;
        Label isDone;
    };
    // The name is in probe.uid. The constructor is intact on miss and on isNotValue, also where it is the result.
    auto readFromConstructor = [&](GPRReg result) {
        SlotRead read { result, { }, { } };
        jit.load32(Address(constructor, JSCell::structureIDOffset()), probe.structureID);
        findHashedSlot(jit, SlotAccess::Read, probe, miss);
        read.isNotValue = branchIfSlotKindDiffers(jit, { SlotForm::Slot, probe.header, probe.payload }, Slot::Kind::Value, probe.uid);
        loadValueBySlot(jit, CachedSlot(SlotForm::Slot, Slot::Kind::Value, probe.header, probe.payload), constructor, probe.structureID, probe.entry, result);
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
    miss.append(jit.branchIfNotObject(constructor));
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(propertyCaches.hasInstanceUID)), probe.uid);
    SlotRead methodRead = readFromConstructor(method);
    Jump isNotDefaultMethod = jit.branchPtr(CCallHelpers::NotEqual, method, Address(instanceGPR, AOT_INSTANCE_OFFSET(propertyCaches.defaultHasInstanceFunction)));
    miss.append(jit.branchTest8(CCallHelpers::Zero, Address(constructor, JSCell::typeInfoFlagsOffset()), TrustedImm32(ImplementsDefaultHasInstance)));

    notInstance.append(jit.branchIfNotCell(value));
    notInstance.append(jit.branchIfNotObject(value));
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(propertyCaches.prototypeUID)), probe.uid);
    SlotRead prototypeRead = readFromConstructor(prototype);
    slowCases.append(jit.branchIfNotCell(prototype));
    slowCases.append(jit.branchIfNotObject(prototype));
    followPrototypeChainAndReturn();

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

StubSignature signatureOfInstanceOf(StubKey) { return StubSignature::withGPRResult({ gprOf(StubRegister::R0), gprOf(StubRegister::A1) }); }

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
