/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLinkUnions.h"

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include "AOTLinkFlow.h"
#include "AOTTypeTable.h"
#include "AOTUnionTaggedMakers.h"

namespace JSC { namespace AOT { namespace Typed {

void comeAliveForUnions(LinkFlow& flow, FunctionID id)
{
    noteLayoutsMadeByTag(flow, id);
}

void notePrototypesLaidAboveOtherObjects(LinkFlow& flow, const LinkType& above)
{
    auto noteLaidAbove = [&](LayoutID layout) {
        if (!flow.prototypesLaidAboveOtherObjects.quickGet(layout.index()))
            flow.note(LinkFlow::Note::PrototypeLaidAboveOtherObjects, layout.index());
    };
    if (above.objects == LinkType::Objects::Prototype) {
        noteLaidAbove(above.layout);
        return;
    }
    if (above.objects != LinkType::Objects::Prototypes)
        return;
    for (LayoutID member : flow.program.types.listOf(flow.families.layoutsOf(above.family)))
        noteLaidAbove(member);
}

static bool onlyOwnObjectsStandBelowPrototypeOf(const LinkFlow& flow, const LinkTables& tables, LayoutID id)
{
    const Program& program = flow.program;
    const Layout& layout = program.at(id);
    if (layout.kind != LayoutKind::Instance || !layout.constructor)
        return false;
    if (tables.layout(id).reachesUnseenCode || flow.escapedLayoutsAsRead().get(id.index()) || flow.prototypesLaidAboveOtherObjects.get(id.index()))
        return false;
    if (tables.layout(id).possibleChanges.contains(LayoutChange::ChangesPrototype))
        return false;
    if (!program.layoutsExpectedBelow(id).empty() && !onlyTaggedMembersLieBelow(program, id))
        return false;
    return tables.function(layout.constructor).everyCallerIsKnown;
}

void writeTablesOfUnions(LinkFlow& flow, LinkTables& tables)
{
    if (flow.reading() != WhatUnseenCodeDoes::Anything)
        return;
    for (const TaggedMaker& maker : flow.taggedMakers) {
        if (flow.reached.get(maker.constructor.index()))
            tables.taggedMakers.append(maker);
    }
    for (uint32_t index = 0; index < tables.layouts.size(); ++index)
        tables.layouts[index].onlyOwnObjectsStandBelowPrototype = onlyOwnObjectsStandBelowPrototypeOf(flow, tables, LayoutID(index));
}

TypeID typeOfElementsOfTemplateObject(const BodyValue& made)
{
    for (TemplateString part : made.as<Opcode::NewTemplateObject>().templateStrings()) {
        if (!part.cooked)
            return typeOfPrimitives(Primitive::String | Primitive::Undefined);
    }
    return TypeTable::string;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
