/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCallingConventions.h"
#include "AOTEffects.h"
#include "AOTFacts.h"
#include "AOTIRProgram.h"
#include "AOTLayout.h"
#include "AOTMemberLookup.h"
#include "AOTPlan.h"

namespace JSC { namespace AOT { namespace Typed {

// Of a constructor function: nothing but its guards says that `prototype` of its one function object holds the object that the class begins with.
// The function is made once and holds no capture, so it has one function object, and its class has a record, so the run time enters the object for the class and hears of every store to `prototype`.
inline bool prototypeOfConstructorFunctionIsLeftToGuards(const AccessContext& context, LayoutID classLayout)
{
    const Layout& layout = context.program.at(classLayout);
    if (!layout.has(LayoutFlags::IsConstructorFunction) || !layout.constructor || !(*context.facts).isEvaluatedAtMostOnce(classLayout))
        return false;
    if (context.program.at(layout.constructor).captures.size() != context.plan.promotedEnvironments().numberOfRemovedCapturesOf(layout.constructor))
        return false;
    return context.plan.hasClassRecord(classLayout);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
