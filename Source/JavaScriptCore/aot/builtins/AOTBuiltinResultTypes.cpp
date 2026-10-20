/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBuiltinCalls.h"

#if ENABLE(AOT)

#include "AOTBuiltinCallPlan.h"
#include "AOTBuiltinTypes.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static std::optional<Range> integersOf(const TypingContext& context, TypeID id)
{
    if (!id)
        return std::nullopt;
    const Type& type = context.types.at(id);
    if (!type.isInteger())
        return std::nullopt;
    return type.integers;
}

static Vector<TypeID, 4> rangeOperandsOf(BuiltinOperand operand, const BuiltinOperandTypes& operands)
{
    switch (operand.kind()) {
    case BuiltinOperand::Kind::Receiver:
    case BuiltinOperand::Kind::Argument:
        return { operands.at(operand) };
    case BuiltinOperand::Kind::ArgumentsFrom:
        if (operand.argumentIndex() >= operands.arguments.size())
            return { };
        return Vector<TypeID, 4>(operands.arguments.span().subspan(operand.argumentIndex()));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static constexpr int64_t signOf(int64_t integer)
{
    if (integer < 0)
        return -1;
    return integer > 0 ? 1 : 0;
}

Range computeResultRange(BuiltinRangeRule rule, std::span<const Range> operandRanges)
{
    RELEASE_ASSERT(!operandRanges.empty());
    Range first = operandRanges[0];
    switch (rule) {
    case BuiltinRangeRule::None:
        break;
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
        return { signOf(first.minimum), signOf(first.maximum) };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Type applyRangeRule(const TypingContext& context, const BuiltinResult& result, const BuiltinOperandTypes* operands, Type declared)
{
    if (result.rangeRule == BuiltinRangeRule::None || !result.rangeOperand || !operands)
        return declared;

    Vector<TypeID, 4> types = rangeOperandsOf(*result.rangeOperand, *operands);
    Vector<Range, 4> integers;
    for (TypeID type : types) {
        if (type && context.types.at(type).isNever())
            return Type::never();
        if (std::optional<Range> range = integersOf(context, type))
            integers.append(*range);
    }

    if (types.isEmpty())
        return declared;
    if (integers.size() == types.size())
        return Type::integer(computeResultRange(result.rangeRule, integers.span())).intersectWith(declared, context.types);
    if (result.rangeRule == BuiltinRangeRule::IntegerPart && types[0] && context.types.at(types[0]).isNumber())
        return Type::fromPrimitives(Primitive::NumberWithoutFraction).intersectWith(declared, context.types);
    return declared;
}

static Type wholeResultType(const TypingContext& context, BuiltinOverloadID id, const BuiltinOperandTypes* operands)
{
    const BuiltinOverload& overload = context.program.builtins().at(id);
    if (overload.isUndeclared())
        return Type::anything();
    Type declared = typeOfBuiltinType({ context, operands }, overload.result.type);
    return applyRangeRule(context, overload.result, operands, WTF::move(declared));
}

static bool hasNeverOperand(const BuiltinOperandTypes& operands)
{
    return operands.receiver == TypeTable::never || operands.arguments.contains(TypeTable::never);
}

// A call ends in an overload with its operands inside the strict parameters. A conversion may change the kind of an operand.
static BuiltinOperandTypes operandTypesWithin(const TypingContext& context, BuiltinOverloadID id, const BuiltinOperandTypes& operands)
{
    const BuiltinTable& table = context.program.builtins();
    auto narrow = [&](BuiltinOperand position, TypeID type) {
        const BuiltinParameter* parameter = table.parameterFor(table.at(id), position);
        if (!type || !parameter || parameter->conversion != BuiltinConversion::None)
            return type;
        return context.types.intern(context.types.at(type).intersectWith(parameterTypeOfOverload(context.program, id, position), context.types));
    };

    BuiltinOperandTypes narrowed;
    narrowed.receiver = narrow(BuiltinOperand::receiver(), operands.receiver);
    narrowed.arguments.reserveInitialCapacity(operands.arguments.size());
    for (unsigned index = 0; index < operands.arguments.size(); ++index)
        narrowed.arguments.append(narrow(BuiltinOperand::argument(index), operands.arguments[index]));
    return narrowed;
}

Type resultTypeOfReachableOverloads(const TypingContext& context, BuiltinID builtin, BuiltinCallMode mode, const BuiltinOperandTypes& passed)
{
    if (hasNeverOperand(passed))
        return Type::never();

    Type result = Type::never();
    bool hasReachableOverload = false;
    forEachReachableOverload(OverloadQuery { context, builtin, mode, passed, passed }, [&](BuiltinOverloadID id) {
        hasReachableOverload = true;
        BuiltinOperandTypes operands = operandTypesWithin(context, id, passed);
        result = result.unionWith(wholeResultType(context, id, &operands), context.types);
    });
    if (!hasReachableOverload)
        return Type::anything();
    return result;
}

static Type unmatchedResultType(const TypingContext& context, const BuiltinResultQuery& query)
{
    const BuiltinOverload& overload = context.program.builtins().at(query.overload);
    return resultTypeOfReachableOverloads(context, overload.builtin, overload.mode, query.operands);
}

static Type prefixElementType(const BuiltinTypeInputs& inputs, const BuiltinTypePart& part, uint32_t index)
{
    switch (part.kind) {
    case BuiltinTypePartKind::Instance:
        for (const BuiltinSlotContents& contents : part.contents) {
            if (contents.slot == BuiltinSlots::ArrayElement)
                return typeOfBuiltinType(inputs, contents.type);
        }
        return Type::anything();
    case BuiltinTypePartKind::Layout: {
        std::span<const BuiltinLayoutField> fields = inputs.context.program.builtins().at(part.layout).fields;
        if (index < fields.size())
            return typeOfBuiltinType(inputs, fields[index].type);
        Type anyElement = Type::never();
        for (const BuiltinLayoutField& field : fields)
            anyElement = anyElement.unionWith(typeOfBuiltinType(inputs, field.type), inputs.context.types);
        return anyElement;
    }
    case BuiltinTypePartKind::InstanceOfReceiverClass:
    case BuiltinTypePartKind::TypedArrayOfReceiverElementType:
    case BuiltinTypePartKind::Builtin:
    case BuiltinTypePartKind::Operand:
    case BuiltinTypePartKind::Slot:
    case BuiltinTypePartKind::IteratedValues:
    case BuiltinTypePartKind::CallbackResult:
    case BuiltinTypePartKind::Prototype:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Type typeOfPart(const TypingContext& context, const BuiltinResultQuery& query, const BuiltinResultIndexes& indexes, uint32_t resultIndex)
{
    if (resultIndex == indexes.presence)
        return Type::fromPrimitives(Primitive::Boolean);
    if (resultIndex == indexes.length)
        return Type::integer(Range::uint32());

    const BuiltinTable& table = context.program.builtins();
    const BuiltinOverload& overload = table.at(query.overload);
    BuiltinTypeInputs inputs { context, &query.operands };
    if (resultIndex < indexes.firstPart) {
        RELEASE_ASSERT(resultIndex >= indexes.firstProperty);
        return typeOfBuiltinType(inputs, overload.code.prefixProperties[resultIndex - indexes.firstProperty].type);
    }

    RELEASE_ASSERT(resultIndex < indexes.numberOfResults());
    if (indexes.partsAreBounds)
        return Type::integer(Range { absentBound, Range::length().maximum });

    uint32_t index = resultIndex - indexes.firstPart;
    const BuiltinType& result = table.at(overload.result.type);
    RELEASE_ASSERT(result.parts.size() == 1);
    const BuiltinTypePart& part = result.parts[0];

    switch (query.form) {
    case BuiltinCallForm::Fields: {
        const BuiltinLayoutField& field = table.at(part.layout).fields[index];
        Type type = typeOfBuiltinType(inputs, field.type);
        if (field.isOptional)
            return type.withPrimitives(Primitive::Undefined);
        return type;
    }
    case BuiltinCallForm::Prefix:
        return prefixElementType(inputs, part, index).withPrimitives(Primitive::Undefined);
    case BuiltinCallForm::Whole:
    case BuiltinCallForm::AnyReceiver:
    case BuiltinCallForm::List:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Type resultTypeOfBuiltinCall(const TypingContext& context, const BuiltinResultQuery& query, uint32_t resultIndex)
{
    switch (query.form) {
    case BuiltinCallForm::Whole:
        RELEASE_ASSERT(!resultIndex);
        return wholeResultType(context, query.overload, &query.operands);
    case BuiltinCallForm::List:
        RELEASE_ASSERT(!resultIndex);
        return wholeResultType(context, query.overload, nullptr);
    case BuiltinCallForm::AnyReceiver:
        RELEASE_ASSERT(!resultIndex);
        return unmatchedResultType(context, query);
    case BuiltinCallForm::Fields:
    case BuiltinCallForm::Prefix: {
        std::optional<BuiltinResultIndexes> indexes = resultIndexesOf(context.program.builtins(), query.overload, query.form, query.prefixLength);
        RELEASE_ASSERT(indexes);
        return typeOfPart(context, query, *indexes, resultIndex);
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

template<typename Carrier>
static Type resultTypeOfCarrier(const TypingContext& context, const Carrier& call, BuiltinOverloadID overload, uint32_t resultIndex)
{
    BuiltinCall view = builtinCallOf(context.program, call);
    RELEASE_ASSERT(view.overload == overload);
    BuiltinOperandTypes operands = operandTypesOf(context, view);
    return resultTypeOfBuiltinCall(context, { overload, view.form, view.prefixLength, operands }, resultIndex);
}

Type resultTypeOfOverload(const TypingContext& context, const Value& call, BuiltinOverloadID overload, uint32_t resultIndex)
{
    return resultTypeOfCarrier(context, call, overload, resultIndex);
}

Type resultTypeOfOverload(const TypingContext& context, const Terminator& call, BuiltinOverloadID overload, uint32_t resultIndex)
{
    return resultTypeOfCarrier(context, call, overload, resultIndex);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
