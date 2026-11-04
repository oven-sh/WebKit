/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTControlEmitter.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include <algorithm>
#include <array>
#include <bit>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint64_t bitsOfConstantInUnionSwitchEmitter = 64;
// By the number of ways, less one. The tests of bits are a difference, a compare and a branch for the range, and a constant, a test and a branch for each way. The cases one by one are a compare and a branch each.
static constexpr std::array<size_t, 3> leastCasesForBitsInUnionSwitchEmitter { 3, 5, 6 };
static_assert(defaultLimitValue(LimitID::MaxBitTestTargets, hostArchitecture()) <= leastCasesForBitsInUnionSwitchEmitter.size());

namespace {

// The cases that lead to one block of the machine. An edge that hands something over has a block of its own: CodeGenerator::edgeTo().
struct WayOfUnionSwitchEmitter {
    BlockEdge edge;
    // Bit n: the case `first + n` leads there.
    uint64_t bits { 0 };
};

} // anonymous namespace

// `kind === 1 || kind === 4 || kind === 9`: the cases lead to few blocks and lie within 64 numbers of one another.
bool ControlEmitter::trySwitchByBitsOfConstants(LValue subject, LType type, std::span<const SwitchTarget> targets, BlockEdge otherwise)
{
    RELEASE_ASSERT(!targets.empty() && subject->type() == type);
    int64_t first = std::ranges::min(targets, { }, &SwitchTarget::caseValue).caseValue;
    int64_t last = std::ranges::max(targets, { }, &SwitchTarget::caseValue).caseValue;
    uint64_t greatestDistance = static_cast<uint64_t>(last) - static_cast<uint64_t>(first);
    if (greatestDistance >= bitsOfConstantInUnionSwitchEmitter)
        return false;

    uint64_t mostWays = std::min<uint64_t>(build().limit(LimitID::MaxBitTestTargets), leastCasesForBitsInUnionSwitchEmitter.size());
    Vector<WayOfUnionSwitchEmitter, 3> ways;
    uint64_t bitsSeen = 0;
    size_t numberOfCasesTested = 0;
    for (const SwitchTarget& target : targets) {
        uint64_t bit = 1ull << (static_cast<uint64_t>(target.caseValue) - static_cast<uint64_t>(first));
        // Of a case that stands twice the first wins.
        if (std::exchange(bitsSeen, bitsSeen | bit) & bit)
            continue;
        // It goes where whatever is no case goes, so nobody asks for it.
        if (target.target.block() == otherwise.block())
            continue;
        ++numberOfCasesTested;
        size_t place = ways.findIf([&](const WayOfUnionSwitchEmitter& way) {
            return way.edge.block() == target.target.block();
        });
        if (place == notFound) {
            if (ways.size() == mostWays)
                return false;
            place = ways.size();
            ways.append(WayOfUnionSwitchEmitter { target.target, 0 });
        }
        ways[place].bits |= bit;
    }
    if (ways.isEmpty()) {
        m_out.jump(otherwise);
        return true;
    }
    if (numberOfCasesTested < leastCasesForBitsInUnionSwitchEmitter[ways.size() - 1])
        return false;

    // The way with the most cases is asked for first.
    std::ranges::stable_sort(ways, std::ranges::greater { }, [](const WayOfUnionSwitchEmitter& way) {
        return std::popcount(way.bits);
    });

    LValue distance = first ? m_out.sub(subject, m_out.constantOfTypeOf(subject, first)) : subject;
    LValue liesInRange = m_out.belowOrEqual(distance, m_out.constantOfTypeOf(distance, static_cast<int64_t>(greatestDistance)));
    // `case 1: case 2: case 3:`: whatever lies in the range goes the one way.
    uint64_t everyBitOfRange = UINT64_MAX >> (bitsOfConstantInUnionSwitchEmitter - 1 - greatestDistance);
    if (ways.size() == 1 && ways[0].bits == everyBitOfRange) {
        m_out.branch(liesInRange, ways[0].edge, otherwise);
        return true;
    }
    LBasicBlock isInRange = newBlock();
    m_out.branch(liesInRange, normalEdge(isInRange), otherwise);
    m_out.appendTo(isInRange);

    // As ShapeTestEmitter::passes() asks ImmediateBits: on x86-64 the shift and the test of the lowest bit are one `bt`.
    LValue amount = m_out.castToInt32(distance);
    for (size_t index = 0; index < ways.size(); ++index) {
        const WayOfUnionSwitchEmitter& way = ways[index];
        LValue shifted = greatestDistance < 32
            ? m_out.lShr(m_out.constInt32(static_cast<int32_t>(static_cast<uint32_t>(way.bits))), amount)
            : m_out.castToInt32(m_out.lShr(m_out.constInt64(static_cast<int64_t>(way.bits)), amount));
        BlockEdge next = otherwise;
        LBasicBlock asksNextWay = nullptr;
        if (index + 1 < ways.size()) {
            asksNextWay = newBlock();
            next = normalEdge(asksNextWay);
        }
        m_out.branch(m_out.testNonZero32(shifted, m_out.int32One), way.edge, next);
        if (asksNextWay)
            m_out.appendTo(asksNextWay);
    }
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
