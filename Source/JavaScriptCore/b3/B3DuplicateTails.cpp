/*
 * Copyright (C) 2016-2019 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. 
 */

#include "config.h"
#include "B3DuplicateTails.h"

#if ENABLE(B3_JIT)

#include "B3BasicBlock.h"
#include "B3BreakCriticalEdges.h"
#include "B3Dominators.h"
#include "B3FixSSA.h"
#include "B3InsertionSet.h"
#include "B3PhaseScope.h"
#include "B3Procedure.h"
#include "B3UpsilonValue.h"
#include "B3ValueInlines.h"
#include <wtf/IndexMap.h>
#include <wtf/IndexSet.h>

namespace JSC { namespace B3 {

namespace {

namespace B3DuplicateTailsInternal {
static constexpr bool verbose = false;
}

class DuplicateTails {
public:
    DuplicateTails(Procedure& proc, bool onlyDecidedTests)
        : m_proc(proc)
        , m_insertionSet(proc)
#if USE(BUN_JSC_ADDITIONS)
        , m_maxSize(proc.positionIndependent() ? 1 : Options::maxB3TailDupBlockSize())
#else
        , m_maxSize(Options::maxB3TailDupBlockSize())
#endif
        , m_maxSuccessors(Options::maxB3TailDupBlockSuccessors())
        , m_onlyDecidedTests(onlyDecidedTests)
    {
    }

#if USE(BUN_JSC_ADDITIONS)
    static constexpr double maxRatioOfTailToLikelyPredecessor = 16;
    static constexpr unsigned maxCostOfCopyWithDecidedTest = 8;
    static constexpr unsigned maxPredecessorsToAskDominatorsFor = 8;
    static constexpr unsigned maxDominatorsToAsk = 32;
    static constexpr unsigned maxDepthOfTest = 4;
    static constexpr double leastFrequency = 1e-6;
    static constexpr uint64_t maxValuesToDemoteTimesBlocks = 1 << 24;

    enum class Outcome : uint8_t { Unknown, NotTaken, Taken };
#endif

