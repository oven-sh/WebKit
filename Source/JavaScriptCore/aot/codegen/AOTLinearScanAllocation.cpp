/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLinearScanInternals.h"

#if ENABLE(AOT) && (CPU(X86_64) || CPU(ARM64)) && !OS(WINDOWS)

#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

using namespace LinearScan;

namespace {

constexpr Position longestClobber = 8;

inline uint64_t keyOf(Position from, uint32_t interval) { return static_cast<uint64_t>(from) << 32 | interval; }

} // anonymous namespace

void LinearScanCodeGenerator::computeLiveness()
{
    constexpr size_t maxWords = 1 << 20;
    m_liveWords = (m_globals.size() + 63) / 64;
    size_t total = m_liveWords * m_blocks.size();
    if (total > maxWords) {
        refuse();
        return;
    }
    m_liveIn.fill(0, total);
    if (!total)
        return;
    Vector<uint64_t, 16> live;
    live.fill(0, m_liveWords);
    for (bool changed = true; changed;) {
        changed = false;
        for (size_t index = m_reversePostOrder.size(); index--;) {
            uint32_t order = m_reversePostOrder[index];
            const Block& block = m_blocks[order];
            if (!block.isReachable)
                continue;
            for (size_t word = 0; word < m_liveWords; ++word)
                live[word] = 0;
            for (unsigned successor = 0; successor < block.numberOfSuccessors; ++successor) {
                if (block.onlySuccessor && block.onlySuccessor != successor + 1)
                    continue;
                const uint64_t* other = m_liveIn.span().data() + static_cast<size_t>(successorOf(block, successor)) * m_liveWords;
                for (size_t word = 0; word < m_liveWords; ++word)
                    live[word] |= other[word];
            }
            for (uint32_t vreg = block.firstDefinedGlobal; vreg; vreg = m_virtualRegisters[vreg].nextDefinedInBlock) {
                uint32_t global = m_virtualRegisters[vreg].global - 1;
                live[global / 64] &= ~(static_cast<uint64_t>(1) << (global % 64));
            }
            for (uint32_t use = 0; use < block.numberOfExposedUses; ++use) {
                uint32_t global = m_exposedUses[block.firstExposedUse + use];
                live[global / 64] |= static_cast<uint64_t>(1) << (global % 64);
            }
            uint64_t* known = m_liveIn.mutableSpan().data() + static_cast<size_t>(order) * m_liveWords;
            for (size_t word = 0; word < m_liveWords; ++word) {
                if (live[word] & ~known[word]) {
                    known[word] |= live[word];
                    changed = true;
                }
            }
        }
        if (!m_hasCycles)
            break;
    }
}

uint32_t LinearScanCodeGenerator::newInterval(uint32_t vreg, uint32_t region, bool isDouble)
{
    Interval interval;
    interval.vreg = vreg;
    interval.region = region;
    interval.isDouble = isDouble;
    m_intervals.append(interval);
    return m_intervals.size() - 1;
}

uint32_t LinearScanCodeGenerator::intervalFor(uint32_t vreg, uint32_t region)
{
    VirtualRegister& v = m_virtualRegisters[vreg];
    if (v.currentRegion == region)
        return v.currentInterval;
    uint32_t index = newInterval(vreg, region, v.isDouble);
    if (v.firstInterval)
        v.hasSeveralIntervals = true;
    m_intervals[index].nextSplit = v.firstInterval;
    v.firstInterval = index;
    v.currentInterval = index;
    v.currentRegion = region;
    return index;
}

void LinearScanCodeGenerator::addRange(uint32_t index, Position from, Position to)
{
    Interval& interval = m_intervals[index];
    if (interval.firstRange) {
        LiveRange& first = m_ranges[interval.firstRange - 1];
        if (first.from <= to) {
            first.from = std::min(first.from, from);
            first.to = std::max(first.to, to);
            interval.from = first.from;
            interval.to = std::max(interval.to, first.to);
            return;
        }
    }
    m_ranges.append(LiveRange { from, to, interval.firstRange });
    interval.firstRange = m_ranges.size();
    interval.from = from;
    interval.to = std::max(interval.to, to);
}

void LinearScanCodeGenerator::addUse(uint32_t index, Position position, UseKind kind, uint32_t operand, bool isLate)
{
    Interval& interval = m_intervals[index];
    uint32_t* link = &interval.firstUse;
    while (*link && m_uses[*link - 1].position < position)
        link = &m_uses[*link - 1].next;
    uint32_t next = *link;
    uint32_t created = m_uses.size() + 1;
    *link = created;
    m_uses.append(UsePosition { position, operand, next, kind, isLate });
}

