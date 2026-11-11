/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTMemberLookup.h"
#include "AOTFacts.h"
#include "AOTPlan.h"
#include "AOTType.h"
#include "AOTTypeTable.h"
#include <algorithm>
#include <optional>
#include <span>
#include <wtf/OptionSet.h>

namespace JSC { namespace AOT { namespace Typed {

inline TypeID storedTypeOf(const AccessContext& context, LayoutSetID receivers, PropertyKeyID key)
{
    const SlotPlan& slots = context.plan.slots();
    TypeID taken;
    for (LayoutID member : membersOf(context.types, receivers)) {
        std::optional<FieldIndex> field = slots.findField(context.program, member, key);
        if (!field)
            return TypeTable::never;
        if (TypeID type = slots.slot(member, *field).typeInLayoutRecord())
            taken = taken ? context.types.intersectionOf(taken, type) : type;
    }
    return taken;
}
inline TypeID typeHeldBySlotsOf(const AccessContext& context, std::span<const LayoutID> members, PropertyKeyID key)
{
    const SlotPlan& slots = context.plan.slots();
    TypeID held;
    for (LayoutID member : members) {
        std::optional<FieldIndex> field = slots.findField(context.program, member, key);
        TypeID type = field ? slots.slot(member, *field).type : TypeID();
        if (!type)
            return { };
        held = held ? context.types.unionOf(held, type) : type;
    }
    return held;
}
inline TypeID testableStoredTypeOf(const AccessContext& context, TypeID stored, TypeID fieldType)
{
    return fieldType && context.types.overlap(stored, fieldType) ? fieldType : TypeID(TypeTable::never);
}
inline bool storedValueFits(const AccessContext& context, TypeID stored, LayoutSetID receivers, PropertyKeyID key)
{
    const SlotPlan& slots = context.plan.slots();
    return std::ranges::all_of(membersOf(context.types, receivers), [&](LayoutID member) {
        std::optional<FieldIndex> field = slots.findField(context.program, member, key);
        if (!field)
            return false;
        const PlacedSlot& slot = slots.slot(member, *field);
        return (!slot.type || context.types.isSubset(stored, slot.type)) && (!slot.provenType || context.types.isSubset(stored, slot.provenType));
    });
}

struct DefinitionByDescriptor {
    TypeID object;
    TypeID key;
    TypeID value;
    TypeID getter;
    TypeID setter;
    OptionSet<PropertyDescriptorFlags> flags;
};

inline OptionSet<FieldForm> formCreatedBy(OptionSet<PropertyDescriptorFlags> flags)
{
    OptionSet<FieldForm> form;
    if (flags.containsAny({ PropertyDescriptorFlags::HasGet, PropertyDescriptorFlags::HasSet }))
        form.add(FieldForm::IsAccessor);
    else if (!flags.contains(PropertyDescriptorFlags::IsWritable))
        form.add(FieldForm::IsReadOnly);
    if (!flags.contains(PropertyDescriptorFlags::IsEnumerable))
        form.add(FieldForm::IsNotEnumerable);
    if (!flags.contains(PropertyDescriptorFlags::IsConfigurable))
        form.add(FieldForm::IsNotConfigurable);
    return form;
}

inline std::optional<FieldAccess> fieldCreatedInSlotBy(const AccessContext& context, const DefinitionByDescriptor& definition)
{
    const Program& program = context.program;
    TypeArena& types = context.types;
    const SlotPlan& slots = context.plan.slots();
    Type object = types.at(definition.object);
    LayoutID layout = object.hasOnlyLayouts() && !object.primitives ? singleLayoutOf(types, object) : LayoutID();
    if (!layout || program.at(layout).isEnvironment() || !slots.holdsCompiledObjects(layout))
        return std::nullopt;
    StringID text = singleStringLiteralOf(types.at(definition.key));
    std::optional<PropertyKeyID> key = text ? program.types.find(PropertyKey { .kind = PropertyKeyKind::Text, .text = text }) : std::nullopt;
    std::optional<FieldIndex> field = key ? slots.findField(program, layout, *key) : std::nullopt;
    if (!field)
        return std::nullopt;

    FieldFacts facts = (*context.facts).field({ layout, *field });
    OptionSet<FieldForm> form = facts.formOfValue;
    if (facts.facts.contains(FieldFact::IsAccessor))
        form.add(FieldForm::IsAccessor);
    if (*field < program.at(layout).fields.size()) {
        const Field& declaration = fieldAt(program, declarationOf(program, { layout, *field }));
        if (declaration.flags.containsAny({ FieldFlags::IsMethod, FieldFlags::IsHidden, FieldFlags::IsPrivate, FieldFlags::IsBrand }))
            return std::nullopt;
        form.add(declaration.form);
    }
    if (form != formCreatedBy(definition.flags))
        return std::nullopt;

    const PlacedSlot& slot = slots.slot(layout, *field);
    if (!slot.hasStorage() || !canLeaveSlot(slot.representation) || slot.mayExistWithoutStorage || slot.isFilledAtBirth || slot.deferredFunction)
        return std::nullopt;
    auto isFunctionOfProgram = [&](TypeID function) {
        Type type = types.at(function);
        return type.isObject() && type.isJavaScriptValue() && type.hasOnlyClosures();
    };
    if (form.contains(FieldForm::IsAccessor)) {
        if (slot.type && (!definition.getter || !types.isSubset(definition.getter, slot.type)))
            return std::nullopt;
        if (slot.representation != SlotRepresentation::JSValue || !slot.holdsAccessor || (definition.getter && !isFunctionOfProgram(definition.getter)) || (definition.setter && !isFunctionOfProgram(definition.setter)))
            return std::nullopt;
    } else {
        if (slot.holdsAccessor || !definition.value || !types.at(definition.value).isJavaScriptValue() || (types.at(definition.value).primitives & (Primitive::Absent | Primitive::Unspecified)))
            return std::nullopt;
        if (slot.representation != SlotRepresentation::JSValue && types.at(definition.value).mayBeUndefinedOrNull())
            return std::nullopt;
        if ((slot.type && !types.isSubset(definition.value, slot.type)) || (slot.provenType && !types.isSubset(definition.value, slot.provenType)))
            return std::nullopt;
    }
    FieldAccess access = slots.fieldAccessOfOneLayout(program, *key, layout);
    if (!access.hasLocation || access.coverage != FieldAccess::Coverage::EveryMember)
        return std::nullopt;
    return access;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
