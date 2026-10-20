/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowSinkRules.h"

#if ENABLE(AOT)

#include "AOTFlowSummary.h"
#include "AOTLayout.h"
#include "AOTOpcode.h"
#include "AOTThreads.h"
#include <algorithm>
#include <array>
#include <initializer_list>
#include <optional>
#include <span>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

class SinkRuleWriter {
public:
    SinkRuleWriter(const FlowSolution& solution, const SinkNodeNumbering& numbering, CollectedSinkRules& rules)
        : m_graph(solution.graph)
        , m_records(solution.records)
        , m_numbering(numbering)
        , m_rules(rules)
        , m_nativeCodeUses(solution.records.mayRunUnknownJavaScript() ? OperandUses::all() : nativeCodeUses)
    {
    }

    void addUses(FlowNodeID node, OperandUses uses)
    {
        if (node)
            m_rules.baseUses.append(BaseUses { m_numbering.graphNode(node), uses });
    }

    void addEveryUse(FlowNodeID node) { addUses(node, OperandUses::all()); }
    void addNativeCodeUses(FlowNodeID node) { addUses(node, m_nativeCodeUses); }

    void addOperandOfNativeCode(FlowNodeID node)
    {
        if (node)
            m_rules.operandsOfNativeCode.append(m_numbering.graphNode(node));
    }

    void addDependency(FlowNodeID subject, FlowNodeID source, SinkTransform transform)
    {
        if (subject && source)
            addRule(m_numbering.graphNode(subject), m_numbering.graphNode(source), transform);
    }

    void addAllocation(FlowObjectID object, FlowNodeID node)
    {
        if (node && object != unknownFlowObject)
            addRule(m_numbering.lifetime(object), m_numbering.graphNode(node), SinkTransform::Same);
    }

    // The unknown object stands for every object that has reached native code.
    void addUnknownObjectLifetime()
    {
        m_rules.baseUses.append(BaseUses { m_numbering.lifetime(unknownFlowObject), m_nativeCodeUses });
    }

    void addClosureParameter(FlowObjectID closure, FlowNodeID parameter)
    {
        addRule(m_numbering.lifetime(closure), m_numbering.graphNode(parameter), SinkTransform::Leaves);
    }

    void addCopiedContents(FlowNodeID source, FlowObjectID target)
    {
        if (source)
            addRule(m_numbering.graphNode(source), m_numbering.lifetime(target), SinkTransform::Contents);
    }

    void addStoreIntoOuterMemory(FlowNodeID stored)
    {
        if (stored)
            addRule(m_numbering.graphNode(stored), m_numbering.outerMemory(), SinkTransform::Lifetime);
    }

    void addResultHub(FlowCallLinkID link, FlowNodeID result)
    {
        addRule(m_numbering.resultHub(link), m_numbering.graphNode(result), SinkTransform::Same);
    }

    void addOperandFlow(const FlowOperandFlow& flow)
    {
        if (!flow.from || !flow.parameter)
            return;

        // An argument that a rest parameter takes is passed to a part of the rest array.
        if (!isEntryParameter(flow.parameter)) {
            addSuccessor(flow.from, flow.parameter);
            return;
        }

        addDependency(flow.from, flow.parameter, SinkTransform::Argument);
        if (!m_records.at(flow.link).result) {
            addDependency(flow.from, flow.parameter, SinkTransform::Leaves);
            return;
        }
        SinkDependency usesOfResult { m_numbering.resultHub(flow.link), m_numbering.graphNode(flow.parameter), SinkTransform::IfReturned };
        m_rules.dependencies.append(SinkRule { m_numbering.graphNode(flow.from), usesOfResult });
    }

