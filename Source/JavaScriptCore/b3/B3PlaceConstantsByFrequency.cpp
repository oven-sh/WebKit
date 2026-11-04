/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "B3PlaceConstantsByFrequency.h"

#if ENABLE(B3_JIT)

#include "B3DominatorJumpPointers.h"
#include "B3Dominators.h"
#include "B3ImmediateForms.h"
#include "B3Limits.h"
#include "B3Procedure.h"
#include "B3Value.h"
#include "CPU.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

namespace JSC { namespace B3 {

namespace {

static constexpr size_t virtualTreeThreshold = 64;
static constexpr size_t virtualTreeThresholdPerUse = 4;

class ConstantPlacer {
public:
    ConstantPlacer(Procedure& proc, ConstantPlacement& placement)
        : m_proc(proc)
        , m_dominators(proc.dominators())
        , m_placement(placement)
    {
    }

    void run(std::span<const ConstantUse> orderedUses)
    {
        collectUses(orderedUses);
        if (m_groups.isEmpty())
            return;
        computeBlockTables();
        for (Group& group : m_groups)
            place(group);
    }

private:
    struct Use {
        BasicBlock* block;
        Value** child;
    };

    struct Group {
        Vector<Use, 2> uses;
        Vector<Use, 2> immediateUses;
    };

    struct Candidate {
        unsigned epoch { 0 };
        unsigned numberOfMaterializations { 0 };
        double frequency { 0 };
        bool isChosen { false };
        unsigned order { 0 };
        BasicBlock* parent { nullptr };
        BasicBlock* materializationBlock { nullptr };
        Value* value { nullptr };
    };

    enum class CandidateSet : uint8_t { Complete, TooLarge };

    void collectUses(std::span<const ConstantUse> orderedUses)
    {
        IndexMap<BasicBlock*, unsigned> stretch(m_proc.size(), 1u);
        if (isARM64() && m_proc.size() > m_proc.limit(Limit::MaxBlocksPerPlacedConstant)) {
            m_proc.didReachLimit(Limit::MaxBlocksPerPlacedConstant, m_proc.at(0)->at(0)->origin());
            unsigned preOrderIndex = 0;
            for (BasicBlock* block : m_proc.cachedBlocksInPreOrder())
                stretch[block] = 1 + preOrderIndex++ / Limits::blocksPerConstantStretch;
        }
        UncheckedKeyHashMap<std::pair<ValueKey, unsigned>, unsigned> groupIndices;
        for (const ConstantUse& use : orderedUses) {
            Value* child = *use.child;
            if (!isPlacedByFrequency(child))
                continue;
            auto result = groupIndices.add({ child->key(), stretch[use.block] }, m_groups.size());
            if (result.isNewEntry)
                m_groups.append(Group { });
            Group& group = m_groups[result.iterator->value];
            (isSelectedAsImmediate(use.user, child) ? group.immediateUses : group.uses).append(Use { use.block, use.child });
        }
    }

    void computeBlockTables()
    {
        m_preOrderIndex.resize(m_proc.size());
        m_leastFrequentDominator.resize(m_proc.size());
        m_candidateData.resize(m_proc.size());
        unsigned nextPreOrderIndex = 0;
        bool hasNaNFrequency = false;
        for (BasicBlock* block : m_proc.cachedBlocksInPreOrder()) {
            m_preOrderIndex[block] = nextPreOrderIndex++;
            hasNaNFrequency |= std::isnan(block->frequency());
            BasicBlock* parent = m_dominators.idom(block);
            BasicBlock* inherited = parent ? m_leastFrequentDominator[parent] : nullptr;
            m_leastFrequentDominator[block] = inherited && inherited->frequency() < block->frequency() ? inherited : block;
        }
        if (hasNaNFrequency)
            m_leastFrequentDominator.clear(nullptr);
    }

    void place(Group& group)
    {
        const Vector<Use, 2>& uses = group.uses;
        Vector<Value*, 4> placed;
        if (uses.isEmpty()) {
            placeImmediateUses(group, placed);
            return;
        }

        bool isUsedInOneBlock = std::ranges::all_of(uses, [block = uses[0].block](const Use& use) {
            return use.block == block;
        });
        if (BasicBlock* materializationBlock = isUsedInOneBlock ? m_leastFrequentDominator[uses[0].block] : nullptr) {
            Value* value = takeOrClone(*uses[0].child);
            placed.append(value);
            m_placement.valuesByKey.add(value->key(), Vector<Value*, 1> { }).iterator->value.append(value);
            m_placement.add(value, materializationBlock);
            for (const Use& use : uses)
                *use.child = value;
            placeImmediateUses(group, placed);
            return;
        }

        BasicBlock* top = findCommonDominator(uses);
        if (collectCandidatesOnPaths(uses, top) == CandidateSet::TooLarge)
            buildVirtualTree(uses, top);
        chooseBlocks(top);
        assign(group, placed);
        placeImmediateUses(group, placed);
    }

