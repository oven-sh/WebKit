/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTMachineOrder.h"
#include "AOTMachineTarget.h"
#include <array>
#include <cstdlib>

namespace JSC { namespace AOT { namespace Machine {

enum class ValueFlag : uint16_t {
    IsDropped = 1 << 0,
    IsFused = 1 << 1,
    IsSunk = 1 << 2,
    LivesAcrossCall = 1 << 3,
    LivesAcrossSafepoint = 1 << 4,
    LivesAcrossSuspension = 1 << 5,
    LivesIntoLandingPad = 1 << 6,
    IsRematerialized = 1 << 7,
    NeedsPointerBesideIt = 1 << 8,
    TakesOverTrap = 1 << 9,
    UpperHalfIsRead = 1 << 10,
    WorksInMemory = 1 << 11,
    AddsToWholeWord = 1 << 12,
    IsKeptForCollector = 1 << 13,
    UsesOverflowFlag = 1 << 14,
    HintIsOfRareRegion = 1 << 15,
};

struct Use {
    Position position;
    uint32_t next;
    Position headOfBlockPassedOnEveryPath;
    Position greatestHeadOfBlockPassedOnEveryPath;
};

struct ValueFacts {
    bool has(ValueFlag flag) const { return flags & static_cast<uint16_t>(flag); }

    Position definition { noPosition };
    Position lastUse { noPosition };
    uint32_t firstUse { 0 };
    uint16_t flags { 0 };
    Reg hint;
    uint8_t distanceToTrappingLoad { 0 };
};
static_assert(sizeof(ValueFacts) == 16);

struct LoopFacts {
    uint16_t pressure[numberOfBanks] { 0, 0 };
    uint16_t pressureAcrossPatches[numberOfBanks] { 0, 0 };
    RegisterMask registersTakenByPatches { 0 };
    Position endOfLastExitingBlock { 0 };
    std::span<Operation* const> valuesReadFromOutside;
};

struct ValueKeptForCollector {
    const Operation* value;
    Position until;
};

class Liveness {
    WTF_MAKE_NONCOPYABLE(Liveness);
public:
    const ValueFacts& of(const Operation* operation) const { return m_values[operation->index()]; }
    const ValueFacts* valueFacts() const { return m_values.begin(); }
    bool isDropped(const Operation* operation) const { return of(operation).has(ValueFlag::IsDropped); }
    bool isFused(const Operation* operation) const { return of(operation).has(ValueFlag::IsFused); }
    bool isSunk(const Operation* operation) const { return of(operation).has(ValueFlag::IsSunk); }
    bool standsInItsBlock(const Operation* operation) const
    {
        constexpr uint16_t absent = static_cast<uint16_t>(ValueFlag::IsDropped) | static_cast<uint16_t>(ValueFlag::IsFused) | static_cast<uint16_t>(ValueFlag::IsSunk);
        return !(of(operation).flags & absent);
    }

    static constexpr uint8_t useIsInRareRegion = 1 << 0;
    static constexpr uint8_t useReadsUpperHalf = 1 << 1;

    const Use& use(uint32_t index) const { return m_uses[index]; }
    const Use* uses() const { return m_uses.begin(); }
    uint32_t numberOfUses() const { return m_uses.size(); }
    bool isUseInRareRegion(uint32_t index) const { ASSERT(index < m_uses.size()); return m_traitsOfUses[index] & useIsInRareRegion; }
    bool isUseOfUpperHalf(uint32_t index) const { ASSERT(index < m_uses.size()); return m_traitsOfUses[index] & useReadsUpperHalf; }
    const LoopFacts& of(const Loop& loop, const Order& order) const { return m_loops[static_cast<uint32_t>(&loop - order.loops().data())]; }

    const Block* blockThatRareBlockBranchesOff(const Block* block) const { return m_blocksThatRareBlocksBranchOff.isEmpty() ? nullptr : m_blocksThatRareBlocksBranchOff[block->index()]; }

