/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTruthinessEmitter.h"

#if ENABLE(AOT)

#include "AOTCompileOptions.h"
#include "AOTConversionEmitter.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTSlotRepresentation.h"
#include "AOTStubsValues.h"
#include "B3FrequencyClass.h"
#include "JSCJSValueInlines.h"
#include "JSTypeInfo.h"

namespace JSC { namespace AOT { namespace Typed {

static constexpr bool areBitsOfNaN(uint64_t bits)
{
    return (bits >> 52 & 0x7ff) == 0x7ff && bits << 12;
}

static_assert(areBitsOfNaN(undefinedDouble) && areBitsOfNaN(nullDouble) && areBitsOfNaN(absentDouble));
static_assert(!static_cast<uint32_t>(undefinedInteger) && !static_cast<uint32_t>(nullInteger) && !static_cast<uint32_t>(absentInteger));
static_assert(absentCell32 < emptyLiteralStringWord && nullCell32 < emptyLiteralStringWord && undefinedCell32 < emptyLiteralStringWord);

TruthinessEmitter::TruthinessEmitter(CodeGenerator& generator)
    : OperationEmitter(generator)
{
}

Condition TruthinessEmitter::isTruthy(const OperationContext& context, Operand operand, const Type& type)
{
    switch (operand.representation) {
    case Representation::Boolean:
        return operand.as<Condition>();
    case Representation::Int32:
        return isNotZero(operand.as<Word32>());
    case Representation::Int64:
        return isNotZero(operand.as<Integer>());
    case Representation::Double:
    case Representation::DoubleWithPayload:
    case Representation::ImpureDouble:
        return isNeitherZeroNorNaN(operand.as<Number>());
    case Representation::OptionalDouble:
        return isNeitherZeroNorNaN(emitter<ConversionEmitter>().withoutMarkers(operand).as<Number>());
    case Representation::OptionalInt64: {
        ConversionEmitter& conversions = emitter<ConversionEmitter>();
        Integer integerOrMarker = conversions.withoutMarkers(operand).as<Integer>();
        if (type.integers.isSubsetOf({ -static_cast<int64_t>(UINT32_MAX), UINT32_MAX }))
            return isNotZero(low32(integerOrMarker));
        Condition isMarker = conversions.isMarker(operand);
        return logicalNot(logicalOr(isMarker, isZero(integerOrMarker)));
    }
    case Representation::Cell:
        return isTruthyCell(operand.as<CellPointer>(), type);
    case Representation::Cell32: {
        NarrowReference word = operand.as<NarrowReference>();
        if (!(type.primitives & (Primitive::AnyString | Primitive::BigInt)))
            return isCellReference(word);
        return selectByBranch<Condition>(isCellReference(word), [&] {
            return isTruthyCell(decode(word), type);
        }, [&] {
            return boolean(false);
        });
    }
    case Representation::LiteralString:
        return above(operand.as<LiteralStringWord>(), LiteralStringWord(m_out.constInt64(emptyLiteralStringWord)));
    case Representation::JSValue:
        return isTruthyBoxed(context, operand.as<Boxed>(), type);
    case Representation::Void:
        if (type.hasOnlyPrimitives(Primitive::UndefinedOrNull | Primitive::Absent))
            return boolean(false);
        break;
    case Representation::RawWord:
    case Representation::Unchosen:
        break;
    }
    fail(context, InternalErrorKind::UnsupportedConversion);
    return boolean(false);
}

void TruthinessEmitter::branchOnTruthiness(const OperationContext& context, Operand operand, const Type& type, BranchTargets targets, Expects expects)
{
    if (operand.is<Boxed>()) {
        branchOnTruthinessOfAnyValue(context, operand.as<Boxed>(), type, targets, expects);
        return;
    }
    branch(isTruthy(context, operand, type), targets);
}

void TruthinessEmitter::branchOnTruthinessOfAnyValue(const OperationContext& context, Boxed value, const Type& type, BranchTargets targets, Expects expects)
{
    auto branchInStub = [&] {
        LValue operands[] = { value.value() };
        emitter<RuntimeCallEmitter>().callStubAndBranch(context, valueIsTruthy(), operands, { }, targets);
    };
    bool expectsSmallValues = type.primitives & (Primitive::UndefinedOrNull | Primitive::Boolean);
    bool expectsObjects = type.hasObjects();
    bool expectsNumbers = expects == Expects::Numbers && type.primitives & Primitive::Number;
    if (context.shape() != CodeShape::Fast || (!expectsSmallValues && !expectsObjects)) {
        branchInStub();
        return;
    }

    LBasicBlock otherwise = newBlock();
    // What is neither a cell nor one of the small values is a number.
    LBasicBlock isLargeAndNoCell = expectsNumbers && expectsObjects && expectsSmallValues ? newBlock() : nullptr;
    Boxed trueValue = boxedConstant(jsBoolean(true));
    static_assert(JSValue::ValueNull < JSValue::ValueUndefined && JSValue::ValueFalse < JSValue::ValueUndefined && JSValue::ValueTrue < JSValue::ValueUndefined && JSValue::ValueUndefined < JSValue::DoubleEncodeOffset);
    bool expectsFalse = B3::isRarerThan(targets.taken.frequency(), targets.notTaken.frequency());
    bool testsSmallValuesFirst = expectsFalse && expectsSmallValues;
    bool testsSmallValuesLast = !expectsFalse && expectsSmallValues;
    auto edgeToNextTest = [&](bool hasNextTest) {
        return hasNextTest ? normalEdge(newBlock()) : isLargeAndNoCell ? unlikelyEdge(isLargeAndNoCell) : rareEdge(otherwise);
    };
    if (testsSmallValuesFirst) {
        LBasicBlock isSmallCase = newBlock();
        B3::FrequentedBlock isLargeCase = edgeToNextTest(expectsObjects);
        m_out.branch(belowOrEqual(value, undefined()).value(), normalEdge(isSmallCase), isLargeCase);
        m_out.appendTo(isSmallCase);
        branch(equal(value, trueValue), targets);
        m_out.appendTo(isLargeCase.block());
    }
    if (expectsObjects) {
        LBasicBlock isCellCase = newBlock();
        B3::FrequentedBlock isOtherCase = edgeToNextTest(testsSmallValuesLast);
        m_out.branch(isCell(value).value(), normalEdge(isCellCase), isOtherCase);
        m_out.appendTo(isCellCase);
        CellPointer cell = unboxCell(value);
        Condition isTruthyObject = isObjectCell(cell);
        if (build().recordedOptions().has(TestingSwitch::MayEncounterMasqueradesAsUndefined))
            isTruthyObject = logicalAnd(isTruthyObject, hasNoBits(loadByte(cell, m_heaps.JSCell_typeInfoFlags), MasqueradesAsUndefined));
        m_out.branch(isTruthyObject.value(), targets.taken, rareEdge(otherwise));
        m_out.appendTo(isOtherCase.block());
    }
    if (testsSmallValuesLast) {
        LBasicBlock isNotTrue = newBlock();
        m_out.branch(equal(value, trueValue).value(), targets.taken, normalEdge(isNotTrue));
        m_out.appendTo(isNotTrue);
        m_out.branch(belowOrEqual(value, undefined()).value(), targets.notTaken, edgeToNextTest(false));
        m_out.appendTo(isLargeAndNoCell ? isLargeAndNoCell : otherwise);
    }
    if (isLargeAndNoCell) {
        LBasicBlock isInt32Case = newBlock();
        LBasicBlock isDoubleCase = newBlock();
        m_out.branch(isInt32(value).value(), normalEdge(isInt32Case), normalEdge(isDoubleCase));
        m_out.appendTo(isInt32Case);
        branch(isNotZero(low32(value)), targets);
        m_out.appendTo(isDoubleCase);
        branch(isNeitherZeroNorNaN(unboxDouble(value)), targets);
        m_out.appendTo(otherwise);
    }
    branchInStub();
}

Condition TruthinessEmitter::isTruthyCell(CellPointer cell, const Type& type)
{
    Condition isNotEmptyString = type.primitives & Primitive::AnyString ? notEqual(cell, emptyString()) : boolean(true);
    if (!(type.primitives & Primitive::BigInt))
        return isNotEmptyString;
    return either<Condition>(hasCellType(cell, HeapBigIntType), [&] {
        return isNotZero(loadField<Word32>(cell, m_heaps.JSBigInt_length));
    }, [&] {
        return isNotEmptyString;
    });
}

Condition TruthinessEmitter::isTruthyBoxed(const OperationContext& context, Boxed value, const Type& type)
{
    if (type.primitives & Primitive::Number) {
        Type rest = type.withoutPrimitives(Primitive::Number);
        Join<Condition> join(m_out);
        auto when = [&](Condition condition, const auto& answer) {
            LBasicBlock holds = newBlock();
            LBasicBlock next = newBlock();
            m_out.branch(condition.value(), normalEdge(holds), normalEdge(next));
            m_out.appendTo(holds);
            join.add(answer());
            m_out.appendTo(next);
        };
        when(isInt32(value), [&] {
            return isNotZero(low32(value));
        });
        auto doubleCase = [&] {
            return isNeitherZeroNorNaN(unboxDouble(value));
        };
        if (rest.isNever())
            join.add(doubleCase());
        else {
            when(isNumber(value), doubleCase);
            join.add(isTruthyBoxed(context, value, rest));
        }
        return join.finish();
    }
    auto isTrue = [&] {
        return type.primitives & Primitive::True ? equal(value, boxedConstant(jsBoolean(true))) : boolean(false);
    };
    if (!type.hasCells())
        return isTrue();
    if (!(type.primitives & ~cellPrimitives))
        return isTruthyCell(unboxCell(value), type);
    if (!(type.primitives & (Primitive::AnyString | Primitive::BigInt))) {
        if (!(type.primitives & Primitive::True))
            return isCell(value);
        static_assert(JSValue::ValueTrue & 1 && !(JSValue::ValueFalse & 1) && !(JSValue::ValueNull & 1) && !(JSValue::ValueUndefined & 1));
        return Condition(m_out.notEqual(m_out.bitAnd(value.value(), 3), JSValue::OtherTag));
    }
    return selectByBranch<Condition>(isCell(value), [&] {
        return isTruthyCell(unboxCell(value), type);
    }, isTrue);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
