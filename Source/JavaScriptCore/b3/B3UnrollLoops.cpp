/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "B3UnrollLoops.h"

#if ENABLE(B3_JIT)

#include "B3BasicBlockInlines.h"
#include "B3ConstDoubleValue.h"
#include "B3DominatingConditions.h"
#include "B3Dominators.h"
#include "B3IntRange.h"
#include "B3Limits.h"
#include "B3LoopUtilities.h"
#include "B3MemoryValue.h"
#include "B3NaturalLoops.h"
#include "B3PatchpointValue.h"
#include "B3PhaseScope.h"
#include "B3PhiChildren.h"
#include "B3ProcedureInlines.h"
#include "B3RegionCloner.h"
#include "B3UpsilonValue.h"
#include "B3ValueInlines.h"
#include "B3VariableValue.h"
#include <limits>
#include <optional>
#include <wtf/RangeSet.h>

namespace JSC { namespace B3 {

namespace {

class UnrollLoops {
public:
    UnrollLoops(Procedure& proc, WritesOutTurns writesOutTurns)
        : m_proc(proc)
        , m_writesOutTurns(writesOutTurns)
    {
    }

    bool run()
    {
        NaturalLoops& loops = m_proc.naturalLoops();
        for (unsigned loopIndex = loops.numLoops(); loopIndex--;) {
            unsigned numberOfPlans = m_plans.size();
            m_pollsOfLoopCanMove = false;
            if (m_writesOutTurns == WritesOutTurns::Yes)
                considerLoop(loops, loops.loop(loopIndex));
            if (m_plans.size() == numberOfPlans)
                considerLeavingPollToCopy(loops, loops.loop(loopIndex));
        }
        m_phiChildren = std::nullopt;
        if (m_plans.isEmpty() && m_plansOfCopiesThatPoll.isEmpty())
            return false;

        RegionCloner cloner(m_proc);
        for (const Plan& plan : m_plans) {
            for (BasicBlock* block : plan.blocks)
                cloner.addBlock(block);
            for (BasicBlock* block : plan.blocksBehindRareEdges)
                cloner.addBlock(block);
        }
        for (const Plan& plan : m_plansOfCopiesThatPoll) {
            for (BasicBlock* block : plan.blocks)
                cloner.addBlock(block);
        }
        for (Plan& plan : m_plans)
            nameEndsInPreHeader(cloner, plan);
        for (Plan& plan : m_plansOfCopiesThatPoll)
            nameEndsInPreHeader(cloner, plan);
        cloner.demoteEscapingValues();
        m_proc.resetValueOwners();
        for (const Plan& plan : m_plans)
            unroll(cloner, plan);
        for (const Plan& plan : m_plansOfCopiesThatPoll)
            leavePollToCopy(cloner, plan);
        return cloner.finish();
    }

private:
    // sum = phi + IToD(integer of 32 bits), or phi - it, and nothing in the loop looks at either.
    struct SumOfIntegers {
        Value* phi { nullptr };
        Value* start { nullptr };
        Value* sum { nullptr };
        Value* summand { nullptr };
    };

    struct Plan {
        Vector<BasicBlock*, 4> blocks;
        BasicBlock* preHeader { nullptr };
        BasicBlock* header { nullptr };
        BasicBlock* test { nullptr };
        unsigned stayingSuccessorIndex { 0 };
        Vector<BasicBlock*, 2> blocksBehindTest;
        Opcode relation { Oops };
        Value* tested { nullptr };
        // Which operand of the comparison it is, where the test branches on the comparison itself.
        std::optional<unsigned> testedOperandOfBranch;
        bool testsCounterBeforeStep { false };
        Value* start { nullptr };
        Value* bound { nullptr };
        int64_t step { 0 };
        unsigned factor { 0 };
        bool needsGuardOfFarBound { false };
        int64_t farthestBoundWithRoom { 0 };
        bool startHasRoom { false };
        Vector<SumOfIntegers, 1> sums;
        // They ask what the test at the top has asked, of the same two values.
        struct RepeatedTest {
            BasicBlock* block { nullptr };
            unsigned stayingSuccessorIndex { 0 };
        };
        Vector<RepeatedTest, 2> repeatedTests;
        Value* counter { nullptr };
        // A side path that ends its turn by itself has a copy of the test at the bottom.
        struct SideTest {
            BasicBlock* block { nullptr };
            unsigned stayingSuccessorIndex { 0 };
            Value* tested { nullptr };
            Vector<BasicBlock*, 2> blocksBehindTest;
        };
        Vector<SideTest, 1> sideTests;
        // With side paths: every value that is the counter of the next turn. Each way through a turn has its own.
        Vector<Value*, 2> nextCounters;
        // What lies behind the rare edges, and the rest of the turn behind that.
        Vector<BasicBlock*, 4> blocksBehindRareEdges;
        // Set where the loop polls between stretches of turns instead of on every turn.
        PatchpointValue* poll { nullptr };
        // The turns that are written out poll once together and run in no stretches: only the loop itself, which takes fewer turns than are written out, goes without its poll.
        bool pollsOnceForAllWrittenOutTurns { false };
    };

    struct Counter {
        Value* phi { nullptr };
        Value* start { nullptr };
        Value* next { nullptr };
        int64_t step { 0 };
    };

    // What is asked in front of a loop names where the loop starts and where it ends. A block that is copied may compute that: a user in the pre-header makes it a value that leaves the block.
    void nameEndsInPreHeader(const RegionCloner& cloner, Plan& plan)
    {
        auto name = [&](Value*& value) {
            if (!cloner.contains(value->owner))
                return;
            Value* named = value->isConstant() ? m_proc.clone(value) : m_proc.add<Value>(Identity, value->origin(), value);
            plan.preHeader->appendNonTerminal(named);
            value = named;
        };
        name(plan.start);
        name(plan.bound);
        for (SumOfIntegers& sum : plan.sums)
            name(sum.start);
    }

    // A word of 64 bits in memory is a pointer or a value of any type. Lists of those are mostly short, and most loops that store them copy or fill one.
    static bool storesWord(const Value* value)
    {
        auto* memory = value->as<MemoryValue>();
        return memory && memory->opcode() == Store && memory->child(0)->type() == Int64;
    }

    bool canPollBetweenStretches(const Value* value) const { return m_pollsOfLoopCanMove && B3::canPollBetweenStretches(m_proc, value); }

    bool countsTowardsSize(const Value* value) const
    {
        if (!emitsInstructions(value) || onlyKeepsValuesAlive(value) || canPollBetweenStretches(value))
            return false;
        return value->opcode() != Branch || !value->child(0)->hasInt();
    }

    static std::optional<int64_t> stepOf(Value* next, Value* phi)
    {
        if (next->opcode() == Add && next->child(0) == phi && next->child(1)->hasInt())
            return next->child(1)->asInt();
        if (next->opcode() == Add && next->child(1) == phi && next->child(0)->hasInt())
            return next->child(0)->asInt();
        if (next->opcode() == Sub && next->child(0) == phi && next->child(1)->hasInt() && next->child(1)->asInt() != std::numeric_limits<int64_t>::min())
            return -next->child(1)->asInt();
        return std::nullopt;
    }

    static std::optional<Counter> counterFor(const NaturalLoops& loops, const NaturalLoop& loop, const IndexSet<BasicBlock*>& likelyBlocks, BasicBlock* preHeader, PhiChildren& phiChildren, Value* phi)
    {
        Counter counter;
        counter.phi = phi;
        for (UpsilonValue* upsilon : phiChildren[phi]) {
            Value* input = upsilon->child(0)->foldIdentity();
            if (upsilon->owner == preHeader) {
                if (counter.start)
                    return std::nullopt;
                counter.start = input;
                continue;
            }
            if (!loops.belongsTo(upsilon->owner, loop))
                return std::nullopt;
            if (!likelyBlocks.contains(upsilon->owner))
                continue;
            if (counter.next && counter.next != input)
                return std::nullopt;
            counter.next = input;
        }
        if (!counter.start || !counter.next)
            return std::nullopt;
        std::optional<int64_t> step = stepOf(counter.next, phi);
        if (!step || !*step || *step > Limits::maxCountedLoopStep || *step < -Limits::maxCountedLoopStep)
            return std::nullopt;
        counter.step = *step;
        return counter;
    }

    // The turns that are left begin at the header of the loop itself, so that it stays a loop with one way in. A test at the top is made there again.
    static bool canRepeatUpToTest(const Plan& plan)
    {
        unsigned numberOfBlocks = 0;
        for (BasicBlock* block = plan.header;; block = block->successorBlock(0)) {
            for (Value* value : *block) {
                if (isLoopPoll(value))
                    continue;
                Effects effects = value->effects();
                auto* upsilon = value->as<UpsilonValue>();
                if (effects.writes || effects.writesPinned || effects.exitsSideways || effects.fence || (upsilon && upsilon->phi()->owner == plan.header))
                    return false;
            }
            if (block == plan.test)
                return true;
            if (block->last()->opcode() != Jump || ++numberOfBlocks > plan.blocks.size())
                return false;
        }
    }

    // Behind a test at the bottom nothing is left of the turn but what hands values to the next.
    static bool findBlocksBehindTest(const NaturalLoops& loops, const NaturalLoop& loop, BasicBlock* test, unsigned stayingSuccessorIndex, Vector<BasicBlock*, 2>& blocksBehindTest)
    {
        blocksBehindTest.clear();
        for (BasicBlock* block = test->successorBlock(stayingSuccessorIndex); block != loop.header(); block = block->successorBlock(0)) {
            if (!loops.belongsTo(block, loop) || block->numPredecessors() != 1 || block->last()->opcode() != Jump || blocksBehindTest.size() == Limits::maxBlocksBesideTestOfUnrolledLoop)
                return false;
            for (Value* value : *block) {
                if (emitsInstructions(value))
                    return false;
            }
            blocksBehindTest.append(block);
        }
        return true;
    }

    enum class TurnsAre : bool { Counted, WrittenOut };

    bool readTest(const NaturalLoops& loops, const NaturalLoop& loop, const IndexSet<BasicBlock*>& likelyBlocks, BasicBlock* block, PhiChildren& phiChildren, Plan& plan, TurnsAre turnsAre = TurnsAre::WrittenOut)
    {
        if (block->last()->opcode() != Branch)
            return false;
        bool firstStays = loops.belongsTo(block->successorBlock(0), loop);
        if (firstStays == loops.belongsTo(block->successorBlock(1), loop) || !likelyBlocks.contains(block->successorBlock(firstStays ? 0 : 1)))
            return false;
        DominatingCondition condition = stripNegations(block->last()->child(0)->foldIdentity());
        Value* comparison = condition.value->foldIdentity();
        std::optional<Opcode> inverted = invertedRelation(comparison->opcode());
        if (!inverted || !comparison->child(0)->type().isInt())
            return false;
        Opcode relation = firstStays == condition.isNonZero ? comparison->opcode() : *inverted;

        for (unsigned testedIndex = 0; testedIndex < 2; ++testedIndex) {
            Value* tested = comparison->child(testedIndex)->foldIdentity();
            Value* bound = comparison->child(1 - testedIndex)->foldIdentity();
            if (!bound->hasInt() && loops.belongsTo(bound->owner, loop))
                continue;
            Value* phi = tested;
            if (phi->opcode() != Phi) {
                if (phi->numChildren() != 2)
                    continue;
                phi = phi->child(phi->child(0)->hasInt() ? 1 : 0);
            }
            if (phi->opcode() != Phi || phi->owner != loop.header())
                continue;
            std::optional<Counter> counter = counterFor(loops, loop, likelyBlocks, plan.preHeader, phiChildren, phi);
            if (!counter || (tested != counter->phi && tested != counter->next))
                continue;
            plan.relation = testedIndex ? flippedRelation(relation) : relation;
            if (isLessRelation(plan.relation) != (counter->step > 0))
                continue;
            plan.test = block;
            plan.stayingSuccessorIndex = firstStays ? 0 : 1;
            plan.counter = counter->phi;
            plan.tested = tested;
            plan.testedOperandOfBranch = block->last()->child(0) == comparison ? std::optional<unsigned>(testedIndex) : std::nullopt;
            plan.testsCounterBeforeStep = tested == counter->phi;
            plan.start = counter->start;
            plan.bound = bound;
            plan.step = counter->step;
            if (turnsAre == TurnsAre::Counted)
                return true;
            if (plan.testsCounterBeforeStep)
                return canRepeatUpToTest(plan);
            return findBlocksBehindTest(loops, loop, plan.test, plan.stayingSuccessorIndex, plan.blocksBehindTest);
        }
        return false;
    }

