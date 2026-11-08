/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "JSLock.h"
#include "PyRuntime.h"
#include <wtf/Condition.h>
#include <wtf/Deque.h>
#include <wtf/FixedVector.h>
#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/ScopedLambda.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/text/CString.h>

namespace JSC {

class CodeBlockSet;
class ConservativeRoots;
class JITStubRoutineSet;

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
//     VMTraps::NeedPythonCalls, and the running thread hands over at its next safe point (makeCallsOwed()). It does not have the GIL
//     back before one of those that were waiting has had it. Otherwise whoever finds the GIL free takes it.
//
// Until a program starts its first thread none of this happens: AllowThreads does nothing.
//
// The collector scans the stacks of all the threads (they have all held the JSLock: MachineThreads). It may run on any of them.

// What a thread sleeps on when it waits for another thread to do something.
class Sleeper {
    WTF_MAKE_NONCOPYABLE(Sleeper);
public:
    Sleeper() = default;

    // Until wake(), or the time on the monotonic clock, in nanoseconds (negative: there is none), or, perhaps, a signal: false then.
    // A wake() that came before counts. Without the GIL.
    bool sleep(int64_t deadline);
    // From any thread, and from signal handlers.
    void wake();
    // Whether there has been a wake() that no sleep() has answered. It counts as answered. Whoever takes one, here or in sleep(), sleeps no more before it has been back in the VM
    // (Threads::acquire()): ThreadState::wakesAnswered rests on that.
    bool takeWake() { return m_isWoken.exchange(0); }
    // How many times wake() has been called. Nothing takes this back. (What the wake is for has been written before.)
    uint64_t wakes() const { return m_wakes.load(); }

private:
    std::atomic<uint64_t> m_wakes { 0 };
#if OS(LINUX)
    std::atomic<uint32_t> m_isWoken { 0 };
#else
    Lock m_lock;
    Condition m_condition;
    std::atomic<bool> m_isWoken { false };
#endif
};

// A collection by a thread that has nothing better to do. See Threads::collectWhileIdle().
enum class IdleLook : uint8_t {
    WithinShare, // If the threads that wait have time left for it. It finalizes what is this thread's and reference counts would have freed.
    WhateverTheShare, // There is reason to think that it finds what the thread asks for. What reference counts would have freed, whosever it is.
    LastResort, // Nothing else can end the wait. Everything.
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

    // The rest is for whoever holds the GIL. (And, of what is said to hold while it is parked, for JSLock::ifEmbedderThreadWaits(). `saved` changes only with access to the heap as well,
    // so it is also for the collector once that has stopped the world.)

