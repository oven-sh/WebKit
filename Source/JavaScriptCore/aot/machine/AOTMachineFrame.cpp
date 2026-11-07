/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachineFrame.h"

#if ENABLE(AOT)

#include "AOTImageFormat.h"
#include "AOTMachineAllocation.h"
#include "AOTMachineAllocator.h"
#include "AOTMachineFrameForms.h"
#include "AOTMachineOrder.h"
#include "AOTMachineSlots.h"
#include "AOTMachineTarget.h"
#include "AOTSavedFrame.h"
#include "AOTStubRegisters.h"
#include "StackAlignment.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstring>
#include <limits>
#include <optional>
#include <wtf/MathExtras.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT { namespace Machine {

static constexpr unsigned plannedFrameWordSize = sizeof(CPURegister);

static_assert(sizeof(CallerFrameAndPC) == stackAlignmentBytes(), "the stack pointer is aligned behind the frame record, so it stays aligned if what lies below the record is a multiple of the alignment");
static_assert(stackAlignmentBytes() == 2 * plannedFrameWordSize);

template<size_t size>
static constexpr RegisterMask plannedFrameMaskOf(const std::array<GPRReg, size>& registers)
{
    RegisterMask mask = 0;
    for (GPRReg reg : registers)
        mask |= maskOf(Reg(reg));
    return mask;
}

static constexpr RegisterMask plannedFrameSuspensionClobbers = plannedFrameMaskOf(suspensionClobberedGPRs);

#if CPU(ARM64)
static_assert(SavedFrame::preservedRegister == InvalidGPRReg);
static_assert(!(suspensionClobberedGPRs.size() % 2) && !((suspensionClobberedGPRs.size() / 2) >> SavedFrame::savedRegisterBits));
static_assert([] {
    for (size_t position = 1; position < suspensionClobberedGPRs.size(); ++position) {
        if (Reg(suspensionClobberedGPRs[position]).index() != Reg(suspensionClobberedGPRs[position - 1]).index() + 1)
            return false;
    }
    return true;
}(), "a run that ascends by Reg::index() holds them in the order of the list, without a gap");
#elif CPU(X86_64)
static_assert(suspensionClobberedGPRs.size() == 1);
static_assert(SavedFrame::preservedRegister != InvalidGPRReg && Reg(SavedFrame::preservedRegister).index() < Reg(suspensionClobberedGPRs[0]).index(), "where both are saved, the stubs find the preserved register below the other one");
#endif

static RegisterMask roundUpCalleeSavesOfPlannedFrame(RegisterMask used)
{
#if CPU(ARM64)
    constexpr size_t numberOfRegisters = suspensionClobberedGPRs.size();
    size_t first = 0;
    while (first < numberOfRegisters && !(used & maskOf(Reg(suspensionClobberedGPRs[first]))))
        ++first;

    RegisterMask saves = 0;
    for (size_t position = first - first % 2; position < numberOfRegisters; ++position)
        saves |= maskOf(Reg(suspensionClobberedGPRs[position]));
    RELEASE_ASSERT(!(used & ~saves));
    return saves;
#elif CPU(X86_64)
    RELEASE_ASSERT(!(used & ~(plannedFrameSuspensionClobbers | maskOf(Reg(SavedFrame::preservedRegister)))));
    return used;
#endif
}

bool plannedBlockOnlyReturns(const Procedure& proc, const Block* block)
{
    if (block->last()->opcode() != Return)
        return false;
    const Allocation& allocation = proc.allocation();
    const BlockSteps& steps = allocation.stepsOf(block);
    if (steps.beginOfEdgeMoves != steps.endOfEdgeMoves)
        return false;
    for (uint32_t index = steps.begin; index < steps.end; ++index) {
        const Step& step = allocation.step(index);
        if (step.kind != StepKind::Execute || allocation.isSpilled(step.operation))
            return false;
        Opcode opcode = step.operation->opcode();
        if (opcode != Nop && opcode != Phi && opcode != Upsilon)
            return false;
    }
    for (const Operation* phi : block->phis()) {
        if (!proc.liveness().isDropped(phi) && allocation.isSpilled(phi))
            return false;
    }
    return true;
}