    unsigned factorFor(unsigned numberOfValues, Origin origin)
    {
        for (unsigned factor = Limits::maxTurnsPerUnrolledLoop; factor >= Limits::minTurnsPerUnrolledLoop; factor /= 2) {
            if (numberOfValues * factor <= m_proc.limit(Limit::MaxValuesPerUnrolledLoop))
                return factor;
            m_proc.didReachLimit(Limit::MaxValuesPerUnrolledLoop, origin);
        }
        return 0;
    }

    // The bound that leaves room for the other turns must not wrap around, and some start must reach it.
    bool planFarBound(Plan& plan)
    {
        Signedness signedness = isUnsignedRelation(plan.relation) ? Signedness::Unsigned : Signedness::Signed;
        Int128 room = Int128 { plan.step } * (plan.factor - 1);
        IntegerInterval full = fullInterval(plan.bound->type(), signedness);
        IntegerInterval bound = intervalFor(m_proc, plan.bound, signedness);
        Int128 farthest = room > 0 ? full.min + room : full.max + room;
        plan.needsGuardOfFarBound = room > 0 ? bound.min < farthest : bound.max > farthest;
        plan.farthestBoundWithRoom = static_cast<int64_t>(farthest);
        if (room > 0 ? bound.max < farthest : bound.min > farthest)
            return false;
        IntegerInterval farBound { std::max(bound.min - room, full.min), std::min(bound.max - room, full.max) };
        IntegerInterval start = intervalFor(m_proc, plan.start, signedness);
        plan.startHasRoom = start.min + room >= full.min && start.max + room <= full.max;
        return evaluateComparison(plan.relation, start, farBound) != TriState::False;
    }

    // How far the counter goes from the first of the turns that are written out to the last.
    int64_t roomForTurnsBehindFirst(const Plan& plan) const
    {
        return plan.step * static_cast<int64_t>(plan.factor - (m_proc.policy().validation.leavesRoomForOneTurnTooFew ? 2 : 1));
    }

    // Two reads of one place in front of the loop, with nothing between them that writes there.
    bool readsWhatBoundRead(Value* value, Value* bound)
    {
        MemoryValue* load = value->as<MemoryValue>();
        MemoryValue* other = bound->as<MemoryValue>();
        if (!load || !other || !load->isLoad() || load->hasFence() || load->kind() != other->kind() || load->type() != other->type() || load->offset() != other->offset() || load->range() != other->range())
            return false;
        if (!computesSameValue(load->lastChild(), other->lastChild(), Limits::maxDepthOfAddressInUnrolledLoop))
            return false;
        auto nothingWritesBetween = [&](MemoryValue* first, MemoryValue* second) {
            unsigned numberOfBlocks = 0;
            for (BasicBlock* block = second->owner;; block = block->predecessor(0)) {
                for (Value* between : *block) {
                    if ((between->effects().writes.overlaps(first->range()) || between->effects().fence) && !m_proc.policy().validation.overlooksWritesBetweenReadsOfBound)
                        return false;
                }
                if (block == first->owner)
                    return true;
                if (block->numPredecessors() != 1 || ++numberOfBlocks > 2 * Limits::maxBlocksBesideTestOfUnrolledLoop)
                    return false;
            }
        };
        auto readsWhatWasRead = [&](MemoryValue* first, MemoryValue* second) {
            size_t index = second->owner->values().find(second);
            if ((first->owner == second->owner && first->owner->values().find(first) > index) || !nothingWritesBetween(first, second))
                return false;
            if (std::optional<MovedLoad> moved = MovedLoad::of(m_proc, second))
                m_proc.movedLoadLog().add(*moved, second->owner, static_cast<unsigned>(index), second->lastChild(), first);
            return true;
        };
        return readsWhatWasRead(load, other) || readsWhatWasRead(other, load);
    }

    // A range check against the bound of the loop is such a test. In the loop itself it falls to the test above it; where the turns are written out that test is gone.
    void findRepeatedTests(Plan& plan)
    {
        if (!plan.testsCounterBeforeStep)
            return;
        auto isBound = [&](Value* value) {
            return value == plan.bound || readsWhatBoundRead(value, plan.bound);
        };
        for (BasicBlock* block : plan.blocks) {
            if (block == plan.test || block->last()->opcode() != Branch)
                continue;
            DominatingCondition condition = stripNegations(block->last()->child(0)->foldIdentity());
            Value* comparison = condition.value->foldIdentity();
            if (comparison->numChildren() != 2 || !invertedRelation(comparison->opcode()))
                continue;
            Value* left = comparison->child(0)->foldIdentity();
            Value* right = comparison->child(1)->foldIdentity();
            Opcode relation = comparison->opcode();
            if (right == plan.tested) {
                std::swap(left, right);
                relation = flippedRelation(relation);
            }
            if (left != plan.tested || !isBound(right))
                continue;
            if (relation == plan.relation)
                plan.repeatedTests.append({ block, condition.isNonZero ? 0u : 1u });
            else if (relation == *invertedRelation(plan.relation))
                plan.repeatedTests.append({ block, condition.isNonZero ? 1u : 0u });
        }
    }

    // A loop that looks for something is expected to find it.
    static bool isLikelyToLeaveOnlyAtTest(const IndexSet<BasicBlock*>& likelyBlocks, const Plan& plan)
    {
        for (BasicBlock* block : plan.blocks) {
            if (block == plan.test)
                continue;
            for (const FrequentedBlock& successor : block->successors()) {
                if (isLikelySuccessor(block, successor) && !likelyBlocks.contains(successor.block()))
                    return false;
            }
        }
        return true;
    }

    // Few loops get as far as this asks for what takes a walk over the whole procedure.
    PhiChildren& prepareToReadCounters()
    {
        if (!m_phiChildren) {
            m_proc.updateFrequencies();
            m_proc.resetValueOwners();
            m_phiChildren.emplace(m_proc);
        }
        return *m_phiChildren;
    }

    static Value* summandOf(Value* sum, Value* phi)
    {
        auto isConvertedInteger = [](Value* value) {
            return value->opcode() == IToD && value->child(0)->type() == Int32;
        };
        if (sum->type() != Double || (sum->opcode() != Add && sum->opcode() != Sub))
            return nullptr;
        if (sum->child(0) == phi && isConvertedInteger(sum->child(1)))
            return sum->child(1);
        if (sum->opcode() == Add && sum->child(1) == phi && isConvertedInteger(sum->child(0)))
            return sum->child(0);
        return nullptr;
    }

    // A fast version still holds the paths behind the comparisons that were decided for it. Nothing takes them.
    static bool isNeverTaken(const BasicBlock* block, unsigned successorIndex)
    {
        Value* condition = block->last()->opcode() == Branch ? block->last()->child(0) : nullptr;
        return condition && condition->hasInt() && successorIndex == (condition->asInt() ? 1u : 0u);
    }

    // While a sum of whole numbers stays under 2^53 every addition of doubles is exact, so the turns that are written out may add integers.
    // The sum is made a double again where they are left, which is at the test alone.
    void findSumsOfIntegers(PhiChildren& phiChildren, Plan& plan)
    {
        if (plan.testsCounterBeforeStep)
            return;
        IndexSet<BasicBlock*> blocksNeverEntered;
        for (BasicBlock* block : plan.blocks) {
            for (unsigned index = 0; index < block->numSuccessors(); ++index) {
                BasicBlock* successor = block->successorBlock(index);
                if (plan.blocks.contains(successor) || (block == plan.test && index != plan.stayingSuccessorIndex))
                    continue;
                if (!isNeverTaken(block, index) || successor->numPredecessors() != 1)
                    return;
                blocksNeverEntered.add(successor);
            }
        }
        for (Value* phi : *plan.header) {
            if (phi->opcode() != Phi || phi->type() != Double)
                continue;
            SumOfIntegers sum;
            sum.phi = phi;
            bool isSum = true;
            for (UpsilonValue* upsilon : phiChildren[phi]) {
                Value* input = upsilon->child(0)->foldIdentity();
                Value*& place = upsilon->owner == plan.preHeader ? sum.start : sum.sum;
                isSum &= (upsilon->owner == plan.preHeader || plan.blocks.contains(upsilon->owner)) && (!place || (place == input && place == sum.sum));
                place = input;
            }
            if (!isSum || !sum.start || !sum.sum || !plan.blocks.contains(sum.sum->owner))
                continue;
            sum.summand = summandOf(sum.sum, phi);
            if (!sum.summand || !plan.blocks.contains(sum.summand->owner))
                continue;
            plan.sums.append(sum);
        }
        if (plan.sums.isEmpty())
            return;
        for (Value* value : m_proc.values()) {
            if (blocksNeverEntered.contains(value->owner))
                continue;
            for (Value* child : value->children()) {
                plan.sums.removeAllMatching([&](const SumOfIntegers& sum) {
                    if (child == sum.phi)
                        return value != sum.sum;
                    return child == sum.sum && value->opcode() != Upsilon && plan.blocks.contains(value->owner);
                });
            }
        }
    }

    static bool changesFromTurnToTurn(const Plan& plan, Value* value, unsigned timeToLive)
    {
        if (!plan.blocks.contains(value->owner))
            return false;
        if (value->opcode() == Phi)
            return value->owner == plan.header;
        if (!timeToLive || value->as<MemoryValue>())
            return false;
        for (Value* child : value->children()) {
            if (changesFromTurnToTurn(plan, child, timeToLive - 1))
                return true;
        }
        return false;
    }

    // Whether the address goes on by the same distance from turn to turn, stays, or comes out of memory.
    static bool goesOnEvenly(const Plan& plan, Value* address, unsigned timeToLive)
    {
        address = address->foldIdentity();
        if (address == plan.counter || address->isConstant() || address->as<MemoryValue>() || !plan.blocks.contains(address->owner))
            return true;
        if (!timeToLive)
            return false;
        switch (address->opcode()) {
        case Add:
        case Sub:
            return goesOnEvenly(plan, address->child(0), timeToLive - 1) && goesOnEvenly(plan, address->child(1), timeToLive - 1);
        case Shl:
            return address->child(1)->hasInt() && goesOnEvenly(plan, address->child(0), timeToLive - 1);
        case ZExt32:
        case SExt32:
            return goesOnEvenly(plan, address->child(0), timeToLive - 1);
        default:
            return false;
        }
    }

    // A loop that reads pointers out of a list and goes on to their fields is written out where it walks the list. One that picks among a few elements of it is as a rule about what it does with them.
    static bool readsWordsOnlyWhereItWalks(const Plan& plan)
    {
        for (BasicBlock* block : plan.blocks) {
            for (Value* value : *block) {
                MemoryValue* memory = value->as<MemoryValue>();
                if (memory && memory->isLoad() && memory->type() == Int64 && !goesOnEvenly(plan, memory->lastChild(), Limits::maxDepthOfAddressInUnrolledLoop))
                    return false;
            }
        }
        return true;
    }

