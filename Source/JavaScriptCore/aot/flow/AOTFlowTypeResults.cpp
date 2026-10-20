/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowResultFamilies.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTFlowStamps.h"
#include "AOTIRProgram.h"
#include "AOTStringArena.h"
#include "AOTThreads.h"
#include "AOTTypeArena.h"
#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <utility>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr unsigned variablesPerTask = 256;

namespace {

using FieldNodes = Vector<FlowNodeID, 8>;

class PositionConverter {
public:
    PositionConverter(const FlowTypeInputs& inputs, TypeInterningPhase& phase, unsigned task)
        : m_inputs(inputs)
        , m_arena(phase, task)
    {
    }

    FlowPositionUpdate convertPosition(std::span<const FlowNodeID>, const DeclaredType&, uint32_t subject, auto& flow);
    void convertSignature(const FlowFunctionVersion&, FlowSignatureUpdate&, FunctionFlow&);
    void convertBody(const FlowFunctionVersion&, FlowBodyUpdate&, FlowSiteTables&);
    const FlowPositionCounts& counts() const { return m_counts; }

private:
    const Program& program() const { return m_inputs.families.program(); }
    const FlowGraph& graph() const { return m_inputs.families.graph(); }
    ResultCheck resultCheckOf(const FlowFunctionVersion& version) const { return m_inputs.tailCalls.resultChecksByVersion[version.id.index()]; }

    void appendCaptureNodes(FunctionID, uint32_t captureIndex, Vector<FlowNodeID, 4>&) const;
    TypeID predictPosition(std::span<const FlowNodeID>, const DeclaredType&, uint32_t subject);
    void convertEntryTypes(const FlowFunctionVersion&, FlowSignatureUpdate&, FunctionFlow&);
    FlowPositionUpdate convertResult(const FlowFunctionVersion&, uint32_t index, FunctionFlow&);
    void convertResultTypes(const FlowFunctionVersion&, Vector<FlowPositionUpdate, 1>&, FunctionFlow&);

    const FlowTypeInputs& m_inputs;
    TypeArena m_arena;
    FlowPositionCounts m_counts;
};

class TypeResultsComputation {
public:
    TypeResultsComputation(const FlowTypeInputs& inputs, const FlowTypeOutputs& outputs)
        : m_inputs(inputs)
        , m_outputs(outputs)
        , m_numberOfLayouts(static_cast<unsigned>(inputs.families.program().layouts.size()))
        , m_numberOfVariableChunks(static_cast<unsigned>((inputs.families.program().variables.size() + variablesPerTask - 1) / variablesPerTask))
    {
    }

    void compute(TypeTable&);

private:
    const Program& program() const { return m_inputs.families.program(); }
    const FlowGraph& graph() const { return m_inputs.families.graph(); }
    unsigned numberOfTasks() const { return firstFunctionTask() + m_inputs.versions.functionCountAsRead; }
    unsigned firstFunctionTask() const { return m_numberOfLayouts + m_numberOfVariableChunks; }
    std::pair<uint32_t, uint32_t> variablesOfChunk(unsigned chunk) const;

    void allocateUpdates();
    void runTask(unsigned task, PositionConverter&);
    FieldNodes collectFieldNodes(const FlowFieldSubject&) const;
    void convertLayout(LayoutID, PositionConverter&);
    void convertAppendedFields(LayoutID, PositionConverter&);
    void convertVariables(unsigned chunk, PositionConverter&);
    void convertFunction(FunctionID, PositionConverter&);
    void remapTask(unsigned task, const TypeRemap&);

    const FlowTypeInputs& m_inputs;
    const FlowTypeOutputs& m_outputs;
    unsigned m_numberOfLayouts;
    unsigned m_numberOfVariableChunks;
};

} // anonymous namespace

void FlowPositionCounts::add(const FlowPositionCounts& other)
{
    proven += other.proven;
    unproven += other.unproven;
    narrowed += other.narrowed;
    removedClaims += other.removedClaims;
    remainingClaims += other.remainingClaims;
}

