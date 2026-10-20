/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowStamps.h"

#if ENABLE(AOT)

#include "AOTFlowObjectState.h"
#include "AOTFlowPropertyState.h"
#include "AOTResultRules.h"
#include <algorithm>
#include <compare>
#include <functional>
#include <optional>
#include <span>
#include <tuple>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct StampContext {
    const FlowBodyTask& task;
    FlowSiteTables& sites;
    const Function& function;
    const FlowSummary& summary;
    Vector<AccessSiteID> accessSites;

    const FlowFamilyInputs& families() const { return task.inputs.families; }
    const FlowGraph& graph() const { return families().graph(); }
    const FlowTypeConverter& converter() const { return task.inputs.converter; }
    const FlowVersionPlan& versions() const { return task.inputs.versions; }
    std::span<const FlowInstanceID> instances() const { return task.version.instances.span(); }

    bool isEnabled(FlowResultKind kind) const { return families().isEnabled(kind, task.version.body.index()); }
    bool isPastWorkLimit() const { return families().solution.isPastWorkLimit(task.version.body); }
    // What arrives in an abandoned function after it gave up is in no node and in no link of it.
    bool valuesAreComplete() const { return !isPastWorkLimit(); }
    Vector<FlowNodeID, 4> nodesOf(auto localOrValue) const { return Typed::nodesOf(graph(), task.version, localOrValue); }

    void recordCallOfBoundFunction(ValueID origin) const
    {
        if (AccessSiteID site = accessSites[origin.index()])
            sites.callsOfBoundFunctions[site.index()] = true;
    }

    void recordLoss(ValueID origin, FlowSiteLoss loss) const
    {
        AccessSiteID site = accessSites[origin.index()];
        if (!site)
            return;
        // What no earlier operation explains begins here.
        if (loss.cause != UnprovenCause::Unknown && !hasFunction(loss.witness))
            loss.witness = { task.version.body.index(), origin.index(), function.locationOf(origin) };
        keepFirstLoss(sites.accessSiteLosses[site.index()], loss);
    }
};

struct FunctionValue {
    FunctionID function;
    ValueID value;
    uint32_t entry { 0 };

    friend auto operator<=>(const FunctionValue&, const FunctionValue&) = default;
};

} // anonymous namespace

FlowAccessSiteNumbering numberAccessSites(const Program& program, BuildContext& build)
{
    unsigned count = program.functions.size();
    FlowAccessSiteNumbering numbering;
    numbering.firstSiteOfFunction = Vector<uint32_t>(FillWith { }, count + 1, 0u);
    forEachIndexInParallel(build.parallelLoops(), count, [&](unsigned index) {
        uint32_t numberOfSites = 0;
        forEachAccessSiteValue(program.functions[index], [&](ValueID, uint32_t) {
            ++numberOfSites;
        });
        numbering.firstSiteOfFunction[index + 1] = numberOfSites;
    });
    for (unsigned index = 0; index < count; ++index)
        numbering.firstSiteOfFunction[index + 1] += numbering.firstSiteOfFunction[index];
    return numbering;
}

Vector<AccessSiteID> accessSitesByValue(const Function& function, const FlowAccessSiteNumbering& numbering, FunctionID functionAsRead)
{
    Vector<AccessSiteID> sites(function.values.size());
    forEachAccessSiteValue(function, [&](ValueID value, uint32_t ordinal) {
        sites[value.index()] = numbering.siteOf(functionAsRead, ordinal);
    });
    return sites;
}

static void resolveAccessSites(const Program& program, std::span<const FunctionValue> ascendingValues, NOESCAPE const Invocable<uint32_t(FunctionID)> auto& firstSiteOf, NOESCAPE const Invocable<void(const FunctionValue&, AccessSiteID)> auto& functor)
{
    while (!ascendingValues.empty()) {
        FunctionID function = ascendingValues.front().function;
        auto end = std::ranges::upper_bound(ascendingValues, function, { }, &FunctionValue::function);
        std::span<const FunctionValue> values = ascendingValues.first(static_cast<size_t>(end - ascendingValues.begin()));
        ascendingValues = ascendingValues.subspan(values.size());

        uint32_t firstSite = firstSiteOf(function);
        forEachAccessSiteValue(program.at(function), [&](ValueID value, uint32_t ordinal) {
            while (!values.empty() && values.front().value < value)
                values = values.subspan(1);
            for (; !values.empty() && values.front().value == value; values = values.subspan(1))
                functor(values.front(), AccessSiteID(firstSite + ordinal));
        });
    }
}

