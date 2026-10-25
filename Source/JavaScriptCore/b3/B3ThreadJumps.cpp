/*
 * Copyright (C) 2016-2019 Apple Inc. All rights reserved.
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
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
#include "B3ThreadJumps.h"

#if ENABLE(B3_JIT)

#include "B3BasicBlockInlines.h"
#include "B3BlockRarity.h"
#include "B3BreakCriticalEdges.h"
#include "B3DominatingConditions.h"
#include "B3IntRange.h"
#include "B3Limits.h"
#include "B3LoopUtilities.h"
#include "B3PhaseScope.h"
#include "B3ProcedureInlines.h"
#include "B3RegionCloner.h"
#include "B3UpsilonValue.h"
#include "B3ValueInlines.h"
#include "B3VariableValue.h"
#include <array>
#include <span>
#include <wtf/IndexMap.h>
#include <wtf/IndexSet.h>

namespace JSC { namespace B3 {

namespace {

class ThreadJumps {
public:
    explicit ThreadJumps(Procedure& proc)
        : m_proc(proc)
        , m_cloner(proc)
        , m_maxSize(proc.policy().upstream.maxTailDuplicationBlockSize)
        , m_maxSuccessors(proc.policy().upstream.maxTailDuplicationBlockSuccessors)
        , m_maxTestDepth(proc.limit(Limit::MaxThreadedTestDepth))
    {
    }

    bool run()
    {
        breakCriticalEdges(m_proc);
        m_proc.resetValueOwners();

        recordBlockClasses();
        findLoopHeaders();
        countBranchLeaves();
        m_conditions.emplace(m_proc);
        findTailsCopiedWhole();
        findPaths();
        ASSERT(m_temporaryConstants.isEmpty());
        if (m_cloner.isEmpty()) {
            m_proc.resetReachability();
            m_proc.invalidateCFG();
            return false;
        }

        m_cloner.demoteEscapingValues(RegionCloner::PhiTreatment::KeepIfJumpFed, RegionCloner::ValueOwners::AreCurrent);
        copyPaths();
        copyWholeTails();
        return m_cloner.finish();
    }

private:
    enum class Outcome : uint8_t { Undecided, NotTaken, Taken, Jumps };

    struct Step {
        BasicBlock* block;
        Outcome outcome;
    };

    struct Path {
        Vector<Step, 4> steps;
        std::array<Vector<Step, 2>, 2> stepsAfterUndecidedTest;
    };

    struct CopyCost {
        enum class Kind : uint8_t { NotComputed, CannotCopy, Known };

        Kind kind { Kind::NotComputed };
        unsigned cost { 0 };
    };

    struct PathLength {
        size_t blocks;
        size_t upsilons;
        size_t sets;
        size_t phiValues;
    };

    void recordBlockClasses()
    {
        BlockRarity& rarity = m_proc.blockRarity();
        m_blockClasses.fill(FrequencyClass::Normal, m_proc.size());
        for (BasicBlock* block : m_proc)
            m_blockClasses[block->index()] = rarity.of(block);
    }

    BasicBlock* addBlock(FrequencyClass blockClass)
    {
        BasicBlock* block = m_proc.addBlock();
        m_blockClasses.grow(m_proc.size());
        m_blockClasses[block->index()] = blockClass;
        return block;
    }

    void findLoopHeaders()
    {
        m_isLoopHeader.resize(m_proc.size());
        enum class WalkState : uint8_t { NotReached, OnStack, Finished };
        IndexMap<BasicBlock*, WalkState> state(m_proc.size(), WalkState::NotReached);
        Vector<std::pair<BasicBlock*, unsigned>, 64> dfsStack;
        auto reach = [&](BasicBlock* block) {
            state[block] = WalkState::OnStack;
            dfsStack.append({ block, 0 });
        };
        reach(m_proc.at(0));
        while (!dfsStack.isEmpty()) {
            auto& [block, successorIndex] = dfsStack.last();
            if (successorIndex == block->numSuccessors()) {
                state[block] = WalkState::Finished;
                dfsStack.removeLast();
                continue;
            }
            BasicBlock* successor = block->successorBlock(successorIndex++);
            if (state[successor] == WalkState::OnStack)
                m_isLoopHeader[successor] = true;
            else if (state[successor] == WalkState::NotReached)
                reach(successor);
        }
    }

    void countLeaves(Value* strippedTest)
    {
        forEachLeaf(strippedTest, false, false, m_maxTestDepth, [&](Value* leaf, bool) {
            uint8_t& count = m_branchCountOn[leaf];
            count += count < 2;
            return true;
        });
    }

    void countBranchLeaves()
    {
        m_branchCountOn.resize(m_proc.values().size());
        for (BasicBlock* block : m_proc) {
            if (block->last()->opcode() == Branch)
                countLeaves(stripNegations(block->last()->child(0)).value);
        }
    }

    bool isLikelyPredecessor(BasicBlock* predecessor, BasicBlock* tail) const
    {
        return !isRarerThan(m_blockClasses[predecessor->index()], m_blockClasses[tail->index()]);
    }

    bool shouldCopyWhole(BasicBlock* block)
    {
        unsigned size = block->size();
        if (size > m_maxSize && !m_isLoopHeader[block] && block->last()->opcode() == Branch && block->last()->child(0)->opcode() == Phi && block->last()->child(0)->owner == block) {
            for (Value* value : *block)
                size -= value->opcode() == Phi;
        }
        if (size > m_maxSize)
            return false;
        if (block->numSuccessors() > m_maxSuccessors)
            return false;
        if (block->last()->type() != Void)
            return false;
        // A copy saves a jump where it runs and costs its bytes everywhere.
        if (m_blockClasses[block->index()] != FrequencyClass::Normal)
            return false;

        bool hasPhi = false;
        for (Value* value : *block) {
            if (!canCloneForLoop(value))
                return false;
            hasPhi |= value->opcode() == Phi;
        }
        if (hasPhi)
            return true;

        unsigned numberOfLikelyPredecessors = 0;
        for (BasicBlock* predecessor : block->predecessors())
            numberOfLikelyPredecessors += isLikelyPredecessor(predecessor, block);
        return numberOfLikelyPredecessors >= 2;
    }

    void findTailsCopiedWhole()
    {
        for (BasicBlock* block : m_proc) {
            if (shouldCopyWhole(block))
                m_cloner.addBlock(block);
        }
    }

    bool isCopiedWhole(BasicBlock* tail) const
    {
        return m_cloner.contains(tail) && !m_tailsOnlyForDecidedTests.contains(tail);
    }

    void findPaths()
    {
        m_pathAfter.resize(m_proc.size());
        m_pathCopyCost.resize(m_proc.size());
        for (BasicBlock* block : m_proc) {
            if (block->numPredecessors() < 2 || m_isLoopHeader[block] || !pathCopyCost(block))
                continue;
            for (BasicBlock* predecessor : block->predecessors()) {
                if (predecessor->last()->opcode() != Jump || predecessor == block)
                    continue;
                Path* path = followDecidedTests(predecessor, block);
                if (!path)
                    continue;
                for (std::span<const Step> steps : { path->steps.span(), path->stepsAfterUndecidedTest[0].span(), path->stepsAfterUndecidedTest[1].span() }) {
                    for (Step step : steps) {
                        if (m_cloner.contains(step.block))
                            continue;
                        m_cloner.addBlock(step.block);
                        m_tailsOnlyForDecidedTests.add(step.block);
                    }
                }
            }
        }
    }

    void copyPaths()
    {
        for (unsigned index = 0, numberOfBlocks = m_proc.size(); index < numberOfBlocks; ++index) {
            BasicBlock* block = m_proc[index];
            Path* path = block ? pathAfter(block) : nullptr;
            if (!path)
                continue;
            bool endsWithUndecidedTest = false;
            for (Step step : path->steps) {
                if (!m_cloner.contains(step.block) || step.block == block)
                    break;
                appendTailCopy(block, step.block, step.outcome);
                endsWithUndecidedTest = step.outcome == Outcome::Undecided;
            }
            if (!endsWithUndecidedTest)
                continue;
            for (unsigned arm = 0; arm < 2; ++arm) {
                if (path->stepsAfterUndecidedTest[arm].isEmpty())
                    continue;
                FrequentedBlock& successor = block->successor(arm);
                BasicBlock* edgeBlock = addBlock(rarerOf(m_blockClasses[block->index()], successor.frequency()));
                edgeBlock->appendNew<Value>(m_proc, Jump, block->last()->origin());
                edgeBlock->setSuccessors(FrequentedBlock(successor.block()));
                successor.block() = edgeBlock;
                for (Step step : path->stepsAfterUndecidedTest[arm]) {
                    if (!m_cloner.contains(step.block))
                        break;
                    appendTailCopy(edgeBlock, step.block, step.outcome);
                }
            }
        }
    }

    void copyWholeTails()
    {
        for (BasicBlock* block : m_proc) {
            if (block->last()->opcode() != Jump)
                continue;
            if (pathAfter(block))
                continue;

            BasicBlock* tail = block->successorBlock(0);
            if (block == tail || !isCopiedWhole(tail) || !isLikelyPredecessor(block, tail))
                continue;

            appendTailCopy(block, tail, Outcome::Undecided);
        }
    }

    void markNeededValues(BasicBlock* tail)
    {
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

    void appendTailCopy(BasicBlock* block, BasicBlock* tail, Outcome outcome)
    {
        m_cloner.removeBlock(block);
        block->removeLast(m_proc);

        bool isDecided = outcome == Outcome::Taken || outcome == Outcome::NotTaken;
        if (isDecided)
            markNeededValues(tail);

        RegionCloner::ValueMap map;
        m_cloner.substitutePhisFedBy(block, tail, map);
        unsigned indexInTail = 0;
        for (Value* value : *tail) {
            if (isDecided && !m_isNeededAfterDecidedTest[indexInTail++])
                continue;
            if (value->opcode() == Phi)
                continue;
            m_cloner.appendClone(value, block, map);
        }

        if (isDecided) {
            block->appendNew<Value>(m_proc, Jump, tail->last()->origin());
            block->setSuccessors(outcome == Outcome::Taken ? tail->taken() : tail->notTaken());
        } else
            block->successors() = tail->successors();
    }

    Path* pathAfter(BasicBlock* block)
    {
        if (block->index() >= m_pathAfter.size())
            return nullptr;
        std::optional<unsigned> index = m_pathAfter[block];
        return index ? &m_paths[*index] : nullptr;
    }

    static std::optional<unsigned> wholeCopyCost(BasicBlock* block)
    {
        unsigned cost = 0;
        for (Value* value : *block) {
            if (!canCloneForLoop(value))
                return std::nullopt;
            cost += !value->isConstant() && value->opcode() != Phi && value->opcode() != Nop && value != block->last();
        }
        return cost;
    }

    std::optional<unsigned> decidedTestCopyCost(BasicBlock* block)
    {
        std::optional<unsigned> cost = wholeCopyCost(block);
        if (!cost)
            return std::nullopt;
        m_testValues.shrink(0);
        auto addToTest = [&](Value* value) {
            if (value->owner == block && !value->isConstant() && value->opcode() != Phi && !m_testValues.contains(value))
                m_testValues.append(value);
        };
        addToTest(block->last()->child(0));
        for (unsigned i = 0; i < m_testValues.size() && m_testValues.size() <= *cost; ++i) {
            for (Value* child : m_testValues[i]->children())
                addToTest(child);
        }
        return *cost - std::min<unsigned>(*cost, m_testValues.size());
    }

    std::optional<unsigned> pathCopyCost(BasicBlock* block)
    {
        CopyCost& cached = m_pathCopyCost[block];
        if (cached.kind == CopyCost::Kind::NotComputed) {
            std::optional<unsigned> cost;
            if (block->last()->opcode() == Jump)
                cost = wholeCopyCost(block);
            else if (block->last()->opcode() == Branch && block->successorBlock(0) != block->successorBlock(1))
                cost = decidedTestCopyCost(block);
            if (cost && *cost > m_proc.limit(Limit::MaxThreadedTestCopyCost)) {
                m_proc.didReachLimit(Limit::MaxThreadedTestCopyCost, block->last()->origin());
                cost = std::nullopt;
            }
            if (cost)
                cached = { CopyCost::Kind::Known, *cost };
            else
                cached.kind = CopyCost::Kind::CannotCopy;
        }
        if (cached.kind == CopyCost::Kind::CannotCopy)
            return std::nullopt;
        return cached.cost;
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
                m_phiValuesOnPath.append({ upsilon->phi(), upsilon->child(0) });
        }
        m_path.append(block);
        addUpsilonsAndSetsFrom(block);
    }

    PathLength pathLength() const
    {
        return { m_path.size(), m_upsilonsOnPath.size(), m_setsOnPath.size(), m_phiValuesOnPath.size() };
    }

    void shrinkPath(PathLength length)
    {
        m_path.shrink(length.blocks);
        m_upsilonsOnPath.shrink(length.upsilons);
        m_setsOnPath.shrink(length.sets);
        m_phiValuesOnPath.shrink(length.phiValues);
    }

    Path* followDecidedTests(BasicBlock* predecessor, BasicBlock* next)
    {
        shrinkPath({ 0, 0, 0, 0 });
        addUpsilonsAndSetsFrom(predecessor);
        Path path;
        unsigned pathCost = 0;
        DominatingConditions::Chain chain = m_conditions->chainAt(predecessor);
        unsigned stepsToLastTest = 0;
        BasicBlock* undecided = followDecidedTests(predecessor, next, chain, pathCost, path.steps, stepsToLastTest);
        if (undecided && isCopiedWhole(undecided) && isLikelyPredecessor(predecessor, undecided)) {
            followArms(predecessor, undecided, chain, pathCost, path);
            if (!path.stepsAfterUndecidedTest[0].isEmpty() || !path.stepsAfterUndecidedTest[1].isEmpty()) {
                path.steps.append(Step { undecided, Outcome::Undecided });
                stepsToLastTest = path.steps.size();
            }
        }
        if (!stepsToLastTest)
            return nullptr;
        path.steps.shrink(stepsToLastTest);
        unsigned index = m_paths.size();
        m_paths.append(WTF::move(path));
        m_pathAfter[predecessor] = index;
        return &m_paths.last();
    }

    void followArms(BasicBlock* predecessor, BasicBlock* undecided, DominatingConditions::Chain chain, unsigned pathCost, Path& path)
    {
        std::optional<DominatingCondition> stripped = stripNegationsOnPath(undecided->last()->child(0));
        if (!stripped)
            return;
        countLeaves(stripped->value);
        PathLength length = pathLength();
        for (unsigned arm = 0; arm < 2; ++arm) {
            DominatingConditions::Chain armChain = m_conditions->extend(chain, { stripped->value, arm ? !stripped->isNonZero : stripped->isNonZero });
            auto& steps = path.stepsAfterUndecidedTest[arm];
            unsigned armCost = pathCost;
            unsigned stepsToLastTest = 0;
            followDecidedTests(predecessor, undecided->successorBlock(arm), armChain, armCost, steps, stepsToLastTest);
            steps.shrink(stepsToLastTest);
            shrinkPath(length);
        }
    }

    template<size_t inlineCapacity>
    BasicBlock* followDecidedTests(BasicBlock* predecessor, BasicBlock* next, DominatingConditions::Chain chain, unsigned& pathCost, Vector<Step, inlineCapacity>& steps, unsigned& stepsToLastTest)
    {
        while (true) {
            if (next == predecessor || m_path.contains(next) || m_isLoopHeader[next])
                return nullptr;
            bool isEmpty = onlyJumps(next);
            if (!isEmpty) {
                std::optional<unsigned> cost = pathCopyCost(next);
                if (!cost || (*cost && !isLikelyPredecessor(predecessor, next)))
                    return nullptr;
                pathCost += *cost;
                if (pathCost > m_proc.limit(Limit::MaxThreadedPathCost)) {
                    m_proc.didReachLimit(Limit::MaxThreadedPathCost, next->last()->origin());
                    return nullptr;
                }
            }
            if (m_path.size() >= m_proc.limit(Limit::MaxBlocksOnThreadedPath)) {
                m_proc.didReachLimit(Limit::MaxBlocksOnThreadedPath, next->last()->origin());
                return nullptr;
            }
            if (isEmpty) {
                m_path.append(next);
                next = next->successorBlock(0);
                continue;
            }
            enter(next);
            if (next->last()->opcode() == Jump) {
                steps.append(Step { next, Outcome::Jumps });
                next = next->successorBlock(0);
                continue;
            }
            TriState isTaken = evaluateTest(next, chain, next->numPredecessors() <= m_proc.limit(Limit::MaxPredecessorsForConditionQueries));
            if (isTaken == TriState::Indeterminate)
                return next;
            steps.append(Step { next, isTaken == TriState::True ? Outcome::Taken : Outcome::NotTaken });
            stepsToLastTest = steps.size();
            next = next->successorBlock(isTaken == TriState::True ? 0 : 1);
        }
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

    Value* valueOnPath(Value* value)
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
            for (unsigned i = m_phiValuesOnPath.size(); i--;) {
                if (m_phiValuesOnPath[i].first == phi) {
                    value = m_phiValuesOnPath[i].second;
                    break;
                }
            }
        }
        return value;
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
            return true;
        default:
            return isIntegerComparison(opcode);
        }
    }

    std::optional<DominatingCondition> stripNegationsOnPath(Value* condition)
    {
        return stripNegations(condition, [&](Value* value) {
            return valueOnPath(value);
        });
    }

    bool childrenAreSameOnPath(Value* value, Value* other, unsigned depth)
    {
        if (value->numChildren() != other->numChildren())
            return false;
        if (!depth) {
            m_proc.didReachLimit(Limit::MaxThreadedTestDepth, value->origin());
            return false;
        }
        for (unsigned i = 0; i < value->numChildren(); ++i) {
            if (!isSameOnPath(value->child(i), other->child(i), depth - 1))
                return false;
        }
        return true;
    }

    bool isSameOnPath(Value* value, Value* other, unsigned depth)
    {
        value = valueOnPath(value);
        if (!value)
            return false;
        if (value == other)
            return true;
        if (value->type() != other->type())
            return false;
        if (value->hasInt() || other->hasInt())
            return value->hasInt() && other->hasInt() && value->asInt() == other->asInt();
        if (value->kind() != other->kind() || !isPureIntegerOperation(value->opcode()))
            return false;
        return childrenAreSameOnPath(value, other, depth);
    }

    Value* trackTemporary(Value* constant)
    {
        if (constant)
            m_temporaryConstants.append(constant);
        return constant;
    }

    void deleteTemporaryConstants()
    {
        for (Value* constant : m_temporaryConstants)
            m_proc.deleteValue(constant);
        m_temporaryConstants.shrink(0);
    }

    Value* foldWithConstants(Value* value, Value* left, Value* right)
    {
        auto boolConstant = [&](TriState result) {
            return trackTemporary(m_proc.addBoolConstant(value->origin(), result));
        };
        auto intConstant = [&](int64_t result) {
            return trackTemporary(m_proc.addIntConstant(value->origin(), value->type(), result));
        };
        switch (value->opcode()) {
        case Add:
            return trackTemporary(left->addConstant(m_proc, right));
        case Sub:
            return trackTemporary(left->subConstant(m_proc, right));
        case BitAnd:
            return trackTemporary(left->bitAndConstant(m_proc, right));
        case BitOr:
            return trackTemporary(left->bitOrConstant(m_proc, right));
        case BitXor:
            return trackTemporary(left->bitXorConstant(m_proc, right));
        case Shl:
            return trackTemporary(left->shlConstant(m_proc, right));
        case SShr:
            return trackTemporary(left->sShrConstant(m_proc, right));
        case ZShr:
            return trackTemporary(left->zShrConstant(m_proc, right));
        case SExt8:
            return intConstant(static_cast<int8_t>(left->asInt()));
        case SExt16:
            return intConstant(static_cast<int16_t>(left->asInt()));
        case SExt32:
        case Trunc:
            return intConstant(static_cast<int32_t>(left->asInt()));
        case ZExt32:
            return intConstant(static_cast<uint32_t>(left->asInt()));
        case Equal:
            return boolConstant(left->equalConstant(right));
        case NotEqual:
            return boolConstant(left->notEqualConstant(right));
        case LessThan:
            return boolConstant(left->lessThanConstant(right));
        case GreaterThan:
            return boolConstant(left->greaterThanConstant(right));
        case LessEqual:
            return boolConstant(left->lessEqualConstant(right));
        case GreaterEqual:
            return boolConstant(left->greaterEqualConstant(right));
        case Above:
            return boolConstant(left->aboveConstant(right));
        case Below:
            return boolConstant(left->belowConstant(right));
        case AboveEqual:
            return boolConstant(left->aboveEqualConstant(right));
        case BelowEqual:
            return boolConstant(left->belowEqualConstant(right));
        default:
            return nullptr;
        }
    }

    TriState evaluateByRanges(Opcode opcode, Value* left, Value* right)
    {
        if (!left || !right || !left->type().isInt())
            return TriState::Indeterminate;
        Signedness signedness = isUnsignedRelation(opcode) ? Signedness::Unsigned : Signedness::Signed;
        return evaluateComparison(opcode, intervalFor(m_proc, left, signedness), intervalFor(m_proc, right, signedness));
    }

    Value* constantOnPath(Value* value, unsigned depth)
    {
        value = valueOnPath(value);
        if (!value)
            return nullptr;
        if (value->hasInt())
            return value;
        if (!m_path.contains(value->owner))
            return nullptr;
        bool isSelect = value->opcode() == Select;
        if (!isSelect && !isPureIntegerOperation(value->opcode()))
            return nullptr;
        if (!depth) {
            m_proc.didReachLimit(Limit::MaxThreadedTestDepth, value->origin());
            return nullptr;
        }

        if (isSelect) {
            Value* condition = constantOnPath(value->child(0), depth - 1);
            if (!condition)
                return nullptr;
            return constantOnPath(value->child(condition->isInt(0) ? 2 : 1), depth - 1);
        }

        Value* left = constantOnPath(value->child(0), depth - 1);
        if (value->numChildren() == 1)
            return left ? foldWithConstants(value, left, nullptr) : nullptr;

        Value* right = constantOnPath(value->child(1), depth - 1);
        if (left && right)
            return foldWithConstants(value, left, right);
        if ((!left && !right) || !isIntegerComparison(value->opcode()) || !value->child(0)->type().isInt())
            return nullptr;
        TriState result = evaluateByRanges(value->opcode(), left ? left : valueOnPath(value->child(0)), right ? right : valueOnPath(value->child(1)));
        return trackTemporary(m_proc.addBoolConstant(value->origin(), result));
    }

    template<typename Functor>
    bool forEachLeaf(Value* value, bool isValueOnPath, bool isReplacedOnPath, unsigned depth, const Functor& functor)
    {
        if (isValueOnPath) {
            Value* resolved = valueOnPath(value);
            if (!resolved)
                return false;
            isReplacedOnPath |= resolved != value;
            value = resolved;
        }
        if (value->isConstant())
            return true;
        if (!isPureIntegerOperation(value->opcode()))
            return functor(value, isReplacedOnPath);
        if (!depth) {
            m_proc.didReachLimit(Limit::MaxThreadedTestDepth, value->origin());
            return functor(value, isReplacedOnPath);
        }
        for (Value* child : value->children()) {
            if (!forEachLeaf(child, isValueOnPath, isReplacedOnPath, depth - 1, functor))
                return false;
        }
        return true;
    }

    bool mayBeDecidedByCondition(Value* strippedTest, bool isReplacedOnPath)
    {
        return forEachLeaf(strippedTest, true, isReplacedOnPath, m_maxTestDepth, [&](Value* leaf, bool isReplaced) {
            return m_branchCountOn[leaf] >= (isReplaced ? 1 : 2);
        });
    }

    TriState evaluateAgainst(const DominatingCondition& test, const DominatingCondition& known)
    {
        auto taken = [&](bool valueIsNonZero) {
            return triState(valueIsNonZero == test.isNonZero);
        };
        if (isSameOnPath(test.value, known.value, m_maxTestDepth))
            return taken(known.isNonZero);
        bool areOpposites = (test.value->opcode() == Equal && known.value->opcode() == NotEqual) || (test.value->opcode() == NotEqual && known.value->opcode() == Equal);
        if (areOpposites && test.value->child(0)->type() == known.value->child(0)->type() && childrenAreSameOnPath(test.value, known.value, m_maxTestDepth))
            return taken(!known.isNonZero);
        if (isComparisonWithConstant(test.value) && isComparisonWithConstant(known.value) && isSameOnPath(test.value->child(0), known.value->child(0), m_maxTestDepth)) {
            TriState result = evaluateImpliedComparison(test.value, known.value, known.isNonZero);
            if (result != TriState::Indeterminate)
                return taken(result == TriState::True);
        }
        return TriState::Indeterminate;
    }

    TriState evaluateTest(BasicBlock* tail, DominatingConditions::Chain chain, bool mayQueryConditions)
    {
        Value* test = tail->last()->child(0);
        Value* constant = constantOnPath(test, m_maxTestDepth);
        TriState result = constant ? triState(!constant->isInt(0)) : TriState::Indeterminate;
        deleteTemporaryConstants();
        if (result != TriState::Indeterminate || !chain)
            return result;
        if (!mayQueryConditions) {
            m_proc.didReachLimit(Limit::MaxPredecessorsForConditionQueries, tail->last()->origin());
            return TriState::Indeterminate;
        }
        std::optional<DominatingCondition> stripped = stripNegationsOnPath(test);
        if (!stripped || !mayBeDecidedByCondition(stripped->value, stripped->value != stripNegations(test).value))
            return TriState::Indeterminate;
        m_conditions->forEach(chain, [&](const DominatingCondition& known) {
            result = evaluateAgainst(*stripped, known);
            return result == TriState::Indeterminate ? IterationStatus::Continue : IterationStatus::Done;
        });
        return result;
    }

    Procedure& m_proc;
    RegionCloner m_cloner;
    std::optional<DominatingConditions> m_conditions;
    Vector<FrequencyClass> m_blockClasses;
    IndexMap<BasicBlock*, bool> m_isLoopHeader;
    IndexMap<Value*, uint8_t> m_branchCountOn;
    IndexSet<BasicBlock*> m_tailsOnlyForDecidedTests;
    IndexMap<BasicBlock*, CopyCost> m_pathCopyCost;
    Vector<Path> m_paths;
    IndexMap<BasicBlock*, std::optional<unsigned>> m_pathAfter;
    Vector<BasicBlock*, 8> m_path;
    Vector<UpsilonValue*, 16> m_upsilonsOnPath;
    Vector<VariableValue*, 8> m_setsOnPath;
    Vector<std::pair<Value*, Value*>, 8> m_phiValuesOnPath;
    Vector<Value*, 8> m_testValues;
    Vector<Value*, 8> m_temporaryConstants;
    Vector<bool, 32> m_isNeededAfterDecidedTest;
    Vector<Value*, 32> m_usedAfterDecidedTest;
    unsigned m_maxSize;
    unsigned m_maxSuccessors;
    unsigned m_maxTestDepth;
};

} // anonymous namespace

bool threadJumps(Procedure& proc)
{
    PhaseScope phaseScope(proc, "threadJumps"_s);
    return ThreadJumps(proc).run();
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
