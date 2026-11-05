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
    for (const Layout& layout : flow.program.layouts) {
        for (const StaticField& field : layout.staticFields) {
            if (field.key && !liesInThread(field.key))
                tables.keysOfStaticFields.set(field.key.index());
        }
    }
    Vector<AccessorLaidInField> laid = flow.accessorsLaidInFields;
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
        if (expected.getter || expected.setter)
            tables.accessorsExpectedInFields.append(expected);
        if (expected.getter || expected.setter) {
            const Layout& layout = flow.program.at(expected.field.layout);
            if (expected.field.field < layout.fields.size() && !liesInThread(layout.fields[expected.field.field].key))
                tables.keysOfAccessorsExpectedInFields.set(layout.fields[expected.field.field].key.index());
        }
        first = end;
    }
}

const LinkedAccessorsOfField* LinkTables::findAccessorsExpectedIn(FieldReference field) const
{
    auto found = std::ranges::lower_bound(accessorsExpectedInFields, field, { }, &LinkedAccessorsOfField::field);
    return found != accessorsExpectedInFields.end() && found->field == field ? &*found : nullptr;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
