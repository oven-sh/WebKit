/*
 * Copyright (C) 2015-2021 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. 
 */

#include "config.h"
#include "AirAllocateStackByGraphColoring.h"

#if ENABLE(B3_JIT)

#include "AirArgInlines.h"
#include "AirBitMatrixLiveness.h"
#include "AirCFG.h"
#include "AirCode.h"
#include "AirFormTraits.h"
#include "AirHandleCalleeSaves.h"
#include "AirInstInlines.h"
#include "AirLimits.h"
#include "AirPhaseScope.h"
#include "AirStackAllocation.h"
#include "AirStackAllocatorStats.h"
#include "B3Common.h"
#include "B3Procedure.h"
#include "B3StackmapValue.h"
#include "B3ValueInlines.h"
#include "CompilerTimingScope.h"
#include <array>
#include <wtf/ForbidHeapAllocation.h>
#include <wtf/InterferenceGraph.h>
#include <wtf/Liveness.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace B3 { namespace Air {

namespace {

namespace AirAllocateStackByGraphColoringInternal {
static constexpr bool verbose = false;
static constexpr bool reportLargeMemoryUses = false;
}

// We will perform some spill coalescing. To make that effective, we need to be able to identify
// coalescable moves and handle them specially in interference analysis.
bool isCoalescableMove(Inst& inst, bool coalesceSpillSlots)
{
    if (!coalesceSpillSlots)
        return false;

    Width width;
    switch (inst.kind.opcode) {
    case Move:
        width = pointerWidth();
        break;
    case Move32:
    case MoveFloat:
        width = Width32;
        break;
    case MoveDouble:
        width = Width64;
        break;
    case MoveVector:
        width = Width128;
        break;
    default:
        return false;
    }

    if (inst.args().size() != 3)
        return false;

    for (unsigned i = 0; i < 2; ++i) {
        Arg arg = inst.args()[i];
        if (!arg.isStack())
            return false;
        StackSlot* slot = arg.stackSlot();
        if (slot->kind() != StackSlotKind::Spill)
            return false;
        if (slot->byteSize() != bytesForWidth(width))
            return false;
    }

    return true;
}

bool isUselessMove(Inst& inst, bool coalesceSpillSlots)
{
    return isCoalescableMove(inst, coalesceSpillSlots) && inst.args()[0] == inst.args()[1];
}

// The stack slot mentions, dead store candidates and coalescable move sites of a whole function,
// collected in one forward walk over the instructions.
//
// A mention carries the instruction boundary it happens at, and they land in one flat array sorted by
// boundary: an early action of instruction i belongs to boundary i and a late action to boundary
// i + 1, so keeping the late actions of i after its early actions keeps it sorted.
//
// The kill candidates cannot be derived from the mentions: an instruction with no arguments at all
// satisfies the predicate vacuously. The coalescable moves have to reach coalesceSlots in the order
// the interference build reports them, because it sorts them with an unstable sort and a different
// order would silently change which slots get coalesced.
class StackSlotActions {
    WTF_MAKE_NONCOPYABLE(StackSlotActions);
    WTF_FORBID_HEAP_ALLOCATION;
public:
    enum ActionKind : uint8_t { UseAction, EarlyDefAction, LateDefAction };

    struct Action {
        uint32_t boundary { 0 };
        uint32_t slotIndex { 0 };
        ActionKind kind { UseAction };
        bool isSpill { false };
    };

    struct MoveSite {
        uint32_t instIndex { 0 };
        uint32_t src { 0 };
        uint32_t dst { 0 };
    };

    StackSlotActions(Code& code, bool coalesceSpillSlots)
        : m_code(code)
    {
        CompilerTimingScope timingScope("Air"_s, "StackAllocator::actions"_s);

        m_blockData.grow(code.size());

        for (BasicBlock* block : code) {
            BlockData& data = m_blockData[block->index()];
            data.actionStart = m_actions.size();
            data.killStart = m_killCandidates.size();
            data.moveStart = m_moveSites.size();

            unsigned blockSize = block->size();
            for (unsigned instIndex = 0; instIndex < blockSize; ++instIndex) {
                Inst& inst = block->at(instIndex);
                if (tryCollectFromFixedForms(inst, instIndex, coalesceSpillSlots)) [[likely]]
                    continue;

                InstScan scan;
                inst.forEachArg(
                    [&](Arg& arg, Arg::Role role, Bank bank, Width width) {
                        collectArg(arg, Greedy::Mentions::traitsOf(role, bank, width), instIndex, scan);
                    });
                finishInst(inst, instIndex, scan, coalesceSpillSlots);
            }

            data.actionEnd = m_actions.size();
            data.killEnd = m_killCandidates.size();
            data.moveEnd = m_moveSites.size();
        }
    }

    Code& code() const { return m_code; }
    unsigned numIndices() const { return m_code.stackSlots().size(); }
    bool sawUselessMove() const { return m_sawUselessMove; }

    std::span<const Action> actionsFor(BasicBlock* block) const
    {
        const BlockData& data = m_blockData[block->index()];
        return m_actions.subspan(data.actionStart, data.actionEnd - data.actionStart);
    }

    // Block by block, in the order of the blocks' indexes.
    std::span<const Action> allActions() const { return m_actions.span(); }
    uint32_t endOfActionsFor(BasicBlock* block) const { return m_blockData[block->index()].actionEnd; }

    std::span<const uint32_t> killCandidatesFor(BasicBlock* block) const
    {
        const BlockData& data = m_blockData[block->index()];
        return m_killCandidates.subspan(data.killStart, data.killEnd - data.killStart);
    }

    std::span<const MoveSite> moveSitesFor(BasicBlock* block) const
    {
        const BlockData& data = m_blockData[block->index()];
        return m_moveSites.subspan(data.moveStart, data.moveEnd - data.moveStart);
    }

    // Boundary numbers restart at zero in every block, so `actions` has to be exactly one block's
    // actions. The actions of one boundary are contiguous, so the last boundary is a suffix of the
    // span, and taking it away leaves the boundary before it as the new suffix.
    static std::span<const Action> consumeLastGroup(std::span<const Action>& actions)
    {
        ASSERT(!actions.empty());
        uint32_t boundary = actions.back().boundary;
        size_t start = actions.size();
        while (start && actions[start - 1].boundary == boundary)
            --start;
        std::span<const Action> group = actions.subspan(start);
        actions = actions.first(start);
        return group;
    }

private:
    struct BlockData {
        uint32_t actionStart { 0 };
        uint32_t actionEnd { 0 };
        uint32_t killStart { 0 };
        uint32_t killEnd { 0 };
        uint32_t moveStart { 0 };
        uint32_t moveEnd { 0 };
    };

    struct InstScan {
        // A dead store candidate has no effects beyond its arguments and defines only spill
        // slots, late. That is the only kind of dead stack store this phase will see.
        // Later, we will check whether this spill is alive or not. And if it is dead, we will remove this Inst.
        bool isKillCandidate { true };
        unsigned numberOfLateActions { 0 };
    };

    ALWAYS_INLINE void collectArg(Arg& arg, unsigned traits, unsigned instIndex, InstScan& scan)
    {
        using Mentions = Greedy::Mentions;
        if (!arg.isStack()) [[likely]] {
            if (traits & (Mentions::earlyDef | Mentions::lateDef))
                scan.isKillCandidate = false;
            return;
        }
        StackSlot* slot = arg.stackSlot();
        uint32_t index = slot->index();
        bool isSpill = slot->kind() == StackSlotKind::Spill;
        if (traits & Mentions::earlyUse)
            m_actions.insert(m_actions.size() - scan.numberOfLateActions, Action { instIndex, index, UseAction, isSpill });
        if (traits & Mentions::earlyDef)
            m_actions.insert(m_actions.size() - scan.numberOfLateActions, Action { instIndex, index, EarlyDefAction, isSpill });
        if (traits & Mentions::lateUse) {
            m_actions.append({ instIndex + 1, index, UseAction, isSpill });
            ++scan.numberOfLateActions;
        }
        if (traits & Mentions::lateDef) {
            m_actions.append({ instIndex + 1, index, LateDefAction, isSpill });
            ++scan.numberOfLateActions;
        }
        if ((traits & Mentions::earlyDef) || ((traits & Mentions::lateDef) && !isSpill))
            scan.isKillCandidate = false;
    }

    ALWAYS_INLINE void finishInst(Inst& inst, unsigned instIndex, const InstScan& scan, bool coalesceSpillSlots)
    {
        if (scan.isKillCandidate && !inst.hasNonArgEffects())
            m_killCandidates.append(instIndex);
        if (inst.args().size() == 3 && isCoalescableMove(inst, coalesceSpillSlots)) [[unlikely]] {
            m_moveSites.append({ instIndex, inst.args()[0].stackSlot()->index(), inst.args()[1].stackSlot()->index() });
            if (inst.args()[0] == inst.args()[1])
                m_sawUselessMove = true;
        }
    }

    ALWAYS_INLINE bool tryCollectFromFixedForms(Inst& inst, unsigned instIndex, bool coalesceSpillSlots)
    {
        unsigned argIndex = 0;
        const uint8_t* forms = inst.fixedForms(argIndex);
        if (!forms) [[unlikely]]
            return false;
        InstScan scan;
        Arg* args = inst.args().data();
        for (unsigned numberOfArgs = inst.args().size(); argIndex < numberOfArgs; ++argIndex)
            collectArg(args[argIndex], Greedy::formTraits[forms[argIndex]], instIndex, scan);
        finishInst(inst, instIndex, scan, coalesceSpillSlots);
        return true;
    }

