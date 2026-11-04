/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachineAllocator.h"

#if ENABLE(AOT)

#include "AOTMachineCallsOnWays.h"
#include <algorithm>
#include <bit>
#include <initializer_list>
#include <type_traits>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT { namespace Machine {

// A head mostly holds three or four values, so what is done once for a block, an edge or a head weighs more here than what is done for each register.
// Each of enterBlock(), leaveBlock(), fixHeadOf() and appendEdgeMoves() therefore has a short way for the plain case, which asks for nothing that the case does not need, and a long way out of line.

static ALWAYS_INLINE Reg lowestRegisterAtEdge(RegisterMask mask)
{
    ASSERT(mask);
    return Reg::fromIndex(static_cast<unsigned>(std::countr_zero(mask)));
}

static ALWAYS_INLINE bool isLiveAtEdgePosition(const Liveness& liveness, const Operation* value, Position position)
{
    Position lastUse = liveness.of(value).lastUse;
    return lastUse != noPosition && lastUse >= position;
}

// A range that was extended over a loop ends at the head of the block behind the loop, where only what sank into that block reads.
static bool isLiveAtHeadPosition(const Allocator& allocator, const Operation* value, Position head)
{
    const Liveness& liveness = allocator.m_liveness;
    if (!isLiveAtEdgePosition(liveness, value, head))
        return false;
    if (liveness.of(value).lastUse > head)
        return true;
    uint32_t use = allocator.findReadFrom(value, head);
    return use && liveness.use(use).position == head;
}

// Allocator::findReadFrom(), from the cursor of the value. Entry zero of Liveness::uses() has noPosition for its position, so the search ends there by itself.
static ALWAYS_INLINE uint32_t findReadFromBetweenBlocks(const Allocator& allocator, const Use* uses, uint32_t firstReadNotPassed, const Operation* value, Position position)
{
    uint32_t use = firstReadNotPassed;
    for (unsigned stepsLeft = 64; uses[use].position < position; use = uses[use].next) {
        if (!--stepsLeft) [[unlikely]]
            return allocator.findReadFromInList(value, position);
    }
    return use;
}

static ALWAYS_INLINE bool isPhiAtHeadOf(const Block* block, const Operation* value)
{
    return value->opcode() == Phi && value->owner() == block;
}

static ALWAYS_INLINE bool isResultOfPatchAtEdge(const Operation* patch, const Operation* value)
{
    return value == patch || (value->opcode() == Extract && value->child(0) == patch);
}

// The results of a Patch that ends its block exist only on the edge to the first successor.
static bool isResultOfPatchEndingBlock(const Block* block, const Operation* value)
{
    const Operation* end = block->last();
    if (end->opcode() != Patch)
        return false;
    return isResultOfPatchAtEdge(end, value);
}

// The callee-saved registers for which Allocator::noteCalleeSaveUse() may still change something where the walk stands.
// It only ever adds. What it adds for a register goes by the register and by whether the walk stands in a rare region, so it adds nothing the second time. Ahead of the framed part it notes a use of the frame too.
static ALWAYS_INLINE RegisterMask calleeSavesStillToNoteBetweenBlocks(const Allocator& allocator)
{
    if (allocator.m_isAheadOfFramedPart)
        return allocator.m_file.calleeSaved;
    return allocator.m_file.calleeSaved & ~(allocator.isInRareRegion() ? allocator.m_allocation.usedCalleeSaves() : allocator.m_calleeSavesTakenOutsideRareRegions);
}

// Allocator::bind() of a register that is free. `state` is that of the value.
static ALWAYS_INLINE void bindFreeRegisterBetweenBlocks(Allocator& allocator, Reg reg, Operation* value, Allocator::ValueState& state, RegisterMask calleeSavesStillToNote)
{
    ASSERT(reg);
    ASSERT(value);
    RegisterMask bit = maskOf(reg);
    ASSERT(bit & allocator.m_file.allocatableIn(bankOf(reg)));
    ASSERT(!value->type().isNumeric() || bankOf(value->type()) == bankOf(reg));
    ASSERT(!allocator.m_valueIn[reg.index()] && !(allocator.m_bound & bit));
    ASSERT(&allocator.stateOf(value) == &state);

    allocator.m_valueIn[reg.index()] = value;
    allocator.m_bound |= bit;
    state.registers |= bit;
    // Whatever way a register came to hold a value, the function writes it.
    if (bit & calleeSavesStillToNote) [[unlikely]]
        allocator.noteCalleeSaveUse(reg);
}

// Allocator::unbind() of a register that is bound. `state` is that of its value.
static ALWAYS_INLINE void unbindRegisterBetweenBlocks(Allocator& allocator, Reg reg, Allocator::ValueState& state)
{
    RegisterMask bit = maskOf(reg);
    ASSERT(allocator.m_valueIn[reg.index()] && &allocator.stateOf(allocator.m_valueIn[reg.index()]) == &state);
    ASSERT(allocator.m_bound & bit);
    ASSERT(state.registers & bit);
    state.registers &= ~bit;
    allocator.m_valueIn[reg.index()] = nullptr;
    allocator.m_bound &= ~bit;
}

// Allocator::unbindAll() of the value with this state.
static ALWAYS_INLINE void unbindAllBetweenBlocks(Allocator& allocator, Allocator::ValueState& state)
{
    RegisterMask registers = state.registers;
    for (RegisterMask left = registers; left; left &= left - 1)
        allocator.m_valueIn[static_cast<unsigned>(std::countr_zero(left))] = nullptr;
    ASSERT((allocator.m_bound & registers) == registers);
    allocator.m_bound &= ~registers;
    state.registers = 0;
}

// Allocator::unbind() of each of them. All are bound.
static ALWAYS_INLINE void unbindRegistersBetweenBlocks(Allocator& allocator, RegisterMask registers)
{
    ASSERT((allocator.m_bound & registers) == registers);
    Allocator::ValueState* const states = allocator.m_values.begin();
    for (RegisterMask left = registers; left; left &= left - 1) {
        unsigned index = static_cast<unsigned>(std::countr_zero(left));
        ASSERT(allocator.m_valueIn[index]);
        states[allocator.m_valueIn[index]->index()].registers &= ~(RegisterMask { 1 } << index);
        allocator.m_valueIn[index] = nullptr;
    }
    allocator.m_bound &= ~registers;
}

static ALWAYS_INLINE void takeSnapshotBetweenBlocks(Allocator& allocator, Allocator::Snapshot& snapshot)
{
    RegisterMask bound = allocator.m_bound;
    const size_t count = static_cast<size_t>(std::popcount(bound));
    RegisterBinding* const bindings = allocator.m_arena.allocateArray<RegisterBinding>(count);
    RegisterBinding* next = bindings;
    for (; bound; bound &= bound - 1) {
        unsigned index = static_cast<unsigned>(std::countr_zero(bound));
        new (NotNull, next++) RegisterBinding { Reg::fromIndex(index), allocator.m_valueIn[index] };
    }
    snapshot.isSet = true;
    snapshot.bindings = std::span<const RegisterBinding>(bindings, count);
    snapshot.registers = allocator.m_bound;
}

// The moves of the edges read the children of the Upsilons here. They stay bound: the heads of the successors drop what is dead.
// No operation makes these reads. Of a value that Upsilons alone hand on nothing else would move the cursor, and nextUseOf(), which moves none, would step over every passed read each time it is asked.
static ALWAYS_INLINE void passReadsOfUpsilonsBetweenBlocks(Allocator& allocator, const Block* from, Position positionOfEdges)
{
    std::span<Operation* const> upsilons = from->upsilons();
    if (upsilons.empty())
        return;
    const Use* const uses = allocator.m_uses;
    Allocator::ValueState* const states = allocator.m_values.begin();
    for (const Operation* upsilon : upsilons) {
        uint32_t& cursor = states[upsilon->valueHandedOn()->index()].nextUse;
        uint32_t use = cursor;
        while (uses[use].position <= positionOfEdges)
            use = uses[use].next;
        cursor = use;
    }
}

// Of a block that is neither an entrypoint nor a landing pad.
static ALWAYS_INLINE bool headBehindEdgesIsFixedByEdge(const ValueFacts* factsOfValues, const Block* block, const BlockPlace& place)
{
    if (place.predecessors.size() != 1)
        return true;
    for (const Operation* phi : block->phis()) {
        if (!factsOfValues[phi->index()].has(ValueFlag::IsDropped))
            return true;
    }
    return false;
}

// A block whose head no edge fixes continues from its one predecessor's registers, or starts empty.
static bool headIsFixedByEdge(const Allocator& allocator, const Block* block)
{
    if (block->isEntry() || block->isLandingPad())
        return false;
    return headBehindEdgesIsFixedByEdge(allocator.m_valueFacts, block, allocator.m_order.of(block));
}

static ALWAYS_INLINE bool isRareEdgeBetweenRegions(uint32_t regionOfFrom, uint32_t regionOfTarget, const Block* from, const Block* target)
{
    if (regionOfFrom != noRareRegion && regionOfFrom != regionOfTarget)
        return true;
    for (const Edge& edge : from->successors()) {
        if (edge.block() == target && !edge.isRare())
            return false;
    }
    return true;
}

// Of a block with a successor or two. orderBlocks() leaves one edge at most that leads from a block straight to another.
static bool isRareEdgeIntoHead(const Order& order, const Block* from, const Block* target)
{
    return isRareEdgeBetweenRegions(order.of(from).rareRegion, order.of(target).rareRegion, from, target);
}

static bool isRareEdgeIntoHead(const Order& order, const Block* target, size_t numberOfPredecessor)
{
    uint32_t regionOfFrom = order.of(order.of(target).predecessors[numberOfPredecessor]).rareRegion;
    if (regionOfFrom != noRareRegion && regionOfFrom != order.of(target).rareRegion)
        return true;
    return order.edgeFromPredecessor(target, numberOfPredecessor).isRare();
}

// The block returned precedes the target in the order, so the head is fixed before the walk reaches it.
// Outside the framed part a head is fixed by an edge from outside it: what comes out of the framed part may lie in a callee-saved register, which the teardown gives back to the caller.
static NEVER_INLINE const Block* findBlockThatFixesHead(const Allocator& allocator, const Block* target, const BlockPlace& place)
{
    const Order& order = allocator.m_order;
    bool headIsAheadOfFramedPart = allocator.headIsAheadOfFramedPart(target);
    const Block* firstFromOutsideFramedPart = nullptr;
    // What comes straight from a Branch has lost no register on the way. It fits itself to what the longer ways have left, which mostly costs nothing.
    const Block* countsLast = nullptr;
    Block* const* const predecessors = place.predecessors.data();
    const size_t numberOfPredecessors = place.predecessors.size();
    for (size_t number = 0; number < numberOfPredecessors; ++number) {
        const Block* predecessor = predecessors[number];
        const BlockPlace& placeOfPredecessor = order.of(predecessor);
        if (placeOfPredecessor.orderIndex >= place.orderIndex)
            break;
        if (headIsAheadOfFramedPart && !allocator.isAheadOfFramedPart(predecessor))
            continue;
        if (predecessor == place.sourceOfUnsplitEdgeThatCountsLast) {
            countsLast = predecessor;
            continue;
        }
        if (!(predecessor->numSuccessors() <= 2 ? isRareEdgeBetweenRegions(placeOfPredecessor.rareRegion, place.rareRegion, predecessor, target) : isRareEdgeIntoHead(order, target, number)))
            return predecessor;
        if (!firstFromOutsideFramedPart)
            firstFromOutsideFramedPart = predecessor;
    }
    if (countsLast)
        return countsLast;
    return headIsAheadOfFramedPart && firstFromOutsideFramedPart ? firstFromOutsideFramedPart : place.predecessors.front();
}

// Null where one of them is null, or where the two have no block in common.
static const Block* findBlockThatDominatesBoth(const Allocator& allocator, const Block* left, const Block* right)
{
    while (left != right) {
        if (!left || !right)
            return nullptr;
        const Block*& lower = allocator.m_order.of(left).orderIndex > allocator.m_order.of(right).orderIndex ? left : right;
        lower = allocator.m_immediateDominators[lower->index()];
    }
    return left;
}

// A path enters a loop around the block here and not at the start of the loop. BlockPlace::predecessors is sorted by the order.
static bool isSideEntranceOfLoop(const Order& order, const BlockPlace& place)
{
    uint32_t loop = place.isLoopHead ? order.loops()[place.loop].parent : place.loop;
    if (loop == noLoop)
        return false;
    if (place.predecessors.empty())
        return true;
    const Loop& around = order.loops()[loop];
    return order.of(place.predecessors.front()).orderIndex < around.firstBlock || order.of(place.predecessors.back()).orderIndex >= around.endBlock;
}

// Cooper, Harvey, Kennedy. The first pass leaves out the predecessors that stand behind a block: they have no entry yet.
// Each of them stands in a loop that starts at the block. Where no loop has a side entrance the block dominates them, so they add nothing and one pass is exact.
// Several entrypoints make side entrances, and so does a rare region that stands behind the block it rejoins. Then the passes go on, over every predecessor, until nothing changes.
//
// Most of the work is the walk up from two predecessors to the block that dominates both, some twenty steps for a join. So the walks go by numbers and not by blocks:
// the number of a block is one more than its BlockPlace::orderIndex, which no other block of the order has, and zero stands for no block. Whichever of two blocks stands further on has the greater number,
// and no block at all has the least, so a walk up to a number stops by itself where it runs out of blocks.
// The first walk that goes on for long makes both lists. By the number of a block: how many blocks dominate it, itself among them, and one of them further up,
// chosen so that the way from a block up to a given height takes about as many steps as the height has digits (Myers: skew-binary jump pointers).
// A row of tests, each dominated by the one before, with a join at each test that a way from the top comes to, costs its length times that, not the square.
struct WaysFurtherUpToDominators {
    Arena& arena;
    uint32_t numberOfBlocks;
    // Blocks with a lesser number have both.
    uint32_t endOfBlocksWithWay { 0 };
    uint32_t* heights { nullptr };
    uint32_t* ways { nullptr };
};

struct WalksUpToDominators {
    const BlockPlace* places;
    // By the number of a block. A pass reads the entries of blocks that have theirs: in the first one those ahead of the block that it stands at.
    uint32_t* numbersOfDominators;
    // By the number of a block: the last join for which the walk up from a predecessor has come by the block. That walk went on up to what dominated the predecessors seen until then,
    // which the block that dominates them now dominates too. So a later walk for the same join ends here: a block that ways out of two long rows come to costs their lengths, not the square.
    // A join of two has one walk, which finds no mark and leaves none that anybody looks for.
    uint32_t* lastJoinsWalkedFor;
    uint32_t numberOfJoin;
    // In PassOverDominators::FirstWithLongWalks.
    WaysFurtherUpToDominators* waysFurtherUp { nullptr };
};

// What findNumberOfBlockThatDominatesBoth() finds. In the first pass alone: each block ahead of `number` has its dominator, for good, and that stands ahead of it.
static NEVER_INLINE uint32_t findNumberOfBlockFarUpThatDominatesBoth(WalksUpToDominators& walks, uint32_t number, uint32_t first, uint32_t second)
{
    const uint32_t* const numbersOfDominators = walks.numbersOfDominators;
    WaysFurtherUpToDominators& further = *walks.waysFurtherUp;
    if (!further.heights) {
        further.heights = further.arena.allocateArray<uint32_t>(further.numberOfBlocks + 1);
        further.ways = further.arena.allocateArray<uint32_t>(further.numberOfBlocks + 1);
        further.heights[0] = 0;
        further.ways[0] = 0;
        further.endOfBlocksWithWay = 1;
    }
    uint32_t* const heights = further.heights;
    uint32_t* const waysFurtherUp = further.ways;
    for (uint32_t next = further.endOfBlocksWithWay; next < number; ++next) {
        uint32_t dominator = numbersOfDominators[next];
        uint32_t way = waysFurtherUp[dominator];
        ASSERT(dominator < next);
        heights[next] = heights[dominator] + 1;
        waysFurtherUp[next] = heights[dominator] - heights[way] == heights[way] - heights[waysFurtherUp[way]] ? waysFurtherUp[way] : dominator;
    }
    further.endOfBlocksWithWay = std::max(further.endOfBlocksWithWay, number);

    ASSERT(first && second && first < number && second < number);
#if ASSERT_ENABLED
    uint32_t foundBlockByBlock = first;
    for (uint32_t other = second; foundBlockByBlock != other;) {
        uint32_t& greater = foundBlockByBlock > other ? foundBlockByBlock : other;
        greater = numbersOfDominators[greater];
    }
#endif
    if (heights[first] < heights[second])
        std::swap(first, second);
    const uint32_t height = heights[second];
    while (heights[first] > height)
        first = heights[waysFurtherUp[first]] >= height ? waysFurtherUp[first] : numbersOfDominators[first];
    // Two blocks of one height have their ways further up to one height.
    while (first != second) {
        bool partFurtherUp = waysFurtherUp[first] != waysFurtherUp[second];
        first = partFurtherUp ? waysFurtherUp[first] : numbersOfDominators[first];
        second = partFurtherUp ? waysFurtherUp[second] : numbersOfDominators[second];
    }
    ASSERT(first == foundBlockByBlock);
    return first;
}

enum class PassOverDominators : uint8_t {
    // Of a procedure of few blocks: no walk is long.
    FirstWithShortWalks,
    // A walk that goes on for long is ended by findNumberOfBlockFarUpThatDominatesBoth().
    FirstWithLongWalks,
    Later,
};

