/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "B3VectorizeLoops.h"

#if ENABLE(B3_JIT)

#include "AllowMacroScratchRegisterUsage.h"
#include "B3BasicBlockInlines.h"
#include "B3CountedLoops.h"
#include "B3DominatingConditions.h"
#include "B3Dominators.h"
#include "B3FixSSA.h"
#include "B3IntRange.h"
#include "B3Limits.h"
#include "B3LoopUtilities.h"
#include "B3MemoryValue.h"
#include "B3MovedLoadCheck.h"
#include "B3NaturalLoops.h"
#include "B3PatchpointValue.h"
#include "B3PhaseScope.h"
#include "B3PhiChildren.h"
#include "B3ProcedureInlines.h"
#include "B3SIMDValue.h"
#include "B3StackmapGenerationParams.h"
#include "B3UpsilonValue.h"
#include "B3ValueInlines.h"
#include "B3VariableValue.h"
#include "CCallHelpers.h"
#include <optional>
#include <wtf/IndexSet.h>

namespace JSC { namespace B3 {

namespace {

static constexpr bool verbose = false;

// The phase looks at few loops and changes fewer. Its own size counts for more than its speed.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)

// It has no register of its own and clobbers none, and nothing but the order of values can tell that it ran. So it emits nothing.
bool runsNoCode(const Value* value)
{
    if (!onlyKeepsValuesAlive(value))
        return false;
    Effects effects = value->effects();
    return !effects.writes && !effects.writesPinned && !effects.readsPinned && !effects.exitsSideways && !effects.controlDependent && !effects.terminal && !effects.fence;
}

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

