/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFlowObjects.h"
#include <utility>
#include <wtf/FastMalloc.h>
#include <wtf/HashMap.h>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

struct FlowObjectIndexes {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(FlowObjectIndexes);
    WTF_MAKE_NONCOPYABLE(FlowObjectIndexes);

    explicit FlowObjectIndexes(const Program&);

    void didAppend(FlowObjectID, const FlowObject&);
    void didAppend(AllocationClassID, const FlowAllocationClassMembers&);

    BuildContext* build { nullptr };
    bool didReportFullObjectIDs { false };
    Vector<FlowObjectID> builtinObjects;
    HashMap<std::pair<BuiltinID, ModuleID>, FlowObjectID> moduleBuiltinObjects;
    Vector<FlowObjectID> namespaceObjects;
    HashMap<FlowObjectID, FlowObjectID> prototypeObjects;
    Vector<Vector<FlowObjectID, 1>> closuresByFunction;
    Vector<Vector<AllocationClassID, 1>> allocationClassesByLayout;
    Vector<FlowObjectID> layoutObjects;
    HashMap<LayoutID, Vector<LayoutID, 1>> derivedLayouts;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
