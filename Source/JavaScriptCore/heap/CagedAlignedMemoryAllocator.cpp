/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "CagedAlignedMemoryAllocator.h"

#include "FastMallocAlignedMemoryAllocator.h"
#include "Options.h"
#include "VM.h"
#include <wtf/AutomaticThread.h>
#include <wtf/Box.h>
#include <wtf/FastMalloc.h>
#include <wtf/Lock.h>
#include <wtf/PageBlock.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/Vector.h>

namespace JSC {

// What WarmUpBlockProvider of FastMallocAlignedMemoryAllocator.cpp is to blocks from malloc: a heap that grows takes a fault at the first store into each page of a fresh block.
// A helper thread keeps a few blocks of this cage resident. It goes away when nobody has asked for a while, and gives them back.
class CagedAlignedMemoryAllocator::ResidentBlocks {
    WTF_MAKE_NONCOPYABLE(ResidentBlocks);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(ResidentBlocks);
public:
    explicit ResidentBlocks(MarkedBlockCage& cage)
        : m_cage(cage)
        , m_lock(Box<Lock>::create())
        , m_condition(AutomaticThreadCondition::create())
        , m_thread(adoptRef(*new Helper(Locker { *m_lock }, *this)))
    {
    }

    ~ResidentBlocks()
    {
        {
            Locker locker { *m_lock };
            m_phase = Phase::Ending;
            if (!m_thread->tryStop(locker))
                m_condition->notifyAll(locker);
        }
        m_thread->join();
        Locker locker { *m_lock };
        release();
    }

    void* tryTake()
    {
        Locker locker { *m_lock };
        void* result = m_blocks.isEmpty() ? nullptr : m_blocks.takeLast();
        m_wasAskedSinceRefill = true;
        m_wasAskedSinceIdleCheck = true;
        if ((!result || isRunningLow()) && m_phase != Phase::StandingDown)
            m_condition->notifyOne(locker);
        return result;
    }

private:
    enum class Phase : uint8_t { Stopped, Armed, StandingDown, Ending };

    class Helper final : public AutomaticThread {
        WTF_MAKE_TZONE_ALLOCATED_INLINE(Helper);
        WTF_OVERRIDE_DELETE_FOR_CHECKED_PTR(Helper);
    public:
        Helper(const AbstractLocker& locker, ResidentBlocks& blocks)
            : AutomaticThread(locker, blocks.m_lock, blocks.m_condition.copyRef(), Seconds(Options::warmUpMarkedBlockIdleTimeout()))
            , m_blocks(blocks)
        {
        }

        ASCIILiteral name() const final { return "JSCWarmUpCage"_s; }
#if ENABLE(AOT)
        // Whoever finds no block here makes one itself.
        bool isOptional() const final { return true; }
#endif

    private:
        void threadDidStart() final
        {
            Locker locker { *m_blocks.m_lock };
            if (m_blocks.m_phase != Phase::Ending)
                m_blocks.m_phase = Phase::Armed;
        }

        PollResult poll(const AbstractLocker&) final
        {
            assertIsHeld(*m_blocks.m_lock);
            if (m_blocks.m_phase == Phase::Ending)
                return PollResult::Stop;
            if (m_blocks.m_phase != Phase::Armed)
                return PollResult::Wait;
            return m_blocks.m_wasAskedSinceRefill || m_blocks.isRunningLow() ? PollResult::Work : PollResult::Wait;
        }

        WorkResult work() final
        {
            m_blocks.refill();
            return WorkResult::Continue;
        }

        bool shouldSleep(const AbstractLocker&) final
        {
            assertIsHeld(*m_blocks.m_lock);
            if (!std::exchange(m_blocks.m_wasAskedSinceIdleCheck, false))
                return true;
            m_blocks.m_phase = Phase::Armed;
            return false;
        }

        void threadIsStopping(const AbstractLocker&) final
        {
            assertIsHeld(*m_blocks.m_lock);
            if (m_blocks.m_phase != Phase::Ending)
                m_blocks.m_phase = Phase::Stopped;
            m_blocks.release();
        }

        ResidentBlocks& m_blocks;
    };

    size_t targetDepth() WTF_REQUIRES_LOCK(*m_lock) { return m_phase == Phase::Armed ? Options::warmUpMarkedBlockCount() : 0; }
    bool isRunningLow() WTF_REQUIRES_LOCK(*m_lock) { return m_blocks.size() * 4 < targetDepth(); }

