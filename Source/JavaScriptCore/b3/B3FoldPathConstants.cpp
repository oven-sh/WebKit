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
#include "B3FoldPathConstants.h"

#if ENABLE(B3_JIT)

#include "B3BasicBlockInlines.h"
#include "B3CaseCollectionInlines.h"
#include "B3DominatingConditions.h"
#include "B3Dominators.h"
#include "B3InsertionSet.h"
#include "B3IntRange.h"
#include "B3PhaseScope.h"
#include "B3SwitchValue.h"
#include "B3ValueInlines.h"
#include <wtf/IndexMap.h>
#include <wtf/IndexSet.h>

namespace JSC { namespace B3 {

namespace {

namespace B3FoldPathConstantsInternal {
static constexpr bool verbose = false;
}

class FoldPathConstants {
public:
    FoldPathConstants(Procedure& proc)
        : m_proc(proc)
        , m_insertionSet(proc)
    {
    }

    void run()
    {
        bool changed = false;

        if (B3FoldPathConstantsInternal::verbose)
            dataLog("B3 before folding path constants: \n", m_proc, "\n");
        
        // Find all of the values that are the subject of a branch or switch. For any successor
        // that we dominate, install a value override at that block.

        UncheckedKeyHashMap<Value*, Vector<Override>> overrides;

        Dominators& dominators = m_proc.dominators();
        
        auto addOverride = [&] (
            BasicBlock* from, Value* value, const Override& override) {

            if (override.block->numPredecessors() != 1)
                return;
            ASSERT(override.block->predecessor(0) == from);

            Vector<Override>& forValue =
                overrides.add(value, Vector<Override>()).iterator->value;

            if (ASSERT_ENABLED) {
                for (const Override& otherOverride : forValue)
                    ASSERT_UNUSED(otherOverride, otherOverride.block != override.block);
            }

            if (B3FoldPathConstantsInternal::verbose)
                dataLog("Overriding ", *value, " from ", *from, ": ", override, "\n");
            
            forValue.append(override);
        };
        
        for (BasicBlock* block : m_proc) {
            Value* branch = block->last();
            switch (branch->opcode()) {
            case Branch:
                if (block->successorBlock(0) == block->successorBlock(1))
                    continue;
                addOverride(
                    block, branch->child(0),
                    branch->child(0)->returnsBool()
                        ? Override::constant(block->successorBlock(0), 1)
                        : Override::nonZero(block->successorBlock(0)));
                addOverride(
                    block, branch->child(0),
                    Override::constant(block->successorBlock(1), 0));
                break;
            case Switch: {
                SwitchValue* switchValue = branch->as<SwitchValue>();

                UncheckedKeyHashMap<BasicBlock*, unsigned> targetUses;
                for (SwitchCase switchCase : switchValue->cases(block))
                    targetUses.add(switchCase.targetBlock(), 0).iterator->value++;
                targetUses.add(switchValue->fallThrough(block), 0).iterator->value++;

                for (SwitchCase switchCase : switchValue->cases(block)) {
                    if (targetUses.find(switchCase.targetBlock())->value != 1)
                        continue;

                    addOverride(
                        block, branch->child(0),
                        Override::constant(switchCase.targetBlock(), switchCase.caseValue()));
                }
                break;
            }
            default:
                break;
            }
        }

        // Install the constants in the override blocks. We use one-shot insertion sets because
        // each block will get at most one thing inserted into it anyway.
        for (auto& entry : overrides) {
            for (Override& override : entry.value) {
                if (!override.hasValue)
                    continue;
                override.valueNode =
                    m_insertionSet.insertIntConstant(0, entry.key, override.value);
                m_insertionSet.execute(override.block);
            }
        }

        // Replace all uses of a value that has an override with that override, if appropriate.
        // Certain instructions get special treatment.
        auto getOverride = [&] (BasicBlock* block, Value* value) -> Override {
            auto iter = overrides.find(value);
            if (iter == overrides.end())
                return Override();

            Vector<Override>& forValue = iter->value;
            Override result;
            for (Override& override : forValue) {
                if (dominators.dominates(override.block, block)
                    && override.isBetterThan(result))
                    result = override;
            }

            if (B3FoldPathConstantsInternal::verbose)
                dataLog("In block ", *block, " getting override for ", *value, ": ", result, "\n");

            return result;
        };
        
        for (BasicBlock* block : m_proc) {
            for (unsigned valueIndex = 0; valueIndex < block->size(); ++valueIndex) {
                Value* value = block->at(valueIndex);

                switch (value->opcode()) {
                case Branch: {
                    if (getOverride(block, value->child(0)).isNonZero) {
                        value->replaceWithJump(block, block->taken());
                        changed = true;
                    }
                    break;
                }

                case Equal: {
                    if (value->child(1)->isInt(0)
                        && getOverride(block, value->child(0)).isNonZero) {
                        value->replaceWithIdentity(
                            m_insertionSet.insertIntConstant(valueIndex, value, 0));
                    }
                    break;
                }

                case NotEqual: {
                    if (value->child(1)->isInt(0)
                        && getOverride(block, value->child(0)).isNonZero) {
                        value->replaceWithIdentity(
                            m_insertionSet.insertIntConstant(valueIndex, value, 1));
                    }
                    break;
                }

                default:
                    break;
                }

                for (Value*& child : value->children()) {
                    Override override = getOverride(block, child);
                    if (override.valueNode)
                        child = override.valueNode;
                }
            }
            m_insertionSet.execute(block);
        }

        if (changed) {
            m_proc.resetReachability();
            m_proc.invalidateCFG();
        }
    }
    
private:
    struct Override {
        Override()
        {
        }

