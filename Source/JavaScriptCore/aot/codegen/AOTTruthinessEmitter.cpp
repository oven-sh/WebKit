/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTruthinessEmitter.h"

#if ENABLE(AOT)

#include "AOTConversionEmitter.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTSlotRepresentation.h"
#include "AOTStubsValues.h"
#include "AOTTypeArena.h"
#include "JSCJSValueInlines.h"

#if COMPILER(CLANG)
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
#endif

namespace JSC { namespace AOT { namespace Typed {

static constexpr bool areBitsOfNaN(uint64_t bits)
{
    return (bits >> 52 & 0x7ff) == 0x7ff && bits << 12;
}

static_assert(areBitsOfNaN(undefinedDouble) && areBitsOfNaN(nullDouble) && areBitsOfNaN(absentDouble));
static_assert(!static_cast<uint32_t>(undefinedInteger) && !static_cast<uint32_t>(nullInteger) && !static_cast<uint32_t>(absentInteger));

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

void TruthinessEmitter::branchOnTruthiness(const OperationContext& context, Operand operand, const Type& type, BranchTargets targets, bool expectsObjectOrNothing, bool expectsInt32OrNothing, bool isAnswerOfCall, bool expectsBoolean)
{
    if (operand.is<Boxed>()) {
        branchOnTruthinessOfAnyValue(context, operand.as<Boxed>(), type, targets, expectsObjectOrNothing, expectsInt32OrNothing, isAnswerOfCall, expectsBoolean);
        return;
    }
    branch(isTruthy(context, operand, type), targets);
}

void TruthinessEmitter::branchOnTruthinessOfAnyValue(const OperationContext& context, Boxed value, const Type& type, BranchTargets targets, bool expectsObjectOrNothing, bool expectsInt32OrNothing, bool isAnswerOfCall, bool expectsBoolean)
{
    auto branchInStub = [&] {
        LValue operands[] = { value.value() };
        emitter<RuntimeCallEmitter>().callStubAndBranch(context, valueIsTruthy(), operands, { }, targets);
    };
    bool expectsSmallValues = type.primitives & (Primitive::UndefinedOrNull | Primitive::Boolean);
    bool expectsObjects = type.hasObjects() && !(type.primitives & Primitive::Absent);
    static constexpr OpcodeSet conversionsToBoolean { Opcode::ToBoolean };
    if (!expectsSmallValues && !expectsObjects) {
        m_out.noteFormOf(conversionsToBoolean, Form::Closed, StateOfKnowledge::BranchOnOtherKinds);
        branchInStub();
        return;
    }
    FormAtPlace named = formByLoopAlone(Opcode::ToBoolean, StateOfKnowledge::BranchOnTestedKinds, context.block() && generator().isInLoop(context.block()));
    m_out.noteFormOf(conversionsToBoolean, named.form, StateOfKnowledge::BranchOnTestedKinds, named.place);
    if (expectsBoolean && (type.primitives & Primitive::Boolean) == Primitive::Boolean) {
        LBasicBlock isNotFalse = newBlock();
        LBasicBlock isNoBoolean = newBlock();
        m_out.branch(equal(value, boxedConstant(jsBoolean(false))).value(), targets.notTaken, normalEdge(isNotFalse));
        m_out.appendTo(isNotFalse);
        m_out.branch(equal(value, boxedConstant(jsBoolean(true))).value(), targets.taken, rareEdge(isNoBoolean));
        m_out.appendTo(isNoBoolean);
        branchInStub();
        return;
    }
    if (named.form != Form::Open) {
        bool asksForObject = expectsObjectOrNothing && expectsObjects;
        bool asksForInt32 = expectsInt32OrNothing && type.primitives & Primitive::Number;
        if ((!asksForObject && !asksForInt32) || !expectsSmallValues || !context.block() || generator().code().at(context.block()).frequency == BlockFrequency::Rare) {
            branchInStub();
            return;
        }
        LBasicBlock isNotUndefined = newBlock();
        LBasicBlock isOfExpectedKind = newBlock();
        LBasicBlock isOfOtherKind = newBlock();
        m_out.branch(equal(value, undefined()).value(), targets.notTaken, normalEdge(isNotUndefined));
        m_out.appendTo(isNotUndefined);
        m_out.branch((asksForObject ? isCell(value) : isInt32(value)).value(), normalEdge(isOfExpectedKind), normalEdge(isOfOtherKind));
        m_out.appendTo(isOfExpectedKind);
        if (asksForObject)
            m_out.branch(isObjectCell(unboxCell(value)).value(), targets.taken, normalEdge(isOfOtherKind));
        else
            branch(isNotZero(low32(value)), targets);
        m_out.appendTo(isOfOtherKind);
        branchInStub();
        return;
    }

    LBasicBlock otherwise = newBlock();
    Boxed trueValue = boxedConstant(jsBoolean(true));
    static_assert(JSValue::ValueEmpty < JSValue::ValueUndefined && JSValue::ValueNull < JSValue::ValueUndefined && JSValue::ValueFalse < JSValue::ValueUndefined && JSValue::ValueTrue < JSValue::ValueUndefined && JSValue::ValueUndefined < JSValue::DoubleEncodeOffset);
    auto edgeToNextTest = [&](bool hasNextTest) {
        return hasNextTest ? normalEdge(newBlock()) : rareEdge(otherwise);
    };
    bool asksForBooleansFirst = isAnswerOfCall && expectsObjects && type.primitives & Primitive::Boolean;
    if (asksForBooleansFirst) {
        LBasicBlock isNotFalse = newBlock();
        LBasicBlock isNoBoolean = newBlock();
        m_out.branch(equal(value, boxedConstant(jsBoolean(false))).value(), targets.notTaken, normalEdge(isNotFalse));
        m_out.appendTo(isNotFalse);
        m_out.branch(equal(value, trueValue).value(), targets.taken, normalEdge(isNoBoolean));
        m_out.appendTo(isNoBoolean);
    }
    if (expectsObjects) {
        LBasicBlock isCellCase = newBlock();
        BlockEdge isOtherCase = edgeToNextTest(expectsSmallValues);
        m_out.branch(isCell(value).value(), normalEdge(isCellCase), isOtherCase);
        m_out.appendTo(isCellCase);
        CellPointer cell = unboxCell(value);
        Condition isTruthyObject = isObjectCell(cell);
        if (generator().isLayingOutTurns()) {
            LBasicBlock isOtherCell = newBlock();
            m_out.branch(isTruthyObject.value(), targets.taken, normalEdge(isOtherCell));
            m_out.appendTo(isOtherCell);
            branch(isTruthyCell(cell, type), targets);
        } else
            m_out.branch(isTruthyObject.value(), targets.taken, rareEdge(otherwise));
        m_out.appendTo(isOtherCase.block());
    }
    if (expectsSmallValues) {
        if (!asksForBooleansFirst) {
            LBasicBlock isNotTrue = newBlock();
            m_out.branch(equal(value, trueValue).value(), targets.taken, normalEdge(isNotTrue));
            m_out.appendTo(isNotTrue);
        }
        m_out.branch(belowOrEqual(value, undefined()).value(), targets.notTaken, edgeToNextTest(false));
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
    if (type.primitives & Primitive::Absent && type.hasCells()) {
        Type present = withoutPrimitives(type, Primitive::Absent);
        return selectByBranch<Condition>(isNotEmpty(value), [&] {
            return isTruthyBoxed(context, value, present);
        }, [&] {
            return boolean(false);
        });
    }
    if (type.primitives & Primitive::Number) {
        Type rest = withoutPrimitives(type, Primitive::Number);
        Join<Condition> join(m_out);
        auto when = [&](Condition condition, const auto& answer) {
            LBasicBlock holds = newBlock();
            LBasicBlock next = newBlock();
            ScopeFork fork = m_out.forkScope();
            m_out.branch(condition.value(), normalEdge(holds), normalEdge(next));
            m_out.appendToArm(holds, fork);
            join.add(answer());
            m_out.appendToArm(next, fork);
            return fork;
        };
        ScopeFork firstFork = when(isInt32(value), [&] {
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
        return join.finishBehind(firstFork);
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

Type TruthinessEmitter::withoutPrimitives(const Type& type, uint16_t removed)
{
    return types().at(types().withoutPrimitives(types().intern(type), removed));
}

} } } // namespace JSC::AOT::Typed

#if COMPILER(CLANG)
#pragma clang attribute pop
#endif

#endif // ENABLE(AOT)
