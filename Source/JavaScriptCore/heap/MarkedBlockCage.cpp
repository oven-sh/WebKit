/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "MarkedBlockCage.h"

#include "MarkedBlock.h"
#include "Options.h"
#include <wtf/BitVector.h>
#include <wtf/CryptographicallyRandomNumber.h>
#include <wtf/DataLog.h>
#include <wtf/Lock.h>
#include <wtf/MathExtras.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/NotFound.h>
#include <wtf/OSAllocator.h>

#if OS(LINUX)
#include <errno.h>
#include <sys/mman.h>
#elif OS(DARWIN)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <sys/mman.h>
#endif

namespace JSC {

class MarkedBlockCageManager {
public:
    MarkedBlockCageManager()
    {
        size_t size = static_cast<size_t>(Options::markedBlockCageSizeInMB()) * MB;
        RELEASE_ASSERT(hasOneBitSet(size) && size <= MarkedBlockCage::maxSize, size);

        uintptr_t start = 0;
        for (unsigned i = 0; i < 8; ++i) {
            start = tryReserve(size);
            if (start)
                break;
            size /= 2;
        }
        RELEASE_ASSERT(start, size);
        RELEASE_ASSERT(!(start & (MarkedBlockCage::maxSize - 1)), start, size);

        g_jscConfig.startOfMarkedBlockCage = start;
        g_jscConfig.sizeOfMarkedBlockCage = size;
        m_blockCount = size / MarkedBlock::blockSize;
        if (m_blockCount > 1) {
            uint64_t random = Options::seedOfVMRandomForFuzzer() ? Options::seedOfVMRandomForFuzzer() : cryptographicallyRandomNumber<uint64_t>();
            m_placeOfFirstBlock = 1 + random % (m_blockCount - 1);
            m_firstWrappedBlock = m_blockCount - m_placeOfFirstBlock + 1;
        }
    }

    void* tryAllocateBlock()
    {
        Locker locker { m_lock };
        size_t index = findBlock(m_committedFreeBlocks, m_firstCommittedFreeBlock);
        if (index == notFound)
            index = recommitFreeBlocks();
        if (index != notFound) {
            m_committedFreeBlocks.quickClear(index);
            m_firstCommittedFreeBlock = index + 1;
            return blockAt(index);
        }
        if (m_firstUnusedBlock == m_mappedBlockCount && !mapMoreBlocks())
            return nullptr;
        return blockAt(m_firstUnusedBlock++);
    }

    void freeBlock(void* block)
    {
        uintptr_t offset = std::bit_cast<uintptr_t>(block) - g_jscConfig.startOfMarkedBlockCage;
        size_t place = offset / MarkedBlock::blockSize;
        size_t index = place >= m_placeOfFirstBlock ? place - m_placeOfFirstBlock + 1 : place + m_firstWrappedBlock - 1;

        Locker locker { m_lock };
        RELEASE_ASSERT(place && index < m_firstUnusedBlock && !(offset % MarkedBlock::blockSize), offset, m_firstUnusedBlock);
        RELEASE_ASSERT(!m_committedFreeBlocks.get(index) && !m_decommittedFreeBlocks.get(index), offset);
        addCommittedFreeBlocks(index, index + 1);
    }

    void decommitFreeBlocks()
    {
        BitVector blocks;
        size_t first;
        {
            Locker locker { m_lock };
            first = findBlock(m_committedFreeBlocks, m_firstCommittedFreeBlock);
            if (first == notFound)
                return;
            blocks = std::exchange(m_committedFreeBlocks, { });
        }

        for (size_t begin = first; begin != notFound;) {
            size_t end = blocks.findBit(begin, false);
            forEachRun(begin, end, [](void* address, size_t bytes) {
                OSAllocator::decommit(address, bytes);
            });
            begin = findBlock(blocks, end);
        }

        Locker locker { m_lock };
        m_decommittedFreeBlocks.merge(blocks);
        m_firstDecommittedFreeBlock = std::min(m_firstDecommittedFreeBlock, first);
    }

private:
    static constexpr size_t blocksPerCommit = MB / MarkedBlock::blockSize;
    static constexpr size_t minBlocksPerMapping = 4 * MB / MarkedBlock::blockSize;
    static constexpr size_t maxBlocksPerMapping = 32 * MB / MarkedBlock::blockSize;

