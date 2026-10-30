/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowResultsInternal.h"

#if ENABLE(AOT)

#include "AOTBlockCycles.h"
#include "AOTForwardedCalls.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include <atomic>
#include <wtf/Atomics.h>

namespace JSC { namespace AOT { namespace Typed {

// n calls may name one set of n callees, so a set is handed over as it is: whoever goes through it can do so once.
static void forEachFunctionOrSetEnteredBy(const FlowBodyUpdate& update, const Function& function, ValueID id, NOESCAPE const Invocable<void(FunctionID)> auto& enterFunction, NOESCAPE const Invocable<void(CalleeSetID)> auto& enterSet)
{
    const Value& value = function.at(id);
    if (value.opcode == Opcode::CallDirect) {
        enterFunction(value.function());
        return;
    }
    if (!PayloadWord::callees(value.opcode))
        return;
    if (const FlowCallStamp* stamp = findByValue(update.calls.span(), id, &FlowCallStamp::value))
        enterSet(stamp->callees);
}

static Vector<uint32_t> countEnteringOperations(const ValueFlowResultInputs& inputs, const FlowResultTables& tables, const FlowApplication& application)
{
    const Program& program = inputs.program;
    Vector<uint32_t> counts(FillWith { }, program.functions.size(), 0u);
    forEachIndexInParallel(inputs.loops(), program.functions.size(), [&](unsigned index) {
        const Function& function = program.functions[index];
        if (!inputs.isReached(FunctionID(index)) && !isUnsolvedCode(inputs, tables, FunctionID(index)))
            return;
        Vector<CalleeSetID, 8> setsEntered;
        if (!inputs.bodies().holdsAnyOf(FunctionID(index), notedOperations({ ValueFlowNotedOperation::CallDirect }))) {
            for (const FlowCallStamp& stamp : application.bodies[index].calls) {
                if (PayloadWord::callees(function.at(stamp.value).opcode))
                    setsEntered.append(stamp.callees);
            }
        } else {
            for (const Block& block : function.blocks) {
                for (ValueID id : block.values) {
                    forEachFunctionOrSetEnteredBy(application.bodies[index], function, id, [&](FunctionID callee) {
                        AtomicRef(counts[callee.index()]).fetch_add(1, std::memory_order_relaxed);
                    }, [&](CalleeSetID set) {
                        setsEntered.append(set);
                    });
                }
            }
        }
        AOT::sort(setsEntered);
        for (size_t first = 0; first < setsEntered.size();) {
            size_t end = first + 1;
            while (end < setsEntered.size() && setsEntered[end] == setsEntered[first])
                ++end;
            for (FunctionID callee : program.at(setsEntered[first]).followed().value())
                AtomicRef(counts[callee.index()]).fetch_add(static_cast<uint32_t>(end - first), std::memory_order_relaxed);
            first = end;
        }
    });
    return counts;
}

static bool isCalledByForwardingBuiltinItself(const ValueFlowResultInputs& inputs, const FlowApplication& application, ValueSetMaker& maker, const FlowIndirectCall& call)
{
    if (!call.site)
        return false;
    ValueID site = application.accessSites.valueOfSite[call.site.index()];
    std::span<const ValueFlowStatement> statements = inputs.bodies().statementsOf(call.caller.index());
    auto statement = std::ranges::find_if(statements, [&](const ValueFlowStatement& candidate) {
        return candidate.kind == ValueFlowStatementKind::Operation && candidate.opcode == Opcode::Call && candidate.site == site;
    });
    if (statement == statements.end())
        return false;
    const FlowCallStamp* stamp = findByValue(application.bodies[call.caller.index()].calls.span(), statement->site, &FlowCallStamp::value);
    if (!stamp)
        return false;

    static_assert(!OperandIndex::Call::callee && OperandIndex::Call::thisValue == 1);
    constexpr size_t firstArgument = 2;
    std::span<const ValueFlowOperand> operands = inputs.bodies().operandsOf(*statement);
    std::optional<ProvenValue<Callees>> callees = inputs.program.at(stamp->callees).all();
    if (!callees)
        return false;
    ValueSetID called = ValueSets::nothing;
    for (BuiltinID builtin : callees->value().builtins) {
        for (const BuiltinOverload& overload : inputs.program.builtins().overloadsOf(builtin)) {
            std::optional<BuiltinOperand> operand = operandCalledBy(overload);
            if (!operand)
                return false;
            size_t index = operand->isReceiver() ? OperandIndex::Call::thisValue : firstArgument + operand->argumentIndex();
            if (index >= operands.size())
                return false;
            called = maker.unite(called, inputs.possibleValuesOf(maker, operands[index]));
        }
    }

    const ValueFlowUniverse& universe = inputs.universe();
    const ValueSets& sets = inputs.sets();
    if (ValueSets::isComplement(called) || !sets.contains(called, universe.closureOf(call.callee)))
        return false;
    bool holdsPlaceholder = sets.contains(called, ValueFlowUniverse::unknownObject()) || sets.contains(called, ValueFlowUniverse::untrackedObject());
    return !holdsPlaceholder && !sets.meet(called, universe.builtinFunctions()) && !sets.meet(called, universe.otherCallables());
}

void computeExecutionResults(const ValueFlowResultInputs& inputs, const ValueFlowStampedCalls& calls, FlowResultTables& tables)
{
    const Program& program = inputs.program;
    const FlowApplication& application = *tables.application;
    Vector<FunctionFlow>& flows = tables.functions.functions;
    Vector<uint32_t> numberOfEnteringOperations = countEnteringOperations(inputs, tables, application);
    std::span<const FlowIndirectCall> indirectCalls = tables.functions.indirectCalls.span();
    Vector<uint32_t> numberOfIndirectCalls(FillWith { }, program.functions.size(), 0u);
    for (const FlowIndirectCall& call : indirectCalls)
        ++numberOfIndirectCalls[call.callee.index()];
    BorrowedValueSetMaker borrowedMaker(inputs.makerPool());
    ValueSetMaker& maker = borrowedMaker.get();

    Vector<FunctionID> worklist;
    auto runsAtMostOnce = [&](FunctionID function) {
        if (!inputs.build.isEnabled(FlowResultKind::ExecutionCounts, function.index()))
            return;
        flows[function.index()].executions = ExecutionCount::AtMostOnce;
        worklist.append(function);
    };
    for (const Module& module : program.modules) {
        FunctionID body = module.body;
        if (!body || !inputs.isReached(body))
            continue;
        ValueFlowObjectID closure = inputs.universe().closureOf(body);
        bool onlyLoaderEnters = !numberOfEnteringOperations[body.index()] && !calls.functionsEnteredWithoutStamp.get(body.index()) && !inputs.hasEscaped(closure) && !inputs.isLost(closure);
        if (onlyLoaderEnters && isEvaluatedAtMostOnceOf(module.kind) && !module.flags.contains(ModuleFlags::RunsAgain))
            runsAtMostOnce(body);
    }

    // What is asked of a callee changes only when the answer is yes, and then it is not asked again: a set that was gone through once has nothing left to find.
    BitVector setsGoneThrough;
    while (!worklist.isEmpty()) {
        FunctionID caller = worklist.takeLast();
        const Function& function = program.at(caller);
        CFG cfg(function);
        BlockCycles cycles(cfg);
        auto enter = [&](FunctionID callee) {
            const FunctionFlow& flow = flows[callee.index()];
            if (numberOfEnteringOperations[callee.index()] == 1 && !calls.functionsEnteredWithoutStamp.get(callee.index()) && flow.callers == CallerState::AllKnown && flow.executions == ExecutionCount::Unknown)
                runsAtMostOnce(callee);
        };
        for (BlockID block : cfg.reversePostOrder()) {
            if (cycles.isInCycle(block))
                continue;
            for (ValueID id : function.at(block).values) {
                forEachFunctionOrSetEnteredBy(application.bodies[caller.index()], function, id, enter, [&](CalleeSetID set) {
                    if (setsGoneThrough.set(set.index()))
                        return;
                    for (FunctionID callee : program.at(set).followed().value())
                        enter(callee);
                });
            }
        }
        for (const FlowIndirectCall& call : std::ranges::equal_range(indirectCalls, caller, { }, &FlowIndirectCall::caller)) {
            if (call.kind != FlowIndirectCallKind::BuiltinCallback || call.times.predicted() != BuiltinCallbackTimes::Once || call.isInCycle)
                continue;
            FunctionID callee = call.callee;
            ValueFlowObjectID closure = inputs.universe().closureOf(callee);
            const FunctionFlow& flow = flows[callee.index()];
            bool isOnlyEntry = !numberOfEnteringOperations[callee.index()] && numberOfIndirectCalls[callee.index()] == 1 && !calls.functionsEnteredWithoutRecord.get(callee.index());
            if (!isOnlyEntry || inputs.hasEscaped(closure) || inputs.isLost(closure) || flow.callers != CallerState::AllKnown || flow.executions != ExecutionCount::Unknown)
                continue;
            if (isCalledByForwardingBuiltinItself(inputs, application, maker, call))
                runsAtMostOnce(callee);
        }
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
