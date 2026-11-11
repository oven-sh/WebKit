/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachineOrder.h"

#if ENABLE(AOT)

#include "AOTInliningAcrossFiles.h"
#include "AOTMachineFolding.h"
#include "AOTMachineTestsOfPhis.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <optional>
#include <type_traits>
#include <wtf/HashSet.h>
#include <wtf/MathExtras.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT { namespace Machine {

namespace {

class JumpThreader {
    WTF_MAKE_NONCOPYABLE(JumpThreader);
public:
    explicit JumpThreader(Procedure& procedure)
        : m_proc(procedure)
        , m_arena(procedure.arena())
    {
    }

    void run();
    void letGoOfLists();

private:
    enum Flag : uint8_t {
        IsBeingForwarded = 1 << 0,
        IsForwarded = 1 << 1,
        IsVisited = 1 << 2,
        IsOnPath = 1 << 3,
        IsLoopHead = 1 << 4,
        HasThreadedOwnJump = 1 << 5,
        HoldsNothingButItsTest = 1 << 6,
        MayBypassTest = 1 << 7,
    };

    struct Visit {
        const Edge* next;
        const Edge* end;
        uint32_t index;
    };

    struct ThreadableJoin {
        const Operation* phi;
        bool isInverted;
        bool comparesPhi { false };
    };

    enum class MatchOfJoin : uint8_t {
        IsNotLookedAt,
        IsNotThreadable,
        TestsPhi,
        TestsInverseOfPhi,
        ComparesPhi,
    };

    struct NumberedTest {
        uint64_t operands { 0 };
        uint32_t opcode { 0 };
        uint32_t number { 0 };
    };

    struct TestOfBranch {
        uint64_t operands;
        uint32_t block;
        uint8_t opcode;
        bool isInverted;
        uint16_t wordRead;
    };
    static_assert(sizeof(TestOfBranch) == 16);

    class TestOfBlock {
    public:
        TestOfBlock() = default;
        TestOfBlock(uint32_t number, bool isInverted, bool readsWordThatMayBeWritten)
            : m_bits(number << 2 | static_cast<uint32_t>(readsWordThatMayBeWritten) << 1 | static_cast<uint32_t>(isInverted))
        {
        }

        uint32_t number() const { return m_bits >> 2; }
        bool readsWordThatMayBeWritten() const { return m_bits & 2; }
        bool isInverted() const { return m_bits & 1; }

    private:
        uint32_t m_bits { 0 };
    };

    enum class Answer : uint8_t { Unknown, IsTrue, IsFalse };

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
        uint32_t testAnswered;
        uint32_t formerDepthOfAnswer;
        uint32_t formerDepthOfLastWrite;
        Answer formerAnswer;
    };

    bool has(const Block* block, uint8_t flags) const { return m_flags[block->index()] & flags; }
    void set(const Block* block, uint8_t flags) { m_flags[block->index()] |= flags; }

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
    static ALWAYS_INLINE SimplifiedPredicate simplifyPredicateOfBranch(Operation* predicate);

    NEVER_INLINE void findLoopHeads();
    ALWAYS_INLINE bool isLoopHead(const Block*);
    static ALWAYS_INLINE bool hasFormOfThreadableJoin(const Block* join);
    MatchOfJoin lookAtJoin(Block* join);
    std::optional<ThreadableJoin> matchThreadableJoin(Block* join);

    void retarget(Block* from, unsigned successorIndex, Block* target);
    void retargetPast(Block* from, unsigned successorIndex, Edge passedBy);
    void setEdgeClass(Block* from, unsigned successorIndex, EdgeClass);
    ALWAYS_INLINE bool isInHand(const Block*) const;
    ALWAYS_INLINE void letGoUnlessSomethingLeadsTo(Block*);
    NEVER_INLINE void letGoOfBlockThatNothingLeadsTo(Block*);
    NEVER_INLINE void letGoOfBlocksThatWereInHand();

    void duplicateBranchIntoPredecessor(Block* from, Block* join, const ThreadableJoin&);
    void threadJumpsOfJoinsFrom(Block* join);
    ALWAYS_INLINE bool wouldThreadThrough(const Block* join) const;
    ALWAYS_INLINE bool mayThreadThrough(const Block* join);
    void threadThroughJoin(Block* from, unsigned successorIndex);
    ALWAYS_INLINE void skipEmptyJumpBlock(Block* from, unsigned successorIndex);
    ALWAYS_INLINE void skipForwardedBlock(Block* from, unsigned successorIndex, Block* skipped);
    ALWAYS_INLINE void forwardEmptyJumpChain(Block*);
    NEVER_INLINE void forwardEmptyJumpChainFrom(Block*);

    NEVER_INLINE void makeRoomForTestOfBranch();
    ALWAYS_INLINE void listTestOfBranch(uint32_t indexOfBlock, const Operation* branch);
    NEVER_INLINE bool numberTestsAskedTwice();
    static bool holdsNothingButItsTest(const Block*);
    ALWAYS_INLINE Answer answerOnWay(TestOfBlock) const;
    NEVER_INLINE void findBlocksThatMayBypassTests(BitSpan isWalked, uint32_t* firstBlocksBelow);
    void passTestsThatWayHasDecided(Block* from);
    NEVER_INLINE void threadEdgesPastDecidedTests();

    static std::optional<LeavingTest> describeTestLeavingBy(const Block*, unsigned leavingSuccessor);
    static bool leavesFor(const LeavingTest&, int64_t value);
    static bool secondLeavesForAllThatFirstLeavesFor(const LeavingTest& first, const LeavingTest& second);
    static bool mayRunForAnyValue(const Block*);
    static ALWAYS_INLINE bool mayAskTestBehindNext(const Block* first);
    static NEVER_INLINE void askTestBehindNextThatImpliesIt(Block* first);
    static ALWAYS_INLINE Operation* findBitsThatBranchAsks(const Block*, unsigned& wayWhereBitIsSet);
    static bool isReadAgainWhereNothingWasWritten(const Operation* read, const Block* first, const Operation* readAgain, const Block* second);
    static bool isAskedByBranchOfItsBlockAlone(const Operation* bits, const Block*);
    static ALWAYS_INLINE bool mayEndInTestOfBitsOf32Bits(const Block*);
    static NEVER_INLINE void askBitsOfNextTestAtOnce(Block* first);
    struct Bound {
        bool isLower;
        bool isSigned;
        int64_t value;
    };
    static std::optional<Bound> boundWithinWhichTestStays(const LeavingTest&);
    static bool leadToOnePlace(const Block*, const Block*);
    static bool mayBeFollowedByTestOfOtherBound(const Block*);
    void askBothBoundsAtOnce(Block* first);
    static bool isFollowedByThreeLittleTests(const Block*);
    static bool areOneValue(const Operation*, const Operation*);
    void askEqualitiesBySwitch(Block* first);
    NEVER_INLINE void askTestsBehindNextThatImplyThem();

