/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLinearScanCodeGenerator.h"

#if ENABLE(AOT) && (CPU(X86_64) || CPU(ARM64)) && !OS(WINDOWS)

#include "AOTLinearScanInternals.h"
#include "AOTRelocation.h"
#include "AOTStubRegisters.h"
#include "AirFrameEmission.h"
#include "AirHandleCalleeSaves.h"
#include "AirInstInlines.h"
#include "AirLimits.h"
#include "AirStackSlot.h"
#include "AllowMacroScratchRegisterUsage.h"
#include "B3BasicBlockInlines.h"
#include "B3RelocatableValue.h"
#include "B3SlotBaseValue.h"
#include "B3ValueInlines.h"
#include "B3Variable.h"
#include "StackAlignment.h"
#include <algorithm>
#include <numeric>
#include <wtf/DataLog.h>
#include <wtf/StringPrintStream.h>

namespace JSC { namespace AOT { namespace Typed {

using namespace LinearScan;

LinearScanCodeGenerator::LinearScanCodeGenerator(B3::Procedure& procedure, DirectCodeAttempt)
    : m_procedure(procedure)
    , m_code(procedure.code())
{
}

LinearScanCodeGenerator::~LinearScanCodeGenerator() = default;

bool LinearScanCodeGenerator::plan()
{
    if (m_procedure.numEntrypoints() != 1)
        return refuse();
    for (B3::Variable* variable : m_procedure.variables()) {
        if (variable->type() != B3::Int32 && variable->type() != B3::Int64 && variable->type() != B3::Double)
            return refuse();
    }
    setUpRegisters();
    if (!orderBlocks())
        return false;
    if (m_hasCycles && m_loopsRule == LoopsInDirectCode::AreLeftToB3)
        return false;
    if (!importValues())
        return false;
    findLoops();
    estimateFrequencies();
    select();
    if (m_isRefused)
        return false;
    m_allocatableCalleeSaves = m_calleeSaves & ~m_clobberedRegisters & ~bitsOf(m_code.calleeSavesClobberedAtSuspensions()) & bitsOf(RegisterSet::allGPRs());
    computeLiveness();
    if (m_isRefused)
        return false;
    buildIntervals();
    meanCalleeSavesForValuesThatCrossCalls();
    allocateRegisters();
    if (m_hasFailed)
        return false;
    writeBackLocations();
    insertMoves();
    resolveEdges();
    hoistMovesSharedByAllPredecessors();
    return !m_hasFailed;
}

bool LinearScanCodeGenerator::isAddressedOffStackPointer(int32_t offset) const
{
    if (!m_hasFrameRecord)
        return true;
    return isX86_64() && offset < -128 && offset + static_cast<int32_t>(m_code.frameSize()) <= 127;
}

MacroAssembler::Address LinearScanCodeGenerator::addressOfSlot(int32_t offset) const
{
    if (!isAddressedOffStackPointer(offset))
        return Address(MacroAssembler::framePointerRegister, offset);
    return Address(MacroAssembler::stackPointerRegister, offset + m_slotBias + static_cast<int32_t>(m_code.frameSize()));
}

int32_t LinearScanCodeGenerator::offsetOfSlotOf(NodeID id) const
{
    return m_virtualRegisters[node(id).vreg].offset;
}

bool LinearScanCodeGenerator::liveRangesIntersect(uint32_t first, uint32_t second) const
{
    struct Cursor {
        uint32_t interval;
        uint32_t range;
    };
    auto settle = [&](Cursor& cursor) {
        while (!cursor.range && cursor.interval) {
            cursor.interval = m_intervals[cursor.interval].nextSplit;
            cursor.range = cursor.interval ? m_intervals[cursor.interval].firstRange : 0;
        }
    };
    auto begin = [&](uint32_t vreg) {
        Cursor cursor { m_virtualRegisters[vreg].firstInterval, 0 };
        cursor.range = cursor.interval ? m_intervals[cursor.interval].firstRange : 0;
        settle(cursor);
        return cursor;
    };
    auto advance = [&](Cursor& cursor) {
        cursor.range = m_ranges[cursor.range - 1].next;
        settle(cursor);
    };
    Cursor a = begin(first);
    Cursor b = begin(second);
    while (a.range && b.range) {
        const LiveRange& rangeA = m_ranges[a.range - 1];
        const LiveRange& rangeB = m_ranges[b.range - 1];
        if (rangeA.to <= rangeB.from)
            advance(a);
        else if (rangeB.to <= rangeA.from)
            advance(b);
        else
            return true;
    }
    return false;
}

void LinearScanCodeGenerator::layOutFrame()
{
    unsigned extent = 0;
    auto takeSlot = [&] {
        extent += sizeof(CPURegister);
        return -static_cast<int32_t>(extent);
    };

    uint64_t clobberedAtSuspensions = bitsOf(m_code.calleeSavesClobberedAtSuspensions());
    uint64_t savedRegisters = (m_clobberedRegisters & m_calleeSaves & ~clobberedAtSuspensions) | m_usedCalleeSaves;
    unsigned bytesOfSaves = 0;
    bool savesAreGeneral = true;
    if (savedRegisters) {
        RegisterSet used;
        for (uint64_t bits = savedRegisters; bits; bits &= bits - 1) {
            used.add(Reg::fromIndex(std::countr_zero(bits)), IgnoreVectors);
            savesAreGeneral &= Reg::fromIndex(std::countr_zero(bits)).isGPR();
        }
        B3::Air::handleCalleeSaves(m_code, used);
        m_needsFrame = true;
        if (B3::Air::StackSlot* slot = m_code.calleeSaveStackSlot())
            bytesOfSaves = WTF::roundUpToMultipleOf<sizeof(CPURegister)>(slot->byteSize());
    }
    unsigned bytesOfArguments = std::max(m_code.callArgAreaSizeInBytes(), m_bytesOfStackArguments);

    Vector<uint32_t, 32> spilled;
    for (uint32_t vreg = 1; vreg < m_virtualRegisters.size(); ++vreg) {
        if (m_virtualRegisters[vreg].needsSlot)
            spilled.append(vreg);
    }
    std::ranges::sort(spilled, { }, [&](uint32_t vreg) {
        return m_intervals[m_virtualRegisters[vreg].firstInterval].from;
    });
    unsigned numberOfShadows = 0;
    for (const Block& block : m_blocks) {
        for (NodeID phi = block.firstPhi; phi; phi = node(phi).next)
            numberOfShadows += node(phi).form == Form::Plain && node(phi).has(ArrivesInSlot);
    }

    size_t boundOfExtent = sizeof(CPURegister) * (spilled.size() + numberOfShadows + m_procedure.variables().size() + 2) + bytesOfSaves + bytesOfArguments;
    for (B3::Air::StackSlot* slot : m_code.stackSlots())
        boundOfExtent += slot->byteSize() + stackAlignmentBytes();
    bool savesLieOnTop = bytesOfSaves && (boundOfExtent > B3::AirLimits::maxFrameSizeWithPushedRegisters || clobberedAtSuspensions);
    if (savesLieOnTop) {
        extent = bytesOfSaves;
        m_code.calleeSaveStackSlot()->setOffsetFromFP(-static_cast<intptr_t>(bytesOfSaves));
    }

    constexpr unsigned maxSlotsSearched = 48;
    constexpr unsigned maxOccupantsCompared = 12;
    struct Slot {
        Position freeAt;
        Vector<uint32_t, 4> occupants;
    };
    Vector<Slot, 32> slots;
    constexpr unsigned maxFamily = 4;
    for (uint32_t vreg : spilled) {
        if (m_virtualRegisters[vreg].hasPlaceInFrame)
            continue;
        Vector<uint32_t, maxFamily> family;
        family.append(vreg);
        for (uint32_t next = m_virtualRegisters[vreg].targetVirtualRegister; next && family.size() < maxFamily; next = m_virtualRegisters[next].targetVirtualRegister) {
            const VirtualRegister& target = m_virtualRegisters[next];
            if (!target.needsSlot || target.hasPlaceInFrame || family.contains(next))
                break;
            bool livesBesideFamily = false;
            for (uint32_t member : family)
                livesBesideFamily |= liveRangesIntersect(member, next);
            if (livesBesideFamily)
                break;
            family.append(next);
        }
        unsigned limit = std::min<unsigned>(slots.size(), maxSlotsSearched);
        auto fits = [&](const Slot& candidate) {
            for (uint32_t member : family) {
                const VirtualRegister& v = m_virtualRegisters[member];
                Position start = m_intervals[v.firstInterval].from;
                if (candidate.freeAt <= start)
                    continue;
                if (candidate.occupants.size() > maxOccupantsCompared)
                    return false;
                for (uint32_t other : candidate.occupants) {
                    if (m_virtualRegisters[other].end > start && liveRangesIntersect(member, other))
                        return false;
                }
            }
            return true;
        };
        unsigned slot = limit;
        auto tryToShareWith = [&](uint32_t other) {
            const VirtualRegister& sharer = m_virtualRegisters[other];
            if (slot == limit && sharer.needsSlot && sharer.hasPlaceInFrame && static_cast<unsigned>(sharer.offset) < limit && fits(slots[sharer.offset]))
                slot = sharer.offset;
        };
        auto tryToShareWithSourcesOf = [&](NodeID phi) {
            forEachSourceOf(phi, [&](NodeID source, uint32_t) {
                if (node(source).form == Form::Plain && node(source).vreg)
                    tryToShareWith(node(source).vreg);
            });
        };
        for (uint32_t member : family) {
            const VirtualRegister& v = m_virtualRegisters[member];
            if (node(v.node).opcode == B3::Phi && node(v.node).form == Form::Plain)
                tryToShareWithSourcesOf(v.node);
            if (v.targetVirtualRegister) {
                tryToShareWith(v.targetVirtualRegister);
                tryToShareWithSourcesOf(m_virtualRegisters[v.targetVirtualRegister].node);
            }
        }
        if (slot == limit) {
            slot = 0;
            while (slot < limit && !fits(slots[slot]))
                ++slot;
        }
        if (slot == limit) {
            slot = slots.size();
            slots.append(Slot { 0, { } });
        }
        for (uint32_t member : family) {
            VirtualRegister& v = m_virtualRegisters[member];
            v.hasPlaceInFrame = true;
            slots[slot].occupants.append(member);
            slots[slot].freeAt = std::max(slots[slot].freeAt, v.end);
            v.offset = static_cast<int32_t>(slot);
        }
    }
    unsigned firstSlot = extent;
    extent += slots.size() * sizeof(CPURegister);
    // An access has a short form near the frame pointer and, on x86-64, near the stack pointer. The slots named most lie at the top, the next at the bottom, the rest between them.
    constexpr unsigned bytesInShortReach = isX86_64() ? 128 : 256;
    Vector<uint32_t, 32> positionOfSlot;
    positionOfSlot.grow(slots.size());
    std::iota(positionOfSlot.begin(), positionOfSlot.end(), 0);
    if (extent > bytesInShortReach) {
        Vector<uint32_t, 32> accessesOfSlot;
        accessesOfSlot.fill(0, slots.size());
        auto noteAccess = [&](NodeID id) {
            uint32_t vreg = node(id).vreg;
            if (vreg && m_virtualRegisters[vreg].hasPlaceInFrame)
                ++accessesOfSlot[m_virtualRegisters[vreg].offset];
        };
        auto noteMoves = [&](const Vector<Move>& moves) {
            for (const Move& move : moves) {
                if (move.from == locationInSlot)
                    noteAccess(move.source);
                if (move.to == locationInSlot)
                    noteAccess(move.destination);
            }
        };
        for (const Operand& operand : m_operands) {
            if (operand.location == locationInSlot)
                noteAccess(operand.node);
        }
        noteMoves(m_moves);
        noteMoves(m_movesOnEdges);
        Vector<uint32_t, 32> slotsByUse = positionOfSlot;
        std::ranges::stable_sort(slotsByUse, std::ranges::greater { }, [&](uint32_t slot) {
            return accessesOfSlot[slot];
        });
        size_t numberAtTop = std::min<size_t>(slots.size(), (bytesInShortReach - std::min(firstSlot, bytesInShortReach)) / sizeof(CPURegister));
        std::ranges::reverse(slotsByUse.mutableSpan().subspan(numberAtTop));
        for (uint32_t position = 0; position < slotsByUse.size(); ++position)
            positionOfSlot[slotsByUse[position]] = position;
    }
    for (uint32_t vreg : spilled)
        m_virtualRegisters[vreg].offset = -static_cast<int32_t>(firstSlot + (positionOfSlot[m_virtualRegisters[vreg].offset] + 1) * sizeof(CPURegister));
    bool needsFrameForSlots = !slots.isEmpty();
    unsigned bytesOfSavesAtBottom = savesLieOnTop ? 0 : bytesOfSaves;
    unsigned extentOfSlots = extent;

    m_variableOffsets.fill(0, m_procedure.variables().size());
    for (B3::Variable* variable : m_procedure.variables())
        m_variableOffsets[variable->index()] = takeSlot();
    for (const Block& block : m_blocks) {
        for (NodeID phi = block.firstPhi; phi; phi = node(phi).next) {
            if (node(phi).form == Form::Plain && node(phi).has(ArrivesInSlot))
                node(phi).constant = takeSlot();
        }
    }
    for (B3::Air::StackSlot* slot : m_code.stackSlots()) {
        if (slot == m_code.calleeSaveStackSlot())
            continue;
        extent = WTF::roundUpToMultipleOf(slot->alignment(), extent + slot->byteSize());
        slot->setOffsetFromFP(-static_cast<intptr_t>(extent));
    }
    extent = WTF::roundUpToMultipleOf<sizeof(CPURegister)>(extent);

    if (needsFrameForSlots || extent > extentOfSlots || bytesOfArguments)
        m_needsFrame = true;
    m_hasFrame = m_needsFrame;

    const B3::Air::FramePolicy& policy = m_code.policy().frame;
    bool hasOtherSlots = m_code.stackSlots().size() > (m_code.calleeSaveStackSlot() ? 1u : 0u);
    bool mayOmitRecord = isX86_64() && m_hasFrame && policy.mayOmitFrameRecord && !m_needsFrameRecord && !hasOtherSlots && !bytesOfArguments;
    bool omitsRecord = mayOmitRecord && !savedRegisters && extent <= sizeof(CPURegister);
    constexpr unsigned maxBytesWithoutRecord = 120;
    if (mayOmitRecord && !omitsRecord && !savesLieOnTop && savesAreGeneral && policy.mayPushCalleeSaves && !stackAdjustmentForAlignment() && extent + bytesOfSaves <= maxBytesWithoutRecord) {
        auto bytesToMoveStackPointer = [](unsigned words) {
            return !words ? 0 : words == 1 ? 1 : 4;
        };
        int accesses = static_cast<int>(m_numberOfSlotAccesses + 3 * numberOfShadows + 2 * m_procedure.variables().size());
        unsigned words = (extent + bytesOfSaves) / sizeof(CPURegister);
        unsigned wordsOfSlots = extent / sizeof(CPURegister);
        unsigned wordsWithRecord = wordsOfSlots + words % 2;
        unsigned wordsWithoutRecord = wordsOfSlots + !(words % 2);
        constexpr int bytesOfRecordSetup = 4;
        constexpr int bytesOfRecordTeardown = 1;
        int gain = bytesOfRecordSetup + bytesToMoveStackPointer(wordsWithRecord) - bytesToMoveStackPointer(wordsWithoutRecord);
        gain += static_cast<int>(m_numberOfTeardowns) * (bytesOfRecordTeardown - bytesToMoveStackPointer(wordsWithoutRecord));
        omitsRecord = gain + static_cast<int>(policy.maxBytesAddedWithoutFrameRecord) > accesses;
    }
    m_hasFrameRecord = !omitsRecord;
    if (!m_hasFrameRecord)
        m_slotBias = sizeof(CPURegister);
    unsigned bytesAboveFramePointer = static_cast<unsigned>(m_slotBias);
    unsigned bytesBelowSlots = bytesOfSavesAtBottom + bytesOfArguments;
    unsigned frameSize = WTF::roundUpToMultipleOf<stackAlignmentBytes()>(std::max(extent + bytesBelowSlots, bytesAboveFramePointer) - bytesAboveFramePointer) + stackAdjustmentForAlignment();
    if (bytesOfSavesAtBottom) {
        while (extent + bytesBelowSlots + sizeof(CPURegister) <= frameSize + bytesAboveFramePointer - stackAdjustmentForAlignment())
            extent += sizeof(CPURegister);
        m_code.calleeSaveStackSlot()->setOffsetFromFP(m_slotBias - static_cast<intptr_t>(extent + bytesOfSaves));
    }
    if (m_hasFrame)
        m_code.setFrameSize(frameSize);

    B3::Air::FrameLayout layout;
    layout.isPlanned = true;
    layout.hasFrame = m_hasFrame;
    layout.hasFrameRecord = m_hasFrameRecord;
    layout.coverage = m_hasFrame ? B3::Air::FrameCoverage::EveryPathNeedsFrame : B3::Air::FrameCoverage::NoPath;
    layout.slotArea = B3::Air::SlotArea::BelowFrameRecord;
    layout.calleeSaveForm = B3::Air::CalleeSaveForm::Stored;
    layout.numberOfFrameSetups = m_hasFrame;
#if CPU(X86_64)
    if (m_hasFrame && frameSize <= B3::AirLimits::maxFrameSizeWithPushedRegisters) {
        if (bytesOfSavesAtBottom && savesAreGeneral && !bytesOfArguments && !stackAdjustmentForAlignment() && policy.mayPushCalleeSaves)
            layout.calleeSaveForm = B3::Air::CalleeSaveForm::PushedAtFrameBottom;
        else if (savesLieOnTop && savesAreGeneral && policy.mayPushCalleeSaves)
            layout.calleeSaveForm = B3::Air::CalleeSaveForm::PushedBelowFrameRecord;
    }
#endif
    m_code.setFrameLayout(WTF::move(layout));
}

void LinearScanCodeGenerator::noteWhereFrameIsUsed()
{
    if (m_usedCalleeSaves) {
        for (uint32_t index = 1; index < m_intervals.size(); ++index) {
            const Interval& interval = m_intervals[index];
            if (interval.place != Place::Register || !(m_calleeSaves >> (interval.reg - 1) & 1))
                continue;
            for (uint32_t range = interval.firstRange; range; range = m_ranges[range - 1].next) {
                for (uint32_t order = blockAt(m_ranges[range - 1].from); order < m_blocks.size() && m_blocks[order].from() < m_ranges[range - 1].to; ++order)
                    m_blocks[order].usesFrame = true;
            }
        }
    }
    for (uint32_t vreg = 1; vreg < m_virtualRegisters.size(); ++vreg) {
        const VirtualRegister& v = m_virtualRegisters[vreg];
        if (!v.needsSlot)
            continue;
        for (uint32_t index = v.firstInterval; index; index = m_intervals[index].nextSplit) {
            const Interval& interval = m_intervals[index];
            if (interval.place != Place::Slot && v.spillState != SpillState::StoredAtDefinition)
                continue;
            for (uint32_t range = interval.firstRange; range; range = m_ranges[range - 1].next) {
                for (uint32_t order = blockAt(m_ranges[range - 1].from); order < m_blocks.size() && m_blocks[order].from() < m_ranges[range - 1].to; ++order)
                    m_blocks[order].usesFrame = true;
            }
        }
    }
    for (const Move& move : m_moves) {
        if (move.from == locationInSlot || move.to == locationInSlot)
            m_blocks[blockAt(move.position)].usesFrame = true;
    }
    for (uint32_t order = 0; order < m_blocks.size(); ++order) {
        Block& block = m_blocks[order];
        if (!block.isReachable)
            continue;
        for (uint32_t move = 0; move < block.numberOfMovesAtStart; ++move) {
            const Move& atStart = m_movesOnEdges[block.firstMoveAtStart + move];
            block.usesFrame |= atStart.from == locationInSlot || atStart.to == locationInSlot;
        }
        for (unsigned index = 0; index < block.numberOfSuccessors; ++index) {
            const EdgeMoves& moves = m_edges[block.firstSuccessor + index];
            for (uint32_t move = 0; move < moves.count; ++move) {
                const Move& onEdge = m_movesOnEdges[moves.first + move];
                if (onEdge.from == locationInSlot || onEdge.to == locationInSlot) {
                    Block& target = m_blocks[successorOf(block, index)];
                    if (block.isRare || !target.isRare)
                        block.usesFrame = true;
                    if (target.isRare || !block.isRare)
                        target.usesFrame = true;
                }
            }
        }
        if (block.usesFrame)
            continue;
        for (uint32_t slot = block.firstSlot; slot < block.endSlot && !block.usesFrame; ++slot) {
            const Node& n = node(m_sequence[slot]);
            if (n.form != Form::Plain)
                continue;
            for (uint32_t operand = 0; operand < n.numberOfOperands; ++operand)
                block.usesFrame |= m_operands[n.firstOperand + operand].location == locationInSlot;
        }
    }
}

void LinearScanCodeGenerator::chooseWhereToSetUpFrame()
{
    if (!m_hasFrame || !m_code.policy().frame.shrinkWraps)
        return;
    noteWhereFrameIsUsed();
    if (m_blocks[0].usesFrame)
        return;

    uint32_t count = m_blocks.size();
    auto isLiveEdge = [&](const Block& source, unsigned index) {
        return !source.onlySuccessor || source.onlySuccessor == index + 1;
    };
    BitVector comesAfterUse;
    BitVector leadsToUse;
    Vector<uint32_t, 32> worklist;
    for (uint32_t order = 0; order < count; ++order) {
        if (m_blocks[order].isReachable && m_blocks[order].usesFrame) {
            comesAfterUse.set(order);
            leadsToUse.set(order);
            worklist.append(order);
        }
    }
    Vector<uint32_t, 32> uses = worklist;
    while (!worklist.isEmpty()) {
        const Block& block = m_blocks[worklist.takeLast()];
        for (unsigned index = 0; index < block.numberOfSuccessors; ++index) {
            uint32_t target = successorOf(block, index);
            if (isLiveEdge(block, index) && m_blocks[target].region == block.region && !comesAfterUse.get(target)) {
                comesAfterUse.set(target);
                worklist.append(target);
            }
        }
    }
    worklist = uses;
    while (!worklist.isEmpty()) {
        const Block& block = m_blocks[worklist.takeLast()];
        for (unsigned index = 0; index < block.numberOfPredecessors; ++index) {
            uint32_t source = m_predecessors[block.firstPredecessor + index];
            const Block& predecessor = m_blocks[source];
            if (!predecessor.isReachable || predecessor.region != block.region || !isLiveEdge(predecessor, m_edgeOfPredecessor[block.firstPredecessor + index] - predecessor.firstSuccessor) || leadsToUse.get(source))
                continue;
            leadsToUse.set(source);
            worklist.append(source);
        }
    }
    if (comesAfterUse.get(0) && leadsToUse.get(0))
        return;

    BitVector runsInFrame;
    for (uint32_t order = 0; order < count; ++order) {
        if (comesAfterUse.get(order) && leadsToUse.get(order))
            runsInFrame.set(order);
    }
    auto forEachLivePredecessor = [&](const Block& block, auto&& function) {
        for (unsigned index = 0; index < block.numberOfPredecessors; ++index) {
            uint32_t source = m_predecessors[block.firstPredecessor + index];
            const Block& predecessor = m_blocks[source];
            if (predecessor.isReachable && isLiveEdge(predecessor, m_edgeOfPredecessor[block.firstPredecessor + index] - predecessor.firstSuccessor))
                function(source);
        }
    };
    for (uint32_t order : m_reversePostOrder) {
        const Block& block = m_blocks[order];
        if (!order || !block.isReachable || runsInFrame.get(order))
            continue;
        bool everyPredecessorRunsInFrame = true;
        forEachLivePredecessor(block, [&](uint32_t source) {
            everyPredecessorRunsInFrame &= runsInFrame.get(source);
        });
        if (everyPredecessorRunsInFrame)
            runsInFrame.set(order);
    }

    double costOfRegions = 0;
    double costAtEntry = 1;
    unsigned numberOfSetups = 0;
    for (uint32_t order = 0; order < count; ++order) {
        const Block& block = m_blocks[order];
        if (!block.isReachable)
            continue;
        bool endsFunction = !block.numberOfSuccessors && node(m_sequence[block.endSlot - 1]).opcode != B3::Oops;
        if (endsFunction) {
            costAtEntry += block.frequency;
            if (runsInFrame.get(order))
                costOfRegions += block.frequency;
        }
        for (unsigned index = 0; index < block.numberOfSuccessors; ++index) {
            if (!isLiveEdge(block, index))
                continue;
            uint32_t target = successorOf(block, index);
            if (runsInFrame.get(order) == runsInFrame.get(target))
                continue;
            costOfRegions += std::min(block.frequency, m_blocks[target].frequency);
            numberOfSetups += runsInFrame.get(target);
        }
    }
    if (costOfRegions >= costAtEntry)
        return;

    m_setsUpFrameAtEntry = false;
    for (uint32_t order = 0; order < count; ++order) {
        Block& block = m_blocks[order];
        block.runsInFrame = runsInFrame.get(order);
        if (!block.runsInFrame || !block.isReachable)
            continue;
        bool everyPredecessorIsOutside = true;
        forEachLivePredecessor(block, [&](uint32_t source) {
            everyPredecessorIsOutside &= !runsInFrame.get(source);
        });
        block.setsUpFrame = everyPredecessorIsOutside;
    }
    B3::Air::FrameLayout layout = m_code.frameLayout();
    layout.coverage = B3::Air::FrameCoverage::SomePaths;
    layout.numberOfFrameSetups = numberOfSetups;
    m_code.setFrameLayout(WTF::move(layout));
}

void LinearScanCodeGenerator::emitFrameTeardownOnEdge()
{
    m_context.setFrameIsSetUp(*m_jit, false);
    B3::Air::emitFrameTeardown(*m_jit, m_code);
    finishUnit(CodeUnit::Kind::Pinned);
}

void LinearScanCodeGenerator::emitFrameSetup()
{
    CCallHelpers& jit = *m_jit;
    Vector<B3::Air::Arg, 16> operands;
    uint64_t scratches = bitsOf(RegisterSet::macroClobberedGPRs());
    operands.append(B3::Air::Arg::imm(std::popcount(scratches)));
    for (uint64_t bits = scratches; bits; bits &= bits - 1)
        operands.append(B3::Air::Tmp(Reg::fromIndex(std::countr_zero(bits))));
    for (const RegisterAtOffset& entry : m_code.calleeSaveRegisterAtOffsetList())
        operands.append(B3::Air::Tmp(entry.reg()));
    B3::Air::Inst enterFrame(B3::Air::EnterFrame, nullptr, operands.span());
    B3::Air::emitFrameSetup(jit, m_context, enterFrame);
    m_context.setFrameIsSetUp(jit, true);
    noteNewLatePaths();
    finishUnit(CodeUnit::Kind::Pinned);
}

void LinearScanCodeGenerator::finishUnit(CodeUnit::Kind kind)
{
    Label end = m_jit->labelIgnoringWatchpoints();
    if (m_pendingTrap) [[unlikely]] {
        B3::Origin origin = b3ValueOf(std::exchange(m_pendingTrap, 0))->origin();
        m_records->traps.trappingAccesses.append(TrappingAccess { m_pendingTrapBegin, valueOfOrigin(origin), m_pendingTrapRegister, m_pendingTrapIsNarrow });
        m_records->units.didGenerateTrappingAccess(m_pendingTrapBegin, origin);
        kind = CodeUnit::Kind::Pinned;
    }
    m_records->units.didGenerateInBlock(m_unitBegin, end, kind);
    m_unitBegin = end;
}

void LinearScanCodeGenerator::recordOrigin(NodeID id)
{
    if (m_procedure.needsPCToOriginMap()) [[unlikely]]
        m_procedure.pcToOriginMap().appendItem(m_jit->labelIgnoringWatchpoints(), b3ValueOf(id)->origin());
}

void LinearScanCodeGenerator::noteNewLatePaths()
{
    for (; m_numberOfLatePathsNoted < m_context.latePaths.size(); ++m_numberOfLatePathsNoted) {
        if (!m_context.frameIsSetUp)
            m_context.latePathsWithoutFrame.set(m_numberOfLatePathsNoted);
    }
}

void LinearScanCodeGenerator::moveRegister(Reg source, Reg destination)
{
    if (source == destination)
        return;
    if (source.isGPR() && destination.isGPR())
        m_jit->move(source.gpr(), destination.gpr());
    else if (source.isFPR() && destination.isFPR())
        m_jit->moveDouble(source.fpr(), destination.fpr());
    else if (source.isFPR())
        m_jit->moveDoubleTo64(source.fpr(), destination.gpr());
    else
        m_jit->move64ToDouble(source.gpr(), destination.fpr());
    finishUnit();
}

void LinearScanCodeGenerator::storeToSlot(Reg reg, const Node& owner, int32_t offset)
{
    if (!m_context.frameIsSetUp) {
        fail();
        return;
    }
    if (reg.isFPR())
        m_jit->storeDouble(reg.fpr(), addressOfSlot(offset));
    else if (owner.type != NodeType::Int32 || owner.has(IsReadWide))
        m_jit->store64(reg.gpr(), addressOfSlot(offset));
    else
        m_jit->store32(reg.gpr(), addressOfSlot(offset));
    finishUnit(kindOfFrameAccess(offset));
}

void LinearScanCodeGenerator::loadFromSlot(Reg reg, const Node& owner, int32_t offset)
{
    if (!m_context.frameIsSetUp) {
        fail();
        return;
    }
    if (reg.isFPR())
        m_jit->loadDouble(addressOfSlot(offset), reg.fpr());
    else if (owner.type != NodeType::Int32)
        m_jit->load64(addressOfSlot(offset), reg.gpr());
    else
        m_jit->load32(addressOfSlot(offset), reg.gpr());
    finishUnit(kindOfFrameAccess(offset));
}

void LinearScanCodeGenerator::materialize(NodeID id, Reg destination)
{
    const Node& n = node(id);
    switch (n.form) {
    case Form::Constant:
        if (destination.isFPR()) {
            if (!n.constant)
                m_jit->moveZeroToDouble(destination.fpr());
            else if (std::optional<B3::PinnedConstant> pinned = pinnedConstantFor(n.constant); pinned && pinned->offset)
                m_jit->loadDouble(Address(pinned->reg.gpr(), *pinned->offset), destination.fpr());
            else {
                m_jit->move(TrustedImm64(n.constant), m_jit->scratchRegister());
                m_jit->move64ToDouble(m_jit->scratchRegister(), destination.fpr());
            }
        } else if (n.type == NodeType::Int32)
            m_jit->move(TrustedImm32(static_cast<int32_t>(n.constant)), destination.gpr());
        else if (std::optional<B3::PinnedConstant> pinned = fitsInt32(n.constant) || isRepresentableAs<uint32_t>(n.constant) ? std::nullopt : pinnedConstantFor(n.constant)) {
            if (pinned->offset)
                m_jit->load64(Address(pinned->reg.gpr(), *pinned->offset), destination.gpr());
            else
                m_jit->move(pinned->reg.gpr(), destination.gpr());
        } else
            m_jit->move(TrustedImm64(n.constant), destination.gpr());
        finishUnit();
        return;
    case Form::FixedRegister:
        moveRegister(decode(n.base), destination);
        return;
    case Form::Rematerialized:
        switch (n.opcode) {
        case B3::SlotBase:
            m_jit->addPtr(TrustedImm32(static_cast<int32_t>(b3ValueOf(id)->as<B3::SlotBaseValue>()->slot()->offsetFromFP())), MacroAssembler::framePointerRegister, destination.gpr());
            finishUnit();
            return;
        case B3::Add:
            m_jit->addPtr(TrustedImm32(static_cast<int32_t>(n.constant)), decode(n.base).gpr(), destination.gpr());
            finishUnit();
            return;
        case B3::Relocatable: {
            const B3::RelocatableValue* relocatable = b3ValueOf(id)->as<B3::RelocatableValue>();
            uint32_t index = relocatable->symbol().index;
            switch (static_cast<RelocationSpace>(relocatable->symbol().space)) {
            case RelocationSpace::SiteCaches:
                m_records->relocations.moveInstanceAddress({ SymbolSpace::SiteCache, index }, instanceGPR, destination.gpr());
                break;
            case RelocationSpace::Constants:
                m_records->relocations.moveAddress({ SymbolSpace::Constant, index }, destination.gpr(), PCRelativeReach::Far);
                break;
            case RelocationSpace::JumpTables:
                m_records->relocations.moveAddress({ SymbolSpace::JumpTable, index }, destination.gpr(), PCRelativeReach::Far);
                break;
            default:
                RELEASE_ASSERT_NOT_REACHED();
            }
            finishUnit(CodeUnit::Kind::Pinned);
            if (relocatable->addend()) {
                m_jit->add64(TrustedImm64(relocatable->addend()), destination.gpr());
                finishUnit();
            }
            return;
        }
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
    default:
        fail();
        return;
    }
}

void LinearScanCodeGenerator::emitTransfer(const Transfer& transfer)
{
    CCallHelpers& jit = *m_jit;
    const Node& source = node(transfer.source);
    const Node& destination = node(transfer.destination);
    auto storeTo = [&](Reg reg) {
        if (transfer.to == locationInSlot) {
            storeToSlot(reg, destination, offsetOfSlotOf(transfer.destination));
            return;
        }
        Node wide { };
        wide.type = NodeType::Int64;
        storeToSlot(reg, wide, transfer.stackArgumentOffset);
    };
    if (isRegisterLocation(transfer.from)) {
        if (isRegisterLocation(transfer.to))
            moveRegister(decode(transfer.from), decode(transfer.to));
        else
            storeTo(decode(transfer.from));
        return;
    }
    if (transfer.from == locationInSlot) {
        if (isRegisterLocation(transfer.to)) {
            loadFromSlot(decode(transfer.to), source, offsetOfSlotOf(transfer.source));
            return;
        }
        Reg scratch(jit.scratchRegister());
        Node raw = source;
        if (raw.type == NodeType::Double)
            raw.type = NodeType::Int64;
        loadFromSlot(scratch, raw, offsetOfSlotOf(transfer.source));
        storeTo(scratch);
        return;
    }
    if (isRegisterLocation(transfer.to)) {
        materialize(transfer.source, decode(transfer.to));
        return;
    }
    if (!m_context.frameIsSetUp) {
        fail();
        return;
    }
    bool isStackArgument = transfer.to != locationInSlot;
    int32_t offset = isStackArgument ? transfer.stackArgumentOffset : offsetOfSlotOf(transfer.destination);
    bool storesWord = isStackArgument || destination.type != NodeType::Int32 || destination.has(IsReadWide);
    if (source.form == Form::Constant) {
        int64_t constant = source.type == NodeType::Int32 && storesWord ? static_cast<int64_t>(static_cast<uint32_t>(source.constant)) : source.constant;
        if (fitsInt32(constant)) {
            if (storesWord)
                jit.store64(TrustedImm32(static_cast<int32_t>(constant)), addressOfSlot(offset));
            else
                jit.store32(TrustedImm32(static_cast<int32_t>(constant)), addressOfSlot(offset));
            finishUnit(kindOfFrameAccess(offset));
            return;
        }
        jit.move(TrustedImm64(constant), jit.scratchRegister());
        finishUnit();
        jit.store64(jit.scratchRegister(), addressOfSlot(offset));
        finishUnit(kindOfFrameAccess(offset));
        return;
    }
    materialize(transfer.source, Reg(jit.scratchRegister()));
    jit.store64(jit.scratchRegister(), addressOfSlot(offset));
    finishUnit(kindOfFrameAccess(offset));
}

void LinearScanCodeGenerator::emitParallelMoves(Vector<Transfer, 8>& transfers)
{
    auto keyOfSource = [&](const Transfer& transfer) -> int64_t {
        if (isRegisterLocation(transfer.from))
            return transfer.from;
        if (transfer.from == locationInSlot)
            return 1000 - static_cast<int64_t>(offsetOfSlotOf(transfer.source));
        return 0;
    };
    auto keyOfDestination = [&](const Transfer& transfer) -> int64_t {
        if (isRegisterLocation(transfer.to))
            return transfer.to;
        if (transfer.to == locationInSlot)
            return 1000 - static_cast<int64_t>(offsetOfSlotOf(transfer.destination));
        return -1;
    };
    auto isStoredWhole = [&](NodeID id) {
        return node(id).type != NodeType::Int32 || node(id).has(IsReadWide);
    };
    for (Transfer& transfer : transfers) {
        bool mustBeWidenedInPlace = transfer.from == locationInSlot && transfer.to == locationInSlot && !isStoredWhole(transfer.source) && isStoredWhole(transfer.destination);
        transfer.isDone = keyOfSource(transfer) == keyOfDestination(transfer) && !mustBeWidenedInPlace;
    }
    for (;;) {
        bool madeProgress = false;
        bool hasPendingMove = false;
        for (Transfer& transfer : transfers) {
            if (transfer.isDone)
                continue;
            int64_t destination = keyOfDestination(transfer);
            bool destinationIsRead = false;
            for (const Transfer& other : transfers)
                destinationIsRead |= !other.isDone && &other != &transfer && keyOfSource(other) == destination;
            if (destinationIsRead) {
                hasPendingMove = true;
                continue;
            }
            emitTransfer(transfer);
            transfer.isDone = true;
            madeProgress = true;
        }
        if (!hasPendingMove || m_hasFailed)
            return;
        if (madeProgress)
            continue;
        bool didBreakCycle = false;
        for (Transfer& transfer : transfers) {
            if (transfer.isDone || !isRegisterLocation(transfer.from) || !isRegisterLocation(transfer.to))
                continue;
            Reg source = decode(transfer.from);
            Reg destination = decode(transfer.to);
            if (source.isGPR() != destination.isGPR())
                continue;
            if (source.isGPR())
                m_jit->swap(source.gpr(), destination.gpr());
            else
                m_jit->swapDouble(source.fpr(), destination.fpr());
            finishUnit();
            for (Transfer& other : transfers) {
                if (other.isDone)
                    continue;
                if (other.from == transfer.to)
                    other.from = encode(source);
                else if (other.from == encode(source))
                    other.from = encode(destination);
                other.isDone = keyOfSource(other) == keyOfDestination(other);
            }
            didBreakCycle = true;
            break;
        }
        if (didBreakCycle)
            continue;
        for (Transfer& transfer : transfers) {
            if (transfer.isDone || transfer.to != locationInSlot)
                continue;
            int64_t destination = keyOfDestination(transfer);
            Reg saved(MacroAssembler::fpTempRegister);
            bool holdsNarrowValue = false;
            for (const Transfer& other : transfers)
                holdsNarrowValue |= !other.isDone && keyOfSource(other) == destination && node(other.source).type == NodeType::Int32;
            int32_t offset = offsetOfSlotOf(transfer.destination);
            if (holdsNarrowValue) {
                m_jit->loadFloat(addressOfSlot(offset), saved.fpr());
                finishUnit(kindOfFrameAccess(offset));
            } else {
                Node raw { };
                raw.type = NodeType::Double;
                loadFromSlot(saved, raw, offset);
            }
            for (Transfer& other : transfers) {
                if (!other.isDone && keyOfSource(other) == destination)
                    other.from = encode(saved);
            }
            didBreakCycle = true;
            break;
        }
        if (!didBreakCycle) {
            fail();
            return;
        }
    }
}

void LinearScanCodeGenerator::emitMovesAt(Position position)
{
    if (m_moveCursor >= m_moves.size() || m_moves[m_moveCursor].position != position)
        return;
    Vector<Transfer, 8> transfers;
    for (; m_moveCursor < m_moves.size() && m_moves[m_moveCursor].position == position; ++m_moveCursor) {
        const Move& move = m_moves[m_moveCursor];
        transfers.append(Transfer { move.to, move.from, move.source, move.destination, 0, false });
    }
    emitParallelMoves(transfers);
}

void LinearScanCodeGenerator::emitMoves(uint32_t first, uint32_t count, NodeID skippedDestination)
{
    if (!count)
        return;
    Vector<Transfer, 8> transfers;
    for (uint32_t index = 0; index < count; ++index) {
        const Move& move = m_movesOnEdges[first + index];
        if (skippedDestination && move.destination == skippedDestination)
            continue;
        transfers.append(Transfer { move.to, move.from, move.source, move.destination, 0, false });
    }
    emitParallelMoves(transfers);
}

void LinearScanCodeGenerator::emitEdgeMoves(uint32_t edge)
{
    emitMoves(m_edges[edge].first, m_edges[edge].count);
}

bool LinearScanCodeGenerator::hasOneLiveSuccessor(const Block& block) const
{
    return block.numberOfSuccessors == 1 || block.onlySuccessor || block.branchesToOneBlock;
}

bool LinearScanCodeGenerator::edgeMovesLieAtStartOfTarget(uint32_t edge) const
{
    if (!m_edges[edge].count)
        return false;
    const Block& target = m_blocks[m_successors[edge]];
    if (target.numberOfPredecessors != 1)
        return false;
    const Block& source = m_blocks[target.predecessor];
    return !hasOneLiveSuccessor(source) && !edgeLeavesFrame(edge, source);
}

bool LinearScanCodeGenerator::edgeHasMovesOnIt(uint32_t source, unsigned successorIndex) const
{
    uint32_t edge = m_blocks[source].firstSuccessor + successorIndex;
    if (edgeLeavesFrame(edge, m_blocks[source]))
        return true;
    if (edgeEntersFrame(edge, m_blocks[source]) && !m_blocks[m_successors[edge]].setsUpFrame)
        return true;
    return m_edges[edge].count && !edgeMovesLieAtStartOfTarget(edge);
}

bool LinearScanCodeGenerator::isNext(uint32_t target, uint32_t from) const
{
    ASSERT_UNUSED(from, from == m_currentBlock);
    return target == m_blockThatFollows;
}

void LinearScanCodeGenerator::link(Jump jump, uint32_t target)
{
    Block& block = m_blocks[target];
    if (block.label.isSet()) {
        jump.linkTo(block.label, m_jit);
        return;
    }
    block.jumps.append(jump);
    if (m_layout != Layout::RareCodeInGaps || !block.region)
        return;
    RareRegion& region = m_rareRegions[block.region];
    if (!region.isWritten && !region.numberOfJumps++) {
        region.offsetOfFirstJump = bytesSoFar();
        m_waitingRegions.append(block.region);
    }
}

void LinearScanCodeGenerator::findEdgesWithSameMoves()
{
    constexpr unsigned maxEdgesCompared = 16;
    constexpr unsigned maxMovesCompared = 8;
    auto isSameSource = [&](NodeID a, NodeID b) {
        if (a == b)
            return true;
        const Node& first = node(a);
        const Node& second = node(b);
        return first.form == Form::Constant && second.form == Form::Constant && first.type == second.type && first.constant == second.constant;
    };
    auto hasSameMoves = [&](const EdgeMoves& a, const EdgeMoves& b) {
        if (a.count != b.count)
            return false;
        for (uint32_t i = 0; i < a.count; ++i) {
            const Move& wanted = m_movesOnEdges[a.first + i];
            bool isFound = false;
            for (uint32_t j = 0; j < b.count && !isFound; ++j) {
                const Move& other = m_movesOnEdges[b.first + j];
                isFound = wanted.destination == other.destination && wanted.from == other.from && wanted.to == other.to && isSameSource(wanted.source, other.source);
            }
            if (!isFound)
                return false;
        }
        return true;
    };
    struct Candidate {
        uint32_t edge;
        bool runsInFrame;
    };
    for (const Block& target : m_blocks) {
        if (!target.isReachable || target.numberOfPredecessors < 2)
            continue;
        Vector<Candidate, maxEdgesCompared> candidates;
        for (unsigned index = 0; index < target.numberOfPredecessors; ++index) {
            uint32_t sender = m_predecessors[target.firstPredecessor + index];
            const Block& source = m_blocks[sender];
            uint32_t edge = m_edgeOfPredecessor[target.firstPredecessor + index];
            if (!source.isReachable || !source.isRare || (source.onlySuccessor && source.firstSuccessor + source.onlySuccessor - 1 != edge))
                continue;
            if (!m_successorDecidedOnEdge.isEmpty() && m_successorDecidedOnEdge[edge])
                continue;
            if (!m_edges[edge].count || m_edges[edge].count > maxMovesCompared || !edgeHasMovesOnIt(sender, edge - source.firstSuccessor))
                continue;
            bool isFound = false;
            for (const Candidate& candidate : candidates) {
                if (candidate.edge == edge || candidate.runsInFrame != source.runsInFrame || !hasSameMoves(m_edges[edge], m_edges[candidate.edge]))
                    continue;
                m_edges[edge].edgeWithSameMoves = candidate.edge + 1;
                m_edges[candidate.edge].isShared = true;
                isFound = true;
                break;
            }
            if (!isFound && candidates.size() < maxEdgesCompared)
                candidates.append(Candidate { edge, source.runsInFrame });
        }
    }
}

uint32_t LinearScanCodeGenerator::stubFor(uint32_t source, unsigned successorIndex)
{
    uint32_t edge = m_blocks[source].firstSuccessor + successorIndex;
    if (m_edges[edge].edgeWithSameMoves)
        edge = m_edges[edge].edgeWithSameMoves - 1;
    if (!m_edges[edge].stub) {
        if (m_numberOfEdgeStubsWritten == m_edgeStubs.size())
            m_offsetOfFirstWaitingEdgeStub = bytesSoFar();
        m_edgeStubs.append(EdgeStub { edge, source, targetOf(m_blocks[source], successorIndex), { }, nullptr });
        m_edges[edge].stub = m_edgeStubs.size();
    }
    return m_edges[edge].stub - 1;
}

void LinearScanCodeGenerator::linkEdge(Jump jump, uint32_t source, unsigned successorIndex)
{
    if (edgeHasMovesOnIt(source, successorIndex)) {
        EdgeStub& stub = m_edgeStubs[stubFor(source, successorIndex)];
        if (stub.label.isSet())
            jump.linkTo(stub.label, m_jit);
        else
            stub.jumps.append(jump);
        return;
    }
    link(jump, targetOf(m_blocks[source], successorIndex));
}

bool LinearScanCodeGenerator::canPassMerge(uint32_t edge, const Block& source) const
{
    const Block& merge = m_blocks[m_successors[edge]];
    const Block& target = m_blocks[successorOf(merge, m_successorDecidedOnEdge[edge] - 1)];
    if (merge.numberOfMovesAtStart || merge.setsUpFrame || target.setsUpFrame)
        return false;
    if (m_hasFrame && (source.runsInFrame != merge.runsInFrame || merge.runsInFrame != target.runsInFrame))
        return false;
    return !(merge.firstMove < m_moves.size() && m_moves[merge.firstMove].position < merge.to());
}

void LinearScanCodeGenerator::jumpToEdge(uint32_t source, unsigned successorIndex)
{
    uint32_t edge = m_blocks[source].firstSuccessor + successorIndex;
    if (!m_successorDecidedOnEdge.isEmpty() && m_successorDecidedOnEdge[edge]) {
        uint32_t merge = m_successors[edge];
        unsigned decided = m_successorDecidedOnEdge[edge] - 1;
        emitMoves(m_edges[edge].first, m_edges[edge].count, m_blocks[merge].testedPhi);
        if (edgeHasMovesOnIt(merge, decided))
            emitEdgeMoves(m_blocks[merge].firstSuccessor + decided);
        uint32_t target = targetOf(m_blocks[merge], decided);
        m_codeIsReachable = isNext(target, source);
        if (m_codeIsReachable)
            return;
        link(m_jit->jump(), target);
        finishUnit();
        return;
    }
    uint32_t target = targetOf(m_blocks[source], successorIndex);
    if (m_edges[edge].isShared || m_edges[edge].edgeWithSameMoves) {
        EdgeStub& stub = m_edgeStubs[stubFor(source, successorIndex)];
        if (stub.label.isSet()) {
            m_jit->jump().linkTo(stub.label, m_jit);
            finishUnit();
            m_codeIsReachable = false;
            return;
        }
        finishUnit();
        stub.jumps.link(m_jit);
        stub.label = m_jit->label();
        if (stub.sharedLabel)
            *stub.sharedLabel = stub.label;
    }
    if (edgeHasMovesOnIt(source, successorIndex)) {
        if (edgeEntersFrame(edge, m_blocks[source])) {
            emitFrameSetup();
            emitEdgeMoves(edge);
            m_codeIsReachable = !m_blocks[target].setsUpFrame && isNext(target, source);
            if (m_codeIsReachable)
                return;
            linkInsideFrame(m_jit->jump(), target);
            finishUnit();
            return;
        }
        emitEdgeMoves(edge);
        if (edgeLeavesFrame(edge, m_blocks[source]))
            emitFrameTeardownOnEdge();
    }
    m_codeIsReachable = isNext(target, source);
    if (m_codeIsReachable)
        return;
    link(m_jit->jump(), target);
    finishUnit();
}

Box<MacroAssembler::Label> LinearScanCodeGenerator::sharedLabelOf(Block& block)
{
    if (!block.sharedLabel) {
        block.sharedLabel = Box<Label>::create();
        if (block.label.isSet())
            *block.sharedLabel = block.label;
    }
    return block.sharedLabel;
}

Box<MacroAssembler::Label> LinearScanCodeGenerator::sharedLabelOfEdge(uint32_t source, unsigned successorIndex)
{
    if (!edgeHasMovesOnIt(source, successorIndex))
        return sharedLabelOf(m_blocks[targetOf(m_blocks[source], successorIndex)]);
    EdgeStub& stub = m_edgeStubs[stubFor(source, successorIndex)];
    if (!stub.sharedLabel) {
        stub.sharedLabel = Box<Label>::create();
        if (stub.label.isSet())
            *stub.sharedLabel = stub.label;
    }
    return stub.sharedLabel;
}

void LinearScanCodeGenerator::linkInsideFrame(Jump jump, uint32_t target)
{
    Block& block = m_blocks[target];
    if (!block.setsUpFrame) {
        link(jump, target);
        return;
    }
    if (block.labelInsideFrame.isSet())
        jump.linkTo(block.labelInsideFrame, m_jit);
    else
        block.jumpsFromInsideFrame.append(jump);
}

void LinearScanCodeGenerator::emitEdgeStubs()
{
    for (unsigned& index = m_numberOfEdgeStubsWritten; index < m_edgeStubs.size() && !m_hasFailed; ++index) {
        EdgeStub& stub = m_edgeStubs[index];
        if (stub.label.isSet())
            continue;
        const Block& source = m_blocks[stub.source];
        const Block& target = m_blocks[stub.target];
        BlockColdness coldness = m_coldness->of(std::min(source.frequency, target.frequency), !source.isReachedNormally || !target.isReachedNormally);
        m_records->units.willGenerateBlock(coldness.isCold, coldness.isRarelyRun);
        m_unitBegin = m_jit->labelIgnoringWatchpoints();
        m_context.setFrameIsSetUp(*m_jit, m_hasFrame && source.runsInFrame);
        stub.jumps.link(m_jit);
        stub.label = m_jit->label();
        if (stub.sharedLabel)
            *stub.sharedLabel = stub.label;
        if (edgeEntersFrame(stub.edge, source)) {
            emitFrameSetup();
            emitEdgeMoves(stub.edge);
            linkInsideFrame(m_jit->jump(), stub.target);
            finishUnit();
            continue;
        }
        emitEdgeMoves(stub.edge);
        if (edgeLeavesFrame(stub.edge, source))
            emitFrameTeardownOnEdge();
        link(m_jit->jump(), stub.target);
        finishUnit();
    }
}

void LinearScanCodeGenerator::orderEmission()
{
    uint32_t cursor = 0;
    for (uint32_t order = 0; order < m_blocks.size(); ++order) {
        Block& block = m_blocks[order];
        while (cursor < m_moves.size() && m_moves[cursor].position < block.from())
            ++cursor;
        block.firstMove = cursor;
        if (!block.isReachable || !order)
            continue;
        bool hasMoves = cursor < m_moves.size() && m_moves[cursor].position < block.to();
        if (hasMoves || !hasOneLiveSuccessor(block))
            continue;
        unsigned successor = block.onlySuccessor ? block.onlySuccessor - 1 : 0;
        if (m_edges[block.firstSuccessor + successor].count || block.numberOfMovesAtStart || block.setsUpFrame || edgeLeavesFrame(block.firstSuccessor + successor, block) || edgeEntersFrame(block.firstSuccessor + successor, block))
            continue;
        if (block.numberOfPredecessors == 1 && edgeMovesLieAtStartOfTarget(block.incomingEdge))
            continue;
        bool hasCode = false;
        for (uint32_t slot = block.firstSlot; slot < block.endSlot && !hasCode; ++slot) {
            const Node& n = node(m_sequence[slot]);
            if (n.form != Form::Plain)
                continue;
            switch (n.opcode) {
            case B3::Jump:
            case B3::Branch:
            case B3::Extract:
                break;
            case B3::Upsilon:
                hasCode = node(n.b).has(ArrivesInSlot);
                break;
            case B3::Phi:
                hasCode = n.has(ArrivesInSlot);
                break;
            default:
                hasCode = true;
                break;
            }
        }
        uint32_t target = successorOf(block, successor);
        if (!hasCode && target != order)
            block.forwardsTo = target;
    }
    for (Block& block : m_blocks) {
        unsigned hops = 0;
        for (uint32_t target = block.forwardsTo; target != UINT32_MAX; target = m_blocks[target].forwardsTo) {
            if (++hops > m_blocks.size()) {
                block.forwardsTo = UINT32_MAX;
                break;
            }
        }
    }
    if (!m_successorDecidedOnEdge.isEmpty()) {
        for (Block& merge : m_blocks) {
            if (!merge.testedPhi || !merge.isReachable)
                continue;
            bool isOnlyReachedRarely = !merge.isRare;
            for (unsigned index = 0; index < merge.numberOfPredecessors; ++index) {
                const Block& sender = m_blocks[m_predecessors[merge.firstPredecessor + index]];
                uint32_t edge = m_edgeOfPredecessor[merge.firstPredecessor + index];
                if (!sender.isReachable)
                    continue;
                if (m_successorDecidedOnEdge[edge] && !canPassMerge(edge, sender))
                    m_successorDecidedOnEdge[edge] = 0;
                isOnlyReachedRarely &= m_successorDecidedOnEdge[edge] || sender.isRare;
            }
            merge.isEmittedLate = isOnlyReachedRarely;
        }
    }
    m_emissionOrder.reserveInitialCapacity(m_blocks.size());
    Vector<uint32_t, 4> late;
    auto append = [&](uint32_t order) {
        m_emissionOrder.append(order);
    };
    auto isWritten = [&](const Block& block) {
        return block.isReachable && block.forwardsTo == UINT32_MAX;
    };
    switch (m_layout) {
    case Layout::RareCodeAtEnd:
        break;
    case Layout::AsControlFlows:
        for (uint32_t order : m_reversePostOrder) {
            if (isWritten(m_blocks[order]))
                append(order);
        }
        return;
    case Layout::RareCodeInGaps:
        // Only the usual way has its order now. emitRareCodeInFrontOf() places the rest while the code is written, when distances are known.
        for (uint32_t order = 0; order < m_blocks.size(); ++order) {
            const Block& block = m_blocks[order];
            if (block.region >= m_rareRegions.size())
                m_rareRegions.grow(block.region + 1);
            if (!isWritten(block))
                continue;
            if (!block.region) {
                append(order);
                continue;
            }
            RareRegion& region = m_rareRegions[block.region];
            if (!region.numberOfSlots)
                region.firstBlock = order;
            region.endBlock = order + 1;
            region.numberOfSlots += std::max<uint32_t>(block.endSlot - block.firstSlot, 1);
        }
        return;
    }
    for (uint32_t order = 0; order < m_blocks.size(); ++order) {
        Block& block = m_blocks[order];
        if (!block.isReachable || block.forwardsTo != UINT32_MAX)
            continue;
        if (block.isEmittedLate) {
            late.append(order);
            continue;
        }
        if (block.isRare) {
            for (uint32_t waiting : std::exchange(late, { }))
                append(waiting);
        }
        append(order);
    }
    for (uint32_t waiting : late)
        append(waiting);
}

void LinearScanCodeGenerator::emitRareRegion(uint32_t index, uint32_t blockThatFollows)
{
    RareRegion& region = m_rareRegions[index];
    if (region.isWritten)
        return;
    region.isWritten = true;
    uint32_t pending = UINT32_MAX;
    auto emitPending = [&](uint32_t next) {
        if (pending == UINT32_MAX || m_hasFailed)
            return;
        m_blockThatFollows = next;
        emitBlock(m_blocks[pending]);
    };
    for (uint32_t order = region.firstBlock; order < region.endBlock; ++order) {
        const Block& block = m_blocks[order];
        if (!block.isReachable || block.forwardsTo != UINT32_MAX)
            continue;
        emitPending(order);
        pending = order;
    }
    emitPending(blockThatFollows);
}

// A short jump reaches 127 bytes. Rare code that waits is written where nothing falls into the next block, and, where the policy asks for it,
// behind a jump over it at the last boundary from which the jumps to it are still short.
void LinearScanCodeGenerator::emitRareCodeInFrontOf(uint32_t next)
{
    constexpr size_t reach = 127;
    constexpr size_t bytesOfJumpOver = 5;
    constexpr size_t bytesExpectedOfSlot = 6;
    constexpr uint32_t maxSlotsInGap = 48;
    while (m_numberOfEdgeStubsWritten < m_edgeStubs.size() && m_edgeStubs[m_numberOfEdgeStubsWritten].label.isSet())
        ++m_numberOfEdgeStubsWritten;
    bool hasWaitingStubs = m_numberOfEdgeStubsWritten < m_edgeStubs.size();
    if (m_waitingRegions.isEmpty() && !hasWaitingStubs)
        return;
    size_t now = bytesSoFar();
    bool makesGap = m_codeIsReachable;
    if (makesGap) {
        const Block& block = m_blocks[next];
        if (!m_code.policy().layout.makesGapsForRareBlocks || block.loop)
            return;
        size_t start = now + bytesOfJumpOver;
        size_t oldest = SIZE_MAX;
        if (hasWaitingStubs && start - m_offsetOfFirstWaitingEdgeStub <= reach)
            oldest = m_offsetOfFirstWaitingEdgeStub;
        for (uint32_t index : m_waitingRegions) {
            const RareRegion& region = m_rareRegions[index];
            if (region.numberOfSlots <= maxSlotsInGap && start - region.offsetOfFirstJump <= reach)
                oldest = std::min(oldest, region.offsetOfFirstJump);
        }
        if (oldest == SIZE_MAX || start - oldest + (block.endSlot - block.firstSlot) * bytesExpectedOfSlot <= reach)
            return;
        m_blocks[next].jumps.append(m_jit->jump());
        finishUnit();
        m_codeIsReachable = false;
    }
    emitEdgeStubs();
    Vector<uint32_t, 8> regions;
    m_waitingRegions.removeAllMatching([&](uint32_t index) {
        if (m_rareRegions[index].numberOfSlots > maxSlotsInGap)
            return false;
        regions.append(index);
        return true;
    });
    // The last of them may fall into the block that follows.
    for (unsigned index = 0; index + 1 < regions.size(); ++index) {
        const Block& last = m_blocks[m_rareRegions[regions[index]].endBlock - 1];
        bool leadsToNext = false;
        for (unsigned successor = 0; successor < last.numberOfSuccessors; ++successor)
            leadsToNext |= targetOf(last, successor) == next;
        if (leadsToNext) {
            std::swap(regions[index], regions.last());
            break;
        }
    }
    for (unsigned index = 0; index < regions.size() && !m_hasFailed; ++index)
        emitRareRegion(regions[index], index + 1 == regions.size() ? next : UINT32_MAX);
}

bool LinearScanCodeGenerator::generate(CCallHelpers& jit, CodeRecords& records, const BlockColdnessRule& coldness)
{
    layOutFrame();
    chooseWhereToSetUpFrame();
    findEdgesWithSameMoves();
    orderEmission();
    m_jit = &jit;
    m_records = &records;
    m_coldness = &coldness;
    AllowMacroScratchRegisterUsage allowScratch(jit);

    m_context.code = &m_code;
    m_context.frameIsSetUp = false;

    Label entry = jit.label();
    BlockColdness entryColdness = coldness.of(1, false);
    records.units.willGenerateBlock(entryColdness.isCold, entryColdness.isRarelyRun);
    m_unitBegin = jit.labelIgnoringWatchpoints();
    if (m_hasFrame && m_setsUpFrameAtEntry)
        emitFrameSetup();

    for (size_t index = 0; index < m_emissionOrder.size(); ++index) {
        if (m_layout == Layout::RareCodeInGaps && index)
            emitRareCodeInFrontOf(m_emissionOrder[index]);
        if (m_hasFailed)
            return false;
        m_blockThatFollows = index + 1 < m_emissionOrder.size() ? m_emissionOrder[index + 1] : UINT32_MAX;
        emitBlock(m_blocks[m_emissionOrder[index]]);
        if (m_hasFailed)
            return false;
    }
    for (uint32_t region = 1; region < m_rareRegions.size() && !m_hasFailed; ++region)
        emitRareRegion(region, UINT32_MAX);
    if (m_hasFailed)
        return false;
    emitDispatchesOfLeavingTests();
    emitEdgeStubs();
    if (m_hasFailed)
        return false;
    for (const Block& block : m_blocks) {
        if (!block.label.isSet() && !block.jumps.empty()) {
            fail();
            return false;
        }
    }
    m_code.setNumEntrypoints(1);
    m_code.setEntrypointLabels(Vector<Label> { entry });
    if (m_procedure.needsPCToOriginMap()) [[unlikely]]
        m_procedure.pcToOriginMap().appendItem(jit.labelIgnoringWatchpoints(), B3::Origin());

    for (unsigned index = 0; index < m_context.latePaths.size(); ++index) {
        Ref<B3::Air::GenerationContext::LatePath> latePath = std::get<1>(m_context.latePaths[index]);
        m_context.setFrameIsSetUp(jit, m_hasFrame && !m_context.latePathsWithoutFrame.get(index));
        latePath->run(jit, m_context);
        noteNewLatePaths();
    }
    m_context.setFrameIsSetUp(jit, false);
    m_code.setFrameRanges(WTF::move(m_context.frameRanges));
    if (m_procedure.needsPCToOriginMap()) [[unlikely]]
        m_procedure.pcToOriginMap().appendItem(jit.labelIgnoringWatchpoints(), B3::Origin());
    return true;
}

void LinearScanCodeGeneratorDeleter::operator()(LinearScanCodeGenerator* generator) const
{
    delete generator;
}

DirectCodeAttempt attemptAfterFailure(const LinearScanCodeGenerator&)
{
    return DirectCodeAttempt::None;
}

LinearScanCodeGeneratorPointer planLinearScanCode(B3::Procedure& procedure, DirectCodeAttempt attempt, LoopsInDirectCode loops)
{
    LinearScanCodeGeneratorPointer generator(new LinearScanCodeGenerator(procedure, attempt));
    generator->setLoops(loops);
    if (!generator->plan())
        return nullptr;
    return generator;
}

bool generateLinearScanCode(LinearScanCodeGenerator& generator, CCallHelpers& jit, CodeRecords& records, const BlockColdnessRule& coldness)
{
    return generator.generate(jit, records, coldness);
}

} } } // namespace JSC::AOT::Typed

#elif ENABLE(AOT)

namespace JSC { namespace AOT { namespace Typed {

class LinearScanCodeGenerator { };

void LinearScanCodeGeneratorDeleter::operator()(LinearScanCodeGenerator* generator) const { delete generator; }
LinearScanCodeGeneratorPointer planLinearScanCode(B3::Procedure&, DirectCodeAttempt, LoopsInDirectCode) { return nullptr; }
DirectCodeAttempt attemptAfterFailure(const LinearScanCodeGenerator&) { return DirectCodeAttempt::None; }
bool generateLinearScanCode(LinearScanCodeGenerator&, CCallHelpers&, CodeRecords&, const BlockColdnessRule&) { return false; }

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT) && (CPU(X86_64) || CPU(ARM64)) && !OS(WINDOWS)
