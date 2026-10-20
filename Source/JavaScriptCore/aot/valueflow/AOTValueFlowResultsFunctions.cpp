/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowResultsInternal.h"

#if ENABLE(AOT)

#include "AOTThreads.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

// The solve enters the body of a module where something reached loads the module, and a function of the library where something reached uses its built-in.
// Where the image has no compiled function of the library, the host creates its ordinary one.
static bool isEnteredOnDemand(const Function& function)
{
    if (function.role == FunctionRole::Library || function.role == FunctionRole::Model)
        return true;
    return function.kind == FunctionKind::Module || function.kind == FunctionKind::CommonJsModule;
}

static const CalleeSet* findStamp(const ValueFlowResultInputs& inputs, const FlowApplication& application, const ValueFlowStatement& statement)
{
    if (statement.kind != ValueFlowStatementKind::Operation || !statement.function)
        return nullptr;
    const FlowCallStamp* stamp = findByValue(application.bodies[statement.function.index()].calls.span(), statement.site, &FlowCallStamp::value);
    return stamp ? &inputs.program.at(stamp->callees) : nullptr;
}

// Every rule that enters a closure asks for the group of a set that holds it. A call lists in its stamp what it enters as its callee. Whatever else is entered,
// by an accessor, a protocol, a built-in that calls an operand or a bound function, is in no stamp.
static BitVector findFunctionsEnteredWithoutStamp(const ValueFlowResultInputs& inputs, const FlowApplication& application)
{
    const ValueFlowSolver& solver = inputs.solver();
    const ValueFlowUniverse& universe = inputs.universe();
    BitVector functions(inputs.program.functions.size());
    bool anyFunctionIsEntered = false;
    for (uint32_t index = 0; index < solver.numberOfStatements(); ++index) {
        ValueFlowStatementID id(index);
        const ValueFlowStatement& statement = solver.statementAt(id);
        bool hasRun = solver.isMadeByRule(id) ? !solver.isAtRest(id) : !statement.function || inputs.isReached(statement.function);
        if (!hasRun)
            continue;

        const CalleeSet* stamp = findStamp(inputs, application, statement);
        solver.forEachMadeNodeAskedForBy(id, [&](const ValueFlowNodeKey& key, ValueFlowStatementID made) {
            // The statement of a group asks for the nodes of that group.
            if (key.kind != ValueFlowMadeNodeKind::CallGroup || made == id)
                return;
            if (ValueSets::isComplement(key.set)) {
                anyFunctionIsEntered = true;
                return;
            }
            inputs.sets().forEachMember(key.set, [&](ValueFlowObjectID callee) {
                FunctionID function = universe.closureFunction(callee);
                if (function && !(stamp && std::ranges::binary_search(stamp->functions, function)))
                    functions.quickSet(function.index());
            });
        });
    }
    if (anyFunctionIsEntered) {
        for (size_t index = 0; index < inputs.program.functions.size(); ++index)
            functions.quickSet(index);
    }
    return functions;
}

static BitVector findFunctionsNamedByUnsolvedCode(const ValueFlowResultInputs& inputs, const FlowResultTables& tables)
{
    const Program& program = inputs.program;
    BitVector functions(program.functions.size());
    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        if (!isUnsolvedCode(inputs, tables, FunctionID(index)))
            continue;
        for (const Value& value : program.functions[index].values) {
            if (value.opcode == Opcode::CallDirect || value.opcode == Opcode::NewClosure)
                functions.quickSet(value.function().index());
        }
    }
    return functions;
}

void computeFunctionResults(const ValueFlowResultInputs& inputs, ValueFlowStampedCalls& calls, FlowResultTables& tables)
{
    const Program& program = inputs.program;
    FlowApplication& application = *tables.application;
    unsigned numberOfFunctions = program.functions.size();
    BitVector isCalledByHost = functionsCalledByHost(program, inputs.loops());
    calls.functionsEnteredWithoutStamp = findFunctionsEnteredWithoutStamp(inputs, application);

    Vector<uint32_t> unreachedFunctions = findIndicesInParallel(inputs.loops(), numberOfFunctions, [&](unsigned index) {
        if (inputs.isReached(FunctionID(index)) || !inputs.build.isEnabled(FlowResultKind::ReachedFunctions, index))
            return false;
        return !isCalledByHost.get(index) || isEnteredOnDemand(program.functions[index]);
    });
    for (uint32_t index : unreachedFunctions)
        tables.functions.functions[index].reachability = FunctionReachability::Unreached;
    inputs.build.statistics().count(StatisticID::FlowUnreachedFunctions, unreachedFunctions.size());

    calls.functionsNamedByUnsolvedCode = findFunctionsNamedByUnsolvedCode(inputs, tables);
    forEachIndexInParallel(inputs.loops(), numberOfFunctions, [&](unsigned index) {
        FunctionFlow& flow = tables.functions.functions[index];
        if (isCalledByHost.get(index) || calls.functionsNamedByUnsolvedCode.get(index) || inputs.hasEscaped(inputs.universe().closureOf(FunctionID(index)))) {
            flow.cause = UnprovenCause::ReachesNativeCode;
            return;
        }
        if (!inputs.build.isEnabled(FlowResultKind::CalleeSets, index)) {
            flow.cause = UnprovenCause::ResultDisabled;
            return;
        }
        if (calls.functionsEnteredWithoutStamp.get(index) || calls.someCallEntersAnything)
            return;
        flow.callers = CallerState::AllKnown;
        OptionSet<FunctionCapabilities>& capabilities = application.signatures[index].provenCapabilities;
        capabilities.add(FunctionCapabilities::IsNeverCalledByHost);
        if (!calls.functionsInStamps.get(index))
            capabilities.add(FunctionCapabilities::HasOnlyDirectCallers);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