    void run()
    {
        // Breaking critical edges introduces blocks that jump to things. Those Jumps' successors
        // become candidates for tail duplication. Prior to critical edge breaking, some of those
        // Jumps would have been Branches, and so no tail duplication would have happened.
        breakCriticalEdges(m_proc);
        
        // Find blocks that would be candidates for tail duplication. They must be small enough
        // and they much not have too many successors.

        m_proc.resetValueOwners();

        IndexSet<BasicBlock*> candidates;

        for (BasicBlock* block : m_proc) {
            if (block->size() > m_maxSize)
                continue;
            if (block->numSuccessors() > m_maxSuccessors)
                continue;
            if (block->last()->type() != Void) // Demoting doesn't handle terminals with values.
                continue;

            bool canCopyBlock = true;
            for (Value* value : *block) {
                if (value->kind().isCloningForbidden() || (m_proc.positionIndependent() && (value->opcode() == Patchpoint || value->opcode() == CCall))) {
                    canCopyBlock = false;
                    break;
                }
            }

#if USE(BUN_JSC_ADDITIONS)
            // Of the blocks that lead here one comes right in front of it in the code. Unless another is likely too, or something here depends on where control came from, a copy saves nothing.
            if (canCopyBlock && m_proc.positionIndependent()) {
                unsigned numberOfLikelyPredecessors = 0;
                for (BasicBlock* predecessor : block->predecessors())
                    numberOfLikelyPredecessors += predecessor->frequency() * maxRatioOfTailToLikelyPredecessor >= block->frequency();
                bool hasPhi = false;
                for (Value* value : *block)
                    hasPhi |= value->opcode() == Phi;
                canCopyBlock = numberOfLikelyPredecessors >= 2 || hasPhi;
            }
#endif
            if (canCopyBlock && !m_onlyDecidedTests)
                candidates.add(block);
        }

#if USE(BUN_JSC_ADDITIONS)
        IndexMap<BasicBlock*, Outcome> outcomeOfTestBehind(m_proc.size(), Outcome::Unknown);
        IndexSet<BasicBlock*> tailsOnlyForDecidedTests;
        if (m_proc.positionIndependent()) {
            Vector<Value*, 8> test;
            for (BasicBlock* block : m_proc) {
                if (block->last()->opcode() != Branch || block->numPredecessors() < 2 || block->successorBlock(0) == block->successorBlock(1))
                    continue;
                constexpr unsigned tooMuch = std::numeric_limits<unsigned>::max() / 2;
                unsigned cost = 0;
                for (Value* value : *block) {
                    if (value->kind().isCloningForbidden() || value->opcode() == Patchpoint || value->opcode() == CCall) {
                        cost = tooMuch;
                        break;
                    }
                    cost += !value->isConstant() && value->opcode() != Phi && value->opcode() != Nop && value != block->last();
                }
                if (cost >= tooMuch)
                    continue;
                test.shrink(0);
                auto addToTest = [&](Value* value) {
                    if (value->owner == block && !value->isConstant() && value->opcode() != Phi && !test.contains(value))
                        test.append(value);
                };
                addToTest(block->last()->child(0));
                for (unsigned i = 0; i < test.size() && test.size() <= cost; ++i) {
                    for (Value* child : test[i]->children())
                        addToTest(child);
                }
                cost -= std::min<unsigned>(cost, test.size());
                if (cost > maxCostOfCopyWithDecidedTest)
                    continue;
                bool isKnownNotToBeLoopHeader = false;
                for (BasicBlock* predecessor : block->predecessors()) {
                    if (predecessor->last()->opcode() != Jump || predecessor == block)
                        continue;
                    if (cost && predecessor->frequency() * maxRatioOfTailToLikelyPredecessor < block->frequency())
                        continue;
                    if (!m_dominators)
                        m_dominators = &m_proc.dominators();
                    if (!isKnownNotToBeLoopHeader) {
                        bool isLoopHeader = false;
                        for (BasicBlock* other : block->predecessors())
                            isLoopHeader |= m_dominators->dominates(block, other);
                        if (isLoopHeader)
                            break;
                        isKnownNotToBeLoopHeader = true;
                    }
                    std::optional<bool> outcome = outcomeOfTestOnEdge(block, predecessor, block->numPredecessors() <= maxPredecessorsToAskDominatorsFor);
                    if (!outcome)
                        continue;
                    outcomeOfTestBehind[predecessor] = *outcome ? Outcome::Taken : Outcome::NotTaken;
                    if (candidates.add(block))
                        tailsOnlyForDecidedTests.add(block);
                }
            }
        }
#endif
        if (m_onlyDecidedTests && candidates.isEmpty())
            return;
#if USE(BUN_JSC_ADDITIONS)
        if (candidates.isEmpty()) {
            m_proc.resetReachability();
            m_proc.invalidateCFG();
            return;
        }
#endif

        // Collect the set of values that must be de-SSA'd.
        IndexSet<Value*> valuesToDemote;
        for (BasicBlock* block : m_proc) {
            for (Value* value : *block) {
                if (value->opcode() == Phi && candidates.contains(block))
                    valuesToDemote.add(value);
                for (Value* child : value->children()) {
                    if (child->owner != block && candidates.contains(child->owner))
                        valuesToDemote.add(child);
                }
            }
        }
#if USE(BUN_JSC_ADDITIONS)
        if (m_onlyDecidedTests && static_cast<uint64_t>(valuesToDemote.size()) * m_proc.size() > maxValuesToDemoteTimesBlocks)
            return;
#endif
        demoteValues(m_proc, valuesToDemote);
        if (B3DuplicateTailsInternal::verbose) {
            dataLog("Procedure after value demotion:\n");
            dataLog(m_proc);
        }

        Vector<BasicBlock*> blocksToVisit;
        for (BasicBlock* block : m_proc)
            blocksToVisit.append(block);
        for (unsigned indexOfBlock = 0; indexOfBlock < blocksToVisit.size(); ++indexOfBlock) {
            BasicBlock* block = blocksToVisit[indexOfBlock];
            if (block->last()->opcode() != Jump)
                continue;

            BasicBlock* tail = block->successorBlock(0);
            if (!candidates.contains(tail))
                continue;

            // Don't tail duplicate a trivial self-loop, because the code below can't handle block and
            // tail being the same block.
            if (block == tail)
                continue;

#if USE(BUN_JSC_ADDITIONS)
            Outcome outcome = outcomeOfTestBehind[block];
            if (outcome == Outcome::Unknown && tailsOnlyForDecidedTests.contains(tail))
                continue;
            if (outcome == Outcome::Unknown && m_proc.positionIndependent() && block->frequency() * maxRatioOfTailToLikelyPredecessor < tail->frequency())
                continue;
#endif

            // We're about to change 'block'. Make sure that nobody duplicates block after this
            // point.
            candidates.remove(block);

            if (B3DuplicateTailsInternal::verbose)
                dataLog("Duplicating ", *tail, " into ", *block, "\n");

            block->removeLast(m_proc);

            UncheckedKeyHashMap<Value*, Value*> map;
            for (Value* value : *tail) {
                Value* clone = m_proc.clone(value);
                for (Value*& child : clone->children()) {
                    if (Value* replacement = map.get(child))
                        child = replacement;
                }
                if (value->type() != Void)
                    map.add(value, clone);
                block->append(clone);
            }
            block->successors() = tail->successors();
#if USE(BUN_JSC_ADDITIONS)
            if (outcome != Outcome::Unknown) {
                block->last()->replaceWithJump(block, outcome == Outcome::Taken ? block->taken() : block->notTaken());
                tail->setFrequency(std::max(tail->frequency() - block->frequency(), leastFrequency));
                outcomeOfTestBehind[block] = Outcome::Unknown;
            } else if (outcomeOfTestBehind[tail] != Outcome::Unknown) {
                outcomeOfTestBehind[block] = outcomeOfTestBehind[tail];
                blocksToVisit.append(block);
            }
#endif
            m_proc.didDuplicateTail();
        }

        m_proc.resetReachability();
        m_proc.invalidateCFG();
    }
    
private:
#if USE(BUN_JSC_ADDITIONS)
    Value* onEdge(Value* value)
    {
        if (value->opcode() == Phi && value->owner == m_tail)
            return m_valuesOfPhisOnEdge.get(value);
        return value;
    }