static constexpr uint32_t mostBlocksOfProcedureWithShortWalks = 2048;
// A turn is up to four steps up, or a change of the side that goes up. Nearly every walk ends far sooner.
static constexpr int turnsOfWalkUpBlockByBlock = 24;

// From a block up to the first one that dominates it and whose number is no greater than `limit`. Where turns are counted and none is left it stops on the way.
template<PassOverDominators pass>
static ALWAYS_INLINE uint32_t walkUpToNumber(const uint32_t* numbersOfDominators, uint32_t from, uint32_t limit, int& turnsLeft)
{
    ASSERT(from > limit);
    if constexpr (pass != PassOverDominators::FirstWithLongWalks) {
        do {
            from = numbersOfDominators[from];
        } while (from > limit);
        return from;
    }
    for (;;) {
        from = numbersOfDominators[from];
        if (from <= limit)
            return from;
        from = numbersOfDominators[from];
        if (from <= limit)
            return from;
        from = numbersOfDominators[from];
        if (from <= limit)
            return from;
        from = numbersOfDominators[from];
        if (from <= limit || --turnsLeft <= 0)
            return from;
    }
}

// findBlockThatDominatesBoth(), by numbers. Neither is zero. `number` is that of the block whose dominator is looked for.
template<PassOverDominators pass>
static ALWAYS_INLINE uint32_t findNumberOfBlockThatDominatesBoth(WalksUpToDominators& walks, const uint32_t* numbersOfDominators, uint32_t number, uint32_t dominator, uint32_t other)
{
    int turnsLeft = turnsOfWalkUpBlockByBlock;
    while (dominator != other) {
        if (pass == PassOverDominators::FirstWithLongWalks && --turnsLeft <= 0) [[unlikely]]
            return findNumberOfBlockFarUpThatDominatesBoth(walks, number, dominator, other);
        if (dominator > other) {
            dominator = walkUpToNumber<pass>(numbersOfDominators, dominator, other, turnsLeft);
            if (!dominator)
                return 0;
            continue;
        }
        other = walkUpToNumber<pass>(numbersOfDominators, other, dominator, turnsLeft);
        if (!other)
            return 0;
    }
    return dominator;
}

template<PassOverDominators pass>
static NEVER_INLINE uint32_t findNumberOfDominatorTheLongWay(WalksUpToDominators& walks, uint32_t number, const BlockPlace& place)
{
    constexpr bool isFirstPass = pass != PassOverDominators::Later;
    const BlockPlace* const places = walks.places;
    uint32_t* const numbersOfDominators = walks.numbersOfDominators;
    uint32_t* const lastJoinsWalkedFor = walks.lastJoinsWalkedFor;
    Block* const* const predecessors = place.predecessors.data();
    const size_t numberOfPredecessors = place.predecessors.size();
    const bool marksWalks = numberOfPredecessors > 2;
    const uint32_t numberOfJoin = marksWalks ? ++walks.numberOfJoin : 0;
    uint32_t dominator = 0;
    bool hasPath = false;
    // The last predecessor first: then the common block is never far above the next one.
    for (size_t index = numberOfPredecessors; index--;) {
        ASSERT(!places[predecessors[index]->index()].wasSplitLate);
        uint32_t other = places[predecessors[index]->index()].orderIndex + 1;
        if (other == number || (isFirstPass && other > number))
            continue;
        if (!std::exchange(hasPath, true)) {
            dominator = other;
            continue;
        }
        if (!dominator)
            continue;
        if (!marksWalks) {
            dominator = findNumberOfBlockThatDominatesBoth<pass>(walks, numbersOfDominators, number, dominator, other);
            continue;
        }
        // findNumberOfBlockThatDominatesBoth(), with the marks. Here a step up from the other block is a turn.
        int turnsLeft = 4 * turnsOfWalkUpBlockByBlock;
        while (dominator != other) {
            if (pass == PassOverDominators::FirstWithLongWalks && --turnsLeft <= 0) [[unlikely]] {
                dominator = findNumberOfBlockFarUpThatDominatesBoth(walks, number, dominator, other);
                break;
            }
            if (dominator > other) {
                dominator = walkUpToNumber<pass>(numbersOfDominators, dominator, other, turnsLeft);
                if (!dominator)
                    break;
                continue;
            }
            if (std::exchange(lastJoinsWalkedFor[other], numberOfJoin) == numberOfJoin)
                break;
            other = numbersOfDominators[other];
            if (!other) {
                dominator = 0;
                break;
            }
        }
    }
    return dominator;
}

// Of a block that has more than its one predecessor ahead of it to go by. Most are joins of two blocks that both count.
template<PassOverDominators pass>
static ALWAYS_INLINE uint32_t findNumberOfDominatorOfJoin(WalksUpToDominators& walks, const BlockPlace* places, const uint32_t* numbersOfDominators, uint32_t number, const BlockPlace& place)
{
    constexpr bool isFirstPass = pass != PassOverDominators::Later;
    if (place.predecessors.empty())
        return 0;
    if (place.predecessors.size() == 2) {
        Block* const* const predecessors = place.predecessors.data();
        uint32_t last = places[predecessors[1]->index()].orderIndex + 1;
        uint32_t first = places[predecessors[0]->index()].orderIndex + 1;
        if (isFirstPass ? last < number && first < number : last != number && first != number)
            return findNumberOfBlockThatDominatesBoth<pass>(walks, numbersOfDominators, number, last, first);
    }
    return findNumberOfDominatorTheLongWay<pass>(walks, number, place);
}

struct FirstPassOverDominators {
    uint32_t numberOfJoins;
    bool needsAnotherPass;
};

// `numbersOfJoins` gets the numbers of the blocks that the later passes go through: whatever pass asks, a block with one predecessor, which stands ahead of it, gets that predecessor.
template<PassOverDominators pass>
static ALWAYS_INLINE FirstPassOverDominators findDominatorsInFirstPass(WalksUpToDominators& walks, const Order& order, std::span<Block* const> blocksInOrder, const Block** dominators, uint32_t* numbersOfJoins)
{
    Block* const* const blocks = blocksInOrder.data();
    const uint32_t numberOfBlocks = static_cast<uint32_t>(blocksInOrder.size());
    const BlockPlace* const places = walks.places;
    uint32_t* const numbersOfDominators = walks.numbersOfDominators;
    uint32_t numberOfJoins = 0;
    bool needsAnotherPass = false;
    for (uint32_t number = 1; number <= numberOfBlocks; ++number) {
        const uint32_t indexOfBlock = blocks[number - 1]->index();
        const BlockPlace& place = places[indexOfBlock];
        ASSERT(place.orderIndex + 1 == number);
        const Block* onlyPredecessor = place.predecessors.size() == 1 ? place.predecessors.data()[0] : nullptr;
        uint32_t numberOfOnlyPredecessor = onlyPredecessor ? places[onlyPredecessor->index()].orderIndex + 1 : number;
        if (numberOfOnlyPredecessor < number) {
            numbersOfDominators[number] = numberOfOnlyPredecessor;
            dominators[indexOfBlock] = onlyPredecessor;
        } else {
            numbersOfJoins[numberOfJoins++] = number;
            uint32_t dominator = findNumberOfDominatorOfJoin<pass>(walks, places, numbersOfDominators, number, place);
            numbersOfDominators[number] = dominator;
            if (dominator)
                dominators[indexOfBlock] = blocks[dominator - 1];
        }
        // One side entrance is enough, and a block that stands in no loop is none.
        if (!needsAnotherPass && place.loop != noLoop)
            needsAnotherPass = isSideEntranceOfLoop(order, place);
    }
    return { numberOfJoins, needsAnotherPass };
}

static NEVER_INLINE FirstPassOverDominators findDominatorsInFirstPassWithLongWalks(WalksUpToDominators& walks, Arena& arena, const Order& order, std::span<Block* const> blocksInOrder, const Block** dominators, uint32_t* numbersOfJoins)
{
    WaysFurtherUpToDominators waysFurtherUp { arena, static_cast<uint32_t>(blocksInOrder.size()) };
    walks.waysFurtherUp = &waysFurtherUp;
    return findDominatorsInFirstPass<PassOverDominators::FirstWithLongWalks>(walks, order, blocksInOrder, dominators, numbersOfJoins);
}

static NEVER_INLINE void findImmediateDominatorsOfOrderedBlocks(Allocator& allocator)
{
    const Order& order = allocator.m_order;
    Arena& arena = allocator.m_arena;
    allocator.m_immediateDominators.fill(arena, order.mostBlocks(), nullptr);
    const std::span<Block* const> blocksInOrder = order.blocksInOrder();
    const uint32_t numberOfBlocks = static_cast<uint32_t>(blocksInOrder.size());
    if (!numberOfBlocks)
        return;

    Block* const* const blocks = blocksInOrder.data();
    const BlockPlace* const places = order.places();
    const Block** const dominators = allocator.m_immediateDominators.begin();
    uint32_t* const numbersOfDominators = arena.allocateArray<uint32_t>(numberOfBlocks + 1);
    WalksUpToDominators walks { places, numbersOfDominators, arena.createSpan<uint32_t>(numberOfBlocks + 1).data(), 0 };
    uint32_t* const numbersOfJoins = arena.allocateArray<uint32_t>(numberOfBlocks);
    auto [numberOfJoins, needsAnotherPass] = numberOfBlocks <= mostBlocksOfProcedureWithShortWalks
        ? findDominatorsInFirstPass<PassOverDominators::FirstWithShortWalks>(walks, order, blocksInOrder, dominators, numbersOfJoins)
        : findDominatorsInFirstPassWithLongWalks(walks, arena, order, blocksInOrder, dominators, numbersOfJoins);
    while (needsAnotherPass) {
        needsAnotherPass = false;
        for (uint32_t join = 0; join < numberOfJoins; ++join) {
            const uint32_t number = numbersOfJoins[join];
            const uint32_t indexOfBlock = blocks[number - 1]->index();
            uint32_t dominator = findNumberOfDominatorOfJoin<PassOverDominators::Later>(walks, places, numbersOfDominators, number, places[indexOfBlock]);
            needsAnotherPass |= numbersOfDominators[number] != dominator;
            numbersOfDominators[number] = dominator;
            dominators[indexOfBlock] = dominator ? blocks[dominator - 1] : nullptr;
        }
    }
}

// A value defined at this position or behind it is not defined on every path to the block, so nothing reads it from there on, however far its range goes in the order.
static ALWAYS_INLINE Position findLimitOfDefinitionsReachingHead(Allocator& allocator, const Block* target, const Block* from)
{
    if (allocator.m_immediateDominators.isEmpty()) [[unlikely]]
        findImmediateDominatorsOfOrderedBlocks(allocator);
    const Block* dominator = allocator.m_immediateDominators[target->index()];
    if (!dominator)
        return 0;
    // Paths that part at a Patch do not all have its results.
    // No two paths to the block part at `from`: Order splits every edge from a block with several successors to a block with several predecessors.
    return allocator.m_order.endOf(dominator) - (dominator == from ? 0 : 1);
}

// In the order of Allocator::takeRegister(). It evicts nothing: a step that an eviction appends here would stand in no block.
// A register in `avoided` is chosen only where no other is free.
static Reg chooseFreeRegisterAtHead(const Allocator& allocator, Bank bank, bool headIsRare, bool livesAcrossCall, RegisterMask avoided, std::initializer_list<Reg> hints)
{
    RegisterMask calleeSaved = allocator.m_file.calleeSavedIn(bank);
    RegisterMask freeRegisters = allocator.m_file.allocatableIn(bank) & ~allocator.m_bound;
    if (headIsRare || allocator.isInRareRegion())
        freeRegisters &= ~calleeSaved | allocator.m_calleeSavesTakenOutsideRareRegions;
    freeRegisters &= ~calleeSaved | allocator.calleeSavesThatMayBeTaken();
    if (livesAcrossCall && (freeRegisters & calleeSaved))
        freeRegisters &= calleeSaved;
    if (freeRegisters & ~avoided)
        freeRegisters &= ~avoided;
    if (!freeRegisters)
        return Reg();

    for (Reg hint : hints) {
        if (hint && (freeRegisters & maskOf(hint)))
            return hint;
    }
    if (RegisterMask callerSaved = freeRegisters & ~calleeSaved)
        return lowestRegisterAtEdge(callerSaved);
    if (RegisterMask savedAlready = freeRegisters & allocator.m_allocation.usedCalleeSaves())
        return lowestRegisterAtEdge(savedAlready);
    // The saves of a function that saves its frame are a run that ends in x24, so the highest register adds the fewest pairs.
    if (isARM64() && allocator.m_file.calleeSavedClobberedAtSuspensions)
        return Reg::fromIndex(static_cast<unsigned>(63 - std::countl_zero(freeRegisters)));
    return lowestRegisterAtEdge(freeRegisters);
}

// Of a block in a rare region: where the path to it last had nothing in a save. That is where it enters the region, a landing pad, or a head that an edge has fixed.
static const Block* findBlockWhereNothingLayInRareRegionSave(const Allocator& allocator, const Block* block)
{
    const Order& order = allocator.m_order;
    const Block* top = block;
    while (!order.of(top).entersRareRegion && !top->isLandingPad() && !headIsFixedByEdge(allocator, top))
        top = order.of(top).predecessors.front();
    return top;
}

// functor(Operation*) for each value that lies in a save of the rare region and nowhere else at the end of `block`, which is where the walk stands. Such a value was in a register at the head of that block.
template<typename Functor>
static ALWAYS_INLINE void forEachValueHeldOnlyInRareRegionSave(Allocator& allocator, const Block* block, const Functor& functor)
{
    for (const RegisterBinding& binding : allocator.m_allocation.registersAtHeadOf(findBlockWhereNothingLayInRareRegionSave(allocator, block))) {
        const Allocator::ValueState& state = allocator.stateOf(binding.value);
        if (!state.registers && state.saveInRareRegion != noSpill && !allocator.spillHoldsValueIn(block, binding.value))
            functor(binding.value);
    }
}

// ValueState::saveInRareRegion names a place for the whole region. Another way through the region may have written it, and this one may have come in without the value: isDeadOnEveryPathFrom().
static bool wasSavedOnWayThroughRareRegion(const Allocator& allocator, const Block* block, const Operation* value)
{
    for (const RegisterBinding& binding : allocator.m_allocation.registersAtHeadOf(findBlockWhereNothingLayInRareRegionSave(allocator, block))) {
        if (binding.value == value)
            return true;
    }
    return false;
}

bool Allocator::wayHasSavedInRareRegion(const Block* block, const Operation* value) const
{
    return wasSavedOnWayThroughRareRegion(*this, block, value);
}

// What the list of a head goes by, but for the value.
struct HeadThatListsRegisters {
    const Block* block;
    // Allocation::spillsOfValues().
    const SpillIndex* spillsOfValues;
    // Allocator::headIsAheadOfFramedPart().
    bool isAheadOfFramedPart;
};

// checkAllocation() proves each binding that Allocation::registersAtHeadOf() lists on every edge into the block.
// A spilled value is not listed, whether a register holds it or not: like any live value that is not listed it lies in its spill.
// So a Phi of the block is listed, and whatever else Allocator::spillHoldsValueAtHeadOf() does not hold of.
static ALWAYS_INLINE bool isListedAtHead(const Allocator& allocator, const HeadThatListsRegisters& head, const Operation* value)
{
    if (head.spillsOfValues[value->index()] == noSpill)
        return true;
    return isPhiAtHeadOf(head.block, value) || (head.isAheadOfFramedPart && allocator.isStoredAtHeadsOfFramedPart(value));
}

static std::span<const RegisterBinding> listRegistersAtHead(Allocator& allocator, const HeadThatListsRegisters& head)
{
    RegisterMask bound = allocator.m_bound;
    if (!bound)
        return { };
    RegisterBinding* bindings = allocator.m_arena.allocateArray<RegisterBinding>(static_cast<size_t>(std::popcount(bound)));
    size_t count = 0;
    for (; bound; bound &= bound - 1) {
        unsigned index = static_cast<unsigned>(std::countr_zero(bound));
        Operation* value = allocator.m_valueIn[index];
        if (isListedAtHead(allocator, head, value))
            new (NotNull, bindings + count++) RegisterBinding { Reg::fromIndex(index), value };
    }
    return { bindings, count };
}

// Those of the bindings that the head lists, in their order. Where that is all of them, the list that was handed in.
static std::span<const RegisterBinding> listBindingsAtHead(Allocator& allocator, const HeadThatListsRegisters& head, std::span<const RegisterBinding> bindings)
{
    const RegisterBinding* const first = bindings.data();
    const size_t size = bindings.size();
    size_t count = 0;
    while (count < size && isListedAtHead(allocator, head, first[count].value))
        ++count;
    if (count == size)
        return bindings;

    RegisterBinding* listed = allocator.m_arena.allocateArray<RegisterBinding>(size - 1);
    for (size_t index = 0; index < count; ++index)
        new (NotNull, listed + index) RegisterBinding(first[index]);
    for (size_t index = count + 1; index < size; ++index) {
        if (isListedAtHead(allocator, head, first[index].value))
            new (NotNull, listed + count++) RegisterBinding(first[index]);
    }
    return { listed, count };
}

// A head with more Phis has its lists in the arena.
static constexpr size_t mostPhisListedInLine = 8;

