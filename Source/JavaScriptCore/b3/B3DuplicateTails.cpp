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

#include "AOTConvention.h"
#include "B3ArgumentRegValue.h"
#include "B3BasicBlock.h"
#include "B3BreakCriticalEdges.h"
#include "B3FixSSA.h"
#include "B3InsertionSet.h"
#include "B3PhaseScope.h"
#include "B3Procedure.h"
#include "B3UpsilonValue.h"
#include "B3ValueInlines.h"
#include "B3VariableValue.h"
#include "GPRInfo.h"
#include "JSCJSValue.h"
#include <wtf/IndexMap.h>
#include <wtf/IndexSet.h>

namespace JSC { namespace B3 {

namespace {

namespace B3DuplicateTailsInternal {
static constexpr bool verbose = false;
}

class DuplicateTails {
public:
    DuplicateTails(Procedure& proc)
        : m_proc(proc)
        , m_insertionSet(proc)
#if USE(BUN_JSC_ADDITIONS)
        , m_maxSize(proc.positionIndependent() ? 1 : Options::maxB3TailDupBlockSize())
#else
        , m_maxSize(Options::maxB3TailDupBlockSize())
#endif
        , m_maxSuccessors(Options::maxB3TailDupBlockSuccessors())
    {
    }

#if USE(BUN_JSC_ADDITIONS)
    static constexpr double maxRatioOfTailToLikelyPredecessor = 16;
    static constexpr unsigned maxDecidedTestCopyCost = 8;
    static constexpr unsigned maxPathCost = 16;
    static constexpr unsigned maxPredecessorsForFactQueries = 8;
    static constexpr unsigned maxFactQueries = 32;
    static constexpr unsigned maxTestDepth = 8;
    static constexpr unsigned maxBlocksOnPath = 8;
    static constexpr double minFrequency = 1e-6;
#endif

    enum class Outcome : uint8_t { Unknown, NotTaken, Taken, Jumps };

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
            unsigned size = block->size();
#if USE(BUN_JSC_ADDITIONS)
            if (m_proc.positionIndependent() && size > m_maxSize && block->last()->opcode() == Branch && block->last()->child(0)->opcode() == Phi && block->last()->child(0)->owner == block) {
                bool isHeader = isLoopHeader(block);
                for (Value* value : *block)
                    size -= !isHeader && value->opcode() == Phi;
            }
#endif
            if (size > m_maxSize)
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
            if (canCopyBlock)
                candidates.add(block);
        }

#if USE(BUN_JSC_ADDITIONS)
        IndexSet<BasicBlock*> tailsOnlyForDecidedTests;
        if (m_proc.positionIndependent()) {
            m_pathAfterIndex.resize(m_proc.size());
            m_pathCopyCost.resize(m_proc.size());
            for (BasicBlock* block : m_proc) {
                if (block->numPredecessors() < 2 || !pathCopyCost(block))
                    continue;
                bool isKnownNotToBeLoopHeader = false;
                for (BasicBlock* predecessor : block->predecessors()) {
                    if (predecessor->last()->opcode() != Jump || predecessor == block)
                        continue;
                    if (!isKnownNotToBeLoopHeader) {
                        if (isLoopHeader(block))
                            break;
                        isKnownNotToBeLoopHeader = true;
                    }
                    Path* path = followDecidedTests(predecessor, block, [&](BasicBlock* tail) {
                        return candidates.contains(tail) && !tailsOnlyForDecidedTests.contains(tail);
                    });
                    if (!path)
                        continue;
                    for (std::span<const Step> steps : { path->steps.span(), path->stepsAfterUndecidedTest[0].span(), path->stepsAfterUndecidedTest[1].span() }) {
                        for (Step step : steps) {
                            if (candidates.add(step.block))
                                tailsOnlyForDecidedTests.add(step.block);
                        }
                    }
                }
            }
        }
#endif
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
#if USE(BUN_JSC_ADDITIONS)
                if (m_proc.positionIndependent()) {
                    auto* upsilon = value->as<UpsilonValue>();
                    if (upsilon && candidates.contains(upsilon->phi()->owner) && (block->last()->opcode() != Jump || block->successorBlock(0) != upsilon->phi()->owner))
                        valuesToDemote.add(upsilon->phi());
                } else
#endif
                if (value->opcode() == Phi && candidates.contains(block))
                    valuesToDemote.add(value);
                for (Value* child : value->children()) {
                    if (child->owner != block && candidates.contains(child->owner))
                        valuesToDemote.add(child);
                }
            }
        }
#if USE(BUN_JSC_ADDITIONS)
        if (!valuesToDemote.isEmpty())
#endif
            demoteValues(m_proc, valuesToDemote);
        if (B3DuplicateTailsInternal::verbose) {
            dataLog("Procedure after value demotion:\n");
            dataLog(m_proc);
        }

