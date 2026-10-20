/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTModuleGraph.h"

#if ENABLE(AOT)

#include "AOTStronglyConnectedComponents.h"
#include <algorithm>
#include <wtf/OptionSet.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

namespace {

enum class ModuleProperty : uint8_t {
    IsAsynchronous = 1 << 0,
    HasLinkWork = 1 << 1,
    IsUncompiledCode = 1 << 2,
};

struct ComponentSummary {
    OptionSet<ModuleProperty> reachedProperties;
    uint32_t numberOfModules { 0 };
};

struct RequestTargets {
    std::span<const ModuleRequestData> requests;

    size_t size() const { return requests.size(); }
    uint32_t operator[](size_t index) const { return requests[index].target; }
};

} // anonymous namespace

static OptionSet<ModuleProperty> ownPropertiesOf(const ModuleData& module, const ModuleData& nextRecord)
{
    OptionSet<ModuleProperty> properties;
    if (module.hasTopLevelAwait() || module.isUncompiledCode())
        properties.add(ModuleProperty::IsAsynchronous);
    if (module.capturesExports() || module.isUncompiledCode() || module.firstSyntheticExport != nextRecord.firstSyntheticExport || module.firstRequiredName != nextRecord.firstRequiredName)
        properties.add(ModuleProperty::HasLinkWork);
    if (module.isUncompiledCode())
        properties.add(ModuleProperty::IsUncompiledCode);
    return properties;
}

static void addComponentFlags(ModuleData& module, const ComponentSummary& component)
{
    if (!component.reachedProperties.contains(ModuleProperty::IsAsynchronous))
        module.add(ModuleData::Flag::IsSynchronous);
    if (component.reachedProperties.contains(ModuleProperty::HasLinkWork))
        module.add(ModuleData::Flag::NeedsLink);
    if (component.reachedProperties.contains(ModuleProperty::IsUncompiledCode))
        module.add(ModuleData::Flag::ReachesUncompiledCode);
    if (module.hasExportTable() && !module.hasTopLevelAwait() && component.numberOfModules == 1)
        module.add(ModuleData::Flag::IsSimple);
}

void computeModuleFlags(std::span<ModuleData> modulesAndEndRecord, std::span<const ModuleRequestData> requests)
{
    RELEASE_ASSERT(!modulesAndEndRecord.empty());
    unsigned numberOfModules = static_cast<unsigned>(modulesAndEndRecord.size() - 1);
    auto requestsOf = [&](unsigned module) {
        uint32_t first = modulesAndEndRecord[module].firstRequest;
        return requests.subspan(first, modulesAndEndRecord[module + 1].firstRequest - first);
    };

    Typed::StronglyConnectedComponents components = Typed::computeStronglyConnectedComponents(numberOfModules, [&](unsigned module) {
        return RequestTargets { requestsOf(module) };
    });
    const Vector<uint32_t>& componentOf = components.componentOf;

    Vector<uint32_t> modulesByComponent(numberOfModules, [](size_t index) {
        return static_cast<uint32_t>(index);
    });
    std::ranges::sort(modulesByComponent, { }, [&](uint32_t module) {
        return componentOf[module];
    });

    Vector<ComponentSummary> summaries(components.numberOfComponents);
    // A component has a higher number than every component it requests.
    for (uint32_t module : modulesByComponent) {
        ComponentSummary& summary = summaries[componentOf[module]];
        ++summary.numberOfModules;
        summary.reachedProperties.add(ownPropertiesOf(modulesAndEndRecord[module], modulesAndEndRecord[module + 1]));
        for (const ModuleRequestData& request : requestsOf(module)) {
            summary.reachedProperties.add(summaries[componentOf[request.target]].reachedProperties);
            if (request.isDynamicStarExport)
                modulesAndEndRecord[module].add(ModuleData::Flag::HasDynamicStarExport);
            if (request.isDeferred)
                modulesAndEndRecord[module].add(ModuleData::Flag::HasDeferredRequest);
        }
    }

    for (unsigned module = 0; module < numberOfModules; ++module)
        addComponentFlags(modulesAndEndRecord[module], summaries[componentOf[module]]);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
