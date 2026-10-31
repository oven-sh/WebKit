/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include <optional>
#include <wtf/OptionSet.h>

namespace JSC { namespace AOT { namespace Typed {

// What ToPropertyDescriptor reads from an object literal.
struct DescriptorLiteral {
    ValueID value;
    ValueID getter;
    ValueID setter;
    ValueID closureEnvironment;
    OptionSet<PropertyDescriptorFlags> flags;

    bool isAccessor() const { return flags.containsAny({ PropertyDescriptorFlags::HasGet, PropertyDescriptorFlags::HasSet }); }
};

// The attributes of a property that did not exist before the definition.
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

// The literal lists all its members, and above it stands Object.prototype.
bool listsMembersOfDescriptor(const Program&, const Value& literal);
std::optional<bool> constantBooleanOf(const Program&, const Function&, ValueID);
// Nothing: the literal does not say what the built-in reads, or the built-in throws.
std::optional<DescriptorLiteral> readDescriptorLiteral(const Program&, const Function&, const Value& literal);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