    Code& m_code;
    bool m_sawUselessMove { false };
    Vector<Action> m_actions;
    Vector<uint32_t> m_killCandidates;
    Vector<MoveSite> m_moveSites;
    Vector<BlockData> m_blockData;
};

// Boundaries with no mentions are simply absent, which is what keeps the fixpoint proportional to the
// mentions rather than to the instructions.
struct StackSlotLivenessAdapter {
    WTF_FORBID_HEAP_ALLOCATION;
public:
    typedef Air::CFG CFG;
    typedef StackSlot* Thing;
    using ActionGroup = std::span<const StackSlotActions::Action>;

    StackSlotLivenessAdapter(const StackSlotActions& actions)
        : actions(actions)
    {
    }

    void prepareToCompute() { }

    unsigned numIndices() const { return actions.numIndices(); }
    unsigned blockSize(BasicBlock* block) const { return block->size(); }
    static unsigned valueToIndex(StackSlot* slot) { return slot->index(); }
    StackSlot* indexToValue(unsigned index) const { return actions.code().stackSlots()[index]; }

    template<typename Func>
    void forEachActionGroupDescending(BasicBlock* block, NOESCAPE const Func& func) const
    {
        auto blockActions = actions.actionsFor(block);
        while (!blockActions.empty()) {
            ActionGroup group = StackSlotActions::consumeLastGroup(blockActions);
            func(group.front().boundary, group);
        }
    }

    template<typename Func>
    static void forEachUseInGroup(ActionGroup group, NOESCAPE const Func& func)
    {
        for (const auto& action : group) {
            if (action.kind == StackSlotActions::UseAction)
                func(action.slotIndex);
        }
    }

    template<typename Func>
    static void forEachDefInGroup(ActionGroup group, NOESCAPE const Func& func)
    {
        for (const auto& action : group) {
            if (action.kind != StackSlotActions::UseAction)
                func(action.slotIndex);
        }
    }

    template<typename Func>
    void forEachUseAtTail(BasicBlock* block, NOESCAPE const Func& func) const
    {
        auto blockActions = actions.actionsFor(block);
        if (blockActions.empty() || blockActions.back().boundary != block->size())
            return;
        forEachUseInGroup(StackSlotActions::consumeLastGroup(blockActions), func);
    }

    const StackSlotActions& actions;
};

class StackSlotLiveness final : public WTF::Liveness<StackSlotLivenessAdapter> {
    WTF_FORBID_HEAP_ALLOCATION;
public:
    StackSlotLiveness(Code& code, const StackSlotActions& actions)
        : WTF::Liveness<StackSlotLivenessAdapter>(code.cfg(), actions)
    {
        CompilerTimingScope timingScope("Air"_s, "StackAllocator::liveness"_s);
        compute();
    }

    bool didGiveUp() const { return false; }
    void validate(Code&, const StackSlotActions&) { }
};

struct StackSlotRows {
    WTF_FORBID_HEAP_ALLOCATION;
public:
    StackSlotRows(const StackSlotActions& actions)
        : actions(actions)
    {
    }

    unsigned numIndices() const { return actions.numIndices(); }

    bool needsLiveAtTail(BasicBlock* block) const
    {
        return !actions.actionsFor(block).empty() || !actions.killCandidatesFor(block).empty() || !actions.moveSitesFor(block).empty();
    }

    // A suspension names every slot that is live across it, so n suspensions with n live slots have n * n actions, and each slice would read them all.
    // The actions are listed slice by slice, in their order.
    void willComputeInSlices(size_t wordsPerSlice)
    {
        const size_t slotsPerSlice = wordsPerSlice * 64;
        std::span<const StackSlotActions::Action> all = actions.allActions();
        Vector<size_t> endOfSlice(FillWith { }, numIndices() / slotsPerSlice + 1, static_cast<size_t>(0));
        for (const auto& action : all)
            ++endOfSlice[action.slotIndex / slotsPerSlice];
        size_t numberOfActions = 0;
        for (size_t& end : endOfSlice)
            numberOfActions += std::exchange(end, numberOfActions);
        m_actionsBySlice.grow(all.size());
        for (uint32_t index = 0; index < all.size(); ++index)
            m_actionsBySlice[endOfSlice[all[index].slotIndex / slotsPerSlice]++] = index;
        for (BasicBlock* block : actions.code()) {
            if (!actions.actionsFor(block).empty())
                m_blocksWithActions.append({ actions.endOfActionsFor(block), block->index() });
        }
        m_isListedBySlice = true;
    }

    std::optional<unsigned> nextBlockInSlice(size_t firstWord, size_t numberOfWords) const
    {
        if (!isNextActionIn(firstWord, numberOfWords))
            return std::nullopt;
        auto block = std::ranges::upper_bound(m_blocksWithActions, m_actionsBySlice[m_nextAction], { }, &BlockWithActions::endOfActions);
        return block->blockIndex;
    }

    // In slices it is asked block by block in index order, and slice by slice in slot order, which is the order of the list.
    bool fillRows(BasicBlock* block, size_t firstWord, size_t numberOfWords, uint64_t* gen, uint64_t* kill, uint64_t* tail)
    {
        if (m_isListedBySlice)
            return fillRowsOfSlice(block, firstWord, numberOfWords, gen, kill, tail);
        auto blockActions = actions.actionsFor(block);
        unsigned blockSize = block->size();
        bool isNamedHere = false;
        while (!blockActions.empty()) {
            auto group = StackSlotActions::consumeLastGroup(blockActions);
            uint64_t* uses = group.front().boundary == blockSize ? tail : gen;
            for (const auto& action : group) {
                size_t word = action.slotIndex / 64 - firstWord;
                if (action.kind == StackSlotActions::UseAction && word < numberOfWords) {
                    uses[word] |= static_cast<uint64_t>(1) << (action.slotIndex % 64);
                    isNamedHere = true;
                }
            }
            for (const auto& action : group) {
                size_t word = action.slotIndex / 64 - firstWord;
                if (action.kind != StackSlotActions::UseAction && word < numberOfWords) {
                    gen[word] &= ~(static_cast<uint64_t>(1) << (action.slotIndex % 64));
                    kill[word] |= static_cast<uint64_t>(1) << (action.slotIndex % 64);
                    isNamedHere = true;
                }
            }
        }
        return isNamedHere;
    }

    const StackSlotActions& actions;

private:
    struct BlockWithActions {
        uint32_t endOfActions;
        unsigned blockIndex;
    };

    bool isNextActionIn(size_t firstWord, size_t numberOfWords) const
    {
        return m_nextAction < m_actionsBySlice.size() && actions.allActions()[m_actionsBySlice[m_nextAction]].slotIndex / 64 - firstWord < numberOfWords;
    }

    bool fillRowsOfSlice(BasicBlock* block, size_t firstWord, size_t numberOfWords, uint64_t* gen, uint64_t* kill, uint64_t* tail)
    {
        std::span<const StackSlotActions::Action> all = actions.allActions();
        const uint32_t endOfBlock = actions.endOfActionsFor(block);
        const size_t first = m_nextAction;
        while (isNextActionIn(firstWord, numberOfWords) && m_actionsBySlice[m_nextAction] < endOfBlock)
            ++m_nextAction;
        std::span<const uint32_t> listed = m_actionsBySlice.span().subspan(first, m_nextAction - first);
        const unsigned blockSize = block->size();
        // From the last boundary to the first, and at a boundary the uses before the definitions, as above.
        for (size_t end = listed.size(); end;) {
            const uint32_t boundary = all[listed[end - 1]].boundary;
            size_t start = end;
            while (start && all[listed[start - 1]].boundary == boundary)
                --start;
            uint64_t* uses = boundary == blockSize ? tail : gen;
            for (uint32_t index : listed.subspan(start, end - start)) {
                const auto& action = all[index];
                if (action.kind == StackSlotActions::UseAction)
                    uses[action.slotIndex / 64 - firstWord] |= static_cast<uint64_t>(1) << (action.slotIndex % 64);
            }
            for (uint32_t index : listed.subspan(start, end - start)) {
                const auto& action = all[index];
                if (action.kind != StackSlotActions::UseAction) {
                    gen[action.slotIndex / 64 - firstWord] &= ~(static_cast<uint64_t>(1) << (action.slotIndex % 64));
                    kill[action.slotIndex / 64 - firstWord] |= static_cast<uint64_t>(1) << (action.slotIndex % 64);
                }
            }
            end = start;
        }
        return !listed.empty();
    }

    Vector<uint32_t> m_actionsBySlice;
    Vector<BlockWithActions> m_blocksWithActions;
    size_t m_nextAction { 0 };
    bool m_isListedBySlice { false };
};

class StackSlotLivenessInWords final : public BitMatrixLiveness<StackSlotRows> {
    WTF_FORBID_HEAP_ALLOCATION;
public:
    StackSlotLivenessInWords(Code& code, const StackSlotActions& actions)
        : BitMatrixLiveness<StackSlotRows>(code, actions)
    {
        CompilerTimingScope timingScope("Air"_s, "StackAllocator::liveness"_s);
        compute();
    }

    bool didGiveUp() const { return status() == BitMatrixLivenessStatus::GaveUp; }

    void validate(Code& code, const StackSlotActions& actions)
    {
        StackSlotLiveness reference(code, actions);
        auto workset = makeWorkset();
        auto referenceWorkset = reference.makeWorkset();
        for (BasicBlock* block : code) {
            if (!needsLiveAtTail(block))
                continue;
            copyLiveAtTailInto(workset, block);
            reference.copyLiveAtTailInto(referenceWorkset, block);
            for (unsigned index = 0; index < actions.numIndices(); ++index)
                RELEASE_ASSERT(workset.contains(index) == referenceWorkset.contains(index));
        }
    }
};

class StackAllocatorBase {
protected:
    StackAllocatorBase(Code& code)
        : m_code(code)
        , m_remappedStackSlotIndices(code.stackSlots().size())
    {
        for (unsigned i = 0; i < m_remappedStackSlotIndices.size(); ++i)
            m_remappedStackSlotIndices[i] = i;
    }

