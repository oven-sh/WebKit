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
#include <wtf/text/CString.h>

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
    // For Runtime::currentThread().
    uint32_t number() const { return isMain ? 0 : static_cast<uint32_t>(serial); }
    Sleeper sleeper;
    // See Threads::interrupt(). Written with the lock of that.
    std::atomic<bool> isBeingInterrupted { false };
    uint64_t timesInterrupted { 0 };

    // The rest is for whoever holds the GIL.

    // While the thread does not hold the GIL: where it left its frames, and what it took out of the runtimes.
    JSLock::ParkedThread* parked { nullptr };
    bool losesNothingIfInterrupted { false }; // While it is parked. See AllowThreads::WhenInterrupted.
    struct SavedStates* saved { nullptr };
    // Whether it is in a queue of Threads::waitUntil().
    bool isQueued { false };
    // See Threads::willCallFunctionThatMayWait(). The wait of that call takes it (IdleWait), so whatever else waits finds nothing.
    CallFrame* callThatMayBeginAgain { nullptr };
    // What the wait of a call that is begun again has come to. No call: there is none.
    struct {
        CallFrame* call { nullptr };
        int64_t deadline { 0 };
        int64_t idleInterval { 0 };
        bool isRequested { false }; // Thrown, and not yet caught by whoever called the function.
        bool collects { false };
        bool isLastResort { false };
    } waitBegunAgain;

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
    // Another thread has changed the current directory. The main one tells the host before it does anything else.
    void mainThreadTellsHostOfDirectory() { m_mainThreadTellsHostOfDirectory = true; }
    // The same for a variable of the environment. The name is a C string.
    void mainThreadTellsHostOfVariable(const Vector<char>& name) { m_variablesToTellHostOf.append(name); }
    // Where the environment of the host differs from that of the process: what the variable is to the host, null if it has none.
    // Bytes, whatever they encode. See PosixChildEnvironment. With the GIL.
    HashMap<Latin1CString, Latin1CString>& variablesOfHost() { return m_variablesOfHost; }

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

    // The interpreter is over. A thread other than the main one that asks for the GIL from now on sleeps: for ever, if the process
    // ends with the interpreter.
    void didFinalize();
    bool isFinalized() const { return m_isFinalized.load(std::memory_order_relaxed); }

    // The VM is about to go, and the process goes on: a worker of the host ends. A thread that sleeps, or sits in a system call, has
    // frames that refer to the VM and may have given the kernel memory that dies with it. So the threads end first. Each is woken
    // wherever it waits, gets the GIL as ever, and at its next safe point (throwIfStopping()) meets what ends the code of a worker that
    // is terminated: an exception that nothing catches, so no `finally` and no `__exit__` runs, while the frames of C++ unwind as
    // they do for any exception. For the main thread, with the GIL. When this returns there is no other thread.
    void stopAll();
    // For a thread that holds the GIL, where it may throw. True: it is to end, and that has been thrown.
    ALWAYS_INLINE bool throwIfStopping() { return m_isStopping.load(std::memory_order_relaxed) && throwIfStoppingSlow(); }

    // ---- Waiting for other threads ----

    // Waits until `isOver` says so, for so many nanoseconds (negative: for ever), without the GIL. `isOver` is called with it.
    // True: it did. False: time ran out, or there is an exception: on the main thread the handlers of signals run meanwhile.
    // Whoever brings about what is waited for calls wakeOne() or wakeAll() with the same address.
    // The texts are for the exceptions raised where nobody could: "deadlock: <the lock is held>, and there is no other thread",
    // and, once the interpreter is over, "cannot <acquire lock at interpreter finalization>".
    // `isWaitOfCall`: see IdleWait.
    bool waitUntil(JSGlobalObject*, const void* address, int64_t timeout, ASCIILiteral whatIsWaitedFor, ASCIILiteral whatCannotBeDone, const ScopedLambda<bool()>& isOver, bool isWaitOfCall = false);
    // A thread that waits collects after a while (collectWhileIdle()), to find what the program has dropped and what it may be
    // waiting for the finalizer of. The collector takes every word of the stack that looks like a reference for one, and the frames
    // of C++ between the code in Python that waits and the collector are full of such words: what earlier calls left where they
    // lie now and they have not written over, and what they have written themselves (a slot that held the address of a cell,
    // with a bool stored over its last byte, is the address of a neighbour of the cell). So the collection is not made from
    // there. A native function whose wait has come to the point (NativeFlag::MayWait) gives up: IdleWait notes how far the wait
    // has come and throws something that nobody but the caller of the function sees, its frames go, the stack below is wiped,
    // collectBeforeCallBeginsAgain() collects, and the function is called again. Its wait goes on where it was.
    // The collections of other threads look at the stack of this one too. So a wait that is about to block gives up once in the same
    // way, without a collection: for as long as it lasts its frames lie on wiped stack.
    // That takes arguments such that looking at them again runs no code of the program. False: they are not. Then the stack is wiped
    // before the call, whether it will wait or not, and the collection is made from inside it, which is second best.
    bool willCallFunctionThatMayWait(CallFrame*);
    // For whoever called the function, which has returned with an exception. If it is that one, it is gone.
    enum class CallAgain : uint8_t { No, OnWipedStack, AfterCollecting };
    CallAgain takeRequestToCallAgain(CallFrame*);
    // False: there is an exception, and the call does not begin again.
    bool collectBeforeCallBeginsAgain(JSGlobalObject*, CallFrame*);
    void wakeOne(const void* address);
    void wakeAll(const void* address);
    // The thread of that number (ThreadState::number()), which does not hold the GIL, owes calls: finalizers, callbacks of weak
    // references. Wherever it waits, it is woken, makes them (checkSignals()) and goes on waiting for the time it has left. If it only waits
    // for the GIL, it finds out when it has it. With the GIL, or for the collector while whoever holds it is stopped.
    void wakeToMakeCalls(uint32_t number);

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

