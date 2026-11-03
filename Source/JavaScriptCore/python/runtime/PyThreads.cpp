/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyThreads.h"

#include "JSCInlines.h"
#include "PyErrors.h"
#include "PyEventWait.h"
#include "PyExec.h"
#include "PyHooks.h"
#include "PyInternals.h"
#include "PyMonitoring.h"
#include "VMTrapsInlines.h"
#include <wtf/Threading.h>

#if !OS(WINDOWS)
#include <pthread.h>
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

// The first thread is about to be started, by the only one there is, which holds the GIL from now on.
void Threads::enable()
{
    ThreadState& thread = current();
    Locker locker { m_gilLock };
    m_gilIsHeld = true;
    m_lastHolder = &thread;
    m_isEnabled = true;
}

// ---- The GIL ----

void Threads::sleepForEver()
{
    while (true)
        WTF::sleep(Seconds::infinity());
}

void Threads::takeGIL(ThreadState& thread)
{
    m_gilLock.lock();
    ++m_waitingForGIL;
    while (m_gilIsHeld && (thread.isMain || !m_isFinalized.load())) {
        uint64_t switchNumber = m_switchNumber;
        bool wasNotified = m_gilCondition.waitFor(m_gilLock, Seconds::fromNanoseconds(static_cast<double>(switchInterval())));
        // Whoever has it has had it all this time.
        if (!wasNotified && m_gilIsHeld && m_switchNumber == switchNumber) {
            // It may be busy with other things than Python code: the host's event loop, say.
            m_dropIsRequested.store(true);
            noteCallsOwed(m_vm);
        }
    }
    --m_waitingForGIL;
    if (!thread.isMain && m_isFinalized.load()) [[unlikely]] {
        // Somebody may be waiting for this thread to take it, and somebody else for it to be free.
        m_switchCondition.notifyAll();
        m_gilCondition.notifyOne();
        m_gilLock.unlock();
        sleepForEver();
    }
    m_gilIsHeld = true;
    if (m_lastHolder != &thread) {
        m_lastHolder = &thread;
        ++m_switchNumber;
    }
    m_dropIsRequested.store(false);
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
    // The host's objects belong to the main thread, dead ones too.
    m_vm.heap.setMayRunEmbedderFinalizers(thread.isMain);
    if (thread.isMain)
        m_didTellMainThreadOfFinalizers = false;
}

// This thread has come across dead objects of the host, which only the main thread may sweep. That one does so at its next safe
// point (makeCallsOwed()). If it sleeps, in a wait for other threads or in the host's event loop, it is woken up for it.
void Threads::tellMainThreadOfFinalizers()
{
    m_didTellMainThreadOfFinalizers = true;
    for (auto& thread : m_threads) {
        if (thread->isMain)
            thread->sleeper.wake();
    }
    noteCallsOwed(m_vm);
}

void AllowThreads::release(Threads& threads)
{
    m_threads = &threads;
    m_thread = &threads.current();
    auto* luggage = new (NotNull, m_luggage) Luggage;
    threads.release(*m_thread, luggage->parked, luggage->saved);
    threads.dropGIL(*m_thread, false);
}

void AllowThreads::acquire()
{
    int savedErrno = errno;
    auto* luggage = std::launder(reinterpret_cast<Luggage*>(m_luggage));
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
    takeGIL(thread);
    {
        // The thread that gave up the GIL left the VM and the runtimes as they are when nothing runs.
        JSLockHolder locker(m_vm);
        didTakeGIL(thread);
        body();
        body = nullptr;

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
        if (m_vm.heap.hasPostponedEmbedderFinalizers() && !m_didTellMainThreadOfFinalizers) [[unlikely]]
            tellMainThreadOfFinalizers();
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
void Threads::collectWhileIdle(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    if (rt.isRunningPendingCalls() || m_vm.heap.totalBytesAllocated() == m_bytesAllocatedAtIdleCollection)
        return;
    collectAndFinalize(globalObject);
    m_bytesAllocatedAtIdleCollection = m_vm.heap.totalBytesAllocated();
}

bool Threads::waitUntil(JSGlobalObject* globalObject, const void* address, int64_t timeout, ASCIILiteral whatIsWaitedFor, ASCIILiteral whatCannotBeDone, const ScopedLambda<bool()>& isOver)
{
    VM& vm = m_vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
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

    int64_t timeToIdleCollection = 50000000;
    int64_t idleCollectionTime = monotonicNanoseconds() + timeToIdleCollection;

    ThreadState& thread = current();
    while (true) {
        if (timeout < 0 && !m_runningCount && !signalMayBeHandled(globalObject)) {
            // CPython would hang until it is interrupted from the keyboard.
            raiseRuntimeError(globalObject, scope, "deadlock: "_s, whatIsWaitedFor, ", and there is no other thread"_s);
            return false;
        }

        m_waiters.add(address, Deque<ThreadState*>()).iterator->value.append(&thread);
        thread.isQueued = true;
        {
            AllowThreads allow(globalObject);
            thread.sleeper.sleep(deadline >= 0 ? std::min(deadline, idleCollectionTime) : idleCollectionTime);
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
            bool canGoOn = checkSignals(globalObject);
            EXCEPTION_ASSERT(!scope.exception() == canGoOn);
            if (!canGoOn) {
                // This thread may have been woken for what it now leaves to others.
                wakeOne(address);
                return false;
            }
        }
        if (isOver())
            return true;
        int64_t now = monotonicNanoseconds();
        if (deadline >= 0 && now >= deadline)
            return false;
        if (now >= idleCollectionTime) {
            collectWhileIdle(globalObject);
            if (isOver())
                return true;
            // Less and less often: the other threads collect as they allocate.
            timeToIdleCollection *= 4;
            idleCollectionTime = monotonicNanoseconds() + timeToIdleCollection;
        }
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
