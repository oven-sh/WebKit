/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTEffectsDescription.h"

#if ENABLE(AOT)

#include "AOTBuiltinCalls.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsOfRules.h"
#include "AOTGuardAvailability.h"
#include "AOTIR.h"

namespace JSC { namespace AOT { namespace Typed {

namespace {

enum class OperandCode : uint8_t { IsDescribed, IsDeclaredByParameters };

} // anonymous namespace

static constexpr Effects heapCapabilityEffects = [] {
    Effects effects = declaredBoundOf(NamedEffectSet::Allocates);
    effects.merge(declaredBoundOf(NamedEffectSet::UsesRuntime));
    effects.merge(declaredBoundOf(NamedEffectSet::ResolvesRopes));
    return effects;
}();

static_assert(!heapCapabilityEffects.mayCallUnknownCode && !heapCapabilityEffects.maySuspend && !heapCapabilityEffects.mayBeTerminated);

Effects ownEffectsOf(const BuiltinOverload& overload)
{
    Effects effects = Effects::none();
    effects.reads = overload.effects.reads;
    effects.writes = overload.effects.writes;
    switch (overload.effects.capability) {
    case BuiltinCapability::None:
        break;
    case BuiltinCapability::Heap:
    case BuiltinCapability::JS:
        effects.merge(heapCapabilityEffects);
        break;
    }
    effects.mayThrow = !overload.throws() ? ThrowEffect(false) : overload.onlyPropagatesErrors() ? ThrowEffect::onlyForLackOfMemory() : ThrowEffect(true);
    return effects;
}

static void describeRuntimeFunction(const Program& program, RuntimeFunctionID id, EffectDescription& description)
{
    const RuntimeFunctionEntry* entry = findRuntimeFunction(program, id);
    if (!entry || !entry->function || !program.at(entry->function).hasBody()) {
        description.addUnknownCode();
        return;
    }
    description.addFunction(entry->function);
}

static constexpr Effects readsOfReachableObjects = [] {
    Effects effects = Effects::none();
    effects.reads = LocationMasks { perObjectHeaps };
    return effects;
}();

static bool isHelperDescribedByEntry(const Program& program, const BuiltinOverload& overload)
{
    return overload.flags.contains(BuiltinOverloadFlag::RunsOnlyOperandCode) && program.builtins().at(overload.builtin).holder == Builtins::CompilerHelpers;
}

static bool describeJavaScriptOf(const Program& program, const BuiltinOverload& overload, EffectDescription& description)
{
    if (RuntimeFunctionID libraryFunction = overload.libraryFunction()) {
        if (isHelperDescribedByEntry(program, overload)) {
            const RuntimeFunctionEntry* entry = findRuntimeFunction(program, libraryFunction);
            if (entry && entry->function)
                description.addSlowPathFunction(entry->function);
            description.addOwn(readsOfReachableObjects);
            return false;
        }
        describeRuntimeFunction(program, libraryFunction, description);
        return true;
    }
    if (isDescribedByModel(overload)) {
        description.addOwn(ownEffectsOf(overload));
        describeRuntimeFunction(program, overload.code.model, description);
        return true;
    }
    return false;
}

static void describeSlowPathOf(const Program& program, const BuiltinOverload& overload, EffectDescription& description)
{
    const RuntimeFunctionEntry* entry = findRuntimeFunction(program, overload.slowPathFunction());
    if (entry && entry->function)
        description.addSlowPathFunction(entry->function);
}

static OperandCode describeCodeOf(const Program& program, const BuiltinOverload& overload, EffectDescription& description)
{
    if (program.mayBeTrimmed(overload.builtin)) {
        description.addOwn(heapCapabilityEffects);
        description.addThrow();
    }
    if (overload.isUndeclared()) {
        description.addUnknownCode();
        return OperandCode::IsDescribed;
    }
    describeSlowPathOf(program, overload, description);
    if (describeJavaScriptOf(program, overload, description))
        return OperandCode::IsDescribed;

    description.addOwn(ownEffectsOf(overload));
    if (overload.effects.capability == BuiltinCapability::JS && !overload.flags.contains(BuiltinOverloadFlag::RunsOnlyOperandCode))
        description.addUnknownCode();
    return OperandCode::IsDeclaredByParameters;
}

static OperandBehavior behaviorOfArgument(const BuiltinOverload& overload, unsigned index)
{
    if (index > BuiltinOperand::maxArgumentIndex)
        return { };
    return declaredBehaviorOf(overload, BuiltinOperand::argument(index));
}

static bool mayFailOnOperands(const Program& program, const BuiltinOverload& overload, const Type* receiver, std::span<const TypeID> arguments)
{
    if (overload.receiver && mayFailOnOperand(program, *overload.receiver, receiver ? *receiver : program.at(TypeTable::anything)))
        return true;
    return std::ranges::any_of(overload.parameters, [&](const BuiltinParameter& parameter) {
        size_t first = parameter.operand.argumentIndex();
        bool takesRest = parameter.operand.kind() == BuiltinOperand::Kind::ArgumentsFrom;
        if (first >= arguments.size())
            return !takesRest && mayFailOnOperand(program, parameter, program.at(TypeTable::undefined));
        return std::ranges::any_of(arguments.subspan(first, takesRest ? std::dynamic_extent : 1), [&](TypeID argument) {
            return mayFailOnOperand(program, parameter, program.at(argument));
        });
    });
}

static void describeConversion(const BuiltinParameter& parameter, const Type& operand, EffectDescription& description)
{
    if (!readsCharactersOfString(parameter.conversion) || !(operand.primitives & Primitive::String))
        return;
    Effects effects = declaredBoundOf(NamedEffectSet::ResolvesRopes);
    effects.mayThrow = ThrowEffect::onlyForLackOfMemory();
    description.addOwn(effects);
}

static void describeOperandsOf(const Program& program, const BuiltinOverload& overload, TypeID receiverID, std::span<const TypeID> arguments, EffectDescription& description)
{
    const Type* receiver = receiverID ? &program.at(receiverID) : nullptr;
    for (const BuiltinParameter& parameter : overload.parameters) {
        size_t first = parameter.operand.argumentIndex();
        if (first >= arguments.size())
            continue;
        bool takesRest = parameter.operand.kind() == BuiltinOperand::Kind::ArgumentsFrom;
        for (TypeID argument : arguments.subspan(first, takesRest ? std::dynamic_extent : 1))
            describeConversion(parameter, program.at(argument), description);
    }
    if (receiver) {
        describeOperandCode(program, receiverID, declaredBehaviorOf(overload, BuiltinOperand::receiver()), description);
        if (receiver->mayBeUndefinedOrNull())
            description.addThrow();
    }
    for (unsigned index = 0; index < arguments.size(); ++index)
        describeOperandCode(program, arguments[index], behaviorOfArgument(overload, index), description);
    if (overload.onlyPropagatesErrors() && mayFailOnOperands(program, overload, receiver, arguments))
        description.addThrow();
}

static void describeArgumentsOfType(const Program& program, const BuiltinOverload& overload, const Type& argument, EffectDescription& description)
{
    for (const BuiltinParameter& parameter : overload.parameters) {
        describeOperandCode(program, argument, declaredBehaviorOf(overload, parameter.operand), description);
        describeConversion(parameter, argument, description);
    }
    if (!overload.parameters.empty())
        description.addThrow();
}

static void describeUntypedCall(const Program& program, const BuiltinOverload& overload, EffectDescription& description)
{
    switch (describeCodeOf(program, overload, description)) {
    case OperandCode::IsDescribed:
        return;
    case OperandCode::IsDeclaredByParameters:
        break;
    }
    describeOperandsOf(program, overload, overload.receiver ? TypeTable::anything : TypeID(), { }, description);
    describeArgumentsOfType(program, overload, program.at(TypeTable::anything), description);
}

void describeCallOf(const Program& program, BuiltinID builtin, BuiltinCallMode mode, EffectDescription& description)
{
    const BuiltinTable& table = program.builtins();
    bool hasOverload = false;
    for (const BuiltinOverload& overload : table.overloadsOf(table.canonicalID(builtin))) {
        if (overload.mode != mode)
            continue;
        hasOverload = true;
        describeUntypedCall(program, overload, description);
    }
    if (!hasOverload)
        description.addThrow();
}

void describeOverload(const Program& program, BuiltinOverloadID id, TypeID receiver, std::span<const TypeID> arguments, EffectDescription& description)
{
    const BuiltinOverload& overload = program.builtins().at(id);
    switch (describeCodeOf(program, overload, description)) {
    case OperandCode::IsDescribed:
        if (receiver && program.at(receiver).mayBeUndefinedOrNull())
            description.addThrow();
        return;
    case OperandCode::IsDeclaredByParameters:
        describeOperandsOf(program, overload, receiver, arguments, description);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void describeOverload(const Program& program, BuiltinOverloadID id, const Type& argument, EffectDescription& description)
{
    const BuiltinOverload& overload = program.builtins().at(id);
    switch (describeCodeOf(program, overload, description)) {
    case OperandCode::IsDescribed:
        return;
    case OperandCode::IsDeclaredByParameters:
        describeOperandCode(program, argument, behaviorOfArgument(overload, 0), description);
        if (overload.throws())
            description.addThrow();
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static const Type& provenElementTypeOf(const Program& program, const Type& list)
{
    const ClassPart* part = list.singleBuiltinClass();
    if (!part || !part->contentsAreProven)
        return program.at(TypeTable::anything);
    return program.at(*part->provenContentsAt(program.builtins(), ContentsSlot::Element));
}

static void describeListElements(const Program& program, const BuiltinOverload& overload, std::span<const TypeID> arguments, EffectDescription& description)
{
    if (arguments.size() != 1) {
        description.addUnknownCode();
        return;
    }
    describeArgumentsOfType(program, overload, provenElementTypeOf(program, program.at(arguments[0])), description);
}

static void describeListCall(const Program& program, const BuiltinOverload& overload, std::span<const TypeID> arguments, EffectDescription& description)
{
    description.addOwn(listReadEffects);
    description.addThrow();
    switch (describeCodeOf(program, overload, description)) {
    case OperandCode::IsDescribed:
        return;
    case OperandCode::IsDeclaredByParameters:
        describeListElements(program, overload, arguments, description);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isDeclaredByParameters(const BuiltinOverload& overload)
{
    return !overload.isUndeclared() && !overload.libraryFunction() && !isDescribedByModel(overload);
}

void describeOperandsOfCall(const Program& program, BuiltinID builtin, BuiltinCallMode mode, BuiltinCallForm form, TypeID receiver, std::span<const TypeID> arguments, EffectDescription& description)
{
    RELEASE_ASSERT(form == BuiltinCallForm::Whole || form == BuiltinCallForm::List);
    const BuiltinTable& table = program.builtins();
    for (const BuiltinOverload& overload : table.overloadsOf(table.canonicalID(builtin))) {
        if (overload.mode != mode || !isDeclaredByParameters(overload))
            continue;
        if (form == BuiltinCallForm::List)
            describeListElements(program, overload, arguments, description);
        else
            describeOperandsOf(program, overload, overload.receiver ? receiver : TypeID(), arguments, description);
    }
}

void describeOverload(const Program& program, BuiltinOverloadID id, BuiltinCallForm form, TypeID receiver, std::span<const TypeID> arguments, EffectDescription& description)
{
    const BuiltinOverload& overload = program.builtins().at(id);
    switch (form) {
    case BuiltinCallForm::Whole:
    case BuiltinCallForm::Fields:
    case BuiltinCallForm::Prefix:
        describeOverload(program, id, receiver, arguments, description);
        return;
    case BuiltinCallForm::AnyReceiver:
        describeCallOf(program, overload.builtin, overload.mode, description);
        describeOperandsOfCall(program, overload.builtin, overload.mode, BuiltinCallForm::Whole, receiver ? receiver : TypeTable::undefined, arguments, description);
        return;
    case BuiltinCallForm::List:
        describeListCall(program, overload, arguments, description);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static BuiltinOperandTypes provenOperandTypesOf(const Function& function, const BuiltinCall& call)
{
    BuiltinOperandTypes types;
    if (call.receiver)
        types.receiver = function.at(call.receiver).type;
    types.arguments.reserveInitialCapacity(call.arguments.size());
    for (ValueID argument : call.arguments)
        types.arguments.append(function.at(argument).type);
    return types;
}

static Effects dependencyReadsOf(const Program& program, Proof proof)
{
    Effects effects = Effects::none();
    for (const MemoryLocation& location : locationsBehindAllocationProof(proof))
        effects.reads.add(program, location);
    return effects;
}

void describeTypedResult(const Program& program, BuiltinOverloadID overload, ExpectedResultID expectedResult, EffectDescription& description)
{
    if (!expectedResult || program.builtins().at(overload).result.typedResultForm == TypedResultForm::None)
        return;
    for (Opcode row : { Opcode::JSONSourceOpen, Opcode::JSONSourceClose, Opcode::JSONSourceAbandon })
        description.addOwn(declaredBoundOf(row));
}

void describeBuiltinCall(const Program& program, const Function& function, const Value& value, const Effects&, EffectDescription& description)
{
    BuiltinCall call = builtinCallOf(program, value);
    BuiltinOperandTypes types = provenOperandTypesOf(function, call);
    describeOverload(program, call.overload, call.form, types.receiver, types.arguments.span(), description);
    if (value.needsPristineReceiver())
        description.addOwn(dependencyReadsOf(program, Proof::Pristine));
    describeTypedResult(program, call.overload, value.expectedResult(), description);
}

// The operands fitted no overload when the call was resolved, so the call counts as one that is handed anything. Here it is judged by the types that its operands have now.
EffectDescription describeUnmatchedCallByTypesOfOperands(const Program& program, const Function& function, const Value& value)
{
    BuiltinCall call = builtinCallOf(program, value);
    RELEASE_ASSERT(call.form == BuiltinCallForm::AnyReceiver);
    BuiltinOperandTypes types = provenOperandTypesOf(function, call);
    const BuiltinTable& table = program.builtins();
    const BuiltinOverload& named = table.at(call.overload);
    EffectDescription description;
    for (const BuiltinOverload& overload : table.overloadsOf(table.canonicalID(named.builtin))) {
        if (overload.mode == named.mode)
            describeCodeOf(program, overload, description);
    }
    description.addThrow();
    describeOperandsOfCall(program, named.builtin, named.mode, BuiltinCallForm::Whole, types.receiver ? types.receiver : TypeTable::undefined, types.arguments.span(), description);
    if (value.needsPristineReceiver())
        description.addOwn(dependencyReadsOf(program, Proof::Pristine));
    describeTypedResult(program, call.overload, value.expectedResult(), description);
    return description;
}

static bool isArraySearchForNonString(const Program& program, InlineBuiltin builtin, std::span<const TypeID> arguments)
{
    bool isArraySearch = builtin == InlineBuiltin::ArrayIndexOf || builtin == InlineBuiltin::ArrayIncludes;
    return isArraySearch && !arguments.empty() && !(program.at(arguments[0]).primitives & Primitive::AnyString);
}

static void describeInline(const Program& program, const Function& function, const BuiltinCall& call, InlineBuiltin builtin, EffectDescription& description)
{
    BuiltinOperandTypes types = provenOperandTypesOf(function, call);
    if (hasEffectsOfOverload(builtin)) {
        describeOverload(program, call.overload, call.form, types.receiver, types.arguments.span(), description);
        description.slowPathFunctions.clear();
        return;
    }

    const BuiltinOverload& overload = program.builtins().at(call.overload);
    Effects bound = declaredBoundOf(builtin);
    if (isArraySearchForNonString(program, builtin, types.arguments.span()))
        bound = declaredBoundOf(InlineBuiltin::ArrayAt);
    if (bound.mayThrow && overload.onlyPropagatesErrors())
        bound.mayThrow = ThrowEffect::onlyForLackOfMemory();
    describeBound(bound, description);
    switch (call.form) {
    case BuiltinCallForm::Whole:
    case BuiltinCallForm::Fields:
    case BuiltinCallForm::Prefix:
        describeOperandsOf(program, overload, types.receiver, types.arguments.span(), description);
        return;
    case BuiltinCallForm::AnyReceiver:
        description.addUnknownCode();
        return;
    case BuiltinCallForm::List:
        describeListElements(program, overload, types.arguments.span(), description);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void describeInlineBuiltin(const Program& program, const Function& function, const Value& value, const Effects&, EffectDescription& description)
{
    describeInline(program, function, builtinCallOf(program, value), value.inlineBuiltin(), description);
}

void describeInlineBuiltin(const Program& program, const Function& function, const Terminator& terminator, const Effects&, EffectDescription& description)
{
    describeInline(program, function, builtinCallOf(program, terminator), terminator.inlineBuiltin(), description);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
