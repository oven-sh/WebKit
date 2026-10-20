/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTArrayChains.h"

#if ENABLE(AOT)

#include "AOTBuiltinCalls.h"
#include "AOTBuiltinTable.h"
#include "AOTIRProgram.h"
#include "AOTValueIdentity.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

std::span<const ChainMethod> chainMethods()
{
    static constexpr std::array<ChainMethod, 12> methods { {
        { Builtins::ArrayPrototypeSlice, ChainMethodRole::Range, ChainLinkKind::None, ChainEndKind::Array, false, false },
        { Builtins::ArrayPrototypeMap, ChainMethodRole::Link, ChainLinkKind::Map, ChainEndKind::Array, true, false },
        { Builtins::ArrayPrototypeFilter, ChainMethodRole::Link, ChainLinkKind::Filter, ChainEndKind::Array, true, false },
        { Builtins::ArrayPrototypeFlatMap, ChainMethodRole::End, ChainLinkKind::None, ChainEndKind::FlatMap, true, false },
        { Builtins::ArrayPrototypeForEach, ChainMethodRole::End, ChainLinkKind::None, ChainEndKind::ForEach, true, false },
        { Builtins::ArrayPrototypeReduce, ChainMethodRole::End, ChainLinkKind::None, ChainEndKind::Reduce, true, false },
        { Builtins::ArrayPrototypeSome, ChainMethodRole::End, ChainLinkKind::None, ChainEndKind::Some, true, true },
        { Builtins::ArrayPrototypeEvery, ChainMethodRole::End, ChainLinkKind::None, ChainEndKind::Every, true, true },
        { Builtins::ArrayPrototypeFind, ChainMethodRole::End, ChainLinkKind::None, ChainEndKind::Find, true, true },
        { Builtins::ArrayPrototypeFindIndex, ChainMethodRole::End, ChainLinkKind::None, ChainEndKind::FindIndex, true, true },
        { Builtins::ArrayPrototypeJoin, ChainMethodRole::End, ChainLinkKind::None, ChainEndKind::Join, false, false },
        { Builtins::ArrayPrototypeLength, ChainMethodRole::End, ChainLinkKind::None, ChainEndKind::Length, false, false },
    } };
    return methods;
}

const ChainMethod* findChainMethod(BuiltinID id)
{
    std::span<const ChainMethod> methods = chainMethods();
    auto method = std::ranges::find(methods, id, &ChainMethod::builtin);
    return method == methods.end() ? nullptr : &*method;
}

static bool isCallOrRead(BuiltinCallMode mode)
{
    switch (mode) {
    case BuiltinCallMode::Call:
    case BuiltinCallMode::Get:
        return true;
    case BuiltinCallMode::Construct:
    case BuiltinCallMode::Set:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<ArrayMethodCall> builtinMethodCallOf(const Program& program, const Function& function, ValueID id)
{
    const BuiltinTable& table = program.builtins();
    BuiltinCall call = builtinCallOf(program, function.at(id));
    if (call.form != BuiltinCallForm::Whole || !call.receiver)
        return std::nullopt;

    const BuiltinOverload& overload = table.at(call.overload);
    if (!isCallOrRead(overload.mode))
        return std::nullopt;
    const ChainMethod* method = findChainMethod(table.canonicalID(overload.builtin));
    if (!method)
        return std::nullopt;
    return ArrayMethodCall { method, id, call.receiver, Vector<ValueID, 2>(call.arguments) };
}

static const ChainMethod* findMethodImplementedBy(const Program& program, FunctionID libraryFunction)
{
    RuntimeFunctionID runtimeFunction = program.at(libraryFunction).enclosingRuntimeFunction;
    const RuntimeFunctionEntry* entry = findRuntimeFunction(program, runtimeFunction);
    if (!entry || entry->function != libraryFunction)
        return nullptr;

    const BuiltinTable& table = program.builtins();
    for (BuiltinID builtin : table.at(runtimeFunction).implements) {
        if (const ChainMethod* method = findChainMethod(table.canonicalID(builtin)))
            return method;
    }
    return nullptr;
}

static std::optional<Vector<ValueID, 2>> findPassedArguments(const Program& program, const Function& function, const Function& target, std::span<const ValueID> operands)
{
    if (target.has(FunctionFlags::HasRestParameter))
        return std::nullopt;

    Vector<ValueID, 2> arguments(operands.subspan(target.firstParameterIndex(), target.numberOfPositionalParameters()));
    if (std::optional<unsigned> hiddenRest = target.hiddenRestParameterIndex()) {
        const Value& rest = function.at(sameObjectThroughCasts(function, operands[*hiddenRest]));
        if (rest.opcode != Opcode::NewArray || !rest.holes().empty() || rest.capacityOperand())
            return std::nullopt;
        arguments.append(rest.elementsOperands());
    }
    if (std::optional<unsigned> argumentCount = target.argumentCountParameterIndex()) {
        std::optional<int64_t> count = integerConstantThroughCasts(program, function, operands[*argumentCount]);
        if (!count || *count < 0 || static_cast<uint64_t>(*count) > arguments.size())
            return std::nullopt;
        arguments.shrink(static_cast<size_t>(*count));
    }
    return arguments;
}

static std::optional<ArrayMethodCall> libraryMethodCallOf(const Program& program, const Function& function, ValueID id)
{
    const Value& value = function.at(id);
    FunctionID callee = value.function();
    const ChainMethod* method = findMethodImplementedBy(program, callee);
    if (!method)
        return std::nullopt;

    const Function& target = program.at(callee);
    std::span<const ValueID> operands = value.argumentsOperands();
    RELEASE_ASSERT(operands.size() == target.numberOfEntryParameters());
    std::optional<unsigned> thisParameter = target.thisParameterIndex();
    if (!thisParameter)
        return std::nullopt;

    std::optional<Vector<ValueID, 2>> arguments = findPassedArguments(program, function, target, operands);
    if (!arguments)
        return std::nullopt;
    return ArrayMethodCall { method, id, operands[*thisParameter], WTF::move(*arguments) };
}

std::optional<ArrayMethodCall> arrayMethodCallOf(const Program& program, const Function& function, ValueID id)
{
    const Value& value = function.at(id);
    if (value.opcode == Opcode::GetArrayLength)
        return ArrayMethodCall { findChainMethod(Builtins::ArrayPrototypeLength), id, value.arrayOperand(), { } };
    if (builtinCallOpcodes.contains(value.opcode))
        return builtinMethodCallOf(program, function, id);
    if (value.opcode == Opcode::CallDirect)
        return libraryMethodCallOf(program, function, id);
    return std::nullopt;
}

FunctionID findChainTraversal(const Program& program)
{
    std::span<const BuiltinOverload> overloads = program.builtins().overloadsOf(Builtins::CompilerHelpersTraverseArray);
    RELEASE_ASSERT(!overloads.empty());
    const BuiltinImplementation& implementation = overloads.front().code.implementation;
    RELEASE_ASSERT(implementation.kind == BuiltinImplementationKind::Compiled);

    const RuntimeFunctionEntry* entry = findRuntimeFunction(program, implementation.function);
    if (!entry || !entry->function || !program.at(entry->function).hasBody())
        return { };
    return entry->function;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
