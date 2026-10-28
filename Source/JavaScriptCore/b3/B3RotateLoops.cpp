/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "B3RotateLoops.h"

#if ENABLE(B3_JIT)

#include "B3BasicBlockInlines.h"
#include "B3InsertionSetInlines.h"
#include "B3LoopUtilities.h"
#include "B3NaturalLoops.h"
#include "B3PhaseScope.h"
#include "B3ProcedureInlines.h"
#include "B3RegionCloner.h"
#include "B3UpsilonValue.h"
#include "B3ValueInlines.h"
#include "B3VariableValue.h"

namespace JSC { namespace B3 {

namespace {

class RotateLoops {
public:
    RotateLoops(Procedure& proc, const Vector<BasicBlock*>* allowedHeaders)
        : m_proc(proc)
        , m_allowedHeaders(allowedHeaders)
    {
    }

    bool run()
    {
        NaturalLoops& loops = m_proc.naturalLoops();
        m_edgesBack.resize(loops.numLoops());
        for (unsigned loopIndex = loops.numLoops(); loopIndex--;) {
            if (!m_allowedHeaders || m_allowedHeaders->contains(loops.loop(loopIndex).header()))
                considerLoop(loops, loops.loop(loopIndex));
        }
        if (m_rotations.isEmpty())
            return false;

        std::ranges::stable_sort(m_rotations, { }, &Rotation::depth);
        for (auto& rotation : m_rotations) {
            m_didJoinWaysBack |= rotation.latches.size() > 1;
            stepCountersOnce(rotation);
        }
        RegionCloner cloner(m_proc);
        for (auto& rotation : m_rotations) {
            for (BasicBlock* block : rotation.testBlocks)
                cloner.addBlock(block);
        }
        cloner.demoteEscapingValues();
        m_proc.resetValueOwners();
        for (auto& rotation : m_rotations)
            rotate(cloner, rotation);
        return cloner.finish();
    }

    bool didJoinWaysBack() const { return m_didJoinWaysBack; }

private:
    struct EdgesBack {
        BasicBlock* top { nullptr };
        Vector<BasicBlock*, 2> likely;
        Vector<BasicBlock*, 2> unlikelyWithoutPoll;
        bool hasLikelyWithoutPoll { false };
    };

    struct DecidedTest {
        BasicBlock* block;
        unsigned successorIndex;
    };

    struct Rotation {
        unsigned depth { 0 };
        Vector<BasicBlock*, 4> testBlocks;
        Vector<DecidedTest, 1> testsDecidedByLatches;
        Vector<BasicBlock*, 2> latches;
        BasicBlock* body { nullptr };
        std::optional<InductionRecord> induction;
        Vector<const EdgesBack*, 2> cyclesThroughGuardWithoutPoll;
        bool guardKeepsPoll { false };
    };

    bool leadsBackToHeaderOnly(NaturalLoops& loops, const NaturalLoop& loop, BasicBlock* start, BasicBlock* body)
    {
        Vector<BasicBlock*, 8> region { start };
        for (unsigned i = 0; i < region.size(); ++i) {
            if (region[i] == body || loops.innerMostLoopOf(region[i]) != &loop)
                return false;
            for (BasicBlock* successor : region[i]->successorBlocks()) {
                if (successor == loop.header() || !loops.belongsTo(successor, loop) || region.contains(successor))
                    continue;
                if (region.size() == m_proc.limit(Limit::MaxRareBlocksPerRotatedLoop)) {
                    m_proc.didReachLimit(Limit::MaxRareBlocksPerRotatedLoop, start->at(0)->origin());
                    return false;
                }
                region.append(successor);
            }
        }
        for (unsigned i = 1; i < region.size(); ++i) {
            for (BasicBlock* predecessor : region[i]->predecessors()) {
                if (!region.contains(predecessor))
                    return false;
            }
        }
        return start->numPredecessors() == 1;
    }