    static uintptr_t tryReserve(size_t size)
    {
#if OS(DARWIN)
        // Without access it is one map entry. Otherwise the kernel makes one per 128 MB, and every call on the range walks them all.
        mach_vm_address_t address = 0;
        if (mach_vm_map(mach_task_self(), &address, size, MarkedBlockCage::maxSize - 1, VM_FLAGS_ANYWHERE | OSAllocator::FastMallocPages, MEMORY_OBJECT_NULL, 0, false, VM_PROT_NONE, VM_PROT_NONE, VM_INHERIT_NONE) != KERN_SUCCESS)
            return 0;
        return address;
#else
        void* address = nullptr;
#if OS(LINUX)
        for (unsigned attempt = 0; attempt < 16 && !address; ++attempt) {
            void* wanted = std::bit_cast<void*>((128 + static_cast<uintptr_t>(cryptographicallyRandomNumber<uint32_t>() % 1024)) * MarkedBlockCage::maxSize);
            void* reserved = OSAllocator::tryReserveUncommitted(size, OSAllocator::FastMallocPages, wanted);
            if (!reserved)
                return 0;
            if (reserved == wanted)
                address = reserved;
            else
                OSAllocator::releaseDecommitted(reserved, size, 0);
        }
#endif
        if (!address)
            address = OSAllocator::tryReserveUncommittedAligned(size, MarkedBlockCage::maxSize, OSAllocator::FastMallocPages);
#if OS(UNIX)
        // What `undefined` and `null` are decoded to is in the first block: see AOT::prepareFaultingAccesses().
        if (address)
            RELEASE_ASSERT(!mprotect(address, MarkedBlock::blockSize, PROT_NONE));
#endif
#if OS(LINUX) && defined(MADV_DOFORK)
        // A child process inherits the blocks, as it does what fastMalloc hands out.
        if (address) {
            while (madvise(address, size, MADV_DOFORK) == -1 && errno == EAGAIN) { }
        }
#endif
        return reinterpret_cast<uintptr_t>(address);
#endif
    }

    static bool tryMap(void* address, size_t size)
    {
#if OS(DARWIN)
        return mmap(address, size, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, OSAllocator::FastMallocPages, 0) != MAP_FAILED;
#else
        OSAllocator::commit(address, size, true, false);
        return true;
#endif
    }

    void* blockAt(size_t index) const
    {
        size_t place = index < m_firstWrappedBlock ? m_placeOfFirstBlock + index - 1 : index - m_firstWrappedBlock + 1;
        return std::bit_cast<void*>(g_jscConfig.startOfMarkedBlockCage + place * MarkedBlock::blockSize);
    }

    template<typename Functor>
    void forEachRun(size_t begin, size_t end, const Functor& functor) const
    {
        size_t middle = std::clamp(m_firstWrappedBlock, begin, end);
        if (begin < middle)
            functor(blockAt(begin), (middle - begin) * MarkedBlock::blockSize);
        if (middle < end)
            functor(blockAt(middle), (end - middle) * MarkedBlock::blockSize);
    }

    static size_t findBlock(const BitVector& blocks, size_t start)
    {
        if (start >= blocks.size())
            return notFound;
        size_t index = blocks.findBit(start, true);
        return index < blocks.size() ? index : notFound;
    }

    void addCommittedFreeBlocks(size_t begin, size_t end)
    {
        if (m_committedFreeBlocks.size() < end)
            m_committedFreeBlocks.ensureSize(std::min(2 * m_firstUnusedBlock, m_blockCount));
        for (size_t index = begin; index < end; ++index)
            m_committedFreeBlocks.quickSet(index);
        m_firstCommittedFreeBlock = std::min(m_firstCommittedFreeBlock, begin);
    }