    // The turns that are written out share one counter: each reads or writes at its own distance from it.
    static bool walksData(const Plan& plan)
    {
        for (BasicBlock* block : plan.blocks) {
            for (Value* value : *block) {
                if (value->as<MemoryValue>() && changesFromTurnToTurn(plan, value->lastChild(), Limits::maxDepthOfAddressInUnrolledLoop))
                    return true;
            }
        }
        return false;
    }

    static bool computesSameValue(Value* left, Value* right, unsigned timeToLive)
    {
        left = left->foldIdentity();
        right = right->foldIdentity();
        if (left == right)
            return true;
        if (!timeToLive || left->kind() != right->kind() || left->type() != right->type() || left->numChildren() != right->numChildren())
            return false;
        if (left->isConstant())
            return left->key() == right->key();
        if (!left->numChildren() || !left->key() || left->as<MemoryValue>())
            return false;
        for (unsigned index = 0; index < left->numChildren(); ++index) {
            if (!computesSameValue(left->child(index), right->child(index), timeToLive - 1))
                return false;
        }
        return true;
    }

    // Answers how many reads the later walk over all values is likely to take away.
    unsigned shareAddressesOfRepeatedLoads(const Plan& plan)
    {
        RangeSet<HeapRange> likelyWritten;
        bool hasFence = false;
        Vector<MemoryValue*, 8> loads;
        for (BasicBlock* block : plan.blocks) {
            for (Value* value : *block) {
                Effects effects = value->effects();
                hasFence |= effects.fence;
                effects.writes.addTo(likelyWritten);
                MemoryValue* memory = value->as<MemoryValue>();
                if (memory && memory->isLoad() && !memory->hasFence())
                    loads.append(memory);
            }
        }
        Dominators& dominators = m_proc.dominators();
        unsigned numberOfRepeatedLoads = 0;
        for (MemoryValue* load : loads) {
            for (MemoryValue* earlier : loads) {
                if (earlier == load || earlier->kind() != load->kind() || earlier->type() != load->type())
                    continue;
                if (!computesSameValue(earlier->lastChild(), load->lastChild(), Limits::maxDepthOfAddressInUnrolledLoop) || earlier->offset() != load->offset() || earlier->range() != load->range())
                    continue;
                bool comesFirst = earlier->owner == load->owner ? earlier->owner->values().find(earlier) < load->owner->values().find(load) : dominators.strictlyDominates(earlier->owner, load->owner);
                if (!comesFirst)
                    continue;
                load->lastChild() = earlier->lastChild();
                numberOfRepeatedLoads += !hasFence && !likelyWritten.overlaps(load->range());
                break;
            }
        }
        return numberOfRepeatedLoads;
    }

    void considerLoop(NaturalLoops& loops, const NaturalLoop& loop)
    {
        if (!isInnermostLoop(loops, loop))
            return;
        Plan plan;
        plan.header = loop.header();
        plan.preHeader = findPreHeader(loops, loop);
        if (!plan.preHeader)
            return;
        m_pollsOfLoopCanMove = true;

        // Only the likely blocks are written out. A rare edge leads into the loop itself, which takes the turns that are left.
        IndexSet<BasicBlock*> likelyBlocks = findLikelyBlocks(loops, loop);
        if (!hasLikelyBackEdge(loop, likelyBlocks))
            return;
        unsigned numberOfValues = 0;
        for (unsigned i = loop.size(); i--;) {
            BasicBlock* block = loop.at(i);
            if (!likelyBlocks.contains(block))
                continue;
            if (block->last()->type() != Void)
                return;
            for (Value* value : *block) {
                if (!canCloneForLoop(value) || (isCall(value) && !isLoopPoll(value)) || storesWord(value))
                    return;
                numberOfValues += countsTowardsSize(value);
            }
            plan.blocks.append(block);
        }
        if (numberOfValues * Limits::minTurnsPerUnrolledLoop > m_proc.limit(Limit::MaxValuesPerUnrolledLoop)) {
            m_proc.didReachLimit(Limit::MaxValuesPerUnrolledLoop, plan.header->at(0)->origin());
            return;
        }
        PhiChildren& phiChildren = prepareToReadCounters();
        if (plan.header->frequency() < m_proc.policy().loops.minFrequencyOfUnrolledLoop)
            return;
        numberOfValues -= shareAddressesOfRepeatedLoads(plan);
        Dominators& dominators = m_proc.dominators();
        auto runsEveryTurn = [&](BasicBlock* block) {
            for (BasicBlock* predecessor : plan.header->predecessors()) {
                if (likelyBlocks.contains(predecessor) && !dominators.dominates(block, predecessor))
                    return false;
            }
            return true;
        };
        for (BasicBlock* block : plan.blocks) {
            if (!runsEveryTurn(block) || !readTest(loops, loop, likelyBlocks, block, phiChildren, plan))
                continue;
            if (!isLikelyToLeaveOnlyAtTest(likelyBlocks, plan) || !readsWordsOnlyWhereItWalks(plan))
                return;
            findRepeatedTests(plan);
            auto chooseFactor = [&](Plan& plan, unsigned numberOfValues) {
                plan.factor = factorFor(numberOfValues - std::min<unsigned>(numberOfValues, 2 * plan.repeatedTests.size()), block->last()->origin());
                return plan.factor && planFarBound(plan);
            };
            if (!chooseFactor(plan, numberOfValues))
                return;
            findSumsOfIntegers(phiChildren, plan);
            // A loop that walks nothing and sums nothing up would only test less often.
            if (plan.sums.isEmpty() && !walksData(plan))
                return;
            if (plan.sums.isEmpty()) {
                Plan withSidePaths = plan;
                unsigned numberOfValuesWithSidePaths = numberOfValues;
                if (addSidePaths(loops, loop, likelyBlocks, phiChildren, withSidePaths, numberOfValuesWithSidePaths) && chooseFactor(withSidePaths, numberOfValuesWithSidePaths))
                    plan = WTF::move(withSidePaths);
            }
            findBlocksBehindRareEdges(loops, loop, plan);
            findPoll(loops, loop, plan);
            m_plans.append(WTF::move(plan));
            return;
        }
    }

    enum class RareEdges : bool { AreFollowed, EndTheWalk };

    static IndexSet<BasicBlock*> findBlocksThatCanRun(const NaturalLoops& loops, const NaturalLoop& loop, RareEdges rareEdges)
    {
        IndexSet<BasicBlock*> result;
        Vector<BasicBlock*, 8> worklist { loop.header() };
        result.add(loop.header());
        while (!worklist.isEmpty()) {
            BasicBlock* block = worklist.takeLast();
            for (unsigned i = block->numSuccessors(); i--;) {
                const FrequentedBlock& successor = block->successor(i);
                if (isNeverTaken(block, i))
                    continue;
                if (successor.isRare() && rareEdges == RareEdges::EndTheWalk)
                    continue;
                if (loops.belongsTo(successor.block(), loop) && result.add(successor.block()))
                    worklist.append(successor.block());
            }
        }
        return result;
    }

    static IndexSet<BasicBlock*> findBlocksInFrontOfRareEdges(const NaturalLoops& loops, const NaturalLoop& loop)
    {
        return findBlocksThatCanRun(loops, loop, RareEdges::EndTheWalk);
    }

    // A loop that seldom ends by the test of its counter and often by another test ends after who knows how few turns: asking in front of it would cost more than its polls.
    static bool endsMostlyElsewhere(const NaturalLoops& loops, const NaturalLoop& loop, const Plan& plan, BasicBlock* test)
    {
        bool testEndsLoopOften = false;
        bool anotherTestEndsLoopOften = false;
        for (BasicBlock* block : plan.blocks) {
            for (const FrequentedBlock& successor : block->successors()) {
                if (loops.belongsTo(successor.block(), loop) || successor.frequency() != FrequencyClass::Normal)
                    continue;
                if (block == test)
                    testEndsLoopOften = true;
                else
                    anotherTestEndsLoopOften = true;
            }
        }
        return !testEndsLoopOften && anotherTestEndsLoopOften;
    }

    // A loop that goes on where the counter of a loop around it stands is that loop without some of its paths. Where such a path is not rare, the loop may be left and entered
    // again on every turn, and each time it would ask anew how many turns are left.
    static bool mayBeEnteredOnAnyTurnOfLoopAround(const NaturalLoops& loops, const NaturalLoop& loop, const Plan& plan)
    {
        Value* start = plan.start->foldIdentity();
        if (start->opcode() != Phi)
            return false;
        for (const NaturalLoop* outer = loops.innerMostOuterLoop(loop); outer; outer = loops.innerMostOuterLoop(*outer)) {
            if (start->owner != outer->header())
                continue;
            for (BasicBlock* block : plan.blocks) {
                if (block == plan.test)
                    continue;
                for (const FrequentedBlock& successor : block->successors()) {
                    if (!successor.isRare() && !loops.belongsTo(successor.block(), loop) && loops.belongsTo(successor.block(), *outer))
                        return true;
                }
            }
        }
        return false;
    }

    // A loop that is not written out has one turn to copy. The loop goes without its poll where few enough turns lie in front of it, and a copy that polls takes the others.
    // Both hold what lies behind the rare edges too: whoever takes such an edge comes back into the loop that he left.
    void considerLeavingPollToCopy(NaturalLoops& loops, const NaturalLoop& loop)
    {
        m_pollsOfLoopCanMove = true;
        if (!isInnermostLoop(loops, loop))
            return;
        Plan plan;
        plan.header = loop.header();
        plan.preHeader = findPreHeader(loops, loop);
        if (!plan.preHeader)
            return;
        IndexSet<BasicBlock*> blocksThatCanRun = findBlocksThatCanRun(loops, loop, RareEdges::AreFollowed);
        if (!hasLikelyBackEdge(loop, findBlocksInFrontOfRareEdges(loops, loop)))
            return;
        unsigned numberOfValues = 0;
        for (unsigned i = loop.size(); i--;) {
            BasicBlock* block = loop.at(i);
            if (!blocksThatCanRun.contains(block))
                continue;
            if (block->last()->type() != Void)
                return;
            // Beside a call the poll costs little.
            for (Value* value : *block) {
                if (!canCloneForLoop(value) || (isCall(value) && !isLoopPoll(value)))
                    return;
                numberOfValues += countsTowardsSize(value);
            }
            plan.blocks.append(block);
        }
        findPoll(loops, loop, plan);
        if (!plan.poll)
            return;
        PhiChildren& phiChildren = prepareToReadCounters();
        // The smaller the turn, the more of it is the poll and the less there is to copy. Every branch in front of a loop halves what is estimated for it.
        bool runsRarely = plan.header->frequency() * Limits::frequencyLostToBranchesInFrontOfLoop < m_proc.policy().loops.minFrequencyOfUnrolledLoop;
        if (runsRarely && numberOfValues > Limits::maxValuesPerTurnCopiedWhereLoopRunsRarely)
            return;
        Dominators& dominators = m_proc.dominators();
        for (BasicBlock* block : plan.blocks) {
            bool runsEveryTurn = true;
            for (BasicBlock* predecessor : plan.header->predecessors())
                runsEveryTurn &= !blocksThatCanRun.contains(predecessor) || dominators.dominates(block, predecessor);
            if (!runsEveryTurn || endsMostlyElsewhere(loops, loop, plan, block) || (!readTest(loops, loop, blocksThatCanRun, block, phiChildren, plan, TurnsAre::Counted) && !readTestOfCountersThatMeet(loops, loop, blocksThatCanRun, block, phiChildren, plan)))
                continue;
            if (mayBeEnteredOnAnyTurnOfLoopAround(loops, loop, plan))
                return;
            m_plansOfCopiesThatPoll.append(WTF::move(plan));
            return;
        }
    }