    std::pair<Value*, bool> withoutNegations(Value* value, bool isSeenFromEdge)
    {
        bool isNegated = false;
        for (;;) {
            if (isSeenFromEdge)
                value = onEdge(value);
            if (!value)
                break;
            if ((value->opcode() == Equal || value->opcode() == NotEqual) && value->child(1)->isInt(0)) {
                isNegated ^= value->opcode() == Equal;
                value = value->child(0);
                continue;
            }
            if (value->opcode() == BitXor && value->child(1)->isInt(1) && value->child(0)->returnsBool()) {
                isNegated ^= true;
                value = value->child(0);
                continue;
            }
            break;
        }
        return { value, isNegated };
    }

    static bool isPureIntegerOperation(Opcode opcode)
    {
        switch (opcode) {
        case Add:
        case Sub:
        case BitAnd:
        case BitOr:
        case BitXor:
        case Shl:
        case SShr:
        case ZShr:
        case SExt8:
        case SExt16:
        case SExt32:
        case ZExt32:
        case Trunc:
        case Equal:
        case NotEqual:
        case LessThan:
        case GreaterThan:
        case LessEqual:
        case GreaterEqual:
        case Above:
        case Below:
        case AboveEqual:
        case BelowEqual:
            return true;
        default:
            return false;
        }
    }

    bool childrenAreSameOnEdge(Value* seenFromEdge, Value* other, unsigned depth)
    {
        if (!depth || seenFromEdge->numChildren() != other->numChildren())
            return false;
        for (unsigned i = 0; i < seenFromEdge->numChildren(); ++i) {
            if (!isSameOnEdge(seenFromEdge->child(i), other->child(i), depth - 1))
                return false;
        }
        return true;
    }

    bool isSameOnEdge(Value* seenFromEdge, Value* other, unsigned depth)
    {
        seenFromEdge = onEdge(seenFromEdge);
        if (!seenFromEdge)
            return false;
        if (seenFromEdge == other)
            return true;
        if (seenFromEdge->type() != other->type())
            return false;
        if (seenFromEdge->hasInt() || other->hasInt())
            return seenFromEdge->hasInt() && other->hasInt() && seenFromEdge->asInt() == other->asInt();
        if (seenFromEdge->kind() != other->kind() || !isPureIntegerOperation(seenFromEdge->opcode()))
            return false;
        return childrenAreSameOnEdge(seenFromEdge, other, depth);
    }

