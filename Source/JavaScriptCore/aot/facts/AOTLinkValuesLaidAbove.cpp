/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLinkValuesLaidAbove.h"

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include "AOTLinkFlow.h"
#include "AOTLinkTables.h"
#include "AOTNameClass.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static bool holdsOnlyPrimitivesInLinkValuesLaidAbove(const LinkType& type)
{
    return type.objects == LinkType::Objects::None && type.functions == LinkType::Functions::None && type.classes == LinkType::Classes::None && !type.holdsUnseen && !type.holdsLostBuiltins && !type.builtin;
}

bool namesFieldBelowPrimitivesLaidAbove(const LinkFlow& flow, LayoutID layout, PropertyKeyID key)
{
    const Program& program = flow.program;
    if (!flow.isBelowConstructorFunction(layout) || !program.types.hasStableNumber(key) || program.at(key).kind != PropertyKeyKind::Text)
        return false;
    NameClass nameClass = program.nameClassOf(key);
    if (mayBeElementKey(nameClass) || nameClass == NameClass::Proto || !flow.findMembers(layout, ClassSide::Prototype, key).isEmpty())
        return false;

    bool isLaidAbove = false;
    // `class A extends B {}` beside `class B extends A {}` compiles.
    size_t stepsLeft = program.layouts.size();
    for (LayoutID current = layout; current && stepsLeft; current = program.at(current).base, --stepsLeft) {
        if (!flow.isOfConstructorFunction(current))
            continue;
        const LinkFlow::NameOnPrototype* underKey = flow.findNameOnPrototype(current, key);
        const LinkFlow::NameOnPrototype* underSomeKey = flow.findNameOnPrototype(current, { });
        if ((underKey && underKey->isDefinedByDescriptor) || (underSomeKey && underSomeKey->isDefinedByDescriptor))
            return false;
        if (!underKey || !underKey->isStoredInto)
            continue;
        if (!holdsOnlyPrimitivesInLinkValuesLaidAbove(flow.typeAt(underKey->storedInSight)))
            return false;
        isLaidAbove = true;
    }
    return isLaidAbove;
}

bool isLaidOnPrototypeAbove(const Program& program, const LinkTables& tables, LayoutID layout, PropertyKeyID key)
{
    if (tables.namesOnPrototypes.isEmpty())
        return false;
    auto placeOf = [](const LinkedNameOnPrototype& laid) {
        return std::pair { laid.layout, laid.key };
    };
    size_t stepsLeft = program.layouts.size();
    for (LayoutID current = layout; current && stepsLeft; current = program.at(current).base, --stepsLeft) {
        if (program.at(current).has(LayoutFlags::IsConstructorFunction) && std::ranges::binary_search(tables.namesOnPrototypes, std::pair { current, key }, { }, placeOf))
            return true;
    }
    return false;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