// By Operation::indexInBlock() of the Phi, into a list of nulls. The last Upsilon counts, as if each wrote the Phi where it was appended.
static ALWAYS_INLINE void listIncomingValuesAtEdge(const Block* from, const Block* target, Operation** incomingValues)
{
    for (const Operation* upsilon : from->upsilons()) {
        const Operation* phi = upsilon->phi();
        if (!phi || phi->owner() != target)
            continue;
        ASSERT(target->phis()[phi->indexInBlock()] == phi);
        incomingValues[phi->indexInBlock()] = upsilon->valueHandedOn();
    }
}

// The child that lies where the result of `user` would, if `user` is the last to read it. A fused Trunc has one child, which lies where the Trunc would.
static Operation* findChildThatDiesInRegisterOfResult(const Liveness& liveness, const Operation* user, unsigned childIndex)
{
    Operation* child = user->child(childIndex);
    if (liveness.isFused(child)) {
        if (child->opcode() != Trunc)
            return nullptr;
        child = child->child(0);
    }
    if (child->hasNoPosition() || liveness.of(child).lastUse + 1 != liveness.of(user).definition)
        return nullptr;
    return child;
}

// A value keeps its first hint, so each is visited once. What stands outside rare regions takes none from a rare edge: it would keep the move on the edge that counts.
static void hintRegisterFromValueDown(Allocator& allocator, Operation* value, Reg reg, bool edgeIsRare, ArenaVector<const Block*>& blocksOfHintedPhis)
{
    const Liveness& liveness = allocator.m_liveness;
    while (value) {
        // What is defined already lies where it lies.
        Position definition = liveness.of(value).definition;
        if (value->hasNoPosition() || definition == noPosition || definition <= allocator.m_position + 1)
            return;
        if (edgeIsRare && !allocator.m_order.of(value->owner()).isRare() && !liveness.isSunk(value))
            return;
        if (!value->type().isNumeric() || bankOf(value->type()) != bankOf(reg) || allocator.m_hints[value->index()])
            return;
        allocator.m_hints[value->index()] = reg;

        Opcode opcode = value->opcode();
        if (opcode == Phi) {
            const Block* block = value->owner();
            if (!allocator.m_heads[block->index()].isSet && !allocator.m_hasPassedOnHintsOfPhis.get(block->index())) {
                allocator.m_hasPassedOnHintsOfPhis.set(block->index());
                blocksOfHintedPhis.append(allocator.m_arena, block);
            }
            return;
        }

        bool overwritesFirstChild = resultPrefersRegisterOfFirstChild() && resultOverwritesFirstChild(opcode);
        if ((!overwritesFirstChild && !changesWidthOfInteger(opcode)) || liveness.isSunk(value))
            return;
        Operation* child = findChildThatDiesInRegisterOfResult(liveness, value, 0);
        if (!child && overwritesFirstChild && isCommutative(opcode))
            child = findChildThatDiesInRegisterOfResult(liveness, value, 1);
        value = child;
    }
}

// What Allocator::noteHintsOfHead() does on account of the Phis of the target, which has some. Its head is fixed, and an edge into it is still to come.
// `placeOfWalk` is BlockPlace::orderIndex of the block of the walk.
static NEVER_INLINE void passOnHintsOfPhisOfHead(Allocator& allocator, const Block* target, uint32_t placeOfWalk)
{
    const BlockPlace* const places = allocator.m_order.places();
    // The target, and behind it each block whose Phis have got hints on the way.
    const Block* blocksInLine[8];
    ArenaVector<const Block*> blocksOfHintedPhis;
    blocksOfHintedPhis.adoptStorage(blocksInLine, 8);
    blocksOfHintedPhis.appendWithinCapacity(target);
    for (uint32_t index = 0; index < blocksOfHintedPhis.size(); ++index) {
        const Block* block = blocksOfHintedPhis[index];
        const BlockPlace& placeOfBlock = places[block->index()];
        for (const Block* predecessor : placeOfBlock.predecessors) {
            const BlockPlace& placeOfPredecessor = places[predecessor->index()];
            if (placeOfPredecessor.orderIndex <= placeOfWalk || predecessor->upsilons().empty())
                continue;
            // isRareEdgeIntoHead().
            bool edgeIsRare = isRareEdgeBetweenRegions(placeOfPredecessor.rareRegion, placeOfBlock.rareRegion, predecessor, block);
            for (const Operation* upsilon : predecessor->upsilons()) {
                const Operation* phi = upsilon->phi();
                if (!phi || phi->owner() != block || allocator.m_valueFacts[upsilon->index()].has(ValueFlag::IsDropped))
                    continue;
                if (Reg reg = block == target ? allocator.m_allocation.registerAtDefinitionOf(phi) : allocator.m_hints[phi->index()])
                    hintRegisterFromValueDown(allocator, upsilon->valueHandedOn(), reg, edgeIsRare, blocksOfHintedPhis);
            }
        }
    }
}

// The hint of what a head holds beside its Phis is the register that the head holds it in.
template<bool headMayHavePhis>
static ALWAYS_INLINE void noteHintOfValueThatHeadHolds(Reg* hints, const Block* target, Reg reg, const Operation* value)
{
    if (!headMayHavePhis || !isPhiAtHeadOf(target, value))
        hints[value->index()] = reg;
}

// False where no read is left: a turn of a loop reads the value again.
// The read that says no is remembered, so the reads of rare regions ahead of it are stepped over once and not at every join.
static bool rareRegionsAloneReadBehindHead(Allocator& allocator, const Operation* value, Position head)
{
    const Liveness& liveness = allocator.m_liveness;
    if (allocator.m_knownReadsOutsideRareRegions.isEmpty())
        allocator.m_knownReadsOutsideRareRegions.fill(allocator.m_arena, allocator.m_order.mostOperations(), 0);
    Position& knownRead = allocator.m_knownReadsOutsideRareRegions[value->index()];
    if (knownRead && knownRead >= head)
        return false;

    bool isRead = false;
    for (uint32_t use = allocator.findReadFrom(value, head); use; use = liveness.use(use).next) {
        Position position = liveness.use(use).position;
        if (!liveness.isUseInRareRegion(use)) {
            knownRead = position;
            return false;
        }
        isRead = true;
    }
    return isRead;
}

// In Order::blocksInOrder(): the place of the last block whose head does not stand behind the position. The head of the block at `placeAhead` does not, and the block that is looked for is mostly near it.
static uint32_t findPlaceOfBlockAtPosition(const Order& order, Position position, uint32_t placeAhead)
{
    Block* const* const blocks = order.blocksInOrder().data();
    const uint32_t numberOfBlocks = static_cast<uint32_t>(order.blocksInOrder().size());
    const BlockPlace* const places = order.places();
    auto standsBehindPosition = [&](uint32_t place) ALWAYS_INLINE_LAMBDA {
        return places[blocks[place]->index()].head > position;
    };
    ASSERT(placeAhead < numberOfBlocks && !standsBehindPosition(placeAhead));

    // The block at `low` does not stand behind the position. The one at `high` does, or the order ends there.
    uint32_t low = placeAhead;
    uint32_t high = numberOfBlocks;
    for (uint32_t step = 1; step < numberOfBlocks - low; step *= 2) {
        if (standsBehindPosition(low + step)) {
            high = low + step;
            break;
        }
        low += step;
    }
    while (high - low > 1) {
        uint32_t middle = low + (high - low) / 2;
        if (standsBehindPosition(middle))
            high = middle;
        else
            low = middle;
    }
    return low;
}

// m_numbersOfDominatedBlocks, by the place of a block in the order.
// A block stands behind the block that m_immediateDominators names for it, so sizes add up from the last block to the first, and numbers are given out from the first to the last.
static NEVER_INLINE void numberDominatedBlocksByPlaceInOrder(Allocator& allocator)
{
    const Order& order = allocator.m_order;
    Block* const* const blocks = order.blocksInOrder().data();
    const uint32_t numberOfBlocks = static_cast<uint32_t>(order.blocksInOrder().size());
    const BlockPlace* const places = order.places();
    const Block* const* const dominators = allocator.m_immediateDominators.begin();
    allocator.m_numbersOfDominatedBlocks.fill(allocator.m_arena, numberOfBlocks, Allocator::NumbersOfDominatedBlocks { });
    Allocator::NumbersOfDominatedBlocks* const numbers = allocator.m_numbersOfDominatedBlocks.begin();
    // The place of the block above, if that stands ahead.
    constexpr uint32_t noBlockAbove = UINT32_MAX;
    uint32_t* const placesOfBlocksAbove = allocator.m_arena.allocateArray<uint32_t>(numberOfBlocks);

    // Until a block has its number, `last` counts the block and those below it.
    for (uint32_t place = numberOfBlocks; place--;) {
        uint32_t size = ++numbers[place].last;
        const Block* dominator = dominators[blocks[place]->index()];
        uint32_t placeAbove = dominator ? places[dominator->index()].orderIndex : noBlockAbove;
        if (placeAbove >= place)
            placeAbove = noBlockAbove;
        else
            numbers[placeAbove].last += size;
        placesOfBlocksAbove[place] = placeAbove;
    }
    // The entry of a block is written where the block gets its number, ahead of every block below it.
    uint32_t* const nextNumbersBelow = allocator.m_arena.allocateArray<uint32_t>(numberOfBlocks);
    uint32_t nextNumberOfRoot = 1;
    for (uint32_t place = 0; place < numberOfBlocks; ++place) {
        Allocator::NumbersOfDominatedBlocks& own = numbers[place];
        uint32_t placeAbove = placesOfBlocksAbove[place];
        uint32_t& next = placeAbove != noBlockAbove ? nextNumbersBelow[placeAbove] : nextNumberOfRoot;
        uint32_t size = own.last;
        own.first = next;
        own.last = next + size - 1;
        next += size;
        nextNumbersBelow[place] = own.first + 1;
    }
}

// Each read of a value is visited once, whoever asks and how often: what is known of a later read is taken as it stands.
// No read from the first one on stands ahead of the block at `placeAhead` in the order.
static NEVER_INLINE Allocator::NumbersOfReadingBlocks findNumbersOfBlocksThatReadFrom(Allocator& allocator, uint32_t firstRead, uint32_t placeAhead)
{
    const Liveness& liveness = allocator.m_liveness;
    const Use* const uses = allocator.m_uses;
    if (allocator.m_numbersOfBlocksThatReadFrom.isEmpty())
        allocator.m_numbersOfBlocksThatReadFrom.fill(allocator.m_arena, liveness.numberOfUses(), Allocator::NumbersOfReadingBlocks { });
    Allocator::NumbersOfReadingBlocks* const known = allocator.m_numbersOfBlocksThatReadFrom.begin();
    const Allocator::NumbersOfDominatedBlocks* const numbersOfBlocks = allocator.m_numbersOfDominatedBlocks.begin();

    // The number of its block, which is no less than one. Zero for a read in a rare region.
    struct ReadWithNumberOfBlock {
        uint32_t read;
        uint32_t number;
    };
    ReadWithNumberOfBlock readsInLine[16];
    ArenaVector<ReadWithNumberOfBlock> reads;
    reads.adoptStorage(readsInLine, 16);
    uint32_t read = firstRead;
    // The reads rise, so each block is looked for from the one before.
    for (; read && !known[read].least && !known[read].greatest; read = uses[read].next) {
        uint32_t number = 0;
        if (!liveness.isUseInRareRegion(read)) {
            placeAhead = findPlaceOfBlockAtPosition(allocator.m_order, uses[read].position, placeAhead);
            number = numbersOfBlocks[placeAhead].first;
        }
        reads.append(allocator.m_arena, ReadWithNumberOfBlock { read, number });
    }
    Allocator::NumbersOfReadingBlocks numbers = read ? known[read] : Allocator::NumbersOfReadingBlocks { UINT32_MAX, 0 };
    for (uint32_t index = reads.size(); index--;) {
        if (uint32_t number = reads[index].number) {
            numbers.least = std::min(numbers.least, number);
            numbers.greatest = std::max(numbers.greatest, number);
        }
        known[reads[index].read] = numbers;
    }
    return numbers;
}

static ALWAYS_INLINE bool blockHoldsCallOutsideRareRegions(const Allocator& allocator, const Block* block, Bank bank)
{
    Position call = allocator.m_liveness.nextCallBehindHeadOf(block, bank);
    return call != noPosition && call < allocator.m_order.endOf(block);
}

// An entry of m_nearestDominatorsWithCall of the bank, which goes by the place of a block in the order. It is found at the first question for the block or for one below it:
// the walk up ends at a block that holds a call, at one whose entry is known, or where no block stands above, and every block that it has come by gets the answer. So each entry is found once.
static NEVER_INLINE uint32_t findNearestDominatorWithCall(Allocator& allocator, Bank bank, uint32_t place)
{
    constexpr uint32_t isNotAsked = UINT32_MAX - 1;
    const Order& order = allocator.m_order;
    Block* const* const blocks = order.blocksInOrder().data();
    const BlockPlace* const places = order.places();
    const Block* const* const dominators = allocator.m_immediateDominators.begin();
    ArenaVector<uint32_t>& list = allocator.m_nearestDominatorsWithCall[static_cast<unsigned>(bank)];
    if (list.isEmpty())
        list.fill(allocator.m_arena, static_cast<uint32_t>(order.blocksInOrder().size()), isNotAsked);
    uint32_t* const nearest = list.begin();

    uint32_t placesInLine[16];
    ArenaVector<uint32_t> placesPassed;
    placesPassed.adoptStorage(placesInLine, 16);
    uint32_t answer;
    for (;;) {
        answer = nearest[place];
        if (answer != isNotAsked)
            break;
        placesPassed.append(allocator.m_arena, place);
        const Block* block = blocks[place];
        if (blockHoldsCallOutsideRareRegions(allocator, block, bank)) {
            answer = place;
            break;
        }
        // A block that stands further on counts as none.
        const Block* dominator = dominators[block->index()];
        uint32_t placeOfDominator = dominator ? places[dominator->index()].orderIndex : UINT32_MAX;
        if (placeOfDominator >= place) {
            answer = UINT32_MAX;
            break;
        }
        place = placeOfDominator;
    }
    for (uint32_t passed : placesPassed)
        nearest[passed] = answer;
    return answer;
}

// Behind rareRegionsAloneReadBehindHead() has said no, and behind findLimitOfDefinitionsReachingHead(). The value lies in registers that no callee saves, and no head of a loop around the block holds it there.
// Every way from the head to a read outside rare regions leads through a call: a block that holds one stands at or behind the head, and every path to each of those reads passes it.
// Two such blocks are asked: that of the first call behind the head, and the nearest one that every path to the first read passes.
static bool callStandsOnEveryWayToReadsBehindHead(Allocator& allocator, const Operation* value, const Block* target, const BlockPlace& place)
{
    const Liveness& liveness = allocator.m_liveness;
    const Order& order = allocator.m_order;
    const Position head = place.head;
    Bank bank = bankOf(value->type());
    Position call = liveness.nextCallBehindHeadOf(target, bank);
    if (call == noPosition)
        return false;
    // Without a read that is left, a turn of a loop reads the value again, and gets to it through the head of that loop.
    Position nextRead = allocator.m_knownReadsOutsideRareRegions[value->index()];
    if (!nextRead || nextRead < head)
        return true;
    if (call > nextRead)
        return false;

    uint32_t read = allocator.findReadFrom(value, head);
    if (!read)
        return false;
    if (allocator.m_numbersOfDominatedBlocks.isEmpty())
        numberDominatedBlocksByPlaceInOrder(allocator);
    Allocator::NumbersOfReadingBlocks reads = findNumbersOfBlocksThatReadFrom(allocator, read, place.orderIndex);
    auto everyPathToReadsPasses = [&](uint32_t placeOfBlock) {
        const Allocator::NumbersOfDominatedBlocks& dominated = allocator.m_numbersOfDominatedBlocks[placeOfBlock];
        return dominated.first <= reads.least && reads.greatest <= dominated.last;
    };
    if (reads.least > reads.greatest)
        return false;
    // Neither the call nor the read stands ahead of the head, and the read does not stand ahead of the call.
    uint32_t placeOfCall = findPlaceOfBlockAtPosition(order, call, place.orderIndex);
    if (everyPathToReadsPasses(placeOfCall))
        return true;

    // A call behind the first read in its block says nothing.
    uint32_t placeOfBlock = findPlaceOfBlockAtPosition(order, nextRead, placeOfCall);
    const Block* block = order.blocksInOrder().data()[placeOfBlock];
    if (!blockHoldsCallOutsideRareRegions(allocator, block, bank) || liveness.nextCallBehindHeadOf(block, bank) > nextRead) {
        block = allocator.m_immediateDominators[block->index()];
        if (!block)
            return false;
        placeOfBlock = order.of(block).orderIndex;
    }
    uint32_t blockWithCall = findNearestDominatorWithCall(allocator, bank, placeOfBlock);
    return blockWithCall != UINT32_MAX && blockWithCall >= place.orderIndex && everyPathToReadsPasses(blockWithCall);
}

