/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "MarkedBlockCage.h"

#include "AddressSpaceBudget.h"
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

#if OS(LINUX)
static constexpr bool isReservedWithoutAccess = true;
#elif !OS(DARWIN)
static constexpr bool isReservedWithoutAccess = false;
#endif

static std::atomic<size_t> s_sizeOfAllCages { 0 };

class MarkedBlockCageManager {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(MarkedBlockCageManager);
    WTF_MAKE_NONCOPYABLE(MarkedBlockCageManager);
public:
    static MarkedBlockCageManager* tryCreate(size_t size)
    {
        if (s_sizeOfAllCages.fetch_add(size, std::memory_order_relaxed) + size <= addressSpaceToReserve() / partOfAddressSpaceForMarkedBlockCages) {
            if (uintptr_t start = tryReserve(size))
                return new MarkedBlockCageManager(start, size);
        }
        s_sizeOfAllCages.fetch_sub(size, std::memory_order_relaxed);
        return nullptr;
    }

    MarkedBlockCageManager(uintptr_t start, size_t size)
    {
        RELEASE_ASSERT(!(start & (MarkedBlockCage::maxSize - 1)), start, size);

        m_start = start;
        m_size = size;
        m_blockCount = size / MarkedBlock::blockSize;
        if (m_blockCount > reservedBlockCount) {
            uint64_t random = Options::seedOfVMRandomForFuzzer() ? Options::seedOfVMRandomForFuzzer() : cryptographicallyRandomNumber<uint64_t>();
            m_placeOfFirstBlock = reservedBlockCount + random % (m_blockCount - reservedBlockCount);
            m_firstWrappedBlock = m_blockCount - m_placeOfFirstBlock + 1;
            m_blockCount -= reservedBlockCount - 1;
        } else
            m_blockCount = 1;
    }

    ~MarkedBlockCageManager()
    {
#if OS(DARWIN)
        RELEASE_ASSERT(mach_vm_deallocate(mach_task_self(), m_start, m_size) == KERN_SUCCESS);
#else
        OSAllocator::releaseDecommitted(std::bit_cast<void*>(m_start), m_size, 0);
#endif
        s_sizeOfAllCages.fetch_sub(m_size, std::memory_order_relaxed);
    }

    uintptr_t start() const { return m_start; }
    size_t size() const { return m_size; }
    bool contains(const void* pointer) const { return std::bit_cast<uintptr_t>(pointer) - m_start < m_size; }

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
        uintptr_t offset = std::bit_cast<uintptr_t>(block) - m_start;
        size_t place = offset / MarkedBlock::blockSize;
        size_t index = indexOf(place);

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
    static constexpr size_t reservedBlockCount = (static_cast<size_t>(MarkedBlockCage::minIndex) << MarkedBlockCage::shift) / MarkedBlock::blockSize;
    static constexpr size_t blocksPerCommit = MB / MarkedBlock::blockSize;
    static constexpr size_t minBlocksPerMapping = 4 * MB / MarkedBlock::blockSize;
    static constexpr size_t maxBlocksPerMapping = 32 * MB / MarkedBlock::blockSize;
#if OS(LINUX)
    static constexpr uintptr_t firstWindow = 2;
    static constexpr uint32_t numberOfWindows = 3966;
#endif

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
            void* wanted = std::bit_cast<void*>((firstWindow + static_cast<uintptr_t>(cryptographicallyRandomNumber<uint32_t>() % numberOfWindows)) * MarkedBlockCage::maxSize);
            void* reserved = OSAllocator::tryReserveUncommitted(size, OSAllocator::FastMallocPages, wanted, false);
            if (!reserved)
                return 0;
            if (reserved == wanted)
                address = reserved;
            else
                OSAllocator::releaseDecommitted(reserved, size, 0);
        }
#endif
        if (!address)
            address = OSAllocator::tryReserveUncommittedAligned(size, MarkedBlockCage::maxSize, OSAllocator::FastMallocPages, nullptr, !isReservedWithoutAccess);