    // While the thread does not hold the GIL: where it left its frames, and what it took out of the runtimes.
    JSLock::ParkedThread* parked { nullptr };
    bool losesNothingIfInterrupted { false }; // While it is parked. See AllowThreads::WhenInterrupted.
    bool defersTermination { false }; // While it is parked.
    // It is in so many of them, one inside the other. See IdleWait.
    unsigned idleWaits { 0 };
    // Rather than works. (In any other system call, or in foreign code, it may be hard at work: compressing, hashing.)
    bool isWaiting() const { return idleWaits || (parked && losesNothingIfInterrupted); }
    // It is in Threads::collectWhileIdle(), in a finalizer that this has found perhaps: it goes on collecting until it finds no more of its own.
    bool collectsWhileIdle { false };
    struct SavedStates* saved { nullptr };
    // Whether it is in a queue of Threads::waitUntil().
    bool isQueued { false };
    // It sleeps in Threads::waitUntil(), without a deadline. If it is no longer in the queue, somebody has let it go on, and it is on its way.
    bool waitsForGood { false };
    // Up to which wake() of its sleeper it has come back (Threads::acquire()). If there has been one since, somebody has sent for it, whoever and whatever for, and it is on its way, too.
    uint64_t wakesAnswered { 0 };
    // It waits for the GIL and for nothing else, or has been made to give it up and will ask for it again: it runs as soon as it is let. With the lock of the GIL: see Threads::isAskingForGIL().
    bool isAskingForGIL { false };
    // See Threads::willCallFunctionThatMayWait(). The wait of that call takes it (IdleWait), so whatever else waits finds nothing.
    CallFrame* callThatMayBeginAgain { nullptr };
    // What the wait of a call that is begun again has come to. No call: there is none.
    struct {
        CallFrame* call { nullptr };
        int64_t deadline { 0 };
        int64_t idleInterval { 0 };
        bool isRequested { false }; // Thrown, and not yet caught by whoever called the function.
        bool collects { false };
        IdleLook look { IdleLook::WithinShare };
    } waitBegunAgain;
    // For how long it has waited since it last collected while idle, in waits between which it did little. See IdleWait.
    struct {
        int64_t total { 0 };
        int64_t endOfLast { 0 };
        int64_t lengthOfLast { 0 };
    } waits;

private:
    ThreadState();
};

// There may be several runtimes in a VM (one for each global object that runs Python), but hardly ever more than one.
PY_BEGIN_WITHOUT_PADDING
struct SavedStates {
    static constexpr unsigned capacity = 4;
    struct Entry {
        Runtime* runtime { nullptr }; // Null: the entry is not used, or the runtime has ended since (Threads::runtimeDidEnd()).
        Runtime::ThreadLocalState state;
    };
    std::array<Entry, capacity> entries;
    // Those of the runtimes beyond that, if there are any. They are not on the stack, so the collector is told where they are, and takes every word there for a
    // reference as well: Threads::gatherConservativeRoots().
    FixedVector<Entry> more;

    template<typename Functor>
    void forEachEntry(const Functor& functor)
    {
        for (auto& entry : entries)
            functor(entry);
        for (auto& entry : more)
            functor(entry);
    }
    Entry* entryOf(Runtime& runtime)
    {
        for (auto& entry : entries) {
            if (entry.runtime == &runtime)
                return &entry;
        }
        for (auto& entry : more) {
            if (entry.runtime == &runtime)
                return &entry;
        }
        return nullptr;
    }
};
PY_END_WITHOUT_PADDING

class Threads final : public ThreadSafeRefCounted<Threads> {
public:
    // That of the VM: the runtimes of a VM share it, those that come when the others have gone too. The heap holds it.
    static Ref<Threads> sharedFor(VM&);
    ~Threads();

    // A second thread has been started. It stays so.
    ALWAYS_INLINE bool isEnabled() const { return m_isEnabled; }
    // One of the runtimes has ended, be it in the collector's thread, which has stopped the world.
    void runtimeDidEnd(Runtime&);
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

    // ---- What a thread can tell of the others: see Runtime::ServesOtherThreads ----

    bool isAskingForGIL(const ThreadState&);
    // Every thread but this one waits for another thread (waitUntil()), without a deadline, and nobody has let it go on or sent for it. So none of them moves again unless this thread does something about it. One
    // that can move: it asks for the GIL; it has been started and has not run yet; it is in a system call or in foreign code; it sleeps; it waits with a deadline, or for descriptors, which
    // somebody outside the process may end; the main thread in the host's event loop.
    bool allOthersWaitForGood();
    // So many Runtime::ServesOtherThreads there are, in all runtimes.
    void didBeginToServeOthers() { ++m_numberThatServeOthers; }
    void didCeaseToServeOthers() { --m_numberThatServeOthers; }

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
    // The same for the main thread: the host wants what runs here ended (worker.terminate(), a time limit). Code finds that out at its safe
    // points. A thread that waits, or has been got out of a system call and is about to make it again, comes to none: it asks here.
    bool throwIfHostEndsWhatRuns();

    // ---- Waiting for other threads ----