void LinearScanCodeGenerator::buildIntervals()
{
    m_intervals.shrink(0);
    m_intervals.append(Interval());
    Vector<uint64_t, 16> live;
    live.fill(0, m_liveWords);
    Vector<uint64_t, 16> reachesOn;
    reachesOn.fill(0, m_liveWords);
    const uint64_t* liveInOfBlockBehind = nullptr;
    Position startOfBlockBehind = 0;
    uint32_t regionOfBlockBehind = 0;

    auto regionOf = [&](const VirtualRegister& v, uint32_t order, const Block& block) {
        return v.isRematerialized ? static_cast<uint32_t>(m_blocks.size()) + order : block.region;
    };
    auto endRangeAt = [&](size_t global, Position start) {
        Interval& interval = m_intervals[m_virtualRegisters[m_globals[global]].currentInterval];
        m_ranges[interval.firstRange - 1].from = start;
        interval.from = start;
    };

    for (uint32_t order = m_blocks.size(); order--;) {
        const Block& block = m_blocks[order];
        if (!block.isReachable)
            continue;
        Position from = block.from();
        Position to = block.to();
        if (m_liveWords) {
            for (size_t word = 0; word < m_liveWords; ++word)
                live[word] = 0;
            for (unsigned successor = 0; successor < block.numberOfSuccessors; ++successor) {
                if (block.onlySuccessor && block.onlySuccessor != successor + 1)
                    continue;
                const uint64_t* other = m_liveIn.span().data() + static_cast<size_t>(successorOf(block, successor)) * m_liveWords;
                for (size_t word = 0; word < m_liveWords; ++word)
                    live[word] |= other[word];
            }
            bool continuesBlockBehind = liveInOfBlockBehind && regionOfBlockBehind == block.region && startOfBlockBehind == to;
            for (size_t word = 0; word < m_liveWords; ++word) {
                uint64_t open = liveInOfBlockBehind ? liveInOfBlockBehind[word] : 0;
                uint64_t carried = continuesBlockBehind ? open & live[word] : 0;
                for (uint64_t bits = open & ~carried; bits; bits &= bits - 1)
                    endRangeAt(word * 64 + std::countr_zero(bits), startOfBlockBehind);
                for (uint64_t bits = live[word] & ~carried; bits; bits &= bits - 1)
                    addRange(intervalFor(m_globals[word * 64 + std::countr_zero(bits)], block.region), from, to);
                reachesOn[word] = live[word];
            }
            liveInOfBlockBehind = m_liveIn.span().data() + static_cast<size_t>(order) * m_liveWords;
            startOfBlockBehind = from;
            regionOfBlockBehind = block.region;
        }

        auto addUseOf = [&](uint32_t operandIndex, Position end, Position position, UseKind kind, bool isLate = false) {
            const Operand& operand = m_operands[operandIndex];
            uint32_t vreg = node(operand.node).vreg;
            const VirtualRegister& v = m_virtualRegisters[vreg];
            uint32_t interval = intervalFor(vreg, regionOf(v, order, block));
            if (v.isRematerialized && m_intervals[interval].firstRange)
                end = std::max(end, m_ranges[m_intervals[interval].firstRange - 1].from);
            if (!v.global || !(reachesOn[(v.global - 1) / 64] >> ((v.global - 1) % 64) & 1))
                addRange(interval, v.isRematerialized ? position & ~1u : from, end);
            addUse(interval, position, kind, operandIndex, isLate);
        };

        for (uint32_t record = block.firstUpsilon; record; record = m_upsilons[record - 1].next) {
            const Node& upsilon = node(m_upsilons[record - 1].upsilon);
            if (upsilon.form != Form::Plain || !upsilon.numberOfOperands || m_operands[upsilon.firstOperand].kind != OperandKind::EndUse)
                continue;
            addUseOf(upsilon.firstOperand, to, to - 1, UseKind::Any);
        }

        for (uint32_t slot = block.endSlot; slot-- > block.firstSlot;) {
            const Node& n = node(m_sequence[slot]);
            if (n.form != Form::Plain || !n.numberOfOperands)
                continue;
            Position argumentPosition = argumentPositionOf(slot);
            Position position = mainPositionOf(slot);
            uint32_t first = n.firstOperand;
            uint32_t end = first + n.numberOfOperands;
            for (uint32_t index = first; index < end; ++index) {
                const Operand& operand = m_operands[index];
                Position definition;
                UseKind kind = UseKind::Register;
                switch (operand.kind) {
                case OperandKind::Definition:
                    definition = position + 1;
                    break;
                case OperandKind::EarlyDefinition:
                    definition = position;
                    break;
                case OperandKind::BlockDefinition:
                    definition = from;
                    if (!operand.fixed)
                        kind = UseKind::Any;
                    break;
                case OperandKind::Temporary: {
                    uint32_t interval = newInterval(0, block.region, operand.flags & IsDoubleOperand);
                    addRange(interval, position, position + 2);
                    addUse(interval, position, UseKind::Register, index);
                    continue;
                }
                default:
                    continue;
                }
                uint32_t vreg = node(operand.node).vreg;
                uint32_t interval = intervalFor(vreg, block.region);
                VirtualRegister& v = m_virtualRegisters[vreg];
                Interval& defined = m_intervals[interval];
                if (!defined.firstRange)
                    addRange(interval, definition, definition + 1);
                else {
                    m_ranges[defined.firstRange - 1].from = definition;
                    defined.from = definition;
                }
                v.definition = definition;
                v.fixedDefinition = operand.fixed;
                addUse(interval, definition, kind, index);
            }
            for (uint32_t index = first; index < end; ++index) {
                switch (m_operands[index].kind) {
                case OperandKind::Use:
                    addUseOf(index, position + 1, position, UseKind::Register);
                    break;
                case OperandKind::LateUse:
                    addUseOf(index, position + 2, position, UseKind::Register, true);
                    break;
                case OperandKind::UseOrSlot:
                    addUseOf(index, position + 1, position, UseKind::RegisterOrSlot);
                    break;
                case OperandKind::LateUseOrSlot:
                    addUseOf(index, position + 2, position, UseKind::RegisterOrSlot, true);
                    break;
                case OperandKind::AnyUse:
                    addUseOf(index, position + 1, position, UseKind::Any);
                    break;
                case OperandKind::LateAnyUse:
                    addUseOf(index, position + 2, position, UseKind::Any, true);
                    break;
                default:
                    break;
                }
            }
            for (uint32_t index = first; index < end; ++index) {
                if (m_operands[index].kind == OperandKind::ArgumentUse)
                    addUseOf(index, argumentPosition + 1, argumentPosition, UseKind::Any);
            }
        }
    }

    if (liveInOfBlockBehind) {
        for (size_t word = 0; word < m_liveWords; ++word) {
            for (uint64_t bits = liveInOfBlockBehind[word]; bits; bits &= bits - 1)
                endRangeAt(word * 64 + std::countr_zero(bits), startOfBlockBehind);
        }
    }

    std::ranges::sort(m_clobbers, { }, &ClobberEvent::from);
    for (const ClobberEvent& event : m_clobbers)
        m_everClobbered |= event.registers;
    std::ranges::sort(m_probes);
}

bool LinearScanCodeGenerator::isBlockStart(Position position) const
{
    if (position & 3)
        return false;
    uint32_t slot = slotOf(position);
    if (slot >= m_blockOfSlot.size())
        return true;
    return m_blocks[m_blockOfSlot[slot]].firstSlot == slot;
}

bool LinearScanCodeGenerator::covers(const Interval& interval, Position position) const
{
    for (uint32_t range = interval.firstRange; range; range = m_ranges[range - 1].next) {
        if (position < m_ranges[range - 1].from)
            return false;
        if (position < m_ranges[range - 1].to)
            return true;
    }
    return false;
}

Position LinearScanCodeGenerator::nextIntersection(const Interval& first, const Interval& second) const
{
    uint32_t a = first.currentRange ? first.currentRange : first.firstRange;
    uint32_t b = second.currentRange ? second.currentRange : second.firstRange;
    while (a && b) {
        const LiveRange& rangeA = m_ranges[a - 1];
        const LiveRange& rangeB = m_ranges[b - 1];
        if (rangeA.to <= rangeB.from)
            a = rangeA.next;
        else if (rangeB.to <= rangeA.from)
            b = rangeB.next;
        else
            return std::max(rangeA.from, rangeB.from);
    }
    return noPosition;
}

Position LinearScanCodeGenerator::firstUseFrom(const Interval& interval, Position position, UseKind least) const
{
    for (uint32_t use = interval.firstUse; use; use = m_uses[use - 1].next) {
        if (m_uses[use - 1].position >= position && m_uses[use - 1].kind >= least)
            return m_uses[use - 1].position;
    }
    return noPosition;
}

Position LinearScanCodeGenerator::lastUseBefore(const Interval& interval, Position position, UseKind least) const
{
    Position result = noPosition;
    for (uint32_t use = interval.firstUse; use && m_uses[use - 1].position < position; use = m_uses[use - 1].next) {
        if (m_uses[use - 1].kind >= least)
            result = m_uses[use - 1].position;
    }
    return result;
}

void LinearScanCodeGenerator::enqueue(uint32_t interval)
{
    m_unhandled.append(keyOf(m_intervals[interval].from, interval));
    std::push_heap(m_unhandled.begin(), m_unhandled.end(), std::greater<uint64_t> { });
}