    // The cage has a lock of its own and does not come back here.
    void release() WTF_REQUIRES_LOCK(*m_lock)
    {
        for (void* block : std::exchange(m_blocks, { }))
            m_cage.free(block);
    }

    void refill()
    {
        size_t wanted;
        {
            Locker locker { *m_lock };
            m_wasAskedSinceRefill = false;
            size_t target = targetDepth();
            wanted = target > m_blocks.size() ? target - m_blocks.size() : 0;
        }

        Vector<void*, 32> staging;
        bool isExhausted = false;
        size_t pageSize = WTF::pageSize();
        for (size_t i = 0; i < wanted; ++i) {
            void* block = m_cage.tryAllocateAligned(MarkedBlock::blockSize, MarkedBlock::blockSize);
            if (!block) {
                isExhausted = true;
                break;
            }
            auto bytes = unsafeMakeSpan(static_cast<volatile char*>(block), MarkedBlock::blockSize);
            for (size_t offset = 0; offset < bytes.size(); offset += pageSize)
                bytes[offset] = 0;
            staging.append(block);
        }

        Locker locker { *m_lock };
        m_blocks.appendVector(WTF::move(staging));
        if (isExhausted && m_phase == Phase::Armed)
            m_phase = Phase::StandingDown;
    }

    MarkedBlockCage& m_cage;
    const Box<Lock> m_lock;
    const Ref<AutomaticThreadCondition> m_condition;
    const Ref<Helper> m_thread;
    Vector<void*, 32> m_blocks WTF_GUARDED_BY_LOCK(*m_lock);
    Phase m_phase WTF_GUARDED_BY_LOCK(*m_lock) { Phase::Stopped };
    bool m_wasAskedSinceRefill WTF_GUARDED_BY_LOCK(*m_lock) { false };
    bool m_wasAskedSinceIdleCheck WTF_GUARDED_BY_LOCK(*m_lock) { false };
};

CagedAlignedMemoryAllocator::CagedAlignedMemoryAllocator() = default;

CagedAlignedMemoryAllocator::~CagedAlignedMemoryAllocator() = default;

void* CagedAlignedMemoryAllocator::tryAllocateBlockToFill()
{
    if (Options::useWarmUpMarkedBlocks() && Options::warmUpMarkedBlockCount() && !VM::isInMiniMode() && ++m_numberOfBlocksToFillRequested >= Options::warmUpMarkedBlockStartAfterBlocks()) {
        if (!m_residentBlocks) {
            m_residentBlocks = makeUnique<ResidentBlocks>(m_cage);
            didPassWarmUpStartThresholdInAnotherAllocator();
        }
        if (void* block = m_residentBlocks->tryTake())
            return block;
    }
    return tryAllocateAlignedMemory(MarkedBlock::blockSize, MarkedBlock::blockSize);
}

void* CagedAlignedMemoryAllocator::tryAllocateAlignedMemory(size_t alignment, size_t size)
{
    if (void* result = m_cage.tryAllocateAligned(alignment, size))
        return result;
    if (MarkedBlockCage::isRequired())
        return m_cage.tryAllocateAlignedFromSharedReserve(alignment, size);
    MarkedBlockCage::didAllocateOutside();
    return tryFastCompactAlignedMalloc(alignment, size);
}

void CagedAlignedMemoryAllocator::freeAlignedMemory(void* basePtr)
{
    if (m_cage.contains(basePtr))
        m_cage.free(basePtr);
    else
        fastFree(basePtr);
}

void CagedAlignedMemoryAllocator::dump(PrintStream& out) const
{
    out.print("Caged(", RawPointer(this), ")");
}

void* CagedAlignedMemoryAllocator::tryAllocateMemory(size_t size)
{
    if (void* result = m_cage.tryAllocate(size))
        return result;
    if (MarkedBlockCage::isRequired())
        return nullptr;
    MarkedBlockCage::didAllocateOutside();
    return FastCompactMalloc::tryMalloc(size);
}

void CagedAlignedMemoryAllocator::freeMemory(void* pointer)
{
    if (m_cage.contains(pointer))
        m_cage.free(pointer);
    else
        FastCompactMalloc::free(pointer);
}

void* CagedAlignedMemoryAllocator::tryReallocateMemory(void* pointer, size_t size)
{
    if (m_cage.contains(pointer))
        return m_cage.tryReallocate(pointer, size);
    return FastCompactMalloc::tryRealloc(pointer, size);
}

} // namespace JSC