    bool isBottomTested(NaturalLoops& loops, const NaturalLoop& loop, BasicBlock* latch)
    {
        BasicBlock* block = latch;
        for (uint64_t i = 0; i < m_proc.limit(Limit::MaxRareBlocksPerRotatedLoop); ++i) {
            if (block->last()->opcode() == Branch)
                return !loops.belongsTo(block->successorBlock(0), loop) || !loops.belongsTo(block->successorBlock(1), loop);
            if (block->last()->opcode() != Jump || block->numPredecessors() != 1 || block == loop.header())
                return false;
            for (Value* value : *block) {
                if (emitsInstructions(value))
                    return false;
            }
            block = block->predecessor(0);
        }
        m_proc.didReachLimit(Limit::MaxRareBlocksPerRotatedLoop, latch->at(0)->origin());
        return false;
    }

    static bool hasEdgeThatIsNotRareTo(BasicBlock* from, BasicBlock* to)
    {
        for (const FrequentedBlock& successor : from->successors()) {
            if (successor.block() == to && !successor.isRare())
                return true;
        }
        return false;
    }

    static IndexSet<BasicBlock*> findBlocksNotBehindRareEdges(NaturalLoops& loops, const NaturalLoop& loop)
    {
        IndexSet<BasicBlock*> result;
        Vector<BasicBlock*, 16> worklist { loop.header() };
        result.add(loop.header());
        while (!worklist.isEmpty()) {
            BasicBlock* block = worklist.takeLast();
            for (const FrequentedBlock& successor : block->successors()) {
                if (successor.isRare() || !loops.belongsTo(successor.block(), loop))
                    continue;
                if (result.add(successor.block()))
                    worklist.append(successor.block());
            }
        }
        return result;
    }

    std::optional<InductionRecord> findInduction(BasicBlock* header) const
    {
        for (Value* value : *header) {
            if (value->opcode() != Phi)
                continue;
            if (std::optional<InductionRecord> record = m_proc.knownInduction(value))
                return record;
        }
        return std::nullopt;
    }

    static std::optional<int64_t> valueLeftBy(BasicBlock* latch, BasicBlock* header, Value* value, unsigned depth)
    {
        if (value->hasInt())
            return value->asInt();
        if (value->opcode() == Phi) {
            if (!header->values().contains(value))
                return std::nullopt;
            for (unsigned index = latch->size(); index--;) {
                auto* upsilon = latch->at(index)->as<UpsilonValue>();
                if (upsilon && upsilon->phi() == value)
                    return upsilon->child(0)->hasInt() ? std::optional<int64_t>(upsilon->child(0)->asInt()) : std::nullopt;
            }
            return std::nullopt;
        }
        if (!depth || !value->numChildren() || value->numChildren() > 2 || !value->child(0)->type().isInt())
            return std::nullopt;
        std::optional<int64_t> left = valueLeftBy(latch, header, value->child(0), depth - 1);
        std::optional<int64_t> right = value->numChildren() == 2 ? valueLeftBy(latch, header, value->child(1), depth - 1) : std::optional<int64_t>(0);
        if (!left || !right)
            return std::nullopt;
        bool is32Bit = value->child(0)->type() == Int32;
        uint64_t unsignedLeft = is32Bit ? static_cast<uint32_t>(*left) : static_cast<uint64_t>(*left);
        uint64_t unsignedRight = is32Bit ? static_cast<uint32_t>(*right) : static_cast<uint64_t>(*right);
        switch (value->opcode()) {
        case BitAnd:
            return *left & *right;
        case BitOr:
            return *left | *right;
        case BitXor:
            return *left ^ *right;
        case Equal:
            return *left == *right;
        case NotEqual:
            return *left != *right;
        case LessThan:
            return *left < *right;
        case GreaterThan:
            return *left > *right;
        case LessEqual:
            return *left <= *right;
        case GreaterEqual:
            return *left >= *right;
        case Below:
            return unsignedLeft < unsignedRight;
        case Above:
            return unsignedLeft > unsignedRight;
        case BelowEqual:
            return unsignedLeft <= unsignedRight;
        case AboveEqual:
            return unsignedLeft >= unsignedRight;
        default:
            return std::nullopt;
        }
    }

