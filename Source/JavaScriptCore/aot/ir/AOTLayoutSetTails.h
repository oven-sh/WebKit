/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include "AOTSort.h"
#include "AOTThreshold.h"
#include <algorithm>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

// By the number of a long complete set: the set of the table that lists all of its members but the first, if there is one.
// In a long row of classes each class names all the classes below it: what is known of the set of the class right below serves for all but one member.
inline Vector<LayoutSetID> findLayoutSetsWithoutFirstMember(const Program& program)
{
    AOT_THRESHOLD(size_t, minMembersOfLongSet, 32);
    struct SetWithSum {
        uint64_t sum;
        uint32_t set;
    };
    auto membersOf = [&](uint32_t index) -> std::span<const LayoutID> {
        const LayoutSet& set = program.at(LayoutSetID(index));
        return set.listsEveryLayout() ? set.written().span() : std::span<const LayoutID> { };
    };
    auto mix = [](LayoutID member) {
        uint64_t mixed = (static_cast<uint64_t>(member.index()) + 1) * 0x9e3779b97f4a7c15ull;
        return mixed ^ (mixed >> 29);
    };

    uint32_t numberOfSets = program.types.numberOfLayoutSets();
    Vector<LayoutSetID> result(numberOfSets);
    Vector<SetWithSum> sets;
    for (uint32_t index = 0; index < numberOfSets; ++index) {
        std::span<const LayoutID> members = membersOf(index);
        if (members.size() + 1 < minMembersOfLongSet)
            continue;
        uint64_t sum = 0;
        for (LayoutID member : members)
            sum += mix(member);
        sets.append({ sum, index });
    }
    Vector<SetWithSum> setsBySum = sets;
    AOT::sort(setsBySum, { }, &SetWithSum::sum);
    for (const SetWithSum& whole : sets) {
        std::span<const LayoutID> members = membersOf(whole.set);
        if (members.size() < minMembersOfLongSet)
            continue;
        for (const SetWithSum& candidate : std::ranges::equal_range(setsBySum, whole.sum - mix(members[0]), { }, &SetWithSum::sum)) {
            if (std::ranges::equal(membersOf(candidate.set), members.subspan(1))) {
                result[whole.set] = LayoutSetID(candidate.set);
                break;
            }
        }
    }
    return result;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
