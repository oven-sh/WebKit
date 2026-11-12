/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBuiltinTypes.h"

#if ENABLE(AOT)

#include "AOTBuiltinTableBindings.h"
#include "AOTIRProgram.h"
#include "AOTLimits.h"
#include "AOTResultRules.h"
#include "AOTTypeArena.h"
#include "AOTVector.h"
#include <algorithm>
#include <optional>
#include <span>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint64_t maxArgumentsOfTypedBuiltinCall = defaultLimitValue(LimitID::MaxBuiltinCallArguments, hostArchitecture());
static_assert(maxArgumentsOfTypedBuiltinCall <= BuiltinOperand::maxArgumentIndex);

static constexpr int64_t signOfRangeBound(int64_t bound)
{
    if (bound < 0)
        return -1;
    return bound > 0 ? 1 : 0;
}

Range computeResultRange(BuiltinRangeRule rule, std::span<const Range> operandRanges)
{
    RELEASE_ASSERT(!operandRanges.empty());
    if (std::ranges::any_of(operandRanges, &Range::isEmpty))
        return Range::empty();

    Range first = operandRanges.front();
    switch (rule) {
    case BuiltinRangeRule::None:
        return Range::full();
    case BuiltinRangeRule::MinimumOfArguments: {
        Range result = first;
        for (Range operand : operandRanges.subspan(1))
            result = { std::min(result.minimum, operand.minimum), std::min(result.maximum, operand.maximum) };
        return result;
    }
    case BuiltinRangeRule::MaximumOfArguments: {
        Range result = first;
        for (Range operand : operandRanges.subspan(1))
            result = { std::max(result.minimum, operand.minimum), std::max(result.maximum, operand.maximum) };
        return result;
    }
    case BuiltinRangeRule::AbsoluteValue: {
        ASSERT(first.isSubsetOf(Range::full()));
        int64_t smallestMagnitude = 0;
        if (first.minimum > 0)
            smallestMagnitude = first.minimum;
        else if (first.maximum < 0)
            smallestMagnitude = -first.maximum;
        return { smallestMagnitude, std::max(-first.minimum, first.maximum) };
    }
    case BuiltinRangeRule::IntegerPart:
        return first;
    case BuiltinRangeRule::Sign:
        return { signOfRangeBound(first.minimum), signOfRangeBound(first.maximum) };
    case BuiltinRangeRule::CodeUnitOrNaN:
        return { 0, UINT16_MAX };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static TypeID typeOfRuledResultOfFractions(TypeArena& types, const BuiltinOverload& overload, Range range)
{
    switch (overload.result.rangeRule) {
    case BuiltinRangeRule::None:
    case BuiltinRangeRule::CodeUnitOrNaN:
        break;
    case BuiltinRangeRule::MinimumOfArguments:
    case BuiltinRangeRule::MaximumOfArguments:
    case BuiltinRangeRule::AbsoluteValue:
        return types.numbersWithin(range);
    case BuiltinRangeRule::Sign:
        return types.integer(range);
    case BuiltinRangeRule::IntegerPart: {
        bool roundsDown = overload.code.implementation.inlineBuiltin == InlineBuiltin::MathFloor;
        bool mayBeNegativeZero = !roundsDown && range.minimum < 0 && range.maximum >= 0;
        return mayBeNegativeZero ? types.withPrimitives(types.integer(range), Primitive::WholeNumber) : types.integer(range);
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static TypeID applyRangeRuleToBuiltinResult(const BuiltinTypeContext& context, const BuiltinOverload& overload, const BuiltinOperandTypes& operands, TypeID declared)
{
    const BuiltinResult& result = overload.result;
    if (result.rangeRule == BuiltinRangeRule::None || !result.rangeOperand)
        return declared;
    if (result.rangeRule == BuiltinRangeRule::CodeUnitOrNaN)
        return context.types.intersectionOf(context.types.withPrimitives(context.types.integer({ 0, UINT16_MAX }), Primitive::WholeNumber), declared);
    if (result.rangeRule == BuiltinRangeRule::IntegerPart)
        declared = context.types.intersectionOf(TypeArena::fromPrimitives(Primitive::NumberWithoutFraction), declared);

    TypeID onlyOperand;
    std::span<const TypeID> rangeOperands;
    switch (result.rangeOperand->kind()) {
    case BuiltinOperand::Kind::Receiver:
    case BuiltinOperand::Kind::Argument:
        onlyOperand = operands.at(*result.rangeOperand);
        rangeOperands = singleElementSpan(onlyOperand);
        break;
    case BuiltinOperand::Kind::ArgumentsFrom:
        rangeOperands = operands.arguments.span().subspan(std::min<size_t>(result.rangeOperand->argumentIndex(), operands.arguments.size()));
        break;
    }
    if (rangeOperands.empty())
        return declared;
    if (std::ranges::contains(rangeOperands, TypeTable::never))
        return TypeTable::never;

    TypeArena& types = context.types;
    Vector<Range, 8> ranges;
    bool takesOnlyIntegers = true;
    for (TypeID operand : rangeOperands) {
        if (!operand)
            break;
        Type type = types.at(operand);
        if (!type.isNumberWithinBounds())
            break;
        takesOnlyIntegers &= type.isInteger();
        ranges.append(type.boundsOfNumbers());
    }
    if (ranges.size() == rangeOperands.size()) {
        Range range = computeResultRange(result.rangeRule, ranges.span());
        return types.intersectionOf(takesOnlyIntegers ? types.integer(range) : typeOfRuledResultOfFractions(types, overload, range), declared);
    }
    return declared;
}

static ALWAYS_INLINE TypeID resultTypeOfWholeBuiltinCall(const BuiltinTypeContext& context, BuiltinOverloadID id, const BuiltinOperandTypes* operands)
{
    const BuiltinOverload& overload = context.program.builtins().at(id);
    if (overload.isUndeclared() || (overload.resultIsDeclaredOnly() && context.level == TypeLevel::Proven))
        return context.types.anything(UnknownSeed::UndescribedNativeResult);
    if (context.program.shouldAssumeNullishHostResults && overload.code.implementation.kind == BuiltinImplementationKind::HostCall)
        return TypeArena::fromPrimitives(Primitive::UndefinedOrNull);
    if (context.program.builtinInjectedAsNull == overload.builtin.index()) [[unlikely]]
        return TypeArena::fromPrimitives(Primitive::Null);

    TypeID declared = typeOfBuiltinType(context, overload.result.type, operands);
    TypeID written = operands ? applyRangeRuleToBuiltinResult(context, overload, *operands, declared) : declared;
    if (context.program.builtinInjectedAsMinimumInteger == overload.builtin.index()) [[unlikely]] {
        Range integers = context.types.at(written).integers;
        if (!integers.isEmpty())
            return context.types.withIntegers(written, Range::one(integers.minimum));
    }
    return written;
}

static bool hasNeverAmongBuiltinOperands(const BuiltinOperandTypes& operands)
{
    return operands.receiver == TypeTable::never || operands.arguments.contains(TypeTable::never);
}

static BuiltinOperandTypes operandTypesInsideParametersOf(const BuiltinTypeContext& context, BuiltinOverloadID id, const BuiltinOperandTypes& passed)
{
    const BuiltinTable& table = context.program.builtins();
    const BuiltinOverload& overload = table.at(id);
    auto narrow = [&](BuiltinOperand position, TypeID type) {
        const BuiltinParameter* parameter = table.parameterFor(overload, position);
        if (!type || !parameter || parameter->conversion != BuiltinConversion::None)
            return type;
        return context.types.intersectionOf(type, parameterTypeOfOverload(context, id, position));
    };

    BuiltinOperandTypes narrowed;
    narrowed.receiver = narrow(BuiltinOperand::receiver(), passed.receiver);
    narrowed.arguments.reserveInitialCapacity(passed.arguments.size());
    for (unsigned index = 0; index < passed.arguments.size(); ++index)
        narrowed.arguments.append(narrow(BuiltinOperand::argument(index), passed.arguments[index]));
    return narrowed;
}

static TypeFit fitProvenByTypeAlone(const BuiltinTypeContext& context, TypeID operandOrNotPassed, BuiltinTypeID tested, PristineAspectSet seenWhereCallStands)
{
    OperandFit fit = fitOf(context, operandOrNotPassed, tested);
    if (fit.unprovenPristine && seenWhereCallStands.containsAll(fit.unprovenPristine.aspects))
        return TypeFit::Inside;
    if (fit.unprovenPristine)
        return TypeFit::Unknown;
    return fit.fit;
}

static TypeFit fitOfOperandsAtDispatchNode(const BuiltinTypeContext& context, const BuiltinDispatchNode& node, BuiltinTypeID tested, const BuiltinOperandTypes& operands)
{
    switch (node.operand.kind()) {
    case BuiltinOperand::Kind::Receiver:
        return fitProvenByTypeAlone(context, operands.receiver, tested, operands.aspectsOfReceiver);
    case BuiltinOperand::Kind::Argument:
        return fitProvenByTypeAlone(context, operands.at(node.operand), tested, operands.aspectsOfArgument(node.operand.argumentIndex()));
    case BuiltinOperand::Kind::ArgumentsFrom: {
        TypeFit fitOfEveryArgument = TypeFit::Inside;
        for (size_t index = node.operand.argumentIndex(); index < operands.arguments.size(); ++index) {
            switch (fitProvenByTypeAlone(context, operands.arguments[index], tested, operands.aspectsOfArgument(index))) {
            case TypeFit::Outside:
                return TypeFit::Outside;
            case TypeFit::Unknown:
                fitOfEveryArgument = TypeFit::Unknown;
                break;
            case TypeFit::Inside:
                break;
            }
        }
        return fitOfEveryArgument;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void collectOverloadsReachedFrom(const BuiltinTypeContext& context, BuiltinDispatchTarget target, const BuiltinOperandTypes& operands, Vector<BuiltinDispatchNodeID, 8>& visitedNodes, Vector<BuiltinOverloadID, 4>& reached)
{
    if (target.isOverload()) {
        if (!reached.contains(target.overload))
            reached.append(target.overload);
        return;
    }
    if (visitedNodes.contains(target.node))
        return;
    visitedNodes.append(target.node);

    const BuiltinDispatchNode& node = context.program.builtins().at(target.node);
    for (const BuiltinDispatchBranch& branch : node.branches) {
        switch (fitOfOperandsAtDispatchNode(context, node, branch.tested, operands)) {
        case TypeFit::Outside:
            continue;
        case TypeFit::Unknown:
            collectOverloadsReachedFrom(context, branch.target, operands, visitedNodes, reached);
            continue;
        case TypeFit::Inside:
            collectOverloadsReachedFrom(context, branch.target, operands, visitedNodes, reached);
            return;
        }
    }
}

Vector<BuiltinOverloadID, 4> overloadsReachedBy(const BuiltinTypeContext& context, BuiltinID builtin, BuiltinCallMode mode, const BuiltinOperandTypes& passed)
{
    const BuiltinTable& table = context.program.builtins();
    Vector<BuiltinOverloadID, 4> reached;
    std::optional<BuiltinDispatchTarget> root = table.dispatchOf(table.canonicalID(builtin), mode);
    if (!root || passed.arguments.size() > maxArgumentsOfTypedBuiltinCall)
        return reached;
    Vector<BuiltinDispatchNodeID, 8> visitedNodes;
    collectOverloadsReachedFrom(context, *root, passed, visitedNodes, reached);
    return reached;
}

TypeID resultTypeOfReachableOverloads(const BuiltinTypeContext& context, BuiltinID builtin, BuiltinCallMode mode, const BuiltinOperandTypes& passed)
{
    if (hasNeverAmongBuiltinOperands(passed))
        return TypeTable::never;

    TypeArena& types = context.types;
    const BuiltinTable& table = context.program.builtins();
    std::optional<BuiltinDispatchTarget> root = table.dispatchOf(table.canonicalID(builtin), mode);
    if (!root || passed.arguments.size() > maxArgumentsOfTypedBuiltinCall || !rowsSpeakOfCallOnReceiver(context, builtin, passed.receiver))
        return types.anything(UnknownSeed::UndescribedNativeResult);

    Vector<BuiltinDispatchNodeID, 8> visitedNodes;
    Vector<BuiltinOverloadID, 4> reached;
    collectOverloadsReachedFrom(context, *root, passed, visitedNodes, reached);
    if (reached.isEmpty())
        return types.anything(UnknownSeed::UndescribedNativeResult);

    TypeID result = TypeTable::never;
    for (BuiltinOverloadID id : reached) {
        BuiltinOperandTypes operands = operandTypesInsideParametersOf(context, id, passed);
        result = types.unionOf(result, resultTypeOfWholeBuiltinCall(context, id, &operands));
    }
    return result;
}

bool rowsSpeakOfCallOnReceiver(const BuiltinTypeContext& context, BuiltinID builtin, TypeID receiverOrNotPassed)
{
    const BuiltinTable& table = context.program.builtins();
    const Builtin& entry = table.at(table.canonicalID(builtin));
    if (!receiverOrNotPassed || !entry.flags.contains(BuiltinFlag::RequiresHolderAsThis))
        return true;

    Type type = context.types.at(receiverOrNotPassed);
    if (!entry.holder)
        return !type.isNever() && type.hasOnlyPrimitives(Primitive::Undefined);
    BuiltinID passed = type.isObject() ? singleBuiltinOf(context.types, type) : BuiltinID();
    return passed && table.canonicalID(passed) == table.canonicalID(entry.holder);
}

TypeID resultTypeOfBuiltinCallOnList(const BuiltinTypeContext& context, BuiltinOverloadID overload, const BuiltinOperandTypes& operands)
{
    return hasNeverAmongBuiltinOperands(operands) ? TypeID(TypeTable::never) : resultTypeOfWholeBuiltinCall(context, overload, nullptr);
}

TypeID resultTypeOfBuiltinCall(const BuiltinTypeContext& context, const BuiltinResultQuery& query)
{
    if (hasNeverAmongBuiltinOperands(query.operands))
        return TypeTable::never;

    switch (query.form) {
    case BuiltinCallForm::Whole:
        return resultTypeOfWholeBuiltinCall(context, query.overload, &query.operands);
    case BuiltinCallForm::AnyReceiver: {
        const BuiltinOverload& overload = context.program.builtins().at(query.overload);
        return resultTypeOfReachableOverloads(context, overload.builtin, overload.mode, query.operands);
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool distinguishesOmittedFromUndefined(const BuiltinTable& table, const BuiltinParameter& parameter)
{
    if (!parameter.accepted)
        return false;
    OptionSet<BuiltinPrimitive> primitives = table.at(parameter.accepted).primitives;
    return primitives.contains(BuiltinPrimitive::NotPassed) && !primitives.contains(BuiltinPrimitive::Undefined);
}

static bool liesInBuiltinParameter(const BuiltinTypeContext& context, const BuiltinOverload& overload, BuiltinOperand position, TypeID type)
{
    const BuiltinParameter* parameter = context.program.builtins().parameterFor(overload, position);
    if (!parameter)
        return false;
    if (fitsIfPristine(context, type, parameter->accepted))
        return true;
    return parameter->converted && fitsIfPristine(context, type, parameter->converted);
}

static bool operandsLieInBuiltinParameters(const BuiltinTypeContext& context, const BuiltinOverload& overload, const BuiltinOperandTypes& operands)
{
    if (!!operands.receiver != !!overload.receiver)
        return false;
    if (operands.receiver && !liesInBuiltinParameter(context, overload, BuiltinOperand::receiver(), operands.receiver))
        return false;
    for (unsigned index = 0; index < operands.arguments.size(); ++index) {
        if (!liesInBuiltinParameter(context, overload, BuiltinOperand::argument(index), operands.arguments[index]))
            return false;
    }

    const BuiltinTable& table = context.program.builtins();
    return std::ranges::none_of(overload.parameters, [&](const BuiltinParameter& parameter) {
        bool isPassed = parameter.operand.argumentIndex() < operands.arguments.size();
        return !isPassed && parameter.mode == BuiltinParameterMode::Required && !distinguishesOmittedFromUndefined(table, parameter);
    });
}

bool overloadTakesOperands(const BuiltinTypeContext& context, BuiltinOverloadID id, BuiltinCallForm form, const BuiltinOperandTypes& operands)
{
    const BuiltinTable& table = context.program.builtins();
    if (!table.contains(id))
        return false;
    if (operands.arguments.size() > maxArgumentsOfTypedBuiltinCall)
        return false;

    switch (form) {
    case BuiltinCallForm::AnyReceiver:
        return true;
    case BuiltinCallForm::Whole:
        return operandsLieInBuiltinParameters(context, table.at(id), operands);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isAnsweredByDirectFunction(const BuiltinTable& table, BuiltinOverloadID overload)
{
    if (!BuiltinBindings::hasHost() && table.at(overload).code.implementation.kind == BuiltinImplementationKind::HostCall)
        return true;
    return !!BuiltinBindings::directFunctionNumberOf(overload);
}

bool requiresWalkLowering(const BuiltinTable& table, BuiltinID builtin)
{
    BuiltinID called = table.canonicalID(builtin);
    const Builtin& entry = table.at(called);
    if (!entry.flags.contains(BuiltinFlag::IsCompilerHelper) && entry.holder != Builtins::BunBundle)
        return false;

    for (BuiltinOverloadID overload : table.overloadIDsOf(called)) {
        if (!isAnsweredByDirectFunction(table, overload))
            return true;
    }
    return false;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