    Position nextCallBehindHeadOf(const Block* block, Bank bank) const { return m_nextCallsBehindHeads[block->index()][static_cast<unsigned>(bank)]; }
    unsigned mostLiveIn(const Block* block, Bank bank) const { return m_mostLiveInBlocks[block->index()][static_cast<unsigned>(bank)]; }
    Position lastCallIn(const Block* block, Bank bank) const { return m_lastCallsInBlocks.isEmpty() ? 0 : m_lastCallsInBlocks[block->index()][static_cast<unsigned>(bank)]; }
    Position nextCallBehindEndOf(const Block* block, Bank bank) const { return lastCallIn(block, bank) ? m_nextCallsBehindEnds[block->index()][static_cast<unsigned>(bank)] : nextCallBehindHeadOf(block, bank); }
    Position endOfWhatWaysLeadToFrom(const Block* block) const { return m_endsOfWhatWaysLeadTo[block->index()]; }
    bool standsInLoopThatMayTurn(const Order& order, const BlockPlace& place) const
    {
        const Loop* const loops = order.loops().data();
        for (uint32_t loop = place.loop; loop != noLoop; loop = loops[loop].parent) {
            if (endOfWhatWaysLeadToFrom(order.blocksInOrder()[loops[loop].firstBlock]) == noPosition)
                return true;
        }
        return false;
    }
    bool hasLandingPads() const { return m_hasLandingPads; }

    std::span<Operation* const> sunkInto(const Block* block) const { return m_sunk.isEmpty() ? std::span<Operation* const>() : m_sunk[block->index()].span(); }

    std::span<const ValueKeptForCollector> valuesKeptForCollector() const { return m_valuesKeptForCollector.span(); }

    const Operation* trapTakenOverBy(const Operation* access) const
    {
        const ValueFacts& facts = of(access);
        if (!facts.has(ValueFlag::TakesOverTrap))
            return nullptr;
        return access->owner()->at(access->indexInBlock() - facts.distanceToTrappingLoad);
    }

    bool worksInMemory(const Operation* operation) const { return of(operation).has(ValueFlag::WorksInMemory); }
    const Operation* storeBehind(const Operation* branch) const { return m_storesBehindBranches[branch->owner()->index()]; }

    bool usesOverflowFlag(const Operation* operation) const { return of(operation).has(ValueFlag::UsesOverflowFlag); }

private:
    friend class Arena;
    friend class BackwardScan;

    Liveness() = default;

    ArenaVector<ValueFacts> m_values;
    ArenaVector<Use> m_uses;
    uint8_t* m_traitsOfUses { nullptr };
    ArenaVector<LoopFacts> m_loops;
    ArenaVector<const Block*> m_blocksThatRareBlocksBranchOff;
    ArenaVector<std::array<Position, numberOfBanks>> m_nextCallsBehindHeads;
    ArenaVector<std::array<uint16_t, numberOfBanks>> m_mostLiveInBlocks;
    ArenaVector<std::array<Position, numberOfBanks>> m_lastCallsInBlocks;
    ArenaVector<std::array<Position, numberOfBanks>> m_nextCallsBehindEnds;
    ArenaVector<Position> m_endsOfWhatWaysLeadTo;
    bool m_hasLandingPads { false };
    ArenaVector<ArenaVector<Operation*>> m_sunk;
    ArenaVector<ValueKeptForCollector> m_valuesKeptForCollector;
    ArenaVector<const Operation*> m_storesBehindBranches;
};

struct MemoryUpdate {
    enum Kind : uint8_t {
        InPlace,
        ComparedAhead,
        TestedBehind,
        LowHalf,
    };

    Operation* operand() const { return userOfOperand->child(indexOfOperand); }

