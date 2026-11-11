/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypeOrigins.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTBuiltinsInReach.h"
#include "AOTIRProgram.h"
#include "AOTLinkFlow.h"
#include "AOTOpcodeSets.h"
#include "AOTThinSummary.h"
#include <algorithm>
#include <array>
#include <optional>
#include <tuple>
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

enum class WidePart : uint8_t { UnknownObjects, Values, Shapes, Builtins, Functions, ExposedObjects };
constexpr unsigned numberOfWideParts = static_cast<unsigned>(WidePart::ExposedObjects) + 1;
constexpr uint32_t none = UINT32_MAX;
constexpr unsigned maxExposureDepth = 8;

template<typename Value> using MapOfNumbers = HashMap<uint32_t, Value, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;

bool holdsWidePart(const LinkType& type, WidePart part)
{
    static constexpr uint16_t nullish = Primitive::UndefinedOrNull | Primitive::Absent;
    switch (part) {
    case WidePart::UnknownObjects:
        return type.hasDirectUnknownObjects();
    case WidePart::Values:
        return type.directPrimitives() & ~nullish;
    case WidePart::Shapes:
        return type.objects == LinkType::Objects::Lost;
    case WidePart::Builtins:
        return type.builtin || type.classes != LinkType::Classes::None;
    case WidePart::Functions:
        return type.functions != LinkType::Functions::None;
    case WidePart::ExposedObjects:
        return type.hasUnknownObjects;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

class TypeOriginTracer {
public:
    explicit TypeOriginTracer(const LinkFlow& flow)
        : m_flow(flow)
        , m_program(flow.program)
        , m_bodies(bodiesLookedAt(flow.looks))
        , m_numberOfNodes(flow.numberOfNodes())
    {
    }

    std::unique_ptr<TypeOrigins> run();

private:
    using NodeKind = LinkFlow::NodeSubject::Kind;

    struct Trace {
        bool isDone { false };
        Vector<uint32_t> births;
        Vector<uint32_t> birthOfNode;
        Vector<uint32_t> previous;
        BitVector hasOtherBirths;
        Vector<uint32_t> firstPlaced;
        MapOfNumbers<uint32_t> originOfBirth;
    };

    struct StoreOfField {
        FunctionID function;
        ValueID site;
        uint32_t value;
    };

    struct RuleEdge {
        uint32_t from;
        uint32_t to;
    };

    void indexSummaries();
    const Trace& traceOf(WidePart);
    void addUsersIn(FunctionID);
    struct Found {
        uint32_t origin;
        bool hasOtherOrigins;
    };

    class Candidates {
    public:
        explicit Candidates(const TypeOriginTracer& tracer)
            : m_tracer(tracer)
        {
        }

        void consider(std::optional<Found> found)
        {
            if (!found || m_hasPlace)
                return;
            const TypeOrigin& origin = m_tracer.m_result->origins[found->origin];
            m_hasPlace = m_tracer.isInProgram(origin.location) && origin.reason != Wire::TypeOriginReason::Untracked;
            if (m_hasPlace || saysNothing())
                m_best = found;
        }

        bool hasPlace() const { return m_hasPlace; }
        bool saysNothing() const { return !m_best || m_tracer.m_result->origins[m_best->origin].reason == Wire::TypeOriginReason::Untracked; }
        std::optional<Found> best() const { return m_best; }

    private:
        const TypeOriginTracer& m_tracer;
        std::optional<Found> m_best;
        bool m_hasPlace { false };
    };

    std::optional<Found> originOfAccess(FunctionID, ValueID site, uint32_t subject, PropertyKeyID, bool isStore);
    ValueID methodReadOf(FunctionID, const SummaryCall&) const;
    std::optional<Found> originOfCall(FunctionID, uint32_t number);
    std::optional<Found> originOfPart(uint32_t node, WidePart);
    std::optional<uint32_t> firstUseOrigin(WidePart, uint32_t birth, uint32_t user);
    std::optional<uint32_t> placedNodeOrigin(WidePart, uint32_t node);
    std::optional<Found> originOfField(uint32_t subject, PropertyKeyID);
    std::optional<Found> originOfChange(uint32_t subject, bool isStore);
    uint32_t originOfReason(Wire::TypeOriginReason);
    uint32_t originOfUntracked(FunctionID, ValueID site);
    uint32_t listOrigin(const TypeOrigin&);
    uint32_t originOfBirth(WidePart, Trace&, uint32_t birth, uint32_t user);

    TypeOrigin locateBirth(uint32_t node, WidePart);
    TypeOrigin locateSummaryNode(FunctionID, uint32_t summaryNode, WidePart);
    void locateEntry(TypeOrigin&, const Trace&, uint32_t birth, uint32_t user);
    bool locateHop(TypeOrigin&, uint32_t from, uint32_t to);
    std::optional<uint32_t> onlyArgumentAt(FunctionID, ValueID site);
    void locateExposure(TypeOrigin&, uint32_t node);
    uint32_t nodeThatExposes(const LinkFlow::NodeSubject&);
    void listUses();
    Wire::SourceLocation locationOfExposure(uint32_t node, unsigned depth);
    Wire::SourceLocation locationOfValue(FunctionID, ValueID) const;
    Wire::SourceLocation locationOfOperation(FunctionID, ValueID) const;
    Wire::SourceLocation locationOfFirstStore(VariableID);
    Wire::SourceLocation locationOfStore(FieldReference, WidePart);
    static bool namesFewLayouts(const LinkType& type) { return type.objects == LinkType::Objects::Layout || type.objects == LinkType::Objects::Set; }
    Wire::SourceLocation locationOfNode(uint32_t node);
    void forEachLayoutOnce(const LinkType&, HashSet<std::pair<uint64_t, uint32_t>>& listedObjects, NOESCAPE const Invocable<void(LayoutID)> auto&) const;
    void listChanges();
    std::optional<uint32_t> originOfChangedLayout(LayoutID, bool isStore);

    LinkFlow::NodeSubject subjectOf(uint32_t node) const
    {
        LinkFlow::NodeSubject subject = m_flow.subjectOfNode(FlowNodeID(node));
        if (subject.kind != NodeKind::SummaryNode)
            return subject;
        return m_summaryNodes[node] == none ? LinkFlow::NodeSubject { } : LinkFlow::NodeSubject { subject.kind, subject.first, m_summaryNodes[node] };
    }
    LinkType typeAt(uint32_t node) const { return m_flow.typeAt(FlowNodeID(node)); }
    uint32_t nodeOf(FunctionID function, uint32_t summaryNode) const { return static_cast<uint32_t>(m_flow.nodeOf(function, summaryNode).index()); }
    uint32_t unseen() const { return static_cast<uint32_t>(m_flow.unseen().index()); }
    bool isInProgram(const Wire::SourceLocation& location) const { return hasModule(location) && isOfProgram(ModuleID(location.module)); }
    bool isOfProgram(FunctionID function) const { return isOfProgram(m_program.at(function).module); }
    bool isOfProgram(ModuleID id) const
    {
        const Module& module = m_program.at(id);
        return module.kind != ModuleKind::RuntimeSource && !module.flags.contains(ModuleFlags::IsLibrary);
    }

    const LinkFlow& m_flow;
    const Program& m_program;
    const BodyIndexTable& m_bodies;
    const size_t m_numberOfNodes;
    Vector<RuleEdge> m_ruleEdges;
    BitVector m_leftOut;
    BitVector m_isOutside;
    Vector<uint32_t> m_summaryNodes;
    MapOfNumbers<BlockID> m_handlersOfCaught;
    MapOfNumbers<uint32_t> m_siteOfContents;
    Vector<LinkFlow::SiteMaker> m_siteMakers;
    std::array<Trace, numberOfWideParts> m_traces;
    std::optional<MapOfNumbers<Wire::SourceLocation>> m_firstStores;
    std::optional<HashMap<std::pair<uint32_t, uint32_t>, Vector<StoreOfField>>> m_stores;
    bool m_hasListedChanges { false };
    MapOfNumbers<Wire::SourceLocation> m_changes;
    MapOfNumbers<Wire::SourceLocation> m_changesForStores;
    HashMap<std::pair<uint64_t, uint32_t>, uint32_t> m_originOfChangeInFamily;
    bool m_hasListedExposers { false };
    bool m_hasListedUses { false };
    MapOfNumbers<LinkFlow::SiteMaker> m_uses;
    std::optional<HashMap<std::pair<uint32_t, uint32_t>, uint32_t>> m_callsWithOneArgument;
    MapOfNumbers<uint32_t> m_exposerOfLayout;
    MapOfNumbers<uint32_t> m_exposerOfFunction;
    MapOfNumbers<uint32_t> m_originOfChange;
    MapOfNumbers<uint32_t> m_originOfReason;
    MapOfNumbers<uint32_t> m_placedNodeOrigin;
    std::unique_ptr<TypeOrigins> m_result;
};

void TypeOriginTracer::indexSummaries()
{
    m_leftOut.ensureSize(m_numberOfNodes);
    m_leftOut.set(unseen());
    m_isOutside.ensureSize(m_numberOfNodes);
    m_summaryNodes.fill(none, m_numberOfNodes);
    for (uint32_t variable = 0; variable < m_program.variables.size(); ++variable) {
        if (!isOfProgram(m_program.variables[variable].module))
            m_isOutside.set(m_flow.variable(VariableID(variable)).index());
    }
    for (size_t index : m_flow.reached) {
        FunctionID function(static_cast<uint32_t>(index));
        m_leftOut.set(m_flow.thrown(function).index());
        const ThinSummary& summary = m_flow.summaries.at(function);
        bool isOutside = !isOfProgram(function);
        for (const SummaryHandler& handler : summary.handlers)
            m_handlersOfCaught.add(nodeOf(function, handler.caught), handler.handler);
        for (uint32_t number = 0; number < summary.nodes.size(); ++number) {
            const SummaryNode& node = summary.nodes[number];
            auto addRule = [&](uint32_t from) {
                if (from == noSummaryNode)
                    return;
                uint32_t source = nodeOf(function, from);
                if (source != unseen())
                    m_ruleEdges.append({ source, nodeOf(function, number) });
            };
            if (isOutside && (node.kind == SummaryNode::Kind::Parameter || node.kind == SummaryNode::Kind::Result || node.kind > SummaryNode::Kind::Contents))
                m_isOutside.set(nodeOf(function, number));
            if (node.kind >= SummaryNode::Kind::Contents)
                m_summaryNodes[nodeOf(function, number)] = number;
            if (node.kind == SummaryNode::Kind::Constant)
                m_leftOut.set(nodeOf(function, number));
            if (node.kind == SummaryNode::Kind::ResultOfAccess)
                addRule(summary.accesses[node.first].subject);
            if (node.kind == SummaryNode::Kind::ResultOfCall) {
                addRule(summary.calls[node.first].subject);
                addRule(summary.calls[node.first].receiver);
            }
        }
    }
    std::ranges::sort(m_ruleEdges, { }, &RuleEdge::from);
    m_flow.forEachContents(scopedLambda<void(SiteID, SlotOfContents, FlowNodeID)>([&](SiteID site, SlotOfContents, FlowNodeID node) {
        m_siteOfContents.add(static_cast<uint32_t>(node.index()), static_cast<uint32_t>(site.index()));
    }));
    m_siteMakers = m_flow.findSiteMakers();
}

const TypeOriginTracer::Trace& TypeOriginTracer::traceOf(WidePart part)
{
    Trace& trace = m_traces[static_cast<unsigned>(part)];
    if (std::exchange(trace.isDone, true))
        return trace;

    BitVector holders(m_numberOfNodes);
    bool followsRules = part == WidePart::UnknownObjects || part == WidePart::ExposedObjects;
    for (uint32_t node = 0; node < m_numberOfNodes; ++node) {
        if (!m_leftOut.get(node) && holdsWidePart(typeAt(node), part))
            holders.set(node);
    }
    auto forEachTarget = [&](uint32_t node, const auto& functor) {
        LinkType type = typeAt(node);
        m_flow.forEachEdgeOutOf(FlowNodeID(node), scopedLambda<void(FlowNodeID, TypeID)>([&](FlowNodeID to, TypeID passes) {
            uint32_t target = static_cast<uint32_t>(to.index());
            if (target < m_numberOfNodes && holders.get(target) && holdsWidePart(m_flow.passedBy(type, passes), part))
                functor(target);
        }));
        if (!followsRules)
            return;
        for (const RuleEdge& rule : std::ranges::equal_range(m_ruleEdges, node, { }, &RuleEdge::from)) {
            if (holders.get(rule.to))
                functor(rule.to);
        }
    };
    trace.birthOfNode.fill(none, m_numberOfNodes);
    trace.previous.fill(none, m_numberOfNodes);
    trace.hasOtherBirths.ensureSize(m_numberOfNodes);
    Vector<uint32_t> level;
    Vector<uint32_t> levelOfNode;
    levelOfNode.fill(0, m_numberOfNodes);
    uint32_t numberOfLevel = 0;
    auto addBirth = [&](uint32_t node) {
        trace.birthOfNode[node] = static_cast<uint32_t>(trace.births.size());
        trace.births.append(node);
        level.append(node);
    };
    auto spread = [&] {
        while (!level.isEmpty()) {
            Vector<uint32_t> next;
            ++numberOfLevel;
            for (uint32_t node : level) {
                uint32_t birth = trace.birthOfNode[node];
                forEachTarget(node, [&](uint32_t target) {
                    bool isNew = trace.birthOfNode[target] == none;
                    if (!isNew && (levelOfNode[target] != numberOfLevel || std::pair { trace.birthOfNode[target], trace.previous[target] } <= std::pair { birth, node }))
                        return;
                    trace.birthOfNode[target] = birth;
                    trace.previous[target] = node;
                    levelOfNode[target] = numberOfLevel;
                    if (isNew)
                        next.append(target);
                });
            }
            std::ranges::sort(next);
            level = WTF::move(next);
        }
    };
    auto addBirths = [&](const auto& isSource) {
        BitVector isBrought(m_numberOfNodes);
        for (uint32_t node = 0; node < m_numberOfNodes; ++node) {
            if (holders.get(node) && isSource(node))
                forEachTarget(node, [&](uint32_t target) { isBrought.set(target); });
        }
        for (uint32_t node = 0; node < m_numberOfNodes; ++node) {
            if (holders.get(node) && trace.birthOfNode[node] == none && !isBrought.get(node) && !m_isOutside.get(node))
                addBirth(node);
        }
        spread();
    };
    addBirths([](uint32_t) { return true; });
    addBirths([&](uint32_t node) { return trace.birthOfNode[node] == none && !m_isOutside.get(node); });
    for (uint32_t node = 0; node < m_numberOfNodes; ++node) {
        if (!holders.get(node) || trace.birthOfNode[node] != none || m_isOutside.get(node))
            continue;
        addBirth(node);
        spread();
    }

    Vector<uint32_t> toSpread;
    for (uint32_t node = 0; node < m_numberOfNodes; ++node) {
        if (!holders.get(node))
            continue;
        forEachTarget(node, [&](uint32_t target) {
            if (trace.birthOfNode[node] != none && trace.birthOfNode[target] != trace.birthOfNode[node] && !trace.hasOtherBirths.set(target))
                toSpread.append(target);
        });
    }
    while (!toSpread.isEmpty()) {
        forEachTarget(toSpread.takeLast(), [&](uint32_t target) {
            if (!trace.hasOtherBirths.set(target))
                toSpread.append(target);
        });
    }
    return trace;
}

Wire::SourceLocation TypeOriginTracer::locationOfValue(FunctionID function, ValueID value) const
{
    const BodyIndex* index = m_bodies.find(function);
    if (!index || !value || index->isParameter(value))
        return noSourceLocation;
    return m_program.at(function).locationOf(index->value(value));
}

Wire::SourceLocation TypeOriginTracer::locationOfOperation(FunctionID function, ValueID value) const
{
    const BodyIndex* index = m_bodies.find(function);
    if (!index || !value || index->isParameter(value))
        return noSourceLocation;
    return m_program.at(function).locationAt(index->value(value).position());
}

Wire::SourceLocation TypeOriginTracer::locationOfFirstStore(VariableID variable)
{
    if (!m_firstStores) {
        m_firstStores.emplace();
        for (size_t number : m_flow.reached) {
            FunctionID function(static_cast<uint32_t>(number));
            const BodyIndex* index = m_bodies.find(function);
            if (!index || !isOfProgram(function))
                continue;
            for (uint32_t block = 0; block < index->numberOfBlocks(); ++block) {
                for (BodyValue value : index->block(BlockID(block)).values()) {
                    if (value.opcode() == Opcode::PutModuleVariable)
                        m_firstStores->add(value.as<Opcode::PutModuleVariable>().variable().index(), m_program.at(function).locationOf(value));
                }
            }
        }
    }
    auto found = m_firstStores->find(variable.index());
    return found == m_firstStores->end() ? noSourceLocation : found->value;
}

Wire::SourceLocation TypeOriginTracer::locationOfStore(FieldReference field, WidePart part)
{
    if (!m_stores) {
        m_stores.emplace();
        for (size_t number : m_flow.reached) {
            FunctionID function(static_cast<uint32_t>(number));
            if (!isOfProgram(function))
                continue;
            for (const SummaryAccess& access : m_flow.summaries.at(function).accesses) {
                bool isStore = access.kind == SummaryAccess::Kind::Store || access.kind == SummaryAccess::Kind::Define;
                if (!isStore || !access.key || !access.site || access.value == noSummaryNode || access.isToContents() || !namesFewLayouts(typeAt(nodeOf(function, access.subject))))
                    continue;
                m_flow.forEachLayoutOf(typeAt(nodeOf(function, access.subject)), [&](LayoutID layout) {
                    m_stores->ensure({ layout.index() + 1, access.key.index() }, [] { return Vector<StoreOfField> { }; }).iterator->value.append({ function, access.site, nodeOf(function, access.value) });
                });
            }
        }
    }
    auto found = m_stores->find({ field.layout.index() + 1, m_program.at(field.layout).fields[field.field].key.index() });
    if (found == m_stores->end())
        return noSourceLocation;
    for (const StoreOfField& store : found->value) {
        if (holdsWidePart(typeAt(store.value), part))
            return locationOfValue(store.function, store.site);
    }
    return noSourceLocation;
}

TypeOrigin TypeOriginTracer::locateSummaryNode(FunctionID function, uint32_t summaryNode, WidePart part)
{
    const ThinSummary& summary = m_flow.summaries.at(function);
    const SummaryNode& node = summary.nodes[summaryNode];
    TypeOrigin origin;
    switch (node.kind) {
    case SummaryNode::Kind::ResultOfCall: {
        const SummaryCall& call = summary.calls[node.first];
        origin.location = locationOfValue(function, call.site);
        origin.reason = Wire::TypeOriginReason::CallResult;
        const LinkFlow::CalleesOfCall& callees = m_flow.calleesOfCalls[m_flow.firstCallOfFunction[function.index()] + node.first];
        origin.builtin = call.builtin ? call.builtin : callees.builtins().isEmpty() ? BuiltinID() : callees.builtins()[0];
        if (origin.builtin)
            origin.reason = Wire::TypeOriginReason::BuiltinResult;
        else if (call.key)
            origin.name = m_program.at(call.key).text;
        break;
    }
    case SummaryNode::Kind::ResultOfAccess: {
        const SummaryAccess& access = summary.accesses[node.first];
        origin.location = locationOfValue(function, access.site);
        origin.reason = Wire::TypeOriginReason::PropertyRead;
        if (access.key)
            origin.name = m_program.at(access.key).text;
        break;
    }
    case SummaryNode::Kind::ResultOfOperator:
        origin.location = locationOfValue(function, summary.operators[node.first].site);
        origin.reason = Wire::TypeOriginReason::OtherValue;
        break;
    case SummaryNode::Kind::ClassInstance:
        if (std::optional<ValueID> maker = LinkFlow::valueThatMakes(node))
            origin.location = locationOfValue(function, *maker);
        origin.reason = Wire::TypeOriginReason::OtherBuiltin;
        break;
    case SummaryNode::Kind::Builtin:
        origin.reason = Wire::TypeOriginReason::OtherBuiltin;
        origin.builtin = BuiltinID(node.first);
        break;
    case SummaryNode::Kind::Closure:
        origin.reason = Wire::TypeOriginReason::OtherFunction;
        origin.location = m_program.at(FunctionID(node.first)).declarationLocation();
        origin.name = m_program.at(FunctionID(node.first)).name;
        break;
    default:
        origin.reason = part == WidePart::Values ? Wire::TypeOriginReason::OtherValue : Wire::TypeOriginReason::Untracked;
        if (auto handler = m_handlersOfCaught.find(nodeOf(function, summaryNode)); handler != m_handlersOfCaught.end()) {
            origin.reason = Wire::TypeOriginReason::CaughtException;
            for (BodyValue value : m_bodies.at(function).block(handler->value).values()) {
                origin.location = m_program.at(function).locationOf(value);
                if (hasModule(origin.location))
                    break;
            }
        }
        break;
    }
    return origin;
}

TypeOrigin TypeOriginTracer::locateBirth(uint32_t node, WidePart part)
{
    TypeOrigin origin;
    origin.reason = part == WidePart::Values ? Wire::TypeOriginReason::OtherValue
        : part == WidePart::Shapes ? Wire::TypeOriginReason::ManyShapes
        : part == WidePart::Builtins ? Wire::TypeOriginReason::OtherBuiltin
        : part == WidePart::Functions ? Wire::TypeOriginReason::OtherFunction : Wire::TypeOriginReason::Untracked;
    bool isUnknown = part == WidePart::UnknownObjects || part == WidePart::ExposedObjects;

    if (auto found = m_siteOfContents.find(node); found != m_siteOfContents.end()) {
        const LinkFlow::SiteMaker& maker = m_siteMakers[found->value];
        if (maker.function)
            origin.location = locationOfValue(maker.function, maker.value);
        if (isUnknown)
            origin.reason = Wire::TypeOriginReason::ExposedObject;
        return origin;
    }
    LinkFlow::NodeSubject subject = subjectOf(node);
    switch (subject.kind) {
    case NodeKind::Variable: {
        VariableID variable(subject.first);
        origin.location = locationOfFirstStore(variable);
        origin.name = m_program.variables[variable.index()].name;
        if (isUnknown)
            origin.reason = Wire::TypeOriginReason::ExposedVariable;
        break;
    }
    case NodeKind::Field: {
        FieldReference field { LayoutID(subject.first), subject.second };
        origin.name = m_program.at(field.layout).name;
        if (isUnknown) {
            origin.reason = Wire::TypeOriginReason::ExposedObject;
            locateExposure(origin, node);
        } else {
            origin.name = m_program.at(m_program.at(field.layout).fields[field.field].key).text;
            origin.location = locationOfStore(field, part);
        }
        break;
    }
    case NodeKind::Parameter: {
        FunctionID function(subject.first);
        origin.name = m_program.at(function).name;
        origin.location = m_program.at(function).declarationLocation();
        if (isUnknown) {
            origin.reason = Wire::TypeOriginReason::OutsideCaller;
            locateExposure(origin, node);
        }
        break;
    }
    case NodeKind::Result:
        origin.name = m_program.at(FunctionID(subject.first)).name;
        origin.location = m_program.at(FunctionID(subject.first)).declarationLocation();
        break;
    case NodeKind::SummaryNode:
        return locateSummaryNode(FunctionID(subject.first), subject.second, part);
    case NodeKind::Thrown:
    case NodeKind::Other:
        break;
    }
    return origin;
}

uint32_t TypeOriginTracer::nodeThatExposes(const LinkFlow::NodeSubject& subject)
{
    if (!std::exchange(m_hasListedExposers, true)) {
        HashSet<std::pair<uint64_t, uint32_t>> listedObjects;
        for (uint32_t node = 0; node < m_numberOfNodes; ++node) {
            bool exposes = m_flow.flowsIntoUnseen(FlowNodeID(node));
            if (!exposes) {
                m_flow.forEachEdgeOutOf(FlowNodeID(node), scopedLambda<void(FlowNodeID, TypeID)>([&](FlowNodeID to, TypeID) {
                    exposes |= to == m_flow.unseen();
                }));
            }
            if (!exposes || node == unseen())
                continue;
            LinkType type = typeAt(node);
            forEachLayoutOnce(type, listedObjects, [&](LayoutID layout) { m_exposerOfLayout.add(layout.index(), node); });
            if (type.functions == LinkType::Functions::Closure)
                m_exposerOfFunction.add(type.function.index(), node);
            else if (type.functions == LinkType::Functions::Set) {
                for (FunctionID function : m_flow.functionsOfSet(type))
                    m_exposerOfFunction.add(function.index(), node);
            }
        }
    }
    const auto& exposers = subject.kind == NodeKind::Field ? m_exposerOfLayout : m_exposerOfFunction;
    auto found = exposers.find(subject.first);
    return found == exposers.end() ? none : found->value;
}

Wire::SourceLocation TypeOriginTracer::locationOfNode(uint32_t node)
{
    LinkFlow::NodeSubject subject = subjectOf(node);
    if (subject.kind == NodeKind::Variable)
        return locationOfFirstStore(VariableID(subject.first));
    if (subject.kind == NodeKind::Parameter || subject.kind == NodeKind::Result)
        return m_program.at(FunctionID(subject.first)).declarationLocation();
    if (subject.kind == NodeKind::SummaryNode)
        return locateSummaryNode(FunctionID(subject.first), subject.second, WidePart::Values).location;
    return noSourceLocation;
}

void TypeOriginTracer::listUses()
{
    if (std::exchange(m_hasListedUses, true))
        return;
    auto add = [&](FunctionID function, uint32_t summaryNode, ValueID site) {
        if (summaryNode != noSummaryNode && site)
            m_uses.add(nodeOf(function, summaryNode), LinkFlow::SiteMaker { function, site });
    };
    for (size_t number : m_flow.reached) {
        FunctionID function(static_cast<uint32_t>(number));
        if (!isOfProgram(function))
            continue;
        const ThinSummary& summary = m_flow.summaries.at(function);
        for (const SummaryCall& call : summary.calls) {
            for (uint32_t argument = 0; argument < call.numberOfArguments; ++argument)
                add(function, summary.arguments[call.firstArgument + argument], call.site);
            add(function, call.receiver, call.site);
        }
        for (const SummaryAccess& access : summary.accesses)
            add(function, access.value, access.site);
    }
}

Wire::SourceLocation TypeOriginTracer::locationOfExposure(uint32_t node, unsigned depth)
{
    LinkFlow::NodeSubject subject = subjectOf(node);
    if (subject.kind == NodeKind::Field && depth < maxExposureDepth) {
        uint32_t exposer = nodeThatExposes(subject);
        return exposer == none || exposer == node ? noSourceLocation : locationOfExposure(exposer, depth + 1);
    }
    listUses();
    if (auto use = m_uses.find(node); use != m_uses.end())
        return locationOfValue(use->value.function, use->value.value);
    return locationOfNode(node);
}

void TypeOriginTracer::locateExposure(TypeOrigin& origin, uint32_t node)
{
    uint32_t exposer = nodeThatExposes(subjectOf(node));
    if (exposer == none || exposer == node)
        return;
    Wire::SourceLocation location = locationOfExposure(exposer, 0);
    if (hasModule(location))
        origin.location = location;
}

std::optional<uint32_t> TypeOriginTracer::onlyArgumentAt(FunctionID caller, ValueID site)
{
    if (!m_callsWithOneArgument) {
        m_callsWithOneArgument.emplace();
        for (size_t number : m_flow.reached) {
            FunctionID function(static_cast<uint32_t>(number));
            const ThinSummary& summary = m_flow.summaries.at(function);
            for (const SummaryCall& call : summary.calls) {
                if (call.site && call.numberOfArguments == 1)
                    m_callsWithOneArgument->add({ function.index() + 1, call.site.index() }, nodeOf(function, summary.arguments[call.firstArgument]));
            }
        }
    }
    auto found = m_callsWithOneArgument->find({ caller.index() + 1, site.index() });
    return found == m_callsWithOneArgument->end() ? std::nullopt : std::optional(found->value);
}

bool TypeOriginTracer::locateHop(TypeOrigin& origin, uint32_t from, uint32_t to)
{
    if (auto found = m_siteOfContents.find(to); found != m_siteOfContents.end()) {
        const LinkFlow::SiteMaker& maker = m_siteMakers[found->value];
        if (!maker.function)
            return false;
        std::optional<uint32_t> argument = onlyArgumentAt(maker.function, maker.value);
        if (argument == from)
            origin.reason = Wire::TypeOriginReason::ArrayArgument;
        origin.entry = locationOfValue(maker.function, maker.value);
        return true;
    }
    LinkFlow::NodeSubject subject = subjectOf(to);
    if (subject.kind == NodeKind::Field) {
        origin.entry = locationOfStore({ LayoutID(subject.first), subject.second }, WidePart::Values);
        return true;
    }
    origin.entry = locationOfNode(to);
    return true;
}

void TypeOriginTracer::locateEntry(TypeOrigin& origin, const Trace& trace, uint32_t birth, uint32_t user)
{
    uint32_t birthNode = trace.births[birth];
    uint32_t entry = none;
    for (uint32_t node = user; node != none; node = trace.previous[node]) {
        LinkType type = typeAt(node);
        if (type.objects != LinkType::Objects::None || type.hasDirectUnknownObjects())
            entry = node;
        if (node == birthNode)
            break;
    }
    if (entry == none || entry == birthNode || trace.previous[entry] == none)
        return;
    locateHop(origin, trace.previous[entry], entry);
}

uint32_t TypeOriginTracer::originOfBirth(WidePart part, Trace& trace, uint32_t birth, uint32_t user)
{
    auto added = trace.originOfBirth.add(birth, 0);
    if (!added.isNewEntry)
        return added.iterator->value;
    TypeOrigin origin = locateBirth(trace.births[birth], part);
    origin.node = trace.births[birth];
    if (part == WidePart::Values)
        locateEntry(origin, trace, birth, user);
    uint32_t number = listOrigin(origin);
    trace.originOfBirth.set(birth, number);
    return number;
}

std::optional<TypeOriginTracer::Found> TypeOriginTracer::originOfPart(uint32_t node, WidePart part)
{
    if (!holdsWidePart(typeAt(node), part))
        return std::nullopt;
    traceOf(part);
    Trace& trace = m_traces[static_cast<unsigned>(part)];
    uint32_t birth = trace.birthOfNode[node];
    if (birth == none)
        return std::nullopt;
    bool hasOtherOrigins = trace.hasOtherBirths.get(node);
    if (part == WidePart::Shapes && hasOtherOrigins)
        return Found { originOfReason(Wire::TypeOriginReason::ManyShapes), true };
    uint32_t origin = originOfBirth(part, trace, birth, node);
    if (!isInProgram(m_result->origins[origin].location)) {
        if (std::optional<uint32_t> placed = firstUseOrigin(part, birth, node))
            return Found { *placed, hasOtherOrigins };
    }
    return Found { origin, hasOtherOrigins };
}

std::optional<uint32_t> TypeOriginTracer::firstUseOrigin(WidePart part, uint32_t birth, uint32_t user)
{
    static constexpr uint32_t unvisited = none - 1;
    Trace& trace = m_traces[static_cast<unsigned>(part)];
    if (trace.firstPlaced.isEmpty())
        trace.firstPlaced.fill(unvisited, m_numberOfNodes);
    Vector<uint32_t, 32> path;
    uint32_t node = user;
    for (; node != none && node != trace.births[birth] && trace.firstPlaced[node] == unvisited; node = trace.previous[node])
        path.append(node);
    uint32_t placed = node == none || node == trace.births[birth] ? none : trace.firstPlaced[node];
    while (!path.isEmpty()) {
        uint32_t next = path.takeLast();
        if (placed == none && placedNodeOrigin(part, next))
            placed = next;
        trace.firstPlaced[next] = placed;
    }
    return placed == none ? std::nullopt : placedNodeOrigin(part, placed);
}

std::optional<uint32_t> TypeOriginTracer::placedNodeOrigin(WidePart part, uint32_t node)
{
    uint32_t origin = m_placedNodeOrigin.ensure(node, [&] {
        LinkFlow::NodeSubject subject = subjectOf(node);
        if (subject.kind != NodeKind::SummaryNode)
            return none;
        TypeOrigin found = locateSummaryNode(FunctionID(subject.first), subject.second, part);
        return isInProgram(found.location) ? listOrigin(found) : none;
    }).iterator->value;
    return origin == none ? std::nullopt : std::optional(origin);
}

std::optional<TypeOriginTracer::Found> TypeOriginTracer::originOfField(uint32_t subject, PropertyKeyID key)
{
    if (!key || !namesFewLayouts(typeAt(subject)))
        return std::nullopt;
    std::optional<uint32_t> field;
    m_flow.forEachLayoutOf(typeAt(subject), [&](LayoutID layout) {
        std::optional<FieldReference> found = field ? std::nullopt : findField(m_program, layout, key);
        if (!found)
            return;
        uint32_t node = static_cast<uint32_t>(m_flow.field(*found).index());
        LinkType held = typeAt(node);
        bool mixesValues = held.objects != LinkType::Objects::None && holdsWidePart(held, WidePart::Values);
        if (held.hasDirectUnknownObjects() || mixesValues)
            field = node;
    });
    if (!field)
        return std::nullopt;
    std::optional<Found> found = originOfPart(*field, WidePart::UnknownObjects);
    return found ? found : originOfPart(*field, WidePart::Values);
}

void TypeOriginTracer::forEachLayoutOnce(const LinkType& type, HashSet<std::pair<uint64_t, uint32_t>>& listedObjects, NOESCAPE const Invocable<void(LayoutID)> auto& functor) const
{
    std::pair<uint64_t, uint32_t> objects { static_cast<uint64_t>(type.objects) << 32 | type.layout.bits(), type.family.bits() };
    if (type.objects != LinkType::Objects::None && listedObjects.add(objects).isNewEntry)
        m_flow.forEachLayoutOf(type, functor);
}

void TypeOriginTracer::listChanges()
{
    if (std::exchange(m_hasListedChanges, true))
        return;
    HashSet<std::pair<uint64_t, uint32_t>> listedObjects;
    HashSet<std::pair<uint64_t, uint32_t>> listedObjectsOfStores;
    for (size_t number : m_flow.reached) {
        FunctionID function(static_cast<uint32_t>(number));
        if (!isOfProgram(function))
            continue;
        for (const SummaryChange& change : m_flow.summaries.at(function).changes) {
            if (!change.site || change.kind == SummaryChange::Kind::BecomesPrototype)
                continue;
            LinkType type = typeAt(nodeOf(function, change.subject));
            Wire::SourceLocation location = locationOfValue(function, change.site);
            forEachLayoutOnce(type, listedObjectsOfStores, [&](LayoutID layout) { m_changesForStores.add(layout.index(), location); });
            if (change.kind != SummaryChange::Kind::Freezes)
                forEachLayoutOnce(type, listedObjects, [&](LayoutID layout) { m_changes.add(layout.index(), location); });
        }
    }
}

std::optional<uint32_t> TypeOriginTracer::originOfChangedLayout(LayoutID layout, bool isStore)
{
    uint32_t listed = m_originOfChange.ensure(2 * layout.index() + isStore, [&] {
        listChanges();
        const auto& changes = isStore ? m_changesForStores : m_changes;
        TypeOrigin found;
        found.name = m_program.at(layout).name;
        if (auto change = changes.find(layout.index()); change != changes.end()) {
            found.reason = Wire::TypeOriginReason::ChangedShape;
            found.location = change->value;
            return listOrigin(found);
        }
        if (!m_flow.escapedLayouts.get(layout.index()))
            return none;
        found.reason = Wire::TypeOriginReason::ExposedObject;
        uint32_t exposer = nodeThatExposes({ NodeKind::Field, layout.index(), 0 });
        if (exposer != none)
            found.location = locationOfExposure(exposer, 0);
        return listOrigin(found);
    }).iterator->value;
    return listed == none ? std::nullopt : std::optional(listed);
}

std::optional<TypeOriginTracer::Found> TypeOriginTracer::originOfChange(uint32_t subject, bool isStore)
{
    LinkType type = typeAt(subject);
    auto findAmongLayouts = [&] {
        std::optional<uint32_t> origin;
        m_flow.forEachLayoutOf(type, [&](LayoutID layout) {
            if (!origin)
                origin = originOfChangedLayout(layout, isStore);
        });
        return origin.value_or(none);
    };
    uint32_t origin;
    if (namesFewLayouts(type))
        origin = findAmongLayouts();
    else {
        std::pair<uint64_t, uint32_t> objects { static_cast<uint64_t>(type.objects) << 33 | static_cast<uint64_t>(isStore) << 32 | type.layout.bits(), type.family.bits() };
        auto found = m_originOfChangeInFamily.find(objects);
        if (found == m_originOfChangeInFamily.end()) {
            origin = findAmongLayouts();
            m_originOfChangeInFamily.add(objects, origin);
        } else
            origin = found->value;
    }
    return origin == none ? std::nullopt : std::optional(Found { origin, false });
}

uint32_t TypeOriginTracer::originOfReason(Wire::TypeOriginReason reason)
{
    return m_originOfReason.ensure(static_cast<uint32_t>(reason), [&] {
        TypeOrigin found;
        found.reason = reason;
        return listOrigin(found);
    }).iterator->value;
}

uint32_t TypeOriginTracer::originOfUntracked(FunctionID function, ValueID site)
{
    const BodyIndex& index = m_bodies.at(function);
    std::optional<uint8_t> receiver = index.isParameter(site) ? std::nullopt : receiverOf(index.opcodeOf(site));
    ValueID source = receiver ? index.stripCasts(index.value(site).operand(*receiver)) : ValueID();
    bool isCaught = source && !index.isParameter(source) && (index.opcodeOf(source) == Opcode::CatchValue || index.opcodeOf(source) == Opcode::CatchException);
    return originOfReason(isCaught ? Wire::TypeOriginReason::CaughtException : Wire::TypeOriginReason::Untracked);
}

uint32_t TypeOriginTracer::listOrigin(const TypeOrigin& origin)
{
    m_result->origins.append(origin);
    return static_cast<uint32_t>(m_result->origins.size() - 1);
}

std::optional<TypeOriginTracer::Found> TypeOriginTracer::originOfAccess(FunctionID function, ValueID site, uint32_t subject, PropertyKeyID key, bool isStore)
{
    if (subject == unseen())
        return Found { originOfUntracked(function, site), false };
    LinkType type = typeAt(subject);
    if (type.objects == LinkType::Objects::None && !type.hasUnknownObjects)
        return Found { originOfReason(Wire::TypeOriginReason::BuiltinProperty), false };

    Candidates candidates(*this);
    for (WidePart part : { WidePart::UnknownObjects, WidePart::Shapes, WidePart::Values })
        candidates.consider(originOfPart(subject, part));
    if (candidates.saysNothing())
        candidates.consider(originOfField(subject, key));
    if (candidates.saysNothing())
        candidates.consider(originOfChange(subject, isStore));
    for (WidePart part : { WidePart::ExposedObjects, WidePart::Builtins, WidePart::Functions }) {
        if (candidates.saysNothing())
            candidates.consider(originOfPart(subject, part));
    }
    return candidates.best();
}

std::optional<TypeOriginTracer::Found> TypeOriginTracer::originOfCall(FunctionID function, uint32_t number)
{
    const SummaryCall& call = m_flow.summaries.at(function).calls[number];
    uint32_t subject = call.subject != noSummaryNode ? call.subject : call.receiver;
    Candidates candidates(*this);
    if (subject != noSummaryNode && nodeOf(function, subject) != unseen()) {
        for (WidePart part : { WidePart::UnknownObjects, WidePart::ExposedObjects })
            candidates.consider(originOfPart(nodeOf(function, subject), part));
    }
    const LinkFlow::CalleesOfCall& callees = m_flow.calleesOfCalls[m_flow.firstCallOfFunction[function.index()] + number];
    if (!candidates.hasPlace() && (callees.mayEnterLost || callees.listed().size() + callees.builtins().size() > 1)) {
        TypeOrigin origin;
        origin.reason = Wire::TypeOriginReason::ManyFunctions;
        origin.location = locationOfOperation(function, call.site);
        candidates.consider(Found { listOrigin(origin), false });
    }
    return candidates.best();
}

ValueID TypeOriginTracer::methodReadOf(FunctionID function, const SummaryCall& call) const
{
    const BodyIndex& index = m_bodies.at(function);
    if (!call.site || !call.key || call.receiver == noSummaryNode || index.isParameter(call.site) || index.opcodeOf(call.site) != Opcode::Call)
        return { };
    ValueID callee = index.stripCasts(index.value(call.site).as<Opcode::Call>().calleeOperand());
    return callee && !index.isParameter(callee) && index.opcodeOf(callee) == Opcode::PropGet && index.value(callee).key() == call.key ? callee : ValueID();
}

void TypeOriginTracer::addUsersIn(FunctionID function)
{
    const ThinSummary& summary = m_flow.summaries.at(function);
    auto add = [&](ValueID site, OriginUser kind, std::optional<Found> found) {
        Wire::SourceLocation location = locationOfOperation(function, site);
        if (found && hasModule(location))
            m_result->users.append({ location, function, kind, found->origin, found->hasOtherOrigins });
    };
    for (const SummaryAccess& access : summary.accesses) {
        if (access.site && !access.isToContents())
            add(access.site, OriginUser::Access, originOfAccess(function, access.site, nodeOf(function, access.subject), access.key, access.kind != SummaryAccess::Kind::Read));
    }
    for (uint32_t number = 0; number < summary.calls.size(); ++number) {
        const SummaryCall& call = summary.calls[number];
        if (call.site && call.callee != SummaryCall::Callee::Builtin && call.callee != SummaryCall::Callee::Binding)
            add(call.site, OriginUser::Call, originOfCall(function, number));
        if (ValueID read = methodReadOf(function, call))
            add(read, OriginUser::Access, originOfAccess(function, read, nodeOf(function, call.receiver), call.key, false));
    }
    for (const SummaryOperator& operation : summary.operators) {
        if (operation.isConversionByEquality())
            continue;
        for (uint32_t operand : operation.operands) {
            if (operation.site && operand != noSummaryNode && nodeOf(function, operand) != unseen())
                add(operation.site, OriginUser::Operator, originOfPart(nodeOf(function, operand), WidePart::UnknownObjects));
        }
    }
}

std::unique_ptr<TypeOrigins> TypeOriginTracer::run()
{
    m_result = makeUnique<TypeOrigins>();
    indexSummaries();
    for (size_t index : m_flow.reached) {
        FunctionID function(static_cast<uint32_t>(index));
        if (isOfProgram(function) && m_bodies.find(function))
            addUsersIn(function);
    }
    std::ranges::stable_sort(m_result->users, { }, [](const TypeOrigins::User& user) {
        return std::tuple { user.location.module, user.location.position, user.function.index(), user.kind };
    });
    return WTF::move(m_result);
}

} // anonymous namespace

const TypeOrigins::User* TypeOrigins::find(const Wire::SourceLocation& location, FunctionID function, OriginUser kind) const
{
    auto keyOf = [](const User& user) { return std::tuple { user.location.module, user.location.position, user.function.index(), user.kind }; };
    auto found = std::ranges::lower_bound(users, std::tuple { location.module, location.position, function.index(), kind }, { }, keyOf);
    if (found == users.end() || keyOf(*found) != std::tuple { location.module, location.position, function.index(), kind })
        return nullptr;
    return &*found;
}

std::unique_ptr<TypeOrigins> traceTypeOrigins(const LinkFlow& flow)
{
    return TypeOriginTracer(flow).run();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