    unsigned LEGACY_NODELETE remap(unsigned slotIndex) const
    {
        for (;;) {
            unsigned remappedSlotIndex = m_remappedStackSlotIndices[slotIndex];
            if (remappedSlotIndex == slotIndex)
                return slotIndex;
            slotIndex = remappedSlotIndex;
        }
    }

    StackSlot* LEGACY_NODELETE remapStackSlot(StackSlot* slot) const
    {
        return m_code.stackSlots()[remap(slot->index())];
    }

    bool LEGACY_NODELETE isRemappedSlotIndex(unsigned slotIndex) const
    {
        return m_remappedStackSlotIndices[slotIndex] != slotIndex;
    };

    Code& m_code;
    Vector<unsigned> m_remappedStackSlotIndices;
};

static_assert(AirLimits::maxSlotsForDenseInterference < std::numeric_limits<DenseIterableInterferenceGraph::IndexType>::max());

enum class SlotPlacement : bool { AssignsOffsets, OnlyColors };

bool shouldOrderSlotsByUse(const Code& code)
{
    return isX86() && code.policy().frame.ordersSlotsByUse;
}

Vector<unsigned> useCountsOfSlots(Code& code)
{
    Vector<unsigned> useCounts;
    useCounts.fill(0, code.stackSlots().size());
    for (BasicBlock* block : code) {
        for (Inst& inst : *block) {
            for (Arg& arg : inst.args()) {
                if (arg.isStack())
                    ++useCounts[arg.stackSlot()->index()];
            }
        }
    }
    return useCounts;
}

size_t numberOfWordsInSlots(Code& code)
{
    size_t numberOfWords = 0;
    for (StackSlot* slot : code.stackSlots())
        numberOfWords += (slot->byteSize() + StackColoring::bytesPerWord - 1) / StackColoring::bytesPerWord;
    return numberOfWords;
}

bool isSuspension(const Inst& inst)
{
    if (inst.kind.opcode != Patch || !inst.origin)
        return false;
    auto* stackmap = inst.origin->as<StackmapValue>();
    return stackmap && stackmap->isSuspension();
}

struct StackSlotRowsOfSuspendingBlocks : StackSlotRows {
    WTF_FORBID_HEAP_ALLOCATION;
public:
    StackSlotRowsOfSuspendingBlocks(const StackSlotActions& actions, const BitVector& suspendingBlocks)
        : StackSlotRows(actions)
        , suspendingBlocks(suspendingBlocks)
    {
    }

    bool needsLiveAtTail(BasicBlock* block) const { return suspendingBlocks.get(block->index()); }

    const BitVector& suspendingBlocks;
};

void recordSpillSlotsLiveAcrossSuspensions(Code& code, StackColoring& coloring, const StackSlotActions& actions, auto& liveness, std::span<BasicBlock* const> suspendingBlocks)
{
    auto workset = liveness.makeWorkset();
    auto addUses = [&](std::span<const StackSlotActions::Action> group) {
        StackSlotLivenessAdapter::forEachUseInGroup(group, [&](unsigned slotIndex) {
            workset.add(slotIndex);
        });
    };
    auto removeDefs = [&](std::span<const StackSlotActions::Action> group) {
        StackSlotLivenessAdapter::forEachDefInGroup(group, [&](unsigned slotIndex) {
            workset.remove(slotIndex);
        });
    };
    for (BasicBlock* block : suspendingBlocks) {
        liveness.copyLiveAtTailInto(workset, block);
        auto blockActions = actions.actionsFor(block);
        for (unsigned instIndex = block->size(); instIndex--;) {
            if (!isSuspension(block->at(instIndex)))
                continue;
            const uint32_t boundaryBehind = instIndex + 1;
            std::span<const StackSlotActions::Action> groupBehind;
            while (!blockActions.empty() && blockActions.back().boundary >= boundaryBehind) {
                auto group = StackSlotActions::consumeLastGroup(blockActions);
                addUses(group);
                if (group.front().boundary == boundaryBehind) {
                    groupBehind = group;
                    break;
                }
                removeDefs(group);
            }
            BitVector& liveSlots = coloring.spillSlotsLiveAcrossEachSuspension.add(block->at(instIndex).origin, BitVector()).iterator->value;
            liveSlots.ensureSize(code.stackSlots().size());
            workset.forEachSetBit([&](unsigned slotIndex) {
                coloring.spillSlotsLiveAcrossSuspensions.set(slotIndex);
                liveSlots.quickSet(slotIndex);
            });
            removeDefs(groupBehind);
        }
    }
}

void findSpillSlotsLiveAcrossSuspensions(Code& code, StackColoring& coloring, bool hasTooManySlotsForLiveness)
{
    if (code.calleeSavesClobberedAtSuspensions().isEmpty())
        return;

    Vector<BasicBlock*, 8> suspendingBlocks;
    BitVector isSuspendingBlock;
    for (BasicBlock* block : code) {
        if (std::ranges::any_of(*block, isSuspension)) {
            suspendingBlocks.append(block);
            isSuspendingBlock.set(block->index());
        }
    }
    if (suspendingBlocks.isEmpty())
        return;

    CompilerTimingScope timingScope("Air"_s, "StackAllocator::suspensions"_s);
    coloring.hasSuspensions = true;
    if (hasTooManySlotsForLiveness) {
        for (StackSlot* slot : code.stackSlots())
            coloring.spillSlotsLiveAcrossSuspensions.set(slot->index());
        return;
    }
    StackSlotActions actions(code, false);
    BitMatrixLiveness<StackSlotRowsOfSuspendingBlocks> livenessInWords(code, actions, isSuspendingBlock);
    livenessInWords.compute();
    if (livenessInWords.status() == BitMatrixLivenessStatus::Solved) {
        recordSpillSlotsLiveAcrossSuspensions(code, coloring, actions, livenessInWords, suspendingBlocks.span());
        return;
    }
    StackSlotLiveness liveness(code, actions);
    recordSpillSlotsLiveAcrossSuspensions(code, coloring, actions, liveness, suspendingBlocks.span());
}

template<typename InterferenceGraph, typename SlotLiveness>
class GraphColoringStackAllocator final : public StackAllocatorBase {
    using IndexType = typename InterferenceGraph::IndexType;
public:
    enum class Outcome : bool { Allocated, GaveUp };

    GraphColoringStackAllocator(Code& code, SlotPlacement placement)
        : StackAllocatorBase(code)
        , m_interference()
        , m_coalescableMoves()
        , m_placement(placement)
        , m_ordersSlotsByUse(shouldOrderSlotsByUse(code))
        , m_stats(code.proc().name())
    {
    }

    Outcome run(const Vector<StackSlot*>& assignedEscapedStackSlots, StackColoring& coloring)
    {
        StackSlotActions actions(m_code, m_code.policy().frame.coalescesSpillSlots);
        SlotLiveness liveness(m_code, actions);
        return run(assignedEscapedStackSlots, coloring, actions, liveness);
    }