static std::span<uint8_t> planBlocksOfFramedPart(Procedure& proc)
{
    const Order& order = proc.order();
    const Allocation& allocation = proc.allocation();
    if (!allocation.hasFramedPart() || allocation.needsFrameAheadOfFramedPart() || proc.traits().setsUpFrameAtEntry())
        return { };
    if (allocation.writesMemoryAheadOfFramedPart()) {
        const uint64_t bytesOfSaves = WTF::roundUpToMultipleOf(stackAlignmentBytes(), static_cast<uint64_t>(std::popcount(allocation.usedCalleeSaves())) * plannedFrameWordSize);
        if (WTF::roundUpToMultipleOf(stackAlignmentBytes(), bytesOfSaves + proc.slots().bytesInAll() + proc.callArgAreaSizeInBytes()) > proc.largestFrameSetUpWithoutStackCheck())
            return { };
    }

    std::span<Block* const> blocksInOrder = order.blocksInOrder();
    std::span<uint8_t> frameAtBlocks = proc.arena().createSpan<uint8_t>(proc.numberOfBlocks());
    uint8_t* const frameAt = frameAtBlocks.data();

    for (const Block* block : blocksInOrder) {
        const BlockPlace& place = order.of(block);
        const bool isInFramedPart = allocation.isInFramedPart(block);
        bool runsInFrame = isInFramedPart || (place.isRare() && allocation.needsFrameIn(place.rareRegion));
        bool returnsForWayInFrame = false;
        if (!runsInFrame && plannedBlockOnlyReturns(proc, block)) {
            for (const Block* predecessor : place.predecessors) {
                if (!allocation.isInFramedPart(predecessor))
                    continue;
                returnsForWayInFrame = true;
                runsInFrame = true;
                break;
            }
        }
        if (!runsInFrame)
            continue;
        uint8_t frameHere = Frame::holdsSteps;
        if (returnsForWayInFrame)
            frameHere |= Frame::returnsForWaysInFrame;
        bool beginsFrameOfRareRegion = !isInFramedPart && !returnsForWayInFrame && place.entersRareRegion;
        if ((isInFramedPart && allocation.headOfFramedPartAbove(block).block == block) || beginsFrameOfRareRegion)
            frameHere |= Frame::isSetUpAtHead;
        frameAt[block->index()] = frameHere;
    }

    auto isEnteredInFrame = [&](const Block* block) {
        return (frameAt[block->index()] & (Frame::holdsSteps | Frame::isSetUpAtHead)) == Frame::holdsSteps;
    };
    const bool addressInFrameMayLiveOn = !proc.stackSlots().empty();
    for (const Block* block : blocksInOrder) {
        bool runsInFrame = frameAt[block->index()] & Frame::holdsSteps;
        if (block->numSuccessors() != 1) {
            for (const Edge& edge : block->successors()) {
                if (isEnteredInFrame(edge.block()) != runsInFrame)
                    return { };
            }
            continue;
        }
        const Block* successor = block->successor(0).block();
        if (isEnteredInFrame(successor) == runsInFrame)
            continue;
        if (!block->last()->isUnconditionalJump())
            return { };
        if (runsInFrame) {
            if (addressInFrameMayLiveOn && allocation.isInFramedPart(block))
                return { };
            frameAt[block->index()] |= Frame::isTornDownAtEnd;
            continue;
        }
        if (!(frameAt[successor->index()] & Frame::returnsForWaysInFrame))
            return { };
        frameAt[block->index()] |= Frame::returnsInPlaceOfJumpAtEnd;
    }
    return frameAtBlocks;
}

static FrameCoverage chooseCoverageOfPlannedFrame(const Procedure& proc, bool hasFramedPart)
{
    const Allocation& allocation = proc.allocation();
    if (hasFramedPart)
        return FrameCoverage::FramedPart;
    if (proc.traits().setsUpFrameAtEntry() || allocation.needsFrameOutsideRareRegions() || allocation.holdsPatchThatFollowsFrameSetup())
        return FrameCoverage::WholeFunction;

    const uint32_t numberOfRareRegions = static_cast<uint32_t>(proc.order().rareRegions().size());
    for (uint32_t region = 0; region < numberOfRareRegions; ++region) {
        if (allocation.needsFrameIn(region))
            return FrameCoverage::RareRegions;
    }
    return FrameCoverage::None;
}

bool Frame::isPinned(Reg reg) const
{
    ASSERT(reg);
    return !(m_mutableRegisters & maskOf(reg));
}

