/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTInt32Proof.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinMembers.h"
#include "AOTBuiltinTypes.h"
#include "AOTFunctionAnalyses.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"
#include <algorithm>
#include <cmath>
#include <wtf/CheckedArithmetic.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint16_t nullishOrAbsentPrimitives = Primitive::UndefinedOrNull | Primitive::Absent;
static constexpr OpcodeSet castsThatNameType = Value::opcodesWithCastType | Value::opcodesWithExpectedType | Value::opcodesWithProvenType;

NumberBounds boundsOfType(const Type& type)
{
    if (!type.hasOnlyPrimitives(Primitive::Number | nullishOrAbsentPrimitives))
        return NumberBounds::unknown(type.isAnything() ? Int32ProofFailure::Untyped : Int32ProofFailure::AnyNumber);
    if (type.primitives & Primitive::Double)
        return NumberBounds::unknown(Int32ProofFailure::AnyNumber);

    NumberBounds bounds;
    bounds.kind = type.primitives & Primitive::Integer ? BoundsKind::Integers : BoundsKind::None;
    bounds.range = type.integers;
    bounds.mayBeNullish = type.primitives & nullishOrAbsentPrimitives;
    return bounds;
}

static NumberBounds boundsOfConstant(double number)
{
    if (std::isnan(number))
        return NumberBounds::unknown(Int32ProofFailure::NotANumber);
    if (!number && std::signbit(number))
        return NumberBounds::unknown(Int32ProofFailure::NegativeZero);
    if (std::isfinite(number) && number != std::trunc(number))
        return NumberBounds::unknown(Int32ProofFailure::Fraction);
    if (isTrackedInteger(number))
        return NumberBounds::integers(Range::one(static_cast<int64_t>(number)));
    return NumberBounds::unknown(Int32ProofFailure::OutOfRange);
}

NumberBounds narrowToType(const NumberBounds& bounds, const Type& type)
{
    NumberBounds admitted = boundsOfType(type);
    if (bounds.kind == BoundsKind::Unknown) {
        if (admitted.kind != BoundsKind::Unknown)
            return admitted;
        bool saysMore = bounds.failure == Int32ProofFailure::Untyped && admitted.failure != Int32ProofFailure::Untyped;
        return saysMore ? admitted : bounds;
    }

    NumberBounds narrowed = bounds;
    narrowed.mayBeNullish = bounds.mayBeNullish && (admitted.kind == BoundsKind::Unknown || admitted.mayBeNullish);
    if (admitted.kind == BoundsKind::Unknown || !bounds.isBounded())
        return narrowed;
    narrowed.range = admitted.kind == BoundsKind::None ? Range::empty() : bounds.range.intersectWith(admitted.range);
    if (narrowed.range.isEmpty()) {
        narrowed.kind = BoundsKind::None;
        narrowed.range = Range::empty();
    }
    return narrowed;
}

