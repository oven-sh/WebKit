/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "B3MergeAdjacentConstantStores.h"

#if ENABLE(B3_JIT)

#include "AssemblerCommon.h"
#include "B3BasicBlockInlines.h"
#include "B3Const64Value.h"
#include "B3InsertionSetInlines.h"
#include "B3MemoryValue.h"
#include "B3PatchpointValue.h"
#include "B3PhaseScope.h"
#include "B3ProcedureInlines.h"
#include "B3StackmapGenerationParams.h"
#include "B3ValueInlines.h"
#include "CCallHelpers.h"
#include "CPU.h"
#include <tuple>
#include <wtf/HashSet.h>
#include <wtf/Range.h>

namespace JSC { namespace B3 {

namespace {

class MergeAdjacentConstantStores {
public:
    MergeAdjacentConstantStores(Procedure& proc)
        : m_proc(proc)
        , m_insertionSet(proc)
    {
    }

    bool run()
    {
        for (BasicBlock* block : m_proc) {
            collectPairs(block);
            chooseSmallerPairs();
            mergePairs(block);
            if (isX86()) {
                collectZeroStretches(block);
                mergeZeroStretches(block);
            }
        }
        return m_changed;
    }

private:
    struct ConstantStore {
        int64_t offset() const { return store->offset(); }

        unsigned index;
        MemoryValue* store;
    };

    struct Pair {
        unsigned insertionIndex() const { return std::max(low.index, high.index); }

        ConstantStore low;
        ConstantStore high;
        uint64_t constant;
        unsigned run;
    };

    struct ZeroStretch {
        unsigned insertionIndex() const
        {
            unsigned result = 0;
            for (const ConstantStore& zeroStore : stores)
                result = std::max(result, zeroStore.index);
            return result;
        }

        Vector<ConstantStore, 8> stores;
    };

    static bool isPlainStore(Value* value)
    {
        return (value->opcode() == Store || value->opcode() == Store8 || value->opcode() == Store16) && !value->kind().traps() && !value->as<MemoryValue>()->hasFence();
    }

    // An object of a thousand slots is filled by a thousand stores, in no order that anybody has promised. No two collected stores have one offset.
    bool overlapsCollectedStore(int64_t begin, int64_t end, int64_t width)
    {
        constexpr unsigned mostStoresSearchedOneByOne = 16;
        if (m_stores.size() <= mostStoresSearchedOneByOne) {
            return std::ranges::any_of(m_stores, [&](const ConstantStore& other) {
                return begin < other.offset() + width && other.offset() < end;
            });
        }
        auto keyOf = [](int64_t offset) {
            return static_cast<uint64_t>(offset + (static_cast<int64_t>(1) << 32));
        };
        for (; m_numberOfOffsetsNoted < m_stores.size(); ++m_numberOfOffsetsNoted)
            m_offsets.add(keyOf(m_stores[m_numberOfOffsetsNoted].offset()));
        for (int64_t offset = begin - width + 1; offset < end; ++offset) {
            if (m_offsets.contains(keyOf(offset)))
                return true;
        }
        return false;
    }

    template<typename IsCandidate, typename DidEndRun>
    void forEachRun(BasicBlock* block, int64_t width, const IsCandidate& isCandidate, const DidEndRun& didEndRun)
    {
        auto endRun = [&] {
            std::ranges::sort(m_stores, { }, &ConstantStore::offset);
            didEndRun();
            m_stores.shrink(0);
            if (m_numberOfOffsetsNoted) {
                m_offsets.clear();
                m_numberOfOffsetsNoted = 0;
            }
            m_collectedBytes = { };
            m_base = nullptr;
        };
        m_base = nullptr;
        for (unsigned index = 0; index < block->size(); ++index) {
            Value* value = block->at(index);
            if (isPlainStore(value)) {
                auto* store = value->as<MemoryValue>();
                int64_t begin = store->offset();
                int64_t end = begin + static_cast<int64_t>(store->accessByteSize());
                Range<int64_t> bytes(begin, end);
                bool overlaps = m_collectedBytes.overlaps(bytes) && overlapsCollectedStore(begin, end, width);
                if (store->lastChild() != m_base || overlaps) {
                    endRun();
                    m_base = store->lastChild();
                }
                if (isCandidate(store)) {
                    m_stores.append({ index, store });
                    m_collectedBytes |= bytes;
                }
                continue;
            }
            if (m_stores.isEmpty())
                continue;
            Effects effects = value->effects();
            if (effects.mustExecute() || effects.reads || effects.readsLocalState || effects.readsPinned || effects.controlDependent)
                endRun();
        }
        endRun();
    }

