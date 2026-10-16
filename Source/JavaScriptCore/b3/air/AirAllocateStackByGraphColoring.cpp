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
#include "AirCFG.h"
#include "AirCode.h"
#include "AirFormTable.h"
#include "AirHandleCalleeSaves.h"
#include "AirInstInlines.h"
#include "AirPhaseScope.h"
#include "AirStackAllocation.h"
#include "AirStackAllocatorStats.h"
#include "CompilerTimingScope.h"
#include <wtf/ForbidHeapAllocation.h>
#include <wtf/GraphOrdering.h>
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
// i + 1, so staging the late actions of i and appending them once i is decoded keeps it sorted.
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

#if USE(BUN_JSC_ADDITIONS)
    static constexpr uint8_t isEarlyUse = 1;
    static constexpr uint8_t isEarlyDef = 2;
    static constexpr uint8_t isLateUse = 4;
    static constexpr uint8_t isLateDef = 8;

    static constexpr std::array<uint8_t, 256> makeTimingsOfForms()
    {
        std::array<uint8_t, 256> result { };
        for (unsigned form = 0; form < 256; ++form) {
            switch (static_cast<Arg::Role>((form >> formRoleShift) & formRoleMask)) {
            case Arg::Use:
            case Arg::ColdUse:
                result[form] = isEarlyUse;
                break;
            case Arg::LateUse:
            case Arg::LateColdUse:
                result[form] = isLateUse;
                break;
            case Arg::Def:
            case Arg::ZDef:
                result[form] = isLateDef;
                break;
            case Arg::UseDef:
            case Arg::UseZDef:
                result[form] = isEarlyUse | isLateDef;
                break;
            case Arg::EarlyDef:
            case Arg::EarlyZDef:
                result[form] = isEarlyDef;
                break;
            case Arg::Scratch:
                result[form] = isEarlyDef | isLateUse;
                break;
            case Arg::UseAddr:
                break;
            }
        }
        return result;
    }