// The head of a loop around the block holds the value in one of these registers, and an edge back to that head is still to come.
// A hint says less: every head with an edge to come has left one, and it stays when those edges have been served.
static bool headOfLoopAroundHoldsValueIn(const Allocator& allocator, const BlockPlace& place, const Operation* value, RegisterMask registers)
{
    std::span<const Loop> loops = allocator.m_order.loops();
    for (uint32_t loop = place.isLoopHead ? loops[place.loop].parent : place.loop; loop != noLoop; loop = loops[loop].parent) {
        const Allocator::Snapshot& head = allocator.m_heads[allocator.m_order.blocksInOrder()[loops[loop].firstBlock]->index()];
        for (const RegisterBinding& binding : head.bindings) {
            if (binding.value == value && (registers & maskOf(binding.reg)))
                return true;
        }
    }
    return false;
}

// Of a value that is live at the head and spilled, and lies in these registers. The head stands outside rare regions. True: the head does without the value.
static NEVER_INLINE bool headLetsGoOfSpilledValue(Allocator& allocator, const Block* target, const BlockPlace& place, const Operation* value, RegisterMask held)
{
    // Allocator::spillHoldsValueAtHeadOf().
    if (allocator.headIsAheadOfFramedPart(target) && allocator.isStoredAtHeadsOfFramedPart(value))
        return false;

    // A rare region loads what it reads. Held here, the value would be loaded on every other edge that has lost it. What a head expects whose edges are still to come stays: the head of a loop around.
    const Reg hint = allocator.m_hints[value->index()];
    const bool isExpectedFurtherOn = hint && (held & maskOf(hint));
    if (!isExpectedFurtherOn && rareRegionsAloneReadBehindHead(allocator, value, place.head))
        return true;
    // A call takes the register. Held here for reads that all stand behind one, the value would be loaded for nothing on every other edge that has lost it.
    // Where rareRegionsAloneReadBehindHead() has been asked above it has said no, and would say so again.
    return !place.isLoopHead && !(held & allocator.m_file.calleeSaved)
        && !headOfLoopAroundHoldsValueIn(allocator, place, value, held)
        && ((isExpectedFurtherOn && rareRegionsAloneReadBehindHead(allocator, value, place.head)) || callStandsOnEveryWayToReadsBehindHead(allocator, value, target, place));
}

static bool valueMadeAgainNamesFrameAtEdge(const Operation* value)
{
    if (value->opcode() == FramePointer || value->opcode() == SlotBase)
        return true;
    for (const Operation* child : value->children()) {
        if (valueMadeAgainNamesFrameAtEdge(child))
            return true;
    }
    return false;
}

// Of a value that is made again and lies in the register at the end of the block that fixes the head. The head stands outside rare regions and starts no loop.
// The register outlasts the way to the next read, so that read need not make the value again.
// The head of a loop holds what bringInWhatLoopReads() chooses. What a loop around keeps and has lost its register there is made again where that loop expects it.
static NEVER_INLINE bool isWorthHoldingWhatIsMadeAgainAtHead(Allocator& allocator, const Block* target, Position head, Reg reg, const Operation* value)
{
    ASSERT(allocator.m_liveness.of(value).has(ValueFlag::IsRematerialized));
    ASSERT(!allocator.m_order.of(target).isRare() && !allocator.m_order.of(target).isLoopHead && allocator.m_order.headOf(target) == head);
    if (valueMadeAgainNamesFrameAtEdge(value) || allocator.isKeptThroughLoopAround(value, head))
        return false;
    Position read = allocator.findFirstReadFromHeadOf(target, value);
    if (read == noPosition)
        return false;
    return (maskOf(reg) & allocator.m_file.calleeSaved) || read < allocator.m_liveness.nextCallBehindHeadOf(target, bankOf(reg));
}

// Of a spilled value that a head outside rare regions holds. The first to read it from there on is the first call behind the head, which takes the register and wants the value somewhere else.
// Held, the value is moved there. Not held, it is loaded there, or read where it lies. So an edge that brought it to the register would do that for nothing.
static bool firstCallBehindHeadReadsValueElsewhere(Allocator& allocator, const Block* target, const RegisterBinding& binding)
{
    if (maskOf(binding.reg) & allocator.m_file.calleeSaved)
        return false;
    Position call = allocator.m_liveness.nextCallBehindHeadOf(target, bankOf(binding.reg));
    if (call == noPosition || allocator.findFirstReadFromHeadOf(target, binding.value) != call - 1)
        return false;

    const Order& order = allocator.m_order;
    const Block* block = order.blocksInOrder().data()[findPlaceOfBlockAtPosition(order, call, order.of(target).orderIndex)];
    const PatchOperation* patch = block->at((call - 1 - order.earlyPositionOf(block, 0)) / 2)->as<PatchOperation>();
    if (!patch)
        return false;
    // An Upsilon of the block that the call ends reads at the same position.
    bool isReadByCall = false;
    for (unsigned child = 0; child < patch->numChildren(); ++child) {
        if (patch->child(child) != binding.value)
            continue;
        isReadByCall = true;
        const ValueRep& constraint = patch->constraintOf(child);
        switch (constraint.kind()) {
        case ValueRep::Register:
            if (constraint.reg() == binding.reg)
                return false;
            break;
        case ValueRep::ColdAny:
        case ValueRep::LateColdAny:
            break;
        case ValueRep::WarmAny:
        case ValueRep::SomeRegister:
        case ValueRep::SomeEarlyRegister:
        case ValueRep::SomeLateRegister:
        case ValueRep::StackArgument:
        case ValueRep::Stack:
        case ValueRep::Constant:
            return false;
        }
    }
    return isReadByCall;
}

bool Allocator::passThroughBlockThatOnlyJumps(Block* blockAsked)
{
    // Four blocks in five are refused. None of the questions changes anything, so those that call nothing come first, and a block that they refuse costs no frame.
    if (blockAsked->size() != 1)
        return false;
    if (blockAsked->last()->opcode() != Jump || !blockAsked->phis().empty() || blockAsked->isEntry() || blockAsked->isLandingPad())
        return false;
    const BlockPlace& placeOfBlockAsked = m_order.places()[blockAsked->index()];
    if (placeOfBlockAsked.predecessors.size() != 1 || !m_liveness.sunkInto(blockAsked).empty())
        return false;
    const Block* predecessor = placeOfBlockAsked.predecessors.data()[0];
    const Block* target = blockAsked->successor(0).block();
    const Snapshot& endOfPredecessor = m_ends[predecessor->index()];
    if (predecessor->last()->opcode() == Patch || !endOfPredecessor.isSet || target->isLandingPad() || !m_heads[target->index()].isSet)
        return false;
    if (!m_operandsTakenBackAtHeads.isEmpty() && m_operandsTakenBackAtHeads[blockAsked->index()].value)
        return false;

    auto passThroughUnlessFramedPartStartsThere = [this](Block* block, const BlockPlace& place, const Snapshot& end, ArenaVector<std::span<const RegisterBinding>>& registersAtHeads, ArenaVector<BlockSteps>& stepsOfBlocks, const ArenaVector<Step>& allSteps) NEVER_INLINE -> bool {
        if (m_allocation.isHeadOfFramedPart(block))
            return false;

        bool changesRareRegion = m_rareRegion != place.rareRegion;
        if (changesRareRegion && isInRareRegion())
            leaveRareRegion();
        if (listsRegistersAtHeadOf(block))
            registersAtHeads[block->index()] = listBindingsAtHead(*this, HeadThatListsRegisters { block, m_allocation.spillsOfValues(), headIsAheadOfFramedPart(block) }, end.bindings);
        if (changesRareRegion && place.isRare())
            enterRareRegion(place.rareRegion);

        m_position = place.head + 2;
        ASSERT(m_position == m_order.earlyPositionOf(block, 0));
        BlockSteps& steps = stepsOfBlocks[block->index()];
        steps.terminator = allSteps.size();
        steps.end = steps.terminator;
        appendStep(StepKind::Execute, block->last());

        passReadsOfUpsilonsBetweenBlocks(*this, block, m_position);
        appendEdgeMoves(block, 0, end);
        return true;
    };
    return passThroughUnlessFramedPartStartsThere(blockAsked, placeOfBlockAsked, endOfPredecessor, m_allocation.m_registersAtHead, m_allocation.m_blocks, m_allocation.m_steps);
}

// What follows, down to enterBlock(), enters a plain block: one that is no entrypoint and no landing pad, in a procedure without a framed part. No Branch has taken an operand back on the way to it.
// It takes what an edge has fixed for it. Or it has no Phi and takes the end of its one predecessor, and no loop around keeps a value that is made again.
// The walk stands in no other rare region than that of the block.

// What the reads of a value say of it at the head of the block.
enum class WhatReadsSayAtHead : uint8_t {
    ValueIsLive,
    ValueIsDead,
    // Allocator::isDeadOnEveryPathFrom() has the last word. Of a rare block it says nothing, so there the value is live.
    WaysOnDecide,
};

// What they go by, whichever value is asked for. The allocator has all of it, so it is taken anew behind a call and nothing of it has to outlast one.
struct HeadBeingEntered {
    const Use* uses;
    const ValueFacts* factsOfValues;
    Allocator::ValueState* states;
    Position head;
    // What the block itself reads is not dead, and in a rare block every read speaks for the value.
    Position endOfReadsThatSpeakForValue;
    // Use::greatestHeadOfBlockPassedOnEveryPath speaks for a read ahead of this position and for no other.
    Position endOfReadsThatCount;
};

static ALWAYS_INLINE HeadBeingEntered describeHeadBeingEntered(Allocator& allocator, bool blockIsRare)
{
    return {
        allocator.m_uses,
        allocator.m_valueFacts,
        allocator.m_values.begin(),
        allocator.m_headOfBlock,
        blockIsRare ? noPosition : allocator.m_endOfBlock,
        allocator.m_everyReadBehindBlockCounts ? allocator.m_endOfWhatWaysLeadToFromBlock : 0,
    };
}

// Every read of a value stands at ValueFacts::lastUse or ahead of it. So a value with a read at the head or behind it is live there,
// and any other only where its range goes on behind the head: a range that was extended over a loop ends at the head of the block behind the loop, where only what sank into that block reads.
// Entry zero of Liveness::uses() is no read and has noPosition for its position, which stands ahead of nothing.
static ALWAYS_INLINE WhatReadsSayAtHead askReadsAtHeadBeingEntered(const HeadBeingEntered& head, const Operation* value, const Allocator::ValueState& state)
{
    const Use* const uses = head.uses;
    uint32_t use = state.nextUse;
    Position position = uses[use].position;
    // The walk has mostly passed whatever stands ahead of the head.
    if (position < head.head) [[unlikely]] {
        do {
            use = uses[use].next;
            position = uses[use].position;
        } while (position < head.head);
    }
    if (position < head.endOfReadsThatSpeakForValue || (position < head.endOfReadsThatCount && uses[use].greatestHeadOfBlockPassedOnEveryPath >= head.head))
        return WhatReadsSayAtHead::ValueIsLive;
    if (!use) {
        Position lastUse = head.factsOfValues[value->index()].lastUse;
        if (lastUse == noPosition || lastUse <= head.head)
            return WhatReadsSayAtHead::ValueIsDead;
    }
    return WhatReadsSayAtHead::WaysOnDecide;
}

// Where the registers are not as the predecessor left them, they are those of whichever block the walk has left, and many hold what the head holds in them. Such a register is left as it is,
// and whatever is bound beside what the head holds is unbound in the end. The registers come out as if all had been unbound first.
static ALWAYS_INLINE void holdValueAtHeadBeingEntered(Allocator& allocator, Allocator::ValueState* states, Reg reg, Operation* value)
{
    Operation* current = allocator.m_valueIn[reg.index()];
    if (current == value)
        return;
    if (current)
        unbindRegisterBetweenBlocks(allocator, reg, states[current->index()]);
    bindFreeRegisterBetweenBlocks(allocator, reg, value, states[value->index()], 0);
}

// noteCalleeSaveUse() of each register that the head holds, once the head holds them all. Without a framed part nothing in between asks for what it notes.
static ALWAYS_INLINE RegisterMask calleeSavesToNoteAtHeadBeingEntered(const Allocator& allocator, RegisterMask registersHeld)
{
    return registersHeld & calleeSavesStillToNoteBetweenBlocks(allocator);
}

static NEVER_INLINE void noteCalleeSavesAtHeadBeingEntered(Allocator& allocator, RegisterMask registers)
{
    forEachRegister(registers, [&](Reg reg) {
        allocator.noteCalleeSaveUse(reg);
    });
}

// Of a block of which Allocator::listsRegistersAtHeadOf() holds. Each binding has a register of its own, so the head holds all that it has taken where it holds as many registers.
static NEVER_INLINE void listRegistersAtHeadBeingEntered(Allocator& allocator, const Block* block, const BlockPlace& place, const Allocator::Snapshot& taken, RegisterMask registersHeld, ArenaVector<std::span<const RegisterBinding>>& registersAtHeads)
{
    if (RegisterMask registersToNote = calleeSavesToNoteAtHeadBeingEntered(allocator, registersHeld)) [[unlikely]]
        noteCalleeSavesAtHeadBeingEntered(allocator, registersToNote);

    const HeadThatListsRegisters headThatLists { block, allocator.m_allocation.spillsOfValues(), false };
    const RegisterBinding* const first = taken.bindings.data();
    const RegisterBinding* const limit = first + taken.bindings.size();
    bool snapshotMatchesHead = static_cast<size_t>(std::popcount(registersHeld)) == taken.bindings.size();
    for (const RegisterBinding* binding = first; snapshotMatchesHead && binding != limit; ++binding)
        snapshotMatchesHead = isListedAtHead(allocator, headThatLists, binding->value);
    registersAtHeads[block->index()] = snapshotMatchesHead ? taken.bindings : listRegistersAtHead(allocator, headThatLists);

    if (allocator.m_rareRegion != place.rareRegion)
        allocator.enterRareRegion(place.rareRegion);
}

template<bool registersAreAsTaken>
static ALWAYS_INLINE void finishEnteringPlainBlock(Allocator& allocator, const Block* block, const BlockPlace& place, bool blockIsRare, const Allocator::Snapshot& taken, RegisterMask registersHeld, ArenaVector<std::span<const RegisterBinding>>& registersAtHeads)
{
    if constexpr (registersAreAsTaken)
        registersHeld = allocator.m_bound;
    else if (RegisterMask others = allocator.m_bound & ~registersHeld)
        unbindRegistersBetweenBlocks(allocator, others);

    if (blockIsRare || allocator.m_proc.traits().checksAllocation)
        return listRegistersAtHeadBeingEntered(allocator, block, place, taken, registersHeld, registersAtHeads);
    if (RegisterMask registersToNote = calleeSavesToNoteAtHeadBeingEntered(allocator, registersHeld)) [[unlikely]]
        return noteCalleeSavesAtHeadBeingEntered(allocator, registersToNote);
}

// Where the registers still are as the predecessor left them, what stays is not bound anew.
// One text, compiled for each of the eight cases: most blocks are not rare and follow no Patch, and their version asks for neither at any binding.
template<bool registersAreAsTaken, bool predecessorEndsInPatch, bool blockIsRare>
static NEVER_INLINE void enterPlainBlockBehindOnePredecessor(Allocator& allocator, const Block* block, const BlockPlace& place, const Allocator::Snapshot& taken, const Operation* endOfPredecessor, ArenaVector<std::span<const RegisterBinding>>& registersAtHeads)
{
    ASSERT(blockIsRare == place.isRare() && predecessorEndsInPatch == (endOfPredecessor->opcode() == Patch));
    const bool blockHasResultsOfPatch = predecessorEndsInPatch && endOfPredecessor->owner()->successor(0).block() == block;
    RegisterMask registersHeld = 0;
    auto settle = [&](const HeadBeingEntered& head, const RegisterBinding& binding, bool isHeld) ALWAYS_INLINE_LAMBDA {
        Operation* value = binding.value;
        if constexpr (predecessorEndsInPatch) {
            if (isHeld)
                isHeld = !head.factsOfValues[value->index()].has(ValueFlag::IsRematerialized) && (blockHasResultsOfPatch || !isResultOfPatchAtEdge(endOfPredecessor, value));
        }
        if constexpr (registersAreAsTaken) {
            ASSERT(allocator.m_valueIn[binding.reg.index()] == value);
            if (!isHeld)
                unbindRegisterBetweenBlocks(allocator, binding.reg, head.states[value->index()]);
        } else if (isHeld) {
            registersHeld |= maskOf(binding.reg);
            holdValueAtHeadBeingEntered(allocator, head.states, binding.reg, value);
        }
    };

    const RegisterBinding* binding = taken.bindings.data();
    const RegisterBinding* const limit = binding + taken.bindings.size();
    while (binding != limit) {
        const HeadBeingEntered head = describeHeadBeingEntered(allocator, blockIsRare);
        do {
            WhatReadsSayAtHead said = askReadsAtHeadBeingEntered(head, binding->value, head.states[binding->value->index()]);
            if (said == WhatReadsSayAtHead::WaysOnDecide && !blockIsRare)
                break;
            settle(head, *binding, said != WhatReadsSayAtHead::ValueIsDead);
        } while (++binding != limit);
        if constexpr (!blockIsRare) {
            if (binding == limit)
                break;
            bool isLive = !allocator.isDeadOnEveryPathFrom(binding->value, block, allocator.m_headOfBlock);
            settle(describeHeadBeingEntered(allocator, blockIsRare), *binding, isLive);
            ++binding;
        }
    }
    finishEnteringPlainBlock<registersAreAsTaken>(allocator, block, place, blockIsRare, taken, registersHeld, registersAtHeads);
}