    // The lowest run of decommitted blocks. Yields the first of them.
    size_t recommitFreeBlocks()
    {
        size_t begin = findBlock(m_decommittedFreeBlocks, m_firstDecommittedFreeBlock);
        if (begin == notFound)
            return notFound;
        size_t end = std::min(m_decommittedFreeBlocks.findBit(begin, false), begin + blocksPerCommit);
        for (size_t index = begin; index < end; ++index)
            m_decommittedFreeBlocks.quickClear(index);
        m_firstDecommittedFreeBlock = end;
        forEachRun(begin, end, [](void* address, size_t bytes) {
            OSAllocator::commit(address, bytes, true, false);
        });
        addCommittedFreeBlocks(begin, end);
        return begin;
    }

    bool mapMoreBlocks()
    {
        size_t count = std::min(std::clamp(m_mappedBlockCount / 4, minBlocksPerMapping, maxBlocksPerMapping), m_blockCount - m_mappedBlockCount);
        if (m_mappedBlockCount < m_firstWrappedBlock)
            count = std::min(count, m_firstWrappedBlock - m_mappedBlockCount);
        if (!count || !tryMap(blockAt(m_mappedBlockCount), count * MarkedBlock::blockSize))
            return false;
        m_mappedBlockCount += count;
        return true;
    }

    Lock m_lock;
    size_t m_blockCount { 0 };
    // The first block of the cage is not used: no cell has offset zero.
    size_t m_firstUnusedBlock { 1 };
    size_t m_mappedBlockCount { 1 };
    size_t m_placeOfFirstBlock { 1 };
    size_t m_firstWrappedBlock { std::numeric_limits<size_t>::max() };
    BitVector m_committedFreeBlocks;
    BitVector m_decommittedFreeBlocks;
    // No block below these is in the respective set.
    size_t m_firstCommittedFreeBlock { 0 };
    size_t m_firstDecommittedFreeBlock { 0 };
};

static_assert(1 << MarkedBlockCage::shift == MarkedBlock::atomSize);

static LazyNeverDestroyed<MarkedBlockCageManager> s_markedBlockCageManager;
std::atomic<bool> MarkedBlockCage::s_isRequired { false };
std::atomic<bool> MarkedBlockCage::s_hasBlocksOutside { false };

void MarkedBlockCage::initialize()
{
    if (Options::useAOT() && Options::markedBlockCageSizeInMB())
        s_markedBlockCageManager.construct();
}

void* MarkedBlockCage::tryAllocateBlock()
{
    return s_markedBlockCageManager->tryAllocateBlock();
}

void MarkedBlockCage::freeBlock(void* block)
{
    s_markedBlockCageManager->freeBlock(block);
}

void MarkedBlockCage::decommitFreeBlocks()
{
    if (isEnabled())
        s_markedBlockCageManager->decommitFreeBlocks();
}

void MarkedBlockCage::require()
{
    if (!isEnabled()) {
        dataLogLn("This program was compiled with 32-bit references and cannot run with markedBlockCageSizeInMB=0. Compile it with useAOTWideReferences=1 to run it without the cage.");
        exit(EXIT_FAILURE);
    }
    s_isRequired.store(true, std::memory_order_relaxed);
    RELEASE_ASSERT(!s_hasBlocksOutside.load(std::memory_order_relaxed));
}

void MarkedBlockCage::logExhaustion()
{
    if (isRequired())
        dataLogLn("FATAL: MarkedBlocks fill the ", g_jscConfig.sizeOfMarkedBlockCage / MB, " MB of address space that a program compiled with 32-bit references has for them (markedBlockCageSizeInMB). Compiled with useAOTWideReferences=1 it has no such limit.");
}

} // namespace JSC