static void recordLoss(auto& flow, const FlowSiteLoss& loss)
{
    if (flow.cause != UnprovenCause::Unknown || loss.cause == UnprovenCause::Unknown)
        return;
    flow.cause = loss.cause;
    flow.witness = loss.witness;
}

// A prediction lies inside the type of its position. Where flow proves nothing about the objects, what the checker expects of them still stands.
static TypeID narrowPredictionAsRead(const Program& program, const DeclaredType& declared, const FlowPositionType& converted, TypeID type, TypeArena& arena)
{
    if (!converted.hasLoss())
        return type;
    Type narrowed = program.at(declared.predicted).intersectWith(converted.type, arena);
    if (narrowed.isNever() || !narrowed.isSubsetOf(converted.type, program))
        return type;
    return arena.intern(narrowed);
}

static FlowPositionUpdate recordPosition(const FlowTypeConverter& converter, const DeclaredType& declared, const Type& asRead, FlowPositionType&& converted, TypeArena& arena, FlowPositionCounts& counts)
{
    const Program& program = converter.solution().program;
    FlowPositionUpdate update;
    TypeID type = arena.intern(converted.type);
    if (type != declared.type)
        update.type = type;
    if (std::optional<Type> prediction = converter.refinePrediction(declared.predicted ? declared.predicted : declared.written, converted, arena))
        update.predicted = arena.intern(*prediction);
    else if (update.type && declared.predicted)
        update.predicted = narrowPredictionAsRead(program, declared, converted, update.type, arena);
    // The prediction of a promised int32 is the promise. What reaches the position does not take it back, and where no int32 reaches it nothing is predicted.
    if (declared.flags.contains(PositionFlags::IsInt32Promise)) {
        if (update.predicted && !arena.at(update.predicted).isInt32OrNullish())
            update.predicted = { };
        if (!update.predicted && update.type && declared.predicted && !program.at(declared.predicted).overlaps(converted.type, program))
            update.predicted = TypeTable::never;
    }

    if (asRead.hasObjects()) {
        if (converted.hasLoss())
            ++counts.unproven;
        else
            ++counts.proven;
    }
    counts.narrowed += !!update.type;
    return update;
}

FlowPositionUpdate PositionConverter::convertPosition(std::span<const FlowNodeID> nodes, const DeclaredType& declared, uint32_t subject, auto& flow)
{
    if (nodes.empty())
        return { };

    const Type& asRead = program().at(declared.type);
    FlowPositionType converted = m_inputs.converter.convert(nodes, asRead, subject, m_arena);
    if (converted.isEmpty || converted.type.isNever())
        return { };

    recordLoss(flow, converted.loss);
    return recordPosition(m_inputs.converter, declared, asRead, WTF::move(converted), m_arena, m_counts);
}

static_assert(!static_cast<unsigned>(EntryParameter::Capture), "The captures are the first entry parameters.");

void PositionConverter::appendCaptureNodes(FunctionID function, uint32_t captureIndex, Vector<FlowNodeID, 4>& nodes) const
{
    for (FlowObjectID closure : m_inputs.families.objects().closuresOf(function))
        nodes.append(graph().capture(closure, captureIndex));
}

TypeID PositionConverter::predictPosition(std::span<const FlowNodeID> nodes, const DeclaredType& declared, uint32_t subject)
{
    FlowPositionType converted = m_inputs.converter.convert(nodes, program().at(declared.type), subject, m_arena);
    if (converted.isEmpty || converted.type.isNever())
        return { };
    std::optional<Type> prediction = m_inputs.converter.refinePrediction(declared.predicted ? declared.predicted : declared.written, converted, m_arena);
    TypeID predicted = m_arena.intern(prediction ? *prediction : converted.type);
    return predicted == declared.type ? TypeID() : predicted;
}

