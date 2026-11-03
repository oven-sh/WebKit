/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "JSLock.h"
#include "PyRuntime.h"
#include <wtf/Condition.h>
#include <wtf/Deque.h>
#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/ScopedLambda.h>
#include <wtf/ThreadSafeRefCounted.h>

namespace JSC {
namespace Python {

// Threads.
//
// A Python thread is a thread of the system. Only one of them at a time runs in the VM, or touches anything of it: the one that
// holds the global interpreter lock (GIL). There is one GIL for each VM. To hold it is to hold the VM's JSLock, with what the VM and
// the Runtime know about the code that is running being that of this thread: a thread that gives up the GIL takes that
// with it (JSLock::park(), Runtime::takeThreadLocalState()) and puts it back when it has the GIL again.
//
// The GIL changes hands
//   - where a thread is about to wait, or to do something long that needs nothing of the VM: AllowThreads;
//   - where a thread that has waited for the GIL for longer than sys.getswitchinterval() asks for it: that fires
//     VMTraps::NeedPythonCalls, and the running thread hands over at its next safe point (makeCallsOwed()).
//
// Until a program starts its first thread none of this happens: AllowThreads does nothing.
//
// The collector scans the stacks of all the threads (they have all held the JSLock: MachineThreads). It may run on any of them.

// What a thread sleeps on when it waits for another thread to do something.
class Sleeper {
    WTF_MAKE_NONCOPYABLE(Sleeper);
public:
    Sleeper() = default;

    // Until wake(), or the time on the monotonic clock, in nanoseconds (negative: there is none), or, perhaps, a signal.
    // A wake() that came before counts. Without the GIL.
    void sleep(int64_t deadline);
    // From any thread, and from signal handlers.
    void wake();

private:
#if OS(LINUX)
    std::atomic<uint32_t> m_isWoken { 0 };
#else
    Lock m_lock;
    Condition m_condition;
    std::atomic<bool> m_isWoken { false };
#endif
};

class ThreadState final : public ThreadSafeRefCounted<ThreadState> {
public:
    static Ref<ThreadState> create() { return adoptRef(*new ThreadState); }

    uint64_t ident { 0 }; // threading.get_ident(). The system gives it to another thread once this one has ended.
    const uint64_t serial; // This is never used again. Not 0.
    bool isMain { false };
    Sleeper sleeper;

    // The rest is for whoever holds the GIL.

    // While the thread does not hold the GIL: where it left its frames, and what it took out of the runtimes.
    JSLock::ParkedThread* parked { nullptr };
    struct SavedStates* saved { nullptr };
    // Whether it is in a queue of Threads::waitUntil().
    bool isQueued { false };
    // See Threads::willCallFunctionThatMayWait().
    CallFrame* callThatMayBeginAgain { nullptr };
    bool wantsCallToBeginAgain { false };

private:
    ThreadState();
};

// There may be several runtimes in a VM (one for each global object that runs Python), but hardly ever more than one.
struct SavedStates {
    static constexpr unsigned capacity = 4;
    struct Entry {
        Runtime* runtime { nullptr };
        Runtime::ThreadLocalState state;
    };
    std::array<Entry, capacity> entries;
};

// A lock among the threads of Python for the use of the runtime, held across points where the GIL may change hands. The thread
// that holds it may take it again. It must stay where it is in memory. All of this is for whoever holds the GIL.
class ThreadLock {
    WTF_MAKE_NONCOPYABLE(ThreadLock);
public:
    ThreadLock() = default;

    bool isHeld() const { return m_count; }
    bool isHeldByThisThread(JSGlobalObject*) const;
    // Waits for as long as it takes. False: an exception, which the handler of a signal raised, or because nobody can release it.
    bool acquire(JSGlobalObject*);
    void release(JSGlobalObject*);

private:
    uint64_t m_owner { 0 }; // ThreadState::serial
    unsigned m_count { 0 };
};

class Threads final : public ThreadSafeRefCounted<Threads> {
public:
    // That of the VM: the runtimes of a VM share it.
    static Ref<Threads> sharedFor(VM&);
    ~Threads();

    // A second thread has been started. It stays so.
    ALWAYS_INLINE bool isEnabled() const { return m_isEnabled; }
    // The thread that the first runtime of the VM was made on. Signal handlers run there, and JS.
    ALWAYS_INLINE bool isMainThread() const { return !m_isEnabled || isMainThreadSlow(); }
    uint64_t mainThreadIdent() const { return m_mainThreadIdent; }

