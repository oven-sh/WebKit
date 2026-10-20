/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"
#include <initializer_list>
#include <wtf/Vector.h>
#include <wtf/text/ASCIILiteral.h>

namespace JSC { namespace AOT { namespace Typed {

#define FOR_EACH_AOT_CLAIM_RESULT(v) \
    v(Unknown) \
    v(Claimed) \
    v(AlreadyClaimed) \
    v(UsedOutsideGroup) \
    v(CrossesEffects) \
    v(SpansBlocks) \
    v(SpansInlineFrames) \
    v(BoundToFrame) \
    v(OnSlowPath) \
    v(MemberLimit) \
    v(SpanLimit) \
    v(RuleIsDisabled)

enum class ClaimResult : uint8_t {
#define AOT_CLAIM_RESULT_ENUMERATOR(name) name,
    FOR_EACH_AOT_CLAIM_RESULT(AOT_CLAIM_RESULT_ENUMERATOR)
#undef AOT_CLAIM_RESULT_ENUMERATOR
};

#define AOT_COUNT_CLAIM_RESULT(name) + 1
inline constexpr unsigned numberOfClaimResults = 0 FOR_EACH_AOT_CLAIM_RESULT(AOT_COUNT_CLAIM_RESULT);
#undef AOT_COUNT_CLAIM_RESULT

constexpr ASCIILiteral nameOf(ClaimResult result)
{
    switch (result) {
#define AOT_CLAIM_RESULT_NAME(name) \
    case ClaimResult::name: \
        return #name ""_s;
    FOR_EACH_AOT_CLAIM_RESULT(AOT_CLAIM_RESULT_NAME)
#undef AOT_CLAIM_RESULT_NAME
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

struct GroupMember {
    ValueID value;
    bool isUsedOutside { false };
    // The member does nothing but produce its value, and the rows that replace the group do without it.
    bool isOnlyErased { false };
};

struct SelectionGroup {
    ValueID root;
    Vector<GroupMember, 4> members;
    bool hasSlowPath { false };
    bool endsEveryArmInCopyOfRoot { false };

    bool contains(ValueID id) const
    {
        return members.containsIf([&](const GroupMember& member) {
            return member.value == id;
        });
    }
};

inline SelectionGroup groupOfOne(ValueID root)
{
    return { .root = root, .members = { GroupMember { root } } };
}

inline SelectionGroup groupOf(std::initializer_list<ValueID> membersInBlockOrder)
{
    SelectionGroup group;
    for (ValueID member : membersInBlockOrder)
        group.members.append(GroupMember { member });
    group.root = group.members.last().value;
    return group;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