Frame* planFrame(Procedure& proc)
{
    Arena& arena = proc.arena();
    const ProcedureTraits& traits = proc.traits();
    const Allocation& allocation = proc.allocation();
    const SlotAssignment& slots = proc.slots();
    const RegisterFile file = registerFileFor(traits);

    Frame* frame = allocation.usedCalleeSaves() ? arena.create<Frame>() : Frame::createWithoutCalleeSaves(arena);
    frame->m_frameAtBlocks = planBlocksOfFramedPart(proc);
    frame->m_coverage = chooseCoverageOfPlannedFrame(proc, !frame->m_frameAtBlocks.empty());
    frame->m_mutableRegisters = file.allocatableIn(Bank::GP) | file.allocatableIn(Bank::FP);

    RegisterMask saves = allocation.usedCalleeSaves();
    RELEASE_ASSERT(!(saves & ~file.calleeSaved));
    switch (frame->m_coverage) {
    case FrameCoverage::None:
    case FrameCoverage::RareRegions:
        RELEASE_ASSERT(!saves && !traits.savesFrameOnSuspend);
        break;
    case FrameCoverage::WholeFunction:
        if (traits.savesFrameOnSuspend)
            saves = roundUpCalleeSavesOfPlannedFrame(saves);
        break;
    case FrameCoverage::FramedPart:
        RELEASE_ASSERT(!traits.savesFrameOnSuspend);
        break;
    }

    ASSERT(!saves || allocation.usedCalleeSaves());
    const unsigned numberOfSaves = static_cast<unsigned>(std::popcount(saves));
    RELEASE_ASSERT(numberOfSaves <= ImageFrame::maxCalleeSavesStart);
    frame->m_calleeSaves = saves;
    frame->m_calleeSavesClobberedAtSuspensions = saves & plannedFrameSuspensionClobbers;
    if (saves)
        frame->m_calleeSaveOffsets = RegisterAtOffsetList(registerSetOf(saves), RegisterAtOffsetList::FramePointerBased);
    RELEASE_ASSERT(frame->m_calleeSaveOffsets.registerCount() == numberOfSaves && frame->m_calleeSaveOffsets.sizeOfAreaInBytes() == numberOfSaves * plannedFrameWordSize);

    RELEASE_ASSERT(slots.bytesKeptAtSuspensions() <= slots.bytesInAll() && (traits.savesFrameOnSuspend || !slots.bytesKeptAtSuspensions()));
    const uint64_t bytesOfSaves = WTF::roundUpToMultipleOf(stackAlignmentBytes(), static_cast<uint64_t>(numberOfSaves) * plannedFrameWordSize);
    const FormOfFrame form = chooseFormOfFrame(proc, frame->m_coverage, saves);
    frame->m_hasFrameRecord = form.hasFrameRecord;
    frame->m_savesLieAtBottom = form.savesLieAtBottom;
    frame->m_topOfSlots = form.topOfSlots;
    if (saves)
        frame->m_calleeSaveOffsets.adjustOffsets(form.offsetOfFirstSave - frame->m_calleeSaveOffsets.at(0).offset());
    const uint64_t bytesBelowFrameRecord = form.bytesBelowFrameRecord;
    RELEASE_ASSERT(bytesBelowFrameRecord <= static_cast<uint64_t>(std::numeric_limits<int32_t>::max()));

    frame->m_nothingCountsOnStackCheck = !traits.setsUpFrameAtEntry() && !allocation.holdsPatchThatCountsOnStackCheck();
    if (frame->hasFrame()) {
        frame->m_bytesBelowFrameRecord = static_cast<unsigned>(bytesBelowFrameRecord);
        frame->m_bytesDeadAtSuspensions = form.hasFrameRecord && !form.savesLieAtBottom ? static_cast<unsigned>(bytesBelowFrameRecord - bytesOfSaves - slots.bytesKeptAtSuspensions()) : 0;
    }

    if (slots.firstWordClearedAtSetup() < slots.endOfWordsClearedAtSetup()) {
        RELEASE_ASSERT(frame->hasFrame() && slots.endOfWordsClearedAtSetup() <= slots.numberOfWordsKeptAtSuspensions());
        frame->m_beginOfWordsClearedAtSetup = -static_cast<int32_t>(bytesOfSaves + static_cast<uint64_t>(slots.endOfWordsClearedAtSetup()) * plannedFrameWordSize);
        frame->m_endOfWordsClearedAtSetup = -static_cast<int32_t>(bytesOfSaves + static_cast<uint64_t>(slots.firstWordClearedAtSetup()) * plannedFrameWordSize);
    }

    std::span<const SpillFacts> spills = allocation.spills();
    if (!spills.empty()) {
        int32_t* offsetOfSpill = static_cast<int32_t*>(arena.allocate(spills.size() * sizeof(int32_t), alignof(int32_t)));
        for (SpillIndex spill = 0; spill < spills.size(); ++spill) {
            const uint32_t distance = slots.distanceOf(spill);
            ASSERT(distance && distance <= slots.bytesInAll() && !(distance % bytesForWidth(spills[spill].width)));
            offsetOfSpill[spill] = form.topOfSlots - static_cast<int32_t>(distance);
        }
        frame->m_offsetOfSpill = std::span<int32_t>(offsetOfSpill, spills.size());
    }

    for (StackSlot* slot : proc.stackSlots()) {
        const uint32_t distance = slots.distanceOf(slot);
        RELEASE_ASSERT(distance && distance >= slot->byteSize() && distance <= slots.bytesInAll());
        slot->m_offsetFromFP = static_cast<intptr_t>(form.topOfSlots) - static_cast<intptr_t>(distance);
    }

    if (!allocation.suspensions().empty())
        SlotAssigner::publishDeadWords(proc, numberOfSaves);
    return frame;
}

struct FactsOfBlockAheadOfFrame {
    Position firstSafepoint { noPosition };
    Position lastSafepoint { noPosition };
    bool holdsPatchThatNeedsFrame { false };
    bool namesFrame { false };
    bool writesMemory { false };
};

static bool operationNamesFrameToBePlanned(const Operation* value)
{
    if (!value->hasNoPosition())
        return false;
    if (value->opcode() == FramePointer || value->opcode() == SlotBase)
        return true;
    for (const Operation* child : value->children()) {
        if (operationNamesFrameToBePlanned(child))
            return true;
    }
    return false;
}

static bool someLeafNamesFrameToBePlanned(const Liveness& liveness, const Operation* operation)
{
    if (operation->isUnconditionalJump())
        return false;
    constexpr uint16_t hasLeavesOfItsOwn = static_cast<uint16_t>(ValueFlag::WorksInMemory) | static_cast<uint16_t>(ValueFlag::GoesByOverflow);
    if (liveness.of(operation).flags & hasLeavesOfItsOwn) [[unlikely]] {
        bool namesFrame = false;
        forEachLeaf(liveness, operation, [&](const Operation*, unsigned, const Operation* leaf) {
            namesFrame |= operationNamesFrameToBePlanned(leaf);
        });
        return namesFrame;
    }
    for (const Operation* child : operation->children()) {
        if (liveness.isFused(child) ? someLeafNamesFrameToBePlanned(liveness, child) : operationNamesFrameToBePlanned(child))
            return true;
    }
    return false;
}

static ALWAYS_INLINE bool everyLeafIsValueOrConstantAheadOfFrame(const ValueFacts* valueFacts, const Operation* operation)
{
    constexpr uint16_t hasLeavesOfItsOwn = static_cast<uint16_t>(ValueFlag::WorksInMemory) | static_cast<uint16_t>(ValueFlag::GoesByOverflow);
    if (valueFacts[operation->index()].flags & hasLeavesOfItsOwn)
        return false;
    for (const Operation* child : operation->children()) {
        if (child->hasNoPosition() ? !child->isConstant() : valueFacts[child->index()].has(ValueFlag::IsFused))
            return false;
    }
    return true;
}

