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
#include <wtf/DataLog.h>
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

// T5-rootscan-skip-coop-parked-suspend (SCALEBENCH §31, offcpu16 row #4):
// same shape as tryCopyOtherThreadStack, but the registers and stackTop come
// from a CurrentThreadState that the TARGET thread captured itself
// (DECLARE_AND_COMPUTE_CURRENT_THREAD_STATE in its parking caller's frame)
// and seq_cst-published into its GCClient::Heap immediately before
// descending into pure libc futex/condvar machinery with heap access
// released. The caller's frame stays live across the park, so both the
// CurrentThreadState struct and the RegisterState it points at are stable
// memory; everything below `snapshot.stackTop` is post-snapshot park
// plumbing with no JSCell* (and may still be mutating between publish and
// the actual futex sleep, or across a bounded-wait timeout tick), so we
// deliberately do NOT extend by the OS red-zone the way captureStack() does
// for a suspend-captured SP — the captured span is already an
// at-least-as-conservative superset of the thread's JS-relevant roots.
// Called BEFORE any thread is suspended (no ThreadSuspendLocker needed) and
// uses copyMemory only (no malloc/locks), so the suspend-phase deadlock
// rules are trivially satisfied.
//
// Returns true iff the cooperative snapshot was accepted (size accumulated;
// caller marks the thread excluded from the suspend pass). Returns false iff
// the snapshot was DECLINED at use time — caller MUST fall back to the
// SIGUSR2 suspend()/getRegisters()/resume() path for this thread (the
// pre-T5-optimisation behaviour; correctness-equivalent).
SUPPRESS_ASAN
bool MachineThreads::tryCopyCooperativelyParkedThreadStack(Thread& thread, CurrentThreadState& snapshot, void* buffer, size_t capacity, size_t* size)
{
    // SCAN-GCATEND-PARKEDSTACK / CVE-AUDIT B9 / SCAN-RESULTS.md residual #2
    // (gcAtEnd × property-wait-termination.js, sweeps 01b/04/06): consumer-
    // side re-validation of the cooperative snapshot AT USE TIME. The
    // publish-side bounds-check in GCClient::Heap::publishParkedRootSnapshot
    // (Heap.cpp, the A3-secondary fix) declines a snapshot whose
    // [stackTop, stackOrigin] falls outside the publishing thread's real
    // stack — that closes every case where stackTop is an ASAN
    // detect_stack_use_after_return fake-stack address AT PUBLISH TIME
    // (LockObject GILDroppedSection's `this`, DECLARE_AND_COMPUTE_CURRENT_
    // THREAD_STATE's `&stateName`). What it structurally CANNOT close is the
    // valid-at-publish, stale-at-use window: a snapshot that passed the
    // publish bounds (e.g. a real-stack frame) but whose backing
    // CurrentThreadState struct has since been popped/reused (watchdog-
    // terminated waiter at end-of-process gcAtEnd; load→dereference race
    // across a clear+republish under bounded-wait quanta; a publish frame
    // that escaped UAR instrumentation via inline-asm). The conductor's
    // seq_cst re-load (Heap.cpp coopParkedSnapshotLookup) returns a non-null
    // pointer; the dereferenced fields below are then garbage and the
    // (begin - end) span runs off the mapped stack — the
    // repro-gcAtEnd-property-wait-termination.log SEGV (≈142MB span,
    // page-aligned fault at the OS-stack guard).
    //
    // Decline-and-fall-back here, the SAME discipline as the publish-side
    // check, applied at the single consumer chokepoint: a snapshot whose
    // recorded span is not a sub-range of `thread.stack()` (the target
    // thread's REAL stack bounds, queried now) is unusable for the
    // cooperative copy. Return false; the caller leaves usedCoopSnapshot
    // clear and the suspend pass captures the real SP from the signal
    // context. SUPPRESS_ASAN above means dereferencing a poisoned/freed
    // fake-stack `snapshot` is not itself diagnosed; the bounds check then
    // catches the garbage values.
    //
    // PROTOCOL EXCEPTION (never-weaken-asserts rule): the prior Debug
    // ASSERT(begin == thread.stack().origin()) is subsumed by this runtime
    // check — same predicate, enforced in ALL builds as a correctness-
    // preserving fall-back rather than a process abort. The invariant is
    // not weakened: a violation is still detected, and the response (decline
    // → suspend) is the conservative-correct outcome the abort would have
    // been protecting. ASSERT_ENABLED-and-not-ASAN dataLog so an unexpected
    // non-ASAN out-of-bounds USE is loud in Debug (mirrors the publish-side
    // dataLog).
    //
    // Flag-off / non-shared: this function is only reached via the
    // [[unlikely]] coopParkedSnapshotLookup branch in tryCopyOtherThreadStacks
    // (gated on isSharedServer() in Heap::gatherStackRoots), so the flag-off
    // EXECUTION PATH is unchanged (this body never runs; the void→bool
    // signature change compiles unconditionally but is unreachable flag-off —
    // same convention as the publish-side precedent in Heap.cpp). Non-ASAN /
    // non-stale snapshots tautologically pass the bounds check, so the T5
    // cooperative-scan optimisation is undisturbed.
    //
    // TOCTOU discipline: the threat model above explicitly includes the
    // snapshot struct's BYTES mutating concurrently (clear+republish under
    // bounded-wait quanta; popped/reused frame). Latch every field of
    // `snapshot` into a local ONCE here, validate the LATCHED values, and use
    // ONLY the latched values for the copy below — never re-read `snapshot.*`
    // after this point. A second read could observe a value that was never
    // validated (off-stack span → the very SEGV this fix targets, or an
    // in-bounds-but-wrong span → silent under-scan / missed roots).
    //
    // Memory-ordering: in the well-behaved case the three field values are
    // visible via the seq_cst m_parkedRootSnapshot pointer load in
    // coopParkedSnapshotLookup (Heap.cpp) — the publisher writes the fields
    // then seq_cst-stores the pointer, the conductor seq_cst-loads the
    // pointer then reads the fields. In the stale window NO ordering is
    // relied upon: the bounds-check on the latched locals is the defense.
    const StackBounds& realStack = thread.stack();
    void* latchedStackOrigin = snapshot.stackOrigin;
    void* latchedStackTop = snapshot.stackTop;
    RegisterState* latchedRegisterState = snapshot.registerState;
    // `snapshot` MUST NOT be dereferenced past this line.

    if (latchedStackOrigin != realStack.origin()
        || latchedStackTop < realStack.end()
        || latchedStackTop > realStack.origin()) [[unlikely]] {
#if ASSERT_ENABLED && !ASAN_ENABLED
        dataLogLn("[SharedGC T5-rootscan] declining coop root snapshot AT USE TIME: stackTop ", RawPointer(latchedStackTop), " / stackOrigin ", RawPointer(latchedStackOrigin), " outside thread.stack() [", RawPointer(realStack.end()), ", ", RawPointer(realStack.origin()), "] — falling back to suspend()");
#endif
        return false;
    }

    // registerState bounds-check: in the stale-reused window stackOrigin is
    // the field MOST likely to survive (written once to the per-thread-
    // constant realStack.origin(), so a partially-overwritten struct often
    // still passes the `==` test above) while registerState may hold
    // arbitrary bytes. copyMemory()ing sizeof(RegisterState) from an
    // arbitrary address is a bounded-size unmapped-page SEGV at best, or a
    // few hundred bytes of arbitrary memory fed into the conservative-root
    // set at worst. The DECLARE_AND_COMPUTE_CURRENT_THREAD_STATE macro
    // ALLOCATE_AND_GET_REGISTER_STATEs the RegisterState on the publishing
    // thread's real stack and stores its address, so a non-null value that
    // does NOT lie wholly within [realStack.end(), realStack.origin()] is
    // garbage by construction — decline-and-fall-back exactly as for the
    // stack span. Null is the deliberate publish-side decline sentinel and
    // is accepted (registers contribute nothing; the suspend fallback is not
    // needed because the stack span itself validated).
    if (latchedRegisterState
        && (static_cast<void*>(latchedRegisterState) < realStack.end()
            || static_cast<void*>(latchedRegisterState + 1) > realStack.origin())) [[unlikely]] {
#if ASSERT_ENABLED && !ASAN_ENABLED
        dataLogLn("[SharedGC T5-rootscan] declining coop root snapshot AT USE TIME: registerState ", RawPointer(latchedRegisterState), " outside thread.stack() [", RawPointer(realStack.end()), ", ", RawPointer(realStack.origin()), "] — falling back to suspend()");
#endif
        return false;
    }

    void* registersBegin = latchedRegisterState;
    size_t registersSize = 0;
    if (registersBegin) {
        void* registersEnd = reinterpret_cast<void*>(WTF::roundUpToMultipleOf<sizeof(CPURegister)>(reinterpret_cast<uintptr_t>(latchedRegisterState + 1)));
        registersSize = static_cast<size_t>(static_cast<char*>(registersEnd) - static_cast<char*>(registersBegin));
    }

    char* begin = reinterpret_cast_ptr<char*>(latchedStackOrigin);
    char* end = std::bit_cast<char*>(WTF::roundUpToMultipleOf<sizeof(CPURegister)>(reinterpret_cast<uintptr_t>(latchedStackTop)));
    ASSERT(begin >= end);
    size_t stackSize = static_cast<size_t>(begin - end);

    bool canCopy = *size + registersSize + stackSize <= capacity;

    if (canCopy && registersSize)
        copyMemory(static_cast<char*>(buffer) + *size, registersBegin, registersSize);
    *size += registersSize;

    if (canCopy)
        copyMemory(static_cast<char*>(buffer) + *size, end, stackSize);
    *size += stackSize;
    return true;
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

bool MachineThreads::tryCopyOtherThreadStacks(const AbstractLocker& locker, void* buffer, size_t capacity, size_t* size, Thread& currentThreadForGC, const ScopedLambda<CurrentThreadState*(Thread&)>* coopParkedSnapshotLookup)
{
    // Prevent two VMs from suspending each other's threads at the same time,
    // which can cause deadlock: <rdar://problem/20300842>.
    static Lock suspendLock;
    Locker suspendLocker { suspendLock };

    *size = 0;

    auto& currentThread = Thread::currentSingleton();
    const ListHashSet<Ref<Thread>>& threads = m_threadGroup->threads(locker);
    BitVector isSuspended(threads.size());

    // T5-rootscan-skip-coop-parked-suspend (SCALEBENCH §31, offcpu16 row #4):
    // 2.07% of W=16 parallel-phase mutator-time was the SIGUSR2
    // signalHandlerSuspendResume sem_wait, and 25/52 of those victims were
    // ALREADY cooperatively parked (drainFromShared / notifyVMStop /
    // acquireHeapAccess) — wasted suspend/resume round-trips on threads whose
    // conservative roots are stable. When the shared-server Heap supplies a
    // coop-parked snapshot lookup, copy those threads' published
    // RegisterState + [stackTop, stackOrigin] directly here and exclude them
    // from the suspend/resume loops below. Runs BEFORE any thread is
    // suspended; uses copyMemory only (no malloc/locks). The lookup itself
    // is a HashMap probe over a table built world-stopped under the (frozen,
    // I13) HeapClientSet — no locks taken inside the lambda.
    // Flag-off / non-shared (`coopParkedSnapshotLookup == nullptr`): the
    // [[unlikely]] block is skipped, `usedCoopSnapshot` stays an empty
    // inline BitVector that is never read (the exclusion clause below
    // short-circuits on the same nullptr test), and the suspend path is
    // byte-for-byte the original.
    BitVector usedCoopSnapshot;
    if (coopParkedSnapshotLookup) [[unlikely]] {
        usedCoopSnapshot.ensureSize(threads.size());
        unsigned index = 0;
        for (const Ref<Thread>& thread : threads) {
            if (thread.ptr() != &currentThread
                && thread.ptr() != &currentThreadForGC) {
                if (CurrentThreadState* snapshot = (*coopParkedSnapshotLookup)(thread.get())) {
                    // SCAN-GCATEND-PARKEDSTACK: only exclude the thread from
                    // the suspend pass below if the cooperative copy ACCEPTED
                    // the snapshot. A use-time decline (stale/out-of-bounds
                    // span — see tryCopyCooperativelyParkedThreadStack above)
                    // leaves usedCoopSnapshot clear so this thread falls
                    // through to the SIGUSR2 suspend()/getRegisters() path.
                    if (tryCopyCooperativelyParkedThreadStack(thread.get(), *snapshot, buffer, capacity, size))
                        usedCoopSnapshot.set(index);
                }
            }
            ++index;
        }
    }

    {
        ThreadSuspendLocker threadSuspendLocker;
        {
            unsigned index = 0;
            for (const Ref<Thread>& thread : threads) {
                if (thread.ptr() != &currentThread
                    && thread.ptr() != &currentThreadForGC
                    && !(coopParkedSnapshotLookup && usedCoopSnapshot.get(index))) {
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
void MachineThreads::gatherConservativeRoots(ConservativeRoots& conservativeRoots, JITStubRoutineSet& jitStubRoutines, CodeBlockSet& codeBlocks, CurrentThreadState* currentThreadState, Thread* currentThread, const ScopedLambda<CurrentThreadState*(Thread&)>* coopParkedSnapshotLookup)
{
    if (currentThreadState)
        gatherFromCurrentThread(conservativeRoots, jitStubRoutines, codeBlocks, *currentThreadState);

    size_t size;
    size_t capacity = 0;
    void* buffer = nullptr;
    Locker locker { m_threadGroup->getLock() };
    while (!tryCopyOtherThreadStacks(locker, buffer, capacity, &size, *currentThread, coopParkedSnapshotLookup))
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