// A head that an edge has fixed holds nothing else.
static NEVER_INLINE void enterPlainBlockWhoseHeadIsFixed(Allocator& allocator, const Block* block, const BlockPlace& place, const Allocator::Snapshot& taken, ArenaVector<std::span<const RegisterBinding>>& registersAtHeads)
{
    Allocator::ValueState* const states = allocator.m_values.begin();
    for (const RegisterBinding& binding : taken.bindings)
        holdValueAtHeadBeingEntered(allocator, states, binding.reg, binding.value);
    finishEnteringPlainBlock<false>(allocator, block, place, place.isRare(), taken, taken.registers, registersAtHeads);
}

static NEVER_INLINE void leaveRareRegionAndEnterBlock(Allocator& allocator, Block* block)
{
    allocator.leaveRareRegion();
    allocator.enterBlock(block);
}

void Allocator::enterBlock(Block* block)
{
    // This part only finds out which way the block goes in, and calls nothing that it has to come back from.
    const BlockPlace& placeOfBlock = m_order.places()[block->index()];
    // Ahead of everything else. Behind it the walk stands outside rare regions, so a rare block still counts as one that changes the region.
    if (isInRareRegion() && m_rareRegion != placeOfBlock.rareRegion)
        return leaveRareRegionAndEnterBlock(*this, block);

    const bool mayBePlain = !block->isEntry() && !block->isLandingPad() && !m_allocation.hasFramedPart()
        && (m_operandsTakenBackAtHeads.isEmpty() || !m_operandsTakenBackAtHeads[block->index()].value);
    if (mayBePlain && placeOfBlock.predecessors.size() != 1) {
        const Snapshot& taken = m_heads[block->index()];
        if (taken.isSet) {
            m_blockWhoseEndRegistersHold = nullptr;
            return enterPlainBlockWhoseHeadIsFixed(*this, block, placeOfBlock, taken, m_allocation.m_registersAtHead);
        }
    } else if (mayBePlain && block->phis().empty() && placeOfBlock.head >= m_endOfLastLoopThatKeepsValueMadeAgain) {
        const Block* predecessor = placeOfBlock.predecessors.data()[0];
        const Snapshot& taken = m_ends[predecessor->index()];
        if (taken.isSet) {
            const Operation* endOfPredecessor = predecessor->last();
            const bool predecessorEndsInPatch = endOfPredecessor->opcode() == Patch;
            const bool blockIsRare = placeOfBlock.isRare();
            const bool registersAreAsTaken = predecessor == m_blockWhoseEndRegistersHold;
            m_blockWhoseEndRegistersHold = nullptr;
            ArenaVector<std::span<const RegisterBinding>>& registersAtHeads = m_allocation.m_registersAtHead;
            if (registersAreAsTaken) {
                if (!predecessorEndsInPatch) {
                    if (!blockIsRare)
                        return enterPlainBlockBehindOnePredecessor<true, false, false>(*this, block, placeOfBlock, taken, endOfPredecessor, registersAtHeads);
                    return enterPlainBlockBehindOnePredecessor<true, false, true>(*this, block, placeOfBlock, taken, endOfPredecessor, registersAtHeads);
                }
                if (!blockIsRare)
                    return enterPlainBlockBehindOnePredecessor<true, true, false>(*this, block, placeOfBlock, taken, endOfPredecessor, registersAtHeads);
                return enterPlainBlockBehindOnePredecessor<true, true, true>(*this, block, placeOfBlock, taken, endOfPredecessor, registersAtHeads);
            }
            if (!predecessorEndsInPatch) {
                if (!blockIsRare)
                    return enterPlainBlockBehindOnePredecessor<false, false, false>(*this, block, placeOfBlock, taken, endOfPredecessor, registersAtHeads);
                return enterPlainBlockBehindOnePredecessor<false, false, true>(*this, block, placeOfBlock, taken, endOfPredecessor, registersAtHeads);
            }
            if (!blockIsRare)
                return enterPlainBlockBehindOnePredecessor<false, true, false>(*this, block, placeOfBlock, taken, endOfPredecessor, registersAtHeads);
            return enterPlainBlockBehindOnePredecessor<false, true, true>(*this, block, placeOfBlock, taken, endOfPredecessor, registersAtHeads);
        }
    }

    auto enterTheLongWay = [this](Block* block, const BlockPlace& place, ArenaVector<std::span<const RegisterBinding>>& registersAtHeads) NEVER_INLINE {
        const Position head = place.head;
        const bool blockIsRare = place.isRare();
        const bool listsRegisters = listsRegistersAtHeadOf(block);
        const bool changesRareRegion = m_rareRegion != place.rareRegion;
        if (changesRareRegion && isInRareRegion())
            leaveRareRegion();

        Snapshot taken;
        const Block* predecessor = nullptr;
        if (block->isEntry() || block->isLandingPad())
            taken.isSet = true;
        else if (headBehindEdgesIsFixedByEdge(m_valueFacts, block, place))
            taken = m_heads[block->index()];
        else {
            predecessor = place.predecessors.front();
            taken = m_ends[predecessor->index()];
        }
        if (!taken.isSet) [[unlikely]] {
            m_proc.fail(FailureKind::MalformedProcedure, block->last());
            return;
        }

        // Where the registers still are as the predecessor left them, what stays is not bound anew.
        const bool registersAreAsTaken = predecessor && predecessor == m_blockWhoseEndRegistersHold;
        m_blockWhoseEndRegistersHold = nullptr;

        ValueState* const states = m_values.begin();
        const RegisterMask calleeSavesStillToNote = calleeSavesStillToNoteBetweenBlocks(*this);
        const HeadThatListsRegisters headThatLists { block, m_allocation.spillsOfValues(), headIsAheadOfFramedPart(block) };
        bool snapshotMatchesHead = listsRegisters;

        // As in holdValueAtHeadBeingEntered().
        RegisterMask registersHeld = 0;
        auto hold = [&](Reg reg, Operation* value, ValueState& state) ALWAYS_INLINE_LAMBDA {
            if (!registersAreAsTaken) {
                registersHeld |= maskOf(reg);
                Operation* current = m_valueIn[reg.index()];
                if (current != value) {
                    if (current)
                        unbindRegisterBetweenBlocks(*this, reg, states[current->index()]);
                    bindFreeRegisterBetweenBlocks(*this, reg, value, state, calleeSavesStillToNote);
                    return;
                }
            }
            ASSERT(m_valueIn[reg.index()] == value);
            if (maskOf(reg) & calleeSavesStillToNote) [[unlikely]]
                noteCalleeSaveUse(reg);
        };

        if (!predecessor) {
            // A head that an edge has fixed holds nothing else.
            for (const RegisterBinding& binding : taken.bindings) {
                hold(binding.reg, binding.value, states[binding.value->index()]);
                if (snapshotMatchesHead)
                    snapshotMatchesHead = isListedAtHead(*this, headThatLists, binding.value);
            }
        } else {
            const ValueFacts* const factsOfValues = m_valueFacts;
            const Use* const uses = m_uses;
            const Position endOfBlock = m_endOfBlock;
            const bool everyReadCounts = m_everyReadBehindBlockCounts;
            const Position endOfWhatWaysLeadTo = m_endOfWhatWaysLeadToFromBlock;
            // As in askReadsAtHeadBeingEntered(). Of a rare block isDeadOnEveryPathFrom() says nothing.
            auto isLiveAtHead = [&](const Operation* value, const ValueState& state) ALWAYS_INLINE_LAMBDA -> bool {
                uint32_t use = state.nextUse;
                while (uses[use].position < head)
                    use = uses[use].next;
                if (use) [[likely]] {
                    if (blockIsRare || uses[use].position < endOfBlock || (everyReadCounts && uses[use].position < endOfWhatWaysLeadTo && uses[use].greatestHeadOfBlockPassedOnEveryPath >= head))
                        return true;
                } else {
                    Position lastUse = factsOfValues[value->index()].lastUse;
                    if (lastUse == noPosition || lastUse <= head)
                        return false;
                    if (blockIsRare)
                        return true;
                }
                return !isDeadOnEveryPathFrom(value, block, head);
            };

            const Operation* const endOfPredecessor = predecessor->last();
            const bool predecessorEndsInPatch = endOfPredecessor->opcode() == Patch;
            const bool someValueMadeAgainIsKept = head < m_endOfLastLoopThatKeepsValueMadeAgain;
            const bool blockHasResultsOfPatch = predecessorEndsInPatch && predecessor->successor(0).block() == block;
            for (const RegisterBinding& binding : taken.bindings) {
                Operation* value = binding.value;
                const uint32_t index = value->index();
                ValueState& state = states[index];
                bool isHeld = (someValueMadeAgainIsKept && isKeptThroughLoopAround(m_hints[index], value, head)) || isLiveAtHead(value, state);
                if (isHeld && predecessorEndsInPatch)
                    isHeld = !factsOfValues[index].has(ValueFlag::IsRematerialized) && (blockHasResultsOfPatch || !isResultOfPatchAtEdge(endOfPredecessor, value));
                if (isHeld)
                    hold(binding.reg, value, state);
                else if (registersAreAsTaken)
                    unbindRegisterBetweenBlocks(*this, binding.reg, state);
                if (snapshotMatchesHead)
                    snapshotMatchesHead = isHeld && isListedAtHead(*this, headThatLists, value);
            }
        }
        if (!registersAreAsTaken)
            unbindRegistersBetweenBlocks(*this, m_bound & ~registersHeld);

        // The way from a Branch that goes by overflow to its failing successor has made the operand again in the register that the sum took.
        if (!m_operandsTakenBackAtHeads.isEmpty()) [[unlikely]] {
            if (OperandTakenBack operand = m_operandsTakenBackAtHeads[block->index()]; operand.value) {
                unbind(operand.reg);
                bind(operand.reg, operand.value);
                snapshotMatchesHead = false;
            }
        }
        if (listsRegisters)
            registersAtHeads[block->index()] = snapshotMatchesHead ? taken.bindings : listRegistersAtHead(*this, headThatLists);

        if (changesRareRegion && blockIsRare)
            enterRareRegion(place.rareRegion);
    };
    enterTheLongWay(block, placeOfBlock, m_allocation.m_registersAtHead);
}

// What fixing a head asks of the head and of the edge that fixes it, whichever value it is asked for.
// A plain head has no Phi, starts no loop and stands outside rare regions. The block that fixes it stands outside them too and ends in no Patch,
// no loop around keeps a value that is made again, and the procedure has no framed part.
// Most of the others differ from a plain head in one thing: they have Phis, or they stand in a rare region, whether or not they have Phis and wherever the block that fixes them stands.
enum class KindOfHeadBeingFixed : uint8_t {
    Plain,
    PlainButForPhis,
    PlainButRare,
    Any,
};

struct HeadBeingFixed {
    Block* target;
    const BlockPlace* place;
    Position head;
    // Order::endOf() the target.
    Position end;
    // findLimitOfDefinitionsReachingHead().
    Position limitOfDefinitions;
    // A way leads on from the target out of the innermost loop around it, if there is one: isDeadOnEveryPathFrom() passes over no read because it stands behind that loop.
    bool everyReadBehindTargetCounts;
    bool mayHoldWhatIsMadeAgain;
    bool isRare;
    // False of every kind but KindOfHeadBeingFixed::Any, all three.
    bool startsLoop;
    bool edgeLacksPatchResults;
    bool someValueMadeAgainIsKept;
    // Null where something else ends the block that fixes the head: of every kind but KindOfHeadBeingFixed::Any.
    const Operation* patchThatEndsFrom;
    const ValueFacts* factsOfValues;
    const Use* uses;
    Allocator::ValueState* states;
};

template<KindOfHeadBeingFixed kind>
static ALWAYS_INLINE HeadBeingFixed describeHeadBeingFixed(Allocator& allocator, Block* target, const Block* from, const BlockPlace& place)
{
    const Order& order = allocator.m_order;
    const Operation* endOfFrom = from->last();
    HeadBeingFixed head;
    head.target = target;
    head.place = &place;
    head.head = place.head;
    head.end = place.head + 2 * (target->size() + 1);
    ASSERT(head.end == order.endOf(target));
    head.limitOfDefinitions = findLimitOfDefinitionsReachingHead(allocator, target, from);
    constexpr bool isOfAnyKind = kind == KindOfHeadBeingFixed::Any;
    head.isRare = kind == KindOfHeadBeingFixed::PlainButRare || (isOfAnyKind && place.isRare());
    head.startsLoop = isOfAnyKind && place.isLoopHead;
    head.everyReadBehindTargetCounts = !head.isRare && !head.startsLoop && (place.loop == noLoop || place.head < allocator.m_liveness.of(order.loops()[place.loop], order).endOfLastExitingBlock);
    head.patchThatEndsFrom = isOfAnyKind && endOfFrom->opcode() == Patch ? endOfFrom : nullptr;
    head.edgeLacksPatchResults = isOfAnyKind && from->successor(0).block() != target;
    head.someValueMadeAgainIsKept = isOfAnyKind && place.head < allocator.m_endOfLastLoopThatKeepsValueMadeAgain;
    head.mayHoldWhatIsMadeAgain = !head.patchThatEndsFrom && !head.isRare && !head.startsLoop;
    head.factsOfValues = allocator.m_valueFacts;
    head.uses = allocator.m_uses;
    head.states = allocator.m_values.begin();
    return head;
}

// Each of these questions changes nothing that another goes by, so the cheapest come first.
// Every read of a value stands at ValueFacts::lastUse or ahead of it. So a value with a read at the head or behind it is live there by its range,
// and any other only where its range goes on behind the head: isLiveAtHeadPosition().
template<KindOfHeadBeingFixed kind>
static ALWAYS_INLINE bool isLiveAtHeadBeingFixed(Allocator& allocator, const HeadBeingFixed& head, const Operation* value)
{
    const uint32_t index = value->index();
    const ValueFacts& facts = head.factsOfValues[index];
    Position definition = facts.definition;
    if constexpr (kind == KindOfHeadBeingFixed::Any) {
        // placeResults() binds an Extract where its Patch is allocated. The Extract itself stands in the first successor, behind the head.
        if (head.patchThatEndsFrom && isResultOfPatchAtEdge(head.patchThatEndsFrom, value)) {
            if (head.edgeLacksPatchResults)
                return false;
            definition = head.factsOfValues[head.patchThatEndsFrom->index()].definition;
        }
    }
    if (definition >= head.limitOfDefinitions)
        return false;
    const bool headIsRare = head.isRare;
    uint32_t use = findReadFromBetweenBlocks(allocator, head.uses, head.states[index].nextUse, value, head.head);
    if (use) [[likely]] {
        if (headIsRare || head.uses[use].position < head.end || (head.everyReadBehindTargetCounts && head.uses[use].greatestHeadOfBlockPassedOnEveryPath >= head.head))
            return true;
    } else {
        Position lastUse = facts.lastUse;
        if (lastUse == noPosition || lastUse <= head.head)
            return false;
        if (headIsRare)
            return true;
    }
    return !allocator.isDeadOnEveryPathFrom(value, head.target, head.head);
}

static NEVER_INLINE void keepValueInOneRegisterAtHead(Allocator& allocator, const ValueFacts& facts, RegisterMask held)
{
    RegisterMask calleeSaved = held & allocator.m_file.calleeSaved;
    Reg kept = lowestRegisterAtEdge(facts.has(ValueFlag::LivesAcrossCall) && calleeSaved ? calleeSaved : held);
    forEachRegister(held & ~maskOf(kept), [&](Reg other) {
        allocator.unbind(other);
    });
}

// Every other edge has to bring what stays, so a value stays in one register. What is made again and no loop around the head keeps is owed by no edge: appendEdgeMoves().
// What is made again gives way to whatever else wants a register at the head, and comes back last where its register is still free:
// the registers of such values are returned, and the values are left in `valuesMadeAgain`, by Reg::index().
template<KindOfHeadBeingFixed kind>
static ALWAYS_INLINE RegisterMask dropWhatHeadBeingFixedDoesNotHold(Allocator& allocator, const HeadBeingFixed& head, Operation** valuesMadeAgain)
{
    const SpillIndex* const spillsOfValues = allocator.m_allocation.spillsOfValues();
    RegisterMask registersOfValuesMadeAgain = 0;
    for (RegisterMask registers = allocator.m_bound; registers; registers &= registers - 1) {
        const Reg reg = lowestRegisterAtEdge(registers);
        Operation* value = allocator.m_valueIn[reg.index()];
        if (!value)
            continue;
        const uint32_t index = value->index();
        Allocator::ValueState& state = head.states[index];
        if constexpr (kind != KindOfHeadBeingFixed::Plain) {
            if (isPhiAtHeadOf(head.target, value))
                continue;
        }
        if constexpr (kind == KindOfHeadBeingFixed::Any) {
            if (head.someValueMadeAgainIsKept) [[unlikely]] {
                const Reg hint = allocator.m_hints[index];
                if (allocator.isKeptThroughLoopAround(hint, value, head.head) && (state.registers & maskOf(hint))) {
                    forEachRegister(state.registers & ~maskOf(hint), [&](Reg other) {
                        allocator.unbind(other);
                    });
                    continue;
                }
            }
        }
        if (!isLiveAtHeadBeingFixed<kind>(allocator, head, value)) {
            if (head.mayHoldWhatIsMadeAgain && head.factsOfValues[index].has(ValueFlag::IsRematerialized) && isWorthHoldingWhatIsMadeAgainAtHead(allocator, head.target, head.head, reg, value)) {
                valuesMadeAgain[reg.index()] = value;
                registersOfValuesMadeAgain |= maskOf(reg);
            }
            unbindAllBetweenBlocks(allocator, state);
            continue;
        }
        const RegisterMask held = state.registers;
        // Most values are not spilled.
        if (!head.isRare && spillsOfValues[index] != noSpill && headLetsGoOfSpilledValue(allocator, head.target, *head.place, value, held)) {
            unbindAllBetweenBlocks(allocator, state);
            continue;
        }
        if (held & (held - 1)) [[unlikely]]
            keepValueInOneRegisterAtHead(allocator, head.factsOfValues[index], held);
    }
    return registersOfValuesMadeAgain;
}

