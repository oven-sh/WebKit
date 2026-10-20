/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowCallState.h"

#if ENABLE(AOT)

#include <algorithm>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

static FlowBuiltinCallStatement& statementAt(FlowSolver& solver, FlowStatementSite site)
{
    RELEASE_ASSERT(site.statement.kind == FlowStatementKind::BuiltinCall);
    return solver.callState().builtins.statements[site.statement.index];
}

static FlowStatementSite siteOf(const FlowBuiltinCallKey& key, uint32_t index)
{
    return { key.summaryStatement.instance, { FlowStatementKind::BuiltinCall, index } };
}

// A request without a statement of its own is made by native code.
static FlowStatementSite callerOf(FlowSolver& solver, const FlowCallRequest& request)
{
    return request.site.instance ? request.site : nativeCodeSite(solver);
}

static Subject subjectOf(const FlowSolver& solver, const FlowBuiltinCallKey& key)
{
    const FlowGraph& graph = solver.graph();
    FlowInstanceID instance = key.summaryStatement.instance;
    if (graph.isHost(instance))
        return { };
    return { .function = graph.at(instance).function, .value = key.origin };
}

// A statement takes nodes. The receivers that a caller passes one by one to one callee are collected in a node per callee.
static FlowNodeID receiverNodeOf(FlowSolver& solver, const FlowCallOperands& operands, const FlowTemporaryKey& receiversOfCallee)
{
    if (!operands.thisObject)
        return operands.thisValue;

    FlowNodeID receiver = solver.temporaryFor(receiversOfCallee);
    solver.joinObject(receiver, operands.thisObject);
    if (!operands.thisValue)
        return receiver;

    // No value is inside never. The edge only says that the node the object was found in is used as the node that stands in for it.
    auto binding = solver.callState().operandFlows.operandOfReceiverHub.add(receiver, operands.thisValue);
    if (binding.isNewEntry)
        solver.connect(operands.thisValue, receiver, TypeTable::never);
    else if (binding.iterator->value != operands.thisValue)
        solver.noteDirectUses(operands.thisValue, OperandUses::all());
    return receiver;
}

static FlowStatementSite summaryStatementOf(FlowSolver& solver, FlowStatementSite caller)
{
    if (caller.statement.kind != FlowStatementKind::BuiltinCall)
        return caller;
    return statementAt(solver, caller).key.summaryStatement;
}

static FlowBuiltinCallKey withOwnNodes(FlowBuiltinCallKey key, const FlowBuiltinCallOperands& passed)
{
    key.nodes = FlowBuiltinCallNodes::Own;
    key.passesReceiver = !!passed.receiver;
    key.passesNewTarget = passed.newTarget || passed.newTargetObject;
    key.takesResult = !!passed.result;
    return key;
}

namespace {

struct FoundStatement {
    uint32_t index { 0 };
    bool isNew { false };
};

} // anonymous namespace

// An operand that is only handed on follows its edges. Whatever reads the value of an operand takes from a cursor or waits on the node itself, so a call that reads none runs once.
static FoundStatement findOrAppendStatement(FlowSolver& solver, const FlowBuiltinCallKey& key, NOESCAPE const Invocable<FlowBuiltinCallOperands()> auto& operandsOfNewStatement)
{
    FlowBuiltinCallState& state = solver.callState().builtins;
    uint32_t index = state.statements.size();
    auto entry = state.statementByKey.add(key, index);
    if (!entry.isNewEntry)
        return { entry.iterator->value, false };

    state.statements.append(FlowBuiltinCallStatement { key, operandsOfNewStatement() });
    state.statementsByInstance.add(key.summaryStatement.instance, Vector<uint32_t> { }).iterator->value.append(index);

    // Most nodes whose values are called stay empty: the node of a holder for the keys nobody names, a getter nobody defines.
    FlowStatementSite site = siteOf(key, index);
    bool hasNothingToCall = key.callee.values && key.callee.protocols.isEmpty() && !solver.valueOf(key.callee.values).mayBeObject();
    if (hasNothingToCall)
        solver.waitOn(key.callee.values, site, FlowWakeUp::ContinueAtObject);
    else
        solver.queueStatement(site);
    return { index, true };
}