void fillAccessSites(const FlowTypeInputs& inputs, FunctionID functionAsRead, FlowSiteTables& sites)
{
    if (!inputs.accessSites.numberOfSitesOf(functionAsRead))
        return;

    const Function& function = inputs.families.program().at(functionAsRead);
    const FlowStatements& statements = inputs.families.graph().summaryOf(functionAsRead).statements;

    Vector<AccessKind> kinds(FillWith { }, function.values.size(), AccessKind::Unknown);
    auto setKind = [&](ValueID origin, AccessKind kind) {
        if (origin)
            kinds[origin.index()] = kind;
    };
    for (const FlowPropertyAccess& access : statements.propertyAccesses)
        setKind(access.origin, accessKindOf(access.kind));
    for (const FlowElementAccess& access : statements.elementAccesses)
        setKind(access.origin, accessKindOf(access.kind));
    for (const FlowFieldAccess& access : statements.fieldAccesses)
        setKind(access.origin, accessKindOf(access.kind));
    for (const FlowCall& call : statements.calls)
        setKind(call.origin, accessKindOf(call.mode));
    for (const FlowDirectCall& call : statements.directCalls) {
        if (call.origin)
            setKind(call.origin, function.at(call.origin).isConstruction() ? AccessKind::Construct : AccessKind::Call);
    }

    forEachAccessSiteValue(function, [&](ValueID value, uint32_t ordinal) {
        AccessSiteID site = inputs.accessSites.siteOf(functionAsRead, ordinal);
        sites.accessSites[site.index()] = AccessSite { functionAsRead, function.locationOf(value), kinds[value.index()] };
    });
}

static Witness findAllocationWithoutLayout(const StampContext& context, std::span<const FlowNodeID> receiverNodes)
{
    const FlowObjectTable& objects = context.families().objects();
    const FlowRecords& records = context.families().records();
    Witness witness = noWitness;
    for (FlowNodeID node : receiverNodes) {
        context.graph().valueOf(node).objects.forEach([&](FlowObjectID object) {
            if (object != unknownFlowObject && !objects.hasLayout(object))
                keepLowerWitness(witness, records.witnessOf(object, FlowObjectEvent::Allocated));
        });
    }
    return witness;
}

static FlowSiteLoss lossOfAccess(const StampContext& context, std::span<const FlowNodeID> receiverNodes, bool receiversAreComplete)
{
    if (context.isPastWorkLimit())
        return { .cause = UnprovenCause::LimitReached, .limit = LimitID::MaxFlowWorkPerFunction };
    if (!context.isEnabled(FlowResultKind::ReceiverSets))
        return { .cause = UnprovenCause::ResultDisabled };

    LayoutID lossLayout;
    FlowSiteLoss loss = context.converter().lossOfObjects(receiverNodes, lossLayout);
    if (loss.cause == UnprovenCause::Unknown && !receiversAreComplete)
        loss.cause = UnprovenCause::NonLayoutObject;
    if (loss.cause == UnprovenCause::NonLayoutObject && !hasFunction(loss.witness))
        loss.witness = findAllocationWithoutLayout(context, receiverNodes);
    return loss;
}

static void stampAccess(const StampContext& context, ValueID origin, FlowLocalID receiver)
{
    if (!origin || !PayloadWord::receivers(context.function.at(origin).opcode))
        return;
    Vector<FlowNodeID, 4> receiverNodes = context.nodesOf(receiver);
    if (receiverNodes.isEmpty())
        return;

    bool receiversAreComplete = false;
    if (context.isEnabled(FlowResultKind::ReceiverSets)) {
        LayoutSet receivers = context.converter().receiversOf(receiverNodes.span());
        receivers.isComplete = receivers.isComplete && context.valuesAreComplete();
        receiversAreComplete = receivers.isComplete;
        if (receiversAreComplete || !receivers.members.isEmpty())
            context.task.update.accesses.append(FlowAccessStamp { origin, context.task.types.intern(WTF::move(receivers)) });
    }
    context.recordLoss(origin, lossOfAccess(context, receiverNodes.span(), receiversAreComplete));
}

