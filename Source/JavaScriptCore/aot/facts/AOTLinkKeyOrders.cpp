/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLinkKeyOrders.h"

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include "AOTLinkFlow.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

void writeAccessorsExpectedInFields(const LinkFlow& flow, LinkTables& tables)
{
    Vector<AccessorLaidInField> laid = flow.accessorsLaidInFields;
    laid.removeAllMatching([&](AccessorLaidInField& accessor) {
        if (!accessor.appendedKey)
            return false;
        std::span<const PropertyKeyID> appendedKeys = tables.appendedKeysOf(accessor.field.layout);
        size_t place = std::ranges::find(appendedKeys, accessor.appendedKey) - appendedKeys.begin();
        accessor.field.field = static_cast<uint32_t>(flow.program.at(accessor.field.layout).fields.size() + place);
        return place == appendedKeys.size();
    });
    std::ranges::sort(laid);
    for (size_t first = 0; first < laid.size();) {
        LinkedAccessorsOfField expected { laid[first].field, { }, { } };
        bool hasSeveralGetters = false;
        bool hasSeveralSetters = false;
        size_t end = first;
        for (; end < laid.size() && laid[end].field == expected.field; ++end) {
            FunctionID& half = laid[end].isGetter ? expected.getter : expected.setter;
            bool& hasSeveral = laid[end].isGetter ? hasSeveralGetters : hasSeveralSetters;
            hasSeveral |= (half && half != laid[end].function) || !flow.tablesMayName(laid[end].function);
            half = laid[end].function;
        }
        if (hasSeveralGetters)
            expected.getter = { };
        if (hasSeveralSetters)
            expected.setter = { };
        uint32_t cell = tables.firstFieldOfLayout[expected.field.layout.index()] + expected.field.field;
        bool holdsAccessorFromBirth = cell < tables.firstFieldOfLayout[expected.field.layout.index() + 1] && tables.fields[cell].holdsAccessorFromBirth;
        expected.getterIsProven = expected.getter && holdsAccessorFromBirth && !flow.gettersThatMayBeAbsent.get(cell);
        expected.setterIsProven = expected.setter && holdsAccessorFromBirth && !flow.settersThatMayBeAbsent.get(cell);
        const LinkedCell* held = tables.findCellOfField(expected.field);
        if (expected.getter && !expected.setter && !hasSeveralSetters && held && held->keepsAccessorOnceLaid) {
            expected.getterIsProven = true;
            expected.getterMayNotBeLaidYet = true;
        }
        if (expected.getter || expected.setter)
            tables.accessorsExpectedInFields.append(expected);
        if (expected.getter || expected.setter) {
            const Layout& layout = flow.program.at(expected.field.layout);
            PropertyKeyID key = expected.field.field < layout.fields.size() ? layout.fields[expected.field.field].key : laid[first].appendedKey;
            if (key && !liesInThread(key))
                tables.keysOfAccessorsExpectedInFields.set(key.index());
        }
        first = end;
    }
}

LayoutID findHolderAboveCreatedLayout(const LinkFlow& flow, LayoutID created)
{
    FlowNodeID above = flow.findWhatStandsAbove(created);
    for (unsigned stepsLeft = LinkFlow::mostStepsAboveCreatedObject; above && stepsLeft; --stepsLeft) {
        const LinkType& type = flow.typeAt(above);
        if (type.classes == LinkType::Classes::Site && type.objects == LinkType::Objects::None)
            above = flow.findWhatStandsAbove(type.site);
        else if (type.objects == LinkType::Objects::Layout && type.classes == LinkType::Classes::None) {
            if (!flow.program.at(type.layout).has(LayoutFlags::IsOfCreatedObject))
                return type.layout;
            above = flow.findWhatStandsAbove(type.layout);
        } else
            return { };
    }
    return { };
}

void writeMembersAboveCreatedLayouts(const LinkFlow& flow, LinkTables& tables)
{
    const Program& program = flow.program;
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        LayoutID holder = program.layouts[index].has(LayoutFlags::IsOfCreatedObject) ? findHolderAboveCreatedLayout(flow, LayoutID(index)) : LayoutID();
        if (!holder)
            continue;
        uint32_t number = 0;
        auto layoutOf = [](const LinkedAccessorsOfField& accessors) {
            return accessors.field.layout;
        };
        for (const LinkedAccessorsOfField& accessors : std::ranges::equal_range(tables.accessorsExpectedInFields, holder, { }, layoutOf)) {
            PropertyKeyID key = accessors.field.field < program.at(holder).fields.size() ? fieldAt(program, accessors.field).key : tables.appendedKeysOf(holder)[accessors.field.field - program.at(holder).fields.size()];
            for (auto [kind, function] : { std::pair { MethodKind::Getter, accessors.getter }, std::pair { MethodKind::Setter, accessors.setter } }) {
                if (!key || !function)
                    continue;
                tables.membersAboveCreatedLayouts.append(LinkedMember { LayoutID(index), ClassSide::Prototype, kind, number++, key, function, false, { } });
            }
        }
    }
}

const LinkedAccessorsOfField* LinkTables::findAccessorsExpectedIn(FieldReference field) const
{
    auto found = std::ranges::lower_bound(accessorsExpectedInFields, field, { }, &LinkedAccessorsOfField::field);
    return found != accessorsExpectedInFields.end() && found->field == field ? &*found : nullptr;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
