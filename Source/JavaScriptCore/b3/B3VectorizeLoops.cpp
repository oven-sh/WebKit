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
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>
#include <wtf/IndexSet.h>

namespace JSC { namespace B3 {

namespace {

static constexpr bool verbose = false;

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
        for (Plan& plan : m_plans) {
            appendVectorLoop(plan);
            valuesToDemote.add(plan.counter);
            if (!plan.testsCounterBeforeStep)
                valuesToDemote.add(plan.next);
        }
        demoteValues(m_proc, valuesToDemote);
        for (const Plan& plan : m_plans)
            leaveVectorLoop(plan);
        m_proc.resetReachability();
        m_proc.invalidateCFG();
        return true;
    }

private:
    static constexpr unsigned bytesPerVector = 16;

    // factorOfCounter * counter + constant + the sum of factor * value over the terms, in arithmetic modulo 2^64. No term changes in the loop.
    struct LinearForm {
        struct Term {
            Value* value { nullptr };
            int64_t factor { 0 };
        };

        void add(const LinearForm& other, int64_t factor)
        {
            auto times = [&](int64_t value) {
                return static_cast<int64_t>(static_cast<uint64_t>(value) * static_cast<uint64_t>(factor));
            };
            auto plus = [](int64_t left, int64_t right) {
                return static_cast<int64_t>(static_cast<uint64_t>(left) + static_cast<uint64_t>(right));
            };
            factorOfCounter = plus(factorOfCounter, times(other.factorOfCounter));
            constant = plus(constant, times(other.constant));
            for (const Term& term : other.terms) {
                size_t index = terms.findIf([&](const Term& own) {
                    return own.value == term.value;
                });
                if (index == notFound) {
                    terms.append({ term.value, times(term.factor) });
                    continue;
                }
                terms[index].factor = plus(terms[index].factor, times(term.factor));
                if (!terms[index].factor)
                    terms.removeAt(index);
            }
        }

        Vector<Term, 2> terms;
        int64_t factorOfCounter { 0 };
        int64_t constant { 0 };
    };

    struct Access {
        MemoryValue* memory { nullptr };
        LinearForm address;
    };

    enum Role : uint8_t {
        IsScalar = 1,
        IsVector = 2,
    };

    struct Plan {
        Vector<BasicBlock*, 8> blocks;
        IndexSet<BasicBlock*> blockSet;
        Vector<BasicBlock*, 4> blocksNeverEntered;
        BasicBlock* preHeader { nullptr };
        BasicBlock* header { nullptr };
        BasicBlock* test { nullptr };
        unsigned stayingSuccessorIndex { 0 };
        Opcode relation { Oops };
        Value* counter { nullptr };
        Value* next { nullptr };
        Value* start { nullptr };
        Value* bound { nullptr };
        int64_t step { 0 };
        bool testsCounterBeforeStep { false };
        bool startHasRoom { false };
        bool needsGuardOfFarBound { false };
        int64_t farthestBoundWithRoom { 0 };
        unsigned bytesPerLane { 0 };
        Vector<Access, 4> accesses;
        Vector<LinearForm, 2> distancesTestedAtEntry;
        UncheckedKeyHashMap<Value*, uint8_t> roles;
        // Behind a test at the bottom: what the last turn hands to Phis behind the loop.
        Vector<UpsilonValue*, 2> upsilonsForPhisBehindLoop;

        BasicBlock* again { nullptr };
        BasicBlock* left { nullptr };
        // The loop itself keeps one way in.
        BasicBlock* inFrontOfLoop { nullptr };
        Value* nextBehindVectorLoop { nullptr };

