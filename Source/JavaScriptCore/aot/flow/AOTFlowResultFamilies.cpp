/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowResultFamilies.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTFlowAnalysis.h"
#include "AOTFlowBuiltinCalls.h"
#include "AOTFlowBuiltinLocations.h"
#include "AOTFlowObjectState.h"
#include "AOTFlowStamps.h"
#include "AOTPhaseTimes.h"
#include "AOTSelectTypedResultsTargets.h"
#include <algorithm>
#include <compare>
#include <functional>
#include <memory>
#include <span>
#include <tuple>
#include <wtf/BitVector.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct FlowFamilies {
    FlowAppendedKeys appendedKeys;
    Vector<PropertyKeyID> symbolsCreatedOnce;
    Vector<ClassFlow> classes;
    FlowShapeResults shapes;
    FlowFieldResults fields;
    FlowEvaluationResults evaluation;
    Vector<LocationFlow> variables;
    Vector<LayoutFlow> layouts;
    FlowLiteralKeyOrders literalKeyOrders;
    FlowReachabilityResults reachability;
    FlowContentsResults contents;
    FlowVersionPlan versions;
    FlowBoundFunctionResults boundFunctions;
    FlowTailCallResults tailCalls;
};

struct TypedResultCallSite {
    FunctionID function;
    ValueID call;
    TypeID type;

    bool isSameCallAs(const TypedResultCallSite& other) const { return function == other.function && call == other.call; }

    friend bool operator==(const TypedResultCallSite&, const TypedResultCallSite&) = default;
    friend auto operator<=>(const TypedResultCallSite&, const TypedResultCallSite&) = default;
};

struct DataNodeSite {
    FunctionID function;
    ValueID data;
    uint32_t ordinal { 0 };
    AllocationSiteID site;

    bool isInSameTreeAs(const DataNodeSite& other) const { return function == other.function && data == other.data; }

    friend auto operator<=>(const DataNodeSite&, const DataNodeSite&) = default;
};

} // anonymous namespace

static void checkTableSizes(size_t expectedSize, const auto&... tables)
{
    RELEASE_ASSERT(((tables.size() == expectedSize) && ...));
}

static FlowConversionTables conversionTablesOf(const FlowFamilies& families)
{
    return { families.shapes.shapes.span(), families.contents.byObject.span(), families.symbolsCreatedOnce.span(), families.boundFunctions.functionByObject.span(), families.contents.elementsBySite.span(), families.layouts.span() };
}

static FlowFamilies computeRecordFamilies(const FlowFamilyInputs& inputs, const FlowBuiltinTables& builtinLocations, FlowCallSiteVersions&& callSiteVersions, TypeTable& types, PhaseTimes& times)
{
    FlowFamilies families;

    families.symbolsCreatedOnce = computeCreatedSymbolResults(inputs);
    times.didFinish("created symbols"_s);

    families.appendedKeys = computeAppendedFieldKeys(inputs, families.symbolsCreatedOnce.span());
    times.didFinish("appended fields"_s);

    families.classes = computeClassResults(inputs);
    times.didFinish("classes"_s);

    families.shapes = computeShapeResults(inputs, families.classes.span(), families.appendedKeys, builtinLocations);
    times.didFinish("shapes"_s);

    families.fields = computeFieldResults(inputs, families.appendedKeys);
    times.didFinish("fields"_s);

    computeDeferredClosureResults(inputs, families.appendedKeys, families.fields);
    times.didFinish("deferred closures"_s);

    families.evaluation = computeEvaluationResults(inputs);
    times.didFinish("evaluation"_s);

    families.variables = computeVariableResults(inputs, families.evaluation.modules.span());
    times.didFinish("variables"_s);

    families.layouts = computeLayoutResults(inputs, families.shapes.shapes.span());
    times.didFinish("layouts"_s);

    families.literalKeyOrders = computeLiteralKeyOrders(inputs, families.layouts.mutableSpan());
    times.didFinish("literal key orders"_s);

    families.reachability = computeReachabilityResults(inputs);
    times.didFinish("reachability"_s);

    families.contents = computeContentsResults(inputs, families.shapes.shapes.span(), types);
    times.didFinish("contents"_s);

    FlowTypeConverter converter(inputs.solution, conversionTablesOf(families));
    families.versions = computeVersionResults(inputs, converter, types);
    families.versions.callSite = WTF::move(callSiteVersions);
    times.didFinish("versions"_s);

    families.boundFunctions = computeBoundFunctionResults(inputs, families.shapes.allocationClasses.span(), converter, families.versions, types);
    times.didFinish("bound functions"_s);

    families.tailCalls = computeTailCallResults(inputs, converter, families.versions, types);
    times.didFinish("tail calls"_s);

    return families;
}