    Outcome run(const Vector<StackSlot*>& assignedEscapedStackSlots, StackColoring& coloring, const StackSlotActions& actions, SlotLiveness& liveness)
    {
        bool coalesceSpillSlots = m_code.policy().frame.coalescesSpillSlots;
        if (liveness.didGiveUp())
            return Outcome::GaveUp;
        if (shouldValidateIRAtEachPhase(m_code.proc()))
            liveness.validate(m_code, actions);
        m_interference.setMaxIndex(m_code.stackSlots().size());
        buildInterferenceGraph(liveness);

        coalesceSlots(coalesceSpillSlots, actions.sawUselessMove());
        if (m_placement == SlotPlacement::OnlyColors)
            findSpillSlotsLiveAcrossSuspensions(m_code, coloring, false);
        assignStackLocations(assignedEscapedStackSlots, coloring);

        m_stats.frameSize = m_code.frameSize();
        return Outcome::Allocated;
    }

private:
    void buildInterferenceGraph(SlotLiveness& liveness)
    {
        CompilerTimingScope timingScope("Air"_s, "StackAllocator::build"_s);

        const StackSlotActions& actions = liveness.actions;
        auto workset = liveness.makeWorkset();

        for (BasicBlock* block : m_code) {
            auto blockActions = actions.actionsFor(block);
            auto killCandidates = actions.killCandidatesFor(block);
            auto moveSites = actions.moveSitesFor(block);
            bool killedAny = false;

            if (!blockActions.empty() || !killCandidates.empty() || !moveSites.empty()) {
                liveness.copyLiveAtTailInto(workset, block);

                for (;;) {
                    // Visit boundaries from the tail down, skipping any that mentions no stack slot
                    // and holds neither a kill candidate nor a coalescable move: those cannot change
                    // the live set or add an edge. Each of the three spans is trimmed from the back
                    // as its last entry is consumed, so their last entries are always the next
                    // candidates going backwards.

                    // Compute the next changing boundary from all three different source. And moving the cursor to the closest one.
                    int64_t actionBoundary = blockActions.empty() ? -1 : static_cast<int64_t>(blockActions.back().boundary);
                    int64_t killBoundary = killCandidates.empty() ? -1 : static_cast<int64_t>(killCandidates.back()) + 1;
                    int64_t moveBoundary = moveSites.empty() ? -1 : static_cast<int64_t>(moveSites.back().instIndex) + 1;
                    int64_t boundary = std::max({ actionBoundary, killBoundary, moveBoundary });
                    if (boundary < 0)
                        break;

                    std::span<const StackSlotActions::Action> group;
                    if (actionBoundary == boundary) {
                        group = StackSlotActions::consumeLastGroup(blockActions);

                        // Completes the previous boundary's transfer. The uses at the tail boundary are
                        // already part of liveAtTail, so re-adding them is a harmless no-op.
                        for (const auto& action : group) {
                            if (action.kind == StackSlotActions::UseAction)
                                workset.add(action.slotIndex);
                        }
                    }

                    if (AirAllocateStackByGraphColoringInternal::verbose) {
                        dataLog("Boundary ", boundary, " of block ", block->index(), ", live:");
                        workset.forEachSetBit(
                            [&] (unsigned slotIndex) {
                                dataLog(" ", pointerDump(m_code.stackSlots()[slotIndex]));
                            });
                        if (boundary)
                            dataLog(", after: ", block->at(boundary - 1));
                        dataLogLn();
                    }

                    // Kill dead stores. For simplicity we say that a store is killable if it has only late
                    // defs and those late defs are to things that are dead right now. We only do that
                    // because that's the only kind of dead stack store we will see here.
                    bool killedPreviousInst = false;
                    if (killBoundary == boundary) {
                        uint32_t candidateInstIndex = consumeLast(killCandidates);

                        // All late defs at this boundary belong to the candidate, so the store is dead
                        // exactly when none of them is live here.
                        bool isDeadStore = true;
                        for (const auto& action : group) {
                            if (action.kind == StackSlotActions::LateDefAction && workset.contains(action.slotIndex)) {
                                isDeadStore = false;
                                break;
                            }
                        }
                        if (isDeadStore) {
                            dataLogLnIf(AirAllocateStackByGraphColoringInternal::verbose, "Killing dead store: ", block->at(candidateInstIndex));
                            block->at(candidateInstIndex) = Inst();
                            killedPreviousInst = true;
                            killedAny = true;
                        }
                    }

                    // A coalescable move's destination does not interfere with its source, so it is
                    // handled separately and its late def is left out of the loop below. A killed
                    // store is gone, so it can no longer be coalesced.
                    bool previousInstIsCoalescableMove = false;
                    if (moveBoundary == boundary) {
                        const auto& site = consumeLast(moveSites);
                        if (!killedPreviousInst) {
                            previousInstIsCoalescableMove = true;
                            m_coalescableMoves.append(CoalescableMove(site.src, site.dst, block->frequency()));
                            workset.forEachSetBit(
                                [&](unsigned otherSlotIndex) {
                                    if (otherSlotIndex != site.src)
                                        addEdge(site.dst, otherSlotIndex);
                                });
                        }
                    }

                    for (const auto& action : group) {
                        if (action.kind == StackSlotActions::UseAction || !action.isSpill)
                            continue;
                        // All late defs at this boundary belong to the previous instruction, so they
                        // are exactly what a kill or a coalescable move suppresses.
                        if (action.kind == StackSlotActions::LateDefAction && (killedPreviousInst || previousInstIsCoalescableMove))
                            continue;
                        m_interference.addAllBut(action.slotIndex, workset);
                    }

                    for (const auto& action : group) {
                        if (action.kind != StackSlotActions::UseAction)
                            workset.remove(action.slotIndex);
                    }
                }

                ASSERT(blockActions.empty() && killCandidates.empty() && moveSites.empty());
            }

            if (killedAny) {
                block->insts().removeAllMatching(
                    [&] (const Inst& inst) -> bool {
                        return !inst;
                    });
            }
        }
        m_interference.mirrorEdges();

        reportInterferenceGraph();
    }

    void reportInterferenceGraph()
    {
        if (AirAllocateStackByGraphColoringInternal::reportLargeMemoryUses && m_interference.memoryUse() > 1024) {
            dataLog("GraphColoringStackAllocator stackSlots, interferenceGraph(kB), coalescable moves(kB): ", m_code.stackSlots().size(), " : ");
            m_interference.dumpMemoryUseInKB();
            dataLog(", ", (m_coalescableMoves.size() * sizeof(CoalescableMove)) / 1024);
            dataLogLn();
        }

        if (AirAllocateStackByGraphColoringInternal::verbose) {
            for (StackSlot* slot : m_code.stackSlots()) {
                dataLog("Interference of ", pointerDump(slot), ": ");
                for (unsigned slotIndex : m_interference[slot->index()])
                    dataLog(pointerDump(m_code.stackSlots()[slotIndex]));
                dataLog("\n");
            }
        }
    }

    void coalesceSlots(bool coalesceSpillSlots, bool sawUselessMove)
    {
        CompilerTimingScope timingScope("Air"_s, "StackAllocator::coalesce"_s);

        if (m_stats.collectingStats()) {
            m_stats.numBlocks = m_code.size();
            m_stats.numStackSlots = m_code.stackSlots().size();
            m_stats.stackSlotInterferenceSizeBytes = m_interference.memoryUse();
            m_stats.numStackSlotsCoalesceableMoves = m_coalescableMoves.size();
        }

        // Now try to coalesce some moves.
        if (m_coalescableMoves.isEmpty() && !sawUselessMove)
            return;

        std::ranges::sort(m_coalescableMoves, std::ranges::greater { }, &CoalescableMove::frequency);

        bool anyRemapHappened = false;
        for (const CoalescableMove& move : m_coalescableMoves) {
            IndexType slotToKill = remap(move.src);
            IndexType slotToKeep = remap(move.dst);
            if (slotToKill == slotToKeep)
                continue;
            if (m_interference.contains(slotToKill, slotToKeep))
                continue;

            m_stats.numStackSlotsCoalesced++;
            m_remappedStackSlotIndices[slotToKill] = slotToKeep;
            anyRemapHappened = true;

            for (IndexType interferingSlot : m_interference[slotToKill])
                addEdge(interferingSlot, slotToKeep);
            m_interference.mayClear(slotToKill);
        }

        if (!anyRemapHappened && !sawUselessMove)
            return;

        for (BasicBlock* block : m_code) {
            bool removedAny = false;
            for (Inst& inst : *block) {
                for (Arg& arg : inst.args()) {
                    if (arg.isStack())
                        arg = Arg::stack(remapStackSlot(arg.stackSlot()), arg.offset());
                }
                if (isUselessMove(inst, coalesceSpillSlots)) {
                    inst = Inst();
                    removedAny = true;
                }
            }
            if (removedAny) {
                block->insts().removeAllMatching(
                    [&] (const Inst& inst) -> bool {
                        return !inst;
                    });
            }
        }
    }

    void countSlotUses()
    {
        if (m_useCounts.isEmpty())
            m_useCounts = useCountsOfSlots(m_code);
    }

    struct NonInterferingSlotsAtUnit {
        std::optional<unsigned> querySlot;
        unsigned count { 0 };
    };

    struct FirstFitState {
        Vector<StackColoring::UnitRange> unitsOfSlot;
        Vector<uint64_t> hasUnits;
        Vector<uint64_t> reservedUnits;
        Vector<uint64_t> unitsOfAssignedSlots;
        Vector<uint64_t> occupiedUnits;
        Vector<unsigned> numberOfSlotsAtUnit;
        Vector<NonInterferingSlotsAtUnit> nonInterferingSlotsAtUnit;
        size_t numberOfSlotsWithUnits { 0 };
    };

    Vector<StackSlot*> slotsInAssignmentOrder()
    {
        Vector<StackSlot*> slotsInOrder;
        for (StackSlot* slot : m_code.stackSlots())
            slotsInOrder.append(slot);
        if (m_ordersSlotsByUse && slotsInOrder.size() >= AirLimits::minSlotsToOrderByUse) {
            countSlotUses();
            std::stable_sort(slotsInOrder.begin(), slotsInOrder.end(), [&](StackSlot* a, StackSlot* b) {
                return m_useCounts[a->index()] > m_useCounts[b->index()];
            });
        }
        return slotsInOrder;
    }

    ALWAYS_INLINE static void markOccupied(Vector<uint64_t>& occupiedUnits, StackColoring::UnitRange units)
    {
        while (occupiedUnits.size() <= units.last / 64) [[unlikely]]
            occupiedUnits.append(0);
        for (unsigned unit = units.first; unit <= units.last; ++unit)
            occupiedUnits[unit / 64] |= static_cast<uint64_t>(1) << (unit % 64);
    }

    static StackColoring::UnitRange computeUnitsFromOffset(StackSlot* slot)
    {
        RELEASE_ASSERT(slot->offsetFromFP() < 0);
        size_t end = -slot->offsetFromFP();
        size_t begin = end - std::min<size_t>(end, slot->byteSize());
        return { static_cast<unsigned>(begin / StackColoring::bytesPerUnit), static_cast<unsigned>((end - 1) / StackColoring::bytesPerUnit) };
    }

    void recordUnits(FirstFitState& state, StackSlot* slot, StackColoring::UnitRange units)
    {
        state.unitsOfSlot[slot->index()] = units;
        state.hasUnits[slot->index() / 64] |= static_cast<uint64_t>(1) << (slot->index() % 64);
        ++state.numberOfSlotsWithUnits;
        markOccupied(state.unitsOfAssignedSlots, units);
        while (state.numberOfSlotsAtUnit.size() <= units.last) {
            state.numberOfSlotsAtUnit.append(0);
            state.nonInterferingSlotsAtUnit.append(NonInterferingSlotsAtUnit { });
        }
        for (unsigned unit = units.first; unit <= units.last; ++unit)
            ++state.numberOfSlotsAtUnit[unit];
    }