    struct JoinPassedBy {
        Block* join;
        Block* passing;
        Operation* lowerHalf;
    };
    static Operation* wordAddedTo(int64_t upperHalf, const Operation* sum);
    NEVER_INLINE void passTestOfUpperHalfBy(Block* from);

    Procedure& m_proc;
    Arena& m_arena;
    ArenaVector<uint8_t> m_flags;
    ArenaVector<MatchOfJoin> m_matchesOfJoins;
    BitSpan m_joinsThatOneWayLedToAtFirst;
    ArenaVector<Block*> m_chain;
    ArenaVector<Block*> m_joinsThatJump;
    ArenaVector<Block*> m_blocksToLetGo;
    ArenaVector<Block*> m_blocksInHandThatNothingLeadsTo;
    bool m_isWalkingWays { false };
    ArenaVector<JoinPassedBy> m_joinsPassedBy;
    bool m_hasFoundLoopHeads { false };
    uint32_t m_indexOfBlockInTurn { std::numeric_limits<uint32_t>::max() };
    ArenaVector<TestOfBranch> m_testsOfBranches;
    bool m_testsOfBranchesAreInOrder { true };
    ArenaVector<TestOfBlock> m_testsOfBlocks;
    ArenaVector<Answer> m_answersOnWay;
    ArenaVector<uint32_t> m_depthsOfAnswers;
    uint32_t m_depthOfLastWrite { 0 };
};

ALWAYS_INLINE bool JumpThreader::canRetargetSuccessors(const Block* block)
{
    if (block->isEmpty())
        return false;
    Opcode opcode = block->last()->opcode();
    return opcode == Jump || opcode == Branch || opcode == Switch;
}

ALWAYS_INLINE bool JumpThreader::canGainPredecessor(const Block* target)
{
    return target->phis().empty() && !target->isLandingPad();
}

ALWAYS_INLINE bool JumpThreader::isEmptyJumpBlock(const Block* block)
{
    if (block->size() != 1 || block->last()->opcode() != Jump)
        return false;
    ASSERT(block->numSuccessors() == 1);
    return block->phis().empty() && !block->isLandingPad() && !block->isEntry();
}

const Operation* JumpThreader::incomingValue(const Operation* phi, const Block* from)
{
    Operation* const* upsilons = from->upsilons().data();
    for (size_t index = from->upsilons().size(); index--;) {
        if (upsilons[index]->phi() == phi)
            return upsilons[index]->valueHandedOn();
    }
    return nullptr;
}

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

void JumpThreader::findLoopHeads()
{
    uint32_t numberOfBlocks = m_proc.numberOfBlocks();
    uint8_t* flags = m_flags.begin();
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
    m_arena.letGoOfArray(path, numberOfBlocks);
}

ALWAYS_INLINE_INTO_OTHER_FILES bool JumpThreader::isLoopHead(const Block* block)
{
    if (!m_hasFoundLoopHeads) [[unlikely]] {
        m_hasFoundLoopHeads = true;
        findLoopHeads();
    }
    return has(block, IsLoopHead);
}

ALWAYS_INLINE bool JumpThreader::hasFormOfThreadableJoin(const Block* join)
{
    if (join->phis().size() != 1 || !join->upsilons().empty() || join->isEmpty() || join->isLandingPad())
        return false;
    Opcode opcode = join->last()->opcode();
    return (opcode == Branch || opcode == Switch) && join->phis().data()[0]->useCount() == 1;
}