    void collectPairs(BasicBlock* block)
    {
        m_run = 0;
        m_pairs.shrink(0);
        auto isCandidate = [](MemoryValue* store) {
            return store->opcode() == Store && store->child(0)->hasInt32();
        };
        forEachRun(block, 4, isCandidate, [&] {
            for (unsigned i = 0; i + 1 < m_stores.size(); ++i) {
                auto& low = m_stores[i];
                auto& high = m_stores[i + 1];
                if (low.offset() % 8 || high.offset() != low.offset() + 4)
                    continue;
                m_pairs.append({ low, high, static_cast<uint32_t>(low.store->child(0)->asInt32()) | static_cast<uint64_t>(static_cast<uint32_t>(high.store->child(0)->asInt32())) << 32, m_run });
            }
            ++m_run;
        });
    }

    void collectZeroStretches(BasicBlock* block)
    {
        m_zeroStretches.shrink(0);
        auto isCandidate = [](MemoryValue* store) {
            return store->opcode() == Store && store->child(0)->isInt64(0);
        };
        forEachRun(block, 8, isCandidate, [&] {
            for (unsigned first = 0, end = 0; first < m_stores.size(); first = end) {
                for (end = first + 1; end < m_stores.size() && m_stores[end].offset() == m_stores[end - 1].offset() + 8; ++end) { }
                unsigned numberOfMergedStores = (end - first) & ~1u;
                if (!numberOfMergedStores)
                    continue;
                ZeroStretch stretch;
                stretch.stores.append(m_stores.subspan(first, numberOfMergedStores));
                m_zeroStretches.append(WTF::move(stretch));
            }
        });
    }

    static void storeZeroVectors(CCallHelpers& jit, GPRReg base, FPRReg zero, int32_t firstOffset, size_t numberOfVectors)
    {
#if CPU(X86_64)
        jit.moveZeroToDouble(zero);
        for (size_t i = 0; i < numberOfVectors; ++i) {
            int32_t offset = firstOffset + static_cast<int32_t>(i * 16);
            if (jit.supportsAVX())
                jit.m_assembler.vmovups_rm(zero, offset, base);
            else
                jit.m_assembler.movups_rm(zero, offset, base);
        }
#else
        UNUSED_PARAM(jit);
        UNUSED_PARAM(base);
        UNUSED_PARAM(zero);
        UNUSED_PARAM(firstOffset);
        UNUSED_PARAM(numberOfVectors);
        RELEASE_ASSERT_NOT_REACHED();
#endif
    }

