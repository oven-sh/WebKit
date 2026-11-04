/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachineOrder.h"

#if ENABLE(AOT)

#include "AOTMachineFolding.h"
#include "AOTMachineTestsOfPhis.h"
#include <algorithm>
#include <bit>
#include <cstring>
#include <optional>
#include <type_traits>
#include <wtf/MathExtras.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT { namespace Machine {

namespace {

// Order::retarget() takes the edge class from the old target's edge to the new target, so each call skips one block.
// Two calls per edge are enough. A skipped block has no Upsilon, so its successor has no Phi and is not another threadable join.
// One that has Upsilons is skipped only by a block that leads to its successor by another edge too, and that edge is threaded by itself.
// An empty jump block is forwarded before any edge into it moves, so its successor is already the end of its chain.
class JumpThreader {
    WTF_MAKE_NONCOPYABLE(JumpThreader);
public:
    explicit JumpThreader(Procedure& procedure)
        : m_proc(procedure)
        , m_arena(procedure.arena())
    {
    }

    void run();

private:
    enum Flag : uint8_t {
        IsBeingForwarded = 1 << 0,
        IsForwarded = 1 << 1,
        // An entrypoint leads to the block. findLoopHeads() sets it and markUnreached() clears it.
        IsVisited = 1 << 2,
        IsOnPath = 1 << 3,
        IsLoopHead = 1 << 4,
        HasThreadedOwnJump = 1 << 5,
        // It is entered for its test alone: holdsNothingButItsTest().
        HoldsNothingButItsTest = 1 << 6,
        // Its edges may move, and one of them leads to a block with HoldsNothingButItsTest.
        MayBypassTest = 1 << 7,
    };

    // A block on the path of findLoopHeads() that the search has gone on from, and the edges that it has yet to follow.
    struct Visit {
        const Edge* next;
        const Edge* end;
        uint32_t index;
    };

    struct ThreadableJoin {
        const Operation* phi;
        bool isInverted;
        // The Branch goes by operations on the Phi and constants: holdsNothingButTestOfItsPhi().
        bool comparesPhi { false };
    };

    // What matchThreadableJoin() has found of a join.
    enum class MatchOfJoin : uint8_t {
        IsNotLookedAt,
        IsNotThreadable,
        TestsPhi,
        TestsInverseOfPhi,
        ComparesPhi,
    };

    // What two Branches ask is one test where it is one operation, or the same comparison or BitAnd of the same two operations: they compute one value.
    struct NumberedTest {
        uint64_t operands { 0 };
        uint32_t opcode { 0 };
        // From 1 on. 0: the place in the table is free.
        uint32_t number { 0 };
    };

    // What the Branch with two ways out asks that ends a block, in the words of NumberedTest.
    struct TestOfBranch {
        uint64_t operands;
        // Block::index().
        uint32_t block;
        uint8_t opcode;
        // The test is true on the second way out.
        bool isInverted;
    };
    static_assert(sizeof(TestOfBranch) == 16);

    // By Block::index(), of a block that ends in a Branch with two ways out.
    class TestOfBlock {
    public:
        TestOfBlock() = default;
        TestOfBlock(uint32_t number, bool isInverted)
            : m_bits(number << 1 | static_cast<uint32_t>(isInverted))
        {
        }

        // NumberedTest::number. 0: the block asks no test, or one that no other block asks.
        uint32_t number() const { return m_bits >> 1; }
        // The test is true on the second way out.
        bool isInverted() const { return m_bits & 1; }

    private:
        uint32_t m_bits { 0 };
    };

    enum class Answer : uint8_t { Unknown, IsTrue, IsFalse };

    // The block is left by `leavingSuccessor` where `subject relation constant` is what leavesWhenItHolds says.
    struct LeavingTest {
        const Operation* subject;
        Opcode relation;
        int64_t constant;
        bool leavesWhenItHolds;
        unsigned leavingSuccessor;
    };

    struct BlockOnWay {
        Block* block;
        uint32_t nextChild;
        // The test whose answer the edge into the block has brought. 0: none, or one that the way knew.
        uint32_t testAnswered;
    };

    bool has(const Block* block, uint8_t flags) const { return m_flags[block->index()] & flags; }
    void set(const Block* block, uint8_t flags) { m_flags[block->index()] |= flags; }
    void clear(const Block* block, uint8_t flags) { m_flags[block->index()] &= static_cast<uint8_t>(~flags); }

    // isComparison(), by the numbers of the opcodes. It also says of the operation that it is no Patch: its children lie in the operation itself.
    static constexpr bool isComparisonByNumber(Opcode opcode)
    {
        static_assert([] {
            for (unsigned number = 0; number < numberOfOpcodes; ++number) {
                if (isComparison(static_cast<Opcode>(number)) != (number >= Equal && number <= EqualOrUnordered))
                    return false;
            }
            return true;
        }());
        return opcode >= Equal && opcode <= EqualOrUnordered;
    }

    static ALWAYS_INLINE bool canRetargetSuccessors(const Block*);
    static ALWAYS_INLINE bool canGainPredecessor(const Block*);
    static ALWAYS_INLINE bool isEmptyJumpBlock(const Block*);
    static const Operation* incomingValue(const Operation* phi, const Block* from);
    static NEVER_INLINE bool handsPhisWhatSkippedBlockHandsThem(const Block* from, const Block* skipped, const Block* target);
    static ALWAYS_INLINE bool bringsWhatSkippedBlockBrings(const Block* from, const Block* skipped, const Block* target);
    static ALWAYS_INLINE SimplifiedPredicate simplifyPredicateOfBranch(Operation* predicate);
    static ALWAYS_INLINE Opcode lowerOfComparisonAndItsInverse(Opcode);

    NEVER_INLINE void findLoopHeads();
    ALWAYS_INLINE bool isLoopHead(const Block*);
    static ALWAYS_INLINE bool hasFormOfThreadableJoin(const Block* join);
    MatchOfJoin lookAtJoin(Block* join);
    std::optional<ThreadableJoin> matchThreadableJoin(Block* join);

    void retarget(Block* from, unsigned successorIndex, Block* target);
    void setEdgeClass(Block* from, unsigned successorIndex, EdgeClass);
    void markUnreached(Block*);

    void duplicateBranchIntoPredecessor(Block* from, Block* join, const ThreadableJoin&);
    void threadJumpsOfJoinsFrom(Block* join);
    ALWAYS_INLINE bool wouldThreadThrough(const Block* join) const;
    ALWAYS_INLINE bool mayThreadThrough(const Block* join);
    void threadThroughJoin(Block* from, unsigned successorIndex);
    ALWAYS_INLINE void skipEmptyJumpBlock(Block* from, unsigned successorIndex);
    ALWAYS_INLINE void skipForwardedBlock(Block* from, unsigned successorIndex, Block* skipped);
    ALWAYS_INLINE void forwardEmptyJumpChain(Block*);
    NEVER_INLINE void forwardEmptyJumpChainFrom(Block*);

    ALWAYS_INLINE void listTestOfBranch(uint32_t indexOfBlock, const Operation* branch);
    NEVER_INLINE bool numberTests();
    static bool holdsNothingButItsTest(const Block*);
    NEVER_INLINE void findBlocksThatMayBypassTests(BitSpan isWalked, uint32_t* firstBlocksBelow);
    void passTestsThatWayHasDecided(Block* from);
    NEVER_INLINE void threadEdgesPastDecidedTests();