        unsigned lanes() const { return bytesPerVector / bytesPerLane; }
        int64_t room() const { return step * static_cast<int64_t>(lanes() - 1); }
        Signedness signedness() const { return isUnsignedRelation(relation) ? Signedness::Unsigned : Signedness::Signed; }
        bool includesEqual() const { return relation == LessEqual || relation == GreaterEqual || relation == BelowEqual || relation == AboveEqual; }
        bool isInvariant(Value* value) const { return value->isConstant() || !blockSet.contains(value->owner); }
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
                if (condition->hasInt())
                    successorIndex = condition->asInt() ? 0 : 1;
                else {
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

    static bool readCounter(PhiChildren& phiChildren, Value* phi, Plan& plan)
    {
        plan.start = nullptr;
        plan.next = nullptr;
        for (UpsilonValue* upsilon : phiChildren[phi]) {
            if (plan.blocksNeverEntered.contains(upsilon->owner))
                continue;
            Value* input = upsilon->child(0)->foldIdentity();
            Value*& place = upsilon->owner == plan.preHeader ? plan.start : plan.next;
            if (place || (upsilon->owner != plan.preHeader && !plan.blockSet.contains(upsilon->owner)))
                return false;
            place = input;
        }
        if (!plan.start || !plan.next)
            return false;
        std::optional<int64_t> step = stepOf(plan.next, phi);
        if (!step || (*step != 1 && *step != -1))
            return false;
        plan.counter = phi;
        plan.step = *step;
        return true;
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

    static bool readTest(PhiChildren& phiChildren, Plan& plan)
    {
        DominatingCondition condition = stripNegations(plan.test->last()->child(0)->foldIdentity());
        Value* comparison = condition.value->foldIdentity();
        std::optional<Opcode> inverted = invertedRelation(comparison->opcode());
        if (!inverted || !comparison->child(0)->type().isInt())
            return false;
        Opcode relation = !plan.stayingSuccessorIndex == condition.isNonZero ? comparison->opcode() : *inverted;

        for (unsigned testedIndex = 0; testedIndex < 2; ++testedIndex) {
            Value* tested = comparison->child(testedIndex)->foldIdentity();
            Value* bound = comparison->child(1 - testedIndex)->foldIdentity();
            if (!plan.isInvariant(bound))
                continue;
            Value* phi = tested;
            if (phi->opcode() != Phi) {
                if (phi->numChildren() != 2)
                    continue;
                phi = phi->child(phi->child(0)->hasInt() ? 1 : 0);
            }
            if (phi->opcode() != Phi || phi->owner != plan.header || !readCounter(phiChildren, phi, plan) || (tested != phi && tested != plan.next))
                continue;
            plan.relation = testedIndex ? flippedRelation(relation) : relation;
            if (isLessRelation(plan.relation) != (plan.step > 0))
                continue;
            plan.testsCounterBeforeStep = tested == phi;
            plan.bound = bound;
            return hasWholeTurnsBetweenTests(plan);
        }
        return false;
    }

    // The counters of the turns that one turn of the vector loop stands for.
    IntegerInterval intervalOfCounter(const Plan& plan, Signedness signedness)
    {
        IntegerInterval start = intervalFor(m_proc, plan.start, plan.signedness());
        IntegerInterval bound = intervalFor(m_proc, plan.bound, plan.signedness());
        Int128 strict = plan.includesEqual() ? 0 : 1;
        IntegerInterval result = plan.step > 0 ? IntegerInterval { start.min, bound.max - strict } : IntegerInterval { bound.min + strict, start.max };
        IntegerInterval known = intervalFor(m_proc, plan.counter, plan.signedness());
        result.min = std::max(result.min, known.min);
        result.max = std::min(result.max, known.max);
        if (signedness == plan.signedness() || (result.min >= 0 && result.max <= fullInterval(plan.counter->type(), Signedness::Signed).max))
            return result;
        return intervalFor(m_proc, plan.counter, signedness);
    }

    std::optional<LinearForm> linearFormOf(const Plan& plan, Value* value, unsigned timeToLive)
    {
        value = value->foldIdentity();
        if (value->type() != Int64)
            return std::nullopt;
        LinearForm form;
        if (value == plan.counter) {
            form.factorOfCounter = 1;
            return form;
        }
        if (value->hasInt64()) {
            form.constant = value->asInt64();
            return form;
        }
        if (plan.isInvariant(value)) {
            form.terms.append({ value, 1 });
            return form;
        }
        if (!timeToLive)
            return std::nullopt;
        switch (value->opcode()) {
        case Add:
        case Sub: {
            std::optional<LinearForm> left = linearFormOf(plan, value->child(0), timeToLive - 1);
            std::optional<LinearForm> right = linearFormOf(plan, value->child(1), timeToLive - 1);
            if (!left || !right)
                return std::nullopt;
            left->add(*right, value->opcode() == Add ? 1 : -1);
            return left;
        }
        case Shl: {
            if (!value->child(1)->hasInt32())
                return std::nullopt;
            std::optional<LinearForm> shifted = linearFormOf(plan, value->child(0), timeToLive - 1);
            if (!shifted)
                return std::nullopt;
            form.add(*shifted, static_cast<int64_t>(1ull << (value->child(1)->asInt32() & 63)));
            return form;
        }
        case ZExt32:
        case SExt32: {
            Signedness signedness = value->opcode() == ZExt32 ? Signedness::Unsigned : Signedness::Signed;
            auto affine = affineFormOf(m_proc, value, plan.counter, signedness);
            if (!affine || !affine->exactBaseRange.contains(intervalOfCounter(plan, signedness)))
                return std::nullopt;
            form.factorOfCounter = 1;
            form.constant = affine->offset;
            return form;
        }
        default:
            return std::nullopt;
        }
    }

    bool findAccesses(Plan& plan)
    {
        for (BasicBlock* block : plan.blocks) {
            for (Value* value : *block) {
                MemoryValue* memory = value->as<MemoryValue>();
                if (!memory)
                    continue;
                if ((!memory->isLoad() && !memory->isStore()) || memory->hasFence() || plan.accesses.size() == Limits::maxAccessesPerTurnOfVectorizedLoop)
                    return false;
                std::optional<LinearForm> address = linearFormOf(plan, memory->lastChild(), Limits::maxDepthOfAddressInVectorizedLoop);
                if (!address) {
                    dataLogLnIf(verbose, "   address: ", deepDump(memory));
                    return false;
                }
                LinearForm offset;
                offset.constant = memory->offset();
                address->add(offset, 1);
                unsigned bytes = memory->accessByteSize();
                if (!plan.bytesPerLane)
                    plan.bytesPerLane = bytes;
                // From turn to turn every access goes on to the next element.
                if (bytes != plan.bytesPerLane || bytes > 8 || address->factorOfCounter * plan.step != static_cast<int64_t>(bytes)) {
                    dataLogLnIf(verbose, "   stride: ", deepDump(memory), " factor ", address->factorOfCounter, " step ", plan.step);
                    return false;
                }
                plan.accesses.append({ memory, WTF::move(*address) });
            }
        }
        return plan.accesses.containsIf([](const Access& access) {
            return access.memory->isStore();
        });
    }

    // The vector loop makes an access for all of its turns before it makes the next access for any. That changes nothing unless a later access of an earlier turn meets an earlier access of a later turn.
    bool keepsOrderOfAccesses(Plan& plan)
    {
        for (unsigned laterIndex = 0; laterIndex < plan.accesses.size(); ++laterIndex) {
            const Access& later = plan.accesses[laterIndex];
            for (unsigned earlierIndex = 0; earlierIndex < laterIndex; ++earlierIndex) {
                const Access& earlier = plan.accesses[earlierIndex];
                if ((!later.memory->isStore() && !earlier.memory->isStore()) || !later.memory->range().overlaps(earlier.memory->range()))
                    continue;
                LinearForm distance = later.address;
                distance.add(earlier.address, -1);
                RELEASE_ASSERT(!distance.factorOfCounter);
                if (distance.terms.isEmpty()) {
                    if (distance.constant > 0 && distance.constant < static_cast<int64_t>(bytesPerVector))
                        return false;
                    continue;
                }
                bool isKnown = plan.distancesTestedAtEntry.containsIf([&](const LinearForm& other) {
                    LinearForm difference = distance;
                    difference.add(other, -1);
                    return difference.terms.isEmpty() && !difference.constant;
                });
                if (isKnown)
                    continue;
                if (plan.distancesTestedAtEntry.size() == Limits::maxDistancesTestedInFrontOfVectorizedLoop)
                    return false;
                plan.distancesTestedAtEntry.append(WTF::move(distance));
            }
        }
        return true;
    }

    static unsigned bytesOf(Type type)
    {
        return sizeofType(type);
    }

    bool needScalar(Plan& plan, Value* value)
    {
        if (value == plan.counter || plan.isInvariant(value))
            return true;
        uint8_t& role = plan.roles.add(value, 0).iterator->value;
        if (role & IsScalar)
            return true;
        role |= IsScalar;
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
        if (type.isFloat()) {
            if (bytesOf(type) != plan.bytesPerLane)
                return false;
        } else if (!type.isInt() || bytesOf(type) < plan.bytesPerLane)
            return false;
        if (plan.isInvariant(value))
            return true;
        if (value == plan.counter)
            return false;
        uint8_t& role = plan.roles.add(value, 0).iterator->value;
        if (role & IsVector)
            return true;
        role |= IsVector;
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
            break;
        case Mul:
            if (!type.isFloat() && plan.bytesPerLane != 2 && plan.bytesPerLane != 4)
                return false;
            break;
        case Div:
        case Abs:
        case Sqrt:
            if (!type.isFloat())
                return false;
            break;
        case Trunc:
        case ZExt32:
        case SExt32:
            if (plan.bytesPerLane > 4)
                return false;
            break;
        default:
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
                    if (value != plan.counter) {
                        dataLogLnIf(verbose, "   phi: ", deepDump(value));
                        return false;
                    }
                    continue;
                }
                if (auto* upsilon = value->as<UpsilonValue>()) {
                    if (upsilon->phi() == plan.counter)
                        continue;
                    if (plan.blockSet.contains(upsilon->phi()->owner))
                        return false;
                    Value* input = upsilon->child(0);
                    if (!plan.testsCounterBeforeStep && !plan.isInvariant(input) && input != plan.counter && input != plan.next)
                        return false;
                    plan.upsilonsForPhisBehindLoop.append(upsilon);
                    continue;
                }
                if (value == block->last())
                    continue;
                if (value->effects().mustExecute()) {
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
        IntegerInterval full = fullInterval(plan.bound->type(), signedness);
        IntegerInterval bound = intervalFor(m_proc, plan.bound, signedness);
        // The counter behind the last turn is computed.
        if (plan.includesEqual() && (plan.step > 0 ? bound.max == full.max : bound.min == full.min))
            return false;
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

    PhiChildren& prepareToReadCounters()
    {
        if (!m_phiChildren) {
            m_proc.resetValueOwners();
            m_phiChildren.emplace(m_proc);
        }
        return *m_phiChildren;
    }

    void considerLoop(NaturalLoops& loops, const NaturalLoop& loop)
    {
        if (!isInnermostLoop(loops, loop))
            return;
        Plan plan;
        plan.header = loop.header();
        plan.preHeader = findPreHeader(loops, loop);
        if (!plan.preHeader || !findTurn(loops, loop, plan)) {
            dataLogLnIf(verbose, "vectorizeLoops: ", *plan.header, ": no pre-header or no single way through a turn");
            return;
        }
        unsigned numberOfValues = 0;
        bool stores = false;
        for (BasicBlock* block : plan.blocks) {
            numberOfValues += block->size();
            for (Value* value : *block) {
                MemoryValue* memory = value->as<MemoryValue>();
                stores |= memory && memory->isStore();
            }
        }
        if (!stores || numberOfValues > Limits::maxValuesPerTurnOfVectorizedLoop)
            return;
        PhiChildren& phiChildren = prepareToReadCounters();
        const char* reason = !readTest(phiChildren, plan) ? "test" : !findAccesses(plan) ? "accesses" : !keepsOrderOfAccesses(plan) ? "order" : !findRoles(plan) ? "roles" : !planFarBound(plan) ? "far bound" : nullptr;
        dataLogLnIf(verbose, "vectorizeLoops: ", *plan.header, ": ", reason ? reason : "planned");
        if (reason)
            return;
        m_plans.append(WTF::move(plan));
    }

    // Behind a test at the bottom the loop may be left without one more turn of its own. Then nothing tells what its last turn would have computed, but for the counter.
    void dropPlansWhoseTurnsAreReadBehindLoop()
    {
        UncheckedKeyHashMap<BasicBlock*, unsigned> planOfBlock;
        for (unsigned index = 0; index < m_plans.size(); ++index) {
            if (m_plans[index].testsCounterBeforeStep)
                continue;
            for (BasicBlock* block : m_plans[index].blocks)
                planOfBlock.add(block, index);
            for (BasicBlock* block : m_plans[index].blocksNeverEntered)
                planOfBlock.add(block, index);
        }
        if (planOfBlock.isEmpty())
            return;
        BitVector dropped;
        for (BasicBlock* block : m_proc) {
            auto own = planOfBlock.find(block);
            for (Value* value : *block) {
                for (Value* child : value->children()) {
                    auto found = planOfBlock.find(child->owner);
                    if (found == planOfBlock.end() || (own != planOfBlock.end() && own->value == found->value) || child->isConstant())
                        continue;
                    const Plan& plan = m_plans[found->value];
                    if (child != plan.counter && child != plan.next)
                        dropped.set(found->value);
                }
            }
        }
        for (unsigned index = m_plans.size(); index--;) {
            if (dropped.get(index))
                m_plans.removeAt(index);
        }
    }

    SIMDLane laneFor(const Plan& plan, Type type)
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
        case 8:
            return SIMDLane::i64x2;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    static Opcode vectorOpcodeFor(Opcode opcode)
    {
        switch (opcode) {
        case Add:
            return VectorAdd;
        case Sub:
            return VectorSub;
        case Mul:
            return VectorMul;
        case Div:
            return VectorDiv;
        case BitAnd:
            return VectorAnd;
        case BitOr:
            return VectorOr;
        case BitXor:
            return VectorXor;
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
    }

    struct Copies {
        BasicBlock* entered { nullptr };
        BasicBlock* turn { nullptr };
        Value* counter { nullptr };
        UncheckedKeyHashMap<Value*, Value*> scalars;
        UncheckedKeyHashMap<Value*, Value*> vectors;
    };

    Value* scalarFor(const Plan& plan, Copies& copies, Value* value)
    {
        if (value == plan.counter)
            return copies.counter;
        if (value->isConstant()) {
            Value* copy = m_proc.clone(value);
            copies.turn->append(copy);
            return copy;
        }
        if (plan.isInvariant(value))
            return value;
        Value* copy = copies.scalars.get(value);
        RELEASE_ASSERT(copy);
        return copy;
    }

    Value* vectorFor(const Plan& plan, Copies& copies, Value* value)
    {
        if (Value* vector = copies.vectors.get(value))
            return vector;
        RELEASE_ASSERT(plan.isInvariant(value));
        Value* lane = value;
        if (value->isConstant()) {
            lane = m_proc.clone(value);
            copies.entered->append(lane);
        }
        if (lane->type() == Int64 && plan.bytesPerLane <= 4)
            lane = copies.entered->appendNew<Value>(m_proc, Trunc, value->origin(), lane);
        Value* vector = copies.entered->appendNew<SIMDValue>(m_proc, value->origin(), VectorSplat, V128, laneFor(plan, value->type()), SIMDSignMode::None, lane);
        copies.vectors.add(value, vector);
        return vector;
    }

    // Every lane holds the same bits. They are laid out in front of the loop.
    Value* appendLanesOf(const Plan& plan, Copies& copies, Origin origin, uint64_t bits)
    {
        BasicBlock* entered = copies.entered;
        Value* lane = plan.bytesPerLane == 8 ? entered->appendIntConstant(m_proc, origin, Int64, static_cast<int64_t>(bits)) : entered->appendIntConstant(m_proc, origin, Int32, static_cast<int32_t>(bits));
        return entered->appendNew<SIMDValue>(m_proc, origin, VectorSplat, V128, laneFor(plan, lane->type()), SIMDSignMode::None, lane);
    }

    Value* appendUnaryOperation(const Plan& plan, Copies& copies, Value* value)
    {
        Value* operand = vectorFor(plan, copies, value->child(0));
        Origin origin = value->origin();
        SIMDLane lane = laneFor(plan, value->type());
        uint64_t signBit = 1ull << (plan.bytesPerLane * 8 - 1);
        auto append = [&](Opcode opcode, SIMDLane lane, auto... operands) {
            return copies.turn->appendNew<SIMDValue>(m_proc, origin, opcode, V128, lane, SIMDSignMode::None, operands...);
        };
        switch (value->opcode()) {
        case Neg:
            if (isARM64())
                return append(VectorNeg, lane, operand);
            if (value->type().isFloat())
                return append(VectorXor, SIMDLane::v128, operand, appendLanesOf(plan, copies, origin, signBit));
            return append(VectorSub, lane, appendLanesOf(plan, copies, origin, 0), operand);
        case Abs:
            if (isARM64())
                return append(VectorAbs, lane, operand);
            return append(VectorAnd, SIMDLane::v128, operand, appendLanesOf(plan, copies, origin, signBit - 1));
        case Sqrt:
            return append(VectorSqrt, lane, operand);
        default:
            return operand;
        }
    }

    void appendTurn(const Plan& plan, Copies& copies)
    {
        BasicBlock* turn = copies.turn;
        for (BasicBlock* block : plan.blocks) {
            for (Value* value : *block) {
                uint8_t role = plan.roles.get(value);
                if (role & IsScalar) {
                    Value* copy = m_proc.clone(value);
                    for (Value*& child : copy->children())
                        child = scalarFor(plan, copies, child);
                    turn->append(copy);
                    copies.scalars.add(value, copy);
                }
                MemoryValue* memory = value->as<MemoryValue>();
                if (role & IsVector) {
                    Value* vector = nullptr;
                    if (memory)
                        vector = turn->appendNew<MemoryValue>(m_proc, Load, V128, value->origin(), scalarFor(plan, copies, memory->lastChild()), memory->offset(), memory->range());
                    else if (value->numChildren() == 1)
                        vector = appendUnaryOperation(plan, copies, value);
                    else {
                        Value* left = vectorFor(plan, copies, value->child(0));
                        Value* right = vectorFor(plan, copies, value->child(1));
                        Opcode opcode = vectorOpcodeFor(value->opcode());
                        bool namesNoLane = opcode == VectorAnd || opcode == VectorOr || opcode == VectorXor;
                        vector = turn->appendNew<SIMDValue>(m_proc, value->origin(), opcode, V128, namesNoLane ? SIMDLane::v128 : laneFor(plan, value->type()), SIMDSignMode::None, left, right);
                    }
                    copies.vectors.add(value, vector);
                }
                if (memory && memory->isStore()) {
                    Value* stored = vectorFor(plan, copies, memory->child(0));
                    turn->appendNew<MemoryValue>(m_proc, Store, value->origin(), stored, scalarFor(plan, copies, memory->lastChild()), memory->offset(), memory->range());
                }
                if (onlyKeepsValuesAlive(value))
                    turn->append(m_proc.clone(value));
            }
        }
    }

    Value* appendSum(BasicBlock* block, Origin origin, const LinearForm& form, int64_t addend)
    {
        Value* sum = block->appendIntConstant(m_proc, origin, Int64, static_cast<int64_t>(static_cast<uint64_t>(form.constant) + static_cast<uint64_t>(addend)));
        for (const LinearForm::Term& term : form.terms) {
            if (term.factor == 1 || term.factor == -1) {
                sum = block->appendNew<Value>(m_proc, term.factor == 1 ? Add : Sub, origin, sum, term.value);
                continue;
            }
            sum = block->appendNew<Value>(m_proc, Add, origin, sum, block->appendNew<Value>(m_proc, Mul, origin, term.value, block->appendIntConstant(m_proc, origin, Int64, term.factor)));
        }
        return sum;
    }

    void appendVectorLoop(Plan& plan)
    {
        Origin origin = plan.test->last()->origin();
        m_proc.addRemark({ RemarkKind::LoopVectorized, GaveUpReason::None, origin, std::nullopt });
        Type type = plan.bound->type();
        BasicBlock* preHeader = plan.preHeader;
        Copies copies;
        copies.entered = m_proc.addBlock(preHeader->frequency());
        copies.turn = m_proc.addBlock(plan.header->frequency());
        plan.again = m_proc.addBlock(preHeader->frequency());
        plan.inFrontOfLoop = m_proc.addBlock(preHeader->frequency());
        plan.inFrontOfLoop->appendNew<Value>(m_proc, Jump, origin);
        plan.inFrontOfLoop->setSuccessors(FrequentedBlock(plan.header));
        BasicBlock* entered = copies.entered;
        BasicBlock* turn = copies.turn;

        auto append = [&](Value* value) {
            preHeader->appendNonTerminal(value);
            return value;
        };
        auto constant = [&](int64_t value) {
            return append(m_proc.addIntConstant(origin, type, value));
        };
        Value* bound = plan.bound->hasInt() ? constant(plan.bound->asInt()) : plan.bound;
        Value* start = plan.start->hasInt() ? constant(plan.start->asInt()) : plan.start;
        int64_t room = plan.room();
        Value* enters = nullptr;
        if (plan.startHasRoom)
            enters = append(m_proc.add<Value>(plan.relation, origin, append(m_proc.add<Value>(Add, origin, start, constant(room))), bound));
        else {
            enters = append(m_proc.add<Value>(plan.relation, origin, start, append(m_proc.add<Value>(Sub, origin, bound, constant(room)))));
            if (plan.needsGuardOfFarBound) {
                Opcode hasRoom = plan.step > 0 ? (isUnsignedRelation(plan.relation) ? AboveEqual : GreaterEqual) : (isUnsignedRelation(plan.relation) ? BelowEqual : LessEqual);
                enters = append(m_proc.add<Value>(BitAnd, origin, enters, append(m_proc.add<Value>(hasRoom, origin, bound, constant(plan.farthestBoundWithRoom)))));
            }
        }
        BasicBlock* behindFirstTest = entered;
        if (!plan.distancesTestedAtEntry.isEmpty()) {
            behindFirstTest = m_proc.addBlock(preHeader->frequency());
            Value* liesApart = nullptr;
            for (const LinearForm& distance : plan.distancesTestedAtEntry) {
                Value* isFar = behindFirstTest->appendNew<Value>(m_proc, AboveEqual, origin, appendSum(behindFirstTest, origin, distance, -1), behindFirstTest->appendIntConstant(m_proc, origin, Int64, bytesPerVector - 1));
                liesApart = liesApart ? behindFirstTest->appendNew<Value>(m_proc, BitAnd, origin, liesApart, isFar) : isFar;
            }
            behindFirstTest->appendNew<Value>(m_proc, Branch, origin, liesApart);
            behindFirstTest->setSuccessors(FrequentedBlock(entered), FrequentedBlock(plan.inFrontOfLoop, FrequencyClass::Rare));
        }
        preHeader->replaceLastWithNew<Value>(m_proc, Branch, origin, enters);
        preHeader->setSuccessors(FrequentedBlock(behindFirstTest), FrequentedBlock(plan.inFrontOfLoop));

        copies.counter = turn->appendNew<Value>(m_proc, Phi, type, plan.counter->origin());
        m_proc.knownProperties().copy(plan.counter, copies.counter);
        appendTurn(plan, copies);
        Value* next = turn->appendNew<Value>(m_proc, Add, origin, copies.counter, turn->appendIntConstant(m_proc, origin, type, plan.step * static_cast<int64_t>(plan.lanes())));
        plan.nextBehindVectorLoop = next;
        turn->appendNew<UpsilonValue>(m_proc, origin, next, copies.counter);

        Value* farBound = entered->appendNew<Value>(m_proc, Sub, origin, bound, entered->appendIntConstant(m_proc, origin, type, room));
        entered->appendNew<UpsilonValue>(m_proc, origin, start, copies.counter);
        entered->appendNew<Value>(m_proc, Jump, origin);
        entered->setSuccessors(FrequentedBlock(turn));

        BasicBlock* rest = plan.again;
        if (!plan.testsCounterBeforeStep) {
            rest = m_proc.addBlock(preHeader->frequency());
            plan.left = m_proc.addBlock(preHeader->frequency());
            rest->appendNew<Value>(m_proc, Branch, origin, rest->appendNew<Value>(m_proc, plan.relation, origin, next, bound));
            rest->setSuccessors(FrequentedBlock(plan.again), FrequentedBlock(plan.left));
        }
        turn->appendNew<Value>(m_proc, Branch, origin, turn->appendNew<Value>(m_proc, plan.relation, origin, next, farBound));
        turn->setSuccessors(FrequentedBlock(turn, plan.test->successor(plan.stayingSuccessorIndex).frequency()), FrequentedBlock(rest, plan.test->successor(1 - plan.stayingSuccessorIndex).frequency()));
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

    void leaveVectorLoop(const Plan& plan)
    {
        Origin origin = plan.test->last()->origin();
        Value* next = plan.nextBehindVectorLoop;
        RELEASE_ASSERT(plan.counter->opcode() == Identity && plan.counter->child(0)->opcode() == Get);
        plan.again->appendNew<VariableValue>(m_proc, Set, origin, plan.counter->child(0)->as<VariableValue>()->variable(), next);
        plan.again->appendNew<Value>(m_proc, Jump, origin);
        plan.again->setSuccessors(FrequentedBlock(plan.inFrontOfLoop));
        if (!plan.left)
            return;

        BasicBlock* left = plan.left;
        Value* last = left->appendNew<Value>(m_proc, Sub, origin, next, left->appendIntConstant(m_proc, origin, next->type(), plan.step));
        left->appendNew<VariableValue>(m_proc, Set, origin, variableOf(plan.counter), last);
        left->appendNew<VariableValue>(m_proc, Set, origin, variableOf(plan.next), next);
        for (UpsilonValue* upsilon : plan.upsilonsForPhisBehindLoop) {
            Value* copy = m_proc.clone(upsilon);
            Value* input = upsilon->child(0);
            if (input->isConstant()) {
                input = m_proc.clone(input);
                left->append(input);
            } else if (input->opcode() == Get) {
                input = m_proc.clone(input);
                left->append(input);
            }
            copy->child(0) = input;
            left->append(copy);
        }
        left->appendNew<Value>(m_proc, Jump, origin);
        left->setSuccessors(FrequentedBlock(plan.test->successorBlock(1 - plan.stayingSuccessorIndex)));
    }

    Procedure& m_proc;
    std::optional<PhiChildren> m_phiChildren;
    Vector<Plan> m_plans;
};

} // anonymous namespace

bool vectorizeLoops(Procedure& proc)
{
    PhaseScope phaseScope(proc, "vectorizeLoops"_s);
    VectorizeLoops vectorizeLoops(proc);
    return vectorizeLoops.run();
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
