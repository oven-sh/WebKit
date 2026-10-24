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
    case ModuleKind::SourceText:
    case ModuleKind::CommonJs:
    case ModuleKind::Data:
    case ModuleKind::RuntimeSource:
        return true;
    case ModuleKind::Runtime:
    case ModuleKind::External:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static const ValueFlowStatement* findCallAt(const ValueFlowResultInputs& inputs, const FlowApplication& application, const FlowIndirectCall& call)
{
    if (!call.site)
        return nullptr;
    ValueID site = application.accessSites.valueOfSite[call.site.index()];
    for (const ValueFlowStatement& statement : inputs.bodies().statementsOf(call.caller.index())) {
        if (statement.kind == ValueFlowStatementKind::Operation && statement.opcode == Opcode::Call && statement.site == site)
            return &statement;
    }
    return nullptr;
}

// `f.call(t)`, `f.apply(t, list)`, `Reflect.apply(f, t, list)`: the callee is what the built-in itself calls, once, then and there.
// A built-in or a bound function that arrives where `f` stands is handed the other operands and calls them as often as it likes, and the list of indirect calls does not tell the two apart.
static bool isCalledByForwardingBuiltinItself(const ValueFlowResultInputs& inputs, const FlowApplication& application, ValueSetMaker& maker, const FlowIndirectCall& call)
{
    const ValueFlowStatement* statement = findCallAt(inputs, application, call);
    if (!statement)
        return false;
    const FlowCallStamp* stamp = findByValue(application.bodies[call.caller.index()].calls.span(), statement->site, &FlowCallStamp::value);
    if (!stamp)
        return false;

    static_assert(!OperandIndex::Call::callee && OperandIndex::Call::thisValue == 1);
    constexpr size_t firstArgument = 2;
    std::span<const ValueFlowOperand> operands = inputs.bodies().operandsOf(*statement);
    const CalleeSet& callees = inputs.program.at(stamp->callees);
    if (!callees.isComplete)
        return false;
    ValueSetID called = ValueSets::nothing;
    for (BuiltinID builtin : callees.builtins) {
        for (const BuiltinOverload& overload : inputs.program.builtins().overloadsOf(builtin)) {
            std::optional<BuiltinOperand> operand = operandCalledBy(overload);
            if (!operand)
                return false;
            size_t index = operand->isReceiver() ? OperandIndex::Call::thisValue : firstArgument + operand->argumentIndex();
            if (index >= operands.size())
                return false;
            called = maker.unite(called, inputs.valueOf(maker, operands[index]));
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
    BitVector modulesWithBodiesThatOnlyLoaderEnters = findModulesWithBodiesThatOnlyLoaderEnters(inputs, calls, numberOfEnteringOperations);
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
        // `(function () { .. }).call(this)` enters the function as a call operation does.
        for (const FlowIndirectCall& call : std::ranges::equal_range(indirectCalls, caller, { }, &FlowIndirectCall::caller)) {
            if (call.kind != FlowIndirectCallKind::BuiltinCallback || call.times != BuiltinCallbackTimes::Once || call.isInCycle)
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