    Kind kind { InPlace };
    Opcode opcode { Add };
    Opcode relation { Equal };
    bool testsOverflow { false };
    bool comparesWord { false };
    uint8_t successorThatStores { 0 };
    const Operation* computation { nullptr };
    const Operation* load { nullptr };
    const Operation* userOfOperand { nullptr };
    unsigned indexOfOperand { 0 };
    int64_t constant { 0 };
    int64_t bias { 0 };
    int64_t bound { 0 };
};

JS_EXPORT_PRIVATE MemoryUpdate memoryUpdateOf(const Liveness&, const Operation* storeOrBranch);

struct OverflowCheckedSum {
    Operation* left() const { return userOfLeft->child(indexOfLeft); }
    Operation* right() const { return userOfRight->child(indexOfRight); }
    bool subtracts() const { return sum->opcode() == Sub; }
    bool branchReadsSum() const { return leastResult || isWidenedOnFailure; }

    const Operation* sum { nullptr };
    const Operation* userOfLeft { nullptr };
    const Operation* userOfRight { nullptr };
    const Operation* userOfSum { nullptr };
    unsigned indexOfLeft { 0 };
    unsigned indexOfRight { 0 };
    unsigned indexOfSum { 0 };
    uint8_t passingSuccessor { 0 };
    std::optional<int32_t> leastResult;
    bool registerHoldsWholeSum { false };
    bool mayFallBelowInt32 { false };
    bool isWidenedOnFailure { false };
};

JS_EXPORT_PRIVATE OverflowCheckedSum overflowCheckedSumOf(const Liveness&, const Operation* sumOrBranch);

struct WayOutOfBranch {
    bool holds(Position position) const { return begin <= position && position < end; }

