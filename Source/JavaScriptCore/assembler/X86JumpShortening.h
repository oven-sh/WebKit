/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <wtf/Platform.h>

#if ENABLE(ASSEMBLER) && CPU(X86_64)

#include <algorithm>
#include <span>
#include <wtf/Vector.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

enum class X86JumpKind : uint8_t { Unconditional, Conditional };

// from is the offset of the byte after the jump, like the label that jmp() and jCC() return.
struct X86EmittedJump {
    uint32_t from;
    X86JumpKind kind;
    uint8_t condition;
};

struct X86LinkedJump {
    uint32_t from;
    uint32_t to;
    X86JumpKind kind { X86JumpKind::Unconditional };
    uint8_t condition { 0 };
    uint8_t savings { 0 };
    uint8_t lengthOfReturn { 0 };
};

struct X86ShortReturn {
    uint32_t start;
    uint32_t length;
};

struct X86JumpShift {
    uint32_t from;
    uint32_t shift;
};

inline constexpr unsigned x86JumpShiftStretchShift = 5;

inline const X86EmittedJump& findEmittedJump(std::span<const X86EmittedJump> emittedJumps, uint32_t from)
{
    auto found = std::ranges::lower_bound(emittedJumps, from, { }, &X86EmittedJump::from);
    RELEASE_ASSERT(found != emittedJumps.end() && found->from == from);
    return *found;
}

inline void collectX86LinkedJumps(std::span<const uint8_t> code, std::span<const X86EmittedJump> emittedJumps, Vector<uint32_t>& jumpsToNextInstruction, Vector<X86LinkedJump>& linkedJumps)
{
    ASSERT(linkedJumps.isEmpty());
    if (!std::ranges::is_sorted(jumpsToNextInstruction))
        std::ranges::sort(jumpsToNextInstruction);
    linkedJumps.reserveCapacity(emittedJumps.size());
    const uint8_t* const bytes = code.data();
    const uint32_t* nextListed = jumpsToNextInstruction.begin();
    const uint32_t* const endListed = jumpsToNextInstruction.end();
    for (const X86EmittedJump& jump : emittedJumps) {
        ASSERT(jump.from >= sizeof(uint32_t) && jump.from <= code.size());
        uint32_t distance;
        memcpy(&distance, bytes + jump.from - sizeof(uint32_t), sizeof(uint32_t));
        if (!distance) {
            if (nextListed == endListed || *nextListed != jump.from)
                continue;
            ++nextListed;
        }
        linkedJumps.append(X86LinkedJump { jump.from, jump.from + distance, jump.kind, jump.condition });
    }
    RELEASE_ASSERT(nextListed == endListed);
}

template<size_t inlineCapacity>
inline void indexX86JumpShifts(std::span<const X86JumpShift> shifts, Vector<uint32_t, inlineCapacity>& firstShiftByStretch)
{
    ASSERT(firstShiftByStretch.isEmpty());
    if (shifts.empty())
        return;
    firstShiftByStretch.grow((shifts.back().from >> x86JumpShiftStretchShift) + 1);
    uint32_t* const firstShift = firstShiftByStretch.mutableSpan().data();
    const X86JumpShift* const entries = shifts.data();
    const uint32_t entryCount = static_cast<uint32_t>(shifts.size());
    uint32_t stretch = 0;
    for (uint32_t i = 0; i < entryCount; ++i) {
        for (uint32_t last = entries[i].from >> x86JumpShiftStretchShift; stretch <= last; ++stretch)
            firstShift[stretch] = i;
    }
}

inline uint32_t shortenedJumpShiftAt(std::span<const X86JumpShift> shifts, std::span<const uint32_t> firstShiftByStretch, uint32_t offset)
{
    size_t stretch = offset >> x86JumpShiftStretchShift;
    if (stretch >= firstShiftByStretch.size())
        return shifts.empty() ? 0 : shifts.back().shift;
    const X86JumpShift* const entries = shifts.data();
    size_t index = firstShiftByStretch.data()[stretch];
    while (index < shifts.size() && entries[index].from <= offset)
        ++index;
    return index ? entries[index - 1].shift : 0;
}

