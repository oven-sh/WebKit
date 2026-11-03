/*
 * Copyright (C) 2005-2026 Apple Inc. All rights reserved.
 * Copyright (C) 2026 Igalia S.L.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the NU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA
 *
 */

#include "config.h"
#include "JSLock.h"

#include "HeapInlines.h"
#include "JSGlobalObject.h"
#include "MachineStackMarker.h"
#include "SamplingProfiler.h"
#include "VMTrapsInlines.h"
#include <wtf/SpinBackoff.h>
#include <wtf/StackPointer.h>
#include <wtf/Threading.h>
#include <wtf/threads/Signals.h>

#if USE(WEB_THREAD)
#include <wtf/ios/WebCoreThread.h>
#endif

#if PLATFORM(COCOA)
#include <wtf/cocoa/RuntimeApplicationChecksCocoa.h>
#endif

namespace JSC {

JSLockHolder::JSLockHolder(JSGlobalObject* globalObject)
    : JSLockHolder(globalObject->vm())
{
}

JSLockHolder::JSLockHolder(VM* vm)
    : JSLockHolder(*vm)
{
}

JSLockHolder::JSLockHolder(VM& vm)
    : m_vm(&vm)
{
    protect(m_vm->apiLock())->lock();
}

JSLockHolder::~JSLockHolder()
{
    RefPtr<JSLock> apiLock(&m_vm->apiLock());
    m_vm = nullptr;
    apiLock->unlock();
}

JSLock::JSLock(VM* vm)
    : m_lockCount(0)
    , m_lockDropDepth(0)
    , m_vm(vm)
    , m_entryAtomStringTable(nullptr)
{
}

JSLock::~JSLock() = default;

void JSLock::willDestroyVM(VM* vm)
{
    ASSERT_UNUSED(vm, m_vm == vm);
    m_vm = nullptr;
}

void JSLock::lock()
{
    lock(1);
}

// Use WTF_IGNORES_THREAD_SAFETY_ANALYSIS because this function conditionally unlocks m_lock, which
// is not supported by analysis.
void JSLock::lock(intptr_t lockCount) WTF_IGNORES_THREAD_SAFETY_ANALYSIS
{
    ASSERT(lockCount > 0);
#if USE(WEB_THREAD)
    if (m_isWebThreadAware) {
        ASSERT(WebCoreWebThreadIsEnabled && WebCoreWebThreadIsEnabled());
        WebCoreWebThreadLock();
    }
#endif

    bool success = m_lock.tryLock();
    if (!success) [[unlikely]] {
        if (currentThreadIsHoldingLock()) {
            m_lockCount += lockCount;
            return;
        }
        m_lock.lock();
    }

    m_ownerThread = &Thread::currentSingleton();
    m_hasOwnerThread.store(true, std::memory_order_release);
    ASSERT(!m_lockCount);
    m_lockCount = lockCount;

    didAcquireLock();
}

void JSLock::didAcquireLock()
{
    // FIXME: What should happen to the per-thread identifier table if we don't have a VM?
    if (!m_vm)
        return;
    
    auto& thread = Thread::currentSingleton();
    ASSERT(!m_entryAtomStringTable);
    m_entryAtomStringTable = thread.setCurrentAtomStringTable(m_vm->atomStringTable());
    ASSERT(m_entryAtomStringTable);

    m_vm->setLastStackTop(thread);

    if (m_vm->heap.hasAccess())
        m_shouldReleaseHeapAccess = false;
    else {
        m_vm->heap.acquireAccess();
        m_shouldReleaseHeapAccess = true;
    }

    RELEASE_ASSERT(!m_vm->stackPointerAtVMEntry());
    void* p = currentStackPointer();
    m_vm->setStackPointerAtVMEntry(p);

    if (thread.uid() != m_lastOwnerThread) {
        m_lastOwnerThread = thread.uid();
        if (m_vm->heap.machineThreads().addCurrentThread()) {
            if (isKernTCSMAvailable())
                enableKernTCSM();
        }
    }

    // Note: everything below must come after addCurrentThread().
    m_vm->traps().notifyGrabAllLocks();

#if ENABLE(SAMPLING_PROFILER)
    {
        SamplingProfiler* samplingProfiler = m_vm->samplingProfiler();
        if (samplingProfiler) [[unlikely]]
            samplingProfiler->noticeJSLockAcquisition();
    }
#endif
}

void JSLock::unlock()
{
    unlock(1);
}

#if PLATFORM(COCOA) && CPU(ADDRESS64) && CPU(ARM64)
WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN
// FIXME: rdar://168614004
NO_RETURN_DUE_TO_CRASH NEVER_INLINE void JSLock::dumpInfoAndCrashForLockNotOwned() // __attribute__((optnone))
{
    size_t pageSize = WTF::pageSize();
    RELEASE_ASSERT(isPowerOfTwo(pageSize));
    uintptr_t pageMask = ~(static_cast<uintptr_t>(pageSize) - 1);

    uintptr_t* thisAsIntPtr = std::bit_cast<uintptr_t*>(this);
    uintptr_t thisAsInt = std::bit_cast<uintptr_t>(this);
    uintptr_t thisEndAsInt = thisAsInt + sizeof(JSLock);
    uintptr_t blockStartAsInt = thisAsInt & pageMask;
    uintptr_t blockEndAsInt = blockStartAsInt + pageSize;
    char* blockStart = std::bit_cast<char*>(blockStartAsInt);

    register uint64_t dumpState __asm__("x28");

#define updateDumpState(newState, used1, used2, used3) do { \
        WTF::compilerFence(); \
        __asm__ volatile ("mov %0, #" #newState : "=r"(dumpState) : "r"(used1), "r"(used2), "r"(used3)); \
        WTF::compilerFence(); \
    } while (false)

    updateDumpState(0x1111, dumpState, dumpState, dumpState);

    register void* currentThread __asm__("x27") = &Thread::currentSingleton();
    updateDumpState(0x2222, currentThread, dumpState, dumpState);

    // Checks if the this pointer is corrupted. Being out of the page bounds is 1 example of corruption.
    bool lockIsWithinPageBoundary = (blockStartAsInt <= thisAsInt) && (thisEndAsInt <= blockEndAsInt);
    register uintptr_t miscState __asm__("x26") = lockIsWithinPageBoundary;
    updateDumpState(0x3333, miscState, dumpState, dumpState);

    register uintptr_t lockWord0 __asm__("x25") = thisAsIntPtr[0];
    updateDumpState(0x4444, lockWord0, dumpState, dumpState);

    register void* ownerThread __asm__("x24") = m_ownerThread.get();
    updateDumpState(0x5555, ownerThread, dumpState, dumpState);

    register uintptr_t lockWord2 __asm__("x23") = thisAsIntPtr[2];
    register uintptr_t lockWord3 __asm__("x22") = thisAsIntPtr[3];
    updateDumpState(0x6666, lockWord2, lockWord3, dumpState);

    miscState |= (!!m_vm) << 8; // Check if VM is null.
    updateDumpState(0x7777, miscState, dumpState, dumpState);

    // Check if the page is zero.
    register uintptr_t numZeroBytesBeforeAfter __asm__("x21") = 0;

    uintptr_t totalZeroBytesInPage = 0;
    uintptr_t currentZeroBytes = 0;

    // Count zero bytes before JSLock.
    uintptr_t bytesBeforeLock = thisAsInt - blockStartAsInt;
    for (auto mem = blockStart; mem < blockStart + bytesBeforeLock; mem++) {
        bool byteIsZero = !*mem;
        if (byteIsZero)
            currentZeroBytes++;
    }
    numZeroBytesBeforeAfter = currentZeroBytes;
    numZeroBytesBeforeAfter |= bytesBeforeLock << 16;
    updateDumpState(0x8888, numZeroBytesBeforeAfter, bytesBeforeLock, currentZeroBytes);

    totalZeroBytesInPage += currentZeroBytes;

    // Count zero bytes after JSLock.
    currentZeroBytes = 0;
    uintptr_t bytesAfterBlock = pageSize - (thisAsInt + sizeof(JSLock));
    for (auto mem = blockStart + bytesBeforeLock + sizeof(JSLock); mem < blockStart + pageSize; mem++) {
        bool byteIsZero = !*mem;
        if (byteIsZero)
            currentZeroBytes++;
    }
    numZeroBytesBeforeAfter |= currentZeroBytes << 32;
    numZeroBytesBeforeAfter |= bytesAfterBlock << 48;
    updateDumpState(0x9999, numZeroBytesBeforeAfter, bytesAfterBlock, currentZeroBytes);

    totalZeroBytesInPage += currentZeroBytes;

    register uintptr_t numZeroBytesInLock __asm__("x20") = 0;
    currentZeroBytes = 0;
    for (auto mem = blockStart + bytesBeforeLock; mem < blockStart + bytesBeforeLock + sizeof(JSLock); mem++) {
        bool byteIsZero = !*mem;
        if (byteIsZero)
            currentZeroBytes++;
    }
    numZeroBytesInLock = currentZeroBytes;
    numZeroBytesInLock |= sizeof(JSLock) << 16;
    updateDumpState(0xAAAA, numZeroBytesInLock, currentZeroBytes, dumpState);

    totalZeroBytesInPage += currentZeroBytes;
    numZeroBytesInLock |= totalZeroBytesInPage << 32;
    updateDumpState(0xBBBB, numZeroBytesInLock, totalZeroBytesInPage, currentZeroBytes);

    register VM* vmPtr __asm__("r19") = m_vm;
    register AtomStringTable* atomStringTable __asm__("x15") = m_entryAtomStringTable;
    register JSLock* thisPtr __asm__("x14") = this;
    updateDumpState(0xCCCC, vmPtr, atomStringTable, thisPtr);

    __asm__ volatile (WTF_FATAL_CRASH_INST : : "r"(dumpState), "r"(miscState), "r"(lockWord0), "r"(currentThread), "r"(ownerThread), "r"(lockWord2), "r"(lockWord3), "r"(numZeroBytesBeforeAfter), "r"(numZeroBytesInLock), "r"(vmPtr), "r"(atomStringTable), "r"(thisPtr));
    __builtin_unreachable();

#undef updateDumpState
}
WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
#endif

// Use WTF_IGNORES_THREAD_SAFETY_ANALYSIS because this function conditionally unlocks m_lock, which
// is not supported by analysis.
void JSLock::unlock(intptr_t unlockCount) WTF_IGNORES_THREAD_SAFETY_ANALYSIS
{
#if PLATFORM(COCOA) && CPU(ADDRESS64) && CPU(ARM64)
    if (!currentThreadIsHoldingLock()) [[unlikely]]
        dumpInfoAndCrashForLockNotOwned();
#else
    RELEASE_ASSERT(currentThreadIsHoldingLock());
#endif

    ASSERT(m_lockCount >= unlockCount);

    // Maintain m_lockCount while calling willReleaseLock() so that its callees know that
    // they still have the lock.
    if (unlockCount == m_lockCount)
        willReleaseLock();

    m_lockCount -= unlockCount;

    if (!m_lockCount) {
        m_hasOwnerThread.store(false, std::memory_order_release);
        m_lock.unlock();
    }
}

void JSLock::willReleaseLock()
{
    {
        RefPtr protectedVM { m_vm };
        if (protectedVM) {
            static bool useLegacyDrain = false;
#if PLATFORM(COCOA)
            static std::once_flag once;
            std::call_once(once, [] {
                useLegacyDrain = !linkedOnOrAfterSDKWithBehavior(SDKAlignedBehavior::DoesNotDrainTheMicrotaskQueueWhenCallingObjC);
            });
#endif

            // While threads are parked, the one that lets go is not the last to have something going on in the VM.
            if ((!m_lockDropDepth && !hasParkedThreads()) || useLegacyDrain)
                protectedVM->drainMicrotasks();

            if (!protectedVM->topCallFrame)
                protectedVM->clearLastException();

            protectedVM->heap.releaseDelayedReleasedObjects();
            protectedVM->setStackPointerAtVMEntry(nullptr);

            if (m_shouldReleaseHeapAccess)
                protectedVM->heap.releaseAccess();
        }
    }

    if (m_entryAtomStringTable) {
        Thread::currentSingleton().setCurrentAtomStringTable(m_entryAtomStringTable);
        m_entryAtomStringTable = nullptr;
    }
}

void JSLock::lock(JSGlobalObject* globalObject)
{
    protect(globalObject->vm().apiLock())->lock();
}

void JSLock::unlock(JSGlobalObject* globalObject)
{
    protect(globalObject->vm().apiLock())->unlock();
}

// This function returns the number of locks that were dropped.
unsigned JSLock::dropAllLocks(DropAllLocks* dropper)
{
    if (!currentThreadIsHoldingLock())
        return 0;

    ++m_lockDropDepth;

    dropper->setDropDepth(m_lockDropDepth);

    auto& thread = Thread::currentSingleton();
    thread.setSavedStackPointerAtVMEntry(m_vm->stackPointerAtVMEntry());
    thread.setSavedLastStackTop(m_vm->lastStackTop());

    unsigned droppedLockCount = m_lockCount;
    unlock(droppedLockCount);

    return droppedLockCount;
}

void JSLock::grabAllLocks(DropAllLocks* dropper, unsigned droppedLockCount)
{
    // If no locks were dropped, nothing to do!
    if (!droppedLockCount)
        return;

    ASSERT(!currentThreadIsHoldingLock());
    lock(droppedLockCount);

    SpinBackoff backoff;
    while (dropper->dropDepth() != m_lockDropDepth) {
        unlock(droppedLockCount);
        backoff.spinOnce();
        lock(droppedLockCount);
    }

    --m_lockDropDepth;

    auto& thread = Thread::currentSingleton();
    m_vm->setStackPointerAtVMEntry(thread.savedStackPointerAtVMEntry());
    m_vm->setLastStackTop(thread);
}

void JSLock::park(ParkedThread& parked)
{
    RELEASE_ASSERT(currentThreadIsHoldingLock());
    VM& vm = *m_vm;
    RELEASE_ASSERT(!vm.isCollectorBusyOnCurrentThread());
    ASSERT(!vm.hasCheckpointOSRSideState());

    parked.m_thread = &Thread::currentSingleton();
    parked.m_lockCount = m_lockCount;
    parked.m_topCallFrame = std::exchange(vm.topCallFrame, nullptr);
    parked.m_topEntryFrame = std::exchange(vm.topEntryFrame, nullptr);
    parked.m_maybeReturnPC = std::exchange(vm.maybeReturnPC, nullptr);
    parked.m_topJSPIContext = std::exchange(vm.topJSPIContext, nullptr);
    parked.m_entryScope = std::exchange(vm.entryScope, nullptr);
    parked.m_lastException = std::exchange(vm.m_lastException, nullptr);
#if ENABLE(EXCEPTION_SCOPE_VERIFICATION)
    parked.m_needExceptionCheck = vm.m_needExceptionCheck;
#endif
    parked.m_exception = vm.m_exception;
    if (parked.m_exception)
        vm.clearException();
    parked.m_stackPointerAtVMEntry = vm.m_stackPointerAtVMEntry;
    parked.m_lastStackTop = vm.m_lastStackTop;
    parked.m_softReservedZoneSize = vm.m_currentSoftReservedZoneSize;
    vm.m_currentSoftReservedZoneSize = Options::softReservedZoneSize();
    parked.m_disallowVMEntryCount = std::exchange(vm.disallowVMEntryCount, 0);
    parked.m_allowLazyMaterializationOfImmutablePropertiesCount = std::exchange(vm.allowLazyMaterializationOfImmutablePropertiesCount, 0);
#if ENABLE(EXCEPTION_SCOPE_VERIFICATION)
    parked.m_topExceptionScope = std::exchange(vm.m_topExceptionScope, nullptr);
    parked.m_simulatedThrowPointRecursionDepth = std::exchange(vm.m_simulatedThrowPointRecursionDepth, 0);
    vm.m_needExceptionCheck = false;
#endif

    {
        Locker locker { m_parkedThreadsLock };
        m_parkedThreads.append(&parked);
        m_parkedThreadCount.store(m_parkedThreads.size(), std::memory_order_relaxed);
    }

    // Whoever comes next acquires access to the heap for itself; and if nobody comes, the collector need not wait for anybody.
    m_shouldReleaseHeapAccess = vm.heap.hasAccess();
    unlock(m_lockCount);
}

void JSLock::unpark(ParkedThread& parked)
{
    ASSERT(parked.m_thread == &Thread::currentSingleton());
    ASSERT(!currentThreadIsHoldingLock());
    lock(parked.m_lockCount);

    // From here on the collector can only run where this thread lets it.
    {
        Locker locker { m_parkedThreadsLock };
        bool wasThere = m_parkedThreads.removeFirst(&parked);
        RELEASE_ASSERT(wasThere);
        m_parkedThreadCount.store(m_parkedThreads.size(), std::memory_order_relaxed);
    }

    VM& vm = *m_vm;
    RELEASE_ASSERT(!vm.topCallFrame && !vm.entryScope && !vm.m_exception);
    vm.topCallFrame = parked.m_topCallFrame;
    vm.topEntryFrame = parked.m_topEntryFrame;
    vm.maybeReturnPC = parked.m_maybeReturnPC;
    vm.topJSPIContext = parked.m_topJSPIContext;
    vm.entryScope = parked.m_entryScope;
    vm.m_lastException = parked.m_lastException;
    if (parked.m_exception)
        vm.setException(parked.m_exception);
    vm.m_lastStackTop = parked.m_lastStackTop;
    vm.m_currentSoftReservedZoneSize = parked.m_softReservedZoneSize;
    vm.setStackPointerAtVMEntry(parked.m_stackPointerAtVMEntry);
    vm.disallowVMEntryCount = parked.m_disallowVMEntryCount;
    vm.allowLazyMaterializationOfImmutablePropertiesCount = parked.m_allowLazyMaterializationOfImmutablePropertiesCount;
#if ENABLE(EXCEPTION_SCOPE_VERIFICATION)
    vm.m_topExceptionScope = parked.m_topExceptionScope;
    vm.m_simulatedThrowPointRecursionDepth = parked.m_simulatedThrowPointRecursionDepth;
    vm.m_needExceptionCheck = parked.m_needExceptionCheck;
#endif
}

JSLock::DropAllLocks::DropAllLocks(VM* vm)
    : m_droppedLockCount(0)
    // If the VM is in the middle of being destroyed then we don't want to resurrect it
    // by allowing DropAllLocks to ref it. By this point the JSLock has already been
    // released anyways, so it doesn't matter that DropAllLocks is a no-op.
    , m_vm(vm->heap.isShuttingDown() ? nullptr : vm)
{
    if (!m_vm)
        return;

    // Contrary to intuition, DropAllLocks does not require that we are actually holding
    // the JSLock before getting here. Its goal is to release the lock if it is held. So,
    // if the lock isn't already held, there's nothing to do, and that's fine.
    // See https://bugs.webkit.org/show_bug.cgi?id=139654#c11.
    RELEASE_ASSERT(!m_vm->currentThreadIsHoldingAPILock() || !m_vm->isCollectorBusyOnCurrentThread(), m_vm->currentThreadIsHoldingAPILock(), m_vm->isCollectorBusyOnCurrentThread());
    m_droppedLockCount = protect(m_vm->apiLock())->dropAllLocks(this);
}

JSLock::DropAllLocks::DropAllLocks(JSGlobalObject* globalObject)
    : DropAllLocks(globalObject ? &globalObject->vm() : nullptr)
{
}

JSLock::DropAllLocks::DropAllLocks(VM& vm)
    : DropAllLocks(&vm)
{
}

JSLock::DropAllLocks::~DropAllLocks()
{
    if (!m_vm)
        return;
    protect(m_vm->apiLock())->grabAllLocks(this, m_droppedLockCount);
}

} // namespace JSC
