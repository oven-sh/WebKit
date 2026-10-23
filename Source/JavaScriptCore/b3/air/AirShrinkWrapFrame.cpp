/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AirShrinkWrapFrame.h"

#if ENABLE(B3_JIT)

#include "AirBlockInsertionSet.h"
#include "AirCode.h"
#include "AirFrameStates.h"
#include "AirInsertionSet.h"
#include "AirInstInlines.h"
#include "AirOpcodeUtils.h"
#include "AirPhaseScope.h"
#include "AirRegLiveness.h"
#include "AirStackColoring.h"
#include "B3Common.h"
#include "B3Procedure.h"
#include "B3StackmapValue.h"
#include <wtf/BitVector.h>
#include <wtf/MathExtras.h>

namespace JSC { namespace B3 { namespace Air {

namespace {

static_assert(Reg::maxIndex() < 64);

uint64_t bitOf(Reg reg)
{
    return static_cast<uint64_t>(1) << reg.index();
}

uint64_t bitsOf(const RegisterSet& registers)
{
    uint64_t result = 0;
    registers.forEach([&](Reg reg) {
        result |= bitOf(reg);
    });
    return result;
}

bool takesSlotAddress(Inst& inst)
{
    bool result = false;
    inst.forEachArg([&](Arg& arg, Arg::Role role, Bank, Width) {
        result |= role == Arg::UseAddr && arg.isStack();
    });
    return result;
}

void addressIncomingArgumentsThroughStackPointer(Inst& inst)
{
    for (Arg& arg : inst.args()) {
        if (isIncomingArgumentAddress(arg))
            arg = Arg::addr(Tmp(MacroAssembler::stackPointerRegister), arg.offset() - static_cast<Value::OffsetType>(prologueStackPointerDelta()));
    }
}

std::optional<FrameCoverage> findReasonOutsideCodeToCoverEveryPath(Code& code)
{
    if (!code.policy().frame.shrinkWraps)
        return FrameCoverage::EveryPathBecauseShrinkWrappingIsOff;
    if (code.proc().numEntrypoints() > 1)
        return FrameCoverage::EveryPathBecauseOfSeveralEntrypoints;
    return std::nullopt;
}

class FrameShrinkWrapper {
public:
    FrameShrinkWrapper(Code&, const StackColoring&);
    FramePlacement run();
    std::optional<BitVector> findRegionBeforeAllocation();
    std::optional<BitVector> growRegionBeforeAllocation(const BitVector& region, const BitVector& crowdedBlocks);
    std::optional<BitVector> findRegionFromAllLocalNeeds(const BitVector& crowdedBlocks);

private:
    struct SplitEdge {
        BasicBlock* from { nullptr };
        BasicBlock* block { nullptr };
    };

    bool isInRegion(BasicBlock* block) const { return m_isInRegion.quickGet(block->index()); }
    bool addToRegion(BasicBlock* block) { return !m_isInRegion.quickSet(block->index()); }
    bool hasRead(BasicBlock* block) const { return m_hasRead.quickGet(block->index()); }
    void findLocalNeeds();
    void findLocalNeedsOf(BasicBlock*);
    bool findPathWithoutLocalNeeds();
    std::optional<BitVector> closeRegionBeforeAllocation();
    uint64_t computeFrameSizeBound() const;
    std::optional<FrameCoverage> findReasonToCoverEveryPath(bool isFrameLarge) const;
    void coverEveryPath();
    void addBlocksWithLiveCalleeSaves(const RegLiveness&);
    bool canSlotAddressEscape(BasicBlock*, uint64_t registersLiveAtTail) const;
    void addBlocksAfterAddressEscapes(const RegLiveness&);
    void findLikelyPaths();
    bool continuesLikelyPath(BasicBlock* from, const FrequentedBlock& edge) const;
    bool closeOverLikelyPaths();
    bool closeOverCertainPaths();
    bool growOverLiveScratchRegisters(const RegLiveness&);
    void closeRegion(const RegLiveness*);
    void rewriteIncomingArgumentAddresses();
    Inst createEnterFrame(Value* origin) const;
    Inst createLeaveFrame(Value* origin) const;
    BasicBlock* insertBlockBefore(BasicBlock* successor, double frequency, Inst&&);
    BasicBlock* splitEdge(BasicBlock* from, FrequentedBlock& edge, Inst&&);
    void insertOnOutgoingEdges(BasicBlock* from);
    bool shouldSetUpFrameAtHead(BasicBlock*) const;
    void insertSetups();
    void insertTeardowns();