struct X86JumpShortener {
    static constexpr uint8_t shortJumpOpcode = 0xeb;
    static constexpr uint8_t shortConditionalJumpOpcode = 0x70;
    static constexpr uint8_t shortJumpLength = 2;
    static constexpr int64_t shortJumpReach = 128;
    static constexpr int64_t maxShrinkFactor = 3;
    static constexpr int64_t maxCandidateDistance = maxShrinkFactor * shortJumpReach;
    static constexpr int64_t maxSavings = 4;
    static constexpr unsigned stretchShift = 4;
    static constexpr unsigned chainIndexBits = 27;
    static constexpr unsigned chainRankBits = 5;
    static constexpr uint32_t noJump = UINT32_MAX;
    static constexpr size_t inlineJumpCapacity = 256;

    struct UndecidedJump {
        uint32_t index;
        uint32_t targetIndex;
        int16_t distance;
        uint16_t savings;
    };

    struct WideWord {
        uint64_t low;
        uint64_t high;
    };

    static constexpr uint8_t longJumpLength(X86JumpKind kind)
    {
        return kind == X86JumpKind::Conditional ? 6 : 5;
    }

    static constexpr uint8_t savingsOf(X86JumpKind kind)
    {
        return longJumpLength(kind) - shortJumpLength;
    }

    static constexpr uint8_t savingsOfReturn(uint8_t lengthOfReturn)
    {
        return longJumpLength(X86JumpKind::Unconditional) - lengthOfReturn;
    }

    static constexpr bool fitsShortJump(int64_t distance)
    {
        return distance >= -shortJumpReach && distance < shortJumpReach;
    }

    size_t run()
    {
        ASSERT(shifts.isEmpty());
        count = static_cast<uint32_t>(jumps.size());
        if (!count)
            return size;

        jumpData = jumps.mutableSpan().data();
        firstJumpByStretch.grow((size >> stretchShift) + 1);
        targetIndex.grow(count);
        shiftBefore.grow(count + 1);
        appliedSavings.grow(count);
        undecided.grow(count);
        if (!indexJumps()) {
            std::ranges::sort(jumps, { }, &X86LinkedJump::from);
            RELEASE_ASSERT(indexJumps());
        }
        if (shortReturns.empty())
            classifyJumps<false>();
        else
            classifyJumps<true>();
        findShortJumps();
        if (replaceLongJumpsWithReturns()) {
            dropDecidedJumps();
            sumUpFrom(firstCopiedReturn);
            findShortJumps();
        }
        if (chainJumpsIntoRareCode()) {
            dropDecidedJumps();
            sumUpFrom(firstChainedJump);
            findShortJumps();
        }
        if (!shiftBefore[count] && firstCopiedReturn == UINT32_MAX) {
            for (X86LinkedJump& jump : jumps)
                jump.savings = 0;
            return size;
        }
        return firstCopiedReturn == UINT32_MAX ? compactCode<false>() : compactCode<true>();
    }

    static uint32_t startOf(const X86LinkedJump& jump)
    {
        return jump.from - longJumpLength(jump.kind);
    }

    // By findShortJumps().
    bool staysLong(uint32_t i) const
    {
        return !appliedSavings.span().data()[i] && !jumpData[i].lengthOfReturn;
    }

    uint8_t returnLengthAt(uint32_t to) const
    {
        auto found = std::ranges::lower_bound(shortReturns, to, { }, &X86ShortReturn::start);
        return found != shortReturns.end() && found->start == to ? static_cast<uint8_t>(found->length) : 0;
    }

    void replaceJumpWithReturn(uint32_t index, uint8_t lengthOfReturn)
    {
        jumpData[index].lengthOfReturn = lengthOfReturn;
        appliedSavings.mutableSpan().data()[index] = savingsOfReturn(lengthOfReturn);
        firstCopiedReturn = std::min(firstCopiedReturn, index);
    }

    bool replaceLongJumpsWithReturns()
    {
        bool hasCopied = false;
        for (auto [index, lengthOfReturn] : jumpsToShortReturns) {
            if (!staysLong(index))
                continue;
            replaceJumpWithReturn(index, lengthOfReturn);
            hasCopied = true;
        }
        return hasCopied;
    }

    // Whether the jump, once it is short, reaches the first byte of the other as the code stands. Shortening only brings the two nearer.
    static bool reachesStartOf(const X86LinkedJump& jump, const X86LinkedJump& other)
    {
        int64_t target = startOf(other);
        if (target >= jump.from)
            return target - jump.from < shortJumpReach;
        return static_cast<int64_t>(startOf(jump)) + shortJumpLength - target <= shortJumpReach;
    }

