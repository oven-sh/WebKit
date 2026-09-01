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
class VM;
class VMLite;
class JSGlobalObject;
class JSLock;

namespace GCClient {
class Heap;
}

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
            return RefPtr<Thread> { m_ownerThreadPtr.load(std::memory_order_relaxed) };
        return std::nullopt;
    }

    // Returns the owner thread's UID without creating temporary RefPtr objects.
    // This avoids ref counting operations that can cause lock contention
    // with thread suspension. Returns std::nullopt if there is no owner thread.
    std::optional<uint64_t> ownerThreadUID() const
    {
        if (!m_hasOwnerThread.load(std::memory_order_acquire))
            return std::nullopt;
        if (Thread* thread = m_ownerThreadPtr.load(std::memory_order_relaxed))
            return thread->uid();
        return std::nullopt;
    }

    bool currentThreadIsHoldingLock() { return m_hasOwnerThread.load(std::memory_order_acquire) && m_ownerThreadPtr.load(std::memory_order_relaxed) == &Thread::currentSingleton(); }

    void NODELETE willDestroyVM(VM*);

    // SPEC-vmstate §6.4.4: called at the TOP of ~VM (M6), while this thread
    // still holds the API lock and before lastChanceToFinalize, so no
    // thread's TLS dangles across teardown (I20). If this hold installed the
    // main carrier, restore the entry value and clear the bookkeeping.
    void uninstallVMLiteForVMDestruction();

    // Shared-memory Thread API (docs/threads/SPEC-api.md 5.2 /
    // INTEGRATE-api.md 9.2-9): fully releases the lock for a thread that is
    // about to PARK, without running willReleaseLock()'s microtask drain
    // (which would execute user JS inside the parking host call). The
    // m_lockDropDepth bump is bumped AND restored while m_lock is still
    // held, so it never escapes into the DropAllLocks strict-LIFO unwind
    // protocol — N threads can park and wake in any order (the
    // GILDroppedSection livelock fix is preserved). Returns the number of
    // lock counts released; the caller reacquires with that many lock()
    // calls. Sole caller: GILDroppedSection (runtime/LockObject.cpp).
    JS_EXPORT_PRIVATE unsigned unlockAllForThreadParking();

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
    // m_ownerThread is the ref-holder only: it is written exclusively by the
    // thread that just acquired m_lock (depth-0 in JSLock::lock) and is never
    // read across threads. All cross-thread identity reads go through
    // m_ownerThreadPtr, an atomic mirror of the pointer word, because
    // contending threads in lock() call currentThreadIsHoldingLock() while a
    // new owner is storing the RefPtr (TSAN data race on the plain pointer
    // word under GILDroppedSection re-lock churn). The mirror is stored
    // before the m_hasOwnerThread release-store, so any acquire-load of the
    // flag that observes true also observes the matching owner pointer;
    // relaxed loads of the mirror are therefore sufficient.
    RefPtr<Thread> m_ownerThread;
    std::atomic<Thread*> m_ownerThreadPtr { nullptr };
    intptr_t m_lockCount;
    unsigned m_lockDropDepth;
    uint32_t m_lastOwnerThread { 0 };
    VM* m_vm;
    AtomStringTable* m_entryAtomStringTable;
    VMLite* m_entryVMLite { nullptr };
    bool m_didInstallVMLite { false };
    // UNGIL §A.3.6 (ANNEXES A36 + A36C; U-T1, dark): GIL-off the swapped TLS
    // state is the TUPLE {lite, TID-tag, heap §10A.1 currentThreadClient
    // slot}. The lite/tag halves ride m_entryVMLite + VMLite::setCurrent's
    // tag hook; this is the saved client-slot half, restored LIFO at the
    // depth-0 unlock. m_didInstallCarrierVMLite keys the GIL-off carrier
    // path, disjoint from the GIL-on m_didInstallVMLite main-carrier path.
    GCClient::Heap* m_entryThreadClient { nullptr };
    bool m_didInstallCarrierVMLite { false };
};

} // namespace