    void prepareFirstFit(FirstFitState& state, const Vector<StackSlot*>& assignedEscapedStackSlots)
    {
        state.unitsOfSlot.grow(m_code.stackSlots().size());
        state.hasUnits.fill(0, (m_code.stackSlots().size() + 63) / 64);
        if (m_placement == SlotPlacement::OnlyColors)
            return;
        for (StackSlot* slot : m_code.stackSlots()) {
            if (slot->offsetFromFP())
                recordUnits(state, slot, computeUnitsFromOffset(slot));
        }
        for (StackSlot* slot : assignedEscapedStackSlots)
            markOccupied(state.reservedUnits, state.unitsOfSlot[slot->index()]);
    }

    static void reserveWordsOfAssignedSlots(FirstFitState& state)
    {
        static_assert(StackColoring::unitsPerWord == 2);
        constexpr uint64_t firstUnitOfEachWord = 0x5555555555555555ULL;
        RELEASE_ASSERT(state.reservedUnits.isEmpty());
        for (uint64_t units : state.unitsOfAssignedSlots)
            state.reservedUnits.append(units | (units & firstUnitOfEachWord) << 1 | (units >> 1 & firstUnitOfEachWord));
    }

    void fillOccupiedUnits(FirstFitState& state, StackSlot* slot)
    {
        state.occupiedUnits.shrink(0);
        if (state.numberOfSlotsWithUnits >= AirLimits::minAssignedSlotsToWalkComplement && AirLimits::minRatioOfAssignedToNonInterfering * (state.numberOfSlotsWithUnits - m_interference.countInterferingIn(slot->index(), state.hasUnits.span())) < state.numberOfSlotsWithUnits) {
            state.occupiedUnits.appendVector(state.unitsOfAssignedSlots);
            const unsigned querySlot = slot->index();
            m_interference.forEachNonInterferingIn(slot->index(), state.hasUnits.span(), [&](unsigned otherSlotIndex) ALWAYS_INLINE_LAMBDA {
                StackColoring::UnitRange units = state.unitsOfSlot[otherSlotIndex];
                for (unsigned unit = units.first; unit <= units.last; ++unit) {
                    NonInterferingSlotsAtUnit& nonInterfering = state.nonInterferingSlotsAtUnit[unit];
                    if (nonInterfering.querySlot != querySlot)
                        nonInterfering = { querySlot, 0 };
                    if (++nonInterfering.count == state.numberOfSlotsAtUnit[unit])
                        state.occupiedUnits[unit / 64] &= ~(static_cast<uint64_t>(1) << (unit % 64));
                }
            });
            for (size_t word = 0; word < state.reservedUnits.size(); ++word)
                state.occupiedUnits[word] |= state.reservedUnits[word];
            return;
        }
        state.occupiedUnits.appendVector(state.reservedUnits);
        m_interference.forEachInterferingIn(slot->index(), state.hasUnits.span(), [&](unsigned otherSlotIndex) ALWAYS_INLINE_LAMBDA {
            markOccupied(state.occupiedUnits, state.unitsOfSlot[otherSlotIndex]);
        });
    }

    static unsigned findFirstFit(std::span<const uint64_t> occupiedUnits, unsigned unitsNeeded, unsigned unitsPerAlignment)
    {
        if (unitsNeeded == 2 && unitsPerAlignment == 2) {
            for (size_t wordIndex = 0; wordIndex < occupiedUnits.size(); ++wordIndex) {
                uint64_t freeUnits = ~occupiedUnits[wordIndex];
                uint64_t freePairs = freeUnits & (freeUnits >> 1) & 0x5555555555555555ULL;
                if (freePairs)
                    return static_cast<unsigned>(64 * wordIndex + std::countr_zero(freePairs) + 2);
            }
            return static_cast<unsigned>(64 * occupiedUnits.size() + 2);
        }
        if (unitsNeeded == 1 && unitsPerAlignment == 1) {
            size_t wordIndex = 0;
            while (wordIndex < occupiedUnits.size() && !~occupiedUnits[wordIndex])
                ++wordIndex;
            return static_cast<unsigned>(wordIndex * 64 + (wordIndex < occupiedUnits.size() ? std::countr_one(occupiedUnits[wordIndex]) : 0) + 1);
        }
        auto isOccupied = [&](size_t unit) {
            return unit / 64 < occupiedUnits.size() && (occupiedUnits[unit / 64] >> (unit % 64) & 1);
        };
        size_t end = WTF::roundUpToMultipleOf<size_t>(unitsPerAlignment, unitsNeeded);
        for (;;) {
            size_t unit = end;
            while (unit > end - unitsNeeded && !isOccupied(unit - 1))
                --unit;
            if (unit == end - unitsNeeded)
                break;
            end = WTF::roundUpToMultipleOf<size_t>(unitsPerAlignment, unit + unitsNeeded);
        }
        return static_cast<unsigned>(end);
    }

    void assignStackLocations(const Vector<StackSlot*>& assignedEscapedStackSlots, StackColoring& coloring)
    {
        CompilerTimingScope timingScope("Air"_s, "StackAllocator::assign"_s);

        // Now we assign stack locations. At its heart this algorithm is just first-fit. For each
        // StackSlot we just want to find the offsetFromFP that is least negative while ensuring no
        // overlap with other StackSlots that this overlaps with.
        FirstFitState state;
        prepareFirstFit(state, assignedEscapedStackSlots);
        auto assignUnits = [&](StackSlot* slot) {
            if (!slot->isSpill() || isRemappedSlotIndex(slot->index()))
                return;
            if (state.hasUnits[slot->index() / 64] >> (slot->index() % 64) & 1)
                return;

            const unsigned unitsNeeded = (slot->byteSize() + StackColoring::bytesPerUnit - 1) / StackColoring::bytesPerUnit;
            const unsigned unitsPerAlignment = std::max(1u, slot->alignment() / StackColoring::bytesPerUnit);
            fillOccupiedUnits(state, slot);
            const unsigned end = findFirstFit(state.occupiedUnits.span(), unitsNeeded, unitsPerAlignment);
            recordUnits(state, slot, { end - unitsNeeded, end - 1 });
            if (m_placement == SlotPlacement::AssignsOffsets)
                slot->setOffsetFromFP(-static_cast<intptr_t>(end) * static_cast<intptr_t>(StackColoring::bytesPerUnit));
        };
        Vector<StackSlot*> slotsInOrder = slotsInAssignmentOrder();
        if (coloring.hasSuspensions) {
            // A word that a suspension keeps holds only what some suspension keeps.
            for (StackSlot* slot : slotsInOrder) {
                if (coloring.spillSlotsLiveAcrossSuspensions.get(slot->index()))
                    assignUnits(slot);
            }
            reserveWordsOfAssignedSlots(state);
        }
        for (StackSlot* slot : slotsInOrder)
            assignUnits(slot);

        switch (m_placement) {
        case SlotPlacement::AssignsOffsets:
            updateFrameSizeBasedOnStackSlots(m_code);
            break;
        case SlotPlacement::OnlyColors:
            coloring.unitsOfSlot.fill(std::nullopt, m_code.stackSlots().size());
            WTF::forEachSetBit(state.hasUnits.span(), [&](size_t slotIndex) {
                coloring.unitsOfSlot[slotIndex] = state.unitsOfSlot[slotIndex];
            });
            coloring.numberOfUnits = state.numberOfSlotsAtUnit.size();
            coloring.useCountOfSlot = WTF::move(m_useCounts);
            break;
        }
    }

    struct CoalescableMove {
        CoalescableMove()
        {
        }

        CoalescableMove(IndexType src, IndexType dst, double frequency)
            : src(src)
            , dst(dst)
            , frequency(frequency)
        {
        }

        friend bool operator==(const CoalescableMove&, const CoalescableMove&) = default;

        explicit operator bool() const
        {
            return *this != CoalescableMove();
        }

        void dump(PrintStream& out) const
        {
            out.print(src, "->", dst, "(", frequency, ")");
        }

        IndexType src { std::numeric_limits<IndexType>::max() };
        IndexType dst { std::numeric_limits<IndexType>::max() };
        float frequency { 0.0 };
    };

    void addEdge(IndexType u, IndexType v)
    {
        if (u == v)
            return;
        m_interference.add(u, v);
    }

    InterferenceGraph m_interference;
    Vector<CoalescableMove> m_coalescableMoves;
    Vector<unsigned> m_useCounts;
    const SlotPlacement m_placement;
    const bool m_ordersSlotsByUse;
    AirStackAllocatorStats m_stats;
};

// What each block does to each spill slot it names, kept sorted so that a slice of the liveness matrix reads only its own slots.
class SpillSlotBoundaryRows {
    WTF_FORBID_HEAP_ALLOCATION;
public:
    static constexpr bool recordsLayoutBoundaries = true;

    explicit SpillSlotBoundaryRows(const StackSlotActions& actions)
        : m_numberOfSlots(actions.numIndices())
    {
        Code& code = actions.code();
        Vector<uint8_t> rowsOfSlot(FillWith { }, m_numberOfSlots, static_cast<uint8_t>(0));
        Vector<uint32_t> namedSlots;
        for (BasicBlock* block : code) {
            auto blockActions = actions.actionsFor(block);
            const unsigned blockSize = block->size();
            namedSlots.shrink(0);
            while (!blockActions.empty()) {
                auto group = StackSlotActions::consumeLastGroup(blockActions);
                const uint8_t rowOfUses = group.front().boundary == blockSize ? Tail : Gen;
                for (const auto& action : group) {
                    if (!action.isSpill)
                        continue;
                    uint8_t& rows = rowsOfSlot[action.slotIndex];
                    if (!rows)
                        namedSlots.append(action.slotIndex);
                    rows |= action.kind == StackSlotActions::UseAction ? rowOfUses | IsNamed : IsNamed;
                }
                for (const auto& action : group) {
                    if (action.isSpill && action.kind != StackSlotActions::UseAction)
                        rowsOfSlot[action.slotIndex] = (rowsOfSlot[action.slotIndex] & ~Gen) | Kill;
                }
            }
            for (uint32_t slotIndex : namedSlots)
                m_facts.append(Fact { block->index(), slotIndex, std::exchange(rowsOfSlot[slotIndex], static_cast<uint8_t>(0)) });
        }
    }

