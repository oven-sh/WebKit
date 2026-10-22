/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFieldValueNarrowing.h"

#if ENABLE(AOT)

#include "AOTFamilyTables.h"
#include "AOTIRProgram.h"
#include <algorithm>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

static LayoutSetID allocatedMembersExcluding(const TypingContext& context, LayoutSetID family, LayoutSetID excluded)
{
    const LayoutSet& familySet = context.types.at(family);
    std::span<const LayoutID> allocatedMembers = context.program.familyTables().allocatedMembersOf(family);
    if (!excluded && allocatedMembers.size() == familySet.members.size())
        return family;

    // `excluded` comes from a shared cache and can be most of a large family, so it is read in place instead of adopted into this arena.
    std::span<const LayoutID> excludedMembers;
    if (excluded)
        excludedMembers = context.types.at(excluded).members.span();

    Vector<LayoutID> remaining;
    remaining.grow(allocatedMembers.size());
    remaining.shrink(std::ranges::set_difference(allocatedMembers, excludedMembers, remaining.begin()).out - remaining.begin());
    if (remaining.isEmpty() && !familySet.hasMembersWithoutLayout)
        return { };
    return context.types.intern(LayoutSet { WTF::move(remaining), true, familySet.hasMembersWithoutLayout });
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

    // The read would have thrown on undefined or null.
    Type result = holder.withoutNullish();
    if (!result.layouts)
        return result;

    LayoutSetID family = std::exchange(result.layouts, membersThatCanHold(context, result.layouts, key, read));
    // The code behind a Check that never passes is still the source of the generic version, so it must not be cut as unreachable.
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
