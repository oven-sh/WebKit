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
    jit.purifyNaN(number, number);
    boxDouble(jit, number, value);
    isBoxed.link(&jit);
    jit.restoreFPR(number, savedNumber);
    jit.jumpToStub(arrayPush());

    isInt32Shape.link(&jit);
    emitSlowPathCall(jit, Entry::operationAOTArrayPushNumber);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
