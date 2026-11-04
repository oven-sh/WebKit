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
    // Nothing that stays uses it, and it need not stay for itself. No later step looks at it.
    IsDropped = 1 << 0,
    // It has one user, which computes it as part of its own instruction. It gets no register. Its children are read where the user stands.
    IsFused = 1 << 1,
    // It stands at the head of the block Liveness::sunkInto() lists it for, and no longer where it was appended.
    IsSunk = 1 << 2,
    // Between its definition and its last use stands a call that counts for it: a Patch that clobbers late every register of its bank that no callee saves, or an edge to a landing pad.
    // A call in a rare region counts only for what is defined in that region.
    // A read in a rare region of what is defined outside it counts where blockThatRareBlockBranchesOff() ends, wherever the order has put the region.
    LivesAcrossCall = 1 << 3,
    // The same for a Patch with Effects::mayCollect, wherever it stands.
    LivesAcrossSafepoint = 1 << 4,
    LivesAcrossSuspension = 1 << 5,
    // A landing pad or what follows it reads it, so it has a slot from its definition on.
    LivesIntoLandingPad = 1 << 6,
    // An operation without position. It is made again at each use and is never spilled.
    IsRematerialized = 1 << 7,
    // A narrow reference that lives across a safepoint and names no permanent cell: the pointer it stands for is stored in a slot behind its definition.
    NeedsPointerBesideIt = 1 << 8,
    // A load or a store that faults in the place of a trapping load in front of it, which is dropped: Liveness::trapTakenOverBy().
    TakesOverTrap = 1 << 9,
    // Some read of it takes in the upper half of an Int64. A Trunc does not, and neither does an Upsilon for a Phi that Truncs alone read: OperationFlag::UpperHalfIsRead.
    UpperHalfIsRead = 1 << 10,
    // A Store that computes the word it stores where the word lies, or a Branch that compares the word there or stores in the place of a Store behind it: memoryUpdateOf().
    WorksInMemory = 1 << 11,
    // Of a Branch that would add to the low half of a word: a Load reads the whole word soon after, and would wait for the half to reach the cache. So the Branch adds to the whole word.
    AddsToWholeWord = 1 << 12,
    // The collector needs it for as long as something else is read: the owner of an inner pointer, the narrow reference behind a decoded word.
    // Where its range ends behind that anyway, no read says so. So it is needed to the end of its range, on whatever path.
    IsKeptForCollector = 1 << 13,
    // An Add or a Sub of two int32 in 64 bits that is made in 32, and the Branch behind it, which goes by the overflow flag that it leaves: sumTestedByOverflowOf().
    GoesByOverflow = 1 << 14,
    // ValueFacts::hint is what a read in a rare region would like, and the value is defined outside that region. The scan alone asks.
    HintIsOfRareRegion = 1 << 15,
};

// One read of a value, by an operation that is neither dropped nor fused.
struct Use {
    Position position;
    // In Liveness::use(), the next read of the same value in rising order of positions. Zero at the end.
    uint32_t next;
    // The head of a block that every path to the block around the position passes: of a rare block Liveness::blockThatRareBlockBranchesOff(), else its one predecessor. noPosition where it has neither.
    Position headOfBlockPassedOnEveryPath;
    // The greatest of those among this read and the later reads of the same value.
    Position greatestHeadOfBlockPassedOnEveryPath;
};

struct ValueFacts {
    bool has(ValueFlag flag) const { return flags & static_cast<uint16_t>(flag); }

    // The late position of the operation, the head of the block for a Phi or for what sank.
    Position definition { noPosition };
    // noPosition if nothing reads it. For a value that is read inside a loop and defined ahead of it: no earlier than the end of the loop.
    Position lastUse { noPosition };
    uint32_t firstUse { 0 };
    uint16_t flags { 0 };
    // The register that the nearest use with a preference would like it in. A use in a rare region of what is defined outside it counts only where no other use has one.
    Reg hint;
    // Of an access with TakesOverTrap: the dropped load stands this many places in front of it in its block.
    uint8_t distanceToTrappingLoad { 0 };
};
static_assert(sizeof(ValueFacts) == 16);

struct LoopFacts {
    // The most values of a bank that are live at one place of the loop, not counting those that are live at its start and read nowhere in it.
    // A place in a rare region counts only where the head of the loop stands in that region: what a rare region takes it saves and restores.
    // A value that rare regions of the loop alone read is counted as read. Allocator::chooseRegistersKeptThroughLoop() takes it off again, and treats it as one that lives through.
    // A Phi of the head that is read behind the loop does not count behind its last read in the loop, in a block of this loop and no inner one that stands behind endOfLastExitingBlock:
    // its range goes on in the order, and Allocator::isDeadOnEveryPathFrom() frees it there.
    uint16_t pressure[numberOfBanks] { 0, 0 };
    // The registers that a Patch of the loop clobbers, takes an operand in by name or leaves a result in by name. What lies in one of them at the head gives way on every turn.
    // A Patch in a rare region counts only where the head of the loop stands in that region.
    RegisterMask registersTakenByPatches { 0 };
    // Order::endOf() the last block of the loop from which an edge leads out of it. Zero if there is none.
    Position endOfLastExitingBlock { 0 };
    // What the loop reads and does not define: a value that is defined ahead of it, and a value without position that an instruction of the loop needs in a register. Each once.
    // A read in a rare region lists nothing, unless the innermost loop around it stands in that region. Such a read lists the value for the loops further out too, whatever region they stand in.
    std::span<Operation* const> valuesReadFromOutside;
};

