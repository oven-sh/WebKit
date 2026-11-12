/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypeTestEmitter.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTConstantEmitter.h"
#include "AOTConversionEmitter.h"
#include "AOTSlotRepresentation.h"
#include "AOTTypeArena.h"
#include "AOTTypeRelations.h"
#include "CCallHelpers.h"
#include "JSCJSValueInlines.h"
#include <bit>
#include <wtf/MathExtras.h>
#include <wtf/SetForScope.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint16_t testedMarkerPrimitives = Primitive::UndefinedOrNull | Primitive::Absent;
static constexpr uint16_t testedMarkerKinds[] = { Primitive::Undefined, Primitive::Null, Primitive::Absent };

static_assert(isNaNConstExpr(std::bit_cast<double>(undefinedDouble)) && isNaNConstExpr(std::bit_cast<double>(nullDouble)) && isNaNConstExpr(std::bit_cast<double>(absentDouble)));
static_assert(cell32Markers.absent < minCell32 && cell32Markers.undefined < minCell32 && cell32Markers.null < minCell32);
static_assert(Primitive::Boolean == (Primitive::True | Primitive::False) && Primitive::UndefinedOrNull == (Primitive::Undefined | Primitive::Null));

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

void TypeTestEmitter::forEachNonCellCase(Boxed value, const TypeTestPlan& plan, const auto& visit)
{
    auto visitPair = [&](uint16_t first, JSValue firstValue, uint16_t second, JSValue secondValue, const auto& isEither) {
        uint16_t passing = plan.passingPrimitives & (first | second);
        if (passing == (first | second))
            visit(isEither);
        else if (passing) {
            visit([&] {
                return equal(value, boxedConstant(passing == first ? firstValue : secondValue));
            });
        }
    };

    RELEASE_ASSERT(verdictOfNumbers(plan) != TypeTestVerdict::NeedsTest);
    if (plan.passingPrimitives & Primitive::Number) {
        visit([&] {
            return isNumber(value);
        });
    }
    visitPair(Primitive::True, jsBoolean(true), Primitive::False, jsBoolean(false), [&] {
        return isBoolean(value);
    });
    visitPair(Primitive::Undefined, jsUndefined(), Primitive::Null, jsNull(), [&] {
        return plan.occurringPrimitives() & ~(cellPrimitives | Primitive::UndefinedOrNull) ? isUndefinedOrNull(value, plan.occurringPrimitives()) : isNotCell(value);
    });
}

TypeTestPlansKeptAcrossUnits& TypeTestEmitter::whatIsKeptAcrossUnits()
{
    if (!m_plansKeptAcrossUnits) {
        m_plansKeptAcrossUnits = &generator().procedure().arena().objectKeptAcrossResets<TypeTestPlansKeptAcrossUnits>();
        if (m_plansKeptAcrossUnits->plan != &m_plan || m_plansKeptAcrossUnits->table != &types().table()) {
            for (TypeTestPlansKeptAcrossUnits::Plans& plans : m_plansKeptAcrossUnits->plans)
                plans.clear();
            m_plansKeptAcrossUnits->kindsInSets.clear();
            m_plansKeptAcrossUnits->plan = &m_plan;
            m_plansKeptAcrossUnits->table = &types().table();
        }
    }
    return *m_plansKeptAcrossUnits;
}

TypeTestPlansKeptAcrossUnits::Plans& TypeTestEmitter::plansKeptAcrossUnits(Representation representation)
{
    return whatIsKeptAcrossUnits().plans[static_cast<size_t>(representation)];
}