    // One counter rises and one falls until they meet: as a counter that starts where the first does and takes the steps of both towards where the second starts.
    bool readTestOfCountersThatMeet(const NaturalLoops& loops, const NaturalLoop& loop, const IndexSet<BasicBlock*>& likelyBlocks, BasicBlock* block, PhiChildren& phiChildren, Plan& plan)
    {
        if (block->last()->opcode() != Branch)
            return false;
        bool firstStays = loops.belongsTo(block->successorBlock(0), loop);
        if (firstStays == loops.belongsTo(block->successorBlock(1), loop))
            return false;
        DominatingCondition condition = stripNegations(block->last()->child(0)->foldIdentity());
        Value* comparison = condition.value->foldIdentity();
        std::optional<Opcode> inverted = invertedRelation(comparison->opcode());
        if (!inverted || !comparison->child(0)->type().isInt())
            return false;
        Opcode relation = firstStays == condition.isNonZero ? comparison->opcode() : *inverted;
        auto counterTestedAs = [&](Value* tested) -> std::optional<Counter> {
            for (Value* phi : *loop.header()) {
                if (phi->opcode() != Phi || phi->type() != tested->type())
                    continue;
                std::optional<Counter> counter = counterFor(loops, loop, likelyBlocks, plan.preHeader, phiChildren, phi);
                if (counter && (tested == counter->phi || tested == counter->next))
                    return counter;
            }
            return std::nullopt;
        };
        std::optional<Counter> left = counterTestedAs(comparison->child(0)->foldIdentity());
        std::optional<Counter> right = left ? counterTestedAs(comparison->child(1)->foldIdentity()) : std::nullopt;
        if (!right || (left->step > 0) == (right->step > 0) || isLessRelation(relation) != (left->step > 0))
            return false;
        plan.relation = relation;
        plan.start = left->start;
        plan.bound = right->start;
        plan.step = left->step - right->step;
        return true;
    }

    bool fitsOneStretch(const Plan& plan) const
    {
        Signedness signedness = isUnsignedRelation(plan.relation) ? Signedness::Unsigned : Signedness::Signed;
        IntegerInterval start = intervalFor(m_proc, plan.start, signedness);
        IntegerInterval bound = intervalFor(m_proc, plan.bound, signedness);
        Int128 distance = plan.step > 0 ? bound.max - start.min : start.max - bound.min;
        return distance <= Int128 { Limits::maxTurnsWithoutPoll } * (plan.step > 0 ? plan.step : -plan.step);
    }

    void removePolls(const Plan& plan)
    {
        m_proc.count(B3::Counter::LoopPollsRemoved);
        for (BasicBlock* block : plan.blocks) {
            for (Value* value : *block) {
                if (isLoopPoll(value))
                    value->replaceWithNop();
            }
        }
    }

    static Variable* variableOf(Value* demotedPhi)
    {
        RELEASE_ASSERT(demotedPhi->opcode() == Identity && demotedPhi->child(0)->opcode() == Get);
        return demotedPhi->child(0)->as<VariableValue>()->variable();
    }

    // A counter that has passed its bound is as far from it as can be: the copy takes the one turn of a loop that is tested at the bottom.
    void appendChoiceOfLoop(const Plan& plan, BasicBlock* block, Value* counter, Value* bound, BasicBlock* headerOfCopy)
    {
        Origin origin = plan.preHeader->last()->origin();
        bool rises = plan.step > 0;
        // A constant may stand in the loop.
        auto inBlock = [&](Value* value) {
            return value->hasInt() ? block->appendIntConstant(m_proc, origin, value->type(), value->asInt()) : value;
        };
        counter = inBlock(counter);
        bound = inBlock(bound);
        Value* upperEnd = rises ? bound : counter;
        Value* lowerEnd = rises ? counter : bound;
        int64_t greatest = Limits::maxTurnsWithoutPoll * (rises ? plan.step : -plan.step);
        auto appendTest = [&](Value* distance, BasicBlock* otherwise) {
            block->appendNew<Value>(m_proc, Branch, origin, block->appendNew<Value>(m_proc, BelowEqual, origin, distance, block->appendIntConstant(m_proc, origin, distance->type(), greatest)));
            block->setSuccessors(FrequentedBlock(plan.header), FrequentedBlock(otherwise, FrequencyClass::Rare));
        };
        // Where the lower end is not below zero, the upper end is no nearer to zero than to it: most loops are told by their upper end alone, without a subtraction.
        // A few turns far from zero are still a few turns: where the lower end may lie that far out, whoever ends there is asked how far apart the two ends lie.
        IntegerInterval lowest = intervalFor(m_proc, lowerEnd, isUnsignedRelation(plan.relation) ? Signedness::Unsigned : Signedness::Signed);
        if (lowest.min >= 0 && lowest.min <= greatest) {
            if (lowest.max <= greatest) {
                appendTest(upperEnd, headerOfCopy);
                return;
            }
            BasicBlock* endsFarFromZero = m_proc.addBlock();
            appendTest(upperEnd, endsFarFromZero);
            block = endsFarFromZero;
        }
        appendTest(block->appendNew<Value>(m_proc, Sub, origin, upperEnd, lowerEnd), headerOfCopy);
    }

    // A loop of more turns than one stretch runs in stretches too, in a copy of its own. Its test asks about the end of the stretch; only there is it asked whether the loop ends, and the loop polls.
    // A side path that ends its turn by itself does not pass that test, so such a copy polls on every turn.
    void leavePollToCopy(RegionCloner& cloner, const Plan& plan)
    {
        if (!fitsOneStretch(plan)) {
            // The end of the stretch takes a register. In a long turn that costs more than the poll, which is one instruction of many.
            unsigned numberOfValues = 0;
            for (BasicBlock* block : plan.blocks) {
                for (Value* value : *block)
                    numberOfValues += countsTowardsSize(value);
            }
            if (plan.sideTests.isEmpty() && plan.testedOperandOfBranch && numberOfValues <= Limits::maxValuesPerTurnRunInStretches)
                runCopyInStretches(cloner, plan);
            else {
                BasicBlock* headerOfCopy = cloner.cloneBlocks(plan.blocks.span()).get(plan.header);
                m_proc.deleteValue(plan.preHeader->values().takeLast());
                appendChoiceOfLoop(plan, plan.preHeader, plan.start, plan.bound, headerOfCopy);
            }
        }
        removePolls(plan);
    }

    void runCopyInStretches(RegionCloner& cloner, const Plan& plan)
    {
        Origin origin = plan.test->last()->origin();
        Type type = plan.tested->type();
        Variable* endOfStretch = m_proc.addVariable(type);
        unsigned testedOperand = *plan.testedOperandOfBranch;

        RegionCloner::BlockMap copies = cloner.cloneBlocks(plan.blocks.span());
        for (BasicBlock* block : plan.blocks) {
            for (Value* value : *copies.get(block)) {
                if (isLoopPoll(value))
                    value->replaceWithNop();
            }
        }

        BasicBlock* entered = m_proc.addBlock(plan.preHeader->frequency());
        entered->appendNew<VariableValue>(m_proc, Set, origin, endOfStretch, appendBoundOfStretch(m_proc, entered, origin, { plan.start, plan.bound, plan.relation, plan.step, BoundLiesBeyond::No }));
        entered->appendNew<Value>(m_proc, Jump, origin);
        entered->setSuccessors(FrequentedBlock(copies.get(plan.header)));
        m_proc.deleteValue(plan.preHeader->values().takeLast());
        appendChoiceOfLoop(plan, plan.preHeader, plan.start, plan.bound, entered);

        BasicBlock* test = copies.get(plan.test);
        BasicBlock* stays = test->successorBlock(plan.stayingSuccessorIndex);
        BasicBlock* leaves = test->successorBlock(!plan.stayingSuccessorIndex);
        BasicBlock* ended = m_proc.addBlock(test->frequency());
        BasicBlock* polls = m_proc.addBlock(test->frequency());
        Value* reached = test->last()->child(0)->child(testedOperand);
        Value* end = m_proc.add<VariableValue>(Get, origin, endOfStretch);
        Value* inStretch = m_proc.add<Value>(plan.relation, origin, reached, end);
        test->appendNonTerminal(end);
        test->appendNonTerminal(inStretch);
        test->replaceLastWithNew<Value>(m_proc, Branch, origin, inStretch);
        test->setSuccessors(FrequentedBlock(stays), FrequentedBlock(ended, FrequencyClass::Rare));

        ended->appendNew<Value>(m_proc, Branch, origin, ended->appendNew<Value>(m_proc, plan.relation, origin, reached, plan.bound));
        ended->setSuccessors(FrequentedBlock(polls), FrequentedBlock(leaves));

        // The next stretch is counted from the end of this one, which the counter has reached or passed by less than a step: whoever counted from the counter would keep it alive beside its successor, at the price of a move in every turn.
        polls->append(m_proc.clone(plan.poll));
        Value* endReached = polls->appendNew<VariableValue>(m_proc, Get, origin, endOfStretch);
        polls->appendNew<VariableValue>(m_proc, Set, origin, endOfStretch, appendBoundOfStretch(m_proc, polls, origin, { endReached, plan.bound, plan.relation, plan.step, BoundLiesBeyond::Yes }));
        polls->appendNew<Value>(m_proc, Jump, origin);
        polls->setSuccessors(FrequentedBlock(stays));
    }

    // Every turn moves the counter one step towards a bound that does not change, so the turns between two polls can be counted. That takes a plan that holds the whole loop:
    // a block that it leaves out leads back into the loop itself, from any copy and with any number of turns left. What lies behind the rare edges of the turns that are written out
    // has a copy of its own, which asks again how many turns are left. In the loop itself it goes round as it did, so there too it must move the counter one step.
    void findPoll(const NaturalLoops& loops, const NaturalLoop& loop, Plan& plan)
    {
        IndexSet<BasicBlock*> blocksThatCanRun = findBlocksThatCanRun(loops, loop, RareEdges::AreFollowed);
        for (BasicBlock* block : plan.blocks)
            blocksThatCanRun.remove(block);
        if (!plan.blocksBehindRareEdges.isEmpty()) {
            for (UpsilonValue* upsilon : prepareToReadCounters()[plan.counter]) {
                if (blocksThatCanRun.contains(upsilon->owner) && stepOf(upsilon->child(0)->foldIdentity(), plan.counter) != plan.step)
                    return;
            }
            for (BasicBlock* block : plan.blocksBehindRareEdges)
                blocksThatCanRun.remove(block);
        }
        if (!blocksThatCanRun.isEmpty())
            return;
        PatchpointValue* poll = nullptr;
        for (BasicBlock* block : plan.blocks) {
            for (Value* value : *block) {
                if (!isLoopPoll(value))
                    continue;
                if (!canPollBetweenStretches(value))
                    return;
                poll = value->as<PatchpointValue>();
            }
        }
        plan.poll = poll;
        // A loop with rare paths that come back is seldom one that walks for long, and a stretch costs more at its start than the polls of a few passes.
        plan.pollsOnceForAllWrittenOutTurns = !plan.blocksBehindRareEdges.isEmpty() && !fitsOneStretch(plan);
    }

