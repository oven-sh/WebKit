/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include "Helpers/Test.h"
#include "TestControlFlowGraph.h"
#include <algorithm>
#include <initializer_list>
#include <iterator>
#include <set>
#include <type_traits>
#include <vector>
#include <wtf/Liveness.h>
#include <wtf/Vector.h>
#include <wtf/WeakRandom.h>

namespace TestWebKitAPI {

namespace {

struct Actions {
    Vector<unsigned, 2> uses;
    Vector<unsigned, 2> defs;
};

using Program = Vector<Vector<Actions>>;
using LiveSet = std::set<unsigned>;
using LiveList = std::vector<unsigned>;

template<bool streams>
struct TestLivenessAdapter {
    using CFG = TestControlFlowGraph;
    using Thing = unsigned;
    using ActionGroup = const Actions*;
    static constexpr bool streamsActions = streams;

    TestLivenessAdapter(const Program& program, unsigned numIndices)
        : m_program(program)
        , m_numIndices(numIndices)
    {
    }

    void prepareToCompute() { }
    unsigned numIndices() const { return m_numIndices; }
    static unsigned valueToIndex(unsigned thing) { return thing; }
    static unsigned indexToValue(unsigned index) { return index; }
    unsigned blockSize(TestBasicBlock* block) const { return static_cast<unsigned>(boundaries(block).size()) - 1; }

    template<typename Func>
    void forEachActionGroupDescending(TestBasicBlock* block, NOESCAPE const Func& func) const
    {
        for (unsigned boundary = blockSize(block) + 1; boundary--;)
            func(boundary, &boundaries(block)[boundary]);
    }

    template<typename Func>
    void forEachActionGroupDescendingStreaming(TestBasicBlock* block, NOESCAPE const Func& func) const
    {
        forEachActionGroupDescending(block, func);
    }

    template<typename Func>
    static void forEachUseInGroup(ActionGroup group, NOESCAPE const Func& func)
    {
        for (unsigned index : group->uses)
            func(index);
    }

    template<typename Func>
    static void forEachDefInGroup(ActionGroup group, NOESCAPE const Func& func)
    {
        for (unsigned index : group->defs)
            func(index);
    }

    template<typename Func>
    void forEachUseAtTail(TestBasicBlock* block, NOESCAPE const Func& func) const
    {
        forEachUseInGroup(&boundaries(block).last(), func);
    }

    template<typename Func>
    void forEachUse(TestBasicBlock* block, unsigned boundary, NOESCAPE const Func& func) const
    {
        forEachUseInGroup(&boundaries(block)[boundary], func);
    }

    template<typename Func>
    void forEachDef(TestBasicBlock* block, unsigned boundary, NOESCAPE const Func& func) const
    {
        forEachDefInGroup(&boundaries(block)[boundary], func);
    }

private:
    const Vector<Actions>& boundaries(TestBasicBlock* block) const { return m_program[block->index()]; }

    const Program& m_program;
    unsigned m_numIndices;
};

template<bool streams>
class TestLiveness : public WTF::Liveness<TestLivenessAdapter<streams>> {
    using Base = WTF::Liveness<TestLivenessAdapter<streams>>;
public:
    enum class Form : uint8_t { Dense, Sparse };

    TestLiveness(TestControlFlowGraph& graph, const Program& program, unsigned numIndices, Form form)
        : Base(graph, program, numIndices)
    {
        switch (form) {
        case Form::Dense:
            this->computeDense();
            return;
        case Form::Sparse:
            this->computeSparse();
            return;
        }
    }
};

constexpr unsigned wholeWordsSize = 448;
constexpr unsigned partialWordSize = wholeWordsSize + 1;
constexpr unsigned randomIndexLimit = 440;
static_assert(!(wholeWordsSize % 64) && randomIndexLimit <= wholeWordsSize);

struct Model {
    Vector<LiveSet> heads;
    Vector<LiveSet> tails;
};

struct RandomInput {
    explicit RandomInput(unsigned seed)
        : weakRandom(seed)
        , graph(1 + weakRandom.getUint32(30))
    {
        unsigned numberOfBlocks = graph.numNodes();
        if (numberOfBlocks >= 2)
            graph.addRandomEdges(weakRandom, weakRandom.getUint32(2 * numberOfBlocks));

        unsigned indexLimit = 1 + weakRandom.getUint32(randomIndexLimit);
        auto appendRandomIndices = [&](Vector<unsigned, 2>& indices) {
            for (unsigned count = weakRandom.getUint32(3); count--;)
                indices.append(weakRandom.getUint32(indexLimit));
        };
        program.grow(numberOfBlocks);
        for (Vector<Actions>& boundaries : program) {
            boundaries.grow(1 + weakRandom.getUint32(7));
            for (Actions& actions : boundaries) {
                appendRandomIndices(actions.uses);
                appendRandomIndices(actions.defs);
            }
        }
    }

