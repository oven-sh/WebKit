/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFunctionDescription.h"
#include "AOTImage.h"
#include <array>
#include <bit>
#include <optional>
#include <wtf/FixedVector.h>
#include <wtf/Noncopyable.h>
#include <wtf/TZoneMalloc.h>

namespace JSC::AOT {

class CallSiteCache {
    WTF_MAKE_TZONE_ALLOCATED(CallSiteCache);
    WTF_MAKE_NONCOPYABLE(CallSiteCache);
public:
    CallSiteCache();

    struct Entry {
        std::optional<unsigned> shownFrameCountFor(bool hasShownFrame, bool calleeWasEntered) const;

        const void* address { nullptr };
        CompiledFunctionID code;
        CallSiteRecord site { CallSiteIndex(0), 0, false, false, std::nullopt };
        bool innermostIsInlinedConstructCall { false };
        bool rejectsOnThrow { false };
        TraceVisibility innermostVisibility { TraceVisibility::Visible };
        FunctionID innermostFunction;
        uint16_t shownFrameCount { 0 };
        uint16_t shownFrameCountOnTop { 0 };
        std::optional<HandlerRecord> handler;
        ImageFrame frame { };
    };

    ALWAYS_INLINE const Entry* find(const void* returnAddress) const;
    const Entry* resolve(const Image&, const void* returnAddress);
    std::optional<SourcePosition> positionOf(const Image&, CompiledFunctionID, CallSiteIndex);

    static constexpr unsigned numberOfEntries = 512;
    static constexpr unsigned numberOfPositions = 16;

private:
    static size_t primaryIndex(const void* address) { return (std::bit_cast<uintptr_t>(address) >> 2) % numberOfEntries; }
    static size_t secondaryIndex(const void* address)
    {
        static_assert(numberOfEntries == 1 << 9);
        return std::bit_cast<uintptr_t>(address) * 0x9e3779b97f4a7c15ull >> (64 - 9);
    }

    struct Position {
        CompiledFunctionID code;
        CallSiteIndex callSite;
        SourcePosition position;
    };

    FixedVector<Entry> m_entries;
    bool m_evictsSecondary { false };
    InlineFrameReader m_inlineFrames;
    std::array<Position, numberOfPositions> m_positions;
};

ALWAYS_INLINE const CallSiteCache::Entry* CallSiteCache::find(const void* returnAddress) const
{
    if (!returnAddress || m_entries.isEmpty()) [[unlikely]]
        return nullptr;
    const Entry& primary = m_entries[primaryIndex(returnAddress)];
    if (primary.address == returnAddress) [[likely]]
        return &primary;
    const Entry& secondary = m_entries[secondaryIndex(returnAddress)];
    return secondary.address == returnAddress ? &secondary : nullptr;
}

} // namespace JSC::AOT

#endif // ENABLE(AOT)