// Such a statement stands for any number of calls.
static FlowBuiltinCallOperands createOwnNodes(FlowSolver& solver, const FlowBuiltinCallKey& key, Range argumentCount)
{
    FlowGraph& graph = solver.graph();
    FlowInstanceID instance = key.summaryStatement.instance;

    FlowBuiltinCallOperands own;
    if (key.passesReceiver)
        own.receiver = graph.addTemporary(instance);
    if (key.passesNewTarget)
        own.newTarget = graph.addTemporary(instance);
    if (key.takesResult)
        own.result = graph.addTemporary(instance);
    own.thrown = graph.addTemporary(instance);
    own.arguments.count = argumentCount;
    own.isInCycle = true;
    return own;
}

namespace {

class OwnArguments {
public:
    OwnArguments(FlowSolver& solver, FlowStatementSite site, FlowBuiltinCallStatement& statement)
        : m_solver(solver)
        , m_site(site)
        , m_statement(statement)
        , m_own(statement.operands.arguments)
        , m_maxPositions(solver.callState().limits.maxPositionalParts)
    {
    }

    void take(const FlowPassedArguments& passed)
    {
        growTo(passed.positions.size());
        for (size_t index = 0; index < passed.positions.size(); ++index)
            pass(passed.positions[index], index < m_own.positions.size() ? m_own.positions[index] : ensureRest());

        if (passed.rest) {
            // A position that was added after this call was taken gets what the call passes from the own rest node, so taking the call again has nothing to connect.
            for (size_t index = passed.positions.size(); index < m_own.positions.size() && !m_solver.isCheckingFixpoint(); ++index)
                m_solver.connect(passed.rest, m_own.positions[index]);
            m_solver.connect(passed.rest, ensureRest());
        }
        // The callee reads undefined at a position that this call may leave out.
        for (size_t index = std::max<int64_t>(passed.count.minimum, 0); index < m_own.positions.size(); ++index)
            m_solver.joinPrimitives(m_own.positions[index], Primitive::Undefined);
        widenCount(passed.count);

        if (!m_hasChanged)
            return;
        m_statement.overloadsWithRecordedOperands.clear();
        m_solver.revisit(m_site);
    }

private:
    FlowNodeID addNode()
    {
        FlowNodeID node = m_solver.graph().addTemporary(m_site.instance);
        m_hasChanged = true;
        return node;
    }

    void growTo(size_t numberOfPositions)
    {
        // Whatever native code passes reaches native code, by position or not.
        bool losesPositions = numberOfPositions > m_maxPositions && !m_solver.graph().isHost(m_site.instance);
        if (losesPositions && !std::exchange(m_statement.hasArgumentsPastLimit, true))
            m_solver.build().gaveUp(LimitID::MaxPositionalParts, subjectOf(m_solver, m_statement.key));

        size_t wanted = std::min<size_t>(numberOfPositions, m_maxPositions);
        while (m_own.positions.size() < wanted) {
            FlowNodeID position = addNode();
            // What an earlier call passed at this position is among its rest arguments, and an earlier call with fewer arguments passed nothing.
            m_solver.connect(m_own.rest, position);
            if (m_own.count.minimum <= static_cast<int64_t>(m_own.positions.size()))
                m_solver.joinPrimitives(position, Primitive::Undefined);
            m_own.positions.append(position);
        }
    }

    FlowNodeID ensureRest()
    {
        if (!m_own.rest) {
            m_own.rest = addNode();
            // It stands for every position past the own ones, and each of the merged calls leaves some of those out.
            m_solver.joinPrimitives(m_own.rest, Primitive::Undefined);
        }
        return m_own.rest;
    }

    void pass(FlowNodeID argument, FlowNodeID target)
    {
        if (argument)
            m_solver.connect(argument, target);
        else
            m_solver.joinPrimitives(target, Primitive::Undefined);
    }

    // A bound function that reaches itself passes more arguments each time round.
    void widenCount(Range passedCount)
    {
        Range count = m_own.count.unionWith(passedCount);
        if (count.maximum > static_cast<int64_t>(m_maxPositions))
            count.maximum = Range::length().maximum;
        if (count == m_own.count)
            return;
        m_own.count = count;
        m_hasChanged = true;
    }

    FlowSolver& m_solver;
    FlowStatementSite m_site;
    FlowBuiltinCallStatement& m_statement;
    FlowPassedArguments& m_own;
    uint64_t m_maxPositions;
    bool m_hasChanged { false };
};

} // anonymous namespace

