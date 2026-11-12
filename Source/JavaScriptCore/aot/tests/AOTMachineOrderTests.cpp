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

namespace JSC { namespace AOT { namespace Machine {

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

[[noreturn]] static void failOrderTest(const OrderTest& test, const char* file, int line, const char* condition)
{
    dataLogLn(test.testProcedure.printedProcedure(), test.printedOrder());
    failNativeTest(file, line, condition);
}

#define AOT_ORDER_TEST_CHECK(test, ...) do { \
        if (!(__VA_ARGS__)) [[unlikely]] \
            failOrderTest(test, __FILE__, __LINE__, #__VA_ARGS__); \
    } while (false)

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

static void orderTestCheckLayout(OrderTest& test)
{
    const Order& order = test.procedure().order();
    std::span<Block* const> layout = order.blocksInLayout();
    Vector<bool> isLikely = orderTestFindLikelyBlocks(test);
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
    for (uint32_t index = 0; index < layout.size(); ++index) {
        AOT_ORDER_TEST_CHECK(test, static_cast<int>(order.of(layout[index]).layoutPart) == partOf(layout[index]));
        AOT_ORDER_TEST_CHECK(test, !index || partOf(layout[index - 1]) <= partOf(layout[index]));
        if (order.of(layout[index]).wasSplitLate)
            continue;
        bool precedesOrder = std::ranges::any_of(layout.subspan(index + 1), [&](const Block* behind) {
            return !order.of(behind).wasSplitLate && partOf(behind) == partOf(layout[index]) && rankOf(behind) < rankOf(layout[index]);
        });
        if (!precedesOrder)
            continue;
        uint32_t front = index;
        do
            --front;
        while (order.of(layout[front]).wasSplitLate);
        AOT_ORDER_TEST_CHECK(test, !partOf(layout[index]) && std::ranges::any_of(layout[front]->successors(), [&](const Edge& edge) {
            const Block* target = order.of(edge.block()).wasSplitLate ? edge.block()->successor(0).block() : edge.block();
            return target == layout[index];
        }));
    }
}

template<bool isInLoop>
static void testMachineOrderMovesShortRunAfterItsJump()
{
    OrderTest test;
    Output& out = test.out;
    LBasicBlock head = out.newBlock();
    LBasicBlock turn = out.newBlock();
    LBasicBlock check = out.newBlock();
    LBasicBlock slow = out.newBlock();
    LBasicBlock other = out.newBlock();
    LBasicBlock answer = out.newBlock();
    LBasicBlock join = out.newBlock();
    LBasicBlock exit = out.newBlock();
    LValue index = out.phiIn(head, Int64);
    LValue sum = out.phiIn(head, Int64);
    LValue amount = out.phiIn(other, Int64);
    LValue step = out.phiIn(join, Int64);
    out.upsilon(out.int64Zero, index);
    out.upsilon(out.int64Zero, sum);
    out.jump(head);
    out.appendTo(head);
    out.branch(out.equal(index, test.first), normalEdge(turn), normalEdge(check));
    test.passAndJump(turn, out.constInt64(100), amount, other);
    out.appendTo(check);
    out.branch(out.equal(index, test.second), rareEdge(slow), normalEdge(answer));
    out.appendTo(slow);
    out.upsilon(out.constInt64(1000), amount);
    out.branch(out.equal(index, test.third), normalEdge(answer), normalEdge(other));
    out.appendTo(other);
    out.upsilon(out.add(amount, out.constInt64(1)), step);
    out.jump(join);
    out.appendTo(answer);
    out.upsilon(out.add(index, out.constInt64(1)), step);
    out.jump(join);
    out.appendTo(join);
    LValue nextIndex = out.add(index, out.constInt64(1));
    LValue nextSum = out.add(sum, step);
    if constexpr (isInLoop) {
        out.upsilon(nextIndex, index);
        out.upsilon(nextSum, sum);
        out.branch(out.below(nextIndex, out.constInt64(8)), normalEdge(head), normalEdge(exit));
    } else
        out.jump(exit);
    test.returnValue(exit, nextSum);
    const Order& order = test.compile();

    AOT_ORDER_TEST_CHECK(test, order.of(answer).layoutIndex == order.of(check).layoutIndex + 1);
    AOT_ORDER_TEST_CHECK(test, order.of(other).layoutIndex == order.of(turn).layoutIndex + 1);
    auto expected = [](int64_t first, int64_t secondValue, int64_t third) {
        int64_t result = 0;
        for (int64_t i = 0; i < (isInLoop ? 8 : 1); ++i)
            result += i == first ? 101 : i == secondValue && i != third ? 1001 : i + 1;
        return result;
    };
    for (int64_t first = -1; first < 9; ++first) {
        for (int64_t secondValue = -1; secondValue < 9; ++secondValue) {
            for (int64_t third : { secondValue, secondValue + 1 })
                AOT_ORDER_TEST_CHECK(test, test.run(first, secondValue, third) == expected(first, secondValue, third));
        }
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

static void testMachineOrderGrowsAsEdgesOfSwitchDo()
{
    constexpr unsigned numberOfCases = 5000;
    std::optional<uint64_t> smaller = orderTestInstructionsForSwitchWith(numberOfCases);
    std::optional<uint64_t> large = orderTestInstructionsForSwitchWith(4 * numberOfCases);
    AOT_TEST_CHECK(growsAsSizeDoes(smaller, large));
}

static ProcedureTraits orderTestTraitsWithHandlers()
{
    ProcedureTraits traits;
    traits.hasHandlers = true;
    return traits;
}

static PatchGenerator* orderTestCreateCallGenerator(OrderTest& test)
{
    return test.procedure().createGenerator([](CCallHelpers&, const PatchParameters&) { });
}

static void orderTestAppendThrowingCall(Output& out, PatchGenerator* generator, LBasicBlock continuation, LBasicBlock landingPad)
{
    PatchOperation* call = out.patchpoint(Void);
    call->setGenerator(generator);
    out.appendSuccessor(normalEdge(continuation));
    out.appendSuccessor(rareEdge(landingPad));
    call->clobberLateOnSuccessor(1, RegisterSet());
    call->effects.terminal = true;
}

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

struct OrderTestBooleanJoin {
    explicit OrderTestBooleanJoin(OrderTest& test)
        : OrderTestBooleanJoin(test, test.out.below(test.first, test.second), EdgeClass::Normal)
    {
    }

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
        AOT_ORDER_TEST_CHECK(test, !order.of(blocks.join).isReached() && blocks.taken->isEmpty() && !blocks.taken->numSuccessors());
    AOT_ORDER_TEST_CHECK(test, test.run(0) == 20);
}

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

enum class OrderTestSecondTest : uint8_t {
    IsAlone,
    IsAskedTheOtherWayRound,
    StandsBehindStore,
    IsReadAgain,
    IsComeToByThirdWay,
    IsBehindWaysThatBlockWithoutWayInLeadsTo,
    IsReadInBlockWithoutWayIn,
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
    if (secondTest == OrderTestSecondTest::IsBehindWaysThatBlockWithoutWayInLeadsTo) {
        out.appendTo(out.newBlock());
        out.branch(out.equal(test.third, out.constInt64(4)), normalEdge(usual), normalEdge(slow));
    }
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
    if (secondTest == OrderTestSecondTest::IsReadInBlockWithoutWayIn) {
        out.appendTo(out.newBlock());
        out.ret(out.zeroExtPtr(asked));
    }
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

static void testMachineThreadingCutsThroughLoopWhoseEntranceHasGone()
{
    OrderTest test;
    Output& out = test.out;
    LBasicBlock five = out.newBlock();
    LBasicBlock seven = out.newBlock();
    LBasicBlock asks = out.newBlock();
    LBasicBlock into = out.newBlock();
    LBasicBlock head = out.newBlock();
    LBasicBlock fiveInTurn = out.newBlock();
    LBasicBlock sevenInTurn = out.newBlock();
    LBasicBlock asksInTurn = out.newBlock();
    LBasicBlock round = out.newBlock();
    LBasicBlock behind = out.newBlock();
    LValue lane = out.phiIn(asks, Int64);
    LValue gate = out.phiIn(asksInTurn, Int64);
    auto arm = [&](LBasicBlock block, int64_t handed, LValue phi, LBasicBlock join) {
        out.appendTo(block);
        out.store64(out.constInt64(handed), out.address(test.testProcedure.heaps().argumentBuffer, test.third));
        out.upsilon(out.constInt64(handed), phi);
        out.jump(join);
    };
    out.branch(out.above(test.first, test.second), normalEdge(five), normalEdge(seven));
    arm(five, 5, lane, asks);
    arm(seven, 7, lane, asks);
    out.appendTo(asks);
    out.branch(out.equal(lane, out.constInt64(6)), normalEdge(into), normalEdge(behind));
    out.appendTo(into);
    out.jump(head);
    out.appendTo(head);
    out.branch(out.below(test.first, test.second), normalEdge(fiveInTurn), normalEdge(sevenInTurn));
    arm(fiveInTurn, 5, gate, asksInTurn);
    arm(sevenInTurn, 7, gate, asksInTurn);
    out.appendTo(asksInTurn);
    out.branch(out.equal(gate, out.constInt64(6)), normalEdge(round), normalEdge(behind));
    out.appendTo(round);
    out.jump(head);
    test.returnConstant(behind, 9);
    const Order& order = test.compile();

    AOT_ORDER_TEST_CHECK(test, orderTestTargetOf(five, 0) == behind && orderTestTargetOf(seven, 0) == behind);
    AOT_ORDER_TEST_CHECK(test, !order.of(asks).isReached() && !order.of(head).isReached() && !order.of(asksInTurn).isReached() && !order.of(round).isReached());
    int64_t word = 0;
    AOT_ORDER_TEST_CHECK(test, test.run(2, 1, reinterpret_cast<int64_t>(&word)) == 9 && word == 5);
    AOT_ORDER_TEST_CHECK(test, test.run(1, 2, reinterpret_cast<int64_t>(&word)) == 9 && word == 7);
}

enum OrderTestWordOfObject : unsigned {
    OrderTestKey,
    OrderTestNumber,
    OrderTestChild,
};

static TypedPointer orderTestWordOf(OrderTest& test, LValue object, OrderTestWordOfObject word)
{
    return test.out.address(test.testProcedure.heaps().argumentBuffer, object, word * sizeof(int64_t));
}

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

enum class OrderTestWordAskedTwice : uint8_t {
    IsWrittenNowhere,
    IsStoredBehindFirstTest,
    IsStoredFartherOn,
    IsWrittenByPatch,
    IsWrittenByPatchThatSaysOnlyThatItMayCollect,
    IsWrittenByPatchThatSaysNothing,
    IsWrittenByPatchThatIsDeclaredPure,
    IsStoredInClosedArm,
    IsStoredBetweenFirstLoadAndFirstTest,
    IsStoredBetweenBlocksOfFirstLoadAndFirstTest,
    IsHeldAgainstAnotherConstant,
    IsReadOffAnotherCell,
    IsAnotherWordOfCell,
    IsReadOffWhatJoinBrings,
    IsReadOffStorage,
    IsReadBelowToo,
    StandsBehindKeeper,
    StandsBehindShiftByVariable,
    IsFilledOnceInClosedArm,
};

template<OrderTestWordAskedTwice kind>
static void testMachineThreadingOfTestOfWordInMemory()
{
    using enum OrderTestWordAskedTwice;
    constexpr bool asksTheSame = kind != IsHeldAgainstAnotherConstant && kind != IsReadOffAnotherCell && kind != IsAnotherWordOfCell && kind != IsReadOffWhatJoinBrings && kind != IsReadOffStorage && kind != IsReadBelowToo;
    constexpr bool usualWayPasses = kind == IsWrittenNowhere || kind == IsStoredInClosedArm || kind == StandsBehindKeeper || kind == IsFilledOnceInClosedArm || (kind == StandsBehindShiftByVariable && !isX86_64());
    constexpr bool closedWayPasses = asksTheSame && kind != IsStoredInClosedArm && kind != IsStoredBetweenFirstLoadAndFirstTest && kind != IsStoredBetweenBlocksOfFirstLoadAndFirstTest;
    OrderTest test;
    Output& out = test.out;
    LBasicBlock usual = out.newBlock();
    LBasicBlock goesOn = out.newBlock();
    LBasicBlock slow = out.newBlock();
    LBasicBlock again = out.newBlock();
    LBasicBlock hit = out.newBlock();
    LBasicBlock usualAgain = out.newBlock();
    LBasicBlock slowAgain = out.newBlock();
    LValue brought = kind == IsReadOffWhatJoinBrings ? out.phiIn(again, Int64) : nullptr;
    auto storeKey = [&](int64_t stored) {
        out.store64(out.constInt64(stored), orderTestWordOf(test, test.first, OrderTestKey));
    };
    auto leaveFor = [&](int64_t third, LBasicBlock next) {
        out.branch(out.equal(test.third, out.constInt64(third)), normalEdge(hit), normalEdge(next));
    };
    LValue storage = nullptr;
    if (kind == IsReadOffStorage) {
        AbstractHeap storageOfOwner(test.testProcedure.heaps().argumentBuffer.index(), 0, OptionSet<HeapTrait> { HeapTrait::HoldsInnerPointer });
        storage = out.load64(out.address(storageOfOwner, test.first, OrderTestChild * sizeof(int64_t)));
    }
    AbstractHeap filledOnce(test.testProcedure.heaps().argumentBuffer.index(), 0, OptionSet<HeapTrait> { HeapTrait::Immutable, HeapTrait::IsFilledOnce });
    auto wordAsked = [&](LValue object, OrderTestWordOfObject word) {
        return kind == IsFilledOnceInClosedArm ? out.address(filledOnce, object, word * sizeof(int64_t)) : orderTestWordOf(test, object, word);
    };
    LValue key = out.load64(wordAsked(storage ? storage : test.first, OrderTestKey));
    if (kind == IsStoredBetweenFirstLoadAndFirstTest)
        storeKey(8);
    if (kind == IsStoredBetweenBlocksOfFirstLoadAndFirstTest) {
        LBasicBlock stores = out.newBlock();
        LBasicBlock asks = out.newBlock();
        out.branch(out.equal(test.third, out.constInt64(13)), normalEdge(asks), normalEdge(stores));
        out.appendTo(stores);
        storeKey(8);
        out.jump(asks);
        out.appendTo(asks);
    }
    out.branch(out.equal(key, out.constInt64(7)), normalEdge(usual), rareEdge(slow));
    out.appendTo(usual);
    if (kind == IsStoredBehindFirstTest)
        storeKey(8);
    leaveFor(12, goesOn);
    out.appendTo(goesOn);
    if (kind == IsStoredFartherOn)
        storeKey(8);
    if (kind == IsWrittenByPatch || kind == IsWrittenByPatchThatSaysOnlyThatItMayCollect || kind == IsWrittenByPatchThatSaysNothing) {
        PatchOperation* patch = out.patch(Void);
        patch->appendSomeRegister(test.first);
        patch->appendSomeRegister(out.constInt64(8));
        patch->effects = Effects::none();
        patch->setRunsWithoutFrame(true);
        patch->setShowsRegistersToCollector(true);
        patch->setGenerator([](CCallHelpers& jit, const PatchParameters& params) {
            jit.store64(params[1].gpr(), CCallHelpers::Address(params[0].gpr(), OrderTestKey * sizeof(int64_t)));
        });
        if (kind == IsWrittenByPatch) {
            const AbstractHeap* written[] = { &test.testProcedure.heaps().argumentBuffer };
            patch->setWrites(written);
        } else if (kind == IsWrittenByPatchThatSaysOnlyThatItMayCollect)
            patch->effects.mayCollect = true;
        else
            patch->effects.writesLocalState = true;
    }
    if (kind == StandsBehindKeeper) {
        PatchOperation* keeper = out.patch(Void);
        keeper->append(test.first, ValueRep::ColdAny);
        keeper->effects = Effects::none();
        keeper->effects.writesLocalState = true;
        keeper->effects.reads = HeapSet::top();
        keeper->setRunsWithoutFrame(true);
        keeper->onlyKeepsChildrenAlive = true;
        keeper->setGenerator([](CCallHelpers&, const PatchParameters&) { });
    }
    if (brought)
        out.upsilon(test.first, brought);
    if (kind == StandsBehindShiftByVariable)
        out.branch(out.equal(out.shl(test.third, out.castToInt32(test.third)), out.constInt64(3 << 3)), normalEdge(hit), normalEdge(again));
    else if (kind == IsWrittenByPatchThatIsDeclaredPure) {
        PatchOperation* patch = out.patch(Int64);
        patch->appendSomeRegister(test.first);
        patch->appendSomeRegister(out.constInt64(8));
        patch->resultConstraints = { ValueRep::SomeEarlyRegister };
        patch->effects = Effects::none();
        patch->pureFunction = PureFunction { PureFunction::firstIDOfBackEnd + 4711 };
        patch->setRunsWithoutFrame(true);
        patch->setGenerator([](CCallHelpers& jit, const PatchParameters& params) {
            jit.store64(params[2].gpr(), CCallHelpers::Address(params[1].gpr(), OrderTestKey * sizeof(int64_t)));
            jit.move(CCallHelpers::TrustedImm32(3), params[0].gpr());
        });
        out.branch(out.equal(test.third, patch), normalEdge(hit), normalEdge(again));
    } else
        leaveFor(3, again);
    out.appendTo(slow);
    if (kind == IsStoredInClosedArm || kind == IsFilledOnceInClosedArm)
        storeKey(7);
    if (brought)
        out.upsilon(test.second, brought);
    leaveFor(2, again);
    out.appendTo(again);
    LValue cell = kind == IsReadOffAnotherCell ? test.second : brought ? brought : storage ? storage : test.first;
    LValue readAgain = out.load64(wordAsked(cell, kind == IsAnotherWordOfCell ? OrderTestNumber : OrderTestKey));
    out.branch(out.equal(readAgain, out.constInt64(kind == IsHeldAgainstAnotherConstant ? 8 : 7)), normalEdge(usualAgain), rareEdge(slowAgain));
    out.appendTo(usualAgain);
    out.ret(kind == IsReadBelowToo ? out.add(readAgain, out.constInt64(3)) : out.constInt64(10));
    test.returnConstant(slowAgain, 20);
    test.returnConstant(hit, 30);
    const Order& order = test.compile();

    AOT_ORDER_TEST_CHECK(test, brought || (orderTestTargetOf(goesOn, 1) == (usualWayPasses ? usualAgain : again) && orderTestTargetOf(slow, 1) == (closedWayPasses ? slowAgain : again)));
    AOT_ORDER_TEST_CHECK(test, order.of(again).isReached() == !(usualWayPasses && closedWayPasses));
    for (int64_t keyOfFirst : { 7, 8, 5 }) {
        for (int64_t keyOfSecond : { 7, 5 }) {
            for (int64_t third : { 0, 1, 2, 3, 11, 12, 13 }) {
                int64_t first[] { keyOfFirst, 7, 0 };
                first[OrderTestChild] = reinterpret_cast<int64_t>(first);
                int64_t second[] { keyOfSecond, 0, 0 };
                bool passesFirst = keyOfFirst == 7;
                int64_t expected;
                if (passesFirst ? third == 3 || third == 12 : third == 2)
                    expected = 30;
                else {
                    int64_t held = keyOfFirst;
                    if (kind == IsStoredBetweenFirstLoadAndFirstTest || (kind == IsStoredBetweenBlocksOfFirstLoadAndFirstTest && third != 13) || (passesFirst && (kind == IsStoredBehindFirstTest || kind == IsStoredFartherOn || kind == IsWrittenByPatch || kind == IsWrittenByPatchThatSaysOnlyThatItMayCollect || kind == IsWrittenByPatchThatSaysNothing || kind == IsWrittenByPatchThatIsDeclaredPure)))
                        held = 8;
                    if (!passesFirst && kind == IsStoredInClosedArm)
                        held = 7;
                    if (kind == IsReadOffAnotherCell || (kind == IsReadOffWhatJoinBrings && !passesFirst))
                        held = keyOfSecond;
                    if (kind == IsAnotherWordOfCell)
                        held = 7;
                    expected = held == (kind == IsHeldAgainstAnotherConstant ? 8 : 7) ? 10 : 20;
                }
                AOT_ORDER_TEST_CHECK(test, test.run(reinterpret_cast<int64_t>(first), reinterpret_cast<int64_t>(second), third) == expected);
            }
        }
    }
}

template<bool isStoredBetween>
static void testMachineThreadingOfBitsOfByteInMemory()
{
    OrderTest test;
    Output& out = test.out;
    LBasicBlock usual = out.newBlock();
    LBasicBlock slow = out.newBlock();
    LBasicBlock again = out.newBlock();
    LBasicBlock hit = out.newBlock();
    LBasicBlock usualAgain = out.newBlock();
    LBasicBlock slowAgain = out.newBlock();
    auto bitIsSet = [&] {
        return out.testNonZero32(out.load8ZeroExt32(orderTestWordOf(test, test.first, OrderTestKey)), out.constInt32(4));
    };
    out.branch(bitIsSet(), normalEdge(usual), rareEdge(slow));
    out.appendTo(usual);
    if (isStoredBetween)
        out.store64(out.constInt64(3), orderTestWordOf(test, test.first, OrderTestKey));
    out.branch(out.equal(test.third, out.constInt64(3)), normalEdge(hit), normalEdge(again));
    out.appendTo(slow);
    out.branch(out.equal(test.third, out.constInt64(2)), normalEdge(hit), normalEdge(again));
    out.appendTo(again);
    out.branch(bitIsSet(), normalEdge(usualAgain), rareEdge(slowAgain));
    test.returnConstant(usualAgain, 10);
    test.returnConstant(slowAgain, 20);
    test.returnConstant(hit, 30);
    const Order& order = test.compile();

    AOT_ORDER_TEST_CHECK(test, orderTestTargetOf(usual, 1) == (isStoredBetween ? again : usualAgain) && orderTestTargetOf(slow, 1) == slowAgain);
    AOT_ORDER_TEST_CHECK(test, order.of(again).isReached() == isStoredBetween);
    for (int64_t key : { 4, 7, 3, 0, 0x104 }) {
        for (int64_t third : { 0, 2, 3 }) {
            int64_t words[] { key, 0, 0 };
            bool passesFirst = key & 4;
            int64_t expected = third == (passesFirst ? 3 : 2) ? 30 : passesFirst && !isStoredBetween ? 10 : 20;
            AOT_ORDER_TEST_CHECK(test, test.run(reinterpret_cast<int64_t>(words), 0, third) == expected);
        }
    }
}

static void testMachineThreadingForgetsWordAtHeadOfLoop()
{
    OrderTest test;
    Output& out = test.out;
    LBasicBlock head = out.newBlock();
    LBasicBlock asks = out.newBlock();
    LBasicBlock body = out.newBlock();
    LBasicBlock exit = out.newBlock();
    LBasicBlock other = out.newBlock();
    auto keyIsSeven = [&] {
        return out.equal(out.load64(orderTestWordOf(test, test.first, OrderTestKey)), out.constInt64(7));
    };
    out.branch(keyIsSeven(), normalEdge(head), normalEdge(other));
    out.appendTo(head);
    out.branch(out.equal(test.third, out.constInt64(1)), normalEdge(other), normalEdge(asks));
    out.appendTo(asks);
    out.branch(keyIsSeven(), normalEdge(body), normalEdge(exit));
    out.appendTo(body);
    out.store64(out.constInt64(8), orderTestWordOf(test, test.first, OrderTestKey));
    out.jump(head);
    test.returnConstant(exit, 7);
    test.returnConstant(other, 9);
    const Order& order = test.compile();

    AOT_ORDER_TEST_CHECK(test, orderTestTargetOf(head, 1) == asks && order.of(asks).isReached() && order.of(head).isLoopHead);
    int64_t words[] { 7, 0, 0 };
    AOT_ORDER_TEST_CHECK(test, test.run(reinterpret_cast<int64_t>(words), 0, 0) == 7 && words[OrderTestKey] == 8);
}

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
    std::optional<uint64_t> smaller = orderTestInstructionsForRowOfTests(numberOfTerms, sameTest);
    std::optional<uint64_t> large = orderTestInstructionsForRowOfTests(4 * numberOfTerms, sameTest);
    AOT_TEST_CHECK(growsAsSizeDoes(smaller, large));
}

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

enum class OrderTestRereadWord : uint8_t {
    ExitsToOneBlock,
    ExitsToOneLikelyBlock,
    IsReadBelow,
    ExitsElsewhere,
    IsReadLaterAndExitsElsewhere,
    ExitsToAnotherLikelyBlock,
    IsNarrowReference,
    IsStoredBetween,
    LetsZeroPass,
};

template<OrderTestRereadWord variant>
static void testMachineThreadingOfTestBeforeRereadWord()
{
    using enum OrderTestRereadWord;
    constexpr bool hasUsualPlaces = variant == ExitsToOneLikelyBlock || variant == ExitsToAnotherLikelyBlock || variant == IsNarrowReference;
    constexpr bool exitsElsewhere = variant == ExitsElsewhere || variant == IsReadLaterAndExitsElsewhere || variant == ExitsToAnotherLikelyBlock || variant == IsNarrowReference;
    constexpr bool isReadBelow = variant == IsReadBelow || variant == IsReadLaterAndExitsElsewhere;
    OrderTest test;
    Output& out = test.out;
    LBasicBlock next = out.newBlock();
    LBasicBlock usual = out.newBlock();
    LBasicBlock slow = out.newBlock();
    LBasicBlock other = out.newBlock();
    LBasicBlock start = test.entry;
    auto edgeToPlace = [&](LBasicBlock block) {
        return hasUsualPlaces ? normalEdge(block) : rareEdge(block);
    };
    auto readWord = [&] {
        LValue word = out.load32(orderTestWordOf(test, test.first, OrderTestKey));
        if (variant == IsNarrowReference)
            word->markAsNarrowReference();
        return word;
    };
    LValue word = readWord();
    if (variant == IsStoredBetween)
        out.store32(out.constInt32(20), orderTestWordOf(test, test.first, OrderTestKey));
    out.branch(word, normalEdge(next), edgeToPlace(slow));
    out.appendTo(next);
    LValue readAgain = readWord();
    if (variant == LetsZeroPass)
        out.branch(out.below(readAgain, out.constInt32(11)), normalEdge(usual), edgeToPlace(slow));
    else
        out.branch(out.above(readAgain, out.constInt32(10)), normalEdge(usual), edgeToPlace(exitsElsewhere ? other : slow));
    out.appendTo(usual);
    out.ret(isReadBelow ? out.zeroExt(word, Int64) : out.constInt64(1));
    test.returnConstant(slow, 2);
    test.returnConstant(other, 3);
    test.compile();

    constexpr bool isAskedBehind = variant != ExitsToAnotherLikelyBlock && variant != IsStoredBetween && variant != LetsZeroPass;
    AOT_ORDER_TEST_CHECK(test, start->last()->isUnconditionalJump() == isAskedBehind);
    for (int64_t value : { 0, 1, 10, 11, 12, 2000000000 }) {
        int64_t words[] { value, 0, 0 };
        int64_t askedAgain = variant == IsStoredBetween ? 20 : value;
        bool isUsual = variant == LetsZeroPass ? askedAgain < 11 : askedAgain > 10;
        int64_t expected = !value ? 2 : isUsual ? (isReadBelow ? value : 1) : exitsElsewhere ? 3 : 2;
        AOT_ORDER_TEST_CHECK(test, test.run(reinterpret_cast<int64_t>(words)) == expected);
    }
}

static void testMachineThreadingAsksSecondOfTwoJoinedTestsOfBitsOfWordThatFirstRead()
{
    OrderTest test;
    Output& out = test.out;
    LBasicBlock second = out.newBlock();
    LBasicBlock hasBitOfFirst = out.newBlock();
    LBasicBlock hasBitOfSecond = out.newBlock();
    LBasicBlock hasNeither = out.newBlock();
    LValue read = out.load32(out.address(test.testProcedure.heaps().argumentBuffer, test.third));
    LValue bitsOfFirst = out.bitAnd(read, out.constInt32(1));
    out.branch(bitsOfFirst, normalEdge(hasBitOfFirst), normalEdge(second));
    out.appendTo(second);
    LValue readAgain = out.load32(out.address(test.testProcedure.heaps().argumentBuffer, test.third));
    LValue bitsOfSecond = out.bitAnd(readAgain, out.constInt32(6));
    out.branch(bitsOfSecond, normalEdge(hasBitOfSecond), normalEdge(hasNeither));
    test.returnConstant(hasBitOfFirst, 10);
    test.returnConstant(hasBitOfSecond, 20);
    test.returnConstant(hasNeither, 30);
    if (isX86_64())
        AOT_ORDER_TEST_CHECK(test, read != readAgain);
    test.compile();

    AOT_ORDER_TEST_CHECK(test, bitsOfFirst->child(1)->asInt() == 7 && bitsOfSecond->child(1)->asInt() == 1);
    AOT_ORDER_TEST_CHECK(test, bitsOfSecond->child(0) == read && (readAgain == read || !readAgain->useCount()));
    for (int32_t word = 0; word < 8; ++word)
        AOT_ORDER_TEST_CHECK(test, test.run(0, 0, reinterpret_cast<int64_t>(&word)) == (word & 1 ? 10 : word & 6 ? 20 : 30));
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

static void testMachineMergingOfRareArmWithLikelyArm()
{
    OrderTest test;
    Output& out = test.out;
    LBasicBlock seldom = out.newBlock();
    LBasicBlock next = out.newBlock();
    LBasicBlock often = out.newBlock();
    LBasicBlock done = out.newBlock();
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
        LValue smaller = loaded & firstIsLoaded ? out.load64(out.address(heap, test.third)) : test.first;
        LValue other = loaded & secondIsLoaded ? out.load64(out.address(heap, test.third, 8)) : test.first;
        LValue isBelow = out.below(smaller, out.constInt64(4));
        LValue isEqual = out.equal(other, test.second);
        LValue tested = out.binary(combination, isBelow, isEqual);
        out.branch(tested, normalEdge(holds), normalEdge(fails));
        test.returnConstant(holds, 10);
        test.returnConstant(fails, 20);
        test.compile();

        const Liveness& liveness = test.procedure().liveness();
        AOT_ORDER_TEST_CHECK(test, liveness.isFused(tested) && liveness.isFused(isBelow) && liveness.isFused(isEqual));
        AOT_ORDER_TEST_CHECK(test, liveness.isFused(smaller) + liveness.isFused(other) == isX86_64());
        for (int64_t smallWord : { 3, 4 }) {
            for (int64_t otherWord : { 7, 8 }) {
                int64_t words[] = { smallWord, otherWord };
                bool expected = isOr ? smallWord < 4 || otherWord == 7 : smallWord < 4 && otherWord == 7;
                AOT_ORDER_TEST_CHECK(test, test.run(loaded & firstIsLoaded ? otherWord : smallWord, 7, reinterpret_cast<int64_t>(words)) == (expected ? 10 : 20));
            }
        }
    }
}

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

#undef AOT_ORDER_TEST_CHECK

} // namespace Machine

std::span<const NativeTest> machineOrderTests()
{
    using Machine::EdgeClass;
    using Machine::OrderTestNextTest;
    using Machine::OrderTestSecondTest;
    using Machine::OrderTestThrowBlocks;
    using Machine::OrderTestRereadWord;
    using Machine::OrderTestWordAskedTwice;
    static constexpr NativeTest tests[] = {
        { "machine-order/unlikely-arm-stands-outside-its-loop"_s, Machine::testMachineOrderTakesUnlikelyArmOutOfLoop },
        { "machine-order/every-entrypoint-is-a-start"_s, Machine::testMachineOrderStartsAtEveryEntrypoint },
        { "machine-order/arm-that-goes-round-through-a-rare-block-lies-within-its-loop"_s, Machine::testMachineOrderKeepsArmThatGoesRoundThroughRareBlockWithinItsLoop },
        { "machine-order/critical-edge-is-split"_s, Machine::testMachineOrderSplitsCriticalEdge },
        { "machine-order/two-edges-to-one-block-are-split-apart"_s, Machine::testMachineOrderSplitsEachOfTwoEdgesToOneBlock },
        { "machine-order/work-grows-as-the-edges-of-a-switch-do"_s, Machine::testMachineOrderGrowsAsEdgesOfSwitchDo },
        { "machine-order/edge-that-holds-the-extracts-of-a-patch-stays-whole"_s, Machine::testMachineOrderLeavesEdgeThatHoldsExtractsWhole },
        { "machine-order/nested-handler-stands-ahead-of-its-rejoin"_s, Machine::testMachineOrderPlacesNestedHandlerAheadOfItsRejoin },
        { "machine-order/landing-pads-that-lead-into-each-other-are-one-region"_s, Machine::testMachineOrderJoinsLandingPadsThatLeadIntoEachOther },
        { "machine-order/throwing-arms-are-one-region-without-a-frame-at-entry"_s, Machine::testMachineOrderJoinsThrowingArmsWithoutFrameAtEntry },
        { "machine-order/branch-on-a-join-of-constants-is-threaded"_s, Machine::testMachineThreadingResolvesBranchOnJoinOfConstants },
        { "machine-order/only-reached-edge-keeps-its-class-through-two-joins"_s, Machine::testMachineThreadingKeepsClassOfOnlyReachedEdge<2> },
        { "machine-order/comparisons-ahead-of-a-join-are-fused-into-branches"_s, Machine::testMachineThreadingFusesComparisonsAheadOfJoin },
        { "machine-order/edge-of-a-branch-that-passes-no-constant-stays"_s, Machine::testMachineThreadingLeavesEdgeOfBranchThatPassesNoConstant },
        { "machine-order/join-that-hands-its-phi-to-a-second-join-is-threaded"_s, Machine::testMachineThreadingPassesJoinThatHandsItsPhiOn },
        { "machine-order/join-that-holds-a-store-stays"_s, Machine::testMachineThreadingKeepsJoinThatHoldsStore },
        { "machine-order/head-of-a-loop-is-not-threaded"_s, Machine::testMachineThreadingLeavesHeadOfLoop },
        { "machine-order/way-that-has-decided-a-test-passes-its-negation-by"_s, Machine::testMachineThreadingOfTestThatWayHasDecided<OrderTestSecondTest::IsAskedTheOtherWayRound> },
        { "machine-order/decided-test-behind-a-store-stays"_s, Machine::testMachineThreadingOfTestThatWayHasDecided<OrderTestSecondTest::StandsBehindStore> },
        { "machine-order/a-block-that-nothing-leads-to-is-no-way-into-the-blocks-it-leads-to"_s, Machine::testMachineThreadingOfTestThatWayHasDecided<OrderTestSecondTest::IsBehindWaysThatBlockWithoutWayInLeadsTo> },
        { "machine-order/what-a-block-that-nothing-leads-to-reads-is-not-read"_s, Machine::testMachineThreadingOfTestThatWayHasDecided<OrderTestSecondTest::IsReadInBlockWithoutWayIn> },
        { "machine-order/a-loop-whose-entrance-goes-in-the-threader-is-cut-through"_s, Machine::testMachineThreadingCutsThroughLoopWhoseEntranceHasGone },
        { "machine-order/words-are-read-again-behind-a-decided-test-whose-arms-have-stored"_s, Machine::testMachineThreadingPastTestBehindArmsThatStore },
        { "machine-order/decided-test-at-the-head-of-a-loop-stays"_s, Machine::testMachineThreadingLeavesDecidedTestAtHeadOfLoop },
        { "machine-order/a-test-of-a-word-that-nothing-has-written-since-is-passed-by"_s, Machine::testMachineThreadingOfTestOfWordInMemory<OrderTestWordAskedTwice::IsWrittenNowhere> },
        { "machine-order/a-word-stored-right-behind-its-test-is-asked-again"_s, Machine::testMachineThreadingOfTestOfWordInMemory<OrderTestWordAskedTwice::IsStoredBehindFirstTest> },
        { "machine-order/a-word-stored-farther-on-is-asked-again"_s, Machine::testMachineThreadingOfTestOfWordInMemory<OrderTestWordAskedTwice::IsStoredFartherOn> },
        { "machine-order/a-word-that-a-patch-writes-is-asked-again"_s, Machine::testMachineThreadingOfTestOfWordInMemory<OrderTestWordAskedTwice::IsWrittenByPatch> },
        { "machine-order/a-word-is-asked-again-behind-a-patch-that-may-collect-and-declares-no-write"_s, Machine::testMachineThreadingOfTestOfWordInMemory<OrderTestWordAskedTwice::IsWrittenByPatchThatSaysOnlyThatItMayCollect> },
        { "machine-order/a-word-is-asked-again-behind-a-patch-that-declares-nothing-of-memory"_s, Machine::testMachineThreadingOfTestOfWordInMemory<OrderTestWordAskedTwice::IsWrittenByPatchThatSaysNothing> },
        { "machine-order/a-word-is-asked-again-behind-a-patch-that-is-declared-pure"_s, Machine::testMachineThreadingOfTestOfWordInMemory<OrderTestWordAskedTwice::IsWrittenByPatchThatIsDeclaredPure> },
        { "machine-order/a-closed-arm-that-stores-the-word-asks-again"_s, Machine::testMachineThreadingOfTestOfWordInMemory<OrderTestWordAskedTwice::IsStoredInClosedArm> },
        { "machine-order/a-word-stored-between-its-load-and-its-test-is-asked-again"_s, Machine::testMachineThreadingOfTestOfWordInMemory<OrderTestWordAskedTwice::IsStoredBetweenFirstLoadAndFirstTest> },
        { "machine-order/a-word-stored-between-the-block-of-its-load-and-its-test-is-asked-again"_s, Machine::testMachineThreadingOfTestOfWordInMemory<OrderTestWordAskedTwice::IsStoredBetweenBlocksOfFirstLoadAndFirstTest> },
        { "machine-order/a-word-held-against-another-constant-is-asked"_s, Machine::testMachineThreadingOfTestOfWordInMemory<OrderTestWordAskedTwice::IsHeldAgainstAnotherConstant> },
        { "machine-order/the-same-word-of-another-cell-is-asked"_s, Machine::testMachineThreadingOfTestOfWordInMemory<OrderTestWordAskedTwice::IsReadOffAnotherCell> },
        { "machine-order/another-word-of-the-same-cell-is-asked"_s, Machine::testMachineThreadingOfTestOfWordInMemory<OrderTestWordAskedTwice::IsAnotherWordOfCell> },
        { "machine-order/the-word-of-a-cell-that-the-join-brings-is-asked"_s, Machine::testMachineThreadingOfTestOfWordInMemory<OrderTestWordAskedTwice::IsReadOffWhatJoinBrings> },
        { "machine-order/a-word-out-of-a-storage-is-asked-again"_s, Machine::testMachineThreadingOfTestOfWordInMemory<OrderTestWordAskedTwice::IsReadOffStorage> },
        { "machine-order/a-word-whose-second-load-is-read-below-is-asked-again"_s, Machine::testMachineThreadingOfTestOfWordInMemory<OrderTestWordAskedTwice::IsReadBelowToo> },
        { "machine-order/a-word-is-not-asked-again-behind-a-patch-that-only-keeps-its-children-alive"_s, Machine::testMachineThreadingOfTestOfWordInMemory<OrderTestWordAskedTwice::StandsBehindKeeper> },
        { "machine-order/a-word-is-asked-again-behind-a-shift-by-a-variable-on-x86-64-alone"_s, Machine::testMachineThreadingOfTestOfWordInMemory<OrderTestWordAskedTwice::StandsBehindShiftByVariable> },
        { "machine-order/a-word-filled-once-is-not-asked-again-behind-the-arm-that-fills-it"_s, Machine::testMachineThreadingOfTestOfWordInMemory<OrderTestWordAskedTwice::IsFilledOnceInClosedArm> },
        { "machine-order/bits-of-a-byte-that-nothing-has-written-since-are-not-asked-again"_s, Machine::testMachineThreadingOfBitsOfByteInMemory<false> },
        { "machine-order/bits-of-a-byte-are-asked-again-behind-a-store-over-its-word"_s, Machine::testMachineThreadingOfBitsOfByteInMemory<true> },
        { "machine-order/what-is-known-of-a-word-ends-at-the-head-of-a-loop"_s, Machine::testMachineThreadingForgetsWordAtHeadOfLoop },
        { "machine-order/work-grows-as-the-decided-tests-in-a-row-do"_s, Machine::testMachineThreadingGrowsAsTestsInRowDo<true> },
        { "machine-order/work-grows-as-the-undecided-tests-in-a-row-do"_s, Machine::testMachineThreadingGrowsAsTestsInRowDo<false> },
        { "machine-order/test-for-zero-is-asked-behind-a-test-that-leaves-by-its-first-way"_s, Machine::testMachineThreadingOfTestThatNextTestImplies<OrderTestNextTest::LeavesForZeroTooByItsFirstWay> },
        { "machine-order/test-of-bits-is-asked-behind-a-bound-that-words-without-them-fail"_s, Machine::testMachineThreadingOfTestThatNextTestImplies<OrderTestNextTest::FollowsTestOfBits> },
        { "machine-order/the-second-of-two-joined-tests-of-bits-asks-the-word-that-the-first-read"_s, Machine::testMachineThreadingAsksSecondOfTwoJoinedTestsOfBitsOfWordThatFirstRead },
        { "machine-order/test-is-left-to-next"_s, Machine::testMachineThreadingOfTestBeforeRereadWord<OrderTestRereadWord::ExitsToOneBlock> },
        { "machine-order/test-is-left-for-usual-place"_s, Machine::testMachineThreadingOfTestBeforeRereadWord<OrderTestRereadWord::ExitsToOneLikelyBlock> },
        { "machine-order/word-read-below-stays-read"_s, Machine::testMachineThreadingOfTestBeforeRereadWord<OrderTestRereadWord::IsReadBelow> },
        { "machine-order/test-is-asked-behind-next-read"_s, Machine::testMachineThreadingOfTestBeforeRereadWord<OrderTestRereadWord::ExitsElsewhere> },
        { "machine-order/word-read-below-is-compared"_s, Machine::testMachineThreadingOfTestBeforeRereadWord<OrderTestRereadWord::IsReadLaterAndExitsElsewhere> },
        { "machine-order/test-for-usual-place-stays"_s, Machine::testMachineThreadingOfTestBeforeRereadWord<OrderTestRereadWord::ExitsToAnotherLikelyBlock> },
        { "machine-order/filled-slot-is-asked-behind"_s, Machine::testMachineThreadingOfTestBeforeRereadWord<OrderTestRereadWord::IsNarrowReference> },
        { "machine-order/test-ahead-of-store-stays"_s, Machine::testMachineThreadingOfTestBeforeRereadWord<OrderTestRereadWord::IsStoredBetween> },
        { "machine-order/test-that-next-passes-stays"_s, Machine::testMachineThreadingOfTestBeforeRereadWord<OrderTestRereadWord::LetsZeroPass> },
        { "machine-order/test-of-bits-stays-ahead-of-a-bound-that-a-word-without-them-passes"_s, Machine::testMachineThreadingOfTestThatNextTestImplies<OrderTestNextTest::FollowsTestOfBitsThatReachAboveItsBound> },
        { "machine-order/short-run-moves-behind-its-jump"_s, Machine::testMachineOrderMovesShortRunAfterItsJump<false> },
        { "machine-order/short-run-moves-within-its-loop"_s, Machine::testMachineOrderMovesShortRunAfterItsJump<true> },
        { "machine-order/cycle-of-jumps-ends-the-search"_s, Machine::testMachineThreadingEndsInCycleOfJumps },
        { "machine-order/identical-throw-blocks-merge"_s, Machine::testMachineMergingOfThrowBlocks<OrderTestThrowBlocks::AreAlike> },
        { "machine-order/throw-blocks-with-generators-of-their-own-stay-apart"_s, Machine::testMachineMergingOfThrowBlocks<OrderTestThrowBlocks::DifferInGenerator> },
        { "machine-order/rare-arm-and-likely-arm-merge-into-a-likely-block"_s, Machine::testMachineMergingOfRareArmWithLikelyArm },
        { "machine-order/test-read-by-two-arms-that-merge-is-made-by-the-branch-that-is-left"_s, Machine::testMachineMergingLeavesTestToItsOneReader },
        { "machine-order/test-whose-arms-merge-goes"_s, Machine::testMachineOrderDropsTestWhoseArmsMerge },
        { "machine-order/value-tested-for-nothing-stays-for-its-other-reader"_s, Machine::testMachineOrderKeepsWhatElseReadsTestedValue },
        { "machine-order/test-in-a-later-block-leaves-a-comparison-of-a-number-that-dies"_s, Machine::testMachineFusionLeavesComparisonOfNumberThatIsNotReadAgain },
        { "machine-order/one-of-two-comparisons-that-both-have-to-hold-reads-its-load-where-it-lies"_s, Machine::testMachineFusionOfLoadBelowTwoComparisons<Machine::BitAnd> },
        { "machine-order/branch-on-one-bit-of-a-word-of-64-bits-shifts-nothing"_s, Machine::testMachineFusionOfShiftBelowBitTest<Machine::ZShr, Machine::Int64> },
        { "machine-order/branch-on-either-a-comparison-or-one-bit-tests-twice"_s, Machine::testMachineFusionOfComparisonBesideBitTest<Machine::BitOr> },
        { "machine-order/branch-on-either-bits-of-a-load-or-a-comparison-tests-twice"_s, Machine::testMachineFusionOfBitsOfLoadBesideComparison<Machine::BitOr> },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
