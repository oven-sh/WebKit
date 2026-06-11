/*
 * Copyright (C) 2017-2023 Apple Inc. All rights reserved.
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
#include "IsoCellSet.h"

#include "MarkedBlockInlines.h"
#include <wtf/Atomics.h>

namespace JSC {

IsoCellSet::IsoCellSet(IsoSubspace& subspace)
    : m_subspace(subspace)
{
    ASSERT_WITH_MESSAGE(!subspace.isPreciseOnly(), "IsoSubspaces with precise-only allocations are not supported by IsoCellSet");
    size_t size = subspace.m_directory.m_blocks.size();
    m_blocksWithBits.resize(size);
    m_bits.grow(size);
    subspace.m_cellSets.append(this);
}

IsoCellSet::~IsoCellSet()
{
    if (isOnList())
        BasicRawSentinelNode<IsoCellSet>::remove();
}

Ref<SharedTask<MarkedBlock::Handle*()>> IsoCellSet::parallelNotEmptyMarkedBlockSource()
{
    class Task final : public SharedTask<MarkedBlock::Handle*()> {
    public:
        Task(IsoCellSet& set)
            : m_set(set)
            , m_directory(set.m_subspace.m_directory)
        {
        }
        
        MarkedBlock::Handle* run() final
        {
            // m_done is read here without m_lock by design (fast path); the
            // transition to true happens under m_lock below. Relaxed atomic
            // makes the monotonic-flag race well-defined.
            if (m_done.load(std::memory_order_relaxed))
                return nullptr;
            // SharedGC (T8 audit, I5b): parallel constraint/marking helper —
            // runs only inside the stop window once shared (deviation 4), so
            // this lock-free bit scan sees a stable m_bits.
            m_directory.assertIsMutatorOrMutatorIsStopped();
            Locker locker { m_lock };
            auto bits = m_directory.markingNotEmptyBitsView() & m_set.m_blocksWithBits;
            m_index = bits.findBit(m_index, true);
            if (m_index >= m_directory.m_blocks.size()) {
                m_done.store(true, std::memory_order_relaxed);
                return nullptr;
            }
            return m_directory.m_blocks[m_index++];
        }
        
    private:
        IsoCellSet& m_set;
        BlockDirectory& m_directory;
        size_t m_index { 0 };
        Lock m_lock;
        Atomic<bool> m_done { false };
    };
    
    return adoptRef(*new Task(*this));
}

NEVER_INLINE WTF::BitSet<MarkedBlock::atomsPerBlock>* IsoCellSet::addSlow(unsigned blockIndex)
{
    Locker locker { m_subspace.m_directory.m_bitvectorLock };
    auto& bitsPtrRef = m_bits[blockIndex];
    auto* bits = bitsPtrRef.get();
    if (!bits) {
        // GIL-off (TSAN family gc-marking-residual, wave-5 residual): the read
        // side (isoCellSetBitsPointerConcurrently, IsoCellSetInlines.h) was
        // converted to an atomic load in wave 4, but this lazy-segment
        // publication was still a PLAIN unique_ptr assignment ordered only by
        // a storeStoreFence — a one-sided data race against the concurrent
        // atomic readers, and the fence is invisible to TSAN, so the freshly
        // zero-initialized BitSet words had no modeled happens-before edge
        // either (the "x BitSet load" report). Publish with a RELEASE store
        // into the unique_ptr's pointer word; it pairs with the consume load
        // on the read side to order the BitSet's construction before any
        // concurrent bit access. All mutation of m_bits[] slots remains
        // serialized by m_bitvectorLock, so releasing the unique_ptr into the
        // slot transfers ownership without a competing writer.
        static_assert(sizeof(std::unique_ptr<WTF::BitSet<MarkedBlock::atomsPerBlock>>) == sizeof(WTF::BitSet<MarkedBlock::atomsPerBlock>*));
        auto newBits = makeUnique<WTF::BitSet<MarkedBlock::atomsPerBlock>>();
        bits = newBits.get();
        WTF::atomicStore(std::bit_cast<WTF::BitSet<MarkedBlock::atomsPerBlock>**>(&bitsPtrRef), newBits.release(), std::memory_order_release);
        // Keep the fence: it orders the pointer publication above before the
        // m_blocksWithBits publication below (the sweepToFreeList protocol
        // reads m_blocksWithBits, loadLoadFences, then expects m_bits to be
        // non-null). A release store only orders EARLIER stores; it does not
        // stop this later store from being reordered before it.
        WTF::storeStoreFence();
        m_blocksWithBits[blockIndex] = true;
    }
    return bits;
}

void IsoCellSet::didResizeBits(unsigned newSize)
{
    m_blocksWithBits.resize(newSize);
    m_bits.grow(newSize);
}

void IsoCellSet::didRemoveBlock(unsigned blockIndex)
{
    {
        Locker locker { m_subspace.m_directory.m_bitvectorLock };
        m_blocksWithBits[blockIndex] = false;
    }
    m_bits[blockIndex] = nullptr;
}

void IsoCellSet::sweepToFreeList(MarkedBlock::Handle* block)
{
    RELEASE_ASSERT(!block->isAllocated());
    
    if (!m_blocksWithBits[block->index()])
        return;
    
    WTF::loadLoadFence();
    
    if (!m_bits[block->index()]) {
        dataLog("FATAL: for block index ", block->index(), ":\n");
        dataLog("Blocks with bits says: ", !!m_blocksWithBits[block->index()], "\n");
        dataLog("Bits says: ", RawPointer(m_bits[block->index()].get()), "\n");
        RELEASE_ASSERT_NOT_REACHED();
    }
    
    if (block->block().hasAnyNewlyAllocated()) {
        // The newlyAllocated() bits are a superset of the marks() bits.
        m_bits[block->index()]->concurrentFilter(block->block().newlyAllocated());
        return;
    }

    if (block->isEmpty() || block->areMarksStaleForSweep()) {
        {
            // Holding the bitvector lock happens to be enough because that's what we also hold in
            // other places where we manipulate this bitvector.
            Locker locker { m_subspace.m_directory.m_bitvectorLock };
            m_blocksWithBits[block->index()] = false;
        }
        m_bits[block->index()] = nullptr;
        return;
    }
    
    m_bits[block->index()]->concurrentFilter(block->block().marks());
}

} // namespace JSC

