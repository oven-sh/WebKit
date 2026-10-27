/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFieldValueNarrowing.h"

#if ENABLE(AOT)

#include "AOTFamilyTables.h"
#include "AOTIRProgram.h"
#include "AOTVector.h"
#include <algorithm>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

static LayoutSetID allocatedMembersExcluding(const TypingContext& context, LayoutSetID family, LayoutSetID excluded)
{
    const LayoutSet& familySet = context.types.at(family);
    std::span<const LayoutID> allocatedMembers = context.program.familyTables().allocatedMembersOf(family);
    if (!excluded && allocatedMembers.size() == familySet.written().size())
        return family;

    std::span<const LayoutID> excludedMembers;
    if (excluded)
        excludedMembers = context.types.at(excluded).completeMembers().value();

    Vector<LayoutID> remaining;
    remaining.grow(allocatedMembers.size());
    remaining.shrink(std::ranges::set_difference(allocatedMembers, excludedMembers, remaining.begin()).out - remaining.begin());
    if (remaining.isEmpty())
        return { };
    return context.types.intern(LayoutSet::complete(WTF::move(remaining)));
}

static LayoutSetID membersThatCanHold(const TypingContext& context, LayoutSetID family, PropertyKeyID key, const Type& read)
{
    LayoutSetID excluded = context.program.familyTables().membersWhoseFieldCannotHold(family, key, context.types.intern(read), context.level);
    return allocatedMembersExcluding(context, family, excluded);
}

Type narrowHolderByFieldValue(const TypingContext& context, const Type& holder, PropertyKeyID key, const Type& read)
{
    if (read.isNever())
        return Type::never();

    Type result = holder.withoutNullish();
    if (!result.layouts)
        return result;

    LayoutSetID family = std::exchange(result.layouts, membersThatCanHold(context, result.layouts, key, read));
    if (result.isNever())
        result.layouts = family;
    return result;
}

bool narrowsHolderToNothing(const TypingContext& context, const Type& holder, PropertyKeyID key, const Type& read)
{
    Type result = holder.withoutNullish();
    if (read.isNever() || !result.layouts)
        return false;
    result.layouts = membersThatCanHold(context, result.layouts, key, read);
    return result.isNever();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