static bool someOperationMayNameFrameToBePlanned(const Procedure& proc)
{
    return proc.namesFramePointer() || !proc.stackSlots().empty();
}

static ALWAYS_INLINE void readOperationAheadOfFrame(const Liveness& liveness, const Operation* operation, Position late, bool stopsAtFirstNeed, bool mayNameFrame, FactsOfBlockAheadOfFrame& facts)
{
    Opcode opcode = operation->opcode();
    if (const PatchOperation* patch = operation->as<PatchOperation>()) {
        facts.holdsPatchThatNeedsFrame |= patch->needsFrameRecord() || !patch->runsWithoutFrame() || patch->mustStandBehindFrameSetup();
        if (patch->onlyKeepsChildrenAlive || (stopsAtFirstNeed && facts.holdsPatchThatNeedsFrame))
            return;
        if (opcode == Patch) {
            facts.writesMemory |= !patch->effects.writes.isEmpty();
            if (patch->effects.mayCollect) {
                if (facts.firstSafepoint == noPosition)
                    facts.firstSafepoint = late;
                facts.lastSafepoint = late;
            }
        }
    } else
        facts.writesMemory |= opcode == Store || opcode == Store8 || opcode == Store16 || liveness.worksInMemory(operation);
    if (mayNameFrame && !facts.namesFrame && !everyLeafIsValueOrConstantAheadOfFrame(liveness.valueFacts(), operation))
        facts.namesFrame = someLeafNamesFrameToBePlanned(liveness, operation);
}

static void readBlockAheadOfFrame(const Allocator& allocator, const Block* block, bool mayNameFrame, FactsOfBlockAheadOfFrame& result)
{
    const Liveness& liveness = allocator.m_liveness;
    const BlockPlace& place = allocator.m_order.of(block);
    const Position head = place.head;
    const bool stopsAtFirstNeed = !place.isRare();
    FactsOfBlockAheadOfFrame facts;
    for (const Operation* sunk : liveness.sunkInto(block))
        readOperationAheadOfFrame(liveness, sunk, head + 1, stopsAtFirstNeed, mayNameFrame, facts);
    Operation* const* const operations = block->operations().data();
    const unsigned numberOfOperations = block->size();
    for (unsigned index = 0; index < numberOfOperations; ++index) {
        if (stopsAtFirstNeed && (facts.holdsPatchThatNeedsFrame || facts.namesFrame))
            break;
        const Operation* operation = operations[index];
        if (liveness.standsInItsBlock(operation))
            readOperationAheadOfFrame(liveness, operation, head + 2 * (index + 1) + 1, stopsAtFirstNeed, mayNameFrame, facts);
    }
    result = facts;
}

static bool blockHoldsPatchThatNeedsFrameToBePlanned(const Liveness& liveness, const Block* block)
{
    auto needsFrame = [](const PatchOperation* patch) ALWAYS_INLINE_LAMBDA {
        return patch->needsFrameRecord() || !patch->runsWithoutFrame() || patch->mustStandBehindFrameSetup();
    };
    for (const Operation* sunk : liveness.sunkInto(block)) {
        const PatchOperation* patch = sunk->as<PatchOperation>();
        if (patch && needsFrame(patch))
            return true;
    }
    Operation* const* const operations = block->operations().data();
    const unsigned numberOfOperations = block->size();
    for (unsigned index = 0; index < numberOfOperations; ++index) {
        const Operation* operation = operations[index];
        const PatchOperation* patch = operation->as<PatchOperation>();
        if (patch && liveness.standsInItsBlock(operation) && needsFrame(patch))
            return true;
    }
    return false;
}

static bool blockLeavesFunctionAheadOfFrame(const Block* block)
{
    return !block->numSuccessors() && block->last()->opcode() != Oops;
}

