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
    bool isShort { false };
};

struct X86JumpShift {
    uint32_t from;
    uint32_t shift;
};

inline const X86EmittedJump& findEmittedJump(std::span<const X86EmittedJump> emittedJumps, uint32_t from)
{
    auto found = std::ranges::lower_bound(emittedJumps, from, { }, &X86EmittedJump::from);
    RELEASE_ASSERT(found != emittedJumps.end() && found->from == from);
    return *found;
}

inline void assignX86JumpKinds(Vector<X86LinkedJump>& linkedJumps, std::span<const X86EmittedJump> emittedJumps)
{
    std::ranges::sort(linkedJumps, { }, &X86LinkedJump::from);
    size_t next = 0;
    for (X86LinkedJump& jump : linkedJumps) {
        while (next < emittedJumps.size() && emittedJumps[next].from < jump.from)
            ++next;
        RELEASE_ASSERT(next < emittedJumps.size() && emittedJumps[next].from == jump.from);
        jump.kind = emittedJumps[next].kind;
        jump.condition = emittedJumps[next].condition;
    }
}

inline uint32_t shortenedJumpShiftAt(std::span<const X86JumpShift> shifts, uint32_t offset)
{
    auto begin = shifts.begin();
    auto found = std::upper_bound(begin, shifts.end(), offset, [](uint32_t offset, const X86JumpShift& entry) {
        return offset < entry.from;
    });
    return found == begin ? 0 : found[-1].shift;
}

struct X86JumpShortener {
    static constexpr uint8_t shortJumpOpcode = 0xeb;
    static constexpr uint8_t shortConditionalJumpOpcode = 0x70;
    static constexpr uint8_t shortJumpLength = 2;
    static constexpr int64_t shortJumpReach = 128;
    static constexpr int64_t maxShrinkFactor = 3;
    static constexpr int64_t maxCandidateDistance = maxShrinkFactor * shortJumpReach;
    static constexpr unsigned shiftIndexGranularityShift = 6;

    static constexpr uint8_t longJumpLength(X86JumpKind kind)
    {
        switch (kind) {
        case X86JumpKind::Unconditional:
            return 5;
        case X86JumpKind::Conditional:
            return 6;
        }
        RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
    }

    static constexpr uint8_t savingsOf(X86JumpKind kind)
    {
        return longJumpLength(kind) - shortJumpLength;
    }

    static constexpr bool fitsShortJump(int64_t distance)
    {
        return distance >= -shortJumpReach && distance < shortJumpReach;
    }

    size_t run()
    {
        ASSERT(shifts.isEmpty());
        collectCandidates();
        findShortJumps();
        size_t newSize = size;
        if (shiftBefore[count]) {
            computeShifts();
            relinkLongJumps();
            newSize = compactCode();
            finalizeJumps();
        }
        std::ranges::sort(jumps, { }, &X86LinkedJump::from);
        return newSize;
    }

    void collectCandidates()
    {
        uint32_t all = jumps.size();
        for (uint32_t i = 0; i < all; ++i) {
            jumps[i].isShort = false;
            X86LinkedJump jump = jumps[i];
            RELEASE_ASSERT(jump.from >= longJumpLength(jump.kind) && jump.from <= size && jump.to <= size);
            uint32_t distance = jump.to > jump.from ? jump.to - jump.from : jump.from - jump.to;
            if (distance > maxCandidateDistance)
                continue;
            jumps[i] = jumps[count];
            jumps[count++] = jump;
        }
        std::ranges::sort(jumps.mutableSpan().first(count), { }, &X86LinkedJump::from);
        numberOfJumpsUpToTarget.fill(0, count);
        shiftBefore.fill(0, count + 1);
        undecided.fill(0, count);
        saving.fill(0, count);
        isShort.fill(0, count);
        for (uint32_t i = 0; i < count; ++i) {
            RELEASE_ASSERT(!i || jumps[i].from >= jumps[i - 1].from + longJumpLength(jumps[i].kind));
            saving[i] = savingsOf(jumps[i].kind);
            isShort[i] = 1;
            uint32_t to = jumps[i].to;
            uint32_t upTo = i + 1;
            if (to >= jumps[i].from) {
                while (upTo < count && jumps[upTo].from <= to)
                    ++upTo;
            } else {
                while (upTo && jumps[upTo - 1].from > to)
                    --upTo;
            }
            numberOfJumpsUpToTarget[i] = upTo;
        }
    }

    void sumUp()
    {
        uint32_t sum = 0;
        for (uint32_t i = 0; i < count; ++i) {
            shiftBefore[i] = sum;
            if (isShort[i])
                sum += saving[i];
        }
        shiftBefore[count] = sum;
    }

    int64_t displacement(uint32_t i, bool asShort) const
    {
        uint32_t own = asShort ? saving[i] : 0;
        int64_t from = static_cast<int64_t>(jumps[i].from) - shiftBefore[i] - own;
        uint32_t upTo = numberOfJumpsUpToTarget[i];
        int64_t to = static_cast<int64_t>(jumps[i].to) - shiftBefore[upTo] - (upTo > i && !isShort[i] ? own : 0);
        return to - from;
    }

