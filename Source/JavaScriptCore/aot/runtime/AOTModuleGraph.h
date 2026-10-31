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
#include "AOTBitVector.h"

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

// By node: the node that every way from the root to it passes last. The root has the highest number and is its own answer; a node that the root does not reach has none.
Vector<uint32_t> computeImmediateDominators(const Vector<Vector<uint32_t, 4>>& successors, uint32_t root);

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

// Sorts the ES modules of the program into the groups of ModuleTable::headOfLoadGroup() and answers the lists of ModuleTable::groupsLoadedWith(). The sites are every import() and every require() that asks the run time for a module.
JS_EXPORT_PRIVATE Vector<uint32_t> groupModulesLoadedTogether(std::span<ModuleData> modulesAndEndRecord, std::span<const ModuleRequestData> requests, std::span<const ModuleImportSite> sitesThatAskRunTime);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