static ALWAYS_INLINE void bindValuesMadeAgainAtHead(Allocator& allocator, RegisterMask registers, Operation* const* valuesMadeAgain)
{
    if (!registers)
        return;
    const RegisterMask calleeSavesStillToNote = calleeSavesStillToNoteBetweenBlocks(allocator);
    for (; registers; registers &= registers - 1) {
        const Reg reg = lowestRegisterAtEdge(registers);
        Operation* value = valuesMadeAgain[reg.index()];
        bindFreeRegisterBetweenBlocks(allocator, reg, value, allocator.stateOf(value), calleeSavesStillToNote);
    }
}

static ALWAYS_INLINE void serveEdgesThatWaitedForHead(Allocator& allocator, const Block* target)
{
    if (allocator.m_waitingEdgesOfHeads.isEmpty())
        return;
    for (uint32_t number = allocator.m_waitingEdgesOfHeads[target->index()].first; number; number = allocator.m_waitingEdges[number - 1].next) {
        Block* waiting = allocator.m_waitingEdges[number - 1].from;
        allocator.appendEdgeMoves(waiting, allocator.m_waitingEdges[number - 1].successorIndex, allocator.m_ends[waiting->index()]);
    }
}

// takeSnapshot() into m_heads and noteHintsOfHead() in one, for every kind but KindOfHeadBeingFixed::Any. `from` is the block of the walk.
// The hints of what the head holds come ahead of those that its Phis pass on, and not behind them. The two do not meet: no Patch ends `from`, so whatever a register holds there is defined by now,
// and hintRegisterFromValueDown() neither asks for the hint of such a value nor gives it one. A Phi whose hint is passed on belongs to a head that is not fixed, so no register holds it.
template<KindOfHeadBeingFixed kind>
static ALWAYS_INLINE void setHeadAndNoteItsHints(Allocator& allocator, const Block* target, const Block* from, const BlockPlace& place)
{
    static_assert(kind != KindOfHeadBeingFixed::Any);
    ASSERT(from == allocator.m_block);
    // BlockPlace::predecessors is sorted by the order, so the last one says whether an edge into the block is still to come.
    const BlockPlace* const places = allocator.m_order.places();
    const uint32_t placeOfWalk = places[from->index()].orderIndex;
    const bool someEdgeIsToCome = places[place.predecessors.data()[place.predecessors.size() - 1]->index()].orderIndex > placeOfWalk;
    Reg* const hints = allocator.m_hints.begin();
    RegisterMask bound = allocator.m_bound;
    const size_t count = static_cast<size_t>(std::popcount(bound));
    RegisterBinding* const bindings = allocator.m_arena.allocateArray<RegisterBinding>(count);
    RegisterBinding* next = bindings;
    for (; bound; bound &= bound - 1) {
        const Reg reg = lowestRegisterAtEdge(bound);
        Operation* value = allocator.m_valueIn[reg.index()];
        new (NotNull, next++) RegisterBinding { reg, value };
        if (someEdgeIsToCome)
            noteHintOfValueThatHeadHolds<kind != KindOfHeadBeingFixed::Plain>(hints, target, reg, value);
    }
    Allocator::Snapshot& snapshot = allocator.m_heads[target->index()];
    snapshot.isSet = true;
    snapshot.bindings = std::span<const RegisterBinding>(bindings, count);
    snapshot.registers = allocator.m_bound;

    // An Upsilon hands its child to a Phi of the block that owns the Phi, so a block without Phis has no hint to pass on.
    if constexpr (kind != KindOfHeadBeingFixed::Plain) {
        if (someEdgeIsToCome && !target->phis().empty())
            passOnHintsOfPhisOfHead(allocator, target, placeOfWalk);
    }
}

// Allocator::fixHeadOf() of a plain head. It binds nothing but what is made again, in the register that it lay in. So whatever the head holds lies in the same register at the end of `from`.
static NEVER_INLINE void fixPlainHeadBetweenBlocks(Allocator& allocator, Block* target, const Block* from, const BlockPlace& place)
{
    ASSERT(from == allocator.m_block);
    const HeadBeingFixed head = describeHeadBeingFixed<KindOfHeadBeingFixed::Plain>(allocator, target, from, place);
    Operation* valuesMadeAgain[64];
    RegisterMask registersOfValuesMadeAgain = dropWhatHeadBeingFixedDoesNotHold<KindOfHeadBeingFixed::Plain>(allocator, head, valuesMadeAgain);
    bindValuesMadeAgainAtHead(allocator, registersOfValuesMadeAgain & ~allocator.m_bound, valuesMadeAgain);
    setHeadAndNoteItsHints<KindOfHeadBeingFixed::Plain>(allocator, target, from, place);
    serveEdgesThatWaitedForHead(allocator, target);
}

// edgeBringsEverythingWhereHeadExpectsIt(), by the registers themselves, which are as the block that the edge comes from leaves them.
static ALWAYS_INLINE bool registersHoldWhatHeadExpects(const Allocator& allocator, const Block* target, std::span<const RegisterBinding> head)
{
    for (const Operation* phi : target->phis()) {
        if (!allocator.m_valueFacts[phi->index()].has(ValueFlag::IsDropped))
            return false;
    }
    for (const RegisterBinding& expected : head) {
        if (allocator.m_valueIn[expected.reg.index()] != expected.value)
            return false;
    }
    return true;
}

// `from` is the block of the walk.
static ALWAYS_INLINE KindOfHeadBeingFixed findKindOfHeadToFix(const Allocator& allocator, const Block* target, const Block* from, const BlockPlace& place)
{
    if (allocator.m_allocation.hasFramedPart() || place.isLoopHead || from->last()->opcode() == Patch || place.head < allocator.m_endOfLastLoopThatKeepsValueMadeAgain)
        return KindOfHeadBeingFixed::Any;
    if (place.isRare())
        return KindOfHeadBeingFixed::PlainButRare;
    if (allocator.isInRareRegion())
        return KindOfHeadBeingFixed::Any;
    return target->phis().empty() ? KindOfHeadBeingFixed::Plain : KindOfHeadBeingFixed::PlainButForPhis;
}

// Allocator::fixHeadOf() appends the moves of the edge that fixes the head too. Beside its Phis such a head holds only what lies in the same register at the end of `from`, so the Phis alone ask for moves,
// and fixHeadOf() knows where their incoming values lay before it changed the registers. Nobody then asks for the list of the registers at that end: the block has no other successor.
static ALWAYS_INLINE bool headIsFixedTogetherWithMovesOfEdge(KindOfHeadBeingFixed kind, const Block* from)
{
    return kind == KindOfHeadBeingFixed::PlainButForPhis && from->numSuccessors() == 1;
}

static Location findPlaceOutsideRegistersAtEndOf(Allocator&, const Block* from, const Operation* value, bool isServedLate);

// Adds to m_pendingMoves what the Phis of the target ask of the edge. placeAtEndOf(Operation* incoming, size_t indexOfPhi, Location wanted). False: the procedure has failed.
template<typename Functor>
static ALWAYS_INLINE bool addMovesOfPhisAtEdge(Allocator& allocator, const Block* from, const Block* target, Operation* const* incomingValues, const Functor& placeAtEndOf)
{
    for (Operation* phi : target->phis()) {
        if (allocator.m_valueFacts[phi->index()].has(ValueFlag::IsDropped))
            continue;
        Reg reg = allocator.m_allocation.registerAtDefinitionOf(phi);
        Location destination = reg ? Location::reg(reg) : Location::spill(allocator.m_allocation.spillOf(phi));
        Operation* incoming = incomingValues[phi->indexInBlock()];
        Location source = incoming ? placeAtEndOf(incoming, phi->indexInBlock(), destination) : Location();
        // The moves stand ahead of the Patch that defines it.
        if (!source || (source != destination && isResultOfPatchEndingBlock(from, incoming))) {
            allocator.m_proc.fail(FailureKind::MalformedProcedure, phi);
            return false;
        }
        if (source != destination)
            allocator.m_pendingMoves.append(allocator.m_arena, PendingMove { incoming, destination, source, widthOf(incoming->type()) });
    }
    return true;
}

// m_pendingMoves, which is not empty, as steps of the block that the edge comes from.
static ALWAYS_INLINE void appendPendingMovesOfEdge(Allocator& allocator, const Block* from, bool isServedLate, ArenaVector<BlockSteps>& stepsOfBlocks, ArenaVector<Step>& allSteps)
{
    if (!from->last()->isUnconditionalJump()) {
        for (const PendingMove& move : allocator.m_pendingMoves) {
            if (move.destination.isRegister()) {
                allocator.m_proc.fail(FailureKind::MalformedProcedure, from->last());
                return;
            }
        }
    }

    BlockSteps& steps = stepsOfBlocks[from->index()];
    if (from->numSuccessors() == 1) {
        RELEASE_ASSERT(steps.beginOfEdgeMoves == steps.endOfEdgeMoves);
        steps.beginOfEdgeMoves = allSteps.size();
        resolveParallelMoves(allocator.m_arena, allocator.m_pendingMoves.span(), allSteps);
        steps.endOfEdgeMoves = allSteps.size();
        return;
    }

    // BlockSteps has moves for the edge of a block with one successor. These write memory alone, which no other successor reads,
    // so they stand among the steps of the block, ahead of the operation that ends it.
    RELEASE_ASSERT(!isServedLate && steps.terminator + 1 == allSteps.size());
    Step terminator = allSteps.takeLast();
    resolveParallelMoves(allocator.m_arena, allocator.m_pendingMoves.span(), allSteps);
    steps.end = allSteps.size();
    steps.terminator = steps.end;
    allSteps.append(allocator.m_arena, terminator);
}

void Allocator::leaveBlock(Block* from)
{
    ASSERT(from == m_block);
    const Position positionOfEdges = m_endOfBlock - 2;
    ASSERT(positionOfEdges == m_order.earlyPositionOf(from, from->size() - 1));
    passReadsOfUpsilonsBetweenBlocks(*this, from, positionOfEdges);

    const unsigned numberOfSuccessors = from->numSuccessors();
    if (!numberOfSuccessors)
        return;

    auto letEdgeWaitForHead = [this](Block* from, unsigned successorIndex, const Block* target, Position positionOfEdges) NEVER_INLINE {
        // The saves of the region are forgotten by the time the edge is served.
        if (isInRareRegion()) {
            forEachValueHeldOnlyInRareRegionSave(*this, from, [&](Operation* value) {
                if (!isLiveAtEdgePosition(m_liveness, value, positionOfEdges))
                    return;
                if (isAheadOfFramedPart(from) && isStoredAtHeadsOfFramedPart(value))
                    m_savesReadByWaitingEdges.append(m_arena, SaveReadByWaitingEdge { from, value, stateOf(value).saveInRareRegion });
                else
                    markSpilled(value);
            });
        }
        if (m_waitingEdgesOfHeads.isEmpty())
            m_waitingEdgesOfHeads.grow(m_arena, m_order.mostBlocks());
        m_waitingEdges.append(m_arena, WaitingEdge { from, successorIndex, 0 });
        WaitingEdgesOfHead& waiting = m_waitingEdgesOfHeads[target->index()];
        (waiting.last ? m_waitingEdges[waiting.last - 1].next : waiting.first) = m_waitingEdges.size();
        waiting.last = m_waitingEdges.size();
    };

    // splitEdge() leads an edge to another block where it stands. The list neither grows nor moves.
    const Edge* const successors = from->successors().data();
    const BlockPlace* const places = m_order.places();
    const bool hasFramedPart = m_allocation.hasFramedPart();
    // The registers at the end of the block are listed once somebody asks for the list, and while they still are as the block leaves them:
    // a successor that takes them, an edge that waits or has something to move, a head that is fixed ahead of another successor.
    Snapshot& end = m_ends[from->index()];
    ASSERT(!end.isSet);
    bool someSuccessorTakesRegisters = false;
    bool registersMatchEndOfBlock = true;
    for (unsigned successorIndex = 0; successorIndex < numberOfSuccessors; ++successorIndex) {
        const Edge& edge = successors[successorIndex];
        Block* target = edge.block();
        if (target->isLandingPad()) [[unlikely]] {
            // No move can stand between a Patch and its landing pad.
            for (const Operation* phi : target->phis()) {
                if (!m_liveness.isDropped(phi)) {
                    m_proc.fail(FailureKind::MalformedProcedure, phi);
                    return;
                }
            }
            continue;
        }

        someSuccessorTakesRegisters = true;
        ASSERT(end.isSet || registersMatchEndOfBlock);
        const BlockPlace& placeOfTarget = places[target->index()];
        if (target->isEntry() || !headBehindEdgesIsFixedByEdge(m_valueFacts, target, placeOfTarget)) {
            if (!end.isSet)
                takeSnapshotBetweenBlocks(*this, end);
            continue;
        }

        const Snapshot& head = m_heads[target->index()];
        if (!head.isSet) {
            if (!registersMatchEndOfBlock) {
                restore(end);
                registersMatchEndOfBlock = true;
            }
            // findBlockThatFixesHead() comes to this block at the first predecessor that it looks at. Of two blocks of the order the one with the lesser head stands ahead.
            const bool isFirstEdgeThatCounts = !hasFramedPart && placeOfTarget.predecessors.data()[0] == from && from != placeOfTarget.sourceOfUnsplitEdgeThatCountsLast
                && numberOfSuccessors <= 2 && !edge.isRare() && (!isInRareRegion() || m_rareRegion == placeOfTarget.rareRegion) && m_headOfBlock < placeOfTarget.head;
            if (!isFirstEdgeThatCounts && findBlockThatFixesHead(*this, target, placeOfTarget) != from) {
                if (!end.isSet)
                    takeSnapshotBetweenBlocks(*this, end);
                letEdgeWaitForHead(from, successorIndex, target, positionOfEdges);
                continue;
            }

            const KindOfHeadBeingFixed kind = findKindOfHeadToFix(*this, target, from, placeOfTarget);
            const bool headIsPlain = kind == KindOfHeadBeingFixed::Plain;
            // What a plain head holds lies in the same register at the end of this block, so the edge has nothing to move.
            const bool edgeIsServedWithHead = headIsPlain || headIsFixedTogetherWithMovesOfEdge(kind, from);
            // Behind the last successor nobody else asks for that end either.
            if (!end.isSet && !(edgeIsServedWithHead && successorIndex + 1 == numberOfSuccessors))
                takeSnapshotBetweenBlocks(*this, end);
            registersMatchEndOfBlock = false;
            if (headIsPlain)
                fixPlainHeadBetweenBlocks(*this, target, from, placeOfTarget);
            else
                fixHeadOf(target, from);
            if (m_proc.failure()) [[unlikely]]
                return;
            if (edgeIsServedWithHead)
                continue;
        } else if (!end.isSet) {
            // Without a framed part frameMayChangeOnEdge() says no, and appendEdgeMoves() goes by the registers where it is handed no list.
            if (hasFramedPart)
                takeSnapshotBetweenBlocks(*this, end);
            else if (registersHoldWhatHeadExpects(*this, target, head.bindings))
                continue;
        }

        appendEdgeMoves(from, successorIndex, end);
        if (m_proc.failure()) [[unlikely]]
            return;
    }
    if (someSuccessorTakesRegisters && registersMatchEndOfBlock)
        m_blockWhoseEndRegistersHold = from;
}

bool Allocator::edgeFixesHead(const Block* from, const Edge& edge) const
{
    const Block* target = edge.block();
    return headIsFixedByEdge(*this, target) && findBlockThatFixesHead(*this, target, m_order.of(target)) == from;
}