    void mergeZeroStretches(BasicBlock* block)
    {
        std::ranges::sort(m_zeroStretches, { }, &ZeroStretch::insertionIndex);
        for (ZeroStretch& stretch : m_zeroStretches) {
            unsigned index = stretch.insertionIndex();
            MemoryValue* first = stretch.stores[0].store;
            Vector<HeapRange, 8> ranges;
            for (const ConstantStore& zeroStore : stretch.stores)
                ranges.append(zeroStore.store->range());

            PatchpointValue* patchpoint = m_insertionSet.insert<PatchpointValue>(index, Void, block->at(index)->origin());
            patchpoint->effects = Effects::none();
            patchpoint->effects.writes = m_proc.heapSets().create(ranges.span());
            patchpoint->appendSomeRegister(first->lastChild());
            patchpoint->numFPScratchRegisters = 1;
            patchpoint->setRunsWithoutFrame(true);
            patchpoint->setGenerator([firstOffset = first->offset(), numberOfVectors = stretch.stores.size() / 2](CCallHelpers& jit, const StackmapGenerationParams& params) {
                storeZeroVectors(jit, params[0].gpr(), params.fpScratch(0), firstOffset, numberOfVectors);
            });
            for (const ConstantStore& zeroStore : stretch.stores) {
                m_proc.movedLoadLog().willMove(zeroStore.store);
                zeroStore.store->replaceWithNop();
            }
        }
        m_insertionSet.execute(block);
        m_changed |= !m_zeroStretches.isEmpty();
    }

    void chooseSmallerPairs()
    {
        std::ranges::sort(m_pairs, [](const Pair& a, const Pair& b) {
            return std::tuple(a.constant, a.run, a.low.offset()) < std::tuple(b.constant, b.run, b.low.offset());
        });
        unsigned numberOfChosenPairs = 0;
        for (unsigned first = 0, end = 0; first < m_pairs.size(); first = end) {
            unsigned adjacent = 0;
            for (end = first + 1; end < m_pairs.size() && m_pairs[end].constant == m_pairs[first].constant; ++end) { }
            for (unsigned i = first; i + 1 < end; ++i) {
                if (m_pairs[i + 1].run != m_pairs[i].run || m_pairs[i + 1].low.offset() != m_pairs[i].low.offset() + 8)
                    continue;
                ++adjacent;
                ++i;
            }
            uint64_t constant = m_pairs[first].constant;
            bool isSmaller = isARM64()
                ? adjacent > (constant && !ARM64LogicalImmediate::create64(constant).isValid() ? 1u : 0u)
                : end - first >= 2 || isRepresentableAs<int32_t>(static_cast<int64_t>(constant));
            if (!isSmaller)
                continue;
            for (unsigned i = first; i < end; ++i)
                m_pairs[numberOfChosenPairs++] = m_pairs[i];
        }
        m_pairs.shrink(numberOfChosenPairs);
    }

    void mergePairs(BasicBlock* block)
    {
        std::ranges::sort(m_pairs, { }, &Pair::insertionIndex);
        for (auto& pair : m_pairs) {
            unsigned index = pair.insertionIndex();
            Origin origin = block->at(index)->origin();
            Value* constant = m_insertionSet.insert<Const64Value>(index, origin, static_cast<int64_t>(pair.constant));
            m_insertionSet.insert<MemoryValue>(index, Store, origin, constant, pair.low.store->lastChild(), pair.low.store->offset(), pair.low.store->range() | pair.high.store->range());
            for (MemoryValue* store : { pair.low.store, pair.high.store }) {
                m_proc.movedLoadLog().willMove(store);
                store->replaceWithNop();
            }
        }
        m_insertionSet.execute(block);
        m_changed |= !m_pairs.isEmpty();
    }

    Procedure& m_proc;
    InsertionSet m_insertionSet;
    Vector<ConstantStore, 32> m_stores;
    // Of the first m_numberOfOffsetsNoted of m_stores, once there are many.
    HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_offsets;
    unsigned m_numberOfOffsetsNoted { 0 };
    Range<int64_t> m_collectedBytes;
    Vector<Pair, 16> m_pairs;
    Vector<ZeroStretch, 4> m_zeroStretches;
    Value* m_base { nullptr };
    unsigned m_run { 0 };
    bool m_changed { false };
};

} // anonymous namespace

bool mergeAdjacentConstantStores(Procedure& proc)
{
    PhaseScope phaseScope(proc, "mergeAdjacentConstantStores"_s);
    if (!proc.hasEverAdded(Store))
        return false;
    MergeAdjacentConstantStores mergeAdjacentConstantStores(proc);
    return mergeAdjacentConstantStores.run();
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