    // Waits until `isOver` says so, for so many nanoseconds (negative: for ever), without the GIL. `isOver` is called with it.
    // True: it did. False: time ran out, or there is an exception: on the main thread the handlers of signals run meanwhile.
    // Whoever brings about what is waited for calls wakeOne() or wakeAll() with the same address.
    // The texts are for the exceptions raised where nobody could: "deadlock: <the lock is held>, and there is no other thread",
    // and, once the interpreter is over, "cannot <acquire lock at interpreter finalization>".
    // `WaitOf::Call`: see IdleWait (`isWaitOfCall`). As wide as a word: it is the tenth word of the arguments, so it lies in the frame of the caller, under which the thread sleeps, and of
    // a bool some systems write one byte there.
    enum class WaitOf : uintptr_t { Other, Call };
    bool waitUntil(JSGlobalObject*, const void* address, int64_t timeout, ASCIILiteral whatIsWaitedFor, ASCIILiteral whatCannotBeDone, const ScopedLambda<bool()>& isOver, WaitOf = WaitOf::Other);
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
    // Whether the call on top has been begun again only to sleep on wiped stack, and has not come to its wait yet. No code of the program's has run since it gave up, and the GIL has
    // not been let go of: what it asked the system on its way to the wait it need not ask again.
    bool isCallBegunAgainOnlyToBlock();
    // False: there is an exception, and the call does not begin again.
    bool collectBeforeCallBeginsAgain(JSGlobalObject*, CallFrame*);
    void wakeOne(const void* address);
    void wakeAll(const void* address);
    // The thread of that number (ThreadState::number()), which does not hold the GIL, owes calls: finalizers, callbacks of weak
    // references. Wherever it waits, it is woken, makes them (checkSignals()) and goes on waiting for the time it has left. If it only waits
    // for the GIL, it finds out when it has it. With the GIL, or for the collector while whoever holds it is stopped.
    void wakeToMakeCalls(uint32_t number);
    // The thread of that number, which may be the one that runs, owes finalizers or callbacks of weak references. Under the same
    // conditions. See didCollectWhileIdle().
    void didQueueCallsThatMayUncoverMore(uint32_t number);
    // For collectAndFinalize(): before and behind each collection that is charged to the threads that wait. The GIL is held from the one to the other: the collector runs no code of Python's, and the C that it
    // does call (the tp_traverse of an extension's type) finds the C API closed but for what only reads (CApi::traverse()). See collectWhileIdle().
    void willCollectWhileIdle();
    void didCollectWhileIdle();

    // ---- The other threads, for whoever holds the GIL ----

    template<typename Functor>
    void forEachThread(const Functor& functor)
    {
        for (auto& thread : m_threads)
            functor(thread.get());
    }
    // What each thread that is not running took out of that runtime: functor(ThreadState&, Runtime::ThreadLocalState&).
    template<typename Functor>
    void forEachStateSetAsideOf(Runtime& runtime, const Functor& functor)
    {
        for (auto& thread : m_threads) {
            if (auto* entry = thread->saved ? thread->saved->entryOf(runtime) : nullptr)
                functor(thread.get(), entry->state);
        }
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
            thread->saved->forEachEntry([&](SavedStates::Entry& entry) {
                if (!entry.runtime)
                    return;
                auto& state = entry.state;
                for (JSValue value : { state.currentContext, state.handledException, state.asyncGenFirstIter, state.asyncGenFinalizer, state.sendResult, state.exceptionGroupRest, state.thrownException, state.throwArguments, state.threadLocals, state.traceFunction, state.profileFunction })
                    consider(value);
                if (state.runningGenerator)
                    functor(std::bit_cast<JSCell*>(state.runningGenerator));
            });
        }
    }
    // For the collector, with the world stopped: what the threads that are not running took out of the runtimes and have no room for on their stacks.
    void gatherConservativeRoots(ConservativeRoots&, JITStubRoutineSet&, CodeBlockSet&);

private:
    friend class AllowThreads;
    friend class IdleWait;

