/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPromisedResults.h"

#if ENABLE(AOT)

#include "AOTChooseCallingConventions.h"
#include "AOTVector.h"

namespace JSC { namespace AOT { namespace Typed {

static constexpr size_t functionsPerShare = 256;

static TypeID findTypeToPromise(const Program& program, const CallingConventions& conventions, FunctionID id)
{
    const Function& function = program.at(id);
    if (!function.hasBody() || function.results.size() != 1 || !function.hasOrdinaryFrame() || function.role == FunctionRole::StubBody)
        return { };
    const DeclaredType& declared = function.results[0];
    if (!declared.isExpected())
        return { };
    const Type& type = program.at(declared.expected().predicted());
    if (!type.isJavaScriptValue() || type.isAnything() || type.isNever())
        return { };
    // A test can name it as it is.
    if (type.withoutClassProofs().withUntestableBuiltinsWidened() != type)
        return { };
    if (conventions.resultRepresentation(program, id) != Representation::JSValue || conventions.of(program, id).spillsArgumentRegisters)
        return { };
    return declared.expected().predicted();
}

static void forEachTailCallee(const Function& function, NOESCAPE const Invocable<void(FunctionID)> auto& functor)
{
    for (const Block& block : function.blocks) {
        if (block.terminator.kind != TerminatorKind::ReturnCall)
            continue;
        const Value& call = function.at(block.terminator.callOperand());
        functor(call.opcode == Opcode::CallDirect ? call.function() : FunctionID());
    }
}

// Its callers take the type of the result as a fact, so it has no misfits.
static bool hasProvenResult(const Function& function)
{
    return function.results.size() == 1 && !function.results[0].isExpected();
}

struct TailCall {
    FunctionID caller;
    FunctionID callee;
};

static bool mayKeepPromise(FunctionID id, const Function& function, const Type& promised, Vector<TailCall>& tailCalls)
{
    for (const Block& block : function.blocks) {
        if (block.terminator.kind == TerminatorKind::Return && !block.terminator.valueOperand() && !(promised.primitives & Primitive::Undefined))
            return false;
    }
    size_t numberOfTailCallsBefore = tailCalls.size();
    bool result = true;
    forEachTailCallee(function, [&](FunctionID callee) {
        result &= !!callee;
        tailCalls.append({ id, callee });
    });
    if (!result)
        tailCalls.shrink(numberOfTailCallsBefore);
    return result;
}

static bool keepsPromise(const Program& program, FunctionID callee, const Type& promised, std::span<const TypeID> types)
{
    TypeID returnedByCallee = types[callee.index()];
    if (!returnedByCallee && hasProvenResult(program.at(callee)))
        returnedByCallee = *program.at(callee).results[0].established();
    return returnedByCallee && program.at(returnedByCallee).isSubsetOf(promised, program);
}

// They have no code: the value is their operand under another type.
static bool handsOnBoxedOperand(const Value& value)
{
    return (value.opcode == Opcode::AssumeType || isCastThatProvesNothing(value.opcode)) && value.representation == Representation::JSValue;
}

static ValueID findCallWithTestedResult(const Function& function, const Block& block)
{
    if (block.handler || block.terminator.kind != TerminatorKind::TestAndBranch || block.values.size() < 2)
        return { };
    ValueID testID = block.values.last();
    const Value& test = function.at(testID);
    if (block.terminator.conditionOperand() != testID || test.opcode != Opcode::TypeTest)
        return { };
    ValueID tested = test.valueOperand();
    uint32_t index = block.values.size() - 2;
    while (index && block.values[index] == tested && handsOnBoxedOperand(function.at(tested))) {
        tested = function.at(tested).valueOperand();
        --index;
    }
    const Value& call = function.at(tested);
    if (block.values[index] != tested || call.opcode != Opcode::CallDirect || call.isConstruction() || call.representation != Representation::JSValue)
        return { };
    return tested;
}

ValueID PromisedResults::findCallThatStandsForTest(const Program& program, const Function& function, const Block& block) const
{
    ValueID call = findCallWithTestedResult(function, block);
    if (!call)
        return { };
    TypeID promised = of(function.at(call).function());
    if (!promised || !program.at(promised).isSubsetOf(program.at(function.at(block.values.last()).testedType()), program))
        return { };
    return call;
}

// A promise that no caller relies on buys nothing, and costs the tests in the function.
static void dropPromisesNobodyReliesOn(const Program& program, const ParallelLoops& loops, Vector<TypeID>& types)
{
    PromisedResults promises(Vector<TypeID>(types), { });
    BitVector isReliedOn(program.functions.size());
    Vector<FunctionID> worklist;
    auto relyOn = [&](FunctionID function) {
        if (function && !isReliedOn.quickSet(function.index()))
            worklist.append(function);
    };
    Vector<Vector<FunctionID>> calleesOfShare((program.functions.size() + functionsPerShare - 1) / functionsPerShare);
    forEachIndexInParallel(loops, calleesOfShare.size(), [&](unsigned share) {
        for (const Function& function : program.functions.span().subspan(share * functionsPerShare, std::min<size_t>(functionsPerShare, program.functions.size() - share * functionsPerShare))) {
            for (const Block& block : function.blocks) {
                if (ValueID call = promises.findCallThatStandsForTest(program, function, block))
                    calleesOfShare[share].append(function.at(call).function());
            }
        }
    });
    for (const Vector<FunctionID>& callees : calleesOfShare) {
        for (FunctionID callee : callees)
            relyOn(callee);
    }
    while (!worklist.isEmpty())
        forEachTailCallee(program.at(worklist.takeLast()), relyOn);
    for (uint32_t index = 0; index < types.size(); ++index) {
        if (!isReliedOn.quickGet(index))
            types[index] = { };
    }
}

static BitVector findFunctionsWithExactMisfits(const Program& program, std::span<const TypeID> types)
{
    BitVector isExact(types.size());
    for (uint32_t index = 0; index < types.size(); ++index)
        isExact.quickSet(index, !!types[index]);
    for (bool changed = true; std::exchange(changed, false);) {
        for (size_t index : isExact) {
            bool staysExact = true;
            forEachTailCallee(program.functions[index], [&](FunctionID callee) {
                if (types[callee.index()])
                    staysExact &= types[callee.index()] == types[index] && isExact.quickGet(callee.index());
            });
            if (staysExact)
                continue;
            isExact.quickClear(index);
            changed = true;
        }
    }
    return isExact;
}

PromisedResults findPromisedResults(const Program& program, const CallingConventions& conventions, const ParallelLoops& loops)
{
    Vector<TypeID> types(program.functions.size());
    Vector<Vector<TailCall>> tailCallsOfShare((program.functions.size() + functionsPerShare - 1) / functionsPerShare);
    forEachIndexInParallel(loops, tailCallsOfShare.size(), [&](unsigned share) {
        for (uint32_t index = share * functionsPerShare; index < std::min<size_t>(program.functions.size(), (share + 1) * functionsPerShare); ++index) {
            TypeID type = findTypeToPromise(program, conventions, FunctionID(index));
            if (type && mayKeepPromise(FunctionID(index), program.functions[index], program.at(type), tailCallsOfShare[share]))
                types[index] = type;
        }
    });

    for (bool changed = true; std::exchange(changed, false);) {
        for (const Vector<TailCall>& tailCalls : tailCallsOfShare) {
            for (const TailCall& call : tailCalls) {
                TypeID& type = types[call.caller.index()];
                if (!type || keepsPromise(program, call.callee, program.at(type), types.span()))
                    continue;
                type = { };
                changed = true;
            }
        }
    }
    dropPromisesNobodyReliesOn(program, loops, types);
    BitVector functionsWithExactMisfits = findFunctionsWithExactMisfits(program, types.span());
    return PromisedResults(WTF::move(types), WTF::move(functionsWithExactMisfits));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