#endif

    StackSlotActions(Code& code, bool coalesceSpillSlots)
        : m_code(code)
    {
        CompilerTimingScope timingScope("Air"_s, "StackAllocator::actions"_s);

        m_blockData.grow(code.size());

        Vector<Action, 8> lateActions;
        for (BasicBlock* block : code) {
            BlockData& data = m_blockData[block->index()];
            data.actionStart = m_actions.size();
            data.killStart = m_killCandidates.size();
            data.moveStart = m_moveSites.size();

            unsigned blockSize = block->size();
            for (unsigned instIndex = 0; instIndex < blockSize; ++instIndex) {
                Inst& inst = block->at(instIndex);

                // A dead store candidate has no effects beyond its arguments and defines only spill
                // slots, late. That is the only kind of dead stack store this phase will see.
                // Later, we will check whether this spill is alive or not. And if it is dead, we will remove this Inst.
                bool isKillCandidate = true;
#if USE(BUN_JSC_ADDITIONS)
                static constexpr std::array<uint8_t, 256> timingsOfForms = makeTimingsOfForms();
                unsigned argIndex = 0;
                if (const uint8_t* forms = inst.fixedForms(argIndex)) [[likely]] {
                    Arg* args = inst.args().data();
                    unsigned numberOfLateActions = 0;
                    for (unsigned numberOfArgs = inst.args().size(); argIndex < numberOfArgs; ++argIndex) {
                        uint8_t timings = timingsOfForms[forms[argIndex]];
                        Arg& arg = args[argIndex];
                        if (!arg.isStack()) [[likely]] {
                            if (timings & (isEarlyDef | isLateDef))
                                isKillCandidate = false;
                            continue;
                        }
                        StackSlot* slot = arg.stackSlot();
                        uint32_t index = slot->index();
                        bool isSpill = slot->kind() == StackSlotKind::Spill;
                        if (timings & isEarlyUse)
                            m_actions.insert(m_actions.size() - numberOfLateActions, Action { instIndex, index, UseAction, isSpill });
                        if (timings & isEarlyDef)
                            m_actions.insert(m_actions.size() - numberOfLateActions, Action { instIndex, index, EarlyDefAction, isSpill });
                        if (timings & isLateUse) {
                            m_actions.append({ instIndex + 1, index, UseAction, isSpill });
                            ++numberOfLateActions;
                        }
                        if (timings & isLateDef) {
                            m_actions.append({ instIndex + 1, index, LateDefAction, isSpill });
                            ++numberOfLateActions;
                        }
                        if ((timings & isEarlyDef) || ((timings & isLateDef) && !isSpill))
                            isKillCandidate = false;
                    }
                    if (isKillCandidate && !inst.hasNonArgEffects())
                        m_killCandidates.append(instIndex);
                    if (inst.args().size() == 3 && isCoalescableMove(inst, coalesceSpillSlots)) [[unlikely]] {
                        m_moveSites.append({ instIndex, inst.args()[0].stackSlot()->index(), inst.args()[1].stackSlot()->index() });
                        if (inst.args()[0] == inst.args()[1])
                            m_sawUselessMove = true;
                    }
                    continue;
                }
#endif
                lateActions.shrink(0);
                inst.forEachArg(
                    [&](Arg& arg, Arg::Role role, Bank, Width) {
                        bool isSpill = false;
                        if (arg.isStack()) {
                            StackSlot* slot = arg.stackSlot();
                            uint32_t index = slot->index();
                            isSpill = slot->kind() == StackSlotKind::Spill;
                            if (Arg::isEarlyUse(role))
                                m_actions.append({ instIndex, index, UseAction, isSpill });
                            if (Arg::isEarlyDef(role))
                                m_actions.append({ instIndex, index, EarlyDefAction, isSpill });
                            if (Arg::isLateUse(role))
                                lateActions.append({ instIndex + 1, index, UseAction, isSpill });
                            if (Arg::isLateDef(role))
                                lateActions.append({ instIndex + 1, index, LateDefAction, isSpill });
                        }
                        if (Arg::isEarlyDef(role))
                            isKillCandidate = false;
                        else if (Arg::isLateDef(role) && !isSpill)
                            isKillCandidate = false;
                    });
                m_actions.appendVector(lateActions);

                if (isKillCandidate && !inst.hasNonArgEffects())
                    m_killCandidates.append(instIndex);

                if (isCoalescableMove(inst, coalesceSpillSlots)) {
                    m_moveSites.append({ instIndex, inst.args()[0].stackSlot()->index(), inst.args()[1].stackSlot()->index() });
                    if (inst.args()[0] == inst.args()[1])
                        m_sawUselessMove = true;
                }
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
};

#if USE(BUN_JSC_ADDITIONS)
class LowestFirstWorklist {
public:
    explicit LowestFirstWorklist(size_t size)
        : m_words(FillWith { }, (size + 63) / 64, static_cast<uint64_t>(0))
    {
    }

    void add(unsigned position)
    {
        m_words[position / 64] |= static_cast<uint64_t>(1) << (position % 64);
        m_lowerBound = std::min<size_t>(m_lowerBound, position);
    }

    std::optional<unsigned> takeLowest()
    {
        for (size_t wordIndex = m_lowerBound / 64; wordIndex < m_words.size(); ++wordIndex) {
            uint64_t word = m_words[wordIndex];
            if (!word)
                continue;
            m_words[wordIndex] = word & (word - 1);
            m_lowerBound = wordIndex * 64 + std::countr_zero(word);
            return m_lowerBound;
        }
        m_lowerBound = m_words.size() * 64;
        return std::nullopt;
    }

private:
    Vector<uint64_t> m_words;
    size_t m_lowerBound { 0 };
};

class StackSlotLivenessInWords {
    WTF_FORBID_HEAP_ALLOCATION;
public:
    static constexpr size_t maxWordsPerMatrix = 1 << 20;
    static constexpr size_t maxWordsOfTails = 1 << 24;

    class Workset {
    public:
        explicit Workset(size_t numberOfWords)
            : m_words(FillWith { }, numberOfWords, static_cast<uint64_t>(0))
        {
        }

        void add(unsigned index) { m_words[index / 64] |= static_cast<uint64_t>(1) << (index % 64); }
        void remove(unsigned index) { m_words[index / 64] &= ~(static_cast<uint64_t>(1) << (index % 64)); }
        bool contains(unsigned index) const { return m_words[index / 64] >> (index % 64) & 1; }
        std::span<const uint64_t> words() const { return m_words.span(); }
        std::span<uint64_t> mutableWords() { return m_words.mutableSpan(); }

        void forEachSetBit(NOESCAPE const Invocable<void(unsigned)> auto& func) const
        {
            for (size_t wordIndex = 0; wordIndex < m_words.size(); ++wordIndex) {
                for (uint64_t word = m_words[wordIndex]; word; word &= word - 1)
                    func(wordIndex * 64 + std::countr_zero(word));
            }
        }

    private:
        Vector<uint64_t, 8> m_words;
    };

    StackSlotLivenessInWords(Code& code, const StackSlotActions& actions)
        : actions(actions)
        , m_wordsPerSet((actions.numIndices() + 63) / 64)
    {
        CompilerTimingScope timingScope("Air"_s, "StackAllocator::liveness"_s);

        const size_t wordsPerSet = m_wordsPerSet;
        const size_t wordsPerMatrix = code.size() * wordsPerSet;
        if (wordsPerMatrix > maxWordsPerMatrix) {
            solveInSlices(code);
            return;
        }
        m_store.fill(0, 4 * wordsPerMatrix);
        uint64_t* liveAtHead = m_store.mutableSpan().data();
        uint64_t* liveAtTail = liveAtHead + wordsPerMatrix;
        uint64_t* gens = liveAtTail + wordsPerMatrix;
        uint64_t* kills = gens + wordsPerMatrix;

        Vector<unsigned, 64> worklist;
        Vector<uint8_t, 64> isDirty(FillWith { }, code.size(), static_cast<uint8_t>(0));
        for (BasicBlock* block : code) {
            auto blockActions = actions.actionsFor(block);
            if (blockActions.empty())
                continue;
            uint64_t* tail = liveAtTail + block->index() * wordsPerSet;
            uint64_t* gen = gens + block->index() * wordsPerSet;
            uint64_t* kill = kills + block->index() * wordsPerSet;
            unsigned blockSize = block->size();
            while (!blockActions.empty()) {
                auto group = StackSlotActions::consumeLastGroup(blockActions);
                uint64_t* uses = group.front().boundary == blockSize ? tail : gen;
                for (const auto& action : group) {
                    if (action.kind == StackSlotActions::UseAction)
                        uses[action.slotIndex / 64] |= static_cast<uint64_t>(1) << (action.slotIndex % 64);
                }
                for (const auto& action : group) {
                    if (action.kind != StackSlotActions::UseAction) {
                        gen[action.slotIndex / 64] &= ~(static_cast<uint64_t>(1) << (action.slotIndex % 64));
                        kill[action.slotIndex / 64] |= static_cast<uint64_t>(1) << (action.slotIndex % 64);
                    }
                }
            }
            isDirty[block->index()] = 1;
            worklist.append(block->index());
        }

        while (!worklist.isEmpty()) {
            unsigned blockIndex = worklist.takeLast();
            isDirty[blockIndex] = 0;
            uint64_t* head = liveAtHead + blockIndex * wordsPerSet;
            const uint64_t* tail = liveAtTail + blockIndex * wordsPerSet;
            const uint64_t* gen = gens + blockIndex * wordsPerSet;
            const uint64_t* kill = kills + blockIndex * wordsPerSet;
            uint64_t changed = 0;
            for (size_t i = 0; i < wordsPerSet; ++i) {
                uint64_t newHead = gen[i] | (tail[i] & ~kill[i]);
                changed |= newHead ^ head[i];
                head[i] = newHead;
            }
            if (!changed)
                continue;
            for (BasicBlock* predecessor : code[blockIndex]->predecessors()) {
                uint64_t* tailOfPredecessor = liveAtTail + predecessor->index() * wordsPerSet;
                uint64_t predecessorChanged = 0;
                for (size_t i = 0; i < wordsPerSet; ++i) {
                    predecessorChanged |= head[i] & ~tailOfPredecessor[i];
                    tailOfPredecessor[i] |= head[i];
                }
                if (predecessorChanged && !isDirty[predecessor->index()]) {
                    isDirty[predecessor->index()] = 1;
                    worklist.append(predecessor->index());
                }
            }
        }
        m_liveAtTail = liveAtTail;
    }

    Workset makeWorkset() const { return Workset(m_wordsPerSet); }

    void copyLiveAtTailInto(Workset& workset, BasicBlock* block) const
    {
        uint64_t* words = workset.mutableWords().data();
        if (!m_rowOfBlock.isEmpty()) {
            for (size_t i = 0; i < m_wordsPerSet; ++i)
                words[i] = 0;
            unsigned row = m_rowOfBlock[block->index()];
            for (unsigned i = m_firstPieceOfRow[row]; i < m_firstPieceOfRow[row + 1]; ++i) {
                const PieceOfTail& piece = m_piecesOfTails[i];
                const uint64_t* tail = m_liveAtTail + piece.offsetInStore;
                for (size_t word = 0; word < piece.numberOfWords; ++word)
                    words[piece.firstWord + word] = tail[word];
            }
            return;
        }
        const uint64_t* tail = m_liveAtTail + block->index() * m_wordsPerSet;
        for (size_t i = 0; i < m_wordsPerSet; ++i)
            words[i] = tail[i];
    }

    bool isTooLarge() const { return m_isTooLarge; }

    const StackSlotActions& actions;

private:
    NEVER_INLINE void solveInSlices(Code& code)
    {
        constexpr unsigned noRow = std::numeric_limits<unsigned>::max();
        const unsigned numberOfBlocks = code.size();
        const size_t wordsPerSlice = std::max<size_t>(1, maxWordsPerMatrix / numberOfBlocks);

        m_rowOfBlock.fill(noRow, numberOfBlocks);
        unsigned numberOfRows = 0;
        for (BasicBlock* block : code) {
            if (!actions.actionsFor(block).empty() || !actions.killCandidatesFor(block).empty() || !actions.moveSitesFor(block).empty())
                m_rowOfBlock[block->index()] = numberOfRows++;
        }
        Vector<PieceOfTail> pieces;

        Vector<uint64_t> scratch(FillWith { }, 4 * numberOfBlocks * std::min(wordsPerSlice, m_wordsPerSet), static_cast<uint64_t>(0));
        Vector<unsigned> sliceThatTouched(FillWith { }, numberOfBlocks, 0u);
        Vector<unsigned> touched;
        unsigned stamp = 0;

        Vector<unsigned> blocksInPostOrder;
        BitVector isInOrder(numberOfBlocks);
        appendNodeIndicesInOrder(code.cfg(), GraphOrder::PostOrder, isInOrder, blocksInPostOrder);
        for (BasicBlock* block : code) {
            if (!isInOrder.quickSet(block->index()))
                blocksInPostOrder.append(block->index());
        }
        Vector<unsigned> positionOfBlock(FillWith { }, numberOfBlocks, 0u);
        for (unsigned position = 0; position < blocksInPostOrder.size(); ++position)
            positionOfBlock[blocksInPostOrder[position]] = position;
        LowestFirstWorklist worklist(blocksInPostOrder.size());
        for (size_t firstWord = 0; firstWord < m_wordsPerSet; firstWord += wordsPerSlice) {
            const size_t words = std::min(wordsPerSlice, m_wordsPerSet - firstWord);
            const size_t wordsPerMatrix = numberOfBlocks * words;
            uint64_t* liveAtHead = scratch.mutableSpan().data();
            uint64_t* liveAtTail = liveAtHead + wordsPerMatrix;
            uint64_t* gens = liveAtTail + wordsPerMatrix;
            uint64_t* kills = gens + wordsPerMatrix;
            ++stamp;
            touched.shrink(0);

            auto touch = [&](unsigned blockIndex) {
                if (sliceThatTouched[blockIndex] == stamp)
                    return;
                sliceThatTouched[blockIndex] = stamp;
                touched.append(blockIndex);
            };

            for (BasicBlock* block : code) {
                auto blockActions = actions.actionsFor(block);
                if (blockActions.empty())
                    continue;
                uint64_t* tail = liveAtTail + block->index() * words;
                uint64_t* gen = gens + block->index() * words;
                uint64_t* kill = kills + block->index() * words;
                unsigned blockSize = block->size();
                bool isNamedHere = false;
                while (!blockActions.empty()) {
                    auto group = StackSlotActions::consumeLastGroup(blockActions);
                    uint64_t* uses = group.front().boundary == blockSize ? tail : gen;
                    for (const auto& action : group) {
                        size_t word = action.slotIndex / 64 - firstWord;
                        if (action.kind == StackSlotActions::UseAction && word < words) {
                            uses[word] |= static_cast<uint64_t>(1) << (action.slotIndex % 64);
                            isNamedHere = true;
                        }
                    }
                    for (const auto& action : group) {
                        size_t word = action.slotIndex / 64 - firstWord;
                        if (action.kind != StackSlotActions::UseAction && word < words) {
                            gen[word] &= ~(static_cast<uint64_t>(1) << (action.slotIndex % 64));
                            kill[word] |= static_cast<uint64_t>(1) << (action.slotIndex % 64);
                            isNamedHere = true;
                        }
                    }
                }
                if (!isNamedHere)
                    continue;
                touch(block->index());
                worklist.add(positionOfBlock[block->index()]);
            }

            while (auto position = worklist.takeLowest()) {
                unsigned blockIndex = blocksInPostOrder[*position];
                uint64_t* head = liveAtHead + blockIndex * words;
                const uint64_t* tail = liveAtTail + blockIndex * words;
                const uint64_t* gen = gens + blockIndex * words;
                const uint64_t* kill = kills + blockIndex * words;
                uint64_t changed = 0;
                for (size_t i = 0; i < words; ++i) {
                    uint64_t newHead = gen[i] | (tail[i] & ~kill[i]);
                    changed |= newHead ^ head[i];
                    head[i] = newHead;
                }
                if (!changed)
                    continue;
                for (BasicBlock* predecessor : code[blockIndex]->predecessors()) {
                    uint64_t* tailOfPredecessor = liveAtTail + predecessor->index() * words;
                    uint64_t predecessorChanged = 0;
                    for (size_t i = 0; i < words; ++i) {
                        predecessorChanged |= head[i] & ~tailOfPredecessor[i];
                        tailOfPredecessor[i] |= head[i];
                    }
                    if (!predecessorChanged)
                        continue;
                    touch(predecessor->index());
                    worklist.add(positionOfBlock[predecessor->index()]);
                }
            }

            for (unsigned blockIndex : touched) {
                size_t offset = blockIndex * words;
                if (unsigned row = m_rowOfBlock[blockIndex]; row != noRow) {
                    uint64_t any = 0;
                    for (size_t i = 0; i < words; ++i)
                        any |= liveAtTail[offset + i];
                    if (any) {
                        pieces.append(PieceOfTail { row, static_cast<unsigned>(firstWord), static_cast<unsigned>(words), m_store.size() });
                        m_store.append(std::span<const uint64_t> { liveAtTail + offset, words });
                    }
                }
                for (uint64_t* matrix : { liveAtHead, liveAtTail, gens, kills }) {
                    for (size_t i = 0; i < words; ++i)
                        matrix[offset + i] = 0;
                }
            }
            if (m_store.size() > maxWordsOfTails) {
                m_isTooLarge = true;
                return;
            }
        }

        m_liveAtTail = m_store.span().data();
        m_firstPieceOfRow.fill(0, numberOfRows + 1);
        for (const PieceOfTail& piece : pieces)
            ++m_firstPieceOfRow[piece.row];
        unsigned sum = 0;
        for (unsigned& first : m_firstPieceOfRow)
            sum += std::exchange(first, sum);
        m_piecesOfTails.grow(pieces.size());
        for (const PieceOfTail& piece : pieces)
            m_piecesOfTails[m_firstPieceOfRow[piece.row]++] = piece;
        for (unsigned row = numberOfRows; row; --row)
            m_firstPieceOfRow[row] = m_firstPieceOfRow[row - 1];
        m_firstPieceOfRow[0] = 0;
    }

    struct PieceOfTail {
        unsigned row;
        unsigned firstWord;
        unsigned numberOfWords;
        size_t offsetInStore;
    };

    size_t m_wordsPerSet;
    Vector<uint64_t> m_store;
    Vector<unsigned> m_rowOfBlock;
    Vector<unsigned> m_firstPieceOfRow;
    Vector<PieceOfTail> m_piecesOfTails;
    const uint64_t* m_liveAtTail { nullptr };
    bool m_isTooLarge { false };
};
#endif

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

#if USE(BUN_JSC_ADDITIONS)
class InterferenceRows {
public:
    using IndexType = uint16_t;
    static constexpr unsigned maxSize = 65535;
    static constexpr unsigned minSizeToMirrorEdgesAtOnce = 1024;

    void setMaxIndex(unsigned n)
    {
        m_size = n;
        m_wordsPerRow = (n + 63) / 64;
        m_mirrorsEdgesAtOnce = n >= minSizeToMirrorEdgesAtOnce;
        m_rows.fill(0, (m_mirrorsEdgesAtOnce ? m_wordsPerRow * 64 : static_cast<size_t>(n)) * m_wordsPerRow);
    }

    bool contains(IndexType u, IndexType v) { return row(u)[v / 64] >> (v % 64) & 1; }

    void add(IndexType u, IndexType v)
    {
        row(u)[v / 64] |= static_cast<uint64_t>(1) << (v % 64);
        row(v)[u / 64] |= static_cast<uint64_t>(1) << (u % 64);
    }

    void addAllBut(IndexType u, std::span<const uint64_t> others)
    {
        ASSERT(others.size() == m_wordsPerRow);
        uint64_t* rowOfU = row(u);
        uint64_t* wordOfU = m_rows.mutableSpan().data() + u / 64;
        const uint64_t bitOfU = static_cast<uint64_t>(1) << (u % 64);
        if (m_mirrorsEdgesAtOnce) {
            for (size_t wordIndex = 0; wordIndex < m_wordsPerRow; ++wordIndex)
                rowOfU[wordIndex] |= others[wordIndex];
            rowOfU[u / 64] &= ~bitOfU;
            return;
        }
        for (size_t wordIndex = 0; wordIndex < m_wordsPerRow; ++wordIndex) {
            uint64_t word = others[wordIndex];
            if (wordIndex == u / 64)
                word &= ~bitOfU;
            uint64_t newOnes = word & ~rowOfU[wordIndex];
            rowOfU[wordIndex] |= word;
            for (; newOnes; newOnes &= newOnes - 1)
                wordOfU[(wordIndex * 64 + std::countr_zero(newOnes)) * m_wordsPerRow] |= bitOfU;
        }
    }

    void mirrorEdges()
    {
        if (!m_mirrorsEdgesAtOnce)
            return;
        std::array<uint64_t, 64> upper;
        std::array<uint64_t, 64> lower;
        uint64_t* rows = m_rows.mutableSpan().data();
        auto load = [&](std::array<uint64_t, 64>& tile, size_t rowOfTiles, size_t columnOfTiles) {
            uint64_t any = 0;
            for (size_t i = 0; i < 64; ++i)
                any |= tile[i] = rows[(rowOfTiles * 64 + i) * m_wordsPerRow + columnOfTiles];
            return any;
        };
        auto store = [&](const std::array<uint64_t, 64>& tile, size_t rowOfTiles, size_t columnOfTiles) {
            for (size_t i = 0; i < 64; ++i)
                rows[(rowOfTiles * 64 + i) * m_wordsPerRow + columnOfTiles] = tile[i];
        };
        for (size_t rowOfTiles = 0; rowOfTiles < m_wordsPerRow; ++rowOfTiles) {
            for (size_t columnOfTiles = rowOfTiles; columnOfTiles < m_wordsPerRow; ++columnOfTiles) {
                uint64_t any = load(upper, rowOfTiles, columnOfTiles);
                any |= load(lower, columnOfTiles, rowOfTiles);
                if (!any)
                    continue;
                transpose(lower);
                for (size_t i = 0; i < 64; ++i)
                    upper[i] |= lower[i];
                store(upper, rowOfTiles, columnOfTiles);
                transpose(upper);
                store(upper, columnOfTiles, rowOfTiles);
            }
        }
    }

    size_t countAmong(IndexType u, std::span<const uint64_t> others)
    {
        const uint64_t* rowOfU = row(u);
        size_t result = 0;
        for (size_t wordIndex = 0; wordIndex < m_wordsPerRow; ++wordIndex)
            result += std::popcount(rowOfU[wordIndex] & others[wordIndex]);
        return result;
    }

    void forEachNotAmongRow(IndexType u, std::span<const uint64_t> others, NOESCAPE const Invocable<void(unsigned)> auto& func)
    {
        const uint64_t* rowOfU = row(u);
        for (size_t wordIndex = 0; wordIndex < m_wordsPerRow; ++wordIndex) {
            for (uint64_t word = others[wordIndex] & ~rowOfU[wordIndex]; word; word &= word - 1)
                func(wordIndex * 64 + std::countr_zero(word));
        }
    }

    void forEachAmong(IndexType u, std::span<const uint64_t> others, NOESCAPE const Invocable<void(unsigned)> auto& func)
    {
        ASSERT(others.size() == m_wordsPerRow);
        const uint64_t* rowOfU = row(u);
        for (size_t wordIndex = 0; wordIndex < m_wordsPerRow; ++wordIndex) {
            for (uint64_t word = rowOfU[wordIndex] & others[wordIndex]; word; word &= word - 1)
                func(wordIndex * 64 + std::countr_zero(word));
        }
    }

    void mayClear(IndexType) { }
    unsigned memoryUse() const { return m_rows.size() * sizeof(uint64_t); }
    void dumpMemoryUseInKB() const { dataLog(memoryUse() / 1024); }

    class Iterable {
    public:
        class iterator {
        public:
            iterator(const uint64_t* words, size_t numberOfWords)
                : m_words(words)
                , m_numberOfWords(numberOfWords)
            {
                if (m_numberOfWords)
                    m_word = m_words[0];
                skip();
            }

            IndexType operator*() const { return m_wordIndex * 64 + std::countr_zero(m_word); }

            iterator& operator++()
            {
                m_word &= m_word - 1;
                skip();
                return *this;
            }

            bool operator==(std::default_sentinel_t) const { return m_wordIndex == m_numberOfWords; }

        private:
            void skip()
            {
                while (!m_word && m_wordIndex < m_numberOfWords) {
                    if (++m_wordIndex < m_numberOfWords)
                        m_word = m_words[m_wordIndex];
                }
            }

            const uint64_t* m_words;
            size_t m_numberOfWords;
            size_t m_wordIndex { 0 };
            uint64_t m_word { 0 };
        };

        Iterable(const uint64_t* words, size_t numberOfWords)
            : m_words(words)
            , m_numberOfWords(numberOfWords)
        {
        }

        iterator begin() const { return iterator(m_words, m_numberOfWords); }
        std::default_sentinel_t end() const { return { }; }

    private:
        const uint64_t* m_words;
        size_t m_numberOfWords;
    };

    Iterable operator[](IndexType u) { return Iterable(row(u), m_wordsPerRow); }

private:
    uint64_t* row(IndexType u) { return m_rows.mutableSpan().data() + static_cast<size_t>(u) * m_wordsPerRow; }

    static void transpose(std::array<uint64_t, 64>& tile)
    {
        uint64_t mask = 0x00000000ffffffffULL;
        for (unsigned distance = 32; distance; distance >>= 1, mask ^= mask << distance) {
            for (unsigned i = 0; i < 64; i = (i + distance + 1) & ~distance) {
                uint64_t swapped = ((tile[i] >> distance) ^ tile[i + distance]) & mask;
                tile[i] ^= swapped << distance;
                tile[i + distance] ^= swapped;
            }
        }
    }

    Vector<uint64_t> m_rows;
    size_t m_wordsPerRow { 0 };
    unsigned m_size { 0 };
    bool m_mirrorsEdgesAtOnce { false };
};
#endif

template<typename InterferenceGraph>
class GraphColoringStackAllocator final : public StackAllocatorBase {
    using IndexType = typename InterferenceGraph::IndexType;
#if USE(BUN_JSC_ADDITIONS)
    using LivenessOfSlots = std::conditional_t<std::is_same_v<InterferenceGraph, InterferenceRows>, StackSlotLivenessInWords, StackSlotLiveness>;
#else
    using LivenessOfSlots = StackSlotLiveness;
#endif
public:
    GraphColoringStackAllocator(Code& code)
        : StackAllocatorBase(code)
        , m_interference()
        , m_coalescableMoves()
        , m_stats(code.proc().name())
    {
#if USE(BUN_JSC_ADDITIONS)
        if constexpr (std::is_same_v<InterferenceGraph, InterferenceRows>)
            return;
#endif
        m_interference.setMaxIndex(m_code.stackSlots().size());
    }

    bool run(const Vector<StackSlot*>& assignedEscapedStackSlots)
    {
        bool coalesceSpillSlots = Options::coalesceSpillSlots();

        StackSlotActions actions(m_code, coalesceSpillSlots);
        LivenessOfSlots liveness(m_code, actions);
#if USE(BUN_JSC_ADDITIONS)
        if constexpr (std::is_same_v<InterferenceGraph, InterferenceRows>) {
            if (liveness.isTooLarge())
                return false;
            m_interference.setMaxIndex(m_code.stackSlots().size());
        }
#endif
        buildInterferenceGraph(liveness);

        coalesceSlots(coalesceSpillSlots, actions.sawUselessMove());
        assignStackLocations(assignedEscapedStackSlots);

        updateFrameSizeBasedOnStackSlots(m_code);
#if USE(BUN_JSC_ADDITIONS)
        orderWordsByUse();
#endif
        m_stats.frameSize = m_code.frameSize();
        return true;
    }

#if USE(BUN_JSC_ADDITIONS)
    void keepInPlace(const BitVector& slots) { m_staysInPlace = slots; }
#endif

private:
    void buildInterferenceGraph(LivenessOfSlots& liveness)
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
                        uint32_t slotIndex = action.slotIndex;
#if USE(BUN_JSC_ADDITIONS)
                        if constexpr (std::is_same_v<InterferenceGraph, InterferenceRows>) {
                            m_interference.addAllBut(slotIndex, workset.words());
                            continue;
                        }
#endif
                        workset.forEachSetBit(
                            [&](unsigned otherSlotIndex) {
                                addEdge(slotIndex, otherSlotIndex);
                            });
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
#if USE(BUN_JSC_ADDITIONS)
        if constexpr (std::is_same_v<InterferenceGraph, InterferenceRows>)
            m_interference.mirrorEdges();
#endif

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
            for (Inst& inst : *block) {
                for (Arg& arg : inst.args()) {
                    if (arg.isStack())
                        arg = Arg::stack(remapStackSlot(arg.stackSlot()), arg.offset());
                }
                if (isUselessMove(inst, coalesceSpillSlots))
                    inst = Inst();
            }
        }
    }

#if USE(BUN_JSC_ADDITIONS)
    void countHowOftenSlotsAreNamed()
    {
        if (!m_timesNamed.isEmpty())
            return;
        m_timesNamed.fill(0, m_code.stackSlots().size());
        for (BasicBlock* block : m_code) {
            for (Inst& inst : *block) {
                for (Arg& arg : inst.args()) {
                    if (arg.isStack())
                        ++m_timesNamed[arg.stackSlot()->index()];
                }
            }
        }
    }

    void orderWordsByUse()
    {
        constexpr size_t bytesPerWord = 8;
        constexpr size_t wordsWithinOneByteOfFramePointer = 16;
        size_t frameSize = m_code.frameSize();
        size_t numberOfWords = frameSize / bytesPerWord;
        if (!isX86() || !m_code.chargesForCalleeSaves() || numberOfWords <= wordsWithinOneByteOfFramePointer || frameSize % bytesPerWord)
            return;
        countHowOftenSlotsAreNamed();
        auto wordsOf = [&](StackSlot* slot) {
            size_t below = -slot->offsetFromFP();
            return std::pair { (below - slot->byteSize()) / bytesPerWord, (below - 1) / bytesPerWord };
        };
        auto forEachSlotInFrame = [&](const auto& functor) {
            for (StackSlot* slot : m_code.stackSlots()) {
                if (!isRemappedSlotIndex(slot->index()) && slot->offsetFromFP() < 0)
                    functor(slot);
            }
        };
        Vector<unsigned> timesWordIsNamed(FillWith { }, numberOfWords, 0u);
        Vector<bool> isInLockedSlot(FillWith { }, numberOfWords, false);
        Vector<bool> isOfSlotOverWords(FillWith { }, numberOfWords, false);
        forEachSlotInFrame([&](StackSlot* slot) {
            auto [first, last] = wordsOf(slot);
            RELEASE_ASSERT(last < numberOfWords);
            for (size_t word = first; word <= last; ++word) {
                timesWordIsNamed[word] += m_timesNamed[slot->index()];
                isInLockedSlot[word] |= slot->isLocked();
                isOfSlotOverWords[word] |= first != last;
            }
        });
        Vector<intptr_t> wordsFurtherDown(FillWith { }, numberOfWords, static_cast<intptr_t>(0));
        auto moveSlots = [&] {
            forEachSlotInFrame([&](StackSlot* slot) {
                slot->setOffsetFromFP(slot->offsetFromFP() - wordsFurtherDown[wordsOf(slot).first] * static_cast<intptr_t>(bytesPerWord));
                RELEASE_ASSERT(!(static_cast<size_t>(-slot->offsetFromFP()) % slot->alignment()));
            });
        };

        struct Item {
            size_t firstWord;
            size_t numberOfWords;
            unsigned timesNamed;
            bool isAlignedTo16;
            size_t place;
        };
        Vector<Item> blocks;
        Vector<Item> singles;
        Vector<bool> stays(FillWith { }, numberOfWords, false);
        size_t wordsOnTop = 0;
        bool isPossible = true;
        forEachSlotInFrame([&](StackSlot* slot) {
            auto [first, last] = wordsOf(slot);
            if (!slot->isLocked()) {
                isPossible &= first == last;
                return;
            }
            if (m_staysInPlace.get(slot->index())) {
                for (size_t word = first; word <= last; ++word)
                    stays[word] = true;
                wordsOnTop = std::max(wordsOnTop, last + 1);
                return;
            }
            isPossible &= !(slot->byteSize() % bytesPerWord) && !(static_cast<size_t>(-slot->offsetFromFP()) % bytesPerWord) && slot->alignment() <= 2 * bytesPerWord;
            blocks.append({ first, last - first + 1, m_timesNamed[slot->index()], slot->alignment() > bytesPerWord, 0 });
        });
        if (isPossible && !blocks.isEmpty()) {
            for (size_t word = 0; word < numberOfWords; ++word) {
                if (!isInLockedSlot[word])
                    singles.append({ word, 1, timesWordIsNamed[word], false, 0 });
            }
            auto isDenser = [](const Item& a, const Item& b) {
                return static_cast<uint64_t>(a.timesNamed) * b.numberOfWords > static_cast<uint64_t>(b.timesNamed) * a.numberOfWords;
            };
            std::stable_sort(blocks.begin(), blocks.end(), isDenser);
            std::stable_sort(singles.begin(), singles.end(), isDenser);
            size_t nextBlock = 0;
            size_t nextSingle = 0;
            size_t top = wordsOnTop;
            size_t bottom = numberOfWords;
            for (size_t word = 0; word < wordsOnTop; ++word) {
                if (stays[word])
                    continue;
                if (nextSingle == singles.size()) {
                    isPossible = false;
                    break;
                }
                singles[nextSingle++].place = word;
            }
            auto layOne = [&](bool fromTop) {
                bool takesBlock = nextBlock < blocks.size() && (nextSingle == singles.size() || isDenser(blocks[nextBlock], singles[nextSingle]));
                if (takesBlock && blocks[nextBlock].isAlignedTo16 && ((fromTop ? top + blocks[nextBlock].numberOfWords : bottom) & 1)) {
                    if (nextSingle == singles.size())
                        return false;
                    takesBlock = false;
                }
                Item& item = takesBlock ? blocks[nextBlock++] : singles[nextSingle++];
                if (bottom - top < item.numberOfWords)
                    return false;
                if (fromTop) {
                    item.place = top;
                    top += item.numberOfWords;
                } else {
                    bottom -= item.numberOfWords;
                    item.place = bottom;
                }
                return true;
            };
            auto isLeft = [&] { return nextBlock < blocks.size() || nextSingle < singles.size(); };
            while (isPossible && isLeft() && top < wordsWithinOneByteOfFramePointer)
                isPossible = layOne(true);
            while (isPossible && isLeft() && numberOfWords - bottom < wordsWithinOneByteOfFramePointer)
                isPossible = layOne(false);
            while (isPossible && isLeft())
                isPossible = layOne(true);
            if (isPossible && top == bottom) {
                auto note = [&](const Item& item) {
                    for (size_t i = 0; i < item.numberOfWords; ++i)
                        wordsFurtherDown[item.firstWord + i] = static_cast<intptr_t>(item.place) - static_cast<intptr_t>(item.firstWord);
                };
                for (auto& item : blocks)
                    note(item);
                for (auto& item : singles)
                    note(item);
                moveSlots();
                return;
            }
        }

        Vector<size_t> words;
        for (size_t word = 0; word < numberOfWords; ++word) {
            if (!isInLockedSlot[word] && !isOfSlotOverWords[word])
                words.append(word);
        }
        Vector<size_t> places = words;
        auto priceOf = [&](size_t place) -> unsigned {
            size_t below = (place + 1) * bytesPerWord;
            if (below <= wordsWithinOneByteOfFramePointer * bytesPerWord)
                return 0;
            return frameSize - below <= 127 - (bytesPerWord - 1) ? 1 : 3;
        };
        std::stable_sort(places.begin(), places.end(), [&](size_t a, size_t b) {
            return priceOf(a) < priceOf(b);
        });
        std::stable_sort(words.begin(), words.end(), [&](size_t a, size_t b) {
            return timesWordIsNamed[a] > timesWordIsNamed[b];
        });
        for (size_t i = 0; i < words.size(); ++i)
            wordsFurtherDown[words[i]] = static_cast<intptr_t>(places[i]) - static_cast<intptr_t>(words[i]);
        moveSlots();
    }

#endif
    void assignStackLocations(const Vector<StackSlot*>& assignedEscapedStackSlots)
    {
        CompilerTimingScope timingScope("Air"_s, "StackAllocator::assign"_s);

        // Now we assign stack locations. At its heart this algorithm is just first-fit. For each
        // StackSlot we just want to find the offsetFromFP that is least negative while ensuring no
        // overlap with other StackSlots that this overlaps with.
        Vector<StackSlot*> otherSlots;
#if USE(BUN_JSC_ADDITIONS)
        constexpr size_t bytesPerWord = 8;
        constexpr size_t fewSlots = 12;
        Vector<unsigned> markOfWord;
        Vector<unsigned> markOfUnit;
        Vector<StackSlot*> slotsInOrder;
        for (StackSlot* slot : m_code.stackSlots())
            slotsInOrder.append(slot);
        if (isX86() && m_code.chargesForCalleeSaves() && slotsInOrder.size() > fewSlots) {
            countHowOftenSlotsAreNamed();
            std::stable_sort(slotsInOrder.begin(), slotsInOrder.end(), [&](StackSlot* a, StackSlot* b) {
                return m_timesNamed[a->index()] > m_timesNamed[b->index()];
            });
        }
        struct WordsOfSlot {
            unsigned first;
            unsigned last;
        };
        constexpr size_t bytesPerUnit = 4;
        Vector<WordsOfSlot> wordsOfSlots;
        Vector<WordsOfSlot> unitsOfSlots;
        Vector<uint64_t> hasOffset;
        Vector<uint64_t> takenByEscapedSlots;
        Vector<uint64_t> unitsTakenByEscapedSlots;
        Vector<uint64_t> takenBySomeSlot;
        Vector<uint64_t> taken;
        struct HarmlessSlotsAtWord {
            unsigned asker { 0 };
            unsigned number { 0 };
        };
        Vector<unsigned> numberOfSlotsAtWord;
        Vector<HarmlessSlotsAtWord> harmlessSlotsAtWord;
        size_t numberOfSlotsWithOffset = 0;
        auto take = [](Vector<uint64_t>& words, WordsOfSlot wordsOfSlot) ALWAYS_INLINE_LAMBDA {
            while (words.size() <= wordsOfSlot.last / 64) [[unlikely]]
                words.append(0);
            for (unsigned word = wordsOfSlot.first; word <= wordsOfSlot.last; ++word)
                words[word / 64] |= static_cast<uint64_t>(1) << (word % 64);
        };
        auto noteOffset = [&](StackSlot* assignedSlot) {
            if constexpr (std::is_same_v<InterferenceGraph, InterferenceRows>) {
                size_t below = -assignedSlot->offsetFromFP();
                wordsOfSlots[assignedSlot->index()] = { static_cast<unsigned>((below - assignedSlot->byteSize()) / bytesPerWord), static_cast<unsigned>((below - 1) / bytesPerWord) };
                unitsOfSlots[assignedSlot->index()] = { static_cast<unsigned>((below - assignedSlot->byteSize()) / bytesPerUnit), static_cast<unsigned>((below - 1) / bytesPerUnit) };
                hasOffset[assignedSlot->index() / 64] |= static_cast<uint64_t>(1) << (assignedSlot->index() % 64);
                ++numberOfSlotsWithOffset;
                WordsOfSlot words = wordsOfSlots[assignedSlot->index()];
                take(takenBySomeSlot, words);
                while (numberOfSlotsAtWord.size() <= words.last) {
                    numberOfSlotsAtWord.append(0);
                    harmlessSlotsAtWord.append(HarmlessSlotsAtWord { });
                }
                for (unsigned word = words.first; word <= words.last; ++word)
                    ++numberOfSlotsAtWord[word];
            }
        };
        if constexpr (std::is_same_v<InterferenceGraph, InterferenceRows>) {
            wordsOfSlots.grow(m_code.stackSlots().size());
            unitsOfSlots.grow(m_code.stackSlots().size());
            hasOffset.fill(0, (m_code.stackSlots().size() + 63) / 64);
            for (StackSlot* slot : m_code.stackSlots()) {
                if (slot->offsetFromFP())
                    noteOffset(slot);
            }
            for (StackSlot* slot : assignedEscapedStackSlots) {
                take(takenByEscapedSlots, wordsOfSlots[slot->index()]);
                take(unitsTakenByEscapedSlots, unitsOfSlots[slot->index()]);
            }
        }
        for (StackSlot* slot : slotsInOrder) {
            if (isRemappedSlotIndex(slot->index()))
                continue;
#else
        for (StackSlot* slot : m_code.stackSlots()) {
            if (isRemappedSlotIndex(slot->index()))
                continue;
#endif

            if (slot->offsetFromFP()) {
                // Already assigned an offset.
                continue;
            }

#if USE(BUN_JSC_ADDITIONS)
            if constexpr (std::is_same_v<InterferenceGraph, InterferenceRows>) {
                if (slot->byteSize() == bytesPerWord && slot->alignment() == bytesPerWord) {
                    taken.shrink(0);
                    constexpr size_t manySlots = 256;
                    if (numberOfSlotsWithOffset >= manySlots && 16 * (numberOfSlotsWithOffset - m_interference.countAmong(slot->index(), hasOffset.span())) < numberOfSlotsWithOffset) {
                        taken.appendVector(takenBySomeSlot);
                        const unsigned asker = slot->index() + 1;
                        m_interference.forEachNotAmongRow(slot->index(), hasOffset.span(), [&](unsigned otherSlotIndex) ALWAYS_INLINE_LAMBDA {
                            WordsOfSlot words = wordsOfSlots[otherSlotIndex];
                            for (unsigned word = words.first; word <= words.last; ++word) {
                                HarmlessSlotsAtWord& harmless = harmlessSlotsAtWord[word];
                                if (harmless.asker != asker)
                                    harmless = { asker, 0 };
                                if (++harmless.number == numberOfSlotsAtWord[word])
                                    taken[word / 64] &= ~(static_cast<uint64_t>(1) << (word % 64));
                            }
                        });
                        for (size_t word = 0; word < takenByEscapedSlots.size(); ++word)
                            taken[word] |= takenByEscapedSlots[word];
                    } else {
                        taken.appendVector(takenByEscapedSlots);
                        m_interference.forEachAmong(slot->index(), hasOffset.span(), [&](unsigned otherSlotIndex) ALWAYS_INLINE_LAMBDA {
                            take(taken, wordsOfSlots[otherSlotIndex]);
                        });
                    }
                    size_t word = 0;
                    while (word < taken.size() && !~taken[word])
                        ++word;
                    size_t end = word * 64 + (word < taken.size() ? std::countr_one(taken[word]) : 0) + 1;
                    slot->setOffsetFromFP(-static_cast<intptr_t>(end * bytesPerWord));
                    noteOffset(slot);
                    continue;
                }
            }
            if (!(slot->byteSize() % bytesPerUnit) && slot->alignment() != bytesPerWord) {
                unsigned mark = slot->index() + 1;
                if constexpr (std::is_same_v<InterferenceGraph, InterferenceRows>) {
                    taken.shrink(0);
                    taken.appendVector(unitsTakenByEscapedSlots);
                    m_interference.forEachAmong(slot->index(), hasOffset.span(), [&](unsigned otherSlotIndex) ALWAYS_INLINE_LAMBDA {
                        take(taken, unitsOfSlots[otherSlotIndex]);
                    });
                } else {
                    auto markUnitsOf = [&](StackSlot* otherSlot) {
                        size_t below = -otherSlot->offsetFromFP();
                        size_t first = (below - otherSlot->byteSize()) / bytesPerUnit;
                        size_t last = (below - 1) / bytesPerUnit;
                        while (markOfUnit.size() <= last)
                            markOfUnit.append(0);
                        for (size_t unit = first; unit <= last; ++unit)
                            markOfUnit[unit] = mark;
                    };
                    for (StackSlot* otherSlot : assignedEscapedStackSlots)
                        markUnitsOf(otherSlot);
                    for (unsigned otherSlotIndex : m_interference[slot->index()]) {
                        if (isRemappedSlotIndex(otherSlotIndex))
                            continue;
                        StackSlot* otherSlot = m_code.stackSlots()[otherSlotIndex];
                        if (otherSlot->offsetFromFP())
                            markUnitsOf(otherSlot);
                    }
                }
                auto isTaken = [&](size_t unit) {
                    if constexpr (std::is_same_v<InterferenceGraph, InterferenceRows>)
                        return unit / 64 < taken.size() && (taken[unit / 64] >> (unit % 64) & 1);
                    else
                        return unit < markOfUnit.size() && markOfUnit[unit] == mark;
                };
                const size_t unitsNeeded = slot->byteSize() / bytesPerUnit;
                const size_t unitsPerAlignment = slot->alignment() / bytesPerUnit;
                size_t end = WTF::roundUpToMultipleOf(unitsPerAlignment, unitsNeeded);
                for (;;) {
                    size_t unit = end;
                    while (unit > end - unitsNeeded && !isTaken(unit - 1))
                        --unit;
                    if (unit == end - unitsNeeded)
                        break;
                    end = WTF::roundUpToMultipleOf(unitsPerAlignment, unit + unitsNeeded);
                }
                slot->setOffsetFromFP(-static_cast<intptr_t>(end * bytesPerUnit));
                noteOffset(slot);
                continue;
            }
            if (!(slot->byteSize() % bytesPerWord) && slot->alignment() == bytesPerWord) {
                unsigned mark = slot->index() + 1;
                auto markWordsOf = [&](StackSlot* otherSlot) {
                    size_t below = -otherSlot->offsetFromFP();
                    size_t first = (below - otherSlot->byteSize()) / bytesPerWord;
                    size_t last = (below - 1) / bytesPerWord;
                    while (markOfWord.size() <= last)
                        markOfWord.append(0);
                    for (size_t word = first; word <= last; ++word)
                        markOfWord[word] = mark;
                };
                for (StackSlot* otherSlot : assignedEscapedStackSlots)
                    markWordsOf(otherSlot);
                for (unsigned otherSlotIndex : m_interference[slot->index()]) {
                    if (isRemappedSlotIndex(otherSlotIndex))
                        continue;
                    StackSlot* otherSlot = m_code.stackSlots()[otherSlotIndex];
                    if (otherSlot->offsetFromFP())
                        markWordsOf(otherSlot);
                }
                size_t wordsNeeded = slot->byteSize() / bytesPerWord;
                size_t end = 0;
                for (size_t wordsFound = 0; wordsFound < wordsNeeded; ++end)
                    wordsFound = end < markOfWord.size() && markOfWord[end] == mark ? 0 : wordsFound + 1;
                slot->setOffsetFromFP(-static_cast<intptr_t>(end * bytesPerWord));
                noteOffset(slot);
                continue;
            }
#endif

            otherSlots.shrink(0);
            otherSlots.appendVector(assignedEscapedStackSlots);
            for (unsigned otherSlotIndex : m_interference[slot->index()]) {
                if (isRemappedSlotIndex(otherSlotIndex))
                    continue;
                StackSlot* otherSlot = m_code.stackSlots()[otherSlotIndex];
                if (otherSlot->offsetFromFP())
                    otherSlots.append(otherSlot);
            }

            std::ranges::sort(otherSlots, std::ranges::greater { }, &StackSlot::offsetFromFP);
            assign(slot, otherSlots);
#if USE(BUN_JSC_ADDITIONS)
            noteOffset(slot);
#endif
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
#if USE(BUN_JSC_ADDITIONS)
    Vector<unsigned> m_timesNamed;
    BitVector m_staysInPlace;
#endif
    AirStackAllocatorStats m_stats;
};

// Avoid computing the liveness information if there is no spill slot to allocate
bool doTrivialStackAllocation(Code& code)
{
    for (StackSlot* slot : code.stackSlots()) {
        if (slot->offsetFromFP())
            continue;
        return false;
    }
    updateFrameSizeBasedOnStackSlots(code);
    return true;
}

} // anonymous namespace

#if USE(BUN_JSC_ADDITIONS)
void allocateStackByGraphColoring(Code& code, RegisterSet&& namedOrClobberedRegisters)
#else
void allocateStackByGraphColoring(Code& code)
#endif
{
    PhaseScope phaseScope(code, "allocateStackByGraphColoring"_s);

#if USE(BUN_JSC_ADDITIONS)
    BitVector wasThereAhead;
    for (StackSlot* slot : code.stackSlots())
        wasThereAhead.set(slot->index());
#endif
    {
        CompilerTimingScope timingScope("Air"_s, "StackAllocator::calleeSaves"_s);
#if USE(BUN_JSC_ADDITIONS)
        handleCalleeSaves(code, WTF::move(namedOrClobberedRegisters));
#else
        handleCalleeSaves(code);
#endif
    }
#if USE(BUN_JSC_ADDITIONS)
    BitVector staysInPlace;
    for (StackSlot* slot : code.stackSlots()) {
        if (slot->offsetFromFP() || !wasThereAhead.get(slot->index()))
            staysInPlace.set(slot->index());
    }
#endif

    Vector<StackSlot*> assignedEscapedStackSlots = [&] {
        CompilerTimingScope timingScope("Air"_s, "StackAllocator::escapedSlots"_s);
        return allocateAndGetEscapedStackSlotsWithoutChangingFrameSize(code);
    }();

    if (!doTrivialStackAllocation(code)) {
#if USE(BUN_JSC_ADDITIONS)
        if (code.stackSlots().size() < InterferenceRows::maxSize) {
            GraphColoringStackAllocator<InterferenceRows> allocator(code);
            allocator.keepInPlace(staysInPlace);
            if (allocator.run(assignedEscapedStackSlots)) {
                code.setStackIsAllocated(true);
                return;
            }
        }
#endif
        if (code.stackSlots().size() < WTF::maxSizeForSmallInterferenceGraph) {
            GraphColoringStackAllocator<SmallIterableInterferenceGraph> allocator(code);
#if USE(BUN_JSC_ADDITIONS)
            allocator.keepInPlace(staysInPlace);
#endif
            allocator.run(assignedEscapedStackSlots);
        } else if (code.stackSlots().size() < std::numeric_limits<uint16_t>::max()) {
            GraphColoringStackAllocator<LargeIterableInterferenceGraph> allocator(code);
#if USE(BUN_JSC_ADDITIONS)
            allocator.keepInPlace(staysInPlace);
#endif
            allocator.run(assignedEscapedStackSlots);
        } else {
            GraphColoringStackAllocator<HugeIterableInterferenceGraph> allocator(code);
#if USE(BUN_JSC_ADDITIONS)
            allocator.keepInPlace(staysInPlace);
#endif
            allocator.run(assignedEscapedStackSlots);
        }
    }

    code.setStackIsAllocated(true);
}

} } } // namespace JSC::B3::Air

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(B3_JIT)