    BasicBlock* findCommonDominator(const Vector<Use, 2>& uses)
    {
        BasicBlock* top = uses[0].block;
        for (const Use& use : uses) {
            if (!m_leastFrequentDominator[use.block])
                return nullptr;
            for (unsigned steps = 0; !m_dominators.dominates(top, use.block); ++steps) {
                if (steps == m_proc.limit(Limit::MaxLinearDominatorSteps)) {
                    m_proc.didReachLimit(Limit::MaxLinearDominatorSteps, (*use.child)->origin());
                    top = jumpPointers().nearestCommonDominator(top, use.block);
                    break;
                }
                top = m_dominators.idom(top);
            }
        }
        return top;
    }

    CandidateSet collectCandidatesOnPaths(const Vector<Use, 2>& uses, BasicBlock* top)
    {
        ++m_epoch;
        m_candidates.shrink(0);
        const size_t maxCandidates = top ? virtualTreeThreshold + virtualTreeThresholdPerUse * uses.size() : std::numeric_limits<size_t>::max();
        for (const Use& use : uses) {
            for (BasicBlock* block = use.block; block && m_candidateData[block].epoch != m_epoch && m_candidates.size() <= maxCandidates; block = m_dominators.idom(block)) {
                addCandidate(block, block == top ? nullptr : m_dominators.idom(block), m_preOrderIndex[block]);
                if (block == top)
                    break;
            }
            m_candidateData[use.block].isChosen = true;
        }
        return m_candidates.size() > maxCandidates ? CandidateSet::TooLarge : CandidateSet::Complete;
    }

    void buildVirtualTree(const Vector<Use, 2>& uses, BasicBlock* top)
    {
        DominatorJumpPointers& pointers = jumpPointers();
        m_candidates.shrink(0);
        auto precedesInDominatorTree = [&pointers](BasicBlock* a, BasicBlock* b) {
            return pointers.dominatorTreePreOrder(a) < pointers.dominatorTreePreOrder(b);
        };
        Vector<BasicBlock*, 16> nodes;
        for (const Use& use : uses)
            nodes.append(use.block);
        std::ranges::sort(nodes, precedesInDominatorTree);
        nodes.shrink(nodes.size() - std::ranges::unique(nodes).size());
        for (size_t i = 1, count = nodes.size(); i < count; ++i)
            nodes.append(pointers.nearestCommonDominator(nodes[i - 1], nodes[i]));
        std::ranges::sort(nodes, precedesInDominatorTree);
        nodes.shrink(nodes.size() - std::ranges::unique(nodes).size());
        RELEASE_ASSERT(nodes[0] == top);
        Vector<BasicBlock*, 16> path;
        for (BasicBlock* node : nodes) {
            while (!path.isEmpty() && !m_dominators.dominates(path.last(), node))
                path.removeLast();
            BasicBlock* parent = path.isEmpty() ? nullptr : path.last();
            path.append(node);
            if (parent && m_dominators.idom(node) != parent) {
                BasicBlock* least = pointers.leastFrequentBetween(m_dominators.idom(node), parent);
                addCandidate(least, parent, m_preOrderIndex[pointers.dominatorAtDepth(node, pointers.depth(parent) + 1)]);
                parent = least;
            }
            addCandidate(node, parent, m_preOrderIndex[node]);
        }
        for (const Use& use : uses)
            m_candidateData[use.block].isChosen = true;
    }

    void chooseBlocks(BasicBlock* top)
    {
        std::ranges::sort(m_candidates, [this](BasicBlock* a, BasicBlock* b) {
            return m_candidateData[a].order > m_candidateData[b].order;
        });
        for (BasicBlock* block : m_candidates) {
            Candidate& candidate = m_candidateData[block];
            if (candidate.isChosen || shouldMaterializeAt(block, candidate)) {
                candidate.isChosen = true;
                candidate.frequency = block->frequency();
                candidate.numberOfMaterializations = 1;
            }
            if (BasicBlock* parent = candidate.parent) {
                m_candidateData[parent].frequency += candidate.frequency;
                m_candidateData[parent].numberOfMaterializations += candidate.numberOfMaterializations;
            }
        }
        BasicBlock* blockAboveTop = nullptr;
        if (BasicBlock* parent = top ? m_dominators.idom(top) : nullptr) {
            BasicBlock* least = m_leastFrequentDominator[parent];
            if (shouldMaterializeAt(least, m_candidateData[top])) {
                blockAboveTop = least;
                m_candidateData[blockAboveTop] = Candidate { };
                m_candidateData[blockAboveTop].materializationBlock = blockAboveTop;
            }
        }
        for (unsigned i = m_candidates.size(); i--;) {
            BasicBlock* block = m_candidates[i];
            BasicBlock* parent = m_candidateData[block].parent;
            BasicBlock* inherited = block == top ? blockAboveTop : parent ? m_candidateData[parent].materializationBlock : nullptr;
            if (inherited)
                m_candidateData[block].materializationBlock = inherited;
            else if (m_candidateData[block].isChosen)
                m_candidateData[block].materializationBlock = block;
        }
    }

