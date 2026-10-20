/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypeTestEmitter.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTConversionEmitter.h"
#include "AOTPlacement.h"
#include "AOTSlotRepresentation.h"
#include "AOTThrowEmitter.h"
#include "B3PatchpointValue.h"
#include "B3StackmapGenerationParams.h"
#include "B3ValueInlines.h"
#include "CCallHelpers.h"
#include "JSCJSValueInlines.h"
#include <bit>
#include <wtf/MathExtras.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint16_t markerPrimitives = Primitive::UndefinedOrNull | Primitive::Absent;
static constexpr uint16_t markerKinds[] = { Primitive::Undefined, Primitive::Null, Primitive::Absent };

static_assert(isNaNConstExpr(std::bit_cast<double>(undefinedDouble)) && isNaNConstExpr(std::bit_cast<double>(nullDouble)) && isNaNConstExpr(std::bit_cast<double>(absentDouble)));
static_assert(boxedMarkers.absent < boxedMarkers.undefined && boxedMarkers.null < boxedMarkers.undefined && boxedMarkers.undefined < emptyLiteralStringWord);
static_assert(cell32Markers.absent < minCell32 && cell32Markers.undefined < minCell32 && cell32Markers.null < minCell32);

static TypeTestVerdict verdictOfCells(const TypeTestPlan& plan)
{
    if (plan.everyCellPasses())
        return TypeTestVerdict::AlwaysPasses;
    return plan.someCellPasses() ? TypeTestVerdict::NeedsTest : TypeTestVerdict::NeverPasses;
}

static TypeTestVerdict verdictOfNumbers(const TypeTestPlan& plan)
{
    if (!(plan.failingPrimitives & Primitive::Number) && !plan.integers)
        return TypeTestVerdict::AlwaysPasses;
    return plan.passingPrimitives & Primitive::Number ? TypeTestVerdict::NeedsTest : TypeTestVerdict::NeverPasses;
}

static Condition isFractional(Output& out, Number number)
{
    return Condition(out.doubleNotEqualAndOrdered(out.doubleTrunc(number.value()), number.value()));
}

TypeTestEmitter::TypeTestEmitter(CodeGenerator& generator)
    : OperationEmitter(generator)
{
}

void TypeTestEmitter::forEachNonCellCase(Boxed value, const TypeTestPlan& plan, const auto& visit)
{
    auto visitConstant = [&](JSValue constant) {
        visit([&] {
            return equal(value, boxedConstant(constant));
        });
    };

    if (plan.passingPrimitives & Primitive::Number) {
        visit([&] {
            return boxedNumberPasses(value, plan);
        });
    }

    uint16_t passingBooleans = plan.passingPrimitives & Primitive::Boolean;
    if (passingBooleans == Primitive::Boolean) {
        visit([&] {
            return isBoolean(value);
        });
    } else if (passingBooleans == Primitive::True)
        visitConstant(jsBoolean(true));
    else if (passingBooleans == Primitive::False)
        visitConstant(jsBoolean(false));

    uint16_t passingNullish = plan.passingPrimitives & Primitive::UndefinedOrNull;
    if (passingNullish == Primitive::UndefinedOrNull) {
        visit([&] {
            return plan.occurringPrimitives() & ~(cellPrimitives | Primitive::UndefinedOrNull) ? isUndefinedOrNull(value, plan.occurringPrimitives()) : isNotCell(value);
        });
    } else if (passingNullish == Primitive::Undefined)
        visitConstant(jsUndefined());
    else if (passingNullish == Primitive::Null)
        visitConstant(jsNull());
}

const TypeTestPlan& TypeTestEmitter::planFor(TypeID from, Representation representation, TypeID to)
{
    RELEASE_ASSERT(from && to);
    uint64_t key = static_cast<uint64_t>(from.index()) << 32 | to.index();
    return *m_plans[static_cast<size_t>(representation)].ensure(key, [&] {
        return makeUniqueWithoutFastMallocCheck<TypeTestPlan>(computeTypeTestPlan(m_program, from, representation, to));
    }).iterator->value;
}

