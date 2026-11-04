/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLinkUnions.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTIRProgram.h"
#include "AOTLinkFlow.h"
#include "AOTTypeRelations.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

// It is asked on every thread at once, behind the flow. So it reads the type at a node and the tables, and asks no family of sites or of lost layouts for its members.

static bool mayBeProxyInUnionEffects(const LinkFlow& flow, const LinkType& type)
{
    if (type.holdsUnseen || type.builtin || type.classes == LinkType::Classes::Mixed)
        return true;
    const BuiltinTable& builtins = flow.program.builtins();
    BuiltinClassID classOfProxies = BuiltinClasses::Proxy;
    return type.classes == LinkType::Classes::Site && relate(builtins, type.builtinClass, classOfProxies) != ClassRelation::Disjoint;
}

// Above an object of the layout stand the prototype objects of the classes of the program and what the engine lays above those, and nobody can lay anything else there.
static bool chainAboveLayoutIsKnownInUnionEffects(const LinkTables& tables, LayoutID layout)
{
    const LinkedLayout& linked = tables.layout(layout);
    return !linked.reachesUnseenCode && !linked.possibleChanges.contains(LayoutChange::ChangesPrototype) && linked.listsEveryMemberAbove;
}

static bool walkUpChainMayRunCodeInUnionEffects(const LinkFlow& flow, const LinkTables& tables, const LinkType& type)
{
    if (mayBeProxyInUnionEffects(flow, type) || type.classes != LinkType::Classes::None || type.functions != LinkType::Functions::None)
        return true;
    switch (type.objects) {
    case LinkType::Objects::None:
        return false;
    case LinkType::Objects::Layout:
    case LinkType::Objects::Prototype:
        return !chainAboveLayoutIsKnownInUnionEffects(tables, type.layout);
    case LinkType::Objects::Family:
    case LinkType::Objects::Prototypes:
        return !std::ranges::all_of(flow.program.types.listOf(flow.families.layoutsOf(type.family)), [&](LayoutID member) {
            return chainAboveLayoutIsKnownInUnionEffects(tables, member);
        });
    case LinkType::Objects::Lost:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool someInspectionMayRunCode(const LinkFlow& flow, const LinkTables& tables, FunctionID id)
{
    for (const ThinSummary::Inspection& inspection : flow.summaries.at(id).inspections) {
        const LinkType& type = flow.typeAt(flow.nodeOf(id, inspection.subject));
        switch (inspection.kind) {
        case ThinSummary::Inspection::Kind::WalksChain:
            if (walkUpChainMayRunCodeInUnionEffects(flow, tables, type))
                return true;
            break;
        case ThinSummary::Inspection::Kind::ListsKeys:
            if (mayBeProxyInUnionEffects(flow, type))
                return true;
            break;
        }
    }
    return false;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
