/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsArrays.h"

#if ENABLE(AOT)

#include "AOTRuntimeTable.h"
#include "AOTStubFrontEnds.h"
#include "AOTStubHelpers.h"
#include "AOTStubRegisters.h"
#include "Butterfly.h"
#include "IndexingType.h"
#include "JSCell.h"
#include "JSObject.h"

namespace JSC { namespace AOT {

static_assert(gprOf(StubRegister::A1) == operationArgumentGPR(1) && gprOf(StubRegister::A2) == operationArgumentGPR(2) && gprOf(StubRegister::A3) == operationArgumentGPR(3));

// What JSObject::putByIndex does for an index inside the vector of an array whose storage is its own and has one of the three kinds: no such array has an accessor, a read-only element or a fixed length, on itself or on its prototypes.
static void emitStoreInsideVector(StubAssembler& jit, GPRReg kind, GPRReg butterfly, GPRReg length, JumpList& callsOperation)
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
        callsOperation.append(jit.branch64(CCallHelpers::AboveOrEqual, position, length));
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
    callsOperation.append(jit.branchIfNotInt32(value));
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

static void emitFrontEnd(StubAssembler& jit, Entry rawOperation)
{
    const GPRReg kind = jit.allocateScratch(StubRegister::T9);
    const GPRReg butterfly = jit.allocateScratch(StubRegister::T10);
    const GPRReg length = jit.allocateScratch(StubRegister::T11);
    JumpList callsOperation;

    emitStoreInsideVector(jit, kind, butterfly, length, callsOperation);
    // The stub that remembers an element takes a register that an operation call preserves, which no front end may. The operation stores the same value again and remembers it.
    Jump isNotCell = jit.branchIfNotCell(jit.operand(1));
    callsOperation.append(branchIfBarrierIsNeeded(jit, jit.operand(0), kind));
    isNotCell.link(&jit);
    jit.move(TrustedImm32(0), jit.result());
    jit.returnToCaller();

    callsOperation.link(&jit);
    jit.tailCallOperation(rawOperation, OperationCall::FirstArgument::Instance);
}

StubSignature signatureOfArrayPutAtIndexFrontEnd(StubKey)
{
    return operationFrontEndSignature(3);
}

void generateArrayPutAtIndexFrontEnd(StubAssembler& jit)
{
    emitFrontEnd(jit, Entry::RawArrayPutAtIndex);
}

StubSignature signatureOfArrayPutNumberAtIndexFrontEnd(StubKey)
{
    return operationFrontEndSignature(3);
}

void generateArrayPutNumberAtIndexFrontEnd(StubAssembler& jit)
{
    emitFrontEnd(jit, Entry::RawArrayPutNumberAtIndex);
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
    JumpList callsOperation;

    emitStoreInsideVector(jit, kind, butterfly, length, callsOperation);
    emitElementBarrierAndReturn(jit, jit.operand(0), jit.operand(1));

    callsOperation.link(&jit);
    Entry operation = storedValueOf(jit) == ArrayStoredValue::Number ? Entry::operationAOTArrayPutNumberAtIndex : Entry::operationAOTArrayPutAtIndex;
    jit.callOperationAndReturn(operation, { .returns = OperationCall::Returns::Void, .saves = OperationCall::Saves::CallerSavedRegisters });
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

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
