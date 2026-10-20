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

// What JSObject::putByIndex does for an index inside the vector of an array whose storage is its own and has one of the three kinds: no such array has an accessor, a read-only element
// or a fixed length, on itself or on its prototypes. Everything else is the operation's: growth, a value that does not fit the kind, shared storage, sparse storage.
static void emitStoreInsideVector(StubAssembler& jit, Entry rawOperation)
{
    const GPRReg array = jit.operand(0);
    const GPRReg value = jit.operand(1);
    const GPRReg position = jit.operand(2);
    const GPRReg kind = jit.allocateScratch(StubRegister::T9);
    const GPRReg butterfly = jit.allocateScratch(StubRegister::T10);
    const GPRReg length = jit.allocateScratch(StubRegister::T11);
    const FPRReg number = jit.allocateScratch(FPRInfo::fpRegT0);
    JumpList callsOperation;
    JumpList isStored;

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
    Jump isInt32 = jit.branchIfInt32(value);
    callsOperation.append(jit.branchIfNotNumber(value));
    unboxDouble(jit, value, kind, number);
    callsOperation.append(jit.branchIfNaN(number));
    Jump isDouble = jit.jump();
    isInt32.link(&jit);
    jit.convertInt32ToDouble(value, number);
    isDouble.link(&jit);
    jit.storeDouble(number, BaseIndex(butterfly, position, CCallHelpers::TimesEight));
    isStored.append(jit.jump());

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
    // The stub that remembers an element takes a register that an operation call preserves, which no front end may. The operation stores the same value again and remembers it.
    Jump isNotCell = jit.branchIfNotCell(value);
    callsOperation.append(branchIfBarrierIsNeeded(jit, array, kind));
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
    emitStoreInsideVector(jit, Entry::RawArrayPutAtIndex);
}

StubSignature signatureOfArrayPutNumberAtIndexFrontEnd(StubKey)
{
    return operationFrontEndSignature(3);
}

void generateArrayPutNumberAtIndexFrontEnd(StubAssembler& jit)
{
    emitStoreInsideVector(jit, Entry::RawArrayPutNumberAtIndex);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