JumpThreader::MatchOfJoin JumpThreader::lookAtJoin(Block* join)
{
    Operation* branch = join->last();
    Operation* phi = join->phis().data()[0];
    ASSERT(branch->opcode() == Switch || join->numSuccessors() == 2);

    SimplifiedPredicate simplified = branch->opcode() == Branch ? simplifyPredicateOfBranch(branch->child(0)) : SimplifiedPredicate { branch->child(0), false };
    if (simplified.predicate != phi) {
        if (branch->opcode() != Branch)
            return MatchOfJoin::IsNotThreadable;
        for (const Operation* operation : join->operations()) {
            if (operation != branch && (operation->useCount() > 1 || operation->mustStay()))
                return MatchOfJoin::IsNotThreadable;
        }
        if (!holdsNothingButTestOfItsPhi(join, phi) || isLoopHead(join))
            return MatchOfJoin::IsNotThreadable;
        return MatchOfJoin::ComparesPhi;
    }

    unsigned peeledOperationCount = 0;
    for (const Operation* operation = branch->child(0); operation != phi; operation = operation->child(0)) {
        if (operation->owner() != join || operation->useCount() != 1 || operation->mustStay() || !operation->numChildren())
            return MatchOfJoin::IsNotThreadable;
        ++peeledOperationCount;
    }

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

std::optional<JumpThreader::ThreadableJoin> JumpThreader::matchThreadableJoin(Block* join)
{
    if (!hasFormOfThreadableJoin(join))
        return std::nullopt;
    if (m_matchesOfJoins.isEmpty()) [[unlikely]] {
        m_matchesOfJoins.fill(m_arena, m_proc.numberOfBlocks(), MatchOfJoin::IsNotLookedAt);
        m_joinsThatOneWayLedToAtFirst = BitSpan::create(m_arena, m_proc.numberOfBlocks());
    }
    MatchOfJoin match = m_matchesOfJoins[join->index()];
    if (match == MatchOfJoin::IsNotLookedAt) {
        match = lookAtJoin(join);
        m_matchesOfJoins[join->index()] = match;
        if (join->numberOfIncomingEdges() == 1)
            m_joinsThatOneWayLedToAtFirst.set(join->index());
    }
    if (match == MatchOfJoin::IsNotThreadable)
        return std::nullopt;
    return ThreadableJoin { join->phis().data()[0], match == MatchOfJoin::TestsInverseOfPhi, match == MatchOfJoin::ComparesPhi };
}

ALWAYS_INLINE void JumpThreader::letGoUnlessSomethingLeadsTo(Block* block)
{
    if (!block->numberOfIncomingEdges() && !block->isEntry()) [[unlikely]]
        letGoOfBlockThatNothingLeadsTo(block);
}

ALWAYS_INLINE bool JumpThreader::isInHand(const Block* block) const
{
    uint8_t flags = m_flags[block->index()];
    return m_isWalkingWays || block->index() == m_indexOfBlockInTurn || (flags & (IsBeingForwarded | IsForwarded)) == IsBeingForwarded || (flags & HasThreadedOwnJump);
}

void JumpThreader::letGoOfBlockThatNothingLeadsTo(Block* first)
{
    while (first->numSuccessors() <= 1 && !isInHand(first)) {
        Block* next = first->numSuccessors() ? first->successor(0).block() : nullptr;
        Order::emptyBlock(first);
        if (!next || next->numberOfIncomingEdges() || next->isEntry() || next->isEmpty())
            return;
        first = next;
    }

    ASSERT(m_blocksToLetGo.isEmpty());
    m_blocksToLetGo.append(m_arena, first);
    while (!m_blocksToLetGo.isEmpty()) {
        Block* block = m_blocksToLetGo.takeLast();
        if (isInHand(block)) {
            m_blocksInHandThatNothingLeadsTo.append(m_arena, block);
            continue;
        }
        uint32_t numberKept = m_blocksToLetGo.size();
        for (const Edge& edge : block->successors())
            m_blocksToLetGo.append(m_arena, edge.block());
        Order::emptyBlock(block);
        for (uint32_t index = numberKept; index < m_blocksToLetGo.size(); ++index) {
            Block* target = m_blocksToLetGo[index];
            if (!target->numberOfIncomingEdges() && !target->isEntry() && !target->isEmpty())
                m_blocksToLetGo[numberKept++] = target;
        }
        m_blocksToLetGo.shrink(numberKept);
    }
}

void JumpThreader::letGoOfBlocksThatWereInHand()
{
    while (!m_blocksInHandThatNothingLeadsTo.isEmpty()) {
        Block* block = m_blocksInHandThatNothingLeadsTo.takeLast();
        if (!block->numberOfIncomingEdges() && !block->isEmpty())
            Order::letGoOfBlockThatNothingLeadsTo(block);
    }
}

void JumpThreader::retarget(Block* from, unsigned successorIndex, Block* target)
{
    Block* formerTarget = from->successor(successorIndex).block();
    Order::retarget(from, successorIndex, target);
    letGoUnlessSomethingLeadsTo(formerTarget);
}

void JumpThreader::retargetPast(Block* from, unsigned successorIndex, Edge passedBy)
{
    Block* formerTarget = from->successor(successorIndex).block();
    Order::retargetPast(from, successorIndex, passedBy);
    letGoUnlessSomethingLeadsTo(formerTarget);
}

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

void JumpThreader::duplicateBranchIntoPredecessor(Block* from, Block* join, const ThreadableJoin& threadable)
{
    if (from->last()->opcode() != Jump || join->last()->opcode() != Branch)
        return;
    for (const Edge& edge : join->successors()) {
        ASSERT(edge.block()->phis().empty());
        if (!canGainPredecessor(edge.block()))
            return;
    }

    Order::replaceJumpWithBranch(from, threadable.phi, threadable.isInverted);
    letGoUnlessSomethingLeadsTo(join);
    if (from->index() > m_indexOfBlockInTurn)
        return;
    if (!m_testsOfBranches.isEmpty() && m_testsOfBranches.last().block > from->index())
        m_testsOfBranchesAreInOrder = false;
    listTestOfBranch(from->index(), from->last());
}

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
    ASSERT(target->phis().empty());
    if (!canGainPredecessor(target))
        return;

    bool isOnlyEdge = m_joinsThatOneWayLedToAtFirst.get(join->index());
    EdgeClass edgeClass = from->successor(successorIndex).edgeClass();
    retargetPast(from, successorIndex, join->successor(chosenSuccessor));
    if (isOnlyEdge)
        setEdgeClass(from, successorIndex, edgeClass);
}

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
        Block* passing = join->successor(test->opcode() == AboveEqual ? 0 : 1).block();
        if (!has(from, IsVisited) || (passing != known.passing && (known.lowerHalf || passing == join || !canGainPredecessor(passing))))
            return;
        Order::passJoinBy(from, passing, known.lowerHalf, known.lowerHalf ? word : nullptr);
        letGoUnlessSomethingLeadsTo(join);
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
    if (reads != readsAheadOfFailingSide && failing->numberOfIncomingEdges() != 1)
        return;
    if (isLoopHead(join) || !has(from, IsVisited))
        return;

    if (truncation)
        Order::makePhiOfTruncationBehindJoin(join, truncation);
    m_joinsPassedBy.append(m_arena, JoinPassedBy { join, passing, truncation });
    Order::passJoinBy(from, passing, truncation, truncation ? word : nullptr);
    letGoUnlessSomethingLeadsTo(join);
}

ALWAYS_INLINE void JumpThreader::skipEmptyJumpBlock(Block* from, unsigned successorIndex)
{
    Block* skipped = from->successor(successorIndex).block();
    if (isEmptyJumpBlock(skipped) && has(skipped, IsForwarded))
        skipForwardedBlock(from, successorIndex, skipped);
}

