/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyThreads.h"

#include "FrameTracers.h"
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

void Sleeper::sleep(int64_t deadline)
{
    struct timespec time;
    if (deadline >= 0) {
        time.tv_sec = deadline / 1000000000;
        time.tv_nsec = deadline % 1000000000;
    }
    while (!m_isWoken.exchange(0)) {
        // The time is absolute, on the monotonic clock.
        long result = syscall(SYS_futex, &m_isWoken, FUTEX_WAIT_BITSET | FUTEX_PRIVATE_FLAG, 0, deadline >= 0 ? &time : nullptr, nullptr, FUTEX_BITSET_MATCH_ANY);
        if (result < 0 && errno != EAGAIN) {
            // Time is up, or a signal arrived.
            m_isWoken.store(0);
            return;
        }
    }
}

void Sleeper::wake()
{
    int savedErrno = errno;
    m_isWoken.store(1);
    syscall(SYS_futex, &m_isWoken, FUTEX_WAKE | FUTEX_PRIVATE_FLAG, 1, nullptr, nullptr, 0);
    errno = savedErrno;
}

#else

void Sleeper::sleep(int64_t deadline)
{
    // A signal handler cannot notify a condition. It sets the flag, which is looked at this often.
    constexpr int64_t slice = 20000000;
    Locker locker { m_lock };
    while (!m_isWoken.exchange(false)) {
        int64_t now = monotonicNanoseconds();
        if (deadline >= 0 && now >= deadline)
            return;
        int64_t time = deadline >= 0 ? std::min(deadline - now, slice) : slice;
        m_condition.waitFor(m_lock, Seconds::fromNanoseconds(static_cast<double>(time)));
    }
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

static thread_local ThreadState* t_currentThread;
static std::atomic<Sleeper*> s_sleeperOfMainThreadOfProcess;

Ref<Threads> Threads::sharedFor(VM& vm)
{
    if (!vm.heap.pythonRuntimes().isEmpty())
        return vm.heap.pythonRuntimes().first()->threads();
    return adoptRef(*new Threads(vm));
}

Threads::Threads(VM& vm)
    : m_vm(vm)
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
    return t_currentThread && !t_currentThread->isMain;
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
    if (t_currentThread) [[likely]]
        return *t_currentThread;
    // Every other thread has had one since before it ran.
    ASSERT(!m_isEnabled);
    ASSERT(m_vm.currentThreadIsHoldingAPILock());
    Ref thread = ThreadState::create();
    thread->ident = currentThreadIdent();
    thread->isMain = thread->ident == m_mainThreadIdent;
    t_currentThread = thread.ptr();
    if (thread->isMain && isMainThreadOfProcess())
        s_sleeperOfMainThreadOfProcess.store(&thread->sleeper);
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
        m_switchCondition.notifyAll();
        m_gilCondition.notifyOne();
        while (!m_isStopping.load())
            m_stopCondition.wait(m_gilLock);
    }
    ++m_waitingForGIL;
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
    m_switchCondition.notifyAll();
    m_gilLock.unlock();
}

void Threads::dropGIL(ThreadState&, bool isYielding)
{
    Locker locker { m_gilLock };
    ASSERT(m_gilIsHeld);
    m_gilIsHeld = false;
    m_gilCondition.notifyOne();
    if (!isYielding)
        return;
    // This thread is about to ask for it again, and would as likely as not get it before the one that was notified wakes up.
    uint64_t switchNumber = m_switchNumber;
    while (m_switchNumber == switchNumber && m_waitingForGIL && !m_isFinalized.load())
        m_switchCondition.wait(m_gilLock);
}

void Threads::release(ThreadState& thread, JSLock::ParkedThread& parked, SavedStates& saved)
{
    ASSERT(&thread == t_currentThread);
    auto& runtimes = m_vm.heap.pythonRuntimes();
    RELEASE_ASSERT(runtimes.size() <= SavedStates::capacity);
    for (unsigned i = 0; i < runtimes.size(); ++i) {
        saved.entries[i].runtime = runtimes[i];
        runtimes[i]->takeThreadLocalState(saved.entries[i].state);
    }
    thread.parked = &parked;
    thread.saved = &saved;
    if (m_vm.heap.hasPostponedEmbedderFinalizers() && !thread.isMain && !m_didTellMainThreadOfFinalizers) [[unlikely]]
        tellMainThreadOfFinalizers();
    m_vm.apiLock().park(parked);
}

