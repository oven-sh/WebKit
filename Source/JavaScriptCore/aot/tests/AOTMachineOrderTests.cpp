/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTGrowthOfWork.h"
#include "AOTMachineOrder.h"
#include "AOTMachineTestSupport.h"
#include <algorithm>
#include <initializer_list>
#include <limits>
#include <wtf/DataLog.h>

// No request goes to the builder between patchpoint() and the last word of the setup: the next request seals the Patch.
// No constant here is one that findPinnedConstant() knows: the tests run without an instance.

namespace JSC { namespace AOT { namespace Machine {

// The entry block reads three arguments, whether or not the test does: the texts below count on it.
struct OrderTest {
    explicit OrderTest(const ProcedureTraits& traits = { })
        : testProcedure(traits)
        , out(testProcedure.out())
        , entry(out.m_block)
        , first(testProcedure.argumentGPR(0))
        , second(testProcedure.argumentGPR(1))
        , third(testProcedure.argumentGPR(2))
    {
    }

    Procedure& procedure() { return testProcedure.procedure(); }
    String printedOrder() const { return testProcedure.printedOrder(); }

    // Each fills a block whole. The caller has made the values, so they stand in other blocks or have no position.
    void returnValue(LBasicBlock block, LValue value)
    {
        out.appendTo(block);
        out.ret(value);
    }
    void returnConstant(LBasicBlock block, int64_t constant) { returnValue(block, out.constInt64(constant)); }
    void passAndJump(LBasicBlock block, LValue value, LValue phi, Edge target)
    {
        out.appendTo(block);
        out.upsilon(value, phi);
        out.jump(target);
    }

    // One of the two, once: compile() prepares too. Both check what holds of every order.
    const Order& prepare();
    const Order& compile();
    int64_t run(int64_t firstArgument, int64_t secondArgument = 0, int64_t thirdArgument = 0) { return testProcedure.call<int64_t>(firstArgument, secondArgument, thirdArgument); }

