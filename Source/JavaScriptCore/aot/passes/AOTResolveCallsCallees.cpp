/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTResolveCallsCallees.h"

#if ENABLE(AOT)

#include "AOTLayout.h"
#include "AOTValueIdentity.h"
#include <wtf/CommaPrinter.h>
#include <wtf/PrintStream.h>

namespace JSC { namespace AOT { namespace Typed {

static bool listsFunctions(const Type& type)
{
    return !type.functions.isEmpty() && !type.anyFunction && !type.anyObject;
}

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
        if (!listsFunctions(type))
            continue;
        if (!strongest || isStronger(program, type, program.typeOf(function, strongest)))
            strongest = id;
    }
    return strongest ? strongest : callee;
}

static FunctionList findExpectedFunctions(const Program& program, const Function& function, ValueID callee)
{
    for (ValueID id : CastChain(function, callee)) {
        TypeID expected = function.at(id).expected();
        if (expected && !program.at(expected).functions.isEmpty())
            return FunctionList(program.at(expected).functions.span());
    }
    return { };
}

static bool hasPartsOtherThanFunctions(const Type& type)
{
    return type.primitives || type.anyFunction || type.anyObject || type.layouts || !type.classes.isEmpty() || !type.builtins.isEmpty();
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
        result.hasOtherCallees = hasPartsOtherThanFunctions(rest);
        return result;
    }
    if (!admitsUnlistedFunctions(program, rest))
        return result;

    result.expected = findExpectedFunctions(program, function, callee);
    if (!result.expected.isEmpty()) {
        result.proof = CalleeProof::Test;
        result.hasOtherCallees = true;
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

// An incomplete stamp lists the functions that flow saw arrive at the operation, among whatever else arrives there.
static FunctionList findFunctionsSeenByFlow(const Program& program, const Value& operation)
{
    if (!Value::opcodesWithCallees.contains(operation.opcode) || !operation.callees())
        return { };
    return FunctionList(program.at(operation.callees()).functions.span());
}

CalleeCandidates calleeCandidatesOf(const Program& program, const Function& function, const Value& operation, ValueID callee)
{
    CalleeCandidates result = calleeCandidates(program, function, callee);
    if (result.proof != CalleeProof::Unknown || !admitsUnlistedFunctions(program, program.typeOf(function, result.value)))
        return result;

    result.expected = findFunctionsSeenByFlow(program, operation);
    if (!result.expected.isEmpty()) {
        result.proof = CalleeProof::Test;
        result.hasOtherCallees = true;
    }
    return result;
}

ValueID findClosureCreation(const Function& function, ValueID closure, FunctionID closureFunction)
{
    ValueID definition = sameObjectThroughCasts(function, closure);
    const Value& value = function.at(definition);
    if (value.opcode != Opcode::NewClosure || value.function() != closureFunction)
        return { };
    return definition;
}

static ASCIILiteral nameOf(CalleeProof proof)
{
    switch (proof) {
    case CalleeProof::Test:
        return "Test"_s;
    case CalleeProof::Unknown:
        return "Unknown"_s;
    case CalleeProof::Type:
        return "Type"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void dumpFunctions(PrintStream& out, const FunctionList& functions)
{
    CommaPrinter comma;
    out.print("[");
    for (FunctionID function : functions)
        out.print(comma, "F", function.index());
    out.print("]");
}

void CalleeCandidates::dump(PrintStream& out) const
{
    out.print("proof=", nameOf(proof), " proven=");
    dumpFunctions(out, proven);
    out.print(" expected=");
    dumpFunctions(out, expected);
    if (hasOtherCallees)
        out.print(" others");
    if (mayBeNullish)
        out.print(" nullish");
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