bool isEnteredAt(TypeSiteKind kind)
{
    switch (kind) {
    case TypeSiteKind::Unknown:
    case TypeSiteKind::Parameter:
    case TypeSiteKind::This:
    case TypeSiteKind::Capture:
    case TypeSiteKind::Result:
    case TypeSiteKind::Field:
    case TypeSiteKind::StaticField:
    case TypeSiteKind::Variable:
    case TypeSiteKind::Assertion:
        return true;
    case TypeSiteKind::Element:
    case TypeSiteKind::MapKey:
    case TypeSiteKind::MapValue:
    case TypeSiteKind::SetElement:
    case TypeSiteKind::PromiseValue:
    case TypeSiteKind::CallResult:
    case TypeSiteKind::PropertyRead:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

TypeSiteKind siteKindOf(const Program& program, const DeclaredType& declared)
{
    return declared.site ? program.at(declared.site).kind : TypeSiteKind::Unknown;
}

bool isAssertionOfDependency(const Program& program, const DeclaredType& declared)
{
    return declared.has(PositionFlags::IsDeclaredByDependency) && siteKindOf(program, declared) == TypeSiteKind::Assertion;
}

static NumberBounds join(const NumberBounds& a, const NumberBounds& b)
{
    auto joinNumbers = [&]() -> NumberBounds {
        if (a.kind == BoundsKind::None)
            return b;
        if (b.kind == BoundsKind::None || a.kind == BoundsKind::Unknown)
            return a;
        if (b.kind == BoundsKind::Unknown)
            return b;
        bool areIntegers = a.kind == BoundsKind::Integers && b.kind == BoundsKind::Integers;
        Range range = a.range.unionWith(b.range);
        return areIntegers ? NumberBounds::integers(range) : NumberBounds::reals(range);
    };
    NumberBounds joined = joinNumbers();
    joined.mayBeNullish = a.mayBeNullish || b.mayBeNullish;
    return joined;
}

static std::optional<Int32ProofFailure> failureSpecificTo(Opcode opcode)
{
    switch (opcode) {
    case Opcode::Mul:
        return Int32ProofFailure::Product;
    case Opcode::Div:
        return Int32ProofFailure::Quotient;
    case Opcode::Mod:
        return Int32ProofFailure::Remainder;
    case Opcode::Pow:
        return Int32ProofFailure::Power;
    default:
        return std::nullopt;
    }
}

static Int32ProofFailure failureOfOperator(Opcode opcode, std::span<const NumberBounds> operands)
{
    if (auto failure = failureSpecificTo(opcode))
        return *failure;
    for (const NumberBounds& operand : operands) {
        if (operand.kind == BoundsKind::Unknown)
            return operand.failure;
    }
    return Int32ProofFailure::AnyNumber;
}

static NumberBounds exactReals(CheckedInt64 minimum, CheckedInt64 maximum, BoundsKind kind)
{
    if (minimum.hasOverflowed() || maximum.hasOverflowed())
        return NumberBounds::unknown(Int32ProofFailure::AnyNumber);
    Range range { minimum.value(), maximum.value() };
    if (!range.isSubsetOf(Range::exact()))
        return NumberBounds::unknown(Int32ProofFailure::AnyNumber);
    return { .kind = kind, .range = range };
}

static int64_t floorOfQuotient(int64_t dividend, int64_t divisor)
{
    int64_t quotient = dividend / divisor;
    bool isRoundedUp = dividend % divisor && (dividend < 0) != (divisor < 0);
    return isRoundedUp ? quotient - 1 : quotient;
}

static NumberBounds boundsOfQuotient(const NumberBounds& dividend, const NumberBounds& divisor)
{
    Range dividends = dividend.range;
    Range divisors = divisor.range;
    bool areIntegers = dividend.kind == BoundsKind::Integers && divisor.kind == BoundsKind::Integers;
    bool mayBeNegativeZero = dividends.contains(0) && divisors.maximum < 0;
    if (!areIntegers || divisors.contains(0) || mayBeNegativeZero || !dividends.isSubsetOf(Range::exact()) || !divisors.isSubsetOf(Range::exact()))
        return NumberBounds::unknown(Int32ProofFailure::Quotient);

    Range quotients = Range::empty();
    for (int64_t numerator : { dividends.minimum, dividends.maximum }) {
        for (int64_t denominator : { divisors.minimum, divisors.maximum })
            quotients = quotients.unionWith({ floorOfQuotient(numerator, denominator), -floorOfQuotient(-numerator, denominator) });
    }
    return NumberBounds::reals(quotients);
}

static std::optional<NumberBounds> boundsOfRealOperator(Opcode opcode, std::span<const NumberBounds> operands)
{
    Range first = operands[0].range;
    switch (opcode) {
    case Opcode::Div:
        return boundsOfQuotient(operands[0], operands[1]);
    case Opcode::Add:
        return exactReals(CheckedInt64(first.minimum) + operands[1].range.minimum, CheckedInt64(first.maximum) + operands[1].range.maximum, BoundsKind::Reals);
    case Opcode::Sub:
        return exactReals(CheckedInt64(first.minimum) - operands[1].range.maximum, CheckedInt64(first.maximum) - operands[1].range.minimum, BoundsKind::Reals);
    case Opcode::Increment:
        return exactReals(CheckedInt64(first.minimum) + 1, CheckedInt64(first.maximum) + 1, BoundsKind::Reals);
    case Opcode::Decrement:
        return exactReals(CheckedInt64(first.minimum) - 1, CheckedInt64(first.maximum) - 1, BoundsKind::Reals);
    case Opcode::Negate:
        if (first.contains(0))
            return NumberBounds::unknown(Int32ProofFailure::NegatedZero);
        return NumberBounds::reals({ -first.maximum, -first.minimum });
    case Opcode::ToNumber:
    case Opcode::ToNumeric:
        return operands[0];
    default:
        return std::nullopt;
    }
}

static Opcode comparisonFromRightOperand(Opcode comparison)
{
    switch (comparison) {
    case Opcode::Less:
        return Opcode::Greater;
    case Opcode::LessEq:
        return Opcode::GreaterEq;
    case Opcode::Greater:
        return Opcode::Less;
    case Opcode::GreaterEq:
        return Opcode::LessEq;
    default:
        return comparison;
    }
}

static Range withoutEndpoint(Range range, Range removed)
{
    if (!removed.isSingleValue())
        return range;
    if (range.minimum == removed.minimum)
        ++range.minimum;
    if (range.maximum == removed.maximum)
        --range.maximum;
    return range;
}

static Range narrowByComparison(Range range, Opcode comparison, bool outcome, Range other)
{
    switch (comparison) {
    case Opcode::Less:
        return outcome ? range.intersectWith({ range.minimum, other.maximum - 1 }) : range.intersectWith({ other.minimum, range.maximum });
    case Opcode::LessEq:
        return outcome ? range.intersectWith({ range.minimum, other.maximum }) : range.intersectWith({ other.minimum + 1, range.maximum });
    case Opcode::Greater:
        return outcome ? range.intersectWith({ other.minimum + 1, range.maximum }) : range.intersectWith({ range.minimum, other.maximum });
    case Opcode::GreaterEq:
        return outcome ? range.intersectWith({ other.minimum, range.maximum }) : range.intersectWith({ range.minimum, other.maximum - 1 });
    case Opcode::StrictEq:
    case Opcode::Eq:
        return outcome ? range.intersectWith(other) : withoutEndpoint(range, other);
    default:
        return range;
    }
}

Int32Prover::Int32Prover(const Int32ProofInputs& inputs)
    : m_inputs(inputs)
    , m_typing { inputs.program, inputs.function, inputs.types, TypeLevel::Expected }
    , m_known(inputs.function.values.size())
{
    m_openDepths.fill(0, inputs.function.values.size());
}

NumberBounds Int32Prover::boundsOf(ValueID value)
{
    m_subject = value;
    m_steps = 0;
    m_hasGivenUp = false;
    m_reliesOnUnprovenFlow = false;
    return find(value);
}

bool Int32Prover::exceedsWorkLimit()
{
    if (m_stackCheck.isSafeToRecurse() && ++m_steps <= m_inputs.build.limit(LimitID::MaxInt32ProofWork))
        return false;
    m_inputs.build.gaveUp(LimitID::MaxInt32ProofWork, m_inputs.analyses.subject(m_subject));
    return true;
}

NumberBounds Int32Prover::find(ValueID id)
{
    if (const std::optional<NumberBounds>& known = m_known[id.index()]) {
        m_reliesOnUnprovenFlow = m_reliesOnUnprovenFlow || m_knownRelyingOnUnprovenFlow.get(id.index());
        return *known;
    }
    if (uint32_t openDepth = m_openDepths[id.index()]) {
        m_outermostOpenDepthReached = std::min(m_outermostOpenDepthReached, openDepth);
        return NumberBounds::unknown(Int32ProofFailure::AnyNumber);
    }
    m_hasGivenUp = m_hasGivenUp || exceedsWorkLimit();
    if (m_hasGivenUp)
        return NumberBounds::unknown(Int32ProofFailure::WorkLimit);

    uint32_t depth = ++m_depth;
    m_openDepths[id.index()] = depth;
    uint32_t reachedBefore = std::exchange(m_outermostOpenDepthReached, noDepth);
    bool reliedBefore = std::exchange(m_reliesOnUnprovenFlow, false);
    NumberBounds bounds = compute(id);
    m_openDepths[id.index()] = 0;
    --m_depth;

    bool dependsOnOpenValue = m_outermostOpenDepthReached < depth;
    if (!dependsOnOpenValue) {
        m_outermostOpenDepthReached = noDepth;
        if (!m_hasGivenUp) {
            m_known[id.index()] = bounds;
            if (m_reliesOnUnprovenFlow)
                m_knownRelyingOnUnprovenFlow.set(id.index());
        }
    }
    m_outermostOpenDepthReached = std::min(m_outermostOpenDepthReached, reachedBefore);
    m_reliesOnUnprovenFlow = m_reliesOnUnprovenFlow || reliedBefore;
    return bounds;
}

NumberBounds Int32Prover::compute(ValueID id)
{
    const Value& value = m_inputs.function.at(id);
    switch (value.opcode) {
    case Opcode::Parameter:
        return computeParameter(id);
    case Opcode::Double:
        return boundsOfConstant(value.number());
    case Opcode::Convert:
        return computePromise(value);
    case Opcode::PropGet:
        return computeNamedRead(id, value);
    case Opcode::Call:
        return computeCall(id, value);
    default:
        break;
    }
    if (isCast(value.opcode))
        return computeCast(id, value);
    if (Value::opcodesWithForm.contains(value.opcode))
        return computeOperator(id, value);
    return boundsOfType(typeOf(id));
}

NumberBounds Int32Prover::computeParameter(ValueID id)
{
    ValueLocation location = m_inputs.analyses.useLists().locationOf(id);
    if (location.placement != ValuePlacement::Parameter || location.block == entryBlock)
        return boundsOfType(typeOf(id));

    NumberBounds joined { .kind = BoundsKind::None };
    for (Edge edge : m_inputs.analyses.cfg().incomingEdges(location.block)) {
        joined = join(joined, computeIncoming(id, edge, location.index));
        if (joined.kind == BoundsKind::Unknown)
            break;
    }
    return joined;
}

NumberBounds Int32Prover::computeIncoming(ValueID parameter, Edge edge, uint32_t parameterIndex)
{
    const Function& function = m_inputs.function;
    const Target& target = targetOf(function, edge);
    if (parameterIndex < target.arguments.size())
        return find(target.arguments[parameterIndex]);
    if (edge.isHandlerEdge())
        return boundsOfType(typeOf(parameter));

    EdgeResultPosition position { edge.from, edge.targetIndex, parameterIndex - static_cast<uint32_t>(target.arguments.size()) };
    std::optional<EdgeResult> result = tryGetEdgeResult(function, position);
    if (!result)
        return boundsOfType(typeOf(parameter));
    switch (result->kind) {
    case EdgeResultKind::NarrowedOperand:
        return computeNarrowedOperand(parameter, *result);
    case EdgeResultKind::Guard:
    case EdgeResultKind::DeclaredResult:
        return boundsOfType(typeOf(parameter));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

NumberBounds Int32Prover::computeNarrowedOperand(ValueID parameter, const EdgeResult& result)
{
    NumberBounds subject = find(result.subject);
    const Type& passed = typeOf(parameter);
    subject.mayBeNullish = subject.mayBeNullish && passed.primitives & nullishOrAbsentPrimitives;
    if (subject.isBounded() && !(passed.primitives & Primitive::Number))
        return { .kind = BoundsKind::None, .mayBeNullish = subject.mayBeNullish };
    if (subject.kind != BoundsKind::Integers || !result.test)
        return subject;

    const Value& test = m_inputs.function.at(result.test);
    if (test.numberOfOperands() != 2 || result.operandIndex > 1)
        return subject;
    NumberBounds other = find(test.operand(result.operandIndex ? 0 : 1));
    if (!other.isNumber())
        return subject;

    Opcode comparison = result.operandIndex ? comparisonFromRightOperand(test.opcode) : test.opcode;
    Range narrowed = narrowByComparison(subject.range, comparison, result.side == EdgeSide::Taken, other.range);
    if (narrowed.isEmpty())
        return { .kind = BoundsKind::None, .mayBeNullish = subject.mayBeNullish };
    subject.range = narrowed;
    return subject;
}

NumberBounds Int32Prover::computePromise(const Value& request)
{
    DeclaredType declared = request.declared();
    TypeSiteKind site = siteKindOf(m_inputs.program, declared);
    if (!isEnteredAt(site) || isAssertionOfDependency(m_inputs.program, declared))
        return NumberBounds::integers(Range::int32());

    uint32_t reachedBefore = std::exchange(m_outermostOpenDepthReached, noDepth);
    NumberBounds entering = find(request.valueOperand());
    bool dependsOnOpenValue = m_outermostOpenDepthReached != noDepth;
    m_outermostOpenDepthReached = std::min(m_outermostOpenDepthReached, reachedBefore);
    if (site == TypeSiteKind::Assertion || entering.isInt32OrNullish())
        return entering;
    m_reliesOnUnprovenFlow = m_reliesOnUnprovenFlow || !dependsOnOpenValue;
    return NumberBounds::integers(Range::int32());
}

NumberBounds Int32Prover::computeCast(ValueID id, const Value& cast)
{
    NumberBounds operand = find(cast.operand(0));
    if (castsThatNameType.contains(cast.opcode))
        return narrowToType(operand, typeOf(id));
    operand.mayBeNullish = operand.mayBeNullish && typeOf(id).primitives & nullishOrAbsentPrimitives;
    return operand;
}

NumberBounds Int32Prover::computeIntegerOperator(Opcode opcode, Operands operands)
{
    Vector<Type, 2> types;
    for (const NumberBounds& operand : operands)
        types.append(Type::integer(operand.range));
    NumberBounds bounds = boundsOfType(computeOperatorResultType(m_typing, opcode, types.span()));
    if (bounds.kind == BoundsKind::Unknown)
        bounds.failure = opcode == Opcode::Negate ? Int32ProofFailure::NegatedZero : failureOfOperator(opcode, operands);
    return bounds;
}

NumberBounds Int32Prover::computeOperator(ValueID id, const Value& value)
{
    Vector<NumberBounds, 2> operands;
    value.forEachChild([&](ValueID operand) {
        operands.append(find(operand));
    });

    bool areNumbers = std::ranges::all_of(operands, &NumberBounds::isNumber);
    bool areIntegers = std::ranges::all_of(operands, &NumberBounds::isInteger);
    if (areNumbers && operands.size() == numberOfNamedOperandsOf(value.opcode)) {
        if (areIntegers && value.opcode != Opcode::Div)
            return computeIntegerOperator(value.opcode, operands.span());
        if (std::optional<NumberBounds> bounds = boundsOfRealOperator(value.opcode, operands.span()))
            return *bounds;
    }

    NumberBounds typed = boundsOfType(typeOf(id));
    if (typed.kind == BoundsKind::Unknown)
        typed.failure = failureOfOperator(value.opcode, operands.span());
    return typed;
}

BuiltinID Int32Prover::findBuiltinMember(const Value& read)
{
    const Program& program = m_inputs.program;
    const BuiltinTable& table = program.builtins();
    BuiltinMemberName name = builtinMemberNameOf(program, read.key());
    if (!name || name.isPrivateAlias)
        return { };

    BuiltinReceiver receiver = builtinReceiverOf(program, typeOf(read.objectOperand()).withoutNullish());
    BuiltinMemberLookup found;
    switch (receiver.kind) {
    case BuiltinReceiverKind::Unknown:
        return { };
    case BuiltinReceiverKind::BuiltinObject:
        found = table.findMember(receiver.object, name.name);
        break;
    case BuiltinReceiverKind::Instance:
    case BuiltinReceiverKind::Primitive:
        found = table.findForInstance(receiver.builtinClass, name.name);
        break;
    }
    if (found.outcome != BuiltinMemberLookup::Outcome::Found)
        return { };
    return table.canonicalID(found.member);
}

NumberBounds Int32Prover::computeNamedRead(ValueID id, const Value& read)
{
    NumberBounds typed = boundsOfType(typeOf(id));
    if (typed.kind != BoundsKind::Unknown)
        return typed;

    BuiltinID member = findBuiltinMember(read);
    if (!member)
        return typed;
    switch (m_inputs.program.builtins().at(member).kind) {
    case BuiltinKind::Accessor:
    case BuiltinKind::DataProperty:
        break;
    case BuiltinKind::Namespace:
    case BuiltinKind::Constructor:
    case BuiltinKind::Function:
    case BuiltinKind::Prototype:
    case BuiltinKind::Method:
    case BuiltinKind::Constant:
    case BuiltinKind::NativeBinding:
        return typed;
    }

    BuiltinOperandTypes operands;
    operands.receiver = m_inputs.types.intern(typeOf(read.objectOperand()).withoutNullish());
    NumberBounds declared = boundsOfType(resultTypeOfReachableOverloads(m_typing, member, BuiltinCallMode::Get, operands));
    return declared.kind == BoundsKind::Unknown ? typed : declared;
}

BuiltinID Int32Prover::findBuiltinCallee(const Value& call)
{
    const BuiltinTable& table = m_inputs.program.builtins();
    if (BuiltinID callee = typeOf(call.calleeOperand()).withoutNullish().singleBuiltin())
        return table.canonicalID(callee);

    const Value& definition = m_inputs.function.at(sameObjectThroughCasts(m_inputs.function, call.calleeOperand()));
    if (definition.opcode != Opcode::PropGet)
        return { };
    return findBuiltinMember(definition);
}

TypeID Int32Prover::typeIDOfOperand(ValueID operand, const NumberBounds& bounds)
{
    if (bounds.isInteger())
        return m_inputs.types.intern(Type::integer(bounds.range));
    return m_typing.typeIDOf(operand);
}

static NumberBounds boundsOfRoundedNumber(const BuiltinOverload& overload, Range range)
{
    bool roundsDown = overload.code.implementation.inlineBuiltin == InlineBuiltin::MathFloor;
    if (!roundsDown && range.minimum < 0 && range.maximum >= 0)
        return NumberBounds::unknown(Int32ProofFailure::AnyNumber);
    return NumberBounds::integers(range);
}

std::optional<NumberBounds> Int32Prover::computeByRangeRule(BuiltinID builtin, Operands arguments)
{
    const BuiltinOverload* called = nullptr;
    for (const BuiltinOverload& overload : m_inputs.program.builtins().overloadsOf(builtin)) {
        if (overload.mode != BuiltinCallMode::Call)
            continue;
        if (called)
            return std::nullopt;
        called = &overload;
    }
    if (!called || called->result.rangeRule == BuiltinRangeRule::None || !called->result.rangeOperand || called->result.rangeOperand->isReceiver())
        return std::nullopt;

    BuiltinOperand operand = *called->result.rangeOperand;
    if (operand.argumentIndex() >= arguments.size())
        return std::nullopt;
    Operands ruled = arguments.subspan(operand.argumentIndex());
    if (operand.kind() == BuiltinOperand::Kind::Argument)
        ruled = ruled.first(1);
    if (!std::ranges::all_of(ruled, &NumberBounds::isNumber))
        return std::nullopt;

    Vector<Range, 4> ranges;
    for (const NumberBounds& bounds : ruled)
        ranges.append(bounds.range);
    Range range = computeResultRange(called->result.rangeRule, ranges.span());
    switch (called->result.rangeRule) {
    case BuiltinRangeRule::None:
        break;
    case BuiltinRangeRule::MinimumOfArguments:
    case BuiltinRangeRule::MaximumOfArguments:
    case BuiltinRangeRule::AbsoluteValue:
        return NumberBounds::reals(range);
    case BuiltinRangeRule::Sign:
        return NumberBounds::integers(range);
    case BuiltinRangeRule::IntegerPart:
        return boundsOfRoundedNumber(*called, range);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

NumberBounds Int32Prover::computeCall(ValueID id, const Value& call)
{
    NumberBounds typed = boundsOfType(typeOf(id));
    BuiltinID callee = findBuiltinCallee(call);
    if (typed.kind != BoundsKind::Unknown || !callee)
        return typed;

    std::span<const ValueID> passed = call.variadicOperands();
    Vector<NumberBounds, 4> arguments;
    for (ValueID argument : passed)
        arguments.append(find(argument));

    bool hasFractions = std::ranges::any_of(arguments, [](const NumberBounds& bounds) {
        return bounds.kind == BoundsKind::Reals;
    });
    if (hasFractions) {
        if (std::optional<NumberBounds> bounds = computeByRangeRule(callee, arguments.span()))
            return *bounds;
    }

    BuiltinOperandTypes operands;
    if (ValueID receiver = call.thisValueOperand())
        operands.receiver = m_typing.typeIDOf(receiver);
    for (size_t index = 0; index < passed.size(); ++index)
        operands.arguments.append(typeIDOfOperand(passed[index], arguments[index]));
    NumberBounds declared = boundsOfType(resultTypeOfReachableOverloads(m_typing, callee, BuiltinCallMode::Call, operands));
    if (declared.kind == BoundsKind::Unknown && m_hasGivenUp)
        declared.failure = Int32ProofFailure::WorkLimit;
    return declared;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
