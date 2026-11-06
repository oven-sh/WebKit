/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyThreads.h"

#include "ConservativeRoots.h"
#include "DeferTermination.h"
#include "JSCInlines.h"
#include "PyErrors.h"
#include "PyEventWait.h"
#include "PyExec.h"
#include "PyHooks.h"
#include "PyInternals.h"
#include "PyMonitoring.h"
#include "PyPosix.h"
#include "TopExceptionScope.h"
#include "VMTrapsInlines.h"
#include <wtf/CPUTime.h>
#include <wtf/SetForScope.h>
#include <wtf/Threading.h>
#include <wtf/WorkQueue.h>

#if !OS(WINDOWS)
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#endif
#if OS(LINUX)
#include <linux/futex.h>
#include <sys/syscall.h>
#endif

namespace JSC {
namespace Python {

// See PyThreads.h. (Python/ceval_gil.c and Python/parking_lot.c in CPython are the reference.)

// ---- Sleeper ----

#if OS(LINUX)

bool Sleeper::sleep(int64_t deadline)
{
    struct timespec time;
    if (deadline >= 0) {
        time.tv_sec = deadline / 1000000000;
        time.tv_nsec = deadline % 1000000000;
    }
    while (!m_isWoken.exchange(0)) {
        // The time is absolute, on the monotonic clock.
        long result = syscall(SYS_futex, &m_isWoken, FUTEX_WAIT_BITSET | FUTEX_PRIVATE_FLAG, 0, deadline >= 0 ? &time : nullptr, nullptr, FUTEX_BITSET_MATCH_ANY);
        // (A wake() that has come since stays, for whoever asks what the signal was for.)
        if (result < 0 && errno == EINTR)
            return false;
        if (result < 0 && errno != EAGAIN) {
            // Time is up.
            m_isWoken.store(0);
            return true;
        }
    }
    return true;
}

void Sleeper::wake()
{
    int savedErrno = errno;
    m_isWoken.store(1);
    syscall(SYS_futex, &m_isWoken, FUTEX_WAKE | FUTEX_PRIVATE_FLAG, 1, nullptr, nullptr, 0);
    errno = savedErrno;
}

#else

bool Sleeper::sleep(int64_t deadline)
{
    // A signal handler cannot notify a condition. It sets the flag, which is looked at this often.
    constexpr int64_t slice = 20000000;
    Locker locker { m_lock };
    while (!m_isWoken.exchange(false)) {
        int64_t now = monotonicNanoseconds();
        if (deadline >= 0 && now >= deadline)
            return true;
        int64_t time = deadline >= 0 ? std::min(deadline - now, slice) : slice;
        m_condition.waitFor(m_lock, Seconds::fromNanoseconds(static_cast<double>(time)));
    }
    return true;
}

void Sleeper::wake()
{
    m_isWoken.store(true);
    if (m_lock.tryLock()) {
        m_condition.notifyOne();
        m_lock.unlock();
    }
}

#endif

// ---- Threads ----

static std::atomic<uint64_t> s_lastThreadSerial;

ThreadState::ThreadState()
    : serial(++s_lastThreadSerial)
{
}

// The state of this thread, and whose it is. A Threads goes with the last runtime of its VM, and the states of its threads with it, while the thread that ran Python there lives on and may start Python
// again: in a new runtime, with a new Threads. So the pointer is good only for the Threads that set it, which is known by a number that is never used again. (Whoever destroys a Threads
// cannot clear this: that may be another thread, the collector's.)
static thread_local ThreadState* t_currentThread;
static thread_local uint64_t t_threadsOfCurrentThread;
static thread_local bool t_isOnStartedThread;
static std::atomic<uint64_t> s_lastNumberOfThreads;
static std::atomic<Sleeper*> s_sleeperOfMainThreadOfProcess;

Ref<Threads> Threads::sharedFor(VM& vm)
{
    if (Threads* threads = vm.heap.pythonThreads()) {
        if (vm.heap.pythonRuntimes().isEmpty())
            threads->forgetRuntimesThatHaveGone();
        return *threads;
    }
    Ref threads = adoptRef(*new Threads(vm));
    vm.heap.setPythonThreads(threads.copyRef());
    return threads;
}

// Python starts anew in the VM: no runtime is left of those that there have been. What they said or brought about is not for the next one. What stays is what is true of the VM and of
// the process: which thread is the main one and its state, that there is a GIL once a thread has been started (enable()) and who holds it, what the host has been told of the environment.
// No thread of theirs is left: one that runs, waits or has been frozen at the end of its interpreter keeps its runtime by way of its frames.
void Threads::forgetRuntimesThatHaveGone()
{
    ASSERT(!m_runningCount);
    m_isFinalized.store(false);
    m_switchInterval.store(defaultSwitchInterval);
    m_bytesAllocatedAtIdleCollection = 0;
    m_endOfLastIdleCollection = 0;
    m_durationOfLastIdleCollection = 0;
    m_costOfLastIdleCollection = 0;
    m_earlyIdleCollectionsLeft = mostEarlyIdleCollections;
    m_lastIdleCollectionFoundSomething = false;
    m_nextIdleCollectionMayFindMore = false;
    m_threadThatWaitsOwesCalls = false;
    for (auto& thread : m_threads)
        thread->waits = { };
}

void Threads::runtimeDidEnd(Runtime& runtime)
{
    // See prepareSignals().
    m_hasPreparedSignals = false;
    // Another may come to lie at its address.
    for (auto& thread : m_threads) {
        if (auto* entry = thread->saved ? thread->saved->entryOf(runtime) : nullptr)
            entry->runtime = nullptr;
    }
}

Threads::Threads(VM& vm)
    : m_vm(vm)
    , m_number(++s_lastNumberOfThreads)
    , m_mainThreadIdent(currentThreadIdent())
{
}

Threads::~Threads()
{
    for (auto& thread : m_threads) {
        Sleeper* sleeper = &thread->sleeper;
        s_sleeperOfMainThreadOfProcess.compare_exchange_strong(sleeper, nullptr);
    }
}

bool Threads::isMainThreadSlow() const
{
    return currentThreadIdent() == m_mainThreadIdent;
}

static bool isMainThreadOfProcess()
{
#if OS(LINUX)
    return syscall(SYS_gettid) == getpid();
#elif OS(DARWIN)
    return pthread_main_np();
#else
    return true;
#endif
}

void Threads::wakeMainThreadOfProcess()
{
    if (Sleeper* sleeper = s_sleeperOfMainThreadOfProcess.load())
        sleeper->wake();
}

bool Threads::isOnStartedThread()
{
    return t_isOnStartedThread;
}

bool Threads::hasThread(uint64_t ident) const
{
    if (ident == currentThreadIdent())
        return true;
    return m_threads.containsIf([&](auto& thread) {
        return thread->ident == ident;
    });
}

ThreadState& Threads::current()
{
    if (t_threadsOfCurrentThread == m_number) [[likely]]
        return *t_currentThread;
    ASSERT(m_vm.currentThreadIsHoldingAPILock());
    t_threadsOfCurrentThread = m_number;
    uint64_t ident = currentThreadIdent();
    // It may have been here before, and in another VM since.
    for (auto& thread : m_threads) {
        if (thread->ident == ident) {
            t_currentThread = thread.ptr();
            return *t_currentThread;
        }
    }
    // Every other thread has had one since before it ran.
    ASSERT(!m_isEnabled);
    Ref thread = ThreadState::create();
    thread->ident = ident;
    thread->isMain = ident == m_mainThreadIdent;
    t_currentThread = thread.ptr();
    if (thread->isMain && isMainThreadOfProcess())
        s_sleeperOfMainThreadOfProcess.store(&thread->sleeper);
    RELEASE_ASSERT(m_vm.heap.hasAccess()); // See gatherConservativeRoots().
    m_threads.append(WTF::move(thread));
    return *t_currentThread;
}

#if !OS(WINDOWS)
// What gets a thread out of a system call. To ignore it is the default, so that one which arrives where nobody expects it does no harm,
// and it may arrive for no reason anyway.
static constexpr int signalThatInterrupts = SIGURG;

int signalThatInterruptsThreads() { return signalThatInterrupts; }

static void makeSignalInterrupt()
{
    struct sigaction current;
    if (sigaction(signalThatInterrupts, nullptr, &current))
        return;
    // Any handler will do, the program's own too. A signal that is ignored interrupts nothing.
    if ((current.sa_flags & SA_SIGINFO) || (current.sa_handler != SIG_DFL && current.sa_handler != SIG_IGN))
        return;
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = [](int) { };
    sigemptyset(&action.sa_mask);
    sigaction(signalThatInterrupts, &action, nullptr);
}
#endif

// The first thread is about to be started, by the only one there is, which holds the GIL from now on.
void Threads::enable()
{
    ThreadState& thread = current();
    Locker locker { m_gilLock };
    m_gilIsHeld = true;
    m_lastHolder = &thread;
    m_isEnabled = true;
}

// The signals that get a thread out of a system call. A runtime that ends puts the actions of the signals back as it found them, if they were its own to set (detachSignals()), while this
// lives on with the other runtimes of the VM and with those to come. So it is done again when the next thread is started.
void Threads::prepareSignals()
{
    m_hasPreparedSignals = true;
#if !OS(WINDOWS)
    makeSignalInterrupt();
#endif
#if OS(LINUX)
    // (Elsewhere that one is SIGURG too.)
    if (!m_vm.heap.pythonRuntimes().isEmpty())
        prepareToWakeMainThread(m_vm.heap.pythonRuntimes().first()->globalObject());
#endif
}

// ---- The GIL ----

void Threads::takeGIL(ThreadState& thread)
{
    m_gilLock.lock();
    if (!thread.isMain && m_isFinalized.load() && !m_isStopping.load()) [[unlikely]] {
        // Somebody may be waiting for this thread to take it, and somebody else for it to be free.
        m_debtCondition.notifyAll();
        m_gilCondition.notifyOne();
        while (!m_isStopping.load())
            m_stopCondition.wait(m_gilLock);
    }
    ++m_waitingForGIL;
    uint64_t number = ++m_numberOfAsks;
    while (m_gilIsHeld) {
        uint64_t switchNumber = m_switchNumber;
        bool wasNotified = m_gilCondition.waitFor(m_gilLock, Seconds::fromNanoseconds(static_cast<double>(switchInterval())));
        // Whoever has it has had it all this time.
        if (!wasNotified && m_gilIsHeld && m_switchNumber == switchNumber) {
            // It may be busy with other things than Python code: the host's event loop, say.
            m_dropIsRequested.store(true);
            noteCallsOwed(m_vm);
        }
        if (!thread.isMain && m_isFinalized.load() && !m_isStopping.load()) [[unlikely]] {
            --m_waitingForGIL;
            m_gilLock.unlock();
            return takeGIL(thread);
        }
    }
    --m_waitingForGIL;
    m_gilIsHeld = true;
    if (m_lastHolder != &thread) {
        m_lastHolder = &thread;
        ++m_switchNumber;
    }
    m_dropIsRequested.store(false);
    // The host's objects belong to the main thread, dead ones too. This is said before the thread takes the lock of the VM: to take
    // it may be to finish a collection, which sweeps.
    m_vm.heap.setMayRunEmbedderFinalizers(thread.isMain);
    for (Debt* debt : m_debts) {
        if (number > debt->asksUpTo || debt->isPaid)
            continue;
        debt->isPaid = true;
        m_debtCondition.notifyAll();
    }
    m_gilLock.unlock();
}

void Threads::dropGIL(ThreadState&, bool isYielding)
{
    Locker locker { m_gilLock };
    ASSERT(m_gilIsHeld);
    m_gilIsHeld = false;
    m_gilCondition.notifyOne();
    if (!isYielding || !m_waitingForGIL)
        return;
    // This thread is about to ask for it again, and would as likely as not get it before the one that was notified wakes up. That
    // somebody else has had it is not enough: a third thread that happens to be awake has it, and is done with it, before any of those
    // that are asking now runs. Nobody but this thread is kept waiting. One of them is in the queue until the debt is paid, unless they
    // all leave it because the interpreter has been finalized.
    Debt debt { m_numberOfAsks };
    m_debts.append(&debt);
    while (!debt.isPaid && !m_isFinalized.load())
        m_debtCondition.wait(m_gilLock);
    m_debts.removeFirst(&debt);
}

static constexpr auto endingTraps = VMTraps::NeedTermination | VMTraps::NeedWatchdogCheck;

// Once it is back. Between calls there is nothing to end, and a thread that defers terminations would only go back to what it was waiting for.
static bool wouldHandleTrap(const ThreadState& thread, bool isTermination)
{
    return thread.parked->isInCalls() && !(isTermination && thread.defersTermination);
}

void Threads::release(ThreadState& thread, JSLock::ParkedThread& parked, SavedStates& saved)
{
    ASSERT(&thread == t_currentThread);
    // The host gives up access to the heap before it waits for events if a collection is under way, which then does not wait for this
    // thread. What follows takes references from where the collector finds them to where it finds them, so it must not be looking
    // meanwhile. JSLock::park() gives access up again.
    if (!m_vm.heap.hasAccess()) [[unlikely]]
        m_vm.heap.acquireAccess();
    auto& runtimes = m_vm.heap.pythonRuntimes();
    if (runtimes.size() > SavedStates::capacity) [[unlikely]]
        saved.more = FixedVector<SavedStates::Entry>(runtimes.size() - SavedStates::capacity);
    // Before anything is put there: it is by this that the collector finds what is not on the stack.
    thread.saved = &saved;
    for (unsigned i = 0; i < runtimes.size(); ++i) {
        auto& entry = i < SavedStates::capacity ? saved.entries[i] : saved.more[i - SavedStates::capacity];
        entry.runtime = runtimes[i];
        runtimes[i]->takeThreadLocalState(entry.state);
    }
    thread.parked = &parked;
    thread.defersTermination = m_vm.traps().isDeferringTermination();
    // Not in stopAll(): nothing is left to end, and it parks again and again, with no look in between.
    bool isToBeFound = thread.isMain && !m_isStopping.load();
    if (isToBeFound) {
        m_parkedMainThread = &thread;
        parked.setIsEmbedderThread(this);
    }
    if (m_vm.heap.hasPostponedEmbedderFinalizers() && !thread.isMain && !m_didTellMainThreadOfFinalizers) [[unlikely]]
        tellMainThreadOfFinalizers();
    // Whatever has caught an exception without making the VM forget it (see PyErrors.cpp): a collection that the program asks for does that, but only for
    // the thread that asks. So no thread takes one along.
    m_vm.clearLastException();
    m_vm.apiLock().park(parked);
    // Whoever fires one either finds this thread parked, or has fired it before this looks. (It still has the GIL: whoever calls this drops it afterwards. So no other
    // thread can have come across the bits meanwhile and set them aside for this one: VMTraps::handleTraps().)
    if (isToBeFound && m_vm.traps().needHandling(endingTraps)) [[unlikely]] {
        for (auto event : { VMTraps::NeedTermination, VMTraps::NeedWatchdogCheck }) {
            if (m_vm.traps().needHandling(event) && wouldHandleTrap(thread, event == VMTraps::NeedTermination))
                m_vm.traps().notifyEmbedderThreadThatWaits(event);
        }
    }
}

void Threads::acquire(ThreadState& thread, JSLock::ParkedThread& parked, SavedStates& saved)
{
    m_vm.apiLock().unpark(parked);
    if (thread.isMain)
        m_parkedMainThread = nullptr;
    thread.parked = nullptr;
    // Now that it is back in the VM nobody hands out calls: that takes the GIL, or a collector that has stopped the VM's thread.
    thread.losesNothingIfInterrupted = false;
#if !OS(WINDOWS)
    if (thread.isBeingInterrupted.load(std::memory_order_relaxed)) [[unlikely]]
        stopInterrupting(thread);
#endif
    // The entries are in the order that the list had. Since then runtimes may have gone, each with its global object, which leaves the others in their order (and their
    // entries name nothing any more: runtimeDidEnd()), and the main thread may have made some, which are at the end.
    auto& runtimes = m_vm.heap.pythonRuntimes();
    unsigned position = 0;
    saved.forEachEntry([&](SavedStates::Entry& entry) {
        if (!entry.runtime)
            return;
        RELEASE_ASSERT(position < runtimes.size() && runtimes[position] == entry.runtime);
        entry.runtime->putThreadLocalState(entry.state);
        entry.runtime = nullptr;
        ++position;
    });
    // In those it has what a thread has that has just started. It runs there: the calls that are owed are made in every runtime of the VM.
    for (; position < runtimes.size(); ++position) {
        Runtime::ThreadLocalState state;
        state.thread = thread.number();
        runtimes[position]->putThreadLocalState(state);
    }
    // Not before: see release().
    RELEASE_ASSERT(m_vm.heap.hasAccess());
    thread.saved = nullptr;
    saved.more.clear();
    didTakeGIL(thread);
}

// A thread enters and leaves the list, and sets `saved`, with access to the heap, so not while the collector looks. As a stack is scanned, so that a word means here what it means in the
// four that are on one. (Heap::evacuateSparseAuxiliaryBlocks(), which has to know of every place from which a butterfly may be pointed into, is not told of these: a state refers to cells.)
// It is a runtime that is asked (Runtime::willScanStacks()). Once the last has gone nobody is: by then every entry has been struck out (runtimeDidEnd()), and nobody reads the state of one that has.
void Threads::gatherConservativeRoots(ConservativeRoots& roots, JITStubRoutineSet& jitStubRoutines, CodeBlockSet& codeBlocks)
{
    ASSERT(m_vm.heap.worldIsStopped());
    for (auto& thread : m_threads) {
        if (thread->saved && !thread->saved->more.isEmpty()) [[unlikely]]
            roots.add(thread->saved->more.begin(), thread->saved->more.end(), jitStubRoutines, codeBlocks);
    }
}

// The trap is the VM's, and what it stands for depends on the thread.
void Threads::didTakeGIL(ThreadState& thread)
{
    m_vm.traps().fireTrap(VMTraps::NeedPythonCalls);
    ASSERT(m_vm.heap.mayRunEmbedderFinalizers() == thread.isMain);
    if (thread.isMain) {
        m_didTellMainThreadOfFinalizers = false;
        m_vm.traps().fireTrapsLeftToEmbedderThread();
#if !OS(WINDOWS)
        if (std::exchange(m_mainThreadTellsHostOfDirectory, false) && !m_vm.heap.pythonRuntimes().isEmpty()) [[unlikely]]
            posixDidChangeDirectory(m_vm.heap.pythonRuntimes().first()->globalObject());
        if (!m_variablesToTellHostOf.isEmpty() && !m_vm.heap.pythonRuntimes().isEmpty()) [[unlikely]] {
            ExceptionSetAside suspended(m_vm.heap.pythonRuntimes().first()->globalObject());
            DeferTerminationForAWhile deferred(m_vm);
            posixDidChangeEnvironment(m_vm.heap.pythonRuntimes().first()->globalObject(), std::exchange(m_variablesToTellHostOf, { }).span());
        }
#endif
    }
}

// Wherever it sleeps: in a wait for other threads, or in the host's event loop.
void Threads::wakeMainThread()
{
    for (auto& thread : m_threads) {
        if (thread->isMain)
            thread->sleeper.wake();
    }
    noteCallsOwed(m_vm);
}

void Threads::wakeToMakeCalls(uint32_t number)
{
    for (auto& thread : m_threads) {
        if (thread->number() != number)
            continue;
        thread->sleeper.wake();
        if (thread->isMain)
            noteCallsOwed(m_vm); // The host's event loop.
#if !OS(WINDOWS)
        // Out of a system call too, where that does no harm. Elsewhere the calls are made when the system call returns.
        if (thread->parked && thread->losesNothingIfInterrupted)
            interrupt(thread.get());
#endif
    }
}

void Threads::didQueueCallsThatMayUncoverMore(uint32_t number)
{
    for (auto& thread : m_threads) {
        if (thread->number() == number && thread.ptr() != m_threadThatCollectsWhileIdle && thread->isWaiting())
            m_threadThatWaitsOwesCalls = true;
    }
}

#if !OS(WINDOWS)
void Threads::interrupt(ThreadState& thread)
{
    Locker locker { m_interruptLock };
    if (thread.isBeingInterrupted.exchange(true))
        return;
    ++thread.timesInterrupted;
    sendInterrupt(thread);
    interruptAgainAfter(1_ms, thread);
}

// The thread cannot have gone on to another system call, or have ended: before that it says that it is back, with the lock.
void Threads::sendInterrupt(ThreadState& thread)
{
    // SIGURG is the program's to handle, to block and to send, and the host's. On the thread where the handlers of Python run there
    // is a signal that is nobody else's.
    if (thread.isMain && wakeMainThreadFromSystemCall(m_vm))
        return;
    pthread_kill(reinterpret_cast<pthread_t>(reinterpret_cast<void*>(static_cast<uintptr_t>(thread.ident))), signalThatInterrupts);
}

void Threads::interruptAgainAfter(Seconds pause, ThreadState& thread)
{
    // Twice as long each time, up to a second. A thread that has the signal blocked, or whose system calls begin again by themselves
    // (signal.siginterrupt()), gets one a second, which costs nothing to speak of, and one that was kept from its system call for
    // long is reached within a second of getting there, not after as long again.
    VMTraps::queue().dispatchAfter(pause, [protectedThis = Ref { *this }, thread = Ref { thread }, pause, time = thread.timesInterrupted] {
        Locker locker { protectedThis->m_interruptLock };
        if (!thread->isBeingInterrupted.load() || thread->timesInterrupted != time)
            return;
        protectedThis->sendInterrupt(thread.get());
        protectedThis->interruptAgainAfter(std::min(2 * pause, 1_s), thread.get());
    });
}

void Threads::stopInterrupting(ThreadState& thread)
{
    {
        Locker locker { m_interruptLock };
        thread.isBeingInterrupted.store(false);
    }
    // What has been sent arrives when a system call returns, at the latest: here, and not in a call that would lose something.
    sigset_t pending;
    sigpending(&pending);
}
#endif

void mainThreadHasTrapsToHandle(VM& vm)
{
    if (!vm.heap.pythonRuntimes().isEmpty())
        vm.heap.pythonRuntimes().first()->threads().wakeMainThread();
}

// The thread is kept parked meanwhile.
void Threads::wakeToHandleTrap(ThreadState& thread, bool isTermination)
{
    thread.sleeper.wake();
#if !OS(WINDOWS)
    // What a signal makes a system call lose, it loses to code that is ended as soon as the call returns. The watchdog may find that it is not time yet.
    if (wouldHandleTrap(thread, isTermination) && (isTermination || thread.losesNothingIfInterrupted))
        interrupt(thread);
#else
    UNUSED_PARAM(isTermination);
#endif
}

void mainThreadIsToHandleTrap(VM& vm, const AbstractLocker&, JSLock::ParkedThread* parked, bool isInEventLoop, bool isTermination)
{
    // There it waits whether there are other threads or not. What waits there looks for calls that are owed, and then at all else: waitForEvents().
    if (isInEventLoop)
        noteCallsOwed(vm);
    // What is found there is what the thread itself goes on with when it is back: the Threads whose release() it is in, and its state.
    if (parked) {
        auto& threads = *static_cast<Threads*>(parked->embedderThreadClient());
        threads.wakeToHandleTrap(*threads.m_parkedMainThread, isTermination);
    }
}

// This thread has come across dead objects of the host, which only the main thread may sweep. That one does so at its next safe
// point (makeCallsOwed()). If it sleeps, in a wait for other threads or in the host's event loop, it is woken up for it.
void Threads::tellMainThreadOfFinalizers()
{
    m_didTellMainThreadOfFinalizers = true;
    wakeMainThread();
}

void AllowThreads::release(Threads& threads, WhenInterrupted whenInterrupted)
{
    m_threads = &threads;
    m_thread = &threads.current();
    m_thread->losesNothingIfInterrupted = whenInterrupted == LosesNothing;
    auto* luggage = new (NotNull, m_luggage) Luggage;
    threads.release(*m_thread, luggage->parked, luggage->saved);
    threads.dropGIL(*m_thread, false);
}

void AllowThreads::acquire()
{
    int savedErrno = errno;
    auto* luggage = std::launder(reinterpret_cast<Luggage*>(m_luggage));
#if !OS(WINDOWS)
    // It may have to wait for the GIL. (Until it is back in the VM it may be interrupted anew: Threads::acquire().)
    if (m_thread->isBeingInterrupted.load(std::memory_order_relaxed)) [[unlikely]]
        m_threads->stopInterrupting(*m_thread);
#endif
    m_threads->takeGIL(*m_thread);
    m_threads->acquire(*m_thread, luggage->parked, luggage->saved);
    luggage->~Luggage();
    errno = savedErrno;
}

void Threads::yield()
{
    ThreadState& thread = current();
    JSLock::ParkedThread parked;
    SavedStates saved;
    release(thread, parked, saved);
    dropGIL(thread, true);
    takeGIL(thread);
    acquire(thread, parked, saved);
}

void Threads::didFinalize()
{
    m_isFinalized.store(true);
    Locker locker { m_gilLock };
    m_gilCondition.notifyAll();
    m_debtCondition.notifyAll();
}

void Threads::stopAll()
{
    ASSERT(isMainThread());
    m_isFinalized.store(true);
    if (!m_runningCount)
        return;
#if !OS(WINDOWS)
    bool hadTerminationRequest = m_vm.hasTerminationRequest();
    m_vm.ensureTerminationException();
    makeSignalInterrupt();
    {
        Locker locker { m_gilLock };
        m_isStopping.store(true);
        m_stopCondition.notifyAll();
        m_debtCondition.notifyAll();
    }
    ThreadState& thread = current();
    int64_t pause = 1000000;
    while (m_runningCount) {
        // Again and again: a thread that was about to make a system call when the signal came is in it now.
        for (auto& other : m_threads) {
            if (other->isMain)
                continue;
            other->sleeper.wake();
            // It takes itself out of the list, with the GIL, before it ends.
            pthread_kill(reinterpret_cast<pthread_t>(reinterpret_cast<void*>(static_cast<uintptr_t>(other->ident))), signalThatInterrupts);
        }
        JSLock::ParkedThread parked;
        SavedStates saved;
        release(thread, parked, saved);
        dropGIL(thread, false);
        thread.sleeper.sleep(monotonicNanoseconds() + pause);
        takeGIL(thread);
        acquire(thread, parked, saved);
        pause = std::min<int64_t>(2 * pause, 50000000);
    }
    if (hadTerminationRequest)
        m_vm.setHasTerminationRequest();
    else
        m_vm.clearHasTerminationRequest();
#endif
}

bool Threads::throwIfStoppingSlow()
{
    if (current().isMain)
        return false;
    // Native code may drop what is thrown here, as it drops other exceptions. The next safe point throws it again.
    m_vm.traps().fireTrap(VMTraps::NeedPythonCalls);
    if (m_vm.hasPendingTerminationException())
        return true;
    if (m_vm.traps().isDeferringTermination())
        return false;
    m_vm.setHasTerminationRequest();
    m_vm.throwTerminationException();
    return true;
}

bool Threads::throwIfHostEndsWhatRuns()
{
    if (!m_vm.traps().needHandling(endingTraps) || !m_vm.entryScope || !isMainThread()) [[likely]]
        return false;
    // Nothing would come of it, and it is not cheap.
    if (m_vm.traps().isDeferringTermination() && !m_vm.traps().needHandling(VMTraps::NeedWatchdogCheck))
        return false;
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(m_vm);
    m_vm.traps().handleTraps(endingTraps);
    return !!scope.exception();
}

void stopThreads(VM& vm)
{
    if (!vm.heap.pythonRuntimes().isEmpty())
        vm.heap.pythonRuntimes().first()->threads().stopAll();
}

bool callWithoutGIL(VM& vm, void (*function)(void*), void* argument)
{
    // The host may have given up access to the heap for its wait. Then the collector's thread may be at work, and it takes a runtime whose global object has died out of the heap's list.
    Threads* threads = vm.heap.pythonThreads();
    if (!threads || !threads->runningCount())
        return false;
    if (!vm.heap.hasAccess())
        vm.heap.acquireAccess();
    if (vm.heap.pythonRuntimes().isEmpty()) [[unlikely]]
        return false;
    AllowThreads allow(vm.heap.pythonRuntimes().first()->globalObject());
    function(argument);
    return true;
}

// ---- Starting and ending threads ----

struct Threads::Start {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(Start);
    Ref<Threads> threads;
    Ref<ThreadState> thread;
    Function<void()> body;
};

void* Threads::threadEntry(void* argument)
{
    std::unique_ptr<Start> start { static_cast<Start*>(argument) };
    start->threads->runThread(start->thread.get(), WTF::move(start->body));
    return nullptr;
}

void Threads::runThread(ThreadState& thread, Function<void()>&& body)
{
    t_currentThread = &thread;
    t_threadsOfCurrentThread = m_number;
    t_isOnStartedThread = true;
#if !OS(WINDOWS)
    // Whoever started this thread may have it blocked.
    sigset_t interrupting;
    sigemptyset(&interrupting);
    sigaddset(&interrupting, signalThatInterrupts);
    pthread_sigmask(SIG_UNBLOCK, &interrupting, nullptr);
#endif
    takeGIL(thread);
    {
        // The thread that gave up the GIL left the VM and the runtimes as they are when nothing runs.
        JSLockHolder locker(m_vm);
        didTakeGIL(thread);
        for (Runtime* runtime : m_vm.heap.pythonRuntimes())
            runtime->setCurrentThread(thread.number());
        body();
        body = nullptr;

        // What it owes, it pays before it goes.
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(m_vm);
        for (Runtime* runtime : Vector<Runtime*, 1>(m_vm.heap.pythonRuntimes())) {
            if (m_vm.heap.pythonRuntimes().contains(runtime) && runtime->hasPendingCalls())
                runtime->runPendingCalls();
            // The host is ending what runs here. That is left to the caller, and this is as far as it goes: whoever has the VM next
            // is to find nothing in it.
            if (m_vm.hasPendingTerminationException()) [[unlikely]] {
                scope.clearException();
                break;
            }
        }
        scope.assertNoException();
        // (A copy: to tell the monitoring may make a frame object, and a collection takes a runtime whose global object it finds dead out of the list.)
        for (Runtime* runtime : Vector<Runtime*, 1>(m_vm.heap.pythonRuntimes())) {
            if (!m_vm.heap.pythonRuntimes().contains(runtime))
                continue;
            runtime->importLock.holderEnds(*this, thread);
            runtime->warningsLock.holderEnds(*this, thread);
            Runtime::ThreadLocalState state;
            runtime->takeThreadLocalState(state);
            if (state.traceFunction || state.profileFunction)
                runtime->monitoring().tracingThreadCountChanged(-!!state.traceFunction, -!!state.profileFunction);
        }
        --m_runningCount;
        RELEASE_ASSERT(m_vm.heap.hasAccess());
        m_threads.removeFirstMatching([&](auto& other) {
            return other.ptr() == &thread;
        });
        for (Runtime* runtime : m_vm.heap.pythonRuntimes())
            runtime->threadDidEnd(thread.number());
        if (m_vm.heap.hasPostponedEmbedderFinalizers() && !m_didTellMainThreadOfFinalizers) [[unlikely]]
            tellMainThreadOfFinalizers();
        if (m_isStopping.load()) [[unlikely]]
            wakeMainThread();
    }
    t_currentThread = nullptr;
    t_threadsOfCurrentThread = 0;
    t_isOnStartedThread = false;
    dropGIL(thread, false);
}

int Threads::start(size_t stackSize, Function<void()>&& body, uint64_t& ident)
{
#if OS(WINDOWS)
    UNUSED_PARAM(stackSize);
    UNUSED_PARAM(body);
    UNUSED_PARAM(ident);
    return EAGAIN;
#else
    if (!m_isEnabled)
        enable();
    if (!m_hasPreparedSignals)
        prepareSignals();

    Ref thread = ThreadState::create();
    auto* start = new Start { *this, thread.copyRef(), WTF::move(body) };

    pthread_attr_t attributes;
    if (int error = pthread_attr_init(&attributes)) {
        delete start;
        return error;
    }
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    if (stackSize) {
        if (int error = pthread_attr_setstacksize(&attributes, stackSize)) {
            pthread_attr_destroy(&attributes);
            delete start;
            return error;
        }
    }

    // In the list before it runs: it takes itself out.
    RELEASE_ASSERT(m_vm.heap.hasAccess());
    m_threads.append(thread.copyRef());
    ++m_runningCount;

    pthread_t handle;
    int error = pthread_create(&handle, &attributes, threadEntry, start);
    pthread_attr_destroy(&attributes);
    if (error) {
        m_threads.removeLast();
        --m_runningCount;
        delete start;
        return error;
    }
    ident = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(reinterpret_cast<void*>(handle)));
    thread->ident = ident; // The thread does nothing before it has the GIL, which this one holds.
    return 0;
#endif
}

// ---- Waiting for other threads ----

// A program written for reference counts may wait for what only a finalizer brings about: it has dropped a response without closing
// it, say, and waits for a server, in another thread, that waits for the end of the connection. A thread that has waited
// for some time has nothing better to do than to find out.
int64_t Threads::timeOfNextIdleCollection(JSGlobalObject* globalObject)
{
    // Such a collection stops every thread, and any number of threads may be idle while one works: all of them together spend
    // at most a twentieth of the time on it. If nothing has been allocated since the last time, there is little to find, but
    // the count moves in steps, and to drop something takes no allocation at all: then a fiftieth. (Of objects with finalizers it
    // is known exactly whether any has been made.) If the last one found something to finalize for a thread that waits,
    // there may be more: then half, for a while (see collectWhileIdle()).
    int64_t cost = m_costOfLastIdleCollection;
    if (m_nextIdleCollectionMayFindMore)
        return m_endOfLastIdleCollection + cost;
    bool hasCause = m_vm.heap.totalBytesAllocated() != m_bytesAllocatedAtIdleCollection || runtime(globalObject).hasRegisteredFinalizableSinceCollection();
    // But what a thread drops before it waits is not to lie there for a minute because the heap is large or the last collection had much
    // to do: after five seconds they look again, unless that came to more than a tenth of the time.
    constexpr int64_t longestPause = 5000000000;
    return m_endOfLastIdleCollection + std::min((hasCause ? 20 : 50) * cost, std::max(longestPause, 9 * cost));
}

bool Threads::shouldCollectWhileIdle(JSGlobalObject* globalObject, IdleLook look)
{
    if (runtime(globalObject).isRunningPendingCalls())
        return false;
    return look != IdleLook::WithinShare || (!m_threadThatCollectsWhileIdle && monotonicNanoseconds() >= timeOfNextIdleCollection(globalObject));
}

// Of all processors: the collector marks on several at once, and a process that does nothing but wait is to use next to none.
static int64_t processorNanoseconds()
{
    auto time = CPUTime::get();
    return time ? static_cast<int64_t>((time->userTime + time->systemTime).nanoseconds()) : 0;
}

bool Threads::collectWhileIdle(JSGlobalObject* globalObject, IdleLook look)
{
    bool leavesCyclicGarbageForLater = look != IdleLook::LastResort;
    auto calls = look == IdleLook::WithinShare ? Runtime::CallsOf::ThisThread : Runtime::CallsOf::AllThreads;
    int64_t start = monotonicNanoseconds();
    int64_t processorTimeAtStart = processorNanoseconds();
    ThreadState& thread = current();
    // One at a time. Finalizers run in the middle of it, and other threads while those wait: one of them that collected for the same
    // reason would do the same work again, and what each has found for the threads that wait would be mixed up.
    if (m_threadThatCollectsWhileIdle) {
        if (look != IdleLook::WithinShare)
            collectAndFinalize(globalObject, nullptr, CollectionScope::Full, leavesCyclicGarbageForLater, calls);
        return m_nextIdleCollectionMayFindMore;
    }
    m_threadThatWaitsOwesCalls = false;
    // What reference counts would have freed would have been finalized by now. Cyclic garbage would not: CPython's collector does not
    // run in a thread that waits, which may well hold a lock that a finalizer wants.
    // It makes the calls that are its own. If nothing else can bring about what it waits for, all.
    {
        SetForScope collector(m_threadThatCollectsWhileIdle, &thread);
        collectAndFinalize(globalObject, nullptr, CollectionScope::Full, leavesCyclicGarbageForLater, calls);
    }
    // A call of the program's wanted an answer, as gc.collect() does. That is not time that threads with nothing to do have taken: they
    // look when they would have. (Else what the program drops right after lies there for fifty times as long as this has taken.)
    if (look == IdleLook::WhateverTheShare)
        return m_nextIdleCollectionMayFindMore;
    m_bytesAllocatedAtIdleCollection = m_vm.heap.totalBytesAllocated();
    int64_t end = monotonicNanoseconds();
    m_durationOfLastIdleCollection = end - start;
    m_endOfLastIdleCollection = end;
    m_costOfLastIdleCollection = std::max(end - start, processorNanoseconds() - processorTimeAtStart);
    thread.waits.total = 0;
    // What only the objects that it found referred to is there to be found as soon as their threads have finalized them. So the
    // threads that wait look again soon.
    // - Not for what this thread has finalized: it has gone on collecting until it found no more of its own. A thread that drops
    //   something between one short wait and the next (it polls) finds something every time, which no finalizer has uncovered.
    // - Not for what a thread that works has left: that one leaves something to finalize all the time, has to stop for every
    //   collection, and collects itself as it allocates.
    // - Not for ever: one thread may poll and drop what another has made, which waits. What hangs together goes level by level
    //   for three levels, and all at once if it is deeper, but a level may take more than one collection: a thread has not made
    //   all its calls yet, or the stack of another thread that waits still has a word of what that one dealt with. So eight
    //   times, and then not again until they have found nothing twice in a row. (Once is not enough, for the same reasons.)
    bool foundSomething = std::exchange(m_lastIdleCollectionFoundSomething, m_threadThatWaitsOwesCalls) || m_threadThatWaitsOwesCalls;
    if (!foundSomething)
        m_earlyIdleCollectionsLeft = mostEarlyIdleCollections;
    m_nextIdleCollectionMayFindMore = foundSomething && m_earlyIdleCollectionsLeft;
    if (m_nextIdleCollectionMayFindMore)
        --m_earlyIdleCollectionsLeft;
    return m_nextIdleCollectionMayFindMore;
}

static bool isOfFunctionThatMayWait(CallFrame* call)
{
    JSCell* callee = call->jsCallee();
    return PyNativeFunction::is(callee) && (uncheckedDowncast<PyNativeFunction>(callee)->def().flags & MayWait);
}

NEVER_INLINE bool Threads::willCallFunctionThatMayWait(CallFrame* callFrame)
{
    // The first argument of a method that is not bound is the object whose method it is, which is only looked at.
    for (unsigned i = uncheckedDowncast<PyNativeFunction>(callFrame->jsCallee())->isDescriptor(); i < callFrame->argumentCount(); ++i) {
        if (callFrame->uncheckedArgument(i).isCell())
            callFrame = nullptr;
        if (!callFrame)
            break;
    }
    ThreadState& thread = current();
    thread.callThatMayBeginAgain = callFrame;
    // Left by a call that was begun again and then had no need to wait. The frame of this call may be where that one's was.
    thread.waitBegunAgain.call = nullptr;
    thread.waitBegunAgain.isRequested = false;
    return callFrame;
}

NEVER_INLINE Threads::CallAgain Threads::takeRequestToCallAgain(CallFrame* callFrame)
{
    ThreadState& thread = current();
    auto& begun = thread.waitBegunAgain;
    if (!std::exchange(begun.isRequested, false)) [[likely]]
        return CallAgain::No;
    ASSERT(begun.call == callFrame);
    if (begun.call != callFrame) [[unlikely]] {
        begun.call = nullptr;
        return CallAgain::No;
    }
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(m_vm);
    ASSERT(scope.exception() && (scope.exception()->value().isNull() || m_vm.hasPendingTerminationException()));
    // On the way here the host may have asked for the end of what runs, and that has taken the place of the request.
    if (!scope.tryClearException()) [[unlikely]] {
        begun.call = nullptr;
        return CallAgain::No;
    }
    thread.callThatMayBeginAgain = callFrame;
    return begun.collects ? CallAgain::AfterCollecting : CallAgain::OnWipedStack;
}

NEVER_INLINE bool Threads::collectBeforeCallBeginsAgain(JSGlobalObject* globalObject, CallFrame* callFrame)
{
    // The caller has no scope: see there. What a finalizer or the handler of a signal raises stays, for the caller of the function.
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(m_vm);
    ThreadState& thread = current();
    ASSERT(thread.waitBegunAgain.call == callFrame && thread.waitBegunAgain.collects);
    // Whatever waits meanwhile is not the call that is to begin again, though the frame on top is still that call's.
    auto begun = thread.waitBegunAgain;
    thread.waitBegunAgain.call = nullptr;
    thread.callThatMayBeginAgain = nullptr;
    if (collectWhileIdle(globalObject, begun.look))
        begun.idleInterval = IdleWait::firstInterval;
    if (scope.exception()) [[unlikely]]
        return false;
    thread.waitBegunAgain = begun;
    thread.callThatMayBeginAgain = callFrame;
    return true;
}

IdleWait::IdleWait(JSGlobalObject* globalObject, int64_t& deadline, bool isWaitOfCall)
    : m_globalObject(globalObject)
    , m_thread(runtime(globalObject).threads().current())
    , m_start(monotonicNanoseconds())
    , m_startOfWhatCounts(m_start)
{
    ++m_thread.idleWaits;
    if (isWaitOfCall) {
        ThreadState& thread = m_thread;
        // What is noted may be left by a call that had no need to wait in the end, and the frame of any later call may be where that
        // one's was. If that is a call of a function that may wait, willCallFunctionThatMayWait() has seen to it.
        CallFrame* call = std::exchange(thread.callThatMayBeginAgain, nullptr);
        auto& begun = thread.waitBegunAgain;
        if (call && call == getVM(globalObject).topCallFrame && isOfFunctionThatMayWait(call)) {
            m_call = call;
            if (begun.call == call) {
                deadline = begun.deadline;
                m_interval = begun.idleInterval;
                m_continuesWait = true;
                m_hasCollectedAsLastResort = begun.collects && begun.look == IdleLook::LastResort;
            }
        }
        begun.call = nullptr;
    }
    m_deadline = deadline;
    auto& waits = m_thread.waits;
    if (!m_continuesWait && m_start - waits.endOfLast > waits.lengthOfLast)
        waits.total = 0;
    m_wasDueAtStart = waits.total >= m_interval;
    m_timeToCollect = m_start + std::max<int64_t>(m_interval - waits.total, 0);
}

IdleWait::~IdleWait()
{
    --m_thread.idleWaits;
    auto& waits = m_thread.waits;
    int64_t now = monotonicNanoseconds();
    waits.total += now - m_startOfWhatCounts;
    // (The wait goes on.)
    if (m_beginsCallAgain)
        return;
    waits.endOfLast = now;
    waits.lengthOfLast = now - m_start;
}

bool IdleWait::beginsCallAgain(bool collects, IdleLook look)
{
    if (!m_call)
        return false;
    VM& vm = getVM(m_globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    ASSERT(m_call == vm.topCallFrame);
    m_thread.waitBegunAgain = { m_call, m_deadline, m_interval, true, collects, look };
    m_beginsCallAgain = true;
    throwException(m_globalObject, scope, Exception::create(vm, jsNull(), Exception::StackCaptureAction::DoNotCaptureStack));
    return true;
}

bool IdleWait::willBlock()
{
    return !beginsCallAgain(false, IdleLook::WithinShare);
}

bool IdleWait::collect(IdleLook look)
{
    auto scope = DECLARE_THROW_SCOPE(getVM(m_globalObject));
    Threads& threads = runtime(m_globalObject).threads();
    if (!threads.shouldCollectWhileIdle(m_globalObject, look)) {
        // Another thread has looked a moment ago, for all that wait.
        if (threads.m_nextIdleCollectionMayFindMore)
            m_interval = firstInterval;
        return true;
    }
    if (failed(scope, !beginsCallAgain(true, look)))
        return false;
    if (threads.collectWhileIdle(m_globalObject, look))
        m_interval = firstInterval;
    m_startOfWhatCounts = monotonicNanoseconds();
    return !scope.exception();
}

bool IdleWait::checkSignals()
{
    auto scope = DECLARE_THROW_SCOPE(getVM(m_globalObject));
    bool owesCalls = runtime(m_globalObject).hasPendingCalls();
    if (failed(scope, Python::checkSignals(m_globalObject)))
        return false;
    if (owesCalls) {
        m_interval = firstInterval;
        m_timeToCollect = std::min(m_timeToCollect, monotonicNanoseconds() + m_interval);
    }
    return true;
}

bool IdleWait::collectAsLastResort()
{
    return std::exchange(m_hasCollectedAsLastResort, true) || collect(IdleLook::LastResort);
}

bool IdleWait::collectBeforeBlocking(IdleLook look, bool& hasLooked)
{
    hasLooked = m_continuesWait;
    if (m_continuesWait || !runtime(m_globalObject).threads().shouldCollectWhileIdle(m_globalObject, look))
        return true;
    hasLooked = true;
    return collect(look);
}

int64_t IdleWait::nextPiece(int64_t remaining) const
{
    int64_t piece = std::max<int64_t>(m_timeToCollect - monotonicNanoseconds(), 0);
    return remaining < 0 ? piece : std::min(piece, remaining);
}

bool IdleWait::collectIfDue(int64_t remaining)
{
    auto scope = DECLARE_THROW_SCOPE(getVM(m_globalObject));
    if (monotonicNanoseconds() < m_timeToCollect)
        return true;
    // A wait that is nearly over is not made longer by a collection. The next one begins with it, however short it is: if all are
    // short, none would ever be long enough.
    if (remaining >= 0 && remaining < 2 * runtime(m_globalObject).threads().m_durationOfLastIdleCollection && !std::exchange(m_wasDueAtStart, false)) {
        m_timeToCollect = std::numeric_limits<int64_t>::max();
        return true;
    }
    // If the threads that wait have used up their share of the time, it is put off until they have some again, not left out: what
    // this thread has dropped since the last collection would otherwise wait four times as long.
    Threads& threads = runtime(m_globalObject).threads();
    if (!threads.shouldCollectWhileIdle(m_globalObject, IdleLook::WithinShare)) {
        if (threads.m_nextIdleCollectionMayFindMore)
            m_interval = firstInterval;
        m_timeToCollect = std::max(threads.timeOfNextIdleCollection(m_globalObject), monotonicNanoseconds() + firstInterval);
        return true;
    }
    // Less and less often: nothing happens while this waits, unless other threads run, and they collect as they allocate. But as long
    // as collections find something to finalize, there may be more: what only that referred to, once its thread has finalized it.
    m_interval *= 4;
    if (failed(scope, collect(IdleLook::WithinShare)))
        return false;
    m_timeToCollect = monotonicNanoseconds() + m_interval;
    return true;
}

bool IdleWait::didWait(int64_t remaining)
{
    VM& vm = getVM(m_globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    // A signal that came between two pieces has interrupted neither.
    if (failed(scope, checkSignals()))
        return false;
    if (monotonicNanoseconds() < m_timeToCollect)
        return true;
    if (runtime(m_globalObject).threads().throwIfStopping()) [[unlikely]] {
        scope.release();
        return false;
    }
    RELEASE_AND_RETURN(scope, collectIfDue(remaining));
}

bool Threads::waitUntil(JSGlobalObject* globalObject, const void* address, int64_t timeout, ASCIILiteral whatIsWaitedFor, ASCIILiteral whatCannotBeDone, const ScopedLambda<bool()>& isOver, bool isWaitOfCall)
{
    VM& vm = m_vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (throwIfStopping()) [[unlikely]] {
        scope.release();
        return false;
    }
    if (isOver())
        return true;
    if (!timeout)
        return false;
    if (isFinalized()) {
        // The other threads never run again.
        raise(globalObject, scope, runtime(globalObject).PythonFinalizationError(), makeString("cannot "_s, whatCannotBeDone));
        return false;
    }

    int64_t deadline = -1;
    if (timeout > 0 && __builtin_add_overflow(monotonicNanoseconds(), timeout, &deadline))
        deadline = std::numeric_limits<int64_t>::max();
    IdleWait idle(globalObject, deadline, isWaitOfCall);
    if (idle.continuesWait()) {
        if (deadline >= 0 && monotonicNanoseconds() >= deadline)
            return false;
    } else if (failed(scope, idle.willBlock()))
        return false;

    ThreadState& thread = current();
    while (true) {
        if (timeout < 0 && !m_runningCount) {
            // Only a finalizer can bring about what is waited for, which in CPython would have run by now.
            if (failed(scope, idle.collectAsLastResort()))
                return false;
            if (isOver())
                return true;
            // Or a signal. A program of Python sleeps until one comes, as it does in CPython. One of JS that has called into Python
            // would only hang: nothing of JS runs while this thread waits.
            if (!signalMayEndWait(globalObject)) {
                raiseRuntimeError(globalObject, scope, "deadlock: "_s, whatIsWaitedFor, ", and there is no other thread"_s);
                return false;
            }
        } else
            idle.canBeEnded();

        m_waiters.add(address, Deque<ThreadState*>()).iterator->value.append(&thread);
        thread.isQueued = true;
        {
            AllowThreads allow(globalObject);
            int64_t until = deadline >= 0 ? std::min(deadline, idle.timeToCollect()) : idle.timeToCollect();
            while (!thread.sleeper.sleep(until) && allow.wasInterruptedForNothing()) { }
        }
        if (thread.isQueued) {
            thread.isQueued = false;
            auto it = m_waiters.find(address);
            it->value.removeFirstMatching([&](ThreadState* other) {
                return other == &thread;
            });
            if (it->value.isEmpty())
                m_waiters.remove(it);
        }

        if (thread.isMain && m_vm.heap.hasPostponedEmbedderFinalizers()) [[unlikely]]
            m_vm.heap.runPostponedEmbedderFinalizers();
        bool canGoOn = idle.checkSignals();
        EXCEPTION_ASSERT(!scope.exception() == canGoOn);
        if (!canGoOn) {
            // This thread may have been woken for what it now leaves to others.
            wakeOne(address);
            return false;
        }
        if (isOver())
            return true;
        int64_t now = monotonicNanoseconds();
        if (deadline >= 0 && now >= deadline)
            return false;
        if (failed(scope, idle.collectIfDue(deadline >= 0 ? deadline - now : -1))) {
            wakeOne(address);
            return false;
        }
        if (isOver())
            return true;
    }
}

void Threads::wakeOne(const void* address)
{
    if (m_waiters.isEmpty())
        return;
    auto it = m_waiters.find(address);
    if (it == m_waiters.end())
        return;
    ThreadState* thread = it->value.takeFirst();
    if (it->value.isEmpty())
        m_waiters.remove(it);
    thread->isQueued = false;
    thread->sleeper.wake();
}

void Threads::wakeAll(const void* address)
{
    if (m_waiters.isEmpty())
        return;
    auto threads = m_waiters.take(address);
    for (ThreadState* thread : threads) {
        thread->isQueued = false;
        thread->sleeper.wake();
    }
}

bool ThreadLock::isHeldByThisThread(JSGlobalObject* globalObject) const
{
    return m_count && m_owner == runtime(globalObject).threads().current().serial;
}

bool ThreadLock::acquire(JSGlobalObject* globalObject)
{
    Threads& threads = runtime(globalObject).threads();
    uint64_t thisThread = threads.current().serial;
    if (m_count && m_owner != thisThread) {
        if (!threads.waitUntil(globalObject, this, -1, "a lock of the interpreter is held"_s, "acquire lock at interpreter finalization"_s, [&] { return !m_count; }))
            return false;
    }
    m_owner = thisThread;
    ++m_count;
    return true;
}

void ThreadLock::release(JSGlobalObject* globalObject)
{
    ASSERT(isHeldByThisThread(globalObject));
    if (--m_count)
        return;
    m_owner = 0;
    runtime(globalObject).threads().wakeOne(this);
}

void ThreadLock::holderEnds(Threads& threads, const ThreadState& thread)
{
    if (!m_count || m_owner != thread.serial)
        return;
    m_count = 0;
    m_owner = 0;
    threads.wakeOne(this);
}

bool ensureMainThreadForJS(JSGlobalObject* globalObject)
{
    if (runtime(globalObject).threads().isMainThread()) [[likely]]
        return true;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    raiseRuntimeError(globalObject, scope, "JavaScript can only be used from the main thread"_s);
    return false;
}

} // namespace Python
} // namespace JSC