    // What lies behind a rare edge is not written out with each turn. It gets one copy, with the rest of the turn, for all the turns that are written out,
    // and behind that copy they are entered again if there is room for them. A counter of 32 bits is asked in 64 bits. One of 64 bits is asked only where the sum cannot wrap around.
    void findBlocksBehindRareEdges(const NaturalLoops& loops, const NaturalLoop& loop, Plan& plan)
    {
        // A stretch of sums begins with more than the counter.
        if (!plan.sums.isEmpty())
            return;
        if (plan.counter->type() == Int64) {
            Signedness signedness = isUnsignedRelation(plan.relation) ? Signedness::Unsigned : Signedness::Signed;
            Int128 room = Int128 { plan.step } * (plan.factor - 1);
            IntegerInterval full = fullInterval(Int64, signedness);
            IntegerInterval counter = intervalFor(m_proc, plan.counter, signedness);
            if (counter.min + room < full.min || counter.max + room > full.max)
                return;
        }
        IndexSet<BasicBlock*> reached;
        Vector<BasicBlock*, 4> blocks;
        auto add = [&](BasicBlock* block) {
            if (block != plan.header && loops.belongsTo(block, loop) && reached.add(block))
                blocks.append(block);
        };
        for (BasicBlock* block : plan.blocks) {
            for (const FrequentedBlock& successor : block->successors()) {
                if (successor.isRare() && !plan.blocks.contains(successor.block()))
                    add(successor.block());
            }
        }
        bool comesBack = false;
        unsigned numberOfValues = 0;
        for (unsigned index = 0; index < blocks.size(); ++index) {
            BasicBlock* block = blocks[index];
            if (loops.innerMostLoopOf(block) != &loop || block->last()->type() != Void)
                return;
            for (Value* value : *block) {
                if (!canCloneForLoop(value))
                    return;
                numberOfValues += countsTowardsSize(value);
            }
            for (BasicBlock* successor : block->successorBlocks()) {
                comesBack |= successor == plan.header;
                add(successor);
            }
        }
        if (!comesBack)
            return;
        if (numberOfValues > m_proc.limit(Limit::MaxValuesPerUnrolledLoop)) {
            m_proc.didReachLimit(Limit::MaxValuesPerUnrolledLoop, plan.header->at(0)->origin());
            return;
        }
        plan.blocksBehindRareEdges = WTF::move(blocks);
    }

    static Opcode relationOfSignedValues(Opcode relation)
    {
        switch (relation) {
        case Below:
            return LessThan;
        case BelowEqual:
            return LessEqual;
        case Above:
            return GreaterThan;
        case AboveEqual:
            return GreaterEqual;
        default:
            return relation;
        }
    }

    std::optional<Plan::SideTest> readSideTest(const NaturalLoops& loops, const NaturalLoop& loop, BasicBlock* block, const Plan& plan)
    {
        bool firstStays = loops.belongsTo(block->successorBlock(0), loop);
        if (firstStays == loops.belongsTo(block->successorBlock(1), loop))
            return std::nullopt;
        DominatingCondition condition = stripNegations(block->last()->child(0)->foldIdentity());
        Value* comparison = condition.value->foldIdentity();
        std::optional<Opcode> inverted = invertedRelation(comparison->opcode());
        if (!inverted || !comparison->child(0)->type().isInt())
            return std::nullopt;
        Opcode relation = firstStays == condition.isNonZero ? comparison->opcode() : *inverted;
        for (unsigned testedIndex = 0; testedIndex < 2; ++testedIndex) {
            Value* tested = comparison->child(testedIndex)->foldIdentity();
            Value* bound = comparison->child(1 - testedIndex)->foldIdentity();
            bool isSameBound = bound == plan.bound || (bound->hasInt() && plan.bound->hasInt() && bound->type() == plan.bound->type() && bound->asInt() == plan.bound->asInt());
            if (!isSameBound || (testedIndex ? flippedRelation(relation) : relation) != plan.relation || stepOf(tested, plan.counter) != plan.step)
                continue;
            Plan::SideTest test { block, firstStays ? 0u : 1u, tested, { } };
            if (!findBlocksBehindTest(loops, loop, block, test.stayingSuccessorIndex, test.blocksBehindTest))
                return std::nullopt;
            return test;
        }
        return std::nullopt;
    }

    // What one turn stores at a place that does not change and the next turn loads there stays in a register from turn to turn, as long as one turn stands in front of the next.
    // Where ways meet between them, it is stored and loaded every turn.
    static bool carriesValueInMemory(const NaturalLoops& loops, const NaturalLoop& loop, const Plan& plan)
    {
        Vector<MemoryValue*, 4> stores;
        for (BasicBlock* block : plan.blocks) {
            for (Value* value : *block) {
                MemoryValue* memory = value->as<MemoryValue>();
                if (memory && memory->isStore() && memory->lastChild()->owner && !loops.belongsTo(memory->lastChild()->owner, loop))
                    stores.append(memory);
            }
        }
        if (stores.isEmpty())
            return false;
        for (BasicBlock* block : plan.blocks) {
            for (Value* value : *block) {
                MemoryValue* memory = value->as<MemoryValue>();
                if (!memory || !memory->isLoad())
                    continue;
                for (MemoryValue* store : stores) {
                    if (store->lastChild() == memory->lastChild() && store->offset() == memory->offset())
                        return true;
                }
            }
        }
        return false;
    }

    // Whoever leaves the turns that are written out ends in the loop itself and stays there. An edge that is merely called unlikely may be taken every other turn,
    // so what lies behind such edges is written out with the turns. What lies behind a rare edge is not.
    bool addSidePaths(const NaturalLoops& loops, const NaturalLoop& loop, const IndexSet<BasicBlock*>& likelyBlocks, PhiChildren& phiChildren, Plan& plan, unsigned& numberOfValues)
    {
        if (carriesValueInMemory(loops, loop, plan))
            return false;
        IndexSet<BasicBlock*> sideBlocks;
        Vector<BasicBlock*, 8> worklist;
        for (BasicBlock* block : plan.blocks)
            worklist.append(block);
        unsigned numberOfLikelyBlocks = plan.blocks.size();
        while (!worklist.isEmpty()) {
            for (const FrequentedBlock& successor : worklist.takeLast()->successors()) {
                if (successor.isRare() || !loops.belongsTo(successor.block(), loop) || likelyBlocks.contains(successor.block()) || !sideBlocks.add(successor.block()))
                    continue;
                plan.blocks.append(successor.block());
                worklist.append(successor.block());
            }
        }
        if (plan.blocks.size() == numberOfLikelyBlocks)
            return false;

        for (BasicBlock* block : plan.blocks.span().subspan(numberOfLikelyBlocks)) {
            if (block->last()->type() != Void)
                return false;
            for (Value* value : *block) {
                if (!canCloneForLoop(value) || (isCall(value) && !isLoopPoll(value)) || storesWord(value))
                    return false;
                numberOfValues += countsTowardsSize(value);
            }
            bool leavesLoop = false;
            for (const FrequentedBlock& successor : block->successors())
                leavesLoop |= !successor.isRare() && !loops.belongsTo(successor.block(), loop);
            if (!leavesLoop)
                continue;
            // A loop that looks for something is expected to find it.
            std::optional<Plan::SideTest> test = plan.testsCounterBeforeStep || block->last()->opcode() != Branch ? std::nullopt : readSideTest(loops, loop, block, plan);
            if (!test)
                return false;
            plan.sideTests.append(WTF::move(*test));
        }

        for (UpsilonValue* upsilon : phiChildren[plan.counter]) {
            if (sideBlocks.contains(upsilon->owner) && stepOf(upsilon->child(0)->foldIdentity(), plan.counter) != plan.step)
                return false;
        }
        for (BasicBlock* block : plan.blocks) {
            for (Value* value : *block) {
                if (stepOf(value, plan.counter) == plan.step)
                    plan.nextCounters.append(value);
            }
        }
        if (plan.testsCounterBeforeStep)
            return true;
        // At the bottom of every turn stands a test.
        for (BasicBlock* predecessor : plan.header->predecessors()) {
            if (!sideBlocks.contains(predecessor))
                continue;
            bool endsBehindTest = plan.sideTests.containsIf([&](const Plan::SideTest& test) {
                return predecessor == (test.blocksBehindTest.isEmpty() ? test.block : test.blocksBehindTest.last());
            });
            if (!endsBehindTest)
                return false;
        }
        return true;
    }

    struct Bounds {
        Value* bound { nullptr };
        Value* farBound { nullptr };
        // The block in front of the turns that are written out, where there is one.
        BasicBlock* entry { nullptr };
    };

    static Variable* variableOfCounter(const Plan& plan) { return variableOf(plan.counter); }

    void appendPoll(const Plan& plan, BasicBlock* block)
    {
        if (plan.poll)
            block->append(m_proc.clone(plan.poll));
    }

    void enterAgainBehindRareEdges(RegionCloner& cloner, const Plan& plan, const Bounds& bounds, std::span<const RegionCloner::BlockMap> turns, BasicBlock* unrolledHeader)
    {
        if (plan.blocksBehindRareEdges.isEmpty())
            return;
        Origin origin = plan.test->last()->origin();
        BasicBlock* again = m_proc.addBlock();
        if (!plan.pollsOnceForAllWrittenOutTurns)
            appendPoll(plan, again);
        Value* counter = again->appendNew<VariableValue>(m_proc, Get, origin, variableOfCounter(plan));
        Value* bound = bounds.bound;
        Opcode relation = plan.relation;
        if (counter->type() == Int32) {
            Opcode extension = isUnsignedRelation(relation) ? ZExt32 : SExt32;
            counter = again->appendNew<Value>(m_proc, extension, origin, counter);
            bound = again->appendNew<Value>(m_proc, extension, origin, bound);
            relation = relationOfSignedValues(relation);
        }
        Value* reached = again->appendNew<Value>(m_proc, Add, origin, counter, again->appendIntConstant(m_proc, origin, Int64, plan.step * static_cast<int64_t>(plan.factor - 1)));
        again->appendNew<Value>(m_proc, Branch, origin, again->appendNew<Value>(m_proc, relation, origin, reached, bound));
        again->setSuccessors(FrequentedBlock(bounds.entry ? bounds.entry : unrolledHeader), FrequentedBlock(plan.header));

        RegionCloner::BlockMap copies = cloner.cloneBlocks(plan.blocksBehindRareEdges.span());
        for (BasicBlock* block : plan.blocksBehindRareEdges)
            copies.get(block)->replaceSuccessor(plan.header, again);
        for (const RegionCloner::BlockMap& turn : turns) {
            for (BasicBlock* block : plan.blocks) {
                for (FrequentedBlock& successor : turn.get(block)->successors()) {
                    BasicBlock* copy = successor.isRare() ? copies.get(successor.block()) : nullptr;
                    if (copy)
                        successor.block() = copy;
                }
            }
        }
    }

    // The block in front of the pre-header that lets the loop be entered exactly where the first turn's test would hold, for a counter that starts at a constant and rises to a bound that is not negative.
    struct TestOfFirstTurn {
        BasicBlock* block { nullptr };
        // From the block behind the test to the pre-header: they only jump, and hold nothing but constants and what the loop starts with.
        Vector<BasicBlock*, 4> blocksBehind;
    };

    std::optional<TestOfFirstTurn> findTestOfFirstTurn(const Plan& plan, Int128 start)
    {
        if (plan.testsCounterBeforeStep || plan.step < 0 || start < 0)
            return std::nullopt;
        TestOfFirstTurn result;
        BasicBlock* block = plan.preHeader;
        do {
            if (block->numPredecessors() != 1 || result.blocksBehind.size() == Limits::maxBlocksBesideTestOfUnrolledLoop)
                return std::nullopt;
            for (Value* value : *block) {
                if (value != block->last() && !value->as<VariableValue>() && value->opcode() != Upsilon && value->opcode() != Nop && !value->isConstant())
                    return std::nullopt;
            }
            result.blocksBehind.insert(0, block);
            block = block->predecessor(0);
        } while (block->last()->opcode() == Jump);
        BasicBlock* preHeader = result.blocksBehind[0];
        if (block->last()->opcode() != Branch || block->successorBlock(0) == block->successorBlock(1))
            return std::nullopt;
        result.block = block;
        IntegerInterval known = intervalFor(m_proc, plan.bound, Signedness::Signed);
        if (known.min < 0)
            return std::nullopt;

        DominatingCondition condition = stripNegations(block->last()->child(0)->foldIdentity());
        Value* tested = condition.value->foldIdentity();
        bool entersWhereTrue = condition.isNonZero == (block->successorBlock(0) == preHeader);
        auto holdsFor = [&](IntegerInterval bounds, bool expected) {
            if (bounds.min > bounds.max)
                return true;
            TriState result = TriState::Indeterminate;
            if (tested == plan.bound)
                result = evaluateComparison(NotEqual, bounds, IntegerInterval { 0, 0 });
            else if (isIntegerComparison(tested->opcode()) && tested->child(0)->foldIdentity() == plan.bound && tested->child(1)->hasInt() && tested->child(1)->asInt() >= 0)
                result = evaluateComparison(tested->opcode(), bounds, IntegerInterval { tested->child(1)->asInt(), tested->child(1)->asInt() });
            else if (isIntegerComparison(tested->opcode()) && tested->child(1)->foldIdentity() == plan.bound && tested->child(0)->hasInt() && tested->child(0)->asInt() >= 0)
                result = evaluateComparison(tested->opcode(), IntegerInterval { tested->child(0)->asInt(), tested->child(0)->asInt() }, bounds);
            return result == triState(expected == entersWhereTrue);
        };
        Int128 leastBoundOfOneTurn = start + (includesEqual(plan.relation) ? 0 : 1);
        if (!holdsFor({ std::max(known.min, leastBoundOfOneTurn), known.max }, true) || !holdsFor({ known.min, std::min(known.max, leastBoundOfOneTurn - 1) }, false))
            return std::nullopt;
        return result;
    }