// The collector needs the value up to the position, whether or not an operation reads it there: what it keeps alive is read there for the last time.
struct ValueKeptForCollector {
    const Operation* value;
    Position until;
};

// Step 3: what one pass from the last block of the order to the first finds out.
class Liveness {
    WTF_MAKE_NONCOPYABLE(Liveness);
public:
    const ValueFacts& of(const Operation* operation) const { return m_values[operation->index()]; }
    // By Operation::index(), for whoever goes through all of them and has no other use for the operation.
    const ValueFacts& ofOperationAt(uint32_t index) const { return m_values[index]; }
    // By Operation::index(), for a loop that asks about many values.
    const ValueFacts* valueFacts() const { return m_values.begin(); }
    bool isDropped(const Operation* operation) const { return of(operation).has(ValueFlag::IsDropped); }
    bool isFused(const Operation* operation) const { return of(operation).has(ValueFlag::IsFused); }
    bool isSunk(const Operation* operation) const { return of(operation).has(ValueFlag::IsSunk); }
    // Whether the allocator and the encoder meet it where it was appended.
    bool standsInItsBlock(const Operation* operation) const
    {
        constexpr uint16_t absent = static_cast<uint16_t>(ValueFlag::IsDropped) | static_cast<uint16_t>(ValueFlag::IsFused) | static_cast<uint16_t>(ValueFlag::IsSunk);
        return !(of(operation).flags & absent);
    }

    static constexpr uint8_t useIsInRareRegion = 1 << 0;
    static constexpr uint8_t useReadsUpperHalf = 1 << 1;

    const Use& use(uint32_t index) const { return m_uses[index]; }
    // What use() indexes. Entry zero is no read and has noPosition for its position.
    const Use* uses() const { return m_uses.begin(); }
    // Entry zero, which is no read, among them.
    uint32_t numberOfUses() const { return m_uses.size(); }
    // Whether the operation that reads stands in a rare region. In a procedure without loops a read that no operation makes counts as one outside.
    bool isUseInRareRegion(uint32_t index) const { ASSERT(index < m_uses.size()); return m_traitsOfUses[index] & useIsInRareRegion; }
    // Whether the read takes in the upper half of an Int64: what ValueFlag::UpperHalfIsRead says of all reads together.
    bool isUseOfUpperHalf(uint32_t index) const { ASSERT(index < m_uses.size()); return m_traitsOfUses[index] & useReadsUpperHalf; }
    const LoopFacts& of(const Loop& loop, const Order& order) const { return m_loops[static_cast<uint32_t>(&loop - order.loops().data())]; }

    // Of a block in a rare region that one way alone leads to from outside the region: the block outside that the way branches off. Every path to the block passes it.
    // Null for any other block, for a landing pad and for what stands behind a landing pad or a join of the region.
    const Block* blockThatRareBlockBranchesOff(const Block* block) const { return m_blocksThatRareBlocksBranchOff.isEmpty() ? nullptr : m_blocksThatRareBlocksBranchOff[block->index()]; }

    // Of a block outside rare regions: the late position of the first Patch at or behind its head, in the order and outside rare regions, that clobbers every register of the bank that no callee saves. noPosition if there is none.
    Position nextCallBehindHeadOf(const Block* block, Bank bank) const { return m_nextCallsBehindHeads[block->index()][static_cast<unsigned>(bank)]; }
    // The most values of the bank whose ranges cover one place of the block.
    unsigned mostLiveIn(const Block* block, Bank bank) const { return m_mostLiveInBlocks[block->index()][static_cast<unsigned>(bank)]; }
    // Of a block outside rare regions: the late position of its last such Patch. Zero if it holds none.
    Position lastCallIn(const Block* block, Bank bank) const { return m_lastCallsInBlocks.isEmpty() ? 0 : m_lastCallsInBlocks[block->index()][static_cast<unsigned>(bank)]; }
    // Of a block outside rare regions: as nextCallBehindHeadOf(), from the end of the block on. In a block without such a Patch the two are one.
    Position nextCallBehindEndOf(const Block* block, Bank bank) const { return lastCallIn(block, bank) ? m_nextCallsBehindEnds[block->index()][static_cast<unsigned>(bank)] : nextCallBehindHeadOf(block, bank); }
    // Order::endOf() the last block in the order that a way leads to from the block, the block itself among them: no way from it comes to this position or to one behind it.
    // noPosition where some way from the block takes an edge that leads back in the order, and of a block that Order::splitLate() has made.
    Position endOfWhatWaysLeadToFrom(const Block* block) const { return m_endsOfWhatWaysLeadTo[block->index()]; }
    // Whether some block of the order is a landing pad.
    bool hasLandingPads() const { return m_hasLandingPads; }