uint32_t LinearScanCodeGenerator::splitInterval(uint32_t index, Position position, bool overlaps)
{
    uint32_t created = newInterval(m_intervals[index].vreg, m_intervals[index].region, m_intervals[index].isDouble);
    Interval& parent = m_intervals[index];
    Interval& child = m_intervals[created];
    RELEASE_ASSERT(parent.from < position && position < parent.to);
    child.isSplitChild = true;
    child.nextSplit = parent.nextSplit;
    parent.nextSplit = created;
    child.to = parent.to;

    uint32_t previous = 0;
    uint32_t range = parent.firstRange;
    while (m_ranges[range - 1].to <= position) {
        previous = range;
        range = m_ranges[range - 1].next;
    }
    if (overlaps && m_ranges[range - 1].to <= position + 1) {
        overlaps = false;
        previous = range;
        range = m_ranges[range - 1].next;
        ++position;
    }
    if (m_ranges[range - 1].from >= position) {
        child.firstRange = range;
        m_ranges[previous - 1].next = 0;
        parent.to = m_ranges[previous - 1].to;
    } else {
        LiveRange tail { position, m_ranges[range - 1].to, m_ranges[range - 1].next };
        m_ranges.append(tail);
        child.firstRange = m_ranges.size();
        m_ranges[range - 1].to = position + overlaps;
        m_ranges[range - 1].next = 0;
        parent.to = position + overlaps;
    }
    child.from = m_ranges[child.firstRange - 1].from;
    child.currentRange = child.firstRange;
    if (parent.currentRange) {
        bool isKept = false;
        for (uint32_t kept = parent.firstRange; kept; kept = m_ranges[kept - 1].next)
            isKept |= kept == parent.currentRange;
        if (!isKept)
            parent.currentRange = 0;
    }

    uint32_t* link = &parent.firstUse;
    while (*link && m_uses[*link - 1].position < position + overlaps)
        link = &m_uses[*link - 1].next;
    child.firstUse = *link;
    *link = 0;

    if (parent.vreg)
        m_virtualRegisters[parent.vreg].hasSeveralIntervals = true;
    return created;
}

Position LinearScanCodeGenerator::findSplitPosition(Position least, Position most, bool prefersEarly) const
{
    constexpr unsigned maxBlocksLookedAt = 48;
    most &= ~1u;
    least = (least + 1) & ~1u;
    if (least >= most)
        return most;
    auto costOfBoundary = [&](uint32_t order) {
        double frequency = m_blocks[order].frequency;
        while (order--) {
            if (m_blocks[order].isReachable)
                return std::min(frequency, m_blocks[order].frequency);
        }
        return frequency;
    };
    auto costOf = [&](Position position) {
        uint32_t order = blockAt(position);
        return slotOf(position) < m_blockOfSlot.size() && isBlockStart(position) ? costOfBoundary(order) : m_blocks[order].frequency;
    };
    uint32_t first = blockAt(least);
    uint32_t last = blockAt(most);
    Position best = prefersEarly ? least : most;
    if (first == last || m_blocks[first].isRare)
        return best;
    double bestFrequency = costOf(best);
    Position other = prefersEarly ? most : least;
    double otherFrequency = costOf(other);
    if (last - first > maxBlocksLookedAt) {
        if (prefersEarly)
            last = first + maxBlocksLookedAt;
        else
            first = last - maxBlocksLookedAt;
    }
    auto consider = [&](uint32_t order) {
        const Block& block = m_blocks[order];
        if (!block.isReachable || block.from() <= least || block.from() >= most)
            return;
        double cost = costOfBoundary(order);
        if (cost < bestFrequency) {
            best = block.from();
            bestFrequency = cost;
        }
    };
    if (prefersEarly) {
        for (uint32_t order = first + 1; order <= last; ++order)
            consider(order);
    } else {
        for (uint32_t order = last; order > first; --order)
            consider(order);
    }
    if (otherFrequency < bestFrequency)
        best = other;
    return best;
}

Position LinearScanCodeGenerator::earliestDeparture(const Interval& interval, Position limit, bool& overlaps) const
{
    overlaps = false;
    const UsePosition* last = nullptr;
    for (uint32_t use = interval.firstUse; use && m_uses[use - 1].position < limit; use = m_uses[use - 1].next) {
        if (m_uses[use - 1].kind >= UseKind::RegisterOrSlot)
            last = &m_uses[use - 1];
    }
    if (!last)
        return (interval.from | 1) + 1;
    if (!last->isLate && !(last->position & 1) && last->position > interval.from && !isBlockStart(last->position)) {
        overlaps = true;
        return last->position;
    }
    return (last->position | 1) + 1;
}

void LinearScanCodeGenerator::splitWhereRegisterIsLost(uint32_t index, Position limit)
{
    bool overlaps;
    Position earliest = earliestDeparture(m_intervals[index], limit, overlaps);
    Position most = limit & ~1u;
    Position position = earliest >= most ? most : findSplitPosition(earliest, most, true);
    if (position >= m_intervals[index].to)
        return;
    if (position <= m_intervals[index].from) {
        fail();
        return;
    }
    enqueue(splitInterval(index, position, overlaps && position == earliest));
}

void LinearScanCodeGenerator::splitBeforeUse(uint32_t index, Position least, Position most)
{
    const Interval& interval = m_intervals[index];
    least = std::max(least, interval.from + 1);
    Position position = findSplitPosition(least, most);
    if (position >= interval.to)
        return;
    if (position <= interval.from) {
        fail();
        return;
    }
    enqueue(splitInterval(index, position));
}

void LinearScanCodeGenerator::assignSlot(uint32_t index, Position position)
{
    Interval& interval = m_intervals[index];
    if (!interval.vreg) {
        fail();
        return;
    }
    VirtualRegister& v = m_virtualRegisters[interval.vreg];
    interval.reg = 0;
    if (v.isRematerialized) {
        interval.place = Place::Nowhere;
        return;
    }
    interval.place = Place::Slot;
    v.needsSlot = true;
    ++v.numberOfSpills;
    v.spillFrequency += static_cast<float>(frequencyAt(position));
    if (v.spillState == SpillState::StoredAtDefinition)
        return;
    bool storesAtDefinition = m_prefersSmallCode ? v.numberOfSpills >= 2 || blockAt(v.definition) == blockAt(position) : v.spillFrequency >= frequencyAt(v.definition);
    if (node(v.node).has(IsTerminal))
        storesAtDefinition = false;
    v.spillState = storesAtDefinition ? SpillState::StoredAtDefinition : SpillState::StoredWhereSpilled;
}

void LinearScanCodeGenerator::spillTail(uint32_t index)
{
    Position most = m_currentPosition & ~1u;
    const Interval& interval = m_intervals[index];
    if (lastUseBefore(interval, most, UseKind::RegisterOrSlot) == noPosition && !(interval.from & 1)) {
        assignSlot(index, interval.from);
        return;
    }
    bool overlaps;
    Position earliest = earliestDeparture(interval, most, overlaps);
    if (earliest > most) {
        fail();
        return;
    }
    Position position = findSplitPosition(earliest, most, true);
    if (position <= interval.from || position >= interval.to) {
        fail();
        return;
    }
    assignSlot(splitInterval(index, position, overlaps && position == earliest), position);
}