#if !OS(WINDOWS)
    // Gets a thread out of the system call for which it gave up the GIL. A signal that comes just before the call begins does
    // nothing, so it is sent again and again, as VMTraps does and from the same queue, until the thread is back.
    void interrupt(ThreadState&);
    void sendInterrupt(ThreadState&) WTF_REQUIRES_LOCK(m_interruptLock);
    void interruptAgainAfter(Seconds, ThreadState&) WTF_REQUIRES_LOCK(m_interruptLock);
    void stopInterrupting(ThreadState&);
#endif

    void release(ThreadState&, JSLock::ParkedThread&, SavedStates&);
    void acquire(ThreadState&, JSLock::ParkedThread&, SavedStates&);
    bool shouldCollectWhileIdle(JSGlobalObject*, bool isLastResort);
    // True: it found something to finalize, for whichever thread. Then the next may find what only that referred to.
    bool collectWhileIdle(JSGlobalObject*, bool isLastResort);
    void takeGIL(ThreadState&);
    void dropGIL(ThreadState&, bool isYielding);
    JS_EXPORT_PRIVATE bool throwIfStoppingSlow();
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
    bool m_lastIdleCollectionFoundCalls { false };
    bool m_didTellMainThreadOfFinalizers { false };
    bool m_mainThreadTellsHostOfDirectory { false };
    Vector<Vector<char>> m_variablesToTellHostOf;
    HashMap<Latin1CString, Latin1CString> m_variablesOfHost;

    Lock m_gilLock;
    Condition m_gilCondition; // It is free.
    Condition m_switchCondition; // It has changed hands.
    Condition m_stopCondition; // stopAll() has begun.
    bool m_gilIsHeld WTF_GUARDED_BY_LOCK(m_gilLock) { false };
    ThreadState* m_lastHolder WTF_GUARDED_BY_LOCK(m_gilLock) { nullptr };
    uint64_t m_switchNumber WTF_GUARDED_BY_LOCK(m_gilLock) { 0 };
    unsigned m_waitingForGIL WTF_GUARDED_BY_LOCK(m_gilLock) { 0 };
#if !OS(WINDOWS)
    Lock m_interruptLock; // Held while a signal is sent.
#endif
    std::atomic<bool> m_dropIsRequested { false };
    std::atomic<bool> m_isFinalized { false };
    std::atomic<bool> m_isStopping { false };
    std::atomic<int64_t> m_switchInterval { 5000000 };
};