const TypeTestPlan& TypeTestEmitter::planFor(TypeID from, Representation representation, TypeID to)
{
    RELEASE_ASSERT(from && to);
    uint64_t key = static_cast<uint64_t>(from.bits()) << 32 | to.bits();
    auto compute = [&] {
        return std::unique_ptr<TypeTestPlan>(new TypeTestPlan(computeTypeTestPlan(types(), TypeTestContext { m_plan, hasOneInstance(), &whatIsKeptAcrossUnits().kindsInSets }, from, representation, to)));
    };
    if (holdsOnlyPrimitives(to) && !liesInThread(from)) {
        TypeTestPlansKeptAcrossUnits::Plans& kept = plansKeptAcrossUnits(representation);
        if (auto found = kept.find(key); found != kept.end())
            return *found->value;
        if (kept.size() < TypeTestPlansKeptAcrossUnits::mostPlansOfRepresentation)
            return *kept.add(key, compute()).iterator->value;
    }
    return *m_plans[static_cast<size_t>(representation)].ensure(key, compute).iterator->value;
}

void TypeTestEmitter::emitPerFunctionBranch(Operand operand, const TypeTestPlan& plan, BranchTargets targets)
{
    SetForScope isEmittingPerFunctionBranch(m_isEmittingPerFunctionBranch, true);
    emitBranch(operand, plan, targets);
}

void TypeTestEmitter::emitBranch(Operand operand, const TypeTestPlan& plan, BranchTargets targets)
{
    Machine::DeclaredAccessesSuspension suspension(m_out);
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
    if (std::optional<BuiltinID> builtin = onlyBuiltinThatPasses(plan)) {
        emitter<ConstantEmitter>().branchOnIsBuiltinCell(operand.value, *builtin, targets);
        return;
    }
    if (std::optional<Condition> condition = tryGetSingleCondition(operand, plan)) {
        branch(*condition, targets);
        return;
    }
    test(operand, plan, targets.notTaken);
    if (m_out.tryToLeadBranchPastEmptyBlock(targets.taken))
        return;
    jump(targets.taken);
}

std::optional<BuiltinID> TypeTestEmitter::onlyBuiltinThatPasses(const TypeTestPlan& plan) const
{
    if (plan.representation != Representation::JSValue && plan.representation != Representation::Cell)
        return std::nullopt;
    if (plan.passingPrimitives || plan.occurringPrimitives() & Primitive::Absent || plan.objects != ObjectsVerdict::NeedTest || plan.cellTests.size() != 1)
        return std::nullopt;
    const CellTest& test = plan.cellTests[0];
    if (test.kind != CellTestKind::IsBuiltinCell || !isTestableBuiltin(m_builtins, test.builtin))
        return std::nullopt;
    return test.builtin;
}