    explicit Threads(VM&);
    JS_EXPORT_PRIVATE bool isMainThreadSlow() const;
    void forgetRuntimesThatHaveGone();
    void enable();
    void prepareSignals();
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
    int64_t timeOfNextIdleCollection(JSGlobalObject*);
    bool shouldCollectWhileIdle(JSGlobalObject*, IdleLook);
    // True: a thread that waits has something to finalize. Then the next may find what only that referred to, and comes soon.
    bool collectWhileIdle(JSGlobalObject*, IdleLook);
    void takeGIL(ThreadState&);
    void dropGIL(ThreadState&, bool isYielding);
    JS_EXPORT_PRIVATE bool throwIfStoppingSlow();
    void didTakeGIL(ThreadState&);
    void tellMainThreadOfFinalizers();
    void giveBackWhatIsTaken(ThreadState&);
    void joinWaiters(const void* address, ThreadState&);
    void leaveWaiters(const void* address, ThreadState&);
    void wakeMainThread();
    void wakeToHandleTrap(ThreadState&, bool isTermination);
    friend void mainThreadIsToHandleTrap(VM&, const AbstractLocker&, JSLock::ParkedThread*, bool, bool);
    friend void mainThreadHasTrapsToHandle(VM&);

    VM& m_vm; // Not to be touched without the GIL, except for its traps.
    const uint64_t m_number; // Never used again. Not 0.
    bool m_isEnabled { false };
    bool m_hasPreparedSignals { false };
    uint64_t m_mainThreadIdent { 0 };
    unsigned m_runningCount { 0 };
    unsigned m_numberThatServeOthers { 0 };
    Vector<Ref<ThreadState>> m_threads;
    ThreadState* m_parkedMainThread { nullptr }; // For JSLock::ifEmbedderThreadWaits().
    UncheckedKeyHashMap<const void*, Deque<ThreadState*>> m_waiters;
    // What the share of the threads that wait goes by. See willCollectWhileIdle().
    uint64_t m_bytesAllocatedAtIdleCollection { 0 };
    int64_t m_endOfLastIdleCollection { 0 };
    int64_t m_durationOfIdleCollections { 0 }; // Those of the current period.
    int64_t m_costOfIdleCollections { 0 }; // The same.
    int64_t m_startOfIdleCollection { 0 }; // Of the one that is under way.
    int64_t m_processorTimeAtStartOfIdleCollection { 0 };
    static constexpr unsigned mostEarlyIdleCollections = 8;
    unsigned m_earlyIdleCollectionsLeft { mostEarlyIdleCollections };
    bool m_nextIdleCollectionBeginsPeriod { false };
    bool m_idleCollectionsFoundSomething { false }; // Those of the current period.
    bool m_lastIdleCollectionsFoundSomething { false }; // Those of the period before.
    bool m_nextIdleCollectionMayFindMore { false };
    bool m_threadThatWaitsOwesCalls { false }; // What one collection leaves behind.
    bool m_didTellMainThreadOfFinalizers { false };
    bool m_mainThreadTellsHostOfDirectory { false };
    Vector<Vector<char>> m_variablesToTellHostOf;
    HashMap<Latin1CString, Latin1CString> m_variablesOfHost;

    Lock m_gilLock;
    Condition m_gilCondition; // It is free.
    Condition m_debtCondition; // One of m_debts is paid, or the interpreter has been finalized.
    Condition m_stopCondition; // stopAll() has begun.
    bool m_gilIsHeld WTF_GUARDED_BY_LOCK(m_gilLock) { false };
    ThreadState* m_lastHolder WTF_GUARDED_BY_LOCK(m_gilLock) { nullptr };
    uint64_t m_switchNumber WTF_GUARDED_BY_LOCK(m_gilLock) { 0 };
    unsigned m_waitingForGIL WTF_GUARDED_BY_LOCK(m_gilLock) { 0 };
    // A thread that is made to give up the GIL owes it to those that are asking for it then. See dropGIL().
    struct Debt {
        uint64_t asksUpTo;
        bool isPaid { false };
    };
    uint64_t m_numberOfAsks WTF_GUARDED_BY_LOCK(m_gilLock) { 0 };
    Vector<Debt*, 1> m_debts WTF_GUARDED_BY_LOCK(m_gilLock);
#if !OS(WINDOWS)
    Lock m_interruptLock; // Held while a signal is sent.
#endif
    std::atomic<bool> m_dropIsRequested { false };
    std::atomic<bool> m_isFinalized { false };
    std::atomic<bool> m_isStopping { false };
    static constexpr int64_t defaultSwitchInterval = 5000000;
    std::atomic<int64_t> m_switchInterval { defaultSwitchInterval };
#if ASSERT_ENABLED
    bool m_isBetweenWillAndDidCollectWhileIdle { false }; // (The last member: the others lie where they lie without it.)
#endif
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
    // transfers nothing before it returns, and what follows it makes it again when it was interrupted. (But to end what the thread runs:
    // Threads::wakeToHandleTrap().)
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

