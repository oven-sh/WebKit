/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTResultRules.h"

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include "AOTResultRulesInternal.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static bool convertAssertsInt32(const Program& program, const DeclaredType& declared)
{
    if (!declared.has(PositionFlags::IsInt32Promise) || !declared.site)
        return false;
    return program.at(declared.site).kind == TypeSiteKind::Assertion;
}

TypeID targetOfConvert(const TypingContext& context, const DeclaredType& declared)
{
    TypeID written = declared.expected().predicted();
    if (!written || !convertAssertsInt32(context.program, declared))
        return written;
    return context.types.withPrimitives(written, Primitive::Number);
}

bool changesCodeWhereProven(const TypeArena& types, TypeID tested)
{
    return holdsOnlyPrimitives(tested) || !types.at(tested).anyObject;
}

static constexpr size_t fewestFieldsOfBagOfOptions = 24;

bool describesBagOfOptions(const TypeArena& types, TypeID written)
{
    if (!written || holdsOnlyPrimitives(written))
        return false;
    Type type = types.at(written);
    std::span<const LayoutID> layouts = membersOf(types, type);
    const Program* program = TypeReader(types).program();
    if (!program)
        return false;
    bool namesBag = false;
    for (LayoutID id : layouts) {
        const Layout& layout = program->at(id);
        if (layout.kind != LayoutKind::Record)
            return false;
        namesBag |= layout.fields.size() >= fewestFieldsOfBagOfOptions && std::ranges::none_of(layout.fields, [](const Field& field) {
            return field.flags.containsAny({ FieldFlags::IsRequired, FieldFlags::IsRequiredUntilDeleted });
        });
    }
    return namesBag;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
