/*
 * Copyright (C) 2005-2026 Apple Inc. All rights reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 */

#pragma once

#include "JSExportMacros.h"
#include <mutex>
#include <wtf/Assertions.h>
#include <wtf/Lock.h>
#include <wtf/Noncopyable.h>
#include <wtf/RefPtr.h>
#include <wtf/Threading.h>
#include <wtf/Vector.h>
#include <wtf/text/AtomStringTable.h>

namespace JSC {

// To make it safe to use JavaScript on multiple threads, it is
// important to lock before doing anything that allocates a
// JavaScript data structure or that interacts with shared state
// such as the protect count hash table. The simplest way to lock
// is to create a local JSLockHolder object in the scope where the lock 
// must be held and pass it the context that requires protection. 
// The lock is recursive so nesting is ok. The JSLock 
// object also acts as a convenience short-hand for running important
// initialization routines.

// To avoid deadlock, sometimes it is necessary to temporarily
// release the lock. Since it is recursive you actually have to
// release all locks held by your thread. This is safe to do if
// you are executing code that doesn't require the lock, and you
// reacquire the right number of locks at the end. You can do this
// by constructing a locally scoped JSLock::DropAllLocks object. The 
// DropAllLocks object takes care to release the JSLock only if your
// thread acquired it to begin with.

class CallFrame;
class EntryFrame;
class Exception;
class ExceptionScope;
class JSPIContext;
class VM;
class VMEntryScope;
class JSGlobalObject;
class JSLock;

// FIXME: We should either have a specialization of WTF::Locker for JSLock or only allow using JSLockHolder.
// It's weird that WTF::Locker<JSLock> doesn't ref() the VM for the lifetime of the lock and it's unclear
// there's any noticable performance difference.
class JSLockHolder {
public:
    JS_EXPORT_PRIVATE JSLockHolder(VM*);
    JS_EXPORT_PRIVATE JSLockHolder(VM&);
    JS_EXPORT_PRIVATE JSLockHolder(JSGlobalObject*);

    JS_EXPORT_PRIVATE ~JSLockHolder();

private:
    RefPtr<VM> m_vm;
};

class JSLock : public ThreadSafeRefCounted<JSLock> {
    WTF_MAKE_NONCOPYABLE(JSLock);
public:
    JSLock(VM*);
    JS_EXPORT_PRIVATE ~JSLock();

    JS_EXPORT_PRIVATE void lock();
    JS_EXPORT_PRIVATE void unlock();

    static void lock(JSGlobalObject*);
    static void unlock(JSGlobalObject*);
    static void lock(VM&);
    static void unlock(VM&);

    VM* vm() { return m_vm; }

    std::optional<RefPtr<Thread>> ownerThread() const
    {
        if (m_hasOwnerThread.load(std::memory_order_acquire))
            return m_ownerThread;
        return std::nullopt;
    }

    // Returns the owner thread's UID without creating temporary RefPtr objects.
    // This avoids ref counting operations that can cause lock contention
    // with thread suspension. Returns std::nullopt if there is no owner thread.
    std::optional<uint64_t> ownerThreadUID() const
    {
        if (!m_hasOwnerThread.load(std::memory_order_acquire))
            return std::nullopt;
        return m_ownerThread->uid();
    }

    bool currentThreadIsHoldingLock() { return m_hasOwnerThread.load(std::memory_order_acquire) && m_ownerThread.get() == &Thread::currentSingleton(); }
    // The same question, for whoever has its Thread at hand: to look that up is most of what it costs where that takes a call (pthread_getspecific()).
    bool isHeldBy(const Thread& thread) const
    {
        ASSERT(&thread == &Thread::currentSingleton());
        return m_hasOwnerThread.load(std::memory_order_acquire) && m_ownerThread.get() == &thread;
    }

    // For code that knows it runs on the thread that holds the lock (it is in the middle of running JS) and wants that
    // thread without looking it up.
    Thread& ownerThreadWhileHoldingLock()
    {
        ASSERT(currentThreadIsHoldingLock());
        return *m_ownerThread;
    }

    void LEGACY_NODELETE willDestroyVM(VM*);

    class DropAllLocks {
        WTF_MAKE_NONCOPYABLE(DropAllLocks);
    public:
        JS_EXPORT_PRIVATE DropAllLocks(JSGlobalObject*);
        JS_EXPORT_PRIVATE DropAllLocks(VM*);
        JS_EXPORT_PRIVATE DropAllLocks(VM&);
        JS_EXPORT_PRIVATE ~DropAllLocks();

        void setDropDepth(unsigned depth) { m_dropDepth = depth; }
        unsigned dropDepth() const { return m_dropDepth; }

    private:
        intptr_t m_droppedLockCount;
        RefPtr<VM> m_vm;
        unsigned m_dropDepth;
    };

