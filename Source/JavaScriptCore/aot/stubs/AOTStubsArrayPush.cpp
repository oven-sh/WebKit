/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsArrays.h"

#if ENABLE(AOT)

#include "AOTConvention.h"
#include "AOTRuntimeTable.h"
#include "AOTStubHelpers.h"
#include "AOTStubRegisters.h"
#include "AOTStubsBarriers.h"
#include "Butterfly.h"
#include "IndexingType.h"
#include "JSCell.h"
#include "JSObject.h"
#include "OperationResult.h"
#include <type_traits>

namespace JSC { namespace AOT {

static_assert(std::is_same_v<decltype(&operationAOTArrayPush), OperationReturnType<EncodedJSValue> (JIT_OPERATION_ATTRIBUTES *)(Instance*, JSCell*, EncodedJSValue)>);
static_assert(std::is_same_v<decltype(&operationAOTArrayPushNumber), OperationReturnType<EncodedJSValue> (JIT_OPERATION_ATTRIBUTES *)(Instance*, JSCell*, uint64_t)>);
static_assert(std::is_same_v<decltype(&operationAOTAppendElements), OperationReturnType<void> (JIT_OPERATION_ATTRIBUTES *)(Instance*, JSCell*, JSCell*)>);
static_assert(gprOf(StubRegister::A1) == operationArgumentGPR(1) && gprOf(StubRegister::A2) == operationArgumentGPR(2));

#if CPU(ARM64)
static constexpr StubRegister arrayPushSavedNumberRegister = StubRegister::T12;
static constexpr StubRegister arrayPushButterflyRegister = StubRegister::T13;
#elif CPU(X86_64)
static constexpr StubRegister arrayPushSavedNumberRegister = StubRegister::A5;
static constexpr StubRegister arrayPushButterflyRegister = StubRegister::A4;
#endif
static constexpr StubRegister arrayPushDoubleSavedNumberRegister = arrayPushKindRegister;
static constexpr StubRegister arrayPushDoubleIntegerRegister = arrayPushButterflyRegister;

static void emitSlowPathCall(StubAssembler& jit, Entry operation)
{
    jit.callOperationAndReturn(operation, { .returns = OperationCall::Returns::Value, .saves = OperationCall::Saves::CallerSavedRegisters });
}

void emitElementBarrierAndReturn(StubAssembler& jit, GPRReg array, GPRReg value)
{
    const StubKey target = rememberElement();
    const GPRReg rememberedArray = jit.reuseScratch(StubRegister::T9);
    const GPRReg rememberedValue = jit.reuseScratch(StubRegister::T11);
    checkStubOperands(target, { rememberedArray, rememberedValue });
    RELEASE_ASSERT(noOverlap(array, value, rememberedArray, rememberedValue));
    const GPRReg state = rememberedArray;

    Jump isNotCell = jit.branchIfNotCell(value);
    Jump needsBarrier = branchIfBarrierIsNeeded(jit, array, state);
    isNotCell.link(&jit);
    jit.returnToCaller();

    needsBarrier.link(&jit);
    jit.move(array, rememberedArray);
    jit.move(value, rememberedValue);
    jit.jumpToStub(target);
}

StubSignature signatureOfArrayPush(StubKey)
{
    return StubSignature::withGPRResult({ gprOf(StubRegister::A1), gprOf(StubRegister::A2) });
}

StubSignature signatureOfArrayPushOfKnownKind(StubKey)
{
    return StubSignature::withGPRResult({ gprOf(StubRegister::A1), gprOf(StubRegister::A2), gprOf(arrayPushKindRegister) });
}

void generateArrayPush(StubAssembler& jit)
{
    const GPRReg array = jit.operand(0);
    const GPRReg value = jit.operand(1);
    const GPRReg length = jit.result();
    const GPRReg kind = jit.allocateScratch(arrayPushKindRegister);
    const GPRReg savedNumber = jit.allocateScratch(arrayPushSavedNumberRegister);
    const GPRReg butterfly = jit.allocateScratch(arrayPushButterflyRegister);
    constexpr FPRReg number = FPRInfo::fpRegT0;
    JumpList slowPath;

    Label loadKind = jit.label();
    jit.load8(Address(array, JSCell::indexingTypeAndMiscOffset()), kind);
    jit.and32(TrustedImm32(IndexingShapeAndWritabilityMask), kind);
    jit.bindEntry(arrayPushOfKnownKind());
    jit.loadPtr(Address(array, JSObject::butterflyOffset()), butterfly);
    jit.load32(Address(butterfly, Butterfly::offsetOfPublicLength()), length);
    Jump isFull = jit.branch32(CCallHelpers::AboveOrEqual, length, Address(butterfly, Butterfly::offsetOfVectorLength()));
    Jump notNumber = jit.branchIfNotNumber(value);
    Jump isDoubleShape = jit.branch32(CCallHelpers::Equal, kind, TrustedImm32(DoubleShape));
    Jump isContiguousShape = jit.branch32(CCallHelpers::Equal, kind, TrustedImm32(ContiguousShape));
    slowPath.append(jit.branch32(CCallHelpers::NotEqual, kind, TrustedImm32(Int32Shape)));
    slowPath.append(jit.branchIfNotInt32(value));
    isContiguousShape.link(&jit);
    jit.store64(value, BaseIndex(butterfly, length, CCallHelpers::TimesEight));
    Jump isStored = jit.jump();

    isDoubleShape.link(&jit);
    jit.saveFPR(number, savedNumber);
    Jump isInt32 = jit.branchIfInt32(value);
    unboxDouble(jit, value, kind, number);
    Jump isNaN = jit.branchIfNaN(number);
    Jump isDouble = jit.jump();
    isInt32.link(&jit);
    jit.convertInt32ToDouble(value, number);
    isDouble.link(&jit);
    jit.storeDouble(number, BaseIndex(butterfly, length, CCallHelpers::TimesEight));
    jit.restoreFPR(number, savedNumber);
    isStored.link(&jit);
    jit.add32(TrustedImm32(1), length);
    jit.store32(length, Address(butterfly, Butterfly::offsetOfPublicLength()));
    boxInt32(jit, length, length);
    jit.returnToCaller();

    notNumber.link(&jit);
    slowPath.append(jit.branch32(CCallHelpers::NotEqual, kind, TrustedImm32(ContiguousShape)));
    jit.store64(value, BaseIndex(butterfly, length, CCallHelpers::TimesEight));
    jit.add32(TrustedImm32(1), length);
    jit.store32(length, Address(butterfly, Butterfly::offsetOfPublicLength()));
    boxInt32(jit, length, length);
    emitElementBarrierAndReturn(jit, array, value);

    // No program can observe the capacity, so the value may still turn out not to fit the kind.
    isFull.link(&jit);
    growFullVector(jit, { array, butterfly, length, savedNumber, kind, jit.reuseScratch(StubRegister::T9) }, slowPath);
    jit.jump().linkTo(loadKind, &jit);

    isNaN.link(&jit);
    jit.restoreFPR(number, savedNumber);
    slowPath.link(&jit);
    emitSlowPathCall(jit, Entry::operationAOTArrayPush);
}

StubSignature signatureOfArrayPushDouble(StubKey)
{
    return StubSignature::withGPRResult({ gprOf(StubRegister::A1), gprOf(StubRegister::A2) });
}

void generateArrayPushDouble(StubAssembler& jit)
{
    const GPRReg array = jit.operand(0);
    const GPRReg value = jit.clobberOperand(1);
    const GPRReg savedNumber = jit.allocateScratch(arrayPushDoubleSavedNumberRegister);
    const GPRReg integer = jit.allocateScratch(arrayPushDoubleIntegerRegister);
    constexpr FPRReg number = FPRInfo::fpRegT0;
    const FPRReg scratch = jit.allocateScratch(FPRInfo::fpRegT1);

    jit.load8(Address(array, JSCell::indexingTypeAndMiscOffset()), integer);
    jit.and32(TrustedImm32(IndexingShapeMask), integer);
    Jump isInt32Shape = jit.branch32(CCallHelpers::Equal, integer, TrustedImm32(Int32Shape));
    jit.saveFPR(number, savedNumber);
    jit.move64ToDouble(value, number);
    JumpList notInt32;
    branchIfNotExactInt32(jit, number, integer, notInt32, scratch);
    boxInt32(jit, integer, value);
    Jump isBoxed = jit.jump();
    notInt32.link(&jit);
    boxDouble(jit, PureDouble::purify(jit, number, number), value);
    isBoxed.link(&jit);
    jit.restoreFPR(number, savedNumber);
    jit.jumpToStub(arrayPush());

    isInt32Shape.link(&jit);
    emitSlowPathCall(jit, Entry::operationAOTArrayPushNumber);
}

StubSignature signatureOfAppendElements(StubKey)
{
    StubSignature signature;
    signature.addOperand(gprOf(StubRegister::A1));
    signature.addOperand(gprOf(StubRegister::A2));
    return signature;
}

// A short source, and a target that can hold what the source holds and has room or may be given room here.
void generateAppendElements(StubAssembler& jit)
{
    const GPRReg array = jit.operand(0);
    const GPRReg source = jit.operand(1);
    const GPRReg newLength = jit.allocateScratch(StubRegister::T9);
    const GPRReg target = jit.allocateScratch(StubRegister::T10);
    // Moved back by the length of the target, so that one position serves both.
    const GPRReg elements = jit.allocateScratch(StubRegister::T11);
    const GPRReg kind = jit.allocateScratch(StubRegister::T12);
    const GPRReg position = jit.allocateScratch(StubRegister::T14);
    const GPRReg element = jit.allocateScratch(firstStubOperandRegister);
    const FPRReg number = jit.allocateScratch(FPRInfo::fpRegT0);
    const FPRReg numberScratch = jit.allocateScratch(FPRInfo::fpRegT1);
    RELEASE_ASSERT(noOverlap(array, source, newLength, target, elements, kind, position, element));
    const BaseIndex read(elements, position, CCallHelpers::TimesEight);
    const BaseIndex written(target, position, CCallHelpers::TimesEight);
    const Address lengthOfTarget(target, Butterfly::offsetOfPublicLength());
    JumpList slowPath;
    JumpList hasHole;

    auto loadKindOfSource = [&] {
        jit.load8(Address(source, JSCell::indexingTypeAndMiscOffset()), kind);
        jit.and32(TrustedImm32(IndexingShapeMask), kind);
    };
    auto forEachElement = [&](const auto& copyElement) {
        Label loop = jit.label();
        copyElement();
        jit.add32(TrustedImm32(1), position);
        jit.branch32(CCallHelpers::Below, position, newLength).linkTo(loop, &jit);
    };
    auto setLengthAndReturn = [&] {
        jit.store32(newLength, lengthOfTarget);
        jit.returnToCaller();
    };

    Label start = jit.label();
    jit.loadPtr(Address(source, JSObject::butterflyOffset()), elements);
    jit.load32(Address(elements, Butterfly::offsetOfPublicLength()), newLength);
    Jump isEmpty = jit.branchTest32(CCallHelpers::Zero, newLength);
    slowPath.append(jit.branch32(CCallHelpers::Above, newLength, TrustedImm32(maxLengthForElementwiseCopy)));
    jit.loadPtr(Address(array, JSObject::butterflyOffset()), target);
    jit.load32(lengthOfTarget, position);
    jit.add32(position, newLength);
    Jump hasNoRoom = jit.branch32(CCallHelpers::Above, newLength, Address(target, Butterfly::offsetOfVectorLength()));
    jit.lshift64(position, TrustedImm32(3), element);
    jit.subPtr(element, elements);
    jit.load8(Address(array, JSCell::indexingTypeAndMiscOffset()), kind);
    jit.and32(TrustedImm32(IndexingShapeAndWritabilityMask), kind);
    Jump targetHoldsDoubles = jit.branch32(CCallHelpers::Equal, kind, TrustedImm32(DoubleShape));
    Jump targetHoldsInt32 = jit.branch32(CCallHelpers::Equal, kind, TrustedImm32(Int32Shape));
    slowPath.append(jit.branch32(CCallHelpers::NotEqual, kind, TrustedImm32(ContiguousShape)));

    loadKindOfSource();
    Jump boxesDoubles = jit.branch32(CCallHelpers::Equal, kind, TrustedImm32(DoubleShape));
    Jump copiesValues = jit.branch32(CCallHelpers::Equal, kind, TrustedImm32(ContiguousShape));
    slowPath.append(jit.branch32(CCallHelpers::NotEqual, kind, TrustedImm32(Int32Shape)));
    copiesValues.link(&jit);
    // The collector visits the elements below the length, which are holes until they are copied.
    jit.store32(newLength, lengthOfTarget);
    forEachElement([&] {
        jit.load64(read, element);
        Jump isNoHole = jit.branchTest64(CCallHelpers::NonZero, element);
        jit.move(TrustedImm64(JSValue::ValueUndefined), element);
        isNoHole.link(&jit);
        jit.store64(element, written);
    });
    Jump holdsNoCells = jit.branch32(CCallHelpers::Equal, kind, TrustedImm32(Int32Shape));
    Jump needsBarrier = branchIfBarrierIsNeeded(jit, array, kind);
    holdsNoCells.link(&jit);
    isEmpty.link(&jit);
    jit.returnToCaller();

    needsBarrier.link(&jit);
    checkStubOperands(writeBarrier(), { element });
    jit.move(array, element);
    jit.jumpToStub(writeBarrier());

    boxesDoubles.link(&jit);
    jit.store32(newLength, lengthOfTarget);
    forEachElement([&] {
        JumpList isNotInt32;
        jit.loadDouble(read, number);
        Jump isHole;
        PureDouble read = PureDouble::branchIfNaN(jit, number, isHole);
        branchIfNotExactInt32(jit, number, element, isNotInt32, numberScratch);
        boxInt32(jit, element, element);
        Jump isBoxedInt32 = jit.jump();
        isNotInt32.link(&jit);
        boxDouble(jit, read, element);
        Jump isBoxedDouble = jit.jump();
        isHole.link(&jit);
        jit.move(TrustedImm64(JSValue::ValueUndefined), element);
        isBoxedInt32.link(&jit);
        isBoxedDouble.link(&jit);
        jit.store64(element, written);
    });
    jit.returnToCaller();

    targetHoldsInt32.link(&jit);
    loadKindOfSource();
    slowPath.append(jit.branch32(CCallHelpers::NotEqual, kind, TrustedImm32(Int32Shape)));
    forEachElement([&] {
        jit.load64(read, element);
        hasHole.append(jit.branchTest64(CCallHelpers::Zero, element));
        jit.store64(element, written);
    });
    setLengthAndReturn();

    targetHoldsDoubles.link(&jit);
    loadKindOfSource();
    Jump convertsInt32 = jit.branch32(CCallHelpers::Equal, kind, TrustedImm32(Int32Shape));
    slowPath.append(jit.branch32(CCallHelpers::NotEqual, kind, TrustedImm32(DoubleShape)));
    forEachElement([&] {
        jit.loadDouble(read, number);
        hasHole.append(jit.branchIfNaN(number));
        jit.storeDouble(number, written);
    });
    setLengthAndReturn();

    convertsInt32.link(&jit);
    forEachElement([&] {
        jit.load64(read, element);
        hasHole.append(jit.branchTest64(CCallHelpers::Zero, element));
        jit.convertInt32ToDouble(element, number);
        jit.storeDouble(number, written);
    });
    setLengthAndReturn();

    // Whatever lies between the length and the end of the vector is a hole.
    hasHole.link(&jit);
    jit.load8(Address(array, JSCell::indexingTypeAndMiscOffset()), kind);
    jit.and32(TrustedImm32(IndexingShapeMask), kind);
    jit.move(TrustedImm64(0), element);
    Jump holeIsEmptyValue = jit.branch32(CCallHelpers::Equal, kind, TrustedImm32(Int32Shape));
    jit.move(TrustedImm64(static_cast<int64_t>(PNaNAsBits)), element);
    holeIsEmptyValue.link(&jit);
    Label clear = jit.label();
    slowPath.append(jit.branch32(CCallHelpers::BelowOrEqual, position, lengthOfTarget));
    jit.sub32(TrustedImm32(1), position);
    jit.store64(element, written);
    jit.jump().linkTo(clear, &jit);

    // No program can observe the capacity, so the target may still turn out not to hold what the source holds.
    hasNoRoom.link(&jit);
    growVectorFor(jit, { array, target, position, elements, kind, element }, newLength, slowPath);
    jit.jump().linkTo(start, &jit);

    slowPath.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTAppendElements, { .returns = OperationCall::Returns::Void, .saves = OperationCall::Saves::CallerSavedRegisters });
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