void LinearScanCodeGenerator::splitAndSpill(uint32_t index, bool isActive)
{
    Position base = m_currentPosition & ~1u;
    if (!isActive) {
        if (base + 2 < m_intervals[index].to && base + 2 > m_intervals[index].from)
            enqueue(splitInterval(index, base + 2));
        return;
    }
    Position nextUse = firstUseFrom(m_intervals[index], base, UseKind::Register);
    if (nextUse != noPosition && nextUse < m_intervals[index].to)
        splitBeforeUse(index, base + 2, nextUse);
    spillTail(index);
}

uint64_t LinearScanCodeGenerator::candidatesFor(const Interval& interval) const
{
    if (interval.isDouble)
        return m_doubleRegisters;
    uint64_t calleeSaves = interval.region || !interval.vreg ? m_usedCalleeSaves & m_allocatableCalleeSaves : m_allocatableCalleeSaves;
    calleeSaves &= ~m_calleeSavesMeantForValues;
    if (interval.vreg && m_virtualRegisters[interval.vreg].calleeSaveMeantForIt)
        calleeSaves |= static_cast<uint64_t>(1) << (m_virtualRegisters[interval.vreg].calleeSaveMeantForIt - 1);
    return m_generalRegisters | calleeSaves;
}

uint8_t LinearScanCodeGenerator::locationAt(uint32_t vreg, Position position) const
{
    for (uint32_t index = m_virtualRegisters[vreg].firstInterval; index; index = m_intervals[index].nextSplit) {
        const Interval& interval = m_intervals[index];
        if (position < interval.from)
            return 0;
        if (position < interval.to && covers(interval, position))
            return interval.location();
    }
    return 0;
}

unsigned LinearScanCodeGenerator::collectHints(const Interval& interval, std::array<uint8_t, 6>& hints) const
{
    unsigned count = 0;
    if (!interval.vreg)
        return 0;
    auto add = [&](uint8_t location) {
        if (!isRegisterLocation(location) || count == hints.size())
            return;
        for (unsigned index = 0; index < count; ++index) {
            if (hints[index] == location)
                return;
        }
        hints[count++] = location;
    };
    const VirtualRegister& v = m_virtualRegisters[interval.vreg];
    bool continuesValue = interval.from != v.definition;
    add(v.calleeSaveMeantForIt);
    if (continuesValue && isBlockStart(interval.from)) {
        const Block& block = m_blocks[blockAt(interval.from)];
        for (unsigned index = 0; index < block.numberOfPredecessors && index < 4; ++index) {
            const Block& predecessor = m_blocks[m_predecessors[block.firstPredecessor + index]];
            if (predecessor.isReachable && predecessor.to() <= interval.from)
                add(locationAt(interval.vreg, predecessor.to() - 1));
        }
    }
    if (!continuesValue && node(v.node).opcode == B3::Phi && isBlockStart(interval.from)) {
        std::array<std::pair<uint8_t, double>, 4> arrivals { };
        forEachSourceOf(v.node, [&](NodeID source, uint32_t sender) {
            if (node(source).form != Form::Plain || !node(source).vreg || m_blocks[sender].to() > interval.from)
                return;
            uint8_t location = locationAt(node(source).vreg, m_blocks[sender].to() - 1);
            if (!isRegisterLocation(location))
                return;
            for (auto& arrival : arrivals) {
                if (!arrival.first || arrival.first == location) {
                    arrival.first = location;
                    arrival.second += m_blocks[sender].frequency;
                    break;
                }
            }
        });
        std::ranges::stable_sort(arrivals, std::ranges::greater { }, [](const auto& arrival) { return arrival.second; });
        for (const auto& arrival : arrivals)
            add(arrival.first);
    }
    if (v.targetVirtualRegister) {
        uint32_t target = m_virtualRegisters[v.targetVirtualRegister].firstInterval;
        if (target && m_intervals[target].place == Place::Register && m_intervals[target].from < interval.from)
            add(m_intervals[target].reg);
    }
    unsigned usesLookedAt = 0;
    for (uint32_t use = interval.firstUse; use && usesLookedAt < 4; use = m_uses[use - 1].next, ++usesLookedAt) {
        const Operand& operand = m_operands[m_uses[use - 1].operand];
        bool isDefinition = operand.kind == OperandKind::Definition || operand.kind == OperandKind::EarlyDefinition || operand.kind == OperandKind::BlockDefinition;
        if (!isDefinition)
            add(operand.fixed);
    }
    if (continuesValue) {
        for (uint32_t index = v.firstInterval; index && m_intervals[index].from < interval.from; index = m_intervals[index].nextSplit) {
            if (m_intervals[index].place == Place::Register && m_intervals[index].region == interval.region)
                add(m_intervals[index].reg);
        }
        add(v.hint);
        return count;
    }
    if (v.hintVirtualRegister) {
        const VirtualRegister& other = m_virtualRegisters[v.hintVirtualRegister];
        if (interval.from)
            add(locationAt(v.hintVirtualRegister, interval.from - 1));
        if (other.firstInterval && m_intervals[other.firstInterval].place == Place::Register)
            add(m_intervals[other.firstInterval].reg);
        add(other.hint);
    }
    add(v.hint);
    return count;
}

void LinearScanCodeGenerator::limitByClobbers(const Interval& interval, uint64_t candidates, std::array<Position, numberOfRegisters>& limits, uint64_t definedHere) const
{
    uint64_t remaining = candidates & m_everClobbered;
    for (uint32_t range = interval.currentRange ? interval.currentRange : interval.firstRange; range && remaining; range = m_ranges[range - 1].next) {
        const LiveRange& covered = m_ranges[range - 1];
        Position searched = covered.from > longestClobber ? covered.from - longestClobber : 0;
        auto found = std::ranges::lower_bound(m_clobbers, searched, { }, &ClobberEvent::from);
        for (; found != m_clobbers.end() && found->from < covered.to && remaining; ++found) {
            if (found->to <= covered.from)
                continue;
            uint64_t hit = found->registers & remaining;
            if (found->from <= interval.from && interval.from < found->to)
                hit &= ~definedHere;
            if (!hit)
                continue;
            Position at = std::max(found->from, covered.from);
            for (uint64_t bits = hit; bits; bits &= bits - 1) {
                Position& limit = limits[std::countr_zero(bits)];
                limit = std::min(limit, at);
            }
            remaining &= ~hit;
        }
    }
}