    std::optional<int64_t> constantOnEdge(Value* value, unsigned depth)
    {
        value = onEdge(value);
        if (!value)
            return std::nullopt;
        if (value->hasInt())
            return value->asInt();
        if (!depth || value->owner != m_tail || value->numChildren() != 2)
            return std::nullopt;
        std::optional<int64_t> left = constantOnEdge(value->child(0), depth - 1);
        if (!left)
            return std::nullopt;
        std::optional<int64_t> right = constantOnEdge(value->child(1), depth - 1);
        if (!right)
            return std::nullopt;
        auto asUnsigned = [&](int64_t operand) -> uint64_t {
            return value->child(0)->type() == Int32 ? static_cast<uint32_t>(operand) : static_cast<uint64_t>(operand);
        };
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
        case Above:
            return asUnsigned(*left) > asUnsigned(*right);
        case Below:
            return asUnsigned(*left) < asUnsigned(*right);
        case AboveEqual:
            return asUnsigned(*left) >= asUnsigned(*right);
        case BelowEqual:
            return asUnsigned(*left) <= asUnsigned(*right);
        default:
            return std::nullopt;
        }
    }

    std::optional<bool> outcomeOfTestOnEdge(BasicBlock* tail, BasicBlock* predecessor, bool mayAskDominators)
    {
        m_tail = tail;
        m_valuesOfPhisOnEdge.clear();
        for (Value* value : *predecessor) {
            if (auto* upsilon = value->as<UpsilonValue>(); upsilon && upsilon->phi()->owner == tail)
                m_valuesOfPhisOnEdge.set(upsilon->phi(), upsilon->child(0));
        }
        Value* test = tail->last()->child(0);
        if (std::optional<int64_t> constant = constantOnEdge(test, maxDepthOfTest))
            return !!*constant;
        if (!mayAskDominators)
            return std::nullopt;
        auto [strippedTest, testIsNegated] = withoutNegations(test, true);
        if (!strippedTest)
            return std::nullopt;
        unsigned numberOfDominatorsAsked = 0;
        for (BasicBlock* block = predecessor; block && numberOfDominatorsAsked < maxDominatorsToAsk; block = m_dominators->idom(block), ++numberOfDominatorsAsked) {
            if (block->numPredecessors() != 1)
                continue;
            BasicBlock* from = block->predecessor(0);
            if (from->last()->opcode() != Branch || from->successorBlock(0) == from->successorBlock(1))
                continue;
            auto [condition, conditionIsNegated] = withoutNegations(from->last()->child(0), false);
            bool conditionHolds = (from->successorBlock(0) == block) != conditionIsNegated;
            if (isSameOnEdge(strippedTest, condition, maxDepthOfTest))
                return conditionHolds != testIsNegated;
            bool areOpposites = (strippedTest->opcode() == Equal && condition->opcode() == NotEqual) || (strippedTest->opcode() == NotEqual && condition->opcode() == Equal);
            if (areOpposites && strippedTest->child(0)->type() == condition->child(0)->type() && childrenAreSameOnEdge(strippedTest, condition, maxDepthOfTest))
                return conditionHolds == testIsNegated;
        }
        return std::nullopt;
    }

    Dominators* m_dominators { nullptr };
    BasicBlock* m_tail { nullptr };
    UncheckedKeyHashMap<Value*, Value*> m_valuesOfPhisOnEdge;
#endif

    Procedure& m_proc;
    InsertionSet m_insertionSet;
    unsigned m_maxSize;
    unsigned m_maxSuccessors;
    bool m_onlyDecidedTests;
};

} // anonymous namespace

void duplicateTails(Procedure& proc, bool onlyDecidedTests)
{
    PhaseScope phaseScope(proc, "duplicateTails"_s);
    DuplicateTails duplicateTails(proc, onlyDecidedTests);
    duplicateTails.run();
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)

