/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowResultFamilies.h"

#if ENABLE(AOT)

#include "AOTForwardedCalls.h"
#include "AOTResolveCallsCallees.h"
#include "AOTStronglyConnectedComponents.h"
#include <algorithm>
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct TailCallSite {
    ValueID call;
    Vector<FunctionID, 2> callees;
    bool hasUnknownCallee { false };
    bool hasClaim { false };
    bool claimsAreRedundant { false };
};

struct TailCaller {
    uint32_t version;
    uint32_t siteIndex;
};

struct TailCallInputs {
    const FlowFamilyInputs& families;
    const FlowTypeConverter& converter;
    const FlowVersionPlan& versions;
};

using TailCallSites = Vector<Vector<TailCallSite>>;
using TailCallSuccessors = Vector<uint32_t, 2>;

} // anonymous namespace

static bool isCalleeLink(FlowCallLinkKind kind)
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

// f.call(t, a), f.apply(t, list) and Reflect.apply(f, t, list) in tail position call f in tail position.
static bool forwardsCallAt(const FlowFamilyInputs& families, FlowInstanceID instance, ValueID call)
{
    const BuiltinTable& builtins = families.solution.builtins();
    return std::ranges::any_of(families.calls.builtinLinksAt(instance, call), [&](const FlowBuiltinCallLink& link) {
        return link.overload && forwardsCall(builtins.at(link.overload));
    });
}

static bool forwardsToUnknownCallee(const FlowFamilyInputs& families, FlowInstanceID instance, ValueID call)
{
    return std::ranges::binary_search(families.records().unknownCallees(), FlowUnknownCallee { instance, call, FlowCallLinkKind::BuiltinCallback });
}

static void collectCallees(const TailCallInputs& inputs, const FlowFunctionVersion& version, TailCallSite& site)
{
    const FlowFamilyInputs& families = inputs.families;
    for (FlowInstanceID instance : version.instances) {
        bool isForwardingSite = forwardsCallAt(families, instance, site.call);
        for (FlowCallLinkID id : families.calls.linksAt(instance, site.call)) {
            const FlowCallLink& link = families.records().at(id);
            bool isForwardedCall = isForwardingSite && link.kind == FlowCallLinkKind::BuiltinCallback;
            if (!isCalleeLink(link.kind) && !isForwardedCall)
                continue;
            forEachVersionRunning(families.graph(), inputs.versions, link.calleeInstance, [&](FunctionID callee) {
                site.callees.append(callee);
            });
        }
        if (families.calls.hasUnknownCalleeAt(instance, site.call) || (isForwardingSite && forwardsToUnknownCallee(families, instance, site.call)))
            site.hasUnknownCallee = true;
    }
    std::ranges::sort(site.callees);
    removeRepeatedElements(site.callees);
}

static bool canProveTypes(const FlowFamilyInputs& families, FunctionID body)
{
    return families.isEnabled(FlowResultKind::NarrowedTypes, body.index()) && !families.solution.isPastWorkLimit(body);
}

static TypeInclusion inclusionOf(const TailCallInputs& inputs, std::span<const FlowNodeID> nodes, TypeID target, TypeArena& types)
{
    if (nodes.empty())
        return TypeInclusion::Unknown;
    return inputs.converter.isInside(nodes, inputs.families.program().at(target), types);
}

static bool hasClaim(const Terminator& terminator)
{
    for (TailCast cast : terminator.casts()) {
        if (cast.resolution == CastResolution::Claim)
            return true;
    }
    return false;
}

static bool areClaimsRedundant(const TailCallInputs& inputs, const FlowFunctionVersion& version, const Terminator& terminator, TypeArena& types)
{
    Vector<FlowNodeID, 4> nodes = nodesOf(inputs.families.graph(), version, terminator.callOperand());
    for (TailCast cast : terminator.casts()) {
        if (cast.resolution != CastResolution::Claim)
            continue;
        if (inclusionOf(inputs, nodes.span(), cast.declared.type, types) != TypeInclusion::Inside)
            return false;
    }
    return true;
}