    Position begin { 0 };
    Position end { 0 };
    bool leadsOn { false };
};
JS_EXPORT_PRIVATE WayOutOfBranch wayOutOfBranchThrough(const Order&, const Block* successor);

inline constexpr unsigned maxLeaves = 5;

inline Operation* armOfSelectThatExtends(const Operation* select, const Operation* word)
{
    if (word->hasNoPosition())
        return nullptr;
    for (unsigned index : { 1u, 2u }) {
        Operation* arm = select->child(index);
        if ((arm->opcode() == ZExt32 || arm->opcode() == SExt32) && arm->child(0) == word)
            return arm;
    }
    return nullptr;
}

template<typename Functor>
void forEachLeafBelow(const Liveness& liveness, const Operation* operation, const Functor& functor, const Operation* selectAbove)
{
    if (operation->isUnconditionalJump())
        return;
    if (liveness.worksInMemory(operation)) [[unlikely]] {
        MemoryUpdate update = memoryUpdateOf(liveness, operation);
        functor(update.load, update.load->numChildren() - 1, update.load->lastChild());
        functor(update.userOfOperand, update.indexOfOperand, update.operand());
        return;
    }
    if (liveness.usesOverflowFlag(operation)) [[unlikely]] {
        OverflowCheckedSum test = overflowCheckedSumOf(liveness, operation);
        if (operation->opcode() != Branch) {
            functor(test.userOfLeft, test.indexOfLeft, test.left());
            functor(test.userOfRight, test.indexOfRight, test.right());
        } else if (test.branchReadsSum())
            functor(test.userOfSum, test.indexOfSum, test.userOfSum->child(test.indexOfSum));
        return;
    }
    std::span<Operation* const> children = operation->children();
    for (unsigned index = 0; index < children.size(); ++index) {
        Operation* child = children[index];
        if (liveness.isFused(child)) {
            forEachLeafBelow(liveness, child, functor, operation->opcode() == Select ? operation : nullptr);
            continue;
        }
        if (selectAbove) [[unlikely]] {
            if (Operation* arm = armOfSelectThatExtends(selectAbove, child))
                child = arm;
        }
        functor(operation, index, child);
    }
}

template<typename FunctorForChild, typename FunctorForRest>
ALWAYS_INLINE void forEachLeafInParts(const Liveness& liveness, const Operation* operation, const FunctorForChild& functorForChild, const FunctorForRest& functorForRest)
{
    constexpr uint16_t hasLeavesOfItsOwn = static_cast<uint16_t>(ValueFlag::WorksInMemory) | static_cast<uint16_t>(ValueFlag::UsesOverflowFlag);
    const ValueFacts* facts = liveness.valueFacts();
    if (facts[operation->index()].flags & hasLeavesOfItsOwn) [[unlikely]] {
        functorForRest(operation, nullptr);
        return;
    }
    if (operation->isUnconditionalJump())
        return;
    std::span<Operation* const> children = operation->children();
    Operation* const* child = children.data();
    for (unsigned index = 0, numberOfChildren = static_cast<unsigned>(children.size()); index < numberOfChildren; ++index) {
        if (facts[child[index]->index()].has(ValueFlag::IsFused)) {
            functorForRest(child[index], operation->opcode() == Select ? operation : nullptr);
            continue;
        }
        functorForChild(index, child[index]);
    }
}

template<typename FunctorForChild, typename FunctorBelowFusedChild, typename FunctorForRest>
ALWAYS_INLINE void forEachLeafInParts(const Liveness& liveness, const Operation* operation, const FunctorForChild& functorForChild, const FunctorBelowFusedChild& functorBelowFusedChild, const FunctorForRest& functorForRest)
{
    const ValueFacts* facts = liveness.valueFacts();
    forEachLeafInParts(liveness, operation, functorForChild, [&](const Operation* below, const Operation* selectAbove) ALWAYS_INLINE_LAMBDA {
        constexpr uint16_t hasLeavesOfItsOwn = static_cast<uint16_t>(ValueFlag::WorksInMemory) | static_cast<uint16_t>(ValueFlag::UsesOverflowFlag);
        if (below == operation || selectAbove || (facts[below->index()].flags & hasLeavesOfItsOwn) || below->isUnconditionalJump()) [[unlikely]] {
            functorForRest(below, selectAbove);
            return;
        }
        std::span<Operation* const> children = below->children();
        Operation* const* child = children.data();
        for (unsigned index = 0, numberOfChildren = static_cast<unsigned>(children.size()); index < numberOfChildren; ++index) {
            if (facts[child[index]->index()].has(ValueFlag::IsFused)) {
                functorForRest(child[index], below->opcode() == Select ? below : nullptr);
                continue;
            }
            functorBelowFusedChild(below, index, child[index]);
        }
    });
}

template<typename Functor>
ALWAYS_INLINE void forEachLeaf(const Liveness& liveness, const Operation* operation, const Functor& functor)
{
    forEachLeafInParts(liveness, operation, [&](unsigned index, Operation* child) ALWAYS_INLINE_LAMBDA {
        functor(operation, index, child);
    }, [&](const Operation* below, const Operation* selectAbove) ALWAYS_INLINE_LAMBDA {
        forEachLeafBelow(liveness, below, functor, selectAbove);
    });
}

inline bool addsScaledIndex(const Liveness& liveness, const Operation* operation)
{
    if (operation->opcode() != Add || liveness.isFused(operation))
        return false;
    auto isFusedShift = [&](const Operation* child) {
        return child->opcode() == Shl && liveness.isFused(child);
    };
    return isFusedShift(operation->child(0)) || isFusedShift(operation->child(1));
}

inline bool isLeafOfAddress(const Liveness& liveness, const Operation* user, unsigned childIndex)
{
    Opcode opcode = user->opcode();
    if (isMemoryAccess(opcode))
        return childIndex + 1 == user->numChildren();
    if (liveness.isFused(user))
        return opcode == Add || opcode == Shl || opcode == ZExt32 || opcode == SExt32;
    return addsScaledIndex(liveness, user);
}

inline bool standsInInstruction(const Liveness& liveness, const Operation* user, unsigned childIndex, const Operation* leaf)
{
    if (addsScaledIndex(liveness, user))
        return scaledSumAcceptsBase(*leaf);
    bool mayStandThere = !isLeafOfAddress(liveness, user, childIndex) || isMemoryAccess(user->opcode()) || (user->opcode() == Shl && childIndex == 1);
    return mayStandThere && acceptsImmediate(*user, childIndex, *leaf);
}

constexpr bool changesWidthOfInteger(Opcode opcode) { return opcode == SExt32 || opcode == ZExt32 || opcode == Trunc; }

inline bool isFreeWhenCoalesced(const Liveness& liveness, const Operation* operation)
{
    Opcode opcode = operation->opcode();
    if (!changesWidthOfInteger(opcode))
        return false;
    if (opcode == Trunc || !liveness.of(operation).has(ValueFlag::UpperHalfIsRead))
        return true;
    if (opcode != ZExt32)
        return false;
    const Operation* child = operation->child(0);
    if (clearsUpperHalf(child))
        return true;
    if (child->opcode() != Trunc || !liveness.usesOverflowFlag(child->child(0)))
        return false;
    const Operation* sum = child->child(0);
    OverflowCheckedSum test = overflowCheckedSumOf(liveness, sum);
    if (!test.isWidenedOnFailure || operation->owner()->procedure().traits().shouldAssumeClearUpperHalves)
        return true;
    const Block* block = operation->owner();
    return block == sum->owner() || block == sum->owner()->successor(test.passingSuccessor).block();
}

inline constexpr unsigned maxOperationsBetweenLoadAndUser = 4;

inline constexpr unsigned maxOperationsBetweenLoadAndUpdate = 24;

constexpr bool mayBeFusedAsAddress(Opcode opcode) { return opcode == Add; }

Liveness* scanBackward(Procedure&);

class BackwardScan {
    WTF_MAKE_NONCOPYABLE(BackwardScan);
public:
    BackwardScan(Procedure&, Liveness&);
    void run();
    void letGoOfWorkingLists();

