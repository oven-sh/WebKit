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

struct PrototypeWalk {
    GPRReg object;
    GPRReg prototype;
    GPRReg current;
    GPRReg next;
    JumpList isInstance { };
    JumpList notInstance { };
    JumpList slowCases { };
};

struct SlotRead {
    GPRReg object;
    GPRReg result;
    Jump isNotValue;
    Label isDone;
};

static PrototypeWalk startPrototypeWalk(StubAssembler& jit, GPRReg prototype)
{
    const GPRReg current = jit.allocateScratch(StubRegister::T11);
    const GPRReg next = jit.allocateScratch(StubRegister::T12);
    return { jit.operand(0), prototype, current, next };
}

static void returnBoolean(StubAssembler& jit, bool value)
{
    jit.move(TrustedImm32(value), jit.result());
    jit.returnToCaller();
}

// `object` and `prototype` are intact on slowCases. Control never falls out of the end.
static void followPrototypeChain(StubAssembler& jit, PrototypeWalk& walk)
{
    struct Link {
        Jump hasPolyProto;
        Label hasPrototype;
        Jump goesOn;
    };
    const GPRReg next = walk.next;
    auto followLink = [&](GPRReg object) {
        Link link;
        walk.slowCases.append(jit.branchTest8(CCallHelpers::NonZero, Address(object, JSObject::typeInfoFlagsOffset()), TrustedImm32(OverridesGetPrototype)));
        loadStructure(jit, object, next);
        jit.load64(Address(next, Structure::prototypeOffset()), next);
        link.hasPolyProto = jit.branchTest64(CCallHelpers::Zero, next);
        link.hasPrototype = jit.label();
        walk.isInstance.append(jit.branch64(CCallHelpers::Equal, next, walk.prototype));
        link.goesOn = jit.branch64(CCallHelpers::NotEqual, next, Address(instanceGPR, AOT_INSTANCE_OFFSET(realm.objectPrototype)));
        returnBoolean(jit, false);
        return link;
    };
    auto readPolyProto = [&](Link& link, GPRReg object) {
        link.hasPolyProto.link(&jit);
        jit.load64(Address(object, offsetRelativeToBase(knownPolyProtoOffset)), next);
        jit.jump().linkTo(link.hasPrototype, &jit);
    };
    Link first = followLink(walk.object);
    first.goesOn.link(&jit);
    Label nextLink = jit.label();
    walk.notInstance.append(jit.branchIfNotCell(next));
    jit.move(next, walk.current);
    Link further = followLink(walk.current);
    further.goesOn.linkTo(nextLink, &jit);
    readPolyProto(first, walk.object);
    readPolyProto(further, walk.current);
}

static void returnWalkResult(StubAssembler& jit, PrototypeWalk& walk)
{
    walk.notInstance.link(&jit);
    returnBoolean(jit, false);
    walk.isInstance.link(&jit);
    returnBoolean(jit, true);
}

static void callDefaultHasInstance(StubAssembler& jit, PrototypeWalk& walk)
{
    walk.slowCases.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTDefaultHasInstance, { .secondArgumentIn = walk.object, .thirdArgumentIn = walk.prototype });
}

// The name is in probe.uid. `object` is intact on miss and on isNotValue, also where it is `result`.
static SlotRead readBySlot(StubAssembler& jit, const HashedProbeRegisters& probe, GPRReg object, GPRReg result, JumpList& miss)
{
    SlotRead read { object, result, { }, { } };
    jit.load32(Address(object, JSCell::structureIDOffset()), probe.structureID);
    findHashedSlot(jit, SlotAccess::Read, probe, miss);
    read.isNotValue = branchIfSlotKindDiffers(jit, { SlotForm::Slot, probe.header, probe.payload }, Slot::Kind::Value, probe.uid);
    loadValueBySlot(jit, CachedSlot(SlotForm::Slot, Slot::Kind::Value, probe.header, probe.payload), object, probe.structureID, probe.entry, result);
    read.isDone = jit.label();
    return read;
}