void Allocator::findFramedPart()
{
    const ProcedureTraits& traits = m_proc.traits();
    std::span<Block* const> blocksInOrder = m_order.blocksInOrder();
    if (traits.setsUpFrameAtEntry() || traits.savesRegistersOfCallers || m_proc.entrypoints().size() != 1 || blocksInOrder.size() < 3)
        return;
    Block* const* const blocks = blocksInOrder.data();
    const size_t numberOfBlocksInOrder = blocksInOrder.size();
    const Block* entry = m_proc.entrypoints().data()->block;
    const unsigned numberOfGPRsThatNoCallerCountsOn = static_cast<unsigned>(std::popcount(m_file.allocatableIn(Bank::GP) & ~m_file.calleeSaved));
    const unsigned numberOfFPRsThatNoCallerCountsOn = static_cast<unsigned>(std::popcount(m_file.allocatableIn(Bank::FP) & ~m_file.calleeSaved));
    auto runsOutOfRegisters = [&](const Block* block) {
        return m_liveness.mostLiveIn(block, Bank::GP) > numberOfGPRsThatNoCallerCountsOn || m_liveness.mostLiveIn(block, Bank::FP) > numberOfFPRsThatNoCallerCountsOn;
    };
    bool hasCalls = m_liveness.nextCallBehindHeadOf(entry, Bank::GP) != noPosition || m_liveness.nextCallBehindHeadOf(entry, Bank::FP) != noPosition;
    if (!hasCalls && std::ranges::none_of(blocksInOrder, [&](const Block* block) { return runsOutOfRegisters(block) || blockHoldsPatchThatNeedsFrameToBePlanned(m_liveness, block); }))
        return;

    const BlockPlace* const places = m_order.places();
    auto placeOf = [places](const Block* block) ALWAYS_INLINE_LAMBDA -> const BlockPlace& {
        return places[block->index()];
    };
    auto needsFrameByFacts = [&](const Block* block, const FactsOfBlockAheadOfFrame& facts) {
        return facts.holdsPatchThatNeedsFrame || facts.namesFrame || runsOutOfRegisters(block);
    };

    const bool mayNameFrame = someOperationMayNameFrameToBePlanned(m_proc);
    auto needsFrameForPatchOrRegisters = [&](const Block* block) {
        ASSERT(!mayNameFrame);
        return runsOutOfRegisters(block) || blockHoldsPatchThatNeedsFrameToBePlanned(m_liveness, block);
    };

    FactsOfBlockAheadOfFrame factsOfEntry;
    const bool entryIsRare = placeOf(entry).isRare();
    const bool entryIsRead = !entryIsRare && mayNameFrame;
    if (entryIsRead) {
        readBlockAheadOfFrame(*this, entry, mayNameFrame, factsOfEntry);
        if (needsFrameByFacts(entry, factsOfEntry))
            return;
    } else if (!entryIsRare && needsFrameForPatchOrRegisters(entry))
        return;

    const uint32_t numberOfBlocks = m_proc.numberOfBlocks();
    constexpr uint8_t isRead = 1 << 0;
    constexpr uint8_t isReachedWithoutFrame = 1 << 1;
    constexpr uint8_t leadsToNeed = 1 << 2;
    constexpr uint8_t followsSetup = 1 << 3;
    constexpr uint8_t isHeadForWhatLiesBelow = 1 << 4;
    constexpr uint8_t everyWayOnNeedsFrame = 1 << 5;
    constexpr uint8_t isHead = 1 << 6;
    constexpr uint8_t isReachedWithoutPassingHead = 1 << 7;
    uint8_t* const marks = m_arena.createSpan<uint8_t>(numberOfBlocks).data();
    FactsOfBlockAheadOfFrame* const factsOfBlocks = static_cast<FactsOfBlockAheadOfFrame*>(m_arena.allocate(numberOfBlocks * sizeof(FactsOfBlockAheadOfFrame), alignof(FactsOfBlockAheadOfFrame)));
    if (entryIsRead) {
        marks[entry->index()] |= isRead;
        factsOfBlocks[entry->index()] = factsOfEntry;
    }
    auto factsOf = [&](const Block* block) ALWAYS_INLINE_LAMBDA -> const FactsOfBlockAheadOfFrame& {
        const uint32_t index = block->index();
        if (!(marks[index] & isRead)) {
            marks[index] |= isRead;
            readBlockAheadOfFrame(*this, block, mayNameFrame, factsOfBlocks[index]);
        }
        return factsOfBlocks[index];
    };
    auto needsFrame = [&](const Block* block) {
        return needsFrameByFacts(block, factsOf(block));
    };

    const Block** const blocksToVisit = static_cast<const Block**>(m_arena.allocate(numberOfBlocks * sizeof(const Block*), alignof(const Block*)));
    size_t numberOfBlocksToVisit = 0;
    auto reach = [&](uint8_t mark, const Block* block) ALWAYS_INLINE_LAMBDA {
        uint8_t& marksOfBlock = marks[block->index()];
        if (marksOfBlock & mark)
            return;
        marksOfBlock |= mark;
        blocksToVisit[numberOfBlocksToVisit++] = block;
    };

    const Block** const blocksToFollowBack = static_cast<const Block**>(m_arena.allocate(numberOfBlocks * sizeof(const Block*), alignof(const Block*)));
    size_t numberOfBlocksToFollowBack = 0;
    bool someWayLeavesFunction = false;
    reach(isReachedWithoutFrame, entry);
    while (numberOfBlocksToVisit) {
        const Block* block = blocksToVisit[--numberOfBlocksToVisit];
        if (!placeOf(block).isRare()) {
            if (block != entry && (mayNameFrame ? needsFrame(block) : needsFrameForPatchOrRegisters(block))) {
                marks[block->index()] |= followsSetup | leadsToNeed;
                blocksToFollowBack[numberOfBlocksToFollowBack++] = block;
                continue;
            }
            someWayLeavesFunction |= blockLeavesFunctionAheadOfFrame(block);
        }
        for (const Edge& edge : block->successors())
            reach(isReachedWithoutFrame, edge.block());
    }
    if (!numberOfBlocksToFollowBack || !someWayLeavesFunction)
        return;

    bool writesMemoryOnWayToNeed = false;
    const bool everySetupMayStopFunction = !m_proc.largestFrameSetUpWithoutStackCheck();
    while (numberOfBlocksToFollowBack) {
        for (const Block* predecessor : placeOf(blocksToFollowBack[--numberOfBlocksToFollowBack]).predecessors) {
            const uint32_t index = predecessor->index();
            if ((marks[index] & leadsToNeed) || !(marks[index] & isReachedWithoutFrame))
                continue;
            marks[index] |= leadsToNeed;
            blocksToFollowBack[numberOfBlocksToFollowBack++] = predecessor;
            const FactsOfBlockAheadOfFrame& facts = factsOf(predecessor);
            bool isRare = placeOf(predecessor).isRare();
            bool leavesSomethingInFrame = isRare && facts.namesFrame;
            bool writesWhatSomebodySees = facts.writesMemory && !(isRare && facts.holdsPatchThatNeedsFrame);
            if (leavesSomethingInFrame || (writesWhatSomebodySees && everySetupMayStopFunction))
                marks[index] |= followsSetup;
            else
                writesMemoryOnWayToNeed |= writesWhatSomebodySees;
        }
    }

    const Block* const* immediateDominators = nullptr;
    auto dominatorOf = [&](const Block* block) ALWAYS_INLINE_LAMBDA -> const Block* {
        if (!immediateDominators) [[unlikely]] {
            immediateDominatorOf(block);
            immediateDominators = m_immediateDominators.begin();
        }
        return immediateDominators[block->index()];
    };

    const Block** const blocksAboveLoopsAndRareRegions = m_arena.createSpan<const Block*>(numberOfBlocks).data();
    for (size_t index = numberOfBlocksInOrder; index--;) {
        const Block* block = blocks[index];
        const BlockPlace& place = placeOf(block);
        const bool isRare = place.isRare();
        const bool mayBeHead = !isRare && place.loop == noLoop;
        bool blockFollowsSetup = marks[block->index()] & followsSetup;
        if (!mayBeHead) {
            blockFollowsSetup = blockFollowsSetup || (!isRare && needsFrame(block));
            if (blockFollowsSetup) {
                const Block* above = block;
                while (above && !blocksAboveLoopsAndRareRegions[above->index()] && (placeOf(above).isRare() || placeOf(above).loop != noLoop))
                    above = dominatorOf(above);
                if (!above)
                    return;
                const Block* endOfWay = above;
                if (const Block* found = blocksAboveLoopsAndRareRegions[above->index()])
                    above = found;
                for (const Block* passed = block; passed != endOfWay; passed = dominatorOf(passed))
                    blocksAboveLoopsAndRareRegions[passed->index()] = above;
                marks[above->index()] |= isHeadForWhatLiesBelow;
            }
            if (isRare)
                continue;
        }
        if (!blockFollowsSetup) {
            bool someWayLeadsOn = false;
            bool everyWayOnDoes = true;
            for (const Edge& edge : block->successors()) {
                const BlockPlace& placeOfTarget = placeOf(edge.block());
                if (placeOfTarget.isRare())
                    continue;
                someWayLeadsOn = true;
                everyWayOnDoes = placeOfTarget.orderIndex <= index || (marks[edge.block()->index()] & everyWayOnNeedsFrame);
                if (!everyWayOnDoes)
                    break;
            }
            if (!(someWayLeadsOn && everyWayOnDoes) && !(mayBeHead && needsFrame(block)))
                continue;
        }
        marks[block->index()] |= everyWayOnNeedsFrame;
    }

    std::span<uint64_t> bitsOfFramedPart = m_arena.createSpan<uint64_t>((static_cast<size_t>(m_order.mostBlocks()) + 63) / 64);
    BitSpan isInFramedPart(bitsOfFramedPart);
    auto liesInFramedPart = [bits = bitsOfFramedPart.data()](const Block* block) ALWAYS_INLINE_LAMBDA -> bool {
        return bits[block->index() / 64] >> (block->index() % 64) & 1;
    };
    for (const Block* block : blocksInOrder) {
        const Block* dominator = dominatorOf(block);
        if (dominator && liesInFramedPart(dominator)) {
            isInFramedPart.set(block->index());
            continue;
        }
        const uint8_t marksOfBlock = marks[block->index()];
        if (!(marksOfBlock & isHeadForWhatLiesBelow)) {
            if (!(marksOfBlock & everyWayOnNeedsFrame))
                continue;
            const BlockPlace& place = placeOf(block);
            if (place.isRare() || place.loop != noLoop)
                continue;
            if (!(marksOfBlock & followsSetup) && !needsFrame(block)) {
                bool someWayLeavesPartUnused = false;
                for (const Edge& edge : block->successors())
                    someWayLeavesPartUnused |= !placeOf(edge.block()).isRare() && dominatorOf(edge.block()) != block;
                if (someWayLeavesPartUnused)
                    continue;
            }
        }
        if (block == entry || block->isLandingPad())
            return;
        marks[block->index()] |= isHead;
        isInFramedPart.set(block->index());
    }

    bool someWayLeavesFunctionWithoutPassingHead = false;
    reach(isReachedWithoutPassingHead, entry);
    while (numberOfBlocksToVisit && !someWayLeavesFunctionWithoutPassingHead) {
        const Block* block = blocksToVisit[--numberOfBlocksToVisit];
        someWayLeavesFunctionWithoutPassingHead = !placeOf(block).isRare() && blockLeavesFunctionAheadOfFrame(block);
        for (const Edge& edge : block->successors()) {
            if (!liesInFramedPart(edge.block()))
                reach(isReachedWithoutPassingHead, edge.block());
        }
    }
    if (!someWayLeavesFunctionWithoutPassingHead)
        return;

    for (size_t index = 0; index < numberOfBlocksInOrder;) {
        const BlockPlace& place = placeOf(blocks[index]);
        Position first = noPosition;
        Position last = noPosition;
        bool comesBack = !place.isRare();
        size_t end = index;
        do {
            const Block* block = blocks[end];
            if (liesInFramedPart(block))
                continue;
            comesBack |= placeOf(block).leavesRareRegion || placeOf(block).loop != noLoop;
            const FactsOfBlockAheadOfFrame& facts = factsOf(block);
            if (facts.firstSafepoint == noPosition)
                continue;
            if (first == noPosition)
                first = facts.firstSafepoint;
            last = facts.lastSafepoint;
        } while (++end < numberOfBlocksInOrder && place.isRare() && placeOf(blocks[end]).rareRegion == place.rareRegion);
        if (first != noPosition)
            m_safepointsAheadOfFramedPart.append(m_arena, SafepointsAheadOfFramedPart { first, last, comesBack ? noPosition : m_order.endOf(blocks[end - 1]) });
        index = end;
    }

    m_allocation.m_headAboveBlock.fill(m_arena, m_order.mostBlocks(), UINT32_MAX);
    for (const Block* block : blocksInOrder) {
        if (!liesInFramedPart(block))
            continue;
        if (marks[block->index()] & isHead) {
            m_allocation.m_headAboveBlock[block->index()] = m_allocation.m_headsOfFramedPart.size();
            m_allocation.m_headsOfFramedPart.append(m_arena, Allocation::HeadOfFramedPart { block });
        } else
            m_allocation.m_headAboveBlock[block->index()] = m_allocation.m_headAboveBlock[dominatorOf(block)->index()];
    }
    m_allocation.m_isInFramedPart = isInFramedPart;
    m_allocation.m_writesMemoryAheadOfFramedPart = writesMemoryOnWayToNeed;
}

