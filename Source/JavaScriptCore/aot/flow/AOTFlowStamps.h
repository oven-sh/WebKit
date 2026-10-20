/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFlowResultFamilies.h"
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

constexpr AccessKind accessKindOf(FlowAccessKind kind)
{
    switch (kind) {
    case FlowAccessKind::Load:
        return AccessKind::Read;
    case FlowAccessKind::Store:
        return AccessKind::Store;
    case FlowAccessKind::Define:
    case FlowAccessKind::DefineGetter:
    case FlowAccessKind::DefineSetter:
        return AccessKind::Define;
    case FlowAccessKind::Delete:
        return AccessKind::Delete;
    case FlowAccessKind::Test:
        return AccessKind::Has;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

constexpr AccessKind accessKindOf(FlowCallMode mode)
{
    switch (mode) {
    case FlowCallMode::Call:
        return AccessKind::Call;
    case FlowCallMode::Construct:
    case FlowCallMode::SuperConstruct:
        return AccessKind::Construct;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

FlowAccessSiteNumbering numberAccessSites(const Program&, BuildContext&);
Vector<AccessSiteID> accessSitesByValue(const Function&, const FlowAccessSiteNumbering&, FunctionID functionAsRead);

void fillAccessSites(const FlowTypeInputs&, FunctionID functionAsRead, FlowSiteTables&);
void fillAllocationSites(const FlowFamilyInputs&, FlowSiteTables&);

void computeStamps(const FlowBodyTask&, FlowSiteTables&);

Vector<FlowIndirectCall> computeIndirectCalls(const FlowFamilyInputs&, const FlowVersionPlan&, const FlowAccessSiteNumbering&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