static Vector<TypedResultCallSite> collectTypedResultCallSites(const FlowFamilyInputs& inputs)
{
    const BuiltinTable& builtins = inputs.solution.builtins();
    const FlowGraph& graph = inputs.graph();

    Vector<TypedResultCallSite> sites;
    for (const FlowBuiltinCallLink& link : inputs.records().builtinCallLinks()) {
        if (!link.callerInstance || !link.site || !link.overload || graph.isHost(link.callerInstance))
            continue;
        if (builtins.at(link.overload).result.typedResultForm == TypedResultForm::None)
            continue;
        // Whether the call ran before its function gave up depends on the order of the solve.
        FunctionID caller = graph.at(link.callerInstance).function;
        if (inputs.solution.isPastWorkLimit(caller))
            continue;
        if (TypeID type = typedResultTypeOf(inputs.solution.callState, link.callerInstance, link.site))
            sites.append({ caller, link.site, type });
    }
    sortAndRemoveDuplicates(inputs.loops(), sites);
    return sites;
}

// An unknown callee may be the built-in at run time, and the result of such an instance holds no typed object.
static bool isModeledInEveryInstance(const FlowFamilyInputs& inputs, const TypedResultCallSite& site)
{
    bool isModeled = true;
    inputs.graph().forEachInstanceOf(site.function, [&](FlowInstanceID instance) {
        if (inputs.calls.hasUnknownCalleeAt(instance, site.call) && !typedResultTypeOf(inputs.solution.callState, instance, site.call))
            isModeled = false;
    });
    return isModeled;
}

static Vector<FlowTypedResultCall> computeTypedResultCalls(const FlowFamilyInputs& inputs, const FlowAccessSiteNumbering& numbering)
{
    Vector<TypedResultCallSite> sites = collectTypedResultCallSites(inputs);

    Vector<FlowTypedResultCall> calls;
    FunctionID function;
    Vector<AccessSiteID> accessSites;
    for (size_t index = 0; index < sites.size(); ++index) {
        const TypedResultCallSite& site = sites[index];
        bool hasSecondType = (index && site.isSameCallAs(sites[index - 1])) || (index + 1 < sites.size() && site.isSameCallAs(sites[index + 1]));
        if (hasSecondType || !isModeledInEveryInstance(inputs, site))
            continue;
        if (function != site.function) {
            function = site.function;
            accessSites = accessSitesByValue(inputs.program().at(function), numbering, function);
        }
        if (AccessSiteID accessSite = accessSites[site.call.index()])
            calls.append({ accessSite, site.type, layoutsReachedBy(inputs.program(), site.type) });
    }
    return calls;
}

static Vector<AllocationSiteID> computeTypedResultSites(const FlowFamilyInputs& inputs, std::span<const AllocationSite> allocationSites)
{
    Vector<AllocationSiteID> lowestSiteByLayout(inputs.program().layouts.size());
    for (unsigned index = 0; index < allocationSites.size(); ++index) {
        AllocationSiteID id(index);
        const AllocationSite& site = allocationSites[index];
        if (inputs.objects().at(id).description.origin != FlowSiteOrigin::TypedResult || !site.layout || !site.allocationClass)
            continue;
        AllocationSiteID& lowestSite = lowestSiteByLayout[site.layout.index()];
        if (!lowestSite)
            lowestSite = id;
    }
    return lowestSiteByLayout;
}

