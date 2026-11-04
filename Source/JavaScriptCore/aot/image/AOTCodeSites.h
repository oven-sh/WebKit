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

struct PrologueCheck {
    uint32_t function { 0 };
    uint32_t callOffset { 0 };
    uint32_t resumeOffset { 0 };

    friend constexpr bool operator==(const PrologueCheck&, const PrologueCheck&) = default;
};

#if CPU(X86_64) && (OS(LINUX) || OS(DARWIN) || OS(WINDOWS))
#define AOT_LOOP_POLLS_FAULT 1
static constexpr size_t sizeOfLoopPoll = 7;
#elif CPU(ARM64) && (OS(LINUX) || OS(DARWIN) || OS(WINDOWS)) && !defined(BUN_HARDENED)
// A hardened instance has the words that are written once where the page would have to lie: InstanceLayout::offsetOfLoopPollPage().
#define AOT_LOOP_POLLS_FAULT 1
static constexpr size_t sizeOfLoopPoll = 4;
#else
#define AOT_LOOP_POLLS_FAULT 0
static constexpr size_t sizeOfLoopPoll = 0;
#endif
static constexpr bool hasFaultingLoopPolls = !!sizeOfLoopPoll;

// "Is it a cell?" in front of a read of the word of its shape is left to the hardware. A boxed int32 or double is no address that the processor takes, and undefined, null, true, false and the empty value lie in the first page,
// which is never mapped: the read faults, and the handler gives it zero, the word of a cell without a shape, which every test that holdsForAnyCell() refuses. A fault costs microseconds: only where no such value is expected.
// Not on ARM64: the top byte of an address is ignored there, so a boxed double may name memory that is mapped.
// NOWHERE, since 11-04: what is expected is what is written, and data breaks that: numbers out of JSON.parse where objects are written cost a fault a read, a hundred times and more what the test costs, as often as whoever sends them likes,
// and nothing here learns: no code is patched, and whatever stood in front of the read to ask would be the test. The test is asked.
#define AOT_READS_OFF_NO_CELL_FAULT 0
static constexpr bool readsOffNoCellFault = !!AOT_READS_OFF_NO_CELL_FAULT;
// The prefix that names the data segment, which changes nothing in 64-bit mode and which the assembler writes nowhere else. It stands in front of an instruction that may fault so: any other fault is as loud as it was.
static constexpr uint8_t markOfReadOffNoCell = 0x3e;
static_assert(hasFaultingLoopPolls == !!AOT_LOOP_POLLS_FAULT);
#if CPU(ARM64)
// The load is of 64 bits: its immediate offset reaches twice as far as that of a load of 32 bits.
static constexpr size_t sizeOfWordLoadedByLoopPoll = sizeof(uint64_t);
#else
static constexpr size_t sizeOfWordLoadedByLoopPoll = sizeof(uint32_t);
#endif
static constexpr ptrdiff_t offsetOfLeafPollInLoopPollPage = sizeOfWordLoadedByLoopPoll;
// The handler of the fault tells a poll that has left its handler by the address it loads.
static constexpr ptrdiff_t offsetOfPollLeavingOwedErrorInLoopPollPage = 2 * sizeOfWordLoadedByLoopPoll;

struct PollSite {
    uint32_t offset { 0 };
    bool callerIsFrameless { false };
    OptionalIndex callSite { };

    constexpr uint32_t returnOffset() const { return offset + static_cast<uint32_t>(sizeOfLoopPoll); }
    constexpr ptrdiff_t offsetInLoopPollPage() const { return callerIsFrameless ? offsetOfLeafPollInLoopPollPage : 0; }
};

static_assert(std::is_trivially_copyable_v<PrologueCheck> && sizeof(PrologueCheck) == 12);
static_assert(std::is_trivially_copyable_v<PollSite> && sizeof(PollSite) == 12);
static_assert(!(offsetOfLeafPollInLoopPollPage % sizeOfWordLoadedByLoopPoll) && !(offsetOfPollLeavingOwedErrorInLoopPollPage % sizeOfWordLoadedByLoopPoll));
static_assert(PollSite { 16, false }.returnOffset() == 16 + sizeOfLoopPoll && !PollSite { 16, false }.offsetInLoopPollPage());
static_assert(PollSite { 16, true }.offsetInLoopPollPage() == offsetOfLeafPollInLoopPollPage);

} // namespace JSC::AOT

#endif // ENABLE(AOT)