    TestProcedure testProcedure;
    Output& out;
    LBasicBlock entry;
    LValue first;
    LValue second;
    LValue third;
};

// Only behind a prepare() or compile() that went well.
[[noreturn]] static void failOrderTest(const OrderTest& test, const char* file, int line, const char* condition)
{
    dataLogLn(test.testProcedure.printedProcedure(), test.printedOrder());
    failNativeTest(file, line, condition);
}

#define AOT_ORDER_TEST_CHECK(test, ...) do { \
        if (!(__VA_ARGS__)) [[unlikely]] \
            failOrderTest(test, __FILE__, __LINE__, #__VA_ARGS__); \
    } while (false)

// By Block::index(): normal edges alone lead to the block from an entrypoint.
static Vector<bool> orderTestFindLikelyBlocks(OrderTest& test)
{
    Vector<bool> isLikely;
    isLikely.fill(false, test.procedure().numberOfBlocks());
    Vector<const Block*> worklist;
    auto reach = [&](const Block* block) {
        if (isLikely[block->index()])
            return;
        isLikely[block->index()] = true;
        worklist.append(block);
    };
    for (const Entrypoint& entrypoint : test.procedure().entrypoints())
        reach(entrypoint.block);
    while (!worklist.isEmpty()) {
        for (const Edge& edge : worklist.takeLast()->successors()) {
            if (edge.edgeClass() == EdgeClass::Normal)
                reach(edge.block());
        }
    }
    return isLikely;
}

// The three parts follow each other. Within a part the blocks stand as in the order of allocation, but that the blocks of an outermost loop, which stand in one piece, may be rotated.
static void orderTestCheckLayout(OrderTest& test)
{
    const Order& order = test.procedure().order();
    std::span<Block* const> layout = order.blocksInLayout();
    Vector<bool> isLikely = orderTestFindLikelyBlocks(test);
    // A block that was split late on a rare edge belongs to no rare region and stands in the rare part all the same.
    auto partOf = [&](const Block* block) {
        const BlockPlace& place = order.of(block);
        bool standsOnRareEdge = place.wasSplitLate && std::ranges::any_of(place.predecessors.front()->successors(), [&](const Edge& edge) {
            return edge.block() == block && edge.isRare();
        });
        return place.isRare() || standsOnRareEdge ? 2 : isLikely[block->index()] ? 0 : 1;
    };
    auto rankOf = [&](const Block* block) {
        uint32_t loop = order.of(block).loop;
        if (loop == noLoop || order.of(block).isRare())
            return order.of(block).orderIndex;
        while (order.loops()[loop].parent != noLoop)
            loop = order.loops()[loop].parent;
        return order.loops()[loop].firstBlock;
    };
    const Block* lastBlockPlacedByOrder = nullptr;
    for (uint32_t index = 0; index < layout.size(); ++index) {
        AOT_ORDER_TEST_CHECK(test, static_cast<int>(order.of(layout[index]).layoutPart) == partOf(layout[index]));
        AOT_ORDER_TEST_CHECK(test, !index || partOf(layout[index - 1]) <= partOf(layout[index]));
        // One that was split late stands right behind the block that leads to it, or behind everything else in its part.
        if (order.of(layout[index]).wasSplitLate)
            continue;
        AOT_ORDER_TEST_CHECK(test, !lastBlockPlacedByOrder || partOf(lastBlockPlacedByOrder) != partOf(layout[index]) || rankOf(lastBlockPlacedByOrder) <= rankOf(layout[index]));
        lastBlockPlacedByOrder = layout[index];
    }
}

static void orderTestCheckPlaces(OrderTest& test)
{
    const Order& order = test.procedure().order();
    std::span<Block* const> blocks = order.blocksInOrder();
    std::span<Block* const> layout = order.blocksInLayout();
    AOT_ORDER_TEST_CHECK(test, !blocks.empty() && blocks.size() == layout.size() && blocks.front() == test.entry && layout.front() == test.entry);
    orderTestCheckLayout(test);

    Position position = 0;
    for (uint32_t index = 0; index < blocks.size(); ++index) {
        const BlockPlace& place = order.of(blocks[index]);
        // Behind a block with an unsplit edge positions are left free, and a block that was split late has some of them.
        bool followsBlockWithSeveralSuccessors = index && (place.wasSplitLate ? place.predecessors.front() : blocks[index - 1])->numSuccessors() > 1;
        AOT_ORDER_TEST_CHECK(test, place.orderIndex == index && (place.head == position || (place.head > position && (followsBlockWithSeveralSuccessors || order.of(blocks[index - 1]).wasSplitLate))));
        AOT_ORDER_TEST_CHECK(test, !place.wasSplitLate || (place.predecessors.size() == 1 && blocks[index]->size() == 1 && blocks[index]->last()->opcode() == Jump && order.of(place.predecessors.front()).orderIndex < index));
        AOT_ORDER_TEST_CHECK(test, place.layoutIndex < layout.size() && layout[place.layoutIndex] == blocks[index]);
        position = order.endOf(blocks[index]);
    }
    AOT_ORDER_TEST_CHECK(test, order.endOfProcedure() >= position);

    uint32_t numberOutsideRareRegions = order.numberOfBlocksOutsideRareRegions();
    const Block* lastRareBlock = nullptr;
    for (uint32_t index = 0; index < layout.size(); ++index) {
        const BlockPlace& place = order.of(layout[index]);
        AOT_ORDER_TEST_CHECK(test, (place.layoutPart == LayoutPart::Rare) == (index >= numberOutsideRareRegions) && (!place.isRare() || place.layoutPart == LayoutPart::Rare));
        if (!place.isRare())
            continue;
        AOT_ORDER_TEST_CHECK(test, !lastRareBlock || order.of(lastRareBlock).orderIndex < place.orderIndex);
        lastRareBlock = layout[index];
    }

    size_t numberReached = 0;
    for (const Block* block : test.procedure().blocks())
        numberReached += order.of(block).isReached();
    AOT_ORDER_TEST_CHECK(test, numberReached == blocks.size());
}

// A value that is defined ahead of the target of an edge that leads back, and read behind it, has to live as far as the edge. So each such target begins a loop of its own that holds the source.
static void orderTestCheckEdges(OrderTest& test)
{
    const Order& order = test.procedure().order();
    size_t numberOfEdges = 0;
    size_t numberOfPredecessors = 0;
    for (Block* block : order.blocksInOrder()) {
        const BlockPlace& place = order.of(block);
        numberOfEdges += block->numSuccessors();
        numberOfPredecessors += place.predecessors.size();
        AOT_ORDER_TEST_CHECK(test, place.predecessors.empty() == block->isEntry());
        AOT_ORDER_TEST_CHECK(test, !place.entersRareRegion || block->isLandingPad() || place.predecessors.size() == 1 || std::ranges::none_of(place.predecessors, [&](const Block* predecessor) { return order.of(predecessor).isRare(); }));
        AOT_ORDER_TEST_CHECK(test, !place.leavesRareRegion || block->numSuccessors() == 1);
        AOT_ORDER_TEST_CHECK(test, !place.leavesRareRegion || block->last()->isUnconditionalJump() || block->successor(0).block()->isLandingPad());
        AOT_ORDER_TEST_CHECK(test, block->last()->opcode() != Branch || block->numSuccessors() == 1 || block->successor(0).block() != block->successor(1).block());
        bool endsInPatch = block->last()->opcode() == Patch;

        for (const Edge& edge : block->successors()) {
            const BlockPlace& target = order.of(edge.block());
            AOT_ORDER_TEST_CHECK(test, target.isReached() && std::ranges::find(target.predecessors, block) != target.predecessors.end());
            bool isUnsplit = !edge.block()->isLandingPad() && Order::isUnsplit(block, target);
            AOT_ORDER_TEST_CHECK(test, edge.block()->isLandingPad() || block->numSuccessors() == 1 || target.predecessors.size() == 1 || isUnsplit);
            AOT_ORDER_TEST_CHECK(test, !isUnsplit || (!place.isRare() && edge.block()->phis().empty() && block->upsilons().empty() && edge.isRare() == target.isRare()));

            // Moves into registers stand ahead of a Jump and nowhere else.
            bool holdsExtractsOfBlock = endsInPatch && block->last()->type().isTuple() && &edge == &block->successor(0);
            AOT_ORDER_TEST_CHECK(test, edge.block()->isLandingPad() || block->numSuccessors() == 1 || edge.block()->phis().empty() || holdsExtractsOfBlock);
            AOT_ORDER_TEST_CHECK(test, edge.block()->isLandingPad() || !endsInPatch || target.predecessors.size() == 1);

            if (place.wasSplitLate || target.wasSplitLate)
                AOT_ORDER_TEST_CHECK(test, !place.isRare() && (!target.isRare() || target.entersRareRegion));
            else if (!place.isRare())
                AOT_ORDER_TEST_CHECK(test, edge.isRare() == target.isRare() && target.entersRareRegion == target.isRare());
            else if (target.isRare())
                AOT_ORDER_TEST_CHECK(test, target.rareRegion == place.rareRegion || (edge.block()->isLandingPad() && target.entersRareRegion && target.rareRegion > place.rareRegion && target.orderIndex > place.orderIndex));
            else
                AOT_ORDER_TEST_CHECK(test, place.leavesRareRegion);

            if (target.orderIndex > place.orderIndex)
                continue;
            AOT_ORDER_TEST_CHECK(test, target.isLoopHead && target.loop < order.loops().size());
            const Loop& loop = order.loops()[target.loop];
            AOT_ORDER_TEST_CHECK(test, loop.firstBlock == target.orderIndex && place.orderIndex < loop.endBlock);
        }
    }
    AOT_ORDER_TEST_CHECK(test, numberOfEdges == numberOfPredecessors);
}

static void orderTestCheckLoops(OrderTest& test)
{
    const Order& order = test.procedure().order();
    std::span<Block* const> blocks = order.blocksInOrder();
    std::span<const Loop> loops = order.loops();
    for (const Loop& loop : loops) {
        AOT_ORDER_TEST_CHECK(test, loop.firstBlock < loop.endBlock && loop.endBlock <= blocks.size());
        AOT_ORDER_TEST_CHECK(test, loop.begin == order.headOf(blocks[loop.firstBlock]) && loop.end == (loop.endBlock < blocks.size() ? order.headOf(blocks[loop.endBlock]) : order.endOfProcedure()));
        AOT_ORDER_TEST_CHECK(test, loop.parent == noLoop || loop.parent < loops.size());
        AOT_ORDER_TEST_CHECK(test, loop.parent == noLoop || (loops[loop.parent].firstBlock < loop.firstBlock && loop.endBlock <= loops[loop.parent].endBlock));
    }

    for (uint32_t index = 0; index < blocks.size(); ++index) {
        uint32_t innermost = noLoop;
        for (uint32_t candidate = 0; candidate < loops.size(); ++candidate) {
            bool holdsBlock = loops[candidate].firstBlock <= index && index < loops[candidate].endBlock;
            if (holdsBlock && (innermost == noLoop || loops[innermost].firstBlock < loops[candidate].firstBlock))
                innermost = candidate;
        }
        const BlockPlace& place = order.of(blocks[index]);
        AOT_ORDER_TEST_CHECK(test, place.loop == innermost && place.isLoopHead == (innermost != noLoop && loops[innermost].firstBlock == index));
    }
}

static void orderTestCheckRareRegions(OrderTest& test)
{
    const Order& order = test.procedure().order();
    std::span<const RareRegion> regions = order.rareRegions();
    Vector<unsigned> numberOfEntries;
    Vector<Block*> lastEntry;
    numberOfEntries.fill(0, regions.size());
    lastEntry.fill(nullptr, regions.size());
    for (Block* block : order.blocksInOrder()) {
        const BlockPlace& place = order.of(block);
        AOT_ORDER_TEST_CHECK(test, !place.isRare() || place.rareRegion < regions.size());
        if (!place.isRare() || !place.entersRareRegion)
            continue;
        // Each unsplit edge is a way in of its own.
        numberOfEntries[place.rareRegion] += block->isLandingPad() ? 1 : place.predecessors.size();
        lastEntry[place.rareRegion] = block;
    }
    for (uint32_t region = 0; region < regions.size(); ++region)
        AOT_ORDER_TEST_CHECK(test, numberOfEntries[region] && regions[region].onlyEntry == (numberOfEntries[region] == 1 ? lastEntry[region] : nullptr));
}

static const Order& orderTestCheckedOrder(OrderTest& test)
{
    orderTestCheckPlaces(test);
    orderTestCheckEdges(test);
    orderTestCheckLoops(test);
    orderTestCheckRareRegions(test);
    return test.procedure().order();
}

const Order& OrderTest::prepare()
{
    prepareOrFail(testProcedure);
    return orderTestCheckedOrder(*this);
}

const Order& OrderTest::compile()
{
    compileOrFail(testProcedure);
    return orderTestCheckedOrder(*this);
}

// Where an edge leads in the end, past the blocks that hold a Jump alone: Order puts such a block on an edge that it splits, and threadJumps() may leave one behind.
static Block* orderTestTargetOf(const Block* from, unsigned successorIndex)
{
    Block* target = from->successor(successorIndex).block();
    while (target->size() == 1 && target->last()->opcode() == Jump && target->phis().empty() && target->upsilons().empty())
        target = target->successor(0).block();
    return target;
}

static unsigned orderTestNumberOfReached(const Order& order, Opcode opcode)
{
    unsigned result = 0;
    for (const Block* block : order.blocksInOrder()) {
        for (const Operation* operation : block->operations())
            result += operation->opcode() == opcode;
    }
    return result;
}

static bool orderTestLoopHoldsExactly(const Order& order, uint32_t loop, std::initializer_list<const Block*> blocks)
{
    if (loop >= order.loops().size())
        return false;
    const Loop& interval = order.loops()[loop];
    return interval.endBlock - interval.firstBlock == blocks.size() && std::ranges::all_of(blocks, [&](const Block* block) {
        return interval.firstBlock <= order.of(block).orderIndex && order.of(block).orderIndex < interval.endBlock;
    });
}

static void testMachineOrderPlacesRareBlockAheadOfItsRejoin()
{
    OrderTest test;
    Output& out = test.out;
    LBasicBlock slow = out.newBlock();
    LBasicBlock usual = out.newBlock();
    LBasicBlock join = out.newBlock();
    LValue result = out.phiIn(join, Int64);
    out.branch(test.first, rareEdge(slow), normalEdge(usual));
    test.passAndJump(slow, test.second, result, join);
    out.appendTo(usual);
    out.upsilon(out.add(test.second, 7), result);
    out.jump(join);
    test.returnValue(join, result);
    test.compile();

    AOT_ORDER_TEST_CHECK(test, test.printedOrder() ==
        "#0: head = 0\n"
        "#2: head = 10 <- #0\n"
        "#1: head = 16, rare region 0, enters it, leaves it <- #0\n"
        "#3: head = 20 <- #2 #1\n"
        "rare region 0: entry #1\n"
        "layout: #0 #2 #3 #1\n"_s);
    AOT_ORDER_TEST_CHECK(test, test.run(0, 5) == 12 && test.run(1, 5) == 5);
}

static void testMachineOrderPlacesRareBlockThatNeverRejoinsBehindItsBranch()
{
    OrderTest test;
    Output& out = test.out;
    LBasicBlock fails = out.newBlock();
    LBasicBlock next = out.newBlock();
    LBasicBlock last = out.newBlock();
    out.branch(test.first, rareEdge(fails), normalEdge(next));
    test.returnConstant(fails, -1);
    out.appendTo(next);
    LValue sum = out.add(test.second, 7);
    out.jump(last);
    test.returnValue(last, sum);
    const Order& order = test.compile();

    const BlockPlace& place = order.of(fails);
    AOT_ORDER_TEST_CHECK(test, place.isRare() && place.entersRareRegion && !place.leavesRareRegion);
    AOT_ORDER_TEST_CHECK(test, place.orderIndex == 1 && order.of(next).orderIndex == 2 && order.blocksInLayout().back() == fails);
    AOT_ORDER_TEST_CHECK(test, order.rareRegions().size() == 1 && order.rareRegions()[0].onlyEntry == fails);
    AOT_ORDER_TEST_CHECK(test, test.run(0, 5) == 12 && test.run(1, 5) == -1);
}

static void testMachineOrderPlacesUnlikelySuccessorBehindNormalOne()
{
    for (EdgeClass classOfFirstWay : { EdgeClass::Normal, EdgeClass::Unlikely }) {
        OrderTest test;
        LBasicBlock firstWay = test.out.newBlock();
        LBasicBlock secondWay = test.out.newBlock();
        test.out.branch(test.first, Edge(firstWay, classOfFirstWay), normalEdge(secondWay));
        test.returnConstant(firstWay, 10);
        test.returnConstant(secondWay, 20);
        const Order& order = test.compile();

        AOT_ORDER_TEST_CHECK(test, (order.of(firstWay).orderIndex < order.of(secondWay).orderIndex) == (classOfFirstWay == EdgeClass::Normal));
        AOT_ORDER_TEST_CHECK(test, order.rareRegions().empty() && std::ranges::equal(order.blocksInOrder(), order.blocksInLayout()));
        AOT_ORDER_TEST_CHECK(test, test.run(1) == 10 && test.run(0) == 20);
    }
}

static void testMachineOrderKeepsLoopContiguous()
{
    OrderTest test;
    Output& out = test.out;
    LBasicBlock head = out.newBlock();
    LBasicBlock body = out.newBlock();
    LBasicBlock sideExit = out.newBlock();
    LBasicBlock latch = out.newBlock();
    LBasicBlock exit = out.newBlock();
    LValue index = out.phiIn(head, Int64);
    out.upsilon(out.int64Zero, index);
    out.jump(head);
    out.appendTo(head);
    out.branch(out.below(index, test.first), normalEdge(body), normalEdge(exit));
    // The way out comes first among the successors, so a plain reverse postorder has it between two blocks of the loop.
    out.appendTo(body);
    out.branch(out.equal(index, test.second), normalEdge(sideExit), normalEdge(latch));
    out.appendTo(sideExit);
    out.ret(out.add(index, 100));
    out.appendTo(latch);
    out.upsilon(out.add(index, 1), index);
    out.jump(head);
    test.returnValue(exit, index);
    const Order& order = test.compile();

    AOT_ORDER_TEST_CHECK(test, order.loops().size() == 1 && orderTestLoopHoldsExactly(order, 0, { head, body, latch }));
    AOT_ORDER_TEST_CHECK(test, test.printedOrder() ==
        "#0: head = 0\n"
        "#1: head = 10, loop 0, loop head <- #0 #4\n"
        "#2: head = 16, loop 0 <- #1\n"
        "#4: head = 22, loop 0 <- #2\n"
        "#3: head = 28 <- #2\n"
        "#5: head = 34 <- #1\n"
        "loop 0: #1 to #4, positions 10 to 28\n"
        "layout: #0 #4 #1 #2 #3 #5\n"_s);
    AOT_ORDER_TEST_CHECK(test, test.run(5, 9) == 5 && test.run(5, 3) == 103);
}

static bool orderTestLayoutIs(const Order& order, std::initializer_list<const Block*> blocks)
{
    return std::ranges::equal(order.blocksInLayout(), blocks);
}

// while (index < first) index++. The body falls through to the test, which branches back to it: one branch a turn.
static void testMachineOrderEndsLoopWithItsTest()
{
    OrderTest test;
    Output& out = test.out;
    LBasicBlock head = out.newBlock();
    LBasicBlock body = out.newBlock();
    LBasicBlock exit = out.newBlock();
    LValue index = out.phiIn(head, Int64);
    out.upsilon(out.int64Zero, index);
    out.jump(head);
    out.appendTo(head);
    out.branch(out.below(index, test.first), normalEdge(body), normalEdge(exit));
    out.appendTo(body);
    out.upsilon(out.add(index, 1), index);
    out.jump(head);
    test.returnValue(exit, index);
    const Order& order = test.compile();

    AOT_ORDER_TEST_CHECK(test, order.of(head).isLoopHead && order.of(head).orderIndex < order.of(body).orderIndex);
    AOT_ORDER_TEST_CHECK(test, orderTestLayoutIs(order, { test.entry, body, head, exit }));
    AOT_ORDER_TEST_CHECK(test, !test.run(0) && test.run(1) == 1 && test.run(5) == 5);
}

// The head only counts, and the test stands in the block behind it. The loop ends with the block that holds the test.
static void testMachineOrderEndsLoopWithTestBehindItsHead()
{
    OrderTest test;
    Output& out = test.out;
    LBasicBlock head = out.newBlock();
    LBasicBlock slow = out.newBlock();
    LBasicBlock tests = out.newBlock();
    LBasicBlock body = out.newBlock();
    LBasicBlock exit = out.newBlock();
    LValue index = out.phiIn(head, Int64);
    TypedPointer counter = out.address(test.testProcedure.heaps().argumentBuffer, test.third);
    out.upsilon(out.int64Zero, index);
    out.jump(head);
    out.appendTo(head);
    out.branch(out.equal(index, test.second), rareEdge(slow), normalEdge(tests));
    out.appendTo(slow);
    out.store64(out.add(out.load64(counter), 1), counter);
    out.jump(tests);
    out.appendTo(tests);
    out.branch(out.below(index, test.first), normalEdge(body), normalEdge(exit));
    out.appendTo(body);
    out.upsilon(out.add(index, 1), index);
    out.jump(head);
    test.returnValue(exit, index);
    const Order& order = test.compile();

    AOT_ORDER_TEST_CHECK(test, order.of(head).isLoopHead && order.of(slow).isRare());
    AOT_ORDER_TEST_CHECK(test, order.of(body).layoutIndex == 1 && order.of(head).layoutIndex == 2 && order.of(exit).layoutIndex == order.of(tests).layoutIndex + 1);
    int64_t count = 0;
    AOT_ORDER_TEST_CHECK(test, test.run(5, 3, reinterpret_cast<int64_t>(&count)) == 5 && count == 1 && !test.run(0, 9, reinterpret_cast<int64_t>(&count)) && count == 1);
}

// A block of the loop leads to rare blocks alone, one of which leads on within the loop. The block is one of the loop all the same: it stands ahead of the jump back, and the interval ends with that jump.
static void testMachineOrderEndsLoopWithItsTestWhereRareBlockLeadsBackIntoIt()
{
    OrderTest test;
    Output& out = test.out;
    LBasicBlock head = out.newBlock();
    LBasicBlock body = out.newBlock();
    LBasicBlock odd = out.newBlock();
    LBasicBlock slow = out.newBlock();
    LBasicBlock fails = out.newBlock();
    LBasicBlock latch = out.newBlock();
    LBasicBlock exit = out.newBlock();
    LValue index = out.phiIn(head, Int64);
    out.upsilon(out.int64Zero, index);
    out.jump(head);
    out.appendTo(head);
    out.branch(out.below(index, test.first), normalEdge(body), normalEdge(exit));
    out.appendTo(body);
    out.branch(out.equal(index, test.second), normalEdge(odd), normalEdge(latch));
    out.appendTo(odd);
    out.branch(test.third, rareEdge(slow), rareEdge(fails));
    out.appendTo(slow);
    out.jump(latch);
    test.returnConstant(fails, -1);
    out.appendTo(latch);
    out.upsilon(out.add(index, 1), index);
    out.jump(head);
    test.returnValue(exit, index);
    const Order& order = test.compile();

    AOT_ORDER_TEST_CHECK(test, !order.of(odd).isRare() && order.of(body).orderIndex < order.of(odd).orderIndex && order.of(odd).orderIndex < order.of(latch).orderIndex);
    AOT_ORDER_TEST_CHECK(test, order.loops().size() == 1 && order.loops()[0].firstBlock == order.of(head).orderIndex && order.loops()[0].endBlock == order.of(latch).orderIndex + 1);
    // The way out of the head falls through, so the head ends what is rotated.
    AOT_ORDER_TEST_CHECK(test, order.of(odd).layoutIndex == order.of(body).layoutIndex + 1 && order.of(head).layoutIndex == order.of(latch).layoutIndex + 1 && order.of(exit).layoutIndex == order.of(head).layoutIndex + 1);
    AOT_ORDER_TEST_CHECK(test, test.run(5, 3, 1) == 5 && test.run(5, 3, 0) == -1 && test.run(5, 9, 0) == 5);
}

// for (row..) for (column..) count++. Each loop ends with its own test, the inner one in one piece within the outer one.
static void testMachineOrderEndsEachNestedLoopWithItsTest()
{
    OrderTest test;
    Output& out = test.out;
    LBasicBlock outerHead = out.newBlock();
    LBasicBlock entersInner = out.newBlock();
    LBasicBlock innerHead = out.newBlock();
    LBasicBlock innerBody = out.newBlock();
    LBasicBlock outerLatch = out.newBlock();
    LBasicBlock done = out.newBlock();
    LValue row = out.phiIn(outerHead, Int64);
    LValue column = out.phiIn(innerHead, Int64);
    TypedPointer counter = out.address(test.testProcedure.heaps().argumentBuffer, test.third);
    out.upsilon(out.int64Zero, row);
    out.jump(outerHead);
    out.appendTo(outerHead);
    out.branch(out.below(row, test.first), normalEdge(entersInner), normalEdge(done));
    out.appendTo(entersInner);
    out.upsilon(out.int64Zero, column);
    out.jump(innerHead);
    out.appendTo(innerHead);
    out.branch(out.below(column, test.second), normalEdge(innerBody), normalEdge(outerLatch));
    out.appendTo(innerBody);
    out.store64(out.add(out.load64(counter), 1), counter);
    out.upsilon(out.add(column, 1), column);
    out.jump(innerHead);
    out.appendTo(outerLatch);
    out.upsilon(out.add(row, 1), row);
    out.jump(outerHead);
    test.returnValue(done, row);
    const Order& order = test.compile();

    AOT_ORDER_TEST_CHECK(test, orderTestLayoutIs(order, { test.entry, entersInner, innerBody, innerHead, outerLatch, outerHead, done }));
    int64_t rounds = 0;
    AOT_ORDER_TEST_CHECK(test, test.run(3, 4, reinterpret_cast<int64_t>(&rounds)) == 3 && rounds == 12);
}

// do { index++ } while (index < first). The test stands at the end as written, and what the edge back moves stands ahead of the head.
static void testMachineOrderLeavesLoopThatEndsWithItsTest()
{
    OrderTest test;
    Output& out = test.out;
    LBasicBlock body = out.newBlock();
    LBasicBlock exit = out.newBlock();
    LValue index = out.phiIn(body, Int64);
    out.upsilon(out.int64Zero, index);
    out.jump(body);
    out.appendTo(body);
    LValue next = out.add(index, 1);
    out.upsilon(next, index);
    out.branch(out.below(next, test.first), normalEdge(body), normalEdge(exit));
    test.returnValue(exit, next);
    const Order& order = test.compile();

    Block* blockOnEdgeBack = body->successor(0).block();
    AOT_ORDER_TEST_CHECK(test, blockOnEdgeBack != body && order.of(body).isLoopHead);
    AOT_ORDER_TEST_CHECK(test, orderTestLayoutIs(order, { test.entry, blockOnEdgeBack, body, exit }));
    AOT_ORDER_TEST_CHECK(test, test.run(0) == 1 && test.run(5) == 5);
}

// The likely arm falls through to the join. The unlikely one stands behind everything likely and ahead of everything rare.
static void testMachineOrderPlacesUnlikelyArmBehindLikelyBlocks()
{
    OrderTest test;
    Output& out = test.out;
    LBasicBlock seldom = out.newBlock();
    LBasicBlock usual = out.newBlock();
    LBasicBlock join = out.newBlock();
    LBasicBlock fails = out.newBlock();
    LBasicBlock done = out.newBlock();
    LValue result = out.phiIn(join, Int64);
    out.branch(test.first, unlikelyEdge(seldom), normalEdge(usual));
    out.appendTo(seldom);
    out.upsilon(out.add(test.second, 100), result);
    out.jump(join);
    out.appendTo(usual);
    out.upsilon(out.add(test.second, 7), result);
    out.jump(join);
    out.appendTo(join);
    out.branch(test.third, rareEdge(fails), normalEdge(done));
    test.returnConstant(fails, -1);
    test.returnValue(done, result);
    const Order& order = test.compile();

    AOT_ORDER_TEST_CHECK(test, order.of(seldom).orderIndex < order.of(join).orderIndex && !order.of(seldom).isRare() && order.numberOfBlocksOutsideRareRegions() == 5);
    AOT_ORDER_TEST_CHECK(test, orderTestLayoutIs(order, { test.entry, usual, join, done, seldom, fails }));
    AOT_ORDER_TEST_CHECK(test, test.run(0, 5) == 12 && test.run(1, 5) == 105 && test.run(0, 5, 1) == -1);
}

// An unlikely arm in a loop stands outside the loop, which is rotated as if the arm were not there.
static void testMachineOrderTakesUnlikelyArmOutOfLoop()
{
    OrderTest test;
    Output& out = test.out;
    LBasicBlock head = out.newBlock();
    LBasicBlock body = out.newBlock();
    LBasicBlock seldom = out.newBlock();
    LBasicBlock latch = out.newBlock();
    LBasicBlock exit = out.newBlock();
    LValue index = out.phiIn(head, Int64);
    LValue step = out.phiIn(latch, Int64);
    out.upsilon(out.int64Zero, index);
    out.jump(head);
    out.appendTo(head);
    out.branch(out.below(index, test.first), normalEdge(body), normalEdge(exit));
    out.appendTo(body);
    out.upsilon(out.constInt64(1), step);
    out.branch(out.equal(index, test.second), unlikelyEdge(seldom), normalEdge(latch));
    test.passAndJump(seldom, out.constInt64(3), step, latch);
    out.appendTo(latch);
    out.upsilon(out.add(index, step), index);
    out.jump(head);
    test.returnValue(exit, index);
    const Order& order = test.compile();

    AOT_ORDER_TEST_CHECK(test, !order.of(seldom).loop && !order.of(seldom).isRare() && order.blocksInLayout().back() == seldom);
    AOT_ORDER_TEST_CHECK(test, order.of(body).layoutIndex == 1 && order.of(head).layoutIndex == order.of(latch).layoutIndex + 1 && order.of(exit).layoutIndex == order.of(head).layoutIndex + 1);
    AOT_ORDER_TEST_CHECK(test, test.run(5, 3) == 6 && test.run(5, 9) == 5);
}

// for (;;) { if (index == first) leave; index++ }, and the way out is rare. The loop ends with the test all the same, and the conditional jump goes round the loop.
static void testMachineOrderEndsLoopWithTestThatLeavesRarely()
{
    OrderTest test;
    Output& out = test.out;
    LBasicBlock fails = out.newBlock();
    LBasicBlock enters = out.newBlock();
    LBasicBlock head = out.newBlock();
    LBasicBlock body = out.newBlock();
    LBasicBlock exit = out.newBlock();
    LValue index = out.phiIn(head, Int64);
    out.branch(test.third, rareEdge(fails), normalEdge(enters));
    test.returnConstant(fails, -1);
    test.passAndJump(enters, out.int64Zero, index, head);
    out.appendTo(head);
    out.branch(out.equal(index, test.first), rareEdge(exit), normalEdge(body));
    out.appendTo(body);
    out.upsilon(out.add(index, 1), index);
    out.jump(head);
    out.appendTo(exit);
    out.ret(out.add(index, 100));
    const Order& order = test.compile();

    // The other rare block stands between the test and the way out, so neither way falls through.
    AOT_ORDER_TEST_CHECK(test, orderTestLayoutIs(order, { test.entry, enters, body, head, fails, exit }));
    AOT_ORDER_TEST_CHECK(test, order.successorOfConditionalJump(head) == 1);
    AOT_ORDER_TEST_CHECK(test, test.run(5) == 105 && test.run(0) == 100 && test.run(5, 0, 1) == -1);
}

// for (row = 0;; row++) { for (column = 0; column < second; column++) { if (row + column == third) return row + 100; } if (row + 1 >= first) return row + 1; }
// The block that the inner loop returns through stands behind the outer loop, so the way out of the outer loop does not fall through.
// The outer loop ends with its test all the same, and the conditional jump goes round the loop whichever successor that is.
template<unsigned successorThatStays>
static void testMachineOrderEndsLoopWithTestWhoseWayOutDoesNotFallThrough()
{
    OrderTest test;
    Output& out = test.out;
    LBasicBlock outerHead = out.newBlock();
    LBasicBlock innerHead = out.newBlock();
    LBasicBlock innerBody = out.newBlock();
    LBasicBlock found = out.newBlock();
    LBasicBlock innerLatch = out.newBlock();
    LBasicBlock afterInner = out.newBlock();
    LBasicBlock outerLatch = out.newBlock();
    LBasicBlock done = out.newBlock();
    LValue row = out.phiIn(outerHead, Int64);
    LValue column = out.phiIn(innerHead, Int64);
    out.upsilon(out.int64Zero, row);
    out.jump(outerHead);
    test.passAndJump(outerHead, out.int64Zero, column, innerHead);
    out.appendTo(innerHead);
    out.branch(out.below(column, test.second), normalEdge(innerBody), normalEdge(afterInner));
    out.appendTo(innerBody);
    out.branch(out.equal(out.add(row, column), test.third), normalEdge(found), normalEdge(innerLatch));
    out.appendTo(found);
    out.ret(out.add(row, 100));
    out.appendTo(innerLatch);
    out.upsilon(out.add(column, 1), column);
    out.jump(innerHead);
    out.appendTo(afterInner);
    LValue next = out.add(row, 1);
    if (successorThatStays)
        out.branch(out.aboveOrEqual(next, test.first), normalEdge(done), normalEdge(outerLatch));
    else
        out.branch(out.below(next, test.first), normalEdge(outerLatch), normalEdge(done));
    test.passAndJump(outerLatch, next, row, outerHead);
    test.returnValue(done, next);
    const Order& order = test.compile();

    AOT_ORDER_TEST_CHECK(test, orderTestLayoutIs(order, { test.entry, outerLatch, outerHead, innerBody, innerLatch, innerHead, afterInner, found, done }));
    AOT_ORDER_TEST_CHECK(test, afterInner->successor(successorThatStays).block() == outerLatch && order.successorOfConditionalJump(afterInner) == successorThatStays);
    AOT_ORDER_TEST_CHECK(test, test.run(3, 4, 100) == 3 && test.run(3, 4, 5) == 102 && test.run(3, 4, 0) == 100 && test.run(0, 0, 0) == 1);
}

static void testMachineOrderMakesNestedLoopsNestedIntervals()
{
    OrderTest test;
    Output& out = test.out;
    LBasicBlock outerHead = out.newBlock();
    LBasicBlock innerHead = out.newBlock();
    LBasicBlock innerBody = out.newBlock();
    LBasicBlock outerLatch = out.newBlock();
    LBasicBlock done = out.newBlock();
    LValue row = out.phiIn(outerHead, Int64);
    LValue column = out.phiIn(innerHead, Int64);
    out.upsilon(out.int64Zero, row);
    out.jump(outerHead);
    out.appendTo(outerHead);
    out.upsilon(out.int64Zero, column);
    out.branch(out.below(row, test.first), normalEdge(innerHead), normalEdge(done));
    out.appendTo(innerHead);
    out.branch(out.below(column, test.second), normalEdge(innerBody), normalEdge(outerLatch));
    out.appendTo(innerBody);
    TypedPointer counter = out.address(test.testProcedure.heaps().argumentBuffer, test.third);
    out.store64(out.add(out.load64(counter), 1), counter);
    out.upsilon(out.add(column, 1), column);
    out.jump(innerHead);
    out.appendTo(outerLatch);
    out.upsilon(out.add(row, 1), row);
    out.jump(outerHead);
    test.returnValue(done, row);
    const Order& order = test.compile();

    uint32_t outer = order.of(outerHead).loop;
    uint32_t inner = order.of(innerHead).loop;
    Block* blockOnEdgeIntoInnerLoop = outerHead->successor(0).block();
    AOT_ORDER_TEST_CHECK(test, order.loops().size() == 2 && outer < 2 && inner < 2 && blockOnEdgeIntoInnerLoop != innerHead);
    AOT_ORDER_TEST_CHECK(test, order.loops()[outer].parent == noLoop && order.loops()[inner].parent == outer);
    AOT_ORDER_TEST_CHECK(test, orderTestLoopHoldsExactly(order, inner, { innerHead, innerBody }));
    AOT_ORDER_TEST_CHECK(test, orderTestLoopHoldsExactly(order, outer, { outerHead, blockOnEdgeIntoInnerLoop, innerHead, innerBody, outerLatch }));
    int64_t rounds = 0;
    AOT_ORDER_TEST_CHECK(test, test.run(3, 4, reinterpret_cast<int64_t>(&rounds)) == 3 && rounds == 12);
}

static int64_t orderTestCountDown(bool isInFirst, int64_t left, int64_t step)
{
    for (;; isInFirst = true) {
        if (isInFirst)
            left -= step;
        if (--left <= 0)
            return left;
    }
}

// The step is defined ahead of the cycle and read inside it, so it has to live to the end of the interval whichever way the cycle is entered.
static void testMachineOrderCoversCycleEnteredInTwoPlaces()
{
    OrderTest test;
    Output& out = test.out;
    LBasicBlock firstBlock = out.newBlock();
    LBasicBlock secondBlock = out.newBlock();
    LBasicBlock exit = out.newBlock();
    LValue leftInFirst = out.phiIn(firstBlock, Int64);
    LValue leftInSecond = out.phiIn(secondBlock, Int64);
    out.upsilon(test.second, leftInFirst);
    out.upsilon(test.second, leftInSecond);
    out.branch(test.first, normalEdge(firstBlock), normalEdge(secondBlock));
    out.appendTo(firstBlock);
    out.upsilon(out.sub(leftInFirst, test.third), leftInSecond);
    out.jump(secondBlock);
    out.appendTo(secondBlock);
    LValue left = out.sub(leftInSecond, 1);
    out.upsilon(left, leftInFirst);
    out.branch(out.greaterThan(left, out.int64Zero), normalEdge(firstBlock), normalEdge(exit));
    test.returnValue(exit, left);
    const Order& order = test.compile();

    AOT_ORDER_TEST_CHECK(test, order.loops().size() == 1 && !order.of(firstBlock).loop && !order.of(secondBlock).loop);
    AOT_ORDER_TEST_CHECK(test, order.of(firstBlock).isLoopHead != order.of(secondBlock).isLoopHead);
    AOT_ORDER_TEST_CHECK(test, order.of(test.entry).loop == noLoop && order.of(exit).loop == noLoop);
    for (const Block* block : { firstBlock, secondBlock }) {
        AOT_ORDER_TEST_CHECK(test, std::ranges::any_of(order.of(block).predecessors, [&](const Block* predecessor) {
            return order.of(predecessor).loop == noLoop;
        }));
    }
    for (int64_t entersAtFirst : { 0, 1 }) {
        for (int64_t iterations : { 1, 4, 7 }) {
            for (int64_t step : { 1, 2 })
                AOT_ORDER_TEST_CHECK(test, test.run(entersAtFirst, iterations, step) == orderTestCountDown(entersAtFirst, iterations, step));
        }
    }
}

// As a twin is entered: behind the head of a loop.
static void testMachineOrderStartsAtEveryEntrypoint()
{
    OrderTest test;
    Output& out = test.out;
    LBasicBlock laterEntry = test.procedure().addEntrypoint(1);
    LBasicBlock head = out.newBlock();
    LBasicBlock body = out.newBlock();
    LBasicBlock exit = out.newBlock();
    LValue leftAtHead = out.phiIn(head, Int64);
    LValue leftAtBody = out.phiIn(body, Int64);
    out.upsilon(test.first, leftAtHead);
    out.jump(head);
    out.appendTo(laterEntry);
    out.upsilon(test.testProcedure.argumentGPR(0), leftAtBody);
    out.jump(body);
    out.appendTo(head);
    out.upsilon(leftAtHead, leftAtBody);
    out.branch(leftAtHead, normalEdge(body), normalEdge(exit));
    out.appendTo(body);
    out.upsilon(out.sub(leftAtBody, 1), leftAtHead);
    out.jump(head);
    test.returnValue(exit, leftAtHead);
    const Order& order = test.prepare();

    AOT_ORDER_TEST_CHECK(test, order.of(laterEntry).isReached() && order.of(laterEntry).predecessors.empty());
    AOT_ORDER_TEST_CHECK(test, order.loops().size() == 1 && !order.of(head).loop && !order.of(body).loop);
    AOT_ORDER_TEST_CHECK(test, order.of(head).isLoopHead != order.of(body).isLoopHead);
    AOT_ORDER_TEST_CHECK(test, order.of(test.entry).loop == noLoop && order.of(laterEntry).loop == noLoop && order.of(exit).loop == noLoop);
}

static void testMachineOrderKeepsRareBlockWithinItsLoop()
{
    OrderTest test;
    Output& out = test.out;
    LBasicBlock head = out.newBlock();
    LBasicBlock body = out.newBlock();
    LBasicBlock slow = out.newBlock();
    LBasicBlock latch = out.newBlock();
    LBasicBlock exit = out.newBlock();
    LValue index = out.phiIn(head, Int64);
    LValue step = out.phiIn(latch, Int64);
    out.upsilon(out.int64Zero, index);
    out.jump(head);
    out.appendTo(head);
    out.branch(out.below(index, test.first), normalEdge(body), normalEdge(exit));
    out.appendTo(body);
    out.upsilon(out.constInt64(1), step);
    out.branch(out.equal(index, test.second), rareEdge(slow), normalEdge(latch));
    test.passAndJump(slow, out.constInt64(3), step, latch);
    out.appendTo(latch);
    out.upsilon(out.add(index, step), index);
    out.jump(head);
    test.returnValue(exit, index);
    const Order& order = test.compile();

    AOT_ORDER_TEST_CHECK(test, order.of(slow).isRare() && order.of(slow).orderIndex + 1 == order.of(latch).orderIndex);
    AOT_ORDER_TEST_CHECK(test, order.loops().size() == 1 && order.of(head).isLoopHead && !order.of(slow).loop && order.of(exit).loop == noLoop);
    AOT_ORDER_TEST_CHECK(test, order.blocksInLayout().back() == slow);
    AOT_ORDER_TEST_CHECK(test, test.run(5, 3) == 6 && test.run(5, 9) == 5);
}

static void testMachineOrderCoversRareBlockThatLeadsBack()
{
    OrderTest test;
    Output& out = test.out;
    LBasicBlock often = out.newBlock();
    LBasicBlock seldom = out.newBlock();
    LBasicBlock exit = out.newBlock();
    LValue left = out.phiIn(often, Int64);
    LValue count = out.phiIn(often, Int64);
    out.upsilon(test.first, left);
    out.upsilon(out.int64Zero, count);
    out.jump(often);
    out.appendTo(often);
    out.branch(left, rareEdge(seldom), normalEdge(exit));
    out.appendTo(seldom);
    out.upsilon(out.sub(left, 1), left);
    out.upsilon(out.add(count, 1), count);
    out.jump(often);
    test.returnValue(exit, count);
    const Order& order = test.compile();

    AOT_ORDER_TEST_CHECK(test, order.of(seldom).isRare() && order.of(seldom).entersRareRegion && order.of(seldom).leavesRareRegion);
    AOT_ORDER_TEST_CHECK(test, order.loops().size() == 1 && orderTestLoopHoldsExactly(order, order.of(often).loop, { often, seldom }));
    AOT_ORDER_TEST_CHECK(test, order.blocksInLayout().back() == seldom);
    AOT_ORDER_TEST_CHECK(test, test.run(3) == 3 && !test.run(0));
}

// As a loop over a list is opened: the usual way leads to the head, the other way leads there through a rare block alone.
// The other way stands ahead of the head, so no edge but the one of the loop leads back, and what is live in the loop ends with it.
static void testMachineOrderPlacesBlockAheadOfWhatItLeadsToThroughRareBlocks()
{
    OrderTest test;
    Output& out = test.out;
    LBasicBlock usual = out.newBlock();
    LBasicBlock other = out.newBlock();
    LBasicBlock slow = out.newBlock();
    LBasicBlock fails = out.newBlock();
    LBasicBlock head = out.newBlock();
    LBasicBlock body = out.newBlock();
    LBasicBlock exit = out.newBlock();
    LBasicBlock last = out.newBlock();
    LValue index = out.phiIn(head, Int64);
    out.branch(test.first, unlikelyEdge(other), normalEdge(usual));
    test.passAndJump(usual, out.int64Zero, index, head);
    out.appendTo(other);
    out.branch(test.third, rareEdge(slow), rareEdge(fails));
    test.passAndJump(slow, out.constInt64(2), index, head);
    test.returnConstant(fails, -1);
    out.appendTo(head);
    out.branch(out.below(index, test.second), normalEdge(body), normalEdge(exit));
    out.appendTo(body);
    out.upsilon(out.add(index, 1), index);
    out.jump(head);
    out.appendTo(exit);
    LValue sum = out.add(index, 100);
    out.jump(last);
    test.returnValue(last, sum);
    const Order& order = test.compile();

    AOT_ORDER_TEST_CHECK(test, !order.of(other).isRare() && order.of(slow).isRare() && order.of(slow).leavesRareRegion);
    AOT_ORDER_TEST_CHECK(test, order.of(usual).orderIndex < order.of(other).orderIndex && order.of(other).orderIndex < order.of(slow).orderIndex && order.of(slow).orderIndex < order.of(head).orderIndex);
    AOT_ORDER_TEST_CHECK(test, order.loops().size() == 1 && orderTestLoopHoldsExactly(order, 0, { head, body }));
    AOT_ORDER_TEST_CHECK(test, test.run(0, 5, 0) == 105 && test.run(1, 5, 1) == 105 && test.run(1, 1, 1) == 102 && test.run(1, 5, 0) == -1);
}

// An unlikely arm of a loop goes round through a rare block alone. It is one of the loop, so what stands behind the loop is not in the interval.
static void testMachineOrderKeepsArmThatGoesRoundThroughRareBlockWithinItsLoop()
{
    OrderTest test;
    Output& out = test.out;
    LBasicBlock head = out.newBlock();
    LBasicBlock body = out.newBlock();
    LBasicBlock odd = out.newBlock();
    LBasicBlock slow = out.newBlock();
    LBasicBlock fails = out.newBlock();
    LBasicBlock latch = out.newBlock();
    LBasicBlock exit = out.newBlock();
    LBasicBlock last = out.newBlock();
    LValue index = out.phiIn(head, Int64);
    out.upsilon(out.int64Zero, index);
    out.jump(head);
    out.appendTo(head);
    out.branch(out.aboveOrEqual(index, test.first), normalEdge(exit), normalEdge(body));
    out.appendTo(body);
    out.branch(out.equal(index, test.second), unlikelyEdge(odd), normalEdge(latch));
    out.appendTo(odd);
    out.branch(test.third, rareEdge(slow), rareEdge(fails));
    out.appendTo(slow);
    out.upsilon(out.add(index, 2), index);
    out.jump(head);
    test.returnConstant(fails, -1);
    out.appendTo(latch);
    out.upsilon(out.add(index, 1), index);
    out.jump(head);
    out.appendTo(exit);
    LValue sum = out.add(index, 100);
    out.jump(last);
    test.returnValue(last, sum);
    const Order& order = test.compile();

    AOT_ORDER_TEST_CHECK(test, !order.of(odd).isRare() && order.of(slow).isRare());
    AOT_ORDER_TEST_CHECK(test, order.loops().size() == 1 && orderTestLoopHoldsExactly(order, 0, { head, body, latch, odd, slow }));
    AOT_ORDER_TEST_CHECK(test, order.of(exit).loop == noLoop && order.of(last).loop == noLoop);
    AOT_ORDER_TEST_CHECK(test, test.run(5, 9, 0) == 105 && test.run(5, 4, 1) == 106 && test.run(5, 3, 0) == -1);
}

static void testMachineOrderSplitsCriticalEdge()
{
    for (EdgeClass classOfEdge : { EdgeClass::Normal, EdgeClass::Unlikely, EdgeClass::Rare }) {
        OrderTest test;
        Output& out = test.out;
        LBasicBlock arm = out.newBlock();
        LBasicBlock join = out.newBlock();
        LValue result = out.phiIn(join, Int64);
        out.upsilon(test.second, result);
        out.branch(test.first, normalEdge(arm), Edge(join, classOfEdge));
        out.appendTo(arm);
        out.upsilon(out.add(test.second, 1), result);
        out.jump(join);
        test.returnValue(join, result);
        const Order& order = test.compile();

        Block* middle = test.entry->successor(1).block();
        AOT_ORDER_TEST_CHECK(test, middle != join && middle->size() == 1 && middle->last()->opcode() == Jump && middle->successor(0).block() == join);
        AOT_ORDER_TEST_CHECK(test, test.entry->successor(1).edgeClass() == classOfEdge && middle->successor(0).edgeClass() == classOfEdge);
        AOT_ORDER_TEST_CHECK(test, test.entry->upsilons().empty() && middle->upsilons().size() == 1);
        AOT_ORDER_TEST_CHECK(test, middle->upsilons()[0]->child(0) == test.second && middle->upsilons()[0]->phi() == result);
        AOT_ORDER_TEST_CHECK(test, test.entry->successor(0).block() == arm && arm->successor(0).block() == join);
        AOT_ORDER_TEST_CHECK(test, order.of(middle).isRare() == (classOfEdge == EdgeClass::Rare) && !order.of(join).isRare());
        AOT_ORDER_TEST_CHECK(test, test.run(1, 5) == 6 && test.run(0, 5) == 5);
    }
}

// The join has no Phi, and both ways bring everything in the same registers. So no block stands on the edge from the Branch to the join.
static void testMachineOrderLeavesEdgeThatGetsNoMoveWhole()
{
    for (EdgeClass classOfEdge : { EdgeClass::Normal, EdgeClass::Unlikely }) {
        OrderTest test;
        Output& out = test.out;
        LBasicBlock arm = out.newBlock();
        LBasicBlock join = out.newBlock();
        out.branch(test.first, normalEdge(arm), Edge(join, classOfEdge));
        out.appendTo(arm);
        out.store64(test.second, out.address(test.testProcedure.heaps().argumentBuffer, test.third));
        out.jump(join);
        test.returnValue(join, test.second);
        const Order& order = test.compile();

        AOT_ORDER_TEST_CHECK(test, test.entry->successor(1).block() == join && test.entry->successor(1).edgeClass() == classOfEdge);
        AOT_ORDER_TEST_CHECK(test, Order::isUnsplit(test.entry, order.of(join)) && order.blocksInOrder().size() == 3 && test.procedure().numberOfBlocks() == 3);
        AOT_ORDER_TEST_CHECK(test, order.of(join).predecessors.size() == 2 && order.of(join).predecessors.front() == test.entry && order.of(join).predecessors.back() == arm);
        int64_t word = 0;
        AOT_ORDER_TEST_CHECK(test, test.run(1, 5, reinterpret_cast<int64_t>(&word)) == 5 && word == 5);
        AOT_ORDER_TEST_CHECK(test, test.run(0, 6, reinterpret_cast<int64_t>(&word)) == 6 && word == 5);
    }
}

// Three tests fail to one rare block, which blocks outside rare regions alone lead to. It is where its region is entered, and no block stands on any of the three edges.
static void testMachineOrderLeavesRareEdgesIntoOneRareBlockWhole()
{
    OrderTest test;
    Output& out = test.out;
    LBasicBlock second = out.newBlock();
    LBasicBlock third = out.newBlock();
    LBasicBlock passes = out.newBlock();
    LBasicBlock fails = out.newBlock();
    out.branch(out.below(test.first, out.constInt64(10)), normalEdge(second), rareEdge(fails));
    out.appendTo(second);
    out.branch(out.below(test.second, out.constInt64(10)), normalEdge(third), rareEdge(fails));
    out.appendTo(third);
    out.branch(out.below(test.third, out.constInt64(10)), normalEdge(passes), rareEdge(fails));
    out.appendTo(passes);
    out.ret(out.add(test.first, test.second));
    test.returnConstant(fails, -1);
    const Order& order = test.compile();

    const BlockPlace& place = order.of(fails);
    AOT_ORDER_TEST_CHECK(test, place.isRare() && place.entersRareRegion && place.predecessors.size() == 3 && order.blocksInOrder().size() == 5);
    AOT_ORDER_TEST_CHECK(test, place.predecessors[0] == test.entry && place.predecessors[1] == second && place.predecessors[2] == third);
    // Nothing sinks into a region that is entered over several edges.
    AOT_ORDER_TEST_CHECK(test, !order.rareRegions()[place.rareRegion].onlyEntry);
    AOT_ORDER_TEST_CHECK(test, test.run(1, 2, 3) == 3 && test.run(11, 2, 3) == -1 && test.run(1, 12, 3) == -1 && test.run(1, 2, 13) == -1);
}

// An edge from within the region leads to the rare block too. A frame that covers the region is set up where the region is entered, so each edge from outside has its block.
static void testMachineOrderSplitsRareEdgesIntoBlockThatItsRegionLeadsTo()
{
    OrderTest test;
    Output& out = test.out;
    LBasicBlock second = out.newBlock();
    LBasicBlock passes = out.newBlock();
    LBasicBlock slow = out.newBlock();
    LBasicBlock fails = out.newBlock();
    out.branch(out.below(test.first, out.constInt64(10)), normalEdge(second), rareEdge(fails));
    out.appendTo(second);
    out.branch(out.below(test.second, out.constInt64(10)), normalEdge(passes), rareEdge(slow));
    out.appendTo(passes);
    out.ret(out.add(test.first, test.second));
    out.appendTo(slow);
    out.store64(test.second, out.address(test.testProcedure.heaps().argumentBuffer, test.third));
    out.jump(fails);
    test.returnConstant(fails, -1);
    const Order& order = test.compile();

    Block* middle = test.entry->successor(1).block();
    AOT_ORDER_TEST_CHECK(test, middle != fails && middle->size() == 1 && middle->successor(0).block() == fails && !order.of(middle).wasSplitLate);
    AOT_ORDER_TEST_CHECK(test, order.of(middle).isRare() && order.of(middle).entersRareRegion && order.of(slow).entersRareRegion && !order.of(fails).entersRareRegion);
    int64_t word = 0;
    AOT_ORDER_TEST_CHECK(test, test.run(1, 2, reinterpret_cast<int64_t>(&word)) == 3 && test.run(11, 2, reinterpret_cast<int64_t>(&word)) == -1 && !word);
    AOT_ORDER_TEST_CHECK(test, test.run(1, 12, reinterpret_cast<int64_t>(&word)) == -1 && word == 12);
}

// One Upsilon serves both edges until the second of them has a block of its own.
static void testMachineOrderSplitsEachOfTwoEdgesToOneBlock()
{
    OrderTest test;
    Output& out = test.out;
    LBasicBlock same = out.newBlock();
    LBasicBlock otherwise = out.newBlock();
    LValue result = out.phiIn(same, Int64);
    out.upsilon(test.second, result);
    SwitchCase cases[] = { { 0, normalEdge(same) }, { 1, normalEdge(same) } };
    out.switchOn(test.first, cases, normalEdge(otherwise));
    test.returnValue(same, result);
    test.returnConstant(otherwise, 20);
    const Order& order = test.compile();

    Block* firstMiddle = test.entry->successor(0).block();
    Block* secondMiddle = test.entry->successor(1).block();
    AOT_ORDER_TEST_CHECK(test, firstMiddle != secondMiddle && test.entry->upsilons().empty() && order.of(same).predecessors.size() == 2);
    for (const Block* middle : { firstMiddle, secondMiddle }) {
        AOT_ORDER_TEST_CHECK(test, middle != same && middle->successor(0).block() == same && middle->upsilons().size() == 1);
        AOT_ORDER_TEST_CHECK(test, middle->upsilons()[0]->child(0) == test.second && middle->upsilons()[0]->phi() == result);
    }
    AOT_ORDER_TEST_CHECK(test, test.run(0, 7) == 7 && test.run(1, 7) == 7 && test.run(2, 7) == 20);
}

// No Phi asks for a move. Each edge gets a block of its own as long as another edge of the Switch leads to the same block, so the last of them stays whole.
static void testMachineOrderLeavesLastOfSeveralEdgesOfSwitchToOneBlockWhole()
{
    OrderTest test;
    Output& out = test.out;
    LBasicBlock same = out.newBlock();
    LBasicBlock alone = out.newBlock();
    LBasicBlock otherwise = out.newBlock();
    SwitchCase cases[] = { { 0, normalEdge(same) }, { 1, normalEdge(alone) }, { 2, normalEdge(same) }, { 3, normalEdge(same) } };
    out.switchOn(test.first, cases, normalEdge(otherwise));
    out.appendTo(same);
    out.ret(out.add(test.second, test.third));
    out.appendTo(alone);
    out.store64(test.second, out.address(test.testProcedure.heaps().argumentBuffer, test.third));
    out.jump(same);
    test.returnConstant(otherwise, 20);
    const Order& order = test.prepare();

    for (unsigned successorIndex : { 0u, 2u }) {
        const Block* middle = test.entry->successor(successorIndex).block();
        AOT_ORDER_TEST_CHECK(test, middle != same && middle->size() == 1 && middle->successor(0).block() == same && !order.of(middle).wasSplitLate);
    }
    AOT_ORDER_TEST_CHECK(test, test.entry->successor(0).block() != test.entry->successor(2).block() && test.entry->successor(1).block() == alone);
    AOT_ORDER_TEST_CHECK(test, test.entry->successor(3).block() == same && Order::isUnsplit(test.entry, order.of(same)) && order.of(same).predecessors.size() == 4);
}

// A table with many rows that come to a few answers.
static std::optional<uint64_t> orderTestInstructionsForSwitchWith(unsigned numberOfCases)
{
    constexpr unsigned numberOfAnswers = 3;
    OrderTest test;
    Output& out = test.out;
    LBasicBlock answers[numberOfAnswers];
    for (LBasicBlock& answer : answers)
        answer = out.newBlock();
    LBasicBlock otherwise = out.newBlock();
    Vector<SwitchCase> cases;
    for (unsigned number = 0; number < numberOfCases; ++number)
        cases.append(SwitchCase { static_cast<int64_t>(number), normalEdge(answers[number % numberOfAnswers]) });
    out.switchOn(test.first, cases.span(), normalEdge(otherwise));
    for (unsigned answer = 0; answer < numberOfAnswers; ++answer) {
        out.appendTo(answers[answer]);
        out.ret(out.add(test.second, out.constInt64(answer)));
    }
    test.returnConstant(otherwise, -1);
    // Not OrderTest::prepare(): what it checks of every order goes through the predecessors of a block for each edge into it.
    std::optional<uint64_t> instructions = instructionsOfCurrentThreadIn([&] {
        prepareOrFail(test.testProcedure);
    });

    const Order& order = test.procedure().order();
    AOT_TEST_CHECK(order.blocksInOrder().size() == 1 + numberOfCases + 1);
    for (unsigned answer = 0; answer < numberOfAnswers; ++answer) {
        AOT_TEST_CHECK(order.of(answers[answer]).predecessors.size() == (numberOfCases + numberOfAnswers - 1 - answer) / numberOfAnswers);
        AOT_TEST_CHECK(Order::isUnsplit(test.entry, order.of(answers[answer])));
        AOT_TEST_CHECK(test.entry->successor(numberOfCases - numberOfAnswers + (answer + numberOfAnswers - numberOfCases % numberOfAnswers) % numberOfAnswers).block() == answers[answer]);
    }
    return instructions;
}

// Each edge costs the same however many the Switch has. Where its edges are gone through for each of them, the work is the square of their number.
static void testMachineOrderGrowsAsEdgesOfSwitchDo()
{
    constexpr unsigned numberOfCases = 5000;
    std::optional<uint64_t> small = orderTestInstructionsForSwitchWith(numberOfCases);
    std::optional<uint64_t> large = orderTestInstructionsForSwitchWith(4 * numberOfCases);
    AOT_TEST_CHECK(growsAsSizeDoes(small, large));
}

// anchor(value), then a Branch whose other way never rejoins. The Phi asks for a move though one edge alone leads to its block.
static void testMachineOrderSplitsEdgeToBlockWithPhisAndOnePredecessor()
{
    for (bool handsOnTestedValue : { false, true }) {
        OrderTest test;
        Output& out = test.out;
        LBasicBlock continuation = out.newBlock();
        LBasicBlock fails = out.newBlock();
        LValue handedOn = handsOnTestedValue ? test.first : test.second;
        LValue result = out.phiIn(continuation, Int64);
        out.upsilon(handedOn, result);
        out.branch(test.first, normalEdge(continuation), rareEdge(fails));
        test.returnValue(continuation, result);
        test.returnConstant(fails, -1);
        const Order& order = test.compile();

        Block* middle = test.entry->successor(0).block();
        AOT_ORDER_TEST_CHECK(test, middle != continuation && middle->size() == 1 && middle->last()->opcode() == Jump && middle->successor(0) == normalEdge(continuation));
        AOT_ORDER_TEST_CHECK(test, test.entry->upsilons().empty() && middle->upsilons().size() == 1);
        AOT_ORDER_TEST_CHECK(test, middle->upsilons()[0]->child(0) == handedOn && middle->upsilons()[0]->phi() == result);
        AOT_ORDER_TEST_CHECK(test, order.of(continuation).predecessors.size() == 1 && test.entry->successor(1).block() == fails);
        AOT_ORDER_TEST_CHECK(test, test.run(3, 5) == (handsOnTestedValue ? 3 : 5) && test.run(0, 5) == -1);
    }
}

static ProcedureTraits orderTestTraitsWithHandlers()
{
    ProcedureTraits traits;
    traits.hasHandlers = true;
    return traits;
}

// The tests that use it only prepare: its code never runs.
static PatchGenerator* orderTestCreateCallGenerator(OrderTest& test)
{
    return test.procedure().createGenerator([](CCallHelpers&, const PatchParameters&) { });
}

// Ends the block that Output stands in.
static void orderTestAppendThrowingCall(Output& out, PatchGenerator* generator, LBasicBlock continuation, LBasicBlock landingPad)
{
    PatchOperation* call = out.patchpoint(Void);
    call->setGenerator(generator);
    out.appendSuccessor(normalEdge(continuation));
    out.appendSuccessor(rareEdge(landingPad));
    call->clobberLateOnSuccessor(1, RegisterSet());
    call->effects.terminal = true;
}

static void testMachineOrderLeavesEdgesToLandingPadWhole()
{
    OrderTest test(orderTestTraitsWithHandlers());
    Output& out = test.out;
    LBasicBlock next = out.newBlock();
    LBasicBlock done = out.newBlock();
    LBasicBlock pad = out.newBlock();
    PatchGenerator* generator = orderTestCreateCallGenerator(test);
    orderTestAppendThrowingCall(out, generator, next, pad);
    out.appendTo(next);
    orderTestAppendThrowingCall(out, generator, done, pad);
    test.returnValue(done, test.first);
    test.returnConstant(pad, -1);
    const Order& order = test.prepare();

    AOT_ORDER_TEST_CHECK(test, pad->isLandingPad() && test.entry->successor(1).block() == pad && next->successor(1).block() == pad);
    AOT_ORDER_TEST_CHECK(test, order.of(pad).predecessors.size() == 2 && order.of(pad).isRare() && order.of(pad).entersRareRegion);
    AOT_ORDER_TEST_CHECK(test, test.procedure().numberOfBlocks() == 4);
}

// The Patch has one way out, which leads to no landing pad. Where it stands in a rare region, the block on the edge is the one that leaves the region.
static void testMachineOrderSplitsEdgeOfPatchToJoin()
{
    for (EdgeClass classOfWayToCall : { EdgeClass::Normal, EdgeClass::Rare }) {
        bool isRare = classOfWayToCall == EdgeClass::Rare;
        OrderTest test;
        Output& out = test.out;
        LBasicBlock calls = out.newBlock();
        LBasicBlock other = out.newBlock();
        LBasicBlock join = out.newBlock();
        LValue result = out.phiIn(join, Int64);
        PatchGenerator* generator = orderTestCreateCallGenerator(test);
        out.branch(test.first, Edge(calls, classOfWayToCall), normalEdge(other));
        out.appendTo(calls);
        out.upsilon(test.second, result);
        PatchOperation* call = out.patchpoint(Void);
        call->setGenerator(generator);
        out.appendSuccessor(normalEdge(join));
        call->effects.terminal = true;
        test.passAndJump(other, test.third, result, join);
        test.returnValue(join, result);
        const Order& order = test.prepare();

        Block* middle = calls->successor(0).block();
        AOT_ORDER_TEST_CHECK(test, middle != join && middle->size() == 1 && middle->last()->opcode() == Jump && middle->successor(0) == normalEdge(join));
        AOT_ORDER_TEST_CHECK(test, calls->upsilons().empty() && middle->upsilons().size() == 1);
        AOT_ORDER_TEST_CHECK(test, middle->upsilons()[0]->child(0) == test.second && middle->upsilons()[0]->phi() == result);
        AOT_ORDER_TEST_CHECK(test, other->successor(0).block() == join && order.of(join).predecessors.size() == 2);
        AOT_ORDER_TEST_CHECK(test, order.of(calls).isRare() == isRare && order.of(middle).isRare() == isRare && !order.of(join).isRare());
        AOT_ORDER_TEST_CHECK(test, !order.of(calls).leavesRareRegion && order.of(middle).leavesRareRegion == isRare);
    }
}

// The allocator looks for the Extracts at the head of the first successor. In a block on the edge it would find none.
static void testMachineOrderLeavesEdgeThatHoldsExtractsWhole()
{
    OrderTest test(orderTestTraitsWithHandlers());
    Output& out = test.out;
    LBasicBlock continuation = out.newBlock();
    LBasicBlock pad = out.newBlock();
    LValue handedOn = out.phiIn(continuation, Int64);
    PatchGenerator* generator = orderTestCreateCallGenerator(test);
    out.upsilon(test.first, handedOn);
    PatchOperation* call = out.patchpoint(test.procedure().addTuple({ Int64, Int64 }));
    call->resultConstraints = { ValueRep::SomeRegister, ValueRep::SomeRegister };
    call->setGenerator(generator);
    out.appendSuccessor(normalEdge(continuation));
    out.appendSuccessor(rareEdge(pad));
    call->clobberLateOnSuccessor(1, RegisterSet());
    call->effects.terminal = true;
    out.appendTo(continuation);
    LValue firstMember = out.extract(call, 0);
    LValue secondMember = out.extract(call, 1);
    out.ret(out.add(out.add(firstMember, secondMember), handedOn));
    test.returnConstant(pad, -1);
    const Order& order = test.prepare();

    AOT_ORDER_TEST_CHECK(test, test.entry->successor(0).block() == continuation && order.of(continuation).predecessors.size() == 1);
    AOT_ORDER_TEST_CHECK(test, test.entry->upsilons().size() == 1 && test.entry->upsilons()[0]->phi() == handedOn);
    AOT_ORDER_TEST_CHECK(test, continuation->at(0) == firstMember && continuation->at(1) == secondMember);
    AOT_ORDER_TEST_CHECK(test, test.procedure().numberOfBlocks() == 3);
}

// if (condition) call(), with the call on a slow arm. Behind the call the arm jumps to the rejoin, where Output stands afterwards.
struct OrderTestThrowingArm {
    OrderTestThrowingArm(OrderTest& test, LValue condition, PatchGenerator* generator, LBasicBlock landingPad)
        : call(test.out.newBlock())
        , continuation(test.out.newBlock())
        , rejoin(test.out.newBlock())
    {
        Output& out = test.out;
        out.branch(condition, rareEdge(call), normalEdge(rejoin));
        out.appendTo(call);
        orderTestAppendThrowingCall(out, generator, continuation, landingPad);
        out.appendTo(continuation);
        out.jump(rejoin);
        out.appendTo(rejoin);
    }

