/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <cstddef>
#include <cstdint>
#include <wtf/Assertions.h>

namespace JSC::AOT {

struct OptionalIndex {
    uint32_t bits { UINT32_MAX };

    constexpr explicit operator bool() const { return bits != UINT32_MAX; }
    constexpr uint32_t operator*() const
    {
        RELEASE_ASSERT_UNDER_CONSTEXPR_CONTEXT(bits != UINT32_MAX);
        return bits;
    }

    friend constexpr bool operator==(OptionalIndex, OptionalIndex) = default;
};

// Every member is a site number of the function.
struct SpreadSite {
    uint32_t callSite;
    uint32_t item;
    uint32_t site;
};

struct PrologueCheck {
    uint32_t function { 0 };
    uint32_t callOffset { 0 };
    uint32_t resumeOffset { 0 };

    friend constexpr bool operator==(const PrologueCheck&, const PrologueCheck&) = default;
};

#if CPU(X86_64) && OS(LINUX)
static constexpr size_t sizeOfLoopPoll = 7;
#else
static constexpr size_t sizeOfLoopPoll = 0;
#endif
static constexpr bool hasFaultingLoopPolls = !!sizeOfLoopPoll;
static constexpr ptrdiff_t offsetOfLeafPollInLoopPollPage = 4;

struct PollSite {
    uint32_t offset { 0 };
    bool callerIsFrameless { false };
    OptionalIndex callSite { };

    constexpr uint32_t returnOffset() const { return offset + static_cast<uint32_t>(sizeOfLoopPoll); }
    constexpr ptrdiff_t offsetInLoopPollPage() const { return callerIsFrameless ? offsetOfLeafPollInLoopPollPage : 0; }
};

static_assert(std::is_trivially_copyable_v<PrologueCheck> && sizeof(PrologueCheck) == 12);
static_assert(std::is_trivially_copyable_v<PollSite> && sizeof(PollSite) == 12);
static_assert(!(offsetOfLeafPollInLoopPollPage % sizeof(uint32_t)));
static_assert(PollSite { 16, false }.returnOffset() == 16 + sizeOfLoopPoll && !PollSite { 16, false }.offsetInLoopPollPage());
static_assert(PollSite { 16, true }.offsetInLoopPollPage() == offsetOfLeafPollInLoopPollPage);

} // namespace JSC::AOT

#endif // ENABLE(AOT)