    unsigned numIndices() const { return m_numberOfSlots; }
    bool needsLiveAtTail(BasicBlock*) const { return false; }

    void willComputeInSlices(size_t wordsPerSlice)
    {
        const size_t slotsPerSlice = wordsPerSlice * 64;
        Vector<size_t> endOfSlice(FillWith { }, m_numberOfSlots / slotsPerSlice + 1, static_cast<size_t>(0));
        for (const Fact& fact : m_facts)
            ++endOfSlice[fact.slotIndex / slotsPerSlice];
        size_t numberOfFacts = 0;
        for (size_t& end : endOfSlice)
            numberOfFacts += std::exchange(end, numberOfFacts);
        Vector<Fact> factsBySlice(m_facts.size());
        for (const Fact& fact : m_facts)
            factsBySlice[endOfSlice[fact.slotIndex / slotsPerSlice]++] = fact;
        m_facts = WTF::move(factsBySlice);
    }

    std::optional<unsigned> nextBlockInSlice(size_t firstWord, size_t numberOfWords) const
    {
        if (m_nextFact == m_facts.size() || m_facts[m_nextFact].slotIndex / 64 - firstWord >= numberOfWords)
            return std::nullopt;
        return m_facts[m_nextFact].blockIndex;
    }

    // Asked block by block in index order, and slice by slice in slot order, which is the order of the facts.
    bool fillRows(BasicBlock* block, size_t firstWord, size_t numberOfWords, uint64_t* gen, uint64_t* kill, uint64_t* tail)
    {
        bool isNamedHere = false;
        for (; m_nextFact < m_facts.size(); ++m_nextFact) {
            const Fact& fact = m_facts[m_nextFact];
            const size_t word = fact.slotIndex / 64 - firstWord;
            if (fact.blockIndex != block->index() || word >= numberOfWords)
                break;
            const uint64_t bit = static_cast<uint64_t>(1) << (fact.slotIndex % 64);
            if (fact.rows & Gen)
                gen[word] |= bit;
            if (fact.rows & Kill)
                kill[word] |= bit;
            if (fact.rows & Tail)
                tail[word] |= bit;
            isNamedHere = true;
        }
        return isNamedHere;
    }

private:
    enum Row : uint8_t { Gen = 1, Kill = 2, Tail = 4, IsNamed = 8 };

    struct Fact {
        uint32_t blockIndex;
        uint32_t slotIndex;
        uint8_t rows;
    };

    unsigned m_numberOfSlots;
    Vector<Fact> m_facts;
    size_t m_nextFact { 0 };
};

// Gives spill slots their places from the ranges of positions, in layout order, at which each is live. A slot shares a
// place with another when all of its ranges lie between two ranges of the other, or behind the last one. No set of
// pairs of slots is built, so the time does not grow with the number of slots that are live together.
class LiveRangeStackAllocator {
    WTF_FORBID_HEAP_ALLOCATION;
public:
    LiveRangeStackAllocator(Code& code, SlotPlacement placement)
        : m_code(code)
        , m_placement(placement)
    {
    }

    void run(StackColoring& coloring)
    {
        removeUselessMoves();
        choosePools();
        collectRanges();
        linkRanges();
        assignCells();
        placeSlots(coloring);
    }

private:
    static constexpr uint32_t noPosition = std::numeric_limits<uint32_t>::max();
    static constexpr uint32_t noSlot = std::numeric_limits<uint32_t>::max();
    static constexpr uint32_t noCell = std::numeric_limits<uint32_t>::max();
    static constexpr unsigned unitsPerWideAlignment = 4;

    enum PoolKind : uint8_t { OneUnit, TwoUnits, Wide, NoPool };
    static constexpr unsigned numberOfPools = 3;

    struct Range {
        uint32_t slotIndex;
        uint32_t start;
        uint32_t end;
        uint32_t startOfNextRange { noPosition };
    };

    // Cells of one size. Finds the lowest cell that stays free beyond a position in logarithmic time.
    class CellPool {
    public:
        void reserve(size_t maxNumberOfCells)
        {
            m_numberOfLeaves = WTF::roundUpToPowerOfTwo(std::max<size_t>(1, maxNumberOfCells));
            m_freeUntil.fill(noPosition, 2 * m_numberOfLeaves);
            m_topOwner.fill(noSlot, maxNumberOfCells);
        }

        uint32_t findLowestCellFreeBeyond(uint32_t position) const
        {
            RELEASE_ASSERT(m_freeUntil[1] > position);
            size_t node = 1;
            while (node < m_numberOfLeaves) {
                node *= 2;
                if (m_freeUntil[node] <= position)
                    ++node;
            }
            return static_cast<uint32_t>(node - m_numberOfLeaves);
        }

        void setFreeUntil(uint32_t cell, uint32_t position)
        {
            size_t node = m_numberOfLeaves + cell;
            m_freeUntil[node] = position;
            for (node /= 2; node; node /= 2)
                m_freeUntil[node] = std::max(m_freeUntil[2 * node], m_freeUntil[2 * node + 1]);
        }

        void didUse(uint32_t cell) { m_numberOfCells = std::max(m_numberOfCells, cell + 1); }
        uint32_t& topOwner(uint32_t cell) { return m_topOwner[cell]; }
        uint32_t numberOfCells() const { return m_numberOfCells; }

        unsigned unitsPerCell { 0 };
        unsigned firstUnit { 0 };

    private:
        size_t m_numberOfLeaves { 0 };
        Vector<uint32_t> m_freeUntil;
        Vector<uint32_t> m_topOwner;
        uint32_t m_numberOfCells { 0 };
    };

    static unsigned unitsNeededBy(StackSlot* slot)
    {
        return (slot->byteSize() + StackColoring::bytesPerUnit - 1) / StackColoring::bytesPerUnit;
    }

    void removeUselessMoves()
    {
        bool coalesceSpillSlots = m_code.policy().frame.coalescesSpillSlots;
        if (!coalesceSpillSlots)
            return;
        for (BasicBlock* block : m_code) {
            block->insts().removeAllMatching([&](Inst& inst) {
                return isUselessMove(inst, coalesceSpillSlots);
            });
        }
    }

    void choosePools()
    {
        std::array<size_t, numberOfPools> numberOfSlotsInPool { };
        m_pools[OneUnit].unitsPerCell = 1;
        m_pools[TwoUnits].unitsPerCell = 2;
        m_pools[Wide].unitsPerCell = unitsPerWideAlignment;
        m_poolOfSlot.fill(NoPool, m_code.stackSlots().size());
        for (StackSlot* slot : m_code.stackSlots()) {
            if (m_placement == SlotPlacement::AssignsOffsets && slot->offsetFromFP()) {
                RELEASE_ASSERT(slot->offsetFromFP() < 0);
                m_firstFreeUnit = std::max<size_t>(m_firstFreeUnit, (static_cast<size_t>(-slot->offsetFromFP()) + StackColoring::bytesPerUnit - 1) / StackColoring::bytesPerUnit);
                continue;
            }
            if (!slot->isSpill())
                continue;
            const unsigned unitsNeeded = unitsNeededBy(slot);
            PoolKind pool = unitsNeeded <= 1 ? OneUnit : unitsNeeded == 2 ? TwoUnits : Wide;
            if (pool == Wide)
                m_pools[Wide].unitsPerCell = std::max<unsigned>(m_pools[Wide].unitsPerCell, WTF::roundUpToMultipleOf<unitsPerWideAlignment>(unitsNeeded));
            m_poolOfSlot[slot->index()] = pool;
            ++numberOfSlotsInPool[pool];
        }
        for (unsigned pool = 0; pool < numberOfPools; ++pool)
            m_pools[pool].reserve(numberOfSlotsInPool[pool]);
    }

