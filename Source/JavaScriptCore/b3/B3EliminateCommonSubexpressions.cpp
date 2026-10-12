/*
 * Copyright (C) 2016-2026 Apple Inc. All rights reserved.
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
#include "B3EliminateCommonSubexpressions.h"

#if ENABLE(B3_JIT)

#include "B3BasicBlockInlines.h"
#include "B3BlockWorklist.h"
#include "B3BreakCriticalEdges.h"
#include "B3Dominators.h"
#include "B3HeapRange.h"
#include "B3InsertionSetInlines.h"
#include "B3LoopUtilities.h"
#include "B3MemoryValue.h"
#include "B3MemoryValueInlines.h"
#include "B3NaturalLoops.h"
#include "B3PhaseScope.h"
#include "B3ProcedureInlines.h"
#include "B3PureCSE.h"
#include "B3SSACalculator.h"
#include "B3UpsilonValue.h"
#include "B3ValueInlines.h"
#include "B3ValueKeyInlines.h"
#include <wtf/CommaPrinter.h>
#include <wtf/HashMap.h>
#include <wtf/IndexSet.h>
#include <wtf/ListDump.h>
#include <wtf/RangeSet.h>
#include <wtf/Scope.h>

namespace JSC { namespace B3 {

namespace {

namespace B3EliminateCommonSubexpressionsInternal {
static constexpr bool verbose = false;
}

// FIXME: We could treat Patchpoints with a non-empty set of reads as a "memory value" and somehow
// eliminate redundant ones. We would need some way of determining if two patchpoints are replacable.
// It doesn't seem right to use the reads set for this. We could use the generator, but that feels
// lame because the FTL will pretty much use a unique generator for each patchpoint even when two
// patchpoints have the same semantics as far as CSE would be concerned. We could invent something
// like a "value ID" for patchpoints. By default, each one gets a unique value ID, but FTL could force
// some patchpoints to share the same one as a signal that they will return the same value if executed
// in the same heap with the same inputs.

using MemoryMatches = Vector<MemoryValue*, 1>;

class MemoryValueMap {
public:
    MemoryValueMap() { }

    void add(MemoryValue* memory)
    {
        Matches& matches = m_map.add(keyFor(memory->lastChild(), memory->offset()), Matches()).iterator->value;
        if (matches.contains(memory))
            return;
        matches.append(memory);

        if (memory->readsMutability() == Mutability::Immutable)
            return;
        Clobberable entry { memory, keyFor(memory->lastChild(), memory->offset()), memory->range() };
        if (entry.range.distance() == 1)
            m_narrow.add(entry.range.begin(), Vector<Clobberable, 1>()).iterator->value.append(entry);
        else
            m_wide.append(entry);
    }

    // Bun: removes the values that a write to this range invalidates. It used to look at every value in the map, which is quadratic in
    void clobber(HeapRange writes)
    {
        auto remove = [&](const Clobberable& entry) {
            auto iter = m_map.find(entry.key);
            if (iter == m_map.end())
                return;
            iter->value.removeFirst(entry.value);
            if (iter->value.isEmpty())
                m_map.remove(iter);
        };
        if (writes.distance() == 1) {
            for (auto& entry : m_narrow.take(writes.begin()))
                remove(entry);
        } else {
            m_narrow.removeIf([&](auto& bucket) {
                if (!HeapRange(bucket.key, bucket.key + 1).overlaps(writes))
                    return false;
                for (auto& entry : bucket.value)
                    remove(entry);
                return true;
            });
        }
        m_wide.removeAllMatching([&](const Clobberable& entry) {
            if (!entry.range.overlaps(writes))
                return false;
            remove(entry);
            return true;
        });
    }

    Matches* find(Value* ptr, int64_t offset)
    {
        auto iter = m_map.find(keyFor(ptr, offset));
        if (iter == m_map.end())
            return nullptr;
        return &iter->value;
    }

    template<typename Functor>
    MemoryValue* find(Value* ptr, int64_t offset, NOESCAPE const Functor& functor)
    {
        dataLogLnIf(B3EliminateCommonSubexpressionsInternal::verbose, "        Looking for ", pointerDump(ptr), " in ", *this);
        if (Matches* matches = find(ptr, offset)) {
            dataLogLnIf(B3EliminateCommonSubexpressionsInternal::verbose, "        Matches: ", pointerListDump(*matches));
            for (Value* candidateValue : *matches) {
                dataLogLnIf(B3EliminateCommonSubexpressionsInternal::verbose, "        Having candidate: ", pointerDump(candidateValue));
                if (MemoryValue* candidateMemory = candidateValue->as<MemoryValue>()) {
                    if (functor(candidateMemory))
                        return candidateMemory;
                }
            }
        }
        return nullptr;
    }

    void dump(PrintStream& out) const
    {
        out.print("{"_s);
        CommaPrinter comma;
        for (auto& entry : m_map)
            out.print(comma, pointerDump(entry.key.first), "+"_s, entry.key.second, "=>"_s, pointerListDump(entry.value));
        out.print("}"_s);
    }
    
private:
    // Bun: keyed by the offset as well as the pointer. A lookup only accepts a value with the same offset, and code compiled ahead
    using Key = std::pair<Value*, int64_t>;
    static Key keyFor(Value* pointer, int64_t offset) { return { pointer, offset }; }

    // This uses Matches for two reasons:
    // - It cannot be a MemoryValue* because the key is imprecise. Many MemoryValues could have the
    //   same key while being unaliased.
    // - It can't be a MemoryMatches array because the MemoryValue*'s could be turned into Identity's.
    UncheckedKeyHashMap<Key, Matches> m_map;

    struct Clobberable {
        Value* value;
        Key key;
        HeapRange range;
    };
    UncheckedKeyHashMap<uint64_t, Vector<Clobberable, 1>, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_narrow;
    Vector<Clobberable> m_wide;
};

struct ImpureBlockData {
    void dump(PrintStream& out) const
    {
        out.print(
            "{reads = ", reads, ", writes = ", writes,
            ", memoryStoresAtHead = ", memoryStoresAtHead, ", memoryValuesAtTail = ", memoryValuesAtTail,
            "}");
    }

    RangeSet<HeapRange> reads; // This only gets used for forward store elimination.
    RangeSet<HeapRange> writes; // This gets used for both load and store elimination.
    bool fence { false };
    bool writesPinned { false };
    bool hasCall { false };

    MemoryValueMap memoryStoresAtHead;
    MemoryValueMap memoryValuesAtTail;

    struct ForwardedLoad {
        Value* ptr;
        int64_t offset;
        HeapRange range;
        Opcode opcode;
        Type type;
        Value* value;
    };
    Vector<ForwardedLoad> forwardedLoadsAtTail;

    // This Maps x->y in "y = WasmAddress(@x)"
    UncheckedKeyHashMap<Value*, Value*> m_candidateWasmAddressesAtTail;
};

class CSE {
public:
    CSE(Procedure& proc)
        : m_proc(proc)
        , m_insertionSet(proc)
    {
    }

    bool run()
    {
        dataLogIf(B3EliminateCommonSubexpressionsInternal::verbose, "B3 before CSE:\n", m_proc);

        // Direct Upsilon insertion needs no critical edges.
        breakCriticalEdges(m_proc);

        m_proc.resetValueOwners();
        m_dominators = &m_proc.dominators();
        m_impureBlockData = IndexMap<BasicBlock*, ImpureBlockData>(m_proc.size());
        m_ssa = makeUnique<SSACalculator>(m_proc);
        m_hasIndex = m_proc.size() > minBlocksForIndex;
        if (m_hasIndex) {
            for (BasicBlock* block : m_proc) {
                if (!block->numSuccessors())
                    m_blocksThatReachAnExit.push(block);
            }
            while (BasicBlock* block = m_blocksThatReachAnExit.pop())
                m_blocksThatReachAnExit.pushAll(block->predecessors());
        }

        // Summarize the impure effects of each block, and the impure values available at the end of
        // each block. This doesn't edit code yet.
        for (BasicBlock* block : m_proc) {
            ImpureBlockData& data = m_impureBlockData[block];
            for (Value* value : *block) {
                Effects effects = value->effects();
                data.hasCall |= m_proc.positionIndependent() && isCall(value);
                MemoryValue* memory = value->as<MemoryValue>();

                if (memory) {
                    if (memory->isStore()
                        && !data.reads.overlaps(memory->range())
                        && !data.writes.overlaps(memory->range())
                        && (!data.fence || !memory->hasFence()))
                        data.memoryStoresAtHead.add(memory);
                }
                data.reads.add(effects.reads);

                if (HeapRange writes = effects.writes)
                    clobber(data, writes);
                data.fence |= effects.fence;

                if (memory) {
                    data.memoryValuesAtTail.add(memory);
                    didAddMemoryValue(block, memory);
                }
                if (WasmAddressValue* wasmAddress = value->as<WasmAddressValue>())
                    data.m_candidateWasmAddressesAtTail.add(wasmAddress->child(0), wasmAddress);

                if (effects.writesPinned) {
                    data.writesPinned = true;
                    data.m_candidateWasmAddressesAtTail.clear();
                }
            }

            dataLogLnIf(B3EliminateCommonSubexpressionsInternal::verbose, "Block ", *block, ": ", data);
        }

        // Perform CSE. This edits code.
        performCSE(nullptr);

        // Re-sweep loop blocks once to catch some additional cases. In reverse-post-order the loop body hasn't been
        // processed when we visit the header, so back-edge queries from the header miss matches that show up after the body is processed.
        NaturalLoops& loops = m_proc.naturalLoops();
        if (loops.numLoops()) {
            IndexSet<BasicBlock*> loopBlocks;
            for (unsigned i = 0; i < loops.numLoops(); ++i) {
                const auto& loop = loops.loop(i);
                loopBlocks.add(loop.header());
                for (unsigned j = 0; j < loop.size(); ++j)
                    loopBlocks.add(loop.at(j));
            }
            performCSE(&loopBlocks);
        }

        finalize();

        dataLogIf(B3EliminateCommonSubexpressionsInternal::verbose, "B3 after CSE:\n", m_proc);

        return m_changed;
    }

private:
    void performCSE(const IndexSet<BasicBlock*>* filter)
    {
        for (auto* block : m_proc.blocksInPostOrder() | std::views::reverse) {
            m_block = block;
            if (filter && !filter->contains(m_block))
                continue;
            dataLogLnIf(B3EliminateCommonSubexpressionsInternal::verbose, "Looking at ", *m_block, ":");

            m_data = ImpureBlockData();
            for (m_index = 0; m_index < m_block->size(); ++m_index) {
                m_value = m_block->at(m_index);
                process();
            }
            m_insertionSet.execute(m_block);
            m_impureBlockData[m_block] = m_data;
        }
    }

    void finalize()
    {
        if (m_pendingResolutions.isEmpty()) {
            ASSERT(m_sets.isEmpty());
            return;
        }

        m_ssa->computePhis(
            [&] (SSACalculator::Variable* var, BasicBlock*) -> Value* {
                Value* placeholder = m_placeholderOfVariable[var->index()];
                return m_proc.add<Value>(Phi, placeholder->type(), placeholder->origin());
            });

        for (BasicBlock* block : m_proc.blocksInPreOrder()) {
            for (SSACalculator::Def* phiDef : m_ssa->phisForBlock(block))
                m_insertionSet.insertValue(0, phiDef->value());

            if (m_blocksWithSets.contains(block)) {
                for (unsigned valueIndex = 0; valueIndex < block->size(); ++valueIndex) {
                    Value* value = block->at(valueIndex);
                    if (!m_matched.contains(value))
                        continue;
                    auto iter = m_sets.find(value);
                    if (iter == m_sets.end())
                        continue;
                    for (Value* extra : iter->value)
                        m_insertionSet.insertValue(valueIndex + 1, extra);
                }
            }

            if (block->size()) {
                unsigned upsilonIndex = block->size() - 1;
                Origin upsilonOrigin = block->last()->origin();
                for (BasicBlock* successor : block->successorBlocks()) {
                    for (SSACalculator::Def* phiDef : m_ssa->phisForBlock(successor)) {
                        SSACalculator::Variable* var = phiDef->variable();
                        SSACalculator::Def* reaching = m_ssa->reachingDefAtTail(block, var);
                        Value* upsilonChild;
                        if (reaching)
                            upsilonChild = reaching->value()->foldIdentity();
                        else {
                            // Predecessor reaches the Phi on a path with no def.
                            upsilonChild = m_insertionSet.insertBottom(upsilonIndex, upsilonOrigin, m_placeholderOfVariable[var->index()]->type());
                        }
                        m_insertionSet.insert<UpsilonValue>(upsilonIndex, upsilonOrigin, upsilonChild, phiDef->value());
                    }
                }
            }

            m_insertionSet.execute(block);
        }

        for (auto& resolution : m_pendingResolutions) {
            Value* reachingValue = nullptr;
            for (SSACalculator::Def* phiDef : m_ssa->phisForBlock(resolution.useBlock)) {
                if (phiDef->variable() == resolution.var) {
                    reachingValue = phiDef->value();
                    break;
                }
            }
            if (!reachingValue) {
                SSACalculator::Def* def = m_ssa->reachingDefAtHead(resolution.useBlock, resolution.var);
                ASSERT(def);
                reachingValue = def->value();
            }
            resolution.placeholder->replaceWithIdentity(reachingValue->foldIdentity());
        }
    }

    void process()
    {
        m_value->performSubstitution();
        m_data.hasCall |= m_proc.positionIndependent() && isCall(m_value);

        if (m_pureCSE.process(m_value, *m_dominators)) {
            ASSERT(!m_value->effects().readsPinned || !m_data.writesPinned);
            ASSERT(!m_value->effects().writes);
            ASSERT(!m_value->effects().writesPinned);
            m_changed = true;
            return;
        }

        // If a WasmArrayLength is dominated by the same key's WasmArrayLength,
        // traps-bit does not matter since dominating WasmArrayLength already ensured that
        // traps check is already done. PureCSE already handles same-variant matches above;
        // this queries for the opposite traps-bit variant via ValueKey lookup.
        if (m_value->opcode() == WasmArrayLength) {
            Kind altKind = m_value->kind();
            altKind.setTraps(!altKind.traps());
            ValueKey altKey(altKind, m_value->type(), m_value->child(0));
            if (Value* match = m_pureCSE.findMatch(altKey, m_block, *m_dominators)) {
                m_value->replaceWithIdentity(match);
                m_changed = true;
                return;
            }
        }

        if (WasmAddressValue* wasmAddress = m_value->as<WasmAddressValue>()) {
            processWasmAddressValue(wasmAddress);
            return;
        }

        Effects effects = m_value->effects();

        if (effects.writesPinned) {
            m_data.writesPinned = true;
            m_data.m_candidateWasmAddressesAtTail.clear();
        }

        MemoryValue* memory = m_value->as<MemoryValue>();

        if (memory && processMemoryBeforeClobber(memory))
            return;

        if (HeapRange writes = effects.writes)
            clobber(m_data, writes);

        if (memory)
            processMemoryAfterClobber(memory);

        // The reads info should be updated even the block is processed
        // since the dominated store nodes may dependent on the data
        // read from the processed block. Note that there is no need to
        // update reads info if the node is deleted.
        m_data.reads.add(effects.reads);
    }

    // Return true if we got rid of the operation. If you changed IR in this function, you have to
    // set m_changed even if you also return true.
    bool processMemoryBeforeClobber(MemoryValue* memory)
    {
        Value* value = memory->child(0);
        Value* ptr = memory->lastChild();
        HeapRange range = memory->range();
        Value::OffsetType offset = memory->offset();

        switch (memory->opcode()) {
        case Store8:
            return handleStoreBeforeClobber(
                ptr, range,
                [&] (MemoryValue* candidate) -> bool {
                    return candidate->offset() == offset
                        && ((candidate->opcode() == Store8 && candidate->child(0) == value)
                            || ((candidate->opcode() == Load8Z || candidate->opcode() == Load8S)
                                && candidate == value));
                });
        case Store16:
            return handleStoreBeforeClobber(
                ptr, range,
                [&] (MemoryValue* candidate) -> bool {
                    return candidate->offset() == offset
                        && ((candidate->opcode() == Store16 && candidate->child(0) == value)
                            || ((candidate->opcode() == Load16Z || candidate->opcode() == Load16S)
                                && candidate == value));
                });
        case Store:
            return handleStoreBeforeClobber(
                ptr, range,
                [&] (MemoryValue* candidate) -> bool {
                    return candidate->offset() == offset
                        && ((candidate->opcode() == Store && candidate->child(0) == value)
                            || (candidate->opcode() == Load && candidate == value));
                });
        default:
            return false;
        }
    }

    void clobber(ImpureBlockData& data, HeapRange writes)
    {
        data.writes.add(writes);

        data.memoryValuesAtTail.clobber(writes);
        data.forwardedLoadsAtTail.removeAllMatching([&](const ImpureBlockData::ForwardedLoad& load) {
            return load.range.overlaps(writes);
        });
    }

    void processMemoryAfterClobber(MemoryValue* memory)
    {
        Value* ptr = memory->lastChild();
        HeapRange range = memory->range();
        Value::OffsetType offset = memory->offset();
        Type type = memory->type();

        // FIXME: Empower this to insert more casts and shifts. For example, a Load8 could match a
        // Store and mask the result. You could even have:
        //
        // Store(@value, @ptr, offset = 0)
        // Load8Z(@ptr, offset = 2)
        //
        // Which could be turned into something like this:
        //
        // Store(@value, @ptr, offset = 0)
        // ZShr(@value, 16)
        
        switch (memory->opcode()) {
        case Load8Z: {
            handleMemoryValue(
                ptr, range,
                [&] (MemoryValue* candidate) -> bool {
                    return candidate->offset() == offset
                        && (candidate->opcode() == Load8Z || candidate->opcode() == Store8);
                },
                [&] (MemoryValue* match, Vector<Value*>& fixups) -> Value* {
                    if (match->opcode() == Store8) {
                        Value* mask = m_proc.add<Const32Value>(m_value->origin(), 0xff);
                        fixups.append(mask);
                        Value* zext = m_proc.add<Value>(
                            BitAnd, m_value->origin(), match->child(0), mask);
                        fixups.append(zext);
                        return zext;
                    }
                    return nullptr;
                });
            break;
        }

        case Load8S: {
            handleMemoryValue(
                ptr, range,
                [&] (MemoryValue* candidate) -> bool {
                    return candidate->offset() == offset
                        && (candidate->opcode() == Load8S || candidate->opcode() == Store8);
                },
                [&] (MemoryValue* match, Vector<Value*>& fixups) -> Value* {
                    if (match->opcode() == Store8) {
                        Value* sext = m_proc.add<Value>(
                            SExt8, m_value->origin(), match->child(0));
                        fixups.append(sext);
                        return sext;
                    }
                    return nullptr;
                });
            break;
        }

        case Load16Z: {
            handleMemoryValue(
                ptr, range,
                [&] (MemoryValue* candidate) -> bool {
                    return candidate->offset() == offset
                        && (candidate->opcode() == Load16Z || candidate->opcode() == Store16);
                },
                [&] (MemoryValue* match, Vector<Value*>& fixups) -> Value* {
                    if (match->opcode() == Store16) {
                        Value* mask = m_proc.add<Const32Value>(m_value->origin(), 0xffff);
                        fixups.append(mask);
                        Value* zext = m_proc.add<Value>(
                            BitAnd, m_value->origin(), match->child(0), mask);
                        fixups.append(zext);
                        return zext;
                    }
                    return nullptr;
                });
            break;
        }

        case Load16S: {
            handleMemoryValue(
                ptr, range, [&] (MemoryValue* candidate) -> bool {
                    return candidate->offset() == offset
                        && (candidate->opcode() == Load16S || candidate->opcode() == Store16);
                },
                [&] (MemoryValue* match, Vector<Value*>& fixups) -> Value* {
                    if (match->opcode() == Store16) {
                        Value* sext = m_proc.add<Value>(
                            SExt16, m_value->origin(), match->child(0));
                        fixups.append(sext);
                        return sext;
                    }
                    return nullptr;
                });
            break;
        }

        case Load: {
            handleMemoryValue(
                ptr, range,
                [&] (MemoryValue* candidate) -> bool {
                    dataLogLnIf(B3EliminateCommonSubexpressionsInternal::verbose, "        Consdering ", pointerDump(candidate));
                    if (candidate->offset() != offset)
                        return false;

                    dataLogLnIf(B3EliminateCommonSubexpressionsInternal::verbose, "            offset ok.");
                    if (candidate->opcode() == Load) {
                        if (candidate->type() == type)
                            return true;
                        if (candidate->type() == Int64 && type == Int32)
                            return true;
                    }

                    dataLogLnIf(B3EliminateCommonSubexpressionsInternal::verbose, "            not a load with ok type.");
                    if (candidate->opcode() == Store) {
                        if (candidate->child(0)->type() == type)
                            return true;
                        if (candidate->child(0)->type() == Int64 && type == Int32)
                            return true;
                    }

                    dataLogLnIf(B3EliminateCommonSubexpressionsInternal::verbose, "            not a store with ok type.");
                    return false;
                },
                [&] (MemoryValue* match, Vector<Value*>& fixups) -> Value* {
                    if (match->opcode() == Load) {
                        if (match->type() == type)
                            return nullptr;

                        if (match->type() == Int64 && type == Int32) {
                            Value* trunc = m_proc.add<Value>(Trunc, m_value->origin(), match);
                            fixups.append(trunc);
                            return trunc;
                        }
                    }

                    if (match->opcode() == Store) {
                        if (match->child(0)->type() == type)
                            return nullptr;

                        if (match->child(0)->type() == Int64 && type == Int32) {
                            Value* trunc = m_proc.add<Value>(Trunc, m_value->origin(), match->child(0));
                            fixups.append(trunc);
                            return trunc;
                        }
                    }

                    return nullptr;
                });
            break;
        }

        case Store8: {
            handleStoreAfterClobber(
                ptr, range,
                [&] (MemoryValue* candidate) -> bool {
                    return candidate->opcode() == Store8
                        && candidate->offset() == offset;
                });
            break;
        }
            
        case Store16: {
            handleStoreAfterClobber(
                ptr, range,
                [&] (MemoryValue* candidate) -> bool {
                    return candidate->opcode() == Store16
                        && candidate->offset() == offset;
                });
            break;
        }
            
        case Store: {
            auto clobberWidth = memory->accessWidth();
            handleStoreAfterClobber(
                ptr, range,
                [&] (MemoryValue* candidate) -> bool {
                    return candidate->opcode() == Store
                        && candidate->offset() == offset
                        && candidate->accessWidth() >= clobberWidth;
                });
            break;
        }

        default:
            break;
        }
    }

    template<typename Filter>
    bool handleStoreBeforeClobber(Value* ptr, HeapRange range, NOESCAPE const Filter& filter)
    {
        MemoryMatches matches = findMemoryValue(ptr, range, filter);
        if (matches.isEmpty())
            return false;

        m_value->replaceWithNop();
        m_changed = true;
        return true;
    }

    template<typename Filter>
    void handleStoreAfterClobber(Value* ptr, HeapRange range, NOESCAPE const Filter& filter)
    {
        if (!m_value->traps() && findStoreAfterClobber(ptr, range, filter)) {
            m_value->replaceWithNop();
            m_changed = true;
            return;
        }

        m_data.memoryValuesAtTail.add(m_value->as<MemoryValue>());
        didAddMemoryValue(m_block, m_value->as<MemoryValue>());
    }

    // Bun: by pointer and offset, the blocks that have ever had such a value in one of their maps, and the one store there is. Nothing is
    // taken out, so this is a superset of what a search beyond the block at hand can find. Without it a search that finds nothing visits
    // every block up to the root, or down to the exits, which is quadratic in the size of the procedure.
    using MemoryKey = std::pair<Value*, int64_t>;

    void didAddMemoryValue(BasicBlock* block, MemoryValue* memory)
    {
        if (!m_hasIndex)
            return;
        MemoryKey key { memory->lastChild(), memory->offset() };
        auto& blocks = m_blocksWithMemoryValue.add(key, Vector<BasicBlock*, 2>()).iterator->value;
        if (blocks.size() <= maxBlocksListed && !blocks.contains(block))
            blocks.append(block);
        if (!memory->isStore())
            return;
        auto result = m_storeWithKey.add(key, memory);
        if (!result.isNewEntry && result.iterator->value != memory)
            result.iterator->value = nullptr;
    }

    // Bun: matches come back only if they cut every path from the root to this block. On a simple one, no block is dominated by this block.
    template<typename Filter>
    bool mayFindMemoryValueInAnotherBlock(Value* ptr, int64_t offset, NOESCAPE const Filter& filter)
    {
        if (!m_hasIndex)
            return true;
        auto iter = m_blocksWithMemoryValue.find(MemoryKey { ptr, offset });
        if (iter == m_blocksWithMemoryValue.end())
            return false;
        if (iter->value.size() > maxBlocksListed)
            return true;
        return iter->value.containsIf([&](BasicBlock* block) {
            if (m_dominators->dominates(m_block, block))
                return false;
            MemoryValue* match = m_impureBlockData[block].memoryValuesAtTail.find(ptr, offset, filter);
            return match && match != m_value;
        });
    }

    // Bun: with no other store to find, the search says no at the first block that has no successors, if it gets that far.
    bool mayFindAnotherStore(Value* ptr, int64_t offset)
    {
        if (!m_hasIndex || !m_blocksThatReachAnExit.saw(m_block))
            return true;
        auto iter = m_storeWithKey.find(MemoryKey { ptr, offset });
        return iter != m_storeWithKey.end() && iter->value != m_value;
    }

    template<typename Filter>
    bool findStoreAfterClobber(Value* ptr, HeapRange range, NOESCAPE const Filter& filter)
    {
        if (m_value->as<MemoryValue>()->hasFence())
            return false;

        if (!mayFindAnotherStore(ptr, m_value->as<MemoryValue>()->offset()))
            return false;
        
        // We can eliminate a store if every forward path hits a store to the same location before
        // hitting any operation that observes the store. This search seems like it should be
        // expensive, but in the overwhelming majority of cases it will almost immediately hit an 
        // operation that interferes.

        dataLogLnIf(B3EliminateCommonSubexpressionsInternal::verbose, *m_value, ": looking forward for stores to ", *ptr, "...");

        // First search forward in this basic block.
        // FIXME: It would be cool to get rid of this linear search. It's not super critical since
        // we will probably bail out very quickly, but it *is* annoying.
        for (unsigned index = m_index + 1; index < m_block->size(); ++index) {
            Value* value = m_block->at(index);

            if (MemoryValue* memoryValue = value->as<MemoryValue>()) {
                if (memoryValue->lastChild() == ptr && filter(memoryValue))
                    return true;
            }

            Effects effects = value->effects();
            if (effects.reads.overlaps(range) || effects.writes.overlaps(range))
                return false;
        }

        if (!m_block->numSuccessors())
            return false;

        BlockWorklist worklist;
        worklist.pushAll(m_block->successorBlocks());

#if USE(BUN_JSC_ADDITIONS)
        unsigned blocksSearched = 0;
#endif
        while (BasicBlock* block = worklist.pop()) {
#if USE(BUN_JSC_ADDITIONS)
            if (++blocksSearched > maxBlocksToSearch)
                return false;
#endif
            ImpureBlockData& data = m_impureBlockData[block];

            MemoryValue* match = data.memoryStoresAtHead.find(ptr, m_value->as<MemoryValue>()->offset(), filter);
            if (match && match != m_value)
                continue;

            if (data.writes.overlaps(range) || data.reads.overlaps(range))
                return false;

            if (!block->numSuccessors())
                return false;

            worklist.pushAll(block->successorBlocks());
        }

        return true;
    }

    template<typename Filter, typename Replace>
    void handleMemoryValue(
        Value* ptr, HeapRange range, NOESCAPE const Filter& filter, NOESCAPE const Replace& replace)
    {
        // FIXME: Currently we observed some performance regression in this case.
        bool mayReloadInRareBlocks = m_proc.positionIndependent() && !m_value->traps() && m_value->as<MemoryValue>()->readsMutability() != Mutability::Immutable;
        MemoryMatches matches = findMemoryValue(ptr, range, filter, m_proc.positionIndependent() ? m_value->as<MemoryValue>()->readsMutability() : Mutability::Mutable, m_proc.positionIndependent(), mayReloadInRareBlocks);
        if (replaceMemoryValue(matches, replace))
            return;
        m_data.memoryValuesAtTail.add(m_value->as<MemoryValue>());
        didAddMemoryValue(m_block, m_value->as<MemoryValue>());
    }

    template<typename Replace>
    bool replaceMemoryValue(const MemoryMatches& matches, NOESCAPE const Replace& replace)
    {
        if (matches.isEmpty() && m_forwardedMatches.isEmpty())
            return false;

        dataLogLnIf(B3EliminateCommonSubexpressionsInternal::verbose, "Eliminating ", *m_value, " due to ", pointerListDump(matches));
        
        m_changed = true;

        if (matches.isEmpty() && m_forwardedMatches.size() == 1) {
            RELEASE_ASSERT(m_dominators->dominates(m_forwardedMatches[0].first, m_block));
            m_value->replaceWithIdentity(m_forwardedMatches[0].second);
            return true;
        }

        if (matches.size() == 1 && m_forwardedMatches.isEmpty()) {
            MemoryValue* dominatingMatch = matches[0];
            RELEASE_ASSERT(m_dominators->dominates(dominatingMatch->owner, m_block));
            
            dataLogLnIf(B3EliminateCommonSubexpressionsInternal::verbose, "    Eliminating using ", *dominatingMatch);
            Vector<Value*> extraValues;
            if (Value* value = replace(dominatingMatch, extraValues)) {
                for (Value* extraValue : extraValues)
                    m_insertionSet.insertValue(m_index, extraValue);
                m_value->replaceWithIdentity(value);
            } else {
                if (dominatingMatch->isStore())
                    m_value->replaceWithIdentity(dominatingMatch->child(0));
                else
                    m_value->replaceWithIdentity(dominatingMatch);
            }
            return true;
        }

        // addBottom creates a fresh Const; InsertionSet::insertBottom would
        // alias placeholders of the same type within one block.
        Vector<Value*, 4> sources;
        for (MemoryValue* match : matches)
            sources.append(match);
        for (auto& match : m_forwardedMatches)
            sources.append(match.second);
        std::ranges::sort(sources, { }, &Value::index);
        SSACalculator::Variable* var = nullptr;
        auto& variablesWithFirstSource = m_sharedVariables.add(sources[0], Vector<SharedVariable, 1>()).iterator->value;
        for (auto& shared : variablesWithFirstSource) {
            if (shared.opcode == m_value->opcode() && shared.type == m_value->type() && shared.sources == sources)
                var = shared.variable;
        }
        bool isNewVariable = !var;
        if (isNewVariable) {
            var = m_ssa->newVariable();
            variablesWithFirstSource.append({ m_value->opcode(), m_value->type(), sources, var });
        }
        Value* placeholder = m_proc.addBottom(m_value->origin(), m_value->type());
        m_insertionSet.insertValue(m_index, placeholder);
        if (isNewVariable)
            m_placeholderOfVariable.append(placeholder);
        dataLogLnIf(B3EliminateCommonSubexpressionsInternal::verbose, "    Inserting placeholder for value: ", *placeholder);
        if (m_proc.positionIndependent()) {
            MemoryValue* memory = m_value->as<MemoryValue>();
            HeapRange range = memory->readsMutability() == Mutability::Immutable ? HeapRange() : memory->range();
            m_data.forwardedLoadsAtTail.append({ memory->lastChild(), memory->offset(), range, memory->opcode(), memory->type(), placeholder });
            placeholder->owner = m_block;
        }
        m_value->replaceWithIdentity(placeholder);

        for (MemoryValue* match : matches) {
            if (!isNewVariable)
                break;
            m_blocksWithSets.add(match->owner);
            auto& extras = m_sets.add(match, Vector<Value*>()).iterator->value;
            m_matched.add(match);
            Value* value = replace(match, extras);
            if (!value) {
                if (match->isStore())
                    value = match->child(0);
                else
                    value = match;
            }

            m_ssa->newDef(var, match->owner, value);
        }

        if (isNewVariable) {
            for (auto& [block, value] : m_forwardedMatches)
                m_ssa->newDef(var, block, value);
        }

        m_pendingResolutions.append({ placeholder, m_block, var });
        return true;
    }

    template<typename Filter>
    MemoryMatches findMemoryValue(Value* ptr, HeapRange range, NOESCAPE const Filter& filter, Mutability readsMutability = Mutability::Mutable, bool mayUseForwardedLoads = false, bool mayReloadInRareBlocks = false)
    {
        m_forwardedMatches.shrink(0);
        if constexpr (B3EliminateCommonSubexpressionsInternal::verbose) {
            dataLogLn(*m_value, ": looking backward for ", *ptr, "...");
            dataLogLn("    Full value: ", deepDump(m_value));
        }
        
        if (m_value->as<MemoryValue>()->hasFence()) {
            dataLogLnIf(B3EliminateCommonSubexpressionsInternal::verbose, "    Giving up because fences.");
            return { };
        }
        
        if (MemoryValue* match = m_data.memoryValuesAtTail.find(ptr, m_value->as<MemoryValue>()->offset(), filter)) {
            dataLogLnIf(B3EliminateCommonSubexpressionsInternal::verbose, "    Found ", *match, " locally.");
            return { match };
        }

        if (mayUseForwardedLoads) {
            if (Value* forwarded = findForwardedLoad(m_data, ptr)) {
                m_forwardedMatches.append({ m_block, forwarded });
                return { };
            }
        }

        if (readsMutability != Mutability::Immutable && m_data.writes.overlaps(range)) {
            dataLogLnIf(B3EliminateCommonSubexpressionsInternal::verbose, "    Giving up because of writes.");
            return { };
        }

        if (!mayFindMemoryValueInAnotherBlock(ptr, m_value->as<MemoryValue>()->offset(), filter))
            return { };

        BlockWorklist worklist;
        worklist.pushAll(m_block->predecessors());

        MemoryMatches matches;
        constexpr unsigned maxBlocksWithWritesToSearch = 64;
        Vector<std::pair<BasicBlock*, Value*>, 1> forwardedMatches;
        Vector<BasicBlock*, 2> blocksToReloadIn;
        Vector<BasicBlock*, 4> blocksWithCallsOnTheWay;
        unsigned blocksWithWritesSearched = 0;

#if USE(BUN_JSC_ADDITIONS)
        unsigned blocksSearched = 0;
#endif
        while (BasicBlock* block = worklist.pop()) {
            dataLogLnIf(B3EliminateCommonSubexpressionsInternal::verbose, "    Looking at ", *block);
#if USE(BUN_JSC_ADDITIONS)
            if (++blocksSearched > maxBlocksToSearch)
                return { };
#endif

            ImpureBlockData& data = m_impureBlockData[block];

            MemoryValue* match = data.memoryValuesAtTail.find(ptr, m_value->as<MemoryValue>()->offset(), filter);
            dataLogLnIf(B3EliminateCommonSubexpressionsInternal::verbose, "    Consdering match: ", pointerDump(match));
            if (match && match != m_value) {
                dataLogLnIf(B3EliminateCommonSubexpressionsInternal::verbose, "    Found match: ", *match);
                matches.append(match);
                continue;
            }

            if (mayUseForwardedLoads) {
                if (Value* forwarded = findForwardedLoad(data, ptr)) {
                    forwardedMatches.append({ block, forwarded });
                    continue;
                }
            }

            if (data.writes.overlaps(range) && (readsMutability != Mutability::Immutable || ++blocksWithWritesSearched > maxBlocksWithWritesToSearch)) {
                if (mayReloadInRareBlocks && canReloadAtTail(block, ptr, range)) {
                    blocksToReloadIn.append(block);
                    continue;
                }
                dataLogLnIf(B3EliminateCommonSubexpressionsInternal::verbose, "    Giving up because of writes.");
                return { };
            }

            if (mayReloadInRareBlocks && data.hasCall)
                blocksWithCallsOnTheWay.append(block);

            if (!block->numPredecessors()) {
                dataLogLnIf(B3EliminateCommonSubexpressionsInternal::verbose, "    Giving up because it's live at root.");
                // This essentially proves that this is live at the prologue. That means that we
                // cannot reliably optimize this case.
                return { };
            }
            
            worklist.pushAll(block->predecessors());
        }

        if (!blocksToReloadIn.isEmpty()) {
            if (matches.isEmpty() && forwardedMatches.isEmpty())
                return { };
            if (m_data.hasCall)
                return { };
            for (BasicBlock* block : blocksWithCallsOnTheWay) {
                if (m_likelyBlocks.saw(block))
                    return { };
            }
            auto isFollowedByCall = [&](BasicBlock* block, Value* source) {
                if (!m_impureBlockData[block].hasCall || !m_likelyBlocks.saw(block))
                    return false;
                for (unsigned index = block->size(); index--;) {
                    if (block->at(index) == source)
                        return false;
                    if (isCall(block->at(index)))
                        return true;
                }
                return true;
            };
            for (MemoryValue* match : matches) {
                if (isFollowedByCall(match->owner, match))
                    return { };
            }
            for (auto& match : forwardedMatches) {
                if (isFollowedByCall(match.first, match.second))
                    return { };
            }
            if (m_value->effects().controlDependent) {
                for (BasicBlock* block : blocksToReloadIn) {
                    bool isAccessedOnEveryWayHere = matches.containsIf([&](MemoryValue* match) {
                        return m_dominators->dominates(match->owner, block);
                    }) || forwardedMatches.containsIf([&](auto& match) {
                        return m_dominators->dominates(match.first, block);
                    });
                    if (!isAccessedOnEveryWayHere)
                        return { };
                }
            }
            for (BasicBlock* block : blocksToReloadIn) {
                MemoryValue* reload = m_proc.clone(m_value)->as<MemoryValue>();
                block->appendNonTerminal(reload);
                m_impureBlockData[block].memoryValuesAtTail.add(reload);
                m_impureBlockData[block].reads.add(range);
                didAddMemoryValue(block, reload);
                matches.append(reload);
            }
        }
        m_forwardedMatches = WTF::move(forwardedMatches);

        dataLogLnIf(B3EliminateCommonSubexpressionsInternal::verbose, "    Got matches: ", pointerListDump(matches));
        return matches;
    }

    Value* findForwardedLoad(ImpureBlockData& data, Value* ptr)
    {
        MemoryValue* memory = m_value->as<MemoryValue>();
        for (auto& load : data.forwardedLoadsAtTail) {
            if (load.ptr == ptr && load.offset == memory->offset() && load.opcode == memory->opcode() && load.type == memory->type())
                return load.value;
        }
        return nullptr;
    }

    bool canReloadAtTail(BasicBlock* block, Value* ptr, HeapRange range)
    {
        if (!m_hasFoundLikelyBlocks) {
            m_hasFoundLikelyBlocks = true;
            m_likelyBlocks.push(m_proc[0]);
            while (BasicBlock* likelyBlock = m_likelyBlocks.pop()) {
                for (FrequentedBlock successor : likelyBlock->successors()) {
                    if (!successor.isRare())
                        m_likelyBlocks.push(successor.block());
                }
            }
        }
        if (m_likelyBlocks.saw(block) || !m_likelyBlocks.saw(m_block))
            return false;
        if (!ptr->owner || !m_dominators->dominates(ptr->owner, block))
            return false;
        return !block->last()->effects().writes.overlaps(range);
    }

    void processWasmAddressValue(WasmAddressValue* wasmAddress)
    {
        Value* ptr = wasmAddress->child(0);

        if (Value* replacement = m_data.m_candidateWasmAddressesAtTail.get(ptr)) {
            dataLogLnIf(B3EliminateCommonSubexpressionsInternal::verbose, "    Replacing WasmAddress: ", *wasmAddress, " with ", *replacement);
            wasmAddress->replaceWithIdentity(replacement);
            m_changed = true;
            return;
        }

        auto addPtrOnScopeExit = makeScopeExit([&] {
            m_data.m_candidateWasmAddressesAtTail.add(ptr, wasmAddress);
        });

        if (m_data.writesPinned) {
            // Someone before us in this block wrote to pinned. So we have no
            // hope of finding a match if the above search failed.
            return;
        }

        Value* candidateReplacement = nullptr;
        BasicBlock* dominator = nullptr;
        m_dominators->forAllStrictDominatorsOf(m_block, [&] (BasicBlock* block) {
            if (candidateReplacement)
                return;

            if (Value* replacement = m_impureBlockData[block].m_candidateWasmAddressesAtTail.get(ptr)) {
                candidateReplacement = replacement;
                dominator = block;
            }
        });

        if (!candidateReplacement)
            return;

        BlockWorklist worklist;
        worklist.pushAll(m_block->predecessors());
        while (BasicBlock* block = worklist.pop()) {
            if (block == dominator)
                continue;
            if (m_impureBlockData[block].writesPinned) {
                candidateReplacement = nullptr;
                break;
            }
            worklist.pushAll(block->predecessors());
        }

        if (candidateReplacement) {
            dataLogLnIf(B3EliminateCommonSubexpressionsInternal::verbose, "    Replacing WasmAddress: ", *wasmAddress, " with ", *candidateReplacement);
            wasmAddress->replaceWithIdentity(candidateReplacement);
            m_changed = true;
        }
    }

    Procedure& m_proc;

    Dominators* m_dominators { nullptr };
    PureCSE m_pureCSE;

    IndexMap<BasicBlock*, ImpureBlockData> m_impureBlockData;

#if USE(BUN_JSC_ADDITIONS)
    // A search for a memory value gives up beyond so many blocks. That keeps the phase linear in the size of the procedure.
    static constexpr unsigned maxBlocksToSearch = 1024;
#endif
    static constexpr unsigned minBlocksForIndex = 64;
    static constexpr unsigned maxBlocksListed = 4;
    bool m_hasIndex { false };
    // One more than maxBlocksListed: there are more.
    UncheckedKeyHashMap<MemoryKey, Vector<BasicBlock*, 2>> m_blocksWithMemoryValue;
    // Null: there are several.
    UncheckedKeyHashMap<MemoryKey, MemoryValue*> m_storeWithKey;
    BlockWorklist m_blocksThatReachAnExit;
    BlockWorklist m_likelyBlocks;
    bool m_hasFoundLikelyBlocks { false };
    Vector<std::pair<BasicBlock*, Value*>, 1> m_forwardedMatches;

    ImpureBlockData m_data;

    BasicBlock* m_block { nullptr };
    unsigned m_index { 0 };
    Value* m_value { nullptr };

    // Match -> extra fixup values (e.g. BitAnd masks for packed Wasm types),
    // flushed at match site during finalize().
    UncheckedKeyHashMap<Value*, Vector<Value*>> m_sets;
    IndexSet<Value*> m_matched;
    // Blocks that own at least one m_sets key, so finalize() can skip whole blocks.
    IndexSet<BasicBlock*> m_blocksWithSets;

    InsertionSet m_insertionSet;

    std::unique_ptr<SSACalculator> m_ssa;

    struct PendingResolution {
        Value* placeholder;
        BasicBlock* useBlock;
        SSACalculator::Variable* var;
    };
    Vector<PendingResolution> m_pendingResolutions;
    Vector<Value*> m_placeholderOfVariable;

    struct SharedVariable {
        Opcode opcode;
        Type type;
        Vector<Value*, 4> sources;
        SSACalculator::Variable* variable;
    };
    UncheckedKeyHashMap<Value*, Vector<SharedVariable, 1>> m_sharedVariables;

    bool m_changed { false };
};

} // anonymous namespace

bool eliminateCommonSubexpressions(Procedure& proc)
{
    PhaseScope phaseScope(proc, "eliminateCommonSubexpressions"_s);

    CSE cse(proc);
    return cse.run();
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