        static Override LEGACY_NODELETE constant(BasicBlock* block, int64_t value)
        {
            Override result;
            result.block = block;
            result.hasValue = true;
            result.value = value;
            if (value)
                result.isNonZero = true;
            return result;
        }

        static Override LEGACY_NODELETE nonZero(BasicBlock* block)
        {
            Override result;
            result.block = block;
            result.isNonZero = true;
            return result;
        }

        bool LEGACY_NODELETE isBetterThan(const Override& override)
        {
            if (hasValue && !override.hasValue)
                return true;
            if (isNonZero && !override.isNonZero)
                return true;
            return false;
        }

        void dump(PrintStream& out) const
        {
            out.print("{block = ", pointerDump(block), ", value = ");
            if (hasValue)
                out.print(value);
            else
                out.print("<none>");
            out.print(", isNonZero = ", isNonZero);
            if (valueNode)
                out.print(", valueNode = ", *valueNode);
            out.print("}");
        }

        BasicBlock* block { nullptr };
        bool hasValue { false };
        bool isNonZero { false };
        int64_t value { 0 };
        Value* valueNode { nullptr };
    };

    Procedure& m_proc;
    InsertionSet m_insertionSet;
};

class FoldPathConstantsInValueOrder {
public:
    FoldPathConstantsInValueOrder(Procedure& proc)
        : m_proc(proc)
        , m_insertionSet(proc)
        , m_foldsImpliedComparisons(proc.policy().optimization.foldsImpliedComparisons)
    {
    }

    void run()
    {
        if (B3FoldPathConstantsInternal::verbose)
            dataLog("B3 before folding path constants: \n", m_proc, "\n");

        collectOverrides();
        if (m_overrides.isEmpty())
            return;

        m_dominators = &m_proc.dominators();
        sortOverrides();
        findRunsOfDecidedComparisons();

        bool changed = applyOverrides();

        if (changed) {
            m_proc.resetReachability();
            m_proc.invalidateCFG();
        }
    }
    
private:
    struct Override {
        Override()
        {
        }

        static Override LEGACY_NODELETE constant(BasicBlock* block, int64_t value)
        {
            Override result;
            result.block = block;
            result.hasValue = true;
            result.value = value;
            if (value)
                result.isNonZero = true;
            return result;
        }