ALWAYS_INLINE void JumpThreader::skipForwardedBlock(Block* from, unsigned successorIndex, Block* skipped)
{
    ASSERT(from->successor(successorIndex).block() == skipped && isEmptyJumpBlock(skipped) && has(skipped, IsForwarded));
    Block* target = skipped->successor(0).block();
    if (target == skipped)
        return;
    if (!skipped->upsilons().empty() || !canGainPredecessor(target))
        return;
    retarget(from, successorIndex, target);
}

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

void JumpThreader::makeRoomForTestOfBranch()
{
    m_testsOfBranches.reserve(m_arena, m_testsOfBranches.capacity() * 2 + 4);
}

ALWAYS_INLINE void JumpThreader::listTestOfBranch(uint32_t indexOfBlock, const Operation* branch)
{
    static_assert(Equal < NotEqual && LessThan < GreaterEqual && GreaterThan < LessEqual && Above < BelowEqual && Below < AboveEqual);
    static constexpr uint8_t goesByOperation = UINT8_MAX;
    static_assert(numberOfOpcodes <= goesByOperation);
    static constexpr std::array<uint8_t, numberOfOpcodes> lowerOpcodesOfTests = [] {
        std::array<uint8_t, numberOfOpcodes> lowerOpcodes { };
        for (unsigned number = 0; number < numberOfOpcodes; ++number)
            lowerOpcodes[number] = isComparisonByNumber(static_cast<Opcode>(number)) || number == BitAnd ? static_cast<uint8_t>(number) : goesByOperation;
        lowerOpcodes[NotEqual] = Equal;
        lowerOpcodes[GreaterEqual] = LessThan;
        lowerOpcodes[LessEqual] = GreaterThan;
        lowerOpcodes[BelowEqual] = Above;
        lowerOpcodes[AboveEqual] = Below;
        return lowerOpcodes;
    }();

    SimplifiedPredicate simplified = simplifyPredicateOfBranch(branch->child(0));
    const Operation* test = simplified.predicate;
    Opcode opcode = test->opcode();
    uint64_t operands = test->index();
    uint8_t opcodeOfTest = 0;
    uint16_t wordRead = 0;
    if (uint8_t lower = lowerOpcodesOfTests[opcode]; lower != goesByOperation) {
        ASSERT(!isComparison(opcode) || opcode == EqualOrUnordered || lower == std::min(opcode, invertedComparison(opcode)));
        Operation* const* children = test->inlineChildren();
        if (lower != opcode && children[0]->type().isInt()) {
            opcode = static_cast<Opcode>(lower);
            simplified.isInverted = !simplified.isInverted;
        }
        const Operation* first = children[0];
        if (isLoad(first->opcode()) && first->useCount() == 1 && test->useCount() == 1) {
            static_assert(Load - Load8Z < 7);
            constexpr int32_t endOfOffsets = 1 << 12;
            bool mayBeWritten = !first->has(OperationFlag::ReadsImmutableHeap);
            const Block* block = branch->owner();
            if (!first->traps() && !first->child(0)->has(OperationFlag::IsInnerPointer) && first->offset() >= 0 && first->offset() < endOfOffsets && (!mayBeWritten || (first->owner() == block && !block->writesMemory()))) {
                wordRead = static_cast<uint16_t>(first->offset() << 4 | (first->opcode() - Load8Z + 1) << 1 | static_cast<int32_t>(mayBeWritten));
                first = first->child(0);
            }
        }
        operands = static_cast<uint64_t>(first->index()) << 32 | children[1]->index();
        opcodeOfTest = static_cast<uint8_t>(opcode + 1);
    }
    if (m_testsOfBranches.size() == m_testsOfBranches.capacity()) [[unlikely]]
        makeRoomForTestOfBranch();
    m_testsOfBranches.appendWithinCapacity(TestOfBranch { operands, indexOfBlock, opcodeOfTest, simplified.isInverted, wordRead });
}

bool JumpThreader::numberTestsAskedTwice()
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
    const TestOfBranch* tests = m_testsOfBranches.begin();
    Block* const* blocks = m_proc.blocks().data();
    uint8_t* flags = m_flags.begin();
    TestOfBlock* testsOfBlocks = nullptr;
    bool holdsSuchBlock = false;
    for (uint32_t branch = 0; branch < numberOfBranches; ++branch) {
        if (blocks[tests[branch].block]->isEmpty())
            continue;
        uint64_t operands = tests[branch].operands;
        uint32_t opcode = tests[branch].opcode | static_cast<uint32_t>(tests[branch].wordRead) << 8;
        uint32_t place = static_cast<uint32_t>((operands ^ static_cast<uint64_t>(opcode) << 27) * 0x9e3779b97f4a7c15ull >> shift);
        for (;;) {
            NumberedTest& held = table[place];
            uint32_t number = held.number;
            if (!number) {
                held.operands = operands;
                held.opcode = opcode;
                held.number = branch + 1;
                break;
            }
            if (held.operands != operands || held.opcode != opcode) {
                place = (place + 1) & mask;
                continue;
            }

            if (!testsOfBlocks) [[unlikely]] {
                m_testsOfBlocks.fill(m_arena, m_proc.numberOfBlocks(), TestOfBlock());
                testsOfBlocks = m_testsOfBlocks.begin();
            }
            const TestOfBranch* const askedAgain = tests + branch;
            const TestOfBranch* asked = tests + (number - 1);
            if (testsOfBlocks[asked->block].number())
                asked = askedAgain;
            for (;; asked = askedAgain) {
                uint32_t index = asked->block;
                testsOfBlocks[index] = TestOfBlock(number, asked->isInverted, asked->wordRead & 1);
                Block* block = blocks[index];
                if (holdsNothingButItsTest(block) && !isLoopHead(block) && (flags[index] & IsVisited)) {
                    flags[index] |= HoldsNothingButItsTest;
                    holdsSuchBlock = true;
                }
                if (asked == askedAgain)
                    break;
            }
            break;
        }
    }
    m_arena.letGoOfArray(table, numberOfPlaces);
    if (!holdsSuchBlock)
        return false;
    m_answersOnWay.fill(m_arena, numberOfBranches + 1, Answer::Unknown);
    m_depthsOfAnswers.fill(m_arena, numberOfBranches + 1, 0);
    return true;
}

