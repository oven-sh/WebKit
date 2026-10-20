/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFlowObjectIndexes.h"
#include "AOTFlowObjectState.h"
#include "AOTFlowParts.h"
#include <span>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT { namespace Typed {

enum class FlowObjectTemporaryRole : uint16_t {
    AllocationPrototypes,
    PrototypeOfAllocatedObject,
    ParentOfClassObject,
};

constexpr FlowTemporaryKey objectTemporaryKey(FlowStatementSite site, FlowObjectTemporaryRole role, uint32_t index = 0)
{
    return { site, FlowTemporaryOwner::Objects, static_cast<uint16_t>(role), index };
}

FlowObject makeFlowObject(const Program&, const FlowSiteDescription&);
FlowAllocationClassMembers makeAllocationClassMembers(const FlowSiteDescription&);
Vector<uint32_t, 8> requiredFieldsOf(const Layout&);

using FlowObjectAppender = ScopedLambda<FlowObjectID(const FlowObject&)>;

Vector<AllocationSiteID> uniteSites(const FlowObjectTable&, std::span<const FlowSummary> summariesByFunction);
FlowObjectID appendAllocationClassFor(FlowObjectTable&, std::span<const AllocationSiteID> ascendingSites, NOESCAPE const FlowObjectAppender&);
FlowObjectID ensureLayoutObject(FlowObjectTable&, BuildContext&, LayoutID, NOESCAPE const FlowObjectAppender&);
void addSiteToLayoutObject(FlowObjectTable&, AllocationSiteID, FlowObjectID layoutObject);

FlowObjectID copyObject(FlowSolver&, FlowObjectID original, AllocationSiteID, FlowInstanceID owner);

struct FlowSolveAllocation {
    FlowObjectID object;
    bool isNew { false };
};

FlowSolveAllocation ensureSolveAllocation(FlowSolver&, FlowInstanceID, const FlowSiteDescription&);

struct FlowAllocatedObject {
    FlowStatementSite site;
    ValueID origin;
    FlowObjectID object;
    bool isInCycle { false };
    bool isByConstructor { false };
    bool isCompletedByPieces { false };
};

void noteAllocation(FlowSolver&, const FlowAllocatedObject&, std::span<const uint32_t> initialFields);

std::span<const DataNode* const> dataNodesInPreorder(FlowObjectState&, const Program&, DataID);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
