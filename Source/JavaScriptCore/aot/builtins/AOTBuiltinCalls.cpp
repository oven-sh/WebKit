/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBuiltinCalls.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTBuiltinTableBindings.h"
#include "AOTBuiltinTypes.h"
#include "AOTEffectSummaries.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsOfRules.h"
#include "AOTLimits.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint64_t maxArgumentCount = defaultLimitValue(LimitID::MaxBuiltinCallArguments, hostArchitecture());
static_assert(maxArgumentCount <= BuiltinOperand::maxArgumentIndex);

static constexpr OptionSet<BuiltinPrimitive> nullishPrimitives { BuiltinPrimitive::Undefined, BuiltinPrimitive::Null };

template<typename Carrier>
static BuiltinCall inlineCallOf(const BuiltinTable& table, const Carrier& carrier)
{
    BuiltinOverloadID overload = carrier.overload();
    bool takesList = table.at(overload).code.listForm == carrier.inlineBuiltin();
    return { overload, takesList ? BuiltinCallForm::List : BuiltinCallForm::Whole, 0, carrier.receiverOperand(), carrier.argumentsOperands() };
}

BuiltinCall builtinCallOf(const Program& program, const Value& value)
{
    if (value.opcode == Opcode::CallInline)
        return inlineCallOf(program.builtins(), value);
    RELEASE_ASSERT(value.opcode == Opcode::CallBuiltin);
    return { value.overload(), value.builtinCallForm(), value.prefixLength().value_or(0), value.receiverOperand(), value.argumentsOperands() };
}

BuiltinCall builtinCallOf(const Program& program, const Terminator& terminator)
{
    RELEASE_ASSERT(terminator.kind == TerminatorKind::TryCallInline);
    return inlineCallOf(program.builtins(), terminator);
}

BuiltinOperandTypes operandTypesOf(const TypingContext& context, const BuiltinCall& call)
{
    return operandTypesOf(context, call.receiver, call.arguments);
}

BuiltinOperandTypes operandTypesOf(const TypingContext& context, ValueID receiver, std::span<const ValueID> arguments)
{
    BuiltinOperandTypes types;
    if (receiver)
        types.receiver = context.typeIDOf(receiver);
    types.arguments.reserveInitialCapacity(arguments.size());
    for (ValueID argument : arguments)
        types.arguments.append(context.typeIDOf(argument));
    return types;
}

static const BuiltinTypePart& aggregatePartOf(const BuiltinType& result)
{
    RELEASE_ASSERT(result.flags.isEmpty());
    RELEASE_ASSERT(result.stringLiterals.empty());
    RELEASE_ASSERT(nullishPrimitives.containsAll(result.primitives));
    RELEASE_ASSERT(result.parts.size() == 1);
    return result.parts[0];
}