    void retargetToJump(uint32_t index, uint32_t relay)
    {
        X86LinkedJump& jump = jumpData[index];
        jump.to = startOf(jumpData[relay]);
        targetIndex.mutableSpan().data()[index] = relay;
        appliedSavings.mutableSpan().data()[index] = savingsOf(jump.kind);
        firstChainedJump = std::min(firstChainedJump, index);
    }

    // Of the jumps to one target the unconditional ones come first: any jump may go through them.
    static uint64_t chainKeyOf(const X86LinkedJump& jump, uint32_t index)
    {
        ASSERT(jump.condition + 1 < 1 << chainRankBits);
        uint64_t rank = jump.kind == X86JumpKind::Conditional ? jump.condition + 1 : 0;
        return static_cast<uint64_t>(jump.to) << (chainRankBits + chainIndexBits) | rank << chainIndexBits | index;
    }

    static uint32_t chainIndexOf(uint64_t key)
    {
        return static_cast<uint32_t>(key) & ((1u << chainIndexBits) - 1);
    }

    // A jump leaves the flags as they are. So a jump that is seldom taken and stays long may go to a jump nearby that has its target, and its condition or none.
    // That relay goes to the target itself, so no way leads through two. Whatever was short stays short: the code between a jump and its target only shrinks.
    bool chainJumpsIntoRareCode()
    {
        if (startOfRareCode >= size)
            return false;

        RELEASE_ASSERT(count <= 1u << chainIndexBits);
        chainKeys.grow(count);
        uint64_t* const keys = chainKeys.mutableSpan().data();
        const X86LinkedJump* const data = jumpData;
        const uint8_t* const applied = appliedSavings.span().data();
        const uint32_t jumpCount = count;
        const uint32_t rareCode = startOfRareCode;
        size_t keyCount = 0;
        uint64_t previousKey = 0;
        bool someStayLong = false;
        bool isAscending = true;
        for (uint32_t i = 0; i < jumpCount; ++i) {
            if (data[i].to < rareCode)
                continue;
            uint64_t key = chainKeyOf(data[i], i);
            isAscending &= previousKey <= key;
            previousKey = key;
            keys[keyCount++] = key;
            someStayLong |= !applied[i];
        }
        if (!someStayLong)
            return false;
        if (!isAscending)
            std::sort(keys, keys + keyCount);

        bool hasChained = false;
        std::span<const uint64_t> all { keys, keyCount };
        for (size_t begin = 0; begin < all.size();) {
            const uint64_t target = all[begin] >> (chainRankBits + chainIndexBits);
            std::span<const uint64_t> unconditionalJumps;
            while (begin < all.size() && all[begin] >> (chainRankBits + chainIndexBits) == target) {
                const uint64_t targetAndRank = all[begin] >> chainIndexBits;
                size_t end = begin + 1;
                while (end < all.size() && all[end] >> chainIndexBits == targetAndRank)
                    ++end;
                std::span<const uint64_t> sameCondition = all.subspan(begin, end - begin);
                if (sameCondition.size() > 1 || !unconditionalJumps.empty())
                    hasChained |= chainJumpsWithSameCondition(sameCondition, static_cast<uint32_t>(target), unconditionalJumps);
                bool isConditional = targetAndRank & ((1u << chainRankBits) - 1);
                if (!isConditional)
                    unconditionalJumps = sameCondition;
                begin = end;
            }
        }
        return hasChained;
    }

    // The nearest jump of the list on either side that still goes to the target and lies within reach.
    uint32_t findJumpToChainThrough(std::span<const uint64_t> ascending, uint32_t target, uint32_t index) const
    {
        if (ascending.empty())
            return noJump;
        const X86LinkedJump* const data = jumpData;
        const X86LinkedJump& jump = data[index];
        auto ahead = std::ranges::lower_bound(ascending, index, { }, chainIndexOf);
        for (auto candidate = ahead; candidate != ascending.end() && reachesStartOf(jump, data[chainIndexOf(*candidate)]); ++candidate) {
            if (data[chainIndexOf(*candidate)].to == target)
                return chainIndexOf(*candidate);
        }
        for (auto candidate = ahead; candidate != ascending.begin() && reachesStartOf(jump, data[chainIndexOf(*(candidate - 1))]); --candidate) {
            if (data[chainIndexOf(*(candidate - 1))].to == target)
                return chainIndexOf(*(candidate - 1));
        }
        return noJump;
    }