    // In the order in which they were appended, so each stands behind what it reads.
    std::span<Operation* const> sunkInto(const Block* block) const { return m_sunk.isEmpty() ? std::span<Operation* const>() : m_sunk[block->index()].span(); }

    // Of a function that saves its frame. A value is listed once for each value that it keeps alive.
    std::span<const ValueKeptForCollector> valuesKeptForCollector() const { return m_valuesKeptForCollector.span(); }

    // Of an access with TakesOverTrap: the dropped load, whose origin the record of the fault names. Null for any other operation.
    const Operation* trapTakenOverBy(const Operation* access) const
    {
        const ValueFacts& facts = of(access);
        if (!facts.has(ValueFlag::TakesOverTrap))
            return nullptr;
        return access->owner()->at(access->indexInBlock() - facts.distanceToTrappingLoad);
    }

    bool worksInMemory(const Operation* operation) const { return of(operation).has(ValueFlag::WorksInMemory); }
    // Of a Branch with WorksInMemory: the Store behind it, which stays only where the Branch compares and does not store.
    const Operation* storeBehind(const Operation* branch) const { return m_storesBehindBranches[branch->owner()->index()]; }

    bool goesByOverflow(const Operation* operation) const { return of(operation).has(ValueFlag::GoesByOverflow); }

private:
    friend class Arena;
    friend class BackwardScan;

    Liveness() = default;

    // By Operation::index().
    ArenaVector<ValueFacts> m_values;
    // Entry zero is not used.
    ArenaVector<Use> m_uses;
    // By the index in m_uses, with room for as many as m_uses has: useIsInRareRegion, useReadsUpperHalf.
    uint8_t* m_traitsOfUses { nullptr };
    ArenaVector<LoopFacts> m_loops;
    // By Block::index(). Empty in a procedure without rare regions.
    ArenaVector<const Block*> m_blocksThatRareBlocksBranchOff;
    // By Block::index().
    ArenaVector<std::array<Position, numberOfBanks>> m_nextCallsBehindHeads;
    ArenaVector<std::array<uint16_t, numberOfBanks>> m_mostLiveInBlocks;
    // Both empty until the scan meets a call outside rare regions. The second is written for a block that holds one.
    ArenaVector<std::array<Position, numberOfBanks>> m_lastCallsInBlocks;
    ArenaVector<std::array<Position, numberOfBanks>> m_nextCallsBehindEnds;
    ArenaVector<Position> m_endsOfWhatWaysLeadTo;
    bool m_hasLandingPads { false };
    // By Block::index(). Empty until the first operation sinks.
    ArenaVector<ArenaVector<Operation*>> m_sunk;
    ArenaVector<ValueKeptForCollector> m_valuesKeptForCollector;
    // By Block::index() of the block that the Branch ends. Empty until the first Branch works in memory.
    ArenaVector<const Operation*> m_storesBehindBranches;
};

// What an operation with ValueFlag::WorksInMemory does with the word that `load` has read. Nothing has written the word since.
struct MemoryUpdate {
    enum Kind : uint8_t {
        // A Store: `opcode` [word], operand.
        InPlace,
        // A Branch: the Store behind it, which is InPlace, is reached where [word] `relation` bound.
        ComparedAhead,
        // A Branch, in the place of the Store behind it, which is dropped: `opcode` [word], operand.
        // Where that overflows, or [word] `relation` bound does not hold behind it, the way to the other successor takes it back.
        TestedBehind,
        // The same with the low half of a word of 64 bits. Where the half carries or borrows it is taken back, and the whole sum is stored if sum + bias `relation` bound.
        LowHalf,
    };

    Operation* operand() const { return userOfOperand->child(indexOfOperand); }

    Kind kind { InPlace };
    // Add, Sub, BitAnd, BitOr or BitXor, as the instruction does it.
    Opcode opcode { Add };
    Opcode relation { Equal };
    // Of TestedBehind.
    bool testsOverflow { false };
    bool comparesWord { false };
    // Of a Branch: the successor that the Store stands behind.
    uint8_t successorThatStores { 0 };
    // What the Store stores, as it stands or below a Trunc.
    const Operation* computation { nullptr };
    // It stays where something else reads it.
    const Operation* load { nullptr };
    // The operand beside the word. Where the instruction has 32 bits, an Int64 is read by its low half.
    const Operation* userOfOperand { nullptr };
    unsigned indexOfOperand { 0 };
    // Where the operand is a constant: as the instruction takes it.
    int64_t constant { 0 };
    int64_t bias { 0 };
    int64_t bound { 0 };
};

// AOTMachineFusion.cpp. It goes by the operations alone, so every step gets the same answer.
JS_EXPORT_PRIVATE MemoryUpdate memoryUpdateOf(const Liveness&, const Operation* storeOrBranch);