bool JumpThreader::holdsNothingButItsTest(const Block* block)
{
    if (!block->phis().empty() || !block->upsilons().empty() || block->isLandingPad() || block->isEntry())
        return false;
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

void JumpThreader::passTestsThatWayHasDecided(Block* from)
{
    if (!canRetargetSuccessors(from) || (!from->numberOfIncomingEdges() && !from->isEntry()))
        return;
    TestOfBlock own = m_testsOfBlocks[from->index()];
    for (unsigned index = 0; index < from->numSuccessors(); ++index) {
        for (;;) {
            Block* passed = from->successor(index).block();
            TestOfBlock test = m_testsOfBlocks[passed->index()];
            if (!test.number() || !has(passed, HoldsNothingButItsTest))
                break;
            Answer answer = answerOnWay(test);
            if (answer == Answer::Unknown && own.number() == test.number())
                answer = !index != own.isInverted() ? Answer::IsTrue : Answer::IsFalse;
            if (answer == Answer::Unknown)
                break;
            Block* target = passed->successor((answer == Answer::IsTrue) != test.isInverted() ? 0 : 1).block();
            if (target == passed || !canGainPredecessor(target))
                break;
            retarget(from, index, target);
        }
    }
}

ALWAYS_INLINE JumpThreader::Answer JumpThreader::answerOnWay(TestOfBlock test) const
{
    if (test.readsWordThatMayBeWritten() && m_depthsOfAnswers[test.number()] <= m_depthOfLastWrite)
        return Answer::Unknown;
    return m_answersOnWay[test.number()];
}

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

void JumpThreader::threadEdgesPastDecidedTests()
{
    if (!numberTestsAskedTwice())
        return;

    uint32_t numberOfBlocks = m_proc.numberOfBlocks();
    Block* const* blocks = m_proc.blocks().data();
    uint8_t* flags = m_flags.begin();

    BitSpan isWalked = BitSpan::create(m_arena, numberOfBlocks);
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
        uint32_t depth = way.size() + 1;
        BlockOnWay entered { block, firstBlocksBelow[block->index()], 0, 0, m_depthOfLastWrite, Answer::Unknown };
        if (TestOfBlock test = above ? m_testsOfBlocks[above->index()] : TestOfBlock(); test.number() && answerOnWay(test) == Answer::Unknown) {
            entered.testAnswered = test.number();
            entered.formerAnswer = m_answersOnWay[test.number()];
            entered.formerDepthOfAnswer = m_depthsOfAnswers[test.number()];
            m_answersOnWay[test.number()] = (above->successor(0).block() == block) != test.isInverted() ? Answer::IsTrue : Answer::IsFalse;
            m_depthsOfAnswers[test.number()] = depth;
        }
        if (block->writesMemory())
            m_depthOfLastWrite = depth;
        way.append(m_arena, entered);
    };
    for (uint32_t place = tops.size(); place--;) {
        enter(tops[place], nullptr);
        while (!way.isEmpty()) {
            BlockOnWay& last = way.last();
            if (uint32_t below = last.nextChild) {
                last.nextChild = nextBlocksBeside[below - 1];
                enter(blocks[below - 1], last.block);
                continue;
            }
            if ((flags[last.block->index()] & (IsVisited | MayBypassTest)) == (IsVisited | MayBypassTest))
                passTestsThatWayHasDecided(last.block);
            if (last.testAnswered) {
                m_answersOnWay[last.testAnswered] = last.formerAnswer;
                m_depthsOfAnswers[last.testAnswered] = last.formerDepthOfAnswer;
            }
            m_depthOfLastWrite = last.formerDepthOfLastWrite;
            way.takeLast();
        }
    }
    way.letGo(m_arena);
    tops.letGo(m_arena);
    m_arena.letGoOfArray(nextBlocksBeside, numberOfBlocks);
    m_arena.letGoOfArray(firstBlocksBelow, numberOfBlocks);
}

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

    const Operation* masked = first.subject;
    bool secondIsUnsignedOrder = second.relation == Above || second.relation == Below || second.relation == AboveEqual || second.relation == BelowEqual;
    if (first.constant || masked->opcode() != BitAnd || !masked->child(1)->hasInt() || masked->child(0) != second.subject || !secondIsUnsignedOrder)
        return false;
    return leavesFor(second, 0) && leavesFor(second, ~masked->child(1)->asInt());
}

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