Condition TypeTestEmitter::emitBoolean(Operand operand, const TypeTestPlan& plan)
{
    Machine::DeclaredAccessesSuspension suspension(m_out);
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

void TypeTestEmitter::leaveUnless(Condition condition, BlockEdge failure)
{
    if (condition.value()->hasInt32()) {
        if (!condition.value()->asInt32())
            leave(failure);
        return;
    }
    LBasicBlock continuation = newBlock();
    m_out.branch(condition.value(), normalEdge(continuation), failure);
    m_out.appendToContinuation(continuation);
}

void TypeTestEmitter::leave(BlockEdge failure)
{
    jump(failure);
    m_out.appendTo(newBlock());
}

void TypeTestEmitter::jump(BlockEdge edge)
{
    m_out.jump(edge);
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
        return plan.passingPrimitives & Primitive::True ? operand.as<Condition>() : logicalNot(operand.as<Condition>());
    case Representation::Int32:
        return integerPasses(signExtended(operand.as<Word32>()), plan, Range::int32());
    case Representation::Int64:
        return integerPasses(operand.as<Integer>(), plan);
    case Representation::Double:
        return doublePasses(operand.as<Number>(), plan);
    case Representation::OptionalInt64:
        return decideByMarker(verdictOfNumbers(plan));
    case Representation::OptionalDouble:
        if (verdictOfNumbers(plan) == TypeTestVerdict::NeedsTest && !(plan.passingPrimitives & (testedMarkerPrimitives | Primitive::Double)))
            return isIntegerIn(emitter<ConversionEmitter>().withoutMarkers(operand).as<Number>(), plan.integers.value_or(Range::full()));
        return decideByMarker(verdictOfNumbers(plan));
    case Representation::Cell:
        return tryGetSingleCondition(operand.as<CellPointer>(), plan);
    case Representation::Cell32:
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
    if (plan.failingPrimitives == Primitive::Absent && plan.everyCellPasses() && plan.cellTests.isEmpty() && !plan.integers)
        return isNotEmpty(value);
    if (plan.someCellPasses())
        return std::nullopt;
    if (plan.passingPrimitives == Primitive::Absent)
        return isEmpty(value);
    if (plan.passingPrimitives == (Primitive::Absent | Primitive::Null))
        return belowOrEqual(value, boxedConstant(jsNull()));
    constexpr int64_t bitsOfMarkers = JSValue::ValueUndefined | JSValue::ValueNull;
    static_assert((JSValue::ValueFalse & ~bitsOfMarkers) && (JSValue::ValueTrue & ~bitsOfMarkers) && bitsOfMarkers < 16);
    if (!(plan.passingPrimitives & ~testedMarkerPrimitives) && !(plan.failingPrimitives & testedMarkerPrimitives))
        return Condition(m_out.testIsZero64(value.value(), m_out.constInt64(~bitsOfMarkers)));
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
    Condition onlyCasePasses;
    forEachNonCellCase(value, plan, [&](const auto& emitTest) {
        onlyCasePasses = emitTest();
    });
    return onlyCasePasses ? onlyCasePasses : boolean(false);
}

void TypeTestEmitter::test(Operand operand, const TypeTestPlan& plan, BlockEdge failure)
{
    switch (operand.representation) {
    case Representation::Cell:
        testCell(operand.as<CellPointer>(), plan, failure);
        return;
    case Representation::JSValue:
        testBoxed(operand.as<Boxed>(), plan, failure);
        return;
    case Representation::Cell32:
        testWithMarkers(operand, plan, failure, [&] {
            testCell(decode(operand.as<NarrowReference>()), plan, failure);
        });
        return;
    case Representation::OptionalDouble:
        testWithMarkers(operand, plan, failure, [&] {
            leaveUnless(doublePasses(emitter<ConversionEmitter>().withoutMarkers(operand).as<Number>(), plan), failure);
        });
        return;
    case Representation::OptionalInt64:
        testWithMarkers(operand, plan, failure, [&] {
            leaveUnless(integerPasses(emitter<ConversionEmitter>().withoutMarkers(operand).as<Integer>(), plan), failure);
        });
        return;
    case Representation::Boolean:
    case Representation::Int32:
    case Representation::Int64:
    case Representation::Double:
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::RawWord:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void TypeTestEmitter::testBoxed(Boxed value, const TypeTestPlan& plan, BlockEdge failure)
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
            if (!present.cellsOccur() || !present.someCellPasses()) {
                testNonCell(value, plan, failure);
                return;
            }
            if (generator().wordWhoseHoleIsLeftToTestAtWork() == value.value() && !(present.passingPrimitives & ~cellPrimitives)) {
                leaveUnless(isCellAndNotEmpty(value), failure);
                testCell(unboxCell(value), present, failure);
                return;
            }
            leaveUnless(isNotEmpty(value), failure);
            testBoxed(value, present, failure);
            return;
        }
        Condition isAbsent = isEmpty(value);
        LBasicBlock isPresent = newBlock();
        LBasicBlock continuation = newBlock();
        ScopeFork fork = m_out.forkScope();
        m_out.branch(isAbsent.value(), unlikelyEdge(continuation), normalEdge(isPresent));
        m_out.appendToArm(isPresent, fork);
        testBoxed(value, present, failure);
        m_out.jump(continuation);
        m_out.appendToJoin(continuation, fork);
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
    LBasicBlock continuation = newBlock();
    if (m_testsNumbersFirst && !everyOtherPasses && (plan.passingPrimitives & Primitive::Number) && verdictOfNumbers(plan) == TypeTestVerdict::AlwaysPasses) {
        LBasicBlock isNoNumber = newBlock();
        ScopeFork fork = m_out.forkScope();
        m_out.branch(isNumber(value).value(), normalEdge(continuation), unlikelyEdge(isNoNumber));
        m_out.appendToArm(isNoNumber, fork);
        SetForScope numberWasTested(m_testsNumbersFirst, false);
        testBoxed(value, plan, failure);
        m_out.jump(continuation);
        m_out.appendToJoin(continuation, fork);
        return;
    }
    Condition holdsCell = isCell(value);
    LBasicBlock cellCase = newBlock();
    LBasicBlock otherCase = everyOtherPasses ? continuation : newBlock();
    ScopeFork fork = m_out.forkScope();
    m_out.branch(holdsCell.value(), normalEdge(cellCase), everyOtherPasses ? rareEdge(otherCase) : unlikelyEdge(otherCase));
    m_out.appendToArm(cellCase, fork);
    testCell(cell, plan, failure);
    m_out.jump(continuation);
    if (!everyOtherPasses) {
        m_out.appendToArm(otherCase, fork);
        testNonCell(value, plan, failure);
        m_out.jump(continuation);
    }
    m_out.appendToJoin(continuation, fork);
}

void TypeTestEmitter::testNonCell(Boxed value, const TypeTestPlan& plan, BlockEdge failure)
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
        testBoxedNumber(value, BoxedValue::MayBeNoNumber, plan, failure);
        return;
    }

    Condition holdsNumber = isNumber(value);
    LBasicBlock numberCase = newBlock();
    LBasicBlock otherCase = newBlock();
    LBasicBlock continuation = newBlock();
    ScopeFork fork = m_out.forkScope();
    m_out.branch(holdsNumber.value(), normalEdge(numberCase), unlikelyEdge(otherCase));
    m_out.appendToArm(numberCase, fork);
    testBoxedNumber(value, BoxedValue::IsNumber, plan, failure);
    m_out.jump(continuation);
    m_out.appendToArm(otherCase, fork);
    testNonCell(value, withoutNumbers, failure);
    m_out.jump(continuation);
    m_out.appendToJoin(continuation, fork);
}

