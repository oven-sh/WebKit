/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowResultsInternal.h"

#if ENABLE(AOT)

#include "AOTBlockCycles.h"
#include "AOTBuiltinIDs.h"
#include "AOTThreads.h"
#include "AOTValueFlowResultsUnseenCode.h"
#include <atomic>

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
        for (const Block& block : function.blocks) {
            for (ValueID id : block.values) {
                forEachFunctionEnteredBy(program, application.bodies[index], function, id, [&](FunctionID callee) {
                    std::atomic_ref(counts[callee.index()]).fetch_add(1, std::memory_order_relaxed);
                });
            }
        }
    });
    return counts;
}

static bool isUsedOtherThanTested(const ValueFlowResultInputs& inputs, BuiltinID builtin)
{
    return !(inputs.solver().usesOf(builtin) - BuiltinUse::Tested).isEmpty();
}

// An ES module keeps the outcome of its one evaluation. The loader runs the body of any other module again once its entry has left the module cache,
// and it forgets a CommonJS module whose body threw: the next require() runs the body again, beside whatever the failed run has let out.
static bool isEvaluatedAtMostOnce(const ValueFlowResultInputs& inputs, const Module& module, bool cacheKeepsEntries)
{
    switch (module.kind) {
    case ModuleKind::SourceText:
    case ModuleKind::RuntimeSource:
        return true;
    case ModuleKind::CommonJs:
        return cacheKeepsEntries && inputs.solver().valueOf(inputs.bodies().nodes().thrown(module.body)) == ValueSets::nothing;
    case ModuleKind::Data:
        return cacheKeepsEntries;
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
    bool cacheKeepsEntries = !unseenCodeCanRun(inputs.solution) && !isUsedOtherThanTested(inputs, Builtins::ModuleRequireCache) && !isUsedOtherThanTested(inputs, Builtins::NodeModule_cache);

    Vector<FunctionID> worklist;
    auto runsAtMostOnce = [&](FunctionID function) {
        if (!inputs.build.isEnabled(FlowResultKind::ExecutionCounts, function.index()))
            return;
        flows[function.index()].executions = ExecutionCount::AtMostOnce;
        worklist.append(function);
    };
    for (uint32_t index = 0; index < program.modules.size(); ++index) {
        const Module& module = program.modules[index];
        if (!module.body || !inputs.isReached(module.body) || !isEvaluatedAtMostOnce(inputs, module, cacheKeepsEntries))
            continue;
        // A body that takes its callee can hand itself on.
        bool isEnteredOnlyByEvaluation = !numberOfEnteringOperations[module.body.index()] && !calls.someCallEntersAnything
            && !calls.functionsEnteredWithoutStamp.get(module.body.index()) && !inputs.hasEscaped(inputs.universe().closureOf(module.body));
        if (isEnteredOnlyByEvaluation)
            runsAtMostOnce(module.body);
    }

    // The one operation that enters a function runs at most once if it lies outside every cycle of a function that does.
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
                    if (numberOfEnteringOperations[callee.index()] == 1 && flow.callers == CallerState::AllKnown && flow.executions == ExecutionCount::Unknown)
                        runsAtMostOnce(callee);
                });
            }
        }
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