void Threads::acquire(ThreadState& thread, JSLock::ParkedThread& parked, SavedStates& saved)
{
    m_vm.apiLock().unpark(parked);
    thread.parked = nullptr;
    thread.saved = nullptr;
    // Now that it is back in the VM nobody hands out calls: that takes the GIL, or a collector that has stopped the VM's thread.
    thread.losesNothingIfInterrupted = false;
#if !OS(WINDOWS)
    if (thread.isBeingInterrupted.load(std::memory_order_relaxed)) [[unlikely]]
        stopInterrupting(thread);
#endif
    for (auto& entry : saved.entries) {
        // A runtime goes away with its global object, which something on this thread's stack may well have kept.
        if (entry.runtime && m_vm.heap.pythonRuntimes().contains(entry.runtime))
            entry.runtime->putThreadLocalState(entry.state);
        entry.runtime = nullptr;
    }
    didTakeGIL(thread);
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
            SuspendExceptionScope suspended(m_vm);
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
        if (thread->number() == number && thread->isWaiting())
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
    // Twice as long each time. A thread that has the signal blocked, or whose system calls begin again by themselves
    // (signal.siginterrupt()), is soon left alone, and one that was kept from its system call for long is reached all the same.
    VMTraps::queue().dispatchAfter(pause, [protectedThis = Ref { *this }, thread = Ref { thread }, pause, time = thread.timesInterrupted] {
        Locker locker { protectedThis->m_interruptLock };
        if (!thread->isBeingInterrupted.load() || thread->timesInterrupted != time)
            return;
        protectedThis->sendInterrupt(thread.get());
        protectedThis->interruptAgainAfter(2 * pause, thread.get());
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

void stopThreads(VM& vm)
{
    if (!vm.heap.pythonRuntimes().isEmpty())
        vm.heap.pythonRuntimes().first()->threads().stopAll();
}

bool callWithoutGIL(VM& vm, void (*function)(void*), void* argument)
{
    if (vm.heap.pythonRuntimes().isEmpty())
        return false;
    Runtime& rt = *vm.heap.pythonRuntimes().first();
    if (!rt.threads().isEnabled())
        return false;
    AllowThreads allow(rt.globalObject());
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
        for (Runtime* runtime : Vector<Runtime*, 1>(m_vm.heap.pythonRuntimes())) {
            if (m_vm.heap.pythonRuntimes().contains(runtime) && runtime->hasPendingCalls())
                runtime->runPendingCalls();
        }
        for (Runtime* runtime : m_vm.heap.pythonRuntimes()) {
            Runtime::ThreadLocalState state;
            runtime->takeThreadLocalState(state);
            if (state.traceFunction || state.profileFunction)
                runtime->monitoring().tracingThreadCountChanged(-!!state.traceFunction, -!!state.profileFunction);
        }
        --m_runningCount;
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
bool Threads::shouldCollectWhileIdle(JSGlobalObject* globalObject, bool isLastResort)
{
    if (runtime(globalObject).isRunningPendingCalls())
        return false;
    // Such a collection stops every thread, and any number of threads may be idle while one works: all of them together spend
    // at most a twentieth of the time on it. If nothing has been allocated since the last time, there is little to find, but
    // the count moves in steps, and to drop something takes no allocation at all: then a fiftieth. If the last one found something
    // to finalize for a thread that waits, there may be more: then half.
    int64_t earliest = m_timeForIdleCollectionWithoutCause;
    if (m_nextIdleCollectionMayFindMore)
        earliest -= 49 * m_durationOfLastIdleCollection;
    else if (m_vm.heap.totalBytesAllocated() != m_bytesAllocatedAtIdleCollection)
        earliest -= 30 * m_durationOfLastIdleCollection;
    return isLastResort || monotonicNanoseconds() >= earliest;
}

bool Threads::collectWhileIdle(JSGlobalObject* globalObject, bool isLastResort)
{
    int64_t start = monotonicNanoseconds();
    ThreadState& thread = current();
    // (The call whose wait this is may have been left, to begin again afterwards.)
    ++thread.idleWaits;
    m_threadThatWaitsOwesCalls = false;
    // What reference counts would have freed would have been finalized by now. Cyclic garbage would not: CPython's collector does not
    // run in a thread that waits, which may well hold a lock that a finalizer wants.
    // It makes the calls that are its own. If nothing else can bring about what it waits for, all.
    collectAndFinalize(globalObject, nullptr, CollectionScope::Full, !isLastResort, isLastResort ? Runtime::CallsOf::AllThreads : Runtime::CallsOf::ThisThread);
    m_bytesAllocatedAtIdleCollection = m_vm.heap.totalBytesAllocated();
    int64_t end = monotonicNanoseconds();
    m_durationOfLastIdleCollection = end - start;
    m_timeForIdleCollectionWithoutCause = end + 50 * (end - start);
    --thread.idleWaits;
    thread.waits.total = 0;
    // What only the objects that it found referred to is there to be found as soon as their threads have finalized them. So the
    // threads that wait look again soon. Not for what a thread that works has left: that one leaves something to finalize all the
    // time, has to stop for every collection, and collects itself as it allocates.
    m_nextIdleCollectionMayFindMore = m_threadThatWaitsOwesCalls;
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
    ASSERT(scope.exception() && scope.exception()->value().isNull());
    scope.clearException();
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
    if (collectWhileIdle(globalObject, begun.isLastResort))
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
                m_hasCollectedAsLastResort = begun.collects && begun.isLastResort;
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

bool IdleWait::beginsCallAgain(bool collects, bool isLastResort)
{
    if (!m_call)
        return false;
    VM& vm = getVM(m_globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    ASSERT(m_call == vm.topCallFrame);
    m_thread.waitBegunAgain = { m_call, m_deadline, m_interval, true, collects, isLastResort };
    m_beginsCallAgain = true;
    throwException(m_globalObject, scope, Exception::create(vm, jsNull(), Exception::StackCaptureAction::DoNotCaptureStack));
    return true;
}

bool IdleWait::willBlock()
{
    return !beginsCallAgain(false, false);
}

bool IdleWait::collect(bool isLastResort)
{
    auto scope = DECLARE_THROW_SCOPE(getVM(m_globalObject));
    Threads& threads = runtime(m_globalObject).threads();
    if (!threads.shouldCollectWhileIdle(m_globalObject, isLastResort)) {
        // Another thread has looked a moment ago, for all that wait.
        if (threads.m_nextIdleCollectionMayFindMore)
            m_interval = firstInterval;
        return true;
    }
    if (failed(scope, !beginsCallAgain(true, isLastResort)))
        return false;
    if (threads.collectWhileIdle(m_globalObject, isLastResort))
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
    return std::exchange(m_hasCollectedAsLastResort, true) || collect(true);
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
    // Less and less often: nothing happens while this waits, unless other threads run, and they collect as they allocate. But as long
    // as collections find something to finalize, there may be more: what only that referred to, once its thread has finalized it.
    m_interval *= 4;
    if (failed(scope, collect(false)))
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
            thread.sleeper.sleep(deadline >= 0 ? std::min(deadline, idle.timeToCollect()) : idle.timeToCollect());
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

        if (thread.isMain) {
            if (m_vm.heap.hasPostponedEmbedderFinalizers()) [[unlikely]]
                m_vm.heap.runPostponedEmbedderFinalizers();
            // The host wants whatever runs here ended. Nothing else looks while this waits.
            constexpr auto endingTraps = VMTraps::NeedTermination | VMTraps::NeedWatchdogCheck;
            if (vm.traps().needHandling(endingTraps) && vm.entryScope) [[unlikely]] {
                vm.traps().handleTraps(endingTraps);
                if (scope.exception()) {
                    wakeOne(address);
                    return false;
                }
            }
        }
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
