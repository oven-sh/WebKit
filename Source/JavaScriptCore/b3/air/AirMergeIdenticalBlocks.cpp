/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AirMergeIdenticalBlocks.h"

#if ENABLE(B3_JIT)

#include "AirCode.h"
#include "AirFrameStates.h"
#include "AirInstInlines.h"
#include "AirOptimizeBlockOrder.h"
#include "AirPhaseScope.h"
#include <algorithm>
#include <wtf/HashMap.h>

namespace JSC { namespace B3 { namespace Air {

namespace {

enum class FallThroughBlocks : uint8_t { Skip, ReplaceByInvertingBranch };

class IdenticalBlockMerger {
public:
    explicit IdenticalBlockMerger(Code& code)
        : m_code(code)
    {
    }

    bool run()
    {
        bool changed = false;
        for (;;) {
            collectCandidates();
            if (!chooseReplacements(FallThroughBlocks::Skip) && !chooseReplacements(FallThroughBlocks::ReplaceByInvertingBranch))
                return changed;
            applyReplacements();
            changed = true;
        }
    }

private:
    // The first two blocks with the same instructions are found before any block is replaced, so these are the states of the code as it came in.
    bool haveSameFrameStateAtHead(BasicBlock* a, BasicBlock* b)
    {
        if (!m_code.policy().frame.emitsFrame)
            return true;
        if (!m_frameStates)
            m_frameStates.emplace(m_code);
        return m_frameStates->atHead(a) == m_frameStates->atHead(b);
    }

    bool areIdentical(BasicBlock* a, BasicBlock* b)
    {
        if (a->size() != b->size() || a->numSuccessors() != b->numSuccessors() || (a->numSuccessors() && a->successorBlock(0) != b->successorBlock(0)))
            return false;
        for (unsigned i = 0; i < a->size(); ++i) {
            if (a->at(i).kind != b->at(i).kind || !std::ranges::equal(a->at(i).args(), b->at(i).args()))
                return false;
        }
        return haveSameFrameStateAtHead(a, b);
    }

    void collectCandidates()
    {
        m_candidates = { };
        m_fallThroughPredecessors.fill(nullptr, m_code.size());
        BasicBlock* previous = nullptr;
        for (BasicBlock* block : m_code) {
            if (previous) {
                for (BasicBlock* successor : previous->successorBlocks()) {
                    if (successor == block)
                        m_fallThroughPredecessors[block->index()] = previous;
                }
            }
            previous = block;
            if (m_code.isEntrypoint(block) || block->numSuccessors() > 1 || (block->numSuccessors() && block->last().kind.opcode != Jump))
                continue;
            unsigned hash = block->size() + (block->numSuccessors() ? block->successorBlock(0)->index() * 131 : 0);
            bool dependsOnOrigin = false;
            for (Inst& inst : *block) {
                if (inst.kind.opcode == Patch || inst.kind.opcode == WasmBoundsCheck || inst.kind.effects) {
                    dependsOnOrigin = true;
                    break;
                }
                hash = hash * 33 + inst.kind.opcode;
                for (Arg& arg : inst.args())
                    hash = hash * 33 + arg.hash();
            }
            if (!dependsOnOrigin && m_candidates.isValidKey(hash))
                m_candidates.add(hash, Vector<BasicBlock*, 2>()).iterator->value.append(block);
        }
    }

    bool chooseReplacements(FallThroughBlocks mode)
    {
        m_replacements.clear();
        m_branchesToInvert.clear();
        for (auto& entry : m_candidates) {
            if (entry.value.size() < 2)
                continue;
            auto blocks = entry.value;
            for (unsigned i = 0; i < blocks.size(); ++i) {
                if (!blocks[i])
                    continue;
                Vector<BasicBlock*, 4> identicalBlocks { blocks[i] };
                for (unsigned k = i + 1; k < blocks.size(); ++k) {
                    if (blocks[k] && areIdentical(blocks[i], blocks[k])) {
                        identicalBlocks.append(blocks[k]);
                        blocks[k] = nullptr;
                    }
                }
                chooseReplacementsAmong(identicalBlocks, mode);
            }
        }
        return !m_replacements.isEmpty();
    }

    void chooseReplacementsAmong(const Vector<BasicBlock*, 4>& identicalBlocks, FallThroughBlocks mode)
    {
        BasicBlock* canonicalBlock = identicalBlocks[0];
        for (BasicBlock* block : identicalBlocks) {
            if (block->numSuccessors() && block->successorBlock(0) == m_code.findNextBlock(block))
                canonicalBlock = block;
        }
        for (BasicBlock* block : identicalBlocks) {
            if (block == canonicalBlock)
                continue;
            BasicBlock* source = m_fallThroughPredecessors[block->index()];
            if (!source) {
                if (mode == FallThroughBlocks::Skip)
                    m_replacements.add(block, canonicalBlock);
                continue;
            }
            if (mode == FallThroughBlocks::Skip || source->numSuccessors() != 2 || source->successorBlock(1) != block || source->successorBlock(0) == block)
                continue;
            if (source->successorBlock(0) != m_code.findNextBlock(block) || !canInvertBranch(source->last()))
                continue;
            m_replacements.add(block, canonicalBlock);
            m_branchesToInvert.append(source);
        }
    }

    void applyReplacements()
    {
        for (BasicBlock* block : m_code) {
            for (BasicBlock*& successor : block->successorBlocks()) {
                if (auto found = m_replacements.find(successor); found != m_replacements.end())
                    successor = found->value;
            }
        }
        for (BasicBlock* source : m_branchesToInvert)
            invertBranch(source);
        m_code.resetReachability();
    }

    Code& m_code;
    std::optional<FrameStates> m_frameStates;
    UncheckedKeyHashMap<unsigned, Vector<BasicBlock*, 2>, IntHash<unsigned>, WTF::UnsignedWithZeroKeyHashTraits<unsigned>> m_candidates;
    Vector<BasicBlock*> m_fallThroughPredecessors;
    UncheckedKeyHashMap<BasicBlock*, BasicBlock*> m_replacements;
    Vector<BasicBlock*, 4> m_branchesToInvert;
};

} // anonymous namespace

bool mergeIdenticalBlocks(Code& code)
{
    PhaseScope phaseScope(code, "mergeIdenticalBlocks"_s);
    return IdenticalBlockMerger(code).run();
}

} } } // namespace JSC::B3::Air

#endif // ENABLE(B3_JIT)
