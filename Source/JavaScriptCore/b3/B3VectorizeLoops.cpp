/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "B3VectorizeLoops.h"

#if ENABLE(B3_JIT)

#include "B3BasicBlockInlines.h"
#include "B3CountedLoops.h"
#include "B3DominatingConditions.h"
#include "B3Dominators.h"
#include "B3FixSSA.h"
#include "B3IntRange.h"
#include "B3Limits.h"
#include "B3LoopUtilities.h"
#include "B3MemoryValue.h"
#include "B3NaturalLoops.h"
#include "B3PatchpointValue.h"
#include "B3PhaseScope.h"
#include "B3PhiChildren.h"
#include "B3ProcedureInlines.h"
#include "B3SIMDValue.h"
#include "B3UpsilonValue.h"
#include "B3ValueInlines.h"
#include "B3VariableValue.h"
#include <optional>
#include <wtf/IndexSet.h>

namespace JSC { namespace B3 {

namespace {

static constexpr bool verbose = false;

// The phase looks at few loops and changes fewer. Its own size counts for more than its speed.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)

class VectorizeLoops {
public:
    explicit VectorizeLoops(Procedure& proc)
        : m_proc(proc)
    {
    }

    bool run()
    {
        NaturalLoops& loops = m_proc.naturalLoops();
        for (unsigned loopIndex = loops.numLoops(); loopIndex--;)
            considerLoop(loops, loops.loop(loopIndex));
        m_phiChildren = std::nullopt;
        dropPlansWhoseTurnsAreReadBehindLoop();
        if (m_plans.isEmpty())
            return false;

        m_proc.setUsesSIMD();
        IndexSet<Value*> valuesToDemote;
        for (auto& plan : m_plans) {
            appendVectorLoop(*plan);
            for (const Counter& counter : plan->counters()) {
                valuesToDemote.add(counter.phi);
                if (!plan->testsCounterBeforeStep)
                    valuesToDemote.add(counter.next);
            }
        }
        demoteValues(m_proc, valuesToDemote);
        for (auto& plan : m_plans)
            leaveVectorLoop(*plan);
        m_proc.resetReachability();
        m_proc.invalidateCFG();
        return true;
    }

private:
    static constexpr unsigned bytesPerVector = 16;

    // A Phi of the header that every turn moves on by the same distance.
    struct Counter {
        Value* phi { nullptr };
        Value* start { nullptr };
        Value* next { nullptr };
        int64_t step { 0 };
        Value* phiOfVectorLoop { nullptr };
        Value* nextOfVectorLoop { nullptr };
    };

    // factorOfCounter * counter + constant + the sum of factor * value over the terms, in arithmetic modulo 2^64. No term changes in the loop.
    struct LinearForm {
        struct Term {
            Value* value { nullptr };
            int64_t factor { 0 };
        };

        static int64_t times(int64_t left, int64_t right) { return static_cast<int64_t>(static_cast<uint64_t>(left) * static_cast<uint64_t>(right)); }
        static int64_t plus(int64_t left, int64_t right) { return static_cast<int64_t>(static_cast<uint64_t>(left) + static_cast<uint64_t>(right)); }

        bool addTerm(Value* value, int64_t factor)
        {
            for (unsigned index = 0; index < numberOfTerms; ++index) {
                if (terms[index].value != value)
                    continue;
                terms[index].factor = plus(terms[index].factor, factor);
                if (!terms[index].factor)
                    terms[index] = terms[--numberOfTerms];
                return true;
            }
            if (numberOfTerms == Limits::maxTermsOfAddressInVectorizedLoop)
                return false;
            terms[numberOfTerms++] = { value, factor };
            return true;
        }

        // False: the sum hangs on two counters or has too many terms.
        bool add(const LinearForm& other, int64_t factor)
        {
            if (other.counter) {
                if (counter && counter != other.counter)
                    return false;
                counter = other.counter;
                factorOfCounter = plus(factorOfCounter, times(other.factorOfCounter, factor));
                if (!factorOfCounter)
                    counter = nullptr;
            }
            constant = plus(constant, times(other.constant, factor));
            for (unsigned index = 0; index < other.numberOfTerms; ++index) {
                if (!addTerm(other.terms[index].value, times(other.terms[index].factor, factor)))
                    return false;
            }
            return true;
        }

        Term terms[Limits::maxTermsOfAddressInVectorizedLoop];
        unsigned numberOfTerms { 0 };
        Value* counter { nullptr };
        int64_t factorOfCounter { 0 };
        int64_t constant { 0 };
    };

    struct Access {
        MemoryValue* memory { nullptr };
        LinearForm address;
        // From turn to turn the address rises or falls by the size of an element.
        bool falls { false };
    };

    // What the vector loop computes in the place of a value of the turn: the value of the first of its turns, or one lane for each.
    struct Copy {
        Value* original { nullptr };
        Value* scalar { nullptr };
        Value* vector { nullptr };
        bool isScalar { false };
        bool isVector { false };
    };

    struct Plan {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(Plan);

        Vector<BasicBlock*, 8> blocks;
        IndexSet<BasicBlock*> blockSet;
        Vector<BasicBlock*, 4> blocksNeverEntered;
        // Outside the loop: nothing leads to them but an edge that is never taken.
        Vector<BasicBlock*, 4> blocksBehindEdgesNeverTaken;
        BasicBlock* preHeader { nullptr };
        BasicBlock* header { nullptr };
        BasicBlock* test { nullptr };
        unsigned stayingSuccessorIndex { 0 };
        Opcode relation { Oops };
        // The first is the one that is tested.
        Counter counterTable[Limits::maxCountersOfVectorizedLoop];
        unsigned numberOfCounters { 0 };
        // It is tested against a value that does not change in the loop, or against another counter.
        Value* bound { nullptr };
        unsigned indexOfBoundingCounter { 0 };
        bool testsCounterBeforeStep { false };
        bool startHasRoom { false };
        bool needsGuardOfFarBound { false };
        int64_t farthestBoundWithRoom { 0 };
        unsigned bytesPerLane { 0 };
        Access accessTable[Limits::maxAccessesPerTurnOfVectorizedLoop];
        unsigned numberOfAccesses { 0 };
        // Each must not lie between 0 and the bytes of a vector.
        LinearForm distancesTestedAtEntry[Limits::maxDistancesTestedInFrontOfVectorizedLoop];
        unsigned numberOfDistancesTestedAtEntry { 0 };
        // Lane 0 holds the last of the turns, where every address falls.
        bool lanesRunBackward { false };
        Copy copyTable[Limits::maxValuesPerTurnOfVectorizedLoop];
        unsigned numberOfCopies { 0 };
        // Behind a test at the bottom: what the last turn hands to Phis behind the loop.
        Vector<Value*, 4> upsilonsForPhisBehindLoop;