static void claimEveryUse(FlowSolver& solver, const FlowBuiltinCallOperands& operands, const Witness& witness)
{
    FlowNodeID unknownJavaScript = solver.graph().unknownJavaScript();
    auto claim = [&](FlowNodeID operand) {
        solver.noteDirectUses(operand, OperandUses::all());
        solver.connect(operand, unknownJavaScript, { }, witness);
    };

    claim(operands.receiver);
    claim(operands.newTarget);
    if (operands.newTargetObject)
        solver.joinObject(unknownJavaScript, operands.newTargetObject, witness);
    for (FlowNodeID position : operands.arguments.positions)
        claim(position);
    claim(operands.arguments.rest);

    solver.joinUnknown(operands.result, witness);
    throwUnknownValue(solver, operands.thrown, witness);
}

static void passToOwnNodes(FlowSolver& solver, const FlowBuiltinCallKey& key, const FlowBuiltinCallOperands& passed)
{
    uint32_t index = findOrAppendStatement(solver, key, [&] {
        return createOwnNodes(solver, key, passed.arguments.count);
    }).index;

    FlowStatementSite site = siteOf(key, index);
    FlowBuiltinCallStatement& statement = statementAt(solver, site);
    const FlowBuiltinCallOperands& own = statement.operands;
    Witness witness = solver.witnessAt(site, key.origin);

    solver.connect(passed.receiver, own.receiver);
    solver.connect(passed.newTarget, own.newTarget);
    if (passed.newTargetObject)
        solver.joinObject(own.newTarget, passed.newTargetObject);
    OwnArguments(solver, site, statement).take(passed.arguments);
    solver.connect(own.result, passed.result);
    connectThrown(solver, own.thrown, passed.thrown, witness);

    if (statement.isAbandoned)
        claimEveryUse(solver, own, witness);
}

bool passesOperandsOfHost(FlowSolver& solver, FlowStatementSite caller, const FlowCallRequest& request)
{
    if (request.kind != FlowCallLinkKind::Protocol)
        return false;

    bool choosesOperands = request.operands.argumentList && request.operands.argumentList == unknownValueNode(solver);
    if (caller.statement.kind != FlowStatementKind::BuiltinCall)
        return choosesOperands;

    const FlowBuiltinCallKey& key = statementAt(solver, caller).key;
    if (key.passesOperandsOfHost)
        return true;
    return choosesOperands && (!key.callee.builtin || solver.builtins().at(key.callee.builtin).origin == BuiltinOrigin::Host);
}

static void ensureStatement(FlowSolver& solver, FlowStatementSite caller, const FlowCallRequest& request, const FlowBuiltinCallee& callee, const FlowBuiltinCallOperands& passed)
{
    FlowBuiltinCallKey key { summaryStatementOf(solver, caller), request.origin, request.kind, request.times, callee };
    key.passesOperandsOfHost = passesOperandsOfHost(solver, caller, request);
    FoundStatement found = findOrAppendStatement(solver, key, [&] {
        return passed;
    });
    if (!found.isNew && solver.callState().builtins.statements[found.index].operands != passed)
        passToOwnNodes(solver, withOwnNodes(key, passed), passed);
}

static FlowBuiltinCallOperands operandsOf(const FlowCallRequest& request, const FlowPassedArguments& arguments, FlowNodeID receiver)
{
    const FlowCallOperands& operands = request.operands;
    return { receiver, operands.newTarget, operands.newTargetObject, arguments, operands.result, operands.thrown, request.isInCycle };
}

void callBuiltin(FlowSolver& solver, const FlowCallRequest& request, BuiltinID builtin, BuiltinCallMode mode)
{
    callBuiltinOfModule(solver, request, expandArguments(solver, request), builtin, mode, ModuleID());
}

void callBuiltinObject(FlowSolver& solver, const FlowCallRequest& request, const FlowPassedArguments& arguments, FlowObjectID callee)
{
    const FlowObject& object = solver.objects().at(callee);
    ModuleID module = object.kind == FlowObjectKind::ModuleBuiltin ? object.module : ModuleID();
    callBuiltinOfModule(solver, request, arguments, object.builtin, builtinCallModeOf(request.kind), module);
}

void callBuiltinOfModule(FlowSolver& solver, const FlowCallRequest& request, const FlowPassedArguments& arguments, BuiltinID builtin, BuiltinCallMode mode, ModuleID module)
{
    builtin = solver.builtins().canonicalID(builtin);
    FlowStatementSite caller = callerOf(solver, request);
    FlowNodeID receiver = receiverNodeOf(solver, request.operands, callTemporaryKey(caller, FlowCallTemporaryRole::PrimitiveReceiver, builtin.index()));
    ensureStatement(solver, caller, request, { .builtin = builtin, .mode = mode, .module = module }, operandsOf(request, arguments, receiver));
}