static FlowDataNodeStamp dataNodeStampOf(std::span<const AllocationSite> allocationSites, const FlowTypeConverter& converter, AllocationSiteID id)
{
    const AllocationSite& site = allocationSites[id.index()];
    bool keepsShape = site.layout && site.allocationClass && converter.tables().shapesByAllocationClass[site.allocationClass.index()].state == FlowShapeState::KeepsShape;
    return { id, site.allocationClass, keepsShape, converter.elementsAllocatedAt(id) };
}

static Vector<FlowDataNode> computeDataNodes(const FlowFamilyInputs& inputs, std::span<const AllocationSite> allocationSites, const FlowTypeConverter& converter)
{
    Vector<DataNodeSite> sites;
    for (unsigned index = 0; index < allocationSites.size(); ++index) {
        AllocationSiteID id(index);
        const FlowSite& site = inputs.objects().at(id);
        if (site.description.origin == FlowSiteOrigin::DataNode)
            sites.append({ site.function, site.description.value, site.description.ordinal, id });
    }
    parallelSort(inputs.loops(), sites.mutableSpan(), std::less<DataNodeSite> { });

    Vector<FlowDataNode> nodes;
    nodes.reserveInitialCapacity(sites.size());
    AllocationSiteID rootSite;
    for (size_t index = 0; index < sites.size(); ++index) {
        const DataNodeSite& site = sites[index];
        if (!index || !site.isInSameTreeAs(sites[index - 1]))
            rootSite = site.ordinal ? AllocationSiteID() : site.site;
        if (rootSite)
            nodes.append({ rootSite, site.ordinal, dataNodeStampOf(allocationSites, converter, site.site) });
    }
    parallelSort(inputs.loops(), nodes.mutableSpan(), [](const FlowDataNode& a, const FlowDataNode& b) {
        return std::tuple(a.rootSite, a.ordinal) < std::tuple(b.rootSite, b.ordinal);
    });
    return nodes;
}

static void computeTypesAndStamps(const FlowFamilyInputs& inputs, FlowFamilies& families, FlowApplication& application, FlowResultTables& tables, TypeTable& types)
{
    application.versions = families.versions;
    application.boundFunctions = WTF::move(families.boundFunctions.plan);
    application.accessSites = numberAccessSites(inputs.program(), inputs.build());

    FlowSiteTables& sites = tables.sites;
    uint32_t numberOfAccessSites = application.accessSites.numberOfSites();
    sites.accessSites.grow(numberOfAccessSites);
    sites.accessSiteLosses.grow(numberOfAccessSites);
    sites.resultSites.grow(numberOfAccessSites);
    sites.callsOfBoundFunctions.fill(false, numberOfAccessSites);

    FlowTypeConverter converter(inputs.solution, conversionTablesOf(families));
    FlowFieldDefinitionPlan fieldDefinitions = planFieldDefinitions(inputs);
    computeTypeResults(
        FlowTypeInputs { inputs, converter, application.versions, application.boundFunctions, fieldDefinitions, families.appendedKeys, application.accessSites, families.tailCalls },
        FlowTypeOutputs { application, families.fields, families.variables, families.reachability.functions, sites },
        types);

    sites.allocationClasses = WTF::move(families.shapes.allocationClasses);
    fillAllocationSites(inputs, sites);
    sites.typedResultCalls = computeTypedResultCalls(inputs, application.accessSites);
    sites.typedResultSites = computeTypedResultSites(inputs, sites.allocationSites.span());
    sites.dataNodes = computeDataNodes(inputs, sites.allocationSites.span(), converter);
}

// An export is a live binding, so its value changes, and a namespace object can become a prototype and reach any code.
static constexpr OptionSet<ObjectMutation> mutationsRefusedByNamespaceObjects {
    ObjectMutation::DeleteField,
    ObjectMutation::RedefineProperty,
    ObjectMutation::AddProperty,
    ObjectMutation::ChangeIntegrity,
    ObjectMutation::ChangePrototype,
    ObjectMutation::LoseShape,
};