static bool isArrayPart(const BuiltinTable& table, const BuiltinTypePart& part)
{
    switch (part.kind) {
    case BuiltinTypePartKind::Instance:
        return part.builtinClass == BuiltinClasses::Array;
    case BuiltinTypePartKind::Layout:
        return table.at(part.layout).isTuple;
    case BuiltinTypePartKind::InstanceOfReceiverClass:
    case BuiltinTypePartKind::TypedArrayOfReceiverElementType:
    case BuiltinTypePartKind::Builtin:
    case BuiltinTypePartKind::Operand:
    case BuiltinTypePartKind::Slot:
    case BuiltinTypePartKind::IteratedValues:
    case BuiltinTypePartKind::CallbackResult:
    case BuiltinTypePartKind::Prototype:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static BuiltinResultIndexes indexesAfterPresence(const BuiltinType& result)
{
    BuiltinResultIndexes indexes;
    if (!result.primitives.isEmpty()) {
        indexes.presence = 0;
        indexes.firstPart = 1;
    }
    return indexes;
}

std::optional<BuiltinResultIndexes> resultIndexesOf(const BuiltinTable& table, BuiltinOverloadID id, BuiltinCallForm form, uint32_t prefixLength)
{
    switch (form) {
    case BuiltinCallForm::Whole:
    case BuiltinCallForm::AnyReceiver:
    case BuiltinCallForm::List:
        return std::nullopt;
    case BuiltinCallForm::Fields: {
        const BuiltinType& result = table.at(table.at(id).result.type);
        const BuiltinTypePart& part = aggregatePartOf(result);
        RELEASE_ASSERT(part.kind == BuiltinTypePartKind::Layout);
        const BuiltinLayout& layout = table.at(part.layout);
        RELEASE_ASSERT(!layout.isTuple);
        BuiltinResultIndexes indexes = indexesAfterPresence(result);
        indexes.numberOfParts = static_cast<uint32_t>(layout.fields.size());
        return indexes;
    }
    case BuiltinCallForm::Prefix: {
        const BuiltinOverload& overload = table.at(id);
        const BuiltinType& result = table.at(overload.result.type);
        RELEASE_ASSERT(isArrayPart(table, aggregatePartOf(result)));
        BuiltinResultIndexes indexes = indexesAfterPresence(result);
        indexes.length = indexes.firstPart++;
        indexes.firstProperty = indexes.firstPart;
        indexes.numberOfProperties = static_cast<uint32_t>(overload.code.prefixProperties.size());
        indexes.firstPart += indexes.numberOfProperties;
        indexes.numberOfParts = prefixLength;
        indexes.partsAreBounds = overload.result.substringOf.has_value();
        return indexes;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

std::optional<uint32_t> numberOfResultsOfOverload(const Program& program, const Value& call)
{
    BuiltinCall view = builtinCallOf(program, call);
    std::optional<BuiltinResultIndexes> indexes = resultIndexesOf(program.builtins(), view.overload, view.form, view.prefixLength);
    if (!indexes)
        return std::nullopt;
    return indexes->numberOfResults();
}

bool distinguishesOmittedFromUndefined(const BuiltinTable& table, const BuiltinParameter& parameter)
{
    if (!parameter.accepted)
        return false;
    OptionSet<BuiltinPrimitive> primitives = table.at(parameter.accepted).primitives;
    return primitives.contains(BuiltinPrimitive::NotPassed) && !primitives.contains(BuiltinPrimitive::Undefined);
}

// An operand that is passed as it is lies in the accepted type, a converted one in the converted type.
static bool liesInParameter(const TypingContext& context, const BuiltinOverload& overload, BuiltinOperand position, ValueID value)
{
    const BuiltinParameter* parameter = context.program.builtins().parameterFor(overload, position);
    if (!parameter)
        return false;
    TypeID type = context.typeIDOf(value);
    if (fitOf(context, type, parameter->accepted).fit == TypeFit::Inside)
        return true;
    return parameter->converted && fitOf(context, type, parameter->converted).fit == TypeFit::Inside;
}

static bool operandsLieInParameters(const TypingContext& context, const BuiltinOverload& overload, const BuiltinCall& call)
{
    if (!!call.receiver != !!overload.receiver)
        return false;
    if (call.receiver && !liesInParameter(context, overload, BuiltinOperand::receiver(), call.receiver))
        return false;
    for (unsigned index = 0; index < call.arguments.size(); ++index) {
        if (!liesInParameter(context, overload, BuiltinOperand::argument(index), call.arguments[index]))
            return false;
    }
    const BuiltinTable& table = context.program.builtins();
    return std::ranges::none_of(overload.parameters, [&](const BuiltinParameter& parameter) {
        bool isPassed = parameter.operand.argumentIndex() < call.arguments.size();
        return !isPassed && parameter.mode == BuiltinParameterMode::Required && !distinguishesOmittedFromUndefined(table, parameter);
    });
}

static bool passesOneArray(const Program& program, const Function& function, const BuiltinCall& call)
{
    if (call.receiver || call.arguments.size() != 1)
        return false;
    const Type& list = program.typeOf(function, call.arguments[0]);
    const ClassPart* part = list.isObject() ? list.singleBuiltinClass() : nullptr;
    return part && part->builtinClass == BuiltinClasses::Array;
}

bool isCallThatRequiredPassRemoves(const Program& program, const Value& call)
{
    if (call.opcode != Opcode::Call && call.opcode != Opcode::CallBuiltin)
        return false;
    if (ExpectedResultID expected = call.expectedResult(); expected && !program.at(expected).validation.isEmpty())
        return true;
    if (call.opcode == Opcode::Call)
        return false;

    const BuiltinTable& table = program.builtins();
    const Builtin& entry = table.at(table.at(call.overload()).builtin);
    if (!entry.flags.contains(BuiltinFlag::IsCompilerHelper) && entry.holder != Builtins::BunBundle)
        return false;
    if (call.builtinCallForm() != BuiltinCallForm::Whole)
        return true;
    // Which of its own helpers a host enters directly is known where there is a host.
    if (!BuiltinBindings::hasHost() && table.at(call.overload()).code.implementation.kind == BuiltinImplementationKind::HostCall)
        return false;
    return !BuiltinBindings::directFunctionNumberOf(call.overload(), call.argumentsOperands().size());
}

bool overloadHasForm(const BuiltinTable& table, BuiltinOverloadID id, BuiltinCallForm form)
{
    const auto& code = table.at(id).code;
    switch (form) {
    case BuiltinCallForm::Whole:
    case BuiltinCallForm::AnyReceiver:
        return true;
    case BuiltinCallForm::List:
        return code.listForm.has_value();
    case BuiltinCallForm::Fields:
        return code.fieldsForm.has_value();
    case BuiltinCallForm::Prefix:
        return code.prefixForm.has_value();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

template<typename Carrier>
static bool takesOperands(const TypingContext& context, const Carrier& carrier)
{
    const Program& program = context.program;
    const BuiltinTable& table = program.builtins();
    if (!table.contains(carrier.overload()))
        return false;

    BuiltinCall call = builtinCallOf(program, carrier);
    if (call.arguments.size() > maxArgumentCount || !overloadHasForm(table, call.overload, call.form))
        return false;

    switch (call.form) {
    case BuiltinCallForm::AnyReceiver:
        return true;
    case BuiltinCallForm::List:
        return passesOneArray(program, context.function, call);
    case BuiltinCallForm::Whole:
    case BuiltinCallForm::Fields:
    case BuiltinCallForm::Prefix:
        return operandsLieInParameters(context, table.at(call.overload), call);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool overloadTakesOperands(const TypingContext& context, const Value& call)
{
    if (call.opcode == Opcode::CallBuiltin && (call.builtinCallForm() == BuiltinCallForm::Prefix) != call.prefixLength().has_value())
        return false;
    return takesOperands(context, call);
}

bool overloadTakesOperands(const TypingContext& context, const Terminator& call)
{
    return takesOperands(context, call);
}

bool overloadTakesOperands(const Program& program, const Function& function, const Value& call)
{
    ScratchTypeArena scratch(program.types);
    return overloadTakesOperands(TypingContext { program, function, scratch.arena() }, call);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