    void addTestsDecidedByLatches(NaturalLoops& loops, const NaturalLoop& loop, Rotation& rotation, unsigned numberOfCopiedValues)
    {
        while (true) {
            BasicBlock* block = rotation.body;
            if (block->last()->opcode() != Branch || block->numPredecessors() != 1 || loops.innerMostLoopOf(block) != &loop || rotation.latches.contains(block))
                return;
            std::optional<unsigned> successorIndex;
            for (BasicBlock* latch : rotation.latches) {
                std::optional<int64_t> condition = valueLeftBy(latch, loop.header(), block->last()->child(0), 3);
                if (!condition || (successorIndex && *successorIndex != !*condition))
                    return;
                successorIndex = !*condition;
            }
            const FrequentedBlock& staying = block->successor(*successorIndex);
            const FrequentedBlock& other = block->successor(1 - *successorIndex);
            if (staying.isRare() || staying.block() == loop.header() || !loops.belongsTo(staying.block(), loop) || loops.headerOf(staying.block()))
                return;
            if (loops.belongsTo(other.block(), loop) && !(other.isRare() && leadsBackToHeaderOnly(loops, loop, other.block(), staying.block())))
                return;
            for (Value* value : *block) {
                if (!canCloneForLoop(value))
                    return;
                numberOfCopiedValues += emitsInstructions(value);
            }
            if (numberOfCopiedValues > m_proc.limit(Limit::MaxCopiedValuesPerRotatedLoop))
                return;
            rotation.testBlocks.append(block);
            rotation.testsDecidedByLatches.append({ block, *successorIndex });
            rotation.body = staying.block();
        }
    }

    void considerLoop(NaturalLoops& loops, const NaturalLoop& loop)
    {
        Rotation rotation;
        const EdgesBack& edgesBack = edgesBackOf(loops, loop);
        rotation.latches = edgesBack.likely;
        if (rotation.latches.isEmpty())
            return;
        // A cycle that leads through the guard and has no poll of its own polls on its unlikely edges back, so what enters from outside does not. Where a likely edge back has none, the guard polls.
        if (!edgesBack.unlikelyWithoutPoll.isEmpty())
            rotation.cyclesThroughGuardWithoutPoll.append(&edgesBack);
        for (const NaturalLoop* outer = loops.innerMostOuterLoop(loop); outer && !hasPoll(outer->header()); outer = loops.innerMostOuterLoop(*outer)) {
            const EdgesBack& edgesBackOfOuter = edgesBackOf(loops, *outer);
            if (edgesBackOfOuter.hasLikelyWithoutPoll) {
                rotation.guardKeepsPoll = true;
                break;
            }
            if (edgesBackOfOuter.unlikelyWithoutPoll.isEmpty())
                break;
            rotation.cyclesThroughGuardWithoutPoll.append(&edgesBackOfOuter);
        }
        if (m_allowedHeaders) {
            for (BasicBlock* latch : rotation.latches) {
                if (isBottomTested(loops, loop, latch))
                    return;
            }
        }
        for (const NaturalLoop* outer = loops.innerMostOuterLoop(loop); outer; outer = loops.innerMostOuterLoop(*outer))
            ++rotation.depth;

        unsigned numberOfCopiedValues = 0;
        for (BasicBlock* block = loop.header();;) {
            if (block != loop.header() && (block->numPredecessors() != 1 || loops.headerOf(block)))
                return;
            if (loops.innerMostLoopOf(block) != &loop)
                return;
            for (Value* value : *block) {
                if (!canCloneForLoop(value))
                    return;
                numberOfCopiedValues += emitsInstructions(value);
            }
            if (numberOfCopiedValues > m_proc.limit(Limit::MaxCopiedValuesPerRotatedLoop)) {
                m_proc.didReachLimit(Limit::MaxCopiedValuesPerRotatedLoop, loop.header()->at(0)->origin());
                return;
            }
            rotation.testBlocks.append(block);

            BasicBlock* next = nullptr;
            if (block->last()->opcode() == Jump)
                next = block->successorBlock(0);
            else if (block->last()->opcode() == Branch) {
                for (unsigned i = 0; i < 2; ++i) {
                    const FrequentedBlock& staying = block->successor(i);
                    const FrequentedBlock& other = block->successor(1 - i);
                    if (!loops.belongsTo(staying.block(), loop) || staying.isRare())
                        continue;
                    bool otherIsRare = other.isRare();
                    bool otherLeaves = !loops.belongsTo(other.block(), loop);
                    if (otherLeaves && otherIsRare) {
                        next = staying.block();
                        break;
                    }
                    if (!otherLeaves && !(otherIsRare && leadsBackToHeaderOnly(loops, loop, other.block(), staying.block())))
                        continue;
                    if (staying.block() == loop.header() || loops.headerOf(staying.block()) || rotation.latches.contains(block))
                        return;
                    rotation.body = staying.block();
                    addTestsDecidedByLatches(loops, loop, rotation, numberOfCopiedValues);
                    rotation.induction = findInduction(loop.header());
                    m_rotations.append(WTF::move(rotation));
                    return;
                }
            }
            if (!next || next == loop.header() || !loops.belongsTo(next, loop))
                return;
            block = next;
        }
    }

