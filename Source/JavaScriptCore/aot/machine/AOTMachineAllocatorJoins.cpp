/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachineAllocator.h"

#if ENABLE(AOT)

#include "AOTInliningAcrossFiles.h"
#include "AOTMachineCallsOnWays.h"
#include <algorithm>
#include <bit>
#include <initializer_list>
#include <type_traits>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT { namespace Machine {

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

static bool isResultOfPatchEndingBlock(const Block* block, const Operation* value)
{
    const Operation* end = block->last();
    if (end->opcode() != Patch)
        return false;
    return isResultOfPatchAtEdge(end, value);
}

static ALWAYS_INLINE RegisterMask calleeSavesStillToNoteBetweenBlocks(const Allocator& allocator)
{
    if (allocator.m_isOutsideFramedPart)
        return allocator.m_file.calleeSaved;
    return allocator.m_file.calleeSaved & ~(allocator.isInRareRegion() ? allocator.m_allocation.usedCalleeSaves() : allocator.m_calleeSavesTakenOutsideRareRegions);
}

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
    if (bit & calleeSavesStillToNote) [[unlikely]]
        allocator.noteCalleeSaveUse(reg);
}

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

static ALWAYS_INLINE void unbindAllBetweenBlocks(Allocator& allocator, Allocator::ValueState& state)
{
    RegisterMask registers = state.registers;
    for (RegisterMask left = registers; left; left &= left - 1)
        allocator.m_valueIn[static_cast<unsigned>(std::countr_zero(left))] = nullptr;
    ASSERT((allocator.m_bound & registers) == registers);
    allocator.m_bound &= ~registers;
    state.registers = 0;
}

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
    snapshot.set(bindings, allocator.m_bound);
}

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

static ALWAYS_INLINE void enterRareRegionBetweenBlocks(Allocator& allocator, uint32_t region)
{
    ASSERT(!allocator.isInRareRegion());
    ASSERT(allocator.m_valuesSavedInRareRegion.isEmpty());
    allocator.m_rareRegion = region;
    allocator.m_endOfRareRegion = 0;
}

static ALWAYS_INLINE void leaveRareRegionBetweenBlocks(Allocator& allocator)
{
    ASSERT(allocator.isInRareRegion());
    Allocator::ValueState* const states = allocator.m_values.begin();
    for (const Operation* value : allocator.m_valuesSavedInRareRegion)
        states[value->index()].saveInRareRegion = noSpill;
    allocator.m_valuesSavedInRareRegion.clear();
    allocator.m_rareRegion = noRareRegion;
}

static ALWAYS_INLINE bool isDeadOnEveryPathFromHeadBetweenBlocks(Allocator& allocator, const ValueFacts& facts, const Operation* value, const Block* block, const BlockPlace& place, Position endOfWhatWaysLeadTo, uint32_t firstRead, Position positionOfFirstRead)
{
    ASSERT(!place.isRare());
    ASSERT(&facts == &allocator.m_valueFacts[value->index()]);
    ASSERT(endOfWhatWaysLeadTo == allocator.m_liveness.endOfWhatWaysLeadToFrom(block));
    ASSERT(firstRead == allocator.findReadFrom(value, place.head) && positionOfFirstRead == allocator.m_uses[firstRead].position);
    ASSERT(!firstRead || positionOfFirstRead >= allocator.m_order.endOf(block));
    const Position definition = facts.definition;
    if (definition == noPosition || definition > place.head || facts.has(ValueFlag::IsKeptForCollector))
        return false;
    if (endOfWhatWaysLeadTo != noPosition && (!firstRead || positionOfFirstRead >= endOfWhatWaysLeadTo))
        return true;
    const std::span<const Loop> loops = allocator.m_order.loops();
    if (place.loop != noLoop && !place.isLoopHead && definition < loops.data()[place.loop].begin && !loops.data()[place.loop].hasSideEntrance)
        return false;
    return isDeadOnEveryPathFromBlockOutsideRareRegions(allocator, value, block, place, place.head, firstRead);
}

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

static NEVER_INLINE const Block* findBlockThatFixesHead(const Allocator& allocator, const Block* target, const BlockPlace& place)
{
    const Order& order = allocator.m_order;
    bool startsOutsideFramedPart = allocator.startsOutsideFramedPart(target);
    const Block* firstFromOutsideFramedPart = nullptr;
    const Block* countsLast = nullptr;
    Block* const* const predecessors = place.predecessors.data();
    const size_t numberOfPredecessors = place.predecessors.size();
    for (size_t number = 0; number < numberOfPredecessors; ++number) {
        const Block* predecessor = predecessors[number];
        const BlockPlace& placeOfPredecessor = order.of(predecessor);
        if (placeOfPredecessor.orderIndex >= place.orderIndex)
            break;
        if (startsOutsideFramedPart && !allocator.isOutsideFramedPart(predecessor))
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
    return startsOutsideFramedPart && firstFromOutsideFramedPart ? firstFromOutsideFramedPart : place.predecessors.front();
}

static bool someLoopHasSideEntrance(const Order& order)
{
    for (const Loop& loop : order.loops()) {
        if (loop.hasSideEntrance)
            return true;
    }
    return false;
}

struct WaysFurtherUpToDominators {
    Arena& arena;
    uint32_t numberOfBlocks;
    uint32_t endOfBlocksWithWay { 0 };
    uint32_t* heights { nullptr };
    uint32_t* ways { nullptr };
};

struct WalksUpToDominators {
    const BlockPlace* places;
    uint32_t* numbersOfDominators;
    uint32_t* lastJoinsWalkedFor;
    uint32_t numberOfJoin;
    WaysFurtherUpToDominators* waysFurtherUp { nullptr };
};

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
    while (first != second) {
        bool partFurtherUp = waysFurtherUp[first] != waysFurtherUp[second];
        first = partFurtherUp ? waysFurtherUp[first] : numbersOfDominators[first];
        second = partFurtherUp ? waysFurtherUp[second] : numbersOfDominators[second];
    }
    ASSERT(first == foundBlockByBlock);
    return first;
}

enum class PassOverDominators : uint8_t {
    FirstWithShortWalks,
    FirstWithLongWalks,
    Later,
};

static constexpr uint32_t mostBlocksOfProcedureWithShortWalks = 2048;
static constexpr int turnsOfWalkUpBlockByBlock = 24;

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

template<PassOverDominators pass>
static ALWAYS_INLINE uint32_t findDominatorsInFirstPass(WalksUpToDominators& walks, std::span<Block* const> blocksInOrder, const Block** dominators, uint32_t* numbersOfJoins)
{
    Block* const* const blocks = blocksInOrder.data();
    const uint32_t numberOfBlocks = static_cast<uint32_t>(blocksInOrder.size());
    const BlockPlace* const places = walks.places;
    uint32_t* const numbersOfDominators = walks.numbersOfDominators;
    uint32_t numberOfJoins = 0;
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
    }
    return numberOfJoins;
}

static NEVER_INLINE uint32_t findDominatorsInFirstPassWithLongWalks(WalksUpToDominators& walks, Arena& arena, std::span<Block* const> blocksInOrder, const Block** dominators, uint32_t* numbersOfJoins)
{
    WaysFurtherUpToDominators waysFurtherUp { arena, static_cast<uint32_t>(blocksInOrder.size()) };
    walks.waysFurtherUp = &waysFurtherUp;
    const uint32_t numberOfJoins = findDominatorsInFirstPass<PassOverDominators::FirstWithLongWalks>(walks, blocksInOrder, dominators, numbersOfJoins);
    walks.waysFurtherUp = nullptr;
    if (waysFurtherUp.heights) {
        arena.letGoOfArray(waysFurtherUp.ways, waysFurtherUp.numberOfBlocks + 1);
        arena.letGoOfArray(waysFurtherUp.heights, waysFurtherUp.numberOfBlocks + 1);
    }
    return numberOfJoins;
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
    allocator.m_numbersOfDominators = numbersOfDominators;
    WalksUpToDominators walks { places, numbersOfDominators, arena.createSpan<uint32_t>(numberOfBlocks + 1).data(), 0 };
    uint32_t* const numbersOfJoins = arena.allocateArray<uint32_t>(numberOfBlocks);
    const uint32_t numberOfJoins = numberOfBlocks <= mostBlocksOfProcedureWithShortWalks
        ? findDominatorsInFirstPass<PassOverDominators::FirstWithShortWalks>(walks, blocksInOrder, dominators, numbersOfJoins)
        : findDominatorsInFirstPassWithLongWalks(walks, arena, blocksInOrder, dominators, numbersOfJoins);
    bool needsAnotherPass = someLoopHasSideEntrance(order);
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
    arena.letGoOfArray(numbersOfJoins, numberOfBlocks);
    arena.letGoOfArray(walks.lastJoinsWalkedFor, numberOfBlocks + 1);
}

static ALWAYS_INLINE Position findLimitOfDefinitionsReachingHead(Allocator& allocator, const Block* target, const Block* from)
{
    if (allocator.m_immediateDominators.isEmpty()) [[unlikely]]
        findImmediateDominatorsOfOrderedBlocks(allocator);
    const Block* dominator = allocator.m_immediateDominators[target->index()];
    if (!dominator)
        return 0;
    return allocator.m_order.endOf(dominator) - (dominator == from ? 0 : 1);
}

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
    if (isARM64() && allocator.m_file.calleeSavedClobberedAtSuspensions)
        return Reg::fromIndex(static_cast<unsigned>(63 - std::countl_zero(freeRegisters)));
    return lowestRegisterAtEdge(freeRegisters);
}

static const Block* findBlockWhereNothingLayInRareRegionSave(const Allocator& allocator, const Block* block)
{
    const Order& order = allocator.m_order;
    const Block* top = block;
    while (!order.of(top).entersRareRegion && !top->isLandingPad() && !headIsFixedByEdge(allocator, top))
        top = order.of(top).predecessors.front();
    return top;
}

template<typename Functor>
static ALWAYS_INLINE void forEachValueHeldOnlyInRareRegionSave(Allocator& allocator, const Block* block, const Functor& functor)
{
    for (const RegisterBinding& binding : allocator.m_allocation.registersAtHeadOf(findBlockWhereNothingLayInRareRegionSave(allocator, block))) {
        const Allocator::ValueState& state = allocator.stateOf(binding.value);
        if (!state.registers && state.saveInRareRegion != noSpill && !allocator.spillHoldsValueIn(block, binding.value))
            functor(binding.value);
    }
}

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

struct HeadThatListsRegisters {
    const Block* block;
    const SpillIndex* spillsOfValues;
    bool isOutsideFramedPart;
};