void TypeTestEmitter::emitBranch(Operand operand, const TypeTestPlan& plan, BranchTargets targets)
{
    RELEASE_ASSERT(operand.representation == plan.representation);
    switch (plan.verdict) {
    case TypeTestVerdict::AlwaysPasses:
        m_out.jump(targets.taken.block());
        return;
    case TypeTestVerdict::NeverPasses:
        m_out.jump(targets.notTaken.block());
        return;
    case TypeTestVerdict::NeedsTest:
        break;
    }
    if (std::optional<Condition> condition = tryGetSingleCondition(operand, plan)) {
        branch(*condition, targets);
        return;
    }
    test(operand, plan, { .target = targets.notTaken });
    jump(targets.taken);
}

Condition TypeTestEmitter::emitBoolean(Operand operand, const TypeTestPlan& plan)
{
    RELEASE_ASSERT(operand.representation == plan.representation);
    switch (plan.verdict) {
    case TypeTestVerdict::AlwaysPasses:
        return boolean(true);
    case TypeTestVerdict::NeverPasses:
        return boolean(false);
    case TypeTestVerdict::NeedsTest:
        break;
    }
    if (std::optional<Condition> condition = tryGetSingleCondition(operand, plan))
        return *condition;
    return asBoolean([&](BranchTargets targets) {
        emitBranch(operand, plan, targets);
    });
}

void TypeTestEmitter::emitThrow(const OperationContext& context, Operand operand, const TypeTestPlan& plan, TypeSiteID site)
{
    RELEASE_ASSERT(operand.representation == plan.representation);
    switch (plan.verdict) {
    case TypeTestVerdict::AlwaysPasses:
        return;
    case TypeTestVerdict::NeverPasses: {
        // A throw site without an operand reports undefined.
        bool isNullWithoutBits = operand.representation == Representation::Void && m_program.at(plan.from).primitives & Primitive::Null;
        emitter<ThrowEmitter>().throwTypeMismatch(context, isNullWithoutBits ? Operand(boxedConstant(jsNull())) : operand, plan.to, site);
        return;
    }
    case TypeTestVerdict::NeedsTest:
        break;
    }

    bool readsCells = !plan.cellTests.isEmpty() || !plan.stringLiterals.isEmpty();
    bool mayBeAbsent = plan.occurringPrimitives() & Primitive::Absent;
    if (context.shape() == CodeShape::Compact && operand.is<Boxed>() && readsCells && !mayBeAbsent && m_placement.typesNeededAtRunTime().get(plan.to.index())) {
        build().statistics().count(StatisticID::TypeChecksByOperation);
        callOperation<Boxed>(context, RuntimeOperation::EnforceType, { operand, word32(plan.to.index()), word32(site.index()) });
        return;
    }

    build().statistics().count(StatisticID::TypeChecksInB3);
    Failure failure { &context, emitter<ThrowEmitter>().typeMismatchSite(plan.to, site), operand, { } };
    test(operand, plan, failure);
}

void TypeTestEmitter::leaveUnless(Condition condition, const Failure& failure)
{
    if (condition.value()->hasInt32()) {
        if (!condition.value()->asInt32())
            leave(failure);
        return;
    }
    if (failure.thrower) {
        emitter<ThrowEmitter>().requireOrThrow(*failure.thrower, condition, failure.throwSite, failure.thrown);
        return;
    }
    LBasicBlock continuation = newBlock();
    m_out.branch(condition.value(), normalEdge(continuation), failure.target);
    m_out.appendTo(continuation);
}

void TypeTestEmitter::leave(const Failure& failure)
{
    if (failure.thrower) {
        emitter<ThrowEmitter>().throwAt(*failure.thrower, failure.throwSite, failure.thrown);
        return;
    }
    jump(failure.target);
    m_out.appendTo(newBlock());
}

void TypeTestEmitter::jump(B3::FrequentedBlock edge)
{
    m_out.m_block->setSuccessors(edge);
    m_out.m_block->appendNew<B3::Value>(m_proc, B3::Jump, m_out.origin());
}