void PositionConverter::convertEntryTypes(const FlowFunctionVersion& version, FlowSignatureUpdate& update, FunctionFlow& flow)
{
    const Function& function = program().at(version.body);
    const FlowSummary& summary = graph().summaryOf(version.body);
    Vector<FlowPositionUpdate> updates(function.numberOfEntryParameters());
    Vector<TypeID> predictionsOfGenericVersion(function.numberOfEntryParameters());
    bool hasUpdate = false;
    bool hasPrediction = false;
    bool takesUnknownOperands = std::ranges::any_of(version.instances, [&](FlowInstanceID instance) {
        return graph().at(instance).context == FlowContext::Generic;
    });
    for (uint32_t index = 0; index < updates.size(); ++index) {
        const DeclaredType* declared = function.declaredEntryParameter(index);
        if (!declared)
            continue;
        Vector<FlowNodeID, 4> nodes = nodesOf(graph(), version, summary.entryParameter(index));
        // Every closure stores its captures, but only a closure that an instance of this version is called with passes them on.
        if (function.entryParameterKind(index) == EntryParameter::Capture)
            appendCaptureNodes(version.body, index, nodes);

        // The unknown object in the parameter is the loss, and what the node adds beside it is predicted.
        FlowNodeID prediction = m_inputs.families.records().predictedEntryParameter(version.body, index);
        if (prediction && takesUnknownOperands)
            nodes.append(prediction);
        updates[index] = convertPosition(nodes.span(), *declared, version.body.index(), flow);
        hasUpdate |= !updates[index].isEmpty();

        if (prediction && !takesUnknownOperands) {
            nodes.append(prediction);
            predictionsOfGenericVersion[index] = predictPosition(nodes.span(), *declared, version.body.index());
            hasPrediction |= !!predictionsOfGenericVersion[index];
        }
    }
    if (hasUpdate)
        update.entryParameters = WTF::move(updates);
    if (hasPrediction)
        update.predictionsOfGenericVersion = WTF::move(predictionsOfGenericVersion);
}

FlowPositionUpdate PositionConverter::convertResult(const FlowFunctionVersion& version, uint32_t index, FunctionFlow& flow)
{
    const DeclaredType& declared = program().at(version.body).results[index];
    bool isCheckedByCallers = resultCheckOf(version) == ResultCheck::ByCallers;
    const Type& asRead = program().at(isCheckedByCallers ? TypeTable::anything : declared.type);
    Vector<FlowNodeID, 4> nodes = nodesOf(graph(), version, graph().summaryOf(version.body).result(index));
    FlowPositionType converted = m_inputs.converter.convert(nodes.span(), asRead, version.body.index(), m_arena);
    recordLoss(flow, converted.loss);

    FlowPositionUpdate update = recordPosition(m_inputs.converter, declared, asRead, WTF::move(converted), m_arena, m_counts);
    // Callers claim the written type, which the application takes from the type as read only where it sets a type.
    if (isCheckedByCallers && !update.type)
        update.type = declared.type;
    return update;
}

void PositionConverter::convertResultTypes(const FlowFunctionVersion& version, Vector<FlowPositionUpdate, 1>& results, FunctionFlow& flow)
{
    // The result local of a coroutine holds what the body returns, and results[0] is the promise or generator that each call allocates.
    if (graph().summaryOf(version.body).coroutine.resultSite)
        return;

    Vector<FlowPositionUpdate, 1> updates(program().at(version.body).results.size());
    bool hasUpdate = false;
    for (uint32_t index = 0; index < updates.size(); ++index) {
        updates[index] = convertResult(version, index, flow);
        hasUpdate |= !updates[index].isEmpty();
    }
    if (hasUpdate)
        results = WTF::move(updates);
}

void PositionConverter::convertSignature(const FlowFunctionVersion& version, FlowSignatureUpdate& update, FunctionFlow& flow)
{
    if (!version.isReached() || !program().at(version.body).mayChangeSignature() || m_inputs.families.solution.isPastWorkLimit(version.body))
        return;

    convertEntryTypes(version, update, flow);
    convertResultTypes(version, update.results, flow);
}