ALWAYS_INLINE bool JumpThreader::mayAskTestBehindNext(const Block* first)
{
    if (first->numSuccessors() != 2)
        return false;
    const Edge* ways = first->successors().data();
    bool firstWayIsLikely = ways[0].edgeClass() == EdgeClass::Normal;
    if (firstWayIsLikely == (ways[1].edgeClass() == EdgeClass::Normal))
        return false;
    if (!first->upsilons().empty() || first->isEmpty() || first->last()->opcode() != Branch)
        return false;
    unsigned leavingSuccessor = firstWayIsLikely ? 1 : 0;

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
    if (!firstTest || isComparison(simplifyPredicate(first->last()->child(0)).predicate->opcode()))
        return;

    Block* second = first->successor(1 - firstTest->leavingSuccessor).block();
    if (second == first || second->numberOfIncomingEdges() != 1)
        return;
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

ALWAYS_INLINE Operation* JumpThreader::findBitsThatBranchAsks(const Block* block, unsigned& wayWhereBitIsSet)
{
    if (block->numSuccessors() != 2 || block->isEmpty() || block->last()->opcode() != Branch || !block->upsilons().empty())
        return nullptr;
    SimplifiedPredicate simplified = simplifyPredicateOfBranch(block->last()->child(0));
    Operation* bits = simplified.predicate;
    if (bits->opcode() != BitAnd || bits->type() != Int32 || !bits->child(1)->hasInt())
        return nullptr;
    wayWhereBitIsSet = simplified.isInverted ? 1 : 0;
    return bits;
}

bool JumpThreader::isReadAgainWhereNothingWasWritten(const Operation* read, const Block* first, const Operation* readAgain, const Block* second)
{
    if (!isLoad(read->opcode()) || readAgain->opcode() != read->opcode() || readAgain->type() != read->type() || read->owner() != first || readAgain->owner() != second)
        return false;
    if (readAgain->child(0) != read->child(0) || readAgain->offset() != read->offset() || readAgain->heap() != read->heap())
        return false;
    for (unsigned index = read->indexInBlock() + 1, size = first->size(); index < size; ++index) {
        const Operation* operation = first->at(index);
        if (operation != first->last() && operation->mustStay())
            return false;
    }
    return true;
}

bool JumpThreader::isAskedByBranchOfItsBlockAlone(const Operation* bits, const Block* block)
{
    const Operation* branch = block->last();
    unsigned numberOfUsesSeen = branch->child(0) == bits;
    for (const Operation* operation : block->operations()) {
        if (operation == branch || !operation->useCount() || !isComparison(operation->opcode()) || operation->child(0) != bits)
            continue;
        if (!operation->child(1)->hasInt() || operation->child(1)->asInt() || operation->useCount() != 1 || branch->child(0) != operation)
            return false;
        ++numberOfUsesSeen;
    }
    return bits->useCount() == numberOfUsesSeen;
}

ALWAYS_INLINE bool JumpThreader::mayEndInTestOfBitsOf32Bits(const Block* block)
{
    if (block->numSuccessors() != 2 || block->isEmpty() || block->last()->opcode() != Branch)
        return false;
    const Operation* asked = block->last()->child(0);
    return asked->type() == Int32 && (asked->opcode() == BitAnd || asked->opcode() == Equal || asked->opcode() == NotEqual);
}

void JumpThreader::askBitsOfNextTestAtOnce(Block* first)
{
    unsigned wayOfFirstWhereBitIsSet = 0;
    Operation* bitsOfFirst = findBitsThatBranchAsks(first, wayOfFirstWhereBitIsSet);
    if (!bitsOfFirst)
        return;
    Block* second = first->successor(1 - wayOfFirstWhereBitIsSet).block();
    if (second == first || second->numberOfIncomingEdges() != 1 || !second->phis().empty() || second->isLandingPad() || second->isEntry())
        return;
    unsigned wayOfSecondWhereBitIsSet = 0;
    Operation* bitsOfSecond = findBitsThatBranchAsks(second, wayOfSecondWhereBitIsSet);
    if (!bitsOfSecond || !isAskedByBranchOfItsBlockAlone(bitsOfFirst, first) || !isAskedByBranchOfItsBlockAlone(bitsOfSecond, second) || bitsOfFirst->child(1)->opcode() != bitsOfSecond->child(1)->opcode())
        return;
    const Operation* wordOfFirst = bitsOfFirst->child(0);
    const Operation* wordOfSecond = bitsOfSecond->child(0);
    if (wordOfSecond != wordOfFirst && !isReadAgainWhereNothingWasWritten(wordOfFirst, first, wordOfSecond, second))
        return;
    const Operation* testOfSecond = second->last()->child(0);
    for (const Operation* operation : second->operations()) {
        if (operation == second->last())
            continue;
        if (operation == bitsOfSecond || (!operation->useCount() && !operation->mustStay()))
            continue;
        if ((operation != testOfSecond && operation != wordOfSecond) || operation->useCount() != 1)
            return;
    }
    Block* targetOfFirst = first->successor(wayOfFirstWhereBitIsSet).block();
    Block* targetOfSecond = second->successor(wayOfSecondWhereBitIsSet).block();
    Block* wayOn = second->successor(1 - wayOfSecondWhereBitIsSet).block();
    if (targetOfFirst == second || targetOfFirst == first || targetOfSecond == second || targetOfSecond == first || wayOn == second || wayOn == first)
        return;
    if (!canGainPredecessor(targetOfFirst) || !canGainPredecessor(targetOfSecond) || !canGainPredecessor(wayOn))
        return;
    if (first->successor(wayOfFirstWhereBitIsSet).edgeClass() != second->successor(wayOfSecondWhereBitIsSet).edgeClass() || first->successor(1 - wayOfFirstWhereBitIsSet).edgeClass() != EdgeClass::Normal || second->successor(1 - wayOfSecondWhereBitIsSet).edgeClass() != EdgeClass::Normal)
        return;
    Order::askBitsOfTwoTestsAtOnce(first, wayOfFirstWhereBitIsSet, bitsOfFirst, second, wayOfSecondWhereBitIsSet, bitsOfSecond);
}

std::optional<JumpThreader::Bound> JumpThreader::boundWithinWhichTestStays(const LeavingTest& test)
{
    if (test.subject->type() != Int32)
        return std::nullopt;
    Opcode staysWhen = test.leavesWhenItHolds ? invertedComparison(test.relation) : test.relation;
    int64_t asSigned = static_cast<int32_t>(test.constant);
    int64_t asUnsigned = static_cast<uint32_t>(test.constant);
    switch (staysWhen) {
    case GreaterEqual:
        return Bound { true, true, asSigned };
    case GreaterThan:
        return Bound { true, true, asSigned + 1 };
    case LessEqual:
        return Bound { false, true, asSigned };
    case LessThan:
        return Bound { false, true, asSigned - 1 };
    case AboveEqual:
        return Bound { true, false, asUnsigned };
    case Above:
        return Bound { true, false, asUnsigned + 1 };
    case BelowEqual:
        return Bound { false, false, asUnsigned };
    case Below:
        return Bound { false, false, asUnsigned - 1 };
    default:
        return std::nullopt;
    }
}

ALWAYS_INLINE bool JumpThreader::mayBeFollowedByTestOfOtherBound(const Block* first)
{
    if (first->numSuccessors() != 2)
        return false;
    auto holdsLittle = [](const Block* block) {
        return block->numberOfIncomingEdges() == 1 && block->numSuccessors() == 2 && block->size() <= 3;
    };
    return holdsLittle(first->successor(0).block()) || holdsLittle(first->successor(1).block());
}

bool JumpThreader::leadToOnePlace(const Block* one, const Block* other)
{
    if (one == other)
        return true;
    auto onlyHandsOn = [](const Block* block) {
        return block->numberOfIncomingEdges() == 1 && block->phis().empty() && !block->isLandingPad() && !block->isEntry() && block->size() == 1 && block->last()->opcode() == Jump;
    };
    if (!onlyHandsOn(one) || !onlyHandsOn(other) || one->successor(0).block() != other->successor(0).block() || one->successor(0).edgeClass() != other->successor(0).edgeClass() || one->upsilons().size() != other->upsilons().size())
        return false;
    for (const Operation* upsilon : one->upsilons()) {
        if (incomingValue(upsilon->phi(), other) != upsilon->valueHandedOn())
            return false;
    }
    return true;
}

void JumpThreader::askBothBoundsAtOnce(Block* first)
{
    if (first->isEmpty() || first->last()->opcode() != Branch || first->numSuccessors() != 2 || !first->upsilons().empty())
        return;
    const Operation* asked = simplifyPredicateOfBranch(first->last()->child(0)).predicate;
    if (!isComparisonByNumber(asked->opcode()) || asked->child(0)->type() != Int32 || !asked->child(1)->hasInt())
        return;
    for (unsigned leavingSuccessorOfFirst = 0; leavingSuccessorOfFirst < 2; ++leavingSuccessorOfFirst) {
        Block* second = first->successor(1 - leavingSuccessorOfFirst).block();
        Block* left = first->successor(leavingSuccessorOfFirst).block();
        if (second == first || left == second || left == first || second->numberOfIncomingEdges() != 1 || second->isEmpty() || second->last()->opcode() != Branch || second->numSuccessors() != 2)
            continue;
        unsigned leavingSuccessorOfSecond = leadToOnePlace(left, second->successor(0).block()) ? 0 : 1;
        Block* inside = second->successor(1 - leavingSuccessorOfSecond).block();
        if (!leadToOnePlace(left, second->successor(leavingSuccessorOfSecond).block()) || inside == left || inside == second || inside == first || inside == second->successor(leavingSuccessorOfSecond).block())
            continue;
        std::optional<LeavingTest> firstTest = describeTestLeavingBy(first, leavingSuccessorOfFirst);
        std::optional<LeavingTest> secondTest = describeTestLeavingBy(second, leavingSuccessorOfSecond);
        if (!firstTest || !secondTest || firstTest->subject != secondTest->subject)
            continue;
        std::optional<Bound> one = boundWithinWhichTestStays(*firstTest);
        std::optional<Bound> other = boundWithinWhichTestStays(*secondTest);
        if (!one || !other || one->isLower == other->isLower)
            continue;
        const Bound& lower = one->isLower ? *one : *other;
        const Bound& upper = one->isLower ? *other : *one;
        int64_t low = lower.value;
        int64_t high = upper.value;
        if (lower.isSigned != upper.isSigned && (!lower.isSigned || low < 0 || high > std::numeric_limits<int32_t>::max()))
            continue;
        int64_t least = lower.isSigned ? std::numeric_limits<int32_t>::min() : 0;
        int64_t most = upper.isSigned ? std::numeric_limits<int32_t>::max() : std::numeric_limits<uint32_t>::max();
        if (low > high || low <= least || high >= most)
            continue;
        if (!holdsNothingButItsTest(second) || !canGainPredecessor(inside))
            continue;
        Order::askBothBoundsAtOnce(first, leavingSuccessorOfFirst, second, leavingSuccessorOfSecond, const_cast<Operation*>(firstTest->subject), static_cast<int32_t>(low), static_cast<int32_t>(high));
        return;
    }
}

ALWAYS_INLINE bool JumpThreader::isFollowedByThreeLittleTests(const Block* first)
{
    const Block* block = first;
    for (unsigned step = 0; step < 3; ++step) {
        const Block* next = nullptr;
        for (const Edge& edge : block->successors()) {
            const Block* target = edge.block();
            if (target->numberOfIncomingEdges() == 1 && target->numSuccessors() == 2 && target->size() <= 3)
                next = target;
        }
        if (!next)
            return false;
        block = next;
    }
    return true;
}

ALWAYS_INLINE bool JumpThreader::areOneValue(const Operation* one, const Operation* other)
{
    return one == other || (one->opcode() == Trunc && other->opcode() == Trunc && one->child(0) == other->child(0));
}

void JumpThreader::askEqualitiesBySwitch(Block* first)
{
    static constexpr size_t fewestEqualitiesOfSwitch = 4;
    static constexpr size_t fewestConstantsKeptInSet = 16;
    HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> constantsAsked;
    Vector<Order::CaseOfEquality, 8> cases;
    Vector<Block*, 8> laterLinks;
    const Operation* subject = nullptr;
    Edge otherwise;
    if (Block* before = first->singlePredecessor(); before && before != first && !before->isEmpty() && before->last()->opcode() == Branch && before->numSuccessors() == 2 && !first->isEmpty() && first->last()->opcode() == Branch) {
        const Operation* askedBefore = simplifyPredicateOfBranch(before->last()->child(0)).predicate;
        const Operation* asked = simplifyPredicateOfBranch(first->last()->child(0)).predicate;
        bool bothAreEqualities = (askedBefore->opcode() == Equal || askedBefore->opcode() == NotEqual) && (asked->opcode() == Equal || asked->opcode() == NotEqual);
        if (bothAreEqualities && askedBefore->child(1)->hasInt() && areOneValue(askedBefore->child(0), asked->child(0)) && holdsNothingButItsTest(first))
            return;
    }
    for (Block* block = first;;) {
        if (block->isEmpty() || block->last()->opcode() != Branch || block->numSuccessors() != 2 || !block->upsilons().empty())
            break;
        if (block != first && (block->numberOfIncomingEdges() != 1 || !holdsNothingButItsTest(block)))
            break;
        std::optional<LeavingTest> test = describeTestLeavingBy(block, 0);
        if (!test || (test->relation != Equal && test->relation != NotEqual) || test->subject->type() != Int32 || (subject && !areOneValue(test->subject, subject)))
            break;
        unsigned hit = (test->relation == Equal) == test->leavesWhenItHolds ? 0 : 1;
        Edge toHit = block->successor(hit);
        Edge toMiss = block->successor(1 - hit);
        int64_t value = static_cast<int32_t>(test->constant);
        bool isAskedAlready;
        if (cases.size() < fewestConstantsKeptInSet) {
            isAskedAlready = cases.containsIf([&](const Order::CaseOfEquality& one) {
                return one.value == value;
            });
        } else {
            if (constantsAsked.isEmpty()) {
                for (const Order::CaseOfEquality& one : cases)
                    constantsAsked.add(static_cast<uint32_t>(one.value));
            }
            isAskedAlready = !constantsAsked.add(static_cast<uint32_t>(value)).isNewEntry;
        }
        if (isAskedAlready || (!cases.isEmpty() && !leadToOnePlace(cases[0].target.block(), toHit.block())) || !canGainPredecessor(toHit.block()) || toHit.block() == first || toMiss.block() == first || toHit.block() == block || toMiss.block() == block)
            break;
        if (!subject)
            subject = test->subject;
        cases.append({ value, toHit });
        if (block != first)
            laterLinks.append(block);
        otherwise = toMiss;
        if (toMiss.edgeClass() != EdgeClass::Normal)
            break;
        block = toMiss.block();
    }
    if (cases.size() < fewestEqualitiesOfSwitch || !canGainPredecessor(otherwise.block()))
        return;
    std::ranges::sort(cases, { }, &Order::CaseOfEquality::value);
    Order::askEqualitiesBySwitch(first, const_cast<Operation*>(subject), laterLinks.span(), cases.span(), otherwise);
}

void JumpThreader::askTestsBehindNextThatImplyThem()
{
    if (m_testsOfBranchesAreInOrder) [[likely]] {
        for (const TestOfBranch& test : m_testsOfBranches) {
            if (test.opcode && test.opcode != BitAnd + 1) {
                if (mayBeFollowedByTestOfOtherBound(m_proc.blockAt(test.block))) {
                    if (test.opcode != Equal + 1)
                        askBothBoundsAtOnce(m_proc.blockAt(test.block));
                    else if (isFollowedByThreeLittleTests(m_proc.blockAt(test.block)))
                        askEqualitiesBySwitch(m_proc.blockAt(test.block));
                }
                continue;
            }
            Block* block = m_proc.blockAt(test.block);
            if (mayAskTestBehindNext(block))
                askTestBehindNextThatImpliesIt(block);
            if (test.opcode && mayEndInTestOfBitsOf32Bits(block) && mayEndInTestOfBitsOf32Bits(block->successor(test.isInverted ? 0 : 1).block()))
                askBitsOfNextTestAtOnce(block);
        }
        return;
    }
    for (unsigned index = 0, numberOfBlocks = m_proc.numberOfBlocks(); index < numberOfBlocks; ++index) {
        Block* block = m_proc.blockAt(index);
        if (mayAskTestBehindNext(block))
            askTestBehindNextThatImpliesIt(block);
        askBitsOfNextTestAtOnce(block);
        askBothBoundsAtOnce(block);
        askEqualitiesBySwitch(block);
    }
}

void JumpThreader::run()
{
    uint32_t numberOfBlocks = m_proc.numberOfBlocks();
    m_flags.fill(m_arena, numberOfBlocks, 0);
    m_testsOfBranches.reserve(m_arena, numberOfBlocks);
    Block* const* blocks = m_proc.blocks().data();
    for (Block* block : m_proc.blocksBegunBeforeAnythingLedToThem())
        letGoUnlessSomethingLeadsTo(block);
    if (m_proc.traits().checksAllocation) {
        for (uint32_t indexOfBlock = 0; indexOfBlock < numberOfBlocks; ++indexOfBlock)
            RELEASE_ASSERT(blocks[indexOfBlock]->isEmpty() || blocks[indexOfBlock]->numberOfIncomingEdges() || blocks[indexOfBlock]->isEntry());
    }
    for (uint32_t indexOfBlock = 0; indexOfBlock < numberOfBlocks; ++indexOfBlock) {
        if (!m_blocksInHandThatNothingLeadsTo.isEmpty()) [[unlikely]]
            letGoOfBlocksThatWereInHand();
        m_indexOfBlockInTurn = indexOfBlock;
        Block* block = blocks[indexOfBlock];
        ASSERT(block->index() == indexOfBlock);
        uint32_t size = block->size();
        if (!size)
            continue;
        const Operation* last = block->operations().data()[size - 1];
        Opcode opcode = last->opcode();
        if (opcode == Jump) [[likely]] {
            if (size == 1) {
                if (m_flags[indexOfBlock] & (IsBeingForwarded | IsForwarded))
                    continue;
                if (block->phis().empty() && !block->isLandingPad() && !block->isEntry()) {
                    forwardEmptyJumpChain(block);
                    continue;
                }
            }
            if (block->upsilons().size() == 1 && block->upsilons().data()[0]->valueHandedOn()->opcode() == Add) [[unlikely]]
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
    letGoOfBlocksThatWereInHand();
    if (m_testsOfBranches.size() < 2)
        return;
    m_isWalkingWays = true;
    threadEdgesPastDecidedTests();
    letGoOfBlocksThatWereInHand();
    askTestsBehindNextThatImplyThem();
}

void JumpThreader::letGoOfLists()
{
    if (m_testsOfBranches.capacity() * sizeof(TestOfBranch) < Arena::smallestRoomLetGo)
        return;
    m_depthsOfAnswers.letGo(m_arena);
    m_answersOnWay.letGo(m_arena);
    m_testsOfBlocks.letGo(m_arena);
    m_joinsPassedBy.letGo(m_arena);
    m_blocksInHandThatNothingLeadsTo.letGo(m_arena);
    m_blocksToLetGo.letGo(m_arena);
    m_joinsThatJump.letGo(m_arena);
    m_chain.letGo(m_arena);
    m_matchesOfJoins.letGo(m_arena);
    m_testsOfBranches.letGo(m_arena);
    m_flags.letGo(m_arena);
}

} // anonymous namespace

void threadJumps(Procedure& procedure)
{
    if (procedure.failure())
        return;
    if (procedure.numberOfBlocks() == 1 && !procedure.blockAt(0)->numSuccessors())
        return;
    JumpThreader threader(procedure);
    threader.run();
    threader.letGoOfLists();
}

} } } // namespace JSC::AOT::Machine

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
