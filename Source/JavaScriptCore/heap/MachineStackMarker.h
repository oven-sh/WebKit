/*
 *  Copyright (C) 1999-2000 Harri Porten (porten@kde.org)
 *  Copyright (C) 2001 Peter Kelly (pmk@post.com)
 *  Copyright (C) 2003-2023 Apple Inc. All rights reserved.
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

#pragma once

#include "RegisterState.h"
#include <wtf/Lock.h>
#include <wtf/ScopedLambda.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/ThreadGroup.h>

namespace JSC {

class CodeBlockSet;
class ConservativeRoots;
class Heap;
class JITStubRoutineSet;

struct CurrentThreadState {
    void* stackOrigin { nullptr };
    void* stackTop { nullptr };
    RegisterState* registerState { nullptr };
};
    
class MachineThreads {
    WTF_MAKE_TZONE_ALLOCATED(MachineThreads);
    WTF_MAKE_NONCOPYABLE(MachineThreads);
public:
    MachineThreads();

    // T5-rootscan-skip-coop-parked-suspend: the optional
    // `coopParkedSnapshotLookup` lets the shared-server Heap hand in a
    // Thread* -> CurrentThreadState* lookup for siblings that are
    // cooperatively parked with a published register/stack snapshot
    // (GCClient::Heap::m_parkedRootSnapshot). For each registered thread the
    // suspend-and-copy loop first consults the lookup; on a hit it copies the
    // saved snapshot directly (tryCopyCooperativelyParkedThreadStack) and
    // SKIPS the SIGUSR2 suspend()/getRegisters()/resume() round-trip.
    // Passing nullptr (the default — every flag-off / non-shared call site)
    // leaves the original suspend-everything path byte-for-byte: the lookup
    // branch is gated [[unlikely]] on the pointer and the per-thread
    // exclusion clause short-circuits on the same nullptr test.
    void gatherConservativeRoots(ConservativeRoots&, JITStubRoutineSet&, CodeBlockSet&, CurrentThreadState*, Thread*, const ScopedLambda<CurrentThreadState*(Thread&)>* coopParkedSnapshotLookup = nullptr);

    // Only needs to be called by clients that can use the same heap from multiple threads.
    bool addCurrentThread() { return Ref { m_threadGroup }->addCurrentThread() == ThreadGroupAddResult::NewlyAdded; }

    // SharedGC (SPEC-heap.md §10.6/I4(b), T6): true iff the calling thread is
    // registered for conservative scanning (its stack and registers are part
    // of the I12 root set). Used by debug cross-checks in the shared-mode
    // heap-access protocol; takes the thread-group lock, so debug-only use.
    bool includesCurrentThread();

    WordLock& getLock() { return m_threadGroup->getLock(); }
    const ListHashSet<Ref<Thread>>& threads(const AbstractLocker& locker) const { return m_threadGroup->threads(locker); }

private:
    void gatherFromCurrentThread(ConservativeRoots&, JITStubRoutineSet&, CodeBlockSet&, CurrentThreadState&);

    void tryCopyOtherThreadStack(const ThreadSuspendLocker&, Thread&, void*, size_t capacity, size_t*);
    bool tryCopyCooperativelyParkedThreadStack(Thread&, CurrentThreadState&, void*, size_t capacity, size_t*);
    bool tryCopyOtherThreadStacks(const AbstractLocker&, void*, size_t capacity, size_t*, Thread&, const ScopedLambda<CurrentThreadState*(Thread&)>*);

    Ref<ThreadGroup> m_threadGroup;
};

#define DECLARE_AND_COMPUTE_CURRENT_THREAD_STATE(stateName) \
    CurrentThreadState stateName; \
    stateName.stackTop = &stateName; \
    stateName.stackOrigin = Thread::currentSingleton().stack().origin(); \
    ALLOCATE_AND_GET_REGISTER_STATE(stateName ## _registerState); \
    stateName.registerState = &stateName ## _registerState

// The return value is meaningless. We just use it to suppress tail call optimization.
int callWithCurrentThreadState(const ScopedLambda<void(CurrentThreadState&)>&);

} // namespace JSC

