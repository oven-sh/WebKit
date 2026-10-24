/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTaggedLayouts.h"

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include "AOTLayout.h"
#include "AOTTypeArena.h"

namespace JSC { namespace AOT { namespace Typed {

void appendTaggedLayouts(Program& program, std::span<const TaggedFamily> families)
{
    TypeArena types(program.types);
    for (const TaggedFamily& family : families) {
        for (const TaggedLayout& member : family.members) {
            RELEASE_ASSERT(member.layout.index() == program.layouts.size());
            program.layouts.append(createDerivedLayout(program, family.maker));
            program.setDeclaredMembers(member.layout, types.completeLayoutSet(singleElementSpan(member.layout)));
        }
    }
}

static void admitDeclaredType(TypeArena& types, DeclaredType& present, const DeclaredType& declared)
{
    present.type = types.unionOf(present.type, declared.type);
    if (declared.flags.contains(PositionFlags::IsExpected))
        present.flags.add(PositionFlags::IsExpected);
    if (!declared.flags.contains(PositionFlags::IsInt32Promise) || present.flags.contains(PositionFlags::IsExpected))
        present.flags.remove(PositionFlags::IsInt32Promise);
}

static void appendFieldsOf(Program& program, TypeArena& types, LayoutID member, LayoutID declaringType)
{
    for (uint32_t index = 0; index < program.at(declaringType).fields.size(); ++index) {
        const Field& declared = program.at(declaringType).fields[index];
        if (!declared.key || declared.flags.contains(FieldFlags::IsMethod) || declared.flags.contains(FieldFlags::IsNeverWritten))
            continue;

        Layout& layout = program.at(member);
        if (std::optional<uint32_t> position = layout.findField(declared.key)) {
            Field& present = layout.fields[*position];
            if (present.flags.contains(FieldFlags::IsInherited))
                continue;
            if (!declared.flags.contains(FieldFlags::IsRequired))
                present.flags.remove(FieldFlags::IsRequired);
            admitDeclaredType(types, present.declared, declared.declared);
            continue;
        }

        Field field;
        field.key = declared.key;
        field.declared = declared.declared;
        field.declared.site = { };
        field.flags = { FieldFlags::IsAddedLate };
        if (declared.flags.contains(FieldFlags::IsRequired))
            field.flags.add(FieldFlags::IsRequired);
        layout.appendField(WTF::move(field));
    }
}

void appendFieldsOfTaggedLayouts(Program& program, std::span<const TaggedFamily> families)
{
    TypeArena types(program.types);
    for (const TaggedFamily& family : families) {
        for (const TaggedLayout& member : family.members) {
            appendInheritedFields(program, member.layout);
            for (LayoutID declaringType : member.declaringTypes)
                appendFieldsOf(program, types, member.layout, declaringType);
        }
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