static void meetAllocationClasses(std::span<const AllocationClass> allocationClasses, LayoutFlow& flow)
{
    if (flow.allocationClasses.isEmpty())
        return;

    flow.impossibleMutations = allObjectMutations;
    flow.keys = KeyEnumeration::NeverEnumerated;
    flow.addedKeysAreComplete = true;
    for (AllocationClassID id : flow.allocationClasses) {
        const AllocationClass& allocationClass = allocationClasses[id.index()];
        flow.impossibleMutations = flow.impossibleMutations & allocationClass.impossibleMutations;
        if (allocationClass.keys != KeyEnumeration::NeverEnumerated)
            flow.keys = KeyEnumeration::Unknown;
        flow.addedKeysAreComplete = flow.addedKeysAreComplete && allocationClass.addedKeysAreComplete;
        flow.addedKeys.appendVector(allocationClass.addedKeys);
    }
    std::ranges::sort(flow.addedKeys);
    removeRepeatedElements(flow.addedKeys);
}

static void assembleLayoutUpdate(const FlowFamilyInputs& inputs, FlowFamilies& families, std::span<const AllocationClass> allocationClasses, LayoutID layout, FlowLayoutUpdate& update)
{
    update.flow = WTF::move(families.layouts[layout.index()]);
    update.literalKeyOrders = WTF::move(families.literalKeyOrders[layout.index()]);
    update.classFlow = WTF::move(families.classes[layout.index()]);
    meetAllocationClasses(allocationClasses, update.flow);
    if (update.flow.allocation == AllocationState::NeverAllocated && inputs.isEnabled(FlowResultKind::HolderMutations, layout.index()))
        update.flow.impossibleMutations = allObjectMutations;
    if (inputs.program().at(layout).kind == LayoutKind::ModuleNamespace)
        update.flow.impossibleMutations.add(mutationsRefusedByNamespaceObjects);

    const Vector<FieldFlow>& declared = families.fields.declared[layout.index()];
    checkTableSizes(update.fields.size(), declared);
    for (size_t index = 0; index < declared.size(); ++index)
        update.fields[index].flow = declared[index];

    const Vector<FieldFlow>& appended = families.fields.appended[layout.index()];
    checkTableSizes(update.appendedFields.size(), appended);
    for (size_t index = 0; index < appended.size(); ++index)
        update.appendedFields[index].flow = appended[index];
}

static void assembleApplication(const FlowFamilyInputs& inputs, FlowFamilies& families, const FlowResultTables& tables, FlowApplication& application)
{
    const ParallelLoops& loops = inputs.loops();

    checkTableSizes(application.layouts.size(), families.layouts, families.literalKeyOrders, families.classes, families.fields.declared, families.fields.appended);
    forEachIndexInParallel(loops, application.layouts.size(), [&](unsigned index) {
        assembleLayoutUpdate(inputs, families, tables.sites.allocationClasses.span(), LayoutID(index), application.layouts[index]);
    });

    checkTableSizes(application.variables.size(), families.variables);
    forEachIndexInParallel(loops, application.variables.size(), [&](unsigned index) {
        application.variables[index].flow = families.variables[index];
    });

    checkTableSizes(application.signatures.size(), families.reachability.provenCapabilities);
    RELEASE_ASSERT(application.signatures.size() <= families.tailCalls.resultChecksByVersion.size());
    forEachIndexInParallel(loops, application.signatures.size(), [&](unsigned index) {
        FlowSignatureUpdate& signature = application.signatures[index];
        signature.provenCapabilities = families.reachability.provenCapabilities[index];
        signature.resultCheck = families.tailCalls.resultChecksByVersion[index];
    });
}

static FunctionFlow createCallSiteVersionFlow(const FunctionFlow& source)
{
    FunctionFlow flow = source;
    flow.callers = CallerState::AllKnown;
    flow.executions = ExecutionCount::Unknown;
    flow.closureUse = ClosureUse::Unknown;
    flow.genericVersion = { };
    return flow;
}

