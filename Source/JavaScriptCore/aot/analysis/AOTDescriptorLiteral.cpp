/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTDescriptorLiteral.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDTypes.h"
#include "AOTValueIdentity.h"
#include <span>

namespace JSC { namespace AOT { namespace Typed {

bool listsMembersOfDescriptor(const Program& program, const Value& literal)
{
    if (literal.opcode != Opcode::NewObject || literal.prototypeOperand())
        return false;
    return program.at(literal.layout()).kind == LayoutKind::Record && literal.fields().size() == literal.valuesOperands().size();
}

std::optional<bool> constantBooleanOf(const Program& program, const Function& function, ValueID id)
{
    std::optional<ValueID> constant = constantThroughCasts(program, function, id);
    if (!constant || function.at(*constant).opcode != Opcode::Boolean)
        return std::nullopt;
    return function.at(*constant).boolean();
}

static bool addMember(const Program& program, const Function& function, BuiltinName name, ValueID value, DescriptorLiteral& read)
{
    using Flag = PropertyDescriptorFlags;
    auto addAttribute = [&](Flag isPresent, Flag isSet) {
        std::optional<bool> flag = constantBooleanOf(program, function, value);
        if (!flag)
            return false;
        read.flags.add(isPresent);
        read.flags.set(isSet, *flag);
        return true;
    };
    auto addPart = [&](Flag isPresent, ValueID& part) {
        read.flags.add(isPresent);
        part = value;
        return true;
    };
    if (name == BuiltinNames::writable)
        return addAttribute(Flag::HasWritable, Flag::IsWritable);
    if (name == BuiltinNames::enumerable)
        return addAttribute(Flag::HasEnumerable, Flag::IsEnumerable);
    if (name == BuiltinNames::configurable)
        return addAttribute(Flag::HasConfigurable, Flag::IsConfigurable);
    if (name == BuiltinNames::value)
        return addPart(Flag::HasValue, read.value);
    if (name == BuiltinNames::get)
        return addPart(Flag::HasGet, read.getter);
    if (name == BuiltinNames::set)
        return addPart(Flag::HasSet, read.setter);
    return false;
}

std::optional<DescriptorLiteral> readDescriptorLiteral(const Program& program, const Function& function, const Value& literal)
{
    if (!listsMembersOfDescriptor(program, literal))
        return std::nullopt;

    DescriptorLiteral read;
    const Layout& layout = program.at(literal.layout());
    std::span<const ValueID> values = literal.valuesOperands();
    for (size_t index = 0; index < values.size(); ++index) {
        uint32_t field = literal.fields()[index];
        if (field == layout.flow.closureEnvironmentField) {
            read.closureEnvironment = values[index];
            continue;
        }
        PropertyKeyID key = layout.fields[field].key;
        if (layout.fields[field].form.contains(FieldForm::IsAccessor) || !addMember(program, function, key ? program.builtinNameOf(key) : BuiltinName(), values[index], read))
            return std::nullopt;
    }
    if (read.isAccessor() && read.flags.containsAny({ PropertyDescriptorFlags::HasValue, PropertyDescriptorFlags::HasWritable }))
        return std::nullopt;
    return read;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
