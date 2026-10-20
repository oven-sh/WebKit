/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowFixpointCheck.h"

#if ENABLE(AOT)

#include "AOTFlowCallState.h"
#include "AOTFlowNames.h"
#include "AOTFlowObjectOperations.h"
#include "AOTFlowObjectState.h"
#include "AOTFlowPropertyLocations.h"
#include "AOTFlowPropertyOperations.h"
#include "AOTFlowPropertyState.h"
#include "AOTFlowUnseenCode.h"
#include <algorithm>
#include <optional>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/HexNumber.h>
#include <wtf/StringPrintStream.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr size_t maxReportedChanges = 16;

namespace {

struct LinkedRequestProgress {
    FlowObjectSet closures;
    Vector<FunctionID> functionsWithoutClosure;
    bool hasUnknownCallee { false };
    FlowObjectSet predictedCallees;
    bool hasTooManyCalleesToPredict { false };
    bool allocatesThis { false };
    bool everyConstructorHasOtherNewTarget { false };
    FlowObjectID onlyNewTarget;
};

struct BuiltinCallProgress {
    Vector<FlowMatchedObjects, 2> matchedObjects;
    Vector<FlowKeysOfNode, 0> keysOfNodes;
    Vector<BuiltinOverloadID, 1> recordedOverloads;
    Vector<BuiltinOverloadID, 1> overloadsWithRecordedOperands;
};

} // anonymous namespace

// Every member of every state of the solve is one of four things:
// an identity (which object, node, statement or request stands for a key), which stays;
// a fact that rules read, which stays, and what its arrival set off is repeated;
// what a statement has done or seen so far, which is taken away here and put back after the check;
// a cache, which is emptied.
// Each state is taken apart by name below, so that a state with another member does not compile until the member has its place.
struct FlowSolver::FixpointCheck::Progress {
    FlowRecords records;
    Counters counters;

    Vector<LinkedRequestProgress> linkedRequests;
    Vector<BuiltinCallProgress> builtinCalls;
    HashSet<std::tuple<FlowInstanceID, ValueID, uint8_t>> recordedUnknownCallees;
    HashSet<std::tuple<FlowCallLinkID, FlowNodeID, FlowNodeID>> recordedFlows;
    HashSet<std::tuple<FlowStatementSite, BuiltinOverloadID, uint32_t, FlowNodeID>> recordedOperandValues;
    HashMap<std::pair<FlowInstanceID, ValueID>, TypeID> typedResultTypes;
    HashMap<std::pair<FlowStatementSite, FlowNodeID>, FlowObjectSet> steppedIterators;
    HashMap<FlowStatementSite, FlowKeySet> keysOfElementAccesses;
    HashMap<std::pair<FlowStatementSite, FlowNodeID>, uint8_t> formsOfDescriptors;
    HashMap<FlowStatementSite, bool> copiesWithOneObjectPerArgument;
    BitVector hasCapturedExports;
    BitVector builtinsWithEnteredAttachments;
    Vector<FlowObjectID> holdersWithoutMembers;
    Vector<FlowObjectID> holdersOfRefusedMembers;
    BitVector withdrawnFields;
    Vector<FlowLocationOrder> locationOrders;
    BitVector locationsReadBeforeInitialization;
};

FlowSolver::FixpointCheck::FixpointCheck(FlowSolver& solver)
    : m_solver(solver)
    , m_progress(makeUniqueWithoutFastMallocCheck<Progress>())
{
}

FlowSolver::FixpointCheck::~FixpointCheck() = default;

auto FlowSolver::FixpointCheck::sizes() const -> Sizes
{
    const FlowCallState& calls = *m_solver.m_states.calls;
    return {
        m_solver.m_graph.numberOfNodes(),
        m_solver.m_graph.numberOfEdges(),
        m_solver.m_graph.numberOfInstances(),
        m_solver.m_objects.size(),
        calls.builtins.statements.size(),
        calls.links.linkedRequests.size(),
        m_solver.m_records.callLinks().size(),
        m_solver.m_records.operandFlows().size(),
    };
}

String FlowSolver::FixpointCheck::nameOf(FlowNodeID node) const
{
    StringPrintStream out;
    m_solver.m_graph.dumpNameOf(out, node);
    return out.toString();
}