void LinearScanCodeGenerator::advanceTo(Position position)
{
    auto advance = [&](Interval& interval) {
        while (interval.currentRange && m_ranges[interval.currentRange - 1].to <= position)
            interval.currentRange = m_ranges[interval.currentRange - 1].next;
    };
    if (position >= m_nextChangeOfActive) {
        Position next = noPosition;
        for (uint64_t bits = m_activeRegisters; bits; bits &= bits - 1) {
            unsigned reg = std::countr_zero(bits);
            uint32_t index = m_activeInRegister[reg];
            Interval& interval = m_intervals[index];
            advance(interval);
            if (interval.currentRange && m_ranges[interval.currentRange - 1].from <= position) {
                next = std::min(next, m_ranges[interval.currentRange - 1].to);
                continue;
            }
            m_activeRegisters &= ~(static_cast<uint64_t>(1) << reg);
            m_activeInRegister[reg] = 0;
            if (interval.currentRange)
                m_inactive.append(index);
        }
        m_nextChangeOfActive = next;
    }
    for (size_t index = 0; index < m_inactive.size();) {
        Interval& interval = m_intervals[m_inactive[index]];
        advance(interval);
        if (interval.currentRange && m_ranges[interval.currentRange - 1].from > position) {
            ++index;
            continue;
        }
        if (interval.currentRange)
            activate(m_inactive[index]);
        m_inactive[index] = m_inactive.last();
        m_inactive.removeLast();
    }
}

void LinearScanCodeGenerator::activate(uint32_t index)
{
    Interval& interval = m_intervals[index];
    unsigned reg = interval.reg - 1;
    if (m_activeInRegister[reg]) {
        fail();
        return;
    }
    m_activeInRegister[reg] = index;
    m_activeRegisters |= static_cast<uint64_t>(1) << reg;
    m_usedCalleeSaves |= (static_cast<uint64_t>(1) << reg) & m_calleeSaves;
    m_nextChangeOfActive = std::min(m_nextChangeOfActive, m_ranges[interval.currentRange - 1].to);
}

bool LinearScanCodeGenerator::tryAllocateFreeRegister(uint32_t current)
{
    const Interval& interval = m_intervals[current];
    uint64_t candidates = candidatesFor(interval);
    std::array<Position, numberOfRegisters> freeUntil;
    freeUntil.fill(noPosition);
    for (uint64_t bits = m_activeRegisters & candidates; bits; bits &= bits - 1)
        freeUntil[std::countr_zero(bits)] = 0;
    for (uint32_t other : m_inactive) {
        const Interval& inactive = m_intervals[other];
        if (!(candidates >> (inactive.reg - 1) & 1) || !freeUntil[inactive.reg - 1])
            continue;
        Position at = nextIntersection(inactive, interval);
        freeUntil[inactive.reg - 1] = std::min(freeUntil[inactive.reg - 1], at);
    }

    uint8_t fixed = interval.vreg && m_virtualRegisters[interval.vreg].definition == interval.from ? m_virtualRegisters[interval.vreg].fixedDefinition : 0;
    if (fixed)
        candidates = bitOf(decode(fixed));
    limitByClobbers(interval, candidates, freeUntil, fixed ? candidates : 0);

    std::array<uint8_t, 6> hints;
    unsigned numberOfHints = fixed ? 0 : collectHints(interval, hints);
    unsigned hinted = numberOfRegisters;
    for (unsigned index = 0; index < numberOfHints && hinted == numberOfRegisters; ++index) {
        unsigned reg = hints[index] - 1;
        if ((candidates >> reg & 1) && freeUntil[reg] >= interval.to)
            hinted = reg;
    }
    for (unsigned index = 0; index < numberOfHints && hinted == numberOfRegisters; ++index) {
        unsigned reg = hints[index] - 1;
        if ((candidates >> reg & 1) && freeUntil[reg] > interval.from + 4)
            hinted = reg;
    }
    unsigned full = numberOfRegisters;
    unsigned partial = numberOfRegisters;
    for (uint8_t reg : m_registerOrder) {
        if (!(candidates >> reg & 1))
            continue;
        Position limit = freeUntil[reg];
        if (limit >= interval.to) {
            if (full == numberOfRegisters || reg == hinted || (limit < freeUntil[full] && full != hinted))
                full = reg;
        } else if (limit > interval.from + 1) {
            if (partial == numberOfRegisters || (reg == hinted && limit > interval.from + 4) || (limit > freeUntil[partial] && partial != hinted))
                partial = reg;
        }
    }
    if (fixed && full == numberOfRegisters && partial == numberOfRegisters) {
        fail();
        return true;
    }

    if (full != numberOfRegisters) {
        Interval& assigned = m_intervals[current];
        assigned.reg = static_cast<uint8_t>(full + 1);
        assigned.place = Place::Register;
        return true;
    }
    if (partial == numberOfRegisters)
        return false;

    Position limit = freeUntil[partial];
    Position needed = firstUseFrom(interval, interval.from, UseKind::Register);
    if (!fixed && (needed == noPosition || needed + 2 > limit))
        return false;
    {
        Interval& assigned = m_intervals[current];
        assigned.reg = static_cast<uint8_t>(partial + 1);
        assigned.place = Place::Register;
    }
    splitWhereRegisterIsLost(current, limit);
    return true;
}

void LinearScanCodeGenerator::allocateBlockedRegister(uint32_t current)
{
    Position from = m_intervals[current].from;
    Position base = from & ~1u;
    uint64_t candidates = candidatesFor(m_intervals[current]);
    std::array<Position, numberOfRegisters> usePosition;
    std::array<Position, numberOfRegisters> blockPosition;
    usePosition.fill(noPosition);
    blockPosition.fill(noPosition);

    auto nextUseOf = [&](const Interval& other) {
        Position use = firstUseFrom(other, base, UseKind::RegisterOrSlot);
        if (!other.vreg)
            return base;
        if (m_virtualRegisters[other.vreg].isRematerialized && use > base + 1 && use != noPosition)
            return noPosition - 1;
        return std::min(use, other.to);
    };
    for (uint64_t bits = m_activeRegisters & candidates; bits; bits &= bits - 1) {
        unsigned reg = std::countr_zero(bits);
        usePosition[reg] = nextUseOf(m_intervals[m_activeInRegister[reg]]);
    }
    for (uint32_t other : m_inactive) {
        const Interval& inactive = m_intervals[other];
        if (!(candidates >> (inactive.reg - 1) & 1))
            continue;
        if (nextIntersection(inactive, m_intervals[current]) == noPosition)
            continue;
        usePosition[inactive.reg - 1] = std::min(usePosition[inactive.reg - 1], nextUseOf(inactive));
    }
    limitByClobbers(m_intervals[current], candidates, blockPosition, 0);

    std::array<uint8_t, 6> hints;
    unsigned hinted = collectHints(m_intervals[current], hints) && (candidates >> (hints[0] - 1) & 1) ? hints[0] - 1 : numberOfRegisters;
    unsigned chosen = numberOfRegisters;
    for (uint8_t reg : m_registerOrder) {
        if (!(candidates >> reg & 1))
            continue;
        usePosition[reg] = std::min(usePosition[reg], blockPosition[reg]);
        if (usePosition[reg] <= from + 1)
            continue;
        if (chosen == numberOfRegisters || usePosition[reg] > usePosition[chosen] || (usePosition[reg] == usePosition[chosen] && reg == hinted))
            chosen = reg;
    }

    Position firstUse = firstUseFrom(m_intervals[current], from, UseKind::Register);
    if (chosen == numberOfRegisters || usePosition[chosen] <= firstUse) {
        if (firstUse <= from + 1) {
            fail();
            return;
        }
        if (firstUse != noPosition && firstUse < m_intervals[current].to)
            splitBeforeUse(current, from + 1, firstUse);
        assignSlot(current, from);
        return;
    }

    {
        Interval& assigned = m_intervals[current];
        assigned.reg = static_cast<uint8_t>(chosen + 1);
        assigned.place = Place::Register;
    }
    if (blockPosition[chosen] < m_intervals[current].to)
        splitWhereRegisterIsLost(current, blockPosition[chosen]);
    if (uint32_t active = m_activeInRegister[chosen]) {
        m_activeInRegister[chosen] = 0;
        m_activeRegisters &= ~(static_cast<uint64_t>(1) << chosen);
        splitAndSpill(active, true);
    }
    for (size_t index = 0; index < m_inactive.size();) {
        uint32_t other = m_inactive[index];
        if (m_intervals[other].reg != chosen + 1 || nextIntersection(m_intervals[other], m_intervals[current]) == noPosition) {
            ++index;
            continue;
        }
        m_inactive[index] = m_inactive.last();
        m_inactive.removeLast();
        splitAndSpill(other, false);
    }
}