void PositionConverter::convertBody(const FlowFunctionVersion& version, FlowBodyUpdate& update, FlowSiteTables& sites)
{
    if (!version.isReached())
        return;

    FlowBodyTask task { m_inputs, version, m_arena, update, m_counts };
    computeValueProofs(task);
    computeStamps(task, sites);
    update.tailCalls = m_inputs.tailCalls.sitesByVersion[version.id.index()];
}

std::pair<uint32_t, uint32_t> TypeResultsComputation::variablesOfChunk(unsigned chunk) const
{
    uint32_t first = chunk * variablesPerTask;
    return { first, std::min(first + variablesPerTask, static_cast<uint32_t>(program().variables.size())) };
}

void TypeResultsComputation::allocateUpdates()
{
    FlowApplication& application = m_outputs.application;
    application.layouts = Vector<FlowLayoutUpdate>(m_numberOfLayouts);
    application.variables = Vector<FlowVariableUpdate>(program().variables.size());
    application.signatures = Vector<FlowSignatureUpdate>(m_inputs.versions.functionCountAsRead);
    application.bodies = Vector<FlowBodyUpdate>(m_inputs.versions.functionCount());
}

FieldNodes TypeResultsComputation::collectFieldNodes(const FlowFieldSubject& subject) const
{
    FieldNodes nodes;
    forEachFieldNode(m_inputs.families, subject, [&](FlowObjectID, FlowNodeID node) {
        nodes.append(node);
        for (const FlowKeptDefinition& kept : m_inputs.fieldDefinitions.keptIn(node))
            nodes.append(kept.value);
    });
    return nodes;
}

void TypeResultsComputation::convertLayout(LayoutID id, PositionConverter& converter)
{
    const Layout& layout = program().at(id);
    Vector<FieldFlow>& flows = m_outputs.fields.declared[id.index()];
    RELEASE_ASSERT(flows.size() == layout.fields.size());

    Vector<FlowFieldUpdate> fields(layout.fields.size());
    for (uint32_t index = 0; index < fields.size(); ++index) {
        const Field& field = layout.fields[index];
        if (field.flags.contains(FieldFlags::IsInherited))
            continue;
        FieldNodes nodes = collectFieldNodes({ id, index, field.key });
        fields[index].position = converter.convertPosition(nodes.span(), field.declared, id.index(), flows[index].location);
    }
    m_outputs.application.layouts[id.index()].fields = WTF::move(fields);
    convertAppendedFields(id, converter);
}

void TypeResultsComputation::convertAppendedFields(LayoutID id, PositionConverter& converter)
{
    const Vector<PropertyKeyID>& keys = m_inputs.appendedKeys[id.index()];
    Vector<FieldFlow>& flows = m_outputs.fields.appended[id.index()];
    RELEASE_ASSERT(flows.size() == keys.size());

    DeclaredType declared;
    declared.type = TypeTable::anything;
    Vector<FlowAppendedField> fields(keys.size());
    for (uint32_t index = 0; index < fields.size(); ++index) {
        FlowAppendedField& field = fields[index];
        field.key = keys[index];
        FieldNodes nodes = collectFieldNodes({ id, std::nullopt, field.key });
        field.position = converter.convertPosition(nodes.span(), declared, id.index(), flows[index].location);
        if (!field.position.type)
            field.position.type = declared.type;
    }
    m_outputs.application.layouts[id.index()].appendedFields = WTF::move(fields);
}

void TypeResultsComputation::convertVariables(unsigned chunk, PositionConverter& converter)
{
    auto [first, end] = variablesOfChunk(chunk);
    for (uint32_t index = first; index < end; ++index) {
        std::array nodes { graph().variable(VariableID(index)) };
        m_outputs.application.variables[index].position = converter.convertPosition(nodes, program().variables[index].declared, index, m_outputs.variables[index]);
    }
}