    void addSuccessor(FlowNodeID node, FlowNodeID successor)
    {
        switch (m_graph.kindOf(successor)) {
        case FlowNodeKind::UnknownJavaScript:
            addEveryUse(node);
            return;
        case FlowNodeKind::NativeCode:
            addNativeCodeUses(node);
            return;
        case FlowNodeKind::Variable:
        case FlowNodeKind::Heritage:
            addUses(node, OperandUse::Retains);
            addDependency(node, successor, SinkTransform::Same);
            return;
        case FlowNodeKind::DeclaredPart:
        case FlowNodeKind::Position:
        case FlowNodeKind::UndeclaredPart:
        case FlowNodeKind::PrototypeLink:
        case FlowNodeKind::IndexedProperties:
        case FlowNodeKind::StringProperties:
        case FlowNodeKind::SymbolProperties:
        case FlowNodeKind::Property:
        case FlowNodeKind::Getter:
        case FlowNodeKind::Setter:
            addPartSuccessor(node, successor);
            return;
        case FlowNodeKind::Temporary:
            addDependency(node, successor, SinkTransform::Same);
            return;
        case FlowNodeKind::Local:
            addLocalSuccessor(node, successor);
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

private:
    void addRule(uint32_t subject, uint32_t source, SinkTransform transform)
    {
        m_rules.dependencies.append(SinkRule { subject, SinkDependency { source, source, transform } });
    }

    bool isEntryParameter(FlowNodeID node) const
    {
        if (m_graph.kindOf(node) != FlowNodeKind::Local)
            return false;
        return m_graph.localOf(node).index() < m_graph.summaryOf(m_graph.instanceOf(node)).numberOfEntryParameters;
    }

    void addPartSuccessor(FlowNodeID node, FlowNodeID part)
    {
        addDependency(node, part, SinkTransform::Same);
        FlowObjectID object = m_graph.objectOf(part);
        if (object == unknownFlowObject)
            addUses(node, { OperandUse::Retains, OperandUse::PassesToUserCode });
        else if (node)
            addRule(m_numbering.graphNode(node), m_numbering.lifetime(object), SinkTransform::Lifetime);
    }

    void addLocalSuccessor(FlowNodeID node, FlowNodeID successor)
    {
        const FlowSummary& summary = m_graph.summaryOf(m_graph.instanceOf(successor));
        FlowLocalID local = m_graph.localOf(successor);
        // An operand flow says under which link the parameter gets the value.
        if (local.index() < summary.numberOfEntryParameters)
            return;
        if (local.index() < summary.numberOfEntryParameters + summary.numberOfResults) {
            addUses(node, OperandUse::Returns);
            return;
        }
        if (local == summary.thrown()) {
            addUses(node, { OperandUse::Retains, OperandUse::PassesToUserCode });
            return;
        }
        addDependency(node, successor, SinkTransform::Same);
    }

    const FlowGraph& m_graph;
    const FlowRecords& m_records;
    const SinkNodeNumbering& m_numbering;
    CollectedSinkRules& m_rules;
    OperandUses m_nativeCodeUses;
};

void collectProgramNodeRules(const FlowSolution& solution, SinkRuleWriter& writer)
{
    const FlowGraph& graph = solution.graph;
    writer.addNativeCodeUses(graph.nativeCode());
    writer.addEveryUse(graph.unknownJavaScript());
    writer.addUnknownObjectLifetime();

    for (unsigned index = 0; index < graph.numberOfNodes(); ++index) {
        FlowNodeID node(index);
        FlowNodeKind kind = graph.kindOf(node);
        if (kind == FlowNodeKind::Local || kind == FlowNodeKind::NativeCode || kind == FlowNodeKind::UnknownJavaScript)
            continue;
        graph.forEachSuccessor(node, [&](FlowNodeID successor, TypeID) {
            writer.addSuccessor(node, successor);
        });
    }

    std::span<const FlowCallLink> links = solution.records.callLinks();
    for (size_t index = 0; index < links.size(); ++index) {
        if (links[index].result)
            writer.addResultHub(FlowCallLinkID(index), links[index].result);
    }

    for (const FlowContentsRead& read : solution.records.contentsReads())
        writer.addDependency(read.container, read.reader, SinkTransform::Contents);
}

struct AccessOperands {
    FlowAccessKind kind { FlowAccessKind::Load };
    OperandUse storeUse { OperandUse::StoresByKey };
    ValueID origin;
    FlowLocalID receiver;
    FlowLocalID key;
    FlowLocalID value;
    FlowLocalID thisValue;
    bool mayLoadFromReceiver { true };
};

class InstanceRuleCollector {
public:
    InstanceRuleCollector(const FlowFamilyInputs& inputs, FlowInstanceID instance, SinkRuleWriter& writer)
        : m_solution(inputs.solution)
        , m_graph(inputs.solution.graph)
        , m_calls(inputs.calls)
        , m_writer(writer)
        , m_instance(instance)
        , m_holder(m_graph.at(instance).function)
        , m_summary(m_graph.summaryOf(instance))
    {
    }

    void collect()
    {
        if (m_solution.isPastWorkLimit(m_holder)) {
            for (uint32_t index = 0; index < m_summary.numberOfLocals; ++index)
                m_writer.addEveryUse(nodeOf(FlowLocalID(index)));
            return;
        }
        collectEntryParameters();
        collectAllocations();
        collectLocalNodes();
        collectStatements();
    }

private:
    FlowNodeID nodeOf(FlowLocalID local) const
    {
        return m_graph.nodeOf(m_instance, local);
    }

    // The statements of the host summary have no origin, so a link cannot be matched with one of them.
    std::span<const FlowCallLinkID> linksAt(ValueID origin) const
    {
        return origin ? m_calls.linksAt(m_instance, origin) : std::span<const FlowCallLinkID>();
    }

    void collectEntryParameters()
    {
        if (m_graph.isHost(m_holder))
            return;

        const Function& function = m_solution.program.at(m_holder);
        if (function.isUnsplitCoroutine()) {
            for (uint32_t index = 0; index < m_summary.numberOfEntryParameters; ++index)
                m_writer.addUses(nodeOf(m_summary.entryParameter(index)), OperandUse::Retains);
        }

        // A call passes the closure to these parameters of its own function without an edge.
        for (std::optional<unsigned> parameter : { function.calleeParameterIndex(), function.newTargetParameterIndex() }) {
            if (parameter)
                addClosuresIn(nodeOf(m_summary.entryParameter(*parameter)));
        }
    }

    void addClosuresIn(FlowNodeID parameter)
    {
        m_graph.valueOf(parameter).objects.forEach([&](FlowObjectID object) {
            if (m_solution.objects.kindOf(object) == FlowObjectKind::Closure)
                m_writer.addClosureParameter(object, parameter);
        });
    }

    void collectAllocations()
    {
        for (const FlowSeed& seed : m_summary.seeds)
            addSeed(seed);
    }

    void addSeed(const FlowSeed& seed)
    {
        if (seed.kind() == FlowSeedKind::Site)
            m_writer.addAllocation(m_graph.objectOf(m_instance, seed.site()), nodeOf(seed.node()));
    }

    void collectLocalNodes()
    {
        for (uint32_t index = 0; index < m_summary.numberOfLocals; ++index) {
            FlowNodeID node = nodeOf(FlowLocalID(index));
            m_graph.forEachSuccessor(node, [&](FlowNodeID successor, TypeID) {
                m_writer.addSuccessor(node, successor);
            });
        }
    }

    void collectStatements()
    {
        const FlowStatements& statements = m_summary.statements;
#define VISIT_FLOW_STATEMENTS(kind, type, member, cursors) \
        for (const type& statement : statements.member) \
            visit(statement);
        FOR_EACH_FLOW_STATEMENT_KIND(VISIT_FLOW_STATEMENTS)
#undef VISIT_FLOW_STATEMENTS
    }

    void visit(const FlowFilter& filter)
    {
        m_writer.addDependency(nodeOf(filter.from), nodeOf(filter.to), SinkTransform::Same);
    }

    void visit(const FlowConditionalSeed& conditionalSeed)
    {
        addSeed(conditionalSeed.seed);
    }

    void visit(const FlowNaturalOperation& operation)
    {
        addOperandsOfBuiltins(operation.origin, m_summary.statements.list(operation.operands));
        if (linksAt(operation.origin).empty())
            return;
        m_writer.addEveryUse(nodeOf(operation.result));
        addUsesToEach(m_summary.statements.list(operation.operands), OperandUses::all());
    }

    void visit(const FlowAllocation& allocation)
    {
        FlowNodeID prototype = nodeOf(allocation.prototype);
        if (!prototype)
            return;
        m_writer.addUses(prototype, { OperandUse::Retains, OperandUse::BecomesPrototype });

        // Such an allocation has no seed: the solver joins the object that each arriving prototype selects into the value.
        FlowNodeID result = m_graph.nodeOf(m_instance, allocation.origin);
        if (!result)
            return;
        m_graph.valueOf(result).objects.forEach([&](FlowObjectID object) {
            m_writer.addAllocation(object, result);
        });
    }

    void visit(const FlowLocationAccess&) { }

    // The uses of an environment say how long it lives. What becomes of the values in its fields is in the uses of the fields.
    void visit(const FlowFieldAccess& access)
    {
        if (m_solution.program.at(access.layout).isEnvironment()) {
            addStoredValue(access.kind, access.receiver, { }, access.value);
            return;
        }
        addAccess(AccessOperands { access.kind, OperandUse::StoresByKey, access.origin, access.receiver, { }, access.value, { } });
    }

    bool isAllocatedByActivation(FlowLocalID container) const
    {
        return findStoreContainer(m_solution.program, m_summary, container).kind == StoreContainerKind::OwnAllocation;
    }

    // The lifetime of an object says whether it leaves the frame that allocated it, and that frame can be older than the activation that stores into the object.
    void addStoredValue(FlowAccessKind kind, FlowLocalID receiver, FlowLocalID thisValue, FlowLocalID stored)
    {
        if (!stored || !isWrite(kind) || kind == FlowAccessKind::Delete)
            return;
        if (!isAllocatedByActivation(receiver) || (thisValue && !isAllocatedByActivation(thisValue)))
            m_writer.addStoreIntoOuterMemory(nodeOf(stored));
    }

    void visit(const FlowPropertyAccess& access)
    {
        AccessOperands operands { access.kind, OperandUse::StoresByKey, access.origin, access.receiver, { }, access.value, access.thisValue };
        operands.mayLoadFromReceiver = access.kind != FlowAccessKind::Load || mayFindInReceiver(access);
        addAccess(operands);
    }

    // A value found on a prototype is no part of the receiver.
    bool mayFindInReceiver(const FlowPropertyAccess& load) const
    {
        FlowNodeID receiver = nodeOf(load.receiver);
        if (!receiver)
            return false;

        FlowKeySet keys = keySetOf(m_solution.program, load.key);
        FlowPropertyWalk walk { FlowWalkOperation::Get, load.flags.contains(FlowAccessFlag::StartsAtPrototype), { }, load.flags.contains(FlowAccessFlag::IsOwnOnly) };
        return m_graph.valueOf(receiver).objects.anyOf([&](FlowObjectID object) {
            bool isFound = false;
            forEachPropertySource(m_solution, object, keys, walk, [&](const FlowPropertySource& source) {
                isFound |= source.isOwn || source.kind == FlowPropertySourceKind::Unknown;
            });
            return isFound;
        });
    }

    // The run time looks a computed key up.
    void visit(const FlowElementAccess& access)
    {
        m_writer.addOperandOfNativeCode(nodeOf(access.receiver));
        m_writer.addOperandOfNativeCode(nodeOf(access.thisValue));
        addKeyUses(access.key);
        addAccess(AccessOperands { access.kind, OperandUse::StoresByKey, access.origin, access.receiver, access.key, access.value, access.thisValue });
    }

    void visit(const FlowSlotAccess& access)
    {
        addAccess(AccessOperands { access.kind, OperandUse::WritesContents, access.origin, access.receiver, { }, access.value, { } });
    }

    void addAccess(const AccessOperands& access)
    {
        FlowNodeID receiver = nodeOf(access.receiver);
        switch (access.kind) {
        case FlowAccessKind::Load:
            if (access.mayLoadFromReceiver)
                m_writer.addDependency(receiver, nodeOf(access.value), SinkTransform::Contents);
            break;
        case FlowAccessKind::Store:
        case FlowAccessKind::Define:
        case FlowAccessKind::DefineGetter:
        case FlowAccessKind::DefineSetter:
            m_writer.addUses(receiver, { OperandUse::Inspects, access.storeUse });
            break;
        case FlowAccessKind::Delete:
            m_writer.addUses(receiver, { OperandUse::Inspects, OperandUse::DeletesByKey });
            break;
        case FlowAccessKind::Test:
            break;
        }
        addStoredValue(access.kind, access.receiver, access.thisValue, access.value);

        std::array<FlowLocalID, 4> operands { access.receiver, access.key, access.value, access.thisValue };
        bool runsOrdinaryCall = std::ranges::any_of(linksAt(access.origin), [&](FlowCallLinkID link) {
            return passesOperandsByPosition(m_solution.records.at(link).kind);
        });
        if (runsOrdinaryCall)
            addUsesToEach(operands, OperandUses::all());
        addOperandsOfBuiltins(access.origin, operands);
        addCalledClosureFlows(access.origin, operands);
    }

    static bool passesOperandsByPosition(FlowCallLinkKind kind)
    {
        switch (kind) {
        case FlowCallLinkKind::Call:
        case FlowCallLinkKind::Construct:
            return true;
        case FlowCallLinkKind::Accessor:
        case FlowCallLinkKind::BuiltinCallback:
        case FlowCallLinkKind::Protocol:
        case FlowCallLinkKind::Host:
            return false;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    void addKeyUses(FlowLocalID key)
    {
        FlowNodeID node = nodeOf(key);
        if (node && m_graph.valueOf(node).mayBeObject())
            m_writer.addEveryUse(node);
    }

    void addUsesToEach(std::span<const FlowLocalID> locals, OperandUses uses)
    {
        for (FlowLocalID local : locals)
            m_writer.addUses(nodeOf(local), uses);
    }

    void addUsesToEach(std::initializer_list<FlowLocalID> locals, OperandUses uses)
    {
        addUsesToEach(std::span<const FlowLocalID>(locals), uses);
    }

    void addEveryUse(std::initializer_list<FlowLocalID> locals)
    {
        addUsesToEach(locals, OperandUses::all());
    }

    void addUsesIfLinked(ValueID origin, std::initializer_list<FlowLocalID> locals)
    {
        if (!linksAt(origin).empty())
            addEveryUse(locals);
    }

    void addOperandsOfBuiltins(ValueID origin, std::span<const FlowLocalID> operands)
    {
        if (!origin || m_calls.builtinLinksAt(m_instance, origin).empty())
            return;
        for (FlowLocalID operand : operands)
            m_writer.addOperandOfNativeCode(nodeOf(operand));
    }

    void addOperandsOfBuiltins(ValueID origin, std::initializer_list<FlowLocalID> operands)
    {
        addOperandsOfBuiltins(origin, std::span<const FlowLocalID>(operands));
    }

    void visit(const FlowPrototypeAccess& access)
    {
        if (!isWrite(access.kind)) {
            m_writer.addDependency(nodeOf(access.object), nodeOf(access.value), SinkTransform::Contents);
            return;
        }
        m_writer.addUses(nodeOf(access.object), OperandUse::ChangesPrototype);
        m_writer.addUses(nodeOf(access.value), OperandUse::BecomesPrototype);
        addStoredValue(access.kind, access.object, { }, access.value);
    }

    void visit(const FlowPropertyCopy& copy)
    {
        FlowNodeID source = nodeOf(copy.source);
        FlowNodeID target = nodeOf(copy.target);
        m_writer.addUses(target, { OperandUse::Inspects, OperandUse::StoresByKey });
        m_writer.addUses(source, { OperandUse::Inspects, OperandUse::ReadsContents });
        addUsesIfLinked(copy.origin, { copy.source, copy.target });
        addOperandsOfBuiltins(copy.origin, { copy.source, copy.target });
        if (!isAllocatedByActivation(copy.target))
            m_writer.addUses(source, OperandUse::RetainsContents);
        if (!target)
            return;
        m_graph.valueOf(target).objects.forEach([&](FlowObjectID object) {
            m_writer.addCopiedContents(source, object);
        });
    }

    void visit(const FlowCall& call)
    {
        std::array<FlowLocalID, 4> namedOperands { call.callee, call.thisValue, call.newTarget, call.argumentList };
        std::span<const FlowLocalID> arguments = m_summary.statements.list(call.arguments);
        if (call.flags.contains(FlowCallFlag::IsTailCall)) {
            addUsesToEach(namedOperands, OperandUse::Returns);
            addUsesToEach(arguments, OperandUse::Returns);
        }
        m_writer.addUses(nodeOf(call.callee), { OperandUse::Inspects, OperandUse::Calls });
        if (call.flags.contains(FlowCallFlag::IsInCycle))
            m_writer.addUses(nodeOf(call.callee), OperandUse::CallsRepeatedly);
        addArgumentListUses(call.argumentList);
        // Where the stub of a call with a list or of a construction cannot make the call, a run-time function does.
        if (call.argumentList || call.mode != FlowCallMode::Call)
            m_writer.addOperandOfNativeCode(nodeOf(call.callee));
        addOperandsOfBuiltins(call.origin, namedOperands);
        addOperandsOfBuiltins(call.origin, arguments);
        addCalledClosureFlows(call.origin, namedOperands);
        addCalledClosureFlows(call.origin, arguments);
    }

    void addArgumentListUses(FlowLocalID argumentList)
    {
        FlowNodeID list = nodeOf(argumentList);
        if (!list)
            return;
        m_writer.addUses(list, { OperandUse::Inspects, OperandUse::ReadsContents });
        m_graph.valueOf(list).objects.forEach([&](FlowObjectID object) {
            forEachElementPart(m_graph, object, arrayIndexRange, [&](FlowNodeID part) {
                m_writer.addDependency(list, part, SinkTransform::Contents);
            });
        });
    }

    // A call hands the closure it calls to these parameters object by object, so no operand flow names the operand that holds the closure or leads to it.
    void addCalledClosureFlows(ValueID origin, std::span<const FlowLocalID> operands)
    {
        for (FlowCallLinkID link : linksAt(origin)) {
            const FlowCallLink& record = m_solution.records.at(link);
            const Function& callee = m_solution.program.at(record.callee);
            const FlowSummary& summary = m_graph.summaryOf(record.calleeInstance);
            for (std::optional<unsigned> parameter : { callee.calleeParameterIndex(), callee.newTargetParameterIndex() }) {
                if (parameter)
                    addCalledClosureFlow(link, m_graph.nodeOf(record.calleeInstance, summary.entryParameter(*parameter)), operands);
            }
        }
    }

    void addCalledClosureFlow(FlowCallLinkID link, FlowNodeID parameter, std::span<const FlowLocalID> operands)
    {
        const FlowObjectSet& closures = m_graph.valueOf(parameter).objects;
        bool mayFindClosureInContents = !passesOperandsByPosition(m_solution.records.at(link).kind);
        for (FlowLocalID operand : operands) {
            FlowNodeID from = nodeOf(operand);
            if (!from || !m_graph.valueOf(from).mayBeObject())
                continue;
            if (m_graph.valueOf(from).objects.overlaps(closures))
                m_writer.addOperandFlow(FlowOperandFlow { from, parameter, link });
            else if (mayFindClosureInContents)
                m_writer.addDependency(from, parameter, SinkTransform::Contents);
        }
    }

    void visit(const FlowDirectCall& call)
    {
        if (call.flags.contains(FlowCallFlag::IsTailCall))
            addUsesToEach(m_summary.statements.list(call.arguments), OperandUse::Returns);
    }

    void visit(const FlowClassDefinition& definition)
    {
        addUsesToEach({ definition.constructor, definition.heritage, definition.prototypeParent }, OperandUse::Retains);
    }

    void visit(const FlowMethodDefinition& definition)
    {
        m_writer.addUses(nodeOf(definition.function), OperandUse::Retains);
    }

    void visit(const FlowAwait& await)
    {
        FlowNodeID value = nodeOf(await.value);
        m_writer.addDependency(value, nodeOf(await.result), SinkTransform::Same);
        m_writer.addUses(value, OperandUse::Retains);
        addUsesIfLinked(await.origin, { await.value, await.result });
    }

    // The step that opens an iterator names the iterable, the steps that resume it name the value. The state is the local they share.
    void visit(const FlowIteration& iteration)
    {
        FlowNodeID iterable = nodeOf(iteration.iterable);
        FlowNodeID state = nodeOf(iteration.state);
        FlowNodeID value = nodeOf(iteration.value);
        m_writer.addDependency(iterable, state, SinkTransform::Same);
        m_writer.addDependency(iterable, value, SinkTransform::Contents);
        m_writer.addDependency(state, value, SinkTransform::Contents);
        addUsesIfLinked(iteration.origin, { iteration.iterable, iteration.state, iteration.sent });
        addOperandsOfBuiltins(iteration.origin, { iteration.iterable, iteration.state, iteration.sent });
    }

    void visit(const FlowOperandUse& use)
    {
        addOperandsOfBuiltins(use.origin, singleElementSpan(use.operand));
        addCalledClosureFlows(use.origin, singleElementSpan(use.operand));
    }

    void visit(const FlowModuleOperation&) { }
    void visit(const FlowUnseenCodeEvent&) { }

    const FlowSolution& m_solution;
    const FlowGraph& m_graph;
    const FlowCallIndex& m_calls;
    SinkRuleWriter& m_writer;
    FlowInstanceID m_instance;
    FunctionID m_holder;
    const FlowSummary& m_summary;
};

Vector<OperandUses> computeBaseUses(const FlowRecords& records, const SinkNodeNumbering& numbering, const Vector<CollectedSinkRules>& collected)
{
    Vector<OperandUses> base(numbering.size(), [&](size_t index) {
        return index < numbering.numberOfGraphNodes ? records.directUsesOf(FlowNodeID(index)) : OperandUses { };
    });
    for (const CollectedSinkRules& rules : collected) {
        for (const BaseUses& uses : rules.baseUses)
            base[uses.subject].add(uses.uses);
    }
    return base;
}

// Built-in objects, prototypes, namespaces and the objects the solver allocates at a call have no allocation node to follow. An object that stands for a layout takes such allocations too.
void retainUntrackedAllocations(const FlowObjectTable& objects, const SinkNodeNumbering& numbering, SinkEquations& equations)
{
    for (uint32_t index = 0; index < numbering.numberOfObjects; ++index) {
        FlowObjectID object(index);
        uint32_t lifetime = numbering.lifetime(object);
        bool hasAllocation = std::ranges::any_of(equations.dependenciesOf(lifetime), [](const SinkDependency& dependency) {
            return dependency.transform == SinkTransform::Same;
        });
        if (!hasAllocation || objects.at(object).standsForLayout)
            equations.base[lifetime].add(OperandUse::Retains);
    }
}

Vector<OperandUses> computeBaseOfFramePointers(const SinkNodeNumbering& numbering, const SinkEquations& equations, const Vector<CollectedSinkRules>& collected)
{
    Vector<OperandUses> base(numbering.size(), [&](size_t node) {
        OperandUses uses;
        for (const SinkDependency& dependency : equations.dependenciesOf(node)) {
            if (dependency.transform == SinkTransform::Lifetime)
                uses.add(OperandUse::Retains);
            if (dependency.transform == SinkTransform::Contents && numbering.kindOf(dependency.node) == SinkNodeKind::ObjectLifetime)
                uses.add(OperandUse::RetainsContents);
        }
        return uses;
    });
    for (const CollectedSinkRules& rules : collected) {
        for (uint32_t operand : rules.operandsOfNativeCode)
            base[operand].add(OperandUse::Retains);
    }
    return base;
}

} // anonymous namespace

SinkNodeNumbering numberSinkNodes(const FlowSolution& solution)
{
    uint64_t numberOfGraphNodes = solution.graph.numberOfNodes();
    uint64_t numberOfObjects = solution.objects.size();
    uint64_t numberOfCallLinks = solution.records.callLinks().size();
    RELEASE_ASSERT(numberOfGraphNodes + numberOfObjects + numberOfCallLinks < UINT32_MAX);
    return { static_cast<uint32_t>(numberOfGraphNodes), static_cast<uint32_t>(numberOfObjects), static_cast<uint32_t>(numberOfCallLinks) };
}

Vector<CollectedSinkRules> collectSinkRules(const FlowFamilyInputs& inputs, const SinkNodeNumbering& numbering)
{
    constexpr size_t flowsPerTask = 4096;

    const FlowSolution& solution = inputs.solution;
    std::span<const FlowOperandFlow> flows = solution.records.operandFlows();
    unsigned numberOfInstances = solution.graph.numberOfInstances();
    unsigned firstFlowTask = numberOfInstances + 1;
    unsigned numberOfFlowTasks = static_cast<unsigned>((flows.size() + flowsPerTask - 1) / flowsPerTask);

    Vector<CollectedSinkRules> collected(firstFlowTask + numberOfFlowTasks);
    forEachIndexInParallel(inputs.loops(), collected.size(), [&](unsigned index) {
        SinkRuleWriter writer(solution, numbering, collected[index]);
        if (index < numberOfInstances) {
            InstanceRuleCollector(inputs, FlowInstanceID(index), writer).collect();
            return;
        }
        if (index == numberOfInstances) {
            collectProgramNodeRules(solution, writer);
            return;
        }
        size_t first = (index - firstFlowTask) * flowsPerTask;
        for (const FlowOperandFlow& flow : flows.subspan(first, std::min(flowsPerTask, flows.size() - first)))
            writer.addOperandFlow(flow);
    });
    return collected;
}

SinkEquations buildSinkEquations(const FlowFamilyInputs& inputs, const SinkNodeNumbering& numbering)
{
    Vector<CollectedSinkRules> collected = collectSinkRules(inputs, numbering);

    SinkEquations equations;
    equations.base = computeBaseUses(inputs.records(), numbering, collected);

    equations.offsets.fill(0, numbering.size() + 1);
    for (const CollectedSinkRules& rules : collected) {
        for (const SinkRule& rule : rules.dependencies)
            ++equations.offsets[rule.subject + 1];
    }
    for (uint32_t node = 0; node < numbering.size(); ++node)
        equations.offsets[node + 1] += equations.offsets[node];

    Vector<uint32_t> cursors = equations.offsets;
    equations.dependencies.grow(equations.offsets.last());
    for (const CollectedSinkRules& rules : collected) {
        for (const SinkRule& rule : rules.dependencies)
            equations.dependencies[cursors[rule.subject]++] = rule.dependency;
    }

    retainUntrackedAllocations(inputs.objects(), numbering, equations);
    equations.base[numbering.outerMemory()].add(OperandUse::Retains);
    equations.sinks = equations.base;
    equations.baseOfFramePointers = computeBaseOfFramePointers(numbering, equations, collected);
    return equations;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