static void emitFarValueRead(StubAssembler& jit, const HashedProbeRegisters& probe, const SlotRead& read, JumpList& miss)
{
    const GPRReg storage = probe.structureID;
    const GPRReg wordIndex = probe.entry;
    read.isNotValue.link(&jit);
    miss.append(branchIfSlotKindDiffers(jit, { SlotForm::Slot, probe.header, probe.payload }, Slot::Kind::FarValue, probe.uid));
    CachedSlot(SlotForm::Slot, Slot::Kind::FarValue, probe.header, probe.payload).loadFirstPayloadWord(jit, wordIndex);
    jit.signExtend32ToPtr(wordIndex, wordIndex);
    jit.loadPtr(Address(read.object, JSObject::butterflyOffset()), storage);
    jit.load64(BaseIndex(storage, wordIndex, CCallHelpers::TimesEight), read.result);
    jit.jump().linkTo(read.isDone, &jit);
}

static void callHasInstanceMethod(StubAssembler& jit, GPRReg value, GPRReg constructor, GPRReg method, GPRReg scratch, JumpList& miss)
{
    const GPRReg argument = jit.reuseScratch(StubRegister::A0);
    RELEASE_ASSERT(method == calleeGPR && argument == argumentGPR(0));

    miss.append(jit.branchIfNotCell(method));
    miss.append(branchIfNotFunctionType(jit, method, scratch));
    jit.emitFunctionPrologue();
    jit.recordClobbers(RegisterSet { thisGPR });
    // In this order: on x86-64 thisGPR is the register of value.
    jit.move(value, argument);
    jit.move(constructor, thisGPR);
    jit.callStub(callValue(1));
    jit.emitFunctionEpilogue();
    jit.jumpToStub(toBoolean());
}

static void generateWalkFrom(StubAssembler& jit, GPRReg prototype)
{
    PrototypeWalk walk = startPrototypeWalk(jit, prototype);
    walk.notInstance.append(jit.branchIfNotCell(walk.object));
    walk.notInstance.append(jit.branchIfNotObject(walk.object));
    followPrototypeChain(jit, walk);
    returnWalkResult(jit, walk);
    callDefaultHasInstance(jit, walk);
}

void generateInstanceOf(StubAssembler& jit)
{
    if (static_cast<InstanceOfOperand>(jit.key().form()) == InstanceOfOperand::Prototype) {
        generateWalkFrom(jit, jit.operand(1));
        return;
    }

    const GPRReg constructor = jit.clobberOperand(1);
    PrototypeWalk walk = startPrototypeWalk(jit, constructor);
    const GPRReg value = walk.object;
    const GPRReg method = jit.allocateScratch(StubRegister::T10);
    const GPRReg payload = jit.allocateScratch(slotPayloadRegister);
    const HashedProbeRegisters probe { walk.current, method, jit.allocateScratch(StubRegister::T14), walk.next, payload, payload };
    JumpList miss;

    miss.append(jit.branchIfNotCell(constructor));
    miss.append(jit.branchIfNotObject(constructor));
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(propertyCaches.hasInstanceUID)), probe.uid);
    SlotRead methodRead = readBySlot(jit, probe, constructor, method, miss);
    Jump isNotDefaultMethod = jit.branchPtr(CCallHelpers::NotEqual, method, Address(instanceGPR, AOT_INSTANCE_OFFSET(propertyCaches.defaultHasInstanceFunction)));
    miss.append(jit.branchTest8(CCallHelpers::Zero, Address(constructor, JSCell::typeInfoFlagsOffset()), TrustedImm32(ImplementsDefaultHasInstance)));

    walk.notInstance.append(jit.branchIfNotCell(value));
    walk.notInstance.append(jit.branchIfNotObject(value));
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(propertyCaches.prototypeUID)), probe.uid);
    SlotRead prototypeRead = readBySlot(jit, probe, constructor, walk.prototype, miss);
    walk.slowCases.append(jit.branchIfNotCell(walk.prototype));
    walk.slowCases.append(jit.branchIfNotObject(walk.prototype));
    followPrototypeChain(jit, walk);
    returnWalkResult(jit, walk);
    callDefaultHasInstance(jit, walk);

    isNotDefaultMethod.link(&jit);
    callHasInstanceMethod(jit, value, constructor, method, walk.current, miss);

    emitFarValueRead(jit, probe, methodRead, miss);
    emitFarValueRead(jit, probe, prototypeRead, miss);

    miss.link(&jit);
    jit.exitOnMiss(Entry::operationAOTInstanceOfAndCache, { .secondArgumentIn = value, .thirdArgumentIn = constructor });
}

StubSignature signatureOfInstanceOf(StubKey)
{
    return StubSignature::withGPRResult({ gprOf(StubRegister::R0), gprOf(StubRegister::A1) });
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