// What a Branch with ValueFlag::GoesByOverflow tests: the Add or Sub in front of it in its block, which has the flag too.
// One instruction of 32 bits makes the sum of the low halves of its operands and sets the flags. The Branch passes where that does not overflow and gives no less than `leastResult`.
struct SumTestedByOverflow {
    Operation* left() const { return userOfLeft->child(indexOfLeft); }
    Operation* right() const { return userOfRight->child(indexOfRight); }
    bool subtracts() const { return sum->opcode() == Sub; }
    bool branchReadsSum() const { return leastResult || isWidenedWhereTestFails; }

    const Operation* sum { nullptr };
    // An operand is the child of the extension below the sum, or what a Trunc below that cuts. One of the two may be a constant that is an int32, a child of the sum itself.
    const Operation* userOfLeft { nullptr };
    const Operation* userOfRight { nullptr };
    // The operation of the test that reads the sum.
    const Operation* userOfSum { nullptr };
    unsigned indexOfLeft { 0 };
    unsigned indexOfRight { 0 };
    unsigned indexOfSum { 0 };
    uint8_t successorThatPasses { 0 };
    std::optional<int32_t> leastResult;
    // No sum is below zero. An instruction of 32 bits clears the upper half of its register, which then holds the whole sum whether or not the instruction overflows.
    bool registerHoldsWholeSum { false };
    // Some sum is less than any int32. Where none is, an instruction that overflows leaves the whole sum too.
    bool mayFallBelowInt32 { false };
    // The failing side reads the upper half, which the way there makes of the low half. Nothing else reads it.
    bool isWidenedWhereTestFails { false };
};

// AOTMachineFusion.cpp. It goes by the operations and by ValueFlag::UpperHalfIsRead of the sum, so every step behind the scan gets the same answer.
JS_EXPORT_PRIVATE SumTestedByOverflow sumTestedByOverflowOf(const Liveness&, const Operation* sumOrBranch);

// What one way out of a Branch alone leads to, as positions from `begin` up to `end`. The successor has no other predecessor.
// It is the rare region that the successor alone enters, which the order keeps in one piece, else the successor by itself.
struct WayOutOfBranch {
    bool holds(Position position) const { return begin <= position && position < end; }

    Position begin { 0 };
    Position end { 0 };
    // An edge leads on from there to some other block.
    bool leadsOn { false };
};
JS_EXPORT_PRIVATE WayOutOfBranch wayOutOfBranchThrough(const Order&, const Block* successor);

// The operands that an operation needs in places of their own: its children, with the children of a fused child in the place of that child, and so on down.
// The scan fuses nothing that would give an operation other than a Patch more than maxLeaves. Of a Check the leaves of its predicate alone count.
inline constexpr unsigned maxLeaves = 5;

// Rule 8.
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