    bool isRareRegion(const Order& order, uint32_t region) const
    {
        const BlockPlace& first = order.of(call);
        const BlockPlace& last = order.of(continuation);
        return first.rareRegion == region && last.rareRegion == region && first.entersRareRegion && last.leavesRareRegion
            && first.orderIndex + 1 == last.orderIndex && order.rareRegions()[region].onlyEntry == call;
    }

    LBasicBlock call;
    LBasicBlock continuation;
    LBasicBlock rejoin;
};

enum class OrderTestCallBehindArms : bool { No, Yes };

// Two arms in a row. Every call leads to the same landing pad.
struct OrderTestTwoThrowingArms {
    OrderTestTwoThrowingArms(OrderTest& test, OrderTestCallBehindArms callBehindArms)
        : pad(test.out.newBlock())
        , generator(orderTestCreateCallGenerator(test))
        , first(test, test.first, generator, pad)
        , second(test, test.second, generator, pad)
    {
        Output& out = test.out;
        if (callBehindArms == OrderTestCallBehindArms::Yes) {
            LBasicBlock done = out.newBlock();
            orderTestAppendThrowingCall(out, generator, done, pad);
            out.appendTo(done);
        }
        out.ret(out.constInt64(7));
        test.returnConstant(pad, -1);
    }