void LinearScanCodeGenerator::meanCalleeSavesForValuesThatCrossCalls()
{
    constexpr unsigned maxCandidates = 32;
    if (!m_prefersSmallCode)
        return;
    Vector<Position, 32>& calls = m_positionsOfCalls;
    for (const ClobberEvent& event : m_clobbers) {
        if (std::popcount(m_generalRegisters & ~event.registers) <= 2 && (calls.isEmpty() || calls.last() != event.from))
            calls.append(event.from);
    }
    if (calls.isEmpty())
        return;
    struct Candidate {
        uint32_t vreg;
        unsigned weight;
    };
    Vector<Candidate, 32> candidates;
    for (uint32_t vreg = 1; vreg < m_virtualRegisters.size(); ++vreg) {
        VirtualRegister& v = m_virtualRegisters[vreg];
        if (v.isRematerialized || !v.global || node(v.node).has(IsTerminal))
            continue;
        unsigned callsCrossed = 0;
        unsigned uses = 0;
        for (uint32_t index = v.firstInterval; index; index = m_intervals[index].nextSplit) {
            for (uint32_t range = m_intervals[index].firstRange; range; range = m_ranges[range - 1].next) {
                const LiveRange& covered = m_ranges[range - 1];
                callsCrossed += std::ranges::lower_bound(calls, covered.to - 1) - std::ranges::upper_bound(calls, covered.from);
            }
            for (uint32_t use = m_intervals[index].firstUse; use; use = m_uses[use - 1].next)
                ++uses;
        }
        if (!callsCrossed)
            continue;
        v.livesInMemory = true;
        if (!v.isDouble)
            candidates.append(Candidate { vreg, uses });
    }
    std::ranges::stable_sort(candidates, std::ranges::greater { }, &Candidate::weight);
    if (candidates.size() > maxCandidates)
        candidates.shrink(maxCandidates);
    std::array<Vector<uint32_t, 4>, numberOfRegisters> holders;
    for (const Candidate& candidate : candidates) {
        for (uint64_t bits = m_allocatableCalleeSaves; bits; bits &= bits - 1) {
            unsigned reg = std::countr_zero(bits);
            bool isFree = true;
            for (uint32_t other : holders[reg])
                isFree &= !liveRangesIntersect(candidate.vreg, other);
            if (!isFree)
                continue;
            holders[reg].append(candidate.vreg);
            m_virtualRegisters[candidate.vreg].calleeSaveMeantForIt = static_cast<uint8_t>(reg + 1);
            m_virtualRegisters[candidate.vreg].livesInMemory = false;
            m_calleeSavesMeantForValues |= static_cast<uint64_t>(1) << reg;
            break;
        }
    }
}

bool LinearScanCodeGenerator::keepsInMemory(uint32_t current)
{
    if (!m_intervals[current].vreg)
        return false;
    VirtualRegister& v = m_virtualRegisters[m_intervals[current].vreg];
    Position from = m_intervals[current].from;
    if (v.calleeSaveMeantForIt) {
        if (from == v.definition && v.fixedDefinition && v.fixedDefinition != v.calleeSaveMeantForIt && m_intervals[current].to > (from | 3) + 1)
            enqueue(splitInterval(current, (from | 3) + 1));
        return false;
    }
    if (!v.livesInMemory)
        return false;
    v.needsSlot = true;
    v.spillState = SpillState::StoredAtDefinition;
    bool mayStartInSlot = from != v.definition || (isBlockStart(from) && !v.fixedDefinition);
    if (mayStartInSlot) {
        Position firstUse = firstUseFrom(m_intervals[current], from, UseKind::Register);
        if (firstUse > from + 1) {
            if (firstUse != noPosition && firstUse < m_intervals[current].to)
                enqueue(splitInterval(current, firstUse & ~1u));
            m_intervals[current].reg = 0;
            m_intervals[current].place = Place::Slot;
            return true;
        }
    }
    Position end = m_blocks[blockAt(from)].to();
    auto call = std::ranges::upper_bound(m_positionsOfCalls, from + 1);
    if (call != m_positionsOfCalls.end())
        end = std::min(end, *call & ~1u);
    if (end > from + 1 && end < m_intervals[current].to)
        enqueue(splitInterval(current, end));
    return false;
}

bool LinearScanCodeGenerator::staysOutOfRegisters(uint32_t current)
{
    const Interval& interval = m_intervals[current];
    if (!interval.vreg)
        return false;
    VirtualRegister& v = m_virtualRegisters[interval.vreg];
    if (!isBlockStart(interval.from))
        return false;
    const Block& block = m_blocks[blockAt(interval.from)];
    if (interval.from == v.definition) {
        if (!block.isRare || node(v.node).opcode != B3::Phi || v.fixedDefinition)
            return false;
        unsigned inSlots = 0;
        unsigned inRegisters = 0;
        forEachSourceOf(v.node, [&](NodeID source, uint32_t sender) {
            if (node(source).form != Form::Plain || !node(source).vreg || m_blocks[sender].to() > interval.from)
                return;
            uint8_t location = locationAt(node(source).vreg, m_blocks[sender].to() - 1);
            inSlots += location == locationInSlot;
            inRegisters += isRegisterLocation(location);
        });
        Position firstUse = firstUseFrom(interval, interval.from, UseKind::Register);
        if (inSlots <= inRegisters || firstUse <= interval.from + 1)
            return false;
        if (firstUse != noPosition && firstUse < interval.to)
            splitBeforeUse(current, interval.from + 1, firstUse);
        Interval& kept = m_intervals[current];
        kept.reg = 0;
        kept.place = Place::Slot;
        v.needsSlot = true;
        if (v.spillState == SpillState::NotSpilled)
            v.spillState = SpillState::StoredWhereSpilled;
        return true;
    }
    bool arrivesInSlot = false;
    for (unsigned index = 0; index < block.numberOfPredecessors; ++index) {
        const Block& predecessor = m_blocks[m_predecessors[block.firstPredecessor + index]];
        if (!predecessor.isReachable || predecessor.to() > interval.from)
            continue;
        uint8_t location = locationAt(interval.vreg, predecessor.to() - 1);
        if (isRegisterLocation(location))
            return false;
        arrivesInSlot |= location == locationInSlot || location == locationMaterialized;
    }
    if (!arrivesInSlot)
        return false;
    Position firstUse = firstUseFrom(interval, interval.from, UseKind::Register);
    if (firstUse <= interval.from + 1)
        return false;
    if (firstUse != noPosition && firstUse < interval.to)
        splitBeforeUse(current, interval.from + 1, firstUse);
    Interval& kept = m_intervals[current];
    kept.reg = 0;
    kept.place = v.isRematerialized ? Place::Nowhere : Place::Slot;
    return true;
}