    // `i++; continue;` and the `i++` at the end of the turn are two sums. Where every way back hands the header the same sum of what the header had and a constant, the sum is made once, at the top of the body:
    // the one test at the bottom then compares a sum and not a join of sums, which is how a counted loop is known.
    void stepCountersOnce(const Rotation& rotation)
    {
        if (rotation.latches.size() < 2)
            return;
        BasicBlock* header = rotation.testBlocks[0];
        InsertionSet insertionSet(m_proc);
        size_t index = 0;
        while (index < rotation.body->size() && rotation.body->at(index)->opcode() == Phi)
            ++index;
        for (Value* phi : *header) {
            if (phi->opcode() != Phi || !phi->type().isInt())
                continue;
            Vector<Value*, 2> sums;
            std::optional<int64_t> addend;
            bool isOneSum = true;
            for (BasicBlock* latch : rotation.latches) {
                Value* handed = nullptr;
                for (Value* value : *latch) {
                    auto* upsilon = value->as<UpsilonValue>();
                    if (upsilon && upsilon->phi() == phi)
                        handed = upsilon->child(0)->foldIdentity();
                }
                if (!handed || handed->opcode() != Add || handed->child(0)->foldIdentity() != phi || !handed->child(1)->hasInt() || (addend && *addend != handed->child(1)->asInt())) {
                    isOneSum = false;
                    break;
                }
                addend = handed->child(1)->asInt();
                if (!sums.contains(handed))
                    sums.append(handed);
            }
            if (!isOneSum || sums.size() < 2)
                continue;
            Value* sum = insertionSet.insert<Value>(index, Add, sums[0]->origin(), phi, insertionSet.insertIntConstant(index, sums[0]->origin(), phi->type(), *addend));
            for (Value* old : sums)
                old->replaceWithIdentity(sum);
        }
        insertionSet.execute(rotation.body);
    }

    void rotate(RegionCloner& cloner, const Rotation& rotation)
    {
        RegionCloner::BlockMap copies = cloner.cloneBlocks(rotation.testBlocks.span());
        if (rotation.induction)
            transferInduction(rotation, copies);
        for (BasicBlock* latch : rotation.latches)
            latch->replaceSuccessor(rotation.testBlocks[0], copies.get(rotation.testBlocks[0]));
        for (const DecidedTest& test : rotation.testsDecidedByLatches) {
            BasicBlock* copy = copies.get(test.block);
            copy->last()->replaceWithJump(copy, copy->successor(test.successorIndex));
        }
        if (!rotation.guardKeepsPoll) {
            for (const EdgesBack* edgesBack : rotation.cyclesThroughGuardWithoutPoll)
                pollOnWayBack(*edgesBack, rotation.testBlocks.span());
            removePolls(rotation.testBlocks.span());
        }

        BasicBlock* preHeader = m_proc.addBlock();
        preHeader->appendNew<Value>(m_proc, Jump, rotation.body->at(0)->origin());
        preHeader->setSuccessors(FrequentedBlock(rotation.body));
        rotation.testBlocks.last()->replaceSuccessor(rotation.body, preHeader);

        m_proc.addRemark({ RemarkKind::LoopRotated, GaveUpReason::None, rotation.testBlocks[0]->at(0)->origin(), std::nullopt });
    }

    static bool hasPoll(BasicBlock* block)
    {
        for (Value* value : *block) {
            if (isLoopPoll(value))
                return true;
        }
        return false;
    }

    // Whatever comes to the block has come through the blocks above it that have one way in.
    static bool pollsOnWayTo(NaturalLoops& loops, const NaturalLoop& loop, BasicBlock* block)
    {
        for (; block != loop.header() && loops.innerMostLoopOf(block) == &loop; block = block->predecessor(0)) {
            if (hasPoll(block))
                return true;
            if (block->numPredecessors() != 1)
                return false;
        }
        return false;
    }