    void assign(Group& group, Vector<Value*, 4>& placed)
    {
        Value* original = *group.uses[0].child;
        auto& values = m_placement.valuesByKey.add(original->key(), Vector<Value*, 1> { }).iterator->value;
        for (Use& use : group.uses) {
            BasicBlock* materializationBlock = m_candidateData[use.block].materializationBlock;
            ASSERT(m_dominators.dominates(materializationBlock, use.block));
            Value*& value = m_candidateData[materializationBlock].value;
            if (!value) {
                value = takeOrClone(original);
                placed.append(value);
                values.append(value);
                m_placement.add(value, materializationBlock);
            }
            *use.child = value;
        }
    }

    void placeImmediateUses(Group& group, const Vector<Value*, 4>& placed)
    {
        BasicBlock* root = m_proc.at(0);
        BasicBlock* top = nullptr;
        Vector<Use*, 4> remainingUses;
        for (Use& use : group.immediateUses) {
            if (Value* value = findDominatingValue(placed, use.block)) {
                *use.child = value;
                continue;
            }
            remainingUses.append(&use);
            if (!top)
                top = use.block;
            for (unsigned steps = 0; top != root && !m_dominators.dominates(top, use.block); ++steps) {
                if (steps == m_proc.limit(Limit::MaxLinearDominatorSteps) && m_dominators.dominates(root, top) && m_dominators.dominates(root, use.block)) {
                    m_proc.didReachLimit(Limit::MaxLinearDominatorSteps, (*use.child)->origin());
                    top = jumpPointers().nearestCommonDominator(top, use.block);
                    break;
                }
                top = m_dominators.idom(top) ? m_dominators.idom(top) : root;
            }
        }
        if (remainingUses.isEmpty())
            return;
        Value* value = takeOrClone(*remainingUses[0]->child);
        m_placement.add(value, top);
        for (Use* use : remainingUses)
            *use->child = value;
    }

    Value* findDominatingValue(const Vector<Value*, 4>& placed, BasicBlock* block) const
    {
        for (Value* value : placed) {
            if (m_dominators.dominates(m_placement.blockOf.get(value), block))
                return value;
        }
        return nullptr;
    }

    static bool shouldMaterializeAt(BasicBlock* block, const Candidate& below)
    {
        return below.frequency > block->frequency() || (below.frequency == block->frequency() && below.numberOfMaterializations > 1);
    }

    void addCandidate(BasicBlock* block, BasicBlock* parent, unsigned order)
    {
        Candidate& candidate = m_candidateData[block];
        candidate = Candidate { };
        candidate.epoch = m_epoch;
        candidate.parent = parent;
        candidate.order = order;
        m_candidates.append(block);
    }

    Value* takeOrClone(Value* original)
    {
        return m_placement.blockOf.contains(original) ? m_proc.clone(original) : original;
    }

    DominatorJumpPointers& jumpPointers()
    {
        if (!m_jumpPointers)
            m_jumpPointers.emplace(m_proc);
        return *m_jumpPointers;
    }

    Procedure& m_proc;
    Dominators& m_dominators;
    ConstantPlacement& m_placement;
    Vector<Group> m_groups;
    IndexMap<BasicBlock*, unsigned> m_preOrderIndex;
    IndexMap<BasicBlock*, BasicBlock*> m_leastFrequentDominator;
    IndexMap<BasicBlock*, Candidate> m_candidateData;
    Vector<BasicBlock*> m_candidates;
    unsigned m_epoch { 0 };
    std::optional<DominatorJumpPointers> m_jumpPointers;
};

} // anonymous namespace

ConstantPlacement::ConstantPlacement(Procedure& proc)
    : materializations(proc.size())
{
}

void ConstantPlacement::add(Value* value, BasicBlock* block)
{
    value->owner = block;
    materializations[block].append(value);
    blockOf.add(value, block);
}

void placeConstantsByFrequency(Procedure& proc, std::span<const ConstantUse> orderedUses, ConstantPlacement& placement)
{
    ConstantPlacer placer(proc, placement);
    placer.run(orderedUses);
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