    // They have one target and one condition, and ascend.
    bool chainJumpsWithSameCondition(std::span<const uint64_t> sameCondition, uint32_t target, std::span<const uint64_t> unconditionalJumps)
    {
        const X86LinkedJump* const data = jumpData;
        bool hasChained = false;
        // The last that is known to go to the target itself. It may stand ahead of the place of the scan.
        size_t relay = sameCondition.size();
        for (size_t place = 0; place < sameCondition.size(); ++place) {
            uint32_t index = chainIndexOf(sameCondition[place]);
            const X86LinkedJump& jump = data[index];
            if (place == relay)
                continue;
            if (!staysLong(index)) {
                if (relay == sameCondition.size() || relay < place)
                    relay = place;
                continue;
            }
            if (uint32_t found = findJumpToChainThrough(unconditionalJumps, target, index); found != noJump) {
                retargetToJump(index, found);
                hasChained = true;
                continue;
            }
            if (relay == sameCondition.size() || !reachesStartOf(jump, data[chainIndexOf(sameCondition[relay])])) {
                relay = place;
                for (size_t ahead = place + 1; ahead < sameCondition.size() && reachesStartOf(jump, data[chainIndexOf(sameCondition[ahead])]); ++ahead)
                    relay = ahead;
                if (relay == place)
                    continue;
            }
            retargetToJump(index, chainIndexOf(sameCondition[relay]));
            hasChained = true;
        }
        return hasChained;
    }

    bool indexJumps()
    {
        const X86LinkedJump* const data = jumpData;
        uint32_t* const firstJump = firstJumpByStretch.mutableSpan().data();
        const uint32_t jumpCount = count;
        const size_t codeSize = size;
        size_t stretch = 0;
        uint32_t end = 0;
        for (uint32_t i = 0; i < jumpCount; ++i) {
            uint32_t from = data[i].from;
            if (from < end)
                return false;
            RELEASE_ASSERT(from - end >= longJumpLength(data[i].kind) && from <= codeSize && data[i].to <= codeSize);
            end = from;
            for (size_t last = from >> stretchShift; stretch <= last; ++stretch)
                firstJump[stretch] = i;
        }
        for (size_t last = codeSize >> stretchShift; stretch <= last; ++stretch)
            firstJump[stretch] = jumpCount;
        return true;
    }

    uint64_t shortReturnFilter() const
    {
        uint64_t filter = 0;
        for (const X86ShortReturn& shortReturn : shortReturns)
            filter |= uint64_t { 1 } << shortReturn.start % 64;
        return filter;
    }

    template<bool hasShortReturns>
    void classifyJumps()
    {
        const X86LinkedJump* const data = jumpData;
        const uint32_t* const firstJump = firstJumpByStretch.span().data();
        uint32_t* const targets = targetIndex.mutableSpan().data();
        uint32_t* const sums = shiftBefore.mutableSpan().data();
        uint8_t* const applied = appliedSavings.mutableSpan().data();
        UndecidedJump* const firstUndecided = undecided.mutableSpan().data();
        UndecidedJump* nextUndecided = firstUndecided;
        const uint32_t jumpCount = count;
        const uint64_t filter = hasShortReturns ? shortReturnFilter() : 0;
        uint32_t sum = 0;
        for (uint32_t i = 0; i < jumpCount; ++i) {
            uint32_t to = data[i].to;
            uint8_t savings = savingsOf(data[i].kind);
            int64_t distance = static_cast<int64_t>(to) - static_cast<int64_t>(data[i].from);
            uint32_t target = firstJump[to >> stretchShift];
            while (target < jumpCount && data[target].from <= to)
                ++target;
            targets[i] = target;
            sums[i] = sum;
            if constexpr (hasShortReturns) {
                if (filter >> to % 64 & 1 && data[i].kind == X86JumpKind::Unconditional) [[unlikely]] {
                    uint8_t lengthOfReturn = returnLengthAt(to);
                    if (lengthOfReturn && lengthOfReturn <= shortJumpLength) {
                        replaceJumpWithReturn(i, lengthOfReturn);
                        sum += applied[i];
                        continue;
                    }
                    if (lengthOfReturn)
                        jumpsToShortReturns.append({ i, lengthOfReturn });
                }
            }
            if (distance >= 0) {
                if (distance < shortJumpReach) {
                    applied[i] = savings;
                    sum += savings;
                    continue;
                }
                applied[i] = 0;
                if (distance > maxCandidateDistance || distance - maxSavings * (static_cast<int64_t>(target) - i - 1) >= shortJumpReach)
                    continue;
            } else {
                bool isCandidate = distance >= -maxCandidateDistance;
                distance += savings;
                if (distance >= -shortJumpReach) {
                    applied[i] = savings;
                    sum += savings;
                    continue;
                }
                applied[i] = 0;
                if (!isCandidate || distance + maxSavings * (static_cast<int64_t>(i) - target) < -shortJumpReach)
                    continue;
            }
            *nextUndecided++ = UndecidedJump { i, target, static_cast<int16_t>(distance), savings };
        }
        sums[jumpCount] = sum;
        undecidedCount = static_cast<uint32_t>(nextUndecided - firstUndecided);
    }

