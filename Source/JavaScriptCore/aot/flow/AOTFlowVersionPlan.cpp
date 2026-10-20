/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowVersionPlan.h"

#if ENABLE(AOT)

#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

FunctionID FlowVersionPlan::genericVersionOf(FunctionID provenVersion) const
{
    auto version = std::ranges::lower_bound(provenVersions, provenVersion);
    if (version == provenVersions.end() || *version != provenVersion)
        return { };
    return FunctionID(functionCountAsRead + static_cast<uint32_t>(version - provenVersions.begin()));
}

FunctionID FlowVersionPlan::provenVersionOf(FunctionID genericVersion) const
{
    RELEASE_ASSERT(isGenericVersion(genericVersion));
    return provenVersions[genericVersion.index() - functionCountAsRead];
}

FunctionID FlowVersionPlan::bodyOf(FunctionID version) const
{
    if (isCallSiteVersion(version))
        return callSiteVersion(version).body;
    return isGenericVersion(version) ? provenVersionOf(version) : version;
}

// A pass may send any call into a generic version: a call by value, a call without a stamp, a bound function, a direct call whose arguments have wider types in the caller
// than the proven parameters. So a generic version stands for every instance of the function, like a function that has one version.
// The instances of a call-site version are the exception: no pass sends their calls anywhere else.
FlowFunctionVersion findFunctionVersion(const FlowGraph& graph, const FlowVersionPlan& plan, FunctionID id)
{
    FlowFunctionVersion version;
    version.id = id;
    version.body = plan.bodyOf(id);
    if (plan.isCallSiteVersion(id)) {
        version.instances.append(plan.callSiteVersion(id).instances.span());
        return version;
    }

    bool takesOnlyTrackedOperands = !!plan.genericVersionOf(id);
    graph.forEachInstanceOf(version.body, [&](FlowInstanceID instance) {
        if (plan.callSite.versionOf(instance))
            return;
        if (!takesOnlyTrackedOperands || hasTrackedOperands(graph.at(instance).context))
            version.instances.append(instance);
    });
    return version;
}

FunctionID versionRunning(const FlowGraph& graph, const FlowVersionPlan& plan, FlowInstanceID id)
{
    const FlowInstance& instance = graph.at(id);
    if (graph.isHost(instance.function))
        return graph.hostFunction();
    if (std::optional<uint32_t> version = plan.callSite.versionOf(id))
        return FunctionID(plan.firstCallSiteVersion() + *version);
    if (instance.context == FlowContext::Generic) {
        if (FunctionID genericVersion = plan.genericVersionOf(instance.function))
            return genericVersion;
    }
    return instance.function;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
