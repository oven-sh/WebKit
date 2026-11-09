/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsPropertyAccessors.h"

#if ENABLE(AOT)

#include "AOTConvention.h"
#include "AOTInstance.h"
#include "AOTRuntimeRecords.h"
#include "AOTStaleAnswers.h"
#include "AOTStubsCalls.h"
#include "GetterSetter.h"
#include "JSCell.h"
#include "JSType.h"
#include "VM.h"
#include <optional>

namespace JSC { namespace AOT {

static constexpr unsigned customAccessorRecordShift = 5;
static_assert(sizeof(CustomAccessorRecord) == 1u << customAccessorRecordShift);
static_assert(exists(callValue(0)) && exists(callValue(1)));

static void verifyGetterOfPendingRead(StubAssembler& jit, GPRReg receiver, GPRReg accessorOrGetter)
{
    if (!jit.options().checksWalk)
        return;
    jit.preservingCallerSavedRegisters(false, [&](unsigned) {
        const GPRReg A0 = jit.savedScratch(StubRegister::A0);
        const GPRReg A1 = jit.savedScratch(StubRegister::A1);
        const GPRReg A2 = jit.savedScratch(StubRegister::A2);
        jit.subPtr(TrustedImm32(2 * sizeof(CPURegister)), sp);
        jit.storePair64(receiver, accessorOrGetter, sp, TrustedImm32(0));
        jit.loadPair64(sp, TrustedImm32(0), A1, A2);
        jit.addPtr(TrustedImm32(2 * sizeof(CPURegister)), sp);
        jit.move(instanceGPR, A0);
        jit.move(TrustedImm32(0), jit.savedScratch(StubRegister::A3));
        jit.move(TrustedImm32(static_cast<int32_t>(kindAndStubOfAnswer(KindOfAnswer::Accessor, StubThatAnswers::GetterOfPendingRead))), jit.savedScratch(StubRegister::A4));
        jit.callOperation(Entry::operationAOTStopAtStaleAnswer, jit.savedScratch(StubRegister::Operation));
    });
}

static void enterGetterAndReturn(StubAssembler&, GPRReg receiver, GPRReg getter, JumpList& cannot);

void callGetterAndReturn(StubAssembler& jit, GPRReg receiver, GPRReg getterSetter, JumpList& cannot)
{
    const GPRReg getter = jit.reuseScratch(StubRegister::T11);
    RELEASE_ASSERT(noOverlap(receiver, getterSetter, getter, gprOf(StubRegister::T14)));
    verifyGetterOfPendingRead(jit, receiver, getterSetter);
    jit.loadPtr(Address(getterSetter, GetterSetter::offsetOfGetter()), getter);
    enterGetterAndReturn(jit, receiver, getter, cannot);
}

void callGetterFunctionAndReturn(StubAssembler& jit, GPRReg receiver, GPRReg getter, JumpList& cannot)
{
    verifyGetterOfPendingRead(jit, receiver, getter);
    enterGetterAndReturn(jit, receiver, getter, cannot);
}

static void enterGetterAndReturn(StubAssembler& jit, GPRReg receiver, GPRReg getter, JumpList& cannot)
{
    const GPRReg scratch = jit.reuseScratch(StubRegister::T14);
    RELEASE_ASSERT(getter == jit.reuseScratch(StubRegister::T11) && noOverlap(receiver, getter, scratch) && jit.result() == GPRInfo::returnValueGPR);
    if (jit.options().checksWalk)
        jit.add32(TrustedImm32(1), Address(instanceGPR, AOT_INSTANCE_OFFSET(propertyCaches.slowPathReadCount)));

#if CPU(X86_64)
    static_assert(ClosureWord::offsetUnit == 1 && ClosureWord::numberOfOffsetBits == 32);
    auto branchIfTopByteRefusesCall = [&](ptrdiff_t offsetOfWord) {
        return jit.branch8(CCallHelpers::LessThan, Address(getter, offsetOfWord + ClosureWord::topByteShift / 8), TrustedImm32(ClosureWord::minTopByteOfCalleeTaking(0)));
    };
    JumpList isNotCommonCase;
    JumpList isNotSlim;
    if (jit.options().hasOneInstance)
        isNotSlim.append(branchIfTopByteRefusesCall(offsetOfSlimClosureWord));
    else {
        jit.load32(Address(getter, JSCell::structureIDOffset()), scratch);
        isNotSlim.append(jit.branch32(CCallHelpers::NotEqual, scratch, Address(instanceGPR, AOT_INSTANCE_OFFSET(slimClosureHeaders[0]))));
        isNotCommonCase.append(branchIfTopByteRefusesCall(offsetOfSlimClosureWord));
    }
    jit.load32(Address(getter, offsetOfSlimClosureWord), scratch);

    Label hasOffsetOfCode = jit.label();
    isNotCommonCase.append(jit.branchPtr(CCallHelpers::Below, sp, StubAssembler::trapAwareSoftStackLimitWord()));
    jit.callAndReturnPreservingRegisters({ .callee = StubCallee::CompiledCode, .returnsValue = true, .checksException = false }, [&] {
        RELEASE_ASSERT(noOverlap(scratch, thisGPR, calleeGPR, countGPR));
        jit.shuffleRegisters<GPRReg, 2>({ receiver, getter }, { thisGPR, calleeGPR });
        jit.move(TrustedImm32(0), countGPR);
        jit.add64(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.code)), scratch);
        jit.call(scratch, JSEntryPtrTag);
    });