        static Override LEGACY_NODELETE nonZero(BasicBlock* block)
        {
            Override result;
            result.block = block;
            result.isNonZero = true;
            return result;
        }

        bool LEGACY_NODELETE isBetterThan(const Override& override)
        {
            if (hasValue && !override.hasValue)
                return true;
            if (isNonZero && !override.isNonZero)
                return true;
            return false;
        }

        void dump(PrintStream& out) const
        {
            out.print("{block = ", pointerDump(block), ", value = ");
            if (hasValue)
                out.print(value);
            else
                out.print("<none>");
            out.print(", isNonZero = ", isNonZero);
            if (valueNode)
                out.print(", valueNode = ", *valueNode);
            out.print("}");
        }

        BasicBlock* block { nullptr };
        bool hasValue { false };
        bool isNonZero { false };
        int64_t value { 0 };
        Value* valueNode { nullptr };
    };

    struct DecidedComparison {
        Value* comparison { nullptr };
        IntRange operandRange;
        Override falseOverride;
        Override trueOverride;
        // The position behind the entries that follow this one in its list and have the same comparison.
        unsigned endOfRun { 0 };
    };

    // A node of a tree over the positions of a list of decided comparisons. Inserting a comparison copies the path to its position.
    struct RangeTreeNode {
        unsigned left { 0 };
        unsigned right { 0 };
        int64_t leastMax { std::numeric_limits<int64_t>::max() };
        int64_t greatestMin { std::numeric_limits<int64_t>::min() };
    };

    // What holds from a time of the walk down the dominator tree until the next entry's time.
    struct Version {
        unsigned time { 0 };
        unsigned state { 0 };
    };

    struct VersionForConstant {
        int64_t constant { 0 };
        Version version;
    };

    struct DecidedComparisons {
        Vector<DecidedComparison> list;
        bool isIndexed { false };
        // The state is the root of the tree of the comparisons that are decided at that time.
        Vector<Version> rangeTrees;
        // The state is one more than the position of the first comparison decided at that time that leaves the operand that one value. Sorted by constant, then time.
        Vector<VersionForConstant> firstWithOnlyValue;
    };

    // For each kind of override of one value, one more than the position in m_overrides of the first that holds at that time.
    struct OverrideVersions {
        Vector<Version> firstNonZeroConstant;
        Vector<Version> firstZero;
        Vector<Version> firstNonZero;
    };

    static bool canHoldOverrides(BasicBlock* block)
    {
        return block->numPredecessors() == 1;
    }

    void addOverride(Value* value, const Override& override)
    {
        if (value->isConstant() || !canHoldOverrides(override.block))
            return;
        m_overrides.append({ value, override });
    }

    void addDecidedComparison(BasicBlock* block, Value* condition)
    {
        if (!m_foldsImpliedComparisons || !isComparisonWithConstant(condition))
            return;
        for (bool holds : { true, false }) {
            BasicBlock* successor = block->successorBlock(holds ? 0 : 1);
            if (!canHoldOverrides(successor))
                continue;
            auto& decidedComparisons = m_decidedComparisons.add(condition->child(0), DecidedComparisons()).iterator->value;
            decidedComparisons.list.append(DecidedComparison { condition, rangeImpliedBy(condition, holds), Override::constant(successor, 0), Override::constant(successor, 1) });
        }
    }

    void collectOverrides()
    {
        for (BasicBlock* block : m_proc) {
            Value* branch = block->last();
            switch (branch->opcode()) {
            case Branch:
                if (block->successorBlock(0) == block->successorBlock(1))
                    continue;
                addOverride(branch->child(0), branch->child(0)->returnsBool() ? Override::constant(block->successorBlock(0), 1) : Override::nonZero(block->successorBlock(0)));
                addOverride(branch->child(0), Override::constant(block->successorBlock(1), 0));
                addDecidedComparison(block, branch->child(0));
                break;
            case Switch: {
                SwitchValue* switchValue = branch->as<SwitchValue>();

                UncheckedKeyHashMap<BasicBlock*, unsigned> targetUses;
                for (SwitchCase switchCase : switchValue->cases(block))
                    targetUses.add(switchCase.targetBlock(), 0).iterator->value++;
                targetUses.add(switchValue->fallThrough(block), 0).iterator->value++;

                for (SwitchCase switchCase : switchValue->cases(block)) {
                    if (targetUses.find(switchCase.targetBlock())->value != 1)
                        continue;
                    addOverride(branch->child(0), Override::constant(switchCase.targetBlock(), switchCase.caseValue()));
                }
                break;
            }
            default:
                break;
            }
        }
    }

