/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTSchemaEnums.h"
#include <wtf/OptionSet.h>

namespace JSC { namespace AOT { namespace Typed {

template<typename Flag>
constexpr OptionSet<Flag> everyFlagOf()
{
    OptionSet<Flag> flags;
    for (Flag flag : allMembersOf<Flag>)
        flags.add(flag);
    return flags;
}

inline constexpr OptionSet<ObjectMutation> allObjectMutations = everyFlagOf<ObjectMutation>();

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
