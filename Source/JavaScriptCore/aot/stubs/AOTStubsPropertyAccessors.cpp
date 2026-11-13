/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsPropertyAccessors.h"

#if ENABLE(AOT)

#include "AOTConvention.h"
#include "AOTClosureWord.h"
#include "AOTInstance.h"
#include "AOTProgramTables.h"
#include "AOTPropertyCachesFront.h"
#include "AOTRuntimeRecords.h"
#include "AOTStaleAnswers.h"
#include "AOTStubsCalls.h"
#include "AOTStubsField.h"
#include "AOTStubsKeyed.h"
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

void countSlowPathRead(StubAssembler& jit)
{
    if (jit.options().checksWalk)
        jit.add32(TrustedImm32(1), Address(instanceGPR, AOT_INSTANCE_OFFSET(propertyCaches.slowPathReadCount)));
}

void callGetterAndReturn(StubAssembler& jit, GPRReg receiver, GPRReg getterSetter, JumpList& cannot)
{
    const GPRReg getter = jit.reuseScratch(StubRegister::T11);
    RELEASE_ASSERT(noOverlap(receiver, getterSetter, getter, gprOf(StubRegister::T14)));
    verifyGetterOfPendingRead(jit, receiver, getterSetter);
    countSlowPathRead(jit);
    const Address returnedCapture(getterSetter, GetterSetter::offsetOfReturnedCapture());
    static_assert(ReturnedCaptureData::returnsNoCapture < 0);
#if CPU(X86_64)
    Jump mayReturnCapture = jit.branch64(CCallHelpers::GreaterThanOrEqual, returnedCapture, TrustedImm32(0));
#else
    const GPRReg scratch = jit.reuseScratch(StubRegister::T14);
    jit.load64(returnedCapture, scratch);
    Jump mayReturnCapture = jit.branchTest64(CCallHelpers::Zero, scratch, TrustedImm64(std::numeric_limits<int64_t>::min()));
#endif
    jit.addOutOfLinePart([=](StubAssembler& jit) {
        const StubKey returnsCapture = returnCapture(jit.key().callerFrame());
        const GPRReg receiverOfGetter = signatureOfReturnCapture(returnsCapture).operands[0].gpr();
        const GPRReg pair = signatureOfReturnCapture(returnsCapture).operands[1].gpr();
        mayReturnCapture.link(&jit);
        jit.shuffleRegisters<GPRReg, 2>({ receiver, getterSetter }, { receiverOfGetter, pair });
        jit.jumpToStub(returnsCapture);
    });
    jit.loadPtr(Address(getterSetter, GetterSetter::offsetOfGetter()), getter);
    enterGetterAndReturn(jit, receiver, getter, cannot);
}

void callGetterFunctionAndReturn(StubAssembler& jit, GPRReg receiver, GPRReg getter, JumpList& cannot)
{
    verifyGetterOfPendingRead(jit, receiver, getter);
    countSlowPathRead(jit);
    enterGetterAndReturn(jit, receiver, getter, cannot);
}