    static bool includesEqual(Opcode relation) { return relation == LessEqual || relation == GreaterEqual || relation == BelowEqual || relation == AboveEqual; }

    // Where the turns run in stretches: the block where the first stretch begins, and the variable that holds the far bound of the stretch.
    struct StretchEntry {
        BasicBlock* begins { nullptr };
        Variable* farBoundVariable { nullptr };
    };

    Bounds appendBoundsAndEnter(const Plan& plan, BasicBlock* unrolledHeader, StretchEntry stretch)
    {
        BasicBlock* preHeader = plan.preHeader;
        Origin origin = plan.test->last()->origin();
        Type type = plan.bound->type();
        auto append = [&](Value* value) {
            preHeader->appendNonTerminal(value);
            return value;
        };
        auto constant = [&](int64_t value) {
            return append(m_proc.addIntConstant(origin, type, value));
        };
        Value* bound = plan.bound->hasInt() ? constant(plan.bound->asInt()) : plan.bound;
        int64_t room = roomForTurnsBehindFirst(plan);
        // A loop of a few turns pays one comparison. Where the counter can take all the steps and still meets the bound, the bound has room for them as well.
        if (plan.startHasRoom) {
            BasicBlock* entered = m_proc.addBlock();
            Value* farBound = entered->appendNew<Value>(m_proc, Sub, origin, bound, entered->appendIntConstant(m_proc, origin, type, room));
            entered->appendNew<Value>(m_proc, Jump, origin);
            entered->setSuccessors(FrequentedBlock(unrolledHeader));

            IntegerInterval start = intervalFor(m_proc, plan.start, isUnsignedRelation(plan.relation) ? Signedness::Unsigned : Signedness::Signed);
            bool startIsKnown = start.min == start.max;
            // One test says that there is a first turn and that there are too few for the turns that are written out: a loop of a few turns pays a subtraction.
            if (auto found = startIsKnown ? findTestOfFirstTurn(plan, start.min) : std::nullopt) {
                BasicBlock* testOfFirstTurn = found->block;
                BasicBlock* other = m_proc.addBlock();
                other->appendNew<Value>(m_proc, Branch, origin, testOfFirstTurn->last()->child(0));
                other->setSuccessors(testOfFirstTurn->successor(0), testOfFirstTurn->successor(1));
                other->replaceSuccessor(found->blocksBehind[0], entered);
                BasicBlock* notShort = other;
                for (BasicBlock* block : found->blocksBehind) {
                    Value* jump = block->last();
                    for (Value* value : *block) {
                        if (value == jump)
                            continue;
                        value->owner = testOfFirstTurn;
                        testOfFirstTurn->appendNonTerminal(value);
                    }
                    block->values().shrink(0);
                    block->values().append(jump);
                }
                auto appendInFront = [&](Value* value) {
                    testOfFirstTurn->appendNonTerminal(value);
                    return value;
                };
                int64_t leastBoundOfOneTurn = static_cast<int64_t>(start.min) + (includesEqual(plan.relation) ? 0 : 1);
                Value* turnsBehindFirst = appendInFront(m_proc.add<Value>(Sub, origin, bound, appendInFront(m_proc.addIntConstant(origin, type, leastBoundOfOneTurn))));
                Value* isNotShort = appendInFront(m_proc.add<Value>(AboveEqual, origin, turnsBehindFirst, appendInFront(m_proc.addIntConstant(origin, type, room))));
                testOfFirstTurn->last()->child(0) = isNotShort;
                // A loop that fits one stretch pays nothing for the stretches: the test that there is a first turn gives way to the test that the turns are neither none nor too many.
                if (stretch.begins) {
                    BasicBlock* fits = m_proc.addBlock();
                    Value* farBoundOfAllTurns = fits->appendNew<Value>(m_proc, Sub, origin, bound, fits->appendIntConstant(m_proc, origin, type, room));
                    fits->appendNew<VariableValue>(m_proc, Set, origin, stretch.farBoundVariable, farBoundOfAllTurns);
                    fits->appendNew<Value>(m_proc, Jump, origin);
                    fits->setSuccessors(FrequentedBlock(stretch.begins));
                    notShort = m_proc.addBlock();
                    Value* most = notShort->appendIntConstant(m_proc, origin, type, Limits::maxTurnsWithoutPoll * plan.step - 1);
                    notShort->appendNew<Value>(m_proc, Branch, origin, notShort->appendNew<Value>(m_proc, BelowEqual, origin, turnsBehindFirst, most));
                    notShort->setSuccessors(FrequentedBlock(fits), FrequentedBlock(other));
                    // What still enters behind that has more turns than a stretch.
                    for (FrequentedBlock& successor : other->successors()) {
                        if (successor.block() == entered)
                            successor = FrequentedBlock(entered, FrequencyClass::Rare);
                    }
                }
                testOfFirstTurn->setSuccessors(FrequentedBlock(notShort), FrequentedBlock(found->blocksBehind[0]));
                return { bound, farBound, entered };
            }
            // Where only the nearest start is known, a bound that is too near even for that one is told by a comparison with a constant.
            Int128 nearestStart = plan.step > 0 ? start.min : start.max;
            IntegerInterval full = fullInterval(type, isUnsignedRelation(plan.relation) ? Signedness::Unsigned : Signedness::Signed);
            BasicBlock* behindFirstTest = entered;
            bool saysSomething = plan.step > 0 ? nearestStart > full.min || !full.min : nearestStart < full.max;
            if (!startIsKnown && saysSomething) {
                behindFirstTest = m_proc.addBlock();
                Value* reached = behindFirstTest->appendNew<Value>(m_proc, Add, origin, plan.start, behindFirstTest->appendIntConstant(m_proc, origin, type, room));
                behindFirstTest->appendNew<Value>(m_proc, Branch, origin, behindFirstTest->appendNew<Value>(m_proc, plan.relation, origin, reached, bound));
                behindFirstTest->setSuccessors(FrequentedBlock(entered), FrequentedBlock(plan.header));
            }
            Value* reached = startIsKnown || behindFirstTest != entered ? constant(static_cast<int64_t>(nearestStart) + room) : append(m_proc.add<Value>(Add, origin, plan.start, constant(room)));
            Value* enters = append(m_proc.add<Value>(plan.relation, origin, reached, bound));
            preHeader->replaceLastWithNew<Value>(m_proc, Branch, origin, enters);
            preHeader->setSuccessors(FrequentedBlock(behindFirstTest), FrequentedBlock(plan.header));
            return { bound, farBound, entered };
        }
        Value* farBound = append(m_proc.add<Value>(Sub, origin, bound, constant(room)));
        Value* enters = nullptr;
        if (plan.needsGuardOfFarBound) {
            Opcode hasRoom = plan.step > 0 ? (isUnsignedRelation(plan.relation) ? AboveEqual : GreaterEqual) : (isUnsignedRelation(plan.relation) ? BelowEqual : LessEqual);
            enters = append(m_proc.add<Value>(hasRoom, origin, bound, constant(plan.farthestBoundWithRoom)));
        }
        if (!plan.testsCounterBeforeStep) {
            Value* reaches = append(m_proc.add<Value>(plan.relation, origin, plan.start, farBound));
            enters = enters ? append(m_proc.add<Value>(BitAnd, origin, enters, reaches)) : reaches;
        }
        if (!enters) {
            preHeader->setSuccessors(FrequentedBlock(unrolledHeader));
            return { bound, farBound, nullptr };
        }
        preHeader->replaceLastWithNew<Value>(m_proc, Branch, origin, enters);
        preHeader->setSuccessors(FrequentedBlock(unrolledHeader), FrequentedBlock(plan.header));
        return { bound, farBound, nullptr };
    }

    static constexpr int64_t greatestSumOfIntegers = 1ll << 52;
    static constexpr int64_t turnsOfStretch = Limits::maxTurnsWithoutPoll;
    // So many integers of 32 bits add up to less than 2^52, with the turns that are written out once more.
    static_assert(turnsOfStretch <= 1ll << 20);

    // The turns that are written out run in stretches. Between two stretches the loop polls. In front of each the sums are tested and the integers start at zero, so nothing in the loop looks at them.
    struct Stretch {
        BasicBlock* first { nullptr };
        BasicBlock* tests { nullptr };
        // Behind the tests. The first stretch begins here where the sums start at constants that pass them.
        BasicBlock* begins { nullptr };
        // Where the first stretch is entered: behind the tests, or at the question in front of them whether every sum starts at zero.
        BasicBlock* entered { nullptr };
        BasicBlock* next { nullptr };
        Variable* farBoundVariable { nullptr };
        Vector<Variable*, 1> firstSums;
        Vector<Value*, 1> sumsInFront;
        Value* farBound { nullptr };
    };

    // Computed anew wherever it is asked for, so that it does not live across the turns beside the far bound of the stretch.
    Value* appendFarBound(const Plan& plan, const Bounds& bounds, BasicBlock* block)
    {
        Origin origin = plan.test->last()->origin();
        return block->appendNew<Value>(m_proc, Sub, origin, bounds.bound, block->appendIntConstant(m_proc, origin, bounds.bound->type(), roomForTurnsBehindFirst(plan)));
    }

    static int64_t lengthOfStretch(const Plan& plan, Type type)
    {
        return std::min<int64_t>(turnsOfStretch * (plan.step > 0 ? plan.step : -plan.step), type == Int32 ? 1 << 30 : 1ll << 62);
    }

    // A counter that rises starts at the lower end. One that falls ends there, at a far bound that has not wrapped around.
    bool lowerEndIsNotBelowZero(const Plan& plan, const Bounds& bounds)
    {
        Signedness signedness = isUnsignedRelation(plan.relation) ? Signedness::Unsigned : Signedness::Signed;
        if (plan.step > 0)
            return intervalFor(m_proc, plan.counter, signedness).min >= 0;
        if (signedness == Signedness::Unsigned)
            return false;
        // The bound may be a choice between the bound of the loop and a stand-in for it.
        Value* choice = bounds.bound->opcode() == Select ? bounds.bound : nullptr;
        IntegerInterval bound = intervalFor(m_proc, choice ? choice->child(1) : bounds.bound, signedness);
        if (choice)
            bound = bound.join(intervalFor(m_proc, choice->child(2), signedness));
        int64_t room = roomForTurnsBehindFirst(plan);
        int64_t greatest = bounds.bound->type() == Int32 ? std::numeric_limits<int32_t>::max() : std::numeric_limits<int64_t>::max();
        return bound.min >= room && bound.max <= greatest + room;
    }

