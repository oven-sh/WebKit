/*
 * Copyright (C) 2015-2016 Apple Inc. All rights reserved.
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

#pragma once

#if ENABLE(B3_JIT)

#include <wtf/GraphNodeWorklist.h>
#include <wtf/HashMap.h>
#include <wtf/IndexSet.h>
#include <wtf/Vector.h>

namespace JSC { namespace B3 {

template<typename BasicBlock>
bool addPredecessor(BasicBlock* block, BasicBlock* predecessor)
{
    auto& predecessors = block->predecessors();

    if (predecessors.contains(predecessor))
        return false;

    predecessors.append(predecessor);
    return true;
}

template<typename BasicBlock>
bool removePredecessor(BasicBlock* block, BasicBlock* predecessor)
{
    auto& predecessors = block->predecessors();
    for (unsigned i = 0; i < predecessors.size(); ++i) {
        if (predecessors[i] == predecessor) {
            predecessors[i--] = predecessors.last();
            predecessors.removeLast();
            ASSERT(!predecessors.contains(predecessor));
            return true;
        }
    }
    return false;
}

template<typename BasicBlock>
bool replacePredecessor(BasicBlock* block, BasicBlock* from, BasicBlock* to)
{
    bool changed = false;
    // We do it this way because 'to' may already be a predecessor of 'block'.
    changed |= removePredecessor(block, from);
    changed |= addPredecessor(block, to);
    return changed;
}

#if USE(BUN_JSC_ADDITIONS)
template<typename BasicBlock>
class LargePredecessorLists {
public:
    bool add(BasicBlock* block, BasicBlock* predecessor)
    {
        Places* places = placesIn(block);
        if (!places)
            return addPredecessor(block, predecessor);
        auto& predecessors = block->predecessors();
        if (!places->add(predecessor, predecessors.size()).isNewEntry)
            return false;
        predecessors.append(predecessor);
        return true;
    }

    bool remove(BasicBlock* block, BasicBlock* predecessor)
    {
        Places* places = placesIn(block);
        if (!places)
            return removePredecessor(block, predecessor);
        auto found = places->find(predecessor);
        if (found == places->end())
            return false;
        unsigned place = found->value;
        places->remove(found);
        auto& predecessors = block->predecessors();
        BasicBlock* last = predecessors.takeLast();
        if (place < predecessors.size()) {
            predecessors[place] = last;
            places->set(last, place);
        }
        return true;
    }

    bool replace(BasicBlock* block, BasicBlock* from, BasicBlock* to)
    {
        bool changed = remove(block, from);
        changed |= add(block, to);
        return changed;
    }

private:
    static constexpr unsigned fewPredecessors = 64;
    using Places = UncheckedKeyHashMap<BasicBlock*, unsigned>;
    struct List {
        Places places;
        bool holdsABlockTwice { false };
    };

    Places* placesIn(BasicBlock* block)
    {
        bool hasFew = block->numPredecessors() < fewPredecessors;
        if (hasFew && m_lists.isEmpty())
            return nullptr;
        auto found = m_lists.find(block);
        if (found == m_lists.end()) {
            if (hasFew)
                return nullptr;
            found = m_lists.add(block, List { }).iterator;
            auto& predecessors = block->predecessors();
            for (unsigned i = 0; i < predecessors.size(); ++i)
                found->value.holdsABlockTwice |= !found->value.places.add(predecessors[i], i).isNewEntry;
        }
        return found->value.holdsABlockTwice ? nullptr : &found->value.places;
    }

    UncheckedKeyHashMap<BasicBlock*, List> m_lists;
};
#endif

template<typename BasicBlock>
void updatePredecessorsAfter(BasicBlock* root)
{
    Vector<BasicBlock*, 16> worklist;
    worklist.append(root);
    while (!worklist.isEmpty()) {
        BasicBlock* block = worklist.takeLast();
        for (BasicBlock* successor : block->successorBlocks()) {
            if (addPredecessor(successor, block))
                worklist.append(successor);
        }
    }
}

template<typename BasicBlock>
void clearPredecessors(Vector<std::unique_ptr<BasicBlock>>& blocks)
{
    for (auto& block : blocks) {
        if (block)
            block->predecessors().shrink(0);
    }
}

template<typename BasicBlock>
void recomputePredecessors(Vector<std::unique_ptr<BasicBlock>>& blocks)
{
    clearPredecessors(blocks);
#if USE(BUN_JSC_ADDITIONS)
    Vector<uint8_t, 256> isDone(FillWith { }, blocks.size(), static_cast<uint8_t>(0));
    Vector<BasicBlock*, 16> worklist;
    worklist.append(blocks[0].get());
    while (!worklist.isEmpty()) {
        BasicBlock* block = worklist.takeLast();
        if (std::exchange(isDone[block->index()], 1))
            continue;
        for (BasicBlock* successor : block->successorBlocks()) {
            auto& predecessors = successor->predecessors();
            if (!predecessors.isEmpty() && predecessors.last() == block)
                continue;
            predecessors.append(block);
            if (!isDone[successor->index()])
                worklist.append(successor);
        }
    }
#else
    updatePredecessorsAfter(blocks[0].get());
#endif
}

#if USE(BUN_JSC_ADDITIONS)
template<typename BasicBlock, typename Functor>
void recomputePredecessorsFromRoots(Vector<std::unique_ptr<BasicBlock>>& blocks, const Functor& forEachRoot)
{
    clearPredecessors(blocks);
    Vector<uint8_t, 256> isDone(FillWith { }, blocks.size(), static_cast<uint8_t>(0));
    Vector<BasicBlock*, 16> worklist;
    forEachRoot([&](BasicBlock* root) {
        worklist.append(root);
        while (!worklist.isEmpty()) {
            BasicBlock* block = worklist.takeLast();
            if (std::exchange(isDone[block->index()], 1))
                continue;
            for (BasicBlock* successor : block->successorBlocks()) {
                auto& predecessors = successor->predecessors();
                if (!predecessors.isEmpty() && predecessors.last() == block)
                    continue;
                predecessors.append(block);
                if (!isDone[successor->index()])
                    worklist.append(successor);
            }
        }
    });
}
#endif

template<typename BasicBlock>
bool isBlockDead(BasicBlock* block)
{
    if (!block)
        return false;
    if (!block->index())
        return false;
    return block->predecessors().isEmpty();
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
