/*
 * Copyright (C) 2008-2025 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1.  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 * 2.  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 * 3.  Neither the name of Apple Inc. ("Apple") nor the names of
 *     its contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE AND ITS CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL APPLE OR ITS CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "CLoopStack.h"

#if ENABLE(C_LOOP)

#include "CLoopStackInlines.h"
#include "ConservativeRoots.h"
#include "Interpreter.h"
#include "JSCInlines.h"
#include "Options.h"
#include <wtf/Lock.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Threading.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

static size_t committedBytesCount = 0;

static size_t commitSize()
{
    static size_t size = std::max<size_t>(16 * 1024, pageSize());
    return size;
}

static Lock stackStatisticsMutex;

thread_local CLoopStack::CurrentThreadStateCache CLoopStack::s_currentThreadStateCache;
std::atomic<uint64_t> CLoopStack::s_stackEpoch { 0 };

// Process-wide registry of live CLoopStacks so the thread-exit hook below can
// retire a dying thread's segments without dangling into a destroyed VM.
// Lock order: s_liveStacksLock -> CLoopStack::m_lock.
static Lock s_liveStacksLock;
static Vector<CLoopStack*>& liveStacks() WTF_REQUIRES_LOCK(s_liveStacksLock)
{
    static NeverDestroyed<Vector<CLoopStack*>> stacks;
    return stacks.get();
}

// A thread that ever acquired a stack segment arms this thread-local; its
// destructor runs at OS thread exit and marks the thread's segments (in every
// still-live CLoopStack) as retired so later threads reuse them instead of
// each spawned thread leaking a PageReservation.
struct CLoopStackThreadExitRetirer {
    uint32_t threadUid { 0 };

    ~CLoopStackThreadExitRetirer()
    {
        if (!threadUid)
            return;
        Locker locker { s_liveStacksLock };
        for (CLoopStack* stack : liveStacks())
            stack->retireSegmentsForThread(threadUid);
    }
};
static thread_local CLoopStackThreadExitRetirer t_threadExitRetirer;

CLoopStack::CLoopStack()
    : m_softReservedZoneSizeInRegisters(0)
{
    {
        Locker locker { s_liveStacksLock };
        liveStacks().append(this);
    }

    // Eagerly create the constructing thread's segment. This also publishes
    // the initial StackManager::m_cloopStackLimit so the asm vm-entry check
    // never compares against a null limit before the first lazy lookup.
    // Note: this relies on StackManager's m_mirrors being initialized before
    // its m_cloopStack member (see the comment in StackManager.h).
    //
    // Pre-registration window: for a per-lite embedded instance (VMTraps
    // inside VMLite::threadContext), this constructor runs before
    // VMLiteRegistry::registerLite/setCurrent, so the ctor-time publish
    // routed through publishTargetStackManager() sees the constructing
    // thread's PREVIOUS current lite (or none). If that previous lite is
    // gilOff, its published limit briefly points into this new instance's
    // reservation. This is benign and self-corrects on the previous lite's
    // next publish hook: CLoop::execute entry calls
    // cloopStack.currentStackPointer() -> publishStackLimit(), and every
    // native/slow-path call republishes via setCurrentStackPointer().
    //
    // Do NOT touch vm() here: for a per-lite embedded instance,
    // VMTraps::vm() in this window falls back to `this - VM::offsetOfTraps()`
    // (see VMTrapsInlines.h), which is valid only for the VM-embedded
    // instance — any store through it would be a wild write. The historical
    // `vm().topCallFrame = nullptr;` that lived here was redundant anyway:
    // VM::topCallFrame is brace-initialized in-class (VM.h, via
    // FOR_EACH_VMLITE_PRIMITIVE_FIELD).
    threadState();
}

CLoopStack::~CLoopStack()
{
    {
        Locker locker { s_liveStacksLock };
        auto& stacks = liveStacks();
        stacks.removeFirst(this);
    }

    {
        Locker locker { m_lock };
        for (auto& state : m_threadStates) {
            ptrdiff_t sizeToDecommit = reinterpret_cast<char*>(highAddress(*state)) - reinterpret_cast<char*>(state->commitTop);
            if (sizeToDecommit) {
                state->reservation.decommit(state->commitTop, sizeToDecommit);
                addToCommittedByteCount(-sizeToDecommit);
            }
            state->reservation.deallocate();
        }
        m_threadStates.clear();
    }

    // Invalidate every thread's cached (stack, segment) pair: a new CLoopStack
    // allocated at this address must not validate a stale cache entry.
    s_stackEpoch.fetch_add(1, std::memory_order_relaxed);
    auto& cache = s_currentThreadStateCache;
    if (cache.stack == this)
        cache = { };
}

CLoopStack::ThreadState& CLoopStack::threadStateSlow() const
{
    uint32_t threadUid = Thread::currentSingleton().uid();
    ASSERT(threadUid);
    ThreadState* result = nullptr;
    {
        Locker locker { m_lock };

        ThreadState* reusable = nullptr;
        for (auto& state : m_threadStates) {
            if (!state->retired && state->threadUid == threadUid) {
                result = state.get();
                break;
            }
            if (!reusable && state->retired)
                reusable = state.get();
        }

        if (!result) {
            if (reusable) {
                // Reuse a retired (dead) thread's segment; it was reset to
                // pristine (empty, fully decommitted above the reserved zone)
                // at retirement.
                reusable->retired = false;
                reusable->threadUid = threadUid;
                result = reusable;
            } else {
                size_t capacity = Options::maxPerThreadStackUsage();
                capacity = WTF::roundUpToMultipleOf(pageSize(), capacity);
                ASSERT(capacity && isPageAligned(capacity));

                auto state = makeUnique<ThreadState>();
                state->threadUid = threadUid;
                state->reservation = PageReservation::reserve(WTF::roundUpToMultipleOf(commitSize(), capacity), OSAllocator::UnknownUsage);

                Register* bottomOfStack = highAddress(*state);
                state->end = bottomOfStack;
                state->commitTop = bottomOfStack;
                state->lastStackPointer = bottomOfStack;
                state->currentStackPointer = bottomOfStack;

                result = state.get();
                m_threadStates.append(WTF::move(state));
            }

            // Match setSoftReservedZoneSize() for segments created after the
            // zone was configured: commit the reserved zone right away.
            if (m_softReservedZoneSizeInRegisters && result->commitTop > result->end - m_softReservedZoneSizeInRegisters)
                grow(*result, result->end);
        }
    }

    // Arm segment retirement for this thread (idempotent).
    t_threadExitRetirer.threadUid = threadUid;

    auto& cache = s_currentThreadStateCache;
    cache.stack = this;
    cache.state = result;
    cache.epoch = s_stackEpoch.load(std::memory_order_relaxed);

    // Every caller of threadStateSlow() runs on the thread that owns (or is
    // creating) this segment, so a cache miss is exactly a thread/VM switch:
    // republish this segment's limit for the asm fast-path stack checks.
    publishStackLimit(*result);

    return *result;
}

CLoopStack::ThreadState* CLoopStack::threadStateIfExists() const
{
    auto& cache = s_currentThreadStateCache;
    if (cache.stack == this && cache.epoch == s_stackEpoch.load(std::memory_order_relaxed))
        return cache.state;

    uint32_t threadUid = Thread::currentSingleton().uid();
    Locker locker { m_lock };
    for (auto& state : m_threadStates) {
        if (!state->retired && state->threadUid == threadUid)
            return state.get();
    }
    return nullptr;
}

void CLoopStack::retireSegmentsForThread(uint32_t threadUid)
{
    Locker locker { m_lock };
    for (auto& state : m_threadStates) {
        if (state->retired || state->threadUid != threadUid)
            continue;

        // The owning thread is exiting: its interpreter frames are dead.
        // Reset the segment to pristine (and decommit its memory) so the GC
        // scans an empty range and a later thread can reuse it.
        Register* bottomOfStack = highAddress(*state);
        ptrdiff_t sizeToDecommit = reinterpret_cast<char*>(bottomOfStack) - reinterpret_cast<char*>(state->commitTop);
        if (sizeToDecommit) {
            state->reservation.decommit(state->commitTop, sizeToDecommit);
            addToCommittedByteCount(-sizeToDecommit);
        }
        state->commitTop = bottomOfStack;
        state->end = bottomOfStack;
        state->lastStackPointer = bottomOfStack;
        state->currentStackPointer = bottomOfStack;
        state->threadUid = 0;
        state->retired = true;
    }
}

void* CLoopStack::currentStackPointer() const
{
    ThreadState& state = threadState();
    // This is called at CLoop::execute entry (sp = currentStackPointer()), so
    // republishing here covers every fresh interpreter (re)entry after a GIL
    // handoff — and, for a gilOff lite thread, it is the publish that is
    // sequenced-before the first asm doVMEntry read of the lite slot, so a
    // fresh thread's first stack check never compares against a null limit.
    // See the class comment in CLoopStack.h.
    publishStackLimit(state);
    return state.currentStackPointer;
}

size_t CLoopStack::size() const
{
    ThreadState& state = threadState();
    return highAddress(state) - lowAddress(state);
}

bool CLoopStack::containsAddress(Register* address)
{
    Locker locker { m_lock };
    for (auto& state : m_threadStates) {
        if (state->retired)
            continue;
        if (lowAddress(*state) <= address && address < highAddress(*state))
            return true;
    }
    return false;
}

bool CLoopStack::grow(ThreadState& state, Register* newTopOfStack) const
{
    Register* newTopOfStackWithReservedZone = newTopOfStack - m_softReservedZoneSizeInRegisters;

    // If we have already committed enough memory to satisfy this request,
    // just update the end pointer and return. (The caller republishes the
    // stack limit.)
    if (newTopOfStackWithReservedZone >= state.commitTop) {
        state.end = newTopOfStack;
        return true;
    }

    // Compute the chunk size of additional memory to commit, and see if we
    // have it still within our budget. If not, we'll fail to grow and
    // return false.
    ptrdiff_t delta = reinterpret_cast<char*>(state.commitTop) - reinterpret_cast<char*>(newTopOfStackWithReservedZone);
    delta = WTF::roundUpToMultipleOf(commitSize(), delta);
    Register* newCommitTop = state.commitTop - (delta / sizeof(Register));
    if (newCommitTop < reservationTop(state))
        return false;

    // Otherwise, the growth is still within our budget. Commit it and return true.
    state.reservation.commit(newCommitTop, delta);
    addToCommittedByteCount(delta);
    state.commitTop = newCommitTop;
    state.end = state.commitTop + m_softReservedZoneSizeInRegisters;
    return true;
}

void CLoopStack::gatherConservativeRoots(ConservativeRoots& conservativeRoots, JITStubRoutineSet& jitStubRoutines, CodeBlockSet& codeBlocks)
{
    // SharedGC I12 ("one VM, N stacks"): scan every live thread's segment,
    // including parked threads', from the SP each thread last saved into its
    // own segment (the offlineasm cloop stores it before every native/slow-path
    // call, so it is current at every park site). Skipping parked segments
    // would silently unroot their live frames.
    Locker locker { m_lock };
    for (auto& state : m_threadStates) {
        if (state->retired)
            continue;
        void* low = state->currentStackPointer;
        void* high = highAddress(*state);
        ASSERT(low <= high);
        if (low == high)
            continue;
        conservativeRoots.add(low, high, jitStubRoutines, codeBlocks);
    }
}

void CLoopStack::sanitizeStack()
{
#if !ASAN_ENABLED
    // GIL-off (useVMLite N-mutator), other threads' segments are NOT stable:
    // sanitizeStack() is reached from ordinary mutator paths (e.g.
    // LocalAllocator::allocate -> sanitizeStackForVM), not from a
    // stop-the-world section, so another thread's interpreter is concurrently
    // moving its SP (setCurrentStackPointer, CLoopStackInlines.h) and writing
    // frames into the very [last, sp) window we would memset. Each thread
    // therefore sanitizes only its OWN segment — mirroring the !C_LOOP path,
    // where sanitizeStackForVMImpl only scrubs the calling thread's native
    // stack. Other segments' dead regions are scrubbed by their owners' next
    // sanitize call (or zeroed wholesale at retirement); missing a scrub is
    // only a conservative-GC precision loss, never a correctness issue.
    //
    // No m_lock needed: threadStateIfExists() is safe to call here (it hits
    // the calling thread's own thread-local cache, and takes m_lock itself on
    // a cache miss), and after that every field touched below is
    // owner-exclusive (sole writers are this thread's interpreter and, only
    // after this thread has exited, retireSegmentsForThread()).
    //
    // Flag-off (single mutator) this degenerates to exactly the old
    // single-segment body. With multiple parked threads (GIL-on) behavior is
    // observationally equivalent: parked segments' dead regions are now
    // scrubbed by their owners' next sanitize instead of by the caller; the
    // skipped region lies below each owner's SP, outside the range
    // gatherConservativeRoots() scans.
    ThreadState* state = threadStateIfExists();
    if (!state)
        return; // This thread never entered the CLoop on this stack; nothing of ours to scrub.
    void* stackTop = state->currentStackPointer;
    ASSERT(stackTop <= highAddress(*state));
    if (state->lastStackPointer < stackTop) {
        char* begin = reinterpret_cast<char*>(state->lastStackPointer);
        char* end = reinterpret_cast<char*>(stackTop);
        memset(begin, 0, end - begin);
    }
    state->lastStackPointer = stackTop;
#endif
}

void CLoopStack::addToCommittedByteCount(long byteCount)
{
    Locker locker { stackStatisticsMutex };
    ASSERT(static_cast<long>(committedBytesCount) + byteCount > -1);
    committedBytesCount += byteCount;
}

void CLoopStack::setSoftReservedZoneSize(size_t reservedZoneSize)
{
    m_softReservedZoneSizeInRegisters = reservedZoneSize / sizeof(Register);
    {
        Locker locker { m_lock };
        for (auto& state : m_threadStates) {
            if (state->retired)
                continue;
            if (state->commitTop > state->end - m_softReservedZoneSizeInRegisters)
                grow(*state, state->end);
        }
    }
    // Only the calling thread's segment limit may be republished: publishing a
    // parked thread's limit would break the running thread's asm stack checks.
    if (ThreadState* state = threadStateIfExists())
        publishStackLimit(*state);
}

bool CLoopStack::isSafeToRecurse() const
{
    ThreadState& state = threadState();
    void* reservationLimit = reinterpret_cast<int8_t*>(reservationTop(state) + m_softReservedZoneSizeInRegisters);

    // Under the phase-1 GIL, vm().topCallFrame can be a frame in another
    // thread's segment (it migrates with the GIL); a cross-segment pointer
    // comparison is meaningless, so fall back to this thread's own saved SP.
    void* position = nullptr;
    if (CallFrame* topCallFrame = vm().topCallFrame) {
        void* topOfFrame = topCallFrame->topOfFrame();
        if (topOfFrame >= static_cast<void*>(reservationTop(state)) && topOfFrame < static_cast<void*>(highAddress(state)))
            position = topOfFrame;
    }
    if (!position)
        position = state.currentStackPointer;
    return position > reservationLimit;
}

size_t CLoopStack::committedByteCount()
{
    Locker locker { stackStatisticsMutex };
    return committedBytesCount;
}

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(C_LOOP)
