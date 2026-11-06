/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"
#include "AOTVector.h"
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

struct FlowModuleOrder {
    Vector<uint32_t> componentOf;
    BitVector modulesInCycles;
    Vector<BitVector> componentsCompletedBefore;
    BitVector modulesThatJobsMayInterrupt;

    bool completesBefore(ModuleID first, ModuleID second) const;
    bool runsAfterRequests(ModuleID module) const { return module.index() < componentOf.size() && !modulesInCycles.get(module.index()); }
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
