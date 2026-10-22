/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPlacementSteps.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTIRPrinter.h"
#include "AOTNarrowFieldTypes.h"
#include "AOTSlotPlanSteps.h"
#include <algorithm>
#include <optional>
#include <wtf/StringPrintStream.h>

namespace JSC { namespace AOT { namespace Typed {

static FieldReference fieldOf(const Program& program, LayoutID member, PropertyKeyID key)
{
    return { member, *program.at(member).findField(key) };
}

static LayoutID findMemberOutsideLocation(const SlotAccessKey& access, const SlotAgreement& agreement)
{
    if (agreement.hasOffsetByShape(access.isStore))
        return { };
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

// The uses are folded by what they access. Only the report of an error asks where one of them stands: the first, by function, block and value.
static InternalError errorAtFirstSite(const Program& program, const SlotAccessKey& access, InternalError&& error)
{
    auto isSite = [&](const Value& value) {
        if (Value::opcodesWithPlacedKeys.contains(value.opcode)) {
            return access.isStore && value.receivers() == access.receivers && std::ranges::any_of(value.placedKeys(), [&](PlacedKey placedKey) {
                return placedKey.key == access.key;
            });
        }
        return Value::opcodesWithFieldAccess.contains(value.opcode) && value.receivers() == access.receivers && value.key() == access.key && isSlotStore(value.opcode) == access.isStore;
    };
    for (uint32_t functionIndex = 0; functionIndex < program.functions.size(); ++functionIndex) {
        const Function& function = program.functions[functionIndex];
        for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
            for (ValueID id : function.blocks[blockIndex].values) {
                if (!isSite(function.at(id)))
                    continue;
                error.function = FunctionID(functionIndex);
                error.block = BlockID(blockIndex);
                error.value = id;
                return WTF::move(error);
            }
        }
    }
    return WTF::move(error);
}

static FieldAccess::Coverage coverageOf(const SlotAgreement& agreement)
{
    if (agreement.coversEveryMember())
        return FieldAccess::Coverage::EveryMember;
    return agreement.membersWithoutSlotKeepBytesZero ? FieldAccess::Coverage::EveryMemberHasSlot : FieldAccess::Coverage::SomeMembers;
}

static PlacedSlot slotOfEveryMember(const PlacementContext& context, const SlotAccessKey& access, const SlotGroup& group)
{
    PlacedSlot slot = group.slot;
    if (group.members.size() == 1)
        return slot;
    slot.deferredFunction = { };
    slot.indexInLayoutRecord = std::nullopt;
    for (LayoutID member : group.members) {
        const PlacedSlot& slotOfMember = context.slots().slot(fieldOf(context.program, member, access.key));
        // A read asks the key order where some member may list the field without storage. A store of undefined asks for no storage only where every member may.
        if (access.isStore)
            slot.mayExistWithoutStorage &= slotOfMember.mayExistWithoutStorage;
        else
            slot.mayExistWithoutStorage |= slotOfMember.mayExistWithoutStorage;
        slot.deferredClosureCapturesObject |= slotOfMember.deferredClosureCapturesObject;
        slot.deferredClosureCapturesEnvironment |= slotOfMember.deferredClosureCapturesEnvironment;
        slot.defersFlattenedObject &= slotOfMember.defersFlattenedObject;
    }
    return slot;
}

static bool mayBeFilledWithoutStorage(const PlacementContext& context, const SlotAccessKey& access, const SlotGroup& group)
{
    return std::ranges::any_of(group.members, [&](LayoutID member) {
        return context.slots().slot(fieldOf(context.program, member, access.key)).mayExistWithoutStorage;
    });
}

// intersectionOf keeps a built-in class that may overlap a class of the other type, so its result can hold an object that one member does not declare.
static TypeID storableContentsOf(const Program& program, TypeArena& arena, TypeID intersection, PropertyKeyID key, const SlotGroup& group)
{
    TypeID contents = intersection;
    for (LayoutID member : group.members) {
        TypeID declared = slotTypeOf(program, fieldAt(program, declarationOf(program, fieldOf(program, member, key))));
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
        TypeID type = slotTypeOf(program, fieldAt(program, declarationOf(program, field)));
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

static std::optional<uint32_t> offsetOfInnerObjectOfEveryMember(const PlacementContext& context, const SlotAccessKey& access, const SlotGroup& group)
{
    std::optional<uint32_t> offset;
    for (LayoutID member : group.members) {
        const PlacedInnerObject* inner = context.slots().layout(member).innerObjectIn(fieldOf(context.program, member, access.key).field);
        if (!inner || !inner->isAlwaysInPlace || (offset && *offset != inner->offset))
            return std::nullopt;
        offset = inner->offset;
    }
    return offset;
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
        context.build.diagnostics().add(errorAtFirstSite(context.program, access, InternalError { .kind = InternalErrorKind::SlotAccessNotPlaced, .layout = member, .detail = describeDisagreement(context.program, access, agreement) }));
        return row;
    }
    if (agreement.groups.isEmpty())
        return row;

    row.findsOffsetByShape = agreement.hasOffsetByShape(access.isStore);
    const SlotGroup group = row.findsOffsetByShape ? agreement.groupOfAllOffsets() : agreement.groups[0];
    row.coverage = coverageOf(agreement);
    row.slot = slotOfEveryMember(context, access, group);
    row.extensionWord = group.extensionWord;
    row.mayBeFilledWithoutStorage = mayBeFilledWithoutStorage(context, access, group);
    bool everyReceiverIsMember = group.members.size() == context.program.types.at(access.receivers).members.size();
    row.membersWithField = everyReceiverIsMember ? access.receivers : arena.completeLayoutSet(group.members.span());
    row.contents = contentsOf(context.program, arena, row, group);
    if (!row.isStore && row.coverage == FieldAccess::Coverage::EveryMember)
        row.offsetOfInnerObject = offsetOfInnerObjectOfEveryMember(context, access, group);
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
