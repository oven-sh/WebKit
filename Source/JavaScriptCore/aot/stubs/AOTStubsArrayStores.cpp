/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsArrays.h"

#if ENABLE(AOT)

#include "AOTRuntimeTable.h"
#include "AOTStubHelpers.h"
#include "AOTStubRegisters.h"
#include "Butterfly.h"
#include "IndexingType.h"
#include "JSCell.h"
#include "JSObject.h"
#include "MarkedSpace.h"
#include <limits>

namespace JSC { namespace AOT {

static_assert(gprOf(StubRegister::A1) == operationArgumentGPR(1) && gprOf(StubRegister::A2) == operationArgumentGPR(2) && gprOf(StubRegister::A3) == operationArgumentGPR(3));

static void emitStoreInsideVector(StubAssembler& jit, GPRReg kind, GPRReg butterfly, GPRReg length, JumpList& isBeyondVector, JumpList& callsOperation)
{
    const GPRReg array = jit.operand(0);
    const GPRReg value = jit.operand(1);
    const GPRReg position = jit.operand(2);
    const GPRReg savedNumber = length;
    constexpr FPRReg number = FPRInfo::fpRegT0;
    JumpList isStored;
    JumpList doesNotFit;

    jit.load8(Address(array, JSCell::indexingTypeAndMiscOffset()), kind);
    jit.and32(TrustedImm32(IndexingShapeAndWritabilityMask), kind);
    Jump isContiguousShape = jit.branch32(CCallHelpers::Equal, kind, TrustedImm32(ContiguousShape));
    Jump isInt32Shape = jit.branch32(CCallHelpers::Equal, kind, TrustedImm32(Int32Shape));
    callsOperation.append(jit.branch32(CCallHelpers::NotEqual, kind, TrustedImm32(DoubleShape)));

    auto loadVector = [&] {
        jit.loadPtr(Address(array, JSObject::butterflyOffset()), butterfly);
        jit.load32(Address(butterfly, Butterfly::offsetOfVectorLength()), length);
        isBeyondVector.append(jit.branch64(CCallHelpers::AboveOrEqual, position, length));
    };

    loadVector();
    callsOperation.append(jit.branchIfNotNumber(value));
    jit.saveFPR(number, savedNumber);
    Jump isInt32 = jit.branchIfInt32(value);
    unboxDouble(jit, value, kind, number);
    doesNotFit.append(jit.branchIfNaN(number));
    Jump isDouble = jit.jump();
    isInt32.link(&jit);
    jit.convertInt32ToDouble(value, number);
    isDouble.link(&jit);
    jit.storeDouble(number, BaseIndex(butterfly, position, CCallHelpers::TimesEight));
    jit.restoreFPR(number, savedNumber);
    isStored.append(jit.jump());

    doesNotFit.link(&jit);
    jit.restoreFPR(number, savedNumber);
    callsOperation.append(jit.jump());

    isInt32Shape.link(&jit);
    Jump isInteger = jit.branchIfInt32(value);
    callsOperation.append(jit.branchIfNumber(value));
    widenStorageOfIntegersForAnyValue(jit, array, kind, callsOperation);
    isInteger.link(&jit);
    isContiguousShape.link(&jit);
    loadVector();
    jit.store64(value, BaseIndex(butterfly, position, CCallHelpers::TimesEight));

    isStored.link(&jit);
    jit.load32(Address(butterfly, Butterfly::offsetOfPublicLength()), length);
    Jump isInsideLength = jit.branch64(CCallHelpers::Below, position, length);
    jit.add32(TrustedImm32(1), position, length);
    jit.store32(length, Address(butterfly, Butterfly::offsetOfPublicLength()));
    isInsideLength.link(&jit);
}

static ArrayStoredValue storedValueOf(StubAssembler& jit)
{
    return static_cast<ArrayStoredValue>(jit.key().form());
}

StubSignature signatureOfArrayPutAtIndex(StubKey)
{
    return StubSignature::withoutResult({ gprOf(StubRegister::A1), gprOf(StubRegister::A2), gprOf(StubRegister::A3) });
}

void generateArrayPutAtIndex(StubAssembler& jit)
{
    const GPRReg kind = jit.allocateScratch(StubRegister::T9);
    const GPRReg butterfly = jit.allocateScratch(StubRegister::T10);
    const GPRReg length = jit.allocateScratch(StubRegister::T11);
    const GPRReg storage = jit.allocateScratch(StubRegister::T12);
    const GPRReg allocator = jit.allocateScratch(firstStubOperandRegister);
    const GPRReg neededLength = jit.allocateScratch(StubRegister::T13);
    JumpList isBeyondVector;
    JumpList callsOperation;

    Label start = jit.label();
    emitStoreInsideVector(jit, kind, butterfly, length, isBeyondVector, callsOperation);
    emitElementBarrierAndReturn(jit, jit.operand(0), jit.operand(1));

    isBeyondVector.link(&jit);
    callsOperation.append(jit.branch64(CCallHelpers::AboveOrEqual, jit.operand(2), TrustedImm32(std::numeric_limits<int32_t>::max())));
    jit.load32(Address(butterfly, Butterfly::offsetOfPublicLength()), length);
    jit.add32(TrustedImm32(1), jit.operand(2), neededLength);
    growVectorFor(jit, { .array = jit.operand(0), .butterfly = butterfly, .length = length, .storage = storage, .allocator = allocator, .scratch = kind, .maxNeededLength = maxLengthForElementwiseCopy + 1 }, neededLength, callsOperation);
    jit.jump().linkTo(start, &jit);

    callsOperation.link(&jit);
    Entry operation = Entry::operationAOTArrayPutAtIndex;
    if (storedValueOf(jit) == ArrayStoredValue::Number)
        operation = Entry::operationAOTArrayPutNumberAtIndex;
    else if (storedValueOf(jit) == ArrayStoredValue::ValueInSloppyCode)
        operation = Entry::operationAOTArrayPutAtIndexInSloppyCode;
    jit.callOperationAndReturn(operation, { .returns = OperationCall::Returns::Void, .saves = OperationCall::Saves::CallerSavedRegisters });
}

StubSignature signatureOfStoreWideningIntegers(StubKey)
{
    return StubSignature::withGPRResult({ gprOf(StubRegister::A1), gprOf(StubRegister::A2), gprOf(StubRegister::A3) });
}

void generateStoreWideningIntegers(StubAssembler& jit)
{
    const GPRReg array = jit.operand(0);
    const GPRReg value = jit.operand(1);
    const GPRReg position = jit.operand(2);
    const GPRReg result = jit.result();
    const GPRReg scratch = jit.allocateScratch(StubRegister::T9);
    const GPRReg butterfly = jit.allocateScratch(StubRegister::T10);
    const GPRReg length = jit.allocateScratch(StubRegister::T11);
    constexpr FPRReg number = CCallHelpers::fpTempRegister;
    JumpList isLeft;
    RELEASE_ASSERT(noOverlap(array, value, position, result, scratch, butterfly, length));

    jit.load8(Address(array, JSCell::indexingTypeAndMiscOffset()), scratch);
    jit.and32(TrustedImm32(IndexingShapeAndWritabilityMask), scratch);
    isLeft.append(jit.branch32(CCallHelpers::NotEqual, scratch, TrustedImm32(Int32Shape)));
    jit.loadPtr(Address(array, JSObject::butterflyOffset()), butterfly);
    jit.load32(Address(butterfly, Butterfly::offsetOfVectorLength()), length);
    isLeft.append(jit.branch64(CCallHelpers::AboveOrEqual, position, length));
    auto includePosition = [&] {
        jit.load32(Address(butterfly, Butterfly::offsetOfPublicLength()), length);
        Jump isInsideLength = jit.branch64(CCallHelpers::Below, position, length);
        jit.add32(TrustedImm32(1), position, length);
        jit.store32(length, Address(butterfly, Butterfly::offsetOfPublicLength()));
        isInsideLength.link(&jit);
    };

    Jump isNoNumber = jit.branchIfNotNumber(value);
    isLeft.append(jit.branchIfInt32(value));
    unboxDouble(jit, value, scratch, number);
    Jump isNaN = jit.branchIfNaN(number);
    jit.load32(Address(array, JSCell::structureIDOffset()), scratch);
    isLeft.append(jit.branch32(CCallHelpers::NotEqual, scratch, Address(instanceGPR, Instance::offsetOfNewArrayHeader(ArrayWithInt32))));
    jit.load32(Address(instanceGPR, Instance::offsetOfNewArrayHeader(ArrayWithDouble)), scratch);
    isLeft.append(jit.branchTest32(CCallHelpers::Zero, scratch));
    jit.store32(scratch, Address(array, JSCell::structureIDOffset()));
    jit.store8(TrustedImm32(ArrayWithDouble), Address(array, JSCell::indexingTypeAndMiscOffset()));
    Label convertNext = jit.label();
    jit.sub32(TrustedImm32(1), length);
    const BaseIndex element(butterfly, length, CCallHelpers::TimesEight);
    jit.load64(element, scratch);
    Jump isHole = jit.branchTest64(CCallHelpers::Zero, scratch);
    jit.convertInt32ToDouble(scratch, number);
    jit.storeDouble(number, element);
    Jump isConverted = jit.jump();
    isHole.link(&jit);
    jit.move(TrustedImm64(static_cast<int64_t>(PNaNAsBits)), scratch);
    jit.store64(scratch, element);
    isConverted.link(&jit);
    jit.branchTest32(CCallHelpers::NonZero, length).linkTo(convertNext, &jit);
    unboxDouble(jit, value, scratch, number);
    jit.storeDouble(number, BaseIndex(butterfly, position, CCallHelpers::TimesEight));
    includePosition();
    jit.move(TrustedImm32(1), result);
    jit.returnToCaller();

    isNoNumber.link(&jit);
    isNaN.link(&jit);
    widenStorageOfIntegersForAnyValue(jit, array, scratch, isLeft);
    jit.store64(value, BaseIndex(butterfly, position, CCallHelpers::TimesEight));
    includePosition();
    jit.move(TrustedImm32(1), result);
    emitElementBarrierAndReturn(jit, array, value);

    isLeft.link(&jit);
    jit.move(TrustedImm32(0), result);
    jit.returnToCaller();
}

StubSignature signatureOfSetArrayLength(StubKey)
{
    return StubSignature::withoutResult({ gprOf(StubRegister::A1), gprOf(StubRegister::A2) });
}

void generateSetArrayLength(StubAssembler& jit)
{
    static_assert(ArrayWithUndecided + 2 == ArrayWithInt32 && ArrayWithInt32 + 2 == ArrayWithDouble && ArrayWithDouble + 2 == ArrayWithContiguous, "an array has no mode between these, and in each its length can be written: one that cannot has a sparse map, or elements frozen in storage that is copied on write");
    static_assert(!((ArrayWithUndecided ^ ArrayWithInt32) & IsArray) && !((ArrayWithUndecided ^ ArrayWithDouble) & IsArray) && !((ArrayWithUndecided ^ ArrayWithContiguous) & IsArray) && IsArray == 1);
    const GPRReg kind = jit.allocateScratch(StubRegister::T9);
    const GPRReg butterfly = jit.allocateScratch(StubRegister::T10);
    const GPRReg length = jit.allocateScratch(StubRegister::T11);
    const GPRReg hole = jit.allocateScratch(StubRegister::T12);
    const Address publicLength(butterfly, Butterfly::offsetOfPublicLength());
    JumpList callsOperation;

    jit.load8(Address(jit.operand(0), JSCell::indexingTypeAndMiscOffset()), kind);
    jit.sub32(TrustedImm32(ListWhoseLengthIsStoredWithoutRunTime::firstKind), kind);
    callsOperation.append(jit.branch32(CCallHelpers::Above, kind, TrustedImm32(ListWhoseLengthIsStoredWithoutRunTime::lastKind - ListWhoseLengthIsStoredWithoutRunTime::firstKind)));
    callsOperation.append(jit.branchTest32(CCallHelpers::NonZero, kind, TrustedImm32(IsArray)));
    loadStructure(jit, jit.operand(0), butterfly);
    callsOperation.append(jit.branchTest32(CCallHelpers::NonZero, Address(butterfly, Structure::bitFieldOffset()), TrustedImm32(ListWhoseLengthIsStoredWithoutRunTime::bitsThatItsStructureMayNotHave)));
    jit.loadPtr(Address(jit.operand(0), JSObject::butterflyOffset()), butterfly);
    jit.load32(publicLength, length);
    Jump isShorterOrLonger = jit.branch32(CCallHelpers::NotEqual, length, jit.operand(1));
    jit.returnToCaller();

    isShorterOrLonger.link(&jit);
    callsOperation.append(jit.branch32(CCallHelpers::Below, length, jit.operand(1)));
    callsOperation.append(jit.branch32(CCallHelpers::Above, Address(butterfly, Butterfly::offsetOfVectorLength()), TrustedImm32(ListWhoseLengthIsStoredWithoutRunTime::mostElementsOfVector)));
    jit.move(TrustedImm64(0), hole);
    Jump holeIsEmptyValue = jit.branch32(CCallHelpers::NotEqual, kind, TrustedImm32(ArrayWithDouble - ArrayWithUndecided));
    jit.move(TrustedImm64(static_cast<int64_t>(PNaNAsBits)), hole);
    holeIsEmptyValue.link(&jit);
    Label emptyNext = jit.label();
    jit.sub32(TrustedImm32(1), length);
    jit.store64(hole, BaseIndex(butterfly, length, CCallHelpers::TimesEight));
    jit.branch32(CCallHelpers::Above, length, jit.operand(1)).linkTo(emptyNext, &jit);
    jit.store32(jit.operand(1), publicLength);
    jit.returnToCaller();

    callsOperation.link(&jit);
    jit.callOperationAndReturn(jit.key().form() ? Entry::operationAOTSetArrayLengthInSloppyCode : Entry::operationAOTSetArrayLength, { .returns = OperationCall::Returns::Void, .saves = OperationCall::Saves::CallerSavedRegisters });
}

StubSignature signatureOfArrayPutAtNumber(StubKey)
{
    return StubSignature::withoutResult({ gprOf(StubRegister::A1), gprOf(StubRegister::A2), FPRInfo::argumentFPR0 });
}

void generateArrayPutAtNumber(StubAssembler& jit)
{
    const FPRReg position = jit.operandFPR(2);
    const GPRReg index = jit.allocateScratch(StubRegister::A3);
    const FPRReg converted = jit.allocateScratch(FPRInfo::argumentFPR1);

    jit.truncateDoubleToInt64(position, index);
    jit.convertInt64ToDouble(index, converted);
    Jump isNotInteger = jit.branchDouble(CCallHelpers::DoubleNotEqualOrUnordered, position, converted);
    checkStubOperands(arrayPutAtIndex(storedValueOf(jit)), { jit.operand(0), jit.operand(1), index });
    jit.jumpToStub(arrayPutAtIndex(storedValueOf(jit)));

    isNotInteger.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTArrayPutAtNumber, { .returns = OperationCall::Returns::Void, .saves = OperationCall::Saves::CallerSavedRegisters });
}

StubSignature signatureOfReserveArrayCapacity(StubKey)
{
    return StubSignature::withoutResult({ gprOf(StubRegister::A1), FPRInfo::argumentFPR0 });
}

void generateReserveArrayCapacity(StubAssembler& jit)
{
    const GPRReg array = jit.operand(0);
    const FPRReg bound = jit.operandFPR(1);
    const GPRReg room = jit.allocateScratch(StubRegister::A2);
    const GPRReg grownToRoom = jit.allocateScratch(StubRegister::A3);
    const GPRReg kind = jit.allocateScratch(StubRegister::T9);
    const GPRReg butterfly = jit.allocateScratch(StubRegister::T10);
    const GPRReg length = jit.allocateScratch(StubRegister::T11);
    const GPRReg storage = jit.allocateScratch(StubRegister::T12);
    const GPRReg allocator = jit.allocateScratch(firstStubOperandRegister);
    const FPRReg converted = jit.allocateScratch(FPRInfo::argumentFPR1);
    JumpList hasRoom;
    JumpList callsOperation;

    static_assert(Int32Shape + 2 == DoubleShape && DoubleShape + 2 == ContiguousShape, "an array has no shape between these");
    jit.load8(Address(array, JSCell::indexingTypeAndMiscOffset()), kind);
    jit.and32(TrustedImm32(IndexingShapeAndWritabilityMask), kind);
    jit.sub32(TrustedImm32(Int32Shape), kind);
    hasRoom.append(jit.branch32(CCallHelpers::Above, kind, TrustedImm32(ContiguousShape - Int32Shape)));

    jit.truncateDoubleToInt64(bound, room);
    jit.convertInt64ToDouble(room, converted);
    Jump isWhole = jit.branchDouble(CCallHelpers::DoubleEqualAndOrdered, bound, converted);
    jit.add64(TrustedImm32(1), room);
    isWhole.link(&jit);
    jit.loadPtr(Address(array, JSObject::butterflyOffset()), butterfly);
    jit.load32(Address(butterfly, Butterfly::offsetOfVectorLength()), length);
    hasRoom.append(jit.branch64(CCallHelpers::LessThanOrEqual, room, length));
    hasRoom.append(jit.branch64(CCallHelpers::Above, room, TrustedImm32(MAX_STORAGE_VECTOR_LENGTH)));

    jit.urshift32(room, TrustedImm32(2), grownToRoom);
    jit.urshift32(room, TrustedImm32(4), kind);
    jit.add32(kind, grownToRoom);
    jit.neg32(grownToRoom);
    jit.add32(room, grownToRoom);
    jit.add32(TrustedImm32(1), length);
    Jump growsPastVector = jit.branch32(CCallHelpers::AboveOrEqual, grownToRoom, length);
    jit.move(length, grownToRoom);
    growsPastVector.link(&jit);
    jit.load32(Address(butterfly, Butterfly::offsetOfPublicLength()), length);
    growVectorFor(jit, { array, butterfly, length, storage, allocator, kind }, grownToRoom, callsOperation);
    hasRoom.link(&jit);
    jit.returnToCaller();

    callsOperation.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTReserveArrayCapacity, { .returns = OperationCall::Returns::Void, .saves = OperationCall::Saves::CallerSavedRegisters, .checksException = false });
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