void Allocator::fixHeadOf(Block* targetOfEdge, Block* blockOfEdge)
{
    // One text for every kind. What a kind rules out is known where its version is compiled, and falls away there with all that serves it.
    auto fixHeadOfKind = [this]<KindOfHeadBeingFixed kind>(Block* target, Block* from, const BlockPlace& place, ArenaVector<Reg>& registersAtDefinitions, ArenaVector<SpillFacts>& factsOfSpills, ArenaVector<BlockSteps>& stepsOfBlocks, ArenaVector<Step>& allSteps) NEVER_INLINE {
        constexpr bool isOfAnyKind = kind == KindOfHeadBeingFixed::Any;
        const HeadBeingFixed head = describeHeadBeingFixed<kind>(*this, target, from, place);
        Operation* const* const phis = target->phis().data();
        const size_t numberOfPhis = target->phis().size();
        // Whatever else ends a block reads or writes registers behind the moves of the edge.
        const bool movesMayWriteRegisters = from->last()->isUnconditionalJump() || Order::isUnsplit(from, place);
        // Every edge into the block comes out of the framed part: findBlockThatFixesHead(). The teardown stands behind the moves of the edge and gives the callee-saved registers back to the caller,
        // so the head holds nothing in one of them. Held there, it would ask for the frame at the entry.
        bool leavesFramedPart = false;
        if constexpr (isOfAnyKind)
            leavesFramedPart = movesMayWriteRegisters && !head.isRare && isAheadOfFramedPart(target) && !isAheadOfFramedPart(from) && !m_allocation.needsFrameAheadOfFramedPart();
        // A Phi that lies in one of them gives way on every turn, and the edge back to the head moves what follows it there again.
        RegisterMask takenByPatchesOfLoop = 0;
        if constexpr (isOfAnyKind) {
            if (head.startsLoop && hasBackEdgeOutsideRareRegions(m_order.loops()[place.loop]))
                takenByPatchesOfLoop = m_liveness.of(m_order.loops()[place.loop], m_order).registersTakenByPatches;
        }

        const ValueFacts* const factsOfValues = head.factsOfValues;
        ValueState* const states = head.states;
        const Reg* const hints = m_hints.begin();
        const RegisterMask calleeSavedRegisters = m_file.calleeSaved;
        const RegisterMask calleeSavesStillToNote = calleeSavesStillToNoteBetweenBlocks(*this);

        // A Phi takes over the register of its incoming value where that value dies on the edge. One that is better off in a callee-saved register waits to see whether one is free.
        // So does one that a head further on expects in another register. The move then stands on this edge and not on that one, and another edge into this head may bring its value in that register.
        Reg registersOfDyingValuesInLine[mostPhisListedInLine];
        Reg* registersOfDyingValues = registersOfDyingValuesInLine;
        Operation* incomingValuesInLine[mostPhisListedInLine];
        Operation** incomingValues = incomingValuesInLine;
        // By Operation::indexInBlock() of the Phi, where the moves of the edge are appended here: the registers that hold its incoming value at the end of `from`.
        const bool appendsMovesOfEdge = headIsFixedTogetherWithMovesOfEdge(kind, from);
        RegisterMask registersOfIncomingValuesInLine[mostPhisListedInLine];
        RegisterMask* registersOfIncomingValues = registersOfIncomingValuesInLine;
        // ValueFlag::LivesAcrossCall goes by the order. This goes by the ways: AOTMachineCallsOnWays.h. Each Phi is asked about once, where the answer first decides something.
        enum : uint8_t { IsNotAskedWhetherPhiLivesAcrossCall, PhiLivesAcrossCall, PhiLivesAcrossNoCall };
        uint8_t phisLiveAcrossCallsInLine[mostPhisListedInLine] { };
        uint8_t* phisLiveAcrossCalls = phisLiveAcrossCallsInLine;
        auto phiLivesAcrossCall = [&](size_t index, const Operation* phi, const ValueFacts& facts) ALWAYS_INLINE_LAMBDA {
            if (!facts.has(ValueFlag::LivesAcrossCall))
                return false;
            uint8_t& answer = phisLiveAcrossCalls[index];
            if (answer == IsNotAskedWhetherPhiLivesAcrossCall)
                answer = head.isRare || callStandsOnWayFromPhiToRead(*this, phi, target) ? PhiLivesAcrossCall : PhiLivesAcrossNoCall;
            return answer == PhiLivesAcrossCall;
        };
        if (numberOfPhis) {
            if (numberOfPhis > mostPhisListedInLine) [[unlikely]] {
                phisLiveAcrossCalls = m_arena.allocateArray<uint8_t>(numberOfPhis);
                std::fill_n(phisLiveAcrossCalls, numberOfPhis, IsNotAskedWhetherPhiLivesAcrossCall);
                incomingValues = m_arena.createSpan<Operation*>(numberOfPhis).data();
                registersOfDyingValues = m_arena.createSpan<Reg>(numberOfPhis).data();
                registersOfIncomingValues = m_arena.allocateArray<RegisterMask>(numberOfPhis);
            } else
                std::fill_n(incomingValuesInLine, mostPhisListedInLine, nullptr);
            listIncomingValuesAtEdge(from, target, incomingValues);
            if (appendsMovesOfEdge) {
                // The registers still are as that block leaves them.
                for (size_t index = 0; index < numberOfPhis; ++index)
                    registersOfIncomingValues[index] = incomingValues[index] ? states[incomingValues[index]->index()].registers : 0;
            }
            for (size_t index = 0; index < numberOfPhis; ++index) {
                Operation* phi = phis[index];
                const ValueFacts& facts = factsOfValues[phi->index()];
                if (facts.has(ValueFlag::IsDropped))
                    continue;
                Operation* incoming = incomingValues[index];
                if (!incoming) {
                    m_proc.fail(FailureKind::MalformedProcedure, phi);
                    return;
                }
                // A Patch may have written the register of a value that nothing reads behind it.
                if (head.patchThatEndsFrom && (head.edgeLacksPatchResults || !isResultOfPatchAtEdge(head.patchThatEndsFrom, incoming)))
                    continue;
                ValueState& stateOfIncoming = states[incoming->index()];
                RegisterMask held = stateOfIncoming.registers;
                if (leavesFramedPart)
                    held &= ~calleeSavedRegisters;
                if (!held || isLiveAtHeadBeingFixed<kind>(*this, head, incoming))
                    continue;

                RegisterMask calleeSaved = held & calleeSavedRegisters;
                const bool livesAcrossCall = phiLivesAcrossCall(index, phi, facts);
                RegisterMask candidates = livesAcrossCall && calleeSaved ? calleeSaved : held;
                Reg hintOfLaterHead = hints[phi->index()];
                bool isBetterOffElsewhere = (livesAcrossCall && !calleeSaved) || (hintOfLaterHead && !(candidates & maskOf(hintOfLaterHead))) || !(candidates & ~takenByPatchesOfLoop);
                if (candidates & ~takenByPatchesOfLoop)
                    candidates &= ~takenByPatchesOfLoop;
                if (isBetterOffElsewhere && movesMayWriteRegisters) {
                    registersOfDyingValues[index] = lowestRegisterAtEdge(candidates);
                    continue;
                }
                // hintOf().
                Reg hint = hintOfLaterHead ? hintOfLaterHead : facts.hint;
                Reg reg = hint && (candidates & maskOf(hint)) ? hint : lowestRegisterAtEdge(candidates);
                unbindRegisterBetweenBlocks(*this, reg, stateOfIncoming);
                bindFreeRegisterBetweenBlocks(*this, reg, phi, states[phi->index()], calleeSavesStillToNote);
            }
        }

        Operation* valuesMadeAgain[64];
        RegisterMask registersOfValuesMadeAgain = dropWhatHeadBeingFixedDoesNotHold<kind>(*this, head, valuesMadeAgain);
        if (leavesFramedPart) {
            registersOfValuesMadeAgain &= ~calleeSavedRegisters;
            for (RegisterMask registers = m_bound & calleeSavedRegisters; registers; registers &= registers - 1) {
                const Reg reg = lowestRegisterAtEdge(registers);
                Operation* value = m_valueIn[reg.index()];
                RegisterMask freeRegisters = m_file.allocatableIn(bankOf(reg)) & ~calleeSavedRegisters & ~m_bound;
                if (!freeRegisters)
                    continue;
                const Reg hint = hintOf(value);
                ValueState& state = states[value->index()];
                unbindRegisterBetweenBlocks(*this, reg, state);
                bindFreeRegisterBetweenBlocks(*this, hint && (freeRegisters & maskOf(hint)) ? hint : lowestRegisterAtEdge(freeRegisters), value, state, calleeSavesStillToNote);
            }
        }

        if constexpr (isOfAnyKind) {
            if (head.startsLoop)
                chooseRegistersKeptThroughLoop(m_order.loops()[place.loop], movesMayWriteRegisters);
        }

        if (movesMayWriteRegisters) {
            for (size_t index = 0; index < numberOfPhis; ++index) {
                Operation* phi = phis[index];
                const ValueFacts& facts = factsOfValues[phi->index()];
                ValueState& state = states[phi->index()];
                if (facts.has(ValueFlag::IsDropped) || state.registers)
                    continue;
                if (Reg reg = chooseFreeRegisterAtHead(*this, bankOf(phi->type()), head.isRare, !leavesFramedPart && phiLivesAcrossCall(index, phi, facts) && (calleeSavesWorthTakingFor(phi, bankOf(phi->type()), target) & ~m_bound), leavesFramedPart ? calleeSavedRegisters : takenByPatchesOfLoop, { hints[phi->index()], registersOfDyingValues[index], facts.hint }))
                    bindFreeRegisterBetweenBlocks(*this, reg, phi, state, calleeSavesStillToNote);
            }
            if constexpr (isOfAnyKind) {
                if (head.startsLoop)
                    bringInWhatLoopReads(m_order.loops()[place.loop], head.limitOfDefinitions);
            }
        }

        // The Save of such a value stands on this path to the head and perhaps on no other.
        if constexpr (kind != KindOfHeadBeingFixed::PlainButForPhis) {
            if (isInRareRegion()) {
                forEachValueHeldOnlyInRareRegionSave(*this, from, [&](Operation* value) {
                    if (!isLiveAtHeadBeingFixed<kind>(*this, head, value))
                        return;
                    const ValueFacts& facts = factsOfValues[value->index()];
                    Reg reg = movesMayWriteRegisters ? chooseFreeRegisterAtHead(*this, bankOf(value->type()), head.isRare, facts.has(ValueFlag::LivesAcrossCall), takenByPatchesOfLoop, { hintOf(value) }) : Reg();
                    if (reg)
                        bind(reg, value);
                    else
                        markSpilled(value);
                });
            }
        }

        if (numberOfPhis) {
            const bool headLiesAheadOfFramedPart = isOfAnyKind && headIsAheadOfFramedPart(target);
            for (size_t index = 0; index < numberOfPhis; ++index) {
                Operation* phi = phis[index];
                const ValueFacts& facts = factsOfValues[phi->index()];
                if (facts.has(ValueFlag::IsDropped))
                    continue;
                Reg reg = anyRegisterOf(phi, ~RegisterMask { 0 });
                // Ahead of markSpilled(), which asks for it.
                registersAtDefinitions[phi->index()] = reg;
                bool isKeptByCalls = reg && (calleeSavedRegisters & maskOf(reg));
                if (reg && !facts.has(ValueFlag::LivesIntoLandingPad) && !facts.has(ValueFlag::LivesAcrossSuspension) && (isKeptByCalls || headLiesAheadOfFramedPart || !phiLivesAcrossCall(index, phi, facts)))
                    continue;

                SpillIndex spill = markSpilled(phi);
                if (reg)
                    continue;
                // The moves of the incoming edges write it. They are ordered by spill and not by place in the frame, so no spill that they read may share its place.
                SpillFacts& range = factsOfSpills[spill];
                for (const Block* predecessor : { place.predecessors.front(), place.predecessors.back() }) {
                    Position positionOfMoves = m_order.earlyPositionOf(predecessor, predecessor->size() - 1);
                    range.begin = std::min(range.begin, positionOfMoves);
                    range.end = std::max(range.end, positionOfMoves);
                }
            }
        }

        bindValuesMadeAgainAtHead(*this, registersOfValuesMadeAgain & ~m_bound, valuesMadeAgain);

        if constexpr (isOfAnyKind) {
            takeSnapshotBetweenBlocks(*this, m_heads[target->index()]);
            noteHintsOfHead(target);
        } else
            setHeadAndNoteItsHints<kind>(*this, target, from, place);
        serveEdgesThatWaitedForHead(*this, target);

        // What appendEdgeMoves() would do for the edge now: headIsFixedTogetherWithMovesOfEdge().
        if (appendsMovesOfEdge && !m_proc.failure()) {
            m_pendingMoves.clear();
            bool hasNotFailed = addMovesOfPhisAtEdge(*this, from, target, incomingValues, [&](Operation* incoming, size_t indexOfPhi, Location wanted) ALWAYS_INLINE_LAMBDA -> Location {
                // The wanted register where it holds the value, else the lowest one that does.
                RegisterMask held = registersOfIncomingValues[indexOfPhi];
                if (!held)
                    return findPlaceOutsideRegistersAtEndOf(*this, from, incoming, false);
                return wanted.isRegister() && (held & maskOf(wanted.reg())) ? wanted : Location::reg(lowestRegisterAtEdge(held));
            });
            if (hasNotFailed && !m_pendingMoves.isEmpty())
                appendPendingMovesOfEdge(*this, from, false, stepsOfBlocks, allSteps);
        }
    };

    const BlockPlace& placeOfTarget = m_order.places()[targetOfEdge->index()];
    ArenaVector<Reg>& registersAtDefinitions = m_allocation.m_registerAtDefinition;
    ArenaVector<SpillFacts>& factsOfSpills = m_allocation.m_spills;
    ArenaVector<BlockSteps>& stepsOfBlocks = m_allocation.m_blocks;
    ArenaVector<Step>& allSteps = m_allocation.m_steps;
    switch (findKindOfHeadToFix(*this, targetOfEdge, blockOfEdge, placeOfTarget)) {
    case KindOfHeadBeingFixed::Plain:
    case KindOfHeadBeingFixed::PlainButForPhis:
        return fixHeadOfKind.operator()<KindOfHeadBeingFixed::PlainButForPhis>(targetOfEdge, blockOfEdge, placeOfTarget, registersAtDefinitions, factsOfSpills, stepsOfBlocks, allSteps);
    case KindOfHeadBeingFixed::PlainButRare:
        return fixHeadOfKind.operator()<KindOfHeadBeingFixed::PlainButRare>(targetOfEdge, blockOfEdge, placeOfTarget, registersAtDefinitions, factsOfSpills, stepsOfBlocks, allSteps);
    case KindOfHeadBeingFixed::Any:
        return fixHeadOfKind.operator()<KindOfHeadBeingFixed::Any>(targetOfEdge, blockOfEdge, placeOfTarget, registersAtDefinitions, factsOfSpills, stepsOfBlocks, allSteps);
    }
}

Position Allocator::findFirstReadFromHeadOf(const Block* block, const Operation* value)
{
    const Position head = m_order.headOf(block);
    if (m_firstReadsFromHeads.isEmpty())
        m_firstReadsFromHeads.fill(m_arena, m_proc.numberOfOperations(), FirstReadFromHead { });
    FirstReadFromHead& known = m_firstReadsFromHeads[value->index()];
    if (known.head == head)
        return known.read;

    uint32_t use = findReadFrom(value, head);
    known = { head, use ? m_liveness.use(use).position : noPosition };
    return known.read;
}

// Both lists rise by Reg::index(). The head has no Phi that stays, and each of its registers holds the same value at the end of the block that the edge comes from.
static ALWAYS_INLINE bool edgeBringsEverythingWhereHeadExpectsIt(const ValueFacts* factsOfValues, const Block* target, const Allocator::Snapshot& head, const Allocator::Snapshot& end)
{
    if (head.registers & ~end.registers)
        return false;
    for (const Operation* phi : target->phis()) {
        if (!factsOfValues[phi->index()].has(ValueFlag::IsDropped))
            return false;
    }
    // The end binds each register of the head, so the search for it comes to it.
    const RegisterBinding* next = end.bindings.data();
    for (const RegisterBinding& expected : head.bindings) {
        const unsigned index = expected.reg.index();
        while (next->reg.index() != index)
            ++next;
        if (next->value != expected.value)
            return false;
    }
    return true;
}

// Of a value that no register holds at the end of `from`. No location: this path does not define the value.
static NEVER_INLINE Location findPlaceOutsideRegistersAtEndOf(Allocator& allocator, const Block* from, const Operation* value, bool isServedLate)
{
    const ValueFacts& facts = allocator.m_liveness.of(value);
    if (facts.has(ValueFlag::IsRematerialized))
        return Location::immediate();
    if (facts.definition > allocator.m_order.earlyPositionOf(from, from->size() - 1))
        return Location();
    if (allocator.spillHoldsValueIn(from, value))
        return Location::spill(allocator.m_allocation.spillOf(value));
    if (!isServedLate) {
        SpillIndex save = allocator.stateOf(value).saveInRareRegion;
        if (save != noSpill && wasSavedOnWayThroughRareRegion(allocator, from, value))
            return Location::spill(save);
    } else {
        for (const Allocator::SaveReadByWaitingEdge& held : allocator.m_savesReadByWaitingEdges) {
            if (held.from == from && held.value == value)
                return Location::spill(held.save);
        }
    }
    if (!allocator.m_allocation.isSpilled(value))
        return Location();
    allocator.giveUpFramedPartForReadOfSpill();
    return Location::spill(allocator.m_allocation.spillOf(value));
}

