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
        if (std::optional<Word32> narrow = narrowWordBehind(operand.as<OptionalInteger>())) {
            if (!emittersCheckProofs() && type.integers.minimum >= 0)
                return logicalNot(lessThan(*narrow, 1));
            return logicalNot(logicalOr(conversions.isMarker(operand), isZero(*narrow)));
        }
        Integer integerOrMarker(operand.value);
        if (!emittersCheckProofs() && type.integers.isSubsetOf({ -static_cast<int64_t>(UINT32_MAX), UINT32_MAX }))
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

ValueID TruthinessEmitter::shapeTestAfter(const ValueContext& question)
{
    constexpr unsigned mostBlocks = 3;
    ValueID source = definitionOf(question->valueOperand());
    auto isSource = [&](ValueID id) {
        return definitionOf(id) == source && representationOf(id) == Representation::JSValue;
    };
    BlockID from = question.block();
    const Block& asking = m_code.at(from);
    if (asking.terminator.kind != TerminatorKind::Branch || asking.terminator.conditionOperand() != question.id())
        return { };
    EdgeID way = asking.terminator.taken();
    for (unsigned number = 0; number < mostBlocks; ++number) {
        const Edge& edge = m_code.at(way);
        const Block& next = m_code.at(edge.to);
        if (!edge.arguments().empty() || generator().singlePredecessorOf(edge.to) != from || next.frequency == BlockFrequency::Rare)
            return { };
        ValueID test = generator().testOfBranch(edge.to);
        for (ValueID id : m_code.valuesOf(next)) {
            if (id != test && (m_code.at(id).opcode != Opcode::Narrowed || !isSource(id)))
                return { };
        }
        if (test) {
            const Operation& asked = m_code.at(test);
            return asked.opcode == Opcode::ShapeTest && !asked.asksInFull() && isSource(asked.objectOperand()) ? test : ValueID();
        }
        if (next.terminator.kind != TerminatorKind::Jump)
            return { };
        from = edge.to;
        way = next.terminator.next();
    }
    return { };
}

bool TruthinessEmitter::tryDeferToShapeTest(const ValueContext& question, Boxed value)
{
    const Type& type = typeOf(question->valueOperand());
    if (!type.hasObjects() || !(type.primitives & Primitive::UndefinedOrNull) || type.primitives & Primitive::Absent)
        return false;
    if (generator().isLayingOutTurns() || AOT::hasFlag(build().options().testing, Wire::TestingSwitch::CheckEffectsAtRunTime))
        return false;
    bool isInLoop = generator().usesLoopForms(question.block());
    if (!isInLoop && (!question->expectsObjectOrNothing() || m_code.at(question.block()).frequency == BlockFrequency::Rare))
        return false;
    ValueID test = shapeTestAfter(question);
    if (!test)
        return false;
    static constexpr OpcodeSet conversionsToBoolean { Opcode::ToBoolean };
    FormAtPlace named = formByLoopAlone(Opcode::ToBoolean, StateOfKnowledge::BranchOnTestedKinds, isInLoop);
    m_out.noteFormOf(conversionsToBoolean, named.form, StateOfKnowledge::BranchOnTestedKinds, named.place);
    BranchTargets targets = question.branchTargets();
    m_deferredTruthTests.append({ test, question->valueOperand(), value, targets.notTaken });
    generator().expectOtherJumps(m_code.at(m_code.at(question.block()).terminator.notTaken()).to);
    build().statistics().count(StatisticID::TruthTestsDeferredToShapeTests);
    LBasicBlock isNoCell = newBlock();
    m_out.branch(isCell(value).value(), targets.taken, normalEdge(isNoCell));
    m_out.appendTo(isNoCell);
    if (!isInLoop) {
        m_out.branch(equal(value, undefined()).value(), targets.notTaken, rareEdge(targets.taken.block()));
        return true;
    }
    if (type.primitives & Primitive::Boolean) {
        LBasicBlock isNotTrue = newBlock();
        m_out.branch(equal(value, boxedConstant(jsBoolean(true))).value(), targets.taken, normalEdge(isNotTrue));
        m_out.appendTo(isNotTrue);
    }
    m_out.branch(belowOrEqual(value, undefined()).value(), targets.notTaken, rareEdge(targets.taken.block()));
    return true;
}

