/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowResultFamilies.h"

#if ENABLE(AOT)

#include "AOTFlowStamps.h"
#include <algorithm>
#include <compare>
#include <span>
#include <wtf/HashSet.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint16_t primitivesDescribedByCandidates = Primitive::LiteralString | Primitive::Undefined | Primitive::AnySymbol;

namespace {

struct OperandNode {
    FunctionID function;
    ValueID call;
    uint32_t argumentIndex { 0 };
    FlowNodeID node;

    friend auto operator<=>(const OperandNode&, const OperandNode&) = default;
};

struct OperandGroup {
    AccessSiteID call;
    std::span<const OperandNode> nodes;
};

struct StringCopy {
    FlowNodeID target;
    FlowNodeID source;

    friend auto operator<=>(const StringCopy&, const StringCopy&) = default;
};

struct SeededString {
    FlowNodeID node;
    StringID string;

    friend auto operator<=>(const SeededString&, const SeededString&) = default;
};

struct StringSources {
    Vector<StringCopy> copies;
    Vector<SeededString> seeds;

    std::span<const StringCopy> copiesInto(FlowNodeID target) const LIFETIME_BOUND
    {
        auto found = std::ranges::equal_range(copies, target, { }, &StringCopy::target);
        return copies.subspan(found.begin() - copies.begin(), found.size());
    }

    std::span<const SeededString> seedsOf(FlowNodeID node) const LIFETIME_BOUND
    {
        auto found = std::ranges::equal_range(seeds, node, { }, &SeededString::node);
        return seeds.subspan(found.begin() - seeds.begin(), found.size());
    }
};

} // anonymous namespace

static bool mayHoldString(const FlowGraph& graph, FlowNodeID node)
{
    return graph.valueOf(node).primitives & Primitive::AnyString;
}

static bool hasWideString(const FlowGraph& graph, FlowNodeID node)
{
    return graph.valueOf(node).primitives & Primitive::String;
}

static bool standsForUnseenCode(const FlowGraph& graph, FlowNodeID node)
{
    FlowNodeKind kind = graph.kindOf(node);
    if (kind == FlowNodeKind::NativeCode || kind == FlowNodeKind::UnknownJavaScript)
        return true;
    return belongsToObject(kind) && graph.objectOf(node) == unknownFlowObject;
}

static Vector<OperandNode> collectContentKeyOperands(const FlowFamilyInputs& inputs)
{
    const FlowGraph& graph = inputs.graph();
    Vector<OperandNode> operands;
    for (const FlowBuiltinOperandValue& value : inputs.records().builtinOperandValues()) {
        if (!value.roles.contains(BuiltinOperandRole::ContentKey) || value.operand.kind() != BuiltinOperand::Kind::Argument)
            continue;
        if (!value.site || graph.isHost(value.callerInstance))
            continue;
        operands.append(OperandNode { graph.at(value.callerInstance).function, value.site, value.operand.argumentIndex(), value.node });
    }
    sortAndRemoveDuplicates(inputs.loops(), operands);
    return operands;
}

static bool isSameOperand(const OperandNode& a, const OperandNode& b)
{
    return a.function == b.function && a.call == b.call && a.argumentIndex == b.argumentIndex;
}

static Vector<OperandGroup> groupOperands(const FlowFamilyInputs& inputs, const FlowAccessSiteNumbering& numbering, std::span<const OperandNode> ascendingOperands)
{
    Vector<OperandGroup> groups;
    FunctionID numberedFunction;
    Vector<AccessSiteID> sitesByValue;
    for (size_t first = 0, end = 0; first < ascendingOperands.size(); first = end) {
        const OperandNode& operand = ascendingOperands[first];
        for (end = first + 1; end < ascendingOperands.size() && isSameOperand(ascendingOperands[end], operand); ++end) { }

        if (numberedFunction != operand.function) {
            numberedFunction = operand.function;
            sitesByValue = accessSitesByValue(inputs.program().at(numberedFunction), numbering, numberedFunction);
        }
        if (AccessSiteID call = sitesByValue[operand.call.index()])
            groups.append(OperandGroup { call, ascendingOperands.subspan(first, end - first) });
    }
    return groups;
}

static SinkCandidates candidatesInValues(const FlowFamilyInputs& inputs, const OperandGroup& group, Vector<FlowNodeID, 2>& nodesWithWideString)
{
    const FlowGraph& graph = inputs.graph();
    SinkCandidates candidates;
    candidates.mayBeUndefined = false;
    candidates.isComplete = true;
    for (const OperandNode& operand : group.nodes) {
        if (!operand.node) {
            candidates.mayBeUndefined = true;
            continue;
        }
        const FlowValue& value = graph.valueOf(operand.node);
        candidates.strings.append(graph.values().stringsOf(value.strings));
        if (value.primitives & Primitive::Undefined)
            candidates.mayBeUndefined = true;
        if (value.mayBeObject() || (value.primitives & ~primitivesDescribedByCandidates))
            candidates.isComplete = false;
        if (value.primitives & Primitive::String)
            nodesWithWideString.append(operand.node);
    }
    return candidates;
}

static void addCopiesOutOf(const FlowGraph& graph, FlowNodeID source, Vector<StringCopy>& found)
{
    if (!mayHoldString(graph, source))
        return;
    graph.forEachSuccessor(source, [&](FlowNodeID target, TypeID) {
        if (hasWideString(graph, target))
            found.append(StringCopy { target, source });
    });
}