    void sumUpFrom(uint32_t first)
    {
        uint32_t* const sums = shiftBefore.mutableSpan().data();
        const uint8_t* const applied = appliedSavings.span().data();
        const uint32_t jumpCount = count;
        uint32_t sum = sums[first];
        for (uint32_t i = first; i < jumpCount; ++i) {
            sum += applied[i];
            sums[i + 1] = sum;
        }
    }

    void dropDecidedJumps()
    {
        UndecidedJump* const list = undecided.mutableSpan().data();
        const uint8_t* const applied = appliedSavings.span().data();
        uint32_t kept = 0;
        for (uint32_t k = 0; k < undecidedCount; ++k) {
            if (!applied[list[k].index] && !jumpData[list[k].index].lengthOfReturn)
                list[kept++] = list[k];
        }
        undecidedCount = kept;
    }

    void findShortJumps()
    {
        UndecidedJump* const list = undecided.mutableSpan().data();
        const uint32_t* const sums = shiftBefore.span().data();
        uint8_t* const applied = appliedSavings.mutableSpan().data();
        uint32_t left = undecidedCount;
        while (left) {
            uint32_t kept = 0;
            uint32_t firstChanged = count;
            for (uint32_t k = 0; k < left; ++k) {
                UndecidedJump jump = list[k];
                int64_t distance = static_cast<int64_t>(jump.distance) + sums[jump.index] - sums[jump.targetIndex];
                if (!fitsShortJump(distance)) {
                    list[kept++] = jump;
                    continue;
                }
                applied[jump.index] = static_cast<uint8_t>(jump.savings);
                firstChanged = std::min(firstChanged, jump.index);
            }
            if (kept == left)
                break;
            left = kept;
            sumUpFrom(firstChanged);
        }
        undecidedCount = left;
    }

    template<typename Word>
    static ALWAYS_INLINE void moveFirstAndLastWord(uint8_t* destination, const uint8_t* source, size_t length)
    {
        Word first;
        Word last;
        memcpy(&first, source, sizeof(Word));
        memcpy(&last, source + length - sizeof(Word), sizeof(Word));
        memcpy(destination, &first, sizeof(Word));
        memcpy(destination + length - sizeof(Word), &last, sizeof(Word));
    }

    static ALWAYS_INLINE void moveCode(uint8_t* destination, const uint8_t* source, size_t length)
    {
        if (length > 2 * sizeof(WideWord)) {
            memmove(destination, source, length);
            return;
        }
        if (length >= sizeof(WideWord))
            moveFirstAndLastWord<WideWord>(destination, source, length);
        else if (length >= sizeof(uint64_t))
            moveFirstAndLastWord<uint64_t>(destination, source, length);
        else if (length >= sizeof(uint32_t))
            moveFirstAndLastWord<uint32_t>(destination, source, length);
        else if (length >= sizeof(uint16_t))
            moveFirstAndLastWord<uint16_t>(destination, source, length);
        else if (length)
            *destination = *source;
    }