    // Threads that take turns in the VM.
    //
    // With DropAllLocks, another thread may use the VM in the middle of a call of this one, but it has to be gone from the VM
    // when this one takes the lock back: what the VM knows about the code that is running (the chain of frames, the entry
    // scope, the limits of the stack) is there once, so drops have to end in the reverse of the order they began in.
    //
    // A thread that parks takes all that out of the VM and leaves the VM as it is when nothing runs in it. Any number of
    // threads may be parked, each in the middle of calls of its own, and they come back in any order. Who runs next is for
    // the client to say (Python has its global interpreter lock): unpark() just takes the lock.
    //
    // The stack of a parked thread is scanned like that of any thread that has held the lock. The object must be on that
    // stack: it refers to cells.
    class ParkedThread {
        WTF_MAKE_NONCOPYABLE(ParkedThread);
    public:
        ParkedThread() = default;

        // The frames of the thread, for walking them as one does from VM::topCallFrame and VM::topEntryFrame.
        CallFrame* topCallFrame() const { return m_topCallFrame; }
        EntryFrame* topEntryFrame() const { return m_topEntryFrame; }
        Thread& thread() const { return *m_thread; }

    private:
        friend class JSLock;

        SUPPRESS_UNCOUNTED_MEMBER Thread* m_thread { nullptr };
        intptr_t m_lockCount { 0 };
        CallFrame* m_topCallFrame { nullptr };
        EntryFrame* m_topEntryFrame { nullptr };
        void* m_maybeReturnPC { nullptr };
        JSPIContext* m_topJSPIContext { nullptr };
        VMEntryScope* m_entryScope { nullptr };
        Exception* m_exception { nullptr };
        Exception* m_lastException { nullptr };
        void* m_stackPointerAtVMEntry { nullptr };
        void* m_lastStackTop { nullptr };
        size_t m_softReservedZoneSize { 0 };
        unsigned m_disallowVMEntryCount { 0 };
        unsigned m_allowLazyMaterializationOfImmutablePropertiesCount { 0 };
        // With ENABLE(EXCEPTION_SCOPE_VERIFICATION) only.
        ExceptionScope* m_topExceptionScope { nullptr };
        unsigned m_simulatedThrowPointRecursionDepth { 0 };
        bool m_needExceptionCheck { false };
    };

    // The current thread holds the lock and has access to the heap, and the collector is not at work on it.
    JS_EXPORT_PRIVATE void park(ParkedThread&);
    JS_EXPORT_PRIVATE void unpark(ParkedThread&);
    bool hasParkedThreads() const { return m_parkedThreadCount.load(std::memory_order_relaxed); }
    // For the thread that holds the lock, and for the collector while the world is stopped.
    template<typename Functor>
    void forEachParkedThread(const Functor& functor)
    {
        Locker locker { m_parkedThreadsLock };
        for (ParkedThread* parked : m_parkedThreads)
            functor(*parked);
    }

    void makeWebThreadAware()
    {
        m_isWebThreadAware = true;
    }

    bool isWebThreadAware() const { return m_isWebThreadAware; }

private:
    void lock(intptr_t lockCount);
    void unlock(intptr_t unlockCount);

    void didAcquireLock();
    void willReleaseLock();

    unsigned dropAllLocks(DropAllLocks*);
    void grabAllLocks(DropAllLocks*, unsigned lockCount);

#if PLATFORM(COCOA) && CPU(ADDRESS64) && CPU(ARM64)
    // FIXME: rdar://168614004
    NO_RETURN_DUE_TO_CRASH NEVER_INLINE void dumpInfoAndCrashForLockNotOwned();
#endif

    Lock m_lock;
    bool m_isWebThreadAware { false };
    // We cannot make m_ownerThread an optional (instead of pairing it with an explicit
    // m_hasOwnerThread) because currentThreadIsHoldingLock() may be called from a
    // different thread, and an optional is vulnerable to races.
    // See https://bugs.webkit.org/show_bug.cgi?id=169042#c6
    std::atomic<bool> m_hasOwnerThread { false };
    bool m_shouldReleaseHeapAccess;
    bool m_isParking { false }; // For willReleaseLock().
    RefPtr<Thread> m_ownerThread;
    intptr_t m_lockCount;
    unsigned m_lockDropDepth;
    uint32_t m_lastOwnerThread { 0 };
    VM* m_vm;
    AtomStringTable* m_entryAtomStringTable; 
    Lock m_parkedThreadsLock;
    Vector<ParkedThread*, 4> m_parkedThreads WTF_GUARDED_BY_LOCK(m_parkedThreadsLock);
    std::atomic<unsigned> m_parkedThreadCount { 0 };
};

} // namespace