        auto copyAfter = [&](BasicBlock* block, BasicBlock* tail, Outcome outcome) {
            // We're about to change 'block'. Make sure that nobody duplicates block after this
            // point.
            candidates.remove(block);

            if (B3DuplicateTailsInternal::verbose)
                dataLog("Duplicating ", *tail, " into ", *block, "\n");

            block->removeLast(m_proc);

#if USE(BUN_JSC_ADDITIONS)
            bool isDecided = outcome == Outcome::Taken || outcome == Outcome::NotTaken;
            if (isDecided) {
                m_isNeededAfterDecidedTest.fill(false, tail->size());
                m_usedAfterDecidedTest.shrink(0);
                for (unsigned index = tail->size() - 1; index--;) {
                    Value* value = tail->at(index);
                    if (!value->effects().mustExecute() && !m_usedAfterDecidedTest.contains(value))
                        continue;
                    m_isNeededAfterDecidedTest[index] = true;
                    for (Value* child : value->children())
                        m_usedAfterDecidedTest.append(child);
                }
            }
            unsigned indexInTail = 0;
#endif
            UncheckedKeyHashMap<Value*, Value*> map;
#if USE(BUN_JSC_ADDITIONS)
            for (Value* value : *block) {
                auto* upsilon = value->as<UpsilonValue>();
                if (!upsilon || upsilon->phi()->owner != tail)
                    continue;
                map.set(upsilon->phi(), upsilon->child(0));
                upsilon->replaceWithNop();
            }
#endif
            for (Value* value : *tail) {
#if USE(BUN_JSC_ADDITIONS)
                if (isDecided && !m_isNeededAfterDecidedTest[indexInTail++])
                    continue;
                if (value->opcode() == Phi)
                    continue;
#endif
                Value* clone = m_proc.clone(value);
                for (Value*& child : clone->children()) {
                    if (Value* replacement = map.get(child))
                        child = replacement;
#if USE(BUN_JSC_ADDITIONS)
                    else
                        RELEASE_ASSERT(child->opcode() != Phi || child->owner != tail);
#endif
                }
                if (value->type() != Void)
                    map.add(value, clone);
                block->append(clone);
            }
#if USE(BUN_JSC_ADDITIONS)
            if (isDecided) {
                block->appendNew<Value>(m_proc, Jump, tail->last()->origin());
                block->setSuccessors(outcome == Outcome::Taken ? tail->taken() : tail->notTaken());
            } else
#endif
                block->successors() = tail->successors();
#if USE(BUN_JSC_ADDITIONS)
            if (outcome != Outcome::Unknown)
                tail->setFrequency(std::max(tail->frequency() - block->frequency(), minFrequency));
#else
            UNUSED_PARAM(outcome);
#endif
        };

#if USE(BUN_JSC_ADDITIONS)
        for (unsigned index = 0, numberOfBlocks = m_proc.size(); index < numberOfBlocks; ++index) {
            BasicBlock* block = m_proc[index];
            Path* path = block ? pathAfter(block) : nullptr;
            if (!path)
                continue;
            bool endsWithUndecidedTest = false;
            for (Step step : path->steps) {
                if (!candidates.contains(step.block) || step.block == block)
                    break;
                copyAfter(block, step.block, step.outcome);
                endsWithUndecidedTest = step.outcome == Outcome::Unknown;
            }
            if (!endsWithUndecidedTest)
                continue;
            for (unsigned arm = 0; arm < 2; ++arm) {
                if (path->stepsAfterUndecidedTest[arm].isEmpty())
                    continue;
                FrequentedBlock& successor = block->successor(arm);
                BasicBlock* behind = m_proc.addBlock(std::min(block->frequency(), successor.block()->frequency()));
                behind->appendNew<Value>(m_proc, Jump, block->last()->origin());
                behind->setSuccessors(FrequentedBlock(successor.block()));
                successor.block() = behind;
                for (Step step : path->stepsAfterUndecidedTest[arm]) {
                    if (!candidates.contains(step.block))
                        break;
                    copyAfter(behind, step.block, step.outcome);
                }
            }
        }
#endif