    WeakRandom weakRandom;
    TestControlFlowGraph graph;
    Program program;
};

} // anonymous namespace

static Program createEmptyProgram(std::initializer_list<unsigned> blockSizes)
{
    Program program;
    for (unsigned blockSize : blockSizes)
        program.append(Vector<Actions>(blockSize + 1));
    return program;
}

static Model computeModel(TestControlFlowGraph& graph, const Program& program)
{
    Model model;
    model.heads.grow(graph.numNodes());
    model.tails.grow(graph.numNodes());
    for (bool changed = true; changed;) {
        changed = false;
        for (unsigned blockIndex = 0; blockIndex < graph.numNodes(); ++blockIndex) {
            TestBasicBlock* block = graph.node(blockIndex);
            if (!block)
                continue;

            const Vector<Actions>& boundaries = program[blockIndex];
            LiveSet tail(boundaries.last().uses.begin(), boundaries.last().uses.end());
            for (TestBasicBlock* successor : graph.successors(block)) {
                const LiveSet& head = model.heads[successor->index()];
                tail.insert(head.begin(), head.end());
            }

            LiveSet live = tail;
            for (size_t boundary = boundaries.size(); boundary--;) {
                live.insert(boundaries[boundary].uses.begin(), boundaries[boundary].uses.end());
                for (unsigned index : boundaries[boundary].defs)
                    live.erase(index);
            }

            if (tail != model.tails[blockIndex] || live != model.heads[blockIndex])
                changed = true;
            model.tails[blockIndex] = WTF::move(tail);
            model.heads[blockIndex] = WTF::move(live);
        }
    }
    return model;
}

static LiveList toList(const LiveSet& set)
{
    return LiveList(set.begin(), set.end());
}

static LiveList difference(const LiveSet& a, const LiveSet& b)
{
    LiveList result;
    std::ranges::set_difference(a, b, std::back_inserter(result));
    return result;
}

template<typename Iterable>
static LiveList collect(const Iterable& iterable)
{
    LiveList result;
    for (unsigned index : iterable)
        result.push_back(index);
    return result;
}

template<typename ForEach>
static LiveList collectFrom(const ForEach& forEach)
{
    LiveList result;
    forEach([&](unsigned index) {
        result.push_back(index);
    });
    return result;
}

template<typename Predicate>
static LiveList collectIf(unsigned numIndices, const Predicate& predicate)
{
    LiveList result;
    for (unsigned index = 0; index < numIndices; ++index) {
        if (predicate(index))
            result.push_back(index);
    }
    return result;
}

template<bool streams, typename Func>
static void forEachStorage(TestControlFlowGraph& graph, const Program& program, unsigned numIndices, const Func& func)
{
    using Liveness = TestLiveness<streams>;
    for (auto form : { Liveness::Form::Dense, Liveness::Form::Sparse }) {
        SCOPED_TRACE(::testing::Message() << numIndices << " indices, " << (form == Liveness::Form::Dense ? "dense" : "sparse") << (streams ? ", streaming" : ""));
        Liveness liveness(graph, program, numIndices, form);
        func(liveness, numIndices);
    }
}

template<typename Func>
static void forEachForm(TestControlFlowGraph& graph, const Program& program, const Func& func)
{
    for (unsigned numIndices : { wholeWordsSize, partialWordSize, 2000u }) {
        forEachStorage<false>(graph, program, numIndices, func);
        forEachStorage<true>(graph, program, numIndices, func);
    }
}

template<typename Liveness>
static void expectMatchesModel(Liveness& liveness, TestControlFlowGraph& graph, const Model& model, unsigned numIndices)
{
    auto liveAtHead = liveness.liveAtHead();
    for (unsigned blockIndex = 0; blockIndex < graph.numNodes(); ++blockIndex) {
        TestBasicBlock* block = graph.node(blockIndex);
        if (!block)
            continue;

        SCOPED_TRACE(::testing::Message() << "block " << blockIndex);
        LiveList head = toList(model.heads[blockIndex]);
        LiveList tail = toList(model.tails[blockIndex]);
        EXPECT_EQ(collect(liveness.liveAtHead(block)), head);
        EXPECT_EQ(collect(liveness.liveAtTail(block)), tail);
        EXPECT_EQ(collectIf(numIndices, [&](unsigned index) { return liveness.liveAtHead(block).contains(index); }), head);
        EXPECT_EQ(collectIf(numIndices, [&](unsigned index) { return liveness.liveAtTail(block).contains(index); }), tail);
        EXPECT_EQ(collectIf(numIndices, [&](unsigned index) { return liveAtHead.isLiveAtHead(block, index); }), head);
        EXPECT_EQ(collectFrom([&](const auto& func) { liveness.forEachLiveAtHead(block, func); }), head);
        EXPECT_EQ(collectFrom([&](const auto& func) { liveness.forEachLiveAtTail(block, func); }), tail);

        for (unsigned otherIndex = 0; otherIndex < graph.numNodes(); ++otherIndex) {
            TestBasicBlock* other = graph.node(otherIndex);
            if (!other)
                continue;

            EXPECT_EQ(collectFrom([&](const auto& func) { liveness.forEachLiveAtHeadNotLiveAtTail(block, other, func); }), difference(model.heads[blockIndex], model.tails[otherIndex])) << "other block " << otherIndex;
            EXPECT_EQ(collectFrom([&](const auto& func) { liveness.forEachLiveAtTailNotLiveAtHead(block, other, func); }), difference(model.tails[blockIndex], model.heads[otherIndex])) << "other block " << otherIndex;
        }
    }
}

TEST(WTF_Liveness, RandomProgramsAllForms)
{
    for (unsigned seed = 1; seed <= 150; ++seed) {
        SCOPED_TRACE(::testing::Message() << "seed " << seed);
        RandomInput input(seed);
        Model model = computeModel(input.graph, input.program);
        forEachForm(input.graph, input.program, [&](auto& liveness, unsigned numIndices) {
            expectMatchesModel(liveness, input.graph, model, numIndices);
        });
    }
}

TEST(WTF_Liveness, LocalCalcMatchesModel)
{
    for (unsigned seed = 1; seed <= 50; ++seed) {
        SCOPED_TRACE(::testing::Message() << "seed " << seed);
        RandomInput input(seed);
        Model model = computeModel(input.graph, input.program);
        forEachForm(input.graph, input.program, [&](auto& liveness, unsigned numIndices) {
            using Liveness = std::remove_reference_t<decltype(liveness)>;
            auto workset = liveness.makeWorkset();
            for (unsigned blockIndex = 0; blockIndex < input.graph.numNodes(); ++blockIndex) {
                SCOPED_TRACE(::testing::Message() << "block " << blockIndex);
                TestBasicBlock* block = input.graph.node(blockIndex);
                const Vector<Actions>& boundaries = input.program[blockIndex];
                LiveSet live = model.tails[blockIndex];

                liveness.copyLiveAtTailInto(workset, block);
                EXPECT_EQ(collectFrom([&](const auto& func) { workset.forEachSetBit(func); }), toList(live));
                EXPECT_EQ(collectIf(numIndices, [&](unsigned index) { return workset.contains(index); }), toList(live));

                typename Liveness::LocalCalc localCalc(liveness, block);
                EXPECT_EQ(collect(localCalc.live()), toList(live));
                for (unsigned instIndex = liveness.blockSize(block); instIndex--;) {
                    SCOPED_TRACE(::testing::Message() << "instruction " << instIndex);
                    localCalc.execute(instIndex);
                    for (unsigned index : boundaries[instIndex + 1].defs)
                        live.erase(index);
                    live.insert(boundaries[instIndex].uses.begin(), boundaries[instIndex].uses.end());

                    EXPECT_EQ(collect(localCalc.live()), toList(live));
                    EXPECT_EQ(collectIf(numIndices, [&](unsigned index) { return localCalc.live().contains(index); }), toList(live));
                    EXPECT_EQ(collectIf(numIndices, [&](unsigned index) { return localCalc.isLive(index); }), toList(live));
                }
            }
        });
    }
}

TEST(WTF_Liveness, ValueLocalToOneBlock)
{
    constexpr unsigned local = 70;
    constexpr unsigned crossing = 300;
    TestControlFlowGraph graph(2);
    graph.addEdge(0, 1);
    Program program = createEmptyProgram({ 3, 1 });
    program[0][1].defs = { local, crossing };
    program[0][2].uses = { local };
    program[1][0].uses = { crossing };
    Model model = computeModel(graph, program);

    forEachForm(graph, program, [&](auto& liveness, unsigned numIndices) {
        expectMatchesModel(liveness, graph, model, numIndices);
        EXPECT_EQ(collect(liveness.liveAtHead(graph.node(0))), LiveList { });
        EXPECT_EQ(collect(liveness.liveAtTail(graph.node(0))), LiveList { crossing });
        EXPECT_EQ(collect(liveness.liveAtHead(graph.node(1))), LiveList { crossing });
        EXPECT_EQ(collect(liveness.liveAtTail(graph.node(1))), LiveList { });
        for (unsigned blockIndex : { 0u, 1u }) {
            EXPECT_FALSE(liveness.liveAtHead(graph.node(blockIndex)).contains(local));
            EXPECT_FALSE(liveness.liveAtTail(graph.node(blockIndex)).contains(local));
            EXPECT_FALSE(liveness.liveAtHead().isLiveAtHead(graph.node(blockIndex), local));
        }
    });
}

TEST(WTF_Liveness, NoIndexCrossesBoundary)
{
    constexpr unsigned numIndices = partialWordSize;
    constexpr unsigned lastIndex = numIndices - 1;
    TestControlFlowGraph graph(2);
    graph.addEdge(0, 1);
    graph.addEdge(1, 1);
    Program program = createEmptyProgram({ 3, 3 });
    for (Vector<Actions>& boundaries : program) {
        boundaries[1].defs = { 3, lastIndex };
        boundaries[2].uses = { 3, lastIndex };
    }
    Model model = computeModel(graph, program);

    auto check = [&](auto& liveness, unsigned) {
        using Liveness = std::remove_reference_t<decltype(liveness)>;
        expectMatchesModel(liveness, graph, model, numIndices);
        for (unsigned blockIndex : { 0u, 1u }) {
            TestBasicBlock* block = graph.node(blockIndex);
            auto head = liveness.liveAtHead(block);
            auto tail = liveness.liveAtTail(block);
            EXPECT_TRUE(head.begin() == head.end());
            EXPECT_TRUE(tail.begin() == tail.end());

            typename Liveness::LocalCalc localCalc(liveness, block);
            EXPECT_EQ(collect(localCalc.live()), LiveList { });
            localCalc.execute(2);
            EXPECT_EQ(collect(localCalc.live()), (LiveList { 3, lastIndex }));
            localCalc.execute(1);
            EXPECT_EQ(collect(localCalc.live()), (LiveList { 3, lastIndex }));
            localCalc.execute(0);
            EXPECT_EQ(collect(localCalc.live()), LiveList { });
        }
    };
    forEachStorage<false>(graph, program, numIndices, check);
    forEachStorage<true>(graph, program, numIndices, check);
}

TEST(WTF_Liveness, IndexPastEnd)
{
    constexpr unsigned lastIndex = wholeWordsSize - 1;
    TestControlFlowGraph graph(2);
    graph.addEdge(0, 1);
    Program program = createEmptyProgram({ 0, 1 });
    program[1][0].uses = { 0, lastIndex };

    forEachForm(graph, program, [&](auto& liveness, unsigned numIndices) {
        using Liveness = std::remove_reference_t<decltype(liveness)>;
        TestBasicBlock* block = graph.node(0);
        EXPECT_EQ(collect(liveness.liveAtHead(block)), (LiveList { 0, lastIndex }));
        typename Liveness::LocalCalc localCalc(liveness, block);
        for (unsigned index : { numIndices, numIndices + 70 }) {
            EXPECT_FALSE(liveness.liveAtHead(block).contains(index));
            EXPECT_FALSE(liveness.liveAtTail(block).contains(index));
            EXPECT_FALSE(liveness.liveAtHead().isLiveAtHead(block, index));
            EXPECT_FALSE(localCalc.live().contains(index));
            EXPECT_FALSE(localCalc.isLive(index));
        }
    });
}

TEST(WTF_Liveness, NullAndUnreachableNodes)
{
    constexpr unsigned definedInRoot = 5;
    constexpr unsigned definedBeforeNullNode = 70;
    constexpr unsigned usedAtTailBeforeNullNode = 130;
    constexpr unsigned definedAfterNullNode = 200;
    constexpr unsigned usedAtTailAfterNullNode = 260;
    constexpr unsigned usedInUnreachableBlock = 400;
    TestControlFlowGraph graph(6, 3);
    graph.removeBlock(2);
    graph.addEdge(0, 1);
    graph.addEdge(1, 3);
    graph.addEdge(3, 4);
    graph.addEdge(5, 1);
    Program program = createEmptyProgram({ 1, 1, 0, 1, 1, 1 });
    program[0][1].defs = { definedInRoot };
    program[1][0].uses = { definedInRoot };
    program[1][1].defs = { definedBeforeNullNode };
    program[1][1].uses = { usedAtTailBeforeNullNode };
    program[3][0].uses = { definedBeforeNullNode };
    program[3][1].defs = { definedAfterNullNode };
    program[3][1].uses = { usedAtTailAfterNullNode };
    program[4][0].uses = { definedAfterNullNode };
    program[5][0].uses = { usedInUnreachableBlock };
    Model model = computeModel(graph, program);

    forEachForm(graph, program, [&](auto& liveness, unsigned numIndices) {
        expectMatchesModel(liveness, graph, model, numIndices);
        EXPECT_EQ(collect(liveness.liveAtHead(graph.node(0))), (LiveList { usedAtTailBeforeNullNode, usedAtTailAfterNullNode }));
        EXPECT_EQ(collect(liveness.liveAtTail(graph.node(0))), (LiveList { definedInRoot, usedAtTailBeforeNullNode, usedAtTailAfterNullNode }));
        EXPECT_EQ(collect(liveness.liveAtHead(graph.node(1))), (LiveList { definedInRoot, usedAtTailBeforeNullNode, usedAtTailAfterNullNode }));
        EXPECT_EQ(collect(liveness.liveAtTail(graph.node(1))), (LiveList { definedBeforeNullNode, usedAtTailBeforeNullNode, usedAtTailAfterNullNode }));
        EXPECT_EQ(collect(liveness.liveAtHead(graph.node(3))), (LiveList { definedBeforeNullNode, usedAtTailAfterNullNode }));
        EXPECT_EQ(collect(liveness.liveAtTail(graph.node(3))), (LiveList { definedAfterNullNode, usedAtTailAfterNullNode }));
        EXPECT_EQ(collect(liveness.liveAtHead(graph.node(4))), LiveList { definedAfterNullNode });
        EXPECT_EQ(collect(liveness.liveAtTail(graph.node(4))), LiveList { });
        EXPECT_EQ(collect(liveness.liveAtHead(graph.node(5))), (LiveList { definedInRoot, usedAtTailBeforeNullNode, usedAtTailAfterNullNode, usedInUnreachableBlock }));
        EXPECT_EQ(collect(liveness.liveAtTail(graph.node(5))), (LiveList { definedInRoot, usedAtTailBeforeNullNode, usedAtTailAfterNullNode }));
    });
}

TEST(WTF_Liveness, NoIndices)
{
    TestControlFlowGraph graph(2);
    graph.addEdge(0, 1);
    Program program = createEmptyProgram({ 1, 0 });
    Model model = computeModel(graph, program);

    auto check = [&](auto& liveness, unsigned numIndices) {
        expectMatchesModel(liveness, graph, model, numIndices);
        EXPECT_FALSE(liveness.liveAtHead(graph.node(1)).contains(0));
        EXPECT_FALSE(liveness.liveAtTail(graph.node(0)).contains(0));
    };
    forEachStorage<false>(graph, program, 0, check);
    forEachStorage<true>(graph, program, 0, check);
}

} // namespace TestWebKitAPI