std::optional<Condition> TypeTestEmitter::tryGetSingleCondition(Operand operand, const TypeTestPlan& plan)
{
    auto decideByMarker = [&](TypeTestVerdict payload) -> std::optional<Condition> {
        if (payload == TypeTestVerdict::NeedsTest)
            return std::nullopt;
        return passesByMarker(operand, plan, payload == TypeTestVerdict::AlwaysPasses);
    };

    switch (operand.representation) {
    case Representation::Boolean:
        return booleanPasses(operand.as<Condition>(), plan);
    case Representation::Int32:
        return integerPasses(signExtended(operand.as<Word32>()), plan);
    case Representation::Int64:
        return integerPasses(operand.as<Integer>(), plan);
    case Representation::Double:
        return doublePasses(operand.as<Number>(), plan);
    case Representation::OptionalInt64:
        return decideByMarker(verdictOfNumbers(plan));
    case Representation::OptionalDouble:
        // A marker is a NaN, so the test for an integer rejects it.
        if (verdictOfNumbers(plan) == TypeTestVerdict::NeedsTest && !(plan.passingPrimitives & (markerPrimitives | Primitive::Double)))
            return isIntegerIn(emitter<ConversionEmitter>().withoutMarkers(operand).as<Number>(), plan.integers.value_or(Range::full()));
        return decideByMarker(verdictOfNumbers(plan));
    case Representation::Cell:
        return tryGetSingleCondition(operand.as<CellPointer>(), plan);
    case Representation::Cell32:
    case Representation::LiteralString:
        return decideByMarker(verdictOfCells(plan));
    case Representation::JSValue:
        return tryGetSingleCondition(operand.as<Boxed>(), plan);
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::RawWord:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

std::optional<Condition> TypeTestEmitter::tryGetSingleCondition(Boxed value, const TypeTestPlan& plan)
{
    uint16_t others = plan.occurringPrimitives() & ~cellPrimitives;
    if (others & Primitive::Absent)
        return tryGetConditionWithAbsent(value, plan);
    if (plan.everyCellPasses() && plan.everyNonCellPasses())
        return boolean(true);
    if (!plan.cellsOccur() || !plan.someCellPasses())
        return tryGetNonCellCondition(value, plan);
    if (!plan.everyCellPasses()) {
        if (others)
            return std::nullopt;
        return tryGetSingleCondition(unboxCell(value), plan);
    }
    if (!(others & plan.passingPrimitives))
        return isCell(value);

    uint16_t failingOthers = others & plan.failingPrimitives;
    if (failingOthers && !(failingOthers & ~Primitive::UndefinedOrNull) && !plan.integers) {
        if (failingOthers == Primitive::Undefined)
            return notEqual(value, undefined());
        if (failingOthers == Primitive::Null)
            return notEqual(value, boxedConstant(jsNull()));
        return logicalNot(isUndefinedOrNull(value, plan.occurringPrimitives()));
    }
    return std::nullopt;
}

std::optional<Condition> TypeTestEmitter::tryGetConditionWithAbsent(Boxed value, const TypeTestPlan& plan)
{
    static_assert(!JSValue::ValueEmpty && JSValue::ValueNull == 2, "no other value has bits as small as these two");
    if (plan.someCellPasses())
        return std::nullopt;
    if (plan.passingPrimitives == Primitive::Absent)
        return isEmpty(value);
    if (plan.passingPrimitives == (Primitive::Absent | Primitive::Null))
        return belowOrEqual(value, boxedConstant(jsNull()));
    return std::nullopt;
}

std::optional<Condition> TypeTestEmitter::tryGetNonCellCondition(Boxed value, const TypeTestPlan& plan)
{
    if (verdictOfNumbers(plan) == TypeTestVerdict::NeedsTest)
        return std::nullopt;

    unsigned numberOfCases = 0;
    forEachNonCellCase(value, plan, [&](const auto&) {
        ++numberOfCases;
    });
    if (numberOfCases > 1)
        return std::nullopt;
    return nonCellPasses(value, plan);
}

void TypeTestEmitter::test(Operand operand, const TypeTestPlan& plan, const Failure& failure)
{
    switch (operand.representation) {
    case Representation::Boolean:
        testCondition(operand.as<Condition>(), plan, failure);
        return;
    case Representation::Int32:
        testWord32(operand.as<Word32>(), plan, failure);
        return;
    case Representation::Int64:
        testInteger(operand.as<Integer>(), plan, failure);
        return;
    case Representation::Double:
        testNumber(operand.as<Number>(), plan, failure);
        return;
    case Representation::OptionalInt64:
        testOptionalInteger(operand.as<OptionalInteger>(), plan, failure);
        return;
    case Representation::OptionalDouble:
        testOptionalNumber(operand.as<OptionalNumber>(), plan, failure);
        return;
    case Representation::Cell:
        testCell(operand.as<CellPointer>(), plan, failure);
        return;
    case Representation::Cell32:
        testNarrowReference(operand.as<NarrowReference>(), plan, failure);
        return;
    case Representation::JSValue:
        testBoxed(operand.as<Boxed>(), plan, failure);
        return;
    case Representation::LiteralString:
        testLiteralString(operand.as<LiteralStringWord>(), plan, failure);
        return;
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::RawWord:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void TypeTestEmitter::testBoxed(Boxed value, const TypeTestPlan& plan, const Failure& failure)
{
    if (std::optional<Condition> condition = tryGetSingleCondition(value, plan)) {
        leaveUnless(*condition, failure);
        return;
    }

    if (plan.occurringPrimitives() & Primitive::Absent) {
        TypeTestPlan present = plan;
        present.passingPrimitives &= ~Primitive::Absent;
        present.failingPrimitives &= ~Primitive::Absent;
        if (plan.failingPrimitives & Primitive::Absent) {
            leaveUnless(isNotEmpty(value), failure);
            testBoxed(value, present, failure);
            return;
        }
        LBasicBlock isPresent = newBlock();
        LBasicBlock continuation = newBlock();
        m_out.branch(isEmpty(value).value(), unlikelyEdge(continuation), normalEdge(isPresent));
        m_out.appendTo(isPresent);
        testBoxed(value, present, failure);
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        return;
    }

    if (!plan.cellsOccur() || !plan.someCellPasses()) {
        testNonCell(value, plan, failure);
        return;
    }

    CellPointer cell = unboxCell(value);
    if (!(plan.passingPrimitives & ~cellPrimitives)) {
        if (plan.occurringPrimitives() & ~cellPrimitives)
            leaveUnless(isCell(value), failure);
        testCell(cell, plan, failure);
        return;
    }

    bool everyOtherPasses = plan.everyNonCellPasses();
    LBasicBlock cellCase = newBlock();
    LBasicBlock continuation = newBlock();
    LBasicBlock otherCase = everyOtherPasses ? continuation : newBlock();
    m_out.branch(isCell(value).value(), normalEdge(cellCase), everyOtherPasses ? rareEdge(otherCase) : unlikelyEdge(otherCase));
    m_out.appendTo(cellCase);
    testCell(cell, plan, failure);
    m_out.jump(continuation);
    if (!everyOtherPasses) {
        m_out.appendTo(otherCase);
        testNonCell(value, plan, failure);
        m_out.jump(continuation);
    }
    m_out.appendTo(continuation);
}

void TypeTestEmitter::testNonCell(Boxed value, const TypeTestPlan& plan, const Failure& failure)
{
    if (verdictOfNumbers(plan) != TypeTestVerdict::NeedsTest) {
        testCases(failure, [&](const auto& visit) {
            forEachNonCellCase(value, plan, visit);
        });
        return;
    }

    TypeTestPlan withoutNumbers = plan;
    withoutNumbers.passingPrimitives &= ~Primitive::Number;
    withoutNumbers.failingPrimitives &= ~Primitive::Number;
    withoutNumbers.integers = std::nullopt;
    if (!(withoutNumbers.passingPrimitives & ~cellPrimitives)) {
        leaveUnless(isNumber(value), failure);
        testBoxedNumber(value, plan, failure);
        return;
    }

    LBasicBlock numberCase = newBlock();
    LBasicBlock otherCase = newBlock();
    LBasicBlock continuation = newBlock();
    m_out.branch(isNumber(value).value(), normalEdge(numberCase), unlikelyEdge(otherCase));
    m_out.appendTo(numberCase);
    testBoxedNumber(value, plan, failure);
    m_out.jump(continuation);
    m_out.appendTo(otherCase);
    testNonCell(value, withoutNumbers, failure);
    m_out.jump(continuation);
    m_out.appendTo(continuation);
}

void TypeTestEmitter::testBoxedNumber(Boxed number, const TypeTestPlan& plan, const Failure& failure)
{
    ifElse(isInt32(number), [&] {
        bool everyInt32Passes = plan.integers && Range::int32().isSubsetOf(*plan.integers);
        if (!everyInt32Passes)
            leaveUnless(integerPasses(signExtended(unboxInt32(number)), plan), failure);
    }, [&] {
        leaveUnless(doublePasses(unboxDouble(number), plan), failure);
    });
}

void TypeTestEmitter::testWithMarkers(Operand operand, const TypeTestPlan& plan, const Failure& failure, NOESCAPE const Invocable<void()> auto& testPayload)
{
    if (std::optional<Condition> condition = tryGetSingleCondition(operand, plan)) {
        leaveUnless(*condition, failure);
        return;
    }

    bool someMarkerPasses = plan.passingPrimitives & markerPrimitives;
    bool someMarkerFails = plan.failingPrimitives & markerPrimitives;
    if (!someMarkerPasses) {
        if (someMarkerFails)
            leaveUnless(logicalNot(isMarker(operand)), failure);
        testPayload();
        return;
    }

    LBasicBlock payloadCase = newBlock();
    LBasicBlock continuation = newBlock();
    LBasicBlock markerCase = someMarkerFails ? newBlock() : continuation;
    m_out.branch(isMarker(operand).value(), unlikelyEdge(markerCase), normalEdge(payloadCase));
    m_out.appendTo(payloadCase);
    testPayload();
    m_out.jump(continuation);
    if (someMarkerFails) {
        m_out.appendTo(markerCase);
        leaveUnless(passesByMarker(operand, plan, true), failure);
        m_out.jump(continuation);
    }
    m_out.appendTo(continuation);
}

void TypeTestEmitter::testNarrowReference(NarrowReference word, const TypeTestPlan& plan, const Failure& failure)
{
    testWithMarkers(word, plan, failure, [&] {
        testCell(decode(word), plan, failure);
    });
}

void TypeTestEmitter::testNumber(Number number, const TypeTestPlan& plan, const Failure& failure)
{
    leaveUnless(doublePasses(number, plan), failure);
}

void TypeTestEmitter::testInteger(Integer value, const TypeTestPlan& plan, const Failure& failure)
{
    leaveUnless(integerPasses(value, plan), failure);
}

void TypeTestEmitter::testWord32(Word32 value, const TypeTestPlan& plan, const Failure& failure)
{
    testInteger(signExtended(value), plan, failure);
}

void TypeTestEmitter::testCondition(Condition value, const TypeTestPlan& plan, const Failure& failure)
{
    leaveUnless(booleanPasses(value, plan), failure);
}

void TypeTestEmitter::testOptionalNumber(OptionalNumber value, const TypeTestPlan& plan, const Failure& failure)
{
    testWithMarkers(value, plan, failure, [&] {
        testNumber(emitter<ConversionEmitter>().withoutMarkers(value).as<Number>(), plan, failure);
    });
}

void TypeTestEmitter::testOptionalInteger(OptionalInteger value, const TypeTestPlan& plan, const Failure& failure)
{
    testWithMarkers(value, plan, failure, [&] {
        testInteger(emitter<ConversionEmitter>().withoutMarkers(value).as<Integer>(), plan, failure);
    });
}

void TypeTestEmitter::testLiteralString(LiteralStringWord word, const TypeTestPlan& plan, const Failure& failure)
{
    testWithMarkers(word, plan, failure, [&] {
        ConversionEmitter& conversions = emitter<ConversionEmitter>();
        LBasicBlock isLiteral = newBlock();
        LBasicBlock isStringCell = newBlock();
        Join<LiteralStringWord> literal(m_out);
        m_out.branch(conversions.isStringCell(word).value(), unlikelyEdge(isStringCell), normalEdge(isLiteral));
        m_out.appendTo(isLiteral);
        literal.add(word);
        m_out.appendTo(isStringCell);
        literal.add(conversions.toLiteralString(CellPointer(word.value())));
        leaveUnless(isOneOfLiteralStringWords(literal.finish(), plan), failure);
    });
}

Condition TypeTestEmitter::isMarker(Operand operand)
{
    switch (operand.representation) {
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
        return emitter<ConversionEmitter>().isMarker(operand);
    case Representation::Cell32:
        return logicalNot(isCellReference(operand.as<NarrowReference>()));
    case Representation::LiteralString:
        return belowOrEqual(operand.as<LiteralStringWord>(), LiteralStringWord(m_out.constInt64(boxedMarkers.undefined)));
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::Boolean:
    case Representation::Int32:
    case Representation::Int64:
    case Representation::Double:
    case Representation::Cell:
    case Representation::JSValue:
    case Representation::RawWord:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Condition TypeTestEmitter::isMarker(Operand operand, uint16_t primitive)
{
    uint64_t marker = ConversionEmitter::markerBits(primitive, operand.representation);
    if (operand.is<NarrowReference>())
        return equal(operand.as<NarrowReference>(), NarrowReference(m_out.constInt32(static_cast<int32_t>(marker))));
    return emitter<ConversionEmitter>().isMarker(operand, marker);
}

Condition TypeTestEmitter::passesByMarker(Operand operand, const TypeTestPlan& plan, bool payloadPasses)
{
    uint16_t agreeing = (payloadPasses ? plan.passingPrimitives : plan.failingPrimitives) & markerPrimitives;
    uint16_t disagreeing = (payloadPasses ? plan.failingPrimitives : plan.passingPrimitives) & markerPrimitives;
    if (!disagreeing)
        return boolean(payloadPasses);

    Condition disagrees;
    if (!agreeing)
        disagrees = isMarker(operand);
    else {
        for (uint16_t primitive : markerKinds) {
            if (disagreeing & primitive)
                disagrees = logicalOr(disagrees, isMarker(operand, primitive));
        }
    }
    return payloadPasses ? logicalNot(disagrees) : disagrees;
}

Condition TypeTestEmitter::nonCellPasses(Boxed value, const TypeTestPlan& plan)
{
    Condition result;
    forEachNonCellCase(value, plan, [&](const auto& emitTest) {
        result = logicalOr(result, emitTest());
    });
    return result ? result : boolean(false);
}

Condition TypeTestEmitter::boxedNumberPasses(Boxed value, const TypeTestPlan& plan)
{
    if (verdictOfNumbers(plan) == TypeTestVerdict::AlwaysPasses)
        return isNumber(value);
    return either<Condition>(isInt32(value), [&] {
        if (plan.integers && Range::int32().isSubsetOf(*plan.integers))
            return boolean(true);
        return integerPasses(signExtended(unboxInt32(value)), plan);
    }, [&] {
        return either<Condition>(isNumber(value), [&] {
            return doublePasses(unboxDouble(value), plan);
        }, [&] {
            return boolean(false);
        });
    });
}

Condition TypeTestEmitter::doublePasses(Number number, const TypeTestPlan& plan)
{
    switch (verdictOfNumbers(plan)) {
    case TypeTestVerdict::AlwaysPasses:
        return boolean(true);
    case TypeTestVerdict::NeverPasses:
        return boolean(false);
    case TypeTestVerdict::NeedsTest:
        break;
    }

    uint16_t passing = plan.passingPrimitives & Primitive::Number;
    uint16_t failing = plan.failingPrimitives & Primitive::Number;
    if (failing == Primitive::FractionalNumber && !plan.integers)
        return logicalNot(isFractional(m_out, number));
    if (passing == Primitive::FractionalNumber)
        return isFractional(m_out, number);
    if (passing == Primitive::Integer)
        return isIntegerIn(number, plan.integers.value_or(Range::full()));

    Condition otherNumberPasses = boolean(true);
    if (failing & Primitive::Double)
        otherNumberPasses = passing & Primitive::FractionalNumber ? isFractional(m_out, number) : logicalNot(isFractional(m_out, number));
    Integer truncated = emitter<ConversionEmitter>().toInteger(number);
    Condition isInteger = isExactIntegerIn(number, truncated, Range::full());
    return select(isInteger, integerPasses(truncated, plan), otherNumberPasses);
}

Condition TypeTestEmitter::integerPasses(Integer value, const TypeTestPlan& plan)
{
    if (!(plan.passingPrimitives & Primitive::Integer))
        return boolean(false);
    if (!plan.integers)
        return boolean(true);

    Range range = *plan.integers;
    Range known = m_program.at(plan.from).integers;
    uint64_t maximum = static_cast<uint64_t>(range.maximum);
    if (known.minimum >= 0 && range.minimum <= 0 && range.maximum > 0 && !(maximum & (maximum + 1)))
        return Condition(m_out.testIsZero64(value.value(), m_out.constInt64(~range.maximum)));
    if (known.minimum >= range.minimum)
        return lessThanOrEqual(value, integer(range.maximum));
    if (known.maximum <= range.maximum)
        return greaterThanOrEqual(value, integer(range.minimum));
    return isInRange(value, range);
}

Condition TypeTestEmitter::booleanPasses(Condition value, const TypeTestPlan& plan)
{
    return plan.passingPrimitives & Primitive::True ? value : logicalNot(value);
}

Condition TypeTestEmitter::isInRange(Integer value, Range range)
{
    if (range.isEmpty())
        return boolean(false);
    if (range.isSingleValue())
        return equal(value, integer(range.minimum));
    if (range == Range::int32())
        return equal(signExtended(low32(value)), value);
    Integer fromMinimum = range.minimum ? sub(value, integer(range.minimum)) : value;
    return belowOrEqual(fromMinimum, integer(static_cast<int64_t>(static_cast<uint64_t>(range.maximum) - static_cast<uint64_t>(range.minimum))));
}

Condition TypeTestEmitter::isExactIntegerIn(Number number, Integer truncated, Range range)
{
    ConversionEmitter& conversions = emitter<ConversionEmitter>();
    Integer bitsOfTruncated = conversions.bitsOf(toNumber(truncated));
    Condition isExact = equal(bitsOfTruncated, conversions.bitsOf(number));
    return logicalAnd(isExact, isInRange(truncated, range));
}

Condition TypeTestEmitter::isIntegerIn(Number number, Range range)
{
    ConversionEmitter& conversions = emitter<ConversionEmitter>();
#if CPU(ARM64)
    if (MacroAssembler::supportsDoubleToInt32ConversionUsingJavaScriptSemantics(build().targetFeatures()) && range.isSubsetOf(Range::int32())) {
        B3::PatchpointValue* patchpoint = m_out.patchpoint(B3::Int32);
        patchpoint->append(B3::ConstrainedValue(number.value(), B3::ValueRep::SomeRegister));
        patchpoint->effects = B3::Effects::none();
        patchpoint->setRunsWithoutFrame(true);
        patchpoint->setGenerator([](CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
            jit.convertDoubleToInt32UsingJavaScriptSemantics(params[1].fpr(), params[0].gpr());
            jit.m_assembler.cset<32>(params[0].gpr(), ARM64Assembler::ConditionEQ);
        });
        Condition isExactInt32(patchpoint);
        if (Range::int32().isSubsetOf(range))
            return isExactInt32;
        return logicalAnd(isExactInt32, isInRange(conversions.toInteger(number), range));
    }
#endif
    return isExactIntegerIn(number, conversions.toInteger(number), range);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