    // Every operation on vectors that the phase makes, beside loads and stores (movups): what it is made of on a target without AVX, and what that takes beyond SSE2.
    static CPUFeatureSet featuresNeededFor(Opcode opcode, SIMDLane lane)
    {
        CPUFeatureSet nothing;
#if CPU(X86_64)
        switch (opcode) {
        case VectorSplat:
            // movq, then by the width of a lane: pinsrb, pshuflw, punpcklqdq | pshuflw, punpcklqdq | shufps or pshufd | movddup, or shufpd where there is no SSE3.
            return lane == SIMDLane::i8x16 ? nothing.with(CPUFeature::SSE4_1) : nothing;
        case VectorReplaceLane:
            // pinsrq.
            return nothing.with(CPUFeature::SSE4_1);
        case VectorSwizzle:
            // pshufb.
            return nothing.with(CPUFeature::SupplementalSSE3);
        case VectorMul:
            // pmullw | pmulld | mulps | mulpd.
            return lane == SIMDLane::i32x4 ? nothing.with(CPUFeature::SSE4_1) : nothing;
        case VectorExtendLow:
            // pmovzxbw, pmovsxbw | pmovzxwd, pmovsxwd.
            return nothing.with(CPUFeature::SSE4_1);
        case VectorMin:
        case VectorMax:
            // Of integers with a sign: pminsw, pmaxsw | pminsd, pmaxsd.
            return lane == SIMDLane::i16x8 ? nothing : nothing.with(CPUFeature::SSE4_1);
        case VectorNarrow:
            // packuswb, packsswb | packusdw, packssdw.
            return lane == SIMDLane::i16x8 ? nothing : nothing.with(CPUFeature::SSE4_1);
        case VectorExtractLane:
            // The lowest number of a vector is the register itself.
            return nothing;
        case VectorAdd:
        case VectorSub:
        case VectorDiv:
        case VectorSqrt:
        case VectorAnd:
        case VectorOr:
        case VectorXor:
            // paddb .. paddq, addps, addpd | psubb .. psubq, subps, subpd | divps, divpd | sqrtps, sqrtpd | andps | orps | xorps.
            return nothing;
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
#else
        UNUSED_PARAM(opcode);
        UNUSED_PARAM(lane);
        return nothing;
#endif
    }

    bool targetHas(Opcode opcode, SIMDLane lane) const
    {
        CPUFeatureSet features = m_proc.policy().targetFeatures;
#if CPU(X86_64)
        if (features.contains(CPUFeature::AVX))
            return true;
#endif
        return featuresNeededFor(opcode, lane).isSubsetOf(features);
    }

    // A Phi of the header that every turn moves on by the same distance.
    struct Counter {
        Value* phi;
        Value* start;
        Value* next;
        int64_t step;
        Value* phiOfVectorLoop;
        Value* nextOfVectorLoop;
    };

    // factorOfCounter * counter + constant + the sum of factor * value over the terms, in arithmetic modulo 2^64. No term changes in the loop.
    struct LinearForm {
        struct Term {
            Value* value;
            int64_t factor;
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
        unsigned numberOfTerms;
        Value* counter;
        int64_t factorOfCounter;
        int64_t constant;
    };

    struct Access {
        MemoryValue* memory;
        LinearForm address;
        // From turn to turn the address rises or falls by the size of an element.
        bool falls;
        // For a store of lanes that are wider than an element: what is made narrow, and whether as integers with a sign. Making it narrow keeps it within what an element holds.
        Value* narrowed;
        bool narrowsWithSign;
    };

    // What the vector loop computes in the place of a value of the turn: the value of the first of its turns, or one lane for each.
    struct Copy {
        Value* original;
        Value* scalar;
        Value* vector;
        bool isScalar;
        bool isVector;
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
        Counter counterTable[Limits::maxCountersOfVectorizedLoop] { };
        unsigned numberOfCounters { 0 };
        // It is tested against a value that does not change in the loop, or against another counter.
        Value* bound { nullptr };
        unsigned indexOfBoundingCounter { 0 };
        bool testsCounterBeforeStep { false };
        bool startHasRoom { false };
        bool needsGuardOfFarBound { false };
        // Two counters that go the same distance a turn, towards each other. Their sum stays, so the test of one against the other is a test of one against a limit that does not change.
        bool meetsAtFixedLimit { false };
        int64_t farthestBoundWithRoom { 0 };
        unsigned bytesPerElement { 0 };
        // As wide as an element, or twice as wide where a turn computes with more than the lowest bits.
        unsigned bytesPerLane { 0 };
        Access accessTable[Limits::maxAccessesPerTurnOfVectorizedLoop] { };
        unsigned numberOfAccesses { 0 };
        // Each must not lie between 0 and the bytes of a vector.
        LinearForm distancesTestedAtEntry[Limits::maxDistancesTestedInFrontOfVectorizedLoop] { };
        unsigned numberOfDistancesTestedAtEntry { 0 };
        // Each is taken not to lie there, untested. Where moved loads are logged a check in front of the vector loop reports one that does.
        LinearForm distancesCheckedAtEntry[Limits::maxDistancesCheckedInFrontOfVectorizedLoop] { };
        const MemoryValue* accessesCheckedAtEntry[Limits::maxDistancesCheckedInFrontOfVectorizedLoop] { };
        unsigned numberOfDistancesCheckedAtEntry { 0 };
        // Lane 0 holds the last of the turns, where every address falls.
        bool lanesRunBackward { false };
        Copy copyTable[Limits::maxValuesPerTurnOfVectorizedLoop] { };
        unsigned numberOfCopies { 0 };
        // What the turn asks on the way: whether the thread is to stop. The vector loop asks between stretches of turns.
        Value* poll { nullptr };
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
        bool hasWideLanes() const { return bytesPerLane != bytesPerElement; }
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
            *copy = { };
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
    bool hasWholeTurnsBetweenTests(const Plan& plan) const
    {
        bool isBehindTest = false;
        for (BasicBlock* block : plan.blocks) {
            for (Value* value : *block) {
                if (canPollBetweenStretches(m_proc, value))
                    continue;
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

    bool readTest(Plan& plan) const
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

    // A bound that was chosen from two lies where one of the two lies.
    IntegerInterval intervalOfBound(Value* bound, Signedness signedness)
    {
        if (bound->opcode() == Select)
            return intervalFor(m_proc, bound->child(1), signedness).join(intervalFor(m_proc, bound->child(2), signedness));
        return intervalFor(m_proc, bound, signedness);
    }

    // The values of a counter in the turns that the turns of the vector loop stand for.
    IntegerInterval intervalOf(const Plan& plan, const Counter& counter, Signedness signedness)
    {
        IntegerInterval result = intervalFor(m_proc, counter.phi, plan.signedness());
        if (&counter == &plan.tested() && plan.bound) {
            IntegerInterval start = intervalFor(m_proc, counter.start, plan.signedness());
            IntegerInterval bound = intervalOfBound(plan.bound, plan.signedness());
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
            LinearForm right { };
            return readLinearForm(plan, value->child(0), timeToLive - 1, form) && readLinearForm(plan, value->child(1), timeToLive - 1, right) && form.add(right, value->opcode() == Add ? 1 : -1);
        }
        case Shl: {
            LinearForm shifted { };
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

    bool findAccesses(Plan& plan, unsigned lanesPerElementWidth)
    {
        bool stores = false;
        plan.lanesRunBackward = true;
        plan.numberOfAccesses = 0;
        plan.bytesPerElement = 0;
        for (BasicBlock* block : plan.blocks) {
            for (Value* value : *block) {
                MemoryValue* memory = value->as<MemoryValue>();
                if (!memory)
                    continue;
                if ((!memory->isLoad() && !memory->isStore()) || memory->hasFence() || plan.numberOfAccesses == Limits::maxAccessesPerTurnOfVectorizedLoop)
                    return false;
                Access& access = plan.accessTable[plan.numberOfAccesses++];
                access = { };
                access.memory = memory;
                if (!readLinearForm(plan, memory->lastChild(), Limits::maxDepthOfAddressInVectorizedLoop, access.address)) {
                    dataLogLnIf(verbose, "   address: ", deepDump(memory));
                    return false;
                }
                access.address.constant = LinearForm::plus(access.address.constant, memory->offset());
                int64_t bytes = memory->accessByteSize();
                if (!plan.bytesPerElement) {
                    plan.bytesPerElement = bytes;
                    plan.bytesPerLane = bytes * lanesPerElementWidth;
                }
                // From turn to turn every access goes on to the element beside.
                int64_t stride = access.address.counter ? LinearForm::times(access.address.factorOfCounter, plan.counterFor(access.address.counter)->step) : 0;
                if (bytes != plan.bytesPerElement || plan.bytesPerLane > 8 || (stride != bytes && stride != -bytes) || (plan.hasWideLanes() && (stride < 0 || plan.bytesPerLane > 4 || memory->accessType().isFloat()))) {
                    dataLogLnIf(verbose, "   stride: ", deepDump(memory), " goes on by ", stride);
                    return false;
                }
                access.falls = stride < 0;
                plan.lanesRunBackward &= access.falls;
                stores |= memory->isStore();
            }
        }
        for (unsigned index = 0; index < plan.numberOfAccesses; ++index) {
            // Its lanes are put in the opposite order.
            if (plan.accessTable[index].falls != plan.lanesRunBackward && !(targetHas(VectorSwizzle, SIMDLane::i8x16) && targetHas(VectorReplaceLane, SIMDLane::i64x2) && targetHas(VectorSplat, SIMDLane::i64x2)))
                return false;
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
    bool keepsOrderOfAccesses(Plan& plan)
    {
        const ValidationPolicy& validation = m_proc.policy().validation;
        plan.numberOfDistancesTestedAtEntry = 0;
        plan.numberOfDistancesCheckedAtEntry = 0;
        for (unsigned laterIndex = 0; laterIndex < plan.numberOfAccesses; ++laterIndex) {
            const Access& later = plan.accessTable[laterIndex];
            for (unsigned earlierIndex = 0; earlierIndex < laterIndex; ++earlierIndex) {
                const Access& earlier = plan.accessTable[earlierIndex];
                if (!later.memory->isStore() && !earlier.memory->isStore())
                    continue;
                bool isTakenToLieApart = !later.memory->range().overlaps(earlier.memory->range());
                if (isTakenToLieApart && !validation.logsMovedLoads)
                    continue;
                if (later.falls != earlier.falls) {
                    if (!isTakenToLieApart && !liesBelowInEveryTurn(plan, later, earlier) && !liesBelowInEveryTurn(plan, earlier, later))
                        return false;
                    continue;
                }
                LinearForm distance { };
                if (!readDistance(plan, later, earlier, distance)) {
                    if (isTakenToLieApart)
                        continue;
                    return false;
                }
                RELEASE_ASSERT(!distance.counter);
                if (isTakenToLieApart || validation.takesAccessesOfVectorTurnsToLieApart) {
                    if (!validation.logsMovedLoads)
                        continue;
                    if (plan.numberOfDistancesCheckedAtEntry == Limits::maxDistancesCheckedInFrontOfVectorizedLoop)
                        return false;
                    plan.accessesCheckedAtEntry[plan.numberOfDistancesCheckedAtEntry] = later.memory;
                    plan.distancesCheckedAtEntry[plan.numberOfDistancesCheckedAtEntry++] = distance;
                    continue;
                }
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

    struct LesserOrGreater {
        Opcode opcode { Oops };
        Value* left { nullptr };
        Value* right { nullptr };
    };

    // The arm of a Select is what was compared, perhaps made wider with its sign.
    static bool isComparedValue(Value* arm, Value* compared)
    {
        if (arm == compared)
            return true;
        if (arm->opcode() == SExt32 || (arm->opcode() == ZExt32 && arm->child(0)->hasInt32() && arm->child(0)->asInt32() >= 0))
            arm = arm->child(0);
        return arm == compared || (arm->hasInt() && compared->hasInt() && arm->asInt() == compared->asInt());
    }

    // Select(a < b, a, b) and the like, of integers with a sign.
    static LesserOrGreater asLesserOrGreater(Value* value)
    {
        if (value->opcode() != Select || !value->type().isInt())
            return { };
        Value* comparison = value->child(0);
        bool picksLesserInOrder = comparison->opcode() == LessThan || comparison->opcode() == LessEqual;
        if (!picksLesserInOrder && comparison->opcode() != GreaterThan && comparison->opcode() != GreaterEqual)
            return { };
        if (isComparedValue(value->child(1), comparison->child(0)) && isComparedValue(value->child(2), comparison->child(1)))
            return { picksLesserInOrder ? VectorMin : VectorMax, value->child(1), value->child(2) };
        if (isComparedValue(value->child(1), comparison->child(1)) && isComparedValue(value->child(2), comparison->child(0)))
            return { picksLesserInOrder ? VectorMax : VectorMin, value->child(2), value->child(1) };
        return { };
    }

    // What the value may be, where that follows from what is loaded and what comes from outside the loop. Nothing: it may wrap around, or it is not known.
    std::optional<IntegerInterval> intervalInTurn(const Plan& plan, Value* value, unsigned timeToLive)
    {
        if (!value->type().isInt())
            return std::nullopt;
        if (plan.isInvariant(value))
            return intervalOfBound(value, Signedness::Signed);
        if (!timeToLive)
            return std::nullopt;
        auto of = [&](unsigned index) {
            return intervalInTurn(plan, value->child(index), timeToLive - 1);
        };
        std::optional<IntegerInterval> result;
        switch (value->opcode()) {
        case Load8Z:
            return IntegerInterval { 0, 0xff };
        case Load8S:
            return IntegerInterval { -0x80, 0x7f };
        case Load16Z:
            return IntegerInterval { 0, 0xffff };
        case Load16S:
            return IntegerInterval { -0x8000, 0x7fff };
        case Load:
            return fullInterval(value->type(), Signedness::Signed);
        case Identity:
        case SExt32:
        case Trunc:
            result = of(0);
            break;
        case ZExt32:
            result = of(0);
            if (result && result->min < 0)
                return std::nullopt;
            break;
        case Add:
        case Sub:
        case Mul: {
            auto left = of(0);
            auto right = of(1);
            if (!left || !right)
                return std::nullopt;
            if (value->opcode() == Add)
                result = IntegerInterval { left->min + right->min, left->max + right->max };
            else if (value->opcode() == Sub)
                result = IntegerInterval { left->min - right->max, left->max - right->min };
            else {
                // Both lie within 64 bits, so no product leaves 128.
                Int128 products[] = { left->min * right->min, left->min * right->max, left->max * right->min, left->max * right->max };
                result = IntegerInterval { std::min({ products[0], products[1], products[2], products[3] }), std::max({ products[0], products[1], products[2], products[3] }) };
            }
            break;
        }
        case Shl:
            result = of(0);
            if (!result || !value->child(1)->hasInt32() || value->child(1)->asInt32() < 0 || value->child(1)->asInt32() > 31)
                return std::nullopt;
            result = IntegerInterval { result->min * (Int128 { 1 } << value->child(1)->asInt32()), result->max * (Int128 { 1 } << value->child(1)->asInt32()) };
            break;
        case BitAnd:
            for (unsigned index = 0; index < 2; ++index) {
                auto mask = of(index);
                if (mask && mask->min >= 0)
                    result = result ? IntegerInterval { 0, std::min(result->max, mask->max) } : IntegerInterval { 0, mask->max };
            }
            break;
        case Select: {
            LesserOrGreater operation = asLesserOrGreater(value);
            if (operation.opcode == Oops)
                return std::nullopt;
            auto left = intervalInTurn(plan, operation.left, timeToLive - 1);
            auto right = intervalInTurn(plan, operation.right, timeToLive - 1);
            if (!left || !right)
                return std::nullopt;
            result = operation.opcode == VectorMin
                ? IntegerInterval { std::min(left->min, right->min), std::min(left->max, right->max) }
                : IntegerInterval { std::max(left->min, right->min), std::max(left->max, right->max) };
            break;
        }
        default:
            return std::nullopt;
        }
        if (!result || !fullInterval(value->type(), Signedness::Signed).contains(*result))
            return std::nullopt;
        return result;
    }

    static IntegerInterval intervalOfLane(unsigned bytes, Signedness signedness)
    {
        Int128 values = Int128 { 1 } << (bytes * 8);
        return signedness == Signedness::Signed ? IntegerInterval { -values / 2, values / 2 - 1 } : IntegerInterval { 0, values - 1 };
    }

    // Lanes that are wider than an element are made narrow in front of a store. What lies outside of what an element holds becomes the nearest that it holds, so a turn that does the same by itself need not.
    bool planNarrowing(Plan& plan, Access& access)
    {
        Value* stored = access.memory->child(0);
        IntegerInterval element = intervalOfLane(plan.bytesPerElement, Signedness::Unsigned);
        LesserOrGreater outer = asLesserOrGreater(stored);
        if (outer.opcode != Oops && outer.right->hasInt()) {
            LesserOrGreater inner = asLesserOrGreater(outer.left);
            if (inner.opcode != Oops && inner.opcode != outer.opcode && inner.right->hasInt()) {
                int64_t least = (outer.opcode == VectorMax ? outer : inner).right->asInt();
                int64_t greatest = (outer.opcode == VectorMin ? outer : inner).right->asInt();
                if (least == element.min && greatest == element.max)
                    stored = inner.left;
            }
        }
        access.narrowed = stored;
        if (stored != access.memory->child(0))
            return true;
        auto interval = intervalInTurn(plan, stored, Limits::maxDepthOfAddressInVectorizedLoop);
        if (m_proc.policy().validation.takesLanesToHoldWholeValues)
            return true;
        if (!interval)
            return false;
        access.narrowsWithSign = !element.contains(*interval);
        return !access.narrowsWithSign || intervalOfLane(plan.bytesPerElement, Signedness::Signed).contains(*interval);
    }

    // A lane has the lowest bits of a value. They are all of it where the value is no wider, or is known to fit.
    bool holdsWholeValue(const Plan& plan, Value* value)
    {
        if (sizeofType(value->type()) == plan.bytesPerLane || m_proc.policy().validation.takesLanesToHoldWholeValues)
            return true;
        auto interval = intervalInTurn(plan, value, Limits::maxDepthOfAddressInVectorizedLoop);
        return interval && intervalOfLane(plan.bytesPerLane, Signedness::Signed).contains(*interval);
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
        if (plan.hasWideLanes() && !holdsWholeValue(plan, value)) {
            dataLogLnIf(verbose, "   range: ", deepDump(value));
            return false;
        }
        Copy* copy = plan.ensureCopyOf(value);
        if (!copy)
            return false;
        if (copy->isVector)
            return true;
        if (plan.isInvariant(value))
            return targetHas(VectorSplat, laneFor(plan, type));
        copy->isVector = true;
        bool isSupported = false;
        switch (value->opcode()) {
        case Load8Z:
        case Load8S:
        case Load16Z:
        case Load16S:
        case Load:
            return (!plan.hasWideLanes() || targetHas(VectorExtendLow, laneFor(plan, type))) && needScalar(plan, value->lastChild());
        case Select: {
            // A lane must hold the whole of what is compared.
            LesserOrGreater operation = asLesserOrGreater(value);
            if (operation.opcode == Oops || !holdsWholeValue(plan, operation.left) || !holdsWholeValue(plan, operation.right) || plan.bytesPerLane == 8 || !targetHas(operation.opcode, laneFor(plan, type))) {
                dataLogLnIf(verbose, "   vector: ", deepDump(value), " on ", deepDump(value->child(0)));
                return false;
            }
            return needVector(plan, operation.left) && needVector(plan, operation.right);
        }
        case Identity:
        case Add:
        case Sub:
        case BitAnd:
        case BitOr:
        case BitXor:
            isSupported = true;
            break;
        case Neg:
            // Where there is no such operation on vectors it is made with a constant in every lane.
            isSupported = targetHas(VectorSplat, laneFor(plan, Int32));
            break;
        case Mul:
            isSupported = (type.isFloat() || plan.bytesPerLane == 2 || plan.bytesPerLane == 4) && targetHas(VectorMul, laneFor(plan, type));
            break;
        case Shl:
            // Times a power of two.
            if ((plan.bytesPerLane != 2 && plan.bytesPerLane != 4) || !value->child(1)->hasInt32() || value->child(1)->asInt32() < 0 || value->child(1)->asInt32() >= static_cast<int32_t>(plan.bytesPerLane * 8) || !targetHas(VectorMul, laneFor(plan, type)))
                return false;
            return needVector(plan, value->child(0));
        case Abs:
            isSupported = type.isFloat() && targetHas(VectorSplat, laneFor(plan, Int32));
            break;
        case Div:
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
        plan.numberOfCopies = 0;
        plan.poll = nullptr;
        plan.upsilonsForPhisBehindLoop.shrink(0);
        unsigned indexOfAccess = 0;
        for (BasicBlock* block : plan.blocks) {
            for (Value* value : *block) {
                if (MemoryValue* memory = value->as<MemoryValue>()) {
                    Access& access = plan.accessTable[indexOfAccess++];
                    if (!memory->isStore())
                        continue;
                    access.narrowed = memory->child(0);
                    if (plan.hasWideLanes() && (!planNarrowing(plan, access) || !targetHas(VectorNarrow, laneFor(plan, Int32))))
                        return false;
                    if (!needVector(plan, access.narrowed) || !needScalar(plan, memory->lastChild()))
                        return false;
                    continue;
                }
                if (runsNoCode(value) && canCloneForLoop(value)) {
                    for (Value* child : value->children()) {
                        if (!plan.isInvariant(child))
                            return false;
                    }
                    continue;
                }
                if (canPollBetweenStretches(m_proc, value) && canCloneForLoop(value) && !plan.poll) {
                    for (Value* child : value->children()) {
                        if (!plan.isInvariant(child))
                            return false;
                    }
                    plan.poll = value;
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
            Int128 leastSum = 0;
            Int128 greatestSum = 0;
            for (const Counter* counter : { &tested, bounding }) {
                IntegerInterval values = intervalFor(m_proc, counter->phi, signedness).join(intervalFor(m_proc, counter->start, signedness));
                Int128 reach = (room < 0 ? -room : room) + Int128 { counter->step < 0 ? -counter->step : counter->step } * plan.lanes();
                if (values.min - reach < full.min || values.max + reach > full.max)
                    return false;
                leastSum += values.min - reach;
                greatestSum += values.max + reach;
            }
            plan.startHasRoom = true;
            IntegerInterval asSigned = fullInterval(tested.phi->type(), Signedness::Signed);
            plan.meetsAtFixedLimit = tested.step == -bounding->step && leastSum >= (signedness == Signedness::Signed ? asSigned.min : 0) && greatestSum <= asSigned.max;
            return true;
        }
        IntegerInterval bound = intervalOfBound(plan.bound, signedness);
        dataLogLnIf(verbose, "   far bound: ", plan.relation, " step ", tested.step, " bound ", deepDump(plan.bound), " in [", static_cast<int64_t>(bound.min), ", ", static_cast<int64_t>(bound.max), "], start ", deepDump(tested.start), ", before the step: ", plan.testsCounterBeforeStep);
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
        const char* reason = !findCounters(phiChildren, *plan) ? "counters" : !readTest(*plan) ? "test" : nullptr;
        for (unsigned lanesPerElementWidth = 1; !reason; ++lanesPerElementWidth) {
            reason = !findAccesses(*plan, lanesPerElementWidth) ? "accesses" : !keepsOrderOfAccesses(*plan) ? "order" : !findRoles(*plan) ? "roles" : !planFarBound(*plan) ? "far bound" : nullptr;
            if (!reason || lanesPerElementWidth == 2)
                break;
            reason = nullptr;
        }
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

    NEVER_INLINE Value* appendVector(BasicBlock* block, Opcode opcode, Origin origin, SIMDLane lane, Value* first, Value* second = nullptr, uint8_t immediate = 0, SIMDSignMode signMode = SIMDSignMode::None)
    {
        RELEASE_ASSERT(targetHas(opcode, lane));
        Type type = opcode == VectorExtractLane ? Double : V128;
        if (second)
            return block->appendNew<SIMDValue>(m_proc, origin, opcode, type, lane, signMode, immediate, first, second);
        return block->appendNew<SIMDValue>(m_proc, origin, opcode, type, lane, signMode, immediate, first);
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
        if (value->opcode() == Select) {
            LesserOrGreater operation = asLesserOrGreater(value);
            Value* left = vectorFor(plan, operation.left);
            return appendVector(plan.turn, operation.opcode, origin, lane, left, vectorFor(plan, operation.right), 0, SIMDSignMode::Signed);
        }
        Value* first = vectorFor(plan, value->child(0));
        if (value->opcode() == Shl)
            return appendVector(plan.turn, VectorMul, origin, lane, first, appendLanesOf(plan, origin, 1ull << value->child(1)->asInt32()));
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
                if (runsNoCode(value))
                    appendCopy(turn, value);
                MemoryValue* memory = value->as<MemoryValue>();
                if (!memory) {
                    if (copy && copy->isVector)
                        copy->vector = appendOperation(plan, value);
                    continue;
                }
                // The address is that of the first turn. Where addresses fall, the last turn has the lowest.
                const Access& access = plan.accessTable[indexOfAccess++];
                Origin origin = value->origin();
                int32_t offset = memory->offset() - (access.falls ? static_cast<int32_t>(bytesPerVector - plan.bytesPerLane) : 0);
                bool isInOppositeOrder = access.falls != plan.lanesRunBackward;
                // Lanes of twice the width of an element: the elements of a turn fill half a vector in memory.
                SIMDLane lane = laneFor(plan, Int32);
                if (memory->isStore()) {
                    Value* stored = vectorFor(plan, access.narrowed);
                    if (isInOppositeOrder && !plan.isInvariant(access.narrowed))
                        stored = appendInOppositeOrder(plan, origin, stored);
                    if (plan.hasWideLanes())
                        stored = appendVector(turn, VectorExtractLane, origin, SIMDLane::f64x2, appendVector(turn, VectorNarrow, origin, lane, stored, stored, 0, access.narrowsWithSign ? SIMDSignMode::Signed : SIMDSignMode::Unsigned));
                    turn->appendNew<MemoryValue>(m_proc, Store, origin, stored, scalarFor(plan, memory->lastChild()), offset, memory->range());
                } else if (copy && copy->isVector) {
                    copy->vector = turn->appendNew<MemoryValue>(m_proc, Load, plan.hasWideLanes() ? Double : V128, origin, scalarFor(plan, memory->lastChild()), offset, memory->range());
                    if (isInOppositeOrder)
                        copy->vector = appendInOppositeOrder(plan, origin, copy->vector);
                    if (plan.hasWideLanes())
                        copy->vector = appendVector(turn, VectorExtendLow, origin, lane, copy->vector, nullptr, 0, memory->opcode() == Load8S || memory->opcode() == Load16S ? SIMDSignMode::Signed : SIMDSignMode::Unsigned);
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

    // It reports and does not come back.
    void appendReportOfDistance(BasicBlock* block, Value* distanceLessOne, const MemoryValue* access)
    {
        Value* least = appendInteger(block, access->origin(), Int64, bytesPerVector - 1);
        PatchpointValue* report = block->appendNew<PatchpointValue>(m_proc, Void, access->origin());
        report->append(ConstrainedValue(distanceLessOne, ValueRep::SomeRegister));
        report->append(ConstrainedValue(least, ValueRep::SomeRegister));
        report->clobber(RegisterSet::macroClobberedGPRs());
        report->effects = Effects::none();
        report->effects.terminal = true;
        report->setRunsWithoutFrame(true);
        MovedLoad load { Load, Int64, access->offset(), access->origin(), access->range() };
        report->setGenerator([load, task = m_proc.policy().reportsMovedLoad](CCallHelpers& jit, const StackmapGenerationParams& params) {
            AllowMacroScratchRegisterUsage allowScratch(jit);
            if (task)
                task->run(jit, load, params[0].gpr(), params[1].gpr());
            jit.breakpoint();
        });
    }

    // Whether there is room for one more turn of the vector loop behind the one that has just been taken.
    Value* appendTestOfRoom(Plan& plan, BasicBlock* block, Origin origin, Value* farBound)
    {
        Value* next = plan.tested().nextOfVectorLoop;
        if (farBound)
            return append(block, plan.relation, origin, next, farBound);
        return append(block, plan.relation, origin, append(block, Add, origin, next, appendInteger(block, origin, next->type(), plan.room())), plan.boundingCounter()->nextOfVectorLoop);
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
        // Where a stretch of turns begins.
        bool hasFarBound = !bounding || plan.meetsAtFixedLimit;
        bool runsInStretches = plan.poll && hasFarBound;
        Value* from[Limits::maxCountersOfVectorizedLoop];
        for (unsigned index = 0; index < plan.numberOfCounters; ++index) {
            from[index] = starts[index];
            if (!runsInStretches)
                continue;
            from[index] = entered->appendNew<Value>(m_proc, Phi, starts[index]->type(), origin);
            appendUpsilon(first, origin, starts[index], from[index]);
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
        if (plan.poll && !hasFarBound) {
            // Two counters that meet within one stretch. Where they lie farther apart the loop itself takes all turns.
            int64_t approach = plan.tested().step - bounding->step;
            Value* distance = approach > 0 ? append(first, Sub, origin, bound, starts[0]) : append(first, Sub, origin, starts[0], bound);
            int64_t farthest = Limits::maxTurnsWithoutPoll * (approach > 0 ? approach : -approach) * static_cast<int64_t>(plan.lanes());
            enters = append(first, BitAnd, origin, enters, append(first, BelowEqual, origin, distance, appendInteger(first, origin, type, farthest)));
        }
        BasicBlock* behindTests = entered;
        if (plan.numberOfDistancesCheckedAtEntry) {
            BasicBlock* next = entered;
            for (unsigned index = 0; index < plan.numberOfDistancesCheckedAtEntry; ++index) {
                behindTests = m_proc.addBlock(frequency);
                BasicBlock* reports = m_proc.addBlock(0);
                Value* distanceLessOne = appendSum(behindTests, origin, plan.distancesCheckedAtEntry[index], -1);
                appendBranch(behindTests, origin, append(behindTests, Below, origin, distanceLessOne, appendInteger(behindTests, origin, Int64, bytesPerVector - 1)), FrequentedBlock(reports, FrequencyClass::Rare), FrequentedBlock(next));
                appendReportOfDistance(reports, distanceLessOne, plan.accessesCheckedAtEntry[index]);
                next = behindTests;
            }
        }
        BasicBlock* behindFirstTest = behindTests;
        if (plan.numberOfDistancesTestedAtEntry) {
            behindFirstTest = m_proc.addBlock(frequency);
            Value* liesApart = nullptr;
            for (unsigned index = 0; index < plan.numberOfDistancesTestedAtEntry; ++index) {
                Value* isFar = append(behindFirstTest, AboveEqual, origin, appendSum(behindFirstTest, origin, plan.distancesTestedAtEntry[index], -1), appendInteger(behindFirstTest, origin, Int64, bytesPerVector - 1));
                liesApart = liesApart ? append(behindFirstTest, BitAnd, origin, liesApart, isFar) : isFar;
            }
            appendBranch(behindFirstTest, origin, liesApart, FrequentedBlock(behindTests), FrequentedBlock(plan.inFrontOfLoop, FrequencyClass::Rare));
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
            appendUpsilon(entered, origin, from[index], counter.phiOfVectorLoop);
        }
        Value* next = plan.tested().nextOfVectorLoop;
        BasicBlock* rest = plan.again;
        if (!plan.testsCounterBeforeStep) {
            rest = m_proc.addBlock(frequency);
            plan.left = m_proc.addBlock(frequency);
            appendBranch(rest, origin, append(rest, plan.relation, origin, next, bounding ? bounding->nextOfVectorLoop : bound), FrequentedBlock(plan.again), FrequentedBlock(plan.left));
        }

        // As far as the tested counter may go while there is room, seen from where the stretch begins.
        Value* farBound = nullptr;
        if (!bounding)
            farBound = append(entered, Sub, origin, bound, appendInteger(entered, origin, type, room));
        else if (hasFarBound) {
            // counter + room REL other, and other = sum - counter: twice the counter REL sum - room.
            bool roundsUp = isLessRelation(plan.relation) != plan.includesEqual();
            Value* sum = append(entered, Add, origin, from[0], from[plan.indexOfBoundingCounter]);
            farBound = append(entered, SShr, origin, append(entered, Sub, origin, sum, appendInteger(entered, origin, type, room - roundsUp)), appendInteger(entered, origin, Int32, 1));
        }
        Value* goesOn = nullptr;
        BasicBlock* behindTurns = rest;
        if (!runsInStretches)
            goesOn = appendTestOfRoom(plan, turn, origin, farBound);
        else {
            goesOn = append(turn, plan.relation, origin, next, appendBoundOfStretch(m_proc, entered, origin, { from[0], farBound, plan.relation, plan.tested().step * static_cast<int64_t>(plan.lanes()), BoundLiesBeyond::Yes }));
            behindTurns = m_proc.addBlock(frequency);
            BasicBlock* betweenStretches = m_proc.addBlock(frequency);
            appendBranch(behindTurns, origin, appendTestOfRoom(plan, behindTurns, origin, farBound), FrequentedBlock(betweenStretches, FrequencyClass::Rare), FrequentedBlock(rest));
            appendCopy(betweenStretches, plan.poll);
            for (unsigned index = 0; index < plan.numberOfCounters; ++index)
                appendUpsilon(betweenStretches, origin, plan.counterTable[index].nextOfVectorLoop, from[index]);
            appendJump(betweenStretches, origin, entered);
        }
        appendJump(entered, origin, turn);
        appendBranch(turn, origin, goesOn, FrequentedBlock(turn, plan.test->successor(plan.stayingSuccessorIndex).frequency()), FrequentedBlock(behindTurns, plan.test->successor(1 - plan.stayingSuccessorIndex).frequency()));
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

// It takes nothing, has no effect and no register but the one for its result. So it puts a constant there, an address for one, and goes nowhere.
bool makesConstant(const Value* value)
{
    auto* patchpoint = value->as<PatchpointValue>();
    return patchpoint && !patchpoint->numChildren() && !patchpoint->numGPScratchRegisters && !patchpoint->numFPScratchRegisters
        && patchpoint->earlyClobbered().isEmpty() && patchpoint->lateClobbered().isEmpty() && patchpoint->effects == Effects::none();
}

// It gives no result, writes nothing and has no register for numbers. Whatever it tests, it leaves for good or the code behind it runs as if it had not been there.
bool onlyTests(const Value* value)
{
    auto* patchpoint = value->as<PatchpointValue>();
    if (!patchpoint || patchpoint->type() != Void || patchpoint->numFPScratchRegisters || patchpoint->isLoopPoll)
        return false;
    RegisterSet clobbered = patchpoint->earlyClobbered();
    clobbered.merge(patchpoint->lateClobbered());
    clobbered.filter(RegisterSet::allFPRs());
    const Effects& effects = patchpoint->effects;
    return clobbered.isEmpty() && !effects.writes && !effects.writesPinned && !effects.fence;
}

bool mayLoseUpperHalvesOfVectors(const Value* value)
{
    return value->opcode() == CCall || (value->opcode() == Patchpoint && !onlyTests(value) && !makesConstant(value));
}

// From a use upward to the definition, through every block between.
Value* findPatchpointOnWayFromDefinition(Value* vector, BasicBlock* blockOfUse, unsigned indexOfUse)
{
    IndexSet<BasicBlock*> seen;
    Vector<BasicBlock*, 8> worklist;
    BasicBlock* block = blockOfUse;
    unsigned end = indexOfUse;
    for (;;) {
        bool foundDefinition = false;
        for (unsigned index = end; index-- && !foundDefinition;) {
            Value* value = block->at(index);
            foundDefinition = value == vector;
            if (!foundDefinition && mayLoseUpperHalvesOfVectors(value))
                return value;
        }
        if (!foundDefinition) {
            for (BasicBlock* predecessor : block->predecessors()) {
                if (seen.add(predecessor))
                    worklist.append(predecessor);
            }
        }
        if (worklist.isEmpty())
            return nullptr;
        block = worklist.takeLast();
        end = block->size();
    }
}

} // anonymous namespace

Value* findPatchpointWhereVectorIsAlive(Procedure& proc)
{
    for (BasicBlock* block : proc) {
        for (unsigned index = 0; index < block->size(); ++index) {
            for (Value* child : block->at(index)->children()) {
                if (child->type() != V128)
                    continue;
                if (Value* patchpoint = findPatchpointOnWayFromDefinition(child, block, index))
                    return patchpoint;
            }
        }
    }
    return nullptr;
}

void checkNoVectorLivesAcrossPatchpoint(Procedure& proc)
{
    PhaseScope phaseScope(proc, "checkNoVectorLivesAcrossPatchpoint"_s);
    Value* patchpoint = findPatchpointWhereVectorIsAlive(proc);
    if (!patchpoint)
        return;
    dataLogLn("B3: a vector is alive at ", deepDump(proc, patchpoint));
    RELEASE_ASSERT_NOT_REACHED();
}

#pragma clang attribute pop

bool vectorizeLoops(Procedure& proc)
{
    PhaseScope phaseScope(proc, "vectorizeLoops"_s);
    VectorizeLoops vectorizeLoops(proc);
    return vectorizeLoops.run();
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
