/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTSchemaEnums.h"
#include "YarrFlags.h"
#include <wtf/OptionSet.h>

namespace JSC { namespace AOT { namespace Typed {

constexpr Yarr::Flags toYarrFlag(RegExpFlags flag)
{
    switch (flag) {
    case RegExpFlags::HasIndices:
        return Yarr::Flags::HasIndices;
    case RegExpFlags::Global:
        return Yarr::Flags::Global;
    case RegExpFlags::IgnoreCase:
        return Yarr::Flags::IgnoreCase;
    case RegExpFlags::Multiline:
        return Yarr::Flags::Multiline;
    case RegExpFlags::DotAll:
        return Yarr::Flags::DotAll;
    case RegExpFlags::Unicode:
        return Yarr::Flags::Unicode;
    case RegExpFlags::UnicodeSets:
        return Yarr::Flags::UnicodeSets;
    case RegExpFlags::Sticky:
        return Yarr::Flags::Sticky;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

inline OptionSet<Yarr::Flags> toYarrFlags(OptionSet<RegExpFlags> flags)
{
    OptionSet<Yarr::Flags> result;
    for (RegExpFlags flag : flags)
        result.add(toYarrFlag(flag));
    return result;
}

inline constexpr OptionSet<RegExpFlags> flagsOfMatchesUsingObject { RegExpFlags::Global, RegExpFlags::Sticky, RegExpFlags::HasIndices };

inline constexpr uint32_t privateRegExpParameter = 1u << 8;
inline constexpr uint32_t expectedRegExpParameter = 1u << 9;

inline constexpr uint32_t countedMatchesParameter = 1u << 31;

inline constexpr unsigned longestRegExpPatternWithCode = 1048576;

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