static ALWAYS_INLINE bool stepNearFramedPartMayNameSpillBesideItsAssignment(const Step& step)
{
    static_assert(sizeof(Location) == sizeof(uint32_t) && sizeof(Step::sources) == 5 * sizeof(Location));
    static_assert(offsetof(Step, sources) == offsetof(Step, destination) + sizeof(Location) && !(offsetof(Step, destination) % alignof(uint64_t)));
    constexpr uint64_t bitOfSpill = std::bit_cast<uint32_t>(Location::spill(0));
    static_assert(hasOneBitSet(bitOfSpill));
    uint64_t words[3];
    memcpy(words, reinterpret_cast<const uint8_t*>(&step) + offsetof(Step, destination), sizeof(words));
    return (words[0] | words[1] | words[2]) & (bitOfSpill << 32 | bitOfSpill);
}

static RegisterMask registersWrittenByStepAheadOfFrame(const Procedure& proc, const Allocation& allocation, const Step& step)
{
    RegisterMask written = step.destination.isRegister() ? maskOf(step.destination.reg()) : 0;
    if (step.kind == StepKind::Swap)
        written |= maskOf(step.sources[0].reg());
    if (step.kind != StepKind::Execute || step.patchAssignment == UINT32_MAX)
        return written;
    const PatchAssignment& assignment = allocation.patchAssignment(step.patchAssignment);
    written |= assignment.gpScratch | assignment.fpScratch;
    size_t numberOfResults = std::min<size_t>(proc.resultCount(step.operation->type()), assignment.locations.size());
    for (Location result : assignment.locations.first(numberOfResults)) {
        if (result.isRegister())
            written |= maskOf(result.reg());
    }
    return written;
}