    bool lowerEndMayLieFartherThanStretch(const Plan& plan, const Bounds& bounds)
    {
        Signedness signedness = isUnsignedRelation(plan.relation) ? Signedness::Unsigned : Signedness::Signed;
        return intervalFor(m_proc, plan.step > 0 ? plan.start : bounds.bound, signedness).max > lengthOfStretch(plan, bounds.bound->type());
    }

    void appendFarBoundOfStretch(const Plan& plan, const Stretch& stretch, const Bounds& bounds, BasicBlock* block, Variable* beginning, BoundLiesBeyond farBoundLiesBeyond, BasicBlock* successor)
    {
        Origin origin = plan.test->last()->origin();
        Value* from = block->appendNew<VariableValue>(m_proc, Get, origin, beginning);
        Value* farBound = appendBoundOfStretch(m_proc, block, origin, { from, appendFarBound(plan, bounds, block), plan.relation, plan.step, farBoundLiesBeyond });
        block->appendNew<VariableValue>(m_proc, Set, origin, stretch.farBoundVariable, farBound);
        block->appendNew<Value>(m_proc, Jump, origin);
        block->setSuccessors(FrequentedBlock(successor));
    }

    static constexpr double roundingOfSums = 1.5 * static_cast<double>(greatestSumOfIntegers);
    static constexpr double greatestStartOfSum = static_cast<double>(greatestSumOfIntegers / 2);

    static bool startPassesTests(const SumOfIntegers& sum)
    {
        Value* start = sum.start->foldIdentity();
        if (!start->hasDouble())
            return false;
        double value = start->asDouble();
        bool isWhole = (value + roundingOfSums) - roundingOfSums == value;
        bool isMinusZero = std::bit_cast<int64_t>(value) == std::numeric_limits<int64_t>::min();
        return isWhole && std::abs(value) < greatestStartOfSum && (sum.sum->opcode() != Sub || !isMinusZero);
    }

    // Adding 1.5 * 2^52 to a number under 2^51 rounds it to a whole number.
    void appendStartOfStretch(const Plan& plan, Stretch& stretch, std::span<Variable* const> integers, const Bounds& bounds, BasicBlock* unrolledHeader, BasicBlock* headerOfLoopThatPolls)
    {
        Origin origin = plan.test->last()->origin();
        BasicBlock* tests = stretch.tests;
        BasicBlock* begins = stretch.begins;
        BasicBlock* entered = stretch.entered;
        // Where the lower end of what the counter walks is not below zero, the two ends lie nearer to each other than the upper end lies to zero:
        // that the upper end lies within a stretch of zero needs no subtraction to be told.
        BasicBlock* first = stretch.first;
        Type type = bounds.bound->type();
        if (lowerEndIsNotBelowZero(plan, bounds)) {
            bool rises = plan.step > 0;
            Value* farBound = appendFarBound(plan, bounds, first);
            Value* farthest = first->appendIntConstant(m_proc, origin, type, lengthOfStretch(plan, type));
            Value* upperEnd = rises ? farBound : first->appendNew<VariableValue>(m_proc, Get, origin, variableOfCounter(plan));
            first->appendNew<VariableValue>(m_proc, Set, origin, stretch.farBoundVariable, farBound);
            first->appendNew<Value>(m_proc, Branch, origin, first->appendNew<Value>(m_proc, Above, origin, upperEnd, farthest));
            BasicBlock* liesFarther = m_proc.addBlock();
            first->setSuccessors(FrequentedBlock(liesFarther, FrequencyClass::Rare), FrequentedBlock(entered));
            first = liesFarther;
            // A few turns far from zero are one stretch as well, which ends at the far bound. The bound lies beyond the far bound by the room, and is at hand.
            if (lowerEndMayLieFartherThanStretch(plan, bounds)) {
                Value* counter = rises ? liesFarther->appendNew<VariableValue>(m_proc, Get, origin, variableOfCounter(plan)) : upperEnd;
                Value* distance = liesFarther->appendNew<Value>(m_proc, Sub, origin, rises ? bounds.bound : counter, rises ? counter : bounds.bound);
                int64_t room = roomForTurnsBehindFirst(plan);
                Value* farthestWithRoom = liesFarther->appendIntConstant(m_proc, origin, type, lengthOfStretch(plan, type) + (rises ? room : -room));
                liesFarther->appendNew<Value>(m_proc, Branch, origin, liesFarther->appendNew<Value>(m_proc, Above, origin, distance, farthestWithRoom));
                first = m_proc.addBlock();
                liesFarther->setSuccessors(FrequentedBlock(first, FrequencyClass::Rare), FrequentedBlock(entered));
            }
        }
        // A test at the bottom is reached only where there is room for the turns that are written out. A test at the top asks that by itself.
        appendFarBoundOfStretch(plan, stretch, bounds, first, variableOfCounter(plan), plan.testsCounterBeforeStep ? BoundLiesBeyond::No : BoundLiesBeyond::Yes, entered);
        appendPoll(plan, stretch.next);
        // The counter has passed the end of the last stretch by less than the turns that are written out. Nothing but the test of those turns looks at the counter here, so it stays in one register.
        // Every stretch comes in by one block, so that the stretches are a loop with one way in.
        appendFarBoundOfStretch(plan, stretch, bounds, stretch.next, stretch.farBoundVariable, BoundLiesBeyond::Yes, entered == begins ? tests : entered);
        stretch.farBound = begins->appendNew<VariableValue>(m_proc, Get, origin, stretch.farBoundVariable);
        if (plan.sums.isEmpty()) {
            tests->appendNew<Value>(m_proc, Jump, origin);
            tests->setSuccessors(FrequentedBlock(unrolledHeader));
            return;
        }
        // Most sums start at zero, which passes every test and is told by its bits. Where the start is no constant, as where a rare path hands a sum back in front of the loop, that is asked first.
        if (entered != begins) {
            Value* bits = nullptr;
            for (Variable* firstSum : stretch.firstSums) {
                Value* bitsOfStart = entered->appendNew<Value>(m_proc, BitwiseCast, origin, entered->appendNew<VariableValue>(m_proc, Get, origin, firstSum));
                bits = bits ? entered->appendNew<Value>(m_proc, BitOr, origin, bits, bitsOfStart) : bitsOfStart;
            }
            entered->appendNew<Value>(m_proc, Branch, origin, bits);
            entered->setSuccessors(FrequentedBlock(tests, FrequencyClass::Unlikely), FrequentedBlock(begins));
        }
        Value* rounding = tests->appendNew<ConstDoubleValue>(m_proc, origin, roundingOfSums);
        Value* greatestStart = tests->appendNew<ConstDoubleValue>(m_proc, origin, greatestStartOfSum);
        Value* enters = nullptr;
        for (unsigned index = 0; index < plan.sums.size(); ++index) {
            Value* start = tests->appendNew<VariableValue>(m_proc, Get, origin, stretch.firstSums[index]);
            stretch.sumsInFront.append(begins == tests ? start : begins->appendNew<VariableValue>(m_proc, Get, origin, stretch.firstSums[index]));
            Value* rounded = tests->appendNew<Value>(m_proc, Sub, origin, tests->appendNew<Value>(m_proc, Add, origin, start, rounding), rounding);
            Value* isWhole = tests->appendNew<Value>(m_proc, Equal, origin, rounded, start);
            Value* isSmall = tests->appendNew<Value>(m_proc, LessThan, origin, tests->appendNew<Value>(m_proc, Abs, origin, start), greatestStart);
            Value* fits = tests->appendNew<Value>(m_proc, BitAnd, origin, isWhole, isSmall);
            // -0 - 0 is -0, and -0 - 1 + 1 is 0: which of the two comes out depends on more than the sum of what is taken away. -0 + 0 is 0.
            if (plan.sums[index].sum->opcode() == Sub) {
                Value* isNotMinusZero = tests->appendNew<Value>(m_proc, NotEqual, origin, tests->appendNew<Value>(m_proc, BitwiseCast, origin, start), tests->appendIntConstant(m_proc, origin, Int64, std::numeric_limits<int64_t>::min()));
                fits = tests->appendNew<Value>(m_proc, BitAnd, origin, fits, isNotMinusZero);
            }
            enters = enters ? tests->appendNew<Value>(m_proc, BitAnd, origin, enters, fits) : fits;
            begins->appendNew<VariableValue>(m_proc, Set, origin, integers[index], begins->appendIntConstant(m_proc, origin, Int64, 0));
        }
        tests->appendNew<Value>(m_proc, Branch, origin, enters);
        tests->setSuccessors(FrequentedBlock(begins == tests ? unrolledHeader : begins), FrequentedBlock(headerOfLoopThatPolls, FrequencyClass::Rare));
        if (begins == tests)
            return;
        begins->appendNew<Value>(m_proc, Jump, origin);
        begins->setSuccessors(FrequentedBlock(unrolledHeader));
    }

    static Value* insertBehind(Value* place, Value* value)
    {
        BasicBlock* block = place->owner;
        size_t index = block->values().find(place);
        RELEASE_ASSERT(index != notFound);
        value->owner = block;
        block->values().insert(index + 1, value);
        return value;
    }

    // The variables that hold the sum where the turns are left.
    Vector<Variable*, 2> variablesOf(const Plan& plan, Value* sum)
    {
        Vector<Variable*, 2> result;
        for (unsigned round = 0; round < 2; ++round) {
            for (BasicBlock* block : plan.blocks) {
                for (Value* value : *block) {
                    if (value->opcode() != Set)
                        continue;
                    Value* stored = value->child(0)->foldIdentity();
                    bool holdsSum = stored == sum || (stored->opcode() == Get && result.contains(stored->as<VariableValue>()->variable()));
                    Variable* variable = value->as<VariableValue>()->variable();
                    if (holdsSum && !result.contains(variable))
                        result.append(variable);
                }
            }
        }
        return result;
    }