    // The sleep or the system call for which this was made has come back because a signal arrived. True: nobody has sent for this thread, so it can go on for the time that is left, without the GIL in
    // between. The collector stops every thread with a signal to scan its stack: each collection would send every thread that waits through the GIL.
    // Whoever sends for a thread that has given up the GIL says so before the signal: Threads::interrupt() by a flag, the end of the VM by another, everybody else by Sleeper::wake(): a signal of the
    // program's, which is for the main thread wherever it was delivered; the traps that another thread has left to the main one; a request for a traceback.
    // With no other thread the GIL was not given up: false. The thread is still in the VM, and the signal may be the engine's own, for a trap that the host has set (a time limit of node:vm,
    // Worker.terminate()), which knows of no flag. Nobody is stopped for a collection there either.
    bool wasInterruptedForNothing()
    {
#if OS(WINDOWS)
        return false;
#else
        // (The wake is taken whatever the flags say: whoever sets one of them wakes as well, and a wake that is left would count the next time.)
        return m_thread && !m_thread->sleeper.takeWake() && !m_thread->isBeingInterrupted.load() && !m_threads->m_isStopping.load();
#endif
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
// One short wait after another is a long one (`while ref() is not None: time.sleep(0.01)`): the time adds up, as long as the thread
// runs for no longer between two waits than the first of them lasted.
class IdleWait {
    WTF_MAKE_NONCOPYABLE(IdleWait);
    WTF_FORBID_HEAP_ALLOCATION;
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
    ~IdleWait();
    bool continuesWait() const { return m_continuesWait; }
    // For a caller that knows its deadline only after it has made this.
    void setDeadline(int64_t deadline) { m_deadline = deadline; }

    bool mayBeginCallAgain() const { return m_call; }
    // For a function that has found a reason why this call must not begin again after all.
    void mustNotBeginCallAgain() { m_call = nullptr; }
    // The wait is about to block, for the first time. False: there is an exception (the call begins again).
    bool willBlock();

    int64_t timeToCollect() const { return m_timeToCollect; }
    // For one wait that is made as several, each with one of these: the next looks no more often than the last would have gone on to. To be called before anything else.
    int64_t interval() const { return m_interval; }
    void goOnWithInterval(int64_t);
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
    // For a call that cannot be made piece by piece, if there is reason to think that what it asks for is held by something that
    // the program has dropped. Once: a call that has been begun again for it does not ask again. `hasLooked`: it has collected,
    // now or before the call began again.
    bool collectBeforeBlocking(IdleLook, bool& hasLooked);

private:
    bool collect(IdleLook);
    bool beginsCallAgain(bool collects, IdleLook);

    JSGlobalObject* m_globalObject;
    ThreadState& m_thread;
    CallFrame* m_call { nullptr };
    int64_t m_deadline;
    int64_t m_interval { firstInterval };
    int64_t m_start;
    int64_t m_startOfWhatCounts; // What it has waited since then is not in ThreadState::waits yet.
    int64_t m_timeToCollect;
    bool m_wasDueAtStart;
    bool m_beginsCallAgain { false };
    bool m_continuesWait { false };
    bool m_hasCollectedAsLastResort { false };
};

// Threads::stopAll() sends it to the threads that Python started, which therefore cannot block it.
int signalThatInterruptsThreads();

// False, with a RuntimeError: this is not the main thread, and what is about to happen would run code of JS, or of the host.
bool ensureMainThreadForJS(JSGlobalObject*);

} // namespace Python
} // namespace JSC