    void sortOverrides()
    {
        std::stable_sort(m_overrides.begin(), m_overrides.end(), [] (const auto& a, const auto& b) {
            return a.first->index() < b.first->index();
        });
        m_firstOverride.fill(std::nullopt, m_proc.values().size());
        for (unsigned i = m_overrides.size(); i--;)
            m_firstOverride[m_overrides[i].first->index()] = i;
    }

    void findRunsOfDecidedComparisons()
    {
        for (auto& decidedComparisons : m_decidedComparisons.values()) {
            auto& list = decidedComparisons.list;
            for (unsigned position = list.size(); position--;) {
                bool isFollowedByItsLike = position + 1 < list.size() && list[position + 1].comparison == list[position].comparison;
                list[position].endOfRun = isFollowedByItsLike ? list[position + 1].endOfRun : position + 1;
            }
        }
    }

    Override* findOverrideOfValue(BasicBlock* block, Value* value)
    {
        unsigned index = value->index();
        if (index >= m_firstOverride.size() || !m_firstOverride[index])
            return nullptr;
        unsigned first = *m_firstOverride[index];
        bool hasManyOverrides = m_maxEntriesToScan < m_overrides.size() - first && m_overrides[first + m_maxEntriesToScan].first == value;
        if (hasManyOverrides && isInDominatorTree(block)) {
            if (std::optional<Override*> indexed = findOverrideOfValueInIndex(block, value, first)) {
                ASSERT(*indexed == findOverrideOfValueInList(block, value, first));
                return *indexed;
            }
        }
        return findOverrideOfValueInList(block, value, first);
    }

    Override* findOverrideOfValueInList(BasicBlock* block, Value* value, unsigned first)
    {
        Override* result = nullptr;
        for (unsigned i = first; i < m_overrides.size() && m_overrides[i].first == value; ++i) {
            Override& override = m_overrides[i].second;
            if (m_dominators->dominates(override.block, block) && override.isBetterThan(result ? *result : Override()))
                result = &override;
        }
        return result;
    }

    // Gives what findOverrideOfValueInList() gives: nothing replaces a nonzero constant, and an override replaces no other of its kind.
    // Where the value is both zero and nonzero the two kinds replace each other in turn: that is left to the list.
    std::optional<Override*> findOverrideOfValueInIndex(BasicBlock* block, Value* value, unsigned first)
    {
        auto added = m_overrideVersions.add(value, OverrideVersions());
        OverrideVersions& versions = added.iterator->value;
        if (added.isNewEntry) {
            m_proc.didReachLimit(Limit::MaxPathConstantsScanned, value->origin());
            indexOverrides(versions, value, first);
        }
        unsigned time = m_timeOfEntry[block];
        auto overrideAt = [&](unsigned state) {
            return state ? &m_overrides[state - 1].second : nullptr;
        };
        if (unsigned nonZeroConstant = stateAt(versions.firstNonZeroConstant.span(), time))
            return overrideAt(nonZeroConstant);
        unsigned zero = stateAt(versions.firstZero.span(), time);
        unsigned nonZero = stateAt(versions.firstNonZero.span(), time);
        if (zero && nonZero)
            return std::nullopt;
        return overrideAt(zero | nonZero);
    }

