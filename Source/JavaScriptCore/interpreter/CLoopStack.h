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

#pragma once

#include <wtf/Platform.h>

#if ENABLE(C_LOOP)

#include "Register.h"
#include <atomic>
#include <memory>
#include <wtf/FastMalloc.h>
#include <wtf/Lock.h>
#include <wtf/Noncopyable.h>
#include <wtf/PageReservation.h>
#include <wtf/Vector.h>

namespace JSC {

    class CodeBlockSet;
    class ConservativeRoots;
    class JITStubRoutineSet;
    class StackManager;
    class VM;
    class LLIntOffsetsExtractor;
    struct CLoopStackThreadExitRetirer;

    // Phase-1 GIL threads (docs/threads/TSAN.md, "Shared CLoopStack vs. parked
    // threads"): the CLoop used to keep every JS thread's interpreter frames on
    // one shared stack. A thread that parked (join, cond.wait, Atomics.wait,
    // blocked lock.hold) left its frames in that shared stack, and the next GIL
    // holder's interpreter SP walked below them and clobbered them; the parked
    // thread then crashed on resume (intermittent SEGV in CLoop::execute).
    //
    // Each JS thread now gets its own stack segment (ThreadState below), created
    // lazily on first use and keyed by WTF::Thread uid, so a parked thread's
    // frames live in a reservation no other thread ever touches. Segments are
    // retired at thread exit (thread-local destructor) and reused by later
    // threads, so the number of live PageReservations is bounded by the peak
    // number of concurrently live JS threads, not by the total spawned.
    //
    // The LLInt asm stack checks (LowLevelInterpreter64.asm:213/729 and the
    // prologue check via StackManager::m_trapAwareSoftStackLimit) compare sp
    // against the single published StackManager::m_cloopStackLimit slot. With
    // per-thread segments that slot must hold the *running* thread's limit, so
    // every in-scope hook that runs on the current thread (segment lookup,
    // setCurrentStackPointer before each native/slow-path call,
    // currentStackPointer at CLoop entry, ensureCapacityFor in the stack-check
    // slow paths) republishes the current segment's limit when it is stale.
    //
    // FIXME(threads): there is a residual window right after a GIL re-acquire at
    // a park site: until the resumed thread hits one of the hooks above, asm
    // stack checks compare against the previous holder's limit. A wrongly
    // failing check self-corrects (slow path -> ensureCapacityFor republishes);
    // a wrongly passing check can only hurt if the thread then recurses through
    // the rest of its committed region (>= the soft reserved zone) with no
    // native/slow-path call at all before any check fails. The complete fix is
    // a republish hook in the GIL re-acquire path (GILDroppedSection's
    // destructor, runtime/LockObject.cpp), which is outside this fix item's
    // file scope; see docs/threads/INTEGRATE-api.md landed deviations.
    class CLoopStack {
        WTF_MAKE_NONCOPYABLE(CLoopStack);
    public:
        // Allow 8k of excess registers before we start trying to reap the stack
        static constexpr ptrdiff_t maxExcessCapacity = 8 * 1024;

        CLoopStack();
        ~CLoopStack();

        bool ensureCapacityFor(Register* newTopOfStack);

        bool containsAddress(Register* address);
        static size_t committedByteCount();

        void gatherConservativeRoots(ConservativeRoots&, JITStubRoutineSet&, CodeBlockSet&);
        void sanitizeStack();

        // One might be tempted to assert that the returned pointer is
        // <= m_topCallFrame->topOfFrame(). That assertion would be incorrect
        // because this function may be called from function prologues (e.g.
        // during a stack check) where the stack pointer has not been
        // initialized to point to frame top yet.
        //
        // Reads (and lazily creates) the calling thread's segment; out-of-line
        // so it can also republish the stack limit at CLoop (re)entry.
        void* currentStackPointer() const;
        void setCurrentStackPointer(void* sp); // Defined in CLoopStackInlines.h.

        size_t size() const;

