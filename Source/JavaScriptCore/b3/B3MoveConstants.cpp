/*
 * Copyright (C) 2015-2017 Apple Inc. All rights reserved.
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
#include "B3MoveConstants.h"

#include "AirArg.h"
#include <wtf/Scope.h>

#if ENABLE(B3_JIT)

#include "B3Dominators.h"
#include "B3InsertionSetInlines.h"
#include "B3PhaseScope.h"
#include "B3ProcedureInlines.h"
#include "B3ValueInlines.h"
#include <wtf/HashMap.h>
#include <wtf/Vector.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace B3 {

namespace {

class MoveConstants {
public:
    MoveConstants(Procedure& proc)
        : m_proc(proc)
        , m_insertionSet(proc)
    {
    }

    void run()
    {
        bool mayHaveWideConstants = !m_proc.positionIndependent();
        for (Value* value : m_proc.values()) {
            if (mayHaveWideConstants)
                break;
            mayHaveWideConstants = value->opcode() == ConstDouble || value->opcode() == Const128;
        }
        if (mayHaveWideConstants) {
            hoistConstants(
                [&] (const Value* value) -> bool {
                    return value->opcode() == ConstDouble || value->opcode() == Const128;
                });

            lowerMaterializationCostHeavyConstants();
        }

        hoistConstants(
            [&] (const Value* value) -> bool {
                return value->opcode() == Const32 || value->opcode() == ConstFloat || value->opcode() == Const64 || value->opcode() == ArgumentReg;
            });
    }

private:
    using ValuesForConstant = UncheckedKeyHashMap<ValueKey, Vector<Value*, 1>>;

    struct UseOfConstant {
        BasicBlock* block;
        Value* user;
        Value** child;
        unsigned indexOfUser;
    };

    struct PlaceOfConstant {
        BasicBlock* block;
        unsigned index;
    };

    // On the dominator tree above the uses of a constant, bottom up: materialize it at a block, or separately in the subtrees below the
    // block, whichever runs less often in total.
    template<typename Filter>
    void placeByFrequency(NOESCAPE const Filter& filter, const Vector<UseOfConstant>& usesInOrder, IndexMap<BasicBlock*, Vector<Value*>>& materializations, UncheckedKeyHashMap<Value*, BasicBlock*>& materializationBlocks, ValuesForConstant& valuesForConstant)
    {
        Dominators& dominators = m_proc.dominators();

        struct Use {
            BasicBlock* block;
            Value** child;
        };
        auto isTakenAsImmediate = [](const Value* user, const Value* child) {
            if (!child->hasInt() || user->numChildren() != 2 || (user->child(0)->hasInt() && user->child(1)->hasInt()))
                return false;
            switch (user->opcode()) {
            case Sub:
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
                if (child != user->child(1))
                    return false;
                [[fallthrough]];
            case Add:
                return Air::Arg::isValidImmForm(child->asInt());
            case BitAnd:
            case BitOr:
            case BitXor:
                return user->type() == Int64 ? Air::Arg::isValidBitImm64Form(child->asInt()) : Air::Arg::isValidBitImmForm(child->asInt());
            case Shl:
            case SShr:
            case ZShr:
            case RotR:
            case RotL:
                return child == user->child(1);
            default:
                return false;
            }
        };
        constexpr unsigned blocksServedByOneConstant = 1024;
        constexpr unsigned mostBlocksServedAsOne = 8192;
        IndexMap<BasicBlock*, unsigned> stretch(m_proc.size(), 1u);
        if (isARM64() && m_proc.size() > mostBlocksServedAsOne) {
            unsigned next = 0;
            for (BasicBlock* block : m_proc.blocksInPreOrder())
                stretch[block] = 1 + next++ / blocksServedByOneConstant;
        }
        UncheckedKeyHashMap<std::pair<ValueKey, unsigned>, unsigned> indexForConstant;
        auto take = [&](Value* original) {
            return materializationBlocks.contains(original) ? m_proc.clone(original) : original;
        };
        Vector<Vector<Use, 2>> usesOfConstants;
        Vector<Vector<Use, 2>> usesAsImmediates;
        for (const UseOfConstant& use : usesInOrder) {
            Value* child = *use.child;
            if (!filter(child))
                continue;
            auto result = indexForConstant.add({ child->key(), stretch[use.block] }, usesOfConstants.size());
            if (result.isNewEntry) {
                usesOfConstants.append(Vector<Use, 2> { });
                usesAsImmediates.append(Vector<Use, 2> { });
            }
            (isTakenAsImmediate(use.user, child) ? usesAsImmediates : usesOfConstants)[result.iterator->value].append(Use { use.block, use.child });
        }

        IndexMap<BasicBlock*, unsigned> preOrderIndex(m_proc.size(), 0u);
        // Among a block and its dominators: the one that runs least often, the nearest of those that tie. For a constant that is used in one block alone the walk below comes to that. The walk is as long as the
        // dominator tree is deep: in a function that makes a table of n records that is n, for each of n constants.
        IndexMap<BasicBlock*, BasicBlock*> leastFrequentDominator(m_proc.size(), nullptr);
        unsigned nextPreOrderIndex = 0;
        bool hasFrequencyThatIsNoNumber = false;
        for (BasicBlock* block : m_proc.blocksInPreOrder()) {
            preOrderIndex[block] = nextPreOrderIndex++;
            hasFrequencyThatIsNoNumber |= std::isnan(block->frequency());
            BasicBlock* parent = dominators.idom(block);
            BasicBlock* above = parent ? leastFrequentDominator[parent] : nullptr;
            leastFrequentDominator[block] = above && above->frequency() < block->frequency() ? above : block;
        }
        // Past a dominator whose frequency is no number the walk hands on what has come up, and a block above it may still be chosen; this table would end at it. Then nobody goes by the table.
        if (hasFrequencyThatIsNoNumber)
            leastFrequentDominator.clear(nullptr);

        constexpr unsigned maxStepsOneByOne = 32;
        IndexMap<BasicBlock*, BasicBlock*> jump;
        IndexMap<BasicBlock*, BasicBlock*> leastBelowJump;
        IndexMap<BasicBlock*, unsigned> depth;
        IndexMap<BasicBlock*, unsigned> preOrderIndexInDominatorTree;
        auto prepareJumps = [&] {
            if (jump.size())
                return;
            jump.resize(m_proc.size());
            leastBelowJump.resize(m_proc.size());
            depth.resize(m_proc.size());
            preOrderIndexInDominatorTree.resize(m_proc.size());
            Vector<BasicBlock*> blocks = m_proc.blocksInPreOrder();
            IndexMap<BasicBlock*, BasicBlock*> firstKid(m_proc.size(), nullptr);
            IndexMap<BasicBlock*, BasicBlock*> nextKid(m_proc.size(), nullptr);
            for (BasicBlock* block : blocks) {
                if (BasicBlock* parent = dominators.idom(block))
                    nextKid[block] = std::exchange(firstKid[parent], block);
            }
            Vector<BasicBlock*, 16> worklist { m_proc.at(0) };
            for (unsigned next = 0; !worklist.isEmpty(); ++next) {
                BasicBlock* block = worklist.takeLast();
                preOrderIndexInDominatorTree[block] = next;
                for (BasicBlock* kid = firstKid[block]; kid; kid = nextKid[kid])
                    worklist.append(kid);
            }
            for (BasicBlock* block : blocks) {
                BasicBlock* parent = dominators.idom(block);
                leastBelowJump[block] = block;
                if (!parent) {
                    jump[block] = block;
                    continue;
                }
                depth[block] = depth[parent] + 1;
                BasicBlock* middle = jump[parent];
                if (depth[parent] - depth[middle] != depth[middle] - depth[jump[middle]]) {
                    jump[block] = parent;
                    continue;
                }
                jump[block] = jump[middle];
                if (parent == middle)
                    continue;
                if (leastBelowJump[parent]->frequency() < leastBelowJump[block]->frequency())
                    leastBelowJump[block] = leastBelowJump[parent];
                if (leastBelowJump[middle]->frequency() < leastBelowJump[block]->frequency())
                    leastBelowJump[block] = leastBelowJump[middle];
            }
        };
        auto nearestCommonDominator = [&](BasicBlock* a, BasicBlock* b) {
            while (!dominators.dominates(a, b))
                a = dominators.dominates(jump[a], b) ? dominators.idom(a) : jump[a];
            return a;
        };
        auto dominatorAtDepth = [&](BasicBlock* block, unsigned wanted) {
            while (depth[block] > wanted)
                block = depth[jump[block]] >= wanted ? jump[block] : dominators.idom(block);
            return block;
        };
        auto leastFrequentBetween = [&](BasicBlock* from, BasicBlock* to) {
            BasicBlock* least = nullptr;
            while (from != to) {
                bool jumps = depth[jump[from]] >= depth[to];
                BasicBlock* candidate = jumps ? leastBelowJump[from] : from;
                if (!least || candidate->frequency() < least->frequency())
                    least = candidate;
                from = jumps ? jump[from] : dominators.idom(from);
            }
            return least;
        };

        struct Candidate {
            unsigned epoch { 0 };
            unsigned numberOfMaterializations { 0 };
            double frequency { 0 };
            bool isChosen { false };
            unsigned order { 0 };
            BasicBlock* parent { nullptr };
            BasicBlock* where { nullptr };
            Value* value { nullptr };
        };
        IndexMap<BasicBlock*, Candidate> data(m_proc.size());
        Vector<BasicBlock*> candidates;
        unsigned epoch = 0;
        for (unsigned constant = 0; constant < usesOfConstants.size(); ++constant) {
            auto& uses = usesOfConstants[constant];
            Vector<Value*, 4> placed;
            auto placeImmediates = makeScopeExit([&] {
                BasicBlock* top = nullptr;
                Vector<Use*, 4> left;
                for (Use& use : usesAsImmediates[constant]) {
                    size_t index = placed.findIf([&](Value* value) {
                        return dominators.dominates(materializationBlocks.get(value), use.block);
                    });
                    if (index != notFound) {
                        *use.child = placed[index];
                        continue;
                    }
                    left.append(&use);
                    if (!top)
                        top = use.block;
                    for (unsigned steps = 0; top != m_proc.at(0) && !dominators.dominates(top, use.block); ++steps) {
                        if (steps == maxStepsOneByOne && dominators.dominates(m_proc.at(0), top) && dominators.dominates(m_proc.at(0), use.block)) {
                            prepareJumps();
                            top = nearestCommonDominator(top, use.block);
                            break;
                        }
                        top = dominators.idom(top) ? dominators.idom(top) : m_proc.at(0);
                    }
                }
                if (left.isEmpty())
                    return;
                Value* value = take(*left[0]->child);
                value->owner = top;
                materializations[top].append(value);
                materializationBlocks.add(value, top);
                for (Use* use : left)
                    *use->child = value;
            });
            if (uses.isEmpty())
                continue;
            bool isUsedInOneBlock = std::ranges::all_of(uses, [&](Use& use) {
                return use.block == uses[0].block;
            });
            if (BasicBlock* where = isUsedInOneBlock ? leastFrequentDominator[uses[0].block] : nullptr) {
                Value* value = take(*uses[0].child);
                value->owner = where;
                placed.append(value);
                valuesForConstant.add(value->key(), Vector<Value*, 1> { }).iterator->value.append(value);
                materializations[where].append(value);
                materializationBlocks.add(value, where);
                for (Use& use : uses)
                    *use.child = value;
                continue;
            }

            // The nearest block that dominates all the uses. Above it there is a chain, and what the walk would come to on that is known beforehand too: so it ends there. None: some use is in a block that
            // is not reached, and the walk goes to the root.
            BasicBlock* top = uses[0].block;
            for (Use& use : uses) {
                if (!leastFrequentDominator[use.block]) {
                    top = nullptr;
                    break;
                }
                for (unsigned steps = 0; !dominators.dominates(top, use.block); ++steps) {
                    if (steps == maxStepsOneByOne) {
                        prepareJumps();
                        top = nearestCommonDominator(top, use.block);
                        break;
                    }
                    top = dominators.idom(top);
                }
            }

            ++epoch;
            candidates.shrink(0);
            const size_t most = top ? 64 + 4 * uses.size() : std::numeric_limits<size_t>::max();
            for (Use& use : uses) {
                for (BasicBlock* block = use.block; block && data[block].epoch != epoch && candidates.size() <= most; block = dominators.idom(block)) {
                    data[block] = Candidate { };
                    data[block].epoch = epoch;
                    candidates.append(block);
                    if (block == top)
                        break;
                }
                data[use.block].isChosen = true;
            }
            const bool areTooMany = candidates.size() > most;
            if (areTooMany) {
                prepareJumps();
                ++epoch;
                candidates.shrink(0);
                auto addCandidate = [&](BasicBlock* block, BasicBlock* parent, unsigned order) {
                    data[block] = Candidate { };
                    data[block].epoch = epoch;
                    data[block].parent = parent;
                    data[block].order = order;
                    candidates.append(block);
                };
                auto comesFirst = [&](BasicBlock* a, BasicBlock* b) {
                    return preOrderIndexInDominatorTree[a] < preOrderIndexInDominatorTree[b];
                };
                Vector<BasicBlock*, 16> nodes;
                for (Use& use : uses)
                    nodes.append(use.block);
                std::ranges::sort(nodes, comesFirst);
                nodes.shrink(std::ranges::unique(nodes).begin() - nodes.begin());
                for (size_t i = 1, count = nodes.size(); i < count; ++i)
                    nodes.append(nearestCommonDominator(nodes[i - 1], nodes[i]));
                std::ranges::sort(nodes, comesFirst);
                nodes.shrink(std::ranges::unique(nodes).begin() - nodes.begin());
                RELEASE_ASSERT(nodes[0] == top);
                Vector<BasicBlock*, 16> path;
                for (BasicBlock* node : nodes) {
                    while (!path.isEmpty() && !dominators.dominates(path.last(), node))
                        path.removeLast();
                    BasicBlock* parent = path.isEmpty() ? nullptr : path.last();
                    path.append(node);
                    if (parent && dominators.idom(node) != parent) {
                        BasicBlock* least = leastFrequentBetween(dominators.idom(node), parent);
                        addCandidate(least, parent, preOrderIndex[dominatorAtDepth(node, depth[parent] + 1)]);
                        parent = least;
                    }
                    addCandidate(node, parent, preOrderIndex[node]);
                }
                for (Use& use : uses)
                    data[use.block].isChosen = true;
                std::ranges::sort(candidates, [&](BasicBlock* a, BasicBlock* b) {
                    return data[a].order > data[b].order;
                });
            }

            // A block comes after its dominators in pre-order.
            if (!areTooMany) {
                std::ranges::sort(candidates, [&](BasicBlock* a, BasicBlock* b) {
                    return preOrderIndex[a] > preOrderIndex[b];
                });
            }
            for (BasicBlock* block : candidates) {
                Candidate& candidate = data[block];
                if (candidate.isChosen || candidate.frequency > block->frequency() || (candidate.frequency == block->frequency() && candidate.numberOfMaterializations > 1)) {
                    candidate.isChosen = true;
                    candidate.frequency = block->frequency();
                    candidate.numberOfMaterializations = 1;
                }
                if (BasicBlock* parent = areTooMany ? candidate.parent : block == top ? nullptr : dominators.idom(block)) {
                    data[parent].frequency += candidate.frequency;
                    data[parent].numberOfMaterializations += candidate.numberOfMaterializations;
                }
            }
            // On the chain above, going up, a block is chosen if it runs less often than what has come up, or as often where that is more than one materialization; from then on only if it runs less often
            // still; and the topmost one that is chosen is where everything below is materialized. That is the one that runs least often, the nearest of those that tie, if any is chosen at all.
            BasicBlock* above = nullptr;
            if (BasicBlock* parent = top ? dominators.idom(top) : nullptr) {
                BasicBlock* least = leastFrequentDominator[parent];
                Candidate& candidate = data[top];
                if (candidate.frequency > least->frequency() || (candidate.frequency == least->frequency() && candidate.numberOfMaterializations > 1)) {
                    above = least;
                    data[above] = Candidate { };
                    data[above].epoch = epoch;
                    data[above].where = above;
                }
            }
            for (unsigned i = candidates.size(); i--;) {
                BasicBlock* block = candidates[i];
                BasicBlock* parent = areTooMany ? data[block].parent : block == top ? nullptr : dominators.idom(block);
                BasicBlock* inherited = block == top ? above : parent ? data[parent].where : nullptr;
                if (inherited)
                    data[block].where = inherited;
                else if (data[block].isChosen)
                    data[block].where = block;
            }

            Value* original = *uses[0].child;
            auto& values = valuesForConstant.add(original->key(), Vector<Value*, 1> { }).iterator->value;
            for (Use& use : uses) {
                BasicBlock* where = data[use.block].where;
                Value*& value = data[where].value;
                if (!value) {
                    value = take(original);
                    placed.append(value);
                    value->owner = where;
                    values.append(value);
                    materializations[where].append(value);
                    materializationBlocks.add(value, where);
                }
                *use.child = value;
            }
        }
    }

    template<typename Filter>
    void hoistConstants(NOESCAPE const Filter& filter)
    {
        Dominators& dominators = m_proc.dominators();
        UncheckedKeyHashMap<ValueKey, Value*> valueForConstant;
        IndexMap<BasicBlock*, Vector<Value*>> materializations(m_proc.size());
        UncheckedKeyHashMap<Value*, BasicBlock*> materializationBlocks;
        ValuesForConstant valuesForConstantByFrequency;

        // Air reads an ArgumentReg in the prologue, wherever it stands.
        auto isPlacedByFrequency = [&](const Value* value) {
            return m_proc.materializesConstantsByFrequency() && value->opcode() != ArgumentReg;
        };
        Vector<UseOfConstant> usesInOrder;
        Vector<PlaceOfConstant> placesInOrder;
        for (BasicBlock* block : m_proc) {
            for (unsigned index = 0; index < block->size(); ++index) {
                Value* value = block->at(index);
                if (filter(value))
                    placesInOrder.append({ block, index });
                for (Value*& child : value->children()) {
                    if (filter(child))
                        usesInOrder.append({ block, value, &child, index });
                }
            }
        }

        if (m_proc.materializesConstantsByFrequency()) {
            placeByFrequency([&](const Value* value) {
                return isPlacedByFrequency(value);
            }, usesInOrder, materializations, materializationBlocks, valuesForConstantByFrequency);
        }

        // We determine where things get materialized based on where they are used.
        for (const UseOfConstant& use : usesInOrder) {
            BasicBlock* block = use.block;
            Value*& child = *use.child;
            if (isPlacedByFrequency(child))
                continue;

            ValueKey key = child->key();
            auto result = valueForConstant.add(key, child);
            if (result.isNewEntry) {
                // Assume that this block is where we want to materialize the value.
                child->owner = block;
                continue;
            }

            // Make 'value' use the canonical constant rather than the one it was using.
            child = result.iterator->value;

            // Determine the least common dominator. That's the lowest place in the CFG where
            // we could materialize the constant while still having only one materialization
            // in the resulting code.
            while (!dominators.dominates(child->owner, block))
                child->owner = dominators.idom(child->owner);
        }

        // Make sure that each basic block knows what to materialize. This also refines the
        // materialization block based on execution frequency. It finds the minimum block frequency
        // of all of its dominators, and selects the closest block amongst those that are tied for
        // lowest frequency.
        for (auto& entry : valueForConstant) {
            Value* value = entry.value;
            for (BasicBlock* block = value->owner; block; block = dominators.idom(block)) {
                if (block->frequency() < value->owner->frequency())
                    value->owner = block;
            }
            materializations[value->owner].append(value);
            materializationBlocks.add(value, value->owner);
        }

        // Get rid of Value's that are fast constants but aren't canonical. Also remove the canonical
        // ones from the CFG, since we're going to reinsert them elsewhere.
        for (PlaceOfConstant place : placesInOrder) {
            Value*& value = place.block->at(place.index);
            if (materializationBlocks.contains(value))
                value = m_proc.add<Value>(Nop, value->origin());
            else
                value->replaceWithNopIgnoringType();
        }

        // Now make sure that we move constants to where they are supposed to go. Again, we do this
        // based on uses.
        size_t nextUse = 0;
        for (BasicBlock* block : m_proc) {
            while (nextUse < usesInOrder.size() && usesInOrder[nextUse].block == block) {
                Value* value = usesInOrder[nextUse].user;
                unsigned valueIndex = usesInOrder[nextUse].indexOfUser;
                while (nextUse < usesInOrder.size() && usesInOrder[nextUse].user == value)
                    ++nextUse;
                
                // This finds the outermost (best) block last. So, the functor overrides the result
                // each time it finds something acceptable.
                auto findBestConstant = [&] (const auto& predicate) -> Value* {
                    Value* result = nullptr;
                    dominators.forAllDominatorsOf(
                        block,
                        [&] (BasicBlock* dominator) {
                            for (Value* value : materializations[dominator]) {
                                if (predicate(value)) {
                                    result = value;
                                    break;
                                }
                            }
                        });
                    return result;
                };
                
                // We call this when we have found a constant that we'd like to use. It's possible that
                // we have computed that the constant should be materialized in this block, but we
                // haven't inserted it yet. This inserts the constant if necessary.
                auto materialize = [&] (Value* child) {
                    if (!filter(child))
                        return;

                    // If we encounter a fast constant, then it must be canonical, since we already
                    // got rid of the non-canonical ones.
                    ASSERT(materializationBlocks.contains(child));

                    if (child->owner != block) {
                        // This constant isn't our problem. It's going to be materialized in another
                        // block.
                        return;
                    }
                    
                    // We're supposed to materialize this constant in this block, and we haven't
                    // done it yet.
                    m_insertionSet.insertValue(valueIndex, child);
                    child->owner = nullptr;
                };
                
                if (MemoryValue* memoryValue = value->as<MemoryValue>()) {
                    Value* pointer = memoryValue->lastChild();
                    if (pointer->hasIntPtr() && filter(pointer)) {
                        auto desiredOffset = [&] (Value* otherPointer) -> intptr_t {
                            // We would turn this:
                            //
                            //     Load(@p, offset = c)
                            //
                            // into this:
                            //
                            //     Load(@q, offset = ?)
                            //
                            // The offset should be c + @p - @q, because then we're loading from:
                            //
                            //     @q + c + @p - @q
                            uintptr_t c = static_cast<uintptr_t>(static_cast<intptr_t>(memoryValue->offset()));
                            uintptr_t p = pointer->asIntPtr();
                            uintptr_t q = otherPointer->asIntPtr();
                            return c + p - q;
                        };
                        
                        Value* bestPointer = findBestConstant(
                            [&] (Value* candidatePointer) -> bool {
                                if (!candidatePointer->hasIntPtr())
                                    return false;
                                
                                int64_t offset = desiredOffset(candidatePointer);
                                return memoryValue->isLegalOffset(offset);
                            });
                        
                        if (bestPointer) {
                            memoryValue->lastChild() = bestPointer;
                            memoryValue->setOffset(static_cast<int32_t>(desiredOffset(bestPointer)));
                        }
                    }
                } else {
                    switch (value->opcode()) {
                    case Add:
                    case Sub: {
                        Value* addend = value->child(1);
                        if (!addend->hasInt() || !filter(addend))
                            break;
                        int64_t addendConst = addend->asInt();
                        if (Air::Arg::isValidImmForm(addendConst))
                            break;
                        // Bun: only the constant's negation can take its place, so look that up. findBestConstant() would go through
                        ValueKey negated(addend->kind(), addend->type(), static_cast<int64_t>(-static_cast<uint64_t>(addendConst)));
                        Value* bestAddend = valueForConstant.get(negated);
                        if (auto iterator = valuesForConstantByFrequency.find(negated); iterator != valuesForConstantByFrequency.end()) {
                            for (Value* candidate : iterator->value) {
                                if (dominators.dominates(materializationBlocks.get(candidate), block)) {
                                    bestAddend = candidate;
                                    break;
                                }
                            }
                        }
                        if (!bestAddend || bestAddend == addend || bestAddend->asInt() != -addendConst)
                            break;
                        BasicBlock* bestBlock = materializationBlocks.get(bestAddend);
                        BasicBlock* addendBlock = materializationBlocks.get(addend);
                        if (!dominators.dominates(bestBlock, block))
                            break;
                        if (bestBlock == addendBlock ? addendConst > 0 : dominators.dominates(addendBlock, bestBlock))
                            break;
                        materialize(value->child(0));
                        materialize(bestAddend);
                        value->replaceWithIdentity(
                            m_insertionSet.insert<Value>(
                                valueIndex, value->opcode() == Add ? Sub : Add, value->origin(),
                                value->child(0), bestAddend));
                        break;
                    }
                    default:
                        break;
                    }
                }
                
                for (Value* child : value->children())
                    materialize(child);
            }

            // We may have some constants that need to be materialized right at the end of this
            // block.
            for (Value* value : materializations[block]) {
                if (!value->owner) {
                    // It's already materialized in this block.
                    continue;
                }

                m_insertionSet.insertValue(block->size() - 1, value);
            }
            m_insertionSet.execute(block);
        }
    }

    void lowerMaterializationCostHeavyConstants()
    {
        if (m_proc.positionIndependent()) {
            for (BasicBlock* block : m_proc) {
                for (unsigned valueIndex = 0; valueIndex < block->size(); ++valueIndex) {
                    Value* value = block->at(valueIndex);
                    if (value->opcode() != ConstDouble || !goesInTable(value))
                        continue;
                    Value* bits = m_insertionSet.insertIntConstant(valueIndex, value->origin(), Int64, std::bit_cast<int64_t>(value->asDouble()));
                    value->replaceWithIdentity(m_insertionSet.insert<Value>(valueIndex, BitwiseCast, value->origin(), bits));
                }
                m_insertionSet.execute(block);
            }
            return;
        }

        unsigned doubleSize = 0;
        unsigned v128Size = 0;
        UncheckedKeyHashMap<ValueKey, unsigned> constTable;
        for (Value* value : m_proc.values()) {
            if (!goesInTable(value))
                continue;

            ValueKey key = value->key();
            switch (value->opcode()) {
            case Const128:
                constTable.add(key, v128Size++);
                break;
            case ConstDouble:
                constTable.add(key, doubleSize++);
                break;
            default:
                RELEASE_ASSERT_NOT_REACHED();
                break;
            }
        }

        auto getOffset = [&](Opcode opcode, unsigned indexInKind) -> size_t {
            switch (opcode) {
            case Const128:
                return sizeof(v128_t) * indexInKind;
            case ConstDouble:
                return sizeof(v128_t) * v128Size + sizeof(double) * indexInKind;
            default:
                RELEASE_ASSERT_NOT_REACHED();
                break;
            }
        };

        uint8_t* dataSection = static_cast<uint8_t*>(m_proc.addDataSection(sizeof(v128_t) * v128Size + sizeof(double) * doubleSize));
        for (auto& entry : constTable) {
            auto* pointer = dataSection + getOffset(entry.key.opcode(), entry.value);
            switch (entry.key.opcode()) {
            case Const128:
                *std::bit_cast<v128_t*>(pointer) = entry.key.vectorValue();
                break;
            case ConstDouble:
                *std::bit_cast<double*>(pointer) = entry.key.doubleValue();
                break;
            default:
                RELEASE_ASSERT_NOT_REACHED();
                break;
            }
        }

        IndexSet<Value*> offLimits;
        for (BasicBlock* block : m_proc) {
            for (unsigned valueIndex = 0; valueIndex < block->size(); ++valueIndex) {
                StackmapValue* value = block->at(valueIndex)->as<StackmapValue>();
                if (!value)
                    continue;

                for (unsigned childIndex = 0; childIndex < value->numChildren(); ++childIndex) {
                    if (!value->constrainedChild(childIndex).rep().isAny())
                        continue;

                    Value*& child = value->child(childIndex);
                    if (!goesInTable(child))
                        continue;

                    // This is still the best representation for vectors
                    if (child->type().isVector())
                        continue;

                    ValueKey key = child->key();
                    child = m_insertionSet.insertValue(
                        valueIndex, key.materialize(m_proc, value->origin()));
                    offLimits.add(child);
                }
            }

            m_insertionSet.execute(block);
        }

        for (BasicBlock* block : m_proc) {
            for (unsigned valueIndex = 0; valueIndex < block->size(); ++valueIndex) {
                Value* value = block->at(valueIndex);
                if (!goesInTable(value))
                    continue;
                if (offLimits.contains(value))
                    continue;

                ValueKey key = value->key();
                auto offset = getOffset(key.opcode(), constTable.get(key));
                if (!isRepresentableAs<Value::OffsetType>(offset))
                    continue;

                Value* tableBase = m_insertionSet.insertIntConstant(
                    valueIndex, value->origin(), pointerType(),
                    std::bit_cast<intptr_t>(dataSection));
                Value* result = m_insertionSet.insert<MemoryValue>(
                    valueIndex, Load, value->type(), value->origin(), tableBase,
                    static_cast<Value::OffsetType>(offset));
                value->replaceWithIdentity(result);
            }

            m_insertionSet.execute(block);
        }
    }

    static bool NODELETE goesInTable(const Value* value)
    {
        switch (value->opcode()) {
        case ConstDouble: {
            return !Air::Arg::isValidFPImm64Form(std::bit_cast<uint64_t>(value->asDouble()));
        }
        case Const128: {
            return !Air::Arg::isValidFPImm128Form(value->asV128());
        }
        default:
            break;
        }
        return false;
    }

    Procedure& m_proc;
    InsertionSet m_insertionSet;
};

} // anonymous namespace

void moveConstants(Procedure& proc)
{
    PhaseScope phaseScope(proc, "moveConstants"_s);
    MoveConstants moveConstants(proc);
    moveConstants.run();
}

} } // namespace JSC::B3

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(B3_JIT)