static FunctionFlow createGenericVersionFlow(const FunctionFlow& provenVersion)
{
    FunctionFlow flow = provenVersion;
    flow.callers = CallerState::Unknown;
    flow.closureUse = ClosureUse::Unknown;
    flow.genericVersion = { };
    return flow;
}

static void assembleFunctionFlows(FlowFamilies& families, const FlowVersionPlan& versions, Vector<FunctionFlow>& functions)
{
    checkTableSizes(versions.functionCountAsRead, families.reachability.functions);
    checkTableSizes(versions.functionCount(), families.tailCalls.resultChecksByVersion);

    functions = WTF::move(families.reachability.functions);
    functions.reserveCapacity(versions.functionCount());
    for (uint32_t index = versions.functionCountAsRead; index < versions.firstCallSiteVersion(); ++index) {
        FunctionID genericVersion(index);
        FunctionID provenVersion = versions.provenVersionOf(genericVersion);
        functions.append(createGenericVersionFlow(functions[provenVersion.index()]));
        functions[provenVersion.index()].genericVersion = genericVersion;
    }
    for (const FlowCallSiteVersion& version : versions.callSite.versions)
        functions.append(createCallSiteVersionFlow(functions[version.body.index()]));
    for (size_t index = 0; index < functions.size(); ++index)
        functions[index].resultCheck = families.tailCalls.resultChecksByVersion[index];
}

static BitVector findFunctionsSeeingOnlyFinishedObjects(const FlowFamilyInputs& inputs)
{
    size_t numberOfFunctions = inputs.program().functions.size();
    BitVector functions(numberOfFunctions);
    for (size_t index = 0; index < numberOfFunctions; ++index) {
        if (inputs.isEnabled(FlowResultKind::Initialization, index) && seesOnlyFinishedObjects(inputs.solution, FunctionID(index)))
            functions.quickSet(index);
    }
    return functions;
}

static void assembleFunctionTables(const FlowFamilyInputs& inputs, FlowFamilies& families, const FlowApplication& application, FlowResultTables& tables, PhaseTimes& times)
{
    const FlowVersionPlan& versions = application.versions;
    RELEASE_ASSERT(versions.functionCountAsRead == inputs.program().functions.size());

    tables.functions.functionCountAsRead = versions.functionCountAsRead;
    assembleFunctionFlows(families, versions, tables.functions.functions);

    FlowEscapeResults escape = computeEscapeResults(inputs, versions);
    checkTableSizes(versions.functionCount(), escape.parameterSummaries);
    checkTableSizes(tables.sites.allocationClasses.size(), escape.localityByAllocationClass);
    tables.functions.parameterSummaries = WTF::move(escape.parameterSummaries);
    tables.functions.framePointerParameterUses = WTF::move(escape.framePointerParameterUses);
    tables.functions.storedValueUses = WTF::move(escape.storedValueUses);
    tables.functions.storedFramePointerUses = WTF::move(escape.storedFramePointerUses);
    for (size_t index = 0; index < escape.localityByAllocationClass.size(); ++index)
        tables.sites.allocationClasses[index].locality = escape.localityByAllocationClass[index];
    times.didFinish("escape"_s);

    tables.functions.indirectCalls = computeIndirectCalls(inputs, versions, application.accessSites);
    times.didFinish("indirect calls"_s);

    tables.functions.functionsSeeingOnlyFinishedObjects = findFunctionsSeeingOnlyFinishedObjects(inputs);
}