void TypeTestEmitter::testBoxedNumber(Boxed value, BoxedValue known, const TypeTestPlan& plan, BlockEdge failure)
{
    ifElse(isInt32(value), [&] {
        bool everyInt32Passes = plan.integers && Range::int32().isSubsetOf(*plan.integers);
        if (!everyInt32Passes)
            leaveUnless(integerPasses(signExtended(unboxInt32(value)), plan, Range::int32()), failure);
    }, [&] {
        if (known == BoxedValue::MayBeNoNumber)
            leaveUnless(isNumber(value), failure);
        leaveUnless(doublePasses(unboxDouble(value), plan), failure);
    });
}

void TypeTestEmitter::testWithMarkers(Operand operand, const TypeTestPlan& plan, BlockEdge failure, NOESCAPE const Invocable<void()> auto& testPayload)
{
    if (std::optional<Condition> condition = tryGetSingleCondition(operand, plan)) {
        leaveUnless(*condition, failure);
        return;
    }

    bool someMarkerPasses = plan.passingPrimitives & testedMarkerPrimitives;
    bool someMarkerFails = plan.failingPrimitives & testedMarkerPrimitives;
    if (!someMarkerPasses) {
        if (someMarkerFails)
            leaveUnless(logicalNot(isMarker(operand)), failure);
        testPayload();
        return;
    }

    Condition holdsMarker = isMarker(operand);
    LBasicBlock payloadCase = newBlock();
    LBasicBlock continuation = newBlock();
    LBasicBlock markerCase = someMarkerFails ? newBlock() : continuation;
    ScopeFork fork = m_out.forkScope();
    m_out.branch(holdsMarker.value(), unlikelyEdge(markerCase), normalEdge(payloadCase));
    m_out.appendToArm(payloadCase, fork);
    testPayload();
    m_out.jump(continuation);
    if (someMarkerFails) {
        m_out.appendToArm(markerCase, fork);
        leaveUnless(passesByMarker(operand, plan, true), failure);
        m_out.jump(continuation);
    }
    m_out.appendToJoin(continuation, fork);
}