        void setSoftReservedZoneSize(size_t);
        bool isSafeToRecurse() const;

    private:
        // Per-thread stack segment. The following is always true per segment:
        //    reservationTop() <= commitTop <= end <= currentStackPointer <= highAddress()
        struct ThreadState {
            WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(ThreadState);

            uint32_t threadUid { 0 };
            bool retired { false };
            Register* end { nullptr }; // lowest address of JS allocatable stack memory.
            Register* commitTop { nullptr }; // lowest address of committed memory.
            PageReservation reservation;
            void* lastStackPointer { nullptr };
            void* currentStackPointer { nullptr };
        };

        // Per-thread cache of the last (CLoopStack, segment) pair this thread
        // used, so the interpreter hot paths (setCurrentStackPointer before
        // every native/slow-path call) stay lock-free after the first lookup.
        // s_stackEpoch is bumped by ~CLoopStack so a destroyed stack reallocated
        // at the same address can never validate a stale cache entry.
        struct CurrentThreadStateCache {
            const CLoopStack* stack { nullptr };
            ThreadState* state { nullptr };
            uint64_t epoch { 0 };
        };
        static thread_local CurrentThreadStateCache s_currentThreadStateCache;
        static std::atomic<uint64_t> s_stackEpoch;

        StackManager& stackManager() const;
        // V7-1 (UNGIL sec A.2.2, C_LOOP arm): the StackManager whose
        // m_cloopStackLimit / m_trapAwareSoftStackLimit the RUNNING thread's
        // LLInt checks read. GIL-off threads on a gilOff lite use the lite's
        // own StackManager (the VMLiteCLoopStackLimitOffset reroute in the
        // asm stack checks); everyone else uses the containing carrier
        // StackManager, byte-identically to the flag-off protocol. The
        // discriminator mirrors gilOffGroup3Check byte-for-byte:
        // JSCConfig::gilOffProcess byte, then lite presence, then
        // lite->gilOff — deliberately NOT VM::gilOffWithProcessGate(),
        // whose extra owner-VM/pre-latch terms could diverge from the asm
        // side and silently split publish slot from read slot.
        StackManager& publishTargetStackManager() const;
        // Single republish hook used by all five C++ publish sites
        // (threadStateSlow, currentStackPointer, setSoftReservedZoneSize,
        // setCurrentStackPointer, ensureCapacityFor).
        void publishStackLimit(ThreadState&) const;
        VM& vm() const;

        ALWAYS_INLINE ThreadState& threadState() const
        {
            auto& cache = s_currentThreadStateCache;
            if (cache.stack == this && cache.epoch == s_stackEpoch.load(std::memory_order_relaxed)) [[likely]]
                return *cache.state;
            return threadStateSlow();
        }
        ThreadState& threadStateSlow() const; // Finds/reuses/creates the current thread's segment, caches it, and republishes the stack limit.
        ThreadState* threadStateIfExists() const; // Never creates; never publishes.

        void retireSegmentsForThread(uint32_t threadUid);

        static Register* lowAddress(const ThreadState& state)
        {
            return state.end;
        }

        WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

        static Register* highAddress(const ThreadState& state)
        {
            return reinterpret_cast_ptr<Register*>(static_cast<char*>(state.reservation.base()) + state.reservation.size());
        }

        WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

        static Register* reservationTop(const ThreadState& state)
        {
            char* reservationTop = static_cast<char*>(state.reservation.base());
            return reinterpret_cast_ptr<Register*>(reservationTop);
        }

        bool grow(ThreadState&, Register* newTopOfStack) const;
        static void addToCommittedByteCount(long);

        mutable Lock m_lock;
        mutable Vector<std::unique_ptr<ThreadState>, 4> m_threadStates WTF_GUARDED_BY_LOCK(m_lock);
        ptrdiff_t m_softReservedZoneSizeInRegisters;

        friend class LLIntOffsetsExtractor;
        friend struct CLoopStackThreadExitRetirer;
    };

} // namespace JSC

#endif // ENABLE(C_LOOP)
