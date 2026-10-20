/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowResultFamilies.h"

#if ENABLE(AOT)

#include "AOTFlowObjectState.h"
#include "AOTFlowSinkEquations.h"
#include "AOTFlowSinkRules.h"
#include "AOTThreads.h"
#include <span>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct SolvedSinks {
    const FlowFamilyInputs& inputs;
    const SinkNodeNumbering& numbering;
    const SinkEquations& equations;

    OperandUses usesOf(FlowNodeID node) const { return equations.sinks[numbering.graphNode(node)]; }
    OperandUses framePointerUsesOf(FlowNodeID node) const { return equations.sinksOfFramePointers[numbering.graphNode(node)]; }
    OperandUses lifetimeUsesOf(FlowObjectID object) const { return equations.sinks[numbering.lifetime(object)]; }
};

struct ParameterResults {
    Vector<OperandSummary> summaries;
    Vector<OperandUses> framePointerUses;
};

struct FieldValueUses {
    OperandUses uses { OperandUses::all() };
    OperandUses framePointerUses { OperandUses::all() };
};

struct StoredValueResults {
    Vector<OperandUses> uses;
    Vector<OperandUses> framePointerUses;
};

} // anonymous namespace

static void addUsesOfEscapedObjects(const FlowSolution& solution, const SinkNodeNumbering& numbering, SinkEquations& equations)
{
    auto addUses = [&](FlowObjectID object, OperandUses uses) {
        uint32_t lifetime = numbering.lifetime(object);
        equations.base[lifetime].add(uses);
        equations.sinks[lifetime].add(uses);
    };
    solution.objectsInNativeCode().forEach([&](FlowObjectID object) {
        addUses(object, nativeCodeUses);
    });
    solution.objectsInUnknownJavaScript().forEach([&](FlowObjectID object) {
        addUses(object, OperandUses::all());
    });
}

static SinkRetention retentionOf(std::span<const Vector<SinkRetention>> retentionByVersion, FunctionID version, size_t entryParameter)
{
    if (version.index() >= retentionByVersion.size() || entryParameter >= retentionByVersion[version.index()].size())
        return { };
    return retentionByVersion[version.index()][entryParameter];
}

static ParameterResults computeParameterResultsOf(const SolvedSinks& sinks, const FlowVersionPlan& versions, std::span<const Vector<SinkRetention>> retentionByVersion, FunctionID id)
{
    const FlowFamilyInputs& inputs = sinks.inputs;
    const FlowGraph& graph = inputs.graph();
    FlowFunctionVersion version = findFunctionVersion(graph, versions, id);
    const FlowSummary& summary = graph.summaryOf(version.body);
    if (!version.isReached() || !summary.hasBody() || inputs.solution.isPastWorkLimit(version.body))
        return { };
    if (!inputs.isEnabled(FlowResultKind::OperandSummaries, version.body.index()))
        return { };

    ParameterResults results;
    results.summaries.reserveInitialCapacity(summary.numberOfEntryParameters);
    results.framePointerUses.reserveInitialCapacity(summary.numberOfEntryParameters);
    for (uint32_t index = 0; index < summary.numberOfEntryParameters; ++index) {
        OperandUses uses;
        OperandUses framePointerUses;
        for (FlowNodeID node : nodesOf(graph, version, summary.entryParameter(index))) {
            uses.add(sinks.usesOf(node));
            framePointerUses.add(sinks.framePointerUsesOf(node));
        }
        SinkRetention retention = retentionOf(retentionByVersion, id, index);
        results.summaries.append(OperandSummary { uses, retention.retainedIn, retention.retainingOperand });
        results.framePointerUses.append(framePointerUses);
    }
    return results;
}

