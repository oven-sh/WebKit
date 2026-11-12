/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include "AOTLayout.h"
#include "AOTTypeArena.h"

namespace JSC { namespace AOT { namespace Typed {

LayoutID findLayoutKnownByStructure(const Walker&, TypeID);

ALWAYS_INLINE BuiltinClassID builtinClassBelowType(const Walker& walker, TypeID type)
{
    if (!type || holdsOnlyPrimitives(type))
        return { };
    LayoutSetID layouts = walker.types().recordOf(type).layouts;
    return holdsOneMember(layouts) ? builtinClassOfInstances(walker.program().at(LayoutID(positionOf(layouts)))) : BuiltinClassID();
}

ALWAYS_INLINE LayoutID layoutKnownByStructureOfClass(const Walker& walker, TypeID type)
{
    return builtinClassBelowType(walker, type) ? findLayoutKnownByStructure(walker, type) : LayoutID();
}

ALWAYS_INLINE bool isArraySubclassInstance(const Walker& walker, TypeID type)
{
    return builtinClassBelowType(walker, type) == BuiltinClasses::Array && findLayoutKnownByStructure(walker, type);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