    static std::optional<LeavingTest> describeTestLeavingBy(const Block*, unsigned leavingSuccessor);
    static bool leavesFor(const LeavingTest&, int64_t value);
    static bool secondLeavesForAllThatFirstLeavesFor(const LeavingTest& first, const LeavingTest& second);
    static bool mayRunForAnyValue(const Block*);
    static ALWAYS_INLINE bool mayAskTestBehindNext(const Block* first);
    static NEVER_INLINE void askTestBehindNextThatImpliesIt(Block* first);
    NEVER_INLINE void askTestsBehindNextThatImplyThem();

    // A join that passTestOfUpperHalfBy() has led an edge past.
    struct JoinPassedBy {
        Block* join;
        Block* passing;
        // The Phi of `passing` that was the Trunc of the Phi of the join. Null: nothing there reads the Phi of the join.
        Operation* lowerHalf;
    };
    static Operation* wordAddedTo(int64_t upperHalf, const Operation* sum);
    NEVER_INLINE void passTestOfUpperHalfBy(Block* from);

    Procedure& m_proc;
    Arena& m_arena;
    ArenaVector<uint8_t> m_flags;
    // By Block::index(), from findLoopHeads() on: the edges into the block from blocks with IsVisited. An edge that was threaded through a join still counts for the join.
    ArenaVector<uint32_t> m_reachedIncomingEdgeCounts;
    // By Block::index(), from the first join on that matchThreadableJoin() looks at.
    ArenaVector<MatchOfJoin> m_matchesOfJoins;
    ArenaVector<Block*> m_chain;
    ArenaVector<Block*> m_joinsThatJump;
    ArenaVector<Block*> m_unreachedBlocks;
    ArenaVector<JoinPassedBy> m_joinsPassedBy;
    bool m_hasFoundLoopHeads { false };
    // Block::index() of the block that run() has come to.
    uint32_t m_indexOfBlockInTurn { 0 };
    // Behind the loop of run(): one for each block that a Branch with two ways out ends. run() lists a block that it finds with a Branch.
    // A Jump that becomes a Branch when run() has come to its block or gone past it is listed by duplicateBranchIntoPredecessor().
    ArenaVector<TestOfBranch> m_testsOfBranches;
    // They stand by rising Block::index(). Only a Jump in a block that run() has gone past ends that.
    bool m_testsOfBranchesAreInOrder { true };
    // Beside m_testsOfBranches, from numberTests() on.
    TestOfBlock* m_numbersOfTestsOfBranches { nullptr };
    ArenaVector<TestOfBlock> m_testsOfBlocks;
    // By NumberedTest::number.
    ArenaVector<bool> m_testsAskedTwice;
    // By NumberedTest::number, on the way from a block that several edges enter down to the block that is being looked at.
    ArenaVector<Answer> m_answersOnWay;
};

// The allocator cannot place register moves before a terminal Patch, so Order puts a block on the edge of a Patch into a block with several predecessors.
// Moving such an edge past a jump block would mostly have Order put one back, so a Patch keeps its successors.
ALWAYS_INLINE bool JumpThreader::canRetargetSuccessors(const Block* block)
{
    if (block->isEmpty())
        return false;
    Opcode opcode = block->last()->opcode();
    return opcode == Jump || opcode == Branch || opcode == Switch;
}

// An edge to a landing pad counts as a call (ValueFlag::LivesAcrossCall), which would penalize every value live at the end of the block that gains the edge.
ALWAYS_INLINE bool JumpThreader::canGainPredecessor(const Block* target)
{
    return target->phis().empty() && !target->isLandingPad();
}

// The block may still have Upsilons. A landing pad is entered with every value in its slot, but its successor is not, so a landing pad is never skipped.
ALWAYS_INLINE bool JumpThreader::isEmptyJumpBlock(const Block* block)
{
    if (block->size() != 1 || block->last()->opcode() != Jump)
        return false;
    ASSERT(block->numSuccessors() == 1);
    return block->phis().empty() && !block->isLandingPad() && !block->isEntry();
}

// The last Upsilon wins, as in Allocator::incomingValueOf().
const Operation* JumpThreader::incomingValue(const Operation* phi, const Block* from)
{
    Operation* const* upsilons = from->upsilons().data();
    for (size_t index = from->upsilons().size(); index--;) {
        if (upsilons[index]->phi() == phi)
            return upsilons[index]->valueHandedOn();
    }
    return nullptr;
}

// simplifyPredicate() peels an Equal or a NotEqual with zero, a BitXor, a ZExt32, a SExt32 or a Trunc, and hands back any other operation as it is.
// Builder::branch() has peeled what most Branches test, so most predicates are not worth the call.
ALWAYS_INLINE SimplifiedPredicate JumpThreader::simplifyPredicateOfBranch(Operation* predicate)
{
    Opcode opcode = predicate->opcode();
    bool mayBePeeled = opcode == BitXor || opcode == ZExt32 || opcode == SExt32 || opcode == Trunc;
    if (opcode == Equal || opcode == NotEqual)
        mayBePeeled = predicate->child(1)->isInt(0);
    if (mayBePeeled) [[unlikely]]
        return simplifyPredicate(predicate);
    ASSERT(simplifyPredicate(predicate).predicate == predicate);
    return SimplifiedPredicate { predicate };
}

// One search marks the blocks that an entrypoint leads to, counts the edges into each block from those and finds the loop heads.
void JumpThreader::findLoopHeads()
{
    uint32_t numberOfBlocks = m_proc.numberOfBlocks();
    m_reachedIncomingEdgeCounts.fill(m_arena, numberOfBlocks, 0);
    uint32_t* counts = m_reachedIncomingEdgeCounts.begin();
    uint8_t* flags = m_flags.begin();
    // A block is on the path once at most, and the block that the search stands in is not listed.
    Visit* const path = m_arena.allocateArray<Visit>(numberOfBlocks);

    for (const Entrypoint& entrypoint : m_proc.entrypoints()) {
        uint32_t index = entrypoint.block->index();
        if (flags[index] & IsVisited)
            continue;
        flags[index] |= IsVisited | IsOnPath;
        std::span<const Edge> successors = entrypoint.block->successors();
        const Edge* next = successors.data();
        const Edge* end = next + successors.size();
        Visit* endOfPath = path;
        for (;;) {
            if (next == end) {
                flags[index] &= static_cast<uint8_t>(~IsOnPath);
                if (endOfPath == path)
                    break;
                --endOfPath;
                next = endOfPath->next;
                end = endOfPath->end;
                index = endOfPath->index;
                continue;
            }
            const Block* successor = next++->block();
            uint32_t indexOfSuccessor = successor->index();
            ++counts[indexOfSuccessor];
            uint8_t flagsOfSuccessor = flags[indexOfSuccessor];
            if (flagsOfSuccessor & IsVisited) {
                if (flagsOfSuccessor & IsOnPath)
                    flags[indexOfSuccessor] = static_cast<uint8_t>(flagsOfSuccessor | IsLoopHead);
                continue;
            }
            flags[indexOfSuccessor] = static_cast<uint8_t>(flagsOfSuccessor | IsVisited | IsOnPath);
            *endOfPath++ = Visit { next, end, index };
            index = indexOfSuccessor;
            successors = successor->successors();
            next = successors.data();
            end = next + successors.size();
        }
    }
}

// Computed on first use, because most procedures have no threadable join.
// Retargeting adds no cycle, and an edge into a block with a Phi only from a block that leads there already. So it never makes a join a loop head and the result stays valid as edges move.
ALWAYS_INLINE bool JumpThreader::isLoopHead(const Block* block)
{
    if (!m_hasFoundLoopHeads) [[unlikely]] {
        m_hasFoundLoopHeads = true;
        findLoopHeads();
    }
    return has(block, IsLoopHead);
}

// What matchThreadableJoin() asks of the block, of what ends it and of its Phi, ahead of everything else.
ALWAYS_INLINE bool JumpThreader::hasFormOfThreadableJoin(const Block* join)
{
    if (join->phis().size() != 1 || !join->upsilons().empty() || join->isEmpty() || join->isLandingPad())
        return false;
    Opcode opcode = join->last()->opcode();
    return (opcode == Branch || opcode == Switch) && join->phis().data()[0]->useCount() == 1;
}

// hasFormOfThreadableJoin() holds of the join.
JumpThreader::MatchOfJoin JumpThreader::lookAtJoin(Block* join)
{
    Operation* branch = join->last();
    Operation* phi = join->phis().data()[0];
    ASSERT(branch->opcode() == Switch || join->numSuccessors() == 2);

    // A Switch compares the value as it is, so nothing is peeled off it.
    SimplifiedPredicate simplified = branch->opcode() == Branch ? simplifyPredicateOfBranch(branch->child(0)) : SimplifiedPredicate { branch->child(0), false };
    if (simplified.predicate != phi) {
        if (branch->opcode() != Branch)
            return MatchOfJoin::IsNotThreadable;
        // holdsNothingButTestOfItsPhi() asks this too, behind reckoning the test. Most joins do work of their own, and the first operation says so.
        for (const Operation* operation : join->operations()) {
            if (operation != branch && (operation->useCount() > 1 || operation->mustStay()))
                return MatchOfJoin::IsNotThreadable;
        }
        if (!holdsNothingButTestOfItsPhi(join, phi) || isLoopHead(join))
            return MatchOfJoin::IsNotThreadable;
        return MatchOfJoin::ComparesPhi;
    }

    // simplifyPredicate() only peels child(0). Every peeled operation must be in the join and feed only the Branch.
    unsigned peeledOperationCount = 0;
    for (const Operation* operation = branch->child(0); operation != phi; operation = operation->child(0)) {
        if (operation->owner() != join || operation->useCount() != 1 || operation->mustStay() || !operation->numChildren())
            return MatchOfJoin::IsNotThreadable;
        ++peeledOperationCount;
    }

    // The scan drops an operation that nothing reads and that need not stay, such as a wrapper that Builder::branch() has already peeled.
    unsigned keptOperationCount = 0;
    for (const Operation* operation : join->operations()) {
        if (!operation->useCount() && !operation->mustStay())
            continue;
        if (++keptOperationCount > peeledOperationCount + 1)
            return MatchOfJoin::IsNotThreadable;
    }
    ASSERT(keptOperationCount == peeledOperationCount + 1);

    if (isLoopHead(join))
        return MatchOfJoin::IsNotThreadable;
    return simplified.isInverted ? MatchOfJoin::TestsInverseOfPhi : MatchOfJoin::TestsPhi;
}

// Each edge into a join asks, and the answer is found once. A join that has the form keeps it: no block gains an Upsilon, and what ends the join stays a Branch or a Switch until every edge has been threaded.
// All else that lookAtJoin() goes by stays as it is too: the operations of the join, how often each is read, and which blocks are loop heads once they are found.
std::optional<JumpThreader::ThreadableJoin> JumpThreader::matchThreadableJoin(Block* join)
{
    if (!hasFormOfThreadableJoin(join))
        return std::nullopt;
    if (m_matchesOfJoins.isEmpty()) [[unlikely]]
        m_matchesOfJoins.fill(m_arena, m_proc.numberOfBlocks(), MatchOfJoin::IsNotLookedAt);
    MatchOfJoin match = m_matchesOfJoins[join->index()];
    if (match == MatchOfJoin::IsNotLookedAt) {
        match = lookAtJoin(join);
        m_matchesOfJoins[join->index()] = match;
    }
    if (match == MatchOfJoin::IsNotThreadable)
        return std::nullopt;
    return ThreadableJoin { join->phis().data()[0], match == MatchOfJoin::TestsInverseOfPhi, match == MatchOfJoin::ComparesPhi };
}

void JumpThreader::retarget(Block* from, unsigned successorIndex, Block* target)
{
    Order::retarget(from, successorIndex, target);
    if (has(from, IsVisited))
        ++m_reachedIncomingEdgeCounts[target->index()];
}

// Block has no way to change one edge, so the successors are appended again in their order.
void JumpThreader::setEdgeClass(Block* from, unsigned successorIndex, EdgeClass edgeClass)
{
    if (from->successor(successorIndex).edgeClass() == edgeClass)
        return;
    std::span<Edge> successors = m_arena.copySpan<Edge>(from->successors());
    successors[successorIndex] = Edge(successors[successorIndex].block(), edgeClass);
    from->clearSuccessors();
    for (const Edge& edge : successors)
        from->appendSuccessor(edge);
}

// Whatever the block alone led to is no longer reached either.
// The blocks of a cycle keep each other marked. That only costs an edge the class that threadThroughJoin() would have kept.
void JumpThreader::markUnreached(Block* block)
{
    ASSERT(has(block, IsVisited) && m_unreachedBlocks.isEmpty());
    clear(block, IsVisited);
    m_unreachedBlocks.append(m_arena, block);
    while (!m_unreachedBlocks.isEmpty()) {
        for (const Edge& edge : m_unreachedBlocks.takeLast()->successors()) {
            Block* successor = edge.block();
            if (!has(successor, IsVisited) || --m_reachedIncomingEdgeCounts[successor->index()])
                continue;
            clear(successor, IsVisited);
            m_unreachedBlocks.append(m_arena, successor);
        }
    }
}

// `from` is reached and hands the Phi a value that is no constant. Where a Jump ends it, it tests the value itself: what computes the value is then fused into the test,
// where it had to be made a word, carried to the join and tested there.
void JumpThreader::duplicateBranchIntoPredecessor(Block* from, Block* join, const ThreadableJoin& threadable)
{
    if (from->last()->opcode() != Jump || join->last()->opcode() != Branch)
        return;
    for (const Edge& edge : join->successors()) {
        // The join has no Upsilon, so its successors have no Phi.
        ASSERT(edge.block()->phis().empty());
        if (!canGainPredecessor(edge.block()))
            return;
    }

    Order::replaceJumpWithBranch(from, threadable.phi, threadable.isInverted);
    for (const Edge& edge : from->successors())
        ++m_reachedIncomingEdgeCounts[edge.block()->index()];
    // run() has yet to come to any other block, and will find the Branch there.
    if (from->index() > m_indexOfBlockInTurn)
        return;
    if (!m_testsOfBranches.isEmpty() && m_testsOfBranches.last().block > from->index())
        m_testsOfBranchesAreInOrder = false;
    listTestOfBranch(from->index(), from->last());
}

// A join that hands its Phi on to a second join ends in a Branch on the Phi once its own Jump is threaded, and only then can the edges into it be threaded.
// So its Jump goes first, whatever the indexes of the blocks, and the last join of a chain ahead of the others. Each call below finds a marked block or the end of the chain, so it lists nothing.
void JumpThreader::threadJumpsOfJoinsFrom(Block* join)
{
    size_t numberListedByCaller = m_joinsThatJump.size();
    for (Block* block = join; !block->isEmpty() && block->last()->opcode() == Jump && block->phis().size() == 1 && !has(block, HasThreadedOwnJump); block = block->successor(0).block()) {
        set(block, HasThreadedOwnJump);
        m_joinsThatJump.append(m_arena, block);
    }
    while (m_joinsThatJump.size() > numberListedByCaller) {
        Block* block = m_joinsThatJump.takeLast();
        if (mayThreadThrough(block->successor(0).block()))
            threadThroughJoin(block, 0);
    }
}

// False: threadThroughJoin() would leave an edge to the block as it is, mark nothing, and would not come to ask for the loop heads, which are found as the edges lie at the first question.
// A join whose own Jump goes first has one Phi, and its Jump has not had its turn. Nothing is threaded through a block that a Jump ends.
ALWAYS_INLINE bool JumpThreader::wouldThreadThrough(const Block* join) const
{
    if (join->phis().size() != 1 || join->isEmpty())
        return false;
    if (join->last()->opcode() == Jump)
        return !has(join, HasThreadedOwnJump);
    if (!hasFormOfThreadableJoin(join))
        return false;
    return m_matchesOfJoins.isEmpty() || m_matchesOfJoins[join->index()] != MatchOfJoin::IsNotThreadable;
}

// False: nothing is left for threadThroughJoin() to do with an edge to the block.
// Most joins that a Jump ends lead to a block through which nothing is threaded. Of such a join threadJumpsOfJoinsFrom() lists no other, marks it and leaves its Jump, and a Jump still ends it: the mark is all that comes of the call.
ALWAYS_INLINE bool JumpThreader::mayThreadThrough(const Block* join)
{
    if (!wouldThreadThrough(join))
        return false;
    if (join->last()->opcode() != Jump || wouldThreadThrough(join->successor(0).block()))
        return true;
    set(join, HasThreadedOwnJump);
    return false;
}

void JumpThreader::threadThroughJoin(Block* from, unsigned successorIndex)
{
    threadJumpsOfJoinsFrom(from->successor(successorIndex).block());
    Block* join = from->successor(successorIndex).block();
    std::optional<ThreadableJoin> threadable = matchThreadableJoin(join);
    if (!threadable || !has(from, IsVisited))
        return;
    const Operation* value = incomingValue(threadable->phi, from);
    if (!value)
        return;
    if (!value->hasInt()) {
        if (!threadable->comparesPhi)
            duplicateBranchIntoPredecessor(from, join, *threadable);
        return;
    }

    unsigned chosenSuccessor;
    if (join->last()->opcode() == Switch) {
        std::span<const int64_t> caseValues = join->last()->switchData().caseValues;
        auto matching = std::ranges::lower_bound(caseValues, static_cast<int64_t>(value->asInt()));
        bool hasCase = matching != caseValues.end() && *matching == value->asInt();
        chosenSuccessor = static_cast<unsigned>(hasCase ? matching - caseValues.begin() : caseValues.size());
    } else {
        std::optional<int64_t> tested = threadable->comparesPhi ? valueOfTestWherePhiHolds(join->last()->child(0), threadable->phi, value->asInt()) : std::optional<int64_t>(value->asInt());
        if (!tested)
            return;
        bool isNotZero = *tested;
        chosenSuccessor = isNotZero != threadable->isInverted ? 0 : 1;
    }
    Block* target = join->successor(chosenSuccessor).block();
    // The join has no Upsilon, so its successors have no Phi.
    ASSERT(target->phis().empty());
    if (!canGainPredecessor(target))
        return;

    // Where no other reached block leads to the join, its Branch always goes this way, so the class of this way says nothing and the edge keeps its own.
    // Builder::branch() drops the class of the way that a constant chooses for the same reason. Otherwise everything behind a rare way would be one rare region on the only path that runs.
    bool isOnlyReachedEdge = m_reachedIncomingEdgeCounts[join->index()] == 1;
    EdgeClass edgeClass = from->successor(successorIndex).edgeClass();
    retarget(from, successorIndex, target);
    if (!isOnlyReachedEdge)
        return;
    setEdgeClass(from, successorIndex, edgeClass);
    markUnreached(join);
}

// ---- `node.kind === 80`, where the read is a guess: its typed arm has an int32 and adds the tag to it, to meet what the stub of the closed arm hands back, and the join asks the word for the tag and takes the int32 out again.
// In the words of this file: C has a clear lower half. C + ZExt32(x) does not wrap, so it is no less than C, and its lower half is x. The arm goes straight to where the test passes, and hands x over.

Operation* JumpThreader::wordAddedTo(int64_t upperHalf, const Operation* sum)
{
    if (sum->opcode() != Add || !sum->owner())
        return nullptr;
    const Operation* extension = sum->child(0);
    if (!sum->child(1)->isInt64(upperHalf)) {
        if (!extension->isInt64(upperHalf))
            return nullptr;
        extension = sum->child(1);
    }
    return extension->opcode() == ZExt32 ? extension->child(0) : nullptr;
}

// A Jump ends `from`.
// The join holds its one Phi, `Phi >= C` or `Phi < C`, and the Branch on that. Beside the test the Phi is read by one Trunc at most in the block where the test passes, which nothing else enters,
// and in the block where it fails, which then nothing else enters either: every read is found, so none lies where the way that passes the join by comes to, on which the Phi is missing.
void JumpThreader::passTestOfUpperHalfBy(Block* from)
{
    ASSERT(from->last()->opcode() == Jump && from->numSuccessors() == 1);
    Block* join = from->successor(0).block();
    if (join->phis().size() != 1 || join->isEmpty() || join->last()->opcode() != Branch || join->numSuccessors() != 2 || join->isLandingPad())
        return;
    Operation* phi = join->phis().data()[0];
    const Operation* handed = incomingValue(phi, from);
    if (!handed || handed->opcode() != Add || phi->type() != Int64)
        return;
    const Operation* branch = join->last();
    const Operation* test = branch->child(0);
    if ((test->opcode() != AboveEqual && test->opcode() != Below) || test->child(0) != phi || !test->child(1)->hasInt64())
        return;
    int64_t bound = test->child(1)->asInt64();
    if (static_cast<uint32_t>(bound))
        return;
    Operation* word = wordAddedTo(bound, handed);
    if (!word)
        return;

    for (size_t index = m_joinsPassedBy.size(); index--;) {
        const JoinPassedBy& known = m_joinsPassedBy[index];
        if (known.join != join)
            continue;
        if (!has(from, IsVisited))
            return;
        Order::passJoinBy(from, known.passing, known.lowerHalf, known.lowerHalf ? word : nullptr);
        ++m_reachedIncomingEdgeCounts[known.passing->index()];
        return;
    }

    unsigned passingSuccessor = test->opcode() == AboveEqual ? 0 : 1;
    Block* passing = join->successor(passingSuccessor).block();
    const Block* failing = join->successor(1 - passingSuccessor).block();
    if (passing == failing || passing == join || failing == join || passing->isLandingPad() || passing->isEntry())
        return;
    if (!join->upsilons().empty() || !passing->phis().empty() || passing->numberOfIncomingEdges() != 1)
        return;
    if (test->owner() != join || test->useCount() != 1)
        return;
    for (const Operation* operation : join->operations()) {
        if (operation != branch && operation != test && (operation->useCount() || operation->mustStay()))
            return;
    }

    auto readsCounted = [&](const Operation* operation) {
        // A pure operation that nothing reads has counted nothing.
        if (countsChildrenAtFirstUse(operation->opcode()) && !operation->useCount())
            return 0u;
        unsigned reads = 0;
        for (const Operation* child : operation->children())
            reads += child == phi;
        return reads;
    };
    unsigned reads = 1;
    Operation* truncation = nullptr;
    for (Operation* operation : passing->operations()) {
        if (!readsCounted(operation))
            continue;
        if (operation->opcode() != Trunc || truncation)
            return;
        truncation = operation;
        ++reads;
    }
    for (const Operation* upsilon : passing->upsilons()) {
        if (upsilon->child(0) == phi)
            return;
    }
    unsigned readsAheadOfFailingSide = reads;
    for (const Operation* operation : failing->operations())
        reads += readsCounted(operation);
    for (const Operation* upsilon : failing->upsilons())
        reads += upsilon->child(0) == phi;
    if (reads != phi->useCount())
        return;
    // `list[i]`: whatever fails behind the test of the tag, the bound or the hole, goes where the tag's failing goes, with the index in its box.
    if (reads != readsAheadOfFailingSide && failing->numberOfIncomingEdges() != 1)
        return;
    if (isLoopHead(join) || !has(from, IsVisited))
        return;

    if (truncation)
        Order::makePhiOfTruncationBehindJoin(join, truncation);
    m_joinsPassedBy.append(m_arena, JoinPassedBy { join, passing, truncation });
    Order::passJoinBy(from, passing, truncation, truncation ? word : nullptr);
    ++m_reachedIncomingEdgeCounts[passing->index()];
}

// `from` has Upsilons for the Phis of the target. They hand each Phi what those of the skipped block hand it.
bool JumpThreader::handsPhisWhatSkippedBlockHandsThem(const Block* from, const Block* skipped, const Block* target)
{
    for (const Operation* phi : target->phis()) {
        const Operation* brought = incomingValue(phi, from);
        if (!brought || brought != incomingValue(phi, skipped))
            return false;
    }
    return true;
}

// Another edge of `from` leads to the target, which few blocks have, so `from` has Upsilons for the Phis there.
// A block that a Switch ends has an Upsilon for a Phi only where one of its edges leads to the block of the Phi, so the Upsilons answer, and nobody goes through its edges once for each of them.
ALWAYS_INLINE bool JumpThreader::bringsWhatSkippedBlockBrings(const Block* from, const Block* skipped, const Block* target)
{
    if (from->last()->opcode() == Switch && !target->phis().empty())
        return !target->isLandingPad() && handsPhisWhatSkippedBlockHandsThem(from, skipped, target);
    const Edge* edge = from->successors().data();
    const Edge* end = edge + from->numSuccessors();
    while (edge != end && edge->block() != target)
        ++edge;
    if (edge == end || target->isLandingPad())
        return false;
    return handsPhisWhatSkippedBlockHandsThem(from, skipped, target);
}

ALWAYS_INLINE void JumpThreader::skipEmptyJumpBlock(Block* from, unsigned successorIndex)
{
    Block* skipped = from->successor(successorIndex).block();
    if (isEmptyJumpBlock(skipped) && has(skipped, IsForwarded))
        skipForwardedBlock(from, successorIndex, skipped);
}

// The edge leads to `skipped`, an empty jump block with IsForwarded.
ALWAYS_INLINE void JumpThreader::skipForwardedBlock(Block* from, unsigned successorIndex, Block* skipped)
{
    ASSERT(from->successor(successorIndex).block() == skipped && isEmptyJumpBlock(skipped) && has(skipped, IsForwarded));
    Block* target = skipped->successor(0).block();
    if (target == skipped)
        return;
    if (skipped->upsilons().empty() ? !canGainPredecessor(target) : !bringsWhatSkippedBlockBrings(from, skipped, target))
        return;
    Order::retarget(from, successorIndex, target);
    if (!has(from, IsVisited))
        return;
    uint32_t* counts = m_reachedIncomingEdgeCounts.begin();
    ++counts[target->index()];
    if (--counts[skipped->index()])
        return;
    // markUnreached(skipped), whose one edge leads to the target.
    ASSERT(has(skipped, IsVisited));
    clear(skipped, IsVisited);
    if (has(target, IsVisited) && !--counts[target->index()])
        markUnreached(target);
}

// Of an empty jump block. Most lead to a block that is neither threaded through nor skipped: such a chain is one block long, and its edge stays.
ALWAYS_INLINE void JumpThreader::forwardEmptyJumpChain(Block* first)
{
    ASSERT(isEmptyJumpBlock(first));
    if (has(first, IsBeingForwarded | IsForwarded))
        return;
    const Block* next = first->successor(0).block();
    if (mayThreadThrough(next) || isEmptyJumpBlock(next)) {
        forwardEmptyJumpChainFrom(first);
        return;
    }
    set(first, IsBeingForwarded | IsForwarded);
}

// Forwards the last block of the chain first, so each block is visited once.
// A block's Upsilons are final only once it is forwarded, because threading its edge through a join removes them.
// A cycle of empty jump blocks is an infinite loop in the program. The walk stops at a block it has already marked, which leaves a cycle.
void JumpThreader::forwardEmptyJumpChainFrom(Block* first)
{
    ASSERT(m_chain.isEmpty());
    Block* block = first;
    for (;;) {
        set(block, IsBeingForwarded);
        if (mayThreadThrough(block->successor(0).block()))
            threadThroughJoin(block, 0);
        Block* next = block->successor(0).block();
        if (!isEmptyJumpBlock(next) || has(next, IsBeingForwarded | IsForwarded))
            break;
        m_chain.append(m_arena, block);
        block = next;
    }
    for (;;) {
        skipEmptyJumpBlock(block, 0);
        set(block, IsForwarded);
        if (m_chain.isEmpty())
            break;
        block = m_chain.takeLast();
    }
}

// ---- `kind === 243 || kind === 244`, where each comparison asks first whether `kind` is an int32: the way on which it is one goes on to the next comparison of integers, and the other way to the next slow one.

// Of a comparison and the one that says the opposite of integers: invertedComparison(). Any other opcode is handed back.
ALWAYS_INLINE Opcode JumpThreader::lowerOfComparisonAndItsInverse(Opcode opcode)
{
    static_assert(Equal < NotEqual && LessThan < GreaterEqual && GreaterThan < LessEqual && Above < BelowEqual && Below < AboveEqual);
    Opcode lower = opcode;
    switch (opcode) {
    case NotEqual:
        lower = Equal;
        break;
    case GreaterEqual:
        lower = LessThan;
        break;
    case LessEqual:
        lower = GreaterThan;
        break;
    case BelowEqual:
        lower = Above;
        break;
    case AboveEqual:
        lower = Below;
        break;
    default:
        break;
    }
    ASSERT(!isComparison(opcode) || opcode == EqualOrUnordered || lower == std::min(opcode, invertedComparison(opcode)));
    return lower;
}

// The Branch, which has two ways out, ends the block. What it asks is read while the block is at hand: no Branch comes to ask anything else.
ALWAYS_INLINE void JumpThreader::listTestOfBranch(uint32_t indexOfBlock, const Operation* branch)
{
    SimplifiedPredicate simplified = simplifyPredicateOfBranch(branch->child(0));
    const Operation* test = simplified.predicate;
    Opcode opcode = test->opcode();
    uint64_t operands = test->index();
    uint8_t opcodeOfTest = 0;
    if (isComparisonByNumber(opcode) || opcode == BitAnd) {
        Operation* const* children = test->children().data();
        // Of the two comparisons of integers that say the opposite of each other, the one with the lower number stands for both.
        Opcode lower = lowerOfComparisonAndItsInverse(opcode);
        if (lower != opcode && children[0]->type().isInt()) {
            opcode = lower;
            simplified.isInverted = !simplified.isInverted;
        }
        operands = static_cast<uint64_t>(children[0]->index()) << 32 | children[1]->index();
        opcodeOfTest = static_cast<uint8_t>(opcode + 1);
    }
    new (NotNull, m_testsOfBranches.appendUninitialized(m_arena)) TestOfBranch { operands, indexOfBlock, opcodeOfTest, simplified.isInverted };
}

// False: no two Branches ask one test, so no way has decided what it comes to.
bool JumpThreader::numberTests()
{
    uint32_t numberOfBranches = m_testsOfBranches.size();
    if (numberOfBranches < 2)
        return false;

    static_assert(std::is_trivially_copyable_v<NumberedTest>);
    uint32_t numberOfPlaces = WTF::roundUpToPowerOfTwo(numberOfBranches * 2);
    uint32_t mask = numberOfPlaces - 1;
    unsigned shift = 64 - static_cast<unsigned>(std::countr_zero(numberOfPlaces));
    NumberedTest* table = m_arena.allocateArray<NumberedTest>(numberOfPlaces);
    std::memset(static_cast<void*>(table), 0, static_cast<size_t>(numberOfPlaces) * sizeof(NumberedTest));
    m_testsAskedTwice.fill(m_arena, numberOfBranches + 1, false);
    bool* testsAskedTwice = m_testsAskedTwice.begin();
    TestOfBlock* numbers = m_arena.allocateArray<TestOfBlock>(numberOfBranches);
    m_numbersOfTestsOfBranches = numbers;
    const TestOfBranch* tests = m_testsOfBranches.begin();
    uint32_t numberOfTests = 0;
    bool isSomeTestAskedTwice = false;
    for (uint32_t branch = 0; branch < numberOfBranches; ++branch) {
        uint64_t operands = tests[branch].operands;
        uint32_t opcode = tests[branch].opcode;
        uint32_t place = static_cast<uint32_t>((operands ^ static_cast<uint64_t>(opcode) << 27) * 0x9e3779b97f4a7c15ull >> shift);
        uint32_t number;
        for (;;) {
            NumberedTest& held = table[place];
            number = held.number;
            if (!number) {
                number = ++numberOfTests;
                held.operands = operands;
                held.opcode = opcode;
                held.number = number;
                break;
            }
            if (held.operands == operands && held.opcode == opcode) {
                testsAskedTwice[number] = true;
                isSomeTestAskedTwice = true;
                break;
            }
            place = (place + 1) & mask;
        }
        numbers[branch] = TestOfBlock(number, tests[branch].isInverted);
    }
    if (!isSomeTestAskedTwice)
        return false;
    m_answersOnWay.fill(m_arena, numberOfTests + 1, Answer::Unknown);
    return true;
}

// Whoever passes it by misses nothing: all that stays in it is the Branch and what the Branch alone reads.
bool JumpThreader::holdsNothingButItsTest(const Block* block)
{
    if (!block->phis().empty() || !block->upsilons().empty() || block->isLandingPad() || block->isEntry())
        return false;
    // Each operation that stays, but for the Branch, is read once. Where as many reads by operations that stay find their child in the block, each is read by another that stays in the block.
    // No operation reads itself through others, so each way from reader to reader ends at the Branch. A reader outside the block, or one that the scan drops, would leave fewer such reads.
    const Operation* branch = block->last();
    uint32_t numberThatStay = 0;
    uint32_t numberOfReadsWithinBlock = 0;
    for (const Operation* operation : block->operations()) {
        if (operation != branch) {
            unsigned useCount = operation->useCount();
            bool mustStay = operation->mustStay();
            if (!useCount && !mustStay)
                continue;
            if (useCount != 1 || mustStay)
                return false;
        }
        ++numberThatStay;
        for (const Operation* child : operation->children())
            numberOfReadsWithinBlock += child->owner() == block && child->opcode() != Phi;
    }
    return numberOfReadsWithinBlock + 1 == numberThatStay;
}

// m_answersOnWay holds what was decided on the way to the end of `from`.
void JumpThreader::passTestsThatWayHasDecided(Block* from)
{
    if (!canRetargetSuccessors(from))
        return;
    TestOfBlock own = m_testsOfBlocks[from->index()];
    for (unsigned index = 0; index < from->numSuccessors(); ++index) {
        for (;;) {
            Block* passed = from->successor(index).block();
            TestOfBlock test = m_testsOfBlocks[passed->index()];
            if (!test.number() || !has(passed, HoldsNothingButItsTest))
                break;
            Answer answer = m_answersOnWay[test.number()];
            if (answer == Answer::Unknown && own.number() == test.number())
                answer = !index != own.isInverted() ? Answer::IsTrue : Answer::IsFalse;
            if (answer == Answer::Unknown)
                break;
            Block* target = passed->successor((answer == Answer::IsTrue) != test.isInverted() ? 0 : 1).block();
            if (target == passed || !canGainPredecessor(target))
                break;
            retarget(from, index, target);
            if (!--m_reachedIncomingEdgeCounts[passed->index()])
                markUnreached(passed);
        }
    }
}

// No edge moves but one that leads to a block with HoldsNothingButItsTest, and its block moves it by what the blocks above know. Only the block itself moves its edges, so which blocks have such an edge is settled here.
// The walk goes through those blocks and the blocks above them alone, in the order in which a walk through every block would come to them: whatever else that walk enters it leaves as it found it.
void JumpThreader::findBlocksThatMayBypassTests(BitSpan isWalked, uint32_t* firstBlocksBelow)
{
    uint32_t numberOfBlocks = m_proc.numberOfBlocks();
    Block* const* blocks = m_proc.blocks().data();
    uint8_t* flags = m_flags.begin();
    for (uint32_t index = 0; index < numberOfBlocks; ++index) {
        if (!(flags[index] & IsVisited))
            continue;
        const Block* block = blocks[index];
        const Edge* edge = block->successors().data();
        const Edge* end = edge + block->numSuccessors();
        while (edge != end && !(flags[edge->block()->index()] & HoldsNothingButItsTest))
            ++edge;
        if (edge == end || !canRetargetSuccessors(block))
            continue;
        flags[index] |= MayBypassTest;
        for (const Block* walked = block; walked && (flags[walked->index()] & IsVisited) && !isWalked.get(walked->index()); walked = walked->singlePredecessor()) {
            isWalked.set(walked->index());
            firstBlocksBelow[walked->index()] = 0;
        }
    }
}

// A block that one edge enters stands below the block that the edge comes out of, and knows what that one knows. The blocks below a block are looked at first:
// so an edge that passes a test by finds the way out of it where it will stay.
void JumpThreader::threadEdgesPastDecidedTests()
{
    if (!numberTests())
        return;

    uint32_t numberOfBlocks = m_proc.numberOfBlocks();
    Block* const* blocks = m_proc.blocks().data();
    uint8_t* flags = m_flags.begin();

    // Nobody looks for the answer to a test that one block alone asks, so such a test is not entered.
    m_testsOfBlocks.fill(m_arena, numberOfBlocks, TestOfBlock());
    TestOfBlock* testsOfBlocks = m_testsOfBlocks.begin();
    const TestOfBranch* tests = m_testsOfBranches.begin();
    const TestOfBlock* numbers = m_numbersOfTestsOfBranches;
    const bool* testsAskedTwice = m_testsAskedTwice.begin();
    bool holdsSuchBlock = false;
    for (uint32_t branch = 0, numberOfBranches = m_testsOfBranches.size(); branch < numberOfBranches; ++branch) {
        TestOfBlock test = numbers[branch];
        if (!testsAskedTwice[test.number()])
            continue;
        uint32_t index = tests[branch].block;
        testsOfBlocks[index] = test;
        Block* block = blocks[index];
        if (!holdsNothingButItsTest(block) || isLoopHead(block) || !(flags[index] & IsVisited))
            continue;
        flags[index] |= HoldsNothingButItsTest;
        holdsSuchBlock = true;
    }
    if (!holdsSuchBlock)
        return;

    BitSpan isWalked = BitSpan::create(m_arena, numberOfBlocks);
    // Who stands below whom is settled before any edge moves. An edge that comes to pass a test by brings the arm what the test would have told it, so what an arm knows holds however it is entered in the end.
    // By Block::index(), of a block that is walked, each an index of a block and one more. 0: none.
    uint32_t* firstBlocksBelow = m_arena.allocateArray<uint32_t>(numberOfBlocks);
    uint32_t* nextBlocksBeside = m_arena.allocateArray<uint32_t>(numberOfBlocks);
    findBlocksThatMayBypassTests(isWalked, firstBlocksBelow);

    ArenaVector<Block*> tops;
    isWalked.forEachSetBit([&](size_t index) {
        Block* block = blocks[index];
        const Block* above = block->singlePredecessor();
        if (!above || !(flags[above->index()] & IsVisited)) {
            tops.append(m_arena, block);
            return;
        }
        nextBlocksBeside[index] = firstBlocksBelow[above->index()];
        firstBlocksBelow[above->index()] = static_cast<uint32_t>(index) + 1;
    });

    ArenaVector<BlockOnWay> way;
    auto enter = [&](Block* block, const Block* above) {
        uint32_t testAnswered = 0;
        if (TestOfBlock test = above ? m_testsOfBlocks[above->index()] : TestOfBlock(); test.number() && m_answersOnWay[test.number()] == Answer::Unknown) {
            testAnswered = test.number();
            m_answersOnWay[test.number()] = (above->successor(0).block() == block) != test.isInverted() ? Answer::IsTrue : Answer::IsFalse;
        }
        way.append(m_arena, BlockOnWay { block, firstBlocksBelow[block->index()], testAnswered });
    };
    for (Block* top : tops) {
        enter(top, nullptr);
        while (!way.isEmpty()) {
            BlockOnWay& last = way.last();
            if (uint32_t below = last.nextChild) {
                last.nextChild = nextBlocksBeside[below - 1];
                enter(blocks[below - 1], last.block);
                continue;
            }
            // Every way may have come to pass it by since it was listed.
            if ((flags[last.block->index()] & (IsVisited | MayBypassTest)) == (IsVisited | MayBypassTest))
                passTestsThatWayHasDecided(last.block);
            if (last.testAnswered)
                m_answersOnWay[last.testAnswered] = Answer::Unknown;
            way.takeLast();
        }
    }
}

// ---- `o.flags & 4`: the read asks whether the slot is empty, the word 0, and the operator whether the word has the tag of an int32, which 0 has not. The likely way is asked for the tag alone.

std::optional<JumpThreader::LeavingTest> JumpThreader::describeTestLeavingBy(const Block* block, unsigned leavingSuccessor)
{
    if (block->isEmpty() || block->last()->opcode() != Branch || block->numSuccessors() != 2 || block->successor(0).block() == block->successor(1).block())
        return std::nullopt;
    SimplifiedPredicate simplified = simplifyPredicate(block->last()->child(0));
    const Operation* test = simplified.predicate;
    bool leavesWhenNotZero = !leavingSuccessor != simplified.isInverted;
    if (isComparison(test->opcode()) && test->opcode() != EqualOrUnordered && test->child(0)->type().isInt() && test->child(1)->hasInt())
        return LeavingTest { test->child(0), test->opcode(), test->child(1)->asInt(), leavesWhenNotZero, leavingSuccessor };
    if (!test->type().isInt())
        return std::nullopt;
    return LeavingTest { test, NotEqual, 0, leavesWhenNotZero, leavingSuccessor };
}

bool JumpThreader::leavesFor(const LeavingTest& test, int64_t left)
{
    int64_t right = test.constant;
    bool is32Bit = test.subject->type() == Int32;
    if (is32Bit) {
        left = static_cast<int32_t>(left);
        right = static_cast<int32_t>(right);
    }
    uint64_t unsignedLeft = is32Bit ? static_cast<uint32_t>(left) : static_cast<uint64_t>(left);
    uint64_t unsignedRight = is32Bit ? static_cast<uint32_t>(right) : static_cast<uint64_t>(right);
    bool holds;
    switch (test.relation) {
    case Equal:
        holds = left == right;
        break;
    case NotEqual:
        holds = left != right;
        break;
    case LessThan:
        holds = left < right;
        break;
    case GreaterThan:
        holds = left > right;
        break;
    case LessEqual:
        holds = left <= right;
        break;
    case GreaterEqual:
        holds = left >= right;
        break;
    case Below:
        holds = unsignedLeft < unsignedRight;
        break;
    case Above:
        holds = unsignedLeft > unsignedRight;
        break;
    case BelowEqual:
        holds = unsignedLeft <= unsignedRight;
        break;
    case AboveEqual:
        holds = unsignedLeft >= unsignedRight;
        break;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
    return holds == test.leavesWhenItHolds;
}

bool JumpThreader::secondLeavesForAllThatFirstLeavesFor(const LeavingTest& first, const LeavingTest& second)
{
    bool firstLeavesForOneValue = first.relation == (first.leavesWhenItHolds ? Equal : NotEqual);
    if (!firstLeavesForOneValue)
        return false;
    if (second.subject == first.subject)
        return leavesFor(second, first.constant);

    // The first leaves for the words without a bit of the mask. They lie from zero to the word of all the other bits, and what an unsigned order leaves for has no gap.
    const Operation* masked = first.subject;
    bool secondIsUnsignedOrder = second.relation == Above || second.relation == Below || second.relation == AboveEqual || second.relation == BelowEqual;
    if (first.constant || masked->opcode() != BitAnd || !masked->child(1)->hasInt() || masked->child(0) != second.subject || !secondIsUnsignedOrder)
        return false;
    return leavesFor(second, 0) && leavesFor(second, ~masked->child(1)->asInt());
}

// What stands in front of the second Branch comes to run for the value that used to leave before it.
bool JumpThreader::mayRunForAnyValue(const Block* block)
{
    if (!block->phis().empty() || !block->upsilons().empty() || block->isLandingPad())
        return false;
    for (const Operation* operation : block->operations()) {
        if (operation == block->last())
            continue;
        Opcode opcode = operation->opcode();
        if (!countsChildrenAtFirstUse(opcode) || operation->mustStay() || opcode == Div || opcode == UDiv || opcode == Mod || opcode == UMod)
            return false;
    }
    return true;
}

// False: askTestBehindNextThatImpliesIt() would leave the block as it is. Each refusal here is one that it comes to itself, behind more work, and nearly every Branch meets one.
ALWAYS_INLINE bool JumpThreader::mayAskTestBehindNext(const Block* first)
{
    if (first->numSuccessors() != 2 || !first->upsilons().empty() || first->isEmpty() || first->last()->opcode() != Branch)
        return false;
    const Edge* ways = first->successors().data();
    bool firstWayIsLikely = ways[0].edgeClass() == EdgeClass::Normal;
    if (firstWayIsLikely == (ways[1].edgeClass() == EdgeClass::Normal))
        return false;
    unsigned leavingSuccessor = firstWayIsLikely ? 1 : 0;

    // What it tests is no comparison, so it is described as `word != 0`, and leaves for one value only where it leaves for zero.
    SimplifiedPredicate simplified = simplifyPredicateOfBranch(first->last()->child(0));
    const Operation* word = simplified.predicate;
    bool leavesWhenNotZero = !leavingSuccessor != simplified.isInverted;
    if (isComparisonByNumber(word->opcode()) || leavesWhenNotZero)
        return false;

    const Block* second = ways[1 - leavingSuccessor].block();
    if (second == first || second->numberOfIncomingEdges() != 1)
        return false;
    if (second->numSuccessors() != 2 || !second->phis().empty() || !second->upsilons().empty() || second->isLandingPad() || second->isEmpty() || second->last()->opcode() != Branch)
        return false;

    // The subject of the second test is what the Branch tests or, of a comparison, its first child. It has to be the word, or what the word is masked out of.
    const Operation* asked = simplifyPredicateOfBranch(second->last()->child(0)).predicate;
    const Operation* masked = word->opcode() == BitAnd ? word->child(0) : nullptr;
    if (asked == word || asked == masked)
        return true;
    if (!isComparisonByNumber(asked->opcode()))
        return false;
    const Operation* compared = asked->child(0);
    return compared == word || compared == masked;
}

void JumpThreader::askTestBehindNextThatImpliesIt(Block* first)
{
    if (first->isEmpty() || first->last()->opcode() != Branch || first->numSuccessors() != 2 || !first->upsilons().empty())
        return;
    bool firstWayIsLikely = first->successor(0).edgeClass() == EdgeClass::Normal;
    if (firstWayIsLikely == (first->successor(1).edgeClass() == EdgeClass::Normal))
        return;
    std::optional<LeavingTest> firstTest = describeTestLeavingBy(first, firstWayIsLikely ? 1 : 0);
    // A comparison would stay behind in the block, and be made a word there for a reader on the way that leaves.
    if (!firstTest || isComparison(simplifyPredicate(first->last()->child(0)).predicate->opcode()))
        return;

    Block* second = first->successor(1 - firstTest->leavingSuccessor).block();
    if (second == first || second->numberOfIncomingEdges() != 1)
        return;
    // The way out of the second may be a likely one: who takes it is asked two tests as before, in the other order. Who takes the other way is asked one, so that has to be a likely one.
    std::optional<LeavingTest> secondTest;
    for (unsigned leavingSuccessor = 0; leavingSuccessor < 2 && !secondTest; ++leavingSuccessor) {
        if (second->numSuccessors() != 2 || second->successor(1 - leavingSuccessor).edgeClass() != EdgeClass::Normal)
            continue;
        secondTest = describeTestLeavingBy(second, leavingSuccessor);
        if (secondTest && !secondLeavesForAllThatFirstLeavesFor(*firstTest, *secondTest))
            secondTest = std::nullopt;
    }
    if (!secondTest || !mayRunForAnyValue(second))
        return;

    Block* targetOfFirst = first->successor(firstTest->leavingSuccessor).block();
    Block* targetOfSecond = second->successor(secondTest->leavingSuccessor).block();
    if (targetOfFirst == second || targetOfSecond == second || targetOfSecond == first || !canGainPredecessor(targetOfFirst) || !canGainPredecessor(targetOfSecond))
        return;
    Order::askBehind(first, firstTest->leavingSuccessor, second, secondTest->leavingSuccessor);
}

// The blocks are asked by rising index. Each Order::askBehind() makes a block, of which nothing above knows and nothing here asks, and a Jump of the Branch of a block that was there:
// so no block comes to end in a Branch that m_testsOfBranches does not list.
void JumpThreader::askTestsBehindNextThatImplyThem()
{
    if (m_testsOfBranchesAreInOrder) [[likely]] {
        for (const TestOfBranch& test : m_testsOfBranches) {
            // It asks a comparison.
            if (test.opcode && test.opcode != BitAnd + 1)
                continue;
            Block* block = m_proc.blockAt(test.block);
            if (mayAskTestBehindNext(block))
                askTestBehindNextThatImpliesIt(block);
        }
        return;
    }
    for (unsigned index = 0, numberOfBlocks = m_proc.numberOfBlocks(); index < numberOfBlocks; ++index) {
        Block* block = m_proc.blockAt(index);
        if (mayAskTestBehindNext(block))
            askTestBehindNextThatImpliesIt(block);
    }
}

void JumpThreader::run()
{
    uint32_t numberOfBlocks = m_proc.numberOfBlocks();
    m_flags.fill(m_arena, numberOfBlocks, 0);
    // No block is made before askTestsBehindNextThatImplyThem().
    Block* const* blocks = m_proc.blocks().data();
    for (uint32_t indexOfBlock = 0; indexOfBlock < numberOfBlocks; ++indexOfBlock) {
        Block* block = blocks[indexOfBlock];
        ASSERT(block->index() == indexOfBlock);
        m_indexOfBlockInTurn = indexOfBlock;
        uint32_t size = block->size();
        if (!size)
            continue;
        const Operation* last = block->operations().data()[size - 1];
        Opcode opcode = last->opcode();
        if (opcode == Jump) [[likely]] {
            if (size == 1) {
                // Most have been forwarded for an edge that leads to them. Only an empty jump block is marked so, and it is one for as long as a Jump ends it.
                if (m_flags[indexOfBlock] & (IsBeingForwarded | IsForwarded))
                    continue;
                // isEmptyJumpBlock().
                if (block->phis().empty() && !block->isLandingPad() && !block->isEntry()) {
                    forwardEmptyJumpChain(block);
                    continue;
                }
            }
            if (block->upsilons().size() == 1 && block->upsilons().data()[0]->child(0)->opcode() == Add) [[unlikely]]
                passTestOfUpperHalfBy(block);
        } else if (opcode == Branch) {
            if (block->numSuccessors() == 2)
                listTestOfBranch(indexOfBlock, last);
        } else if (opcode != Switch)
            continue;
        ASSERT(canRetargetSuccessors(block) && !isEmptyJumpBlock(block));

        const Edge* successors = block->successors().data();
        const Edge* end = successors + block->numSuccessors();
        for (const Edge* edge = successors; edge != end; ++edge) {
            // Most edges lead to a block that is neither threaded through nor skipped.
            // Whatever is called here may make a Branch of the Jump that ends the block, which gives it a second edge and may move its edges to other storage.
            Block* target = edge->block();
            if (mayThreadThrough(target)) [[unlikely]] {
                unsigned index = static_cast<unsigned>(edge - successors);
                threadThroughJoin(block, index);
                successors = block->successors().data();
                end = successors + block->numSuccessors();
                edge = successors + index;
                target = edge->block();
            }
            if (target->size() != 1 || target->last()->opcode() != Jump)
                continue;

            unsigned index = static_cast<unsigned>(edge - successors);
            // Only an empty jump block is marked so, and it is one for as long as a Jump ends it.
            if (has(target, IsForwarded))
                skipForwardedBlock(block, index, target);
            else if (isEmptyJumpBlock(target)) {
                forwardEmptyJumpChain(target);
                skipEmptyJumpBlock(block, index);
            } else
                continue;
            successors = block->successors().data();
            end = successors + block->numSuccessors();
            edge = successors + index;
        }
    }
    // Each speaks of two blocks that a Branch with two ways out ends.
    if (m_testsOfBranches.size() < 2)
        return;
    threadEdgesPastDecidedTests();
    askTestsBehindNextThatImplyThem();
}

} // anonymous namespace

void threadJumps(Procedure& procedure)
{
    if (procedure.failure())
        return;
    // No edge, no work.
    if (procedure.numberOfBlocks() == 1 && !procedure.blockAt(0)->numSuccessors())
        return;
    JumpThreader(procedure).run();
}

} } } // namespace JSC::AOT::Machine

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