static Vector<TailCallSite> collectSitesOf(const TailCallInputs& inputs, const FlowFunctionVersion& version, TypeInterningPhase& scratchTypes)
{
    const Function& function = inputs.families.program().at(version.body);
    bool canProveClaims = canProveTypes(inputs.families, version.body);

    Vector<TailCallSite> sites;
    std::optional<TypeArena> types;
    for (const Block& block : function.blocks) {
        const Terminator& terminator = block.terminator;
        if (terminator.kind != TerminatorKind::ReturnCall)
            continue;

        TailCallSite site;
        site.call = terminator.callOperand();
        collectCallees(inputs, version, site);
        site.hasClaim = hasClaim(terminator);
        if (site.hasClaim && canProveClaims) {
            if (!types)
                types.emplace(scratchTypes, TypeInterningPhase::Scratch);
            site.claimsAreRedundant = areClaimsRedundant(inputs, version, terminator, *types);
        }
        sites.append(WTF::move(site));
    }
    std::ranges::sort(sites, { }, &TailCallSite::call);
    return sites;
}

static TailCallSites collectSites(const TailCallInputs& inputs, TypeTable& types)
{
    const FlowFamilyInputs& families = inputs.families;
    unsigned count = inputs.versions.functionCount();
    TailCallSites sites(count);
    TypeInterningPhase scratchTypes(types, 0u, TypeInterningPhase::Scratch);
    forEachIndexInParallel(families.loops(), count, [&](unsigned index) {
        FlowFunctionVersion version = findFunctionVersion(families.graph(), inputs.versions, FunctionID(index));
        if (version.isReached())
            sites[index] = collectSitesOf(inputs, version, scratchTypes);
    });
    scratchTypes.discard();
    return sites;
}

static bool mayBeEnteredByUnseenCode(const TailCallInputs& inputs, const BitVector& hostCalled, FunctionID version)
{
    const FlowFamilyInputs& families = inputs.families;
    if (inputs.versions.isCallSiteVersion(version) || (!inputs.versions.isGenericVersion(version) && inputs.versions.genericVersionOf(version)))
        return false;

    FunctionID body = inputs.versions.bodyOf(version);
    if (hostCalled.get(body.index()))
        return true;
    bool isLinkedToHost = std::ranges::any_of(families.calls.linksTo(body), [&](FlowCallLinkID link) {
        return families.records().at(link).kind == FlowCallLinkKind::Host;
    });
    if (isLinkedToHost)
        return true;
    return std::ranges::any_of(families.objects().closuresOf(body), [&](FlowObjectID closure) {
        return families.factsOf(closure).mutations.containsAny(reachMutations);
    });
}

static Vector<TailCallSuccessors> computeSuccessors(const TailCallInputs& inputs, const TailCallSites& sites)
{
    const FlowFamilyInputs& families = inputs.families;
    unsigned count = sites.size();
    unsigned unknownCalleeNode = count;
    Vector<TailCallSuccessors> successors(count + 1);
    forEachIndexInParallel(families.loops(), count, [&](unsigned caller) {
        for (const TailCallSite& site : sites[caller]) {
            for (FunctionID callee : site.callees)
                successors[caller].append(callee.index());
            if (site.hasUnknownCallee)
                successors[caller].append(unknownCalleeNode);
        }
    });
    BitVector hostCalled = functionsCalledByHost(families.program());
    successors[unknownCalleeNode].appendVector(findIndicesInParallel(families.loops(), count, [&](unsigned version) {
        return !sites[version].isEmpty() && mayBeEnteredByUnseenCode(inputs, hostCalled, FunctionID(version));
    }));
    return successors;
}

static TailCallRequirement requirementOf(const StronglyConnectedComponents& components, unsigned unknownCalleeNode, unsigned caller, const TailCallSite& site)
{
    if (!components.isInCycle.get(caller))
        return TailCallRequirement::Allowed;

    uint32_t cycle = components.componentOf[caller];
    bool isOnCycle = site.hasUnknownCallee && components.componentOf[unknownCalleeNode] == cycle;
    isOnCycle |= site.callees.containsIf([&](FunctionID callee) {
        return components.componentOf[callee.index()] == cycle;
    });
    return isOnCycle ? TailCallRequirement::Required : TailCallRequirement::Allowed;
}

static bool canBeCheckedByCallers(const TailCallInputs& inputs, FunctionID version)
{
    const FlowFamilyInputs& families = inputs.families;
    FunctionID body = inputs.versions.bodyOf(version);
    return families.isEnabled(FlowResultKind::ResultChecks, body.index())
        && families.program().at(body).mayChangeSignature()
        && !families.solution.isPastWorkLimit(body);
}

