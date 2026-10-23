/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTModuleData.h"
#include "AOTVector.h"
#include "JSExportMacros.h"
#include <optional>
#include <span>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT {

struct ContractedModuleRequests {
    Vector<uint32_t> linkRequests;
    Vector<uint32_t> evaluationRequests;
    // One entry for each list that is too long to stand for its module in the lists of the importers.
    Vector<uint32_t> modulesOverLimit;
};

JS_EXPORT_PRIVATE void computeModuleFlags(std::span<ModuleData> modulesAndEndRecord, std::span<const ModuleRequestData> requests);

// Both go by the flags that computeModuleFlags() sets.
JS_EXPORT_PRIVATE ContractedModuleRequests contractModuleRequests(std::span<ModuleData> modulesAndEndRecord, std::span<const ModuleRequestData> requests, const BitVector& modulesNamedByRequire, uint32_t maxContractedRequests);
JS_EXPORT_PRIVATE Vector<LinkPlanStepData> planLinkOfEntryPoint(std::span<ModuleData> modulesAndEndRecord, std::span<const ModuleRequestData> requests, ModuleID entryPoint);

// An import() of a module that the build knows.
struct ModuleImportSite {
    uint32_t importer;
    uint32_t target;

    friend auto operator<=>(const ModuleImportSite&, const ModuleImportSite&) = default;
};

struct LinkPlansOfRoots {
    // In the order of the modules, with the end record. Empty where there is no plan.
    Vector<LinkPlanRootData> roots;
    Vector<LinkPlanStepData> steps;
};

// What a walk of all the requests does while no module is linked.
JS_EXPORT_PRIVATE LinkPlansOfRoots planLinksOfRoots(std::span<ModuleData> modulesAndEndRecord, std::span<const ModuleRequestData> requests, std::span<const ModuleImportSite>, std::optional<ModuleID> plannedEntryPoint);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