    const EdgesBack& edgesBackOf(NaturalLoops& loops, const NaturalLoop& loop)
    {
        std::optional<EdgesBack>& result = m_edgesBack[loop.index()];
        if (result)
            return *result;
        result.emplace();
        result->top = loop.header();
        // An arm that is merely the less likely one, as the arm of a `continue`, is taken every few turns: it is tested at its end as the likely way is, and stays in the loop. A rare one goes round by the guard.
        IndexSet<BasicBlock*> blocksOfTurns = findBlocksNotBehindRareEdges(loops, loop);
        for (BasicBlock* predecessor : loop.header()->predecessors()) {
            if (blocksOfTurns.contains(predecessor) && hasEdgeThatIsNotRareTo(predecessor, loop.header())) {
                result->likely.append(predecessor);
                result->hasLikelyWithoutPoll |= !pollsOnWayTo(loops, loop, predecessor);
            } else if (loops.belongsTo(predecessor, loop) && !hasPoll(predecessor))
                result->unlikelyWithoutPoll.append(predecessor);
        }
        return *result;
    }

    void pollOnWayBack(const EdgesBack& edgesBack, std::span<BasicBlock* const> guard)
    {
        if (m_topsWithPollOnWayBack.contains(edgesBack.top))
            return;
        BasicBlock* wayBack = nullptr;
        for (BasicBlock* block : guard) {
            for (Value* value : *block) {
                if (!isLoopPoll(value))
                    continue;
                if (!wayBack)
                    wayBack = m_proc.addBlock();
                wayBack->append(m_proc.clone(value));
            }
        }
        if (!wayBack)
            return;
        m_topsWithPollOnWayBack.add(edgesBack.top);
        wayBack->appendNew<Value>(m_proc, Jump, wayBack->at(0)->origin());
        wayBack->setSuccessors(FrequentedBlock(edgesBack.top));
        for (BasicBlock* from : edgesBack.unlikelyWithoutPoll)
            from->replaceSuccessor(edgesBack.top, wayBack);
    }

    static void removePolls(std::span<BasicBlock* const> blocks)
    {
        for (BasicBlock* block : blocks) {
            for (Value* value : *block) {
                if (isLoopPoll(value))
                    value->replaceWithNop();
            }
        }
    }

    void transferInduction(const Rotation& rotation, const RegionCloner::BlockMap& copies)
    {
        const InductionRecord& record = *rotation.induction;
        Variable* variable = nullptr;
        for (Value* value : *record.phi->owner) {
            if (value->opcode() == Set && value->child(0) == record.phi)
                variable = value->as<VariableValue>()->variable();
        }
        if (!variable)
            return;
        auto copyOf = [&](Value* value) -> Value* {
            BasicBlock* copy = copies.get(value->owner);
            if (!copy)
                return value;
            size_t index = value->owner->values().find(value);
            RELEASE_ASSERT(index != notFound);
            return copy->at(index);
        };
        DemotedInduction demoted;
        demoted.variable = variable;
        demoted.header = rotation.body;
        demoted.next = copyOf(record.next);
        demoted.test = record.test ? copyOf(record.test) : nullptr;
        demoted.step = record.step;
        demoted.doesNotWrap = record.doesNotWrap;
        m_proc.setKnownInduction(demoted);
    }

    Procedure& m_proc;
    bool m_didJoinWaysBack { false };
    const Vector<BasicBlock*>* m_allowedHeaders;
    Vector<Rotation> m_rotations;
    Vector<std::optional<EdgesBack>> m_edgesBack;
    IndexSet<BasicBlock*> m_topsWithPollOnWayBack;
};

} // anonymous namespace

bool rotateLoops(Procedure& proc, const Vector<BasicBlock*>* allowedHeaders, bool* didJoinWaysBack)
{
    PhaseScope phaseScope(proc, "rotateLoops"_s);
    RotateLoops rotateLoops(proc, allowedHeaders);
    bool changed = rotateLoops.run();
    if (didJoinWaysBack)
        *didJoinWaysBack = rotateLoops.didJoinWaysBack();
    return changed;
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