    LBasicBlock pad;
    PatchGenerator* generator;
    OrderTestThrowingArm first;
    OrderTestThrowingArm second;
};

static void testMachineOrderGivesEachThrowingArmItsOwnRegion()
{
    OrderTest test(orderTestTraitsWithHandlers());
    OrderTestTwoThrowingArms arms(test, OrderTestCallBehindArms::Yes);
    const Order& order = test.prepare();

    const BlockPlace& pad = order.of(arms.pad);
    AOT_ORDER_TEST_CHECK(test, order.rareRegions().size() == 3 && order.loops().empty());
    AOT_ORDER_TEST_CHECK(test, arms.first.isRareRegion(order, 0) && order.of(arms.first.continuation).orderIndex + 1 == order.of(arms.first.rejoin).orderIndex);
    AOT_ORDER_TEST_CHECK(test, arms.second.isRareRegion(order, 1) && order.of(arms.second.continuation).orderIndex + 1 == order.of(arms.second.rejoin).orderIndex);
    AOT_ORDER_TEST_CHECK(test, pad.rareRegion == 2 && pad.entersRareRegion && order.rareRegions()[2].onlyEntry == arms.pad && pad.predecessors.size() == 3);
    AOT_ORDER_TEST_CHECK(test, pad.orderIndex == order.of(arms.second.rejoin).orderIndex + 1);
}

// No block outside the rare regions leads to the landing pad.
static void testMachineOrderPlacesLandingPadBehindLastThrowingArm()
{
    OrderTest test(orderTestTraitsWithHandlers());
    OrderTestTwoThrowingArms arms(test, OrderTestCallBehindArms::No);
    const Order& order = test.prepare();

    const BlockPlace& pad = order.of(arms.pad);
    AOT_ORDER_TEST_CHECK(test, order.rareRegions().size() == 3 && order.loops().empty());
    AOT_ORDER_TEST_CHECK(test, arms.first.isRareRegion(order, 0) && arms.second.isRareRegion(order, 1));
    AOT_ORDER_TEST_CHECK(test, pad.rareRegion == 2 && pad.entersRareRegion && order.rareRegions()[2].onlyEntry == arms.pad && pad.predecessors.size() == 2);
    AOT_ORDER_TEST_CHECK(test, order.of(arms.second.continuation).orderIndex + 1 == pad.orderIndex && pad.orderIndex + 1 == order.of(arms.second.rejoin).orderIndex);
}

// try { try { call(); } catch { call(); } call(); } catch { }
static void testMachineOrderPlacesNestedHandlerAheadOfItsRejoin()
{
    OrderTest test(orderTestTraitsWithHandlers());
    Output& out = test.out;
    LBasicBlock returned = out.newBlock();
    LBasicBlock innerPad = out.newBlock();
    LBasicBlock handled = out.newBlock();
    LBasicBlock rejoin = out.newBlock();
    LBasicBlock done = out.newBlock();
    LBasicBlock outerPad = out.newBlock();
    PatchGenerator* generator = orderTestCreateCallGenerator(test);
    orderTestAppendThrowingCall(out, generator, returned, innerPad);
    out.appendTo(returned);
    out.jump(rejoin);
    out.appendTo(innerPad);
    orderTestAppendThrowingCall(out, generator, handled, outerPad);
    out.appendTo(handled);
    out.jump(rejoin);
    out.appendTo(rejoin);
    orderTestAppendThrowingCall(out, generator, done, outerPad);
    test.returnConstant(done, 7);
    test.returnConstant(outerPad, -1);
    const Order& order = test.prepare();

    AOT_ORDER_TEST_CHECK(test, innerPad->isLandingPad() && outerPad->isLandingPad() && order.rareRegions().size() == 2 && order.loops().empty());
    AOT_ORDER_TEST_CHECK(test, !order.of(innerPad).rareRegion && !order.of(handled).rareRegion && order.rareRegions()[0].onlyEntry == innerPad);
    AOT_ORDER_TEST_CHECK(test, order.of(outerPad).rareRegion == 1 && order.rareRegions()[1].onlyEntry == outerPad && order.of(outerPad).predecessors.size() == 2);
    AOT_ORDER_TEST_CHECK(test, order.of(innerPad).orderIndex + 1 == order.of(handled).orderIndex && order.of(handled).orderIndex + 1 == order.of(rejoin).orderIndex);
    AOT_ORDER_TEST_CHECK(test, order.of(outerPad).orderIndex == order.of(rejoin).orderIndex + 1);
}

// The second landing pad leads back to the call that leads to the first one. As two regions, each would have to stand behind the other.
static void testMachineOrderJoinsLandingPadsThatLeadIntoEachOther()
{
    OrderTest test(orderTestTraitsWithHandlers());
    Output& out = test.out;
    LBasicBlock usual = out.newBlock();
    LBasicBlock head = out.newBlock();
    LBasicBlock returned = out.newBlock();
    LBasicBlock firstPad = out.newBlock();
    LBasicBlock handled = out.newBlock();
    LBasicBlock secondPad = out.newBlock();
    PatchGenerator* generator = orderTestCreateCallGenerator(test);
    out.branch(test.first, rareEdge(head), normalEdge(usual));
    test.returnConstant(usual, 7);
    out.appendTo(head);
    orderTestAppendThrowingCall(out, generator, returned, firstPad);
    test.returnConstant(returned, 10);
    out.appendTo(firstPad);
    orderTestAppendThrowingCall(out, generator, handled, secondPad);
    test.returnConstant(handled, 20);
    out.appendTo(secondPad);
    out.jump(head);
    const Order& order = test.prepare();

    Block* blockOnEdgeIntoRegion = test.entry->successor(0).block();
    AOT_ORDER_TEST_CHECK(test, firstPad->isLandingPad() && secondPad->isLandingPad() && blockOnEdgeIntoRegion != head);
    AOT_ORDER_TEST_CHECK(test, order.rareRegions().size() == 1 && order.rareRegions()[0].onlyEntry == blockOnEdgeIntoRegion);
    for (const Block* block : { head, returned, firstPad, handled, secondPad })
        AOT_ORDER_TEST_CHECK(test, !order.of(block).rareRegion && !order.of(block).entersRareRegion);
    AOT_ORDER_TEST_CHECK(test, order.loops().size() == 1 && orderTestLoopHoldsExactly(order, order.of(head).loop, { head, firstPad, secondPad }));
}

// An edge between two regions would have to set a frame up or tear one down. The one region stands behind the last call, so each arm leads back to its rejoin.
static void testMachineOrderJoinsThrowingArmsWithoutFrameAtEntry()
{
    OrderTest test;
    OrderTestTwoThrowingArms arms(test, OrderTestCallBehindArms::Yes);
    const Order& order = test.prepare();

    AOT_ORDER_TEST_CHECK(test, order.rareRegions().size() == 1 && !order.rareRegions()[0].onlyEntry);
    for (const Block* block : { arms.first.call, arms.first.continuation, arms.second.call, arms.second.continuation, arms.pad })
        AOT_ORDER_TEST_CHECK(test, !order.of(block).rareRegion && order.of(block).orderIndex > order.of(arms.second.rejoin).orderIndex);
    AOT_ORDER_TEST_CHECK(test, order.loops().size() == 2 && order.of(arms.first.rejoin).isLoopHead && order.of(arms.second.rejoin).isLoopHead);
}

static void testMachineOrderDropsBlocksThatNothingReaches()
{
    OrderTest test;
    Output& out = test.out;
    LBasicBlock taken = out.newBlock();
    LBasicBlock notTaken = out.newBlock();
    LBasicBlock join = out.newBlock();
    LBasicBlock neverFilled = out.newBlock();
    LValue result = out.phiIn(join, Int64);
    out.branch(out.booleanTrue, normalEdge(taken), normalEdge(notTaken));
    test.passAndJump(taken, test.first, result, join);
    test.passAndJump(notTaken, test.second, result, join);
    test.returnValue(join, result);
    const Order& order = test.compile();

    AOT_ORDER_TEST_CHECK(test, !order.of(notTaken).isReached() && !order.of(neverFilled).isReached());
    AOT_ORDER_TEST_CHECK(test, join->numberOfIncomingEdges() == 2 && order.of(join).predecessors.size() == 1);
    AOT_ORDER_TEST_CHECK(test, test.printedOrder() ==
        "#0: head = 0\n"
        "#1: head = 10 <- #0\n"
        "#3: head = 14 <- #1\n"
        "layout: #0 #1 #3\n"_s);
    AOT_ORDER_TEST_CHECK(test, test.run(5, 6) == 5);
}

// What PrimitiveEmitter::asBoolean() makes of a comparison of the first two arguments. Output stands in the join, which holds the Phi alone so far.
struct OrderTestBooleanJoin {
    explicit OrderTestBooleanJoin(OrderTest& test)
        : OrderTestBooleanJoin(test, test.out.below(test.first, test.second), EdgeClass::Normal)
    {
    }

    // Of any other value, tested where Output stands.
    OrderTestBooleanJoin(OrderTest& test, LValue tested, EdgeClass classOfFalseWay)
        : taken(test.out.newBlock())
        , notTaken(test.out.newBlock())
        , join(test.out.newBlock())
        , condition(test.out.phiIn(join, Int32))
    {
        Output& out = test.out;
        out.branch(tested, normalEdge(taken), Edge(notTaken, classOfFalseWay));
        test.passAndJump(taken, out.booleanTrue, condition, join);
        test.passAndJump(notTaken, out.booleanFalse, condition, join);
        out.appendTo(join);
    }

    bool isKept(const Order& order) const
    {
        return taken->successor(0).block() == join && notTaken->successor(0).block() == join && order.of(join).predecessors.size() == 2;
    }