    void indexOverrides(OverrideVersions& versions, Value* value, unsigned first)
    {
        auto blockAt = [&](unsigned position) {
            return m_overrides[position].second.block;
        };
        Vector<unsigned> positions;
        for (unsigned position = first; position < m_overrides.size() && m_overrides[position].first == value; ++position) {
            if (m_timeOfEntry[blockAt(position)] != unreachedTime)
                positions.append(position);
        }
        std::sort(positions.begin(), positions.end(), [&](unsigned a, unsigned b) {
            return m_timeOfEntry[blockAt(a)] < m_timeOfEntry[blockAt(b)];
        });
        auto indexKind = [&](Vector<Version>& result, bool hasValue, bool isNonZero) {
            Vector<unsigned> positionsOfKind;
            for (unsigned position : positions) {
                const Override& override = m_overrides[position].second;
                if (override.hasValue == hasValue && override.isNonZero == isNonZero)
                    positionsOfKind.append(position);
            }
            forEachVersion(blockAt, positionsOfKind.span(), keepFirstPosition, [&](Version version) {
                result.append(version);
            });
        };
        indexKind(versions.firstNonZeroConstant, true, true);
        indexKind(versions.firstZero, true, false);
        indexKind(versions.firstNonZero, false, true);
    }

    static unsigned keepFirstPosition(unsigned first, unsigned position)
    {
        return first ? std::min(first, position + 1) : position + 1;
    }

    Override* findOverride(BasicBlock* block, Value* value)
    {
        Override* result = findOverrideOfValue(block, value);
        if (m_decidedComparisons.isEmpty() || !isComparisonWithConstant(value))
            return result;
        auto found = m_decidedComparisons.find(value->child(0));
        if (found == m_decidedComparisons.end())
            return result;
        if (found->value.list.size() > m_maxEntriesToScan && canUseIndex(block, value)) {
            Override* indexed = findImpliedOverrideInIndex(found->value, block, value, result);
            ASSERT(indexed == findImpliedOverrideInList(found->value.list, block, value, result));
            return indexed;
        }
        return findImpliedOverrideInList(found->value.list, block, value, result);
    }

    Override* findImpliedOverrideInList(Vector<DecidedComparison>& list, BasicBlock* block, Value* value, Override* result)
    {
        for (unsigned position = 0; position < list.size(); ++position) {
            DecidedComparison& decided = list[position];
            if (decided.comparison == value) {
                position = decided.endOfRun - 1;
                continue;
            }
            if (!m_dominators->dominates(decided.trueOverride.block, block))
                continue;
            TriState outcome = evaluateComparison(value->opcode(), decided.operandRange, value->child(1)->asInt());
            if (outcome == TriState::Indeterminate)
                continue;
            Override& override = outcome == TriState::True ? decided.trueOverride : decided.falseOverride;
            if (override.isBetterThan(result ? *result : Override()))
                result = &override;
        }
        return result;
    }

    // With the least or the greatest value of the type, a comparison can be decided against its own outcome, which the list passes over.
    bool canUseIndex(BasicBlock* block, Value* value)
    {
        IntRange top = IntRange::top(value->child(0)->type());
        int64_t constant = value->child(1)->asInt();
        if (constant == top.min() || constant == top.max())
            return false;
        return isInDominatorTree(block);
    }

    bool isInDominatorTree(BasicBlock* block)
    {
        numberBlocksInDominatorTree();
        return m_timeOfEntry[block] != unreachedTime;
    }

    void numberBlocksInDominatorTree()
    {
        if (m_timeOfEntry.size())
            return;
        m_timeOfEntry.resize(m_proc.size());
        m_timeOfExit.resize(m_proc.size());
        for (BasicBlock* block : m_proc)
            m_timeOfEntry[block] = m_timeOfExit[block] = unreachedTime;
        unsigned time = 0;
        m_dominators->forAllBlocksInDominatorTreePreOrder([&](BasicBlock* block) {
            m_timeOfEntry[block] = time++;
        }, [&](BasicBlock* block) {
            m_timeOfExit[block] = time++;
        });
    }