        BasicBlock* entered { nullptr };
        BasicBlock* turn { nullptr };
        BasicBlock* again { nullptr };
        BasicBlock* left { nullptr };
        // The loop itself keeps one way in.
        BasicBlock* inFrontOfLoop { nullptr };
        Value* lanesInOppositeOrder { nullptr };

        std::span<Counter> counters() { return std::span { counterTable, numberOfCounters }; }
        std::span<const Counter> counters() const { return std::span { counterTable, numberOfCounters }; }
        unsigned lanes() const { return bytesPerVector / bytesPerLane; }
        const Counter& tested() const { return counterTable[0]; }
        const Counter* boundingCounter() const { return indexOfBoundingCounter ? &counterTable[indexOfBoundingCounter] : nullptr; }
        // How much nearer the tested counter comes to what it is tested against in the turns behind the first that a turn of the vector loop stands for.
        int64_t room() const { return (tested().step - (boundingCounter() ? boundingCounter()->step : 0)) * static_cast<int64_t>(lanes() - 1); }
        Signedness signedness() const { return isUnsignedRelation(relation) ? Signedness::Unsigned : Signedness::Signed; }
        bool includesEqual() const { return relation == LessEqual || relation == GreaterEqual || relation == BelowEqual || relation == AboveEqual; }
        bool isInvariant(const Value* value) const { return value->isConstant() || !blockSet.contains(value->owner); }

        const Counter* counterFor(const Value* value) const
        {
            for (const Counter& counter : counters()) {
                if (counter.phi == value)
                    return &counter;
            }
            return nullptr;
        }

        bool isCounterOrNext(const Value* value) const
        {
            for (const Counter& counter : counters()) {
                if (counter.phi == value || counter.next == value)
                    return true;
            }
            return false;
        }

        Copy* copyOf(const Value* value)
        {
            for (unsigned index = 0; index < numberOfCopies; ++index) {
                if (copyTable[index].original == value)
                    return &copyTable[index];
            }
            return nullptr;
        }

        // Null: more values than a turn may have, with those from outside that it spreads over lanes.
        Copy* ensureCopyOf(Value* value)
        {
            if (Copy* copy = copyOf(value))
                return copy;
            if (numberOfCopies == Limits::maxValuesPerTurnOfVectorizedLoop)
                return nullptr;
            Copy* copy = &copyTable[numberOfCopies++];
            copy->original = value;
            return copy;
        }
    };

