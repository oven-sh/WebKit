/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTResolveCallsCallees.h"

#if ENABLE(AOT)

#include "AOTFlowResults.h"
#include "AOTValueIdentity.h"
#include <algorithm>
#include <wtf/CommaPrinter.h>
#include <wtf/PrintStream.h>

namespace JSC { namespace AOT { namespace Typed {

static bool listsFunctions(const Type& type)
{
    return !type.functions.isEmpty() && !type.anyFunction && !type.anyObject;
}

static bool admitsUnlistedFunctions(const Type& type)
{
    return type.anyFunction || type.anyObject;
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

static FunctionList findPredictedFunctions(const Program& program, const Function& function, ValueID callee)
{
    for (ValueID id : CastChain(function, callee)) {
        TypeID prediction = function.at(id).prediction();
        if (prediction && !program.at(prediction).functions.isEmpty())
            return FunctionList(program.at(prediction).functions.span());
    }
    return { };
}

// SplitCoroutines gives the generic version of a coroutine the role and the record of a ramp, so flow's record is asked.
FunctionID closureFunctionOf(const Program& program, const FlowResults& flow, FunctionID version)
{
    FunctionID source = program.at(version).derivation.source;
    bool isVersionOfFlow = source && (flow.function(source).genericVersion == version || program.at(version).role == FunctionRole::CallSiteVersion);
    return isVersionOfFlow ? source : version;
}

static FunctionList closureFunctionsOf(const Program& program, const FlowResults& flow, const CalleeSet& stamp)
{
    Vector<FunctionID, 8> functions(stamp.functions.size(), [&](size_t index) {
        return closureFunctionOf(program, flow, stamp.functions[index]);
    });
    return FunctionList(functions.span());
}

// The run time creates the one closure of a function that implements a built-in, and a test for the built-in is the test for that closure.
static bool implementsBuiltin(const Program& program, FunctionID function)
{
    const RuntimeFunctionEntry* entry = findRuntimeFunction(program, program.at(function).enclosingRuntimeFunction);
    return entry && entry->function == function;
}

static FunctionList expectedClosureFunctionsOf(const Program& program, const FlowResults& flow, const CalleeSet& stamp)
{
    FunctionList functions = closureFunctionsOf(program, flow, stamp);
    if (stamp.builtins.isEmpty())
        return functions;

    FunctionList result;
    for (FunctionID function : functions) {
        if (!implementsBuiltin(program, function))
            result.add(function);
    }
    return result;
}

static FunctionList withoutUnlinkedFunctions(const Program& program, const FlowResults& flow, std::span<const FunctionID> functions, const CalleeSet& stamp)
{
    FunctionList linked = closureFunctionsOf(program, flow, stamp);
    FunctionList result;
    for (FunctionID function : functions) {
        // Flow links the functions it read, so a stamp says nothing about a function that a pass derived later.
        bool isKnownToFlow = function.index() < program.numberOfFunctionsAsRead();
        if (!isKnownToFlow || linked.contains(function))
            result.add(function);
    }
    return result;
}

static bool hasPartsOtherThanFunctions(const Type& type)
{
    return type.primitives || admitsUnlistedFunctions(type) || type.layouts || !type.classes.isEmpty() || !type.builtins.isEmpty();
}

CalleeCandidates calleeCandidates(const Program& program, const FlowResults& flow, const Function& function, ValueID callee, CalleeSetID stamp, AccessSiteID site)
{
    CalleeCandidates result;
    result.value = findStrongestLink(program, function, callee);

    const Type& type = program.typeOf(function, result.value);
    result.mayBeNullish = type.mayBeUndefinedOrNull();
    Type rest = type.withoutNullish();
    const CalleeSet* stamped = stamp ? &program.at(stamp) : nullptr;

    if (!rest.functions.isEmpty()) {
        result.proof = CalleeProof::Type;
        bool isStampComplete = stamped && stamped->isComplete;
        result.proven = isStampComplete ? withoutUnlinkedFunctions(program, flow, rest.functions.span(), *stamped) : FunctionList(rest.functions.span());
        // A stamp proves nothing alone: a function of the type that only the stamp excludes stays a possible callee.
        result.hasOtherCallees = hasPartsOtherThanFunctions(rest) || result.proven.size() < rest.functions.size();
    }

    // Beside the unknown object a value lists whatever has escaped. Flow links at the call what it saw arrive there as a callee, and only that is expected.
    FunctionList expected = findPredictedFunctions(program, function, callee);
    if (stamped)
        expected = withoutUnlinkedFunctions(program, flow, expected.span(), *stamped);
    if (expected.isEmpty() && !result.isProven() && stamped && !flow.mayCallBoundFunction(site))
        expected = expectedClosureFunctionsOf(program, flow, *stamped);
    if (result.isProven()) {
        expected.intersect(result.proven);
        if (expected.size() >= result.proven.size())
            expected.clear();
    } else if (!admitsUnlistedFunctions(rest))
        expected.clear();
    result.predicted = WTF::move(expected);
    return result;
}

void requireArmFor(CalleeCandidates& candidates, FunctionID closureFunction)
{
    if (candidates.proven.contains(closureFunction))
        return;
    candidates.proven.add(closureFunction);
    if (candidates.isProven())
        return;
    candidates.proof = CalleeProof::Test;
    candidates.hasOtherCallees = true;
}

void requireArmsForCallSiteVersions(const Program& program, CalleeCandidates& candidates, CalleeSetID stamp)
{
    if (!stamp)
        return;
    for (FunctionID version : program.at(stamp).functions) {
        const Function& function = program.at(version);
        if (function.role == FunctionRole::CallSiteVersion)
            requireArmFor(candidates, function.derivation.source);
    }
}

static FunctionID findCallSiteVersion(const Program& program, FunctionID closureFunction, CalleeSetID stamp)
{
    if (!stamp)
        return { };
    for (FunctionID version : program.at(stamp).functions) {
        const Function& function = program.at(version);
        if (function.role == FunctionRole::CallSiteVersion && function.derivation.source == closureFunction)
            return version;
    }
    return { };
}

FunctionID versionToEnter(const Program& program, const FlowResults& flow, FunctionID closureFunction, CalleeSetID stamp)
{
    // Which version runs decides which layouts its allocations name, and the types at the call rest on that.
    if (FunctionID callSiteVersion = findCallSiteVersion(program, closureFunction, stamp))
        return callSiteVersion;
    // A derived function answers with the record of its source, whose generic version is no version of the derived function.
    FunctionID genericVersion = flow.function(closureFunction).genericVersion;
    if (!genericVersion || closureFunctionOf(program, flow, genericVersion) != closureFunction)
        return closureFunction;
    if (stamp && std::ranges::binary_search(program.at(stamp).functions, closureFunction))
        return closureFunction;
    return genericVersion;
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
    out.print(" predicted=");
    dumpFunctions(out, predicted);
    if (hasOtherCallees)
        out.print(" others");
    if (mayBeNullish)
        out.print(" nullish");
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