    template<bool regionsHaveKinds> void scanBlock(Block*);

    void addUse(Operation* value, Position, Reg hint = Reg());
    void define(Operation*, Position);
    void noteCall(const PatchOperation*, Position late, const Block*);
    void enterLoopsEndingAt(uint32_t innermostLoop, Position);
    void leaveLoopsBeginningAt(uint32_t innermostLoop, Position);
    void findBlocksThatRareBlocksBranchOff();
    ValueFacts& facts(const Operation* operation) { return m_facts[operation->index()]; }
    ArenaVector<Use>& uses() { return m_liveness.m_uses; }
    uint8_t*& traitsOfUses() { return m_liveness.m_traitsOfUses; }
    ArenaVector<std::array<Position, numberOfBanks>>& lastCallsInBlocks() { return m_liveness.m_lastCallsInBlocks; }
    ArenaVector<std::array<Position, numberOfBanks>>& nextCallsBehindEnds() { return m_liveness.m_nextCallsBehindEnds; }
    void set(const Operation* operation, ValueFlag flag) { facts(operation).flags |= static_cast<uint16_t>(flag); }

    void fuseInto(Operation* user);
    bool tryFuseScaledIndex(Operation* sum);
    bool tryHandOverTrap(Operation* trappingLoad);
    unsigned numberOfLeavesOf(const Operation*) const;
    bool tryUpdateInMemory(Operation* store);
    void settleLowHalfUpdatesOf(uint32_t loop);
    bool tryTestSumByOverflow(Operation* branch);

    bool trySink(Operation*);
    void noteRegionOfUse(Operation* value, const Block* user);

    void noteWaysOnFrom(const Block*, Position end);

    void applyCollectorRules(Operation*);
    void noteValueKeptForCollector(const Operation* value, Position until)
    {
        if (m_proc.traits().savesFrameOnSuspend)
            m_liveness.m_valuesKeptForCollector.append(m_proc.arena(), ValueKeptForCollector { value, until });
    }