ValueID FlowSolver::FixpointCheck::originOf(FlowStatementSite site) const
{
    uint32_t index = site.statement.index;
    if (site.statement.kind == FlowStatementKind::BuiltinCall)
        return m_solver.m_states.calls->builtins.statements[index].key.origin;

    auto originOfStatement = [](const auto& statement) {
        if constexpr (requires { statement.origin; })
            return statement.origin;
        else
            return ValueID();
    };
    const FlowStatements& statements = m_solver.summaryOf(site).statements;
    switch (site.statement.kind) {
#define ORIGIN_OF_FLOW_STATEMENT(kind, type, member, cursors) \
    case FlowStatementKind::kind: \
        return originOfStatement(statements.member[index]);
        FOR_EACH_FLOW_STATEMENT_KIND(ORIGIN_OF_FLOW_STATEMENT)
#undef ORIGIN_OF_FLOW_STATEMENT
    case FlowStatementKind::BuiltinCall:
    case FlowStatementKind::Lookup:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

String FlowSolver::FixpointCheck::describeStep() const
{
    const FlowGraph& graph = m_solver.m_graph;
    auto describeObject = [&](ASCIILiteral whatHappens) {
        return makeString(nameOf(m_step.object), ' ', whatHappens);
    };

    switch (m_step.rule) {
    case Rule::None:
        return "all rules together"_s;
    case Rule::Roots:
        return "the roots"_s;
    case Rule::UnseenCode:
        return makeString("unseen code, cause "_s, m_step.index);
    case Rule::OtherNewTarget:
        return "the construction of an unknown callee for another new.target"_s;
    case Rule::BuiltinUse:
        return makeString("the first use of built-in "_s, m_step.index);
    case Rule::ModuleEvaluation:
        return makeString("the evaluation of m"_s, m_step.index);
    case Rule::ArrivalInNativeCode:
        return describeObject("reaches native code"_s);
    case Rule::ArrivalInUnknownJavaScript:
        return describeObject("reaches unknown JavaScript"_s);
    case Rule::LocationCreation:
        return makeString("the creation of "_s, nameOf(m_step.node));
    case Rule::UnnamedKeyStores:
        return describeObject("is stored to under unnamed keys"_s);
    case Rule::MovedElements:
        return describeObject("has its elements moved"_s);
    case Rule::Seeds:
        return makeString("the seeds of i"_s, m_step.site.instance.index());
    case Rule::Statement:
    case Rule::AbandonedCall: {
        FlowStatementSite site = m_step.site;
        StringPrintStream out;
        out.print(m_step.rule == Rule::AbandonedCall ? "abandoned "_s : ""_s, Typed::nameOf(site.statement.kind), " "_s, site.statement.index, " in i"_s, site.instance.index());
        if (graph.isHost(site.instance))
            out.print(" (host)"_s);
        else
            out.print(" (f"_s, graph.at(site.instance).function.index(), ")"_s);
        if (ValueID origin = originOf(site))
            out.print(" at v"_s, origin.index());
        return out.toString();
    }
    case Rule::AbandonedInstance:
        return makeString("abandoned i"_s, m_step.site.instance.index());
    case Rule::Edges:
        return makeString("the edges and waiters of "_s, nameOf(m_step.node));
    case Rule::Refinement:
        return "a refinement"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void FlowSolver::FixpointCheck::addChange(FlowNodeID node, const String& whatIsGained)
{
    if (++m_numberOfChanges > maxReportedChanges)
        return;

    bool isStatement = m_step.rule == Rule::Statement || m_step.rule == Rule::AbandonedCall;
    m_changes.append(FlowFixpointChange {
        m_step.site,
        isStatement ? originOf(m_step.site) : ValueID(),
        node,
        makeString(describeStep(), ": "_s, whatIsGained),
    });
}

void FlowSolver::FixpointCheck::finishStep()
{
    Sizes now = sizes();
    if (now == m_sizes) [[likely]]
        return;

    // A node or an edge that changes no set that was there before is no step away from the fixpoint: a rule may make a temporary of its own each time it runs.

    auto report = [&](auto before, auto after, ASCIILiteral what) {
        if (before != after)
            addChange({ }, makeString("makes "_s, after - before, ' ', what, ", the first with number "_s, before));
    };
    report(m_sizes.nodes, now.nodes, "nodes"_s);
    report(m_sizes.instances, now.instances, "instances"_s);
    report(m_sizes.objects, now.objects, "objects"_s);
    for (size_t index = m_sizes.builtinCalls; index < now.builtinCalls; ++index) {
        const FlowBuiltinCallKey& key = m_solver.m_states.calls->builtins.statements[index].key;
        const FlowBuiltinCallee& callee = key.callee;
        StringPrintStream out;
        out.print("makes BuiltinCall "_s, index, key.nodes == FlowBuiltinCallNodes::Own ? " with nodes of its own"_s : ""_s, ", which "_s);
        if (callee.boundFunction)
            out.print("calls "_s, nameOf(callee.boundFunction));
        else if (callee.values)
            out.print(callee.protocols.isEmpty() ? "calls the values of "_s : "runs protocols on "_s, nameOf(callee.values));
        else
            out.print("calls built-in "_s, callee.builtin.index());
        addChange(callee.values, out.toString());
    }
    // A request to callees is a place to keep progress. What it links, hands on or answers is reported where it happens.
    m_sizes = now;
}

void FlowSolver::FixpointCheck::begin(const Step& step)
{
    finishStep();
    m_step = step;
}

void FlowSolver::FixpointCheck::willRun(FlowStatementSite site)
{
    begin({ .rule = Rule::Statement, .site = site });
}

void FlowSolver::FixpointCheck::willInitialize(FlowInstanceID instance)
{
    begin({ .rule = Rule::Seeds, .site = { instance, { } } });
}

void FlowSolver::FixpointCheck::willProcess(FlowNodeID node)
{
    begin({ .rule = Rule::Edges, .node = node });
}

template<typename Flag>
static String namesOf(OptionSet<Flag> flags)
{
    StringPrintStream out;
    dumpFlags(out, flags);
    return out.toString();
}

String FlowSolver::FixpointCheck::nameOf(FlowObjectID object) const
{
    StringPrintStream out;
    m_solver.m_objects.dump(out, object);
    return out.toString();
}

void FlowSolver::FixpointCheck::didChangeValue(FlowNodeID node, const FlowObjectSet& addedObjects, bool hasNewPrimitives)
{

    StringPrintStream out;
    out.print(nameOf(node), " gains "_s);
    if (!addedObjects.isEmpty()) {
        FlowValue added;
        added.objects = addedObjects;
        m_solver.values().dump(out, added);
    }
    if (hasNewPrimitives) {
        out.print(addedObjects.isEmpty() ? "primitives: it holds "_s : " and primitives: it holds "_s);
        m_solver.values().dump(out, m_solver.valueOf(node).withoutObjects());
    }
    addChange(node, out.toString());
}

void FlowSolver::FixpointCheck::didAddEdge(FlowNodeID from, FlowNodeID to)
{
    addChange(to, makeString("connects "_s, nameOf(from), " to "_s, nameOf(to)));
}

void FlowSolver::FixpointCheck::didAddMutations(FlowObjectID object, OptionSet<ObjectMutation> mutations)
{
    addChange({ }, makeString(nameOf(object), " gets mutations "_s, namesOf(mutations)));
}

void FlowSolver::FixpointCheck::didAddUse(FlowObjectID object, FlowObjectUse use)
{
    addChange({ }, makeString(nameOf(object), " gets use "_s, Typed::nameOf(use)));
}

void FlowSolver::FixpointCheck::didAddEvent(FlowObjectID object, FlowObjectEvent event)
{
    bool isStatedAgain = m_step.rule == Rule::MovedElements && m_step.object == object && event == FlowObjectEvent::ElementsMoved;
    if (isStatedAgain)
        return;
    addChange({ }, makeString(nameOf(object), " gets event "_s, Typed::nameOf(event)));
}

void FlowSolver::FixpointCheck::didAddLocationEvents(FlowNodeID location, OptionSet<FlowLocationEvent> events)
{
    addChange(location, makeString(nameOf(location), " gets events "_s, namesOf(events)));
}

void FlowSolver::FixpointCheck::didAddDirectUses(FlowNodeID node, OperandUses uses)
{
    addChange(node, makeString(nameOf(node), " gets direct uses "_s, namesOf(uses & everyFlagOf<OperandUse>())));
}

void FlowSolver::FixpointCheck::forgetProgress()
{
    Progress& saved = *m_progress;
    saved.records = m_solver.m_records.copy();
    saved.counters = m_solver.m_counters;

    RELEASE_ASSERT(m_solver.m_worklist.cursorsWithMembersToRepeat.isEmpty());
    for (uint32_t index = 0; index < m_solver.m_cursors.all.size(); ++index)
        m_solver.startOver(FlowCursorID(index));

    {
        auto& [maxSitesPerLayout, functionsUsingSharedObjects, solveSites, solveObjects, typedResultClasses, coroutineResults, dataNodesInPreorder] = *m_solver.m_states.objects;
        UNUSED_VARIABLE(maxSitesPerLayout);
    }

    {
        auto& [keys, ownKeysOfLists, objectOrNull, objectsWithLinkedUnnamedKeys, membersFoundByName, membersOutsidePrototypes, settersChangingReceivers, membersLockedWhileConstant, membersTakenForConstants, namesTakenForAbsent, namesWrittenOnBuiltins, builtinsWrittenUnderUnnamedKeys, holdersWithLinkTakenForConstant, builtinsWithPrototypeChangedByProgram, constructorsHeldAsUnknownValues, prototypesGivenByBuiltin, objectsDescribedByMap, keysOfElementAccesses, lookups, accessorsAssumedPresent, formsOfDescriptors, copiesWithOneObjectPerArgument, symbols, constructions] = *m_solver.m_states.properties;
        membersFoundByName.clear();
        lookups.forgetAnswers();
        saved.keysOfElementAccesses = std::exchange(keysOfElementAccesses, { });
        saved.formsOfDescriptors = std::exchange(formsOfDescriptors, { });
        saved.copiesWithOneObjectPerArgument = std::exchange(copiesWithOneObjectPerArgument, { });

        auto& [keysAsRead, creations, creationIndexes] = symbols;
        UNUSED_VARIABLE(keysAsRead);

        auto& [functions, solvedConstructors, objectIndexes, objects, withdrawnFields, functionsSeeingOnlyFinishedObjects] = constructions;
        saved.withdrawnFields = withdrawnFields;
        withdrawnFields.clearAll();
    }

    auto& [limits, links, operandFlows, coroutines, iteration, builtins, thrown, unseenCode, escapedInheritors, modules, order] = *m_solver.m_states.calls;
    UNUSED_VARIABLE(limits);

    {
        auto& [callLinks, linkedRequests, lastLinkedRequestOfSite, listArguments, genericSites, recordedUnknownCallees, constructsUnknownCalleeForOtherNewTarget, instancesEnteredWithoutOtherNewTarget] = links;
        saved.recordedUnknownCallees = std::exchange(recordedUnknownCallees, { });
        for (FlowLinkedRequest& request : linkedRequests) {
            auto& [index, previousOfSite, origin, kind, times, isInCycle, context, policy, passesExactReceiver, thisValue, newTarget, newTargetObject, result, thrownValues, arguments,
                closures, functionsWithoutClosure, hasUnknownCallee, predictedCallees, hasTooManyCalleesToPredict, allocatesThis, everyConstructorHasOtherNewTarget, onlyNewTarget] = request;
            saved.linkedRequests.append(LinkedRequestProgress {
                std::exchange(closures, { }),
                std::exchange(functionsWithoutClosure, { }),
                std::exchange(hasUnknownCallee, false),
                std::exchange(predictedCallees, { }),
                std::exchange(hasTooManyCalleesToPredict, false),
                std::exchange(allocatesThis, false),
                std::exchange(everyConstructorHasOtherNewTarget, false),
                std::exchange(onlyNewTarget, { }),
            });
        }
    }

    {
        auto& [recordedFlows, operandOfReceiverHub] = operandFlows;
        saved.recordedFlows = std::exchange(recordedFlows, { });
    }

    {
        auto& [bodyOfResult] = coroutines;
        UNUSED_VARIABLE(bodyOfResult);
        auto& [kindOfIterator, steppedIterators] = iteration;
        saved.steppedIterators = std::exchange(steppedIterators, { });
        auto& [runtimeError] = thrown;
        UNUSED_VARIABLE(runtimeError);
    }

    {
        auto& [statements, statementByKey, statementsByInstance, recordedOperandValues, typedResultTypes, usesOfFunctionsWithTypedResults, builtinsWithEnteredAttachments, featuresEnteredByHost] = builtins;
        saved.recordedOperandValues = std::exchange(recordedOperandValues, { });
        saved.typedResultTypes = std::exchange(typedResultTypes, { });
        saved.builtinsWithEnteredAttachments = builtinsWithEnteredAttachments;
        featuresEnteredByHost.clearAll();
        for (FlowBuiltinCallStatement& statement : statements) {
            auto& [key, operands, matchedObjects, keysOfNodes, recordedOverloads, overloadsWithRecordedOperands, hasArgumentsPastLimit, isAbandoned] = statement;
            saved.builtinCalls.append(BuiltinCallProgress {
                std::exchange(matchedObjects, { }),
                std::exchange(keysOfNodes, { }),
                std::exchange(recordedOverloads, { }),
                std::exchange(overloadsWithRecordedOperands, { }),
            });
        }
    }

    {
        auto& [hostInstance, nativeCodeStatement, holdersWithoutMembers, holdersOfRefusedMembers, membersWithRefusedWrites, membersFollowHolders, startupModulesAreExposed, everyUseIsAppliedToUnknownObject, libraryInstancesCalledBySeenCode, calleesOfInstancesAwaitingSeenCaller, statementsAwaitingSeenCaller, nodesHandedToUnknownRecipients, objectsHandedToUnknownRecipients, nativeCellsReachedThroughClass, classesWithInstanceInUnknownJavaScript] = unseenCode;
        saved.holdersWithoutMembers = holdersWithoutMembers;
        saved.holdersOfRefusedMembers = holdersOfRefusedMembers;
        auto& [prototypes, isRequested] = escapedInheritors;
        UNUSED_VARIABLE(prototypes);
    }

    {
        auto& [moduleOfRuntimeModule, modulesToEvaluate, hasCapturedExports, modulesExportingAgain, isEvaluatingModules, everyModuleIsExposed, libraryModulesAreOpenToUnseenCode, everyModuleObjectIsExposed] = modules;
        RELEASE_ASSERT(modulesToEvaluate.isEmpty() && !isEvaluatingModules);
        saved.hasCapturedExports = hasCapturedExports;
        hasCapturedExports.clearAll();
    }

    {
        auto& [moduleOrder, scopes, accessesByFunction, computedFor, locationsReadBeforeInitialization] = order;
        saved.locationOrders = scopes.orders;
        saved.locationsReadBeforeInitialization = locationsReadBeforeInitialization;
        computedFor = std::nullopt;
    }
}

void FlowSolver::FixpointCheck::compareAndRestoreProgress()
{
    Progress& saved = *m_progress;
    FlowCallState& calls = *m_solver.m_states.calls;
    FlowPropertyState& properties = *m_solver.m_states.properties;
    begin({ });

    for (size_t index = 0; index < saved.linkedRequests.size(); ++index) {
        FlowLinkedRequest& request = calls.links.linkedRequests[index];
        LinkedRequestProgress& before = saved.linkedRequests[index];

        auto report = [&](const String& whatIsGained) {
            addChange({ }, makeString("request "_s, index, " at v"_s, request.origin ? request.origin.index() : 0, ' ', whatIsGained));
        };
        if (!request.closures.isSubsetOf(before.closures)) {
            FlowObjectSet added = request.closures;
            added.subtract(before.closures);
            report(makeString("gets callees "_s, toString(added)));
        }
        for (FunctionID function : request.functionsWithoutClosure) {
            if (!before.functionsWithoutClosure.contains(function))
                report(makeString("gets callee f"_s, function.index()));
        }
        if (request.hasUnknownCallee && !before.hasUnknownCallee)
            report("gets an unknown callee"_s);
        if (request.allocatesThis && !before.allocatesThis)
            report("allocates for the values of new.target"_s);
        if (request.everyConstructorHasOtherNewTarget && !before.everyConstructorHasOtherNewTarget)
            report("passes every constructor another new.target"_s);

        request.closures = WTF::move(before.closures);
        request.functionsWithoutClosure = WTF::move(before.functionsWithoutClosure);
        request.hasUnknownCallee = before.hasUnknownCallee;
        request.predictedCallees = WTF::move(before.predictedCallees);
        request.hasTooManyCalleesToPredict = before.hasTooManyCalleesToPredict;
        request.allocatesThis = before.allocatesThis;
        request.everyConstructorHasOtherNewTarget = before.everyConstructorHasOtherNewTarget;
        request.onlyNewTarget = before.onlyNewTarget;
    }

    for (size_t index = 0; index < saved.builtinCalls.size(); ++index) {
        FlowBuiltinCallStatement& statement = calls.builtins.statements[index];
        BuiltinCallProgress& before = saved.builtinCalls[index];
        for (BuiltinOverloadID overload : statement.recordedOverloads) {
            if (!before.recordedOverloads.contains(overload))
                addChange({ }, makeString("BuiltinCall "_s, index, " selects overload "_s, overload.index()));
        }
        statement.matchedObjects = WTF::move(before.matchedObjects);
        statement.keysOfNodes = WTF::move(before.keysOfNodes);
        statement.recordedOverloads = WTF::move(before.recordedOverloads);
        statement.overloadsWithRecordedOperands = WTF::move(before.overloadsWithRecordedOperands);
    }

    for (auto& [key, type] : calls.builtins.typedResultTypes) {
        auto before = saved.typedResultTypes.find(key);
        if (before == saved.typedResultTypes.end() || before->value != type)
            addChange({ }, makeString('v', key.second.index(), " in i"_s, key.first.index(), " gets another typed result"_s));
    }

    const Vector<FlowLocationOrder>& orders = calls.order.scopes.orders;
    for (size_t location = 0; location < orders.size(); ++location) {
        if (location >= saved.locationOrders.size() || orders[location] != saved.locationOrders[location])
            addChange({ }, makeString("scope location "_s, location, " gets another order of initialization and stores"_s));
    }
    for (size_t location : calls.order.locationsReadBeforeInitialization) {
        if (!saved.locationsReadBeforeInitialization.get(location))
            addChange({ }, makeString("scope location "_s, location, " is read before its initialization"_s));
    }
    for (size_t location : properties.constructions.withdrawnFields) {
        if (!saved.withdrawnFields.get(location))
            addChange(FlowNodeID(static_cast<uint32_t>(location)), makeString(nameOf(FlowNodeID(static_cast<uint32_t>(location))), " is no longer set at creation"_s));
    }

    calls.links.recordedUnknownCallees = WTF::move(saved.recordedUnknownCallees);
    calls.operandFlows.recordedFlows = WTF::move(saved.recordedFlows);
    calls.builtins.recordedOperandValues = WTF::move(saved.recordedOperandValues);
    calls.builtins.typedResultTypes = WTF::move(saved.typedResultTypes);
    calls.builtins.builtinsWithEnteredAttachments = WTF::move(saved.builtinsWithEnteredAttachments);
    calls.iteration.steppedIterators = WTF::move(saved.steppedIterators);
    calls.unseenCode.holdersWithoutMembers = WTF::move(saved.holdersWithoutMembers);
    calls.unseenCode.holdersOfRefusedMembers = WTF::move(saved.holdersOfRefusedMembers);
    calls.modules.hasCapturedExports = WTF::move(saved.hasCapturedExports);
    properties.keysOfElementAccesses = WTF::move(saved.keysOfElementAccesses);
    properties.formsOfDescriptors = WTF::move(saved.formsOfDescriptors);
    properties.copiesWithOneObjectPerArgument = WTF::move(saved.copiesWithOneObjectPerArgument);
    properties.constructions.withdrawnFields = WTF::move(saved.withdrawnFields);

    m_solver.m_records.forEachAdditionSince(saved.records, [&](const String& addition) {
        addChange({ }, addition);
    });
    m_solver.m_records = WTF::move(saved.records);
    m_solver.m_counters = saved.counters;
}

template<typename T, size_t inlineCapacity>
static void appendMissing(Vector<T, inlineCapacity>& list, const Vector<T, inlineCapacity>& earlier)
{
    for (const T& element : earlier) {
        if (!list.contains(element))
            list.append(element);
    }
}

void FlowSolver::FixpointCheck::repeatRoots()
{
    begin({ .rule = Rule::Roots });
    addRoots(m_solver);
}

void FlowSolver::FixpointCheck::repeatUnseenCode()
{
    FlowModuleState& modules = m_solver.m_states.calls->modules;
    Vector<UnseenCodeRecord> causes(m_solver.m_records.unseenCode());
    modules.everyModuleIsExposed = false;
    modules.libraryModulesAreOpenToUnseenCode = false;
    modules.everyModuleObjectIsExposed = false;
    m_solver.m_records.retractUnknownJavaScript();

    for (const UnseenCodeRecord& record : causes) {
        begin({ .rule = Rule::UnseenCode, .index = static_cast<uint32_t>(record.cause) });
        raiseUnseenCode(m_solver, record.cause, record.builtin == Wire::none ? BuiltinID() : BuiltinID(record.builtin), record.witness);
    }

    bool& constructsForOtherNewTarget = m_solver.m_states.calls->links.constructsUnknownCalleeForOtherNewTarget;
    if (std::exchange(constructsForOtherNewTarget, false)) {
        begin({ .rule = Rule::OtherNewTarget });
        constructEscapedFunctionsForOtherNewTarget(m_solver, noWitness);
    }
}

void FlowSolver::FixpointCheck::repeatBuiltinUses()
{
    const BuiltinTable& builtins = m_solver.builtins();
    for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
        BuiltinID builtin(index);
        FlowBuiltinUse use = m_solver.m_records.retractBuiltinUse(builtin);
        if (use.uses.isEmpty())
            continue;

        begin({ .rule = Rule::BuiltinUse, .index = index });
        m_solver.m_states.calls->builtins.builtinsWithEnteredAttachments.clear(builtins.canonicalID(builtin).index());
        noteBuiltinUse(m_solver, builtin, use.uses, use.firstUse);
    }
}

void FlowSolver::FixpointCheck::repeatModuleEvaluations()
{
    BitVector evaluatedModules = m_solver.m_records.evaluatedModules();
    for (size_t index : evaluatedModules) {
        ModuleID module(static_cast<uint32_t>(index));
        begin({ .rule = Rule::ModuleEvaluation, .index = module.index() });
        m_solver.m_records.retractEvaluatedModule(module);
        evaluateModule(m_solver, module, noWitness);
    }
}

void FlowSolver::FixpointCheck::repeatArrivalsInUnseenCode()
{
    const FlowGraph& graph = m_solver.m_graph;
    FlowObjectSet objectsInNativeCode = m_solver.valueOf(graph.nativeCode()).objects;
    objectsInNativeCode.forEach([&](FlowObjectID object) {
        if (object == unknownFlowObject)
            return;
        begin({ .rule = Rule::ArrivalInNativeCode, .object = object });
        didReachNativeCode(m_solver, object);
    });

    FlowObjectSet objectsInUnknownJavaScript = m_solver.valueOf(graph.unknownJavaScript()).objects;
    objectsInUnknownJavaScript.forEach([&](FlowObjectID object) {
        if (object == unknownFlowObject)
            return;
        begin({ .rule = Rule::ArrivalInUnknownJavaScript, .object = object });
        didReachUnknownJavaScript(m_solver, object);
    });
}

void FlowSolver::FixpointCheck::repeatLocationCreations()
{
    const FlowGraph& graph = m_solver.m_graph;
    auto repeat = [&](FlowObjectID object, PropertyKeyID key, FlowNodeID node) {
        begin({ .rule = Rule::LocationCreation, .node = node, .object = object });
        didCreateLocation(m_solver, object, key, node);
        // The unknown object has no arrival in native code: what is stored through it is handed to native code by the store.
        if (object != unknownFlowObject && m_solver.hasReachedNativeCode(object))
            exposeLocation(m_solver, object, node);
    };

    for (uint32_t index = 0, count = m_solver.m_objects.size(); index < count; ++index) {
        FlowObjectID object(index);
        for (FlowOpenPart part : { FlowOpenPart::PrototypeLink, FlowOpenPart::IndexedProperties, FlowOpenPart::StringProperties, FlowOpenPart::SymbolProperties }) {
            if (FlowNodeID node = graph.find(object, part))
                repeat(object, PropertyKeyID(), node);
        }

        Vector<FlowProperty> properties(graph.propertiesOf(object));
        for (const FlowProperty& property : properties) {
            if (property.value && graph.kindOf(property.value) == FlowNodeKind::Property)
                repeat(object, property.key, property.value);
            if (property.getter)
                repeat(object, property.key, property.getter);
            if (property.setter)
                repeat(object, property.key, property.setter);
        }
    }
}

void FlowSolver::FixpointCheck::repeatUnnamedKeyStores()
{
    FlowPropertyState& properties = *m_solver.m_states.properties;
    for (FlowOpenPart part : { FlowOpenPart::IndexedProperties, FlowOpenPart::StringProperties, FlowOpenPart::SymbolProperties }) {
        BitVector& isLinked = properties.objectsWithLinkedUnnamedKeys[static_cast<unsigned>(part)];
        BitVector linkedObjects = isLinked;
        for (size_t index : linkedObjects) {
            FlowObjectID object(static_cast<uint32_t>(index));
            begin({ .rule = Rule::UnnamedKeyStores, .object = object });
            isLinked.clear(index);
            linkUnnamedKeyStores(m_solver, object, part);
        }
    }
}

void FlowSolver::FixpointCheck::repeatMovedElements()
{
    for (uint32_t index = 0, count = m_solver.m_objects.size(); index < count; ++index) {
        FlowObjectID object(index);
        if (!m_solver.m_records.eventsOf(object).contains(FlowObjectEvent::ElementsMoved))
            continue;
        begin({ .rule = Rule::MovedElements, .object = object });
        Witness witness = m_solver.m_records.witnessOf(object, FlowObjectEvent::ElementsMoved);
        m_solver.m_records.retractEvent(object, FlowObjectEvent::ElementsMoved);
        joinPositions(m_solver, object, witness);
    }
}

void FlowSolver::FixpointCheck::checkAbandonedInstance(FlowInstanceID instance)
{
    FlowGraph& graph = m_solver.m_graph;
    const FlowSummary& summary = graph.summaryOf(instance);
    begin({ .rule = Rule::AbandonedInstance, .site = { instance, { } } });

    FlowObjectSet objects;
    for (uint32_t site = 0; site < summary.sites.size(); ++site)
        objects.add(graph.objectOf(instance, site));
    for (uint32_t index = 0; index < summary.numberOfLocals; ++index) {
        FlowNodeID local = graph.nodeOf(instance, FlowLocalID(index));
        objects.unite(graph.valueOf(local).objects);

        TypeID type = graph.typeAsRead(local);
        FlowValue widest = type ? m_solver.values().valueOfType(type) : m_solver.values().unknownValue();
        if (!m_solver.values().isSubsetOf(widest, graph.valueOf(local)))
            addChange(local, makeString(nameOf(local), " holds less than its type allows"_s));
    }
    m_solver.reach(graph.unknownJavaScript(), objects, noWitness);
}

void FlowSolver::FixpointCheck::repeatInstances()
{
    FlowGraph& graph = m_solver.m_graph;
    for (uint32_t index = 0, count = graph.numberOfInstances(); index < count; ++index) {
        FlowInstanceID instance(index);
        switch (graph.at(instance).state) {
        case FlowInstanceState::Created:
            begin({ .rule = Rule::Seeds, .site = { instance, { } } });
            addChange({ }, "the instance was never initialized"_s);
            continue;
        case FlowInstanceState::PastWorkLimit:
            checkAbandonedInstance(instance);
            continue;
        case FlowInstanceState::Initialized:
            break;
        }

        const FlowSummary& summary = graph.summaryOf(instance);
        begin({ .rule = Rule::Seeds, .site = { instance, { } } });
        for (const FlowSeed& seed : summary.seeds)
            m_solver.join(m_solver.node(instance, seed.node()), m_solver.valueOfSeed(instance, seed), noWitness);
        for (auto& [from, to] : summary.copiesFromExternals)
            m_solver.connect(m_solver.node(instance, from), m_solver.node(instance, to));

        summary.statements.forEach([&](FlowStatementReference statement) {
            m_solver.run(FlowStatementSite { instance, statement });
        });
    }
}

void FlowSolver::FixpointCheck::repeatBuiltinCalls()
{
    FlowBuiltinCallState& builtins = m_solver.m_states.calls->builtins;
    for (uint32_t index = 0, count = builtins.statements.size(); index < count; ++index) {
        FlowBuiltinCallStatement& statement = builtins.statements[index];
        FlowStatementSite site { statement.key.summaryStatement.instance, { FlowStatementKind::BuiltinCall, index } };
        if (!statement.isAbandoned) {
            m_solver.run(site);
            continue;
        }
        begin({ .rule = Rule::AbandonedCall, .site = site });
        statement.isAbandoned = false;
        abandonBuiltinCall(m_solver, site);
    }
}

void FlowSolver::FixpointCheck::repeatEdges()
{
    FlowGraph& graph = m_solver.m_graph;
    for (uint32_t index = 0, count = graph.numberOfNodes(); index < count; ++index) {
        FlowNodeID node(index);
        FlowValue value = graph.valueOf(node);
        if (value.isEmpty())
            continue;

        begin({ .rule = Rule::Edges, .node = node });
        std::optional<FlowValue> valueWithoutEscapedObjects;
        auto valueFor = [&](FlowNodeID target) -> const FlowValue& {
            if (!m_solver.listsNoMoreEscapedObjects(target, value))
                return value;
            if (!valueWithoutEscapedObjects) {
                valueWithoutEscapedObjects = value.withoutObjects();
                valueWithoutEscapedObjects->objects = m_solver.withoutEscapedObjects(value.objects);
            }
            return *valueWithoutEscapedObjects;
        };
        graph.forEachSuccessor(node, [&](FlowNodeID target, TypeID filter) {
            const FlowValue& offered = valueFor(target);
            m_solver.propagate(target, filter ? m_solver.values().filter(offered, m_solver.m_program.at(filter)) : offered, noWitness);
        });

        FlowNodeKind kind = graph.kindOf(node);
        if (kind == FlowNodeKind::NativeCode || kind == FlowNodeKind::UnknownJavaScript)
            continue;
        m_solver.m_waiters.onNodes.forEachWaiterOn(node, [&](const NodeWaiter& waiter) {
            if (graph.at(waiter.site.instance).state == FlowInstanceState::PastWorkLimit)
                m_solver.reach(graph.unknownJavaScript(), value.objects, noWitness);
        });
    }
}

void FlowSolver::FixpointCheck::repeatRefinements()
{
    while (true) {
        m_solver.drainQueues();
        begin({ .rule = Rule::Refinement });
        FlowProgress constructions = refineConstructions(m_solver);
        FlowProgress order = refineExecutionOrder(m_solver);
        FlowProgress inheritors = refineEscapedInheritors(m_solver);
        if (constructions == FlowProgress::Finished && order == FlowProgress::Finished && inheritors == FlowProgress::Finished && !m_solver.hasQueuedWork())
            return;
    }
}

void FlowSolver::FixpointCheck::repeatEveryRule()
{
    m_solver.m_build.setRecordsLimitHits(false);
    forgetProgress();
    m_sizes = sizes();
    m_solver.m_fixpointCheck = this;

    repeatRoots();
    repeatUnseenCode();
    repeatBuiltinUses();
    repeatModuleEvaluations();
    repeatArrivalsInUnseenCode();
    repeatLocationCreations();
    repeatUnnamedKeyStores();
    repeatMovedElements();
    repeatInstances();
    repeatBuiltinCalls();
    repeatEdges();
    repeatRefinements();
    compareAndRestoreProgress();

    m_solver.m_fixpointCheck = nullptr;
    m_solver.m_build.setRecordsLimitHits(true);
}

// The statement that lends native code its site does nothing when it runs, so whoever waits under that site is never told.
void FlowSolver::FixpointCheck::reportWaitsOfNativeCode()
{
    FlowStatementSite site = nativeCodeSite(m_solver);
    auto numberOfWaitersIn = [&](const auto& table) {
        return std::ranges::count(table.waiters, site, &Waiter::site);
    };
    const Waiters& waiters = m_solver.m_waiters;
    auto numberOfWaiters = numberOfWaitersIn(waiters.onNodes) + numberOfWaitersIn(waiters.onMutations) + numberOfWaitersIn(waiters.onProperties) + numberOfWaitersIn(waiters.onNamedProperties);
    if (!numberOfWaiters)
        return;
    m_step = { .rule = Rule::Statement, .site = site };
    addChange({ }, makeString("waits in "_s, numberOfWaiters, " places and does nothing when it is woken"_s));
    m_step = { };
}

Vector<FlowFixpointChange> FlowSolver::FixpointCheck::run()
{
    RELEASE_ASSERT(!m_solver.hasQueuedWork() && !m_solver.m_fixpointCheck);
    reportWaitsOfNativeCode();
    repeatEveryRule();

    if (m_numberOfChanges > m_changes.size())
        m_changes.last().description = makeString(m_changes.last().description, ", and "_s, m_numberOfChanges - m_changes.size(), " more changes"_s);
    return WTF::move(m_changes);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
