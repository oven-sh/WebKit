/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStraightEmitter.h"

#if ENABLE(AOT)

#include "AOTArrayAllocationProfile.h"
#include "AOTArrayStorageForms.h"
#include "Butterfly.h"
#include "JSObject.h"
#include "JSTypeInfo.h"
#include <bit>

namespace JSC { namespace AOT { namespace Typed {

OpcodeSet StraightEmitter::opcodesOfArrayIntrinsics()
{
    return {
        Opcode::IsPristineArray, Opcode::ArrayStorageKind,
        Opcode::HasContiguousIndex, Opcode::GetContiguousIndex, Opcode::GetDoubleIndex, Opcode::PutContiguousIndex, Opcode::PutDoubleIndex,
        Opcode::GetOwnContiguousIndex, Opcode::GetOwnDoubleIndex, Opcode::GetOwnInt32Index,
        Opcode::PutOwnContiguousIndex, Opcode::PutOwnDoubleIndex, Opcode::PutOwnInt32Index,
        Opcode::NewContiguousArray, Opcode::NewDoubleArray, Opcode::NewArrayLike, Opcode::NewArrayWithCapacity,
        Opcode::TryPutByValDirect, Opcode::TryPutIndex, Opcode::TryCopyFilledElements, Opcode::TryPutElements, Opcode::TryFillNewArray,
        Opcode::GrowNewArray, Opcode::TruncateNewArray,
    };
}

static constexpr int32_t bitsOfShapesWithoutBoxedElements = IndexingShapeMask & ~(ContiguousShape - Int32Shape);

static_assert([] {
    for (uint32_t byte = 0; byte <= 0xff; ++byte) {
        uint32_t shape = byte & IndexingShapeMask;
        bool passesTest = !((byte - Int32Shape) & bitsOfShapesWithoutBoxedElements);
        if (passesTest != (shape == Int32Shape || shape == ContiguousShape))
            return false;
    }
    return true;
}());
static_assert(!arrayStorageFormOf(ArrayStorage::Int32).hole && !arrayStorageFormOf(ArrayStorage::Contiguous).hole);
static_assert(std::endian::native == std::endian::little);

// An integer lies in its slot as an int32 or as a double.
static void loadIntegerOperandOfList(StraightEmitter& emitter, ValueID operand, GPRReg integer)
{
    CCallHelpers& jit = emitter.jit();
    if (std::optional<JSValue> constant = emitter.constantOf(operand); constant && constant->isAnyInt()) {
        jit.move(CCallHelpers::TrustedImm64(constant->asAnyInt()), integer);
        return;
    }
    emitter.load(operand, integer);
    CCallHelpers::Jump isInt32 = jit.branchIfInt32(integer);
    jit.unboxDoubleWithoutAssertions(integer, integer, StraightEmitter::resultFPR);
    jit.truncateDoubleToInt64(StraightEmitter::resultFPR, integer);
    CCallHelpers::Jump isConverted = jit.jump();
    isInt32.link(&jit);
    jit.signExtend32ToPtr(integer, integer);
    isConverted.link(&jit);
}

static CCallHelpers::BaseIndex elementOfList(StraightEmitter& emitter, const BodyValue& value)
{
    GPRReg storage = StraightEmitter::temporary(1);
    GPRReg position = StraightEmitter::temporary(2);
    emitter.load(value.arrayOperand(), storage);
    emitter.jit().loadPtr(CCallHelpers::Address(storage, JSObject::butterflyOffset()), storage);
    loadIntegerOperandOfList(emitter, value.positionOperand(), position);
    return CCallHelpers::BaseIndex(storage, position, CCallHelpers::TimesEight);
}

static CCallHelpers::BaseIndex elementBelowLengthOfList(StraightEmitter& emitter, const BodyValue& value, ArrayStorage kind, CCallHelpers::JumpList& isAbsent)
{
    CCallHelpers& jit = emitter.jit();
    GPRReg storage = StraightEmitter::temporary(1);
    GPRReg position = StraightEmitter::temporary(2);
    GPRReg word = StraightEmitter::temporary(3);
    emitter.load(value.arrayOperand(), storage);
    jit.load8(CCallHelpers::Address(storage, JSCell::indexingTypeAndMiscOffset()), word);
    if (kind == ArrayStorage::Double) {
        jit.and32(CCallHelpers::TrustedImm32(arrayStorageTestMask), word);
        isAbsent.append(jit.branch32(CCallHelpers::NotEqual, word, CCallHelpers::TrustedImm32(DoubleShape)));
    } else {
        jit.sub32(CCallHelpers::TrustedImm32(Int32Shape), word);
        isAbsent.append(jit.branchTest32(CCallHelpers::NonZero, word, CCallHelpers::TrustedImm32(bitsOfShapesWithoutBoxedElements)));
    }
    jit.loadPtr(CCallHelpers::Address(storage, JSObject::butterflyOffset()), storage);
    loadIntegerOperandOfList(emitter, value.positionOperand(), position);
    jit.load32(CCallHelpers::Address(storage, Butterfly::offsetOfPublicLength()), word);
    isAbsent.append(jit.branch64(CCallHelpers::AboveOrEqual, position, word));
    return CCallHelpers::BaseIndex(storage, position, CCallHelpers::TimesEight);
}

static void loadContiguousElementOrEmpty(StraightEmitter& emitter, const BodyValue& value, GPRReg element)
{
    CCallHelpers& jit = emitter.jit();
    CCallHelpers::JumpList isAbsent;
    jit.move(CCallHelpers::TrustedImm32(0), element);
    jit.load64(elementBelowLengthOfList(emitter, value, ArrayStorage::Contiguous, isAbsent), element);
    isAbsent.link(&jit);
}

static void defineBitsOfDouble(StraightEmitter& emitter, const BodyValue& value, GPRReg bits)
{
    CCallHelpers& jit = emitter.jit();
    jit.withNumberTag([&](auto tag) {
        jit.sub64(tag, bits);
    });
    emitter.define(value, bits);
}

static void generateIsPristineArray(StraightEmitter& emitter, const BodyValue& value)
{
    static_assert(DerivedArrayType == ArrayType + 1);
    CCallHelpers& jit = emitter.jit();
    GPRReg tested = StraightEmitter::temporary(1);
    emitter.load(value.valueOperand(), tested);
    jit.move(CCallHelpers::TrustedImm32(0), StraightEmitter::resultGPR);
    CCallHelpers::JumpList isNoArray;
    isNoArray.append(jit.branchIfNotCell(tested));
    isNoArray.append(jit.branchIfNotType(tested, JSTypeRange { ArrayType, DerivedArrayType }));
    jit.test8(CCallHelpers::NonZero, CCallHelpers::Address(tested, JSCell::typeInfoFlagsOffset()), CCallHelpers::TrustedImm32(JSC::IsPristine), StraightEmitter::resultGPR);
    isNoArray.link(&jit);
    emitter.defineBoolean(value);
}

static void generateArrayStorageKind(StraightEmitter& emitter, const BodyValue& value)
{
    static_assert(Int32Shape + 2 == DoubleShape && DoubleShape + 2 == ContiguousShape && Int32Shape >= 2);
    static_assert(!static_cast<int32_t>(ArrayStorage::Other) && static_cast<int32_t>(ArrayStorage::Int32) == 1 && static_cast<int32_t>(ArrayStorage::Double) == 2 && static_cast<int32_t>(ArrayStorage::Contiguous) == 3);
    CCallHelpers& jit = emitter.jit();
    GPRReg kind = StraightEmitter::resultGPR;
    emitter.load(value.arrayOperand(), kind);
    jit.load8(CCallHelpers::Address(kind, JSCell::indexingTypeAndMiscOffset()), kind);
    jit.and32(CCallHelpers::TrustedImm32(arrayStorageTestMask), kind);
    jit.sub32(CCallHelpers::TrustedImm32(Int32Shape - 2), kind);
    jit.urshift32(CCallHelpers::TrustedImm32(1), kind);
    CCallHelpers::Jump hasForm = jit.branch32(CCallHelpers::BelowOrEqual, kind, CCallHelpers::TrustedImm32(static_cast<int32_t>(ArrayStorage::Contiguous)));
    jit.move(CCallHelpers::TrustedImm32(static_cast<int32_t>(ArrayStorage::Other)), kind);
    hasForm.link(&jit);
    emitter.defineInt32(value, kind);
}

static void generatePutContiguousIndex(StraightEmitter& emitter, const BodyValue& value)
{
    CCallHelpers::BaseIndex element = elementOfList(emitter, value);
    emitter.load(value.valueOperand(), StraightEmitter::resultGPR);
    emitter.jit().store64(StraightEmitter::resultGPR, element);
    if (emitter.isConstant(value.valueOperand()))
        return;
    GPRReg array = StraightEmitter::temporary(1);
    emitter.load(value.arrayOperand(), array);
    emitter.barrierAfterStore(array);
}

static void generateNewArrayOfHoles(StraightEmitter& emitter, const BodyValue& value, ValueID source, ArrayStorage kind, uint32_t flags)
{
    static constexpr int32_t maxFusedArrayCapacity = 268435456;
    CCallHelpers& jit = emitter.jit();
    GPRReg sourceCell = operationArgumentGPR(1);
    GPRReg length = operationArgumentGPR(2);
    StraightArgument sourceOrNull = StraightArgument::word(0);
    if (source && !emitter.isConstant(source)) {
        emitter.load(source, sourceCell);
        CCallHelpers::Jump isCell = jit.branchIfCell(sourceCell);
        jit.move(CCallHelpers::TrustedImm32(0), sourceCell);
        isCell.link(&jit);
        sourceOrNull = StraightArgument::inRegister(sourceCell);
    }
    loadIntegerOperandOfList(emitter, value.lengthOperand(), length);
    if (flags & NewArrayWithHolesFlag::lengthIsCapacity) {
        CCallHelpers::Jump fits = jit.branch64(CCallHelpers::Below, length, CCallHelpers::TrustedImm32(maxFusedArrayCapacity));
        jit.move(CCallHelpers::TrustedImm32(maxFusedArrayCapacity), length);
        fits.link(&jit);
    }
    emitter.callOperation(value, RuntimeOperation::NewArrayWithHoles, { sourceOrNull, StraightArgument::inRegister(length), StraightArgument::word(arrayStorageFormOf(kind).indexingType), StraightArgument::word(flags) });
    emitter.define(value);
}

static void callWithListAndLength(StraightEmitter& emitter, const BodyValue& value, RuntimeOperation operation)
{
    GPRReg length = operationArgumentGPR(2);
    loadIntegerOperandOfList(emitter, value.lengthOperand(), length);
    emitter.callOperation(value, operation, { value.arrayOperand(), StraightArgument::inRegister(length) });
}

void StraightEmitter::generateArrayIntrinsic(const BodyValue& value)
{
    switch (value.opcode()) {
    case Opcode::IsPristineArray:
        generateIsPristineArray(*this, value);
        return;
    case Opcode::ArrayStorageKind:
        generateArrayStorageKind(*this, value);
        return;
    case Opcode::HasContiguousIndex:
        loadContiguousElementOrEmpty(*this, value, resultGPR);
        jit().test64(CCallHelpers::NonZero, resultGPR, resultGPR, resultGPR);
        defineBoolean(value);
        return;
    case Opcode::GetContiguousIndex: {
        loadContiguousElementOrEmpty(*this, value, resultGPR);
        CCallHelpers::Jump isFilled = jit().branchTest64(CCallHelpers::NonZero, resultGPR);
        jit().move(CCallHelpers::TrustedImm64(JSValue::encode(jsUndefined())), resultGPR);
        isFilled.link(&jit());
        define(value);
        return;
    }
    case Opcode::GetDoubleIndex: {
        CCallHelpers::JumpList isAbsent;
        jit().move(CCallHelpers::TrustedImm64(static_cast<int64_t>(arrayStorageFormOf(ArrayStorage::Double).hole)), resultGPR);
        jit().load64(elementBelowLengthOfList(*this, value, ArrayStorage::Double, isAbsent), resultGPR);
        isAbsent.link(&jit());
        defineBitsOfDouble(*this, value, resultGPR);
        return;
    }
    case Opcode::GetOwnContiguousIndex:
        jit().load64(elementOfList(*this, value), resultGPR);
        define(value);
        return;
    case Opcode::GetOwnDoubleIndex:
        jit().load64(elementOfList(*this, value), resultGPR);
        defineBitsOfDouble(*this, value, resultGPR);
        return;
    case Opcode::GetOwnInt32Index:
        jit().load32(elementOfList(*this, value), resultGPR);
        defineInt32(value);
        return;
    case Opcode::PutContiguousIndex:
    case Opcode::PutOwnContiguousIndex:
        generatePutContiguousIndex(*this, value);
        return;
    case Opcode::PutDoubleIndex:
    case Opcode::PutOwnDoubleIndex: {
        CCallHelpers::BaseIndex element = elementOfList(*this, value);
        load(value.valueOperand(), resultGPR);
        unboxNumber(resultGPR, resultFPR);
        jit().storeDouble(resultFPR, element);
        return;
    }
    case Opcode::PutOwnInt32Index: {
        CCallHelpers::BaseIndex element = elementOfList(*this, value);
        loadIntegerOperandOfList(*this, value.valueOperand(), resultGPR);
        jit().store32(resultGPR, element);
        return;
    }
    case Opcode::NewContiguousArray:
        generateNewArrayOfHoles(*this, value, ValueID(), ArrayStorage::Contiguous, NewArrayWithHolesFlag::mustHaveIndexingType);
        return;
    case Opcode::NewDoubleArray:
        generateNewArrayOfHoles(*this, value, ValueID(), ArrayStorage::Double, NewArrayWithHolesFlag::mustHaveIndexingType);
        return;
    case Opcode::NewArrayLike:
        generateNewArrayOfHoles(*this, value, value.sourceOperand(), ArrayStorage::Contiguous, 0);
        return;
    case Opcode::NewArrayWithCapacity:
        generateNewArrayOfHoles(*this, value, value.sourceOperand(), ArrayStorage::Contiguous, NewArrayWithHolesFlag::lengthIsCapacity);
        return;
    // Behind a no the library stores the ordinary way, which is right for every list. A list that was not filled has length 0.
    case Opcode::TryCopyFilledElements:
        load(value.arrayOperand(), temporary(1));
        jit().loadPtr(CCallHelpers::Address(temporary(1), JSObject::butterflyOffset()), temporary(1));
        jit().store32(CCallHelpers::TrustedImm32(0), CCallHelpers::Address(temporary(1), Butterfly::offsetOfPublicLength()));
        [[fallthrough]];
    case Opcode::TryPutByValDirect:
    case Opcode::TryPutIndex:
    case Opcode::TryPutElements:
    case Opcode::TryFillNewArray:
        jit().move(CCallHelpers::TrustedImm32(JSValue::ValueFalse), resultGPR);
        define(value);
        return;
    case Opcode::GrowNewArray:
        callWithListAndLength(*this, value, RuntimeOperation::GrowNewArray);
        return;
    case Opcode::TruncateNewArray:
        callWithListAndLength(*this, value, RuntimeOperation::TruncateNewArray);
        define(value);
        return;
    default:
        fail(value);
        return;
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