    // The state of the current thread, which holds the GIL.
    ThreadState& current();
    // Any thread, and signal handlers: the main thread of the process, if it sleeps in waitUntil(), looks at its signals.
    static void wakeMainThreadOfProcess();
    // Any thread, and signal handlers: whether start() made it.
    static bool isOnStartedThread();

    // ---- Starting and ending threads ----

    // For the thread that starts another. An errno.
    int start(size_t stackSize, Function<void()>&& body, uint64_t& ident);
    // How many started threads have not ended.
    unsigned runningCount() const { return m_runningCount; }
    // Whether that is threading.get_ident() of a thread that has not ended. The functions of the system do not check.
    bool hasThread(uint64_t ident) const;

    // ---- The GIL ----

    bool dropIsRequested() const { return m_dropIsRequested.load(std::memory_order_relaxed); }
    // At a safe point, when that is so.
    void yield();
    int64_t switchInterval() const { return m_switchInterval.load(std::memory_order_relaxed); } // In nanoseconds.
    void setSwitchInterval(int64_t nanoseconds) { m_switchInterval.store(nanoseconds, std::memory_order_relaxed); }

    // The interpreter is over. A thread other than the main one that asks for the GIL from now on sleeps for ever.
    void didFinalize();
    bool isFinalized() const { return m_isFinalized.load(std::memory_order_relaxed); }

    // ---- Waiting for other threads ----

    // Waits until `isOver` says so, for so many nanoseconds (negative: for ever), without the GIL. `isOver` is called with it.
    // True: it did. False: time ran out, or there is an exception: on the main thread the handlers of signals run meanwhile.
    // Whoever brings about what is waited for calls wakeOne() or wakeAll() with the same address.
    // The texts are for the exceptions raised where nobody could: "deadlock: <the lock is held>, and there is no other thread",
    // and, once the interpreter is over, "cannot <acquire lock at interpreter finalization>".
    bool waitUntil(JSGlobalObject*, const void* address, int64_t timeout, ASCIILiteral whatIsWaitedFor, ASCIILiteral whatCannotBeDone, const ScopedLambda<bool()>& isOver);
    // A thread that waits collects after a while (collectWhileIdle()), to find what the program has dropped and what it may be
    // waiting for the finalizer of. The frames of C++ between the code in Python that waits and the collector lie where the frames
    // of earlier calls were, and what they do not write still refers to what those dealt with: the very objects that were
    // dropped last. So a native function that finds that it has to wait gives up (waitUntil() throws something that nobody but
    // its caller sees), its frames go, the stack below is wiped, and it is called again: NativeFlag::MayWait. Only if its
    // arguments are such that looking at them a second time runs no code of the program.
    void willCallFunctionThatMayWait(CallFrame*);
    bool takeRequestToCallAgain(JSGlobalObject*);
    // For such a function, when it finds that it has to wait and has done nothing yet that must not be done twice.
    // True: that has been thrown, and it is to return at once.
    bool beginsCallAgain(JSGlobalObject*);
    bool mayBeginCallAgain() { return !!current().callThatMayBeginAgain; }
    void wakeOne(const void* address);
    void wakeAll(const void* address);

    // _imp.acquire_lock()
    ThreadLock importLock;
    ThreadLock warningsLock; // _warnings._acquire_lock()

    // ---- The other threads, for whoever holds the GIL ----

    template<typename Functor>
    void forEachThread(const Functor& functor)
    {
        for (auto& thread : m_threads)
            functor(thread.get());
    }
    // Every cell that a thread that is not running took out of a runtime. (Its frames are to be had from JSLock.)
    template<typename Functor>
    void forEachCellSetAside(const Functor& functor)
    {
        auto consider = [&](JSValue value) {
            if (value && value.isCell())
                functor(value.asCell());
        };
        for (auto& thread : m_threads) {
            if (!thread->saved)
                continue;
            for (auto& entry : thread->saved->entries) {
                if (!entry.runtime)
                    continue;
                auto& state = entry.state;
                for (JSValue value : { state.currentContext, state.handledException, state.asyncGenFirstIter, state.asyncGenFinalizer, state.pendingKeywordNames, state.sendResult, state.exceptionGroupRest, state.thrownException, state.throwArguments, state.threadLocals, state.traceFunction, state.profileFunction })
                    consider(value);
                if (state.runningGenerator)
                    functor(std::bit_cast<JSCell*>(state.runningGenerator));
            }
        }
    }

private:
    friend class AllowThreads;
    friend class IdleWait;