    Code& m_code;
    const StackColoring& m_coloring;
    BlockInsertionSet m_blockInsertionSet;
    InsertionSet m_insertionSet;
    BitVector m_isInRegion;
    BitVector m_hasRead;
    BitVector m_isOnLikelyPath;
    BitVector m_addressesIncomingArguments;
    BitVector m_takesSlotAddress;
    BitVector m_setsUpFrameAtHead;
    Vector<SplitEdge> m_latestSplitByTarget;
    uint64_t m_savedCalleeSaves { 0 };
    uint64_t m_scratchRegisters { 0 };
    FramePlacement m_placement;
};

FrameShrinkWrapper::FrameShrinkWrapper(Code& code, const StackColoring& coloring)
    : m_code(code)
    , m_coloring(coloring)
    , m_blockInsertionSet(code)
    , m_insertionSet(code)
    , m_isInRegion(code.size())
    , m_hasRead(code.size())
    , m_isOnLikelyPath(code.size())
    , m_addressesIncomingArguments(code.size())
    , m_takesSlotAddress(code.size())
    , m_setsUpFrameAtHead(code.size())
{
}

FramePlacement FrameShrinkWrapper::run()
{
    BasicBlock* entry = m_code[0];
    m_savedCalleeSaves = savedCalleeSaveBits(m_code);
    m_scratchRegisters = bitsOf(m_code.policy().frame.registersClobberedByFrameSetup);
    findLocalNeeds();

    const bool isFrameLarge = computeFrameSizeBound() > m_code.policy().frame.maxBytesAllocatedBeforeStackCheck;
    std::optional<FrameCoverage> reasonToCoverEveryPath = findReasonToCoverEveryPath(isFrameLarge);
    if (!reasonToCoverEveryPath && m_isInRegion.isEmpty()) {
        m_placement.coverage = FrameCoverage::NoPath;
        rewriteIncomingArgumentAddresses();
        return WTF::move(m_placement);
    }

    std::optional<RegLiveness> liveness;
    if (reasonToCoverEveryPath) {
        m_placement.coverage = *reasonToCoverEveryPath;
        if (isFrameLarge)
            m_scratchRegisters |= bitsOf(RegisterSet::macroClobberedGPRs());
        coverEveryPath();
        if (isFrameLarge || (ASSERT_ENABLED && m_scratchRegisters))
            liveness.emplace(m_code);
    } else {
        if (m_savedCalleeSaves || m_scratchRegisters || !m_takesSlotAddress.isEmpty()) {
            liveness.emplace(m_code);
            addBlocksWithLiveCalleeSaves(*liveness);
            addBlocksAfterAddressEscapes(*liveness);
        }
        closeRegion(liveness ? &*liveness : nullptr);
        m_placement.coverage = isInRegion(entry) ? FrameCoverage::EveryPathNeedsFrame : FrameCoverage::SomePaths;
    }

    if (liveness && isInRegion(entry))
        RELEASE_ASSERT(!(bitsOf(liveness->liveAtHead(entry)) & m_scratchRegisters));

    rewriteIncomingArgumentAddresses();
    insertSetups();
    insertTeardowns();
    if (m_blockInsertionSet.execute()) {
        m_code.resetReachability();
        m_placement.didAddBlocks = true;
    }
    return WTF::move(m_placement);
}

std::optional<BitVector> FrameShrinkWrapper::findRegionBeforeAllocation()
{
    if (!findPathWithoutLocalNeeds())
        return std::nullopt;
    return findRegionFromAllLocalNeeds({ });
}

std::optional<BitVector> FrameShrinkWrapper::findRegionFromAllLocalNeeds(const BitVector& crowdedBlocks)
{
    findLocalNeeds();
    m_isInRegion.merge(crowdedBlocks);
    return closeRegionBeforeAllocation();
}

std::optional<BitVector> FrameShrinkWrapper::growRegionBeforeAllocation(const BitVector& region, const BitVector& crowdedBlocks)
{
    m_isInRegion.merge(region);
    m_isInRegion.merge(crowdedBlocks);
    return closeRegionBeforeAllocation();
}

std::optional<BitVector> FrameShrinkWrapper::closeRegionBeforeAllocation()
{
    if (findReasonToCoverEveryPath(false))
        return std::nullopt;
    closeRegion(nullptr);
    if (isInRegion(m_code[0]))
        return std::nullopt;
    return WTF::move(m_isInRegion);
}

// Where every path from the entry block to an exit meets a local need, closeOverCertainPaths() adds the entry block. Only the blocks in front of those needs are read.
bool FrameShrinkWrapper::findPathWithoutLocalNeeds()
{
    Vector<BasicBlock*, 16> worklist;
    worklist.append(m_code[0]);
    while (!worklist.isEmpty()) {
        BasicBlock* block = worklist.takeLast();
        if (isInRegion(block) || hasRead(block))
            continue;
        findLocalNeedsOf(block);
        if (isInRegion(block))
            continue;
        if (!block->numSuccessors())
            return true;
        for (BasicBlock* successor : block->successorBlocks())
            worklist.append(successor);
    }
    return false;
}

void FrameShrinkWrapper::findLocalNeeds()
{
    for (BasicBlock* block : m_code) {
        if (!hasRead(block))
            findLocalNeedsOf(block);
    }
}

void FrameShrinkWrapper::findLocalNeedsOf(BasicBlock* block)
{
    m_hasRead.quickSet(block->index());
    const unsigned conservativeCallArgBytes = m_code.usesSIMD() ? conservativeRegisterBytes(Bank::FP) : conservativeRegisterBytesWithoutVectors(Bank::FP);
    for (Inst& inst : *block) {
        const bool isPatch = inst.kind.opcode == Patch;
        bool namesLockedSlot = false;
        for (Arg& arg : inst.args()) {
            if (arg.isCallArg()) {
                ASSERT(arg.offset() >= 0);
                m_code.requestCallArgAreaSizeInBytes(arg.offset() + conservativeCallArgBytes);
            } else if (arg.isStack())
                namesLockedSlot |= arg.stackSlot()->isLocked();
            else if (isPatch && isIncomingArgumentAddress(arg))
                m_addressesIncomingArguments.quickSet(block->index());
        }
        if (namesLockedSlot && takesSlotAddress(inst))
            m_takesSlotAddress.quickSet(block->index());

        FrameNeeds needs = frameNeedsOf(inst, m_savedCalleeSaves);
        if (needs.needsFrame || needs.namesStackPointer)
            addToRegion(block);
        else if (isPatch && inst.origin) {
            if (auto* stackmap = inst.origin->as<StackmapValue>(); stackmap && stackmap->followsFrameSetup())
                addToRegion(block);
        }
        if (needs.addressesIncomingArguments) {
            m_addressesIncomingArguments.quickSet(block->index());
            Inst copy = inst;
            addressIncomingArgumentsThroughStackPointer(copy);
            if (!copy.isValidForm(m_code.targetFeatures()))
                addToRegion(block);
        }
    }

    if (block->at(0).clobbersAtBlockEntry()) {
        for (BasicBlock* predecessor : block->predecessors())
            addToRegion(predecessor);
    }
    Inst& terminal = block->last();
    if (terminal.kind.opcode == Patch && terminal.needsFrame()) {
        for (BasicBlock* successor : block->successorBlocks())
            addToRegion(successor);
    }
}

uint64_t FrameShrinkWrapper::computeFrameSizeBound() const
{
    constexpr uint64_t wordSize = StackColoring::bytesPerWord;
    StackSlot* saveSlot = m_code.calleeSaveStackSlot();
    uint64_t bytes = saveSlot ? WTF::roundUpToMultipleOf<uint64_t>(stackAlignmentBytes(), saveSlot->byteSize()) : 0;

    intptr_t fixedSlotDepth = 0;
    for (StackSlot* slot : m_code.stackSlots()) {
        if (!slot->isLocked() || slot == saveSlot)
            continue;
        if (slot->offsetFromFP()) {
            fixedSlotDepth = std::max(fixedSlotDepth, -slot->offsetFromFP());
            continue;
        }
        bytes += WTF::roundUpToMultipleOf<uint64_t>(wordSize, slot->byteSize());
        if (slot->alignment() > wordSize)
            bytes += wordSize;
    }
    bytes += WTF::roundUpToMultipleOf<uint64_t>(stackAlignmentBytes(), fixedSlotDepth);

    bytes += wordSize * (m_coloring.numberOfWords() + 1);
    for (const std::optional<StackColoring::UnitRange>& units : m_coloring.unitsOfSlot) {
        if (units && units->first / StackColoring::unitsPerWord != units->last / StackColoring::unitsPerWord)
            bytes += wordSize;
    }

    bytes += m_code.callArgAreaSizeInBytes();
    return WTF::roundUpToMultipleOf<uint64_t>(stackAlignmentBytes(), bytes);
}

std::optional<FrameCoverage> FrameShrinkWrapper::findReasonToCoverEveryPath(bool isFrameLarge) const
{
    if (std::optional<FrameCoverage> reason = findReasonOutsideCodeToCoverEveryPath(m_code))
        return reason;
    if (!m_isInRegion.isEmpty() && isFrameLarge)
        return FrameCoverage::EveryPathBecauseFrameIsLarge;
    if (isInRegion(m_code[0]))
        return FrameCoverage::EveryPathNeedsFrame;
    return std::nullopt;
}

void FrameShrinkWrapper::coverEveryPath()
{
    for (BasicBlock* block : m_code)
        addToRegion(block);
}

void FrameShrinkWrapper::addBlocksWithLiveCalleeSaves(const RegLiveness& liveness)
{
    if (!m_savedCalleeSaves)
        return;
    for (BasicBlock* block : m_code) {
        if ((bitsOf(liveness.liveAtHead(block)) | bitsOf(liveness.liveAtTail(block))) & m_savedCalleeSaves)
            addToRegion(block);
    }
}

bool FrameShrinkWrapper::canSlotAddressEscape(BasicBlock* block, uint64_t registersLiveAtTail) const
{
    // A Patch may return an address it is given. It keeps none in memory once it is done.
    uint64_t addressRegisters = 0;
    for (Inst& inst : *block) {
        bool derivesAddress = false;
        bool writesMemory = false;
        uint64_t definedRegisters = 0;
        inst.forEachArg([&](Arg& arg, Arg::Role role, Bank bank, Width width) {
            derivesAddress |= role == Arg::UseAddr && arg.isStack();
            writesMemory |= Arg::isAnyDef(role) && arg.isMemory() && !arg.isCallArg();
            arg.forEachTmp(role, bank, width, [&](Tmp& tmp, Arg::Role tmpRole, Bank, Width) {
                if (!tmp.isReg())
                    return;
                derivesAddress |= Arg::isAnyUse(tmpRole) && (addressRegisters & bitOf(tmp.reg()));
                if (Arg::isAnyDef(tmpRole))
                    definedRegisters |= bitOf(tmp.reg());
            });
        });
        if (derivesAddress && writesMemory)
            return true;

        if (inst.kind.opcode == Patch)
            addressRegisters &= ~(bitsOf(inst.extraEarlyClobberedRegs()) | bitsOf(inst.extraClobberedRegs()));
        if (derivesAddress)
            addressRegisters |= definedRegisters;
        else
            addressRegisters &= ~definedRegisters;
    }
    return addressRegisters & registersLiveAtTail;
}

void FrameShrinkWrapper::addBlocksAfterAddressEscapes(const RegLiveness& liveness)
{
    Vector<BasicBlock*, 16> worklist;
    for (size_t blockIndex : m_takesSlotAddress) {
        BasicBlock* block = m_code[blockIndex];
        if (canSlotAddressEscape(block, bitsOf(liveness.liveAtTail(block))))
            worklist.append(block);
    }
    if (worklist.isEmpty())
        return;

    BitVector isReached(m_code.size());
    while (!worklist.isEmpty()) {
        for (BasicBlock* successor : worklist.takeLast()->successorBlocks()) {
            if (isReached.quickSet(successor->index()))
                continue;
            addToRegion(successor);
            worklist.append(successor);
        }
    }
}

void FrameShrinkWrapper::findLikelyPaths()
{
    Vector<BasicBlock*, 16> worklist;
    for (BasicBlock* block : m_code) {
        if (block == m_code[0] || m_code.isEntrypoint(block) || !block->numPredecessors()) {
            m_isOnLikelyPath.quickSet(block->index());
            worklist.append(block);
        }
    }
    while (!worklist.isEmpty()) {
        for (const FrequentedBlock& edge : worklist.takeLast()->successors()) {
            if (edge.isLikely() && !m_isOnLikelyPath.quickSet(edge.block()->index()))
                worklist.append(edge.block());
        }
    }
}

bool FrameShrinkWrapper::continuesLikelyPath(BasicBlock* from, const FrequentedBlock& edge) const
{
    return edge.isLikely() && (m_isOnLikelyPath.quickGet(from->index()) || !m_isOnLikelyPath.quickGet(edge.block()->index()));
}

bool FrameShrinkWrapper::closeOverLikelyPaths()
{
    Vector<BasicBlock*, 16> worklist;
    auto addRegionToWorklist = [&] {
        for (size_t blockIndex : m_isInRegion)
            worklist.append(m_code[blockIndex]);
    };

    BitVector isReachedFromRegion = m_isInRegion;
    addRegionToWorklist();
    while (!worklist.isEmpty()) {
        BasicBlock* block = worklist.takeLast();
        for (const FrequentedBlock& edge : block->successors()) {
            if (continuesLikelyPath(block, edge) && !isReachedFromRegion.quickSet(edge.block()->index()))
                worklist.append(edge.block());
        }
    }

    auto hasEdgeOnLikelyPath = [&](BasicBlock* from, BasicBlock* to) {
        for (const FrequentedBlock& edge : from->successors()) {
            if (edge.block() == to && continuesLikelyPath(from, edge))
                return true;
        }
        return false;
    };

    BitVector reachesRegion = m_isInRegion;
    addRegionToWorklist();
    while (!worklist.isEmpty()) {
        BasicBlock* block = worklist.takeLast();
        for (BasicBlock* predecessor : block->predecessors()) {
            if (reachesRegion.quickGet(predecessor->index()) || !hasEdgeOnLikelyPath(predecessor, block))
                continue;
            reachesRegion.quickSet(predecessor->index());
            worklist.append(predecessor);
        }
    }

    isReachedFromRegion.filter(reachesRegion);
    isReachedFromRegion.exclude(m_isInRegion);
    if (isReachedFromRegion.isEmpty())
        return false;
    m_isInRegion.merge(isReachedFromRegion);
    return true;
}

bool FrameShrinkWrapper::closeOverCertainPaths()
{
    BitVector reachesExitWithoutFrame(m_code.size());
    BitVector isReachedWithoutFrame(m_code.size());
    Vector<BasicBlock*, 16> worklist;
    auto visit = [&](BitVector& visited, BasicBlock* block) {
        if (!isInRegion(block) && !visited.quickSet(block->index()))
            worklist.append(block);
    };

    for (BasicBlock* block : m_code) {
        if (!block->numSuccessors())
            visit(reachesExitWithoutFrame, block);
    }
    while (!worklist.isEmpty()) {
        for (BasicBlock* predecessor : worklist.takeLast()->predecessors())
            visit(reachesExitWithoutFrame, predecessor);
    }

    for (BasicBlock* block : m_code) {
        if (block == m_code[0] || m_code.isEntrypoint(block) || !block->numPredecessors())
            visit(isReachedWithoutFrame, block);
    }
    while (!worklist.isEmpty()) {
        for (BasicBlock* successor : worklist.takeLast()->successorBlocks())
            visit(isReachedWithoutFrame, successor);
    }

    bool changed = false;
    for (BasicBlock* block : m_code) {
        if (!reachesExitWithoutFrame.quickGet(block->index()) || !isReachedWithoutFrame.quickGet(block->index()))
            changed |= addToRegion(block);
    }
    return changed;
}

bool FrameShrinkWrapper::growOverLiveScratchRegisters(const RegLiveness& liveness)
{
    Vector<BasicBlock*, 16> worklist;
    for (size_t blockIndex : m_isInRegion)
        worklist.append(m_code[blockIndex]);

    bool changed = false;
    while (!worklist.isEmpty()) {
        BasicBlock* block = worklist.takeLast();
        if (!(bitsOf(liveness.liveAtHead(block)) & m_scratchRegisters))
            continue;
        for (BasicBlock* predecessor : block->predecessors()) {
            if (!addToRegion(predecessor))
                continue;
            worklist.append(predecessor);
            changed = true;
        }
    }
    return changed;
}

void FrameShrinkWrapper::closeRegion(const RegLiveness* liveness)
{
    findLikelyPaths();
    bool changed;
    do {
        changed = closeOverLikelyPaths();
        changed |= closeOverCertainPaths();
        if (liveness && m_scratchRegisters)
            changed |= growOverLiveScratchRegisters(*liveness);
    } while (changed);
}

void FrameShrinkWrapper::rewriteIncomingArgumentAddresses()
{
    for (size_t blockIndex : m_addressesIncomingArguments) {
        BasicBlock* block = m_code[blockIndex];
        if (isInRegion(block)) {
            m_placement.addressesIncomingArgumentsInFrame = true;
            continue;
        }
        for (Inst& inst : *block) {
            if (inst.kind.opcode != Patch)
                addressIncomingArgumentsThroughStackPointer(inst);
        }
    }
}

Inst FrameShrinkWrapper::createEnterFrame(Value* origin) const
{
    Vector<Arg, 16> operands;
    operands.append(Arg::imm(std::popcount(m_scratchRegisters)));
    for (uint64_t registers = m_scratchRegisters; registers; registers &= registers - 1)
        operands.append(Tmp(Reg::fromIndex(std::countr_zero(registers))));
    for (const RegisterAtOffset& entry : m_code.calleeSaveRegisterAtOffsetList())
        operands.append(Tmp(entry.reg()));
    return Inst(EnterFrame, origin, operands.span());
}

Inst FrameShrinkWrapper::createLeaveFrame(Value* origin) const
{
    Vector<Arg, 16> operands;
    for (const RegisterAtOffset& entry : m_code.calleeSaveRegisterAtOffsetList())
        operands.append(Tmp(entry.reg()));
    return Inst(LeaveFrame, origin, operands.span());
}

BasicBlock* FrameShrinkWrapper::insertBlockBefore(BasicBlock* successor, double frequency, Inst&& inst)
{
    BasicBlock* block = m_blockInsertionSet.insertBefore(successor, frequency);
    Value* origin = inst.origin;
    block->appendInst(WTF::move(inst));
    block->append(Jump, origin);
    block->setSuccessors(FrequentedBlock(successor));
    return block;
}

BasicBlock* FrameShrinkWrapper::splitEdge(BasicBlock* from, FrequentedBlock& edge, Inst&& inst)
{
    BasicBlock* to = edge.block();
    BasicBlock* block = insertBlockBefore(to, std::min(from->frequency(), to->frequency()), WTF::move(inst));
    edge.block() = block;
    return block;
}

void FrameShrinkWrapper::insertOnOutgoingEdges(BasicBlock* from)
{
    const bool hasFrame = isInRegion(from);
    auto changesFrameState = [&](BasicBlock* to) {
        if (hasFrame)
            return !isInRegion(to);
        return isInRegion(to) && !m_setsUpFrameAtHead.quickGet(to->index());
    };
    auto createInst = [&](BasicBlock* to) {
        return hasFrame ? createLeaveFrame(from->last().origin) : createEnterFrame(to->at(0).origin);
    };
    auto recordSite = [&](BasicBlock* block, unsigned instIndex) {
        if (hasFrame)
            ++m_placement.numberOfTeardowns;
        else
            m_placement.setups.append(FrameSetupSite { block, instIndex });
    };

    if (from->last().kind.opcode == Jump) {
        BasicBlock* to = from->successorBlock(0);
        if (!changesFrameState(to))
            return;
        unsigned instIndex = from->size() - 1;
        m_insertionSet.insertInst(instIndex, createInst(to));
        m_insertionSet.execute(from);
        recordSite(from, instIndex);
        return;
    }

    for (FrequentedBlock& edge : from->successors()) {
        BasicBlock* to = edge.block();
        if (!changesFrameState(to))
            continue;
        if (m_latestSplitByTarget.isEmpty())
            m_latestSplitByTarget.grow(m_code.size());
        SplitEdge& latestSplit = m_latestSplitByTarget[to->index()];
        if (latestSplit.from == from) {
            edge.block() = latestSplit.block;
            continue;
        }
        latestSplit = { from, splitEdge(from, edge, createInst(to)) };
        recordSite(latestSplit.block, 0);
    }
}

bool FrameShrinkWrapper::shouldSetUpFrameAtHead(BasicBlock* block) const
{
    if (block == m_code[0])
        return !block->numPredecessors();
    for (BasicBlock* predecessor : block->predecessors()) {
        if (isInRegion(predecessor))
            return false;
    }
    return true;
}

void FrameShrinkWrapper::insertSetups()
{
    BasicBlock* entry = m_code[0];
    for (size_t blockIndex : m_isInRegion) {
        BasicBlock* block = m_code[blockIndex];
        Value* origin = block->at(0).origin;
        if (shouldSetUpFrameAtHead(block)) {
            m_setsUpFrameAtHead.quickSet(blockIndex);
            m_insertionSet.insertInst(0, createEnterFrame(origin));
            m_insertionSet.execute(block);
            m_placement.setups.append(FrameSetupSite { block, 0 });
        } else if (block == entry)
            m_placement.setups.append(FrameSetupSite { insertBlockBefore(entry, entry->frequency(), createEnterFrame(origin)), 0 });
    }

    for (BasicBlock* block : m_code) {
        if (!isInRegion(block))
            insertOnOutgoingEdges(block);
    }
}

void FrameShrinkWrapper::insertTeardowns()
{
    for (size_t blockIndex : m_isInRegion) {
        BasicBlock* block = m_code[blockIndex];
        // A patch that ends the function and runs without a frame: a tail call.
        bool leavesFunction = isReturn(block->last().kind.opcode) || (block->last().kind.opcode == Patch && !block->numSuccessors() && !block->last().needsFrame());
        if (!leavesFunction) {
            insertOnOutgoingEdges(block);
            continue;
        }
        m_insertionSet.insertInst(block->size() - 1, createLeaveFrame(block->last().origin));
        m_insertionSet.execute(block);
        ++m_placement.numberOfTeardowns;
    }
}

} // anonymous namespace

std::optional<BitVector> findFrameRegionBeforeAllocation(Code& code)
{
    ASSERT(code.policy().frame.emitsFrame);
    if (findReasonOutsideCodeToCoverEveryPath(code))
        return std::nullopt;
    StackColoring noSpills;
    std::optional<BitVector> region = FrameShrinkWrapper(code, noSpills).findRegionBeforeAllocation();
    if (shouldValidateIRAtEachPhase(code.proc()))
        RELEASE_ASSERT(region == FrameShrinkWrapper(code, noSpills).findRegionFromAllLocalNeeds({ }));
    return region;
}

std::optional<BitVector> growFrameRegionBeforeAllocation(Code& code, const BitVector& region, const BitVector& crowdedBlocks)
{
    ASSERT(code.policy().frame.emitsFrame);
    StackColoring noSpills;
    std::optional<BitVector> grownRegion = FrameShrinkWrapper(code, noSpills).growRegionBeforeAllocation(region, crowdedBlocks);
    if (shouldValidateIRAtEachPhase(code.proc()))
        RELEASE_ASSERT(grownRegion == FrameShrinkWrapper(code, noSpills).findRegionFromAllLocalNeeds(crowdedBlocks));
    return grownRegion;
}

FramePlacement shrinkWrapFrame(Code& code, const StackColoring& coloring)
{
    PhaseScope phaseScope(code, "shrinkWrapFrame"_s);
    ASSERT(code.policy().frame.emitsFrame);
    FramePlacement placement = FrameShrinkWrapper(code, coloring).run();
    if (shouldValidateIRAtEachPhase(code.proc()))
        RELEASE_ASSERT(!findFrameStateViolation(code));
    return placement;
}

} } } // namespace JSC::B3::Air

#endif // ENABLE(B3_JIT)