static TypeInclusion inclusionInResultType(const TailCallInputs& inputs, FunctionID id, ValueID call, TypeArena& types)
{
    const FlowFamilyInputs& families = inputs.families;
    FunctionID body = inputs.versions.bodyOf(id);
    TypeID resultType = families.program().at(body).result().type;
    if (families.program().at(resultType).isAnything())
        return TypeInclusion::Inside;
    if (!canProveTypes(families, body))
        return TypeInclusion::Unknown;
    FlowFunctionVersion version = findFunctionVersion(families.graph(), inputs.versions, id);
    Vector<FlowNodeID, 4> nodes = nodesOf(families.graph(), version, call);
    return inclusionOf(inputs, nodes.span(), resultType, types);
}

static Vector<Vector<TailCaller>> collectRequiredCallers(const TailCallSites& sites, const FlowTailCallResults& results)
{
    Vector<Vector<TailCaller>> requiredCallersOf(sites.size());
    for (uint32_t caller = 0; caller < sites.size(); ++caller) {
        for (uint32_t siteIndex = 0; siteIndex < sites[caller].size(); ++siteIndex) {
            if (results.sitesByVersion[caller][siteIndex].requirement != TailCallRequirement::Required)
                continue;
            for (FunctionID callee : sites[caller][siteIndex].callees)
                requiredCallersOf[callee.index()].append(TailCaller { caller, siteIndex });
        }
    }
    return requiredCallersOf;
}

static void propagateResultChecks(const TailCallInputs& inputs, const TailCallSites& sites, TypeTable& types, FlowTailCallResults& results)
{
    Vector<uint32_t> worklist;
    for (uint32_t version = 0; version < sites.size(); ++version) {
        if (results.resultChecksByVersion[version] == ResultCheck::ByCallers)
            worklist.append(version);
    }
    if (worklist.isEmpty())
        return;

    Vector<Vector<TailCaller>> requiredCallersOf = collectRequiredCallers(sites, results);
    ScratchTypeArena scratchTypes(types);
    while (!worklist.isEmpty()) {
        uint32_t callee = worklist.takeLast();
        for (TailCaller caller : requiredCallersOf[callee]) {
            ResultCheck& check = results.resultChecksByVersion[caller.version];
            FlowTailCallUpdate& update = results.sitesByVersion[caller.version][caller.siteIndex];
            if (check == ResultCheck::ByCallers || update.requirement != TailCallRequirement::Required)
                continue;
            if (inclusionInResultType(inputs, FunctionID(caller.version), update.call, scratchTypes.arena()) == TypeInclusion::Inside)
                continue;
            if (canBeCheckedByCallers(inputs, FunctionID(caller.version))) {
                check = ResultCheck::ByCallers;
                worklist.append(caller.version);
                continue;
            }
            // The application demotes an allowed tail call of such a callee and claims the result after the call.
            update.requirement = TailCallRequirement::Allowed;
        }
    }
}

FlowTailCallResults computeTailCallResults(const FlowFamilyInputs& families, const FlowTypeConverter& converter, const FlowVersionPlan& versions, TypeTable& types)
{
    TailCallInputs inputs { families, converter, versions };
    unsigned count = versions.functionCount();
    unsigned unknownCalleeNode = count;

    TailCallSites sites = collectSites(inputs, types);
    Vector<TailCallSuccessors> successors = computeSuccessors(inputs, sites);
    StronglyConnectedComponents components = computeStronglyConnectedComponents(count + 1, [&](unsigned node) -> const TailCallSuccessors& {
        return successors[node];
    });

    FlowTailCallResults results;
    results.sitesByVersion.grow(count);
    results.resultChecksByVersion.fill(ResultCheck::ByCallee, count);
    forEachIndexInParallel(families.loops(), count, [&](unsigned index) {
        if (sites[index].isEmpty())
            return;

        FunctionID version(index);
        bool isEnabled = families.isEnabled(FlowResultKind::ResultChecks, versions.bodyOf(version).index());
        bool callersCanCheck = canBeCheckedByCallers(inputs, version);
        Vector<FlowTailCallUpdate>& updates = results.sitesByVersion[index];
        updates.reserveInitialCapacity(sites[index].size());
        for (const TailCallSite& site : sites[index]) {
            TailCallRequirement requirement = isEnabled ? requirementOf(components, unknownCalleeNode, index, site) : TailCallRequirement::Required;
            updates.append(FlowTailCallUpdate { site.call, requirement, site.claimsAreRedundant });
            if (callersCanCheck && requirement == TailCallRequirement::Required && site.hasClaim && !site.claimsAreRedundant)
                results.resultChecksByVersion[index] = ResultCheck::ByCallers;
        }
    });
    propagateResultChecks(inputs, sites, types, results);
    return results;
}

ResultCheck resultCheckOf(const FlowResults& flow, FunctionID function)
{
    return flow.function(function).resultCheck;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