    template<bool hasCopiedReturns>
    size_t compactCode()
    {
        shifts.grow(count);
        X86JumpShift* const firstShift = shifts.mutableSpan().data();
        X86JumpShift* nextShift = firstShift;
        X86LinkedJump* const data = jumpData;
        const uint32_t* const targets = targetIndex.span().data();
        const uint32_t* const sums = shiftBefore.span().data();
        const uint8_t* const applied = appliedSavings.span().data();
        uint8_t* const bytes = code;
        const uint32_t jumpCount = count;
        size_t in = 0;
        size_t out = 0;
        for (uint32_t i = 0; i < jumpCount; ++i) {
            X86LinkedJump jump = data[i];
            uint32_t shift = sums[i + 1];
            uint32_t from = jump.from - shift;
            uint32_t to = jump.to - sums[targets[i]];
            uint32_t distance = to - from;
            const uint8_t savings = applied[i];
            const uint8_t lengthOfReturn = hasCopiedReturns ? jump.lengthOfReturn : 0;
            data[i] = X86LinkedJump { from, to, jump.kind, jump.condition, savings, lengthOfReturn };
            if (!savings && !lengthOfReturn) {
                memcpy(bytes + jump.from - sizeof(uint32_t), &distance, sizeof(uint32_t));
                continue;
            }
            size_t start = jump.from - longJumpLength(jump.kind);
            if (out != in)
                moveCode(bytes + out, bytes + in, start - in);
            out += start - in;
            in = jump.from;
            if (savings || !hasCopiedReturns)
                *nextShift++ = X86JumpShift { jump.from, shift };
            if (lengthOfReturn) [[unlikely]] {
                memcpy(bytes + out, bytes + (jump.to < jump.from ? to : jump.to), lengthOfReturn);
                out += lengthOfReturn;
                continue;
            }
            bytes[out] = jump.kind == X86JumpKind::Conditional ? shortConditionalJumpOpcode | jump.condition : shortJumpOpcode;
            bytes[out + 1] = static_cast<uint8_t>(distance);
            out += shortJumpLength;
        }
        memmove(bytes + out, bytes + in, size - in);
        shifts.shrink(static_cast<size_t>(nextShift - firstShift));
        return out + size - in;
    }

    uint8_t* code;
    size_t size;
    Vector<X86LinkedJump>& jumps;
    Vector<X86JumpShift>& shifts;
    // A jump to this offset or behind it is seldom taken.
    uint32_t startOfRareCode { UINT32_MAX };
    std::span<const X86ShortReturn> shortReturns { };
    X86LinkedJump* jumpData { nullptr };
    uint32_t count { 0 };
    uint32_t undecidedCount { 0 };
    uint32_t firstChainedJump { UINT32_MAX };
    uint32_t firstCopiedReturn { UINT32_MAX };
    Vector<uint32_t, inlineJumpCapacity> firstJumpByStretch { };
    Vector<uint32_t, inlineJumpCapacity> targetIndex { };
    Vector<uint32_t, inlineJumpCapacity + 1> shiftBefore { };
    Vector<uint8_t, inlineJumpCapacity> appliedSavings { };
    Vector<UndecidedJump, inlineJumpCapacity> undecided { };
    Vector<std::pair<uint32_t, uint8_t>, 8> jumpsToShortReturns { };
    Vector<uint64_t, inlineJumpCapacity> chainKeys { };
};

static_assert(X86JumpShortener::maxShrinkFactor * X86JumpShortener::shortJumpLength == X86JumpShortener::longJumpLength(X86JumpKind::Conditional));
static_assert(X86JumpShortener::maxSavings == std::max(X86JumpShortener::savingsOf(X86JumpKind::Conditional), X86JumpShortener::savingsOf(X86JumpKind::Unconditional)));
static_assert(X86JumpShortener::maxCandidateDistance + X86JumpShortener::maxSavings <= INT16_MAX);

inline size_t shortenX86Jumps(std::span<uint8_t> code, Vector<X86LinkedJump>& jumps, Vector<X86JumpShift>& shifts, uint32_t startOfRareCode = UINT32_MAX, std::span<const X86ShortReturn> shortReturns = { })
{
    return X86JumpShortener { code.data(), code.size(), jumps, shifts, startOfRareCode, shortReturns }.run();
}

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(ASSEMBLER) && CPU(X86_64)
