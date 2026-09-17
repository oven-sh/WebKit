/*
 * Copyright (C) 2026 the WebKit project authors.
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
#include "DFGPollVisibilityPhase.h"

#if ENABLE(DFG_JIT)

#include "DFGBasicBlockInlines.h"
#include "DFGClobberize.h"
#include "DFGGraph.h"
#include "DFGNaturalLoops.h"
#include "DFGPhase.h"
#include "JSCInlines.h"
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>

namespace JSC { namespace DFG {

namespace {

class PollVisibilityPhase : public Phase {
    static constexpr bool verbose = false;

public:
    PollVisibilityPhase(Graph& graph)
        : Phase(graph, "poll visibility analysis"_s)
    {
    }

    bool run()
    {
        if (!Options::useJSThreads() || Options::useThreadGIL())
            return false;
        if (m_graph.m_form != SSA || !m_graph.m_plan.isFTL())
            return false;

        // Whatever an earlier run attached describes a graph that has changed since.
        forEachPoll([&](BasicBlock*, Node* poll) {
            poll->setPollVisibilityData(nullptr);
        });
        if (!Options::useJSThreadsPollVisibilityAnalysis())
            return false;

        m_graph.ensureSSADominators();
        m_graph.ensureSSANaturalLoops();

        m_blockOf.clear();
        m_upsilonsOf.clear();
        for (BasicBlock* block : m_graph.blocksInNaturalOrder()) {
            for (Node* node : *block) {
                m_blockOf.add(node, block);
                if (node->op() == Upsilon)
                    m_upsilonsOf.add(node->phi(), Vector<Node*>()).iterator->value.append(node);
            }
        }

        unsigned numLoops = m_graph.m_ssaNaturalLoops->numLoops();
        m_computed.fill(false, numLoops);
        m_data.fill(nullptr, numLoops);

        forEachPoll([&](BasicBlock* block, Node* poll) {
            if (!poll->origin.exitOK)
                return; // The conservative clobber in clobberize() covers it.
            const SSANaturalLoop* loop = m_graph.m_ssaNaturalLoops->innerMostLoopOf(block);
            if (!loop)
                return;
            poll->setPollVisibilityData(dataFor(*loop));
        });
        return false; // Annotations only; the graph's shape is untouched.
    }

private:
    template<typename Functor>
    void forEachPoll(const Functor& functor)
    {
        for (BasicBlock* block : m_graph.blocksInNaturalOrder()) {
            for (Node* node : *block) {
                if (node->op() == CheckTraps)
                    functor(block, node);
            }
        }
    }

    // The heaps the interim rule makes a poll write (DFGClobberize.h); the analysis never asks a poll to write
    // anything outside them.
    static bool isPollBoundedValueHeap(AbstractHeap heap)
    {
        static const AbstractHeapKind kinds[] = {
            NamedProperties, IndexedProperties, Butterfly_publicLength, MiscFields, Absolute, JSMapFields, JSSetFields,
            JSWeakMapFields, JSWeakSetFields, JSInternalFields, JSDateFields, RegExpObject_lastIndex
        };
        for (AbstractHeapKind kind : kinds) {
            if (heap.overlaps(AbstractHeap(kind)))
                return true;
        }
        return false;
    }

    static bool isWholeHeap(AbstractHeap heap)
    {
        return heap.kind() == World || heap.kind() == Heap;
    }

    // What a slice is followed through: the value heaps, and the stack (a value parked in a stack slot by one node
    // and read back by another is still the same value).
    static bool isSlicedHeap(AbstractHeap heap)
    {
        return heap.kind() == Stack || isPollBoundedValueHeap(heap);
    }

    PollVisibilityData* dataFor(const SSANaturalLoop& loop)
    {
        unsigned index = loop.index();
        if (m_computed[index])
            return m_data[index];
        m_computed[index] = true;
        m_data[index] = compute(loop);
        return m_data[index];
    }

    // Null means "write the interim set".
    PollVisibilityData* compute(const SSANaturalLoop& loop)
    {
        UncheckedKeyHashSet<BasicBlock*> blocks;
        for (unsigned i = loop.size(); i--;)
            blocks.add(loop.at(i));
        auto inLoop = [&](Node* node) {
            auto iterator = m_blockOf.find(node);
            return iterator != m_blockOf.end() && blocks.contains(iterator->value);
        };

        // A node that writes the whole heap (a call) makes every heap read in the loop unhoistable anyway; leave the
        // interim set rather than reason about what such a node may do.
        Vector<Node*> stores;
        for (BasicBlock* block : blocks) {
            for (Node* node : *block) {
                if (node->op() == CheckTraps)
                    continue;
                bool writesWholeHeap = false;
                bool writesSlicedHeap = false;
                clobberize(m_graph, node, NoOpClobberize(), [&](AbstractHeap heap) {
                    if (isWholeHeap(heap))
                        writesWholeHeap = true;
                    else if (isSlicedHeap(heap))
                        writesSlicedHeap = true;
                }, NoOpClobberize());
                if (writesWholeHeap)
                    return nullptr;
                if (writesSlicedHeap)
                    stores.append(node);
            }
        }

        Vector<Node*> worklist;
        UncheckedKeyHashSet<Node*> seen;
        auto push = [&](Node* node) {
            if (node && inLoop(node) && seen.add(node).isNewEntry)
                worklist.append(node);
        };

        // A loop that no Branch or Switch leaves ends, if at all, by an exception or an exit from a check; which reads
        // those depend on is not something this analysis tracks, so such a loop keeps the interim set.
        bool hasExitBranch = false;
        for (BasicBlock* block : blocks) {
            Node* terminal = block->terminal();
            switch (terminal->op()) {
            case Branch:
            case Switch:
                push(terminal->child1().node());
                for (BasicBlock* successor : block->successors()) {
                    if (!blocks.contains(successor))
                        hasExitBranch = true;
                }
                break;
            default:
                break;
            }
        }
        if (!hasExitBranch)
            return nullptr;

        Vector<AbstractHeap, 8> heaps;
        bool gaveUp = false;
        auto addHeap = [&](AbstractHeap heap) {
            if (isWholeHeap(heap)) {
                gaveUp = true;
                return;
            }
            if (!isSlicedHeap(heap))
                return;
            for (AbstractHeap existing : heaps) {
                if (heap.isSubtypeOf(existing))
                    return;
            }
            heaps.removeAllMatching([&](AbstractHeap existing) {
                return existing.isStrictSubtypeOf(heap);
            });
            heaps.append(heap);
        };

        UncheckedKeyHashSet<Node*> slicedStores;
        while (!gaveUp) {
            while (!worklist.isEmpty() && !gaveUp) {
                Node* node = worklist.takeLast();
                if (node->op() == Phi) {
                    auto iterator = m_upsilonsOf.find(node);
                    if (iterator != m_upsilonsOf.end()) {
                        for (Node* upsilon : iterator->value)
                            push(upsilon->child1().node());
                    }
                    continue;
                }
                m_graph.doToChildren(node, [&](Edge edge) {
                    push(edge.node());
                });
                clobberize(m_graph, node, addHeap, NoOpClobberize(), NoOpClobberize());
            }
            if (gaveUp)
                break;
            // Memory dependence: a store in the loop to a heap a slice reads brings its operands into the slice.
            bool changed = false;
            for (Node* store : stores) {
                if (slicedStores.contains(store))
                    continue;
                bool feedsSlice = false;
                clobberize(m_graph, store, NoOpClobberize(), [&](AbstractHeap written) {
                    for (AbstractHeap read : heaps) {
                        if (written.overlaps(read))
                            feedsSlice = true;
                    }
                }, NoOpClobberize());
                if (!feedsSlice)
                    continue;
                slicedStores.add(store);
                changed = true;
                m_graph.doToChildren(store, [&](Edge edge) {
                    push(edge.node());
                });
            }
            if (!changed)
                break;
        }
        if (gaveUp)
            return nullptr;
        // The stack is this thread's own; a poll has nothing to make visible there.
        heaps.removeAllMatching([](AbstractHeap heap) {
            return heap.kind() == Stack;
        });
        if (heaps.size() > 16)
            return nullptr;

        PollVisibilityData* data = m_graph.m_pollVisibilityData.add();
        data->heaps = WTF::move(heaps);
        dataLogLnIf(verbose, "Poll visibility for loop ", loop.index(), " (header ", *loop.header(), "): ", listDump(data->heaps));
        return data;
    }

    UncheckedKeyHashMap<Node*, BasicBlock*> m_blockOf;
    UncheckedKeyHashMap<Node*, Vector<Node*>> m_upsilonsOf;
    Vector<bool> m_computed;
    Vector<PollVisibilityData*> m_data;
};

} // anonymous namespace

bool performPollVisibilityAnalysis(Graph& graph)
{
    return runPhase<PollVisibilityPhase>(graph);
}

} } // namespace JSC::DFG

#endif // ENABLE(DFG_JIT)