auto TruthinessEmitter::takeTruthTestOf(ValueID shapeTest) -> std::optional<DeferredTruthTest>
{
    for (size_t index = m_deferredTruthTests.size(); index--;) {
        if (m_deferredTruthTests[index].shapeTest == shapeTest) {
            DeferredTruthTest found = m_deferredTruthTests[index];
            m_deferredTruthTests.removeAt(index);
            return found;
        }
    }
    return std::nullopt;
}

void TruthinessEmitter::branchOnShapeOrTruth(const ValueContext& shapeTest, const DeferredTruthTest& deferred, BranchTargets shape, BlockEdge isTruthy, MarkedObjects marked)
{
    LBasicBlock isCellBlock = newBlock();
    LBasicBlock isOtherCell = newBlock();
    LBasicBlock isOtherValue = newBlock();
    CellPointer cell = unboxCell(deferred.value);
    m_out.branch(isCell(deferred.value).value(), normalEdge(isCellBlock), rareEdge(isOtherValue));
    m_out.appendTo(isCellBlock);
    emitter<ShapeTestEmitter>().branchOnShapeIn(cell, shapeTest->layouts(), typeOf(deferred.source), { shape.taken, BlockEdge(isOtherCell, isTruthy.edgeClass()) }, marked);
    m_out.appendTo(isOtherCell);
    m_out.branch(isObjectCell(cell).value(), isTruthy, rareEdge(isOtherValue));
    m_out.appendTo(isOtherValue);
    LValue operands[] = { deferred.value.value() };
    emitter<RuntimeCallEmitter>().callStubAndBranch(shapeTest, valueIsTruthy(), operands, { }, { isTruthy, deferred.isFalsy });
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
    FormAtPlace named = formByLoopAlone(Opcode::ToBoolean, StateOfKnowledge::BranchOnTestedKinds, context.block() && generator().usesLoopForms(context.block()));
    m_out.noteFormOf(conversionsToBoolean, named.form, StateOfKnowledge::BranchOnTestedKinds, named.place);
    if (expectsBoolean && (type.primitives & Primitive::Boolean) == Primitive::Boolean) {
        LBasicBlock isSmallValue = newBlock();
        LBasicBlock isNoSmallValue = newBlock();
        m_out.branch(belowOrEqual(value, undefined()).value(), normalEdge(isSmallValue), rareEdge(isNoSmallValue));
        m_out.appendTo(isSmallValue);
        m_out.branch(equal(value, boxedConstant(jsBoolean(true))).value(), targets.taken, targets.notTaken);
        m_out.appendTo(isNoSmallValue);
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
        LBasicBlock isOfExpectedKind = newBlock();
        LBasicBlock isNotOfExpectedKind = newBlock();
        LBasicBlock isOfOtherKind = newBlock();
        m_out.branch((asksForObject ? isCell(value) : isInt32(value)).value(), normalEdge(isOfExpectedKind), normalEdge(isNotOfExpectedKind));
        m_out.appendTo(isOfExpectedKind);
        if (asksForObject)
            m_out.branch(isObjectCell(unboxCell(value)).value(), targets.taken, rareEdge(isOfOtherKind));
        else
            branch(isNotZero(int32InBox(value)), targets);
        m_out.appendTo(isNotOfExpectedKind);
        m_out.branch(equal(value, undefined()).value(), targets.notTaken, rareEdge(isOfOtherKind));
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
    bool isOfLibrary = isLibraryCode(m_program, m_program.at(generator().code().inlineFrames()[context.origin().inlineFrame].callee));
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
        } else if (isOfLibrary && (type.primitives & Primitive::AnyString)) {
            LBasicBlock isNoObject = newBlock();
            LBasicBlock isString = newBlock();
            m_out.branch(isTruthyObject.value(), targets.taken, normalEdge(isNoObject));
            m_out.appendTo(isNoObject);
            m_out.branch(hasCellType(cell, StringType).value(), normalEdge(isString), rareEdge(otherwise));
            m_out.appendTo(isString);
            branch(notEqual(cell, emptyString()), targets);
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
        if (isOfLibrary && (type.primitives & Primitive::Number)) {
            LBasicBlock isNoSmallValue = newBlock();
            LBasicBlock isInteger = newBlock();
            m_out.branch(belowOrEqual(value, undefined()).value(), targets.notTaken, normalEdge(isNoSmallValue));
            m_out.appendTo(isNoSmallValue);
            m_out.branch(isInt32(value).value(), normalEdge(isInteger), rareEdge(otherwise));
            m_out.appendTo(isInteger);
            branch(isNotZero(int32InBox(value)), targets);
        } else
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
            return isNotZero(int32InBox(value));
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