static void stampAccesses(const StampContext& context)
{
    const FlowStatements& statements = context.summary.statements;
    for (const FlowPropertyAccess& access : statements.propertyAccesses)
        stampAccess(context, access.origin, access.receiver);
    for (const FlowElementAccess& access : statements.elementAccesses)
        stampAccess(context, access.origin, access.receiver);
    for (const FlowFieldAccess& access : statements.fieldAccesses)
        stampAccess(context, access.origin, access.receiver);
}

static void collectReadsFromPrototype(const StampContext& context)
{
    const FlowFunctionConstruction* construction = context.families().solution.propertyState.constructions.functions[context.task.version.body.index()].get();
    if (!construction || !context.task.version.isReached())
        return;
    for (const FlowReadOfCreatedObject& read : construction->readsOfCreatedObject) {
        if (read.start == FlowLookupStart::Prototype)
            context.task.update.readsFromPrototype.append(read.read);
    }
}

static void collectBoundClosures(const StampContext& context)
{
    for (const FlowBoundClosureSite& site : context.task.inputs.boundFunctions.sitesIn(context.task.version.body))
        context.task.update.boundClosures.append({ site.call, site.boundFunction });
}

static void collectDefinedValues(const StampContext& context)
{
    for (const FlowReplacedDefinition& replaced : context.task.inputs.fieldDefinitions.replacedIn(context.task.version.body))
        context.task.update.definedValues.append({ replaced.definition, replaced.value });
}