#if OS(UNIX)
        // What `undefined` and `null` are decoded to is in the first block: see AOT::prepareFaultingAccesses().
        if (address)
            RELEASE_ASSERT(!mprotect(address, isReservedWithoutAccess ? size : MarkedBlock::blockSize, PROT_NONE));
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
#if OS(LINUX)
        if (mprotect(address, size, PROT_READ | PROT_WRITE))
            return false;
#endif
        OSAllocator::commit(address, size, true, false);
        return true;
#endif
    }

    void* blockAt(size_t index) const
    {
        size_t place = index < m_firstWrappedBlock ? m_placeOfFirstBlock + index - 1 : index - m_firstWrappedBlock + reservedBlockCount;
        return std::bit_cast<void*>(m_start + place * MarkedBlock::blockSize);
    }

    size_t indexOf(size_t place) const
    {
        return place >= m_placeOfFirstBlock ? place - m_placeOfFirstBlock + 1 : place + m_firstWrappedBlock - reservedBlockCount;
    }

    size_t endOfRunFrom(size_t begin, size_t end) const
    {
        if (begin < m_firstWrappedBlock)
            end = std::min(end, m_firstWrappedBlock);
        return end;
    }

    template<typename Functor>
    void forEachRun(size_t begin, size_t end, const Functor& functor) const
    {
        while (begin < end) {
            size_t endOfRun = endOfRunFrom(begin, end);
            functor(blockAt(begin), (endOfRun - begin) * MarkedBlock::blockSize);
            begin = endOfRun;
        }
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
        count = endOfRunFrom(m_mappedBlockCount, m_mappedBlockCount + count) - m_mappedBlockCount;
        if (!count || !tryMap(blockAt(m_mappedBlockCount), count * MarkedBlock::blockSize))
            return false;
        m_mappedBlockCount += count;
        return true;
    }

    Lock m_lock;
    uintptr_t m_start { 0 };
    size_t m_size { 0 };
    size_t m_blockCount { 0 };
    // The first block of the cage is not used: no cell has offset zero.
    size_t m_firstUnusedBlock { 1 };
    size_t m_mappedBlockCount { 1 };
    size_t m_placeOfFirstBlock { reservedBlockCount };
    size_t m_firstWrappedBlock { std::numeric_limits<size_t>::max() };
    BitVector m_committedFreeBlocks;
    BitVector m_decommittedFreeBlocks;
    // No block below these is in the respective set.
    size_t m_firstCommittedFreeBlock { 0 };
    size_t m_firstDecommittedFreeBlock { 0 };
};

static_assert(1 << MarkedBlockCage::shift <= MarkedBlock::atomSize);

constinit thread_local MarkedBlockCage* MarkedBlockCage::s_current = nullptr;
constinit thread_local uintptr_t MarkedBlockCage::s_baseOfCurrent = 0;
#if OS(DARWIN)
std::atomic<uintptr_t> MarkedBlockCage::s_baseOfOnlyCage { 0 };

NEVER_INLINE uintptr_t MarkedBlockCage::baseOfCurrentOutOfLine()
{
    return s_baseOfCurrent;
}
#endif
std::atomic<bool> MarkedBlockCage::s_isRequired { false };
std::atomic<bool> MarkedBlockCage::s_hasBlocksOutside { false };
std::atomic<MarkedBlockCage::ExhaustionHandler> MarkedBlockCage::s_exhaustionHandler { nullptr };
static MarkedBlockCageManager* s_firstManager;
static std::atomic<bool> s_firstManagerIsTaken { false };

void MarkedBlockCage::initialize()
{
    if (!Options::useAOT() || !Options::markedBlockCageSizeInMB())
        return;
    size_t size = static_cast<size_t>(Options::markedBlockCageSizeInMB()) * MB;
    if (!hasOneBitSet(size) || size > maxSize) {
        dataLogLn("markedBlockCageSizeInMB is not 0 or a power of two of at most ", maxSize / MB, ": ", Options::markedBlockCageSizeInMB());
        exit(EXIT_FAILURE);
    }
    for (unsigned i = 0; i < 8 && !s_firstManager; ++i)
        s_firstManager = MarkedBlockCageManager::tryCreate(size >> i);
    RELEASE_ASSERT(s_firstManager, size);
    g_jscConfig.sizeOfMarkedBlockCage = s_firstManager->size();
#if OS(DARWIN)
    s_baseOfOnlyCage.store(s_firstManager->start(), std::memory_order_relaxed);
#endif
}