static void addCopiesThroughFilters(const FlowGraph& graph, FlowInstanceID instance, Vector<StringCopy>& found)
{
    for (const FlowFilter& filter : graph.summaryOf(instance).statements.filters) {
        FlowNodeID source = graph.nodeOf(instance, filter.from);
        FlowNodeID target = graph.nodeOf(instance, filter.to);
        if (mayHoldString(graph, source) && hasWideString(graph, target))
            found.append(StringCopy { target, source });
    }
}

static void addSeededStrings(const FlowFamilyInputs& inputs, FlowInstanceID instance, const FlowSeed& seed, Vector<SeededString>& found)
{
    switch (seed.kind()) {
    case FlowSeedKind::String:
    case FlowSeedKind::TypeAsRead:
        break;
    case FlowSeedKind::Primitives:
    case FlowSeedKind::Integers:
    case FlowSeedKind::Symbol:
    case FlowSeedKind::Site:
    case FlowSeedKind::Builtin:
    case FlowSeedKind::ModuleBuiltin:
    case FlowSeedKind::Namespace:
    case FlowSeedKind::Unknown:
        return;
    }

    FlowNodeID node = inputs.graph().nodeOf(instance, seed.node());
    if (!hasWideString(inputs.graph(), node))
        return;
    if (seed.kind() == FlowSeedKind::String) {
        found.append(SeededString { node, seed.string() });
        return;
    }
    for (StringID string : inputs.program().at(seed.type()).stringLiterals)
        found.append(SeededString { node, string });
}

static StringSources collectStringSources(const FlowFamilyInputs& inputs)
{
    const FlowGraph& graph = inputs.graph();
    const ParallelLoops& loops = inputs.loops();

    StringSources sources;
    sources.copies = collectInParallel<StringCopy>(loops, graph.numberOfNodes(), [&](unsigned index, Vector<StringCopy>& found) {
        addCopiesOutOf(graph, FlowNodeID(index), found);
    });
    sources.copies.appendVector(collectInParallel<StringCopy>(loops, graph.numberOfInstances(), [&](unsigned index, Vector<StringCopy>& found) {
        addCopiesThroughFilters(graph, FlowInstanceID(index), found);
    }));
    sources.seeds = collectInParallel<SeededString>(loops, graph.numberOfInstances(), [&](unsigned index, Vector<SeededString>& found) {
        FlowInstanceID instance(index);
        const FlowSummary& summary = graph.summaryOf(instance);
        for (const FlowSeed& seed : summary.seeds)
            addSeededStrings(inputs, instance, seed, found);
        for (const FlowConditionalSeed& statement : summary.statements.conditionalSeeds)
            addSeededStrings(inputs, instance, statement.seed, found);
    });
    sortAndRemoveDuplicates(loops, sources.copies);
    sortAndRemoveDuplicates(loops, sources.seeds);
    return sources;
}

// The solver joins the result of a built-in without a seed or an edge, so the walk finds strings and never proves that it found all of them.
static void addStringsCopiedInto(const FlowFamilyInputs& inputs, const StringSources& sources, const OperandGroup& group, std::span<const FlowNodeID> nodesWithWideString, Vector<StringID>& strings)
{
    const FlowGraph& graph = inputs.graph();
    Subject subject { .function = group.nodes.front().function, .value = group.nodes.front().call };

    HashSet<FlowNodeID> visited;
    Vector<FlowNodeID, 32> worklist;
    auto visit = [&](FlowNodeID node) {
        if (visited.add(node).isNewEntry)
            worklist.append(node);
    };
    for (FlowNodeID node : nodesWithWideString)
        visit(node);

    while (!worklist.isEmpty()) {
        if (inputs.build().exceeds(LimitID::MaxFlowWorkPerFunction, visited.size(), subject))
            return;
        FlowNodeID node = worklist.takeLast();
        if (!hasWideString(graph, node)) {
            strings.append(graph.values().stringsOf(graph.valueOf(node).strings));
            continue;
        }
        if (standsForUnseenCode(graph, node))
            continue;
        for (const SeededString& seed : sources.seedsOf(node))
            strings.append(seed.string);
        for (const StringCopy& copy : sources.copiesInto(node))
            visit(copy.source);
    }
}

Vector<FlowSinkOperand> computeSinkCandidateResults(const FlowFamilyInputs& inputs, const FlowAccessSiteNumbering& numbering)
{
    Vector<OperandNode> operands = collectContentKeyOperands(inputs);
    if (operands.isEmpty())
        return { };

    Vector<OperandGroup> groups = groupOperands(inputs, numbering, operands.span());
    unsigned numberOfGroups = static_cast<unsigned>(groups.size());
    Vector<FlowSinkOperand> results(numberOfGroups);
    Vector<Vector<FlowNodeID, 2>> nodesWithWideString(numberOfGroups);
    bool needsSources = false;
    for (unsigned index = 0; index < numberOfGroups; ++index) {
        const OperandGroup& group = groups[index];
        results[index] = FlowSinkOperand { group.call, group.nodes.front().argumentIndex, candidatesInValues(inputs, group, nodesWithWideString[index]) };
        needsSources |= !nodesWithWideString[index].isEmpty();
    }

    StringSources sources;
    if (needsSources)
        sources = collectStringSources(inputs);
    forEachIndexInParallel(inputs.loops(), numberOfGroups, [&](unsigned index) {
        Vector<StringID>& strings = results[index].candidates.strings;
        if (!nodesWithWideString[index].isEmpty())
            addStringsCopiedInto(inputs, sources, groups[index], nodesWithWideString[index].span(), strings);
        std::ranges::sort(strings);
        removeRepeatedElements(strings);
    });
    if (needsSources)
        inputs.build().destroyLater(WTF::move(sources));
    return results;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