    void collectRanges()
    {
        CompilerTimingScope timingScope("Air"_s, "StackAllocator::ranges"_s);

        StackSlotActions actions(m_code, false);
        Vector<BasicBlock*> blocks;
        Vector<uint32_t> firstPositions;
        size_t numberOfPositions = 0;
        for (BasicBlock* block : m_code) {
            blocks.append(block);
            firstPositions.append(static_cast<uint32_t>(numberOfPositions));
            numberOfPositions += block->size() + 1;
            RELEASE_ASSERT(numberOfPositions < noPosition);
        }

        BitMatrixLiveness<SpillSlotBoundaryRows> liveness(m_code, actions);
        // A slot that counts as live in a gap keeps its place there, and no more.
        liveness.countGapsAsLivePast(numberOfPositions * AirLimits::maxBoundaryIndicesPerInst);
        liveness.compute();
        RELEASE_ASSERT(liveness.status() == BitMatrixLivenessStatus::Solved);

        Vector<uint32_t> endOfOpenRange(FillWith { }, m_code.stackSlots().size(), noPosition);
        auto isLive = [&](uint32_t slotIndex) {
            return endOfOpenRange[slotIndex] != noPosition;
        };
        auto openRange = [&](uint32_t slotIndex, uint32_t position) {
            if (!isLive(slotIndex))
                endOfOpenRange[slotIndex] = position;
        };
        auto closeRange = [&](uint32_t slotIndex, uint32_t position) {
            RELEASE_ASSERT(isLive(slotIndex));
            m_ranges.append(Range { slotIndex, position, std::exchange(endOfOpenRange[slotIndex], noPosition) });
        };

        for (size_t order = blocks.size(); order--;) {
            BasicBlock* block = blocks[order];
            const uint32_t firstPosition = firstPositions[order];
            if (order + 1 < blocks.size()) {
                liveness.forEachLiveOnlyAtHeadOfNextBlock(block, [&](unsigned slotIndex) {
                    closeRange(slotIndex, firstPositions[order + 1]);
                });
            }
            liveness.forEachLiveOnlyAtTail(block, [&](unsigned slotIndex) {
                openRange(slotIndex, firstPosition + block->size());
            });

            auto blockActions = actions.actionsFor(block);
            auto killCandidates = actions.killCandidatesFor(block);
            bool killedAny = false;
            for (;;) {
                int64_t actionBoundary = blockActions.empty() ? -1 : static_cast<int64_t>(blockActions.back().boundary);
                int64_t killBoundary = killCandidates.empty() ? -1 : static_cast<int64_t>(killCandidates.back()) + 1;
                int64_t boundary = std::max(actionBoundary, killBoundary);
                if (boundary < 0)
                    break;

                const uint32_t position = firstPosition + static_cast<uint32_t>(boundary);
                std::span<const StackSlotActions::Action> group;
                if (actionBoundary == boundary)
                    group = StackSlotActions::consumeLastGroup(blockActions);
                for (const auto& action : group) {
                    if (action.isSpill && action.kind == StackSlotActions::UseAction)
                        openRange(action.slotIndex, position);
                }

                // A store whose late definitions are all dead is removed, as the graph coloring allocator does.
                bool killedPreviousInst = false;
                if (killBoundary == boundary) {
                    uint32_t candidateInstIndex = consumeLast(killCandidates);
                    killedPreviousInst = std::ranges::none_of(group, [&](const auto& action) {
                        return action.kind == StackSlotActions::LateDefAction && isLive(action.slotIndex);
                    });
                    if (killedPreviousInst) {
                        block->at(candidateInstIndex) = Inst();
                        killedAny = true;
                    }
                }

                for (const auto& action : group) {
                    if (!action.isSpill || action.kind == StackSlotActions::UseAction)
                        continue;
                    if (action.kind == StackSlotActions::LateDefAction && killedPreviousInst)
                        continue;
                    // A definition that nothing reads still writes to its slot.
                    openRange(action.slotIndex, position);
                    closeRange(action.slotIndex, position);
                }
            }

            if (killedAny) {
                block->insts().removeAllMatching(
                    [&] (const Inst& inst) -> bool {
                        return !inst;
                    });
            }
        }
        liveness.forEachLiveAtHeadOfFirstBlock([&](unsigned slotIndex) {
            closeRange(slotIndex, 0);
        });
    }

    // Sorts the ranges by start, joins those of one slot that touch, and tells each where the next one of its slot starts.
    void linkRanges()
    {
        m_ranges.removeAllMatching([&](const Range& range) {
            return m_poolOfSlot[range.slotIndex] == NoPool;
        });
        std::ranges::sort(m_ranges, [](const Range& a, const Range& b) {
            return a.start != b.start ? a.start < b.start : a.slotIndex < b.slotIndex;
        });

        constexpr size_t noRange = std::numeric_limits<size_t>::max();
        Vector<size_t> lastRangeOfSlot(FillWith { }, m_code.stackSlots().size(), noRange);
        for (size_t index = 0; index < m_ranges.size(); ++index) {
            Range& range = m_ranges[index];
            size_t& last = lastRangeOfSlot[range.slotIndex];
            if (last != noRange && m_ranges[last].end + 1 >= range.start) {
                m_ranges[last].end = std::max(m_ranges[last].end, range.end);
                range.slotIndex = noSlot;
                continue;
            }
            last = index;
        }
        m_ranges.removeAllMatching([&](const Range& range) {
            return range.slotIndex == noSlot;
        });

        m_lastEndOfSlot.fill(noPosition, m_code.stackSlots().size());
        Vector<uint32_t> startOfNextRange(FillWith { }, m_code.stackSlots().size(), noPosition);
        for (size_t index = m_ranges.size(); index--;) {
            Range& range = m_ranges[index];
            if (m_lastEndOfSlot[range.slotIndex] == noPosition)
                m_lastEndOfSlot[range.slotIndex] = range.end;
            range.startOfNextRange = std::exchange(startOfNextRange[range.slotIndex], range.start);
        }
    }

    template<typename StartFunc, typename EndFunc>
    void forEachRangeBoundaryInOrder(NOESCAPE const StartFunc& start, NOESCAPE const EndFunc& end) const
    {
        // A range holds its place at its last position, so what starts at a position comes before what ends there.
        size_t nextToStart = 0;
        for (uint32_t rangeToEnd : m_rangesByEnd) {
            for (; nextToStart < m_ranges.size() && m_ranges[nextToStart].start <= m_ranges[rangeToEnd].end; ++nextToStart)
                start(m_ranges[nextToStart]);
            end(m_ranges[rangeToEnd]);
        }
    }

    void assignCells()
    {
        CompilerTimingScope timingScope("Air"_s, "StackAllocator::assign"_s);

        m_rangesByEnd.grow(m_ranges.size());
        for (size_t index = 0; index < m_ranges.size(); ++index)
            m_rangesByEnd[index] = static_cast<uint32_t>(index);
        std::ranges::sort(m_rangesByEnd, [&](uint32_t a, uint32_t b) {
            return m_ranges[a].end != m_ranges[b].end ? m_ranges[a].end < m_ranges[b].end : a < b;
        });

        const size_t numberOfSlots = m_code.stackSlots().size();
        m_cellOfSlot.fill(noCell, numberOfSlots);
        Vector<uint32_t> ownerBelow(FillWith { }, numberOfSlots, noSlot);
        Vector<uint32_t> startAfterGap(FillWith { }, numberOfSlots, noPosition);

        forEachRangeBoundaryInOrder(
            [&](const Range& range) {
                CellPool& pool = m_pools[m_poolOfSlot[range.slotIndex]];
                uint32_t& cell = m_cellOfSlot[range.slotIndex];
                if (cell == noCell) {
                    cell = pool.findLowestCellFreeBeyond(m_lastEndOfSlot[range.slotIndex]);
                    pool.didUse(cell);
                    ownerBelow[range.slotIndex] = std::exchange(pool.topOwner(cell), range.slotIndex);
                }
                pool.setFreeUntil(cell, 0);
            },
            [&](const Range& range) {
                CellPool& pool = m_pools[m_poolOfSlot[range.slotIndex]];
                const uint32_t cell = m_cellOfSlot[range.slotIndex];
                if (range.startOfNextRange != noPosition) {
                    startAfterGap[range.slotIndex] = range.startOfNextRange;
                    pool.setFreeUntil(cell, range.startOfNextRange);
                    return;
                }
                RELEASE_ASSERT(pool.topOwner(cell) == range.slotIndex);
                const uint32_t owner = ownerBelow[range.slotIndex];
                pool.topOwner(cell) = owner;
                pool.setFreeUntil(cell, owner == noSlot ? noPosition : startAfterGap[owner]);
            });
    }

    StackColoring::UnitRange unitsOf(StackSlot* slot) const
    {
        const CellPool& pool = m_pools[m_poolOfSlot[slot->index()]];
        const unsigned end = pool.firstUnit + (m_cellOfSlot[slot->index()] + 1) * pool.unitsPerCell;
        return { end - unitsNeededBy(slot), end - 1 };
    }

    void placeSlots(StackColoring& coloring)
    {
        for (StackSlot* slot : m_code.stackSlots()) {
            // A slot that nothing names can be anywhere.
            if (m_poolOfSlot[slot->index()] != NoPool && m_cellOfSlot[slot->index()] == noCell) {
                m_cellOfSlot[slot->index()] = 0;
                m_pools[m_poolOfSlot[slot->index()]].didUse(0);
            }
        }

        size_t numberOfUnits = WTF::roundUpToMultipleOf<unitsPerWideAlignment>(m_firstFreeUnit);
        for (PoolKind kind : { Wide, TwoUnits, OneUnit }) {
            CellPool& pool = m_pools[kind];
            RELEASE_ASSERT(numberOfUnits <= std::numeric_limits<int32_t>::max() / StackColoring::bytesPerUnit);
            pool.firstUnit = static_cast<unsigned>(numberOfUnits);
            numberOfUnits += static_cast<size_t>(pool.numberOfCells()) * pool.unitsPerCell;
        }
        RELEASE_ASSERT(numberOfUnits <= std::numeric_limits<int32_t>::max() / StackColoring::bytesPerUnit);

        if (shouldValidateIRAtEachPhase(m_code.proc()))
            validate(numberOfUnits);

        switch (m_placement) {
        case SlotPlacement::AssignsOffsets:
            for (StackSlot* slot : m_code.stackSlots()) {
                if (m_poolOfSlot[slot->index()] != NoPool)
                    slot->setOffsetFromFP(-static_cast<intptr_t>(unitsOf(slot).last + 1) * static_cast<intptr_t>(StackColoring::bytesPerUnit));
            }
            updateFrameSizeBasedOnStackSlots(m_code);
            break;
        case SlotPlacement::OnlyColors:
            coloring.unitsOfSlot.fill(std::nullopt, m_code.stackSlots().size());
            for (StackSlot* slot : m_code.stackSlots()) {
                if (m_poolOfSlot[slot->index()] != NoPool)
                    coloring.unitsOfSlot[slot->index()] = unitsOf(slot);
            }
            coloring.numberOfUnits = static_cast<unsigned>(numberOfUnits);
            break;
        }
    }