void callBoundFunction(FlowSolver& solver, const FlowCallRequest& request, const FlowPassedArguments& arguments, FlowObjectID boundFunction)
{
    ensureStatement(solver, callerOf(solver, request), request, { .boundFunction = boundFunction }, operandsOf(request, arguments, FlowNodeID()));
}

void callValuesWith(FlowSolver& solver, const FlowCallRequest& request, const FlowPassedArguments& arguments, FlowNodeID callees)
{
    if (!callees)
        return;
    FlowStatementSite caller = callerOf(solver, request);
    FlowNodeID receiver = receiverNodeOf(solver, request.operands, callTemporaryKey(caller, FlowCallTemporaryRole::ReceiversOfValues, callees.index()));
    ensureStatement(solver, caller, request, { .values = callees }, operandsOf(request, arguments, receiver));
}

void callValuesOf(FlowSolver& solver, const FlowCallRequest& request, FlowNodeID callees)
{
    callValuesWith(solver, request, expandArguments(solver, request), callees);
}

void runProtocolsInOwnStatement(FlowSolver& solver, const FlowProtocolRequest& request)
{
    RELEASE_ASSERT(request.namedMembers.size() <= 1 && request.calledMembers.empty());
    BuiltinName namedMember = request.namedMembers.empty() ? BuiltinName() : request.namedMembers.front();
    FlowCallRequest call { request.site, request.origin, FlowCallLinkKind::Protocol, BuiltinCallbackTimes::Once, true, { .result = request.result, .thrown = request.thrown } };
    ensureStatement(solver, callerOf(solver, call), call, { .values = request.operand, .protocols = request.protocols, .namedMember = namedMember }, operandsOf(call, { }, FlowNodeID()));
}

static void exposeCallee(FlowSolver& solver, const FlowBuiltinCallee& callee, const Witness& witness)
{
    FlowNodeID nativeCode = solver.graph().nativeCode();
    if (!callee.protocols.isEmpty()) {
        solver.connect(callee.values, solver.graph().unknownJavaScript(), { }, witness);
        return;
    }
    if (callee.values) {
        solver.connect(callee.values, nativeCode, { }, witness);
        return;
    }
    if (callee.boundFunction) {
        solver.joinObject(nativeCode, callee.boundFunction, witness);
        return;
    }
    if (!isObjectKind(solver.builtins().at(callee.builtin).kind)) {
        noteBuiltinUse(solver, callee.builtin, BuiltinUse::Escaped, witness);
        return;
    }
    FlowObjectID object = callee.module ? ensureModuleBuiltinObject(solver, callee.builtin, callee.module) : ensureBuiltinObject(solver, callee.builtin);
    solver.joinObject(nativeCode, object, witness);
}

// No overload is selected any more: the operation still names the built-in, and nothing bounds what else runs there.
static void linkAbandonedCall(FlowSolver& solver, FlowStatementSite site, const FlowBuiltinCallKey& key)
{
    if (!key.origin)
        return;
    if (key.callee.builtin)
        solver.records().addBuiltinCallLink({ site.instance, key.origin, key.callee.builtin, { }, key.callee.mode, key.kind });
    solver.records().addUnknownCallee({ site.instance, key.origin, key.kind });
}

void abandonBuiltinCall(FlowSolver& solver, FlowStatementSite site)
{
    FlowBuiltinCallStatement& statement = statementAt(solver, site);
    if (std::exchange(statement.isAbandoned, true))
        return;

    Witness witness = solver.witnessAt(site, statement.key.origin);
    claimEveryUse(solver, statement.operands, witness);
    // An operand may be the unknown object, now or later. What the call does to it is done through the unknown object's own locations, which every object in native code aliases.
    applyEveryUseThroughUnknownObject(solver, { .site = site, .origin = statement.key.origin, .thrown = statement.operands.thrown });
    exposeCallee(solver, statement.key.callee, witness);
    linkAbandonedCall(solver, site, statement.key);
}

void abandonBuiltinCallsOf(FlowSolver& solver, FlowInstanceID instance)
{
    // Abandoning a call may make another one in the same instance, which would never run.
    const FlowBuiltinCallState& state = solver.callState().builtins;
    for (size_t position = 0; ; ++position) {
        auto found = state.statementsByInstance.find(instance);
        if (found == state.statementsByInstance.end() || position >= found->value.size())
            return;
        abandonBuiltinCall(solver, { instance, { FlowStatementKind::BuiltinCall, found->value[position] } });
    }
}

BuiltinID builtinCalledAt(FlowSolver& solver, FlowStatementSite site)
{
    return statementAt(solver, site).key.callee.builtin;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