static ALWAYS_INLINE bool isListedAtHead(const Allocator& allocator, const HeadThatListsRegisters& head, const Operation* value)
{
    if (head.spillsOfValues[value->index()] == noSpill)
        return true;
    return isPhiAtHeadOf(head.block, value) || (head.isOutsideFramedPart && allocator.isStoredAtHeadsOfFramedPart(value));
}

static ALWAYS_INLINE std::span<const RegisterBinding> listRegistersAtHead(Allocator& allocator, const HeadThatListsRegisters& head)
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

static constexpr size_t mostPhisListedInLine = 8;

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

static Operation* findChildThatDiesInRegisterOfResult(const Liveness& liveness, const Operation* user, unsigned childIndex)
{
    Operation* child = user->child(childIndex);
    if (liveness.isFused(child)) {
        if (child->opcode() != Trunc && !namesWord(child))
            return nullptr;
        child = child->child(0);
    }
    if (child->hasNoPosition() || liveness.of(child).lastUse + 1 != liveness.of(user).definition)
        return nullptr;
    return child;
}

static void hintRegisterFromValueDown(Allocator& allocator, Operation* value, Reg reg, bool edgeIsRare, ArenaVector<const Block*>& blocksOfHintedPhis)
{
    const Liveness& liveness = allocator.m_liveness;
    while (value) {
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
            if (!allocator.m_heads[block->index()].isSet() && !allocator.m_hasPassedOnHintsOfPhis.get(block->index())) {
                allocator.m_hasPassedOnHintsOfPhis.set(block->index());
                blocksOfHintedPhis.append(allocator.m_arena, block);
            }
            return;
        }

        if (opcode == Select && !liveness.isSunk(value)) {
            Operation* arm = findChildThatDiesInRegisterOfResult(liveness, value, 1);
            value = arm ? arm : findChildThatDiesInRegisterOfResult(liveness, value, 2);
            continue;
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

static NEVER_INLINE void passOnHintsOfPhisOfHead(Allocator& allocator, const Block* target, uint32_t placeOfWalk)
{
    const BlockPlace* const places = allocator.m_order.places();
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

template<bool headMayHavePhis>
static ALWAYS_INLINE void noteHintOfValueThatHeadHolds(Reg* hints, const Block* target, Reg reg, const Operation* value)
{
    if (!headMayHavePhis || !isPhiAtHeadOf(target, value))
        hints[value->index()] = reg;
}

static ALWAYS_INLINE bool rareRegionsAloneReadBehindHead(Allocator& allocator, const Operation* value, Position head)
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

static NEVER_INLINE void numberDominatedBlocksByPlaceInOrder(Allocator& allocator)
{
    const uint32_t numberOfBlocks = static_cast<uint32_t>(allocator.m_order.blocksInOrder().size());
    const uint32_t* const numbersOfDominators = allocator.m_numbersOfDominators;
    allocator.m_numbersOfDominatedBlocks.fill(allocator.m_arena, numberOfBlocks, Allocator::NumbersOfDominatedBlocks { });
    Allocator::NumbersOfDominatedBlocks* const numbers = allocator.m_numbersOfDominatedBlocks.begin();
    uint32_t* const numbersOfBlocksAbove = allocator.m_arena.allocateArray<uint32_t>(numberOfBlocks);

    for (uint32_t place = numberOfBlocks; place--;) {
        uint32_t size = ++numbers[place].last;
        uint32_t numberAbove = numbersOfDominators[place + 1];
        ASSERT(numberAbove == (allocator.m_immediateDominators[allocator.m_order.blocksInOrder()[place]->index()] ? allocator.m_order.of(allocator.m_immediateDominators[allocator.m_order.blocksInOrder()[place]->index()]).orderIndex + 1 : 0));
        if (numberAbove > place)
            numberAbove = 0;
        else if (numberAbove)
            numbers[numberAbove - 1].last += size;
        numbersOfBlocksAbove[place] = numberAbove;
    }
    uint32_t* const nextNumbersBelow = allocator.m_arena.allocateArray<uint32_t>(numberOfBlocks + 1);
    nextNumbersBelow[0] = 1;
    for (uint32_t place = 0; place < numberOfBlocks; ++place) {
        Allocator::NumbersOfDominatedBlocks& own = numbers[place];
        uint32_t& next = nextNumbersBelow[numbersOfBlocksAbove[place]];
        uint32_t size = own.last;
        own.first = next;
        own.last = next + size - 1;
        next += size;
        nextNumbersBelow[place + 1] = own.first + 1;
    }
    allocator.m_arena.letGoOfArray(nextNumbersBelow, numberOfBlocks + 1);
    allocator.m_arena.letGoOfArray(numbersOfBlocksAbove, numberOfBlocks);
}

static NEVER_INLINE Allocator::NumbersOfReadingBlocks findNumbersOfBlocksThatReadFrom(Allocator& allocator, uint32_t firstRead, uint32_t placeAhead)
{
    const Liveness& liveness = allocator.m_liveness;
    const Use* const uses = allocator.m_uses;
    if (allocator.m_numbersOfBlocksThatReadFrom.isEmpty())
        allocator.m_numbersOfBlocksThatReadFrom.fill(allocator.m_arena, liveness.numberOfUses(), Allocator::NumbersOfReadingBlocks { });
    Allocator::NumbersOfReadingBlocks* const known = allocator.m_numbersOfBlocksThatReadFrom.begin();
    const Allocator::NumbersOfDominatedBlocks* const numbersOfBlocks = allocator.m_numbersOfDominatedBlocks.begin();

    struct ReadWithNumberOfBlock {
        uint32_t read;
        uint32_t number;
    };
    ReadWithNumberOfBlock readsInLine[16];
    ArenaVector<ReadWithNumberOfBlock> reads;
    reads.adoptStorage(readsInLine, 16);
    uint32_t read = firstRead;
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

static NEVER_INLINE uint32_t findNearestDominatorWithCall(Allocator& allocator, Bank bank, uint32_t place)
{
    constexpr uint32_t isNotAsked = UINT32_MAX - 1;
    const Order& order = allocator.m_order;
    Block* const* const blocks = order.blocksInOrder().data();
    const uint32_t* const numbersOfDominators = allocator.m_numbersOfDominators;
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
        uint32_t placeOfDominator = numbersOfDominators[place + 1] - 1;
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

static bool callStandsOnEveryWayToReadsBehindHead(Allocator& allocator, const Operation* value, const Block* target, const BlockPlace& place)
{
    const Liveness& liveness = allocator.m_liveness;
    const Order& order = allocator.m_order;
    const Position head = place.head;
    Bank bank = bankOf(value->type());
    Position call = liveness.nextCallBehindHeadOf(target, bank);
    if (call == noPosition)
        return false;
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
    uint32_t placeOfCall = findPlaceOfBlockAtPosition(order, call, place.orderIndex);
    if (everyPathToReadsPasses(placeOfCall))
        return true;

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

static bool headOfLoopAroundHoldsValueIn(Allocator& allocator, const BlockPlace& place, const Operation* value, RegisterMask registers)
{
    std::span<const Loop> loops = allocator.m_order.loops();
    const Position home = allocator.m_order.headOf(value->owner());
    const uint32_t innermost = place.isLoopHead ? loops[place.loop].parent : place.loop;
    if (innermost == noLoop)
        return false;
    auto liesAround = [&](uint32_t around, uint32_t loop) ALWAYS_INLINE_LAMBDA {
        return loops[around].firstBlock <= loops[loop].firstBlock && loops[loop].endBlock <= loops[around].endBlock;
    };
    const Allocator::WhetherHeadAroundHolds* known = allocator.m_whetherHeadsAroundHold.isEmpty() ? nullptr : &allocator.m_whetherHeadsAroundHold[value->index()];
    if (known && known->loopWhoseHeadHolds != noLoop && (registers & known->registersHeld) && liesAround(known->loopWhoseHeadHolds, innermost))
        return true;
    const bool knowsOfNone = known && known->loopFromWhichNoHeadHolds != noLoop && !(registers & ~known->registersNotHeld);
    unsigned numberOfHeadsLookedAt = 0;
    uint32_t loopWhoseHeadHolds = noLoop;
    RegisterMask registersHeld = 0;
    for (uint32_t loop = innermost; loop != noLoop && loops[loop].begin >= home; loop = loops[loop].parent) {
        if (knowsOfNone && liesAround(loop, known->loopFromWhichNoHeadHolds))
            break;
        const Allocator::Snapshot& head = allocator.m_heads[allocator.m_order.blocksInOrder()[loops[loop].firstBlock]->index()];
        for (const RegisterBinding& binding : head.bindings()) {
            if (binding.value == value)
                registersHeld |= registers & maskOf(binding.reg);
        }
        ++numberOfHeadsLookedAt;
        if (registersHeld) {
            loopWhoseHeadHolds = loop;
            break;
        }
    }
    constexpr unsigned headsWorthRemembering = 4;
    if (numberOfHeadsLookedAt >= headsWorthRemembering) {
        if (allocator.m_whetherHeadsAroundHold.isEmpty())
            allocator.m_whetherHeadsAroundHold.fill(allocator.m_arena, allocator.m_order.mostOperations(), Allocator::WhetherHeadAroundHolds { });
        Allocator::WhetherHeadAroundHolds& remembered = allocator.m_whetherHeadsAroundHold[value->index()];
        if (registersHeld) {
            remembered.loopWhoseHeadHolds = loopWhoseHeadHolds;
            remembered.registersHeld = registersHeld;
        } else {
            remembered.loopFromWhichNoHeadHolds = innermost;
            remembered.registersNotHeld = registers;
        }
    }
    return registersHeld;
}

static NEVER_INLINE bool headLetsGoOfSpilledValue(Allocator& allocator, const Block* target, const BlockPlace& place, const Operation* value, RegisterMask held, bool isExpectedFurtherOn)
{
    ASSERT(isExpectedFurtherOn == (allocator.m_hints[value->index()] && (held & maskOf(allocator.m_hints[value->index()]))));
    if (allocator.startsOutsideFramedPart(target) && allocator.isStoredAtHeadsOfFramedPart(value))
        return false;

    if (!isExpectedFurtherOn && rareRegionsAloneReadBehindHead(allocator, value, place.head))
        return true;
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

bool Allocator::firstCallBehindHeadReadsValueElsewhere(const Block* target, Reg reg, const Operation* value)
{
    if (maskOf(reg) & m_file.calleeSaved)
        return false;
    Position call = m_liveness.nextCallBehindHeadOf(target, bankOf(reg));
    if (call == noPosition || findFirstReadFromHeadOf(target, value) != call - 1)
        return false;

    const Order& order = m_order;
    const Block* block = order.blocksInOrder().data()[findPlaceOfBlockAtPosition(order, call, order.of(target).orderIndex)];
    const PatchOperation* patch = block->at((call - 1 - order.earlyPositionOf(block, 0)) / 2)->as<PatchOperation>();
    if (!patch)
        return false;
    bool isReadByCall = false;
    for (unsigned child = 0; child < patch->numChildren(); ++child) {
        if (patch->child(child) != value)
            continue;
        isReadByCall = true;
        const ValueRep& constraint = patch->constraintOf(child);
        switch (constraint.kind()) {
        case ValueRep::Register:
            if (constraint.reg() == reg)
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

struct WhatCallInBlockOfHeadTakes {
    bool isKnown { false };
    RegisterMask registers { 0 };
    const Operation* valueIn[64];
};

static NEVER_INLINE void findWhatCallInBlockOfHeadTakes(const Allocator& allocator, const Block* target, WhatCallInBlockOfHeadTakes& taken)
{
    const Order& order = allocator.m_order;
    taken.isKnown = true;
    for (Bank bank : { Bank::GP, Bank::FP }) {
        Position call = allocator.m_liveness.nextCallBehindHeadOf(target, bank);
        if (call == noPosition || call >= order.endOf(target))
            continue;
        const PatchOperation* patch = target->at((call - 1 - order.earlyPositionOf(target, 0)) / 2)->as<PatchOperation>();
        if (!patch)
            continue;
        for (unsigned child = 0; child < patch->numChildren(); ++child) {
            const ValueRep& constraint = patch->constraintOf(child);
            if (!constraint.isReg() || bankOf(constraint.reg()) != bank)
                continue;
            taken.registers |= maskOf(constraint.reg());
            taken.valueIn[constraint.reg().index()] = patch->child(child);
        }
    }
}

static ALWAYS_INLINE RegisterMask registersWhereCallInBlockOfHeadTakes(const Allocator& allocator, const Block* target, WhatCallInBlockOfHeadTakes& taken, const Operation* value, RegisterMask among)
{
    if (!taken.isKnown)
        findWhatCallInBlockOfHeadTakes(allocator, target, taken);
    RegisterMask found = 0;
    for (RegisterMask registers = among & taken.registers; registers; registers &= registers - 1) {
        if (taken.valueIn[std::countr_zero(registers)] == value)
            found |= registers & -registers;
    }
    return found;
}

RegisterMask Allocator::registersWhereCallInBlockTakesWhatLiesThere(const Block* block) const
{
    WhatCallInBlockOfHeadTakes taken;
    findWhatCallInBlockOfHeadTakes(*this, block, taken);
    RegisterMask found = 0;
    for (RegisterMask registers = m_bound & taken.registers; registers; registers &= registers - 1) {
        if (taken.valueIn[std::countr_zero(registers)] == m_valueIn[std::countr_zero(registers)])
            found |= registers & -registers;
    }
    return found;
}

bool Allocator::passThroughBlockThatOnlyJumps(Block* blockAsked)
{
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
    if (predecessor->last()->opcode() == Patch || !endOfPredecessor.isSet() || target->isLandingPad() || !m_heads[target->index()].isSet())
        return false;
    if (!m_operandsTakenBackAtHeads.isEmpty() && m_operandsTakenBackAtHeads[blockAsked->index()].value)
        return false;

    auto passThroughUnlessFramedPartStartsThere = [this](Block* block, const BlockPlace& place, const Snapshot& end, ArenaVector<std::span<const RegisterBinding>>& registersAtHeads, ArenaVector<BlockSteps>& stepsOfBlocks, const ArenaVector<Step>& allSteps) NEVER_INLINE -> bool {
        if (m_allocation.isHeadOfFramedPart(block))
            return false;

        bool changesRareRegion = m_rareRegion != place.rareRegion;
        if (changesRareRegion && isInRareRegion())
            leaveRareRegionBetweenBlocks(*this);
        if (place.entersRareRegion || m_allocation.recordsWhatCheckersRead())
            registersAtHeads[block->index()] = listBindingsAtHead(*this, HeadThatListsRegisters { block, m_allocation.spillsOfValues(), startsOutsideFramedPart(block) }, end.bindings());
        if (changesRareRegion && place.isRare())
            enterRareRegionBetweenBlocks(*this, place.rareRegion);

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

enum class WhatReadsSayAtHead : uint8_t {
    ValueIsLive,
    ValueIsDead,
    WaysOnDecide,
};

struct HeadBeingEntered {
    const Allocator& allocator;
    const Use* uses;
    const ValueFacts* factsOfValues;
    Allocator::ValueState* states;
    Position head;
    Position endOfReadsThatSpeakForValue;
    Position endOfReadsThatCount;
    Position beginOfWaysOn;
};

static ALWAYS_INLINE HeadBeingEntered describeHeadBeingEntered(Allocator& allocator, bool blockIsRare)
{
    return {
        allocator,
        allocator.m_uses,
        allocator.m_valueFacts,
        allocator.m_values.begin(),
        allocator.m_headOfBlock,
        blockIsRare ? noPosition : allocator.m_endOfBlock,
        allocator.m_everyReadBehindBlockCounts ? allocator.m_endOfWhatWaysLeadToFromBlock : 0,
        allocator.m_beginOfWaysOnFromBlock,
    };
}

static ALWAYS_INLINE WhatReadsSayAtHead askReadsAtHeadBeingEntered(const HeadBeingEntered& head, const Operation* value, const Allocator::ValueState& state, uint32_t& firstRead)
{
    const Use* const uses = head.uses;
    uint32_t use = state.nextUse;
    Position position = uses[use].position;
    if (position < head.head) [[unlikely]] {
        do {
            use = uses[use].next;
            position = uses[use].position;
        } while (position < head.head);
    }
    firstRead = use;
    if (position < head.endOfReadsThatSpeakForValue)
        return WhatReadsSayAtHead::ValueIsLive;
    const uint32_t readOnWaysOn = head.allocator.findReadFromAmongLaterReads(value, use, head.beginOfWaysOn);
    if (uses[readOnWaysOn].position < head.endOfReadsThatCount && uses[readOnWaysOn].greatestHeadOfBlockPassedOnEveryPath >= head.head)
        return WhatReadsSayAtHead::ValueIsLive;
    if (!use) {
        Position lastUse = head.factsOfValues[value->index()].lastUse;
        if (lastUse == noPosition || lastUse <= head.head)
            return WhatReadsSayAtHead::ValueIsDead;
    }
    return WhatReadsSayAtHead::WaysOnDecide;
}

static ALWAYS_INLINE void holdValueAtHeadBeingEntered(Allocator& allocator, Allocator::ValueState* states, Reg reg, Operation* value)
{
    Operation* current = allocator.m_valueIn[reg.index()];
    if (current == value)
        return;
    if (current)
        unbindRegisterBetweenBlocks(allocator, reg, states[current->index()]);
    bindFreeRegisterBetweenBlocks(allocator, reg, value, states[value->index()], 0);
}

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

template<bool edgeHasFixedHead>
static NEVER_INLINE void listRegistersAtHeadBeingEntered(Allocator& allocator, const Block* block, const BlockPlace& place, const Allocator::Snapshot& taken, RegisterMask registersHeld)
{
    if (RegisterMask registersToNote = calleeSavesToNoteAtHeadBeingEntered(allocator, registersHeld)) [[unlikely]]
        noteCalleeSavesAtHeadBeingEntered(allocator, registersToNote);

    if (edgeHasFixedHead || place.entersRareRegion || allocator.m_allocation.recordsWhatCheckersRead()) {
        const HeadThatListsRegisters headThatLists { block, allocator.m_allocation.spillsOfValues(), false };
        const RegisterBinding* const first = taken.bindings().data();
        const RegisterBinding* const limit = first + taken.bindings().size();
        bool snapshotMatchesHead = static_cast<size_t>(std::popcount(registersHeld)) == taken.bindings().size();
        for (const RegisterBinding* binding = first; snapshotMatchesHead && binding != limit; ++binding)
            snapshotMatchesHead = isListedAtHead(allocator, headThatLists, binding->value);
        allocator.registersAtHeads()[block->index()] = snapshotMatchesHead ? taken.bindings() : listRegistersAtHead(allocator, headThatLists);
    }

    if (allocator.m_rareRegion != place.rareRegion)
        enterRareRegionBetweenBlocks(allocator, place.rareRegion);
}

template<bool registersAreAsTaken, bool edgeHasFixedHead>
static ALWAYS_INLINE void finishEnteringPlainBlock(Allocator& allocator, const Block* block, const BlockPlace& place, bool blockIsRare, const Allocator::Snapshot& taken, RegisterMask registersHeld)
{
    if constexpr (registersAreAsTaken)
        registersHeld = allocator.m_bound;
    else if (RegisterMask others = allocator.m_bound & ~registersHeld)
        unbindRegistersBetweenBlocks(allocator, others);

    if (blockIsRare || allocator.m_allocation.recordsWhatCheckersRead())
        return listRegistersAtHeadBeingEntered<edgeHasFixedHead>(allocator, block, place, taken, registersHeld);
    ASSERT(!allocator.m_isOutsideFramedPart && !allocator.isInRareRegion());
    if (RegisterMask registersToNote = registersHeld & allocator.m_file.calleeSaved & ~allocator.m_calleeSavesTakenOutsideRareRegions) [[unlikely]]
        return noteCalleeSavesAtHeadBeingEntered(allocator, registersToNote);
}

template<bool registersAreAsTaken, bool predecessorEndsInPatch, bool blockIsRare>
static NEVER_INLINE void enterPlainBlockBehindOnePredecessor(Allocator& allocator, const Block* block, const BlockPlace& place, const Allocator::Snapshot& taken, const Operation* endOfPredecessor)
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

    const RegisterBinding* binding = taken.bindings().data();
    const RegisterBinding* const limit = binding + taken.bindings().size();
    while (binding != limit) {
        const HeadBeingEntered head = describeHeadBeingEntered(allocator, blockIsRare);
        uint32_t firstRead = 0;
        do {
            WhatReadsSayAtHead said = askReadsAtHeadBeingEntered(head, binding->value, head.states[binding->value->index()], firstRead);
            if (said == WhatReadsSayAtHead::WaysOnDecide && !blockIsRare)
                break;
            settle(head, *binding, said != WhatReadsSayAtHead::ValueIsDead);
        } while (++binding != limit);
        if constexpr (!blockIsRare) {
            if (binding == limit)
                break;
            const Operation* value = binding->value;
            bool isLive = !isDeadOnEveryPathFromHeadBetweenBlocks(allocator, head.factsOfValues[value->index()], value, block, place, allocator.m_endOfWhatWaysLeadToFromBlock, firstRead, head.uses[firstRead].position);
            settle(describeHeadBeingEntered(allocator, blockIsRare), *binding, isLive);
            ++binding;
        }
    }
    finishEnteringPlainBlock<registersAreAsTaken, false>(allocator, block, place, blockIsRare, taken, registersHeld);
}

static NEVER_INLINE void enterPlainBlockWhoseHeadIsFixed(Allocator& allocator, const Block* block, const BlockPlace& place, const Allocator::Snapshot& taken)
{
    Allocator::ValueState* const states = allocator.m_values.begin();
    for (const RegisterBinding& binding : taken.bindings())
        holdValueAtHeadBeingEntered(allocator, states, binding.reg, binding.value);
    finishEnteringPlainBlock<false, true>(allocator, block, place, place.isRare(), taken, taken.registers);
}

static NEVER_INLINE void enterBlockThatTakesNoRegisters(Allocator& allocator, const BlockPlace& place)
{
    allocator.m_blockWhoseEndRegistersHold = nullptr;
    unbindRegistersBetweenBlocks(allocator, allocator.m_bound);
    if (allocator.m_rareRegion != place.rareRegion)
        enterRareRegionBetweenBlocks(allocator, place.rareRegion);
}

void Allocator::enterBlock(Block* block)
{
    const BlockPlace& placeOfBlock = m_order.places()[block->index()];
    if (isInRareRegion() && m_rareRegion != placeOfBlock.rareRegion)
        leaveRareRegionBetweenBlocks(*this);

    const bool mayBePlain = !block->isEntry() && !block->isLandingPad() && !startsOutsideFramedPart(block)
        && (m_operandsTakenBackAtHeads.isEmpty() || !m_operandsTakenBackAtHeads[block->index()].value);
    if (mayBePlain && placeOfBlock.predecessors.size() != 1) {
        const Snapshot& taken = m_heads[block->index()];
        if (taken.isSet()) {
            m_blockWhoseEndRegistersHold = nullptr;
            return enterPlainBlockWhoseHeadIsFixed(*this, block, placeOfBlock, taken);
        }
    } else if (mayBePlain && block->phis().empty() && placeOfBlock.head >= m_endOfLastLoopThatKeepsValueMadeAgain) {
        const Block* predecessor = placeOfBlock.predecessors.data()[0];
        const Snapshot& taken = m_ends[predecessor->index()];
        if (taken.isSet()) {
            const Operation* endOfPredecessor = predecessor->last();
            const bool predecessorEndsInPatch = endOfPredecessor->opcode() == Patch;
            const bool blockIsRare = placeOfBlock.isRare();
            const bool registersAreAsTaken = predecessor == m_blockWhoseEndRegistersHold;
            m_blockWhoseEndRegistersHold = nullptr;
            if (registersAreAsTaken) {
                if (!predecessorEndsInPatch) {
                    if (!blockIsRare)
                        return enterPlainBlockBehindOnePredecessor<true, false, false>(*this, block, placeOfBlock, taken, endOfPredecessor);
                    return enterPlainBlockBehindOnePredecessor<true, false, true>(*this, block, placeOfBlock, taken, endOfPredecessor);
                }
                if (!blockIsRare)
                    return enterPlainBlockBehindOnePredecessor<true, true, false>(*this, block, placeOfBlock, taken, endOfPredecessor);
                return enterPlainBlockBehindOnePredecessor<true, true, true>(*this, block, placeOfBlock, taken, endOfPredecessor);
            }
            if (!predecessorEndsInPatch) {
                if (!blockIsRare)
                    return enterPlainBlockBehindOnePredecessor<false, false, false>(*this, block, placeOfBlock, taken, endOfPredecessor);
                return enterPlainBlockBehindOnePredecessor<false, false, true>(*this, block, placeOfBlock, taken, endOfPredecessor);
            }
            if (!blockIsRare)
                return enterPlainBlockBehindOnePredecessor<false, true, false>(*this, block, placeOfBlock, taken, endOfPredecessor);
            return enterPlainBlockBehindOnePredecessor<false, true, true>(*this, block, placeOfBlock, taken, endOfPredecessor);
        }
    } else if ((block->isEntry() || block->isLandingPad()) && !m_allocation.hasFramedPart() && (m_operandsTakenBackAtHeads.isEmpty() || !m_operandsTakenBackAtHeads[block->index()].value))
        return enterBlockThatTakesNoRegisters(*this, placeOfBlock);

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
            taken.set(m_arena.allocateArray<RegisterBinding>(0), 0);
        else if (headBehindEdgesIsFixedByEdge(m_valueFacts, block, place))
            taken = m_heads[block->index()];
        else {
            predecessor = place.predecessors.front();
            taken = m_ends[predecessor->index()];
        }
        if (!taken.isSet()) [[unlikely]] {
            m_proc.fail(FailureKind::MalformedProcedure, block->last());
            return;
        }

        const bool registersAreAsTaken = predecessor && predecessor == m_blockWhoseEndRegistersHold;
        m_blockWhoseEndRegistersHold = nullptr;

        ValueState* const states = m_values.begin();
        const RegisterMask calleeSavesStillToNote = calleeSavesStillToNoteBetweenBlocks(*this);
        const HeadThatListsRegisters headThatLists { block, m_allocation.spillsOfValues(), startsOutsideFramedPart(block) };
        bool snapshotMatchesHead = listsRegisters;

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
            for (const RegisterBinding& binding : taken.bindings()) {
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
            const Position beginOfWaysOn = m_beginOfWaysOnFromBlock;
            auto isLiveAtHead = [&](const Operation* value, const ValueState& state) ALWAYS_INLINE_LAMBDA -> bool {
                uint32_t use = state.nextUse;
                while (uses[use].position < head)
                    use = uses[use].next;
                if (use) [[likely]] {
                    if (blockIsRare || uses[use].position < endOfBlock)
                        return true;
                    if (everyReadCounts) {
                        const uint32_t readOnWaysOn = findReadFromAmongLaterReads(value, use, beginOfWaysOn);
                        if (uses[readOnWaysOn].position < endOfWhatWaysLeadTo && uses[readOnWaysOn].greatestHeadOfBlockPassedOnEveryPath >= head)
                            return true;
                    }
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
            for (const RegisterBinding& binding : taken.bindings()) {
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

        if (!m_operandsTakenBackAtHeads.isEmpty()) [[unlikely]] {
            if (OperandTakenBack operand = m_operandsTakenBackAtHeads[block->index()]; operand.value) {
                unbind(operand.reg);
                bind(operand.reg, operand.value);
                snapshotMatchesHead = false;
            }
        }
        if (listsRegisters)
            registersAtHeads[block->index()] = snapshotMatchesHead ? taken.bindings() : listRegistersAtHead(*this, headThatLists);

        if (changesRareRegion && blockIsRare)
            enterRareRegion(place.rareRegion);
    };
    enterTheLongWay(block, placeOfBlock, m_allocation.m_registersAtHead);
}

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
    Position end;
    Position beginOfWaysOn;
    Position limitOfDefinitions;
    bool everyReadBehindTargetCounts;
    bool mayHoldWhatIsMadeAgain;
    bool isRare;
    bool startsLoop;
    bool edgeLacksPatchResults;
    bool someValueMadeAgainIsKept;
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
    head.beginOfWaysOn = allocator.beginOfWaysOnFrom(target, head.end);
    head.limitOfDefinitions = findLimitOfDefinitionsReachingHead(allocator, target, from);
    constexpr bool isOfAnyKind = kind == KindOfHeadBeingFixed::Any;
    head.isRare = kind == KindOfHeadBeingFixed::PlainButRare || (isOfAnyKind && place.isRare());
    head.startsLoop = isOfAnyKind && place.isLoopHead;
    head.everyReadBehindTargetCounts = !head.isRare && !head.startsLoop && (place.loop == noLoop || place.head < allocator.m_liveness.of(order.loops().data()[place.loop], order).endOfLastExitingBlock);
    head.patchThatEndsFrom = isOfAnyKind && endOfFrom->opcode() == Patch ? endOfFrom : nullptr;
    head.edgeLacksPatchResults = isOfAnyKind && from->successor(0).block() != target;
    head.someValueMadeAgainIsKept = isOfAnyKind && place.head < allocator.m_endOfLastLoopThatKeepsValueMadeAgain;
    head.mayHoldWhatIsMadeAgain = !head.patchThatEndsFrom && !head.isRare && !head.startsLoop;
    head.factsOfValues = allocator.m_valueFacts;
    head.uses = allocator.m_uses;
    head.states = allocator.m_values.begin();
    return head;
}

template<KindOfHeadBeingFixed kind>
static ALWAYS_INLINE bool isLiveAtHeadBeingFixed(Allocator& allocator, const HeadBeingFixed& head, const Operation* value)
{
    const uint32_t index = value->index();
    const ValueFacts& facts = head.factsOfValues[index];
    Position definition = facts.definition;
    if constexpr (kind == KindOfHeadBeingFixed::Any) {
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
        if (headIsRare || head.uses[use].position < head.end)
            return true;
        if (head.everyReadBehindTargetCounts) {
            const uint32_t readOnWaysOn = allocator.findReadFromAmongLaterReads(value, use, head.beginOfWaysOn);
            if (readOnWaysOn && head.uses[readOnWaysOn].greatestHeadOfBlockPassedOnEveryPath >= head.head)
                return true;
        }
    } else {
        Position lastUse = facts.lastUse;
        if (lastUse == noPosition || lastUse <= head.head)
            return false;
        if (headIsRare)
            return true;
    }
    return !isDeadOnEveryPathFromHeadBetweenBlocks(allocator, facts, value, head.target, *head.place, allocator.m_liveness.endOfWhatWaysLeadToFrom(head.target), use, head.uses[use].position);
}

static NEVER_INLINE void keepValueInOneRegisterAtHead(Allocator& allocator, const ValueFacts& facts, RegisterMask held)
{
    RegisterMask calleeSaved = held & allocator.m_file.calleeSaved;
    Reg kept = lowestRegisterAtEdge(facts.has(ValueFlag::LivesAcrossCall) && calleeSaved ? calleeSaved : held);
    forEachRegister(held & ~maskOf(kept), [&](Reg other) {
        allocator.unbind(other);
    });
}

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
        if (!head.isRare && spillsOfValues[index] != noSpill) {
            const Reg hint = allocator.m_hints[index];
            const bool isExpectedFurtherOn = hint && (held & maskOf(hint));
            ASSERT(head.startsLoop == head.place->isLoopHead);
            const bool headMayLetGo = !isExpectedFurtherOn || (!head.startsLoop && !(held & allocator.m_file.calleeSaved));
            if (headMayLetGo && headLetsGoOfSpilledValue(allocator, head.target, *head.place, value, held, isExpectedFurtherOn)) {
                unbindAllBetweenBlocks(allocator, state);
                continue;
            }
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

template<KindOfHeadBeingFixed kind>
static ALWAYS_INLINE void setHeadAndNoteItsHints(Allocator& allocator, const Block* target, const Block* from, const BlockPlace& place)
{
    static_assert(kind != KindOfHeadBeingFixed::Any);
    ASSERT(from == allocator.m_block);
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
    allocator.m_heads[target->index()].set(bindings, allocator.m_bound);

    if constexpr (kind != KindOfHeadBeingFixed::Plain) {
        if (someEdgeIsToCome && !target->phis().empty())
            passOnHintsOfPhisOfHead(allocator, target, placeOfWalk);
    }
}

template<KindOfHeadBeingFixed kind>
static NEVER_INLINE void fixPlainHeadBetweenBlocks(Allocator& allocator, Block* target, const Block* from, const BlockPlace& place)
{
    static_assert(kind == KindOfHeadBeingFixed::Plain || kind == KindOfHeadBeingFixed::PlainButRare);
    ASSERT(from == allocator.m_block);
    ASSERT(target->phis().empty() && !allocator.isInRareRegion());
    const HeadBeingFixed head = describeHeadBeingFixed<kind>(allocator, target, from, place);
    Operation* valuesMadeAgain[64];
    RegisterMask registersOfValuesMadeAgain = dropWhatHeadBeingFixedDoesNotHold<kind>(allocator, head, valuesMadeAgain);
    bindValuesMadeAgainAtHead(allocator, registersOfValuesMadeAgain & ~allocator.m_bound, valuesMadeAgain);
    setHeadAndNoteItsHints<kind>(allocator, target, from, place);
    serveEdgesThatWaitedForHead(allocator, target);
}

static ALWAYS_INLINE bool registersMatchBlockHead(const Allocator& allocator, const Block* target, std::span<const RegisterBinding> head)
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

static ALWAYS_INLINE KindOfHeadBeingFixed findKindOfHeadToFix(const Allocator& allocator, bool hasFramedPart, const Block* target, const Block* from, const BlockPlace& place)
{
    ASSERT(hasFramedPart == allocator.m_allocation.hasFramedPart());
    if (hasFramedPart || place.isLoopHead || from->last()->opcode() == Patch || place.head < allocator.m_endOfLastLoopThatKeepsValueMadeAgain)
        return KindOfHeadBeingFixed::Any;
    if (place.isRare())
        return KindOfHeadBeingFixed::PlainButRare;
    if (allocator.isInRareRegion())
        return KindOfHeadBeingFixed::Any;
    return target->phis().empty() ? KindOfHeadBeingFixed::Plain : KindOfHeadBeingFixed::PlainButForPhis;
}

static ALWAYS_INLINE bool headIsFixedTogetherWithMovesOfEdge(KindOfHeadBeingFixed kind, const Block* from)
{
    return kind == KindOfHeadBeingFixed::PlainButForPhis && from->numSuccessors() == 1;
}

static Location findPlaceOutsideRegistersAtEndOf(Allocator&, const Block* from, const Operation* value, bool isServedLate, bool headMayDoWithoutDeadValue = false);

template<typename Functor>
static ALWAYS_INLINE bool addMovesOfPhisAtEdge(Allocator& allocator, const Block* from, const Block* target, Operation* const* incomingValues, const Functor& placeAtEndOf)
{
    for (Operation* phi : target->phis()) {
        if (allocator.m_valueFacts[phi->index()].has(ValueFlag::IsDropped))
            continue;
        Reg reg = allocator.m_allocation.registerAtDefinitionOf(phi);
        const SpillIndex spill = allocator.m_allocation.spillOf(phi);
        Location destination = reg ? Location::reg(reg) : Location::spill(spill);
        Operation* incoming = incomingValues[phi->indexInBlock()];
        const bool liesInSpillOfPhi = incoming && spill != noSpill && allocator.m_allocation.spillOf(incoming) == spill;
        if (liesInSpillOfPhi && !reg)
            continue;
        Location source = incoming ? placeAtEndOf(incoming, phi->indexInBlock(), destination) : Location();
        if (!source || (source != destination && isResultOfPatchEndingBlock(from, incoming))) {
            allocator.m_proc.fail(FailureKind::MalformedProcedure, phi);
            return false;
        }
        if (source != destination)
            allocator.m_pendingMoves.append(allocator.m_arena, PendingMove { incoming, destination, source, widthOf(incoming->type()) });
        if (reg && !liesInSpillOfPhi && allocator.m_allocation.isWrittenByEdges(phi))
            allocator.m_pendingMoves.append(allocator.m_arena, PendingMove { incoming, Location::spill(spill), source, widthOf(incoming->type()) });
    }
    return true;
}

static bool edgesMayWritePhisWithRegistersAtHead(const Allocator& allocator, const Block* target, const BlockPlace& place)
{
    if (allocator.m_allocation.hasFramedPart() || allocator.m_proc.traits().savesFrameOnSuspend || place.isRare() || allocator.isInRareRegion() || target->isLandingPad() || target->isEntry())
        return false;
    return std::ranges::all_of(place.predecessors, [](const Block* predecessor) {
        return predecessor->last()->isUnconditionalJump();
    });
}

static bool valueMayShareSpill(const Allocator& allocator, const Operation* value)
{
    const ValueFacts& facts = allocator.m_valueFacts[value->index()];
    return !value->isNarrowReference() && !value->has(OperationFlag::IsInnerPointer) && !value->endsBlock() && value->opcode() != Extract && !facts.has(ValueFlag::IsSunk)
        && !facts.has(ValueFlag::LivesIntoLandingPad) && !facts.has(ValueFlag::LivesAcrossSuspension) && !facts.has(ValueFlag::IsKeptForCollector);
}

Allocator::PhisInSpill& Allocator::phisInSpill(SpillIndex spill)
{
    if (m_phisInSpills.size() <= spill)
        m_phisInSpills.grow(m_arena, spill + 1);
    PhisInSpill*& phis = m_phisInSpills[spill];
    if (!phis) {
        phis = m_arena.create<PhisInSpill>();
        phis->append(m_arena, m_allocation.m_spills[spill].value);
    }
    return *phis;
}

SpillIndex Allocator::findSpillOfDeadSource(const Operation* phi)
{
    const Block* owner = phi->owner();
    const BlockPlace& place = m_order.of(owner);
    if (!m_allocation.m_registerAtDefinition[phi->index()] || !valueMayShareSpill(*this, phi))
        return noSpill;
    if (m_allocation.hasFramedPart() || m_proc.traits().savesFrameOnSuspend || place.isRare() || isInRareRegion() || owner->isLandingPad() || owner->isEntry())
        return noSpill;
    const Position definition = m_valueFacts[phi->index()].definition;
    for (const Block* predecessor : place.predecessors) {
        for (const Operation* upsilon : predecessor->upsilons()) {
            const Operation* source = upsilon->valueHandedOn();
            if (upsilon->phi() != phi || source == phi || !m_allocation.isSpilled(source) || source->type() != phi->type() || !valueMayShareSpill(*this, source) || m_order.of(source->owner()).isRare())
                continue;
            SpillIndex candidate = m_allocation.spillOf(source);
            const SpillFacts& facts = m_allocation.m_spills[candidate];
            if (!facts.isSaveInRareRegion && !facts.isPointerBesideNarrowReference && facts.end < definition)
                return candidate;
        }
    }
    return noSpill;
}

void Allocator::letEdgesWritePhi(Operation* phi, SpillIndex spill, const BlockPlace& place)
{
    if (!m_allocation.m_phisWrittenByEdges.size())
        m_allocation.m_phisWrittenByEdges = BitSpan::create(m_arena, m_proc.numberOfOperations());
    m_allocation.m_phisWrittenByEdges.set(phi->index());
    SpillFacts& range = m_allocation.m_spills[spill];
    const ValueFacts& facts = m_valueFacts[phi->index()];
    range.begin = std::min(range.begin, facts.definition);
    if (facts.lastUse != noPosition)
        range.end = std::max(range.end, facts.lastUse);
    for (const Block* predecessor : place.predecessors) {
        Position positionOfMoves = m_order.earlyPositionOf(predecessor, predecessor->size() - 1);
        range.begin = std::min(range.begin, positionOfMoves);
        range.end = std::max(range.end, positionOfMoves + 1);
    }
}

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
        if (isInRareRegion()) {
            forEachValueHeldOnlyInRareRegionSave(*this, from, [&](Operation* value) {
                if (isLiveAtEdgePosition(m_liveness, value, positionOfEdges))
                    m_savesReadByWaitingEdges.append(m_arena, SaveReadByWaitingEdge { from, value, stateOf(value).saveInRareRegion });
            });
        }
        if (m_waitingEdgesOfHeads.isEmpty())
            m_waitingEdgesOfHeads.grow(m_arena, m_order.mostBlocks());
        m_waitingEdges.append(m_arena, WaitingEdge { from, successorIndex, 0 });
        WaitingEdgesOfHead& waiting = m_waitingEdgesOfHeads[target->index()];
        (waiting.last ? m_waitingEdges[waiting.last - 1].next : waiting.first) = m_waitingEdges.size();
        waiting.last = m_waitingEdges.size();
    };

    const Edge* const successors = from->successors().data();
    const BlockPlace* const places = m_order.places();
    const bool hasFramedPart = m_allocation.hasFramedPart();
    Snapshot& end = m_ends[from->index()];
    ASSERT(!end.isSet());
    bool someSuccessorTakesRegisters = false;
    bool registersMatchBlockEnd = true;
    for (unsigned successorIndex = 0; successorIndex < numberOfSuccessors; ++successorIndex) {
        const Edge& edge = successors[successorIndex];
        Block* target = edge.block();
        if (target->isLandingPad()) [[unlikely]] {
            for (const Operation* phi : target->phis()) {
                if (!m_liveness.isDropped(phi)) {
                    m_proc.fail(FailureKind::MalformedProcedure, phi);
                    return;
                }
            }
            continue;
        }

        someSuccessorTakesRegisters = true;
        ASSERT(end.isSet() || registersMatchBlockEnd);
        ASSERT(!target->isEntry());
        const BlockPlace& placeOfTarget = places[target->index()];
        if (!headBehindEdgesIsFixedByEdge(m_valueFacts, target, placeOfTarget)) {
            if (!end.isSet())
                takeSnapshotBetweenBlocks(*this, end);
            continue;
        }

        const Snapshot& head = m_heads[target->index()];
        if (!head.isSet()) {
            if (!registersMatchBlockEnd) {
                restore(end);
                registersMatchBlockEnd = true;
            }
            const bool isFirstEdgeThatCounts = !hasFramedPart && placeOfTarget.predecessors.data()[0] == from && from != placeOfTarget.sourceOfUnsplitEdgeThatCountsLast
                && numberOfSuccessors <= 2 && !edge.isRare() && (!isInRareRegion() || m_rareRegion == placeOfTarget.rareRegion) && m_headOfBlock < placeOfTarget.head;
            if (!isFirstEdgeThatCounts && findBlockThatFixesHead(*this, target, placeOfTarget) != from) {
                if (!end.isSet())
                    takeSnapshotBetweenBlocks(*this, end);
                letEdgeWaitForHead(from, successorIndex, target, positionOfEdges);
                continue;
            }

            const KindOfHeadBeingFixed kind = findKindOfHeadToFix(*this, hasFramedPart, target, from, placeOfTarget);
            const bool headIsPlain = kind == KindOfHeadBeingFixed::Plain;
            const bool headIsPlainButRare = kind == KindOfHeadBeingFixed::PlainButRare && target->phis().empty() && !isInRareRegion();
            const bool edgeIsServedWithHead = headIsPlain || headIsPlainButRare || headIsFixedTogetherWithMovesOfEdge(kind, from);
            if (!end.isSet() && !(edgeIsServedWithHead && successorIndex + 1 == numberOfSuccessors))
                takeSnapshotBetweenBlocks(*this, end);
            registersMatchBlockEnd = false;
            if (headIsPlain)
                fixPlainHeadBetweenBlocks<KindOfHeadBeingFixed::Plain>(*this, target, from, placeOfTarget);
            else if (headIsPlainButRare)
                fixPlainHeadBetweenBlocks<KindOfHeadBeingFixed::PlainButRare>(*this, target, from, placeOfTarget);
            else
                fixHeadOf(target, from, placeOfTarget, kind);
            if (m_proc.failure()) [[unlikely]]
                return;
            if (edgeIsServedWithHead)
                continue;
        } else if (registersMatchBlockEnd && !(hasFramedPart && edgeMayChangeFrame(from, target)) && registersMatchBlockHead(*this, target, head.bindings()))
            continue;
        else if (hasFramedPart && !end.isSet())
            takeSnapshotBetweenBlocks(*this, end);

        appendEdgeMoves(from, successorIndex, end);
        if (m_proc.failure()) [[unlikely]]
            return;
    }
    if (someSuccessorTakesRegisters && registersMatchBlockEnd)
        m_blockWhoseEndRegistersHold = from;
}

static NEVER_INLINE void loadBackOwnersWhereFrameEndsOnEdge(Allocator& allocator, const HeadBeingFixed& head, const Block* from, bool movesMayWriteRegisters, RegisterMask calleeSavesStillToNote)
{
    const Allocation& allocation = allocator.m_allocation;
    if (!allocation.hasFramedPart() || allocation.needsFrameAheadOfFramedPart() || !allocator.startsOutsideFramedPart(head.target) || allocator.isOutsideFramedPart(from))
        return;
    allocation.everyHeadOfFramedPartAbove(from, [&](const Allocation::HeadOfFramedPart& headAbove) {
        for (const RegisterBinding& binding : allocation.registersAtHeadOf(headAbove.block)) {
            Operation* value = binding.value;
            Allocator::ValueState& state = head.states[value->index()];
            const ValueFacts& facts = head.factsOfValues[value->index()];
            if (!facts.has(ValueFlag::IsKeptForCollector) || state.registers || !allocation.isSpilled(value) || facts.has(ValueFlag::NeedsPointerBesideIt) || !isLiveAtHeadBeingFixed<KindOfHeadBeingFixed::Any>(allocator, head, value))
                continue;
            RegisterMask freeRegisters = movesMayWriteRegisters ? allocator.m_file.allocatableIn(bankOf(binding.reg)) & ~allocator.m_file.calleeSaved & ~allocator.m_bound : 0;
            if (!freeRegisters) {
                allocator.giveUpFramedPartForReadOfSpill();
                return false;
            }
            bindFreeRegisterBetweenBlocks(allocator, freeRegisters & maskOf(binding.reg) ? binding.reg : lowestRegisterAtEdge(freeRegisters), value, state, calleeSavesStillToNote);
        }
        return true;
    });
}

void Allocator::fixHeadOf(Block* targetOfEdge, Block* blockOfEdge, const BlockPlace& placeOfTarget, KindOfHeadBeingFixed kindOfHead)
{
    ASSERT(&placeOfTarget == &m_order.of(targetOfEdge) && kindOfHead == findKindOfHeadToFix(*this, m_allocation.hasFramedPart(), targetOfEdge, blockOfEdge, placeOfTarget));
    auto fixHeadOfKind = [this]<KindOfHeadBeingFixed kind>(Block* target, Block* from, const BlockPlace& place, ArenaVector<Reg>& registersAtDefinitions, ArenaVector<SpillFacts>& factsOfSpills, ArenaVector<BlockSteps>& stepsOfBlocks, ArenaVector<Step>& allSteps) NEVER_INLINE {
        constexpr bool isOfAnyKind = kind == KindOfHeadBeingFixed::Any;
        const HeadBeingFixed head = describeHeadBeingFixed<kind>(*this, target, from, place);
        Operation* const* const phis = target->phis().data();
        const size_t numberOfPhis = target->phis().size();
        const bool movesMayWriteRegisters = from->last()->isUnconditionalJump() || Order::isUnsplit(from, place);
        bool exitsFramedPart = false;
        if constexpr (isOfAnyKind)
            exitsFramedPart = movesMayWriteRegisters && !head.isRare && isOutsideFramedPart(target) && !isOutsideFramedPart(from) && !m_allocation.needsFrameAheadOfFramedPart();
        RegisterMask takenByPatchesOfLoop = 0;
        if constexpr (isOfAnyKind) {
            if (head.startsLoop && hasBackEdgeOutsideRareRegions(m_order.loops()[place.loop]))
                takenByPatchesOfLoop = m_liveness.of(m_order.loops()[place.loop], m_order).registersTakenByPatches;
        }

        const ValueFacts* const factsOfValues = head.factsOfValues;
        ValueState* const states = head.states;
        const Reg* const hints = m_hints.begin();
        const RegisterMask calleeSavedRegisters = m_file.calleeSaved;
        ASSERT(kind != KindOfHeadBeingFixed::PlainButForPhis || (!m_isOutsideFramedPart && !isInRareRegion()));
        const RegisterMask calleeSavesStillToNote = kind == KindOfHeadBeingFixed::PlainButForPhis ? calleeSavedRegisters & ~m_calleeSavesTakenOutsideRareRegions : calleeSavesStillToNoteBetweenBlocks(*this);

        WhatCallInBlockOfHeadTakes whatCallTakes;
        Reg registersOfDyingValuesInLine[mostPhisListedInLine];
        Reg* registersOfDyingValues = registersOfDyingValuesInLine;
        Operation* incomingValuesInLine[mostPhisListedInLine];
        Operation** incomingValues = incomingValuesInLine;
        const bool appendsMovesOfEdge = headIsFixedTogetherWithMovesOfEdge(kind, from);
        RegisterMask registersOfIncomingValuesInLine[mostPhisListedInLine];
        RegisterMask* registersOfIncomingValues = registersOfIncomingValuesInLine;
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
                if (head.patchThatEndsFrom && (head.edgeLacksPatchResults || !isResultOfPatchAtEdge(head.patchThatEndsFrom, incoming)))
                    continue;
                ValueState& stateOfIncoming = states[incoming->index()];
                RegisterMask held = stateOfIncoming.registers;
                if (exitsFramedPart)
                    held &= ~calleeSavedRegisters;
                if (!held || isLiveAtHeadBeingFixed<kind>(*this, head, incoming))
                    continue;

                RegisterMask calleeSaved = held & calleeSavedRegisters;
                const bool livesAcrossCall = phiLivesAcrossCall(index, phi, facts);
                RegisterMask candidates = livesAcrossCall && calleeSaved ? calleeSaved : held;
                bool liesWhereCallTakesIt = false;
                if (livesAcrossCall && !calleeSaved && !head.startsLoop && (calleeSavesWorthTakingFor(phi, bankOf(phi->type()), target) & ~m_bound)) {
                    if (RegisterMask taken = registersWhereCallInBlockOfHeadTakes(*this, target, whatCallTakes, phi, held)) {
                        candidates = taken;
                        liesWhereCallTakesIt = true;
                        phisLiveAcrossCalls[index] = PhiLivesAcrossNoCall;
                    }
                }
                Reg hintOfLaterHead = hints[phi->index()];
                bool isBetterOffElsewhere = (livesAcrossCall && !calleeSaved && !liesWhereCallTakesIt) || (hintOfLaterHead && !(candidates & maskOf(hintOfLaterHead))) || !(candidates & ~takenByPatchesOfLoop);
                if (candidates & ~takenByPatchesOfLoop)
                    candidates &= ~takenByPatchesOfLoop;
                if (isBetterOffElsewhere && movesMayWriteRegisters) {
                    registersOfDyingValues[index] = lowestRegisterAtEdge(candidates);
                    continue;
                }
                Reg hint = hintOfLaterHead ? hintOfLaterHead : facts.hint;
                Reg reg = hint && (candidates & maskOf(hint)) ? hint : lowestRegisterAtEdge(candidates);
                unbindRegisterBetweenBlocks(*this, reg, stateOfIncoming);
                bindFreeRegisterBetweenBlocks(*this, reg, phi, states[phi->index()], calleeSavesStillToNote);
            }
        }

        Operation* valuesMadeAgain[64];
        RegisterMask registersOfValuesMadeAgain = dropWhatHeadBeingFixedDoesNotHold<kind>(*this, head, valuesMadeAgain);
        if (exitsFramedPart) {
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
            if (m_proc.numberOfInnerPointersAcrossSafepoints()) [[unlikely]]
                loadBackOwnersWhereFrameEndsOnEdge(*this, head, from, movesMayWriteRegisters, calleeSavesStillToNote);
        }

        if constexpr (isOfAnyKind) {
            if (head.startsLoop)
                chooseRegistersKeptThroughLoop(m_order.loops()[place.loop], movesMayWriteRegisters);
        }
        const Block* headOfLoopLedBackTo = movesMayWriteRegisters && !appendsMovesOfEdge && !exitsFramedPart && !isInRareRegion() ? headOfLoopThatBlockOnlyLeadsBackTo(target) : nullptr;
        if (headOfLoopLedBackTo)
            dropWhatBlockThatLeadsBackNeitherReadsNorHandsOn(target, headOfLoopLedBackTo);

        if (movesMayWriteRegisters) {
            for (size_t index = 0; index < numberOfPhis; ++index) {
                Operation* phi = phis[index];
                const ValueFacts& facts = factsOfValues[phi->index()];
                ValueState& state = states[phi->index()];
                if (facts.has(ValueFlag::IsDropped) || state.registers)
                    continue;
                if (Reg reg = chooseFreeRegisterAtHead(*this, bankOf(phi->type()), head.isRare, !exitsFramedPart && phiLivesAcrossCall(index, phi, facts) && (calleeSavesWorthTakingFor(phi, bankOf(phi->type()), target) & ~m_bound), exitsFramedPart ? calleeSavedRegisters : takenByPatchesOfLoop, { hints[phi->index()], registersOfDyingValues[index], facts.hint }))
                    bindFreeRegisterBetweenBlocks(*this, reg, phi, state, calleeSavesStillToNote);
                else if constexpr (isOfAnyKind) {
                    if (head.startsLoop && !exitsFramedPart) {
                        if (Reg taken = takeRegisterOfValueThatLoopReadsBehindPhi(m_order.loops()[place.loop], phi))
                            bindFreeRegisterBetweenBlocks(*this, taken, phi, state, calleeSavesStillToNote);
                    }
                }
            }
            if constexpr (isOfAnyKind) {
                if (head.startsLoop)
                    bringInWhatLoopReads(m_order.loops()[place.loop], head.limitOfDefinitions);
            }
            if (headOfLoopLedBackTo)
                holdWhatHeadOfLoopHolds(target, headOfLoopLedBackTo, head.limitOfDefinitions);
        }

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
            const bool targetStartsUnframed = isOfAnyKind && startsOutsideFramedPart(target);
            bool isKnownWhetherEdgesMayWritePhis = false;
            bool edgesMayWritePhis = false;
            struct SpillsHandedToPhi {
                SpillIndex first { noSpill };
                SpillIndex second { noSpill };
            };
            SpillsHandedToPhi* spillsHandedToPhis = nullptr;
            for (size_t index = 0; index < numberOfPhis; ++index) {
                Operation* phi = phis[index];
                const ValueFacts& facts = factsOfValues[phi->index()];
                if (facts.has(ValueFlag::IsDropped))
                    continue;
                Reg reg = anyRegisterOf(phi, ~RegisterMask { 0 });
                registersAtDefinitions[phi->index()] = reg;
                bool isKeptByCalls = reg && (calleeSavedRegisters & maskOf(reg));
                if (reg && !facts.has(ValueFlag::LivesIntoLandingPad) && !facts.has(ValueFlag::LivesAcrossSuspension) && (isKeptByCalls || targetStartsUnframed || !phiLivesAcrossCall(index, phi, facts)))
                    continue;

                if constexpr (kind != KindOfHeadBeingFixed::PlainButRare) {
                    if (!isKnownWhetherEdgesMayWritePhis) {
                        isKnownWhetherEdgesMayWritePhis = true;
                        edgesMayWritePhis = edgesMayWritePhisWithRegistersAtHead(*this, target, place);
                    }
                    if (edgesMayWritePhis && valueMayShareSpill(*this, phi) && !m_allocation.isSpilled(phi)) {
                        if (!spillsHandedToPhis) {
                            spillsHandedToPhis = m_arena.allocateArray<SpillsHandedToPhi>(numberOfPhis);
                            std::fill_n(spillsHandedToPhis, numberOfPhis, SpillsHandedToPhi { });
                            for (const Block* predecessor : place.predecessors) {
                                for (const Operation* upsilon : predecessor->upsilons()) {
                                    const Operation* taker = upsilon->phi();
                                    const Operation* handedOn = upsilon->valueHandedOn();
                                    if (!taker || taker->owner() != target || handedOn == taker || !m_allocation.isSpilled(handedOn) || handedOn->type() != taker->type() || !valueMayShareSpill(*this, handedOn) || m_order.of(handedOn->owner()).isRare())
                                        continue;
                                    SpillIndex candidate = m_allocation.spillOf(handedOn);
                                    const SpillFacts& factsOfCandidate = factsOfSpills[candidate];
                                    if (factsOfCandidate.isSaveInRareRegion || factsOfCandidate.isPointerBesideNarrowReference)
                                        continue;
                                    SpillsHandedToPhi& known = spillsHandedToPhis[taker->indexInBlock()];
                                    if (known.first == candidate || known.second == candidate)
                                        continue;
                                    auto isWordOfPhi = [&](SpillIndex spill) {
                                        return factsOfSpills[spill].value->opcode() == Phi;
                                    };
                                    if (known.first == noSpill)
                                        known.first = candidate;
                                    else if (isWordOfPhi(candidate) && !isWordOfPhi(known.first))
                                        known.second = std::exchange(known.first, candidate);
                                    else if (known.second == noSpill || (isWordOfPhi(candidate) && !isWordOfPhi(known.second)))
                                        known.second = candidate;
                                }
                            }
                        }
                        SpillIndex shared = noSpill;
                        for (SpillIndex candidate : { spillsHandedToPhis[index].first, spillsHandedToPhis[index].second }) {
                            if (candidate == noSpill)
                                break;
                            PhisInSpill& phisInCandidate = phisInSpill(candidate);
                            Operation** endOfPhisNotYetDead = phisInCandidate.begin();
                            for (Operation* other : phisInCandidate) {
                                const Position lastUse = factsOfValues[other->index()].lastUse;
                                if (lastUse == noPosition || lastUse >= m_headOfBlock)
                                    *endOfPhisNotYetDead++ = other;
                            }
                            phisInCandidate.shrink(static_cast<uint32_t>(endOfPhisNotYetDead - phisInCandidate.begin()));
                            bool liesApart = std::ranges::none_of(phisInCandidate, [&](const Operation* other) {
                                return other->owner() == target || (isLiveAtHeadBeingFixed<kind>(*this, head, other) && (other->opcode() != Phi || isReadOnSomeWayFromHeadOf(other, target)));
                            });
                            if (liesApart) {
                                shared = candidate;
                                break;
                            }
                        }
                        if (reg && (shared != noSpill || head.startsLoop) && !isKeptByCalls) {
                            Position call = m_liveness.nextCallBehindHeadOf(target, bankOf(reg));
                            if (call != noPosition && findFirstReadFromHeadOf(target, phi) > call) {
                                unbindRegisterBetweenBlocks(*this, reg, states[phi->index()]);
                                registersAtDefinitions[phi->index()] = Reg();
                            }
                        }
                        if (shared != noSpill) {
                            m_allocation.m_spillOfValue[phi->index()] = shared;
                            factsOfSpills[shared].isSharedByPhis = true;
                            letEdgesWritePhi(phi, shared, place);
                            m_phisInSpills[shared]->append(m_arena, phi);
                            continue;
                        }
                        if (head.startsLoop) {
                            SpillIndex own = markSpilled(phi);
                            letEdgesWritePhi(phi, own, place);
                            continue;
                        }
                    }
                }

                SpillIndex spill = markSpilled(phi);
                if (reg)
                    continue;
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

        if (appendsMovesOfEdge && !m_proc.failure()) {
            m_pendingMoves.clear();
            bool hasNotFailed = addMovesOfPhisAtEdge(*this, from, target, incomingValues, [&](Operation* incoming, size_t indexOfPhi, Location wanted) ALWAYS_INLINE_LAMBDA -> Location {
                RegisterMask held = registersOfIncomingValues[indexOfPhi];
                if (!held)
                    return findPlaceOutsideRegistersAtEndOf(*this, from, incoming, false);
                return wanted.isRegister() && (held & maskOf(wanted.reg())) ? wanted : Location::reg(lowestRegisterAtEdge(held));
            });
            if (hasNotFailed && !m_pendingMoves.isEmpty())
                appendPendingMovesOfEdge(*this, from, false, stepsOfBlocks, allSteps);
        }
    };

    ArenaVector<Reg>& registersAtDefinitions = m_allocation.m_registerAtDefinition;
    ArenaVector<SpillFacts>& factsOfSpills = m_allocation.m_spills;
    ArenaVector<BlockSteps>& stepsOfBlocks = m_allocation.m_blocks;
    ArenaVector<Step>& allSteps = m_allocation.m_steps;
    switch (kindOfHead) {
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

static ALWAYS_INLINE bool edgeBringsEverythingWhereHeadExpectsIt(const ValueFacts* factsOfValues, const Block* target, const Allocator::Snapshot& head, const Allocator::Snapshot& end)
{
    if (head.registers & ~end.registers)
        return false;
    for (const Operation* phi : target->phis()) {
        if (!factsOfValues[phi->index()].has(ValueFlag::IsDropped))
            return false;
    }
    const RegisterBinding* next = end.bindings().data();
    for (const RegisterBinding& expected : head.bindings()) {
        const unsigned index = expected.reg.index();
        while (next->reg.index() != index)
            ++next;
        if (next->value != expected.value)
            return false;
    }
    return true;
}

static NEVER_INLINE Location findPlaceOutsideRegistersAtEndOf(Allocator& allocator, const Block* from, const Operation* value, bool isServedLate, bool headMayDoWithoutDeadValue)
{
    const ValueFacts& facts = allocator.m_liveness.of(value);
    if (facts.has(ValueFlag::IsRematerialized))
        return Location::immediate();
    if (facts.definition > allocator.m_order.earlyPositionOf(from, from->size() - 1))
        return Location();
    const SpillIndex spill = allocator.m_allocation.spillOf(value);
    if (spill != noSpill && !(allocator.isOutsideFramedPart(from) && allocator.isStoredAtHeadsOfFramedPart(value)))
        return allocator.placeToReadIn(spill);
    if (!isServedLate) {
        SpillIndex save = allocator.stateOf(value).saveInRareRegion;
        if (save != noSpill && wasSavedOnWayThroughRareRegion(allocator, from, value))
            return Location::spill(save);
    } else {
        const Order& order = allocator.m_order;
        std::span<const Allocator::SaveReadByWaitingEdge> saves = allocator.m_savesReadByWaitingEdges.span();
        auto held = std::ranges::lower_bound(saves, order.of(from).orderIndex, { }, [&](const Allocator::SaveReadByWaitingEdge& save) {
            return order.of(save.from).orderIndex;
        });
        for (; held != saves.end() && held->from == from; ++held) {
            if (held->value == value)
                return Location::spill(held->save);
        }
    }
    if (spill == noSpill)
        return Location();
    if (headMayDoWithoutDeadValue && allocator.isDeadOnEveryPathFrom(value, from, allocator.m_order.endOf(from)))
        return Location();
    allocator.giveUpFramedPartForReadOfSpill();
    return allocator.placeToReadIn(spill);
}

ALWAYS_INLINE_INTO_OTHER_FILES void Allocator::appendEdgeMoves(Block* blockOfEdge, unsigned indexOfEdge, const Snapshot& endOfBlockOfEdge)
{
    Block* targetOfEdge = blockOfEdge->successor(indexOfEdge).block();
    const bool registersAreEndOfBlockOfEdge = !endOfBlockOfEdge.isSet();
    ASSERT(m_heads[targetOfEdge->index()].isSet());
    ASSERT(!registersAreEndOfBlockOfEdge || (blockOfEdge == m_block && &endOfBlockOfEdge == &m_ends[blockOfEdge->index()] && !m_allocation.hasFramedPart()));
    const bool edgeBringsEverything = !registersAreEndOfBlockOfEdge && edgeBringsEverythingWhereHeadExpectsIt(m_valueFacts, targetOfEdge, m_heads[targetOfEdge->index()], endOfBlockOfEdge);
    if (edgeBringsEverything && !edgeMayChangeFrame(blockOfEdge, targetOfEdge))
        return;

    auto appendTheLongWay = [this]<bool registersAreEndOfFrom>(Block* from, unsigned successorIndex, const Snapshot& endOfFrom, Block* target, bool bringsEverything, ArenaVector<BlockSteps>& stepsOfBlocks, ArenaVector<Step>& allSteps) NEVER_INLINE {
        Snapshot& head = m_heads[target->index()];
        const BlockPlace& placeOfTarget = m_order.places()[target->index()];
        const bool isUnsplit = Order::isUnsplit(from, placeOfTarget);
        if (bringsEverything) {
            if (isUnsplit && edgeMayChangeFrame(from, target))
                splitEdge(from, successorIndex, endOfFrom);
            return;
        }
        const bool isServedLate = from != m_block;
        const RegisterBinding* const firstAtHead = head.bindings().data();
        const RegisterBinding* const limitAtHead = firstAtHead + head.bindings().size();
        const RegisterBinding* const firstAtEnd = endOfFrom.bindings().data();
        const RegisterBinding* const limitAtEnd = firstAtEnd + endOfFrom.bindings().size();

        auto placeAtEndOf = [&](Operation* value, Location wanted, bool headMayDoWithoutDeadValue = false) ALWAYS_INLINE_LAMBDA -> Location {
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
            return findPlaceOutsideRegistersAtEndOf(*this, from, value, isServedLate, headMayDoWithoutDeadValue);
        };

        m_pendingMoves.clear();
        auto addMove = [&](Operation* value, Location destination, Location source) ALWAYS_INLINE_LAMBDA {
            if (source != destination)
                m_pendingMoves.append(m_arena, PendingMove { value, destination, source, widthOf(value->type()) });
        };

        const bool headIsEntered = placeOfTarget.orderIndex <= m_order.of(m_block).orderIndex;
        const bool headLoadsWhatItReads = !headIsEntered && placeOfTarget.isRare() && !placeOfTarget.isLoopHead;
        const bool headMayLetGo = !headIsEntered && !placeOfTarget.isRare() && !placeOfTarget.isLoopHead;
        bool isKnownWhetherEdgeMakesWhatHeadHolds = false;
        bool edgeMakesWhatHeadHolds = false;
        RegisterMask registersOfAbsentValues = 0;
        const RegisterBinding* nextAtEnd = firstAtEnd;
        for (const RegisterBinding* bindingAtHead = firstAtHead; bindingAtHead != limitAtHead; ++bindingAtHead) {
            const RegisterBinding& binding = *bindingAtHead;
            Operation* value = binding.value;
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
            Location source = placeAtEndOf(value, destination, !headIsEntered);
            if (!source) {
                if (headIsEntered) {
                    m_proc.fail(FailureKind::WrongValueInRegister, value);
                    return;
                }
                registersOfAbsentValues |= maskOf(binding.reg);
                continue;
            }
            if (headLoadsWhatItReads && source.isSpill() && spillHoldsValueIn(from, value) && spillHoldsValueAtHeadOf(target, value)) {
                registersOfAbsentValues |= maskOf(binding.reg);
                continue;
            }
            if (headMayLetGo && source != destination) {
                bool isOwedByNoEdge;
                if (m_valueFacts[value->index()].has(ValueFlag::IsRematerialized)) {
                    if (!std::exchange(isKnownWhetherEdgeMakesWhatHeadHolds, true))
                        edgeMakesWhatHeadHolds = placeOfTarget.predecessors.size() <= 2 && from->last()->isUnconditionalJump() && isRareEdgeIntoHead(m_order, from, target);
                    isOwedByNoEdge = !edgeMakesWhatHeadHolds && !isKeptThroughLoopAround(binding.reg, value, placeOfTarget.head);
                } else
                    isOwedByNoEdge = spillHoldsValueAtHeadOf(target, value) && firstCallBehindHeadReadsValueElsewhere(target, binding.reg, value);
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

        if (registersOfAbsentValues) {
            size_t numberKept = static_cast<size_t>(limitAtHead - firstAtHead) - static_cast<size_t>(std::popcount(registersOfAbsentValues));
            RegisterBinding* kept = m_arena.allocateArray<RegisterBinding>(numberKept);
            RegisterBinding* next = kept;
            for (const RegisterBinding* binding = firstAtHead; binding != limitAtHead; ++binding) {
                if (!(registersOfAbsentValues & maskOf(binding->reg)))
                    new (NotNull, next++) RegisterBinding(*binding);
            }
            ASSERT(next == kept + numberKept);
            ASSERT(numberKept == static_cast<size_t>(std::popcount(head.registers & ~registersOfAbsentValues)));
            head.set(kept, head.registers & ~registersOfAbsentValues);
        }

        if (m_pendingMoves.isEmpty() && !(isUnsplit && edgeMayChangeFrame(from, target)))
            return;
        if (isUnsplit) {
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

ALWAYS_INLINE_INTO_OTHER_FILES bool Allocator::edgeMayChangeFrame(const Block* from, const Block* target)
{
    if (!m_allocation.hasFramedPart())
        return false;
    if (m_allocation.isInFramedPart(target))
        return m_allocation.isHeadOfFramedPart(target) && m_allocation.isInFramedPart(from);
    if (m_allocation.isInFramedPart(from))
        return true;
    if (target->last()->opcode() != Return)
        return false;
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
        if (!m_allocation.m_setOfHeadsAboveBlock.isEmpty())
            m_allocation.m_setOfHeadsAboveBlock[middle->index()] = m_allocation.m_setOfHeadsAboveBlock[from->index()];
    }
    if (!m_immediateDominators.isEmpty())
        m_immediateDominators[middle->index()] = from;
    if (listsRegistersAtHeadOf(middle))
        m_allocation.m_registersAtHead[middle->index()] = listBindingsAtHead(*this, HeadThatListsRegisters { middle, m_allocation.spillsOfValues(), startsOutsideFramedPart(middle) }, endOfFrom.bindings());
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
    ASSERT(snapshot.isSet());
    unbindRegistersBetweenBlocks(*this, m_bound);
    ValueState* const states = m_values.begin();
    const RegisterMask calleeSavesStillToNote = calleeSavesStillToNoteBetweenBlocks(*this);
    for (const RegisterBinding& binding : snapshot.bindings())
        bindFreeRegisterBetweenBlocks(*this, binding.reg, binding.value, states[binding.value->index()], calleeSavesStillToNote);
}

void Allocator::noteHintsOfHead(const Block* target)
{
    const BlockPlace* const places = m_order.places();
    const uint32_t placeOfWalk = places[m_block->index()].orderIndex;
    const std::span<Block* const> predecessorsOfTarget = places[target->index()].predecessors;
    if (places[predecessorsOfTarget.data()[predecessorsOfTarget.size() - 1]->index()].orderIndex <= placeOfWalk)
        return;

    if (!target->phis().empty())
        passOnHintsOfPhisOfHead(*this, target, placeOfWalk);

    Reg* const hints = m_hints.begin();
    for (const RegisterBinding& binding : m_heads[target->index()].bindings())
        noteHintOfValueThatHeadHolds<true>(hints, target, binding.reg, binding.value);
}

const Block* Allocator::immediateDominatorOf(const Block* block)
{
    if (m_immediateDominators.isEmpty())
        findImmediateDominatorsOfOrderedBlocks(*this);
    return m_immediateDominators[block->index()];
}

bool Allocator::dominates(const Block* above, const Block* below)
{
    if (m_immediateDominators.isEmpty())
        findImmediateDominatorsOfOrderedBlocks(*this);
    if (m_numbersOfDominatedBlocks.isEmpty())
        numberDominatedBlocksByPlaceInOrder(*this);
    const NumbersOfDominatedBlocks& dominated = m_numbersOfDominatedBlocks[m_order.of(above).orderIndex];
    uint32_t number = m_numbersOfDominatedBlocks[m_order.of(below).orderIndex].first;
    return dominated.first <= number && number <= dominated.last;
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
    OverflowCheckedSum test = overflowCheckedSumOf(m_liveness, sum);
    if (test.isWidenedOnFailure)
        return { };

    const Block* passes = m_block->successor(test.passingSuccessor).block();
    Position headOfPassingSide = m_order.headOf(passes);
    Operation* operands[] = { test.left(), test.right() };
    OperandTakenBack found;
    for (unsigned index = 0; index < (test.subtracts() ? 1u : 2u) && !found.value; ++index) {
        Operation* operand = operands[index];
        Location other = sources[1 - index];
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

    WayOutOfBranch failingSide = wayOutOfBranchThrough(m_order, m_block->successor(!test.passingSuccessor).block());
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
    OverflowCheckedSum test = overflowCheckedSumOf(m_liveness, sum);
    const Block* fails = m_block->successor(!test.passingSuccessor).block();
    if (m_operandsTakenBackAtHeads.isEmpty())
        m_operandsTakenBackAtHeads.fill(m_arena, m_proc.numberOfBlocks(), OperandTakenBack { });
    m_operandsTakenBackAtHeads[fails->index()] = operand;
    m_operandTakenBackByBranch = operand.value;
    if (otherOperand.isRegister())
        m_registersReadOnWayToFailingSuccessor = maskOf(otherOperand.reg());
}

} } } // namespace JSC::AOT::Machine

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