MarkedBlockCage::~MarkedBlockCage()
{
    if (s_current == this)
        setCurrent(nullptr, 0);
    MarkedBlockCageManager* manager = m_manager.load(std::memory_order_relaxed);
    if (manager != s_firstManager) {
        delete manager;
        return;
    }
    if (m_hasTakenFirstManager) {
        manager->decommitFreeBlocks();
        s_firstManagerIsTaken.store(false, std::memory_order_release);
    }
}

void MarkedBlockCage::makeCurrent()
{
    MarkedBlockCageManager* manager = m_manager.load(std::memory_order_acquire);
    setCurrent(this, manager ? manager->start() : 0);
}

void* MarkedBlockCage::tryAllocateBlock()
{
    MarkedBlockCageManager* manager = m_manager.load(std::memory_order_acquire);
    if (!manager) [[unlikely]] {
        RELEASE_ASSERT(!s_current || s_current == this);
        if (!s_firstManagerIsTaken.exchange(true, std::memory_order_acq_rel)) {
            manager = s_firstManager;
            m_hasTakenFirstManager = true;
        } else if ((manager = MarkedBlockCageManager::tryCreate(g_jscConfig.sizeOfMarkedBlockCage))) {
#if OS(DARWIN)
            s_baseOfOnlyCage.store(0, std::memory_order_relaxed);
#endif
        } else
            manager = s_firstManager;
        m_manager.store(manager, std::memory_order_release);
        makeCurrent();
    }
    void* block = manager->tryAllocateBlock();
#if ASAN_ENABLED
    if (block)
        __asan_unpoison_memory_region(block, MarkedBlock::blockSize);
#endif
    return block;
}

void MarkedBlockCage::freeBlock(void* block)
{
#if ASAN_ENABLED
    __asan_poison_memory_region(block, MarkedBlock::blockSize);
#endif
    m_manager.load(std::memory_order_relaxed)->freeBlock(block);
}

bool MarkedBlockCage::owns(const void* pointer) const
{
    MarkedBlockCageManager* manager = m_manager.load(std::memory_order_relaxed);
    return manager && manager->contains(pointer);
}

void MarkedBlockCage::decommitFreeBlocks()
{
    if (MarkedBlockCageManager* manager = s_current ? s_current->m_manager.load(std::memory_order_acquire) : nullptr)
        manager->decommitFreeBlocks();
}

void MarkedBlockCage::require()
{
    if (!isEnabled()) {
        dataLogLn("This program was compiled with 32-bit references and cannot run with markedBlockCageSizeInMB=0. Compile it with useAOTWideReferences=1 to run it without the cage. In bun build the flag is --aot-huge-heap.");
        exit(EXIT_FAILURE);
    }
    s_isRequired.store(true, std::memory_order_relaxed);
    if (s_hasBlocksOutside.load(std::memory_order_relaxed)) {
        dataLogLn("This program was compiled with 32-bit references, and the objects made before it was loaded did not fit markedBlockCageSizeInMB=", Options::markedBlockCageSizeInMB(), ". Give it a larger range, or compile the program with useAOTWideReferences=1. In bun build the flag is --aot-huge-heap.");
        exit(EXIT_FAILURE);
    }
}

void MarkedBlockCage::logExhaustion()
{
    if (!isRequired())
        return;
    dataLogLn("FATAL: MarkedBlocks fill the ", g_jscConfig.sizeOfMarkedBlockCage / MB, " MB of address space that each thread of a program compiled with 32-bit references has for them (markedBlockCageSizeInMB). Compiled with useAOTWideReferences=1 it has no such limit. In bun build the flag is --aot-huge-heap.");
    if (ExhaustionHandler handler = s_exhaustionHandler.load(std::memory_order_relaxed))
        handler();
}

void MarkedBlockCage::setExhaustionHandler(ExhaustionHandler handler)
{
    s_exhaustionHandler.store(handler, std::memory_order_relaxed);
}

} // namespace JSC