    void unroll(RegionCloner& cloner, const Plan& plan)
    {
        Origin origin = plan.test->last()->origin();
        m_proc.addRemark({ RemarkKind::LoopUnrolled, GaveUpReason::None, origin, std::nullopt });
        Variable* testedVariable = m_proc.addVariable(plan.tested->type());
        {
            BasicBlock* owner = plan.tested->owner;
            size_t index = owner->values().find(plan.tested);
            RELEASE_ASSERT(index != notFound);
            Value* set = m_proc.add<VariableValue>(Set, origin, testedVariable, plan.tested);
            set->owner = owner;
            owner->values().insert(index + 1, set);
        }
        for (const Plan::SideTest& side : plan.sideTests)
            insertBehind(side.tested, m_proc.add<VariableValue>(Set, origin, testedVariable, side.tested));

        Vector<RegionCloner::BlockMap, 8> turns;
        for (unsigned turn = 0; turn < plan.factor; ++turn)
            turns.append(cloner.cloneBlocks(plan.blocks.span()));

        // The loop itself takes the turns that are left, fewer than are written out, and goes without its poll. Where a sum does not fit, a copy that polls takes all the turns.
        BasicBlock* headerOfLoopThatPolls = plan.header;
        if (plan.poll && !plan.sums.isEmpty())
            headerOfLoopThatPolls = cloner.cloneBlocks(plan.blocks.span()).get(plan.header);

        // Where the ways through a turn meet, their counters would meet as well, and no turn would know how far it is from the first. So a turn computes its next counter once, at its top.
        // Behind the last turn they meet in the Phi of the loop anyway, and a counter that is computed early would live beside the one it follows.
        if (!plan.nextCounters.isEmpty()) {
            size_t indexOfCounter = plan.header->values().find(plan.counter);
            RELEASE_ASSERT(indexOfCounter != notFound);
            Vector<std::pair<BasicBlock*, size_t>, 2> places;
            for (Value* next : plan.nextCounters) {
                size_t index = next->owner->values().find(next);
                RELEASE_ASSERT(index != notFound);
                places.append({ next->owner, index });
            }
            for (unsigned turn = 0; turn + 1 < plan.factor; ++turn) {
                Vector<Value*, 2> copies;
                for (auto [block, index] : places)
                    copies.append(turns[turn].get(block)->at(index));
                BasicBlock* header = turns[turn].get(plan.header);
                Value* counter = header->at(indexOfCounter);
                counter->owner = header;
                Value* step = insertBehind(counter, m_proc.addIntConstant(origin, counter->type(), plan.step));
                Value* next = insertBehind(step, m_proc.add<Value>(Add, origin, counter, step));
                for (Value* copy : copies)
                    copy->replaceWithIdentity(next);
            }
        }

        // A copy has its values in the order of the block it was made of. All the copies are found before anything is put between them.
        Vector<Variable*, 1> integers;
        Vector<Vector<Value*, 8>, 1> summandsOfTurns;
        for (const SumOfIntegers& sum : plan.sums) {
            m_proc.count(B3::Counter::SumsKeptAsIntegers);
            integers.append(m_proc.addVariable(Int64));
            size_t index = sum.summand->owner->values().find(sum.summand);
            RELEASE_ASSERT(index != notFound);
            Vector<Value*, 8> summands;
            for (unsigned turn = 0; turn < plan.factor; ++turn) {
                BasicBlock* copy = turns[turn].get(sum.summand->owner);
                Value* summand = copy->at(index);
                RELEASE_ASSERT(summand->opcode() == IToD);
                summand->owner = copy;
                summands.append(summand);
            }
            summandsOfTurns.append(WTF::move(summands));
        }
        for (unsigned index = 0; index < plan.sums.size(); ++index) {
            for (Value* summand : summandsOfTurns[index]) {
                Value* before = insertBehind(summand, m_proc.add<VariableValue>(Get, origin, integers[index]));
                Value* wide = insertBehind(before, m_proc.add<Value>(SExt32, origin, summand->child(0)));
                Value* after = insertBehind(wide, m_proc.add<Value>(plan.sums[index].sum->opcode(), origin, before, wide));
                insertBehind(after, m_proc.add<VariableValue>(Set, origin, integers[index], after));
            }
        }

        Stretch stretch;
        // Behind a stretch there may be room for another.
        BasicBlock* headerBehindRest = plan.header;
        if (!plan.sums.isEmpty() || (plan.poll && !plan.pollsOnceForAllWrittenOutTurns && !fitsOneStretch(plan))) {
            stretch.first = m_proc.addBlock();
            stretch.tests = m_proc.addBlock();
            stretch.begins = plan.sums.isEmpty() ? stretch.tests : m_proc.addBlock();
            stretch.entered = plan.sums.isEmpty() || std::ranges::all_of(plan.sums, startPassesTests) ? stretch.begins : m_proc.addBlock();
            stretch.next = m_proc.addBlock();
            stretch.farBoundVariable = m_proc.addVariable(plan.tested->type());
            headerBehindRest = m_proc.addBlock();
            for (const SumOfIntegers& sum : plan.sums) {
                stretch.firstSums.append(m_proc.addVariable(Double));
                plan.preHeader->appendNonTerminal(m_proc.add<VariableValue>(Set, origin, stretch.firstSums.last(), sum.start));
            }
        }
        BasicBlock* unrolledHeader = stretch.first ? stretch.first : turns[0].get(plan.header);
        Bounds bounds = appendBoundsAndEnter(plan, unrolledHeader, { stretch.entered, stretch.farBoundVariable });
        if (stretch.tests) {
            appendStartOfStretch(plan, stretch, integers.span(), bounds, turns[0].get(plan.header), headerOfLoopThatPolls);
            BasicBlock* asks = headerBehindRest;
            // A stretch that is not the last ends a whole stretch beyond a counter that is not below zero.
            if (plan.step > 0 && lowerEndIsNotBelowZero(plan, bounds)) {
                asks = m_proc.addBlock();
                Value* end = headerBehindRest->appendNew<VariableValue>(m_proc, Get, origin, stretch.farBoundVariable);
                headerBehindRest->appendNew<Value>(m_proc, Branch, origin, headerBehindRest->appendNew<Value>(m_proc, AboveEqual, origin, end, headerBehindRest->appendIntConstant(m_proc, origin, end->type(), lengthOfStretch(plan, end->type()))));
                headerBehindRest->setSuccessors(FrequentedBlock(asks, FrequencyClass::Rare), FrequentedBlock(plan.header));
            }
            asks->appendNew<Value>(m_proc, Branch, origin, asks->appendNew<Value>(m_proc, plan.relation, origin, asks->appendNew<VariableValue>(m_proc, Get, origin, testedVariable), appendFarBound(plan, bounds, asks)));
            asks->setSuccessors(FrequentedBlock(stretch.next, FrequencyClass::Rare), FrequentedBlock(plan.header));
        }
        Value* farBoundOfTurns = stretch.tests ? stretch.farBound : bounds.farBound;
        enterAgainBehindRareEdges(cloner, plan, bounds, turns.span(), unrolledHeader);
        unsigned testingTurn = plan.testsCounterBeforeStep ? 0 : plan.factor - 1;
        const FrequentedBlock staying = plan.test->successor(plan.stayingSuccessorIndex);
        const FrequentedBlock leaving = plan.test->successor(1 - plan.stayingSuccessorIndex);

        for (unsigned turn = 0; turn < plan.factor; ++turn) {
            BasicBlock* ownHeader = turns[turn].get(plan.header);
            BasicBlock* nextHeader = turns[(turn + 1) % plan.factor].get(plan.header);
            for (BasicBlock* block : plan.blocks) {
                BasicBlock* copy = turns[turn].get(block);
                copy->replaceSuccessor(ownHeader, nextHeader);
                if (!turn && (!plan.poll || plan.pollsOnceForAllWrittenOutTurns))
                    continue;
                for (Value* value : *copy) {
                    if (isLoopPoll(value))
                        value->replaceWithNop();
                }
            }
            // The branch at the end of the block is about to give way to a jump along one of its edges.
            auto tellLogWhichWayIsTaken = [&](BasicBlock* block, unsigned takenSuccessorIndex) {
                if (Value* test = block->last()->child(0); test->numChildren() == 2)
                    m_proc.movedLoadLog().addDecidedComparison(m_proc, test, block, block->size() - 1, test->child(0), test->child(1), !takenSuccessorIndex);
            };
            for (const Plan::RepeatedTest& repeated : plan.repeatedTests) {
                BasicBlock* copy = turns[turn].get(repeated.block);
                BasicBlock* staying = copy->successorBlock(repeated.stayingSuccessorIndex);
                tellLogWhichWayIsTaken(copy, repeated.stayingSuccessorIndex);
                copy->replaceLastWithNew<Value>(m_proc, Jump, origin);
                copy->setSuccessors(FrequentedBlock(staying));
            }
            for (const Plan::SideTest& side : plan.sideTests) {
                BasicBlock* test = turns[turn].get(side.block);
                BasicBlock* stayingCopy = test->successorBlock(side.stayingSuccessorIndex);
                if (turn != testingTurn) {
                    tellLogWhichWayIsTaken(test, side.stayingSuccessorIndex);
                    test->replaceLastWithNew<Value>(m_proc, Jump, origin);
                    test->setSuccessors(FrequentedBlock(stayingCopy));
                    continue;
                }
                BasicBlock* rest = m_proc.addBlock();
                Value* tested = m_proc.add<VariableValue>(Get, origin, testedVariable);
                test->appendNonTerminal(tested);
                Value* hasRoom = m_proc.add<Value>(plan.relation, origin, tested, farBoundOfTurns);
                test->appendNonTerminal(hasRoom);
                test->last()->child(0) = hasRoom;
                test->setSuccessors(FrequentedBlock(stayingCopy, side.block->successor(side.stayingSuccessorIndex).frequency()), FrequentedBlock(rest, side.block->successor(1 - side.stayingSuccessorIndex).frequency()));
                BasicBlock* behindTest = copyBlocksBehindTest(cloner, plan, side.blocksBehindTest, headerBehindRest);
                Value* goesOn = rest->appendNew<Value>(m_proc, plan.relation, origin, rest->appendNew<VariableValue>(m_proc, Get, origin, testedVariable), bounds.bound);
                rest->appendNew<Value>(m_proc, Branch, origin, goesOn);
                rest->setSuccessors(FrequentedBlock(behindTest), FrequentedBlock(side.block->successorBlock(1 - side.stayingSuccessorIndex)));
            }
            BasicBlock* test = turns[turn].get(plan.test);
            FrequentedBlock stayingCopy = test->successor(plan.stayingSuccessorIndex);
            if (turn != testingTurn) {
                tellLogWhichWayIsTaken(test, plan.stayingSuccessorIndex);
                test->replaceLastWithNew<Value>(m_proc, Jump, origin);
                test->setSuccessors(FrequentedBlock(stayingCopy.block()));
                continue;
            }

            BasicBlock* rest = plan.testsCounterBeforeStep ? headerBehindRest : m_proc.addBlock();
            Value* tested = m_proc.add<VariableValue>(Get, origin, testedVariable);
            test->appendNonTerminal(tested);
            Value* hasRoom = m_proc.add<Value>(plan.relation, origin, tested, farBoundOfTurns);
            test->appendNonTerminal(hasRoom);
            test->last()->child(0) = hasRoom;
            test->setSuccessors(FrequentedBlock(stayingCopy.block(), staying.frequency()), FrequentedBlock(rest, leaving.frequency()));
            if (plan.testsCounterBeforeStep)
                continue;

            BasicBlock* behindTest = copyBlocksBehindTest(cloner, plan, plan.blocksBehindTest, headerBehindRest);
            for (unsigned index = 0; index < plan.sums.size(); ++index) {
                Value* added = rest->appendNew<Value>(m_proc, IToD, origin, rest->appendNew<VariableValue>(m_proc, Get, origin, integers[index]));
                Value* sum = rest->appendNew<Value>(m_proc, Add, origin, stretch.sumsInFront[index], added);
                for (Variable* variable : variablesOf(plan, plan.sums[index].sum))
                    rest->appendNew<VariableValue>(m_proc, Set, origin, variable, sum);
                rest->appendNew<VariableValue>(m_proc, Set, origin, stretch.firstSums[index], sum);
            }
            Value* testedInRest = rest->appendNew<VariableValue>(m_proc, Get, origin, testedVariable);
            Value* goesOn = rest->appendNew<Value>(m_proc, plan.relation, origin, testedInRest, bounds.bound);
            rest->appendNew<Value>(m_proc, Branch, origin, goesOn);
            rest->setSuccessors(FrequentedBlock(behindTest), FrequentedBlock(leaving.block()));
        }

        if (!plan.poll)
            return;
        removePolls(plan);
    }

    static BasicBlock* copyBlocksBehindTest(RegionCloner& cloner, const Plan& plan, const Vector<BasicBlock*, 2>& blocksBehindTest, BasicBlock* header)
    {
        if (blocksBehindTest.isEmpty())
            return header;
        RegionCloner::BlockMap copies = cloner.cloneBlocks(blocksBehindTest.span());
        for (BasicBlock* block : blocksBehindTest)
            copies.get(block)->replaceSuccessor(plan.header, header);
        return copies.get(blocksBehindTest[0]);
    }

    Procedure& m_proc;
    std::optional<PhiChildren> m_phiChildren;
    Vector<Plan> m_plans;
    Vector<Plan> m_plansOfCopiesThatPoll;
    WritesOutTurns m_writesOutTurns;
    bool m_pollsOfLoopCanMove { false };
};

} // anonymous namespace

bool unrollLoops(Procedure& proc, WritesOutTurns writesOutTurns)
{
    PhaseScope phaseScope(proc, "unrollLoops"_s);
    UnrollLoops unrollLoops(proc, writesOutTurns);
    return unrollLoops.run();
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
