/*
 * Copyright (C) 2017-2021 Apple Inc. All rights reserved.
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

#pragma once

#include "AtomIndices.h"
#include "HeapCellInlines.h"
#include "IsoCellSet.h"
#include "MarkedBlockInlines.h"
#include <wtf/Atomics.h>

namespace JSC {

// GIL-off (TSAN family gc-marking-residual, wave-5 residual, amended per the
// wave-5 review): addSlow() (IsoCellSet.cpp) RELEASE-publishes the lazily
// allocated per-block bits into the unique_ptr's pointer word. This load is
// the matching read side: the ADDRESS DEPENDENCY on the returned pointer
// orders the subsequent bit-word accesses after the BitSet's construction on
// all supported targets (x86-64 TSO; ARM64 dependency ordering), so the
// non-TSAN load is RELAXED + WTF::dependentLoadLoadFence() — codegen
// identical to the previous plain load (the fence is a compiler barrier on
// these targets). An earlier draft used std::memory_order_consume, which all
// compilers promote to ACQUIRE: that emitted a new ldar on ARM64 flag-off
// in IsoCellSet::add()/contains() — FAST paths of the cell-set protocol run
// during every GC for finalizer/destruction subspaces — violating the
// flag-off-codegen-unchanged rule. TSAN cannot see the dependency edge, so
// under TSAN_ENABLED ONLY the load is acquire (the JITCodePointerConsumeOrder
// / Structure.h tryRareData precedent; recorded in TSAN-TRIAGE.md §13).
ALWAYS_INLINE WTF::BitSet<MarkedBlock::atomsPerBlock>* isoCellSetBitsPointerConcurrently(const std::unique_ptr<WTF::BitSet<MarkedBlock::atomsPerBlock>>& ptr)
{
    static_assert(sizeof(std::unique_ptr<WTF::BitSet<MarkedBlock::atomsPerBlock>>) == sizeof(WTF::BitSet<MarkedBlock::atomsPerBlock>*));
#if TSAN_ENABLED
    return WTF::atomicLoad(std::bit_cast<WTF::BitSet<MarkedBlock::atomsPerBlock>**>(const_cast<std::unique_ptr<WTF::BitSet<MarkedBlock::atomsPerBlock>>*>(&ptr)), std::memory_order_acquire);
#else
    auto* bits = WTF::atomicLoad(std::bit_cast<WTF::BitSet<MarkedBlock::atomsPerBlock>**>(const_cast<std::unique_ptr<WTF::BitSet<MarkedBlock::atomsPerBlock>>*>(&ptr)), std::memory_order_relaxed);
    WTF::dependentLoadLoadFence();
    return bits;
#endif
}

inline bool IsoCellSet::add(HeapCell* cell)
{
    // We want to return true if the cell is newly added. concurrentTestAndSet() returns the
    // previous bit value. Since we're trying to set the bit for this add, the cell would be
    // newly added only if the previous bit was not set. Hence, our result will be the
    // inverse of the concurrentTestAndSet() result.
    if (cell->isPreciseAllocation())
        return !m_lowerTierPreciseBits.concurrentTestAndSet(cell->preciseAllocation().lowerTierPreciseIndex());
    AtomIndices atomIndices(cell);
    auto& bitsPtrRef = m_bits[atomIndices.blockIndex];
    auto* bits = isoCellSetBitsPointerConcurrently(bitsPtrRef);
    if (!bits) [[unlikely]]
        bits = addSlow(atomIndices.blockIndex);
    return !bits->concurrentTestAndSet(atomIndices.atomNumber);
}

inline bool IsoCellSet::remove(HeapCell* cell)
{
    // We want to return true if the cell was previously present and will be removed now.
    // concurrentTestAndClear() returns the previous bit value. Since we're trying to clear
    // the bit for this remove, the cell would be newly removed only if the previous bit
    // was set. Hence, our result matches the concurrentTestAndClear() result.
    if (cell->isPreciseAllocation())
        return m_lowerTierPreciseBits.concurrentTestAndClear(cell->preciseAllocation().lowerTierPreciseIndex());
    AtomIndices atomIndices(cell);
    auto& bitsPtrRef = m_bits[atomIndices.blockIndex];
    auto* bits = isoCellSetBitsPointerConcurrently(bitsPtrRef);
    if (!bits)
        return false;
    return bits->concurrentTestAndClear(atomIndices.atomNumber);
}

inline bool IsoCellSet::contains(HeapCell* cell) const
{
    if (cell->isPreciseAllocation())
        return !m_lowerTierPreciseBits.concurrentGet(cell->preciseAllocation().lowerTierPreciseIndex());
    AtomIndices atomIndices(cell);
    auto* bits = isoCellSetBitsPointerConcurrently(m_bits[atomIndices.blockIndex]);
    if (bits)
        return bits->concurrentGet(atomIndices.atomNumber);
    return false;
}

template<typename Func>
void IsoCellSet::forEachMarkedCell(const Func& func)
{
    BlockDirectory& directory = m_subspace.m_directory;
    directory.assertIsMutatorOrMutatorIsStopped();
    (directory.markingNotEmptyBitsView() & m_blocksWithBits).forEachSetBit(
        [&] (unsigned blockIndex) {
            MarkedBlock::Handle* block = directory.m_blocks[blockIndex];

            auto* bits = isoCellSetBitsPointerConcurrently(m_bits[blockIndex]);
            block->forEachMarkedCell(
                [&] (unsigned atomNumber, HeapCell* cell, HeapCell::Kind kind) -> IterationStatus {
                    if (bits->concurrentGet(atomNumber))
                        func(cell, kind);
                    return IterationStatus::Continue;
                });
        });

    CellAttributes attributes = m_subspace.attributes();
    m_subspace.forEachPreciseAllocation(
        [&] (PreciseAllocation* allocation) {
            if (m_lowerTierPreciseBits.concurrentGet(allocation->lowerTierPreciseIndex()) && allocation->isMarked())
                func(allocation->cell(), attributes.cellKind);
        });
}

template<typename Visitor, typename Func>
Ref<SharedTask<void(Visitor&)>> IsoCellSet::forEachMarkedCellInParallel(const Func& func)
{
    class Task final : public SharedTask<void(Visitor&)> {
    public:
        Task(IsoCellSet& set, const Func& func)
            : m_set(set)
            , m_blockSource(set.parallelNotEmptyMarkedBlockSource())
            , m_func(func)
        {
        }
        
        void run(Visitor& visitor) final
        {
            while (MarkedBlock::Handle* handle = m_blockSource->run()) {
                unsigned blockIndex = handle->index();
                auto* bits = isoCellSetBitsPointerConcurrently(m_set.m_bits[blockIndex]);
                handle->forEachMarkedCell(
                    [&] (unsigned atomNumber, HeapCell* cell, HeapCell::Kind kind) -> IterationStatus {
                        if (bits->concurrentGet(atomNumber))
                            m_func(visitor, cell, kind);
                        return IterationStatus::Continue;
                    });
            }

            if (m_doneVisitingPreciseAllocations.test_and_set(std::memory_order_relaxed))
                return;

            CellAttributes attributes = m_set.m_subspace.attributes();
            m_set.m_subspace.forEachPreciseAllocation(
                [&] (PreciseAllocation* allocation) {
                    if (m_set.m_lowerTierPreciseBits.concurrentGet(allocation->lowerTierPreciseIndex()) && allocation->isMarked())
                        m_func(visitor, allocation->cell(), attributes.cellKind);
                });
        }
        
    private:
        IsoCellSet& m_set;
        Ref<SharedTask<MarkedBlock::Handle*()>> m_blockSource;
        Func m_func;
        std::atomic_flag m_doneVisitingPreciseAllocations { };
    };
    
    return adoptRef(*new Task(*this, func));
}

template<typename Func>
void IsoCellSet::forEachLiveCell(const Func& func)
{
    BlockDirectory& directory = m_subspace.m_directory;
    m_blocksWithBits.forEachSetBit(
        [&] (unsigned blockIndex) {
            MarkedBlock::Handle* block = directory.m_blocks[blockIndex];

            auto* bits = isoCellSetBitsPointerConcurrently(m_bits[blockIndex]);
            block->forEachCell(
                [&] (unsigned atomNumber, HeapCell* cell, HeapCell::Kind kind) -> IterationStatus {
                    if (bits->concurrentGet(atomNumber) && block->isLive(cell))
                        func(cell, kind);
                    return IterationStatus::Continue;
                });
        });

    CellAttributes attributes = m_subspace.attributes();
    m_subspace.forEachPreciseAllocation(
        [&] (PreciseAllocation* allocation) {
            if (m_lowerTierPreciseBits.concurrentGet(allocation->lowerTierPreciseIndex()) && allocation->isLive())
                func(allocation->cell(), attributes.cellKind);
        });
}

inline void IsoCellSet::clearLowerTierPreciseCell(unsigned index)
{
    m_lowerTierPreciseBits.concurrentTestAndClear(index);
}

} // namespace JSC