Condition TypeTestEmitter::isMarker(Operand operand)
{
    switch (operand.representation) {
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
        return emitter<ConversionEmitter>().isMarker(operand);
    case Representation::Cell32:
        return logicalNot(isCellReference(operand.as<NarrowReference>()));
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
    uint16_t agreeing = (payloadPasses ? plan.passingPrimitives : plan.failingPrimitives) & testedMarkerPrimitives;
    uint16_t disagreeing = (payloadPasses ? plan.failingPrimitives : plan.passingPrimitives) & testedMarkerPrimitives;
    if (!disagreeing)
        return boolean(payloadPasses);

    Condition disagrees;
    if (!agreeing)
        disagrees = isMarker(operand);
    else if (operand.representation == Representation::OptionalDouble && agreeing == Primitive::Undefined && disagreeing == (testedMarkerPrimitives & ~Primitive::Undefined))
        disagrees = emitter<ConversionEmitter>().isNullOrAbsent(operand.as<OptionalNumber>());
    else {
        for (uint16_t primitive : testedMarkerKinds) {
            if (disagreeing & primitive)
                disagrees = logicalOr(disagrees, isMarker(operand, primitive));
        }
    }
    return payloadPasses ? logicalNot(disagrees) : disagrees;
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
    Integer truncated = emitter<ConversionEmitter>().truncateUnchecked(number);
    Condition isInteger = isExactIntegerIn(number, truncated, Range::full());
    return select(isInteger, integerPasses(truncated, plan), otherNumberPasses);
}

Condition TypeTestEmitter::integerPasses(Integer value, const TypeTestPlan& plan, Range heldByForm)
{
    if (!(plan.passingPrimitives & Primitive::Integer))
        return boolean(false);
    if (!plan.integers)
        return boolean(true);

    Range range = *plan.integers;
    Range known = types().at(plan.from).integers.intersectWith(heldByForm);
    if (known.minimum >= range.minimum)
        return lessThanOrEqual(value, integer(range.maximum));
    if (known.maximum <= range.maximum)
        return greaterThanOrEqual(value, integer(range.minimum));
    return isInRange(value, range);
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
    ASSERT(range.isSubsetOf(Range::full()));
    ConversionEmitter& conversions = emitter<ConversionEmitter>();
    Integer bitsOfTruncated = conversions.bitsOf(toNumber(truncated));
    Condition isExact = equal(bitsOfTruncated, conversions.bitsOf(number));
    return logicalAnd(isExact, isInRange(truncated, range));
}

Condition TypeTestEmitter::isIntegerIn(Number number, Range range)
{
    ConversionEmitter& conversions = emitter<ConversionEmitter>();
    if (std::optional<Integer> int32 = range == Range::int32() ? conversions.int32Behind(number) : std::nullopt)
        return lessThanOrEqual(*int32, integer(std::numeric_limits<int32_t>::max()));
#if CPU(ARM64)
    if (MacroAssembler::supportsDoubleToInt32ConversionUsingJavaScriptSemantics(build().targetFeatures()) && range.isSubsetOf(Range::int32())) {
        Machine::PatchOperation* patchpoint = m_out.patchpoint(Machine::Int32);
        patchpoint->appendSomeRegister(number.value());
        patchpoint->effects = Machine::Effects::none();
        patchpoint->setRunsWithoutFrame(true);
        patchpoint->setGenerator([](CCallHelpers& jit, const Machine::PatchParameters& params) {
            jit.convertDoubleToInt32UsingJavaScriptSemantics(params[1].fpr(), params[0].gpr());
            jit.m_assembler.cset<32>(params[0].gpr(), ARM64Assembler::ConditionEQ);
        });
        Condition isExactInt32(patchpoint);
        if (Range::int32().isSubsetOf(range))
            return isExactInt32;
        return logicalAnd(isExactInt32, isInRange(conversions.truncateUnchecked(number), range));
    }
#endif
    if (range == Range::int32()) {
        Word32 word = conversions.truncateToWord(number);
        return equal(conversions.bitsOf(toNumber(word)), conversions.bitsOf(number));
    }
    return isExactIntegerIn(number, conversions.truncateUnchecked(number), range);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