static LegacyRegExpStatics computeLegacyRegExpStatics(const FlowFamilyInputs& inputs)
{
    const BuiltinTable& builtins = inputs.solution.builtins();
    const FlowRecords& records = inputs.records();
    BuiltinID constructor = builtins.canonicalID(builtins.at(BuiltinClasses::RegExp).constructor);
    if (!inputs.isEnabled(FlowResultKind::ObjectUses, constructor.index()) || records.unseenCodeCanRun())
        return LegacyRegExpStatics::Unknown;

    if (FlowObjectID object = inputs.objects().findBuiltin(constructor)) {
        const FlowObjectFacts& facts = inputs.factsOf(object);
        if (facts.mutations.containsAny(reachMutations) || facts.uses.containsAny(unnamedReadUses))
            return LegacyRegExpStatics::Unknown;
    }

    auto isMentionedLegacyStatic = [&](BuiltinID member) {
        if (builtins.at(member).mutability != BuiltinMutability::ChangesOnItsOwn)
            return false;
        return !records.builtinUse(member).uses.isEmpty() || !records.builtinUse(builtins.canonicalID(member)).uses.isEmpty();
    };
    if (std::ranges::any_of(builtins.membersOf(constructor), isMentionedLegacyStatic))
        return LegacyRegExpStatics::Unknown;
    return LegacyRegExpStatics::NeverRead;
}

static void assembleProgramTables(const FlowFamilyInputs& inputs, FlowFamilies& families, const FlowApplication& application, FlowResultTables& tables, PhaseTimes& times)
{
    checkTableSizes(inputs.program().modules.size(), families.evaluation.modules);
    tables.wholeProgram.modules = WTF::move(families.evaluation.modules);
    tables.wholeProgram.instances = families.reachability.instances;

    tables.builtins.legacyRegExpStatics = computeLegacyRegExpStatics(inputs);
    times.didFinish("legacy RegExp statics"_s);

    computeFeatureResults(inputs, tables.builtins, tables.wholeProgram.unseenCode);
    times.didFinish("features"_s);

    tables.sites.sinkOperands = computeSinkCandidateResults(inputs, application.accessSites);
    times.didFinish("sink candidates"_s);
}

static uint64_t countProvenContentsSites(const FlowFamilyInputs& inputs, std::span<const FlowContentsResult> contentsByObject)
{
    BitVector provenSites;
    BitVector unprovenSites;
    for (unsigned index = 0; index < contentsByObject.size(); ++index) {
        FlowObjectID object(index);
        AllocationSiteID site = inputs.objects().at(object).site;
        if (!site || !inputs.factsOf(object).events.contains(FlowObjectEvent::Allocated))
            continue;
        if (inputs.graph().isHost(inputs.objects().at(site).function))
            continue;
        BitVector& sites = contentsByObject[index].isProven ? provenSites : unprovenSites;
        sites.set(site.index());
    }
    provenSites.exclude(unprovenSites);
    return provenSites.bitCount();
}

static void countLayoutResults(const Program& program, const FlowApplication& application, Statistics& statistics)
{
    uint64_t appendedFields = 0;
    uint64_t provenBases = 0;
    uint64_t deferredClosureFields = 0;
    uint64_t constructorsLeakingThis = 0;
    for (size_t index = 0; index < application.layouts.size(); ++index) {
        const Layout& layout = program.layouts[index];
        const FlowLayoutUpdate& update = application.layouts[index];

        appendedFields += update.appendedFields.size();
        provenBases += layout.has(LayoutFlags::HasHeritage) && update.flow.baseState != BaseState::Unknown;
        constructorsLeakingThis += layout.kind == LayoutKind::Instance
            && update.flow.allocation != AllocationState::NeverAllocated
            && update.flow.thisLeak == ThisLeak::Unknown;
        for (const FlowFieldUpdate& field : update.fields)
            deferredClosureFields += field.flow.location.deferral != ClosureDeferral::Unknown;
        for (const FlowAppendedField& field : update.appendedFields)
            deferredClosureFields += field.flow.location.deferral != ClosureDeferral::Unknown;
    }

    statistics.count(StatisticID::FlowAppendedFields, appendedFields);
    statistics.count(StatisticID::FlowProvenBases, provenBases);
    statistics.count(StatisticID::FlowDeferredClosureFields, deferredClosureFields);
    statistics.count(StatisticID::FlowConstructorsLeakingThis, constructorsLeakingThis);
}