    isNotSlim.link(&jit);
    if (jit.options().hasOneInstance)
        jit.load32(Address(getter, JSCell::structureIDOffset()), scratch);
    isNotCommonCase.append(jit.branch32(CCallHelpers::NotEqual, scratch, Address(instanceGPR, AOT_INSTANCE_OFFSET(closures.closureHeaders[0]))));
    isNotCommonCase.append(branchIfTopByteRefusesCall(offsetOfClosureEntry));
    jit.load32(Address(getter, offsetOfClosureEntry), scratch);
    jit.jump().linkTo(hasOffsetOfCode, &jit);

    isNotCommonCase.link(&jit);
#else
    static_assert(ClosureWord::numberOfOffsetBits == 32);
    constexpr TrustedImm64 minWord(ClosureWord::minWordOfCalleeTaking(0));
    JumpList isNotCommonCase;
    JumpList isNotSlim;
    if (!jit.options().hasOneInstance) {
        jit.load32(Address(getter, JSCell::structureIDOffset()), scratch);
        isNotSlim.append(jit.branch32(CCallHelpers::NotEqual, scratch, Address(instanceGPR, AOT_INSTANCE_OFFSET(slimClosureHeaders[0]))));
    }
    jit.load64(Address(getter, offsetOfSlimClosureWord), scratch);
    (jit.options().hasOneInstance ? isNotSlim : isNotCommonCase).append(jit.branch64(CCallHelpers::LessThan, scratch, minWord));

    Label hasWord = jit.label();
    isNotCommonCase.append(jit.branchPtr(CCallHelpers::Below, sp, StubAssembler::trapAwareSoftStackLimitWord()));
    jit.callAndReturnPreservingRegisters({ .callee = StubCallee::CompiledCode, .returnsValue = true, .checksException = false }, [&] {
        RELEASE_ASSERT(noOverlap(scratch, thisGPR, calleeGPR, countGPR));
        jit.shuffleRegisters<GPRReg, 2>({ receiver, getter }, { thisGPR, calleeGPR });
        jit.move(TrustedImm32(0), countGPR);
        toCodeAddress(jit, scratch, OwnedScratchRegister(jit), StackIsTested::Yes);
        jit.call(scratch, JSEntryPtrTag);
    });

    isNotSlim.link(&jit);
    jit.load32(Address(getter, JSCell::structureIDOffset()), scratch);
    isNotCommonCase.append(jit.branch32(CCallHelpers::NotEqual, scratch, Address(instanceGPR, AOT_INSTANCE_OFFSET(closures.closureHeaders[0]))));
    jit.load64(Address(getter, offsetOfClosureEntry), scratch);
    isNotCommonCase.append(jit.branch64(CCallHelpers::LessThan, scratch, minWord));
    jit.jump().linkTo(hasWord, &jit);

    isNotCommonCase.link(&jit);
#endif
    cannot.append(branchIfNotFunctionType(jit, getter, scratch));
    jit.checkStackBeforeEntering();
    jit.callAndReturnPreservingRegisters({ .callee = StubCallee::CompiledCode, .returnsValue = true, .checksException = true }, [&] {
        jit.shuffleRegisters<GPRReg, 2>({ receiver, getter }, { thisGPR, calleeGPR });
        jit.callStub(callValueCell(0));
    });
}

