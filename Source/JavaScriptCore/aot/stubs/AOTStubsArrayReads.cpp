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
#include "AOTStubsHashTables.h"
#include "ArrayConventions.h"
#include "ArrayStorage.h"
#include "Butterfly.h"
#include "IndexingType.h"
#include "JSCell.h"
#include "JSObject.h"
#include "SparseArrayValueMap.h"
#include "Structure.h"

namespace JSC { namespace AOT {

static_assert(gprOf(StubRegister::A1) == operationArgumentGPR(1) && gprOf(StubRegister::A2) == operationArgumentGPR(2));

enum class ElementQuestion : bool { Get, Has };

static StubKey stubAtIndexFor(ElementQuestion question)
{
    return question == ElementQuestion::Get ? arrayGetAtIndex() : arrayHasAtIndex();
}

static void callOperationAndReturn(StubAssembler& jit, ElementQuestion question)
{
    Entry operation = question == ElementQuestion::Get ? Entry::operationAOTGetByVal : Entry::operationAOTInByValAndCache;
    jit.callOperationAndReturn(operation, { .returns = OperationCall::Returns::Value, .saves = OperationCall::Saves::CallerSavedRegisters });
}

// The search of WTF::HashTable::inlineLookup().
void emitFindSparseElement(StubAssembler& jit, const SparseElementSearch& search, JumpList& isAbsent)
{
    const auto [owner, index, entry, probe, table] = search;
    constexpr unsigned entrySizeShift = 4;
    static_assert(sizeof(SparseArrayEntry) == 1 << entrySizeShift);
    const Address attributes(entry, SparseArrayEntry::offsetOfAttributes());

    emitHashOfBits(jit, index, entry, probe, table);
    jit.loadPtr(Address(owner, JSObject::butterflyOffset()), table);
    jit.loadPtr(Address(table, ArrayStorage::sparseMapOffset()), table);
    isAbsent.append(jit.branchTestPtr(CCallHelpers::Zero, table));
    jit.loadPtr(Address(table, SparseArrayValueMap::offsetOfTable()), table);
    isAbsent.append(jit.branchTestPtr(CCallHelpers::Zero, table));
    jit.move(TrustedImm32(0), probe);

    Label hasBucketNumber = jit.label();
    jit.and32(Address(table, SparseArrayValueMap::offsetOfTableSizeMask()), entry);
    jit.lshift64(TrustedImm32(entrySizeShift), entry);
    jit.add64(table, entry);
    isAbsent.append(jit.branchTest32(CCallHelpers::NonZero, attributes, TrustedImm32(static_cast<int32_t>(SparseArrayEntry::isEmptyBucketFlag))));
    Jump hasOtherIndex = jit.branch32(CCallHelpers::NotEqual, Address(entry, SparseArrayEntry::offsetOfIndex()), index);
    Jump isFound = jit.branchTest32(CCallHelpers::Zero, attributes, TrustedImm32(SparseArrayEntry::isDeletedBucketFlag));

    hasOtherIndex.link(&jit);
    jit.sub64(table, entry);
    jit.urshift64(TrustedImm32(entrySizeShift), entry);
    jit.add32(TrustedImm32(1), probe);
    jit.add32(probe, entry);
    jit.jump().linkTo(hasBucketNumber, &jit);

    isFound.link(&jit);
}

// Nothing stands under an index on the prototypes of the array, so an element that is not there reads as undefined and is in nothing. A position that is no index is a name.
static void answerAtIndex(StubAssembler& jit, ElementQuestion question)
{
    const GPRReg array = jit.operand(0);
    const GPRReg position = jit.clobberOperand(1);
    const GPRReg result = jit.result();
    const GPRReg shape = jit.allocateScratch(StubRegister::T9);
    const GPRReg butterfly = jit.allocateScratch(StubRegister::T10);
    const FPRReg number = jit.allocateScratch(FPRInfo::fpRegT0);
    const FPRReg numberScratch = jit.allocateScratch(FPRInfo::fpRegT1);
    JumpList isAbsent;
    JumpList isAbsentAtIndex;
    JumpList callsOperation;
    auto returnTrue = [&] {
        jit.move(TrustedImm32(1), result);
        jit.returnToCaller();
    };

    jit.load8(Address(array, JSCell::indexingTypeAndMiscOffset()), shape);
    jit.and32(TrustedImm32(IndexingShapeMask), shape);
    jit.loadPtr(Address(array, JSObject::butterflyOffset()), butterfly);
    Jump isContiguousShape = jit.branch32(CCallHelpers::Equal, shape, TrustedImm32(ContiguousShape));
    Jump isInt32Shape = jit.branch32(CCallHelpers::Equal, shape, TrustedImm32(Int32Shape));
    Jump isDoubleShape = jit.branch32(CCallHelpers::Equal, shape, TrustedImm32(DoubleShape));
    isAbsent.append(jit.branch32(CCallHelpers::Equal, shape, TrustedImm32(UndecidedShape)));
    static_assert(SlowPutArrayStorageShape > ArrayStorageShape && SlowPutArrayStorageShape == IndexingShapeMask - 2);
    callsOperation.append(jit.branch32(CCallHelpers::Below, shape, TrustedImm32(ArrayStorageShape)));
    callsOperation.append(jit.branch32(CCallHelpers::Above, shape, TrustedImm32(SlowPutArrayStorageShape)));
    jit.move(TrustedImm64(MAX_ARRAY_INDEX), result);
    callsOperation.append(jit.branch64(CCallHelpers::Above, position, result));
    Jump isOutsideVector = jit.branch32(CCallHelpers::AboveOrEqual, position, Address(butterfly, ArrayStorage::vectorLengthOffset()));
    jit.load64(BaseIndex(butterfly, position, CCallHelpers::TimesEight, ArrayStorage::vectorOffset()), result);
    Jump isHoleInVector = jit.branchTest64(CCallHelpers::Zero, result);
    if (question == ElementQuestion::Has)
        returnTrue();
    else
        jit.returnToCaller();

    isOutsideVector.link(&jit);
    isHoleInVector.link(&jit);
    const GPRReg index = shape;
    const GPRReg entry = result;
    jit.move(position, index);
    emitFindSparseElement(jit, { array, index, entry, position, butterfly }, isAbsentAtIndex);
    if (question == ElementQuestion::Has)
        returnTrue();
    else {
        Jump isAccessor = jit.branchTest32(CCallHelpers::NonZero, Address(entry, SparseArrayEntry::offsetOfAttributes()), TrustedImm32(static_cast<int32_t>(PropertyAttribute::Accessor)));
        jit.load64(Address(entry, SparseArrayEntry::offsetOfValue()), result);
        jit.returnToCaller();

        isAccessor.link(&jit);
        jit.move(index, position);
        callsOperation.append(jit.jump());
    }

    isContiguousShape.link(&jit);
    isInt32Shape.link(&jit);
    jit.load32(Address(butterfly, Butterfly::offsetOfPublicLength()), result);
    isAbsent.append(jit.branch64(CCallHelpers::AboveOrEqual, position, result));
    jit.load64(BaseIndex(butterfly, position, CCallHelpers::TimesEight), result);
    isAbsent.append(jit.branchTest64(CCallHelpers::Zero, result));
    if (question == ElementQuestion::Has)
        returnTrue();
    else
        jit.returnToCaller();

    isDoubleShape.link(&jit);
    jit.load32(Address(butterfly, Butterfly::offsetOfPublicLength()), result);
    isAbsent.append(jit.branch64(CCallHelpers::AboveOrEqual, position, result));
    jit.loadDouble(BaseIndex(butterfly, position, CCallHelpers::TimesEight), number);
    PureDouble element = PureDouble::branchIfNaN(jit, number, isAbsent);
    if (question == ElementQuestion::Has)
        returnTrue();
    else {
        JumpList isNotInt32;
        branchIfNotExactInt32(jit, number, result, isNotInt32, numberScratch);
        boxExactInt32(jit, result, result);
        jit.returnToCaller();
        isNotInt32.link(&jit);
        boxDouble(jit, element, result);
        jit.returnToCaller();
    }

    isAbsent.link(&jit);
    jit.move(TrustedImm64(MAX_ARRAY_INDEX), shape);
    Jump isName = jit.branch64(CCallHelpers::Above, position, shape);
    isAbsentAtIndex.link(&jit);
    Label isFoundNowhere = jit.label();
    jit.move(question == ElementQuestion::Get ? TrustedImm64(JSValue::ValueUndefined) : TrustedImm64(0), result);
    jit.returnToCaller();

    isName.link(&jit);
    const GPRReg structure = butterfly;
    callsOperation.append(jit.branchIfNotType(array, ArrayType));
    loadStructure(jit, array, structure);
    callsOperation.append(jit.branchTest32(CCallHelpers::Zero, Address(structure, Structure::bitFieldOffset()), TrustedImm32(Structure::s_hasPristineElementsBits)));
    branchIfHasNamesOrIsNotOfThisRealm(jit, structure, callsOperation);
    jit.jump().linkTo(isFoundNowhere, &jit);

    callsOperation.link(&jit);
    jit.boxInt52(position, position, shape, number);
    callOperationAndReturn(jit, question);
}

static void answerAtNumber(StubAssembler& jit, ElementQuestion question)
{
    const FPRReg position = jit.operandFPR(1);
    const GPRReg index = jit.allocateScratch(StubRegister::A2);
    const FPRReg converted = jit.allocateScratch(FPRInfo::argumentFPR1);

    jit.truncateDoubleToInt64(position, index);
    jit.convertInt64ToDouble(index, converted);
    Jump isNotInteger = jit.branchDouble(CCallHelpers::DoubleNotEqualOrUnordered, position, converted);
    checkStubOperands(stubAtIndexFor(question), { jit.operand(0), index });
    jit.jumpToStub(stubAtIndexFor(question));

    isNotInteger.link(&jit);
    boxDouble(jit, PureDouble::purify(jit, position, converted), index);
    callOperationAndReturn(jit, question);
}

static StubSignature signatureAtIndex()
{
    return StubSignature::withGPRResult({ gprOf(StubRegister::A1), gprOf(StubRegister::A2) });
}

static StubSignature signatureAtNumber()
{
    return StubSignature::withGPRResult({ gprOf(StubRegister::A1), FPRInfo::argumentFPR0 });
}

StubSignature signatureOfArrayGetAtIndex(StubKey) { return signatureAtIndex(); }
StubSignature signatureOfArrayHasAtIndex(StubKey) { return signatureAtIndex(); }
StubSignature signatureOfArrayGetAtNumber(StubKey) { return signatureAtNumber(); }
StubSignature signatureOfArrayHasAtNumber(StubKey) { return signatureAtNumber(); }

void generateArrayGetAtIndex(StubAssembler& jit) { answerAtIndex(jit, ElementQuestion::Get); }
void generateArrayHasAtIndex(StubAssembler& jit) { answerAtIndex(jit, ElementQuestion::Has); }
void generateArrayGetAtNumber(StubAssembler& jit) { answerAtNumber(jit, ElementQuestion::Get); }
void generateArrayHasAtNumber(StubAssembler& jit) { answerAtNumber(jit, ElementQuestion::Has); }

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