    void validate(size_t numberOfUnits) const
    {
        Vector<uint32_t> slotAtUnit(FillWith { }, numberOfUnits, noSlot);
        forEachRangeBoundaryInOrder(
            [&](const Range& range) {
                StackColoring::UnitRange units = unitsOf(m_code.stackSlots()[range.slotIndex]);
                for (unsigned unit = units.first; unit <= units.last; ++unit)
                    RELEASE_ASSERT(std::exchange(slotAtUnit[unit], range.slotIndex) == noSlot);
            },
            [&](const Range& range) {
                StackColoring::UnitRange units = unitsOf(m_code.stackSlots()[range.slotIndex]);
                for (unsigned unit = units.first; unit <= units.last; ++unit)
                    RELEASE_ASSERT(std::exchange(slotAtUnit[unit], noSlot) == range.slotIndex);
            });
    }

    Code& m_code;
    const SlotPlacement m_placement;
    size_t m_firstFreeUnit { 0 };
    std::array<CellPool, numberOfPools> m_pools;
    Vector<PoolKind> m_poolOfSlot;
    Vector<Range> m_ranges;
    Vector<uint32_t> m_rangesByEnd;
    Vector<uint32_t> m_lastEndOfSlot;
    Vector<uint32_t> m_cellOfSlot;
};

// Avoid computing the liveness information if there is no spill slot to allocate
bool doTrivialStackAllocation(Code& code, SlotPlacement placement, StackColoring& coloring)
{
    switch (placement) {
    case SlotPlacement::AssignsOffsets:
        for (StackSlot* slot : code.stackSlots()) {
            if (slot->offsetFromFP())
                continue;
            return false;
        }
        updateFrameSizeBasedOnStackSlots(code);
        return true;
    case SlotPlacement::OnlyColors:
        for (StackSlot* slot : code.stackSlots()) {
            if (slot->isSpill())
                return false;
        }
        coloring.unitsOfSlot.fill(std::nullopt, code.stackSlots().size());
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
    return false;
}

// An upper bound on the number of interference edges: for each spill slot, the number of slots that are live at
// its definitions, capped at the number of other slots.
std::optional<size_t> computeInterferenceEdgeBound(Code& code, const StackSlotActions& actions, const StackSlotLivenessInWords& liveness)
{
    if (liveness.didGiveUp())
        return std::nullopt;

    auto liveSlots = liveness.makeWorkset();
    const size_t numberOfSlots = code.stackSlots().size();
    Vector<size_t> liveSlotsAtDefinitions;
    liveSlotsAtDefinitions.fill(0, numberOfSlots);
    for (BasicBlock* block : code) {
        auto blockActions = actions.actionsFor(block);
        if (blockActions.empty())
            continue;

        liveness.copyLiveAtTailInto(liveSlots, block);
        size_t numberOfLiveSlots = 0;
        for (uint64_t word : liveSlots.words())
            numberOfLiveSlots += std::popcount(word);

        while (!blockActions.empty()) {
            auto group = StackSlotActions::consumeLastGroup(blockActions);
            for (const auto& action : group) {
                if (action.kind == StackSlotActions::UseAction && !liveSlots.contains(action.slotIndex)) {
                    liveSlots.add(action.slotIndex);
                    ++numberOfLiveSlots;
                }
            }
            for (const auto& action : group) {
                if (action.kind != StackSlotActions::UseAction && action.isSpill)
                    liveSlotsAtDefinitions[action.slotIndex] += numberOfLiveSlots;
            }
            for (const auto& action : group) {
                if (action.kind != StackSlotActions::UseAction && liveSlots.contains(action.slotIndex)) {
                    liveSlots.remove(action.slotIndex);
                    --numberOfLiveSlots;
                }
            }
        }
    }

    size_t numberOfEdges = 0;
    for (size_t count : liveSlotsAtDefinitions)
        numberOfEdges += std::min(count, numberOfSlots - 1);
    return numberOfEdges;
}

// The number of pairs of spill slots that are live together if each is live from its first mention, in layout order, to its last.
size_t estimateInterferenceEdges(Code& code)
{
    constexpr uint32_t noPosition = std::numeric_limits<uint32_t>::max();
    Vector<uint32_t> lastMentionOfSlot(FillWith { }, code.stackSlots().size(), noPosition);
    Vector<uint32_t> firstMentions;
    uint32_t position = 0;
    for (BasicBlock* block : code) {
        for (Inst& inst : *block) {
            inst.forEach<StackSlot*>([&](StackSlot*& slot, Arg::Role, Bank, Width) {
                if (!slot->isSpill())
                    return;
                if (lastMentionOfSlot[slot->index()] == noPosition)
                    firstMentions.append(position);
                lastMentionOfSlot[slot->index()] = position;
            });
            ++position;
        }
    }
    lastMentionOfSlot.removeAll(noPosition);
    std::ranges::sort(lastMentionOfSlot);

    size_t numberOfEdges = 0;
    size_t numberOfSlotsBehindLastMention = 0;
    for (size_t index = 0; index < firstMentions.size(); ++index) {
        while (lastMentionOfSlot[numberOfSlotsBehindLastMention] < firstMentions[index])
            ++numberOfSlotsBehindLastMention;
        numberOfEdges += index - numberOfSlotsBehindLastMention;
    }
    return numberOfEdges;
}

struct StackSlotActionsAndLiveness {
    explicit StackSlotActionsAndLiveness(Code& code)
        : actions(code, code.policy().frame.coalescesSpillSlots)
        , liveness(code, actions)
    {
    }

    StackSlotActions actions;
    StackSlotLivenessInWords liveness;
};

bool shouldUseDenseInterferenceGraph(Code& code, bool& hasTooManyLiveSlots, std::optional<StackSlotActionsAndLiveness>& computed)
{
    const size_t numberOfSlots = code.stackSlots().size();
    if (numberOfSlots >= std::numeric_limits<DenseIterableInterferenceGraph::IndexType>::max())
        return false;
    if (numberOfSlots <= code.proc().limit(Limit::MaxSlotsForDenseInterference))
        return true;

    // Either graph is walked edge by edge.
    hasTooManyLiveSlots = estimateInterferenceEdges(code) > AirLimits::maxInterferenceEdgesAboveDenseLimit;
    if (hasTooManyLiveSlots)
        return false;
    computed.emplace(code);
    std::optional<size_t> numberOfEdges = computeInterferenceEdgeBound(code, computed->actions, computed->liveness);
    hasTooManyLiveSlots = !numberOfEdges || *numberOfEdges > AirLimits::maxInterferenceEdgesAboveDenseLimit;
    // Above the limit, use the rows only if the hash sets of the sparse graph would take at least as much memory.
    return !hasTooManyLiveSlots && 2 * *numberOfEdges * AirLimits::bytesPerNeighborInSparseInterference >= numberOfSlots * numberOfSlots / 8;
}

} // anonymous namespace

StackColoring allocateStackByGraphColoring(Code& code)
{
    PhaseScope phaseScope(code, "allocateStackByGraphColoring"_s);

    {
        CompilerTimingScope timingScope("Air"_s, "StackAllocator::calleeSaves"_s);
        handleCalleeSaves(code);
    }

    const SlotPlacement placement = code.policy().frame.emitsFrame ? SlotPlacement::OnlyColors : SlotPlacement::AssignsOffsets;

    Vector<StackSlot*> assignedEscapedStackSlots;
    if (placement == SlotPlacement::AssignsOffsets) {
        CompilerTimingScope timingScope("Air"_s, "StackAllocator::escapedSlots"_s);
        assignedEscapedStackSlots = allocateAndGetEscapedStackSlotsWithoutChangingFrameSize(code);
    }

    StackColoring coloring;
    bool hasTooManySlotsForLiveness = false;
    if (!doTrivialStackAllocation(code, placement, coloring)) {
        bool isAllocated = false;
        // The sets of live slots that were too large as rows of bits are larger still as the lists of the sparse graph's liveness.
        bool hasTooManyLiveSlots = false;
        std::optional<StackSlotActionsAndLiveness> computed;
        if (shouldUseDenseInterferenceGraph(code, hasTooManyLiveSlots, computed)) {
            using DenseAllocator = GraphColoringStackAllocator<DenseIterableInterferenceGraph, StackSlotLivenessInWords>;
            DenseAllocator allocator(code, placement);
            auto outcome = computed ? allocator.run(assignedEscapedStackSlots, coloring, computed->actions, computed->liveness) : allocator.run(assignedEscapedStackSlots, coloring);
            isAllocated = outcome == DenseAllocator::Outcome::Allocated;
            hasTooManyLiveSlots = !isAllocated;
        }
        computed.reset();
        if (!isAllocated) {
            code.proc().didReachLimit(Limit::MaxSlotsForDenseInterference);
            if (!hasTooManyLiveSlots && code.stackSlots().size() < std::numeric_limits<uint16_t>::max()) {
                GraphColoringStackAllocator<LargeIterableInterferenceGraph, StackSlotLiveness> allocator(code, placement);
                allocator.run(assignedEscapedStackSlots, coloring);
            } else {
                LiveRangeStackAllocator allocator(code, placement);
                allocator.run(coloring);
                hasTooManySlotsForLiveness = true;
            }
        }
    }

    if (placement == SlotPlacement::AssignsOffsets)
        code.setStackIsAllocated(true);
    else if (coloring.useCountOfSlot.isEmpty() && shouldOrderSlotsByUse(code) && numberOfWordsInSlots(code) >= AirLimits::minSlotsToOrderByUse)
        coloring.useCountOfSlot = useCountsOfSlots(code);
    if (placement == SlotPlacement::OnlyColors && !coloring.hasSuspensions)
        findSpillSlotsLiveAcrossSuspensions(code, coloring, hasTooManySlotsForLiveness);
    return coloring;
}

} } } // namespace JSC::B3::Air

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(B3_JIT)
