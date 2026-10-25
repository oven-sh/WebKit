/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTClosure.h"
#include "AOTCompactTable.h"
#include "AOTEnvironment.h"
#include "AOTObject.h"
#include "AOTSlimClosure.h"

namespace JSC {

template<> struct InlineCellVisit<CellVisitKind::CompiledObject> : CellFieldsVisit<AOT::Object, AOT::Object> { };
template<> struct InlineCellVisit<CellVisitKind::CompiledEnvironment> : CellFieldsVisit<AOT::Environment, AOT::Environment> { };
template<> struct InlineCellVisit<CellVisitKind::CompactTable> : CellFieldsVisit<AOT::CompactTable, AOT::CompactTable> { };
template<> struct InlineCellVisit<CellVisitKind::CompiledClosure> : ObjectFieldsVisit<AOT::Closure, JSCallee, JSFunction, AOT::Closure> { };

template<> struct InlineCellVisit<CellVisitKind::SlimClosure> {
    ALWAYS_INLINE static bool tryVisit(FastMarkAppender& appender, JSCell* cell, Structure* structure)
    {
        appender.appendStructure(structure);
        // Not uncheckedDowncast, which asserts the class: the mutator may be reifying this cell right now.
        SUPPRESS_MEMORY_UNSAFE_CAST return AOT::SlimClosure::tryVisitFields(static_cast<AOT::SlimClosure*>(cell), appender);
    }
};

} // namespace JSC

#endif // ENABLE(AOT)