    // One way leads through a turn, and one branch on it leaves the loop. A branch on a constant is a jump, so the blocks of the loop that are not on that way are never entered.
    static bool findTurn(const NaturalLoops& loops, const NaturalLoop& loop, Plan& plan)
    {
        BasicBlock* block = plan.header;
        do {
            if (plan.blocks.size() == loop.size())
                return false;
            plan.blocks.append(block);
            plan.blockSet.add(block);
            Value* last = block->last();
            unsigned successorIndex = 0;
            if (last->opcode() == Branch) {
                Value* condition = last->child(0)->foldIdentity();
                if (condition->hasInt()) {
                    successorIndex = condition->asInt() ? 0 : 1;
                    BasicBlock* other = block->successorBlock(1 - successorIndex);
                    if (!loops.belongsTo(other, loop) && other->numPredecessors() == 1)
                        plan.blocksBehindEdgesNeverTaken.append(other);
                } else {
                    bool firstStays = loops.belongsTo(block->successorBlock(0), loop);
                    if (plan.test || firstStays == loops.belongsTo(block->successorBlock(1), loop)) {
                        dataLogLnIf(verbose, "   second branch: ", deepDump(last), " in ", *block);
                        return false;
                    }
                    plan.test = block;
                    plan.stayingSuccessorIndex = firstStays ? 0 : 1;
                    successorIndex = plan.stayingSuccessorIndex;
                }
            } else if (last->opcode() != Jump) {
                dataLogLnIf(verbose, "   terminal: ", deepDump(last));
                return false;
            }
            block = block->successorBlock(successorIndex);
            if (!loops.belongsTo(block, loop))
                return false;
        } while (block != plan.header);
        if (!plan.test)
            return false;
        for (unsigned index = 0; index < plan.blocks.size(); ++index) {
            BasicBlock* before = plan.blocks[index ? index - 1 : plan.blocks.size() - 1];
            for (BasicBlock* predecessor : plan.blocks[index]->predecessors()) {
                if (predecessor != before && plan.blockSet.contains(predecessor))
                    return false;
            }
        }
        for (unsigned index = loop.size(); index--;) {
            if (!plan.blockSet.contains(loop.at(index)))
                plan.blocksNeverEntered.append(loop.at(index));
        }
        return true;
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

    static bool readCounter(PhiChildren& phiChildren, const Plan& plan, Counter& counter)
    {
        for (UpsilonValue* upsilon : phiChildren[counter.phi]) {
            if (plan.blocksNeverEntered.contains(upsilon->owner))
                continue;
            Value* input = upsilon->child(0)->foldIdentity();
            Value*& place = upsilon->owner == plan.preHeader ? counter.start : counter.next;
            if (place || (upsilon->owner != plan.preHeader && !plan.blockSet.contains(upsilon->owner)))
                return false;
            place = input;
        }
        if (!counter.start || !counter.next || !counter.phi->type().isInt())
            return false;
        std::optional<int64_t> step = stepOf(counter.next, counter.phi);
        if (!step || !*step || *step > Limits::maxCountedLoopStep || *step < -Limits::maxCountedLoopStep)
            return false;
        counter.step = *step;
        return true;
    }

    static bool findCounters(PhiChildren& phiChildren, Plan& plan)
    {
        for (Value* phi : *plan.header) {
            if (phi->opcode() != Phi)
                continue;
            if (plan.numberOfCounters == Limits::maxCountersOfVectorizedLoop)
                return false;
            Counter& counter = plan.counterTable[plan.numberOfCounters++];
            counter.phi = phi;
            if (!readCounter(phiChildren, plan, counter)) {
                dataLogLnIf(verbose, "   phi: ", deepDump(phi));
                return false;
            }
        }
        return plan.numberOfCounters;
    }

    // A test at the top has nothing in front of it that must not run once more. A test at the bottom has nothing behind it but what hands values to the next turn.
    static bool hasWholeTurnsBetweenTests(const Plan& plan)
    {
        bool isBehindTest = false;
        for (BasicBlock* block : plan.blocks) {
            for (Value* value : *block) {
                if (plan.testsCounterBeforeStep && !isBehindTest) {
                    Effects effects = value->effects();
                    auto* upsilon = value->as<UpsilonValue>();
                    if (effects.writes || effects.writesPinned || effects.exitsSideways || effects.fence || (upsilon && upsilon->phi()->owner == plan.header))
                        return false;
                }
                if (!plan.testsCounterBeforeStep && isBehindTest && emitsInstructions(value))
                    return false;
            }
            isBehindTest |= block == plan.test;
        }
        return true;
    }

    static int indexOfCounter(const Plan& plan, Value* value, bool beforeStep)
    {
        for (unsigned index = 0; index < plan.numberOfCounters; ++index) {
            if (value == (beforeStep ? plan.counterTable[index].phi : plan.counterTable[index].next))
                return index;
        }
        return -1;
    }

    static bool readTest(Plan& plan)
    {
        DominatingCondition condition = stripNegations(plan.test->last()->child(0)->foldIdentity());
        Value* comparison = condition.value->foldIdentity();
        std::optional<Opcode> inverted = invertedRelation(comparison->opcode());
        if (!inverted || !comparison->child(0)->type().isInt())
            return false;
        Opcode relation = !plan.stayingSuccessorIndex == condition.isNonZero ? comparison->opcode() : *inverted;

        for (unsigned form = 0; form < 4; ++form) {
            bool beforeStep = form & 2;
            unsigned testedIndex = form & 1;
            Value* bound = comparison->child(1 - testedIndex)->foldIdentity();
            int tested = indexOfCounter(plan, comparison->child(testedIndex)->foldIdentity(), beforeStep);
            int bounding = indexOfCounter(plan, bound, beforeStep);
            if (tested < 0 || (bounding < 0 && !plan.isInvariant(bound)))
                continue;
            int64_t approach = plan.counterTable[tested].step - (bounding < 0 ? 0 : plan.counterTable[bounding].step);
            plan.relation = testedIndex ? flippedRelation(relation) : relation;
            if (!approach || isLessRelation(plan.relation) != (approach > 0))
                continue;
            std::swap(plan.counterTable[0], plan.counterTable[tested]);
            if (bounding >= 0)
                plan.indexOfBoundingCounter = bounding ? bounding : tested;
            else
                plan.bound = bound;
            plan.testsCounterBeforeStep = beforeStep;
            return hasWholeTurnsBetweenTests(plan);
        }
        return false;
    }

    // The values of a counter in the turns that the turns of the vector loop stand for.
    IntegerInterval intervalOf(const Plan& plan, const Counter& counter, Signedness signedness)
    {
        IntegerInterval result = intervalFor(m_proc, counter.phi, plan.signedness());
        if (&counter == &plan.tested() && plan.bound) {
            IntegerInterval start = intervalFor(m_proc, counter.start, plan.signedness());
            IntegerInterval bound = intervalFor(m_proc, plan.bound, plan.signedness());
            Int128 strict = plan.includesEqual() ? 0 : 1;
            result.min = std::max(result.min, counter.step > 0 ? start.min : bound.min + strict);
            result.max = std::min(result.max, counter.step > 0 ? bound.max - strict : start.max);
        }
        if (signedness == plan.signedness() || (result.min >= 0 && result.max <= fullInterval(counter.phi->type(), Signedness::Signed).max))
            return result;
        return intervalFor(m_proc, counter.phi, signedness);
    }

    bool readLinearForm(const Plan& plan, Value* value, unsigned timeToLive, LinearForm& form)
    {
        value = value->foldIdentity();
        if (value->type() != Int64)
            return false;
        if (plan.counterFor(value)) {
            form.counter = value;
            form.factorOfCounter = 1;
            return true;
        }
        if (value->hasInt64()) {
            form.constant = value->asInt64();
            return true;
        }
        if (plan.isInvariant(value))
            return form.addTerm(value, 1);
        if (!timeToLive)
            return false;
        switch (value->opcode()) {
        case Add:
        case Sub: {
            LinearForm right;
            return readLinearForm(plan, value->child(0), timeToLive - 1, form) && readLinearForm(plan, value->child(1), timeToLive - 1, right) && form.add(right, value->opcode() == Add ? 1 : -1);
        }
        case Shl: {
            LinearForm shifted;
            return value->child(1)->hasInt32() && readLinearForm(plan, value->child(0), timeToLive - 1, shifted) && form.add(shifted, static_cast<int64_t>(1ull << (value->child(1)->asInt32() & 63)));
        }
        case ZExt32:
        case SExt32: {
            Signedness signedness = value->opcode() == ZExt32 ? Signedness::Unsigned : Signedness::Signed;
            for (const Counter& counter : plan.counters()) {
                auto affine = affineFormOf(m_proc, value, counter.phi, signedness);
                if (!affine)
                    continue;
                form.counter = counter.phi;
                form.factorOfCounter = 1;
                form.constant = affine->offset;
                return affine->exactBaseRange.contains(intervalOf(plan, counter, signedness));
            }
            return false;
        }
        default:
            return false;
        }
    }

    bool findAccesses(Plan& plan)
    {
        bool stores = false;
        plan.lanesRunBackward = true;
        for (BasicBlock* block : plan.blocks) {
            for (Value* value : *block) {
                MemoryValue* memory = value->as<MemoryValue>();
                if (!memory)
                    continue;
                if ((!memory->isLoad() && !memory->isStore()) || memory->hasFence() || plan.numberOfAccesses == Limits::maxAccessesPerTurnOfVectorizedLoop)
                    return false;
                Access& access = plan.accessTable[plan.numberOfAccesses++];
                access.memory = memory;
                if (!readLinearForm(plan, memory->lastChild(), Limits::maxDepthOfAddressInVectorizedLoop, access.address)) {
                    dataLogLnIf(verbose, "   address: ", deepDump(memory));
                    return false;
                }
                access.address.constant = LinearForm::plus(access.address.constant, memory->offset());
                int64_t bytes = memory->accessByteSize();
                if (!plan.bytesPerLane)
                    plan.bytesPerLane = bytes;
                // From turn to turn every access goes on to the element beside.
                int64_t stride = access.address.counter ? LinearForm::times(access.address.factorOfCounter, plan.counterFor(access.address.counter)->step) : 0;
                if (bytes != plan.bytesPerLane || bytes > 8 || (stride != bytes && stride != -bytes)) {
                    dataLogLnIf(verbose, "   stride: ", deepDump(memory), " goes on by ", stride);
                    return false;
                }
                access.falls = stride < 0;
                plan.lanesRunBackward &= access.falls;
                stores |= memory->isStore();
            }
        }
        return stores;
    }

    // The address in the first turn.
    static bool putStartInPlaceOfCounter(const Plan& plan, LinearForm& address)
    {
        Value* start = plan.counterFor(address.counter)->start;
        address.counter = nullptr;
        if (start->hasInt()) {
            address.constant = LinearForm::plus(address.constant, LinearForm::times(address.factorOfCounter, start->asInt()));
            return true;
        }
        return start->type() == Int64 && address.addTerm(start, address.factorOfCounter);
    }

    // Two counters that go towards each other by one, and the loop goes on while the lower is less than the higher. Whatever is indexed by the lower lies below whatever is indexed by the higher.
    static bool liesBelowInEveryTurn(const Plan& plan, const Access& lower, const Access& higher)
    {
        const Counter* bounding = plan.boundingCounter();
        if (!bounding || plan.includesEqual() || plan.tested().step != (isLessRelation(plan.relation) ? 1 : -1) || bounding->step != -plan.tested().step)
            return false;
        const Counter& low = isLessRelation(plan.relation) ? plan.tested() : *bounding;
        const Counter& high = isLessRelation(plan.relation) ? *bounding : plan.tested();
        if (lower.address.counter != low.phi || higher.address.counter != high.phi || lower.address.factorOfCounter != higher.address.factorOfCounter || lower.address.factorOfCounter <= 0)
            return false;
        LinearForm distance = higher.address;
        distance.counter = nullptr;
        LinearForm other = lower.address;
        other.counter = nullptr;
        return distance.add(other, -1) && !distance.numberOfTerms && distance.constant >= 0;
    }

    // How far the later access lies ahead of the earlier, in the direction both go.
    static bool readDistance(const Plan& plan, const Access& later, const Access& earlier, LinearForm& distance)
    {
        LinearForm first = later.address;
        LinearForm second = earlier.address;
        if (first.counter != second.counter && (!putStartInPlaceOfCounter(plan, first) || !putStartInPlaceOfCounter(plan, second)))
            return false;
        int64_t sign = later.falls ? -1 : 1;
        return distance.add(first, sign) && distance.add(second, -sign);
    }

    // The vector loop makes an access for all of its turns before it makes the next access for any. That changes nothing unless a later access of an earlier turn meets an earlier access of a later turn.
    static bool keepsOrderOfAccesses(Plan& plan)
    {
        for (unsigned laterIndex = 0; laterIndex < plan.numberOfAccesses; ++laterIndex) {
            const Access& later = plan.accessTable[laterIndex];
            for (unsigned earlierIndex = 0; earlierIndex < laterIndex; ++earlierIndex) {
                const Access& earlier = plan.accessTable[earlierIndex];
                if ((!later.memory->isStore() && !earlier.memory->isStore()) || !later.memory->range().overlaps(earlier.memory->range()))
                    continue;
                if (later.falls != earlier.falls) {
                    if (!liesBelowInEveryTurn(plan, later, earlier) && !liesBelowInEveryTurn(plan, earlier, later))
                        return false;
                    continue;
                }
                LinearForm distance;
                if (!readDistance(plan, later, earlier, distance))
                    return false;
                RELEASE_ASSERT(!distance.counter);
                if (!distance.numberOfTerms) {
                    if (distance.constant > 0 && distance.constant < static_cast<int64_t>(bytesPerVector))
                        return false;
                    continue;
                }
                bool isKnown = false;
                for (unsigned index = 0; index < plan.numberOfDistancesTestedAtEntry; ++index) {
                    LinearForm difference = distance;
                    isKnown |= difference.add(plan.distancesTestedAtEntry[index], -1) && !difference.numberOfTerms && !difference.constant;
                }
                if (isKnown)
                    continue;
                if (plan.numberOfDistancesTestedAtEntry == Limits::maxDistancesTestedInFrontOfVectorizedLoop)
                    return false;
                plan.distancesTestedAtEntry[plan.numberOfDistancesTestedAtEntry++] = distance;
            }
        }
        return true;
    }

    bool needScalar(Plan& plan, Value* value)
    {
        if (plan.counterFor(value) || plan.isInvariant(value))
            return true;
        Copy* copy = plan.ensureCopyOf(value);
        if (!copy)
            return false;
        if (copy->isScalar)
            return true;
        copy->isScalar = true;
        switch (value->opcode()) {
        case Identity:
        case Add:
        case Sub:
        case Shl:
        case ZExt32:
        case SExt32:
        case Trunc:
            break;
        default:
            dataLogLnIf(verbose, "   scalar: ", deepDump(value));
            return false;
        }
        for (Value* child : value->children()) {
            if (!needScalar(plan, child))
                return false;
        }
        return true;
    }

    // An integer that is wider than a lane is asked for its lowest bits only. Every operation here takes them from the lowest bits of its operands.
    bool needVector(Plan& plan, Value* value)
    {
        Type type = value->type();
        if (type.isFloat() ? sizeofType(type) != plan.bytesPerLane : !type.isInt() || sizeofType(type) < plan.bytesPerLane)
            return false;
        if (plan.counterFor(value))
            return false;
        Copy* copy = plan.ensureCopyOf(value);
        if (!copy)
            return false;
        if (copy->isVector || plan.isInvariant(value))
            return true;
        copy->isVector = true;
        bool isSupported = false;
        switch (value->opcode()) {
        case Load8Z:
        case Load8S:
        case Load16Z:
        case Load16S:
        case Load:
            return needScalar(plan, value->lastChild());
        case Identity:
        case Add:
        case Sub:
        case Neg:
        case BitAnd:
        case BitOr:
        case BitXor:
            isSupported = true;
            break;
        case Mul:
            isSupported = type.isFloat() || plan.bytesPerLane == 2 || plan.bytesPerLane == 4;
            break;
        case Div:
        case Abs:
        case Sqrt:
            isSupported = type.isFloat();
            break;
        case Trunc:
        case ZExt32:
        case SExt32:
            isSupported = plan.bytesPerLane <= 4;
            break;
        default:
            break;
        }
        if (!isSupported) {
            dataLogLnIf(verbose, "   vector: ", deepDump(value));
            return false;
        }
        for (Value* child : value->children()) {
            if (!needVector(plan, child))
                return false;
        }
        return true;
    }

    bool findRoles(Plan& plan)
    {
        for (BasicBlock* block : plan.blocks) {
            for (Value* value : *block) {
                if (MemoryValue* memory = value->as<MemoryValue>()) {
                    if (memory->isStore() && (!needVector(plan, memory->child(0)) || !needScalar(plan, memory->lastChild())))
                        return false;
                    continue;
                }
                if (onlyKeepsValuesAlive(value) && canCloneForLoop(value)) {
                    for (Value* child : value->children()) {
                        if (!plan.isInvariant(child))
                            return false;
                    }
                    continue;
                }
                if (value->opcode() == Phi) {
                    if (!plan.counterFor(value))
                        return false;
                    continue;
                }
                if (auto* upsilon = value->as<UpsilonValue>()) {
                    if (plan.counterFor(upsilon->phi()))
                        continue;
                    if (plan.blockSet.contains(upsilon->phi()->owner))
                        return false;
                    Value* input = upsilon->child(0);
                    if (!plan.testsCounterBeforeStep && !plan.isInvariant(input) && !plan.isCounterOrNext(input))
                        return false;
                    plan.upsilonsForPhisBehindLoop.append(upsilon);
                    continue;
                }
                if (value != block->last() && value->effects().mustExecute()) {
                    dataLogLnIf(verbose, "   effects: ", deepDump(value));
                    return false;
                }
            }
        }
        return true;
    }

    // The bound that leaves room for the other turns must not wrap around, and some start must reach it.
    bool planFarBound(Plan& plan)
    {
        Signedness signedness = plan.signedness();
        Int128 room = plan.room();
        const Counter& tested = plan.tested();
        IntegerInterval full = fullInterval(tested.phi->type(), signedness);
        if (const Counter* bounding = plan.boundingCounter()) {
            // Every sum that the vector loop computes lies within so many steps of a value that the counter has in the loop itself.
            for (const Counter* counter : { &tested, bounding }) {
                IntegerInterval values = intervalFor(m_proc, counter->phi, signedness).join(intervalFor(m_proc, counter->start, signedness));
                Int128 reach = (room < 0 ? -room : room) + Int128 { counter->step < 0 ? -counter->step : counter->step } * plan.lanes();
                if (values.min - reach < full.min || values.max + reach > full.max)
                    return false;
            }
            plan.startHasRoom = true;
            return true;
        }
        IntegerInterval bound = intervalFor(m_proc, plan.bound, signedness);
        // The counter behind the last turn is computed.
        Int128 beyond = Int128 { tested.step } - (plan.includesEqual() ? 0 : tested.step > 0 ? 1 : -1);
        if (bound.max + beyond > full.max || bound.min + beyond < full.min)
            return false;
        Int128 farthest = room > 0 ? full.min + room : full.max + room;
        plan.needsGuardOfFarBound = room > 0 ? bound.min < farthest : bound.max > farthest;
        plan.farthestBoundWithRoom = static_cast<int64_t>(farthest);
        if (room > 0 ? bound.max < farthest : bound.min > farthest)
            return false;
        IntegerInterval farBound { std::max(bound.min - room, full.min), std::min(bound.max - room, full.max) };
        IntegerInterval start = intervalFor(m_proc, tested.start, signedness);
        plan.startHasRoom = start.min + room >= full.min && start.max + room <= full.max;
        return evaluateComparison(plan.relation, start, farBound) != TriState::False;
    }

    PhiChildren& prepareToReadCounters()
    {
        if (!m_phiChildren) {
            m_proc.updateFrequencies();
            m_proc.resetValueOwners();
            m_phiChildren.emplace(m_proc);
        }
        return *m_phiChildren;
    }

    void considerLoop(NaturalLoops& loops, const NaturalLoop& loop)
    {
        if (!isInnermostLoop(loops, loop))
            return;
        BasicBlock* preHeader = findPreHeader(loops, loop);
        if (!preHeader)
            return;
        auto plan = makeUnique<Plan>();
        plan->header = loop.header();
        plan->preHeader = preHeader;
        if (!findTurn(loops, loop, *plan)) {
            dataLogLnIf(verbose, "vectorizeLoops: ", *plan->header, ": no single way through a turn");
            return;
        }
        unsigned numberOfValues = 0;
        bool stores = false;
        for (BasicBlock* block : plan->blocks) {
            numberOfValues += block->size();
            for (Value* value : *block) {
                MemoryValue* memory = value->as<MemoryValue>();
                if (!memory || !memory->isStore())
                    continue;
                // A word of 64 bits in memory is a pointer or a value of any type. Lists of those are mostly short.
                if (memory->child(0)->type() == Int64)
                    return;
                stores = true;
            }
        }
        if (!stores || numberOfValues > Limits::maxValuesPerTurnOfVectorizedLoop)
            return;
        PhiChildren& phiChildren = prepareToReadCounters();
        // The loop in front takes room, which pays where the loop is run often.
        if (plan->header->frequency() < m_proc.policy().loops.minFrequencyOfUnrolledLoop)
            return;
        const char* reason = !findCounters(phiChildren, *plan) ? "counters" : !readTest(*plan) ? "test" : !findAccesses(*plan) ? "accesses" : !keepsOrderOfAccesses(*plan) ? "order" : !findRoles(*plan) ? "roles" : !planFarBound(*plan) ? "far bound" : nullptr;
        dataLogLnIf(verbose, "vectorizeLoops: ", *plan->header, ": ", reason ? reason : "planned");
        if (!reason)
            m_plans.append(WTF::move(plan));
    }

    bool isNeverEntered(const Plan& plan, BasicBlock* block)
    {
        for (BasicBlock* root : plan.blocksBehindEdgesNeverTaken) {
            if (m_proc.dominators().dominates(root, block))
                return true;
        }
        return false;
    }

    // Behind a test at the bottom the loop may be left without one more turn of its own. Then nothing tells what its last turn would have computed, but for the counters.
    void dropPlansWhoseTurnsAreReadBehindLoop()
    {
        Vector<unsigned> planOfBlock;
        for (unsigned index = 0; index < m_plans.size(); ++index) {
            Plan& plan = *m_plans[index];
            if (plan.testsCounterBeforeStep)
                continue;
            if (planOfBlock.isEmpty())
                planOfBlock.fill(0, m_proc.size());
            for (BasicBlock* block : plan.blocks)
                planOfBlock[block->index()] = index + 1;
            for (BasicBlock* block : plan.blocksNeverEntered)
                planOfBlock[block->index()] = index + 1;
        }
        if (planOfBlock.isEmpty())
            return;
        for (BasicBlock* block : m_proc) {
            for (Value* value : *block) {
                for (Value* child : value->children()) {
                    unsigned found = planOfBlock[child->owner->index()];
                    if (!found || found == planOfBlock[block->index()] || !m_plans[found - 1] || child->isConstant())
                        continue;
                    const Plan& plan = *m_plans[found - 1];
                    if (plan.isCounterOrNext(child) || isNeverEntered(plan, block))
                        continue;
                    dataLogLnIf(verbose, "vectorizeLoops: ", *plan.header, ": read behind the loop: ", deepDump(child), " by ", deepDump(value), " in ", *block);
                    m_plans[found - 1] = nullptr;
                }
            }
        }
        m_plans.removeAll(nullptr);
    }

    static SIMDLane laneFor(const Plan& plan, Type type)
    {
        if (type == Float)
            return SIMDLane::f32x4;
        if (type == Double)
            return SIMDLane::f64x2;
        switch (plan.bytesPerLane) {
        case 1:
            return SIMDLane::i8x16;
        case 2:
            return SIMDLane::i16x8;
        case 4:
            return SIMDLane::i32x4;
        default:
            return SIMDLane::i64x2;
        }
    }

    NEVER_INLINE Value* append(BasicBlock* block, Opcode opcode, Origin origin, Value* first = nullptr, Value* second = nullptr)
    {
        if (second)
            return block->appendNew<Value>(m_proc, opcode, origin, first, second);
        if (first)
            return block->appendNew<Value>(m_proc, opcode, origin, first);
        return block->appendNew<Value>(m_proc, opcode, origin);
    }

    NEVER_INLINE Value* appendInteger(BasicBlock* block, Origin origin, Type type, int64_t value)
    {
        return block->appendIntConstant(m_proc, origin, type, value);
    }

    NEVER_INLINE Value* appendVector(BasicBlock* block, Opcode opcode, Origin origin, SIMDLane lane, Value* first, Value* second = nullptr, uint8_t immediate = 0)
    {
        if (second)
            return block->appendNew<SIMDValue>(m_proc, origin, opcode, V128, lane, SIMDSignMode::None, immediate, first, second);
        return block->appendNew<SIMDValue>(m_proc, origin, opcode, V128, lane, SIMDSignMode::None, immediate, first);
    }

    NEVER_INLINE Value* appendCopy(BasicBlock* block, Value* value)
    {
        Value* copy = m_proc.clone(value);
        block->append(copy);
        return copy;
    }

    NEVER_INLINE void appendSet(BasicBlock* block, Origin origin, Variable* variable, Value* value)
    {
        block->appendNew<VariableValue>(m_proc, Set, origin, variable, value);
    }

    NEVER_INLINE void appendUpsilon(BasicBlock* block, Origin origin, Value* value, Value* phi)
    {
        block->appendNew<UpsilonValue>(m_proc, origin, value, phi);
    }

    NEVER_INLINE void appendJump(BasicBlock* block, Origin origin, BasicBlock* target)
    {
        append(block, Jump, origin);
        block->setSuccessors(FrequentedBlock(target));
    }

    NEVER_INLINE void appendBranch(BasicBlock* block, Origin origin, Value* condition, FrequentedBlock taken, FrequentedBlock notTaken)
    {
        append(block, Branch, origin, condition);
        block->setSuccessors(taken, notTaken);
    }

    Value* scalarFor(Plan& plan, Value* value)
    {
        if (const Counter* counter = plan.counterFor(value))
            return counter->phiOfVectorLoop;
        if (value->isConstant())
            return appendCopy(plan.turn, value);
        if (plan.isInvariant(value))
            return value;
        return plan.copyOf(value)->scalar;
    }

    Value* vectorFor(Plan& plan, Value* value)
    {
        Copy* copy = plan.copyOf(value);
        if (copy->vector)
            return copy->vector;
        RELEASE_ASSERT(plan.isInvariant(value));
        Value* lane = value->isConstant() ? appendCopy(plan.entered, value) : value;
        if (lane->type() == Int64 && plan.bytesPerLane <= 4)
            lane = append(plan.entered, Trunc, value->origin(), lane);
        copy->vector = appendVector(plan.entered, VectorSplat, value->origin(), laneFor(plan, value->type()), lane);
        return copy->vector;
    }

    // Every lane holds the same bits. They are laid out in front of the loop.
    Value* appendLanesOf(Plan& plan, Origin origin, uint64_t bits)
    {
        Type type = plan.bytesPerLane == 8 ? Int64 : Int32;
        return appendVector(plan.entered, VectorSplat, origin, laneFor(plan, type), appendInteger(plan.entered, origin, type, type == Int64 ? static_cast<int64_t>(bits) : static_cast<int32_t>(bits)));
    }

    // An access whose address goes the other way than the lanes has its first turn in the last lane.
    Value* appendInOppositeOrder(Plan& plan, Origin origin, Value* vector)
    {
        if (!plan.lanesInOppositeOrder) {
            uint64_t halves[2] = { 0, 0 };
            for (unsigned byte = 0; byte < bytesPerVector; ++byte) {
                uint64_t source = (plan.lanes() - 1 - byte / plan.bytesPerLane) * plan.bytesPerLane + byte % plan.bytesPerLane;
                halves[byte / 8] |= source << (byte % 8 * 8);
            }
            Value* low = appendVector(plan.entered, VectorSplat, origin, SIMDLane::i64x2, appendInteger(plan.entered, origin, Int64, static_cast<int64_t>(halves[0])));
            plan.lanesInOppositeOrder = appendVector(plan.entered, VectorReplaceLane, origin, SIMDLane::i64x2, low, appendInteger(plan.entered, origin, Int64, static_cast<int64_t>(halves[1])), 1);
        }
        return appendVector(plan.turn, VectorSwizzle, origin, SIMDLane::i8x16, vector, plan.lanesInOppositeOrder);
    }

    Value* appendOperation(Plan& plan, Value* value)
    {
        Origin origin = value->origin();
        SIMDLane lane = laneFor(plan, value->type());
        bool isNumber = value->type().isFloat();
        uint64_t signBit = 1ull << (plan.bytesPerLane * 8 - 1);
        Value* first = vectorFor(plan, value->child(0));
        Value* second = value->numChildren() > 1 ? vectorFor(plan, value->child(1)) : nullptr;
        Opcode opcode = Oops;
        switch (value->opcode()) {
        case Add:
            opcode = VectorAdd;
            break;
        case Sub:
            opcode = VectorSub;
            break;
        case Mul:
            opcode = VectorMul;
            break;
        case Div:
            opcode = VectorDiv;
            break;
        case Sqrt:
            opcode = VectorSqrt;
            break;
        case BitAnd:
            opcode = VectorAnd;
            break;
        case BitOr:
            opcode = VectorOr;
            break;
        case BitXor:
            opcode = VectorXor;
            break;
        case Neg:
            opcode = VectorNeg;
            if (isARM64())
                break;
            opcode = isNumber ? VectorXor : VectorSub;
            second = isNumber ? appendLanesOf(plan, origin, signBit) : first;
            if (!isNumber)
                first = appendLanesOf(plan, origin, 0);
            break;
        case Abs:
            opcode = VectorAbs;
            if (isARM64())
                break;
            opcode = VectorAnd;
            second = appendLanesOf(plan, origin, signBit - 1);
            break;
        default:
            return first;
        }
        if (opcode == VectorAnd || opcode == VectorOr || opcode == VectorXor)
            lane = SIMDLane::v128;
        return appendVector(plan.turn, opcode, origin, lane, first, second);
    }

    void appendTurn(Plan& plan)
    {
        BasicBlock* turn = plan.turn;
        unsigned indexOfAccess = 0;
        for (BasicBlock* block : plan.blocks) {
            for (Value* value : *block) {
                Copy* copy = plan.copyOf(value);
                if (copy && copy->isScalar) {
                    copy->scalar = m_proc.clone(value);
                    for (Value*& child : copy->scalar->children())
                        child = scalarFor(plan, child);
                    turn->append(copy->scalar);
                }
                if (onlyKeepsValuesAlive(value))
                    appendCopy(turn, value);
                MemoryValue* memory = value->as<MemoryValue>();
                if (!memory) {
                    if (copy && copy->isVector)
                        copy->vector = appendOperation(plan, value);
                    continue;
                }
                // The address is that of the first turn. Where addresses fall, the last turn has the lowest.
                bool falls = plan.accessTable[indexOfAccess++].falls;
                int32_t offset = memory->offset() - (falls ? static_cast<int32_t>(bytesPerVector - plan.bytesPerLane) : 0);
                bool isInOppositeOrder = falls != plan.lanesRunBackward;
                if (memory->isStore()) {
                    Value* stored = vectorFor(plan, memory->child(0));
                    if (isInOppositeOrder && !plan.isInvariant(memory->child(0)))
                        stored = appendInOppositeOrder(plan, value->origin(), stored);
                    turn->appendNew<MemoryValue>(m_proc, Store, value->origin(), stored, scalarFor(plan, memory->lastChild()), offset, memory->range());
                } else if (copy && copy->isVector) {
                    copy->vector = turn->appendNew<MemoryValue>(m_proc, Load, V128, value->origin(), scalarFor(plan, memory->lastChild()), offset, memory->range());
                    if (isInOppositeOrder)
                        copy->vector = appendInOppositeOrder(plan, value->origin(), copy->vector);
                }
            }
        }
    }

    Value* appendSum(BasicBlock* block, Origin origin, const LinearForm& form, int64_t addend)
    {
        Value* sum = appendInteger(block, origin, Int64, LinearForm::plus(form.constant, addend));
        for (unsigned index = 0; index < form.numberOfTerms; ++index) {
            const LinearForm::Term& term = form.terms[index];
            Value* product = term.factor == 1 || term.factor == -1 ? term.value : append(block, Mul, origin, term.value, appendInteger(block, origin, Int64, term.factor));
            sum = append(block, term.factor == -1 ? Sub : Add, origin, sum, product);
        }
        return sum;
    }

    void appendVectorLoop(Plan& plan)
    {
        Origin origin = plan.test->last()->origin();
        m_proc.addRemark({ RemarkKind::LoopVectorized, GaveUpReason::None, origin, std::nullopt });
        Type type = plan.tested().phi->type();
        double frequency = plan.preHeader->frequency();
        BasicBlock* first = m_proc.addBlock(frequency);
        BasicBlock* entered = plan.entered = m_proc.addBlock(frequency);
        BasicBlock* turn = plan.turn = m_proc.addBlock(plan.header->frequency());
        plan.again = m_proc.addBlock(frequency);
        plan.inFrontOfLoop = m_proc.addBlock(frequency);
        appendJump(plan.inFrontOfLoop, origin, plan.header);
        plan.preHeader->setSuccessors(FrequentedBlock(first));

        const Counter* bounding = plan.boundingCounter();
        Value* starts[Limits::maxCountersOfVectorizedLoop];
        for (unsigned index = 0; index < plan.numberOfCounters; ++index) {
            Value* start = plan.counterTable[index].start;
            starts[index] = start->isConstant() ? appendCopy(first, start) : start;
        }
        Value* bound = bounding ? starts[plan.indexOfBoundingCounter] : plan.bound->isConstant() ? appendCopy(first, plan.bound) : plan.bound;
        int64_t room = plan.room();
        Value* enters = nullptr;
        if (plan.startHasRoom)
            enters = append(first, plan.relation, origin, append(first, Add, origin, starts[0], appendInteger(first, origin, type, room)), bound);
        else {
            enters = append(first, plan.relation, origin, starts[0], append(first, Sub, origin, bound, appendInteger(first, origin, type, room)));
            if (plan.needsGuardOfFarBound) {
                Opcode hasRoom = room > 0 ? (isUnsignedRelation(plan.relation) ? AboveEqual : GreaterEqual) : (isUnsignedRelation(plan.relation) ? BelowEqual : LessEqual);
                enters = append(first, BitAnd, origin, enters, append(first, hasRoom, origin, bound, appendInteger(first, origin, type, plan.farthestBoundWithRoom)));
            }
        }
        BasicBlock* behindFirstTest = entered;
        if (plan.numberOfDistancesTestedAtEntry) {
            behindFirstTest = m_proc.addBlock(frequency);
            Value* liesApart = nullptr;
            for (unsigned index = 0; index < plan.numberOfDistancesTestedAtEntry; ++index) {
                Value* isFar = append(behindFirstTest, AboveEqual, origin, appendSum(behindFirstTest, origin, plan.distancesTestedAtEntry[index], -1), appendInteger(behindFirstTest, origin, Int64, bytesPerVector - 1));
                liesApart = liesApart ? append(behindFirstTest, BitAnd, origin, liesApart, isFar) : isFar;
            }
            appendBranch(behindFirstTest, origin, liesApart, FrequentedBlock(entered), FrequentedBlock(plan.inFrontOfLoop, FrequencyClass::Rare));
        }
        appendBranch(first, origin, enters, FrequentedBlock(behindFirstTest), FrequentedBlock(plan.inFrontOfLoop));

        for (Counter& counter : plan.counters()) {
            counter.phiOfVectorLoop = turn->appendNew<Value>(m_proc, Phi, counter.phi->type(), counter.phi->origin());
            m_proc.knownProperties().copy(counter.phi, counter.phiOfVectorLoop);
        }
        appendTurn(plan);
        for (unsigned index = 0; index < plan.numberOfCounters; ++index) {
            Counter& counter = plan.counterTable[index];
            counter.nextOfVectorLoop = append(turn, Add, origin, counter.phiOfVectorLoop, appendInteger(turn, origin, counter.phi->type(), counter.step * static_cast<int64_t>(plan.lanes())));
            appendUpsilon(turn, origin, counter.nextOfVectorLoop, counter.phiOfVectorLoop);
            appendUpsilon(entered, origin, starts[index], counter.phiOfVectorLoop);
        }
        Value* next = plan.tested().nextOfVectorLoop;
        Value* roomInLoop = appendInteger(bounding ? turn : entered, origin, type, room);
        Value* goesOn = bounding
            ? append(turn, plan.relation, origin, append(turn, Add, origin, next, roomInLoop), bounding->nextOfVectorLoop)
            : append(turn, plan.relation, origin, next, append(entered, Sub, origin, bound, roomInLoop));
        appendJump(entered, origin, turn);

        BasicBlock* rest = plan.again;
        if (!plan.testsCounterBeforeStep) {
            rest = m_proc.addBlock(frequency);
            plan.left = m_proc.addBlock(frequency);
            appendBranch(rest, origin, append(rest, plan.relation, origin, next, bounding ? bounding->nextOfVectorLoop : bound), FrequentedBlock(plan.again), FrequentedBlock(plan.left));
        }
        appendBranch(turn, origin, goesOn, FrequentedBlock(turn, plan.test->successor(plan.stayingSuccessorIndex).frequency()), FrequentedBlock(rest, plan.test->successor(1 - plan.stayingSuccessorIndex).frequency()));
    }

    static Variable* variableOf(Value* value)
    {
        BasicBlock* block = value->owner;
        size_t index = block->values().find(value);
        RELEASE_ASSERT(index != notFound);
        Value* set = block->at(index + 1);
        RELEASE_ASSERT(set->opcode() == Set && set->child(0) == value);
        return set->as<VariableValue>()->variable();
    }

    void leaveVectorLoop(Plan& plan)
    {
        Origin origin = plan.test->last()->origin();
        BasicBlock* left = plan.left;
        for (const Counter& counter : plan.counters()) {
            Value* next = counter.nextOfVectorLoop;
            RELEASE_ASSERT(counter.phi->opcode() == Identity && counter.phi->child(0)->opcode() == Get);
            appendSet(plan.again, origin, counter.phi->child(0)->as<VariableValue>()->variable(), next);
            if (!left)
                continue;
            appendSet(left, origin, variableOf(counter.phi), append(left, Sub, origin, next, appendInteger(left, origin, next->type(), counter.step)));
            appendSet(left, origin, variableOf(counter.next), next);
        }
        appendJump(plan.again, origin, plan.inFrontOfLoop);
        if (!left)
            return;
        for (Value* upsilon : plan.upsilonsForPhisBehindLoop) {
            Value* input = upsilon->child(0);
            if (input->isConstant() || input->opcode() == Get)
                input = appendCopy(left, input);
            appendCopy(left, upsilon)->child(0) = input;
        }
        appendJump(left, origin, plan.test->successorBlock(1 - plan.stayingSuccessorIndex));
    }

    Procedure& m_proc;
    std::optional<PhiChildren> m_phiChildren;
    Vector<std::unique_ptr<Plan>> m_plans;
};

#pragma clang attribute pop

} // anonymous namespace

bool vectorizeLoops(Procedure& proc)
{
    PhaseScope phaseScope(proc, "vectorizeLoops"_s);
    VectorizeLoops vectorizeLoops(proc);
    return vectorizeLoops.run();
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