        for (BasicBlock* block : m_proc) {
            if (block->last()->opcode() != Jump)
                continue;
#if USE(BUN_JSC_ADDITIONS)
            if (pathAfter(block))
                continue;
#endif

            BasicBlock* tail = block->successorBlock(0);
            if (!candidates.contains(tail))
                continue;

            // Don't tail duplicate a trivial self-loop, because the code below can't handle block and
            // tail being the same block.
            if (block == tail)
                continue;

#if USE(BUN_JSC_ADDITIONS)
            if (tailsOnlyForDecidedTests.contains(tail))
                continue;
            if (m_proc.positionIndependent() && block->frequency() * maxRatioOfTailToLikelyPredecessor < tail->frequency())
                continue;
#endif

            copyAfter(block, tail, Outcome::Unknown);
        }

        m_proc.resetReachability();
        m_proc.invalidateCFG();
    }
    
private:
#if USE(BUN_JSC_ADDITIONS)
    struct Step {
        BasicBlock* block;
        Outcome outcome;
    };

    struct Path {
        Vector<Step, 4> steps;
        std::array<Vector<Step, 2>, 2> stepsAfterUndecidedTest;
    };

    Path* pathAfter(BasicBlock* block)
    {
        unsigned index = block->index() < m_pathAfterIndex.size() ? m_pathAfterIndex[block] : 0;
        return index ? &m_paths[index - 1] : nullptr;
    }

    std::optional<unsigned> wholeCopyCost(BasicBlock* block)
    {
        unsigned cost = 0;
        for (Value* value : *block) {
            if (value->kind().isCloningForbidden() || value->opcode() == Patchpoint || value->opcode() == CCall)
                return std::nullopt;
            cost += !value->isConstant() && value->opcode() != Phi && value->opcode() != Nop && value != block->last();
            if (cost > maxDecidedTestCopyCost)
                return std::nullopt;
        }
        return cost;
    }

    std::optional<unsigned> pathCopyCost(BasicBlock* block)
    {
        static constexpr uint8_t isUnknown = 0;
        static constexpr uint8_t cannotBeCopied = 1;
        static constexpr uint8_t zeroCost = 2;
        uint8_t& cached = m_pathCopyCost[block];
        if (cached == isUnknown) {
            std::optional<unsigned> cost;
            if (block->last()->opcode() == Jump)
                cost = wholeCopyCost(block);
            else if (block->last()->opcode() == Branch && block->successorBlock(0) != block->successorBlock(1))
                cost = decidedTestCopyCost(block);
            cached = cost ? zeroCost + *cost : cannotBeCopied;
        }
        if (cached == cannotBeCopied)
            return std::nullopt;
        return cached - zeroCost;
    }

    std::optional<unsigned> decidedTestCopyCost(BasicBlock* block)
    {
        unsigned cost = 0;
        for (Value* value : *block) {
            if (value->kind().isCloningForbidden() || value->opcode() == Patchpoint || value->opcode() == CCall)
                return std::nullopt;
            cost += !value->isConstant() && value->opcode() != Phi && value->opcode() != Nop && value != block->last();
        }
        m_test.shrink(0);
        auto addToTest = [&](Value* value) {
            if (value->owner == block && !value->isConstant() && value->opcode() != Phi && !m_test.contains(value))
                m_test.append(value);
        };
        addToTest(block->last()->child(0));
        for (unsigned i = 0; i < m_test.size() && m_test.size() <= cost; ++i) {
            for (Value* child : m_test[i]->children())
                addToTest(child);
        }
        cost -= std::min<unsigned>(cost, m_test.size());
        if (cost > maxDecidedTestCopyCost)
            return std::nullopt;
        return cost;
    }

    bool isLoopHeader(BasicBlock* block)
    {
        if (!m_factIndexInBlock.size())
            walkBlocks();
        return m_isLoopHeader[block];
    }

    static bool onlyJumps(BasicBlock* block)
    {
        if (block->last()->opcode() != Jump)
            return false;
        for (Value* value : *block) {
            if (value != block->last() && value->opcode() != Nop)
                return false;
        }
        return true;
    }

    void addUpsilonsAndSetsFrom(BasicBlock* block)
    {
        for (Value* value : *block) {
            if (auto* upsilon = value->as<UpsilonValue>())
                m_upsilonsOnPath.append(upsilon);
            else if (value->opcode() == Set)
                m_setsOnPath.append(value->as<VariableValue>());
        }
    }

    void enter(BasicBlock* block)
    {
        for (UpsilonValue* upsilon : m_upsilonsOnPath) {
            if (upsilon->phi()->owner == block)
                m_phiValuesOnEdge.append({ upsilon->phi(), upsilon->child(0) });
        }
        m_path.append(block);
        addUpsilonsAndSetsFrom(block);
    }

    template<typename Functor>
    Path* followDecidedTests(BasicBlock* predecessor, BasicBlock* next, const Functor& isWhollyCopied)
    {
        m_path.shrink(0);
        m_upsilonsOnPath.shrink(0);
        m_setsOnPath.shrink(0);
        m_phiValuesOnEdge.shrink(0);
        addUpsilonsAndSetsFrom(predecessor);
        Path path;
        unsigned pathCost = 0;
        unsigned indexOfFact = m_factIndexInBlock[predecessor];
        unsigned stepsToLastTest = 0;
        BasicBlock* undecided = followDecidedTests(predecessor, next, indexOfFact, pathCost, path.steps, stepsToLastTest);
        if (undecided && isWhollyCopied(undecided) && predecessor->frequency() * maxRatioOfTailToLikelyPredecessor >= undecided->frequency()) {
            auto [strippedTest, testIsNegated] = withoutNegations(undecided->last()->child(0), true);
            if (strippedTest) {
                forEachLeaf(strippedTest, false, false, maxTestDepth, [&](Value* leaf, bool) {
                    uint8_t& number = m_branchCountOn[leaf];
                    number += number < 2;
                    return true;
                });
                unsigned pathSize = m_path.size();
                unsigned numberOfUpsilons = m_upsilonsOnPath.size();
                unsigned numberOfSets = m_setsOnPath.size();
                unsigned numberOfPhis = m_phiValuesOnEdge.size();
                for (unsigned arm = 0; arm < 2; ++arm) {
                    m_facts.append(Fact { strippedTest, !arm != testIsNegated, indexOfFact });
                    auto& steps = path.stepsAfterUndecidedTest[arm];
                    unsigned branchCost = pathCost;
                    unsigned numberOfSteps = 0;
                    followDecidedTests(predecessor, undecided->successorBlock(arm), m_facts.size(), branchCost, steps, numberOfSteps);
                    steps.shrink(numberOfSteps);
                    m_path.shrink(pathSize);
                    m_upsilonsOnPath.shrink(numberOfUpsilons);
                    m_setsOnPath.shrink(numberOfSets);
                    m_phiValuesOnEdge.shrink(numberOfPhis);
                }
                if (!path.stepsAfterUndecidedTest[0].isEmpty() || !path.stepsAfterUndecidedTest[1].isEmpty()) {
                    path.steps.append(Step { undecided, Outcome::Unknown });
                    stepsToLastTest = path.steps.size();
                }
            }
        }
        if (!stepsToLastTest)
            return nullptr;
        path.steps.shrink(stepsToLastTest);
        m_paths.append(WTF::move(path));
        m_pathAfterIndex[predecessor] = m_paths.size();
        return &m_paths.last();
    }

    template<size_t inlineCapacity>
    BasicBlock* followDecidedTests(BasicBlock* predecessor, BasicBlock* next, unsigned indexOfFact, unsigned& pathCost, Vector<Step, inlineCapacity>& steps, unsigned& stepsToLastTest)
    {
        while (m_path.size() < maxBlocksOnPath) {
            if (next == predecessor || m_path.contains(next) || (!m_path.isEmpty() && isLoopHeader(next)))
                break;
            if (onlyJumps(next)) {
                m_path.append(next);
                next = next->successorBlock(0);
                continue;
            }
            bool jumps = next->last()->opcode() == Jump;
            std::optional<unsigned> cost = pathCopyCost(next);
            if (!cost || (*cost && predecessor->frequency() * maxRatioOfTailToLikelyPredecessor < next->frequency()))
                break;
            pathCost += *cost;
            if (pathCost > maxPathCost)
                break;
            enter(next);
            if (jumps) {
                steps.append(Step { next, Outcome::Jumps });
                next = next->successorBlock(0);
                continue;
            }
            std::optional<bool> isTaken = evaluateTest(next, indexOfFact, next->numPredecessors() <= maxPredecessorsForFactQueries);
            if (!isTaken)
                return next;
            steps.append(Step { next, *isTaken ? Outcome::Taken : Outcome::NotTaken });
            stepsToLastTest = steps.size();
            next = next->successorBlock(*isTaken ? 0 : 1);
        }
        return nullptr;
    }

    Value* valueSetOnPath(VariableValue* get)
    {
        Value* valueSetInSameBlock = nullptr;
        for (Value* value : *get->owner) {
            if (value == get)
                break;
            if (value->opcode() == Set && value->as<VariableValue>()->variable() == get->variable())
                valueSetInSameBlock = value->child(0);
        }
        if (valueSetInSameBlock)
            return valueSetInSameBlock;
        size_t position = m_path.find(get->owner);
        for (unsigned i = m_setsOnPath.size(); i--;) {
            VariableValue* set = m_setsOnPath[i];
            if (set->variable() != get->variable())
                continue;
            size_t setPosition = m_path.find(set->owner);
            if (setPosition == notFound || setPosition < position)
                return set->child(0);
        }
        return nullptr;
    }

    Value* onEdge(Value* value)
    {
        while (value && (value->opcode() == Phi || value->opcode() == Get) && m_path.contains(value->owner)) {
            if (value->opcode() == Get) {
                Value* valueSet = valueSetOnPath(value->as<VariableValue>());
                if (!valueSet)
                    break;
                value = valueSet;
                continue;
            }
            Value* phi = std::exchange(value, nullptr);
            for (unsigned i = m_phiValuesOnEdge.size(); i--;) {
                if (m_phiValuesOnEdge[i].first == phi) {
                    value = m_phiValuesOnEdge[i].second;
                    break;
                }
            }
        }
        return value;
    }

    std::pair<Value*, bool> withoutNegations(Value* value, bool isValueOnEdge)
    {
        bool isNegated = false;
        for (;;) {
            if (isValueOnEdge)
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

    bool childrenAreSameOnEdge(Value* valueOnEdge, Value* other, unsigned depth)
    {
        if (!depth || valueOnEdge->numChildren() != other->numChildren())
            return false;
        for (unsigned i = 0; i < valueOnEdge->numChildren(); ++i) {
            if (!isSameOnEdge(valueOnEdge->child(i), other->child(i), depth - 1))
                return false;
        }
        return true;
    }

    bool isSameOnEdge(Value* valueOnEdge, Value* other, unsigned depth)
    {
        valueOnEdge = onEdge(valueOnEdge);
        if (!valueOnEdge)
            return false;
        if (valueOnEdge == other)
            return true;
        if (valueOnEdge->type() != other->type())
            return false;
        if (valueOnEdge->hasInt() || other->hasInt())
            return valueOnEdge->hasInt() && other->hasInt() && valueOnEdge->asInt() == other->asInt();
        if (valueOnEdge->kind() != other->kind() || !isPureIntegerOperation(valueOnEdge->opcode()))
            return false;
        return childrenAreSameOnEdge(valueOnEdge, other, depth);
    }

    bool isBoxedInt32OnEdge(Value* value)
    {
        value = onEdge(value);
        if (!value || value->opcode() != Add)
            return false;
        for (unsigned i = 0; i < 2; ++i) {
            if (value->child(i)->opcode() == ZExt32 && isNumberTagOnEdge(value->child(1 - i)))
                return true;
        }
        return false;
    }

    bool isNumberTagOnEdge(Value* value)
    {
        return constantOnEdge(value, 0) == static_cast<int64_t>(JSValue::NumberTag);
    }

    std::optional<int64_t> constantOnEdge(Value* value, unsigned depth)
    {
        value = onEdge(value);
        if (!value)
            return std::nullopt;
        if (value->hasInt())
            return value->asInt();
        if (auto* argument = value->as<ArgumentRegValue>(); argument && m_proc.positionIndependent()) {
            if (argument->argumentReg() == Reg(GPRInfo::notCellMaskRegister))
                return static_cast<int64_t>(JSValue::NotCellMask);
            if (argument->argumentReg() == Reg(AOT::numberTagArgumentGPR))
                return static_cast<int64_t>(JSValue::NumberTag);
        }
        if (!depth || !m_path.contains(value->owner))
            return std::nullopt;
        if (value->opcode() == Select) {
            std::optional<int64_t> condition = constantOnEdge(value->child(0), depth - 1);
            if (!condition)
                return std::nullopt;
            return constantOnEdge(value->child(*condition ? 1 : 2), depth - 1);
        }
        if (value->numChildren() == 1) {
            std::optional<int64_t> operand = constantOnEdge(value->child(0), depth - 1);
            if (!operand)
                return std::nullopt;
            switch (value->opcode()) {
            case Trunc:
            case SExt32:
                return static_cast<int32_t>(*operand);
            case ZExt32:
                return static_cast<int64_t>(static_cast<uint32_t>(*operand));
            default:
                return std::nullopt;
            }
        }
        if (value->numChildren() != 2)
            return std::nullopt;
        if ((value->opcode() == AboveEqual || value->opcode() == Below) && isNumberTagOnEdge(value->child(1)) && isBoxedInt32OnEdge(value->child(0)))
            return value->opcode() == AboveEqual;
        if ((value->opcode() == BelowEqual || value->opcode() == Above) && isNumberTagOnEdge(value->child(0)) && isBoxedInt32OnEdge(value->child(1)))
            return value->opcode() == BelowEqual;
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

    template<typename Functor>
    bool forEachLeaf(Value* value, bool isValueOnEdge, bool isReplacedOnEdge, unsigned depth, const Functor& functor)
    {
        if (isValueOnEdge) {
            Value* valueOnEdge = onEdge(value);
            if (!valueOnEdge)
                return false;
            isReplacedOnEdge |= valueOnEdge != value;
            value = valueOnEdge;
        }
        if (value->isConstant())
            return true;
        if (!depth || !isPureIntegerOperation(value->opcode()))
            return functor(value, isReplacedOnEdge);
        for (Value* child : value->children()) {
            if (!forEachLeaf(child, isValueOnEdge, isReplacedOnEdge, depth - 1, functor))
                return false;
        }
        return true;
    }

    bool mayBeDecidedByFact(Value* strippedTest, bool isReplacedOnEdge)
    {
        return forEachLeaf(strippedTest, true, isReplacedOnEdge, maxTestDepth, [&](Value* leaf, bool isReplaced) {
            return m_branchCountOn[leaf] >= (isReplaced ? 1 : 2);
        });
    }

    void walkBlocks()
    {
        m_branchCountOn.resize(m_proc.values().size());
        m_factIndexInBlock.resize(m_proc.size());
        m_isLoopHeader.resize(m_proc.size());
        enum : uint8_t { IsNotReached, IsOnPath, IsUnchanged };
        IndexMap<BasicBlock*, uint8_t> state(m_proc.size(), static_cast<uint8_t>(IsNotReached));
        Vector<std::pair<BasicBlock*, unsigned>, 64> way;
        auto reach = [&](BasicBlock* block) {
            state[block] = IsOnPath;
            way.append({ block, 0 });
            if (block->last()->opcode() == Branch) {
                forEachLeaf(withoutNegations(block->last()->child(0), false).first, false, false, maxTestDepth, [&](Value* leaf, bool) {
                    uint8_t& number = m_branchCountOn[leaf];
                    number += number < 2;
                    return true;
                });
            }
            if (block->numPredecessors() != 1)
                return;
            BasicBlock* from = block->predecessor(0);
            unsigned index = m_factIndexInBlock[from];
            if (from->last()->opcode() == Branch && from->successorBlock(0) != from->successorBlock(1)) {
                auto [condition, conditionIsNegated] = withoutNegations(from->last()->child(0), false);
                m_facts.append(Fact { condition, (from->successorBlock(0) == block) != conditionIsNegated, index });
                index = m_facts.size();
            }
            m_factIndexInBlock[block] = index;
        };
        reach(m_proc.at(0));
        while (!way.isEmpty()) {
            auto& [block, indexOfSuccessor] = way.last();
            if (indexOfSuccessor == block->numSuccessors()) {
                state[block] = IsUnchanged;
                way.removeLast();
                continue;
            }
            BasicBlock* successor = block->successorBlock(indexOfSuccessor++);
            if (state[successor] == IsOnPath)
                m_isLoopHeader[successor] = true;
            else if (state[successor] == IsNotReached)
                reach(successor);
        }
    }

    std::optional<bool> evaluateTest(BasicBlock* tail, unsigned indexOfFact, bool mayQueryFacts)
    {
        Value* test = tail->last()->child(0);
        if (std::optional<int64_t> constant = constantOnEdge(test, maxTestDepth))
            return !!*constant;
        if (!mayQueryFacts)
            return std::nullopt;
        auto [strippedTest, testIsNegated] = withoutNegations(test, true);
        if (!strippedTest)
            return std::nullopt;
        if (!indexOfFact || !mayBeDecidedByFact(strippedTest, strippedTest != withoutNegations(test, false).first))
            return std::nullopt;
        unsigned factQueryCount = 0;
        for (unsigned index = indexOfFact; index && factQueryCount < maxFactQueries; index = m_facts[index - 1].indexOfNext, ++factQueryCount) {
            Value* condition = m_facts[index - 1].condition;
            bool conditionHolds = m_facts[index - 1].holds;
            if (isSameOnEdge(strippedTest, condition, maxTestDepth))
                return conditionHolds != testIsNegated;
            bool areOpposites = (strippedTest->opcode() == Equal && condition->opcode() == NotEqual) || (strippedTest->opcode() == NotEqual && condition->opcode() == Equal);
            if (areOpposites && strippedTest->child(0)->type() == condition->child(0)->type() && childrenAreSameOnEdge(strippedTest, condition, maxTestDepth))
                return conditionHolds == testIsNegated;
        }
        return std::nullopt;
    }

    Vector<BasicBlock*, maxBlocksOnPath> m_path;
    Vector<UpsilonValue*, 16> m_upsilonsOnPath;
    Vector<VariableValue*, 8> m_setsOnPath;
    Vector<Value*, 8> m_test;
    Vector<std::pair<Value*, Value*>, 8> m_phiValuesOnEdge;
    IndexMap<BasicBlock*, uint8_t> m_pathCopyCost;
    Vector<bool, 32> m_isNeededAfterDecidedTest;
    Vector<Value*, 32> m_usedAfterDecidedTest;
    struct Fact {
        Value* condition;
        bool holds;
        unsigned indexOfNext;
    };
    Vector<Fact> m_facts;
    IndexMap<BasicBlock*, unsigned> m_factIndexInBlock;
    IndexMap<BasicBlock*, bool> m_isLoopHeader;
    IndexMap<Value*, uint8_t> m_branchCountOn;
    Vector<Path> m_paths;
    IndexMap<BasicBlock*, unsigned> m_pathAfterIndex;
#endif

    Procedure& m_proc;
    InsertionSet m_insertionSet;
    unsigned m_maxSize;
    unsigned m_maxSuccessors;
};

} // anonymous namespace

void duplicateTails(Procedure& proc)
{
    PhaseScope phaseScope(proc, "duplicateTails"_s);
    DuplicateTails duplicateTails(proc);
    duplicateTails.run();
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)