// functor(const Operation* user, unsigned childIndex, Operation* leaf), depth first, children in rising order. Every step that lists leaves lists them in this order.
// A Branch that tests nothing has none. An operation that works in memory has two: the base of the word, as the child of the load, and the operand.
// A sum that goes by overflow has its two operands, and the Branch behind it the sum or nothing: SumTestedByOverflow::branchReadsSum().
// The leaf is that child of the user, but for rule 8. `selectAbove` is the Select that `operation` is fused into, null if it is fused into none.
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
    if (liveness.goesByOverflow(operation)) [[unlikely]] {
        SumTestedByOverflow test = sumTestedByOverflowOf(liveness, operation);
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

// Most operations have their children for leaves, so those are gone through where the caller stands.
// functorForChild(unsigned childIndex, Operation* leaf) for each child of the operation that is a leaf.
// functorForRest(const Operation* below, const Operation* selectAbove) in the place of the leaves that forEachLeafBelow() lists for the two: of a fused child, or of the operation itself where it works in memory or goes by overflow.
template<typename FunctorForChild, typename FunctorForRest>
ALWAYS_INLINE void forEachLeafInParts(const Liveness& liveness, const Operation* operation, const FunctorForChild& functorForChild, const FunctorForRest& functorForRest)
{
    constexpr uint16_t hasLeavesOfItsOwn = static_cast<uint16_t>(ValueFlag::WorksInMemory) | static_cast<uint16_t>(ValueFlag::GoesByOverflow);
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

// The same with one level more gone through where the caller stands: functorBelowFusedChild(const Operation* user, unsigned childIndex, Operation* leaf) for each leaf among the children of a fused child, which is `user`.
// A comparison below a Branch and a sum below a load are most of what is fused.
template<typename FunctorForChild, typename FunctorBelowFusedChild, typename FunctorForRest>
ALWAYS_INLINE void forEachLeafInParts(const Liveness& liveness, const Operation* operation, const FunctorForChild& functorForChild, const FunctorBelowFusedChild& functorBelowFusedChild, const FunctorForRest& functorForRest)
{
    const ValueFacts* facts = liveness.valueFacts();
    forEachLeafInParts(liveness, operation, functorForChild, [&](const Operation* below, const Operation* selectAbove) ALWAYS_INLINE_LAMBDA {
        constexpr uint16_t hasLeavesOfItsOwn = static_cast<uint16_t>(ValueFlag::WorksInMemory) | static_cast<uint16_t>(ValueFlag::GoesByOverflow);
        if (below == operation || selectAbove || (facts[below->index()].flags & hasLeavesOfItsOwn) || below->isUnconditionalJump()) [[unlikely]] {
            functorForRest(below, selectAbove);
            return;
        }
        // As forEachLeafBelow() goes through them.
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

// An Add that has a place of its own and has taken in the Shl below it: rule 7. One instruction makes it of its leaves, as it would make an address.
inline bool addsScaledIndex(const Liveness& liveness, const Operation* operation)
{
    if (operation->opcode() != Add || liveness.isFused(operation))
        return false;
    auto isFusedShift = [&](const Operation* child) {
        return child->opcode() == Shl && liveness.isFused(child);
    };
    return isFusedShift(operation->child(0)) || isFusedShift(operation->child(1));
}

// An Add is fused into an address and into nothing else. So is a Shl or an extension, but for rule 7.
inline bool isLeafOfAddress(const Liveness& liveness, const Operation* user, unsigned childIndex)
{
    Opcode opcode = user->opcode();
    if (isMemoryAccess(opcode))
        return childIndex + 1 == user->numChildren();
    if (liveness.isFused(user))
        return opcode == Add || opcode == Shl || opcode == ZExt32 || opcode == SExt32;
    return addsScaledIndex(liveness, user);
}

// Of a leaf without position. A leaf under an address stands in the instruction only as the base of the access itself or as the amount of a Shl.
inline bool standsInInstruction(const Liveness& liveness, const Operation* user, unsigned childIndex, const Operation* leaf)
{
    if (addsScaledIndex(liveness, user))
        return scaledSumAcceptsBase(*leaf);
    bool mayStandThere = !isLeafOfAddress(liveness, user, childIndex) || isMemoryAccess(user->opcode()) || (user->opcode() == Shl && childIndex == 1);
    return mayStandThere && acceptsImmediate(*user, childIndex, *leaf);
}

// The low half of the result is the low half of the child.
constexpr bool changesWidthOfInteger(Opcode opcode) { return opcode == SExt32 || opcode == ZExt32 || opcode == Trunc; }

// Whether the operation is no instruction where its result takes the register that its child lies in. The allocator prefers that register, and the encoder writes nothing where the result got it.
//  - A Trunc: the upper half of a register that holds an Int32 is unspecified.
//  - An extension whose upper half nothing reads. So an Int64 that goes round a loop and is only ever cut back to an Int32 costs what an Int32 would.
//  - A ZExt32 of a word whose upper half is clear already.
inline bool isFreeInRegisterOfChild(const Liveness& liveness, const Operation* operation)
{
    Opcode opcode = operation->opcode();
    if (!changesWidthOfInteger(opcode))
        return false;
    if (opcode == Trunc || !liveness.of(operation).has(ValueFlag::UpperHalfIsRead))
        return true;
    return opcode == ZExt32 && clearsUpperHalfOfRegister(operation->child(0));
}

// What is fused, and nothing else. In each case the fused operation has a use count of one and stands in the block of its user, but for what 1, 2 and 7 say of an earlier block.
//  1. A comparison into the Branch, Select or Check that tests it. It may stand in an earlier block where each of its children with a position is read again behind the test,
//     but for one at most, which is an integer and neither a narrow reference nor an inner pointer.
//  2. A BitAnd into a Branch, Select or Check that tests it, from an earlier block as in 1, or into an Equal or NotEqual with the constant zero that is fused by 1.
//     A BitAnd or a BitOr of two operations that give one or zero, each a comparison or a BitAnd with the constant one, into the Branch that tests it, and both with it:
//     the Branch tests twice and jumps behind each test. Each of the two takes in what it would by itself, as far as the leaves go.
//     On x86-64, below a BitAnd with the constant one that is fused into a Branch, the Patch that stands for a ZShr or an SShr by a register: the Branch tests the bit that the amount names.
//  3. Into the address of a load or a store, where isEncodableIndexedAddress() allows the outcome: Add(base, index), the index being any value, Shl(value, constant),
//     ZExt32(value), SExt32(value), or Shl of one of those two by a constant. The base is the first child of the Add and stays a leaf: a value, a pinned register or an AddressOf.
//     A Shl or an extension that the outcome has no room for stays a value.
//     A ZExt32 of a value of which clearsUpperHalfOfRegister() holds asks for no room: the register of the value is the index as it stands.
//     An access that traps is fused only as Add(cage base register, Shl(word, cell32Shift)), the word as it stands or below a ZExt32: the index is then the reference that the record names.
//     In every other case the base is, and no index stands beside it.
//  4. On x86-64, a Load that does not trap into the one user for which acceptsMemoryOperand() holds, at most one into a user, if the user stays and is no Branch, Select or Check,
//     at most maxOperationsBetweenLoadAndUser stand between them, and none of them is a Patch or has to stay.
//     And a load of an integer that does not trap into the Branch or Select that tests it, or into the comparison or BitAnd that 1 or 2 has fused into one, with the same between the load and the Branch or Select.
//     Of the two tests of one Branch one reads such a load.
//     Beside it stands a value or an immediate, and beside a load of 8 or 16 bits a constant with which one instruction tests the word where it lies.
//  5. A trapping load whose result nothing reads, into the first load or store behind it in its block that stays and goes through the same base operation,
//     if that access lies within trapReach() with its last byte, does not trap, has taken over no other trap and is fused into nothing,
//     at most maxOperationsBetweenLoadAndUser stand between them, and each of them that stays is pure or a load through a base without position.
//  6. A Trunc of a value with a position, unless it is a narrow reference, into a user that stays and that Encoder::encodeInteger() writes, into an IToD,
//     or into the comparison or BitAnd that 1 or 2 has fused into a Branch or a Check of its block: the instruction reads the low half of the register that holds the Int64.
//     So the Trunc is no instruction where the Int64 is read again behind it either, and its result could not take that register.
//  7. A Shl by a constant into the Int64 Add that reads it, whether the Add stays or sinks, where isEncodableScaledSum() allows the outcome, and below the Shl a ZExt32 or SExt32 as in 3.
//     Neither is a narrow reference. The other child of the Add is the base and stays a leaf: anything but a constant. Where scaledSumAcceptsBase() holds of it, it takes no register.
//     The Add keeps its place, its range and what the rules of the collector say of it: a decoded reference is the address of its cell. Nothing else is fused into it.
//     Into an Add that sinks they are fused from an earlier block too: they would have sunk behind it.
//  8. Nothing more is fused, but a leaf is another: where an arm of a Select is the ZExt32 or SExt32 of a word that the comparison or BitAnd fused into the Select reads, the test reads the arm in the place of the word.
//     Its instruction reads 32 bits, and the low half of an extension is its word. So the word ends at the extension, which may then be no instruction: isFreeInRegisterOfChild().
// What stands between two operations is counted in places of the block, whether or not it stays.
inline constexpr unsigned maxOperationsBetweenLoadAndUser = 4;

// What works in memory, where acceptsMemoryDestination() holds. Where updatesMemoryThroughScratchRegister() does, only TestedBehind for overflow, with no immediate of more than maxConstantAddedInScratchRegister either way and no least value further than that from the least int32.
// Nothing is marked as fused: the Store or the Branch has leaves of its own, and what nothing else reads is dropped like anything unread.
//  A Store of an Add, Sub, BitAnd, BitOr or BitXor of the word that a Load has read through the same base operation at the same offset, the Load below a SExt32 and the result below a Trunc where
//  a word of 32 bits is computed in 64. One way leads from the Load to the Store, through blocks with one predecessor each. At most maxOperationsBetweenLoadAndUpdate stand on it,
//  none of them a suspension, a Check or anything else that has to stay but what ends a block and a Patch that only keeps its children alive.
//   - Where the Store alone reads the result: MemoryUpdate::InPlace.
//   - Where the Branch that ends the block in front of the Store tests the sum and nothing else reads it, the Branch works in memory too:
//     ComparedAhead for a constant added in 64 bits, to a word of 32 bits or to a Load with OperationFlag::IsIntegerAddedTo, and compared in signed order,
//     TestedBehind where the test is that for overflow of 32 bits, with or without a least value, or compares a sum of 32 bits with a constant and fails on a rare edge,
//     LowHalf for a Load with OperationFlag::IsIntegerAddedTo, an amount of at most 32 bits and a test that every integer added to passes.
//     Where the Branch stores, no Load between it and the Store reads a heap that overlaps the heap of the Store, and no Patch there reads any.
inline constexpr unsigned maxOperationsBetweenLoadAndUpdate = 24;

// What goes by the overflow flag. Nothing is marked as fused here either: the sum and the Branch have leaves of their own.
//  A Branch on a comparison of an Int64 Add or Sub that stands in its block. Each operand of the sum is a SExt32 or a ZExt32 of which rangeOf() says that it is an int32, one of the two perhaps a constant.
//  The comparison holds, or fails, just where the sum is an int32, with or without a least value: sum `relation` bound, sum + bias `relation` bound, or sum == SExt32(Trunc(sum)).
//  The Branch alone reads the comparison, which alone reads the biased sum and the SExt32. Between the sum and the Branch stand these and Truncs of the sum, none of them a narrow reference.
//  So no instruction between the two writes the flags: a Trunc, a Move, a Reload, a Save and the store of a spilled value leave them alone on both targets.
//  Each successor has one predecessor and no Phi, so no move of an edge stands there.
//   - The sum stays, is never dropped and never sinks. Its register holds the low half, and the upper half is clear.
//   - Where a sum may be below zero, the upper half is read on the failing side alone: in what wayOutOfBranchThrough() says of the failing successor. The way there makes it, and stores the sum anew where it is spilled.
//     The Branch then reads the sum, in a register. It does so too where it compares the sum with a least value.
//   - Where the failing side alone reads an Int32 operand that lies in one register, nothing there reads the sum and no edge leads on from there, the allocator may give the sum that register:
//     Step::other of the Branch names the operand, the way to the failing successor takes the other operand off again, and that successor is entered with the operand in the register.
//     The other operand is an immediate or lies in a register that nothing takes ahead of the Branch.

Liveness* scanBackward(Procedure&);

// The pass itself. AOTMachineScan.cpp has the walk, dropping, uses, ranges, calls and pressure. The other parts are files of their own.
// Everything is public: a helper that this header does not declare is a static function of the file that needs it, takes BackwardScan&, and has a name that no other file of aot/ uses.
class BackwardScan {
    WTF_MAKE_NONCOPYABLE(BackwardScan);
public:
    BackwardScan(Procedure&, Liveness&);
    void run();

    // AOTMachineScan.cpp
    void scanBlock(Block*);
    // A read of `value` by an operation that stays. It revives a value that no read has reached yet.
    void addUse(Operation* value, Position, Reg hint = Reg());
    void define(Operation*, Position);
    void noteCall(const PatchOperation*, Position late, const Block*);
    void enterLoopsEndingAt(Position);
    void leaveLoopsBeginningAt(Position);
    void findBlocksThatRareBlocksBranchOff();
    ValueFacts& facts(const Operation* operation) { return m_liveness.m_values[operation->index()]; }
    ArenaVector<Use>& uses() { return m_liveness.m_uses; }
    uint8_t*& traitsOfUses() { return m_liveness.m_traitsOfUses; }
    // Liveness::lastCallIn() and nextCallBehindEndOf().
    ArenaVector<std::array<Position, numberOfBanks>>& lastCallsInBlocks() { return m_liveness.m_lastCallsInBlocks; }
    ArenaVector<std::array<Position, numberOfBanks>>& nextCallsBehindEnds() { return m_liveness.m_nextCallsBehindEnds; }
    void set(const Operation* operation, ValueFlag flag) { facts(operation).flags |= static_cast<uint16_t>(flag); }

    // AOTMachineFusion.cpp. Called for an operation that stays, ahead of the reads of its children.
    void fuseInto(Operation* user);
    bool tryFusePredicate(Operation* user, Operation* predicate);
    bool tryFuseAddress(Operation* access);
    bool tryFuseLoad(Operation* user, unsigned childIndex);
    // Also called for an operation that sinks.
    bool tryFuseScaledIndex(Operation* sum);
    // Called for a trapping load that nothing reads. The accesses behind it have been scanned.
    bool tryHandOverTrap(Operation* trappingLoad);
    unsigned numberOfLeavesOf(const Operation*) const;
    // Called for a Store, ahead of everything else. True: the Branch in front of its block stores in its place, and it is dropped.
    bool tryUpdateInMemory(Operation* store);
    // Called where the scan leaves the loop at its start: every Load of the loop has been seen.
    void settleLowHalfUpdatesOf(uint32_t loop);
    // Called for a Branch, ahead of everything else that could be fused into it. Whatever reads the sum behind the Branch has been scanned.
    bool tryTestSumByOverflow(Operation* branch);

    // AOTMachineSinking.cpp. Called where a pure operation is reached whose reads have all been seen.
    // It sinks if every read lies in one rare region other than its own, the region has one entry, and its children are still there to be read.
    bool trySink(Operation*);
    void noteRegionOfUse(Operation* value, const Block* user);

    // AOTMachineCallsOnWaysInScan.h.
    // Called where the scan enters a block at its end: Liveness::endOfWhatWaysLeadToFrom().
    void noteWaysOnFrom(const Block*, Position end);

    // AOTMachineReferences.cpp. Called at the definition of a value, when its last use is known.
    // An inner pointer reads its owner once more where it is last read itself. A narrow reference that lives across a safepoint needs its pointer.
    void applyCollectorRules(Operation*);
    // No step reads the value for the collector, so whoever goes by the steps asks Liveness::valuesKeptForCollector() as well. Only suspensions make anybody do so.
    void noteValueKeptForCollector(const Operation* value, Position until)
    {
        if (m_proc.traits().savesFrameOnSuspend)
            m_liveness.m_valuesKeptForCollector.append(m_proc.arena(), ValueKeptForCollector { value, until });
    }

    Procedure& m_proc;
    const Order& m_order;
    Liveness& m_liveness;
    // What facts() indexes, by Operation::index(), and Order::places(). The constructor makes the first list as long as it ever gets, so neither moves while the scan runs.
    ValueFacts* m_facts { nullptr };
    const BlockPlace* m_places { nullptr };
    RegisterMask m_callerSaved[numberOfBanks];
    bool m_hasLoops { false };
    bool m_hasRareRegions { false };

    // What the reads that stand in one block have in common.
    struct PlaceOfReads {
        bool isRare() const { return rareRegion != noRareRegion; }

        // Of the block. Neither, in a procedure without loops, for a read that no operation makes.
        uint32_t loop { noLoop };
        uint32_t rareRegion { noRareRegion };
        // Where there is a loop: its Loop::begin. What is defined in a block whose head does not stand ahead of it is defined ahead of no loop around the block: loops nest.
        Position beginOfLoop { 0 };
        // Use::headOfBlockPassedOnEveryPath.
        Position passedOnEveryPath { noPosition };
        // The order puts a rare region behind every block that leads to it, which may be far behind the block that this way into it branches off.
        // Of a rare block with Liveness::blockThatRareBlockBranchesOff(): where a read of what is defined outside the region counts for ValueFlag::LivesAcrossCall. noPosition of any other block.
        Position positionThatCountsForCalls { noPosition };
        // Where that is a position: the innermost loop around it.
        uint32_t loopThatCountsForCalls { noLoop };
        // There is a loop, and its head stands in the rare region of the block or, like the block, in none.
        // Only then does a read of the block list its value for the loops around it, and only then does the block count for the pressure of the loop.
        bool countsForLoops { false };
        // Liveness::useIsInRareRegion in a rare region, else nothing.
        uint8_t traitsOfUses { 0 };
    };
    // Of the block of the scan.
    PlaceOfReads m_placeOfReads;

    // No further on than any of the positions below, those of the rare regions among them. A range that ends ahead of it lives across none of them.
    Position m_nearestEventBehind { noPosition };
    // The nearest one behind the place of the scan, in code outside rare regions.
    Position m_nextCall[numberOfBanks] { noPosition, noPosition };
    // The same without the edges to landing pads, which take no register on the way on.
    std::array<Position, numberOfBanks> m_nextCallThatTakesRegisters { noPosition, noPosition };
    Position m_nextSafepoint { noPosition };
    Position m_nextSuspension { noPosition };
    // The head of the nearest landing pad behind the place of the scan, wherever it stands.
    Position m_nextLandingPad { noPosition };
    struct RegionState {
        Position nextCall[numberOfBanks] { noPosition, noPosition };
    };
    // By the index of the rare region.
    ArenaVector<RegionState> m_regions;
    static constexpr uint32_t severalRegions = UINT32_MAX - 1;
    struct SummaryOfReads {
        // The one rare region that has read it, noRareRegion before the first read, severalRegions behind a read from anywhere else.
        uint32_t regionOfUses { noRareRegion };
        // The end of the range as ValueFlag::LivesAcrossCall goes by it.
        Position lastReadCountedForCalls { 0 };
    };
    // By Operation::index(). Empty in a procedure without rare regions, where nothing sinks and the end of the range is ValueFacts::lastUse.
    ArenaVector<SummaryOfReads> m_summariesOfReads;
    // By Operation::index(): the outermost loop for which it has been counted as read inside.
    ArenaVector<uint32_t> m_loopCountedFor;
    struct LoopState {
        uint16_t mostLive[numberOfBanks] { 0, 0 };
        uint16_t outerValuesRead[numberOfBanks] { 0, 0 };
        RegisterMask registersTakenByPatches { 0 };
        ArenaVector<Operation*> valuesReadFromOutside;
        // The Phis of the head that are live only because something behind the loop reads them.
        uint16_t phisReadOnlyBehindLoop[numberOfBanks] { 0, 0 };
        Position endOfLastExitingBlock { 0 };
    };
    ArenaVector<LoopState> m_loopStates;
    // By Operation::index(): the innermost loop around the last read that has listed it in LoopState::valuesReadFromOutside.
    ArenaVector<uint32_t> m_loopOfLastListedRead;
    // By Operation::index(): counted in LoopState::phisReadOnlyBehindLoop.
    BitSpan m_isPhiReadOnlyBehindLoop;
    uint16_t m_numberLive[numberOfBanks] { 0, 0 };
    // Of the block of the scan: Liveness::mostLiveIn(), as far as its operations have been scanned.
    std::array<uint16_t, numberOfBanks> m_mostLiveInBlock { 0, 0 };

    static constexpr uint64_t bitOfWord(int32_t offset) { return uint64_t { 1 } << (static_cast<uint32_t>(offset) >> 3 & 63); }
    // By Operation::index() of a base: the words of 64 bits that a Load outside rare regions reads through it behind the place of the scan, by bitOfWord().
    // Two words may share a bit, which claims more and never less. Empty unless a word can be added to by its low half.
    ArenaVector<uint64_t> m_wordsReadWhole;
    struct LowHalfUpdate {
        Operation* branch;
        // Operation::index().
        uint32_t base;
        uint32_t loop;
        // In m_wordsReadWhole.
        uint64_t bit;
    };
    // Those in a loop that the scan has not left, those of the innermost loop last.
    ArenaVector<LowHalfUpdate> m_lowHalfUpdatesInLoops;
};

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT)