    // Goes through entries in the order in which the walk down the dominator tree enters their blocks. Each is added to the state of the
    // nearest one whose block dominates its block. Calls record() wherever the state changes.
    void forEachVersion(NOESCAPE const Invocable<BasicBlock*(unsigned position)> auto& blockAt, std::span<const unsigned> positionsByTimeOfEntry, NOESCAPE const Invocable<unsigned(unsigned state, unsigned position)> auto& add, NOESCAPE const Invocable<void(Version)> auto& record)
    {
        Vector<Version, 16> entered;
        auto leaveUntil = [&](unsigned time) {
            while (!entered.isEmpty() && entered.last().time < time) {
                unsigned timeOfExit = entered.takeLast().time;
                record(Version { timeOfExit, entered.isEmpty() ? 0 : entered.last().state });
            }
        };
        for (unsigned position : positionsByTimeOfEntry) {
            BasicBlock* block = blockAt(position);
            leaveUntil(m_timeOfEntry[block]);
            unsigned state = add(entered.isEmpty() ? 0 : entered.last().state, position);
            entered.append(Version { m_timeOfExit[block], state });
            record(Version { m_timeOfEntry[block], state });
        }
        leaveUntil(std::numeric_limits<unsigned>::max());
    }

    unsigned insertIntoRangeTree(unsigned root, unsigned begin, unsigned end, unsigned position, const IntRange& range)
    {
        RangeTreeNode node = m_rangeTreeNodes[root];
        node.leastMax = std::min(node.leastMax, range.max());
        node.greatestMin = std::max(node.greatestMin, range.min());
        if (end - begin > 1) {
            unsigned middle = begin + (end - begin) / 2;
            if (position < middle)
                node.left = insertIntoRangeTree(node.left, begin, middle, position, range);
            else
                node.right = insertIntoRangeTree(node.right, middle, end, position, range);
        }
        m_rangeTreeNodes.append(node);
        return m_rangeTreeNodes.size() - 1;
    }

    std::optional<unsigned> findFirstInRangeTree(unsigned root, unsigned size, NOESCAPE const Invocable<bool(const RangeTreeNode&)> auto& hasMatch)
    {
        if (!hasMatch(m_rangeTreeNodes[root]))
            return std::nullopt;
        unsigned begin = 0;
        unsigned end = size;
        while (end - begin > 1) {
            unsigned middle = begin + (end - begin) / 2;
            unsigned left = m_rangeTreeNodes[root].left;
            if (hasMatch(m_rangeTreeNodes[left])) {
                root = left;
                end = middle;
            } else {
                root = m_rangeTreeNodes[root].right;
                begin = middle;
            }
        }
        return begin;
    }

    void indexDecidedComparisons(DecidedComparisons& decidedComparisons)
    {
        decidedComparisons.isIndexed = true;
        const Vector<DecidedComparison>& list = decidedComparisons.list;
        if (m_rangeTreeNodes.isEmpty())
            m_rangeTreeNodes.append(RangeTreeNode());

        auto blockAt = [&](unsigned position) {
            return list[position].trueOverride.block;
        };
        Vector<unsigned> positions;
        for (unsigned position = 0; position < list.size(); ++position) {
            if (m_timeOfEntry[blockAt(position)] != unreachedTime)
                positions.append(position);
        }
        std::sort(positions.begin(), positions.end(), [&](unsigned a, unsigned b) {
            return m_timeOfEntry[blockAt(a)] < m_timeOfEntry[blockAt(b)];
        });
        forEachVersion(blockAt, positions.span(), [&](unsigned root, unsigned position) {
            return insertIntoRangeTree(root, 0, list.size(), position, list[position].operandRange);
        }, [&](Version version) {
            decidedComparisons.rangeTrees.append(version);
        });

        positions.removeAllMatching([&](unsigned position) {
            return list[position].operandRange.min() != list[position].operandRange.max();
        });
        std::stable_sort(positions.begin(), positions.end(), [&](unsigned a, unsigned b) {
            return list[a].operandRange.min() < list[b].operandRange.min();
        });
        for (unsigned begin = 0; begin < positions.size();) {
            int64_t constant = list[positions[begin]].operandRange.min();
            unsigned end = begin;
            while (end < positions.size() && list[positions[end]].operandRange.min() == constant)
                ++end;
            forEachVersion(blockAt, positions.span().subspan(begin, end - begin), keepFirstPosition, [&](Version version) {
                decidedComparisons.firstWithOnlyValue.append(VersionForConstant { constant, version });
            });
            begin = end;
        }
    }