void LinearScanCodeGenerator::allocateRegisters()
{
    m_unhandled.shrink(0);
    for (uint32_t index = 1; index < m_intervals.size(); ++index) {
        if (m_intervals[index].firstRange)
            m_unhandled.append(keyOf(m_intervals[index].from, index));
    }
    std::make_heap(m_unhandled.begin(), m_unhandled.end(), std::greater<uint64_t> { });
    m_activeInRegister.fill(0);
    m_activeRegisters = 0;
    m_inactive.shrink(0);
    m_nextChangeOfActive = noPosition;
    size_t probe = 0;
    auto answerProbesUpTo = [&](Position position) {
        for (; probe < m_probes.size() && m_probes[probe] <= position; ++probe) {
            advanceTo(m_probes[probe]);
            m_registersLiveAtProbes.append(m_activeRegisters);
        }
    };

    while (!m_unhandled.isEmpty() && !m_hasFailed) {
        std::pop_heap(m_unhandled.begin(), m_unhandled.end(), std::greater<uint64_t> { });
        uint32_t current = static_cast<uint32_t>(m_unhandled.takeLast());
        Position from = m_intervals[current].from;
        answerProbesUpTo(from);
        m_currentPosition = from;
        m_intervals[current].currentRange = m_intervals[current].firstRange;
        advanceTo(from);

        if (keepsInMemory(current) || staysOutOfRegisters(current))
            continue;
        if (!tryAllocateFreeRegister(current))
            allocateBlockedRegister(current);
        if (m_hasFailed)
            return;
        if (m_intervals[current].place == Place::Register)
            activate(current);
    }
    answerProbesUpTo(noPosition);
}

void LinearScanCodeGenerator::writeBackLocations()
{
    for (uint32_t index = 1; index < m_intervals.size(); ++index) {
        const Interval& interval = m_intervals[index];
        uint8_t location = interval.location();
        for (uint32_t use = interval.firstUse; use; use = m_uses[use - 1].next) {
            if (m_uses[use - 1].kind == UseKind::Register && interval.place != Place::Register) {
                fail();
                return;
            }
            m_operands[m_uses[use - 1].operand].location = location;
            m_numberOfSlotAccesses += location == locationInSlot;
        }
        if (interval.vreg)
            m_virtualRegisters[interval.vreg].end = std::max(m_virtualRegisters[interval.vreg].end, interval.to);
    }
}

uint32_t LinearScanCodeGenerator::definingIntervalOf(const VirtualRegister& v) const
{
    uint32_t index = v.firstInterval;
    while (index && m_intervals[index].from != v.definition)
        index = m_intervals[index].nextSplit;
    return index ? index : v.firstInterval;
}

void LinearScanCodeGenerator::insertMoves()
{
    for (uint32_t vreg = 1; vreg < m_virtualRegisters.size(); ++vreg) {
        const VirtualRegister& v = m_virtualRegisters[vreg];
        uint8_t locationBefore = 0;
        Position positionOfLastMove = noPosition;
        for (uint32_t index = v.firstInterval; index; index = m_intervals[index].nextSplit) {
            const Interval& interval = m_intervals[index];
            if (v.isRematerialized && !interval.isSplitChild && interval.place == Place::Register)
                m_moves.append(Move { interval.from, v.node, v.node, locationMaterialized, interval.reg, 1 });
            if (!interval.nextSplit)
                break;
            const Interval& next = m_intervals[interval.nextSplit];
            if ((next.from != interval.to && next.from + 1 != interval.to) || next.region != interval.region || isBlockStart(next.from))
                continue;
            uint8_t from = interval.from == next.from && positionOfLastMove == next.from ? locationBefore : interval.location();
            locationBefore = from;
            positionOfLastMove = next.from;
            uint8_t to = next.location();
            if (from == to || to == locationMaterialized)
                continue;
            if (to == locationInSlot && v.spillState == SpillState::StoredAtDefinition)
                continue;
            m_moves.append(Move { next.from, v.node, v.node, from, to, 1 });
        }
        if (!v.needsSlot || v.spillState != SpillState::StoredAtDefinition || !v.firstInterval)
            continue;
        const Interval& defining = m_intervals[definingIntervalOf(v)];
        if (defining.place == Place::Register)
            m_moves.append(Move { v.definition & 3 ? (v.definition | 1) + 1 : v.definition, v.node, v.node, defining.reg, locationInSlot, 1 });
    }
    std::ranges::stable_sort(m_moves, { }, &Move::position);
    for (const Move& move : m_moves)
        m_numberOfSlotAccesses += move.from == locationInSlot || move.to == locationInSlot;
}

void LinearScanCodeGenerator::addEdgeMove(uint32_t edge, NodeID source, uint8_t from, NodeID destination, uint8_t to)
{
    if (!from || !to) {
        fail();
        return;
    }
    if (to == locationMaterialized)
        return;
    if (source == destination) {
        if (from == to)
            return;
        if (to == locationInSlot && m_virtualRegisters[node(source).vreg].spillState == SpillState::StoredAtDefinition)
            return;
    } else if (from == to && isRegisterLocation(from))
        return;
    EdgeMoves& moves = m_edges[edge];
    if (!moves.count)
        moves.first = m_movesOnEdges.size();
    ++moves.count;
    m_numberOfSlotAccesses += from == locationInSlot || to == locationInSlot;
    m_movesOnEdges.append(Move { 0, source, destination, from, to, 0 });
}

