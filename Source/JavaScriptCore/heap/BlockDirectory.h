/*
 * Copyright (C) 2012-2024 Apple Inc. All rights reserved.
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

#include <JavaScriptCore/BlockDirectoryBits.h>
#include <JavaScriptCore/CellAttributes.h>
#include <JavaScriptCore/FreeList.h>
#include <JavaScriptCore/JSExportMacros.h>
#include <JavaScriptCore/LocalAllocator.h>
#include <JavaScriptCore/MarkedBlock.h>
#include <limits>
#include <wtf/Atomics.h>
#include <wtf/DataLog.h>
#include <wtf/DebugHeap.h>
#include <wtf/Lock.h>
#include <wtf/SharedTask.h>
#include <wtf/Vector.h>

namespace WTF {
class SimpleStats;
}

namespace JSC {

class GCDeferralContext;
class Heap;
class IsoCellSet;
class MarkedSpace;
class LLIntOffsetsExtractor;

DECLARE_ALLOCATOR_WITH_HEAP_IDENTIFIER(BlockDirectory);

class BlockDirectory {
    WTF_MAKE_NONCOPYABLE(BlockDirectory);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED_WITH_HEAP_IDENTIFIER(BlockDirectory, BlockDirectory);
    
    friend class LLIntOffsetsExtractor;

public:
    BlockDirectory(Heap&, size_t cellSize);
    ~BlockDirectory();
    void NODELETE setSubspace(Subspace*);
    void lastChanceToFinalize();
    void prepareForAllocation();
    void stopAllocating();
    void stopAllocatingForGood();
    void resumeAllocating();
    void NODELETE beginMarkingForFullCollection();
    void endMarking();
    void NODELETE snapshotUnsweptForEdenCollection();
    void NODELETE snapshotUnsweptForFullCollection();
    void sweep();
    void shrink();
    void assertNoUnswept();
    size_t cellSize() const { return m_cellSize; }
    CellAttributes attributes() const { return m_attributes; }
    DestructionMode destruction() const { return m_attributes.destruction; }
    HeapCell::Kind cellKind() const { return m_attributes.cellKind; }

    inline void forEachBlock(const std::invocable<MarkedBlock::Handle*> auto&);
    inline void forEachNotEmptyBlock(const std::invocable<MarkedBlock::Handle*> auto&);
    
    RefPtr<SharedTask<MarkedBlock::Handle*()>> parallelNotEmptyBlockSource();
    
    void addBlock(MarkedBlock::Handle*);
    enum class WillDeleteBlock : bool { No, Yes };
    // If WillDeleteBlock::Yes is passed then the block will be left in an invalid state. We do this, however, to avoid potentially paging in / decompressing old blocks to update their handle just before freeing them.
    void removeBlock(MarkedBlock::Handle*, WillDeleteBlock = WillDeleteBlock::No);

    void updatePercentageOfPagedOutPages(WTF::SimpleStats&);
    
#if ASSERT_ENABLED
    JS_EXPORT_PRIVATE void assertIsMutatorOrMutatorIsStopped() const WTF_ASSERTS_ACQUIRED_SHARED_LOCK(m_bitvectorLock);
    void assertSweeperIsSuspended() const WTF_ASSERTS_ACQUIRED_LOCK(m_bitvectorLock);
#else
    ALWAYS_INLINE void assertIsMutatorOrMutatorIsStopped() const WTF_ASSERTS_ACQUIRED_SHARED_LOCK(m_bitvectorLock) { }
    ALWAYS_INLINE void assertSweeperIsSuspended() const WTF_ASSERTS_ACQUIRED_LOCK(m_bitvectorLock) { }
#endif
    // This feels like it shouldn't be needed to go from assertIsMutatorOrMutatorIsStopped -> Locker but Clang's seems to think it is necessary
    // to release the capability.
    ALWAYS_INLINE void releaseAssertAcquiredBitVectorLock() const WTF_RELEASES_SHARED_CAPABILITY(m_bitvectorLock) WTF_IGNORES_THREAD_SAFETY_ANALYSIS { }

    Lock& bitvectorLock() LIFETIME_BOUND WTF_RETURNS_LOCK(m_bitvectorLock) { return m_bitvectorLock; }

#define BLOCK_DIRECTORY_BIT_ACCESSORS(lowerBitName, capitalBitName)     \
    bool is ## capitalBitName(size_t index) const WTF_REQUIRES_SHARED_LOCK(m_bitvectorLock) { return m_bits.is ## capitalBitName(index); } \
    bool is ## capitalBitName(MarkedBlock::Handle* block) const WTF_REQUIRES_SHARED_LOCK(m_bitvectorLock) { return is ## capitalBitName(block->index()); } \
    BlockDirectoryBits::BlockDirectoryBitVectorView<BlockDirectoryBits::Kind::capitalBitName> lowerBitName ## BitsView() const WTF_REQUIRES_SHARED_LOCK(m_bitvectorLock) { return m_bits.lowerBitName(); } \
    \
    void setIs ## capitalBitName(size_t index, bool value) WTF_REQUIRES_LOCK(m_bitvectorLock) { m_bits.setIs ## capitalBitName(index, value); } \
    void setIs ## capitalBitName(MarkedBlock::Handle* block, bool value) WTF_REQUIRES_LOCK(m_bitvectorLock) { setIs ## capitalBitName(block->index(), value); } \
    BlockDirectoryBits::BlockDirectoryBitVectorRef<BlockDirectoryBits::Kind::capitalBitName> lowerBitName ## Bits() WTF_REQUIRES_LOCK(m_bitvectorLock) { return m_bits.lowerBitName(); }

    FOR_EACH_BLOCK_DIRECTORY_BIT(BLOCK_DIRECTORY_BIT_ACCESSORS)
#undef BLOCK_DIRECTORY_BIT_ACCESSORS

    template<typename Func>
    void forEachBitVector(const Func& func) WTF_REQUIRES_LOCK(m_bitvectorLock)
    {
#define BLOCK_DIRECTORY_BIT_CALLBACK(lowerBitName, capitalBitName) \
        func(m_bits.lowerBitName());
        FOR_EACH_BLOCK_DIRECTORY_BIT(BLOCK_DIRECTORY_BIT_CALLBACK);
#undef BLOCK_DIRECTORY_BIT_CALLBACK
    }
    
    template<typename Func>
    void forEachBitVectorWithName(const Func& func) const WTF_REQUIRES_SHARED_LOCK(m_bitvectorLock)
    {
#define BLOCK_DIRECTORY_BIT_CALLBACK(lowerBitName, capitalBitName) \
        func(m_bits.lowerBitName(), #capitalBitName);
        FOR_EACH_BLOCK_DIRECTORY_BIT(BLOCK_DIRECTORY_BIT_CALLBACK);
#undef BLOCK_DIRECTORY_BIT_CALLBACK
    }
    
    // GIL-off (TSAN family gc-marking-residual): the m_nextDirectory list is
    // traversed concurrently by the marker/sweeper while a mutator appends a
    // newly created directory. The writer publishes the fully constructed
    // directory with a storeStoreFence before linking (MarkedSpace.cpp
    // addBlockDirectory), so relaxed atomics on the link word itself are
    // sufficient; they only make the previously-plain pointer accesses
    // well-defined C++. Codegen is identical to the plain accesses.
    // TSAN r11 (reports 3-8, ctor-vs-findEmptyBlockToSteal): TSAN cannot see
    // the storeStoreFence, so without an ordered link edge every constructor
    // write of a freshly appended directory pairs against a sibling Thread's
    // steal-walk reads. Under TSAN only, the link stores are release and the
    // link loads acquire (all traversals are slow paths); production keeps
    // the relaxed accesses (the fence is the real publication edge). Same
    // gate shape as JSString::fiberConcurrently (TSAN-TRIAGE §13.4).
#if TSAN_ENABLED
    static constexpr std::memory_order linkLoadOrder = std::memory_order_acquire;
    static constexpr std::memory_order linkStoreOrder = std::memory_order_release;
#else
    static constexpr std::memory_order linkLoadOrder = std::memory_order_relaxed;
    static constexpr std::memory_order linkStoreOrder = std::memory_order_relaxed;
#endif
    BlockDirectory* nextDirectory() const { return WTF::atomicLoad(const_cast<BlockDirectory**>(&m_nextDirectory), linkLoadOrder); }
    BlockDirectory* nextDirectoryInSubspace() const { return WTF::atomicLoad(const_cast<BlockDirectory**>(&m_nextDirectoryInSubspace), linkLoadOrder); }
    BlockDirectory* nextDirectoryInAlignedMemoryAllocator() const { return WTF::atomicLoad(const_cast<BlockDirectory**>(&m_nextDirectoryInAlignedMemoryAllocator), linkLoadOrder); }

    void setNextDirectory(BlockDirectory* directory) { WTF::atomicStore(&m_nextDirectory, directory, linkStoreOrder); }
    void setNextDirectoryInSubspace(BlockDirectory* directory) { WTF::atomicStore(&m_nextDirectoryInSubspace, directory, linkStoreOrder); }
    void setNextDirectoryInAlignedMemoryAllocator(BlockDirectory* directory) { WTF::atomicStore(&m_nextDirectoryInAlignedMemoryAllocator, directory, linkStoreOrder); }
    
    MarkedBlock::Handle* findEmptyBlockToSteal();
    
    inline MarkedBlock::Handle* findBlockToSweep();
    MarkedBlock::Handle* findBlockToSweep(unsigned& unsweptCursor);

    // FIXME: rdar://139998916
    MarkedBlock::Handle* NODELETE findMarkedBlockHandleDebug(MarkedBlock*);

    void didFinishUsingBlock(MarkedBlock::Handle*);
    void didFinishUsingBlock(AbstractLocker&, MarkedBlock::Handle*) WTF_REQUIRES_LOCK(m_bitvectorLock);

    Subspace* subspace() const { return m_subspace; }
    MarkedSpace& NODELETE markedSpace() const;

    Heap& heap() const { return m_heap; }

    // SharedGC (SPEC-heap.md §5.3; THREADS T4): slot index of this directory
    // in every client's GCThreadLocalCache flat table — the owning
    // CompleteSubspace's tlcIndexBase() plus the canonical size-class index.
    // invalidTlcIndex for iso directories (those are lookup-only via the
    // TLC's per-directory map). Assigned once, under
    // MarkedSpace::m_directoryLock, at directory creation.
    static constexpr unsigned invalidTlcIndex = std::numeric_limits<unsigned>::max();
    unsigned tlcIndex() const { return m_tlcIndex; }
    void setTlcIndex(unsigned index)
    {
        ASSERT(m_tlcIndex == invalidTlcIndex);
        ASSERT(index != invalidTlcIndex);
        m_tlcIndex = index;
    }

    // SharedGC (§5.3 teardown/I9; THREADS T4): unlink one client's
    // LocalAllocator under m_localAllocatorsLock (rank 8); the caller holds
    // MSPL (rank 7) when the server is shared. No-op if already unlinked.
    void detachLocalAllocator(LocalAllocator&);

    void dump(PrintStream&) const;
    void dumpBits(PrintStream& = WTF::dataFile()) WTF_REQUIRES_SHARED_LOCK(m_bitvectorLock);

private:
    friend class IsoCellSet;
    friend class LocalAllocator;
    friend class LocalSideAllocator;
    friend class MarkedBlock;
    
    MarkedBlock::Handle* findBlockForAllocation(LocalAllocator&);

    // SharedGC (§5.2(3)): the AbstractLocker& is the caller's
    // MutatorSlowPathLocker token — when the heap is a shared server, the
    // server's MSPL must be held across tryAllocateBlock/addBlock
    // (debug-asserted inside; a no-op locker is fine when !isSharedServer()).
    MarkedBlock::Handle* tryAllocateBlock(const AbstractLocker& mutatorSlowPathLocker, Heap&);
    
    Vector<MarkedBlock::Handle*> m_blocks;
    Vector<unsigned> m_freeBlockIndices;

    // Mutator uses this to guard resizing the bitvectors. Those things in the GC that may run
    // concurrently to the mutator must lock this when accessing the bitvectors.
    BlockDirectoryBits m_bits WTF_GUARDED_BY_LOCK(m_bitvectorLock); // Don't access this directly use one of the accessors above.
    Lock m_bitvectorLock;
    Lock m_localAllocatorsLock;
    CellAttributes m_attributes;

    Heap& m_heap; // SharedGC (T4): available from construction (before setSubspace()).

    unsigned m_cellSize;
    unsigned m_tlcIndex { invalidTlcIndex }; // SharedGC (§5.3): see tlcIndex().

    // After you do something to a block based on one of these cursors, you clear the bit in the
    // corresponding bitvector and leave the cursor where it was. We can use unsigned instead of size_t since
    // this number is bound by capacity of Vector m_blocks, which must be within unsigned.
    unsigned m_emptyCursor { 0 };
    unsigned m_unsweptCursor { 0 }; // Points to the next block that is a candidate for incremental sweeping.
    
    // FIXME: All of these should probably be references.
    // https://bugs.webkit.org/show_bug.cgi?id=166988
    Subspace* m_subspace { nullptr };
    // THREADS/TSAN: null-initialized in the constructor body via relaxed atomic
    // stores (a member-init plain write at a recycled malloc address pairs with
    // stale lock-free walkers' atomic loads).
    BlockDirectory* m_nextDirectory;
    BlockDirectory* m_nextDirectoryInSubspace;
    BlockDirectory* m_nextDirectoryInAlignedMemoryAllocator;
    
    SentinelLinkedList<LocalAllocator, BasicRawSentinelNode<LocalAllocator>> m_localAllocators;
};

} // namespace JSC