    LBasicBlock taken;
    LBasicBlock notTaken;
    LBasicBlock join;
    LValue condition;
};

// The arms are made as ifElse() makes them: each as the continuation of the join alone, which an edge that passes the join by then enters too.
static void testMachineThreadingResolvesBranchOnJoinOfConstants()
{
    for (EdgeClass classOfFalseWay : { EdgeClass::Normal, EdgeClass::Unlikely, EdgeClass::Rare }) {
        OrderTest test;
        Output& out = test.out;
        OrderTestBooleanJoin blocks(test);
        LBasicBlock whenTrue = out.newBlock();
        LBasicBlock whenFalse = out.newBlock();
        ScopeFork fork = out.forkScope();
        out.branch(blocks.condition, normalEdge(whenTrue), Edge(whenFalse, classOfFalseWay));
        out.appendToArm(whenTrue, fork);
        out.ret(out.constInt64(10));
        out.appendToArm(whenFalse, fork);
        out.ret(out.constInt64(20));
        const Order& order = test.compile();

        AOT_ORDER_TEST_CHECK(test, orderTestTargetOf(test.entry, 0) == whenTrue && orderTestTargetOf(test.entry, 1) == whenFalse);
        AOT_ORDER_TEST_CHECK(test, !order.of(blocks.join).isReached() && orderTestNumberOfReached(order, Branch) == 1);
        AOT_ORDER_TEST_CHECK(test, !order.of(whenTrue).isRare() && order.of(whenFalse).isRare() == (classOfFalseWay == EdgeClass::Rare));
        AOT_ORDER_TEST_CHECK(test, test.run(1, 2) == 10 && test.run(2, 1) == 20);
    }
}

// asBoolean() of a constant, then ifElse() with a rare way for false. Nothing reaches the block that passes true, so the rare way is the only path that runs.
// With a second join the arms of the first Branch pass it constants in turn, and the arm that passes true is no longer reached either.
template<unsigned numberOfJoins>
static void testMachineThreadingKeepsClassOfOnlyReachedEdge()
{
    OrderTest test;
    Output& out = test.out;
    Vector<OrderTestBooleanJoin> joins;
    joins.append(OrderTestBooleanJoin(test, out.booleanFalse, EdgeClass::Normal));
    while (joins.size() < numberOfJoins)
        joins.append(OrderTestBooleanJoin(test, joins.last().condition, EdgeClass::Rare));
    LBasicBlock usual = out.newBlock();
    LBasicBlock slow = out.newBlock();
    ScopeFork fork = out.forkScope();
    out.branch(joins.last().condition, normalEdge(usual), rareEdge(slow));
    out.appendToArm(usual, fork);
    out.ret(out.constInt64(10));
    out.appendToArm(slow, fork);
    out.ret(out.constInt64(20));
    const Order& order = test.compile();

    AOT_ORDER_TEST_CHECK(test, test.entry->successor(0) == normalEdge(slow) && order.rareRegions().empty() && !order.of(usual).isReached());
    for (const OrderTestBooleanJoin& blocks : joins)
        AOT_ORDER_TEST_CHECK(test, !order.of(blocks.join).isReached() && blocks.taken->successor(0).block() == blocks.join);
    AOT_ORDER_TEST_CHECK(test, test.run(0) == 20);
}

// The block that passes no constant ends in a Jump, so it tests what it passes in the place of the join.
static void testMachineThreadingTestsValueAheadOfJoin()
{
    OrderTest test;
    Output& out = test.out;
    LBasicBlock left = out.newBlock();
    LBasicBlock right = out.newBlock();
    LBasicBlock join = out.newBlock();
    LBasicBlock whenNotZero = out.newBlock();
    LBasicBlock whenZero = out.newBlock();
    LValue tested = out.phiIn(join, Int64);
    out.branch(test.first, normalEdge(left), normalEdge(right));
    test.passAndJump(left, out.int64Zero, tested, join);
    test.passAndJump(right, test.second, tested, join);
    out.appendTo(join);
    out.branch(tested, normalEdge(whenNotZero), normalEdge(whenZero));
    test.returnConstant(whenNotZero, 10);
    test.returnConstant(whenZero, 20);
    const Order& order = test.compile();

    AOT_ORDER_TEST_CHECK(test, orderTestTargetOf(test.entry, 0) == whenZero && !order.of(join).isReached());
    AOT_ORDER_TEST_CHECK(test, right->last()->opcode() == Branch && right->last()->child(0) == test.second && right->upsilons().empty());
    AOT_ORDER_TEST_CHECK(test, orderTestTargetOf(right, 0) == whenNotZero && orderTestTargetOf(right, 1) == whenZero);
    AOT_ORDER_TEST_CHECK(test, test.run(1, 1) == 20 && test.run(0, 1) == 10 && test.run(0, 0) == 20);
}

// c ? a < b : a == b, then a Branch on the answer. Each arm compares and branches, and no answer is made a word. The join tests for zero, so the ways change places.
static void testMachineThreadingFusesComparisonsAheadOfJoin()
{
    OrderTest test;
    Output& out = test.out;
    LBasicBlock left = out.newBlock();
    LBasicBlock right = out.newBlock();
    LBasicBlock join = out.newBlock();
    LBasicBlock whenFalse = out.newBlock();
    LBasicBlock whenTrue = out.newBlock();
    LValue answer = out.phiIn(join, Int32);
    out.branch(test.third, normalEdge(left), normalEdge(right));
    out.appendTo(left);
    LValue isBelow = out.below(test.first, test.second);
    out.upsilon(isBelow, answer);
    out.jump(join);
    out.appendTo(right);
    LValue isEqual = out.equal(test.first, test.second);
    out.upsilon(isEqual, answer);
    out.jump(join);
    out.appendTo(join);
    out.branch(out.isZero32(answer), normalEdge(whenFalse), rareEdge(whenTrue));
    test.returnConstant(whenFalse, 20);
    test.returnConstant(whenTrue, 10);
    const Order& order = test.compile();

    const Liveness& liveness = test.procedure().liveness();
    AOT_ORDER_TEST_CHECK(test, !order.of(join).isReached() && orderTestNumberOfReached(order, Branch) == 3 && order.of(whenTrue).isRare());
    AOT_ORDER_TEST_CHECK(test, isBelow->useCount() == 1 && isEqual->useCount() == 1 && liveness.isFused(isBelow) && liveness.isFused(isEqual));
    for (const Block* arm : { left, right })
        AOT_ORDER_TEST_CHECK(test, orderTestTargetOf(arm, 0) == whenTrue && arm->successor(0).isRare() && orderTestTargetOf(arm, 1) == whenFalse && !arm->successor(1).isRare());
    AOT_ORDER_TEST_CHECK(test, test.run(1, 2, 1) == 10 && test.run(2, 1, 1) == 20 && test.run(2, 2, 0) == 10 && test.run(1, 2, 0) == 20);
}

// A Branch has no place for a second test, so its edge goes on to the join.
static void testMachineThreadingLeavesEdgeOfBranchThatPassesNoConstant()
{
    OrderTest test;
    Output& out = test.out;
    LBasicBlock other = out.newBlock();
    LBasicBlock join = out.newBlock();
    LBasicBlock whenNotZero = out.newBlock();
    LBasicBlock whenZero = out.newBlock();
    LValue tested = out.phiIn(join, Int64);
    out.upsilon(test.second, tested);
    out.branch(test.first, normalEdge(join), normalEdge(other));
    test.passAndJump(other, out.int64Zero, tested, join);
    out.appendTo(join);
    out.branch(tested, normalEdge(whenNotZero), normalEdge(whenZero));
    test.returnConstant(whenNotZero, 10);
    test.returnConstant(whenZero, 20);
    const Order& order = test.compile();

    AOT_ORDER_TEST_CHECK(test, orderTestTargetOf(test.entry, 1) == whenZero && order.of(join).isReached() && order.of(join).predecessors.size() == 1);
    AOT_ORDER_TEST_CHECK(test, orderTestTargetOf(join, 0) == whenNotZero && orderTestTargetOf(join, 1) == whenZero);
    AOT_ORDER_TEST_CHECK(test, test.run(0, 1) == 20 && test.run(1, 1) == 10 && test.run(1, 0) == 20);
}

// c && a < b: the answer of the comparison is made a boolean, handed on to a second join, which another block passes false, and tested there.
// The blocks that pass the first join its constants come ahead of it, and go to the arms of the second all the same. No boolean is made.
static void testMachineThreadingPassesJoinThatHandsItsPhiOn()
{
    OrderTest test;
    Output& out = test.out;
    LBasicBlock asks = out.newBlock();
    LBasicBlock other = out.newBlock();
    out.branch(test.third, normalEdge(asks), normalEdge(other));
    out.appendTo(asks);
    OrderTestBooleanJoin inner(test);
    LBasicBlock outerJoin = out.newBlock();
    LBasicBlock whenTrue = out.newBlock();
    LBasicBlock whenFalse = out.newBlock();
    LValue answer = out.phiIn(outerJoin, Int32);
    out.upsilon(inner.condition, answer);
    out.jump(outerJoin);
    test.passAndJump(other, out.booleanFalse, answer, outerJoin);
    out.appendTo(outerJoin);
    out.branch(answer, normalEdge(whenTrue), normalEdge(whenFalse));
    test.returnConstant(whenTrue, 10);
    test.returnConstant(whenFalse, 20);
    const Order& order = test.compile();

    AOT_ORDER_TEST_CHECK(test, inner.taken->index() < inner.join->index() && inner.notTaken->index() < inner.join->index());
    AOT_ORDER_TEST_CHECK(test, !order.of(inner.join).isReached() && !order.of(outerJoin).isReached() && orderTestNumberOfReached(order, Branch) == 2);
    AOT_ORDER_TEST_CHECK(test, orderTestTargetOf(asks, 0) == whenTrue && orderTestTargetOf(asks, 1) == whenFalse && orderTestTargetOf(test.entry, 1) == whenFalse);
    AOT_ORDER_TEST_CHECK(test, test.run(1, 2, 1) == 10 && test.run(2, 1, 1) == 20 && test.run(1, 2, 0) == 20);
}

static void testMachineThreadingKeepsJoinWhosePhiIsReadAgain()
{
    OrderTest test;
    Output& out = test.out;
    OrderTestBooleanJoin blocks(test);
    LBasicBlock whenTrue = out.newBlock();
    LBasicBlock whenFalse = out.newBlock();
    out.branch(blocks.condition, normalEdge(whenTrue), normalEdge(whenFalse));
    out.appendTo(whenTrue);
    out.ret(out.zeroExtPtr(blocks.condition));
    test.returnConstant(whenFalse, 20);
    const Order& order = test.compile();

    AOT_ORDER_TEST_CHECK(test, blocks.isKept(order) && orderTestNumberOfReached(order, Branch) == 2);
    AOT_ORDER_TEST_CHECK(test, test.run(1, 2) == 1 && test.run(2, 1) == 20);
}

// An edge that passed the join by would bring the Phi behind it no value.
static void testMachineThreadingKeepsJoinThatHoldsUpsilon()
{
    OrderTest test;
    Output& out = test.out;
    OrderTestBooleanJoin blocks(test);
    LBasicBlock other = out.newBlock();
    LBasicBlock last = out.newBlock();
    LValue result = out.phiIn(last, Int64);
    out.upsilon(test.third, result);
    out.branch(blocks.condition, normalEdge(last), normalEdge(other));
    test.passAndJump(other, out.constInt64(20), result, last);
    test.returnValue(last, result);
    const Order& order = test.compile();

    AOT_ORDER_TEST_CHECK(test, blocks.isKept(order));
    AOT_ORDER_TEST_CHECK(test, test.run(1, 2, 5) == 5 && test.run(2, 1, 5) == 20);
}

static void testMachineThreadingKeepsJoinThatHoldsStore()
{
    OrderTest test;
    Output& out = test.out;
    OrderTestBooleanJoin blocks(test);
    LBasicBlock whenTrue = out.newBlock();
    LBasicBlock whenFalse = out.newBlock();
    out.store64(out.constInt64(5), out.address(test.testProcedure.heaps().argumentBuffer, test.third));
    out.branch(blocks.condition, normalEdge(whenTrue), normalEdge(whenFalse));
    test.returnConstant(whenTrue, 10);
    test.returnConstant(whenFalse, 20);
    const Order& order = test.compile();

    AOT_ORDER_TEST_CHECK(test, blocks.isKept(order));
    for (int64_t first : { 1, 3 }) {
        int64_t word = 0;
        AOT_ORDER_TEST_CHECK(test, test.run(first, 2, reinterpret_cast<int64_t>(&word)) == (first < 2 ? 10 : 20) && word == 5);
    }
}

// Builder::branch() tests the Phi itself. The comparison stays behind in the join, where nothing reads it and the scan would drop it.
static void testMachineThreadingPassesJoinThatHoldsUnreadComparison()
{
    OrderTest test;
    Output& out = test.out;
    OrderTestBooleanJoin blocks(test);
    LBasicBlock whenTrue = out.newBlock();
    LBasicBlock whenFalse = out.newBlock();
    out.branch(out.notZero32(blocks.condition), normalEdge(whenTrue), normalEdge(whenFalse));
    test.returnConstant(whenTrue, 10);
    test.returnConstant(whenFalse, 20);
    const Order& order = test.compile();

    AOT_ORDER_TEST_CHECK(test, blocks.join->size() == 2 && blocks.join->at(0)->opcode() == NotEqual && !blocks.join->at(0)->useCount());
    AOT_ORDER_TEST_CHECK(test, orderTestTargetOf(test.entry, 0) == whenTrue && orderTestTargetOf(test.entry, 1) == whenFalse);
    AOT_ORDER_TEST_CHECK(test, !order.of(blocks.join).isReached() && orderTestNumberOfReached(order, Branch) == 1);
    AOT_ORDER_TEST_CHECK(test, test.run(1, 2) == 10 && test.run(2, 1) == 20);
}

static void testMachineThreadingLeavesHeadOfLoop()
{
    OrderTest test;
    Output& out = test.out;
    LBasicBlock head = out.newBlock();
    LBasicBlock body = out.newBlock();
    LBasicBlock exit = out.newBlock();
    LValue continues = out.phiIn(head, Int32);
    out.upsilon(out.booleanTrue, continues);
    out.jump(head);
    out.appendTo(head);
    out.branch(continues, normalEdge(body), normalEdge(exit));
    out.appendTo(body);
    TypedPointer counter = out.address(test.testProcedure.heaps().argumentBuffer, test.first);
    LValue left = out.sub(out.load64(counter), 1);
    out.store64(left, counter);
    out.upsilon(out.notZero64(left), continues);
    out.jump(head);
    test.returnConstant(exit, 7);
    const Order& order = test.compile();

    AOT_ORDER_TEST_CHECK(test, test.entry->successor(0).block() == head && body->successor(0).block() == head && order.of(head).isLoopHead);
    int64_t rounds = 3;
    AOT_ORDER_TEST_CHECK(test, test.run(reinterpret_cast<int64_t>(&rounds)) == 7 && !rounds);
}

// `kind === 1 || kind === 2` on a boxed value. The entry asks whether first is above second, each of its two ways asks something of its own and comes to `again`, which asks what the entry has asked.
enum class OrderTestSecondTest : uint8_t {
    IsAlone,
    IsAskedTheOtherWayRound,
    StandsBehindStore,
    IsReadAgain,
    IsComeToByThirdWay,
};

template<OrderTestSecondTest secondTest>
static void testMachineThreadingOfTestThatWayHasDecided()
{
    OrderTest test;
    Output& out = test.out;
    LBasicBlock start = out.newBlock();
    LBasicBlock usual = out.newBlock();
    LBasicBlock slow = out.newBlock();
    LBasicBlock again = out.newBlock();
    LBasicBlock hit = out.newBlock();
    LBasicBlock usualAgain = out.newBlock();
    LBasicBlock slowAgain = out.newBlock();
    if (secondTest == OrderTestSecondTest::IsComeToByThirdWay)
        out.branch(out.equal(test.third, out.constInt64(3)), normalEdge(again), normalEdge(start));
    else
        out.jump(start);
    out.appendTo(start);
    out.branch(out.above(test.first, test.second), normalEdge(usual), rareEdge(slow));
    out.appendTo(usual);
    out.branch(out.equal(test.third, out.constInt64(1)), normalEdge(hit), normalEdge(again));
    out.appendTo(slow);
    out.branch(out.equal(test.third, out.constInt64(2)), normalEdge(hit), normalEdge(again));
    out.appendTo(again);
    if (secondTest == OrderTestSecondTest::StandsBehindStore)
        out.store64(out.constInt64(5), out.address(test.testProcedure.heaps().argumentBuffer, test.third));
    LValue asked = out.above(test.first, test.second);
    if (secondTest == OrderTestSecondTest::IsAskedTheOtherWayRound)
        out.branch(out.isZero32(asked), rareEdge(slowAgain), normalEdge(usualAgain));
    else
        out.branch(asked, normalEdge(usualAgain), rareEdge(slowAgain));
    out.appendTo(usualAgain);
    out.ret(secondTest == OrderTestSecondTest::IsReadAgain ? out.add(out.zeroExtPtr(asked), out.constInt64(9)) : out.constInt64(10));
    test.returnConstant(slowAgain, 20);
    test.returnConstant(hit, 30);
    const Order& order = test.compile();

    bool isPassedBy = secondTest != OrderTestSecondTest::StandsBehindStore && secondTest != OrderTestSecondTest::IsReadAgain;
    AOT_ORDER_TEST_CHECK(test, orderTestTargetOf(usual, 1) == (isPassedBy ? usualAgain : again) && orderTestTargetOf(slow, 1) == (isPassedBy ? slowAgain : again));
    AOT_ORDER_TEST_CHECK(test, order.of(again).isReached() == (!isPassedBy || secondTest == OrderTestSecondTest::IsComeToByThirdWay));
    AOT_ORDER_TEST_CHECK(test, !order.of(usualAgain).isRare() && order.of(slowAgain).isRare());
    if (secondTest == OrderTestSecondTest::StandsBehindStore) {
        int64_t word = 0;
        AOT_ORDER_TEST_CHECK(test, test.run(2, 1, reinterpret_cast<int64_t>(&word)) == 10 && word == 5);
        word = 0;
        AOT_ORDER_TEST_CHECK(test, test.run(1, 2, reinterpret_cast<int64_t>(&word)) == 20 && word == 5);
        return;
    }
    for (int64_t third : { 0, 1, 2, 3 }) {
        AOT_ORDER_TEST_CHECK(test, test.run(2, 1, third) == (third == 1 ? 30 : 10));
        AOT_ORDER_TEST_CHECK(test, test.run(1, 2, third) == (third == 2 ? 30 : 20));
    }
}

enum class OrderTestUseOfPick : uint8_t {
    Reads,
    Stores,
};

enum OrderTestWordOfObject : unsigned {
    OrderTestKey,
    OrderTestNumber,
    OrderTestChild,
};

static TypedPointer orderTestWordOf(OrderTest& test, LValue object, OrderTestWordOfObject word)
{
    return test.out.address(test.testProcedure.heaps().argumentBuffer, object, word * sizeof(int64_t));
}

// `side = flag ? a : b; n = flag ? a.n : b.n; if (n && side.c) ..`, the flag out of `exact ? key == a.k : key < b.k`, with n and c of both objects read and tested in front.
// Each arm that picks n tests the word that it has read, so the join of n goes. The flag is asked twice and `side` is read behind its join, so those two joins stay, and c is read, or written, through `side`.
template<OrderTestUseOfPick use>
static void testMachineThreadingOfValuesPickedByOneFlag()
{
    OrderTest test;
    Output& out = test.out;
    LBasicBlock none = out.newBlock();
    LBasicBlock leaf = out.newBlock();
    auto goOnIfNotZero = [&](LValue tested) {
        LBasicBlock next = out.newBlock();
        out.branch(tested, normalEdge(next), normalEdge(none));
        out.appendTo(next);
    };
    LValue numberOfFirst = out.load64(orderTestWordOf(test, test.first, OrderTestNumber));
    goOnIfNotZero(numberOfFirst);
    LValue numberOfSecond = out.load64(orderTestWordOf(test, test.second, OrderTestNumber));
    goOnIfNotZero(numberOfSecond);
    goOnIfNotZero(out.load64(orderTestWordOf(test, test.first, OrderTestChild)));
    goOnIfNotZero(out.load64(orderTestWordOf(test, test.second, OrderTestChild)));

    LBasicBlock asksForEqual = out.newBlock();
    LBasicBlock asksForLess = out.newBlock();
    LBasicBlock joinOfFlag = out.newBlock();
    LBasicBlock picksFirst = out.newBlock();
    LBasicBlock picksSecond = out.newBlock();
    LBasicBlock joinOfSide = out.newBlock();
    LBasicBlock picksNumberOfFirst = out.newBlock();
    LBasicBlock picksNumberOfSecond = out.newBlock();
    LBasicBlock joinOfNumber = out.newBlock();
    LBasicBlock looks = out.newBlock();
    LBasicBlock found = out.newBlock();
    LValue flag = out.phiIn(joinOfFlag, Int32);
    LValue side = out.phiIn(joinOfSide, Int64);
    LValue number = out.phiIn(joinOfNumber, Int64);
    LValue key = out.lShr(test.third, 1);
    out.branch(out.bitAnd(test.third, out.constInt64(1)), normalEdge(asksForEqual), normalEdge(asksForLess));
    out.appendTo(asksForEqual);
    out.upsilon(out.equal(key, out.load64(orderTestWordOf(test, test.first, OrderTestKey))), flag);
    out.jump(joinOfFlag);
    out.appendTo(asksForLess);
    out.upsilon(out.lessThan(key, out.load64(orderTestWordOf(test, test.second, OrderTestKey))), flag);
    out.jump(joinOfFlag);
    out.appendTo(joinOfFlag);
    out.branch(flag, normalEdge(picksFirst), normalEdge(picksSecond));
    test.passAndJump(picksFirst, test.first, side, joinOfSide);
    test.passAndJump(picksSecond, test.second, side, joinOfSide);
    out.appendTo(joinOfSide);
    out.branch(flag, normalEdge(picksNumberOfFirst), normalEdge(picksNumberOfSecond));
    test.passAndJump(picksNumberOfFirst, numberOfFirst, number, joinOfNumber);
    test.passAndJump(picksNumberOfSecond, numberOfSecond, number, joinOfNumber);
    out.appendTo(joinOfNumber);
    out.branch(number, normalEdge(looks), normalEdge(leaf));
    out.appendTo(looks);
    LValue child = out.load64(orderTestWordOf(test, side, OrderTestChild));
    out.branch(child, normalEdge(found), normalEdge(leaf));
    out.appendTo(found);
    if (use == OrderTestUseOfPick::Stores) {
        out.store64(out.constInt64(100), orderTestWordOf(test, side, OrderTestChild));
        out.ret(out.add(out.mul(out.load64(orderTestWordOf(test, test.first, OrderTestChild)), out.constInt64(1000)), out.load64(orderTestWordOf(test, test.second, OrderTestChild))));
    } else
        out.ret(child);
    test.returnConstant(leaf, -1);
    test.returnConstant(none, -2);
    const Order& order = test.compile();

    AOT_ORDER_TEST_CHECK(test, order.of(joinOfFlag).isReached() && order.of(joinOfSide).isReached() && !order.of(joinOfNumber).isReached());
    AOT_ORDER_TEST_CHECK(test, picksNumberOfFirst->last()->opcode() == Branch && picksNumberOfFirst->last()->child(0) == numberOfFirst);
    AOT_ORDER_TEST_CHECK(test, picksNumberOfSecond->last()->opcode() == Branch && picksNumberOfSecond->last()->child(0) == numberOfSecond);
    AOT_ORDER_TEST_CHECK(test, child->child(0) == side);
    for (int64_t asked = 0; asked < 8; ++asked) {
        for (int64_t exact = 0; exact < 2; ++exact) {
            int64_t first[] { 1, 2, 10 };
            int64_t second[] { 5, 3, 50 };
            bool picksFirstObject = exact ? asked == first[OrderTestKey] : asked < second[OrderTestKey];
            int64_t expected = picksFirstObject ? 10 : 50;
            if (use == OrderTestUseOfPick::Stores)
                expected = picksFirstObject ? 100050 : 10100;
            AOT_ORDER_TEST_CHECK(test, test.run(reinterpret_cast<int64_t>(first), reinterpret_cast<int64_t>(second), asked << 1 | exact) == expected);
            second[OrderTestNumber] = 0;
            AOT_ORDER_TEST_CHECK(test, test.run(reinterpret_cast<int64_t>(first), reinterpret_cast<int64_t>(second), asked << 1 | exact) == -2);
        }
    }
}

// `if (flag) a.n = 8; else b.n = 9; if (flag) return a.n * 100 + b.n; return b.n * 100 + a.n;`: each arm has decided the second test and passes it by. What stands behind the test reads the words again.
static void testMachineThreadingPastTestBehindArmsThatStore()
{
    OrderTest test;
    Output& out = test.out;
    LBasicBlock storesFirst = out.newBlock();
    LBasicBlock storesSecond = out.newBlock();
    LBasicBlock again = out.newBlock();
    LBasicBlock readsFirst = out.newBlock();
    LBasicBlock readsSecond = out.newBlock();
    auto returnNumbersOf = [&](LBasicBlock block, LValue hundreds, LValue units) {
        out.appendTo(block);
        out.ret(out.add(out.mul(out.load64(orderTestWordOf(test, hundreds, OrderTestNumber)), out.constInt64(100)), out.load64(orderTestWordOf(test, units, OrderTestNumber))));
    };
    out.branch(out.below(test.third, out.constInt64(5)), normalEdge(storesFirst), normalEdge(storesSecond));
    out.appendTo(storesFirst);
    out.store64(out.constInt64(8), orderTestWordOf(test, test.first, OrderTestNumber));
    out.jump(again);
    out.appendTo(storesSecond);
    out.store64(out.constInt64(9), orderTestWordOf(test, test.second, OrderTestNumber));
    out.jump(again);
    out.appendTo(again);
    out.branch(out.below(test.third, out.constInt64(5)), normalEdge(readsFirst), normalEdge(readsSecond));
    returnNumbersOf(readsFirst, test.first, test.second);
    returnNumbersOf(readsSecond, test.second, test.first);
    const Order& order = test.compile();

    AOT_ORDER_TEST_CHECK(test, !order.of(again).isReached() && orderTestTargetOf(storesFirst, 0) == readsFirst && orderTestTargetOf(storesSecond, 0) == readsSecond);
    for (int64_t third : { 1, 9 }) {
        int64_t first[] { 1, 2, 10 };
        int64_t second[] { 5, 3, 50 };
        AOT_ORDER_TEST_CHECK(test, test.run(reinterpret_cast<int64_t>(first), reinterpret_cast<int64_t>(second), third) == (third < 5 ? 803 : 902));
    }
}

// The head of the loop asks in each turn what the entry has asked. The edge that comes round has decided it too, and the head stays the one way into the loop.
static void testMachineThreadingLeavesDecidedTestAtHeadOfLoop()
{
    OrderTest test;
    Output& out = test.out;
    LBasicBlock head = out.newBlock();
    LBasicBlock body = out.newBlock();
    LBasicBlock exit = out.newBlock();
    out.branch(out.above(test.first, test.second), normalEdge(head), normalEdge(exit));
    out.appendTo(head);
    out.branch(out.above(test.first, test.second), normalEdge(body), normalEdge(exit));
    out.appendTo(body);
    TypedPointer counter = out.address(test.testProcedure.heaps().argumentBuffer, test.third);
    LValue left = out.sub(out.load64(counter), 1);
    out.store64(left, counter);
    out.branch(out.notZero64(left), normalEdge(head), normalEdge(exit));
    test.returnConstant(exit, 7);
    const Order& order = test.compile();

    AOT_ORDER_TEST_CHECK(test, orderTestTargetOf(test.entry, 0) == head && orderTestTargetOf(body, 0) == head && order.of(head).isLoopHead);
    int64_t rounds = 3;
    AOT_ORDER_TEST_CHECK(test, test.run(2, 1, reinterpret_cast<int64_t>(&rounds)) == 7 && !rounds);
}

// A row of `first === n`, each behind the question whether first is above second. With `sameTest` false each asks that of another bound, so no way has decided anything and every answer is kept to the end of the row.
static std::optional<uint64_t> orderTestInstructionsForRowOfTests(unsigned numberOfTerms, bool sameTest)
{
    OrderTest test;
    Output& out = test.out;
    LBasicBlock hit = out.newBlock();
    LBasicBlock next = out.newBlock();
    out.jump(next);
    Vector<LBasicBlock> usualWays;
    for (unsigned term = 0; term < numberOfTerms; ++term) {
        LBasicBlock usual = out.newBlock();
        LBasicBlock slow = out.newBlock();
        LBasicBlock behind = out.newBlock();
        out.appendTo(next);
        out.branch(out.above(test.first, sameTest ? test.second : out.constInt64(term)), normalEdge(usual), rareEdge(slow));
        out.appendTo(usual);
        out.branch(out.equal(test.third, out.constInt64(2 * term + 1)), normalEdge(hit), normalEdge(behind));
        out.appendTo(slow);
        out.branch(out.equal(test.third, out.constInt64(2 * term + 2)), normalEdge(hit), normalEdge(behind));
        usualWays.append(usual);
        next = behind;
    }
    test.returnConstant(next, 10);
    test.returnConstant(hit, 30);
    std::optional<uint64_t> instructions = instructionsOfCurrentThreadIn([&] {
        prepareOrFail(test.testProcedure);
    });
    if (sameTest) {
        for (unsigned term = 0; term + 1 < numberOfTerms; ++term)
            AOT_TEST_CHECK(orderTestTargetOf(usualWays[term], 1) == usualWays[term + 1]);
    }
    return instructions;
}

template<bool sameTest>
static void testMachineThreadingGrowsAsTestsInRowDo()
{
    constexpr unsigned numberOfTerms = 4000;
    std::optional<uint64_t> small = orderTestInstructionsForRowOfTests(numberOfTerms, sameTest);
    std::optional<uint64_t> large = orderTestInstructionsForRowOfTests(4 * numberOfTerms, sameTest);
    AOT_TEST_CHECK(growsAsSizeDoes(small, large));
}

// `o.flags & 4`: the entry leaves for `empty` where first is 0, and the next block leaves for `slow` where first is below 1000.
enum class OrderTestNextTest : uint8_t {
    LeavesForZeroToo,
    LeavesForZeroTooByItsFirstWay,
    LetsZeroPass,
    StandsBehindLoad,
    IsEnteredByAnotherWay,
    FollowsTestOfBits,
    FollowsTestOfBitsThatReachAboveItsBound,
};

template<OrderTestNextTest nextTest>
static void testMachineThreadingOfTestThatNextTestImplies()
{
    OrderTest test;
    Output& out = test.out;
    LBasicBlock start = out.newBlock();
    LBasicBlock next = out.newBlock();
    LBasicBlock empty = out.newBlock();
    LBasicBlock usual = out.newBlock();
    LBasicBlock slow = out.newBlock();
    if (nextTest == OrderTestNextTest::IsEnteredByAnotherWay)
        out.branch(out.equal(test.second, out.constInt64(3)), normalEdge(next), normalEdge(start));
    else
        out.jump(start);
    out.appendTo(start);
    // Without a bit at or above 1024 the word is below 1000 or from 1000 to 1023: with the mask that starts at 512 all such words are below 1000.
    if (nextTest == OrderTestNextTest::FollowsTestOfBits)
        out.branch(out.bitAnd(test.first, out.constInt64(~static_cast<int64_t>(511))), normalEdge(next), rareEdge(empty));
    else if (nextTest == OrderTestNextTest::FollowsTestOfBitsThatReachAboveItsBound)
        out.branch(out.bitAnd(test.first, out.constInt64(~static_cast<int64_t>(1023))), normalEdge(next), rareEdge(empty));
    else
        out.branch(test.first, normalEdge(next), rareEdge(empty));
    out.appendTo(next);
    LValue added = out.int64Zero;
    if (nextTest == OrderTestNextTest::StandsBehindLoad)
        added = out.load64(out.address(test.testProcedure.heaps().argumentBuffer, test.third));
    if (nextTest == OrderTestNextTest::LetsZeroPass)
        out.branch(out.below(test.first, out.constInt64(1000)), normalEdge(usual), rareEdge(slow));
    else if (nextTest == OrderTestNextTest::LeavesForZeroTooByItsFirstWay)
        out.branch(out.below(test.first, out.constInt64(1000)), rareEdge(slow), normalEdge(usual));
    else
        out.branch(out.aboveOrEqual(test.first, out.constInt64(1000)), normalEdge(usual), rareEdge(slow));
    out.appendTo(usual);
    out.ret(out.add(added, out.constInt64(10)));
    test.returnConstant(slow, 20);
    test.returnConstant(empty, 30);
    const Order& order = test.compile();

    bool isAskedBehind = nextTest == OrderTestNextTest::LeavesForZeroToo || nextTest == OrderTestNextTest::LeavesForZeroTooByItsFirstWay || nextTest == OrderTestNextTest::FollowsTestOfBits;
    AOT_ORDER_TEST_CHECK(test, (start->last()->opcode() == Jump) == isAskedBehind);
    if (isAskedBehind) {
        unsigned leaving = nextTest == OrderTestNextTest::LeavesForZeroTooByItsFirstWay ? 0 : 1;
        Block* asked = orderTestTargetOf(next, leaving);
        AOT_ORDER_TEST_CHECK(test, orderTestTargetOf(start, 0) == next && orderTestTargetOf(next, 1 - leaving) == usual && asked != slow && order.of(asked).isRare());
        AOT_ORDER_TEST_CHECK(test, asked->size() == 1 && asked->last()->opcode() == Branch && orderTestTargetOf(asked, 0) == slow && orderTestTargetOf(asked, 1) == empty);
    }
    int64_t word = 0;
    int64_t third = reinterpret_cast<int64_t>(&word);
    bool zeroPasses = nextTest == OrderTestNextTest::LetsZeroPass;
    int64_t leastThatGoesOn = nextTest == OrderTestNextTest::FollowsTestOfBits ? 512 : nextTest == OrderTestNextTest::FollowsTestOfBitsThatReachAboveItsBound ? 1024 : 1;
    for (int64_t first : { 0, 1, 511, 512, 999, 1000, 1023, 1024, 5000, -1 }) {
        bool goesOn = static_cast<uint64_t>(first) >= static_cast<uint64_t>(leastThatGoesOn);
        bool isBelow = static_cast<uint64_t>(first) < 1000;
        AOT_ORDER_TEST_CHECK(test, test.run(first, 0, third) == (!goesOn ? 30 : isBelow == zeroPasses ? 10 : 20));
    }
    if (nextTest == OrderTestNextTest::IsEnteredByAnotherWay)
        AOT_ORDER_TEST_CHECK(test, test.run(0, 3, third) == 20 && test.run(2000, 3, third) == 10);
}

template<unsigned numberOfHops, EdgeClass classOfLastJump>
static void testMachineThreadingPassesBlocksThatOnlyJump()
{
    OrderTest test;
    Output& out = test.out;
    LBasicBlock target = out.newBlock();
    LBasicBlock other = out.newBlock();
    Vector<LBasicBlock> hops;
    for (unsigned index = 0; index < numberOfHops; ++index)
        hops.append(out.newBlock());
    out.branch(test.first, normalEdge(hops[0]), normalEdge(other));
    for (unsigned index = 0; index < numberOfHops; ++index) {
        out.appendTo(hops[index]);
        out.jump(index + 1 < numberOfHops ? normalEdge(hops[index + 1]) : Edge(target, classOfLastJump));
    }
    test.returnConstant(target, 10);
    test.returnConstant(other, 20);
    const Order& order = test.compile();

    AOT_ORDER_TEST_CHECK(test, test.entry->successor(0).block() == target && test.entry->successor(0).edgeClass() == classOfLastJump);
    for (LBasicBlock hop : hops)
        AOT_ORDER_TEST_CHECK(test, !order.of(hop).isReached());
    AOT_ORDER_TEST_CHECK(test, order.of(target).isRare() == (classOfLastJump == EdgeClass::Rare));
    AOT_ORDER_TEST_CHECK(test, test.run(1) == 10 && test.run(0) == 20);
}

static void testMachineThreadingEndsInCycleOfJumps()
{
    OrderTest test;
    Output& out = test.out;
    LBasicBlock spins = out.newBlock();
    LBasicBlock spinsOn = out.newBlock();
    LBasicBlock done = out.newBlock();
    out.branch(test.first, normalEdge(spins), normalEdge(done));
    out.appendTo(spins);
    out.jump(spinsOn);
    out.appendTo(spinsOn);
    out.jump(spins);
    test.returnConstant(done, 7);
    const Order& order = test.compile();

    AOT_ORDER_TEST_CHECK(test, order.loops().size() == 1 && order.of(done).loop == noLoop);
    AOT_ORDER_TEST_CHECK(test, test.run(0) == 7);
}

// Leaves the function. The answer is the origin of the Patch plus the addend, so a Patch that stands in for one with another origin is seen.
static PatchGenerator* orderTestCreateThrowGenerator(OrderTest& test, int64_t addend)
{
    return test.procedure().createGenerator([addend](CCallHelpers& jit, const PatchParameters& parameters) {
        if (parameters.frameIsSetUp())
            emitFrameTeardown(jit, parameters.frame());
        jit.move(CCallHelpers::TrustedImm64(parameters.origin().bits() + addend), GPRInfo::returnValueGPR);
        jit.ret();
    });
}

static void orderTestFillWithThrow(Output& out, LBasicBlock block, Origin origin, PatchGenerator* generator)
{
    out.appendTo(block);
    out.setOrigin(origin);
    PatchOperation* patch = out.patchpoint(Void);
    patch->effects.terminal = true;
    patch->setRunsWithoutFrame(true);
    patch->setGenerator(generator);
    out.setOrigin(Origin());
}

enum class OrderTestThrowBlocks : uint8_t { AreAlike, DifferInOrigin, DifferInGenerator };

template<OrderTestThrowBlocks kind>
static void testMachineMergingOfThrowBlocks()
{
    constexpr bool merges = kind == OrderTestThrowBlocks::AreAlike;
    constexpr int64_t answerOfSecondThrow = merges ? 11 : kind == OrderTestThrowBlocks::DifferInOrigin ? 12 : 111;
    OrderTest test;
    Output& out = test.out;
    LBasicBlock firstThrow = out.newBlock();
    LBasicBlock next = out.newBlock();
    LBasicBlock secondThrow = out.newBlock();
    LBasicBlock done = out.newBlock();
    PatchGenerator* generator = orderTestCreateThrowGenerator(test, 0);
    PatchGenerator* secondGenerator = kind == OrderTestThrowBlocks::DifferInGenerator ? orderTestCreateThrowGenerator(test, 100) : generator;
    out.branch(test.first, rareEdge(firstThrow), normalEdge(next));
    out.appendTo(next);
    out.branch(test.second, rareEdge(secondThrow), normalEdge(done));
    test.returnConstant(done, 7);
    orderTestFillWithThrow(out, firstThrow, Origin(11), generator);
    orderTestFillWithThrow(out, secondThrow, Origin(kind == OrderTestThrowBlocks::DifferInOrigin ? 12 : 11), secondGenerator);
    const Order& order = test.compile();

    AOT_ORDER_TEST_CHECK(test, orderTestTargetOf(test.entry, 0) == firstThrow && orderTestTargetOf(next, 0) == (merges ? firstThrow : secondThrow));
    AOT_ORDER_TEST_CHECK(test, order.of(secondThrow).isReached() == !merges && order.rareRegions().size() == (merges ? 1u : 2u));
    AOT_ORDER_TEST_CHECK(test, order.rareRegions()[order.of(firstThrow).rareRegion].onlyEntry == (merges ? nullptr : firstThrow));
    AOT_ORDER_TEST_CHECK(test, test.run(1, 0) == 11 && test.run(0, 1) == answerOfSecondThrow && test.run(0, 0) == 7);
}

static void testMachineMergingComparesChildren()
{
    OrderTest test;
    Output& out = test.out;
    LBasicBlock addsFive = out.newBlock();
    LBasicBlock next = out.newBlock();
    LBasicBlock addsFiveToo = out.newBlock();
    LBasicBlock addsSix = out.newBlock();
    out.branch(test.first, normalEdge(addsFive), normalEdge(next));
    out.appendTo(next);
    out.branch(test.second, normalEdge(addsFiveToo), normalEdge(addsSix));
    auto fill = [&](LBasicBlock block, int64_t addend) {
        out.appendTo(block);
        out.ret(out.add(test.third, addend));
    };
    fill(addsFive, 5);
    fill(addsFiveToo, 5);
    fill(addsSix, 6);
    const Order& order = test.compile();

    AOT_ORDER_TEST_CHECK(test, orderTestTargetOf(test.entry, 0) == addsFive && orderTestTargetOf(next, 0) == addsFive && orderTestTargetOf(next, 1) == addsSix);
    AOT_ORDER_TEST_CHECK(test, !order.of(addsFiveToo).isReached());
    AOT_ORDER_TEST_CHECK(test, test.run(1, 0, 10) == 15 && test.run(0, 1, 10) == 15 && test.run(0, 0, 10) == 16);
}

enum class OrderTestWayIntoArm : uint8_t { AsArmOfEmitter, AsSuccessorInCode };

// if (a) return a * b + c; if (b) return a * b + c; return 7. Each block goes on in the scope of the one block that leads to it, and both arms find the product that the entry has computed.
template<OrderTestWayIntoArm way>
static void testMachineMergingOfBlocksThatContinueScope()
{
    OrderTest test;
    Output& out = test.out;
    LBasicBlock adds = out.newBlock();
    LBasicBlock next = out.newBlock();
    LBasicBlock addsToo = out.newBlock();
    LBasicBlock done = out.newBlock();
    LValue product = out.mul(test.first, test.second);

    auto branch = [&](LValue tested, LBasicBlock taken, LBasicBlock notTaken) {
        if (way == OrderTestWayIntoArm::AsArmOfEmitter) {
            ScopeFork fork = out.forkScope();
            out.branch(tested, normalEdge(taken), normalEdge(notTaken));
            return fork;
        }
        out.branch(tested, normalEdge(taken), normalEdge(notTaken));
        return out.forkScope();
    };
    auto enter = [&](LBasicBlock block, const ScopeFork& fork) {
        if (way == OrderTestWayIntoArm::AsArmOfEmitter)
            out.appendToArm(block, fork);
        else
            out.appendToSuccessor(block, fork);
    };
    bool bothFoundProduct = true;
    auto fill = [&](LBasicBlock block, const ScopeFork& fork) {
        enter(block, fork);
        bothFoundProduct &= out.mul(test.first, test.second) == product;
        out.ret(out.add(product, test.third));
    };

    ScopeFork outer = branch(test.first, adds, next);
    fill(adds, outer);
    enter(next, outer);
    ScopeFork inner = branch(test.second, addsToo, done);
    fill(addsToo, inner);
    enter(done, inner);
    out.ret(out.constInt64(7));
    bool bothWentOnInScope = adds->hasOnlyOnePredecessor() && addsToo->hasOnlyOnePredecessor();
    bool bothReadProduct = product->useCount() == 2;
    const Order& order = test.compile();

    AOT_ORDER_TEST_CHECK(test, bothFoundProduct && bothWentOnInScope);
    AOT_ORDER_TEST_CHECK(test, bothReadProduct && product->useCount() == 1);
    AOT_ORDER_TEST_CHECK(test, orderTestTargetOf(test.entry, 0) == adds && orderTestTargetOf(next, 0) == adds && orderTestTargetOf(next, 1) == done);
    AOT_ORDER_TEST_CHECK(test, !order.of(addsToo).isReached() && order.of(adds).predecessors.size() == 2 && !adds->hasOnlyOnePredecessor());
    AOT_ORDER_TEST_CHECK(test, test.run(2, 3, 10) == 16 && test.run(0, 3, 10) == 10 && test.run(0, 0, 10) == 7);
}

// The block that stays is as likely as the likeliest way into either.
static void testMachineMergingOfRareArmWithLikelyArm()
{
    OrderTest test;
    Output& out = test.out;
    LBasicBlock seldom = out.newBlock();
    LBasicBlock next = out.newBlock();
    LBasicBlock often = out.newBlock();
    LBasicBlock done = out.newBlock();
    // A scope that holds nothing is handed to nobody.
    LValue sum = out.add(test.first, test.second);
    out.branch(test.first, rareEdge(seldom), normalEdge(next));
    ScopeFork outer = out.forkScope();
    out.appendToSuccessor(seldom, outer);
    out.ret(out.add(test.third, 5));
    out.appendToSuccessor(next, outer);
    out.branch(test.second, normalEdge(often), normalEdge(done));
    ScopeFork inner = out.forkScope();
    out.appendToSuccessor(often, inner);
    out.ret(out.add(test.third, 5));
    out.appendToSuccessor(done, inner);
    out.ret(out.add(sum, test.third));
    bool onlyLikelyArmWentOnInScope = !seldom->hasOnlyOnePredecessor() && often->hasOnlyOnePredecessor();
    const Order& order = test.compile();

    AOT_ORDER_TEST_CHECK(test, onlyLikelyArmWentOnInScope);
    AOT_ORDER_TEST_CHECK(test, orderTestTargetOf(test.entry, 0) == seldom && orderTestTargetOf(next, 0) == seldom);
    AOT_ORDER_TEST_CHECK(test, !order.of(often).isReached() && !order.of(seldom).isRare() && order.of(seldom).predecessors.size() == 2);
    AOT_ORDER_TEST_CHECK(test, test.run(1, 0, 10) == 15 && test.run(0, 1, 10) == 15 && test.run(0, 0, 10) == 10);
}

// less = a < b; if (c) return less ? a : b; if (a) return less ? a : b; return 7. The test has two readers, so it would be a value in a register. One arm stands for both, and the branch that is left makes the test itself.
static void testMachineMergingLeavesTestToItsOneReader()
{
    OrderTest test;
    Output& out = test.out;
    LBasicBlock chooses = out.newBlock();
    LBasicBlock next = out.newBlock();
    LBasicBlock choosesToo = out.newBlock();
    LBasicBlock done = out.newBlock();
    LBasicBlock smaller = out.newBlock();
    LBasicBlock larger = out.newBlock();
    LValue less = out.below(test.first, test.second);
    out.branch(test.third, normalEdge(chooses), normalEdge(next));
    ScopeFork outer = out.forkScope();
    out.appendToSuccessor(chooses, outer);
    out.branch(less, normalEdge(smaller), normalEdge(larger));
    out.appendToSuccessor(next, outer);
    out.branch(test.first, normalEdge(choosesToo), normalEdge(done));
    ScopeFork inner = out.forkScope();
    out.appendToSuccessor(choosesToo, inner);
    out.branch(less, normalEdge(smaller), normalEdge(larger));
    test.returnConstant(done, 7);
    test.returnValue(smaller, test.first);
    test.returnValue(larger, test.second);
    bool bothWentOnInScope = chooses->hasOnlyOnePredecessor() && choosesToo->hasOnlyOnePredecessor();
    bool bothReadTest = chooses->last()->child(0) == less && choosesToo->last()->child(0) == less && less->useCount() == 2;
    const Order& order = test.compile();

    AOT_ORDER_TEST_CHECK(test, bothWentOnInScope && bothReadTest);
    AOT_ORDER_TEST_CHECK(test, orderTestTargetOf(test.entry, 0) == chooses && orderTestTargetOf(next, 0) == chooses && !order.of(choosesToo).isReached());
    AOT_ORDER_TEST_CHECK(test, less->useCount() == 1 && test.procedure().liveness().isFused(less));
    AOT_ORDER_TEST_CHECK(test, test.run(2, 3, 1) == 2 && test.run(5, 4, 1) == 4 && test.run(2, 3, 0) == 2 && test.run(5, 4, 0) == 4 && test.run(0, 3, 0) == 7);
}

template<bool passSameValue>
static void testMachineMergingComparesUpsilons()
{
    OrderTest test;
    Output& out = test.out;
    LBasicBlock left = out.newBlock();
    LBasicBlock right = out.newBlock();
    LBasicBlock join = out.newBlock();
    LValue result = out.phiIn(join, Int64);
    out.branch(test.first, normalEdge(left), normalEdge(right));
    test.passAndJump(left, test.second, result, join);
    test.passAndJump(right, passSameValue ? test.second : test.first, result, join);
    test.returnValue(join, result);
    const Order& order = test.compile();

    AOT_ORDER_TEST_CHECK(test, orderTestTargetOf(test.entry, 0) == left && test.entry->numSuccessors() == (passSameValue ? 1u : 2u));
    AOT_ORDER_TEST_CHECK(test, passSameValue || orderTestTargetOf(test.entry, 1) == right);
    AOT_ORDER_TEST_CHECK(test, order.of(right).isReached() == !passSameValue);
    AOT_ORDER_TEST_CHECK(test, test.run(3, 5) == 5 && test.run(0, 5) == (passSameValue ? 5 : 0));
}

// function f(o: A | B) { return o.x }, with x at one offset in both: the arms load the same word, so one stands for both and nothing is left to choose between.
static void testMachineOrderDropsTestWhoseArmsMerge()
{
    OrderTest test;
    Output& out = test.out;
    LBasicBlock left = out.newBlock();
    LBasicBlock right = out.newBlock();
    LValue sum = out.add(test.first, test.second);
    LValue tested = out.below(sum, test.second);
    out.branch(tested, unlikelyEdge(left), normalEdge(right));
    for (LBasicBlock arm : { left, right }) {
        out.appendTo(arm);
        out.ret(out.load64(out.address(test.testProcedure.heaps().argumentBuffer, test.third, 8)));
    }
    const Order& order = test.compile();

    const Liveness& liveness = test.procedure().liveness();
    const Step& end = test.procedure().allocation().step(test.procedure().allocation().stepsOf(test.entry).terminator);
    AOT_ORDER_TEST_CHECK(test, test.entry->last()->opcode() == Branch && test.entry->last()->isUnconditionalJump());
    AOT_ORDER_TEST_CHECK(test, test.entry->numSuccessors() == 1 && test.entry->successor(0) == normalEdge(left) && left->numberOfIncomingEdges() == 1);
    AOT_ORDER_TEST_CHECK(test, order.of(left).predecessors.size() == 1 && !order.of(right).isReached() && test.procedure().numberOfBlocks() == 3);
    AOT_ORDER_TEST_CHECK(test, !tested->useCount() && !sum->useCount() && liveness.isDropped(tested) && liveness.isDropped(sum));
    AOT_ORDER_TEST_CHECK(test, end.operation == test.entry->last() && !end.sources[0]);
    int64_t words[] = { 3, 5 };
    AOT_ORDER_TEST_CHECK(test, test.run(1, 2, reinterpret_cast<int64_t>(words)) == 5 && test.run(-1, 2, reinterpret_cast<int64_t>(words)) == 5);
}

// if (a < b) { } else { }. What is tested for nothing stays where something else reads it, and so does whatever else the block does.
static void testMachineOrderKeepsWhatElseReadsTestedValue()
{
    OrderTest test;
    Output& out = test.out;
    LBasicBlock left = out.newBlock();
    LBasicBlock right = out.newBlock();
    LBasicBlock join = out.newBlock();
    TypedPointer counter = out.address(test.testProcedure.heaps().argumentBuffer, test.third);
    out.store64(out.add(out.load64(counter), 1), counter);
    LValue tested = out.below(test.first, test.second);
    out.branch(tested, normalEdge(left), normalEdge(right));
    for (LBasicBlock arm : { left, right }) {
        out.appendTo(arm);
        out.jump(join);
    }
    out.appendTo(join);
    out.ret(out.zeroExtPtr(tested));
    const Order& order = test.compile();

    AOT_ORDER_TEST_CHECK(test, test.entry->last()->isUnconditionalJump() && test.entry->successor(0) == normalEdge(join) && order.of(join).predecessors.size() == 1);
    AOT_ORDER_TEST_CHECK(test, tested->useCount() == 1 && !test.procedure().liveness().isDropped(tested));
    int64_t count = 0;
    AOT_ORDER_TEST_CHECK(test, test.run(1, 2, reinterpret_cast<int64_t>(&count)) == 1 && !test.run(2, 1, reinterpret_cast<int64_t>(&count)) && count == 2);
}

// The block that comes first fixes the head of the join with the two values in each other's registers. The moves of the other edge stand ahead of the Branch, which reads no register that they write.
static void testMachineOrderMovesRegistersAheadOfBranchThatTestsNothing()
{
    OrderTest test;
    Output& out = test.out;
    LBasicBlock exchanges = out.newBlock();
    LBasicBlock tests = out.newBlock();
    LBasicBlock left = out.newBlock();
    LBasicBlock right = out.newBlock();
    LBasicBlock join = out.newBlock();
    out.branch(test.first, normalEdge(exchanges), normalEdge(tests));
    out.appendTo(exchanges);
    PatchOperation* exchange = out.patchpoint(Void);
    exchange->append(test.second, ValueRep::reg(test.third->argumentReg()));
    exchange->append(test.third, ValueRep::reg(test.second->argumentReg()));
    exchange->setGenerator([](CCallHelpers&, const PatchParameters&) { });
    out.jump(join);
    out.appendTo(tests);
    out.branch(out.below(test.second, test.third), normalEdge(left), normalEdge(right));
    for (LBasicBlock arm : { left, right }) {
        out.appendTo(arm);
        out.jump(join);
    }
    out.appendTo(join);
    out.ret(out.sub(test.second, test.third));
    const Order& order = test.compile();

    const BlockSteps& steps = test.procedure().allocation().stepsOf(tests);
    AOT_ORDER_TEST_CHECK(test, tests->last()->opcode() == Branch && tests->last()->isUnconditionalJump() && tests->successor(0) == normalEdge(join));
    AOT_ORDER_TEST_CHECK(test, order.of(join).predecessors.size() == 2 && order.of(exchanges).orderIndex < order.of(tests).orderIndex);
    AOT_ORDER_TEST_CHECK(test, steps.beginOfEdgeMoves != steps.endOfEdgeMoves);
    AOT_ORDER_TEST_CHECK(test, test.run(1, 9, 4) == 5 && test.run(0, 9, 4) == 5);
}

// The block that leaves a rare region tears the frame of the region down ahead of its last operation, which then may write no way out but a jump.
static void testMachineOrderLeavesRareRegionThroughBranchThatTestsNothing()
{
    OrderTest test;
    Output& out = test.out;
    LBasicBlock slow = out.newBlock();
    LBasicBlock left = out.newBlock();
    LBasicBlock right = out.newBlock();
    LBasicBlock rejoin = out.newBlock();
    out.branch(test.first, rareEdge(slow), normalEdge(rejoin));
    out.appendTo(slow);
    PatchOperation* call = out.patchpoint(Void);
    call->setGenerator([](CCallHelpers& jit, const PatchParameters&) {
        jit.nop();
    });
    out.branch(test.second, normalEdge(left), normalEdge(right));
    for (LBasicBlock arm : { left, right }) {
        out.appendTo(arm);
        out.jump(rejoin);
    }
    test.returnValue(rejoin, test.third);
    const Order& order = test.compile();

    AOT_ORDER_TEST_CHECK(test, slow->last()->opcode() == Branch && slow->last()->isUnconditionalJump() && slow->successor(0).block() == rejoin);
    AOT_ORDER_TEST_CHECK(test, order.of(slow).isRare() && order.of(slow).entersRareRegion && order.of(slow).leavesRareRegion);
    AOT_ORDER_TEST_CHECK(test, test.procedure().frame().coverage() == FrameCoverage::RareRegions);
    AOT_ORDER_TEST_CHECK(test, test.run(1, 0, 7) == 7 && test.run(1, 1, 7) == 7 && test.run(0, 0, 7) == 7);
}

enum class OrderTestChildrenOfBits : uint8_t { TwoThatAreReadAgain, TwoThatAreNotReadAgain, OneThatIsNotReadAgain };

// Bits are asked for ahead of a diamond and branched on behind it. Where both children are read again behind the Branch, the Branch tests them itself and the bits take no register on the way.
// So it does where the one child beside a constant is not read again: that child lives as far as the bits would have. Two such children would take two registers where the bits take one.
template<OrderTestChildrenOfBits children>
static void testMachineFusionOfTestInLaterBlock()
{
    constexpr bool childrenAreReadBehindTest = children == OrderTestChildrenOfBits::TwoThatAreReadAgain;
    OrderTest test;
    Output& out = test.out;
    LBasicBlock left = out.newBlock();
    LBasicBlock right = out.newBlock();
    LBasicBlock join = out.newBlock();
    LBasicBlock whenSet = out.newBlock();
    LBasicBlock whenClear = out.newBlock();
    LValue sum = out.phiIn(join, Int64);
    LValue bits = children == OrderTestChildrenOfBits::OneThatIsNotReadAgain ? out.bitAnd(test.first, 3) : out.bitAnd(test.first, test.second);
    out.branch(test.third, normalEdge(left), normalEdge(right));
    out.appendTo(left);
    out.upsilon(out.add(test.third, 1), sum);
    out.jump(join);
    test.passAndJump(right, out.constInt64(5), sum, join);
    out.appendTo(join);
    out.branch(bits, normalEdge(whenSet), normalEdge(whenClear));
    out.appendTo(whenSet);
    out.ret(childrenAreReadBehindTest ? out.add(out.add(sum, test.first), test.second) : sum);
    test.returnConstant(whenClear, -1);
    test.compile();

    constexpr int64_t addend = childrenAreReadBehindTest ? 9 : 0;
    AOT_ORDER_TEST_CHECK(test, bits->owner() == test.entry && bits->useCount() == 1);
    AOT_ORDER_TEST_CHECK(test, test.procedure().liveness().isFused(bits) == (children != OrderTestChildrenOfBits::TwoThatAreNotReadAgain));
    AOT_ORDER_TEST_CHECK(test, test.run(6, 3, 1) == 2 + addend && test.run(6, 3, 0) == 5 + addend && test.run(4, 3, 1) == -1);
}

// The number would keep a register of its bank as far as the Branch, where the answer keeps one of the other bank.
static void testMachineFusionLeavesComparisonOfNumberThatIsNotReadAgain()
{
    OrderTest test;
    Output& out = test.out;
    LBasicBlock left = out.newBlock();
    LBasicBlock right = out.newBlock();
    LBasicBlock join = out.newBlock();
    LBasicBlock whenBelow = out.newBlock();
    LBasicBlock otherwise = out.newBlock();
    LValue sum = out.phiIn(join, Int64);
    LValue isBelow = out.doubleLessThan(test.testProcedure.argumentFPR(0), out.constDouble(4));
    out.branch(test.third, normalEdge(left), normalEdge(right));
    out.appendTo(left);
    out.upsilon(out.add(test.third, 1), sum);
    out.jump(join);
    test.passAndJump(right, out.constInt64(5), sum, join);
    out.appendTo(join);
    out.branch(isBelow, normalEdge(whenBelow), normalEdge(otherwise));
    test.returnValue(whenBelow, sum);
    test.returnConstant(otherwise, -1);
    test.compile();

    auto run = [&](int64_t third, double number) {
        return test.testProcedure.call<int64_t>(int64_t { 0 }, int64_t { 0 }, third, number);
    };
    AOT_ORDER_TEST_CHECK(test, isBelow->owner() == test.entry && isBelow->useCount() == 1 && !test.procedure().liveness().isFused(isBelow));
    AOT_ORDER_TEST_CHECK(test, run(1, 3.5) == 2 && run(0, 3.5) == 5 && run(1, 4) == -1);
}

// if (a < 4 && b == c), if (a < 4 || b == c), as an emitter asks with bitAnd() and bitOr(): neither answer is made a word.
template<Opcode combination>
static void testMachineFusionOfBranchOnTwoComparisons()
{
    constexpr bool isOr = combination == BitOr;
    for (EdgeClass classOfWayThatHolds : { EdgeClass::Normal, EdgeClass::Unlikely, EdgeClass::Rare }) {
        OrderTest test;
        Output& out = test.out;
        LBasicBlock holds = out.newBlock();
        LBasicBlock fails = out.newBlock();
        LValue isBelow = out.below(test.first, out.constInt64(4));
        LValue isEqual = out.equal(test.second, test.third);
        LValue tested = out.binary(combination, isBelow, isEqual);
        out.branch(tested, Edge(holds, classOfWayThatHolds), normalEdge(fails));
        test.returnConstant(holds, 10);
        test.returnConstant(fails, 20);
        test.compile();

        const Liveness& liveness = test.procedure().liveness();
        AOT_ORDER_TEST_CHECK(test, test.entry->last()->child(0) == tested && liveness.isFused(tested) && liveness.isFused(isBelow) && liveness.isFused(isEqual));
        for (int64_t first : { 3, 4 }) {
            for (int64_t second : { 7, 8 }) {
                bool expected = isOr ? first < 4 || second == 7 : first < 4 && second == 7;
                AOT_ORDER_TEST_CHECK(test, test.run(first, second, 7) == (expected ? 10 : 20));
            }
        }
    }
}

// if (*p < 4 && b == c), if (a < 4 && *q == c), if (*p < 4 && *q == c). On x86-64 one of the two comparisons reads its word where it lies.
template<Opcode combination>
static void testMachineFusionOfLoadBelowTwoComparisons()
{
    constexpr bool isOr = combination == BitOr;
    constexpr unsigned firstIsLoaded = 1;
    constexpr unsigned secondIsLoaded = 2;
    for (unsigned loaded : { firstIsLoaded, secondIsLoaded, firstIsLoaded | secondIsLoaded }) {
        OrderTest test;
        Output& out = test.out;
        const AbstractHeap& heap = test.testProcedure.heaps().argumentBuffer;
        LBasicBlock holds = out.newBlock();
        LBasicBlock fails = out.newBlock();
        LValue small = loaded & firstIsLoaded ? out.load64(out.address(heap, test.third)) : test.first;
        LValue other = loaded & secondIsLoaded ? out.load64(out.address(heap, test.third, 8)) : test.first;
        LValue isBelow = out.below(small, out.constInt64(4));
        LValue isEqual = out.equal(other, test.second);
        LValue tested = out.binary(combination, isBelow, isEqual);
        out.branch(tested, normalEdge(holds), normalEdge(fails));
        test.returnConstant(holds, 10);
        test.returnConstant(fails, 20);
        test.compile();

        const Liveness& liveness = test.procedure().liveness();
        AOT_ORDER_TEST_CHECK(test, liveness.isFused(tested) && liveness.isFused(isBelow) && liveness.isFused(isEqual));
        AOT_ORDER_TEST_CHECK(test, liveness.isFused(small) + liveness.isFused(other) == isX86_64());
        for (int64_t smallWord : { 3, 4 }) {
            for (int64_t otherWord : { 7, 8 }) {
                int64_t words[] = { smallWord, otherWord };
                bool expected = isOr ? smallWord < 4 || otherWord == 7 : smallWord < 4 && otherWord == 7;
                AOT_ORDER_TEST_CHECK(test, test.run(loaded & firstIsLoaded ? otherWord : smallWord, 7, reinterpret_cast<int64_t>(words)) == (expected ? 10 : 20));
            }
        }
    }
}

static bool orderTestRelationHolds(Opcode relation, double left, double right)
{
    switch (relation) {
    case Equal:
        return left == right;
    case NotEqual:
        return left != right;
    case LessThan:
        return left < right;
    case GreaterThan:
        return left > right;
    case LessEqual:
        return left <= right;
    case GreaterEqual:
        return left >= right;
    case EqualOrUnordered:
        return !(left < right) && !(left > right);
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

// A comparison of numbers beside one of integers, in both orders. The jump for a comparison that does not hold is taken for unordered operands as well.
template<Opcode combination>
static void testMachineFusionOfBranchOnComparisonOfNumbers()
{
    constexpr bool isOr = combination == BitOr;
    static constexpr double numbers[] = { std::numeric_limits<double>::quiet_NaN(), -0.0, 0.0, 1.5, -2.0 };
    for (Opcode relation : { Equal, NotEqual, LessThan, GreaterThan, LessEqual, GreaterEqual, EqualOrUnordered }) {
        for (bool numbersComeFirst : { true, false }) {
            OrderTest test;
            Output& out = test.out;
            LValue left = test.testProcedure.argumentFPR(0);
            LValue right = test.testProcedure.argumentFPR(1);
            LBasicBlock holds = out.newBlock();
            LBasicBlock fails = out.newBlock();
            LValue ofNumbers = numbersComeFirst ? out.binary(relation, left, right) : nullptr;
            LValue ofIntegers = out.equal(test.first, test.second);
            if (!numbersComeFirst)
                ofNumbers = out.binary(relation, left, right);
            LValue tested = out.binary(combination, ofNumbers, ofIntegers);
            out.branch(tested, normalEdge(holds), normalEdge(fails));
            test.returnConstant(holds, 10);
            test.returnConstant(fails, 20);
            test.compile();

            const Liveness& liveness = test.procedure().liveness();
            AOT_ORDER_TEST_CHECK(test, liveness.isFused(tested) && liveness.isFused(ofNumbers) && liveness.isFused(ofIntegers));
            AOT_ORDER_TEST_CHECK(test, (tested->child(0) == ofNumbers) == numbersComeFirst);
            for (double leftNumber : numbers) {
                for (double rightNumber : numbers) {
                    for (int64_t second : { 7, 8 }) {
                        bool numbersHold = orderTestRelationHolds(relation, leftNumber, rightNumber);
                        bool expected = isOr ? numbersHold || second == 7 : numbersHold && second == 7;
                        AOT_ORDER_TEST_CHECK(test, test.testProcedure.call<int64_t>(int64_t { 7 }, second, int64_t { 0 }, leftNumber, rightNumber) == (expected ? 10 : 20));
                    }
                }
            }
        }
    }
}

// if ((word >> amount) & 1), as ShapeTestEmitter asks a set of bits. On x86-64 the Branch tests the bit itself: nothing is shifted, and the amount lies in any register.
// Where the shifted word is read again, it is shifted.
template<Opcode shift, TypeKind type>
static void testMachineFusionOfShiftBelowBitTest()
{
    constexpr bool isWide = type == Int64;
    static constexpr uint64_t words[] = { 0, 1, 0x80000000ull, 0x8000000000000000ull, 0xdeadbeefcafef00dull, UINT64_MAX };
    for (bool branchesWhereClear : { false, true }) {
        for (bool shiftedWordIsReadAgain : { false, true }) {
            OrderTest test;
            Output& out = test.out;
            LBasicBlock holds = out.newBlock();
            LBasicBlock fails = out.newBlock();
            LValue shifted = out.binary(shift, isWide ? test.first : out.castToInt32(test.first), out.castToInt32(test.second));
            LValue bit = out.bitAnd(shifted, 1);
            out.branch(branchesWhereClear ? out.equal(bit, 0) : bit, normalEdge(holds), normalEdge(fails));
            test.returnConstant(holds, 10);
            out.appendTo(fails);
            out.ret(shiftedWordIsReadAgain ? out.add(out.zeroExtPtr(shifted), 100) : out.int64Zero);
            test.compile();

            const Liveness& liveness = test.procedure().liveness();
            AOT_ORDER_TEST_CHECK(test, shifted->useCount() == (shiftedWordIsReadAgain ? 2 : 1) && liveness.isFused(bit));
            AOT_ORDER_TEST_CHECK(test, liveness.isFused(shifted) == (isX86_64() && !shiftedWordIsReadAgain));
            for (uint64_t word : words) {
                for (int64_t amount : { 0, 1, 5, 31, 32, 33, 63, 64, 65 }) {
                    uint64_t shiftedWord;
                    if constexpr (isWide)
                        shiftedWord = shift == ZShr ? word >> (amount & 63) : static_cast<uint64_t>(static_cast<int64_t>(word) >> (amount & 63));
                    else
                        shiftedWord = shift == ZShr ? static_cast<uint32_t>(word) >> (amount & 31) : static_cast<uint32_t>(static_cast<int32_t>(word) >> (amount & 31));
                    bool holdsIsReached = static_cast<bool>(shiftedWord & 1) != branchesWhereClear;
                    uint64_t expected = holdsIsReached ? 10 : shiftedWordIsReadAgain ? shiftedWord + 100 : 0;
                    AOT_ORDER_TEST_CHECK(test, static_cast<uint64_t>(test.run(static_cast<int64_t>(word), amount)) == expected);
                }
            }
        }
    }
}

// distance < 8 && (0xf9 >> distance) & 1, as ShapeTestEmitter asks for one of a few shapes. Neither answer is made a word, and on x86-64 nothing is shifted.
template<Opcode combination>
static void testMachineFusionOfComparisonBesideBitTest()
{
    constexpr bool isOr = combination == BitOr;
    constexpr uint32_t bits = 0xf9;
    OrderTest test;
    Output& out = test.out;
    LBasicBlock holds = out.newBlock();
    LBasicBlock fails = out.newBlock();
    LValue distance = out.castToInt32(test.first);
    LValue shifted = out.lShr(out.constInt32(bits), distance);
    LValue isInRange = out.below(distance, 8);
    LValue bit = out.bitAnd(shifted, 1);
    LValue tested = out.binary(combination, isInRange, bit);
    out.branch(tested, normalEdge(holds), normalEdge(fails));
    test.returnConstant(holds, 10);
    test.returnConstant(fails, 20);
    test.compile();

    const Liveness& liveness = test.procedure().liveness();
    AOT_ORDER_TEST_CHECK(test, liveness.isFused(tested) && liveness.isFused(isInRange) && liveness.isFused(bit) && liveness.isFused(shifted) == isX86_64());
    for (int64_t number = 0; number < 70; ++number) {
        bool isSet = bits >> (number & 31) & 1;
        bool expected = isOr ? number < 8 || isSet : number < 8 && isSet;
        AOT_ORDER_TEST_CHECK(test, test.run(number) == (expected ? 10 : 20));
    }
}

// (*byte & 64) != 0 && a >= 0. The bits are no word either, and on x86-64 the byte is tested where it lies.
template<Opcode combination>
static void testMachineFusionOfBitsOfLoadBesideComparison()
{
    constexpr bool isOr = combination == BitOr;
    OrderTest test;
    Output& out = test.out;
    LBasicBlock holds = out.newBlock();
    LBasicBlock fails = out.newBlock();
    LValue byte = out.load8ZeroExt32(out.address(test.testProcedure.heaps().argumentBuffer, test.third, 6));
    LValue masked = out.bitAnd(byte, 64);
    LValue hasBit = out.notZero32(masked);
    LValue isNotNegative = out.greaterThanOrEqual(test.first, 0);
    LValue tested = out.binary(combination, hasBit, isNotNegative);
    out.branch(tested, normalEdge(holds), normalEdge(fails));
    test.returnConstant(holds, 10);
    test.returnConstant(fails, 20);
    test.compile();

    const Liveness& liveness = test.procedure().liveness();
    AOT_ORDER_TEST_CHECK(test, liveness.isFused(tested) && liveness.isFused(hasBit) && liveness.isFused(isNotNegative) && liveness.isFused(masked) && liveness.isFused(byte) == isX86_64());
    for (uint8_t held : { 0, 64, 0xbf, 0xff }) {
        for (int64_t first : { -1, 0, 5 }) {
            uint8_t bytes[8] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, held, 0xff };
            bool expected = isOr ? (held & 64) || first >= 0 : (held & 64) && first >= 0;
            AOT_ORDER_TEST_CHECK(test, test.run(first, 0, reinterpret_cast<int64_t>(bytes)) == (expected ? 10 : 20));
        }
    }
}

// The answer of one comparison is read again, so it is a word, and the Branch tests the bits of two words.
static void testMachineFusionLeavesComparisonThatIsReadAgain()
{
    OrderTest test;
    Output& out = test.out;
    LBasicBlock holds = out.newBlock();
    LBasicBlock fails = out.newBlock();
    LValue isBelow = out.below(test.first, out.constInt64(4));
    LValue isEqual = out.equal(test.second, test.third);
    LValue tested = out.bitAnd(isBelow, isEqual);
    out.branch(tested, normalEdge(holds), normalEdge(fails));
    test.returnConstant(holds, 10);
    out.appendTo(fails);
    out.ret(out.zeroExtPtr(isBelow));
    test.compile();

    const Liveness& liveness = test.procedure().liveness();
    AOT_ORDER_TEST_CHECK(test, liveness.isFused(tested) && !liveness.isFused(isBelow) && !liveness.isFused(isEqual));
    AOT_ORDER_TEST_CHECK(test, test.run(3, 7, 7) == 10 && test.run(3, 8, 7) == 1 && !test.run(4, 7, 7));
}

#undef AOT_ORDER_TEST_CHECK

} // namespace Machine

std::span<const NativeTest> machineOrderTests()
{
    using Machine::EdgeClass;
    using Machine::OrderTestChildrenOfBits;
    using Machine::OrderTestNextTest;
    using Machine::OrderTestSecondTest;
    using Machine::OrderTestThrowBlocks;
    using Machine::OrderTestWayIntoArm;
    static constexpr NativeTest tests[] = {
        { "machine-order/rare-block-stands-ahead-of-its-rejoin-and-last-in-the-layout"_s, Machine::testMachineOrderPlacesRareBlockAheadOfItsRejoin },
        { "machine-order/rare-block-that-never-rejoins-stands-behind-its-branch"_s, Machine::testMachineOrderPlacesRareBlockThatNeverRejoinsBehindItsBranch },
        { "machine-order/unlikely-successor-stands-behind-the-normal-one"_s, Machine::testMachineOrderPlacesUnlikelySuccessorBehindNormalOne },
        { "machine-order/loop-is-contiguous"_s, Machine::testMachineOrderKeepsLoopContiguous },
        { "machine-order/loop-ends-with-its-test"_s, Machine::testMachineOrderEndsLoopWithItsTest },
        { "machine-order/loop-ends-with-the-test-behind-its-head"_s, Machine::testMachineOrderEndsLoopWithTestBehindItsHead },
        { "machine-order/loop-ends-with-its-test-where-a-rare-block-leads-back-into-it"_s, Machine::testMachineOrderEndsLoopWithItsTestWhereRareBlockLeadsBackIntoIt },
        { "machine-order/each-nested-loop-ends-with-its-test"_s, Machine::testMachineOrderEndsEachNestedLoopWithItsTest },
        { "machine-order/loop-written-with-its-test-at-the-end-keeps-it-there"_s, Machine::testMachineOrderLeavesLoopThatEndsWithItsTest },
        { "machine-order/loop-ends-with-a-test-that-leaves-it-rarely"_s, Machine::testMachineOrderEndsLoopWithTestThatLeavesRarely },
        { "machine-order/loop-ends-with-a-test-whose-way-out-does-not-fall-through"_s, Machine::testMachineOrderEndsLoopWithTestWhoseWayOutDoesNotFallThrough<0> },
        { "machine-order/conditional-jump-goes-round-the-loop-where-the-second-successor-stays"_s, Machine::testMachineOrderEndsLoopWithTestWhoseWayOutDoesNotFallThrough<1> },
        { "machine-order/unlikely-arm-stands-behind-the-likely-blocks"_s, Machine::testMachineOrderPlacesUnlikelyArmBehindLikelyBlocks },
        { "machine-order/unlikely-arm-stands-outside-its-loop"_s, Machine::testMachineOrderTakesUnlikelyArmOutOfLoop },
        { "machine-order/nested-loops-are-nested-intervals"_s, Machine::testMachineOrderMakesNestedLoopsNestedIntervals },
        { "machine-order/cycle-entered-in-two-places-is-one-interval"_s, Machine::testMachineOrderCoversCycleEnteredInTwoPlaces },
        { "machine-order/every-entrypoint-is-a-start"_s, Machine::testMachineOrderStartsAtEveryEntrypoint },
        { "machine-order/rare-block-lies-within-the-interval-of-its-loop"_s, Machine::testMachineOrderKeepsRareBlockWithinItsLoop },
        { "machine-order/rare-block-that-leads-back-is-covered-by-the-loop"_s, Machine::testMachineOrderCoversRareBlockThatLeadsBack },
        { "machine-order/block-stands-ahead-of-what-it-leads-to-through-rare-blocks"_s, Machine::testMachineOrderPlacesBlockAheadOfWhatItLeadsToThroughRareBlocks },
        { "machine-order/arm-that-goes-round-through-a-rare-block-lies-within-its-loop"_s, Machine::testMachineOrderKeepsArmThatGoesRoundThroughRareBlockWithinItsLoop },
        { "machine-order/critical-edge-is-split"_s, Machine::testMachineOrderSplitsCriticalEdge },
        { "machine-order/edge-that-gets-no-move-stays-whole"_s, Machine::testMachineOrderLeavesEdgeThatGetsNoMoveWhole },
        { "machine-order/rare-edges-into-one-rare-block-stay-whole"_s, Machine::testMachineOrderLeavesRareEdgesIntoOneRareBlockWhole },
        { "machine-order/rare-edges-into-a-block-that-its-region-leads-to-are-split"_s, Machine::testMachineOrderSplitsRareEdgesIntoBlockThatItsRegionLeadsTo },
        { "machine-order/two-edges-to-one-block-are-split-apart"_s, Machine::testMachineOrderSplitsEachOfTwoEdgesToOneBlock },
        { "machine-order/last-of-several-edges-of-a-switch-to-one-block-stays-whole"_s, Machine::testMachineOrderLeavesLastOfSeveralEdgesOfSwitchToOneBlockWhole },
        { "machine-order/work-grows-as-the-edges-of-a-switch-do"_s, Machine::testMachineOrderGrowsAsEdgesOfSwitchDo },
        { "machine-order/edge-to-a-block-with-phis-and-one-predecessor-is-split"_s, Machine::testMachineOrderSplitsEdgeToBlockWithPhisAndOnePredecessor },
        { "machine-order/edges-to-a-landing-pad-stay-whole"_s, Machine::testMachineOrderLeavesEdgesToLandingPadWhole },
        { "machine-order/edge-of-a-patch-to-a-join-is-split"_s, Machine::testMachineOrderSplitsEdgeOfPatchToJoin },
        { "machine-order/edge-that-holds-the-extracts-of-a-patch-stays-whole"_s, Machine::testMachineOrderLeavesEdgeThatHoldsExtractsWhole },
        { "machine-order/each-throwing-arm-under-a-handler-is-its-own-region"_s, Machine::testMachineOrderGivesEachThrowingArmItsOwnRegion },
        { "machine-order/landing-pad-reached-only-from-arms-stands-behind-the-last-arm"_s, Machine::testMachineOrderPlacesLandingPadBehindLastThrowingArm },
        { "machine-order/nested-handler-stands-ahead-of-its-rejoin"_s, Machine::testMachineOrderPlacesNestedHandlerAheadOfItsRejoin },
        { "machine-order/landing-pads-that-lead-into-each-other-are-one-region"_s, Machine::testMachineOrderJoinsLandingPadsThatLeadIntoEachOther },
        { "machine-order/throwing-arms-are-one-region-without-a-frame-at-entry"_s, Machine::testMachineOrderJoinsThrowingArmsWithoutFrameAtEntry },
        { "machine-order/blocks-that-nothing-reaches-vanish"_s, Machine::testMachineOrderDropsBlocksThatNothingReaches },
        { "machine-order/branch-on-a-join-of-constants-is-threaded"_s, Machine::testMachineThreadingResolvesBranchOnJoinOfConstants },
        { "machine-order/only-reached-edge-into-a-join-of-constants-keeps-its-class"_s, Machine::testMachineThreadingKeepsClassOfOnlyReachedEdge<1> },
        { "machine-order/only-reached-edge-keeps-its-class-through-two-joins"_s, Machine::testMachineThreadingKeepsClassOfOnlyReachedEdge<2> },
        { "machine-order/block-that-passes-no-constant-tests-it-ahead-of-the-join"_s, Machine::testMachineThreadingTestsValueAheadOfJoin },
        { "machine-order/comparisons-ahead-of-a-join-are-fused-into-branches"_s, Machine::testMachineThreadingFusesComparisonsAheadOfJoin },
        { "machine-order/edge-of-a-branch-that-passes-no-constant-stays"_s, Machine::testMachineThreadingLeavesEdgeOfBranchThatPassesNoConstant },
        { "machine-order/join-that-hands-its-phi-to-a-second-join-is-threaded"_s, Machine::testMachineThreadingPassesJoinThatHandsItsPhiOn },
        { "machine-order/join-whose-phi-is-read-again-stays"_s, Machine::testMachineThreadingKeepsJoinWhosePhiIsReadAgain },
        { "machine-order/join-that-holds-an-upsilon-stays"_s, Machine::testMachineThreadingKeepsJoinThatHoldsUpsilon },
        { "machine-order/join-that-holds-a-store-stays"_s, Machine::testMachineThreadingKeepsJoinThatHoldsStore },
        { "machine-order/join-that-holds-an-unread-comparison-is-threaded"_s, Machine::testMachineThreadingPassesJoinThatHoldsUnreadComparison },
        { "machine-order/head-of-a-loop-is-not-threaded"_s, Machine::testMachineThreadingLeavesHeadOfLoop },
        { "machine-order/way-that-has-decided-a-test-passes-it-by"_s, Machine::testMachineThreadingOfTestThatWayHasDecided<OrderTestSecondTest::IsAlone> },
        { "machine-order/way-that-has-decided-a-test-passes-its-negation-by"_s, Machine::testMachineThreadingOfTestThatWayHasDecided<OrderTestSecondTest::IsAskedTheOtherWayRound> },
        { "machine-order/decided-test-behind-a-store-stays"_s, Machine::testMachineThreadingOfTestThatWayHasDecided<OrderTestSecondTest::StandsBehindStore> },
        { "machine-order/decided-test-whose-answer-is-read-again-stays"_s, Machine::testMachineThreadingOfTestThatWayHasDecided<OrderTestSecondTest::IsReadAgain> },
        { "machine-order/test-stays-for-the-way-that-has-not-decided-it"_s, Machine::testMachineThreadingOfTestThatWayHasDecided<OrderTestSecondTest::IsComeToByThirdWay> },
        { "machine-order/word-is-read-through-the-object-that-a-flag-asked-twice-has-picked"_s, Machine::testMachineThreadingOfValuesPickedByOneFlag<Machine::OrderTestUseOfPick::Reads> },
        { "machine-order/word-is-written-through-the-object-that-a-flag-asked-twice-has-picked"_s, Machine::testMachineThreadingOfValuesPickedByOneFlag<Machine::OrderTestUseOfPick::Stores> },
        { "machine-order/words-are-read-again-behind-a-decided-test-whose-arms-have-stored"_s, Machine::testMachineThreadingPastTestBehindArmsThatStore },
        { "machine-order/decided-test-at-the-head-of-a-loop-stays"_s, Machine::testMachineThreadingLeavesDecidedTestAtHeadOfLoop },
        { "machine-order/work-grows-as-the-decided-tests-in-a-row-do"_s, Machine::testMachineThreadingGrowsAsTestsInRowDo<true> },
        { "machine-order/work-grows-as-the-undecided-tests-in-a-row-do"_s, Machine::testMachineThreadingGrowsAsTestsInRowDo<false> },
        { "machine-order/test-for-zero-is-asked-behind-a-test-that-zero-fails-too"_s, Machine::testMachineThreadingOfTestThatNextTestImplies<OrderTestNextTest::LeavesForZeroToo> },
        { "machine-order/test-for-zero-is-asked-behind-a-test-that-leaves-by-its-first-way"_s, Machine::testMachineThreadingOfTestThatNextTestImplies<OrderTestNextTest::LeavesForZeroTooByItsFirstWay> },
        { "machine-order/test-for-zero-stays-ahead-of-a-test-that-zero-passes"_s, Machine::testMachineThreadingOfTestThatNextTestImplies<OrderTestNextTest::LetsZeroPass> },
        { "machine-order/test-for-zero-stays-ahead-of-a-load"_s, Machine::testMachineThreadingOfTestThatNextTestImplies<OrderTestNextTest::StandsBehindLoad> },
        { "machine-order/test-for-zero-stays-ahead-of-a-test-that-another-way-enters"_s, Machine::testMachineThreadingOfTestThatNextTestImplies<OrderTestNextTest::IsEnteredByAnotherWay> },
        { "machine-order/test-of-bits-is-asked-behind-a-bound-that-words-without-them-fail"_s, Machine::testMachineThreadingOfTestThatNextTestImplies<OrderTestNextTest::FollowsTestOfBits> },
        { "machine-order/test-of-bits-stays-ahead-of-a-bound-that-a-word-without-them-passes"_s, Machine::testMachineThreadingOfTestThatNextTestImplies<OrderTestNextTest::FollowsTestOfBitsThatReachAboveItsBound> },
        { "machine-order/edge-that-passes-a-rare-jump-is-rare"_s, Machine::testMachineThreadingPassesBlocksThatOnlyJump<1, EdgeClass::Rare> },
        { "machine-order/edge-passes-a-chain-of-jumps"_s, Machine::testMachineThreadingPassesBlocksThatOnlyJump<3, EdgeClass::Normal> },
        { "machine-order/cycle-of-jumps-ends-the-search"_s, Machine::testMachineThreadingEndsInCycleOfJumps },
        { "machine-order/identical-throw-blocks-merge"_s, Machine::testMachineMergingOfThrowBlocks<OrderTestThrowBlocks::AreAlike> },
        { "machine-order/throw-blocks-with-different-origins-stay-apart"_s, Machine::testMachineMergingOfThrowBlocks<OrderTestThrowBlocks::DifferInOrigin> },
        { "machine-order/throw-blocks-with-generators-of-their-own-stay-apart"_s, Machine::testMachineMergingOfThrowBlocks<OrderTestThrowBlocks::DifferInGenerator> },
        { "machine-order/blocks-merge-by-their-children"_s, Machine::testMachineMergingComparesChildren },
        { "machine-order/arms-of-an-emitter-that-go-on-in-a-scope-merge"_s, Machine::testMachineMergingOfBlocksThatContinueScope<OrderTestWayIntoArm::AsArmOfEmitter> },
        { "machine-order/blocks-of-code-that-go-on-in-a-scope-merge"_s, Machine::testMachineMergingOfBlocksThatContinueScope<OrderTestWayIntoArm::AsSuccessorInCode> },
        { "machine-order/rare-arm-and-likely-arm-merge-into-a-likely-block"_s, Machine::testMachineMergingOfRareArmWithLikelyArm },
        { "machine-order/test-read-by-two-arms-that-merge-is-made-by-the-branch-that-is-left"_s, Machine::testMachineMergingLeavesTestToItsOneReader },
        { "machine-order/arms-that-pass-one-value-merge"_s, Machine::testMachineMergingComparesUpsilons<true> },
        { "machine-order/arms-that-pass-different-values-stay-apart"_s, Machine::testMachineMergingComparesUpsilons<false> },
        { "machine-order/test-whose-arms-merge-goes"_s, Machine::testMachineOrderDropsTestWhoseArmsMerge },
        { "machine-order/value-tested-for-nothing-stays-for-its-other-reader"_s, Machine::testMachineOrderKeepsWhatElseReadsTestedValue },
        { "machine-order/moves-stand-ahead-of-a-branch-that-tests-nothing"_s, Machine::testMachineOrderMovesRegistersAheadOfBranchThatTestsNothing },
        { "machine-order/branch-that-tests-nothing-leaves-a-rare-region"_s, Machine::testMachineOrderLeavesRareRegionThroughBranchThatTestsNothing },
        { "machine-order/test-in-a-later-block-absorbs-bits-whose-children-live-on"_s, Machine::testMachineFusionOfTestInLaterBlock<OrderTestChildrenOfBits::TwoThatAreReadAgain> },
        { "machine-order/test-in-a-later-block-absorbs-bits-whose-one-child-dies"_s, Machine::testMachineFusionOfTestInLaterBlock<OrderTestChildrenOfBits::OneThatIsNotReadAgain> },
        { "machine-order/test-in-a-later-block-leaves-bits-whose-two-children-die"_s, Machine::testMachineFusionOfTestInLaterBlock<OrderTestChildrenOfBits::TwoThatAreNotReadAgain> },
        { "machine-order/test-in-a-later-block-leaves-a-comparison-of-a-number-that-dies"_s, Machine::testMachineFusionLeavesComparisonOfNumberThatIsNotReadAgain },
        { "machine-order/branch-on-both-of-two-comparisons-is-two-jumps"_s, Machine::testMachineFusionOfBranchOnTwoComparisons<Machine::BitAnd> },
        { "machine-order/branch-on-either-of-two-comparisons-is-two-jumps"_s, Machine::testMachineFusionOfBranchOnTwoComparisons<Machine::BitOr> },
        { "machine-order/one-of-two-comparisons-that-both-have-to-hold-reads-its-load-where-it-lies"_s, Machine::testMachineFusionOfLoadBelowTwoComparisons<Machine::BitAnd> },
        { "machine-order/one-of-two-comparisons-either-of-which-is-enough-reads-its-load-where-it-lies"_s, Machine::testMachineFusionOfLoadBelowTwoComparisons<Machine::BitOr> },
        { "machine-order/branch-on-both-a-comparison-of-numbers-and-one-of-integers"_s, Machine::testMachineFusionOfBranchOnComparisonOfNumbers<Machine::BitAnd> },
        { "machine-order/branch-on-either-a-comparison-of-numbers-or-one-of-integers"_s, Machine::testMachineFusionOfBranchOnComparisonOfNumbers<Machine::BitOr> },
        { "machine-order/branch-on-one-bit-of-a-word-of-32-bits-shifts-nothing"_s, Machine::testMachineFusionOfShiftBelowBitTest<Machine::ZShr, Machine::Int32> },
        { "machine-order/branch-on-one-bit-of-a-word-of-64-bits-shifts-nothing"_s, Machine::testMachineFusionOfShiftBelowBitTest<Machine::ZShr, Machine::Int64> },
        { "machine-order/branch-on-one-bit-of-a-signed-word-of-32-bits-shifts-nothing"_s, Machine::testMachineFusionOfShiftBelowBitTest<Machine::SShr, Machine::Int32> },
        { "machine-order/branch-on-one-bit-of-a-signed-word-of-64-bits-shifts-nothing"_s, Machine::testMachineFusionOfShiftBelowBitTest<Machine::SShr, Machine::Int64> },
        { "machine-order/branch-on-both-a-comparison-and-one-bit-tests-twice"_s, Machine::testMachineFusionOfComparisonBesideBitTest<Machine::BitAnd> },
        { "machine-order/branch-on-either-a-comparison-or-one-bit-tests-twice"_s, Machine::testMachineFusionOfComparisonBesideBitTest<Machine::BitOr> },
        { "machine-order/branch-on-both-bits-of-a-load-and-a-comparison-tests-twice"_s, Machine::testMachineFusionOfBitsOfLoadBesideComparison<Machine::BitAnd> },
        { "machine-order/branch-on-either-bits-of-a-load-or-a-comparison-tests-twice"_s, Machine::testMachineFusionOfBitsOfLoadBesideComparison<Machine::BitOr> },
        { "machine-order/comparison-that-is-read-again-stays-a-word-beside-a-branch"_s, Machine::testMachineFusionLeavesComparisonThatIsReadAgain },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
