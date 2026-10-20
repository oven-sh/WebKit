/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPlacementSteps.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTIRPrinter.h"
#include <optional>
#include <wtf/StringPrintStream.h>

namespace JSC { namespace AOT { namespace Typed {

static FieldReference fieldOf(const Program& program, LayoutID member, PropertyKeyID key)
{
    return { member, *program.at(member).findField(key) };
}

static LayoutID findMemberOutsideLocation(const SlotAccessKey& access, const SlotAgreement& agreement)
{
    if (agreement.groups.size() > 1)
        return agreement.groups[1].members[0];
    if (access.isStore && !agreement.coversEveryMember())
        return agreement.membersWithoutSlot[0];
    return { };
}

static String describeDisagreement(const Program& program, const SlotAccessKey& access, const SlotAgreement& agreement)
{
    StringPrintStream out;
    out.print(access.isStore ? "a store to "_s : "a read of "_s);
    printPropertyKey(out, program, access.key);
    out.print(" through "_s, program.types.at(access.receivers).members.size(), " layouts, which keep it in "_s, agreement.groups.size(), " places; "_s);
    out.print(agreement.membersWithoutSlot.size(), " of them have no slot for it"_s);
    return out.toString();
}

static FieldAccess::Coverage coverageOf(const SlotAgreement& agreement)
{
    if (agreement.coversEveryMember())
        return FieldAccess::Coverage::EveryMember;
    return agreement.membersWithoutSlotKeepBytesZero ? FieldAccess::Coverage::EveryMemberHasSlot : FieldAccess::Coverage::SomeMembers;
}

static PlacedSlot slotOfEveryMember(const PlacementContext& context, PropertyKeyID key, const SlotGroup& group)
{
    PlacedSlot slot = group.slot;
    if (group.members.size() == 1)
        return slot;
    slot.deferredFunction = { };
    slot.indexInLayoutRecord = std::nullopt;
    for (LayoutID member : group.members) {
        const PlacedSlot& slotOfMember = context.slots().slot(fieldOf(context.program, member, key));
        slot.mayExistWithoutStorage |= slotOfMember.mayExistWithoutStorage;
        slot.deferredClosureCapturesObject |= slotOfMember.deferredClosureCapturesObject;
    }
    return slot;
}

// intersectionOf keeps a built-in class that may overlap a class of the other type, so its result can hold an object that one member does not declare.
static TypeID storableContentsOf(const Program& program, TypeArena& arena, TypeID intersection, PropertyKeyID key, const SlotGroup& group)
{
    TypeID contents = intersection;
    for (LayoutID member : group.members) {
        TypeID declared = fieldAt(program, declarationOf(program, fieldOf(program, member, key))).declared.type;
        contents = arena.differenceOf(contents, arena.differenceOf(contents, declared));
    }
    return contents;
}

static TypeID contentsOf(const Program& program, TypeArena& arena, const FieldAccess& row, const SlotGroup& group)
{
    TypeID contents;
    bool isAlwaysFilled = true;
    for (LayoutID member : group.members) {
        FieldReference field = fieldOf(program, member, row.key);
        TypeID type = fieldAt(program, declarationOf(program, field)).declared.type;
        if (!contents)
            contents = type;
        else
            contents = row.isStore ? arena.intersectionOf(contents, type) : arena.unionOf(contents, type);
        isAlwaysFilled &= flowOf(program, field).occupancy == Occupancy::AlwaysFilled;
    }
    if (row.isStore)
        return group.members.size() == 1 ? contents : storableContentsOf(program, arena, contents, row.key, group);

    // A load from an Extension that is missing or too short answers absent, whatever flow proved about the field.
    bool slotMayBeEmpty = canLeaveSlot(row.slot.representation) && (row.slot.isOutOfLine() || !isAlwaysFilled);
    if (row.coverage == FieldAccess::Coverage::EveryMember && !slotMayBeEmpty)
        return contents;
    return arena.withPrimitives(contents, Primitive::Absent);
}

static FieldAccess createFieldAccess(const PlacementContext& context, TypeArena& arena, const SlotAccessKey& access)
{
    FieldAccess row;
    row.key = access.key;
    row.receivers = access.receivers;
    row.isStore = access.isStore;
    row.contents = TypeTable::absent;

    SlotAgreement agreement = context.slots().agreementOf(context.program, access.key, access.receivers);
    if (LayoutID member = findMemberOutsideLocation(access, agreement)) {
        context.build.diagnostics().add(InternalError { .kind = InternalErrorKind::SlotAccessNotPlaced, .layout = member, .detail = describeDisagreement(context.program, access, agreement) });
        return row;
    }
    if (agreement.groups.isEmpty())
        return row;

    const SlotGroup& group = agreement.groups[0];
    row.coverage = coverageOf(agreement);
    row.slot = slotOfEveryMember(context, access.key, group);
    row.extensionWord = group.extensionWord;
    row.membersWithField = row.coverage == FieldAccess::Coverage::EveryMember ? access.receivers : arena.completeLayoutSet(group.members.span());
    row.contents = contentsOf(context.program, arena, row, group);
    return row;
}

Vector<FieldAccess> createFieldAccesses(const PlacementContext& context, const PlacedSites& sites, TypeArena& arena)
{
    return WTF::map(sites.fieldAccesses, [&](const FieldAccessUse& use) {
        return createFieldAccess(context, arena, use.access);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
