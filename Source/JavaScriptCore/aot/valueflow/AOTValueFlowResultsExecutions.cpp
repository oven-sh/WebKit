/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowResultsInternal.h"

#if ENABLE(AOT)

#include "AOTBlockCycles.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include <atomic>
#include <wtf/Atomics.h>

namespace JSC { namespace AOT { namespace Typed {

// What a call operation enters: the function it names, or what its stamp lists.
static void forEachFunctionEnteredBy(const Program& program, const FlowBodyUpdate& update, const Function& function, ValueID id, NOESCAPE const Invocable<void(FunctionID)> auto& functor)
{
    const Value& value = function.at(id);
    if (value.opcode == Opcode::CallDirect) {
        functor(value.function());
        return;
    }
    if (!PayloadWord::callees(value.opcode))
        return;
    if (const FlowCallStamp* stamp = findByValue(update.calls.span(), id, &FlowCallStamp::value)) {
        for (FunctionID callee : program.at(stamp->callees).functions)
            functor(callee);
    }
}

static Vector<uint32_t> countEnteringOperations(const ValueFlowResultInputs& inputs, const FlowResultTables& tables, const FlowApplication& application)
{
    const Program& program = inputs.program;
    Vector<uint32_t> counts(FillWith { }, program.functions.size(), 0u);
    forEachIndexInParallel(inputs.loops(), program.functions.size(), [&](unsigned index) {
        const Function& function = program.functions[index];
        if (!inputs.isReached(FunctionID(index)) && !isUnsolvedCode(inputs, tables, FunctionID(index)))
            return;
        // A stamp stands at an operation of a statement, which some block lists once.
        if (!inputs.bodies().holdsAnyOf(FunctionID(index), notedOperations({ ValueFlowNotedOperation::CallDirect }))) {
            for (const FlowCallStamp& stamp : application.bodies[index].calls) {
                if (!PayloadWord::callees(function.at(stamp.value).opcode))
                    continue;
                for (FunctionID callee : program.at(stamp.callees).functions)
                    AtomicRef(counts[callee.index()]).fetch_add(1, std::memory_order_relaxed);
            }
            return;
        }
        for (const Block& block : function.blocks) {
            for (ValueID id : block.values) {
                forEachFunctionEnteredBy(program, application.bodies[index], function, id, [&](FunctionID callee) {
                    AtomicRef(counts[callee.index()]).fetch_add(1, std::memory_order_relaxed);
                });
            }
        }
    });
    return counts;
}

// By module: only the evaluation of the module enters the body. A body that takes its callee can hand itself on.
static BitVector findModulesWithBodiesThatOnlyLoaderEnters(const ValueFlowResultInputs& inputs, const ValueFlowStampedCalls& calls, const Vector<uint32_t>& numberOfEnteringOperations)
{
    const Program& program = inputs.program;
    BitVector modules(program.modules.size());
    for (uint32_t index = 0; index < program.modules.size(); ++index) {
        FunctionID body = program.modules[index].body;
        if (!body || !inputs.isReached(body))
            continue;
        // No stamp lists what a placeholder beside it covers.
        ValueFlowObjectID closure = inputs.universe().closureOf(body);
        if (!numberOfEnteringOperations[body.index()] && !calls.functionsEnteredWithoutStamp.get(body.index()) && !inputs.hasEscaped(closure) && !inputs.isLost(closure))
            modules.quickSet(index);
    }
    return modules;
}

// A module of the image is evaluated at most once, whatever its kind: no cache lists it, and a body that has thrown is not run again.
static bool isEvaluatedAtMostOnce(ModuleKind kind)
{
    switch (kind) {
    default:
        return true;
    case ModuleKind::Runtime:
    case ModuleKind::External:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void computeExecutionResults(const ValueFlowResultInputs& inputs, const ValueFlowStampedCalls& calls, FlowResultTables& tables)
{
    const Program& program = inputs.program;
    const FlowApplication& application = *tables.application;
    Vector<FunctionFlow>& flows = tables.functions.functions;
    Vector<uint32_t> numberOfEnteringOperations = countEnteringOperations(inputs, tables, application);
    BitVector modulesWithBodiesThatOnlyLoaderEnters = findModulesWithBodiesThatOnlyLoaderEnters(inputs, calls, numberOfEnteringOperations);

    Vector<FunctionID> worklist;
    auto runsAtMostOnce = [&](FunctionID function) {
        if (!inputs.build.isEnabled(FlowResultKind::ExecutionCounts, function.index()))
            return;
        flows[function.index()].executions = ExecutionCount::AtMostOnce;
        worklist.append(function);
    };
    for (size_t index : modulesWithBodiesThatOnlyLoaderEnters) {
        const Module& module = program.modules[index];
        if (isEvaluatedAtMostOnce(module.kind))
            runsAtMostOnce(module.body);
    }

    // The one operation that enters a function runs at most once if it lies outside every cycle of a function that does. An accessor, a protocol or a callback that enters the function too is in no stamp.
    while (!worklist.isEmpty()) {
        FunctionID caller = worklist.takeLast();
        const Function& function = program.at(caller);
        CFG cfg(function);
        BlockCycles cycles(cfg);
        for (BlockID block : cfg.reversePostOrder()) {
            if (cycles.isInCycle(block))
                continue;
            for (ValueID id : function.at(block).values) {
                forEachFunctionEnteredBy(program, application.bodies[caller.index()], function, id, [&](FunctionID callee) {
                    const FunctionFlow& flow = flows[callee.index()];
                    if (numberOfEnteringOperations[callee.index()] == 1 && !calls.functionsEnteredWithoutStamp.get(callee.index()) && flow.callers == CallerState::AllKnown && flow.executions == ExecutionCount::Unknown)
                        runsAtMostOnce(callee);
                });
            }
        }
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