    void findShortJumps()
    {
        sumUp();
        uint32_t undecidedCount = 0;
        for (uint32_t i = 0; i < count; ++i) {
            int64_t nearest = displacement(i, true);
            if (fitsShortJump(nearest))
                undecided[undecidedCount++] = i;
        }
        for (uint32_t i = 0; i < count; ++i)
            isShort[i] = 0;
        for (bool changed = undecidedCount; changed;) {
            changed = false;
            sumUp();
            uint32_t left = 0;
            for (uint32_t k = 0; k < undecidedCount; ++k) {
                uint32_t i = undecided[k];
                int64_t distance = displacement(i, true);
                if (!fitsShortJump(distance)) {
                    undecided[left++] = i;
                    continue;
                }
                isShort[i] = 1;
                changed = true;
            }
            undecidedCount = left;
        }
        sumUp();
    }

    void computeShifts()
    {
        for (uint32_t i = 0; i < count; ++i) {
            if (isShort[i])
                shifts.append(X86JumpShift { jumps[i].from, shiftBefore[i + 1] });
        }
        uint32_t numberOfShifts = shifts.size();
        uint32_t numberOfStretches = (size >> shiftIndexGranularityShift) + 2;
        firstShiftAfter.fill(0, numberOfStretches);
        for (uint32_t stretch = 0, next = 0; stretch < numberOfStretches; ++stretch) {
            while (next < numberOfShifts && shifts[next].from < stretch << shiftIndexGranularityShift)
                ++next;
            firstShiftAfter[stretch] = next;
        }
    }

    uint32_t shiftAt(uint32_t offset) const
    {
        uint32_t numberOfShifts = shifts.size();
        uint32_t next = firstShiftAfter[offset >> shiftIndexGranularityShift];
        while (next < numberOfShifts && shifts[next].from <= offset)
            ++next;
        return next ? shifts[next - 1].shift : 0;
    }

    void setDisplacement(uint32_t from, int64_t distance)
    {
        int32_t narrow = static_cast<int32_t>(distance);
        memcpy(code + from - 4, &narrow, 4);
    }

    void relinkLongJumps()
    {
        for (uint32_t i = 0; i < count; ++i) {
            if (!isShort[i])
                setDisplacement(jumps[i].from, displacement(i, false));
        }
        uint32_t all = jumps.size();
        for (uint32_t i = count; i < all; ++i) {
            X86LinkedJump jump = jumps[i];
            setDisplacement(jump.from, (static_cast<int64_t>(jump.to) - shiftAt(jump.to)) - (static_cast<int64_t>(jump.from) - shiftAt(jump.from)));
        }
    }

    size_t compactCode()
    {
        size_t in = 0;
        size_t out = 0;
        for (uint32_t i = 0; i < count; ++i) {
            if (!isShort[i])
                continue;
            uint32_t from = jumps[i].from;
            size_t start = from - longJumpLength(jumps[i].kind);
            if (out != in)
                memmove(code + out, code + in, start - in);
            out += start - in;
            int64_t distance = displacement(i, true);
            code[out] = jumps[i].kind == X86JumpKind::Conditional ? shortConditionalJumpOpcode | jumps[i].condition : shortJumpOpcode;
            code[out + 1] = static_cast<uint8_t>(static_cast<int8_t>(distance));
            out += shortJumpLength;
            in = from;
        }
        memmove(code + out, code + in, size - in);
        return out + size - in;
    }

    void finalizeJumps()
    {
        for (uint32_t i = 0; i < count; ++i) {
            jumps[i].to -= shiftBefore[numberOfJumpsUpToTarget[i]];
            jumps[i].from -= shiftBefore[i + 1];
            jumps[i].isShort = isShort[i];
        }
        uint32_t all = jumps.size();
        for (uint32_t i = count; i < all; ++i) {
            jumps[i].to -= shiftAt(jumps[i].to);
            jumps[i].from -= shiftAt(jumps[i].from);
        }
    }

    uint8_t* code;
    size_t size;
    Vector<X86LinkedJump>& jumps;
    Vector<X86JumpShift>& shifts;
    uint32_t count { 0 };
    Vector<uint32_t> numberOfJumpsUpToTarget { };
    Vector<uint32_t> shiftBefore { };
    Vector<uint32_t> undecided { };
    Vector<uint32_t> firstShiftAfter { };
    Vector<uint8_t> saving { };
    Vector<uint8_t> isShort { };
};

static_assert(X86JumpShortener::maxShrinkFactor * X86JumpShortener::shortJumpLength == X86JumpShortener::longJumpLength(X86JumpKind::Conditional));

inline size_t shortenX86Jumps(std::span<uint8_t> code, Vector<X86LinkedJump>& jumps, Vector<X86JumpShift>& shifts)
{
    return X86JumpShortener { code.data(), code.size(), jumps, shifts }.run();
}

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(ASSEMBLER) && CPU(X86_64)