    Procedure& m_proc;
    const Order& m_order;
    Liveness& m_liveness;
    ValueFacts* m_facts { nullptr };
    const BlockPlace* m_places { nullptr };
    RegisterMask m_callerSaved[numberOfBanks];
    bool m_hasLoops { false };
    bool m_hasRareRegions { false };

    struct PlaceOfReads {
        bool isRare() const { return rareRegion != noRareRegion; }

        uint32_t loop { noLoop };
        uint32_t rareRegion { noRareRegion };
        Position beginOfLoop { 0 };
        Position passedOnEveryPath { noPosition };
        Position positionThatCountsForCalls { noPosition };
        uint32_t loopThatCountsForCalls { noLoop };
        bool countsForLoops { false };
        uint8_t traitsOfUses { 0 };
        uint16_t flagsOfWhatIsRead { 0 };
    };
    PlaceOfReads m_placeOfReads;

    Position m_nearestEventBehind { noPosition };
    Position m_nextCall[numberOfBanks] { noPosition, noPosition };
    std::array<Position, numberOfBanks> m_nextCallThatTakesRegisters { noPosition, noPosition };
    Position m_nextSafepoint { noPosition };
    Position m_nextSuspension { noPosition };
    Position m_nextLandingPad { noPosition };
    Position m_nextLandingPadThatComesBack { noPosition };
    static constexpr uint8_t regionHoldsLandingPad = 1;
    static constexpr uint8_t regionComesBack = 2;
    ArenaVector<uint8_t> m_kindsOfRegions;
    struct RegionState {
        Position nextCall[numberOfBanks] { noPosition, noPosition };
    };
    ArenaVector<RegionState> m_regions;
    static constexpr uint32_t severalRegions = UINT32_MAX - 1;
    struct SummaryOfReads {
        uint32_t regionOfUses { noRareRegion };
        Position lastReadCountedForCalls { 0 };
    };
    ArenaVector<SummaryOfReads> m_summariesOfReads;
    ArenaVector<uint32_t> m_loopCountedFor;
    struct LoopState {
        uint16_t mostLive[numberOfBanks] { 0, 0 };
        uint16_t mostLiveAcrossPatches[numberOfBanks] { 0, 0 };
        uint32_t outerValuesRead[numberOfBanks] { 0, 0 };
        uint32_t outerValuesOfRunNotCounted[numberOfBanks] { 0, 0 };
        RegisterMask registersTakenByPatches { 0 };
        ArenaVector<Operation*> valuesReadFromOutside;
        uint16_t phisReadOnlyBehindLoop[numberOfBanks] { 0, 0 };
        Position endOfLastExitingBlock { 0 };
        static constexpr uint32_t notLookedFor = noLoop - 1;
        uint32_t nearestLoopAroundOfOtherRegion { notLookedFor };
        uint32_t loopAroundThatMayLackExitingBlock { noLoop };
    };
    ArenaVector<LoopState> m_loopStates;
    ArenaVector<uint32_t> m_loopOfLastListedRead;
    ArenaVector<uint32_t> m_loopOfLastReadExtendedForCalls;
    BitSpan m_isPhiReadOnlyBehindLoop;
    uint16_t m_numberLive[numberOfBanks] { 0, 0 };
    std::array<uint16_t, numberOfBanks> m_mostLiveInBlock { 0, 0 };

    static constexpr uint64_t bitOfWord(int32_t offset) { return uint64_t { 1 } << (static_cast<uint32_t>(offset) >> 3 & 63); }
    ArenaVector<uint64_t> m_wordsReadWhole;
    struct LowHalfUpdate {
        Operation* branch;
        uint32_t base;
        uint32_t loop;
        uint64_t bit;
    };
    ArenaVector<LowHalfUpdate> m_lowHalfUpdatesInLoops;
};

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT)
