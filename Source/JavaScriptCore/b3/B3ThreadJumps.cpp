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
#include "B3InsertionSetInlines.h"
#include "B3IntRange.h"
#include "B3Limits.h"
#include "B3LoopUtilities.h"
#include "B3MemoryNumbering.h"
#include "B3MemoryValue.h"
#include "B3NaturalLoops.h"
#include "B3PhaseScope.h"
#include "B3ProcedureInlines.h"
#include "B3RegionCloner.h"
#include "B3UpsilonValue.h"
#include "B3ValueInlines.h"
#include "B3VariableValue.h"
#include <array>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>
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
        withdrawTailsWhoseValuesCrossManyTails();
        if (m_cloner.isEmpty()) {
            m_proc.resetReachability();
            m_proc.invalidateCFG();
            return false;
        }

        m_cloner.demoteEscapingValues(RegionCloner::PhiTreatment::KeepIfJumpFed, RegionCloner::ValueOwners::AreCurrent);
        keepEarlierLoadsInVariables();
        copyPaths();
        copyWholeTails();
        return m_cloner.finish();
    }

private:
    enum class Outcome : uint8_t { Undecided, NotTaken, Taken, Jumps };

    struct Step {
        BasicBlock* block;
        Outcome outcome;
        bool isDecidedByWordReadAgain { false };
    };

    struct WordReadAgain {
        Value* load;
        Value* earlierLoad;
    };

    struct Path {
        Vector<Step, 4> steps;
        std::array<Vector<Step, 2>, 2> stepsAfterUndecidedTest;
        // On this path each of these loads gives what a load in front of the path has given.
        Vector<WordReadAgain, 2> wordsReadAgain;
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
            if (state[successor] == WalkState::OnStack) {
                m_isLoopHeader[successor] = true;
                m_hasLoop = true;
            } else if (state[successor] == WalkState::NotReached)
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
        m_boundsOfWrites.resize(m_proc.size());
        m_isOnWalk.ensureSize(m_proc.size());
        for (BasicBlock* block : m_proc) {
            if (block->numPredecessors() < 2 || m_isLoopHeader[block] || !pathCopyCost(block))
                continue;
            for (BasicBlock* predecessor : block->predecessors()) {
                if (predecessor->last()->opcode() != Jump || predecessor == block)
                    continue;
                followDecidedTests(predecessor, block);
            }
        }

        // An unlikely way gets no copy of a block that costs something. But where every other way into the block has got one, the block is left to that way alone, and its copy takes the place of the block.
        // It is made where the test of a word read again would otherwise be left standing for that way.
        for (BasicBlock* block : m_proc) {
            if (!m_hasPassedWordReadAgain)
                break;
            if (block->numPredecessors() < 2 || m_isLoopHeader[block] || block->last()->opcode() != Jump || !pathCopyCost(block).value_or(0))
                continue;
            BasicBlock* left = nullptr;
            bool isLeftToOne = true;
            for (BasicBlock* predecessor : block->predecessors()) {
                Path* path = pathAfter(predecessor);
                if (path && path->steps[0].block == block)
                    continue;
                isLeftToOne &= !left && predecessor->last()->opcode() == Jump && predecessor != block && !path;
                left = predecessor;
            }
            if (!left || !isLeftToOne)
                continue;
            m_blockLeftToPredecessor = block;
            Path* path = followDecidedTests(left, block);
            m_blockLeftToPredecessor = nullptr;
            if (path && !path->steps.containsIf([](const Step& step) { return step.isDecidedByWordReadAgain; }))
                m_pathAfter[left] = std::nullopt;
        }

        endPathsAtTestsThatStay();
        for (BasicBlock* block : m_proc) {
            Path* path = pathAfter(block);
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

    static BasicBlock* blockBehind(Step step)
    {
        return step.block->successorBlock(step.outcome == Outcome::NotTaken ? 1 : 0);
    }

    template<typename Functor>
    void forEachSuccessorBehindPaths(BasicBlock* block, const Functor& functor)
    {
        Path* path = pathAfter(block);
        if (!path) {
            for (BasicBlock* successor : block->successorBlocks())
                functor(successor);
            return;
        }
        Step last = path->steps.last();
        if (last.outcome != Outcome::Undecided) {
            functor(blockBehind(last));
            return;
        }
        for (unsigned arm = 0; arm < 2; ++arm) {
            const auto& steps = path->stepsAfterUndecidedTest[arm];
            functor(steps.isEmpty() ? last.block->successorBlock(arm) : blockBehind(steps.last()));
        }
    }

    template<size_t inlineCapacity>
    static bool endAtTestThatStays(Vector<Step, inlineCapacity>& steps, const BitVector& stays)
    {
        size_t end = steps.findIf([&](const Step& step) {
            return step.isDecidedByWordReadAgain && stays.quickGet(step.block->index());
        });
        if (end == notFound)
            return false;
        while (end && steps[end - 1].outcome == Outcome::Jumps)
            --end;
        steps.shrink(end);
        return true;
    }

    // The test of a word that is read again is passed by only where every way to it passes it by: then the test is gone. Where one way still needs it, the others
    // would save a load and a branch, and what stands behind the test would soon be there twice.
    void endPathsAtTestsThatStay()
    {
        if (!m_hasPassedWordReadAgain)
            return;

        BitVector stays(m_proc.size());
        Vector<BasicBlock*, 32> worklist;
        for (bool didEndPath = true; didEndPath;) {
            didEndPath = false;
            stays.clearAll();
            stays.quickSet(m_proc.at(0)->index());
            worklist.append(m_proc.at(0));
            while (!worklist.isEmpty()) {
                forEachSuccessorBehindPaths(worklist.takeLast(), [&](BasicBlock* successor) {
                    if (!stays.quickSet(successor->index()))
                        worklist.append(successor);
                });
            }
            for (BasicBlock* block : m_proc) {
                Path* path = pathAfter(block);
                if (!path)
                    continue;
                if (endAtTestThatStays(path->steps, stays)) {
                    didEndPath = true;
                    path->stepsAfterUndecidedTest[0].shrink(0);
                    path->stepsAfterUndecidedTest[1].shrink(0);
                } else if (path->steps.last().outcome == Outcome::Undecided) {
                    didEndPath |= endAtTestThatStays(path->stepsAfterUndecidedTest[0], stays);
                    didEndPath |= endAtTestThatStays(path->stepsAfterUndecidedTest[1], stays);
                    if (path->stepsAfterUndecidedTest[0].isEmpty() && path->stepsAfterUndecidedTest[1].isEmpty()) {
                        path->steps.removeLast();
                        while (!path->steps.isEmpty() && path->steps.last().outcome == Outcome::Jumps)
                            path->steps.removeLast();
                    }
                }
                if (path->steps.isEmpty())
                    m_pathAfter[block] = std::nullopt;
            }
        }
    }

    // A value used outside its tail gets a definition in each copy, and those can need a Phi at every later copied tail
    // where the value is still live.
    void withdrawTailsWhoseValuesCrossManyTails()
    {
        uint64_t limit = m_proc.limit(Limit::MaxTailsCrossedByCopiedValues);
        uint64_t numberOfTails = 0;
        uint64_t mostValuesInTail = 0;
        for (BasicBlock* block : m_proc) {
            if (!m_cloner.contains(block))
                continue;
            ++numberOfTails;
            mostValuesInTail = std::max<uint64_t>(mostValuesInTail, block->size());
        }
        if (numberOfTails * mostValuesInTail <= limit)
            return;

        Vector<BasicBlock*> postOrder = m_proc.blocksInPostOrder();
        unsigned numberOfBlocks = postOrder.size();
        IndexMap<BasicBlock*, unsigned> position(m_proc.size(), 0);
        for (unsigned index = 0; index < numberOfBlocks; ++index)
            position[postOrder[index]] = numberOfBlocks - 1 - index;

        NaturalLoops* loops = m_hasLoop ? &m_proc.naturalLoops() : nullptr;
        Vector<unsigned> lastPositionInLoop;
        if (loops) {
            lastPositionInLoop.fill(0, loops->numLoops());
            for (BasicBlock* block : postOrder) {
                for (const NaturalLoop* loop = loops->innerMostLoopOf(block); loop; loop = loops->innerMostOuterLoop(*loop))
                    lastPositionInLoop[loop->index()] = std::max(lastPositionInLoop[loop->index()], position[block]);
            }
        }

        IndexMap<Value*, unsigned> lastLivePosition(m_proc.values().size(), 0);
        for (BasicBlock* block : postOrder) {
            for (Value* value : *block) {
                for (Value* child : value->children()) {
                    BasicBlock* tail = child->owner;
                    if (tail == block || !m_cloner.contains(tail))
                        continue;
                    unsigned last = position[block];
                    for (const NaturalLoop* loop = loops ? loops->innerMostLoopOf(block) : nullptr; loop && !loops->belongsTo(tail, *loop); loop = loops->innerMostOuterLoop(*loop))
                        last = lastPositionInLoop[loop->index()];
                    lastLivePosition[child] = std::max(lastLivePosition[child], last);
                }
            }
        }

        Vector<unsigned> numberOfTailsFrom(FillWith { }, numberOfBlocks + 1, 0u);
        for (unsigned index = 0; index < numberOfBlocks; ++index) {
            BasicBlock* tail = postOrder[index];
            unsigned tailPosition = numberOfBlocks - 1 - index;
            bool isCopied = m_cloner.contains(tail);
            if (isCopied) {
                uint64_t crossings = 0;
                for (Value* value : *tail) {
                    if (unsigned last = lastLivePosition[value])
                        crossings += numberOfTailsFrom[tailPosition + 1] - numberOfTailsFrom[last + 1];
                }
                if (crossings > limit) {
                    m_proc.didReachLimit(Limit::MaxTailsCrossedByCopiedValues, tail->last()->origin());
                    m_cloner.withdrawBlock(tail);
                    isCopied = false;
                }
            }
            numberOfTailsFrom[tailPosition] = numberOfTailsFrom[tailPosition + 1] + isCopied;
        }
    }

    // A block that is copied hands its values on through variables. The copy of a load that reads a word again takes the value of the earlier load from there.
    void keepEarlierLoadsInVariables()
    {
        for (const Path& path : m_paths) {
            for (const WordReadAgain& word : path.wordsReadAgain) {
                if (m_cloner.contains(word.earlierLoad->owner))
                    m_variableOfEarlierLoad.add(word.earlierLoad, nullptr);
            }
        }
        if (m_variableOfEarlierLoad.isEmpty())
            return;
        InsertionSet insertionSet(m_proc);
        for (BasicBlock* block : m_proc) {
            for (unsigned index = 0; index < block->size(); ++index) {
                Value* value = block->at(index);
                auto entry = m_variableOfEarlierLoad.find(value);
                if (entry == m_variableOfEarlierLoad.end())
                    continue;
                entry->value = m_proc.addVariable(value->type());
                if (m_proc.policy().optimization.keepsOriginsOfDemotedValues)
                    entry->value->setOrigin(value->origin());
                insertionSet.insert<VariableValue>(index + 1, Set, value->origin(), entry->value, value)->owner = block;
            }
            insertionSet.execute(block);
        }
    }

    Value* appendEarlierLoad(BasicBlock* block, const Path* path, Value* load)
    {
        if (!path)
            return nullptr;
        for (const WordReadAgain& word : path->wordsReadAgain) {
            if (word.load != load)
                continue;
            if (Variable* variable = m_variableOfEarlierLoad.get(word.earlierLoad))
                return block->appendNew<VariableValue>(m_proc, Get, load->origin(), variable);
            return word.earlierLoad;
        }
        return nullptr;
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
                appendTailCopy(block, step.block, step.outcome, path);
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
                    appendTailCopy(edgeBlock, step.block, step.outcome, path);
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

    void appendTailCopy(BasicBlock* block, BasicBlock* tail, Outcome outcome, const Path* path = nullptr)
    {
        m_cloner.removeBlock(block);
        block->removeLast(m_proc);

        bool isDecided = outcome == Outcome::Taken || outcome == Outcome::NotTaken;
        if (isDecided)
            markNeededValues(tail);

        MovedLoadLog::RegionCopy regionCopy(m_proc.movedLoadLog());
        RegionCloner::ValueMap map;
        m_cloner.substitutePhisFedBy(block, tail, map);
        unsigned indexInTail = 0;
        for (Value* value : *tail) {
            if (isDecided && !m_isNeededAfterDecidedTest[indexInTail++])
                continue;
            if (value->opcode() == Phi)
                continue;
            if (Value* earlierLoad = appendEarlierLoad(block, path, value)) {
                m_cloner.substituteOnOnePath(map, value, earlierLoad);
                continue;
            }
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
        m_predecessor = predecessor;
        m_wordsReadAgain.shrink(0);
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
        path.wordsReadAgain = m_wordsReadAgain;
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
        // A block that jumps to where ways meet has a path of its own. A path that has passed tests of words read again and that a limit ends
        // ends in front of such a block instead, so that the next such test is not left standing for it.
        unsigned stepsToLastWayIn = 0;
        auto endAtLimit = [&](Limit limit) -> BasicBlock* {
            m_proc.didReachLimit(limit, next->last()->origin());
            bool hasPassedSuchTest = steps.containsIf([](const Step& step) {
                return step.isDecidedByWordReadAgain;
            });
            if (stepsToLastWayIn && hasPassedSuchTest)
                stepsToLastTest = stepsToLastWayIn;
            return nullptr;
        };
        while (true) {
            if (next == predecessor || m_path.contains(next) || m_isLoopHeader[next])
                return nullptr;
            if (next->last()->opcode() == Jump && stepsToLastTest == steps.size() && next->successorBlock(0)->numPredecessors() >= 2)
                stepsToLastWayIn = steps.size();
            bool isEmpty = onlyJumps(next);
            unsigned costOfBlock = 0;
            if (!isEmpty) {
                std::optional<unsigned> cost = pathCopyCost(next);
                if (!cost || (*cost && next != m_blockLeftToPredecessor && !isLikelyPredecessor(predecessor, next)))
                    return nullptr;
                costOfBlock = *cost;
                pathCost += *cost;
                if (pathCost > m_proc.limit(Limit::MaxThreadedPathCost))
                    return endAtLimit(Limit::MaxThreadedPathCost);
            }
            if (m_path.size() >= m_proc.limit(Limit::MaxBlocksOnThreadedPath))
                return endAtLimit(Limit::MaxBlocksOnThreadedPath);
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
            m_didCompareLoads = false;
            TriState isTaken = evaluateTest(next, chain, next->numPredecessors() <= m_proc.limit(Limit::MaxPredecessorsForConditionQueries));
            // A word that is read again decides a block that does nothing but test it. What else the block does would have to be copied, for a test that costs a load and a branch.
            if (isTaken == TriState::Indeterminate || (m_didCompareLoads && costOfBlock))
                return next;
            // Nor does it change the ways into a loop or out of it: the phases that work on loops take a loop for another one then.
            if (m_didCompareLoads && (!isInSameLoop(blockThatBranchesTo(predecessor), next) || !isInSameLoop(next, next->successorBlock(isTaken == TriState::True ? 0 : 1))))
                return next;
            steps.append(Step { next, isTaken == TriState::True ? Outcome::Taken : Outcome::NotTaken, m_didCompareLoads });
            m_hasPassedWordReadAgain |= m_didCompareLoads;
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
        case BitwiseCast:
            return true;
        default:
            return isIntegerComparison(opcode);
        }
    }

    // `if (isCell(v) && v->word & 1) ..; if (isCell(v) && v->word & 1) ..`: the second read stands below a test of its own, so the first does not dominate it and both are there.
    // A read at the head of the block where the ways meet is there because one of the ways has written the word: that way still needs the test.
    static bool mayReadWordAgain(Value* value)
    {
        return isLoad(value->opcode()) && !value->as<MemoryValue>()->hasFence() && value->owner->numPredecessors() == 1;
    }

    // Two of these give the same value if their operands do and, for a load, nothing has written the word in between.
    static bool isComparedByOperands(Value* value)
    {
        return isPureIntegerOperation(value->opcode()) || value->opcode() == Select || mayReadWordAgain(value);
    }

    static bool writesBetween(BasicBlock* block, unsigned begin, unsigned end, const HeapRange& range)
    {
        for (unsigned index = begin; index < end; ++index) {
            if (MemoryNumbering::writesOf(block->at(index)).overlaps(range))
                return true;
        }
        return false;
    }

    bool writes(BasicBlock* block, const HeapRange& range)
    {
        std::optional<HeapRange>& bounds = m_boundsOfWrites[block];
        if (!bounds) {
            bounds = HeapRange();
            for (Value* value : *block)
                *bounds |= MemoryNumbering::writesOf(value).bounds();
        }
        return bounds->overlaps(range) && writesBetween(block, 0, block->size(), range);
    }

    // Whether no way from the load to the end of the block, on which the load does not run again, writes what it read. The block of the load dominates `last`, so every way back from there comes to it.
    bool isUnchangedAtEndOf(BasicBlock* last, MemoryValue* load)
    {
        BasicBlock* first = load->owner;
        if (!m_proc.dominators().dominates(first, last))
            return false;
        auto keyOf = [&](BasicBlock* block) {
            return static_cast<uint64_t>(load->index()) << 32 | block->index();
        };
        if (auto known = m_isUnchangedAtEnd.find(keyOf(last)); known != m_isUnchangedAtEnd.end())
            return known->value;

        const HeapRange& range = load->range();
        bool isUnchanged = true;
        m_walk.shrink(0);
        auto visit = [&](BasicBlock* block) {
            if (!m_isOnWalk.quickSet(block->index()))
                m_walk.append(block);
        };
        visit(last);
        for (unsigned index = 0; index < m_walk.size() && isUnchanged; ++index) {
            BasicBlock* block = m_walk[index];
            if (block == first) {
                size_t position = block->values().find(load);
                RELEASE_ASSERT(position != notFound);
                isUnchanged = !writesBetween(block, static_cast<unsigned>(position) + 1, block->size(), range);
                continue;
            }
            if (auto known = m_isUnchangedAtEnd.find(keyOf(block)); known != m_isUnchangedAtEnd.end()) {
                isUnchanged = known->value;
                continue;
            }
            isUnchanged = !writes(block, range);
            for (BasicBlock* predecessor : block->predecessors())
                visit(predecessor);
        }
        for (BasicBlock* block : m_walk) {
            m_isOnWalk.quickClear(block->index());
            if (isUnchanged)
                m_isUnchangedAtEnd.add(keyOf(block), true);
        }
        if (!isUnchanged)
            m_isUnchangedAtEnd.add(keyOf(last), false);
        return isUnchanged;
    }

    // An edge out of a loop has a block of its own, which is outside the loop.
    static BasicBlock* blockThatBranchesTo(BasicBlock* block)
    {
        return onlyJumps(block) && block->numPredecessors() == 1 ? block->predecessor(0) : block;
    }

    bool isInSameLoop(BasicBlock* a, BasicBlock* b)
    {
        if (!m_hasLoop)
            return true;
        NaturalLoops& loops = m_proc.naturalLoops();
        return loops.innerMostLoopOf(a) == loops.innerMostLoopOf(b);
    }

    // `known` has run on the way to the path or on it, `load` runs on it.
    bool readsSameWordOnPath(MemoryValue* load, MemoryValue* known)
    {
        if (load->readsMutability() == Mutability::Immutable && known->readsMutability() == Mutability::Immutable)
            return true;
        size_t position = m_path.find(load->owner);
        if (position == notFound)
            return false;
        for (size_t index = 0; index <= position; ++index) {
            if (writes(m_path[index], load->range()))
                return false;
        }
        size_t positionOfKnown = m_path.find(known->owner);
        if (positionOfKnown != notFound)
            return positionOfKnown <= position;
        return isUnchangedAtEndOf(m_predecessor, known);
    }

    void noteWordReadAgain(MemoryValue* load, MemoryValue* known)
    {
        if (!m_path.contains(load->owner) || m_path.contains(known->owner) || !m_proc.dominators().dominates(known->owner, m_predecessor))
            return;
        bool isNoted = m_wordsReadAgain.containsIf([&](const WordReadAgain& word) {
            return word.load == load;
        });
        if (!isNoted)
            m_wordsReadAgain.append(WordReadAgain { load, known });
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
        if (value->kind() != other->kind() || !isComparedByOperands(value))
            return false;
        if (!childrenAreSameOnPath(value, other, depth))
            return false;
        if (!isLoad(value->opcode()))
            return true;
        MemoryValue* load = value->as<MemoryValue>();
        MemoryValue* known = other->as<MemoryValue>();
        if (known->hasFence() || load->offset() != known->offset() || load->range() != known->range() || !readsSameWordOnPath(load, known))
            return false;
        m_didCompareLoads = true;
        noteWordReadAgain(load, known);
        return true;
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
        if (!isComparedByOperands(value))
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
        TriState result = evaluateBitsOfNonNaNDouble(test.value, known);
        if (result != TriState::Indeterminate)
            return taken(result == TriState::True);
        return TriState::Indeterminate;
    }

    // A double that equals itself is not NaN, and the bits of a non-NaN double, read as a signed integer, never exceed those of +Infinity.
    TriState evaluateBitsOfNonNaNDouble(Value* test, const DominatingCondition& known)
    {
        bool saysEqual = known.value->opcode() == Equal ? known.isNonZero : known.value->opcode() == NotEqual && !known.isNonZero;
        Value* number = saysEqual ? known.value->child(0) : nullptr;
        if (!number || number != known.value->child(1) || number->type() != Double || !isComparisonWithConstant(test))
            return TriState::Indeterminate;
        Value* bits = valueOnPath(test->child(0));
        if (!bits || bits->opcode() != BitwiseCast || !isSameOnPath(bits->child(0), number, m_maxTestDepth))
            return TriState::Indeterminate;
        IntRange nonNaNBits(std::numeric_limits<int64_t>::min(), std::bit_cast<int64_t>(std::numeric_limits<double>::infinity()));
        return evaluateComparison(test->opcode(), nonNaNBits, test->child(1)->asInt());
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
            m_didCompareLoads = false;
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
    bool m_hasLoop { false };
    IndexMap<Value*, uint8_t> m_branchCountOn;
    IndexSet<BasicBlock*> m_tailsOnlyForDecidedTests;
    IndexMap<BasicBlock*, CopyCost> m_pathCopyCost;
    Vector<Path> m_paths;
    IndexMap<BasicBlock*, std::optional<unsigned>> m_pathAfter;
    Vector<BasicBlock*, 8> m_path;
    BasicBlock* m_predecessor { nullptr };
    BasicBlock* m_blockLeftToPredecessor { nullptr };
    Vector<WordReadAgain, 2> m_wordsReadAgain;
    UncheckedKeyHashMap<Value*, Variable*> m_variableOfEarlierLoad;
    bool m_didCompareLoads { false };
    bool m_hasPassedWordReadAgain { false };
    IndexMap<BasicBlock*, std::optional<HeapRange>> m_boundsOfWrites;
    UncheckedKeyHashMap<uint64_t, bool, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_isUnchangedAtEnd;
    Vector<BasicBlock*, 16> m_walk;
    BitVector m_isOnWalk;
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