void callSetterAndReturn(StubAssembler& jit, GPRReg receiver, GPRReg value, GPRReg getterSetter, JumpList& cannot)
{
    const GPRReg setter = jit.reuseScratch(StubRegister::T11);
    const GPRReg scratch = jit.reuseScratch(StubRegister::T14);
    RELEASE_ASSERT(noOverlap(receiver, value, getterSetter, setter, scratch));

    jit.loadPtr(Address(getterSetter, GetterSetter::offsetOfSetter()), setter);
    cannot.append(branchIfNotFunctionType(jit, setter, scratch));
    cannot.append(jit.branchIfType(setter, NullSetterFunctionType));
    jit.checkStackBeforeEntering();
    jit.callAndReturnPreservingRegisters({ .callee = StubCallee::CompiledCode, .returnsValue = false, .checksException = true }, [&] {
        jit.shuffleRegisters<GPRReg, 3>({ receiver, value, setter }, { thisGPR, argumentGPR(0), calleeGPR });
        jit.callStub(callValue(1));
    });
}

static void loadCustomAccessorRecord(StubAssembler& jit, GPRReg accessorIndexAndRecord)
{
    jit.lshift64(TrustedImm32(customAccessorRecordShift), accessorIndexAndRecord);
    jit.addPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(propertyCaches.customAccessors)), accessorIndexAndRecord);
}

static void callRecordedAccessorAndReturn(StubAssembler& jit, GPRReg receiver, GPRReg record, GPRReg uid, std::optional<GPRReg> storedValue, std::optional<GPRReg> keyCell)
{
    jit.callAndReturnPreservingRegisters({ .callee = StubCallee::HostFunction, .returnsValue = !storedValue, .checksException = true, .cellsToKeep = { receiver, storedValue, keyCell } }, [&] {
        const GPRReg A0 = jit.reuseScratch(StubRegister::A0);
        const GPRReg A1 = jit.reuseScratch(StubRegister::A1);
        const GPRReg A2 = jit.reuseScratch(StubRegister::A2);
        const GPRReg function = jit.reuseScratch(StubRegister::T11);
        RELEASE_ASSERT(noOverlap(record, A0, A1, A2, function) && noOverlap(receiver, record, A1, A2, function));

        if (storedValue) {
            const GPRReg A3 = jit.reuseScratch(StubRegister::A3);
            RELEASE_ASSERT(noOverlap(A3, record, receiver));
            jit.shuffleRegisters<GPRReg, 2>({ *storedValue, uid }, { A2, A3 });
        } else
            jit.move(uid, A2);
        jit.loadPtr(Address(record, OBJECT_OFFSETOF(CustomAccessorRecord, slotBase)), A1);
        jit.moveConditionallyTest64(CCallHelpers::Zero, A1, A1, receiver, A1);
        jit.loadPtr(Address(record, OBJECT_OFFSETOF(CustomAccessorRecord, function)), function);
        jit.loadPtr(Address(record, OBJECT_OFFSETOF(CustomAccessorRecord, globalObject)), A0);
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(vm)), record);
        jit.storePtr(GPRInfo::callFrameRegister, Address(record, VM::offsetOfTopCallFrame()));
        jit.call(function, CustomAccessorPtrTag);
    });
}

void callCustomGetterAndReturn(StubAssembler& jit, GPRReg receiver, GPRReg accessorIndex, GPRReg uid, std::optional<GPRReg> keyCell)
{
    const GPRReg cachedValue = jit.reuseScratch(StubRegister::T11);
    const GPRReg record = jit.clobberRegister(accessorIndex);
    RELEASE_ASSERT(noOverlap(receiver, record, uid, cachedValue));
    loadCustomAccessorRecord(jit, record);

    jit.load32(Address(record, OBJECT_OFFSETOF(CustomAccessorRecord, offsetOfCachedValue)), cachedValue);
    Jump hasNoCachedValue = jit.branchTest32(CCallHelpers::Zero, cachedValue);
    jit.load64(BaseIndex(receiver, cachedValue, CCallHelpers::TimesOne), cachedValue);
    Jump isEmpty = jit.branchTest64(CCallHelpers::Zero, cachedValue);
    jit.move(cachedValue, jit.result());
    jit.returnToCaller();

    hasNoCachedValue.link(&jit);
    isEmpty.link(&jit);
    callRecordedAccessorAndReturn(jit, receiver, record, uid, std::nullopt, keyCell);
}

void callCustomSetterAndReturn(StubAssembler& jit, GPRReg receiver, GPRReg value, GPRReg accessorIndex, GPRReg uid, std::optional<GPRReg> keyCell)
{
    const GPRReg record = jit.clobberRegister(accessorIndex);
    RELEASE_ASSERT(noOverlap(receiver, value, record, uid));
    loadCustomAccessorRecord(jit, record);
    callRecordedAccessorAndReturn(jit, receiver, record, uid, value, keyCell);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