    static unsigned stateAt(std::span<const Version> versions, unsigned time)
    {
        auto after = std::upper_bound(versions.begin(), versions.end(), time, [](unsigned time, const Version& version) {
            return time < version.time;
        });
        return after == versions.begin() ? 0 : (after - 1)->state;
    }

    static unsigned stateAt(std::span<const VersionForConstant> versions, int64_t constant, unsigned time)
    {
        auto after = std::upper_bound(versions.begin(), versions.end(), std::pair { constant, time }, [](const auto& key, const VersionForConstant& entry) {
            return key < std::pair { entry.constant, entry.version.time };
        });
        if (after == versions.begin() || (after - 1)->constant != constant)
            return 0;
        return (after - 1)->version.state;
    }

    // The first comparison of the list that is decided in the block and gives the comparison with the constant that outcome.
    std::optional<unsigned> findFirstThatImplies(DecidedComparisons& decidedComparisons, BasicBlock* block, Opcode opcode, int64_t constant, bool outcome)
    {
        unsigned time = m_timeOfEntry[block];
        auto findRange = [&](NOESCAPE const auto& hasMatch) {
            return findFirstInRangeTree(stateAt(decidedComparisons.rangeTrees.span(), time), decidedComparisons.list.size(), hasMatch);
        };
        auto findBelow = [&] {
            return findRange([&](const RangeTreeNode& node) { return node.leastMax < constant; });
        };
        auto findAtMost = [&] {
            return findRange([&](const RangeTreeNode& node) { return node.leastMax <= constant; });
        };
        auto findAbove = [&] {
            return findRange([&](const RangeTreeNode& node) { return node.greatestMin > constant; });
        };
        auto findAtLeast = [&] {
            return findRange([&](const RangeTreeNode& node) { return node.greatestMin >= constant; });
        };
        switch (opcode) {
        case LessThan:
            return outcome ? findBelow() : findAtLeast();
        case GreaterThan:
            return outcome ? findAbove() : findAtMost();
        case LessEqual:
            return outcome ? findAtMost() : findAbove();
        case GreaterEqual:
            return outcome ? findAtLeast() : findBelow();
        case Equal:
        case NotEqual: {
            if (outcome == (opcode == NotEqual)) {
                return findRange([&](const RangeTreeNode& node) {
                    return node.leastMax < constant || node.greatestMin > constant;
                });
            }
            unsigned first = stateAt(decidedComparisons.firstWithOnlyValue.span(), constant, time);
            if (!first)
                return std::nullopt;
            return first - 1;
        }
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
    }

    // Gives what findImpliedOverrideInList() gives. A comparison that is true replaces everything but a nonzero constant, and one that is false only replaces what has no value.
    Override* findImpliedOverrideInIndex(DecidedComparisons& decidedComparisons, BasicBlock* block, Value* value, Override* result)
    {
        if (result && result->hasValue && result->isNonZero)
            return result;
        if (!decidedComparisons.isIndexed) {
            m_proc.didReachLimit(Limit::MaxPathConstantsScanned, value->origin());
            indexDecidedComparisons(decidedComparisons);
        }
        int64_t constant = value->child(1)->asInt();
        if (std::optional<unsigned> position = findFirstThatImplies(decidedComparisons, block, value->opcode(), constant, true))
            return &decidedComparisons.list[*position].trueOverride;
        if (result && result->hasValue)
            return result;
        if (std::optional<unsigned> position = findFirstThatImplies(decidedComparisons, block, value->opcode(), constant, false))
            return &decidedComparisons.list[*position].falseOverride;
        return result;
    }

    Override getOverride(BasicBlock* block, Value* value)
    {
        Override* result = findOverride(block, value);
        return result ? *result : Override();
    }

    bool applyOverrides()
    {
        bool changed = false;
        Vector<Override*, 16> overridesWithNewConstants;

        for (BasicBlock* block : m_proc) {
            for (unsigned valueIndex = 0; valueIndex < block->size(); ++valueIndex) {
                Value* value = block->at(valueIndex);

                switch (value->opcode()) {
                case Branch: {
                    if (getOverride(block, value->child(0)).isNonZero) {
                        value->replaceWithJump(block, block->taken());
                        changed = true;
                    }
                    break;
                }

                case Equal: {
                    if (value->child(1)->isInt(0) && getOverride(block, value->child(0)).isNonZero)
                        value->replaceWithIdentity(m_insertionSet.insertIntConstant(valueIndex, value, 0));
                    break;
                }

                case NotEqual: {
                    if (value->child(1)->isInt(0) && getOverride(block, value->child(0)).isNonZero)
                        value->replaceWithIdentity(m_insertionSet.insertIntConstant(valueIndex, value, 1));
                    break;
                }

                case Select: {
                    if (m_foldsImpliedComparisons && getOverride(block, value->child(0)).isNonZero)
                        value->replaceWithIdentity(value->child(1));
                    break;
                }

                default:
                    break;
                }

                for (Value*& child : value->children()) {
                    if (m_foldsImpliedComparisons) {
                        bool isCast = child->opcode() == BitwiseCast;
                        Value* select = isCast ? child->child(0) : child;
                        if (select->opcode() == Select) {
                            Override decided = getOverride(block, select->child(0));
                            if (decided.isNonZero || decided.hasValue) {
                                Value* chosen = select->child(decided.isNonZero ? 1 : 2);
                                child = isCast ? m_insertionSet.insert<Value>(valueIndex, BitwiseCast, child->origin(), chosen) : chosen;
                            }
                        }
                    }
                    Override* override = findOverride(block, child);
                    if (!override || !override->hasValue)
                        continue;
                    if (!override->valueNode) {
                        override->valueNode = m_proc.addIntConstant(child->origin(), child->type(), override->value);
                        overridesWithNewConstants.append(override);
                    }
                    child = override->valueNode;
                }
            }
            m_insertionSet.execute(block);
        }
        for (Override* override : overridesWithNewConstants) {
            m_insertionSet.insertValue(0, override->valueNode);
            m_insertionSet.execute(override->block);
        }
        return changed;
    }

    Procedure& m_proc;
    InsertionSet m_insertionSet;
    const bool m_foldsImpliedComparisons;
    const uint64_t m_maxEntriesToScan { m_proc.limit(Limit::MaxPathConstantsScanned) };
    Dominators* m_dominators { nullptr };
    Vector<std::pair<Value*, Override>, 32> m_overrides;
    UncheckedKeyHashMap<Value*, DecidedComparisons> m_decidedComparisons;
    UncheckedKeyHashMap<Value*, OverrideVersions> m_overrideVersions;
    static constexpr unsigned unreachedTime = std::numeric_limits<unsigned>::max();
    IndexMap<BasicBlock*, unsigned> m_timeOfEntry;
    IndexMap<BasicBlock*, unsigned> m_timeOfExit;
    Vector<RangeTreeNode> m_rangeTreeNodes;
    Vector<std::optional<unsigned>, 512> m_firstOverride;
};

} // anonymous namespace

void foldPathConstants(Procedure& proc)
{
    PhaseScope phaseScope(proc, "foldPathConstants"_s);
    if (proc.policy().optimization.foldsImpliedComparisons || proc.policy().addressing.relocationLowering) {
        FoldPathConstantsInValueOrder foldPathConstants(proc);
        foldPathConstants.run();
        return;
    }
    FoldPathConstants foldPathConstants(proc);
    foldPathConstants.run();
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)