static void enterGetterAndReturn(StubAssembler& jit, GPRReg receiver, GPRReg getter, JumpList& cannot)
{
    const GPRReg scratch = jit.reuseScratch(StubRegister::T14);
    RELEASE_ASSERT(getter == jit.reuseScratch(StubRegister::T11) && noOverlap(receiver, getter, scratch) && jit.result() == GPRInfo::returnValueGPR);

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

StubSignature signatureOfReturnCapture(StubKey)
{
    return StubSignature::withGPRResult({ gprOf(firstStubOperandRegister), gprOf(StubRegister::T10) });
}

void generateReturnCapture(StubAssembler& jit)
{
    const GPRReg receiver = jit.operand(0);
    const GPRReg getterSetter = jit.clobberOperand(1);
    const GPRReg getter = jit.allocateScratch(StubRegister::T11);
    const GPRReg scratch = jit.allocateScratch(StubRegister::T14);
    const GPRReg secondScratch = jit.allocateScratch(slotHeaderRegister);
    const GPRReg capture = jit.allocateScratch(slotPayloadRegister);
    const GPRReg key = jit.allocateScratch(slotNameRegister);
    const GPRReg result = jit.result();
    const FPRReg number = jit.allocateScratch(FPRInfo::fpRegT0);
    const FPRReg numberScratch = jit.allocateScratch(FPRInfo::fpRegT1);
    RELEASE_ASSERT(result == receiver && noOverlap(result, getterSetter, getter, scratch, secondScratch, capture, key));
    const Address returnedCapture(getterSetter, GetterSetter::offsetOfReturnedCapture());
    JumpList callsGetter;
    JumpList isNoElement;

    jit.load64(returnedCapture, capture);
    Jump isUnknown = jit.branchTest64(CCallHelpers::Zero, capture);
    Label isKnown = jit.label();
    jit.urshift64(capture, TrustedImm32(ReturnedCaptureData::formShift), scratch);
    Jump hasForm = jit.branchTest32(CCallHelpers::NonZero, scratch);
    jit.load64(Address(capture), scratch);
    callsGetter.append(jit.branchTest64(CCallHelpers::Zero, scratch));
    jit.move(scratch, result);
    jit.returnToCaller();

    hasForm.link(&jit);
    jit.lshift64(TrustedImm32(64 - ReturnedCaptureData::formShift), capture);
    jit.urshift64(TrustedImm32(64 - ReturnedCaptureData::formShift), capture);
    Jump isSpecial = jit.branch32(CCallHelpers::AboveOrEqual, scratch, TrustedImm32(ReturnedCaptureData::isElement));
    {
        FieldReadExits exits;
        readFieldAndReturn(jit, {
            .field = { .object = getterSetter, .slotData = InvalidGPRReg, .extensionWordOffset = InvalidGPRReg, .address = capture, .representation = scratch },
            .value = secondScratch,
            .result = result,
            .number = number,
            .numberScratch = numberScratch,
        }, exits);
        callsGetter.append(exits.isAbsent);
        callsGetter.append(exits.cannot);
    }

    isSpecial.link(&jit);
    Jump isElement = jit.branch32(CCallHelpers::Equal, scratch, TrustedImm32(ReturnedCaptureData::isElement));
    auto returnUnlessNotMade = [&](uint8_t form, JSValue notMade) {
        Jump hasOtherForm = jit.branch32(CCallHelpers::NotEqual, scratch, TrustedImm32(form));
        jit.load64(Address(capture), scratch);
        callsGetter.append(jit.branchTest64(CCallHelpers::Zero, scratch));
        callsGetter.append(jit.branch64(CCallHelpers::Equal, scratch, TrustedImm32(static_cast<int32_t>(JSValue::encode(notMade)))));
        jit.move(scratch, result);
        jit.returnToCaller();
        hasOtherForm.link(&jit);
    };
    if (jit.options().useWideReferences)
        callsGetter.append(jit.branch32(CCallHelpers::Equal, scratch, TrustedImm32(ReturnedCaptureData::isClosureCell)));
    else {
        Jump isNoClosureCell = jit.branch32(CCallHelpers::NotEqual, scratch, TrustedImm32(ReturnedCaptureData::isClosureCell));
        jit.load32(Address(capture), scratch);
        callsGetter.append(jit.branchTest32(CCallHelpers::Zero, scratch));
        jit.add32(TrustedImm32(1), scratch);
        decodeCell32(jit, scratch, result);
        jit.returnToCaller();
        isNoClosureCell.link(&jit);
    }
    returnUnlessNotMade(ReturnedCaptureData::undefinedIsNotMade, jsUndefined());
    returnUnlessNotMade(ReturnedCaptureData::nullIsNotMade, jsNull());
    jit.breakpoint();

    isElement.link(&jit);
    jit.load64(Address(capture), result);
    jit.load64(Address(capture, sizeof(EncodedJSValue)), key);
    returnValueOfKeyByPlace(jit, result, key, { secondScratch, scratch, capture, getter }, number, numberScratch, isNoElement);
    isNoElement.link(&jit);
    jit.move(TrustedImm64(JSValue::ValueUndefined), result);

    callsGetter.link(&jit);
    jit.loadPtr(Address(getterSetter, GetterSetter::offsetOfGetter()), getter);
    Jump hasGetter = jit.jump();

    isUnknown.link(&jit);
    jit.loadPtr(Address(getterSetter, GetterSetter::offsetOfGetter()), getter);
    {
        JumpList returnsNone;
        const GPRReg closureNumber = scratch;
        const GPRReg row = secondScratch;
        const GPRReg slot = key;
        auto loadClosureNumberIfReturnsCapture = [&](ptrdiff_t offsetOfWord, JumpList& isNoSuchClosure) {
            constexpr TrustedImm64 minWord(ClosureWord::minWordOfCalleeTaking(0));
            jit.load64(Address(getter, offsetOfWord), closureNumber);
            jit.move(minWord, slot);
            isNoSuchClosure.append(jit.branch64(CCallHelpers::LessThan, closureNumber, slot));
            returnsNone.append(jit.branchTest64(CCallHelpers::Zero, closureNumber, TrustedImm64(1ll << ClosureWord::returnsCaptureBit)));
            jit.urshift64(TrustedImm32(ClosureWord::functionShift), closureNumber);
            jit.and32(TrustedImm32(static_cast<int32_t>(ClosureWord::functionMask)), closureNumber);
        };
        JumpList isNotSlim;
        jit.load32(Address(getter, JSCell::structureIDOffset()), slot);
        isNotSlim.append(jit.branch32(CCallHelpers::NotEqual, slot, Address(instanceGPR, AOT_INSTANCE_OFFSET(slimClosureHeaders[0]))));
        loadClosureNumberIfReturnsCapture(offsetOfSlimClosureWord, returnsNone);
        Jump hasNumber = jit.jump();
        isNotSlim.link(&jit);
        returnsNone.append(jit.branch32(CCallHelpers::NotEqual, slot, Address(instanceGPR, AOT_INSTANCE_OFFSET(closures.closureHeaders[0]))));
        loadClosureNumberIfReturnsCapture(offsetOfClosureEntry, returnsNone);
        hasNumber.link(&jit);

        static_assert(sizeof(ReturnedCaptureData) == 8 && !OBJECT_OFFSETOF(ReturnedCaptureData, closureNumberAndBase) && ReturnedCaptureData::closureNumberMask == ClosureWord::functionMask);
        const GPRReg found = capture;
        jit.add32(TrustedImm32(1), closureNumber);
        jit.mul32(TrustedImm32(static_cast<int32_t>(ReturnedCaptureData::multiplier)), closureNumber, row);
        jit.load64(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.capacityOfReturnedCaptures)), slot);
        jit.mul64(slot, row);
        jit.urshift64(TrustedImm32(32), row);
        jit.lshift64(TrustedImm32(WTF::fastLog2(static_cast<unsigned>(sizeof(ReturnedCaptureData)))), row);
        jit.addPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.returnedCaptures)), row);
        Label probe = jit.label();
        jit.load32(Address(row), slot);
        jit.and32(TrustedImm32(static_cast<int32_t>(ReturnedCaptureData::closureNumberMask)), slot, found);
        Jump isFound = jit.branch32(CCallHelpers::Equal, found, closureNumber);
        jit.addPtr(TrustedImm32(sizeof(ReturnedCaptureData)), row);
        jit.branchTest32(CCallHelpers::NonZero, slot).linkTo(probe, &jit);

        returnsNone.link(&jit);
        Label returnsNoneAtAll = jit.label();
        jit.move(TrustedImm64(ReturnedCaptureData::returnsNoCapture), capture);
        jit.store64(capture, returnedCapture);
        Jump returnsNoCapture = jit.jump();

        isFound.link(&jit);
        const GPRReg base = scratch;
        JumpList hasBase;
        jit.urshift32(slot, TrustedImm32(ReturnedCaptureData::numberOfClosureNumberBits), base);
        jit.load32(Address(row, OBJECT_OFFSETOF(ReturnedCaptureData, slot)), slot);
        jit.urshift32(slot, TrustedImm32(SlotData::offsetBits), secondScratch);
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.statics)), capture);
        hasBase.append(jit.branch32(CCallHelpers::Equal, base, TrustedImm32(ReturnedCaptureData::baseOfStatics)));
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(closureCells)), capture);
        hasBase.append(jit.branch32(CCallHelpers::Equal, secondScratch, TrustedImm32(ReturnedCaptureData::isClosureCell)));
        jit.move(getter, capture);
        hasBase.append(jit.branch32(CCallHelpers::Equal, base, TrustedImm32(ReturnedCaptureData::baseOfClosure)));
        jit.loadPtr(BaseIndex(getter, base, CCallHelpers::TimesEight), capture);
        hasBase.link(&jit);
        jit.and32(TrustedImm32(SlotData::maxOffset), slot);
        jit.addPtr(slot, capture);
        Jump isNoPair = jit.branch32(CCallHelpers::NotEqual, secondScratch, TrustedImm32(ReturnedCaptureData::isElement));
        for (int32_t offset : { 0, static_cast<int32_t>(sizeof(EncodedJSValue)) }) {
            jit.load64(Address(capture, offset), slot);
            jit.branchTest64(CCallHelpers::Zero, slot).linkTo(returnsNoneAtAll, &jit);
            jit.branchIfNotCell(slot).linkTo(returnsNoneAtAll, &jit);
        }
        isNoPair.link(&jit);
        jit.lshift64(TrustedImm32(ReturnedCaptureData::formShift), secondScratch);
        jit.or64(secondScratch, capture);
        jit.store64(capture, returnedCapture);
        jit.jump().linkTo(isKnown, &jit);

        returnsNoCapture.link(&jit);
    }
    hasGetter.link(&jit);
    const SlotOrigin origin = jit.key().callerFrame() == CallerFrame::StubFrame ? SlotOrigin::ByIdInStubFrame : SlotOrigin::ById;
    JumpList cannot;
    enterGetterAndReturn(jit, receiver, getter, cannot);
    jit.linkToStub(cannot, slotCannot(SlotFinisher::CallGetter, origin));
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
