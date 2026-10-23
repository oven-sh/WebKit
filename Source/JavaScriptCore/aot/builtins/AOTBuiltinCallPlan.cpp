/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBuiltinCallPlan.h"

#if ENABLE(AOT)

#include "AOTEffectsDeclared.h"
#include "AOTEffectsDescription.h"
#include "AOTEffectsOfOperands.h"
#include "AOTEffectsOfProtocols.h"
#include "AOTEffectsOfRules.h"
#include "AOTIRBuilder.h"
#include "AOTVector.h"
#include <array>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint16_t primitivesForToNumber = Primitive::UndefinedOrNull | Primitive::Boolean | Primitive::Number | Primitive::AnyString;
static constexpr uint16_t primitivesForToString = primitivesForToNumber | Primitive::BigInt;
static constexpr uint16_t primitivesForToPropertyKey = primitivesForToString | Primitive::AnySymbol;

std::span<const Opcode> conversionOpcodesOf(BuiltinConversion conversion)
{
    static constexpr std::array toBoolean { Opcode::ToBoolean };
    static constexpr std::array toNumber { Opcode::ToNumber };
    static constexpr std::array toNumeric { Opcode::ToNumeric };
    static constexpr std::array toString { Opcode::ToString };
    static constexpr std::array toPropertyKey { Opcode::ToPropertyKey };
    static constexpr std::array toObject { Opcode::ToObject };
    static constexpr std::array toIntegerOrInfinity { Opcode::ToNumber, Opcode::ToIntegerOrInfinity };
    static constexpr std::array toInt32 { Opcode::ToNumber, Opcode::ToInt32 };
    static constexpr std::array toUint32 { Opcode::ToNumber, Opcode::ToUint32 };

    switch (conversion) {
    case BuiltinConversion::ToBoolean:
        return toBoolean;
    case BuiltinConversion::ToNumber:
        return toNumber;
    case BuiltinConversion::ToNumeric:
        return toNumeric;
    case BuiltinConversion::ToString:
        return toString;
    case BuiltinConversion::ToPropertyKey:
        return toPropertyKey;
    case BuiltinConversion::ToObject:
        return toObject;
    case BuiltinConversion::ToIntegerOrInfinity:
        return toIntegerOrInfinity;
    case BuiltinConversion::ToInt32:
        return toInt32;
    case BuiltinConversion::ToUint32:
        return toUint32;
    case BuiltinConversion::None:
    case BuiltinConversion::ToPrimitive:
    case BuiltinConversion::ToLength:
    case BuiltinConversion::ToIndex:
    case BuiltinConversion::ToBigInt:
    case BuiltinConversion::Dictionary:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Protocols protocolsRunBy(BuiltinConversion conversion)
{
    switch (conversion) {
    case BuiltinConversion::None:
    case BuiltinConversion::ToBoolean:
    case BuiltinConversion::ToObject:
        return { };
    case BuiltinConversion::ToNumber:
    case BuiltinConversion::ToNumeric:
    case BuiltinConversion::ToString:
    case BuiltinConversion::ToPropertyKey:
    case BuiltinConversion::ToPrimitive:
    case BuiltinConversion::ToIntegerOrInfinity:
    case BuiltinConversion::ToLength:
    case BuiltinConversion::ToIndex:
    case BuiltinConversion::ToInt32:
    case BuiltinConversion::ToUint32:
    case BuiltinConversion::ToBigInt:
        return { Protocol::ToPrimitive };
    case BuiltinConversion::Dictionary:
        return Protocols::all();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static uint16_t primitivesConvertedWithoutThrowing(BuiltinConversion conversion)
{
    switch (conversion) {
    case BuiltinConversion::ToNumber:
    case BuiltinConversion::ToIntegerOrInfinity:
    case BuiltinConversion::ToLength:
    case BuiltinConversion::ToInt32:
    case BuiltinConversion::ToUint32:
        return primitivesForToNumber;
    case BuiltinConversion::ToNumeric:
    case BuiltinConversion::ToString:
        return primitivesForToString;
    case BuiltinConversion::ToPropertyKey:
    case BuiltinConversion::ToPrimitive:
        return primitivesForToPropertyKey;
    case BuiltinConversion::ToIndex:
        return Primitive::UndefinedOrNull | Primitive::Boolean;
    case BuiltinConversion::ToBigInt:
        return Primitive::Boolean | Primitive::BigInt;
    case BuiltinConversion::Dictionary:
        return Primitive::UndefinedOrNull;
    case BuiltinConversion::None:
    case BuiltinConversion::ToBoolean:
    case BuiltinConversion::ToObject:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool runsUserCode(const Program& program, const Type& operand, Protocols protocols)
{
    EffectDescription description;
    describeOperandCode(program, operand, { .uses = { }, .protocols = protocols }, description);
    return description.callsUnknownCode || !description.functions.isEmpty();
}

Type typeConvertedWithoutUserCode(const TypingContext& context, const Type& operand, BuiltinConversion conversion)
{
    Protocols protocols = protocolsRunBy(conversion);
    if (protocols.isEmpty())
        return operand;

    uint16_t throwingPrimitives = static_cast<uint16_t>(Primitive::Defined & ~primitivesConvertedWithoutThrowing(conversion));
    Type converted = operand.withoutPrimitives(throwingPrimitives);
    return runsUserCode(context.program, operand, protocols) ? converted.withoutObjects() : converted;
}

// The built-in repeats an integer conversion on a number it is passed, and the second time changes nothing.
static bool isUnchangedByConversion(const Type& type, BuiltinConversion conversion)
{
    switch (conversion) {
    case BuiltinConversion::ToBoolean:
        return type.hasOnlyPrimitives(Primitive::Boolean);
    case BuiltinConversion::ToNumber:
    case BuiltinConversion::ToIntegerOrInfinity:
    case BuiltinConversion::ToInt32:
    case BuiltinConversion::ToUint32:
        return type.hasOnlyPrimitives(Primitive::Number);
    case BuiltinConversion::ToNumeric:
        return type.hasOnlyPrimitives(Primitive::Number | Primitive::BigInt);
    case BuiltinConversion::ToString:
        return type.hasOnlyPrimitives(Primitive::AnyString);
    case BuiltinConversion::ToPropertyKey:
        return type.hasOnlyPrimitives(Primitive::AnyString | Primitive::AnySymbol);
    case BuiltinConversion::ToObject:
        return !type.primitives && type.isJavaScriptValue();
    case BuiltinConversion::None:
    case BuiltinConversion::ToPrimitive:
    case BuiltinConversion::ToLength:
    case BuiltinConversion::ToIndex:
    case BuiltinConversion::ToBigInt:
    case BuiltinConversion::Dictionary:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The IR has no form for these two conversions. A number that is passed where an index or a length is expected is one nearly always.
static bool isExpectedToBeAccepted(const Type& type, BuiltinConversion conversion)
{
    switch (conversion) {
    case BuiltinConversion::ToLength:
    case BuiltinConversion::ToIndex:
        return type.hasOnlyPrimitives(Primitive::Number | Primitive::Undefined);
    case BuiltinConversion::None:
    case BuiltinConversion::ToBoolean:
    case BuiltinConversion::ToNumber:
    case BuiltinConversion::ToNumeric:
    case BuiltinConversion::ToString:
    case BuiltinConversion::ToPropertyKey:
    case BuiltinConversion::ToPrimitive:
    case BuiltinConversion::ToObject:
    case BuiltinConversion::ToIntegerOrInfinity:
    case BuiltinConversion::ToInt32:
    case BuiltinConversion::ToUint32:
    case BuiltinConversion::ToBigInt:
    case BuiltinConversion::Dictionary:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool precedes(BuiltinOperand a, BuiltinOperand b)
{
    if (a.isReceiver() || b.isReceiver())
        return a.isReceiver() && !b.isReceiver();
    return a.argumentIndex() < b.argumentIndex();
}

Type withoutClassProofs(const Type& type)
{
    return type.withoutClassProofs().withUntestableBuiltinsWidened();
}

static TypeID typeAfterTests(TypeArena& types, const BuiltinOperandTypes& proven, const BuiltinCallPlan& plan, BuiltinOperand position)
{
    TypeID type = proven.at(position);
    for (const OperandRequirement& requirement : plan.requirements) {
        if (requirement.operand == position && requirement.requiredType)
            return types.intern(types.at(type).castTo(types.at(requirement.requiredType), types));
    }
    return type;
}

namespace {

enum class OperandOutcome : uint8_t { Undecided, Planned, NoMoreOperands, GaveUp, NeedsReplanning };

struct PlannedOperand {
    BuiltinOperand position;
    const BuiltinParameter& parameter;
    TypeID type;
};

class CallPlanner {
public:
    explicit CallPlanner(const BuiltinCallQuery&);

    BuiltinCallPlan plan();

private:
    TypeArena& types() const { return m_context.types; }
    TypeFit fitIn(TypeID type, BuiltinTypeID tested) const { return fitOf(m_context, type, tested).fit; }

    void resetToMatch();
    bool planSteps();
    OperandOutcome planReceiver();
    OperandOutcome planArgument(unsigned index);
    OperandOutcome planOperand(BuiltinOperand);
    OperandOutcome tryPass(const PlannedOperand&);
    OperandOutcome excludeThrowingValues(PlannedOperand&);
    OperandOutcome planConversion(PlannedOperand&);
    OperandOutcome requireAcceptedType(PlannedOperand&);

    OperandOutcome require(PlannedOperand&, TypeID required);
    OperandOutcome requirePristine(BuiltinOperand, const PristineRequirement&);
    OperandOutcome giveUp();

    TypeID expectedWithin(const PlannedOperand&) const;
    bool followsOperandRefusedByValue(BuiltinOperand) const;
    OperandRequirement& ensureRequirement(BuiltinOperand);
    bool hasStepFor(BuiltinOperand) const;
    void addStep(BuiltinOperand, OperandStepKind, BuiltinConversion = BuiltinConversion::None);

    const BuiltinCallQuery& m_query;
    const TypingContext& m_context;
    const BuiltinTable& m_table;
    BuiltinOperandTypes m_proven;
    OverloadMatch m_match;
    const BuiltinOverload* m_overload { nullptr };
    BuiltinCallPlan m_plan;
    bool m_checksNullishReceiver { false };
    bool m_mayHoistUserCode { false };
    bool m_hoistsUserCode { false };
};

CallPlanner::CallPlanner(const BuiltinCallQuery& query)
    : m_query(query)
    , m_context(query.overloads.context)
    , m_table(m_context.program.builtins())
    , m_proven(query.overloads.proven)
{
    TypeID receiver = m_proven.receiver;
    if (query.nullishReceiver != NullishReceiver::ThrowsAtRead || !receiver || !types().at(receiver).mayBeUndefinedOrNull())
        return;
    m_checksNullishReceiver = true;
    m_proven.receiver = types().withoutPrimitives(receiver, Primitive::UndefinedOrNull);
}

BuiltinCallPlan CallPlanner::plan()
{
    const OverloadQuery& overloads = m_query.overloads;
    m_match = matchOverload({ m_context, overloads.builtin, overloads.mode, m_proven, overloads.expected });
    if (m_match.state == BuiltinMatchState::Unknown || !m_match.overload) {
        giveUp();
        return WTF::move(m_plan);
    }

    m_overload = &m_table.at(m_match.overload);
    bool matchNeedsPristineOperand = m_match.requirements.containsIf([](const OperandRequirement& requirement) {
        return !!requirement.pristine;
    });
    m_mayHoistUserCode = !m_overload->flags.contains(BuiltinOverloadFlag::ReadsStateBeforeConversions) && !matchNeedsPristineOperand;

    resetToMatch();
    if (!planSteps()) {
        m_mayHoistUserCode = false;
        resetToMatch();
        bool isPlanned = planSteps();
        RELEASE_ASSERT(isPlanned);
    }
    return WTF::move(m_plan);
}

void CallPlanner::resetToMatch()
{
    m_plan = BuiltinCallPlan { .state = m_match.state, .overload = m_match.overload, .requirements = m_match.requirements };
    m_hoistsUserCode = false;
}

bool CallPlanner::planSteps()
{
    OperandOutcome outcome = planReceiver();
    for (unsigned index = 0; outcome == OperandOutcome::Planned; ++index)
        outcome = planArgument(index);
    return outcome != OperandOutcome::NeedsReplanning;
}

OperandOutcome CallPlanner::planReceiver()
{
    if (!m_overload->receiver) {
        bool mayReadReceiver = m_overload->isUndeclared() && m_proven.receiver;
        return m_checksNullishReceiver || mayReadReceiver ? giveUp() : OperandOutcome::Planned;
    }

    m_plan.hasReceiver = true;
    OperandOutcome outcome = planOperand(BuiltinOperand::receiver());
    return outcome == OperandOutcome::NoMoreOperands ? giveUp() : outcome;
}

OperandOutcome CallPlanner::planArgument(unsigned index)
{
    if (index > BuiltinOperand::maxArgumentIndex)
        return giveUp();

    OperandOutcome outcome = planOperand(BuiltinOperand::argument(index));
    if (outcome == OperandOutcome::Planned)
        ++m_plan.numberOfArguments;
    return outcome;
}

OperandOutcome CallPlanner::planOperand(BuiltinOperand position)
{
    const BuiltinParameter* parameter = m_table.parameterFor(*m_overload, position);
    if (!parameter)
        return OperandOutcome::NoMoreOperands;

    PlannedOperand operand { position, *parameter, typeAfterTests(types(), m_proven, m_plan, position) };
    if (!operand.type) {
        switch (parameter->mode) {
        case BuiltinParameterMode::Required:
            if (distinguishesOmittedFromUndefined(m_table, *parameter))
                return OperandOutcome::NoMoreOperands;
            addStep(position, OperandStepKind::PassUndefined);
            operand.type = TypeTable::undefined;
            break;
        case BuiltinParameterMode::Optional:
        case BuiltinParameterMode::Rest:
            return OperandOutcome::NoMoreOperands;
        }
    }
    if (position.isReceiver() && m_checksNullishReceiver)
        addStep(position, OperandStepKind::RequireObjectCoercible);

    OperandOutcome outcome = tryPass(operand);
    if (outcome == OperandOutcome::Undecided)
        outcome = excludeThrowingValues(operand);
    if (outcome == OperandOutcome::Undecided)
        outcome = conversionOpcodesOf(parameter->conversion).empty() ? requireAcceptedType(operand) : planConversion(operand);
    return outcome;
}

OperandOutcome CallPlanner::tryPass(const PlannedOperand& operand)
{
    OperandFit fit = fitOf(m_context, operand.type, operand.parameter.accepted);
    if (fit.fit != TypeFit::Inside)
        return OperandOutcome::Undecided;

    if (fit.unprovenPristine) {
        OperandOutcome outcome = requirePristine(operand.position, fit.unprovenPristine);
        if (outcome != OperandOutcome::Planned)
            return outcome;
    }
    if (!hasStepFor(operand.position))
        addStep(operand.position, OperandStepKind::Pass);
    return OperandOutcome::Planned;
}

OperandOutcome CallPlanner::excludeThrowingValues(PlannedOperand& operand)
{
    BuiltinTypeID refused = operand.parameter.refused;
    if (!refused || fitIn(operand.type, refused) == TypeFit::Outside)
        return OperandOutcome::Undecided;

    TypeID expected = expectedWithin(operand);
    if (!expected || fitIn(expected, refused) != TypeFit::Outside)
        return giveUp();

    TypeID admitted = types().intern(types().at(operand.type).without(typeOfBuiltinType({ m_context }, refused), types()));
    if (require(operand, admitted) == OperandOutcome::GaveUp)
        return OperandOutcome::GaveUp;
    if (fitIn(operand.type, refused) != TypeFit::Outside)
        return giveUp();
    return tryPass(operand);
}

OperandOutcome CallPlanner::planConversion(PlannedOperand& operand)
{
    BuiltinConversion conversion = operand.parameter.conversion;
    const Type& type = types().at(operand.type);
    Type takenUnconverted = type.intersectWith(typeOfBuiltinType({ m_context }, operand.parameter.accepted), types());
    if (!isUnchangedByConversion(takenUnconverted, conversion))
        return requireAcceptedType(operand);

    Type convertible = typeConvertedWithoutUserCode(m_context, type, conversion);
    bool mayRunUserCode = convertible != type;

    if (mayRunUserCode && !m_mayHoistUserCode) {
        TypeID expected = expectedWithin(operand);
        if (!expected && followsOperandRefusedByValue(operand.position))
            expected = types().intern(convertible);
        if (!expected || !types().at(expected).isSubsetOf(convertible, m_context.program))
            return giveUp();
        if (require(operand, types().intern(convertible)) == OperandOutcome::GaveUp)
            return OperandOutcome::GaveUp;
        mayRunUserCode = false;
    }

    m_hoistsUserCode |= mayRunUserCode;
    addStep(operand.position, OperandStepKind::Convert, conversion);
    return OperandOutcome::Planned;
}

OperandOutcome CallPlanner::requireAcceptedType(PlannedOperand& operand)
{
    BuiltinTypeID accepted = operand.parameter.accepted;
    Type narrowed = types().at(operand.type).intersectWith(typeOfBuiltinType({ m_context }, accepted), types());
    TypeID expected = expectedWithin(operand);
    if (!expected && isExpectedToBeAccepted(types().at(operand.type), operand.parameter.conversion))
        expected = types().intern(narrowed);
    if (!expected || fitIn(expected, accepted) != TypeFit::Inside)
        return giveUp();

    // An operand that stands for a missing one is passed as it is. Behind a test that lets it through, no inline form knows whether the operand is there.
    narrowed = narrowed.withoutPrimitives(Primitive::UndefinedOrNull & ~types().at(expected).primitives);
    bool holdsExpectedType = types().at(expected).isSubsetOf(narrowed, m_context.program);
    if (require(operand, holdsExpectedType ? types().intern(narrowed) : expected) == OperandOutcome::GaveUp)
        return OperandOutcome::GaveUp;
    OperandOutcome outcome = tryPass(operand);
    return outcome == OperandOutcome::Undecided ? giveUp() : outcome;
}

OperandOutcome CallPlanner::require(PlannedOperand& operand, TypeID required)
{
    TypeID proven = m_query.overloads.proven.at(operand.position);
    const Type& requiredType = types().at(required);
    if (!proven || requiredType.isNever() || requiredType == types().at(proven))
        return giveUp();

    ensureRequirement(operand.position).requiredType = types().intern(withoutClassProofs(requiredType));
    m_plan.state = BuiltinMatchState::NeedsTests;
    operand.type = typeAfterTests(types(), m_proven, m_plan, operand.position);
    return OperandOutcome::Undecided;
}

OperandOutcome CallPlanner::requirePristine(BuiltinOperand position, const PristineRequirement& pristine)
{
    if (m_hoistsUserCode)
        return OperandOutcome::NeedsReplanning;
    m_mayHoistUserCode = false;

    PristineRequirement& required = ensureRequirement(position).pristine;
    if (required && required.builtinClass != pristine.builtinClass)
        return giveUp();
    required.builtinClass = pristine.builtinClass;
    required.aspects.add(pristine.aspects);
    return OperandOutcome::Planned;
}

OperandOutcome CallPlanner::giveUp()
{
    m_plan = BuiltinCallPlan { .overload = m_match.overload };
    return OperandOutcome::GaveUp;
}

TypeID CallPlanner::expectedWithin(const PlannedOperand& operand) const
{
    const OverloadQuery& overloads = m_query.overloads;
    TypeID expected = overloads.expected.at(operand.position);
    if (!expected || expected == overloads.proven.at(operand.position))
        return { };
    Type met = withoutClassProofs(types().at(expected)).intersectWith(types().at(operand.type), types());
    return met.isNever() ? TypeID() : types().intern(met);
}

static bool failsByValue(BuiltinConversion conversion)
{
    switch (conversion) {
    case BuiltinConversion::ToIndex:
    case BuiltinConversion::ToBigInt:
    case BuiltinConversion::Dictionary:
        return true;
    case BuiltinConversion::None:
    case BuiltinConversion::ToBoolean:
    case BuiltinConversion::ToNumber:
    case BuiltinConversion::ToNumeric:
    case BuiltinConversion::ToString:
    case BuiltinConversion::ToPropertyKey:
    case BuiltinConversion::ToPrimitive:
    case BuiltinConversion::ToObject:
    case BuiltinConversion::ToIntegerOrInfinity:
    case BuiltinConversion::ToLength:
    case BuiltinConversion::ToInt32:
    case BuiltinConversion::ToUint32:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The built-in refuses a negative index before it converts what follows the index, so code of what follows stays in the built-in. A value that is stored behind an index is a number nearly always, and the conversion of one runs no code.
bool CallPlanner::followsOperandRefusedByValue(BuiltinOperand position) const
{
    return std::ranges::any_of(m_overload->parameters, [&](const BuiltinParameter& parameter) {
        return precedes(parameter.operand, position) && (failsByValue(parameter.conversion) || parameter.admitted);
    });
}

OperandRequirement& CallPlanner::ensureRequirement(BuiltinOperand position)
{
    Vector<OperandRequirement, 2>& requirements = m_plan.requirements;
    size_t index = 0;
    while (index < requirements.size() && precedes(requirements[index].operand, position))
        ++index;
    if (index == requirements.size() || requirements[index].operand != position)
        requirements.insert(index, OperandRequirement { position, { }, { } });
    return requirements[index];
}

bool CallPlanner::hasStepFor(BuiltinOperand position) const
{
    return !m_plan.steps.isEmpty() && m_plan.steps.last().operand == position;
}

void CallPlanner::addStep(BuiltinOperand position, OperandStepKind kind, BuiltinConversion conversion)
{
    m_plan.steps.append(OperandStep { position, kind, conversion });
}

} // anonymous namespace

BuiltinCallPlan planBuiltinCall(const BuiltinCallQuery& query)
{
    return CallPlanner(query).plan();
}

static std::span<const OperandStep> takeOperandSteps(std::span<const OperandStep>& steps)
{
    size_t count = 1;
    while (count < steps.size() && steps[count].operand == steps[0].operand)
        ++count;
    std::span<const OperandStep> taken = steps.first(count);
    steps = steps.subspan(count);
    return taken;
}

template<typename Operands, typename Element>
static void appendOperand(Operands& operands, BuiltinOperand position, Element element)
{
    if (position.isReceiver()) {
        operands.receiver = element;
        return;
    }
    RELEASE_ASSERT(position.argumentIndex() == operands.arguments.size());
    operands.arguments.append(element);
}

static Value makeRequireObjectCoercible(ValueID value, const NullishCheck& check)
{
    Value result = IRBuilder::make(Opcode::RequireObjectCoercible, { value });
    result.setNullishAccess(check.access);
    if (check.name)
        result.setName(check.name);
    return result;
}

static ValueID insertStep(const OperandStep& step, ValueID value, const NullishCheck& check, const InsertBeforeCall& insert)
{
    switch (step.kind) {
    case OperandStepKind::Pass:
        return value;
    case OperandStepKind::PassUndefined:
        return insert(IRBuilder::make(Opcode::Undefined));
    case OperandStepKind::RequireObjectCoercible:
        return insert(makeRequireObjectCoercible(value, check));
    case OperandStepKind::Convert:
        for (Opcode opcode : conversionOpcodesOf(step.conversion))
            value = insert(IRBuilder::make(opcode, { value }));
        return value;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

BuiltinCallOperands insertOperandSteps(const BuiltinCallPlan& plan, const BuiltinCallOperands& operands, const NullishCheck& check, NOESCAPE const InsertBeforeCall& insert)
{
    BuiltinCall passed { plan.overload, BuiltinCallForm::Whole, 0, operands.receiver, operands.arguments.span() };
    BuiltinCallOperands result;
    result.arguments.reserveInitialCapacity(plan.numberOfArguments);

    std::span<const OperandStep> remaining = plan.steps.span();
    while (!remaining.empty()) {
        std::span<const OperandStep> steps = takeOperandSteps(remaining);
        BuiltinOperand position = steps.front().operand;
        ValueID value = passed.operandAt(position);
        for (const OperandStep& step : steps)
            value = insertStep(step, value, check, insert);
        appendOperand(result, position, value);
    }
    return result;
}

void becomeBuiltinCall(Value& value, BuiltinOverloadID overload, BuiltinCallForm form, const BuiltinCallOperands& operands, const BuiltinCallStamps& stamps)
{
    value.become(Opcode::CallBuiltin);
    value.setOverload(overload);
    value.setBuiltinCallForm(form);
    if (operands.receiver)
        value.setReceiverOperand(operands.receiver);
    value.setArgumentsOperands(operands.arguments.span());
    if (stamps.expectedResult)
        value.setExpectedResult(stamps.expectedResult);
    if (stamps.accessSite)
        value.setAccessSite(stamps.accessSite);
    if (stamps.allocationSite)
        value.setAllocationSite(stamps.allocationSite);
}

Value makeBuiltinCall(BuiltinOverloadID overload, BuiltinCallForm form, const BuiltinCallOperands& operands, const BuiltinCallStamps& stamps)
{
    Value value;
    becomeBuiltinCall(value, overload, form, operands, stamps);
    return value;
}

static TypeID addStepEffects(const BuiltinCallQuery& query, const BuiltinOverload& overload, const OperandStep& step, TypeID before, Effects& effects)
{
    const TypingContext& context = query.overloads.context;
    switch (step.kind) {
    case OperandStepKind::Pass:
        return before;
    case OperandStepKind::PassUndefined:
        return TypeTable::undefined;
    case OperandStepKind::RequireObjectCoercible:
        effects.merge(declaredBoundOf(Opcode::RequireObjectCoercible));
        return context.types.withoutPrimitives(before, Primitive::UndefinedOrNull);
    case OperandStepKind::Convert: {
        for (Opcode opcode : conversionOpcodesOf(step.conversion))
            effects.merge(declaredBoundOf(opcode));
        EffectsOnEdges operandCode = EffectsOnEdges::none();
        addOperandCode(query.effects, context.types.at(before), { .uses = { }, .protocols = protocolsRunBy(step.conversion) }, operandCode);
        effects.merge(operandCode.normal);

        const BuiltinParameter* parameter = context.program.builtins().parameterFor(overload, step.operand);
        RELEASE_ASSERT(parameter);
        return context.types.intern(typeOfBuiltinType({ context }, parameter->converted ? parameter->converted : parameter->accepted));
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Effects effectsOfPlannedCall(const BuiltinCallQuery& query, const BuiltinCallPlan& plan, BuiltinCallForm form)
{
    const OverloadQuery& overloads = query.overloads;
    if (plan.state == BuiltinMatchState::Unknown)
        return effectsOfCalling(query.effects, overloads.builtin, overloads.mode).normal;

    const TypingContext& context = overloads.context;
    const BuiltinOverload& overload = context.program.builtins().at(plan.overload);
    Effects effects = Effects::none();
    BuiltinOperandTypes converted;

    std::span<const OperandStep> remaining = plan.steps.span();
    while (!remaining.empty()) {
        std::span<const OperandStep> steps = takeOperandSteps(remaining);
        BuiltinOperand position = steps.front().operand;
        TypeID type = typeAfterTests(context.types, overloads.proven, plan, position);
        for (const OperandStep& step : steps)
            type = addStepEffects(query, overload, step, type, effects);
        appendOperand(converted, position, type);
    }

    const Type* receiver = converted.receiver ? &context.types.at(converted.receiver) : nullptr;
    effects.merge(effectsOfOverload(query.effects, plan.overload, form, receiver, converted.arguments.span()).normal);
    return effects;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