void Allocator::settleFramedPart()
{
    if (!m_allocation.hasFramedPart() || m_allocation.needsFrameAheadOfFramedPart())
        return;

    ArenaVector<SpillFacts>& spills = m_allocation.m_spills;
    bool someSpillIsStoredAtHead = false;
    for (SpillFacts& spill : spills) {
        const Operation* value = spill.value;
        if (spill.isSaveInRareRegion || spill.isPointerBesideNarrowReference || m_liveness.isSunk(value))
            continue;
        const Block* blockOfStore = value->endsBlock() ? value->owner()->successor(0).block() : value->owner();
        spill.isStoredAtHeadOfFramedPart = isAheadOfFramedPart(blockOfStore) && !m_order.of(blockOfStore).isRare();
        someSpillIsStoredAtHead |= spill.isStoredAtHeadOfFramedPart;
    }
    auto setUpFrameAtEntryAfterAll = [&] {
        for (SpillFacts& spill : spills)
            spill.isStoredAtHeadOfFramedPart = false;
        m_allocation.m_needsFrameAheadOfFramedPart = true;
        m_allocation.m_needsFrameOutsideRareRegions = true;
    };

    auto isStoredAtHead = [&](Location location) {
        return location.isSpill() && spills[location.spillIndex()].isStoredAtHeadOfFramedPart;
    };
    auto stepMayStandAhead = [&](const Step& step, const BlockPlace& place) ALWAYS_INLINE_LAMBDA {
        if (someSpillIsStoredAtHead && stepNearFramedPartMayNameSpillBesideItsAssignment(step) && (isStoredAtHead(step.destination) || std::ranges::any_of(step.sources, isStoredAtHead)))
            return false;
        if (step.kind == StepKind::Execute && step.patchAssignment != UINT32_MAX) {
            const PatchOperation* patch = step.operation->as<PatchOperation>();
            if (patch && patch->mustStandBehindFrameSetup())
                return false;
            if (someSpillIsStoredAtHead && std::ranges::any_of(m_allocation.patchAssignment(step.patchAssignment).locations, isStoredAtHead))
                return false;
        }
        if (!(registersWrittenByStepAheadOfFrame(m_proc, m_allocation, step) & m_file.calleeSaved))
            return true;
        if (!place.isRare())
            return false;
        m_allocation.m_regionNeedsFrame.set(place.rareRegion);
        return true;
    };
    for (const Block* block : m_order.blocksInOrder()) {
        if (!isAheadOfFramedPart(block))
            continue;
        const BlockPlace& place = m_order.of(block);
        const BlockSteps& steps = m_allocation.stepsOf(block);
        bool mayStandAhead = stepMayStandAhead(m_allocation.step(steps.terminator), place);
        for (uint32_t index = steps.begin; mayStandAhead && index < steps.end; ++index)
            mayStandAhead = stepMayStandAhead(m_allocation.step(index), place);
        for (uint32_t index = steps.beginOfEdgeMoves; mayStandAhead && index < steps.endOfEdgeMoves; ++index)
            mayStandAhead = stepMayStandAhead(m_allocation.step(index), place);
        if (!mayStandAhead) {
            setUpFrameAtEntryAfterAll();
            return;
        }
    }

    const uint32_t begin = m_allocation.m_steps.size();
    for (Allocation::HeadOfFramedPart& head : m_allocation.m_headsOfFramedPart) {
        Position position = m_order.headOf(head.block);
        head.beginOfSteps = m_allocation.m_steps.size();
        for (const Operation* phi : head.block->phis()) {
            if (m_liveness.isDropped(phi) || !m_allocation.isSpilled(phi) || m_allocation.registerAtDefinitionOf(phi).isSet())
                continue;
            m_allocation.m_steps.shrink(begin);
            for (Allocation::HeadOfFramedPart& other : m_allocation.m_headsOfFramedPart)
                other.beginOfSteps = other.endOfSteps = 0;
            setUpFrameAtEntryAfterAll();
            return;
        }
        for (const RegisterBinding& binding : m_allocation.registersAtHeadOf(head.block)) {
            if (maskOf(binding.reg) & m_file.calleeSaved) {
                m_allocation.m_steps.shrink(begin);
                for (Allocation::HeadOfFramedPart& other : m_allocation.m_headsOfFramedPart)
                    other.beginOfSteps = other.endOfSteps = 0;
                setUpFrameAtEntryAfterAll();
                return;
            }
            SpillIndex index = m_allocation.spillOf(binding.value);
            if (index == noSpill || !spills[index].isStoredAtHeadOfFramedPart || position < spills[index].begin || position > spills[index].end)
                continue;
            bool isSaved = false;
            for (uint32_t earlier = head.beginOfSteps; !isSaved && earlier < m_allocation.m_steps.size(); ++earlier)
                isSaved = m_allocation.m_steps[earlier].destination.spillIndex() == index;
            if (isSaved)
                continue;
            Step& save = appendStep(StepKind::Save, spills[index].value);
            save.width = spills[index].width;
            save.destination = Location::spill(index);
            save.sources[0] = Location::reg(binding.reg);
            noteFrameUse();
        }
        head.endOfSteps = m_allocation.m_steps.size();
    }
    if (!someSpillIsStoredAtHead)
        return;

    auto isSavedAbove = [&](const Block* block, Location location) {
        if (!isStoredAtHead(location))
            return true;
        const Allocation::HeadOfFramedPart& head = m_allocation.headOfFramedPartAbove(block);
        for (uint32_t index = head.beginOfSteps; index < head.endOfSteps; ++index) {
            if (m_allocation.m_steps[index].destination == location)
                return true;
        }
        return false;
    };
    auto readsWhatIsSavedAbove = [&](const Block* block, const Step& step) ALWAYS_INLINE_LAMBDA {
        if (stepNearFramedPartMayNameSpillBesideItsAssignment(step)) {
            for (Location source : step.sources) {
                if (!isSavedAbove(block, source))
                    return false;
            }
        }
        if (step.kind != StepKind::Execute || step.patchAssignment == UINT32_MAX)
            return true;
        return std::ranges::all_of(m_allocation.patchAssignment(step.patchAssignment).locations, [&](Location location) {
            return isSavedAbove(block, location);
        });
    };
    for (const Block* block : m_order.blocksInOrder()) {
        if (!m_allocation.isInFramedPart(block))
            continue;
        const BlockSteps& steps = m_allocation.stepsOf(block);
        bool isSound = readsWhatIsSavedAbove(block, m_allocation.step(steps.terminator));
        for (uint32_t index = steps.begin; isSound && index < steps.end; ++index)
            isSound = readsWhatIsSavedAbove(block, m_allocation.step(index));
        for (uint32_t index = steps.beginOfEdgeMoves; isSound && index < steps.endOfEdgeMoves; ++index)
            isSound = readsWhatIsSavedAbove(block, m_allocation.step(index));
        if (isSound)
            continue;
        m_allocation.m_steps.shrink(begin);
        for (Allocation::HeadOfFramedPart& head : m_allocation.m_headsOfFramedPart)
            head.beginOfSteps = head.endOfSteps = 0;
        setUpFrameAtEntryAfterAll();
        return;
    }
}

bool Allocator::pointerWaitsForFramedPart(Operation* reference)
{
    if (!leavesValuesAcrossCallsToFramedPart())
        return false;
    const ValueFacts& facts = m_liveness.of(reference);
    for (const SafepointsAheadOfFramedPart& safepoints : m_safepointsAheadOfFramedPart) {
        if (facts.definition >= safepoints.last)
            continue;
        if (safepoints.until == noPosition) {
            if (facts.lastUse != noPosition && facts.lastUse > safepoints.first)
                return false;
            continue;
        }
        for (uint32_t use = facts.firstUse; use; use = m_liveness.use(use).next) {
            Position read = m_liveness.use(use).position;
            if (read > safepoints.first && read < safepoints.until)
                return false;
        }
    }
    m_referencesWhosePointersWaitForFramedPart.append(m_arena, ReferenceWhosePointerWaits { reference });
    return true;
}

} } } // namespace JSC::AOT::Machine

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