static void countResults(const FlowFamilyInputs& inputs, const FlowFamilies& families, const FlowApplication& application, const FlowResultTables& tables)
{
    Statistics& statistics = inputs.build().statistics();
    countLayoutResults(inputs.program(), application, statistics);

    uint64_t unreachedFunctions = 0;
    for (const FunctionFlow& flow : tables.functions.functions.span().first(tables.functions.functionCountAsRead))
        unreachedFunctions += flow.reachability == FunctionReachability::Unreached;

    uint64_t constantBuiltinLocations = 0;
    uint64_t writtenBuiltinLocations = 0;
    for (const BuiltinLocationFlow& location : tables.builtins.locations) {
        if (isWritten(location))
            ++writtenBuiltinLocations;
        else if (location.state == BuiltinLocationState::Constant)
            ++constantBuiltinLocations;
    }

    statistics.count(StatisticID::FlowGenericVersions, application.versions.provenVersions.size());
    statistics.count(StatisticID::FlowCallSiteVersions, application.versions.callSite.versions.size());
    statistics.count(StatisticID::FlowUnreachedFunctions, unreachedFunctions);
    statistics.count(StatisticID::FlowProvenContentsSites, countProvenContentsSites(inputs, families.contents.byObject.span()));
    statistics.count(StatisticID::FlowConstantBuiltinLocations, constantBuiltinLocations);
    statistics.count(StatisticID::FlowWrittenBuiltinLocations, writtenBuiltinLocations);
}

FlowResults computeFlowResults(const FlowSolution& solution, Program& program, FlowObjectTable& objects, TypeTable& types)
{
    PhaseTimes times(solution.build, PassID::AnalyzeFlow);

    FlowCallIndex calls(solution);
    times.didFinish("call index"_s);

    Vector<ExecutionCount> executions = computeExecutionResults(solution, calls);
    times.didFinish("executions"_s);

    Vector<FlowObjectFacts> facts = computeObjectFacts(solution);
    times.didFinish("object facts"_s);

    FlowObservedLifetimes observedLifetimes = computeObservedLifetimes(solution);
    times.didFinish("observed lifetimes"_s);

    auto hierarchyOfLayouts = makeUnique<FlowHierarchy>(solution, computeBaseResults(solution));
    times.didFinish("bases"_s);

    size_t numberOfDeclaredLayouts = program.layouts.size();
    FlowCallSiteVersions callSiteVersions;
    {
        FlowFamilyInputs inputs { solution, *hierarchyOfLayouts, calls, facts.span(), executions.span(), observedLifetimes };
        Vector<PropertyKeyID> symbolsCreatedOnce = computeCreatedSymbolResults(inputs);
        callSiteVersions = planCallSiteVersions(inputs, symbolsCreatedOnce.span(), objects);
        deriveLayouts(inputs, callSiteVersions, symbolsCreatedOnce.span(), program, objects);
    }
    if (program.layouts.size() != numberOfDeclaredLayouts)
        hierarchyOfLayouts = makeUnique<FlowHierarchy>(solution, computeBaseResults(solution));
    const FlowHierarchy& hierarchy = *hierarchyOfLayouts;
    times.didFinish("derived layouts"_s);

    FlowFamilyInputs inputs { solution, hierarchy, calls, facts.span(), executions.span(), observedLifetimes };
    FlowResultTables tables;
    tables.program = &solution.program;

    computeBuiltinLocationResults(inputs, tables.builtins);
    times.didFinish("built-in locations"_s);

    FlowFamilies families = computeRecordFamilies(inputs, tables.builtins, WTF::move(callSiteVersions), types, times);
    auto application = std::unique_ptr<FlowApplication, FlowApplicationDeleter>(new FlowApplication);

    computeTypesAndStamps(inputs, families, *application, tables, types);
    times.didFinish("types and stamps"_s);

    assembleApplication(inputs, families, tables, *application);
    times.didFinish("application"_s);

    assembleFunctionTables(inputs, families, *application, tables, times);
    assembleProgramTables(inputs, families, *application, tables, times);
    countResults(inputs, families, *application, tables);

    tables.application = WTF::move(application);
    solution.build.destroyLater(WTF::move(families));
    return FlowResults(WTF::move(tables));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
