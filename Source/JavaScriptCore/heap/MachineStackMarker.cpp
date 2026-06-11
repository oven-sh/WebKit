/*
 *  Copyright (C) 2003-2023 Apple Inc. All rights reserved.
 *  Copyright (C) 2007 Eric Seidel <eric@webkit.org>
 *  Copyright (C) 2009 Acision BV. All rights reserved.
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include "config.h"
#include "MachineStackMarker.h"

#include "ConservativeRoots.h"
#include "MachineContext.h"
#include <wtf/BitVector.h>
#include <wtf/PageBlock.h>
#include <wtf/StdLibExtras.h>
#include <wtf/TZoneMallocInlines.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

WTF_MAKE_TZONE_ALLOCATED_IMPL(MachineThreads);

MachineThreads::MachineThreads()
    : m_threadGroup(ThreadGroup::create())
{
}

SUPPRESS_ASAN
void MachineThreads::gatherFromCurrentThread(ConservativeRoots& conservativeRoots, JITStubRoutineSet& jitStubRoutines, CodeBlockSet& codeBlocks, CurrentThreadState& currentThreadState)
{
    if (currentThreadState.registerState) {
        void* registersBegin = currentThreadState.registerState;
        void* registersEnd = reinterpret_cast<void*>(roundUpToMultipleOf<sizeof(void*)>(reinterpret_cast<uintptr_t>(currentThreadState.registerState + 1)));
        conservativeRoots.add(registersBegin, registersEnd, jitStubRoutines, codeBlocks);
    }

    conservativeRoots.add(currentThreadState.stackTop, currentThreadState.stackOrigin, jitStubRoutines, codeBlocks);
}

static inline int NODELETE osRedZoneAdjustment()
{
    int redZoneAdjustment = 0;
#if CPU(X86_64)
    // See http://people.freebsd.org/~obrien/amd64-elf-abi.pdf Section 3.2.2.
    redZoneAdjustment = -128;
#elif CPU(ARM64)
#if OS(DARWIN)
    // See https://developer.apple.com/library/ios/documentation/Xcode/Conceptual/iPhoneOSABIReference/Articles/ARM64FunctionCallingConventions.html#//apple_ref/doc/uid/TP40013702-SW7
    redZoneAdjustment = -128;
#elif OS(WINDOWS)
    // https://devblogs.microsoft.com/oldnewthing/20220726-00/?p=106898
    redZoneAdjustment = -16;
#else
    // There is no red zone.
    // https://stackoverflow.com/questions/77908878/aarch64-is-there-a-red-zone-on-linux-if-so-16-or-128-bytes
#endif
#endif
    return redZoneAdjustment;
}

static std::pair<void*, size_t> NODELETE captureStack(Thread& thread, void* stackTop)
{
    char* begin = reinterpret_cast_ptr<char*>(thread.stack().origin());
    char* end = std::bit_cast<char*>(WTF::roundUpToMultipleOf<sizeof(void*)>(reinterpret_cast<uintptr_t>(stackTop)));
    ASSERT(begin >= end);

    char* endWithRedZone = end + osRedZoneAdjustment();
    ASSERT(WTF::roundUpToMultipleOf<sizeof(void*)>(reinterpret_cast<uintptr_t>(endWithRedZone)) == reinterpret_cast<uintptr_t>(endWithRedZone));

    if (endWithRedZone < thread.stack().end())
        endWithRedZone = reinterpret_cast_ptr<char*>(thread.stack().end());

    std::swap(begin, endWithRedZone);
    return std::make_pair(begin, endWithRedZone - begin);
}

// V7-3 (TSAN): `src` is the raw stack of a thread that Thread::suspend()
// has stopped (signal-suspended on POSIX, thread_suspend on Darwin) — see
// tryCopyOtherThreadStacks: a thread is copied only if its suspend()
// succeeded. On POSIX, success means the target's signal handler saved its
// registers and sem_post()ed globalSemaphoreForSuspendResume before the
// scanner's sem_wait() returned; the target may still be executing a few
// handler instructions (it posts before entering sigsuspend), but any
// concurrent writes are confined to its signal-handler frame, which the
// kernel places below the interrupted SP minus the red zone — disjoint
// from the [stackTop - redZoneAdjustment, origin] span captureStack hands
// us. The COPIED SPAN is therefore quiescent, and the target's prior plain
// stores into it are ordered by signal delivery + the semaphore pair. TSAN
// cannot model signal-suspension as a happens-before edge over all of the
// parked thread's prior stack stores, and `dst` can be fastMalloc memory
// recycled (without TSAN-visible synchronization inside bmalloc) from
// allocations the scanned thread freed. Conservative scanning is typeless
// byte inspection: a stale/torn word can only cause spurious retention,
// never a dangling root. Keep this suppression on copyMemory ONLY — the
// suspend/resume handshake and the getRegisters/m_platformRegisters path
// stay TSAN-instrumented (copyMemory is essentially the only reader of
// remote stack bytes, so detection of a genuine "suspend lied" bug here is
// reduced; restoring it via explicit __tsan_acquire/__tsan_release edges
// around the ThreadingPOSIX suspend handshake is a tracked follow-up).
SUPPRESS_ASAN SUPPRESS_TSAN
static void NODELETE copyMemory(void* dst, const void* src, size_t size)
{
    size_t dstAsSize = reinterpret_cast<size_t>(dst);
    size_t srcAsSize = reinterpret_cast<size_t>(src);
    RELEASE_ASSERT(dstAsSize == WTF::roundUpToMultipleOf<sizeof(CPURegister)>(dstAsSize));
    RELEASE_ASSERT(srcAsSize == WTF::roundUpToMultipleOf<sizeof(CPURegister)>(srcAsSize));
    RELEASE_ASSERT(size == WTF::roundUpToMultipleOf<sizeof(CPURegister)>(size));

    CPURegister* dstPtr = reinterpret_cast<CPURegister*>(dst);
    const CPURegister* srcPtr = reinterpret_cast<const CPURegister*>(src);
    size /= sizeof(CPURegister);
    while (size--)
        *dstPtr++ = *srcPtr++;
}
    


// This function must not call malloc(), free(), or any other function that might
// acquire a lock. Since 'thread' is suspended, trying to acquire a lock
// will deadlock if 'thread' holds that lock.
// This function, specifically the memory copying, was causing problems with Address Sanitizer in
// apps. Since we cannot disallow the system memcpy we must use our own naive implementation,
// copyMemory, for ASan to work on either instrumented or non-instrumented builds. This is not a
// significant performance loss as tryCopyOtherThreadStack is only called as part of an O(heapsize)
// operation. As the heap is generally much larger than the stack the performance hit is minimal.
// See: https://bugs.webkit.org/show_bug.cgi?id=146297
void MachineThreads::tryCopyOtherThreadStack(const ThreadSuspendLocker& locker, Thread& thread, void* buffer, size_t capacity, size_t* size)
{
    PlatformRegisters registers;
    size_t registersSize = thread.getRegisters(locker, registers);

    // This is a workaround for <rdar://problem/27607384>. libdispatch recycles work
    // queue threads without running pthread exit destructors. This can cause us to scan a
    // thread during work queue initialization, when the stack pointer is null.
    if (!MachineContext::stackPointer(registers)) [[unlikely]] {
        *size = 0;
        return;
    }

    std::pair<void*, size_t> stack = captureStack(thread, MachineContext::stackPointer(registers));

    bool canCopy = *size + registersSize + stack.second <= capacity;

    if (canCopy)
        copyMemory(static_cast<char*>(buffer) + *size, &registers, registersSize);
    *size += registersSize;

    if (canCopy)
        copyMemory(static_cast<char*>(buffer) + *size, stack.first, stack.second);
    *size += stack.second;
}

bool MachineThreads::includesCurrentThread()
{
    auto& currentThread = Thread::currentSingleton();
    Locker locker { m_threadGroup->getLock() };
    for (const Ref<Thread>& thread : m_threadGroup->threads(locker)) {
        if (thread.ptr() == &currentThread)
            return true;
    }
    return false;
}

bool MachineThreads::tryCopyOtherThreadStacks(const AbstractLocker& locker, void* buffer, size_t capacity, size_t* size, Thread& currentThreadForGC)
{
    // Prevent two VMs from suspending each other's threads at the same time,
    // which can cause deadlock: <rdar://problem/20300842>.
    static Lock suspendLock;
    Locker suspendLocker { suspendLock };

    *size = 0;

    auto& currentThread = Thread::currentSingleton();
    const ListHashSet<Ref<Thread>>& threads = m_threadGroup->threads(locker);
    BitVector isSuspended(threads.size());

    {
        ThreadSuspendLocker threadSuspendLocker;
        {
            unsigned index = 0;
            for (const Ref<Thread>& thread : threads) {
                if (thread.ptr() != &currentThread
                    && thread.ptr() != &currentThreadForGC) {
                    auto result = thread->suspend(threadSuspendLocker);
                    if (result)
                        isSuspended.set(index);
                    else {
#if OS(DARWIN)
                        // These threads will be removed from the ThreadGroup. Thus, we do not do anything here except for reporting.
                        ASSERT(result.error() != KERN_SUCCESS);
                        WTFReportError(__FILE__, __LINE__, WTF_PRETTY_FUNCTION,
                            "JavaScript garbage collection encountered an invalid thread (err 0x%x): Thread [%d/%d: %p].",
                            result.error(), index, threads.size(), thread.ptr());
#endif
                    }
                }
                ++index;
            }
        }

        {
            unsigned index = 0;
            for (auto& thread : threads) {
                if (isSuspended.get(index))
                    tryCopyOtherThreadStack(threadSuspendLocker, thread.get(), buffer, capacity, size);
                ++index;
            }
        }

        {
            unsigned index = 0;
            for (auto& thread : threads) {
                if (isSuspended.get(index))
                    thread->resume(threadSuspendLocker);
                ++index;
            }
        }
    }

    return *size <= capacity;
}

static void growBuffer(size_t size, void** buffer, size_t* capacity)
{
    if (*buffer)
        fastFree(*buffer);

    *capacity = WTF::roundUpToMultipleOf(WTF::pageSize(), size * 2);
    *buffer = fastMalloc(*capacity);
}

// SharedGC (SPEC-heap.md §10.6, T6): this is the N-mutator stack scan. Every
// thread that ever acquired heap access on any client is I4(b)-registered in
// m_threadGroup (GCClient::Heap::acquireHeapAccess enforces this; JSLock and
// attachCurrentThread register eagerly), so the suspend-and-copy loop below
// covers the stacks and registers of every mutator other than the conductor —
// whether it is parked in notifyVMStop, blocked in acquireHeapAccess's F8
// revert wait, or running access-released native code (conservative scanning
// a live native stack is a sound superset). The conductor's own stack and
// registers flow through `currentThreadState` (captured by
// callWithCurrentThreadState at the bottom of the conducted cycle's phase
// loop) and `currentThread` (== Heap::m_currentThread, stamped by
// runCurrentPhase), which is excluded from suspension even when this function
// runs on a parallel marking helper (the helper itself is excluded via
// Thread::currentSingleton(); helpers are never registered). This satisfies
// I12's stack ∪ registers clause for all registered threads.
void MachineThreads::gatherConservativeRoots(ConservativeRoots& conservativeRoots, JITStubRoutineSet& jitStubRoutines, CodeBlockSet& codeBlocks, CurrentThreadState* currentThreadState, Thread* currentThread)
{
    if (currentThreadState)
        gatherFromCurrentThread(conservativeRoots, jitStubRoutines, codeBlocks, *currentThreadState);

    size_t size;
    size_t capacity = 0;
    void* buffer = nullptr;
    Locker locker { m_threadGroup->getLock() };
    while (!tryCopyOtherThreadStacks(locker, buffer, capacity, &size, *currentThread))
        growBuffer(size, &buffer, &capacity);

    if (!buffer)
        return;

    conservativeRoots.add(buffer, static_cast<char*>(buffer) + size, jitStubRoutines, codeBlocks);
    fastFree(buffer);
}

NEVER_INLINE int callWithCurrentThreadState(const ScopedLambda<void(CurrentThreadState&)>& lambda)
{
    DECLARE_AND_COMPUTE_CURRENT_THREAD_STATE(state);
    lambda(state);
    return 42; // Suppress tail call optimization.
}

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
