/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFlowSolution.h"
#include <algorithm>
#include <optional>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

// Call-site instances of one function whose allocations name the same derived layouts. Which version runs decides what is allocated, so a call that flow linked to one of the
// instances enters this version and no other, and no other call enters it.
struct FlowCallSiteVersion {
    FunctionID body;
    Vector<FlowInstanceID, 1> instances;
};

struct FlowCallSiteVersions {
    Vector<FlowCallSiteVersion> versions;
    // The index of the version of an instance plus one. Empty where no function has such a version.
    Vector<uint32_t> versionOfInstance;

    std::optional<uint32_t> versionOf(FlowInstanceID instance) const
    {
        if (instance.index() >= versionOfInstance.size() || !versionOfInstance[instance.index()])
            return std::nullopt;
        return versionOfInstance[instance.index()] - 1;
    }
};

// The functions as read come first, then the generic versions, then the call-site versions.
struct FlowVersionPlan {
    Vector<FunctionID> provenVersions;
    FlowCallSiteVersions callSite;
    uint32_t functionCountAsRead { 0 };

    uint32_t firstCallSiteVersion() const { return functionCountAsRead + static_cast<uint32_t>(provenVersions.size()); }
    uint32_t functionCount() const { return firstCallSiteVersion() + static_cast<uint32_t>(callSite.versions.size()); }
    bool isGenericVersion(FunctionID version) const { return version.index() >= functionCountAsRead && version.index() < firstCallSiteVersion(); }
    bool isCallSiteVersion(FunctionID version) const { return version.index() >= firstCallSiteVersion(); }
    const FlowCallSiteVersion& callSiteVersion(FunctionID version) const LIFETIME_BOUND { return callSite.versions[version.index() - firstCallSiteVersion()]; }
    void forEachCallSiteVersionOf(FunctionID body, NOESCAPE const Invocable<void(FunctionID)> auto& functor) const
    {
        auto versionsOfBody = std::ranges::equal_range(callSite.versions, body, { }, &FlowCallSiteVersion::body);
        for (const FlowCallSiteVersion& version : versionsOfBody)
            functor(FunctionID(firstCallSiteVersion() + static_cast<uint32_t>(&version - callSite.versions.begin())));
    }
    FunctionID genericVersionOf(FunctionID provenVersion) const;
    FunctionID provenVersionOf(FunctionID genericVersion) const;
    FunctionID bodyOf(FunctionID version) const;
};

struct FlowFunctionVersion {
    FunctionID id;
    FunctionID body;
    Vector<FlowInstanceID, 2> instances;

    bool isReached() const { return !instances.isEmpty(); }
};

FlowFunctionVersion findFunctionVersion(const FlowGraph&, const FlowVersionPlan&, FunctionID version);

// The version that a call linked to the instance names.
FunctionID versionRunning(const FlowGraph&, const FlowVersionPlan&, FlowInstanceID);

// That version, and the generic version that the call may enter in its place.
void forEachVersionRunning(const FlowGraph& graph, const FlowVersionPlan& plan, FlowInstanceID instance, NOESCAPE const Invocable<void(FunctionID)> auto& functor)
{
    FunctionID version = versionRunning(graph, plan, instance);
    functor(version);
    if (FunctionID genericVersion = plan.genericVersionOf(version))
        functor(genericVersion);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