static std::optional<FlowIndirectCallKind> indirectCallKindOf(FlowCallLinkKind kind)
{
    switch (kind) {
    case FlowCallLinkKind::Call:
    case FlowCallLinkKind::Construct:
        return std::nullopt;
    case FlowCallLinkKind::Accessor:
        return FlowIndirectCallKind::Accessor;
    case FlowCallLinkKind::BuiltinCallback:
        return FlowIndirectCallKind::BuiltinCallback;
    case FlowCallLinkKind::Protocol:
        return FlowIndirectCallKind::Protocol;
    case FlowCallLinkKind::Host:
        return FlowIndirectCallKind::Host;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool hasUnknownObject(const FlowGraph& graph, std::span<const FlowNodeID> nodes)
{
    return std::ranges::any_of(nodes, [&](FlowNodeID node) {
        return graph.valueOf(node).hasUnknownObject();
    });
}

// A site with one link into the generic instance enters the generic version for every closure of that function.
static void removeFunctionsEnteredGenerically(const FlowVersionPlan& versions, Vector<FunctionID>& ascendingFunctions)
{
    bool entersGenericVersion = std::ranges::any_of(ascendingFunctions, [&](FunctionID function) {
        return versions.isGenericVersion(function);
    });
    if (!entersGenericVersion)
        return;

    Vector<FunctionID> enteredVersions;
    enteredVersions.reserveInitialCapacity(ascendingFunctions.size());
    for (FunctionID function : ascendingFunctions) {
        FunctionID genericVersion = versions.genericVersionOf(function);
        if (!genericVersion || !std::ranges::binary_search(ascendingFunctions, genericVersion))
            enteredVersions.append(function);
    }
    ascendingFunctions = WTF::move(enteredVersions);
}

static bool hasBoundFunction(const FlowGraph& graph, std::span<const FlowNodeID> nodes)
{
    return std::ranges::any_of(nodes, [&](FlowNodeID node) {
        return graph.valueOf(node).objects.anyOf([&](FlowObjectID object) {
            return graph.objects().isInstanceOf(object, BuiltinClasses::BoundFunction);
        });
    });
}

// What reaches an operand of a function past the work limit goes to unknown JavaScript and not on to the callee, so the proven version has not seen what such a call passes.
static FunctionID versionEnteredThrough(const StampContext& context, const FlowCallLink& link)
{
    FunctionID version = versionRunning(context.graph(), context.versions(), link.calleeInstance);
    if (!context.isPastWorkLimit() || context.versions().isGenericVersion(version))
        return version;
    FunctionID genericVersion = context.versions().genericVersionOf(version);
    return genericVersion ? genericVersion : version;
}

// A built-in is linked where an overload is selected, and that waits for the operands. A call with an operand that nothing flows into has no link,
// and the built-in is its callee all the same: the passes find it by its type.
static void appendBuiltinsAmong(const StampContext& context, std::span<const FlowNodeID> calleeNodes, CalleeSet& callees)
{
    const FlowObjectTable& objects = context.families().objects();
    const BuiltinTable& builtins = context.families().solution.builtins();
    for (FlowNodeID node : calleeNodes) {
        context.graph().valueOf(node).objects.forEach([&](FlowObjectID object) {
            FlowObjectKind kind = objects.kindOf(object);
            if ((kind == FlowObjectKind::Builtin || kind == FlowObjectKind::ModuleBuiltin) && objects.isCallable(object) && isObjectKind(builtins.at(objects.at(object).builtin).kind))
                callees.builtins.append(builtins.canonicalID(objects.at(object).builtin));
        });
    }
}

static CalleeSet calleesOf(const StampContext& context, const FlowCall& call, std::span<const FlowNodeID> calleeNodes)
{
    const FlowFamilyInputs& families = context.families();
    const BuiltinTable& builtins = families.solution.builtins();

    CalleeSet callees;
    callees.isComplete = context.valuesAreComplete() && !calleeNodes.empty() && !hasUnknownObject(context.graph(), calleeNodes);
    for (FlowInstanceID instance : context.instances()) {
        for (FlowCallLinkID id : families.calls.linksAt(instance, call.origin)) {
            const FlowCallLink& link = families.records().at(id);
            if (!indirectCallKindOf(link.kind))
                callees.functions.append(versionEnteredThrough(context, link));
        }
        for (const FlowBuiltinCallLink& link : families.calls.builtinLinksAt(instance, call.origin)) {
            if (indirectCallKindOf(link.kind))
                continue;
            // What stands for all that a native binding answers is no object of the realm: the call stays a call of a value nobody knows.
            if (isObjectKind(builtins.at(link.builtin).kind))
                callees.builtins.append(builtins.canonicalID(link.builtin));
            else
                callees.isComplete = false;
        }
        if (families.calls.hasUnknownCalleeAt(instance, call.origin))
            callees.isComplete = false;
    }
    appendBuiltinsAmong(context, calleeNodes, callees);
    canonicalize(callees);
    removeFunctionsEnteredGenerically(context.versions(), callees.functions);
    return callees;
}

static FlowSiteLoss lossOfCall(const StampContext& context, std::span<const FlowNodeID> calleeNodes, bool calleesAreComplete)
{
    if (!context.isEnabled(FlowResultKind::CalleeSets))
        return { .cause = UnprovenCause::ResultDisabled };
    if (calleesAreComplete)
        return { };
    if (context.isPastWorkLimit())
        return { .cause = UnprovenCause::LimitReached, .limit = LimitID::MaxFlowWorkPerFunction };

    FlowSiteLoss loss { .cause = UnprovenCause::UnknownCallee };
    for (FlowNodeID node : calleeNodes) {
        if (context.graph().valueOf(node).hasUnknownObject())
            keepLowerWitness(loss.witness, context.families().records().unknownObjectWitness(node));
    }
    return loss;
}

static FlowShapeState shapeStateOf(const StampContext& context, std::span<const FlowNodeID> nodes, NOESCAPE const Invocable<bool(LayoutID)> auto& admitsLayout)
{
    const FlowObjectTable& objects = context.families().objects();
    bool hasObject = false;
    for (FlowNodeID node : nodes) {
        const FlowValue& value = context.graph().valueOf(node);
        bool everyValueKeepsShape = value.hasOnlyObjects() && value.objects.allOf([&](FlowObjectID object) {
            return objects.hasLayout(object)
                && admitsLayout(objects.allocatedLayoutOf(object))
                && context.converter().shapeOf(object).state == FlowShapeState::KeepsShape;
        });
        if (!everyValueKeepsShape)
            return FlowShapeState::Unknown;
        hasObject |= value.mayBeObject();
    }
    return hasObject ? FlowShapeState::KeepsShape : FlowShapeState::Unknown;
}

static FlowShapeState shapeStateOfResult(const StampContext& context, const FlowCall& call, Opcode opcode)
{
    if (!FlagMask::keepsShape(opcode) || !call.derivedLayout)
        return FlowShapeState::Unknown;

    std::span<const LayoutID> layoutsBelow = context.families().hierarchy.layoutsBelow(call.derivedLayout);
    Vector<FlowNodeID, 4> resultNodes = context.nodesOf(call.result);
    return shapeStateOf(context, resultNodes.span(), [&](LayoutID layout) {
        return std::ranges::binary_search(layoutsBelow, layout);
    });
}

// The empty complete set says that nothing comes out of the callee or that it is never a function. The first is dead code, which is cut where the narrowed
// types are applied. Where they are not, the call stays, and a pass that follows the prediction of its callee would contradict a set that says "does nothing".
static bool isWorthStamping(const StampContext& context, const CalleeSet& callees)
{
    if (!callees.functions.isEmpty() || !callees.builtins.isEmpty())
        return true;
    return callees.isComplete && context.isEnabled(FlowResultKind::NarrowedTypes);
}

static void stampCalls(const StampContext& context)
{
    for (const FlowCall& call : context.summary.statements.calls) {
        if (!call.origin)
            continue;
        Opcode opcode = context.function.at(call.origin).opcode;
        if (!PayloadWord::callees(opcode))
            continue;

        Vector<FlowNodeID, 4> calleeNodes = context.nodesOf(call.callee);
        CalleeSet callees = calleesOf(context, call, calleeNodes.span());
        context.recordLoss(call.origin, lossOfCall(context, calleeNodes.span(), callees.isComplete));
        if (hasBoundFunction(context.graph(), calleeNodes.span()))
            context.recordCallOfBoundFunction(call.origin);

        if (!context.isEnabled(FlowResultKind::CalleeSets))
            continue;
        if (!isWorthStamping(context, callees))
            continue;
        bool keepsShape = shapeStateOfResult(context, call, opcode) == FlowShapeState::KeepsShape;
        context.task.update.calls.append(FlowCallStamp { call.origin, context.task.types.intern(WTF::move(callees)), keepsShape });
    }
}

static FunctionID versionEnteredBy(const StampContext& context, const FlowDirectCall& call)
{
    FunctionID genericVersion = context.versions().genericVersionOf(call.function);
    if (!genericVersion && context.versions().callSite.versions.isEmpty())
        return call.function;

    const FlowFamilyInputs& families = context.families();
    for (FlowInstanceID instance : context.instances()) {
        for (FlowCallLinkID id : families.calls.linksAt(instance, call.origin)) {
            FunctionID version = versionEnteredThrough(context, families.records().at(id));
            if (version == genericVersion || context.versions().isCallSiteVersion(version))
                return version;
        }
    }
    return call.function;
}

static TypeInclusion writtenTypeInclusionOf(const StampContext& context, const FlowDirectCall& call)
{
    // The values of an abandoned function do not bound what the call returns.
    if (!context.isEnabled(FlowResultKind::NarrowedTypes) || context.isPastWorkLimit())
        return TypeInclusion::Unknown;

    const Program& program = context.families().program();
    const Function& callee = program.at(call.function);
    std::span<const FlowLocalID> results = context.summary.statements.list(call.results);
    if (callee.results.isEmpty() || results.empty())
        return TypeInclusion::Unknown;
    Vector<FlowNodeID, 4> resultNodes = context.nodesOf(results.front());
    if (resultNodes.isEmpty())
        return TypeInclusion::Unknown;

    TypingContext written { program, context.function, context.task.types, TypeLevel::Written };
    return context.converter().isInside(resultNodes.span(), written.typeOf(callee.results[0]).withoutProofs(), context.task.types);
}

static void stampDirectCalls(const StampContext& context)
{
    const Vector<ResultCheck>& resultChecksByVersion = context.task.inputs.tailCalls.resultChecksByVersion;
    for (const FlowDirectCall& call : context.summary.statements.directCalls) {
        if (!call.origin)
            continue;
        FunctionID enteredVersion = versionEnteredBy(context, call);
        ResultCheck resultCheck = resultChecksByVersion[enteredVersion.index()];
        if (enteredVersion == call.function && resultCheck == ResultCheck::ByCallee)
            continue;

        bool resultIsInsideWrittenType = resultCheck == ResultCheck::ByCallers && writtenTypeInclusionOf(context, call) == TypeInclusion::Inside;
        context.task.update.directCalls.append(FlowDirectCallUpdate { call.origin, enteredVersion, resultCheck, resultIsInsideWrittenType });
    }
}

static AllocationClassID allocationClassOf(const FlowObjectTable& objects, AllocationSiteID site)
{
    FlowObjectID sharedObject = objects.at(site).sharedObject;
    return sharedObject ? objects.at(sharedObject).allocationClass : AllocationClassID();
}

static FlowShapeState shapeStateOfAllocation(const StampContext& context, const FlowSiteDescription& description, Opcode opcode, LayoutID allocatedLayout)
{
    if (!FlagMask::keepsShape(opcode) || !allocatedLayout)
        return FlowShapeState::Unknown;

    Vector<FlowNodeID, 4> nodes = context.nodesOf(description.value);
    return shapeStateOf(context, nodes.span(), [&](LayoutID layout) {
        return layout == allocatedLayout;
    });
}

static LayoutID layoutAllocatedBy(const FlowObjectTable& objects, AllocationSiteID site, AllocationClassID allocationClass)
{
    LayoutID layout = objects.at(site).description.layout;
    return layout && allocationClass ? objects.at(allocationClass).layout : layout;
}

// The objects that the instances of one version allocate at a site are in one allocation class.
static AllocationClassID allocationClassOf(const StampContext& context, AllocationSiteID site, uint32_t siteInSummary)
{
    const FlowObjectTable& objects = context.families().objects();
    AllocationClassID allocationClass;
    for (FlowInstanceID instance : context.instances()) {
        if (context.graph().at(instance).state == FlowInstanceState::Created)
            continue;
        FlowObjectID object = context.graph().objectOf(instance, siteInSummary);
        if (!object || object == unknownFlowObject || !objects.at(object).allocationClass)
            return allocationClassOf(objects, site);
        RELEASE_ASSERT(!allocationClass || allocationClass == objects.at(object).allocationClass);
        allocationClass = objects.at(object).allocationClass;
    }
    return allocationClass ? allocationClass : allocationClassOf(objects, site);
}

static void stampAllocations(const StampContext& context)
{
    const FlowObjectTable& objects = context.families().objects();
    const Vector<FlowSiteDescription>& descriptions = context.summary.sites;
    for (uint32_t index = 0; index < descriptions.size(); ++index) {
        const FlowSiteDescription& description = descriptions[index];
        if (!description.value)
            continue;
        if (description.origin == FlowSiteOrigin::DataNode && description.ordinal)
            continue;
        Opcode opcode = context.function.at(description.value).opcode;
        if (!PayloadWord::allocationSite(opcode))
            continue;

        AllocationSiteID site = objects.siteOf(context.task.version.body, index);
        AllocationClassID allocationClass = allocationClassOf(context, site, index);
        LayoutID allocatedLayout = layoutAllocatedBy(objects, site, allocationClass);
        bool keepsShape = shapeStateOfAllocation(context, description, opcode, allocatedLayout) == FlowShapeState::KeepsShape;
        bool createsDictionary = allocationClass && context.families().hierarchy.createsDictionaries(allocationClass);
        TypeID elements = PayloadWord::contents(opcode) ? context.converter().elementsAllocatedAt(site) : TypeID();
        LayoutID derivedLayout = allocatedLayout != description.layout ? allocatedLayout : LayoutID();
        ValueID fillingCopy = createsDictionary ? findCopyFillingLiteral(context.function, description.value) : ValueID();
        context.task.update.allocations.append(FlowAllocationStamp { description.value, site, allocationClass, derivedLayout, keepsShape, createsDictionary, elements, fillingCopy });
    }
}

// The object of a constructor function is allocated where it is constructed, and the operation that constructs it names no layout.
static LayoutID derivedLayoutConstructedAt(const StampContext& context, ValueID construct)
{
    const FlowFamilyInputs& families = context.families();
    LayoutID derivedLayout;
    bool allocatesOtherLayout = false;
    for (FlowInstanceID instance : context.task.version.instances) {
        for (FlowCallLinkID link : families.calls.linksAt(instance, construct)) {
            LayoutID constructed = families.program().at(families.records().at(link).callee).constructedClass;
            if (!constructed)
                continue;
            FlowSiteDescription description { .kind = FlowSiteKind::Layout, .origin = FlowSiteOrigin::Operation, .value = construct, .layout = constructed };
            FlowObjectID object = findCallSiteAllocation(families.solution, instance, description);
            if (!object || object == unknownFlowObject)
                continue;
            LayoutID allocated = families.objects().allocatedLayoutOf(object);
            if (allocated == constructed || (derivedLayout && derivedLayout != allocated))
                allocatesOtherLayout = true;
            else
                derivedLayout = allocated;
        }
    }
    RELEASE_ASSERT(!derivedLayout || !allocatesOtherLayout);
    return derivedLayout;
}

static void stampConstructions(const StampContext& context)
{
    for (const FlowCall& call : context.summary.statements.calls) {
        if (!call.origin || context.function.at(call.origin).opcode != Opcode::Construct)
            continue;
        if (LayoutID derivedLayout = derivedLayoutConstructedAt(context, call.origin))
            context.task.update.constructions.append(FlowConstructionStamp { call.origin, derivedLayout });
    }
}

void computeStamps(const FlowBodyTask& task, FlowSiteTables& sites)
{
    const FlowFamilyInputs& families = task.inputs.families;
    FunctionID body = task.version.body;
    const Function& function = families.program().at(body);
    StampContext context { task, sites, function, families.graph().summaryOf(body), accessSitesByValue(function, task.inputs.accessSites, body) };

    stampAccesses(context);
    collectReadsFromPrototype(context);
    collectBoundClosures(context);
    collectDefinedValues(context);
    stampCalls(context);
    stampDirectCalls(context);
    stampAllocations(context);
    stampConstructions(context);

    FlowBodyUpdate& update = task.update;
    std::ranges::sort(update.accesses, { }, &FlowAccessStamp::value);
    std::ranges::sort(update.calls, { }, &FlowCallStamp::value);
    std::ranges::sort(update.directCalls, { }, &FlowDirectCallUpdate::call);
    std::ranges::sort(update.allocations, { }, &FlowAllocationStamp::value);
}

static bool isFunctionOfProgram(const FlowGraph& graph, FunctionID function)
{
    return function && !graph.isHost(function);
}

static AllocationSite createAllocationSite(const FlowFamilyInputs& inputs, AllocationSiteID id)
{
    const FlowSite& site = inputs.objects().at(id);
    const FlowSiteDescription& description = site.description;

    AllocationSite record;
    record.allocationClass = allocationClassOf(inputs.objects(), id);
    record.layout = layoutAllocatedBy(inputs.objects(), id, record.allocationClass);
    record.builtinClass = description.builtinClass;
    if (!isFunctionOfProgram(inputs.graph(), site.function))
        return record;

    const Function& function = inputs.program().at(site.function);
    record.function = site.function;
    record.location = description.value ? function.locationOf(description.value) : function.declarationLocation();
    return record;
}

static void fillResultSites(const FlowFamilyInputs& inputs, FlowSiteTables& sites)
{
    const FlowObjectTable& objects = inputs.objects();
    Vector<FunctionValue> calls;
    for (uint32_t index = 0; index < objects.numberOfSites(); ++index) {
        const FlowSite& site = objects.at(AllocationSiteID(index));
        if (site.description.origin != FlowSiteOrigin::BuiltinResult || !site.description.value)
            continue;
        if (isFunctionOfProgram(inputs.graph(), site.function))
            calls.append(FunctionValue { site.function, site.description.value, index });
    }
    parallelSort(inputs.loops(), calls.mutableSpan(), std::less<FunctionValue> { });

    auto firstSiteOf = [&](FunctionID function) {
        auto firstSite = std::ranges::lower_bound(sites.accessSites, function, { }, &AccessSite::function);
        return static_cast<uint32_t>(firstSite - sites.accessSites.begin());
    };
    AccessSiteID previousCall;
    resolveAccessSites(inputs.program(), calls.span(), firstSiteOf, [&](const FunctionValue& call, AccessSiteID accessSite) {
        bool hasOtherResultSite = accessSite == previousCall;
        sites.resultSites[accessSite.index()] = hasOtherResultSite ? AllocationSiteID() : AllocationSiteID(call.entry);
        previousCall = accessSite;
    });
}

void fillAllocationSites(const FlowFamilyInputs& inputs, FlowSiteTables& sites)
{
    unsigned count = inputs.objects().numberOfSites();
    sites.allocationSites = Vector<AllocationSite>(count);
    forEachIndexInParallel(inputs.loops(), count, [&](unsigned index) {
        sites.allocationSites[index] = createAllocationSite(inputs, AllocationSiteID(index));
    });
    fillResultSites(inputs, sites);
}

static void mergeIndirectCalls(const ParallelLoops& loops, Vector<FlowIndirectCall>& calls)
{
    auto keyOf = [](const FlowIndirectCall& call) {
        return std::tuple(call.caller, call.site, call.callee, call.kind);
    };
    parallelSort(loops, calls.mutableSpan(), [&](const FlowIndirectCall& a, const FlowIndirectCall& b) {
        return keyOf(a) < keyOf(b);
    });

    size_t count = 0;
    for (FlowIndirectCall call : calls) {
        if (!count || keyOf(calls[count - 1]) != keyOf(call)) {
            calls[count++] = call;
            continue;
        }
        FlowIndirectCall& merged = calls[count - 1];
        merged.isInCycle |= call.isInCycle;
        if (merged.times != call.times) {
            bool isCalledPerEvent = merged.times == BuiltinCallbackTimes::PerEvent || call.times == BuiltinCallbackTimes::PerEvent;
            merged.times = isCalledPerEvent ? BuiltinCallbackTimes::PerEvent : BuiltinCallbackTimes::Repeatedly;
        }
    }
    calls.shrink(count);
}

Vector<FlowIndirectCall> computeIndirectCalls(const FlowFamilyInputs& inputs, const FlowVersionPlan& versions, const FlowAccessSiteNumbering& numbering)
{
    const FlowGraph& graph = inputs.graph();
    Vector<FlowIndirectCall> calls;
    Vector<FunctionValue> callSiteValues;
    for (const FlowCallLink& link : inputs.records().callLinks()) {
        std::optional<FlowIndirectCallKind> kind = indirectCallKindOf(link.kind);
        if (!kind)
            continue;
        FunctionID caller = isFunctionOfProgram(graph, link.caller) ? link.caller : FunctionID();
        if (caller && link.site)
            callSiteValues.append(FunctionValue { caller, link.site, static_cast<uint32_t>(calls.size()) });
        calls.append(FlowIndirectCall { caller, { }, versionRunning(graph, versions, link.calleeInstance), *kind, link.times, link.isInCycle });
    }
    parallelSort(inputs.loops(), callSiteValues.mutableSpan(), std::less<FunctionValue> { });

    auto firstSiteOf = [&](FunctionID function) {
        return numbering.firstSiteOfFunction[function.index()];
    };
    resolveAccessSites(inputs.program(), callSiteValues.span(), firstSiteOf, [&](const FunctionValue& value, AccessSiteID site) {
        calls[value.entry].site = site;
    });

    mergeIndirectCalls(inputs.loops(), calls);
    return calls;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