void Allocator::appendEdgeMoves(Block* blockOfEdge, unsigned indexOfEdge, const Snapshot& endOfBlockOfEdge)
{
    Block* targetOfEdge = blockOfEdge->successor(indexOfEdge).block();
    // leaveBlock() hands in the entry of m_ends for its block before anybody has asked for the list. Then the registers themselves say what lies where at the end of the block,
    // they do not bring everything where the head expects it, and the procedure has no framed part.
    const bool registersAreEndOfBlockOfEdge = !endOfBlockOfEdge.isSet;
    ASSERT(m_heads[targetOfEdge->index()].isSet);
    ASSERT(!registersAreEndOfBlockOfEdge || (blockOfEdge == m_block && &endOfBlockOfEdge == &m_ends[blockOfEdge->index()] && !m_allocation.hasFramedPart()));
    const bool edgeBringsEverything = !registersAreEndOfBlockOfEdge && edgeBringsEverythingWhereHeadExpectsIt(m_valueFacts, targetOfEdge, m_heads[targetOfEdge->index()], endOfBlockOfEdge);
    // Without a framed part frameMayChangeOnEdge() says no.
    if (edgeBringsEverything && !m_allocation.hasFramedPart())
        return;

    auto appendTheLongWay = [this]<bool registersAreEndOfFrom>(Block* from, unsigned successorIndex, const Snapshot& endOfFrom, Block* target, bool bringsEverything, ArenaVector<BlockSteps>& stepsOfBlocks, ArenaVector<Step>& allSteps) NEVER_INLINE {
        Snapshot& head = m_heads[target->index()];
        const BlockPlace& placeOfTarget = m_order.places()[target->index()];
        const bool isUnsplit = Order::isUnsplit(from, placeOfTarget);
        if (bringsEverything) {
            if (isUnsplit && frameMayChangeOnEdge(from, target))
                splitEdge(from, successorIndex, endOfFrom);
            return;
        }
        const bool isServedLate = from != m_block;
        const RegisterBinding* const firstAtHead = head.bindings.data();
        const RegisterBinding* const limitAtHead = firstAtHead + head.bindings.size();
        const RegisterBinding* const firstAtEnd = endOfFrom.bindings.data();
        const RegisterBinding* const limitAtEnd = firstAtEnd + endOfFrom.bindings.size();

        // The wanted register where it holds the value, else the lowest one that does.
        auto placeAtEndOf = [&](Operation* value, Location wanted) ALWAYS_INLINE_LAMBDA -> Location {
            if constexpr (registersAreEndOfFrom) {
                if (RegisterMask held = m_values[value->index()].registers)
                    return wanted.isRegister() && (held & maskOf(wanted.reg())) ? wanted : Location::reg(lowestRegisterAtEdge(held));
            } else {
                Reg lowest;
                for (const RegisterBinding* binding = firstAtEnd; binding != limitAtEnd; ++binding) {
                    if (binding->value != value)
                        continue;
                    if (Location::reg(binding->reg) == wanted)
                        return wanted;
                    if (!lowest)
                        lowest = binding->reg;
                }
                if (lowest)
                    return Location::reg(lowest);
            }
            return findPlaceOutsideRegistersAtEndOf(*this, from, value, isServedLate);
        };

        m_pendingMoves.clear();
        auto addMove = [&](Operation* value, Location destination, Location source) ALWAYS_INLINE_LAMBDA {
            if (source != destination)
                m_pendingMoves.append(m_arena, PendingMove { value, destination, source, widthOf(value->type()) });
        };

        // A head that the walk has entered holds what is defined ahead of its loop, and every edge back to it has that.
        const bool headIsEntered = placeOfTarget.orderIndex <= m_order.of(m_block).orderIndex;
        const bool headLoadsWhatItReads = !headIsEntered && placeOfTarget.isRare() && !placeOfTarget.isLoopHead;
        const bool headMayLetGo = !headIsEntered && !placeOfTarget.isRare() && !placeOfTarget.isLoopHead;
        // One rare edge may pay for what the code behind the head then saves. With more of them the code grows. It is asked of a head that may let go, at the first value that is made again.
        bool isKnownWhetherEdgeMakesWhatHeadHolds = false;
        bool edgeMakesWhatHeadHolds = false;
        RegisterMask registersOfAbsentValues = 0;
        const RegisterBinding* nextAtEnd = firstAtEnd;
        for (const RegisterBinding* bindingAtHead = firstAtHead; bindingAtHead != limitAtHead; ++bindingAtHead) {
            const RegisterBinding& binding = *bindingAtHead;
            Operation* value = binding.value;
            // Both lists rise by Reg::index(). What lies where the head expects it gets no move, and the head keeps it.
            const unsigned indexOfRegister = binding.reg.index();
            if constexpr (registersAreEndOfFrom) {
                if (m_valueIn[indexOfRegister] == value)
                    continue;
            } else {
                while (nextAtEnd != limitAtEnd && nextAtEnd->reg.index() < indexOfRegister)
                    ++nextAtEnd;
                if (nextAtEnd != limitAtEnd && nextAtEnd->reg.index() == indexOfRegister && nextAtEnd->value == value)
                    continue;
            }
            if (isPhiAtHeadOf(target, value))
                continue;
            Location destination = Location::reg(binding.reg);
            Location source = placeAtEndOf(value, destination);
            if (!source) {
                if (headIsEntered) {
                    m_proc.fail(FailureKind::RegisterLacksItsValue, value);
                    return;
                }
                registersOfAbsentValues |= maskOf(binding.reg);
                continue;
            }
            // A rare region loads what it reads: one load behind the head, and none on this edge nor on any other that has lost the register.
            if (headLoadsWhatItReads && source.isSpill() && spillHoldsValueIn(from, value) && spillHoldsValueAtHeadOf(target, value)) {
                registersOfAbsentValues |= maskOf(binding.reg);
                continue;
            }
            if (headMayLetGo && source != destination) {
                // fixHeadOf() holds what is made again only while that costs nothing where time counts.
                bool isOwedByNoEdge;
                if (m_valueFacts[value->index()].has(ValueFlag::IsRematerialized)) {
                    if (!std::exchange(isKnownWhetherEdgeMakesWhatHeadHolds, true))
                        edgeMakesWhatHeadHolds = placeOfTarget.predecessors.size() <= 2 && from->last()->isUnconditionalJump() && isRareEdgeIntoHead(m_order, from, target);
                    isOwedByNoEdge = !edgeMakesWhatHeadHolds && !isKeptThroughLoopAround(binding.reg, value, placeOfTarget.head);
                } else
                    isOwedByNoEdge = spillHoldsValueAtHeadOf(target, value) && firstCallBehindHeadReadsValueElsewhere(*this, target, binding);
                if (isOwedByNoEdge) {
                    registersOfAbsentValues |= maskOf(binding.reg);
                    continue;
                }
            }
            addMove(value, destination, source);
        }

        if (const size_t numberOfPhis = target->phis().size()) {
            Operation* incomingValuesInLine[mostPhisListedInLine] { };
            Operation** incomingValues = numberOfPhis <= mostPhisListedInLine ? incomingValuesInLine : m_arena.createSpan<Operation*>(numberOfPhis).data();
            listIncomingValuesAtEdge(from, target, incomingValues);
            bool hasNotFailed = addMovesOfPhisAtEdge(*this, from, target, incomingValues, [&](Operation* incoming, size_t, Location wanted) ALWAYS_INLINE_LAMBDA {
                return placeAtEndOf(incoming, wanted);
            });
            if (!hasNotFailed)
                return;
        }

        // What one path lacks, nothing behind the head reads, what is spilled lies in its spill, and what is made again is made again. Until the walk enters the block, the head can do without any of them.
        if (registersOfAbsentValues) {
            size_t numberKept = static_cast<size_t>(limitAtHead - firstAtHead) - static_cast<size_t>(std::popcount(registersOfAbsentValues));
            RegisterBinding* kept = m_arena.allocateArray<RegisterBinding>(numberKept);
            RegisterBinding* next = kept;
            for (const RegisterBinding* binding = firstAtHead; binding != limitAtHead; ++binding) {
                if (!(registersOfAbsentValues & maskOf(binding->reg)))
                    new (NotNull, next++) RegisterBinding(*binding);
            }
            ASSERT(next == kept + numberKept);
            head.bindings = std::span<const RegisterBinding>(kept, numberKept);
            head.registers &= ~registersOfAbsentValues;
        }

        if (m_pendingMoves.isEmpty() && !(isUnsplit && frameMayChangeOnEdge(from, target)))
            return;
        if (isUnsplit) {
            // splitEdge() may list what the new block is entered with. Nothing here has changed the registers.
            if constexpr (registersAreEndOfFrom)
                takeSnapshotBetweenBlocks(*this, m_ends[from->index()]);
            from = splitEdge(from, successorIndex, endOfFrom);
        }
        if (!m_pendingMoves.isEmpty())
            appendPendingMovesOfEdge(*this, from, isServedLate, stepsOfBlocks, allSteps);
    };
    if (registersAreEndOfBlockOfEdge)
        return appendTheLongWay.operator()<true>(blockOfEdge, indexOfEdge, endOfBlockOfEdge, targetOfEdge, false, m_allocation.m_blocks, m_allocation.m_steps);
    appendTheLongWay.operator()<false>(blockOfEdge, indexOfEdge, endOfBlockOfEdge, targetOfEdge, edgeBringsEverything, m_allocation.m_blocks, m_allocation.m_steps);
}

bool Allocator::frameMayChangeOnEdge(const Block* from, const Block* target)
{
    if (!m_allocation.hasFramedPart())
        return false;
    if (m_allocation.isInFramedPart(target))
        return m_allocation.isHeadOfFramedPart(target) && m_allocation.isInFramedPart(from);
    if (m_allocation.isInFramedPart(from))
        return true;
    // One teardown serves every way out of the framed part that only returns, and a way that comes without the frame returns where it stands: planFrame().
    if (target->last()->opcode() != Return)
        return false;
    // As many edges as the procedure has blocks may lead to one Return. What splitEdge() puts on an edge stands in the framed part where its source does, so the answer holds.
    if (!m_isAskedWhetherFramedPartLeadsTo.size()) {
        m_isAskedWhetherFramedPartLeadsTo = BitSpan::create(m_arena, m_order.mostBlocks());
        m_framedPartLeadsTo = BitSpan::create(m_arena, m_order.mostBlocks());
    }
    if (!m_isAskedWhetherFramedPartLeadsTo.get(target->index())) {
        m_isAskedWhetherFramedPartLeadsTo.set(target->index());
        bool leadsThere = std::ranges::any_of(m_order.of(target).predecessors, [&](const Block* predecessor) {
            return m_allocation.isInFramedPart(predecessor);
        });
        if (leadsThere)
            m_framedPartLeadsTo.set(target->index());
    }
    return m_framedPartLeadsTo.get(target->index());
}

Block* Allocator::splitEdge(Block* from, unsigned successorIndex, const Snapshot& endOfFrom)
{
    Block* middle = Order::splitLate(m_proc, from, successorIndex);

    BlockSteps& steps = m_allocation.m_blocks[middle->index()];
    steps.begin = m_allocation.m_steps.size();
    steps.end = steps.begin;
    steps.terminator = steps.begin;
    appendStep(StepKind::Execute, middle->last());

    if (m_allocation.hasFramedPart() && m_allocation.isInFramedPart(from)) {
        m_allocation.m_isInFramedPart.set(middle->index());
        m_allocation.m_headAboveBlock[middle->index()] = m_allocation.m_headAboveBlock[from->index()];
    }
    if (!m_immediateDominators.isEmpty())
        m_immediateDominators[middle->index()] = from;
    if (listsRegistersAtHeadOf(middle))
        m_allocation.m_registersAtHead[middle->index()] = listBindingsAtHead(*this, HeadThatListsRegisters { middle, m_allocation.spillsOfValues(), headIsAheadOfFramedPart(middle) }, endOfFrom.bindings);
    return middle;
}

Allocator::Snapshot Allocator::takeSnapshot()
{
    Snapshot snapshot;
    takeSnapshotBetweenBlocks(*this, snapshot);
    return snapshot;
}

void Allocator::restore(const Snapshot& snapshot)
{
    ASSERT(snapshot.isSet);
    unbindRegistersBetweenBlocks(*this, m_bound);
    ValueState* const states = m_values.begin();
    const RegisterMask calleeSavesStillToNote = calleeSavesStillToNoteBetweenBlocks(*this);
    for (const RegisterBinding& binding : snapshot.bindings)
        bindFreeRegisterBetweenBlocks(*this, binding.reg, binding.value, states[binding.value->index()], calleeSavesStillToNote);
}

void Allocator::noteHintsOfHead(const Block* target)
{
    const BlockPlace* const places = m_order.places();
    const uint32_t placeOfWalk = places[m_block->index()].orderIndex;
    // BlockPlace::predecessors is sorted by the order, so the last one says whether an edge into the block is still to come.
    const std::span<Block* const> predecessorsOfTarget = places[target->index()].predecessors;
    if (places[predecessorsOfTarget.data()[predecessorsOfTarget.size() - 1]->index()].orderIndex <= placeOfWalk)
        return;

    // An Upsilon hands its child to a Phi of the block that owns the Phi, so a block without Phis has no hint to pass on.
    if (!target->phis().empty())
        passOnHintsOfPhisOfHead(*this, target, placeOfWalk);

    Reg* const hints = m_hints.begin();
    for (const RegisterBinding& binding : m_heads[target->index()].bindings)
        noteHintOfValueThatHeadHolds<true>(hints, target, binding.reg, binding.value);
}

const Block* Allocator::immediateDominatorOf(const Block* block)
{
    if (m_immediateDominators.isEmpty())
        findImmediateDominatorsOfOrderedBlocks(*this);
    return m_immediateDominators[block->index()];
}

const Block* Allocator::lastBlockOnEveryPathToBoth(const Block* left, const Block* right)
{
    if (m_immediateDominators.isEmpty())
        findImmediateDominatorsOfOrderedBlocks(*this);
    return findBlockThatDominatesBoth(*this, left, right);
}

bool Allocator::hasBackEdgeOutsideRareRegions(const Loop& loop) const
{
    const Block* head = m_order.blocksInOrder()[loop.firstBlock];
    std::span<Block* const> predecessors = m_order.of(head).predecessors;
    for (size_t index = predecessors.size(); index-- && m_order.of(predecessors[index]).orderIndex >= loop.firstBlock;) {
        if (!isRareEdgeIntoHead(m_order, head, index))
            return true;
    }
    return false;
}

static bool isReadOnWayOutOfBranch(const Liveness& liveness, const Operation* value, const WayOutOfBranch& way)
{
    for (uint32_t use = liveness.of(value).firstUse; use; use = liveness.use(use).next) {
        if (way.holds(liveness.use(use).position))
            return true;
    }
    return false;
}

Allocator::OperandTakenBack Allocator::findOperandThatSumMayOverwrite(const Operation* sum, std::span<const Location> sources)
{
    ASSERT(sum->owner() == m_block && sources.size() == 2);
    SumTestedByOverflow test = sumTestedByOverflowOf(m_liveness, sum);
    if (test.isWidenedWhereTestFails)
        return { };

    const Block* passes = m_block->successor(test.successorThatPasses).block();
    Position headOfPassingSide = m_order.headOf(passes);
    Operation* operands[] = { test.left(), test.right() };
    OperandTakenBack found;
    // What is left of a difference when the right operand is added again is the left one.
    for (unsigned index = 0; index < (test.subtracts() ? 1u : 2u) && !found.value; ++index) {
        Operation* operand = operands[index];
        Location other = sources[1 - index];
        // The way back makes 32 bits, and the upper half of a register that holds an Int32 is unspecified.
        if (operand->hasNoPosition() || operand->type() != Int32 || operand == operands[1 - index] || !sources[index].isRegister())
            continue;
        if (!other.isRegister() && !other.isImmediate())
            continue;
        Reg reg = sources[index].reg();
        const ValueState& state = stateOf(operand);
        if (m_valueIn[reg.index()] != operand || state.registers != maskOf(reg) || state.saveInRareRegion != noSpill || m_allocation.isSpilled(operand))
            continue;
        if (isLiveAtHeadPosition(*this, operand, headOfPassingSide) && !isDeadOnEveryPathFrom(operand, passes, headOfPassingSide))
            continue;
        found = { reg, operand };
    }
    if (!found.value)
        return { };

    // On the failing side the register holds the operand, so neither the sum nor a Trunc that has taken its register over is to be had there.
    WayOutOfBranch failingSide = wayOutOfBranchThrough(m_order, m_block->successor(!test.successorThatPasses).block());
    if (failingSide.leadsOn || isReadOnWayOutOfBranch(m_liveness, sum, failingSide))
        return { };
    for (unsigned index = sum->indexInBlock() + 1; index + 1 < m_block->size(); ++index) {
        const Operation* between = m_block->at(index);
        if (m_liveness.standsInItsBlock(between) && isReadOnWayOutOfBranch(m_liveness, between, failingSide))
            return { };
    }
    return found;
}

void Allocator::noteOperandTakenBack(const Operation* sum, OperandTakenBack operand, Location otherOperand)
{
    SumTestedByOverflow test = sumTestedByOverflowOf(m_liveness, sum);
    const Block* fails = m_block->successor(!test.successorThatPasses).block();
    if (m_operandsTakenBackAtHeads.isEmpty())
        m_operandsTakenBackAtHeads.fill(m_arena, m_proc.numberOfBlocks(), OperandTakenBack { });
    m_operandsTakenBackAtHeads[fails->index()] = operand;
    m_operandTakenBackByBranch = operand.value;
    if (otherOperand.isRegister())
        m_registersReadOnWayToFailingSuccessor = maskOf(otherOperand.reg());
}

Operation* Allocator::incomingValueOf(const Operation* phi, const Block* from) const
{
    // The last one counts, as if each wrote the Phi where it was appended.
    std::span<Operation* const> upsilons = from->upsilons();
    for (size_t index = upsilons.size(); index--;) {
        if (upsilons[index]->phi() == phi)
            return upsilons[index]->valueHandedOn();
    }
    return nullptr;
}

} } } // namespace JSC::AOT::Machine

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