    explicit Threads(VM&);
    JS_EXPORT_PRIVATE bool isMainThreadSlow() const;
    void enable();
    struct Start;
    static void* threadEntry(void*);
    void runThread(ThreadState&, Function<void()>&& body);

    void release(ThreadState&, JSLock::ParkedThread&, SavedStates&);
    void acquire(ThreadState&, JSLock::ParkedThread&, SavedStates&);
    void collectWhileIdle(JSGlobalObject*, bool isLastResort = false);
    void takeGIL(ThreadState&);
    void dropGIL(ThreadState&, bool isYielding);
    [[noreturn]] void sleepForEver();
    void didTakeGIL(ThreadState&);
    void tellMainThreadOfFinalizers();
    void wakeMainThread();
    friend void mainThreadHasTrapsToHandle(VM&);

    VM& m_vm; // Not to be touched without the GIL, except for its traps.
    bool m_isEnabled { false };
    uint64_t m_mainThreadIdent { 0 };
    unsigned m_runningCount { 0 };
    Vector<Ref<ThreadState>> m_threads;
    UncheckedKeyHashMap<const void*, Deque<ThreadState*>> m_waiters;
    uint64_t m_bytesAllocatedAtIdleCollection { 0 };
    int64_t m_timeForIdleCollectionWithoutCause { 0 };
    int64_t m_durationOfLastIdleCollection { 0 };
    bool m_didTellMainThreadOfFinalizers { false };

    Lock m_gilLock;
    Condition m_gilCondition; // It is free.
    Condition m_switchCondition; // It has changed hands.
    bool m_gilIsHeld WTF_GUARDED_BY_LOCK(m_gilLock) { false };
    ThreadState* m_lastHolder WTF_GUARDED_BY_LOCK(m_gilLock) { nullptr };
    uint64_t m_switchNumber WTF_GUARDED_BY_LOCK(m_gilLock) { 0 };
    unsigned m_waitingForGIL WTF_GUARDED_BY_LOCK(m_gilLock) { 0 };
    std::atomic<bool> m_dropIsRequested { false };
    std::atomic<bool> m_isFinalized { false };
    std::atomic<int64_t> m_switchInterval { 5000000 };
};

// While one of these exists, the thread does not hold the GIL, and other threads run. It must not touch the VM: no cell is read
// or written (memory that a cell on this thread's stack keeps alive may be), nothing is allocated in the heap, no String that
// may be an atom is created or destroyed. errno is as it was when this ends.
class AllowThreads {
    WTF_MAKE_NONCOPYABLE(AllowThreads);
    WTF_FORBID_HEAP_ALLOCATION;
public:
    ALWAYS_INLINE explicit AllowThreads(JSGlobalObject* globalObject)
    {
        Runtime& rt = runtime(globalObject);
        Threads& threads = rt.threads();
        // With no other thread there is nobody to let in, and nobody to start one.
        if (threads.runningCount() && !rt.buffersThatKeepGIL) [[unlikely]]
            release(threads);
    }

    ALWAYS_INLINE ~AllowThreads()
    {
        if (m_threads) [[unlikely]]
            acquire();
    }

private:
    JS_EXPORT_PRIVATE void release(Threads&);
    JS_EXPORT_PRIVATE void acquire();

    struct Luggage {
        JSLock::ParkedThread parked;
        SavedStates saved;
    };

    Threads* m_threads { nullptr };
    ThreadState* m_thread { nullptr };
    // Made only if there is something to put in it.
    alignas(Luggage) std::byte m_luggage[sizeof(Luggage)];
};

// A thread that waits collects (Threads::collectWhileIdle()), also if it is the kernel that it waits for. So a system call that may
// take long is made piece by piece: sleep, wait until a descriptor is ready. With the GIL.
class IdleWait {
public:
    static constexpr int64_t firstInterval = 50000000;

    explicit IdleWait(JSGlobalObject*);
    // For how many nanoseconds to wait next, at most, if so many are left to wait (negative: there is no end).
    int64_t nextPiece(int64_t remaining) const;
    // That is over, what was waited for has not come, and so many nanoseconds are left to wait (negative: there is no end).
    // False: there is an exception.
    bool didWait(int64_t remaining);

private:
    JSGlobalObject* m_globalObject;
    int64_t m_interval { firstInterval };
    int64_t m_timeToCollect;
};

// False, with a RuntimeError: this is not the main thread, and what is about to happen would run code of JS, or of the host.
bool ensureMainThreadForJS(JSGlobalObject*);

} // namespace Python
} // namespace JSC
