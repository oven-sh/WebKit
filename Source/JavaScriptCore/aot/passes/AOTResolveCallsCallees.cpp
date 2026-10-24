/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTResolveCallsCallees.h"

#if ENABLE(AOT)

#include "AOTLayout.h"
#include "AOTValueIdentity.h"

namespace JSC { namespace AOT { namespace Typed {

bool admitsUnlistedFunctions(const Program& program, const Type& type)
{
    return type.anyFunction || type.anyObject || hasMembersWithoutLayout(program, type);
}

static bool isStronger(const Program& program, const Type& candidate, const Type& current)
{
    if (candidate.functions.size() != current.functions.size())
        return candidate.functions.size() < current.functions.size();
    return candidate != current && candidate.isSubsetOf(current, program);
}

static ValueID findStrongestLink(const Program& program, const Function& function, ValueID callee)
{
    ValueID strongest;
    for (ValueID id : CastChain(function, callee)) {
        const Type& type = program.typeOf(function, id);
        if (type.functions.isEmpty() || type.anyFunction || type.anyObject)
            continue;
        if (!strongest || isStronger(program, type, program.typeOf(function, strongest)))
            strongest = id;
    }
    return strongest ? strongest : callee;
}

static void expectFunctions(CalleeCandidates& candidates, std::span<const FunctionID> functions)
{
    candidates.expected = FunctionList(functions);
    if (functions.empty())
        return;
    candidates.proof = CalleeProof::Test;
    candidates.hasOtherCallees = true;
}

CalleeCandidates calleeCandidates(const Program& program, const Function& function, ValueID callee)
{
    CalleeCandidates result;
    result.value = findStrongestLink(program, function, callee);

    const Type& type = program.typeOf(function, result.value);
    result.mayBeNullish = type.mayBeUndefinedOrNull();
    Type rest = type.withoutNullish();
    if (!rest.functions.isEmpty()) {
        result.proof = CalleeProof::Type;
        result.proven = FunctionList(rest.functions.span());
        result.hasOtherCallees = rest.primitives || rest.anyFunction || rest.anyObject || rest.layouts || !rest.classes.isEmpty() || !rest.builtins.isEmpty();
        return result;
    }
    if (!admitsUnlistedFunctions(program, rest))
        return result;

    for (ValueID id : CastChain(function, callee)) {
        TypeID expected = function.at(id).expected();
        if (expected && !program.at(expected).functions.isEmpty()) {
            expectFunctions(result, program.at(expected).functions.span());
            break;
        }
    }
    return result;
}

std::optional<OnlyCallee> findOnlyCallee(const Program& program, const Function& function, ValueID callee)
{
    ValueID value = findStrongestLink(program, function, callee);
    const Type& type = program.typeOf(function, value);
    FunctionID only = type.singleFunction();
    if (!only || type.mayBeUndefinedOrNull())
        return std::nullopt;
    return OnlyCallee { value, only };
}

CalleeCandidates calleeCandidatesOf(const Program& program, const Function& function, const Value& operation, ValueID callee)
{
    CalleeCandidates result = calleeCandidates(program, function, callee);
    if (result.proof != CalleeProof::Unknown || !admitsUnlistedFunctions(program, program.typeOf(function, result.value)))
        return result;
    // An incomplete stamp lists the functions that flow saw arrive at the operation, among whatever else arrives there.
    if (Value::opcodesWithCallees.contains(operation.opcode) && operation.callees())
        expectFunctions(result, program.at(operation.callees()).functions.span());
    return result;
}

ValueID findClosureCreation(const Function& function, ValueID closure, FunctionID closureFunction)
{
    ValueID definition = sameObjectThroughCasts(function, closure);
    const Value& value = function.at(definition);
    return value.opcode == Opcode::NewClosure && value.function() == closureFunction ? definition : ValueID();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