static FieldValueUses computeUsesOfFieldValue(const SolvedSinks& sinks, const FlowFieldSubject& field)
{
    FieldValueUses result { { }, { } };
    bool hasNode = false;
    forEachFieldNode(sinks.inputs, field, [&](FlowObjectID, FlowNodeID node) {
        hasNode = true;
        result.uses.add(sinks.usesOf(node));
        result.framePointerUses.add(sinks.framePointerUsesOf(node));
    });
    return hasNode ? result : FieldValueUses();
}

static StoredValueResults computeStoredValueResultsOf(const SolvedSinks& sinks, LayoutID id)
{
    const Layout& layout = sinks.inputs.program().at(id);
    if (!layout.isEnvironment() || !sinks.inputs.isEnabled(FlowResultKind::OperandSummaries, id.index()))
        return { };

    StoredValueResults results;
    results.uses.reserveInitialCapacity(layout.fields.size());
    results.framePointerUses.reserveInitialCapacity(layout.fields.size());
    for (uint32_t index = 0; index < layout.fields.size(); ++index) {
        FieldValueUses field = computeUsesOfFieldValue(sinks, FlowFieldSubject { id, index, layout.fields[index].key });
        results.uses.append(field.uses);
        results.framePointerUses.append(field.framePointerUses);
    }
    return results;
}

static FrameLocality computeLocalityOf(const SolvedSinks& sinks, AllocationClassID allocationClass)
{
    const FlowFamilyInputs& inputs = sinks.inputs;
    if (!inputs.isEnabled(FlowResultKind::FrameLocalAllocations, allocationClass.index()))
        return FrameLocality::Unknown;

    bool isAllocated = false;
    bool mayOutliveFrame = false;
    forEachObjectOfClass(inputs.objects(), allocationClass, [&](FlowObjectID object) {
        if (!inputs.factsOf(object).events.contains(FlowObjectEvent::Allocated))
            return;
        isAllocated = true;
        mayOutliveFrame |= sinks.lifetimeUsesOf(object).containsAny(frameLocalBlockers);
    });
    return isAllocated && !mayOutliveFrame ? FrameLocality::FrameLocal : FrameLocality::Unknown;
}

FlowEscapeResults computeEscapeResults(const FlowFamilyInputs& inputs, const FlowVersionPlan& versions)
{
    SinkNodeNumbering numbering = numberSinkNodes(inputs.solution);
    SinkEquations equations = buildSinkEquations(inputs, numbering);
    addUsesOfEscapedObjects(inputs.solution, numbering, equations);
    solveSinkEquations(equations);
    Vector<Vector<SinkRetention>> retention = computeSinkRetention(SinkRetentionInputs { inputs, versions, numbering, equations });

    SolvedSinks sinks { inputs, numbering, equations };
    FlowEscapeResults results;

    results.parameterSummaries.grow(versions.functionCount());
    results.framePointerParameterUses.grow(versions.functionCount());
    forEachIndexInParallel(inputs.loops(), results.parameterSummaries.size(), [&](unsigned index) {
        ParameterResults parameters = computeParameterResultsOf(sinks, versions, retention.span(), FunctionID(index));
        results.parameterSummaries[index] = WTF::move(parameters.summaries);
        results.framePointerParameterUses[index] = WTF::move(parameters.framePointerUses);
    });

    results.storedValueUses.grow(inputs.program().layouts.size());
    results.storedFramePointerUses.grow(inputs.program().layouts.size());
    forEachIndexInParallel(inputs.loops(), results.storedValueUses.size(), [&](unsigned index) {
        StoredValueResults storedValues = computeStoredValueResultsOf(sinks, LayoutID(index));
        results.storedValueUses[index] = WTF::move(storedValues.uses);
        results.storedFramePointerUses[index] = WTF::move(storedValues.framePointerUses);
    });

    results.localityByAllocationClass.fill(FrameLocality::Unknown, inputs.objects().numberOfAllocationClasses());
    forEachIndexInParallel(inputs.loops(), results.localityByAllocationClass.size(), [&](unsigned index) {
        results.localityByAllocationClass[index] = computeLocalityOf(sinks, AllocationClassID(index));
    });
    return results;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
