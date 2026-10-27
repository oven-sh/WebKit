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
    explicit UnrollLoops(Procedure& proc)
        : m_proc(proc)
    {
    }

    bool run()
    {
        NaturalLoops& loops = m_proc.naturalLoops();
        for (unsigned loopIndex = loops.numLoops(); loopIndex--;)
            considerLoop(loops, loops.loop(loopIndex));
        m_phiChildren = std::nullopt;
        if (m_plans.isEmpty())
            return false;

        RegionCloner cloner(m_proc);
        for (const Plan& plan : m_plans) {
            for (BasicBlock* block : plan.blocks)
                cloner.addBlock(block);
            for (BasicBlock* block : plan.blocksBehindRareEdges)
                cloner.addBlock(block);
        }
        cloner.demoteEscapingValues();
        m_proc.resetValueOwners();
        for (const Plan& plan : m_plans)
            unroll(cloner, plan);
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
    };

    struct Counter {
        Value* phi { nullptr };
        Value* start { nullptr };
        Value* next { nullptr };
        int64_t step { 0 };
    };

    // A patchpoint that is given no register has nothing to compute with: it only holds its operands alive.
    static bool onlyKeepsValuesAlive(const Value* value)
    {
        auto* patchpoint = value->as<PatchpointValue>();
        if (!patchpoint || patchpoint->type() != Void || patchpoint->numGPScratchRegisters || patchpoint->numFPScratchRegisters || isLoopPoll(value))
            return false;
        if (!patchpoint->earlyClobbered().isEmpty() || !patchpoint->lateClobbered().isEmpty())
            return false;
        for (unsigned index = 0; index < patchpoint->numChildren(); ++index) {
            ValueRep::Kind kind = patchpoint->constrainedChild(index).rep().kind();
            if (kind != ValueRep::ColdAny && kind != ValueRep::LateColdAny)
                return false;
        }
        return true;
    }

    // Beside a call the test of the counter costs little.
    static bool isCall(const Value* value)
    {
        if (value->opcode() == CCall)
            return true;
        auto* patchpoint = value->as<PatchpointValue>();
        if (!patchpoint)
            return false;
        RegisterSet scratch = RegisterSet::macroClobberedGPRs();
        scratch.merge(RegisterSet::macroClobberedFPRs());
        return !scratch.subsumes(patchpoint->earlyClobbered()) || !scratch.subsumes(patchpoint->lateClobbered());
    }

    // A word of 64 bits in memory is a pointer or a value of any type. Lists of those are mostly short, and most loops that store them copy or fill one.
    static bool storesWord(const Value* value)
    {
        auto* memory = value->as<MemoryValue>();
        return memory && memory->opcode() == Store && memory->child(0)->type() == Int64;
    }

    static bool countsTowardsSize(const Value* value)
    {
        if (!emitsInstructions(value) || onlyKeepsValuesAlive(value))
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

    bool readTest(const NaturalLoops& loops, const NaturalLoop& loop, const IndexSet<BasicBlock*>& likelyBlocks, BasicBlock* block, PhiChildren& phiChildren, Plan& plan)
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
            plan.testsCounterBeforeStep = tested == counter->phi;
            plan.start = counter->start;
            plan.bound = bound;
            plan.step = counter->step;
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

    // Two reads of one place in front of the loop, with nothing between them that writes there.
    static bool readsWhatBoundRead(Value* value, Value* bound)
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
                    if (between->effects().writes.overlaps(first->range()) || between->effects().fence)
                        return false;
                }
                if (block == first->owner)
                    return true;
                if (block->numPredecessors() != 1 || ++numberOfBlocks > 2 * Limits::maxBlocksBesideTestOfUnrolledLoop)
                    return false;
            }
        };
        return nothingWritesBetween(load, other) || nothingWritesBetween(other, load);
    }

    // A range check against the bound of the loop is such a test. In the loop itself it falls to the test above it; where the turns are written out that test is gone.
    static void findRepeatedTests(Plan& plan)
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

    // While a sum of whole numbers stays under 2^53 every addition of doubles is exact, so the turns that are written out may add integers.
    // The sum is made a double again where they are left, which is at the test alone.
    void findSumsOfIntegers(PhiChildren& phiChildren, Plan& plan)
    {
        if (plan.testsCounterBeforeStep)
            return;
        for (BasicBlock* block : plan.blocks) {
            for (unsigned index = 0; index < block->numSuccessors(); ++index) {
                if (!plan.blocks.contains(block->successorBlock(index)) && (block != plan.test || index == plan.stayingSuccessorIndex))
                    return;
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
        // The end of a stretch is computed from wherever the counter stands.
        Signedness signedness = isUnsignedRelation(plan.relation) ? Signedness::Unsigned : Signedness::Signed;
        IntegerInterval counter = intervalFor(m_proc, plan.tested, signedness).join(intervalFor(m_proc, plan.start, signedness));
        IntegerInterval full = fullInterval(plan.tested->type(), signedness);
        Int128 stretch = Int128 { plan.step } * (turnsOfStretch + plan.factor);
        if (counter.min + stretch < full.min || counter.max + stretch > full.max)
            plan.sums.clear();
        if (plan.sums.isEmpty())
            return;
        for (Value* value : m_proc.values()) {
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

    // A turn may read one place twice, once to see what is there and once to take it. In the loop itself the later walk over all values finds that. In the turns that are written out
    // each read gets an address of its own making, and the walk compares addresses by the value that computes them.
    unsigned mergeRepeatedLoads(const Plan& plan)
    {
        RangeSet<HeapRange> written;
        Vector<MemoryValue*, 8> loads;
        for (BasicBlock* block : plan.blocks) {
            for (Value* value : *block) {
                Effects effects = value->effects();
                if (effects.fence)
                    return 0;
                effects.writes.addTo(written);
                MemoryValue* memory = value->as<MemoryValue>();
                if (memory && memory->isLoad() && !memory->hasFence())
                    loads.append(memory);
            }
        }
        Dominators& dominators = m_proc.dominators();
        unsigned numberOfMergedLoads = 0;
        for (MemoryValue* load : loads) {
            if (written.overlaps(load->range()))
                continue;
            for (MemoryValue* earlier : loads) {
                if (earlier == load || earlier->opcode() == Identity || earlier->kind() != load->kind() || earlier->type() != load->type())
                    continue;
                if (!computesSameValue(earlier->lastChild(), load->lastChild(), Limits::maxDepthOfAddressInUnrolledLoop) || earlier->offset() != load->offset() || earlier->range() != load->range())
                    continue;
                bool comesFirst = earlier->owner == load->owner ? earlier->owner->values().find(earlier) < load->owner->values().find(load) : dominators.strictlyDominates(earlier->owner, load->owner);
                if (!comesFirst)
                    continue;
                load->replaceWithIdentity(earlier);
                ++numberOfMergedLoads;
                break;
            }
        }
        return numberOfMergedLoads;
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
        numberOfValues -= mergeRepeatedLoads(plan);
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
            m_plans.append(WTF::move(plan));
            return;
        }
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

    void enterAgainBehindRareEdges(RegionCloner& cloner, const Plan& plan, const Bounds& bounds, std::span<const RegionCloner::BlockMap> turns)
    {
        if (plan.blocksBehindRareEdges.isEmpty())
            return;
        Origin origin = plan.test->last()->origin();
        RELEASE_ASSERT(plan.counter->opcode() == Identity && plan.counter->child(0)->opcode() == Get);
        BasicBlock* again = m_proc.addBlock();
        Value* counter = again->appendNew<VariableValue>(m_proc, Get, origin, plan.counter->child(0)->as<VariableValue>()->variable());
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
        again->setSuccessors(FrequentedBlock(bounds.entry ? bounds.entry : turns[0].get(plan.header)), FrequentedBlock(plan.header));

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

    Bounds appendBoundsAndEnter(const Plan& plan, BasicBlock* unrolledHeader)
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
        int64_t room = plan.step * static_cast<int64_t>(plan.factor - 1);
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
                testOfFirstTurn->setSuccessors(FrequentedBlock(other), FrequentedBlock(found->blocksBehind[0]));
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
    // So many integers of 32 bits add up to less than 2^52, with the turns that are written out once more.
    static constexpr int64_t turnsOfStretch = 1ll << 20;

    // The turns that are written out run in stretches. In front of each the sums are tested and the integers start at zero, so nothing in the loop looks at them.
    struct Stretch {
        BasicBlock* first { nullptr };
        BasicBlock* tests { nullptr };
        Variable* end { nullptr };
        Vector<Variable*, 1> firstSums;
        Vector<Value*, 1> sumsInFront;
        Value* farBound { nullptr };
    };

    // Adding 1.5 * 2^52 to a number under 2^51 rounds it to a whole number.
    void appendTestsOfSums(const Plan& plan, Stretch& stretch, std::span<Variable* const> integers, const Bounds& bounds, BasicBlock* unrolledHeader)
    {
        Origin origin = plan.test->last()->origin();
        BasicBlock* tests = stretch.tests;
        Type type = plan.bound->type();
        stretch.first->appendNew<VariableValue>(m_proc, Set, origin, stretch.end, stretch.first->appendNew<Value>(m_proc, Add, origin, plan.start, stretch.first->appendIntConstant(m_proc, origin, type, turnsOfStretch * plan.step)));
        stretch.first->appendNew<Value>(m_proc, Jump, origin);
        stretch.first->setSuccessors(FrequentedBlock(tests));
        Value* end = tests->appendNew<VariableValue>(m_proc, Get, origin, stretch.end);
        stretch.farBound = tests->appendNew<Value>(m_proc, Select, origin, tests->appendNew<Value>(m_proc, plan.relation, origin, end, bounds.farBound), end, bounds.farBound);
        Value* rounding = tests->appendNew<ConstDoubleValue>(m_proc, origin, 1.5 * static_cast<double>(greatestSumOfIntegers));
        Value* greatestStart = tests->appendNew<ConstDoubleValue>(m_proc, origin, static_cast<double>(greatestSumOfIntegers / 2));
        Value* enters = nullptr;
        for (unsigned index = 0; index < plan.sums.size(); ++index) {
            Value* start = tests->appendNew<VariableValue>(m_proc, Get, origin, stretch.firstSums[index]);
            stretch.sumsInFront.append(start);
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
            tests->appendNew<VariableValue>(m_proc, Set, origin, integers[index], tests->appendIntConstant(m_proc, origin, Int64, 0));
        }
        tests->appendNew<Value>(m_proc, Branch, origin, enters);
        tests->setSuccessors(FrequentedBlock(unrolledHeader), FrequentedBlock(plan.header, FrequencyClass::Rare));
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
        if (!plan.sums.isEmpty()) {
            stretch.first = m_proc.addBlock();
            stretch.tests = m_proc.addBlock();
            stretch.end = m_proc.addVariable(plan.tested->type());
            for (const SumOfIntegers& sum : plan.sums) {
                stretch.firstSums.append(m_proc.addVariable(Double));
                plan.preHeader->appendNonTerminal(m_proc.add<VariableValue>(Set, origin, stretch.firstSums.last(), sum.start));
            }
        }
        Bounds bounds = appendBoundsAndEnter(plan, stretch.first ? stretch.first : turns[0].get(plan.header));
        if (stretch.tests)
            appendTestsOfSums(plan, stretch, integers.span(), bounds, turns[0].get(plan.header));
        enterAgainBehindRareEdges(cloner, plan, bounds, turns.span());
        unsigned testingTurn = plan.testsCounterBeforeStep ? 0 : plan.factor - 1;
        const FrequentedBlock staying = plan.test->successor(plan.stayingSuccessorIndex);
        const FrequentedBlock leaving = plan.test->successor(1 - plan.stayingSuccessorIndex);

        for (unsigned turn = 0; turn < plan.factor; ++turn) {
            BasicBlock* ownHeader = turns[turn].get(plan.header);
            BasicBlock* nextHeader = turns[(turn + 1) % plan.factor].get(plan.header);
            for (BasicBlock* block : plan.blocks) {
                BasicBlock* copy = turns[turn].get(block);
                copy->replaceSuccessor(ownHeader, nextHeader);
                if (!turn)
                    continue;
                for (Value* value : *copy) {
                    if (isLoopPoll(value))
                        value->replaceWithNop();
                }
            }
            for (const Plan::RepeatedTest& repeated : plan.repeatedTests) {
                BasicBlock* copy = turns[turn].get(repeated.block);
                BasicBlock* staying = copy->successorBlock(repeated.stayingSuccessorIndex);
                copy->replaceLastWithNew<Value>(m_proc, Jump, origin);
                copy->setSuccessors(FrequentedBlock(staying));
            }
            for (const Plan::SideTest& side : plan.sideTests) {
                BasicBlock* test = turns[turn].get(side.block);
                BasicBlock* stayingCopy = test->successorBlock(side.stayingSuccessorIndex);
                if (turn != testingTurn) {
                    test->replaceLastWithNew<Value>(m_proc, Jump, origin);
                    test->setSuccessors(FrequentedBlock(stayingCopy));
                    continue;
                }
                BasicBlock* rest = m_proc.addBlock();
                Value* tested = m_proc.add<VariableValue>(Get, origin, testedVariable);
                test->appendNonTerminal(tested);
                Value* hasRoom = m_proc.add<Value>(plan.relation, origin, tested, bounds.farBound);
                test->appendNonTerminal(hasRoom);
                test->last()->child(0) = hasRoom;
                test->setSuccessors(FrequentedBlock(stayingCopy, side.block->successor(side.stayingSuccessorIndex).frequency()), FrequentedBlock(rest, side.block->successor(1 - side.stayingSuccessorIndex).frequency()));
                BasicBlock* behindTest = side.blocksBehindTest.isEmpty() ? plan.header : cloner.cloneBlocks(side.blocksBehindTest.span()).get(side.blocksBehindTest[0]);
                Value* goesOn = rest->appendNew<Value>(m_proc, plan.relation, origin, rest->appendNew<VariableValue>(m_proc, Get, origin, testedVariable), bounds.bound);
                rest->appendNew<Value>(m_proc, Branch, origin, goesOn);
                rest->setSuccessors(FrequentedBlock(behindTest), FrequentedBlock(side.block->successorBlock(1 - side.stayingSuccessorIndex)));
            }
            BasicBlock* test = turns[turn].get(plan.test);
            FrequentedBlock stayingCopy = test->successor(plan.stayingSuccessorIndex);
            if (turn != testingTurn) {
                test->replaceLastWithNew<Value>(m_proc, Jump, origin);
                test->setSuccessors(FrequentedBlock(stayingCopy.block()));
                continue;
            }

            BasicBlock* rest = plan.testsCounterBeforeStep ? plan.header : m_proc.addBlock();
            Value* tested = m_proc.add<VariableValue>(Get, origin, testedVariable);
            test->appendNonTerminal(tested);
            Value* hasRoom = m_proc.add<Value>(plan.relation, origin, tested, stretch.tests ? stretch.farBound : bounds.farBound);
            test->appendNonTerminal(hasRoom);
            test->last()->child(0) = hasRoom;
            test->setSuccessors(FrequentedBlock(stayingCopy.block(), staying.frequency()), FrequentedBlock(rest, leaving.frequency()));
            if (plan.testsCounterBeforeStep)
                continue;

            // Behind a stretch there may be room for another.
            BasicBlock* headerBehindRest = plan.header;
            if (stretch.tests) {
                headerBehindRest = m_proc.addBlock();
                Type type = plan.tested->type();
                Value* nextEnd = headerBehindRest->appendNew<Value>(m_proc, Add, origin, headerBehindRest->appendNew<VariableValue>(m_proc, Get, origin, stretch.end), headerBehindRest->appendIntConstant(m_proc, origin, type, turnsOfStretch * plan.step));
                headerBehindRest->appendNew<VariableValue>(m_proc, Set, origin, stretch.end, nextEnd);
                headerBehindRest->appendNew<Value>(m_proc, Branch, origin, headerBehindRest->appendNew<Value>(m_proc, plan.relation, origin, headerBehindRest->appendNew<VariableValue>(m_proc, Get, origin, testedVariable), bounds.farBound));
                headerBehindRest->setSuccessors(FrequentedBlock(stretch.tests, FrequencyClass::Rare), FrequentedBlock(plan.header));
            }
            BasicBlock* behindTest = headerBehindRest;
            if (!plan.blocksBehindTest.isEmpty()) {
                RegionCloner::BlockMap copies = cloner.cloneBlocks(plan.blocksBehindTest.span());
                behindTest = copies.get(plan.blocksBehindTest[0]);
                for (BasicBlock* block : plan.blocksBehindTest)
                    copies.get(block)->replaceSuccessor(plan.header, headerBehindRest);
            }
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
    }

    Procedure& m_proc;
    std::optional<PhiChildren> m_phiChildren;
    Vector<Plan> m_plans;
};

} // anonymous namespace

bool unrollLoops(Procedure& proc)
{
    PhaseScope phaseScope(proc, "unrollLoops"_s);
    UnrollLoops unrollLoops(proc);
    return unrollLoops.run();
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