// While one of these exists, the thread does not hold the GIL, and other threads run. It must not touch the VM: no cell is read
// or written (memory that a cell on this thread's stack keeps alive may be), nothing is allocated in the heap, no String that
// may be an atom is created or destroyed. errno is as it was when this ends.
class AllowThreads {
    WTF_MAKE_NONCOPYABLE(AllowThreads);
    WTF_FORBID_HEAP_ALLOCATION;
public:
    // What a signal of ours would do to what the thread is about to do (Threads::wakeToMakeCalls() sends one, to a thread that owes
    // calls). A system call that has transferred a part returns that part: write, send, sendfile, recv with MSG_WAITALL come back
    // short, where a program that trusts a blocking call never looks. A time limit that the kernel keeps begins anew. pause() returns.
    // A foreign function does who knows what. So none is sent unless whoever gives up the GIL says that nothing is lost: the call
    // transfers nothing before it returns, and what follows it makes it again when it was interrupted.
    enum WhenInterrupted : bool { MayLoseSomething, LosesNothing };

    ALWAYS_INLINE explicit AllowThreads(JSGlobalObject* globalObject, WhenInterrupted whenInterrupted = MayLoseSomething)
    {
        Runtime& rt = runtime(globalObject);
        Threads& threads = rt.threads();
        // With no other thread there is nobody to let in, and nobody to start one.
        if (threads.runningCount() && !rt.buffersThatKeepGIL) [[unlikely]]
            release(threads, whenInterrupted);
    }

    ALWAYS_INLINE ~AllowThreads()
    {
        if (m_threads) [[unlikely]]
            acquire();
    }

private:
    JS_EXPORT_PRIVATE void release(Threads&, WhenInterrupted);
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

// A thread that waits collects (Threads::collectWhileIdle()), whether it waits for another thread (Threads::waitUntil()) or for the
// kernel. So a system call that may take long is made piece by piece: sleep, wait until a descriptor is ready. With the GIL.
class IdleWait {
public:
    static constexpr int64_t firstInterval = 50000000;

    // `deadline` is the caller's own, and means to this class what it means to the caller. If the call has been begun again in the
    // middle of this wait, it is set to what it was.
    // `isWaitOfCall`: this is the wait of the native function that is being called, the one place where it waits, and it has done
    // nothing yet that must not be done twice. Only such a wait has the call begun again
    // (Threads::willCallFunctionThatMayWait()). Not what the function calls on its way, not the locks of the interpreter, not
    // what runs while it waits: handlers of signals, finalizers, the loop of the host. None of these has a frame that would tell
    // it from the function, if it is written in C++.
    IdleWait(JSGlobalObject*, int64_t& deadline, bool isWaitOfCall);
    bool continuesWait() const { return m_continuesWait; }
    // For a caller that knows its deadline only after it has made this.
    void setDeadline(int64_t deadline) { m_deadline = deadline; }

    bool mayBeginCallAgain() const { return m_call; }
    // For a function that has found a reason why this call must not begin again after all.
    void mustNotBeginCallAgain() { m_call = nullptr; }
    // The wait is about to block, for the first time. False: there is an exception (the call begins again).
    bool willBlock();

    int64_t timeToCollect() const { return m_timeToCollect; }
    // For how many nanoseconds to wait next, at most, if so many are left to wait (negative: there is no end).
    int64_t nextPiece(int64_t remaining) const;
    // That is over, what was waited for has not come, and so many nanoseconds are left to wait (negative: there is no end).
    // False, here and below: there is an exception.
    bool didWait(int64_t remaining);
    // The two halves of that. Signals, if this is the main thread; the calls that the thread owes; the end of the VM. What the
    // thread has finalized may have been all that referred to something else, so it looks again soon.
    bool checkSignals();
    bool collectIfDue(int64_t remaining);
    // Nothing can end the wait but a finalizer. Once for each time that this is so: a call that has been begun again for it does not ask again.
    bool collectAsLastResort();
    void canBeEnded() { m_hasCollectedAsLastResort = false; }

private:
    bool collect(bool isLastResort);
    bool beginsCallAgain(bool collects, bool isLastResort);

    JSGlobalObject* m_globalObject;
    CallFrame* m_call { nullptr };
    int64_t m_deadline;
    int64_t m_interval { firstInterval };
    int64_t m_timeToCollect;
    bool m_continuesWait { false };
    bool m_hasCollectedAsLastResort { false };
};

// Threads::stopAll() sends it to the threads that Python started, which therefore cannot block it.
int signalThatInterruptsThreads();

// False, with a RuntimeError: this is not the main thread, and what is about to happen would run code of JS, or of the host.
bool ensureMainThreadForJS(JSGlobalObject*);

} // namespace Python
} // namespace JSC