void TypeResultsComputation::convertFunction(FunctionID id, PositionConverter& converter)
{
    fillAccessSites(m_inputs, id, m_outputs.sites);
    if (!program().at(id).hasBody())
        return;

    FlowApplication& application = m_outputs.application;
    FlowFunctionVersion provenVersion = findFunctionVersion(graph(), m_inputs.versions, id);
    converter.convertSignature(provenVersion, application.signatures[id.index()], m_outputs.functions[id.index()]);
    converter.convertBody(provenVersion, application.bodies[id.index()], m_outputs.sites);
    // The versions of a function share its access sites, so one task converts them all.
    m_inputs.versions.forEachCallSiteVersionOf(id, [&](FunctionID version) {
        converter.convertBody(findFunctionVersion(graph(), m_inputs.versions, version), application.bodies[version.index()], m_outputs.sites);
    });

    FunctionID genericID = m_inputs.versions.genericVersionOf(id);
    if (!genericID)
        return;
    FlowFunctionVersion genericVersion = findFunctionVersion(graph(), m_inputs.versions, genericID);
    converter.convertBody(genericVersion, application.bodies[genericID.index()], m_outputs.sites);
}

void TypeResultsComputation::runTask(unsigned task, PositionConverter& converter)
{
    if (task < m_numberOfLayouts)
        convertLayout(LayoutID(task), converter);
    else if (task < firstFunctionTask())
        convertVariables(task - m_numberOfLayouts, converter);
    else
        convertFunction(FunctionID(task - firstFunctionTask()), converter);
}

void TypeResultsComputation::remapTask(unsigned task, const TypeRemap& remap)
{
    FlowApplication& application = m_outputs.application;
    if (task < m_numberOfLayouts) {
        remapTypes(application.layouts[task], remap);
        return;
    }
    if (task < firstFunctionTask()) {
        auto [first, end] = variablesOfChunk(task - m_numberOfLayouts);
        for (uint32_t index = first; index < end; ++index)
            remapTypes(application.variables[index].position, remap);
        return;
    }

    FunctionID id(task - firstFunctionTask());
    m_inputs.versions.forEachCallSiteVersionOf(id, [&](FunctionID version) {
        remapTypes(application.bodies[version.index()], remap);
    });
    remapTypes(application.signatures[id.index()], remap);
    remapTypes(application.bodies[id.index()], remap);
    if (FunctionID genericID = m_inputs.versions.genericVersionOf(id))
        remapTypes(application.bodies[genericID.index()], remap);
}

static void countPositions(BuildContext& build, std::span<const FlowPositionCounts> countsByTask)
{
    FlowPositionCounts total;
    for (const FlowPositionCounts& counts : countsByTask)
        total.add(counts);
    build.statistics().count(StatisticID::FlowProvenPositions, total.proven);
    build.statistics().count(StatisticID::FlowUnprovenPositions, total.unproven);
    build.statistics().count(StatisticID::FlowNarrowedPositions, total.narrowed);
}

void TypeResultsComputation::compute(TypeTable& types)
{
    allocateUpdates();

    const ParallelLoops& loops = m_inputs.families.loops();
    Vector<FlowPositionCounts> countsByTask(numberOfTasks());
    TypeInterningPhase phase(types, numberOfTasks());
    forEachIndexInParallel(loops, numberOfTasks(), [&](unsigned task) {
        PositionConverter converter(m_inputs, phase, task);
        runTask(task, converter);
        countsByTask[task] = converter.counts();
    });
    TypeRemap remap = phase.finish(StringRemap { });
    forEachIndexInParallel(loops, numberOfTasks(), [&](unsigned task) {
        if (!remap.isIdentityFor(task))
            remapTask(task, remap);
    });

    countPositions(m_inputs.families.build(), countsByTask.span());
}

void computeTypeResults(const FlowTypeInputs& inputs, const FlowTypeOutputs& outputs, TypeTable& types)
{
    TypeResultsComputation(inputs, outputs).compute(types);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