void LinearScanCodeGenerator::resolveEdges()
{
    m_edges.fill(EdgeMoves(), m_successors.size());
    bool hasValueInSeveralPlaces = false;
    for (VirtualRegister& v : m_virtualRegisters) {
        if (!v.hasSeveralIntervals || v.isRematerialized)
            continue;
        uint8_t place = m_intervals[v.firstInterval].location();
        bool hasOnePlace = true;
        for (uint32_t index = m_intervals[v.firstInterval].nextSplit; index && hasOnePlace; index = m_intervals[index].nextSplit)
            hasOnePlace = !m_intervals[index].firstRange || m_intervals[index].location() == place;
        v.hasSeveralIntervals = !hasOnePlace;
        hasValueInSeveralPlaces |= !hasOnePlace;
    }

    auto forEachEdgeFrom = [&](uint32_t order, const auto& functor) {
        const Block& from = m_blocks[order];
        if (!from.isReachable)
            return;
        for (unsigned successor = 0; successor < from.numberOfSuccessors; ++successor) {
            if (!from.onlySuccessor || from.onlySuccessor == successor + 1)
                functor(from.firstSuccessor + successor, successorOf(from, successor));
        }
    };

    // A value moves along an edge only where the end of the one block and the start of the other lie in two ranges. Most blocks have all their predecessors close by, inside the range.
    if (hasValueInSeveralPlaces) {
        struct Predecessors {
            uint32_t least { UINT32_MAX };
            uint32_t most { 0 };
        };
        Vector<Predecessors, 64> predecessors;
        predecessors.fill(Predecessors(), m_blocks.size());
        for (uint32_t order = 0; order < m_blocks.size(); ++order) {
            forEachEdgeFrom(order, [&](uint32_t, uint32_t target) {
                predecessors[target].least = std::min(predecessors[target].least, order);
                predecessors[target].most = std::max(predecessors[target].most, order);
            });
        }
        for (uint32_t index = 1; index < m_intervals.size(); ++index) {
            const Interval& interval = m_intervals[index];
            if (!interval.vreg || !interval.firstRange)
                continue;
            const VirtualRegister& v = m_virtualRegisters[interval.vreg];
            if (!v.hasSeveralIntervals || v.isRematerialized)
                continue;
            uint8_t location = interval.location();
            for (uint32_t range = interval.firstRange; range; range = m_ranges[range - 1].next) {
                const LiveRange& covered = m_ranges[range - 1];
                uint32_t first = blockAt(covered.from);
                uint32_t last = blockAt(covered.to - 1);
                uint32_t endOfBlocksInRange = m_blocks[last].to() <= covered.to ? last + 1 : last;
                for (uint32_t order = m_blocks[first].from() >= covered.from ? first : first + 1; order <= last; ++order) {
                    Predecessors known = predecessors[order];
                    if (known.least == UINT32_MAX || (known.least >= first && known.most < endOfBlocksInRange))
                        continue;
                    Block& block = m_blocks[order];
                    m_boundaryEntries.append(BoundaryEntry { interval.vreg, block.firstEntryAtStart, first, endOfBlocksInRange, location });
                    block.firstEntryAtStart = m_boundaryEntries.size();
                }
            }
        }
    }

    auto locationOfPieceAt = [&](const VirtualRegister& v, Position position) -> uint8_t {
        for (uint32_t index = v.firstInterval; index; index = m_intervals[index].nextSplit) {
            const Interval& interval = m_intervals[index];
            if (interval.from <= position && position < interval.to && covers(interval, position))
                return interval.location();
        }
        return 0;
    };
    for (uint32_t order = 0; order < m_blocks.size() && !m_hasFailed; ++order) {
        const Block& from = m_blocks[order];
        auto locationAtEndOf = [&](NodeID id) -> uint8_t {
            const Node& source = node(id);
            switch (source.form) {
            case Form::Constant:
            case Form::Rematerialized:
                return locationMaterialized;
            case Form::FixedRegister:
                return source.base;
            default:
                break;
            }
            const VirtualRegister& v = m_virtualRegisters[source.vreg];
            if (!v.hasSeveralIntervals)
                return m_intervals[v.firstInterval].location();
            return v.isRematerialized ? 0 : locationOfPieceAt(v, from.to() - 1);
        };
        forEachEdgeFrom(order, [&](uint32_t edge, uint32_t target) {
            const Block& to = m_blocks[target];
            for (uint32_t index = to.firstEntryAtStart; index; index = m_boundaryEntries[index - 1].next) {
                const BoundaryEntry& entry = m_boundaryEntries[index - 1];
                if (entry.firstBlockInRange <= order && order < entry.endOfBlocksInRange)
                    continue;
                const VirtualRegister& v = m_virtualRegisters[entry.vreg];
                if (v.definition == to.from())
                    continue;
                addEdgeMove(edge, v.node, locationOfPieceAt(v, from.to() - 1), v.node, entry.location);
            }
            for (uint32_t record = from.firstUpsilon; record; record = m_upsilons[record - 1].next) {
                const Node& upsilon = node(m_upsilons[record - 1].upsilon);
                if (upsilon.form != Form::Plain)
                    continue;
                const Node& phi = node(upsilon.b);
                if (phi.block != target || phi.form != Form::Plain || phi.has(ArrivesInSlot) || !phi.vreg)
                    continue;
                addEdgeMove(edge, upsilon.a, locationAtEndOf(upsilon.a), upsilon.b, m_intervals[definingIntervalOf(m_virtualRegisters[phi.vreg])].location());
            }
        });
    }
}

void LinearScanCodeGenerator::hoistMovesSharedByAllPredecessors()
{
    constexpr uint32_t maxMovesCompared = 16;
    auto isSameMove = [](const Move& a, const Move& b) {
        return a.source == b.source && a.destination == b.destination && a.from == b.from && a.to == b.to;
    };
    for (Block& block : m_blocks) {
        if (!block.isReachable || block.numberOfPredecessors < 2)
            continue;
        uint32_t firstEdge = UINT32_MAX;
        bool isShared = true;
        for (unsigned index = 0; index < block.numberOfPredecessors && isShared; ++index) {
            const Block& predecessor = m_blocks[m_predecessors[block.firstPredecessor + index]];
            uint32_t edge = m_edgeOfPredecessor[block.firstPredecessor + index];
            if (!predecessor.isReachable || (predecessor.onlySuccessor && predecessor.firstSuccessor + predecessor.onlySuccessor - 1 != edge))
                continue;
            const EdgeMoves& moves = m_edges[edge];
            if (!moves.count || moves.count > maxMovesCompared) {
                isShared = false;
                break;
            }
            if (firstEdge == UINT32_MAX) {
                firstEdge = edge;
                continue;
            }
            const EdgeMoves& first = m_edges[firstEdge];
            isShared = first.count == moves.count;
            for (uint32_t a = 0; a < moves.count && isShared; ++a) {
                bool isFound = false;
                for (uint32_t b = 0; b < first.count; ++b)
                    isFound |= isSameMove(m_movesOnEdges[moves.first + a], m_movesOnEdges[first.first + b]);
                isShared = isFound;
            }
        }
        if (!isShared || firstEdge == UINT32_MAX)
            continue;
        if (!block.isRare) {
            bool touchesFrame = false;
            for (uint32_t index = 0; index < m_edges[firstEdge].count; ++index) {
                const Move& move = m_movesOnEdges[m_edges[firstEdge].first + index];
                touchesFrame |= move.from == locationInSlot || move.to == locationInSlot;
            }
            if (touchesFrame)
                continue;
        }
        block.firstMoveAtStart = m_edges[firstEdge].first;
        block.numberOfMovesAtStart = m_edges[firstEdge].count;
        for (unsigned index = 0; index < block.numberOfPredecessors; ++index)
            m_edges[m_edgeOfPredecessor[block.firstPredecessor + index]].count = 0;
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT) && (CPU(X86_64) || CPU(ARM64)) && !OS(WINDOWS)
