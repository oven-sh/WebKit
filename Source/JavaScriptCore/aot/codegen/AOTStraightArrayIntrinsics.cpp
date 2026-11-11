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
        Opcode::HasContiguousIndex, Opcode::GetContiguousIndex, Opcode::GetDoubleIndex,
        Opcode::NewContiguousArray, Opcode::NewArrayLike,
        Opcode::TryPutByValDirect, Opcode::TryPutIndex, Opcode::TryCopyFilledElements, Opcode::TryPutElements, Opcode::SortNumbers, Opcode::TryFillNewArray,
        Opcode::TruncateNewArray,
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

static void loadIntegerOperandOfList(StraightEmitter& emitter, ValueID operand, GPRReg integer)
{
    CCallHelpers& jit = emitter.jit();
    if (std::optional<JSValue> constant = emitter.constantOf(operand); constant && constant->isAnyInt()) {
        jit.move(CCallHelpers::TrustedImm64(constant->asAnyInt()), integer);
        return;
    }
    emitter.load(operand, integer);
    CCallHelpers::Jump isInt32 = jit.branchIfInt32(integer);
    FPRReg number = emitter.takeResultFPR();
    jit.unboxDoubleWithoutAssertions(integer, integer, number);
    jit.truncateDoubleToInt64(number, integer);
    CCallHelpers::Jump isConverted = jit.jump();
    isInt32.link(&jit);
    jit.signExtend32ToPtr(integer, integer);
    isConverted.link(&jit);
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

static void generateIsPristineArray(StraightEmitter& emitter, const BodyValue& value)
{
    static_assert(DerivedArrayType == ArrayType + 1);
    CCallHelpers& jit = emitter.jit();
    GPRReg tested = StraightEmitter::temporary(1);
    emitter.load(value.valueOperand(), tested);
    GPRReg result = emitter.takeResultRegister();
    jit.move(CCallHelpers::TrustedImm32(0), result);
    CCallHelpers::JumpList isNoArray;
    isNoArray.append(jit.branchIfNotCell(tested));
    isNoArray.append(jit.branchIfNotType(tested, JSTypeRange { ArrayType, DerivedArrayType }));
    jit.test8(CCallHelpers::NonZero, CCallHelpers::Address(tested, JSCell::typeInfoFlagsOffset()), CCallHelpers::TrustedImm32(JSC::IsPristine), result);
    isNoArray.link(&jit);
    emitter.defineBoolean(value, result);
}

static void generateArrayStorageKind(StraightEmitter& emitter, const BodyValue& value)
{
    static_assert(Int32Shape + 2 == DoubleShape && DoubleShape + 2 == ContiguousShape && Int32Shape >= 2);
    static_assert(!static_cast<int32_t>(ArrayStorage::Other) && static_cast<int32_t>(ArrayStorage::Int32) == 1 && static_cast<int32_t>(ArrayStorage::Double) == 2 && static_cast<int32_t>(ArrayStorage::Contiguous) == 3);
    CCallHelpers& jit = emitter.jit();
    GPRReg kind = emitter.loadIntoResultRegister(value.arrayOperand());
    jit.load8(CCallHelpers::Address(kind, JSCell::indexingTypeAndMiscOffset()), kind);
    jit.and32(CCallHelpers::TrustedImm32(arrayStorageTestMask), kind);
    jit.sub32(CCallHelpers::TrustedImm32(Int32Shape - 2), kind);
    jit.urshift32(CCallHelpers::TrustedImm32(1), kind);
    CCallHelpers::Jump hasForm = jit.branch32(CCallHelpers::BelowOrEqual, kind, CCallHelpers::TrustedImm32(static_cast<int32_t>(ArrayStorage::Contiguous)));
    jit.move(CCallHelpers::TrustedImm32(static_cast<int32_t>(ArrayStorage::Other)), kind);
    hasForm.link(&jit);
    emitter.defineInt32(value, kind);
}

static void generateNewArrayOfHoles(StraightEmitter& emitter, const BodyValue& value, ValueID source, ArrayStorage kind, uint32_t flags)
{
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
    case Opcode::HasContiguousIndex: {
        GPRReg result = takeResultRegister();
        loadContiguousElementOrEmpty(*this, value, result);
        jit().test64(CCallHelpers::NonZero, result, result, result);
        defineBoolean(value, result);
        return;
    }
    case Opcode::GetContiguousIndex: {
        GPRReg result = takeResultRegister();
        loadContiguousElementOrEmpty(*this, value, result);
        CCallHelpers::Jump isFilled = jit().branchTest64(CCallHelpers::NonZero, result);
        jit().move(CCallHelpers::TrustedImm64(JSValue::encode(jsUndefined())), result);
        isFilled.link(&jit());
        define(value, result);
        return;
    }
    case Opcode::GetDoubleIndex: {
        CCallHelpers::JumpList isAbsent;
        GPRReg result = takeResultRegister();
        jit().move(CCallHelpers::TrustedImm64(static_cast<int64_t>(arrayStorageFormOf(ArrayStorage::Double).hole)), result);
        jit().load64(elementBelowLengthOfList(*this, value, ArrayStorage::Double, isAbsent), result);
        isAbsent.link(&jit());
        jit().move64ToDouble(result, takeResultFPR());
        defineDouble(value);
        return;
    }
    case Opcode::NewContiguousArray:
        generateNewArrayOfHoles(*this, value, ValueID(), ArrayStorage::Contiguous, NewArrayWithHolesFlag::mustHaveIndexingType);
        return;
    case Opcode::NewArrayLike:
        generateNewArrayOfHoles(*this, value, value.sourceOperand(), value.sourceOperand() && !isConstant(value.sourceOperand()) ? ArrayStorage::Contiguous : ArrayStorage::Int32, 0);
        return;
    case Opcode::TryCopyFilledElements:
        load(value.arrayOperand(), temporary(1));
        jit().loadPtr(CCallHelpers::Address(temporary(1), JSObject::butterflyOffset()), temporary(1));
        jit().store32(CCallHelpers::TrustedImm32(0), CCallHelpers::Address(temporary(1), Butterfly::offsetOfPublicLength()));
        [[fallthrough]];
    case Opcode::TryPutByValDirect:
    case Opcode::TryPutIndex:
    case Opcode::TryPutElements:
    case Opcode::SortNumbers:
    case Opcode::TryFillNewArray:
        jit().move(CCallHelpers::TrustedImm32(JSValue::ValueFalse), takeResultRegister());
        define(value);
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
