/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include "JSCInlines.h"
#include "PyAbstract.h"
#include "PyErrors.h"
#include "PyException.h"
#include "PyExec.h"
#include "PyHooks.h"
#include "PyInternals.h"
#include "PyList.h"
#include "PyModule.h"
#include "PyNumbers.h"
#include "PySet.h"
#include "PyStr.h"
#include "PyStructSeq.h"
#include "PyThreads.h"
#include "VMTraps.h"
#include <atomic>
#include <wtf/Lock.h>
#include <wtf/WTFConfig.h>

#if !OS(WINDOWS)
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#if OS(LINUX)
#include <sys/syscall.h>
#endif

namespace JSC {
namespace Python {

// Signals, and the module _signal. (Modules/signalmodule.c in CPython is the reference.)
//
// What the system calls when a signal arrives (signalArrived) only takes note of it: a flag for the signal, a byte to the
// wakeup descriptor, and VMTraps::NeedPythonCalls. The function Python code gave for the signal is called at the next safe
// point (makeCallsOwed), or by a function written in C++ that was interrupted while it waited (checkSignals), and what it
// raises is raised there.
//
// Signals belong to the process. The engine and Bun have many threads that the program knows nothing of, which block no signals,
// so the system may pick any of them for a signal sent to the process. Two things make the program see what it would see if
// its threads were the only ones:
//   - Such a thread, when a signal arrives on it, blocks that signal for good and sends it to the process again. So the signal
//     ends up on a thread of Python's that does not block it, whose system call it interrupts, or stays pending for the
//     process if they all block it, where sigwait() in any of them finds it.
//   - That takes a function to be called. So while the main thread blocks a signal that is to have its default effect, the
//     signal is "shielded": it has our function instead, until it is unblocked. Otherwise another thread would take the signal
//     and the process would end (or the signal would be dropped) while the program waits for it in sigwait().

#if !OS(WINDOWS)

static constexpr int signalLimit = NSIG;

namespace {

struct SignalState {
    std::atomic<bool> isTripped { false };
    // (There is no signal 0: that one is a request for a traceback.)
    std::atomic<bool> tripped[signalLimit] { };
    std::atomic<bool> isShielded[signalLimit] { };
    // Whether signalArrived() is what the system calls for it.
    bool isOurs[signalLimit] { };
    // faulthandler.register()
    bool isWatched[signalLimit] { };
    bool watchChains[signalLimit] { };
    struct sigaction actionBeforeWatch[signalLimit] { };
    // What the host does about a signal before the process dies of it, if it does something: see signalIsHandledByHostAsByDefault().
    bool hasHostAction[signalLimit] { };
    struct sigaction hostAction[signalLimit] { };
    // Sent to the thread to get it out of a system call. 0: not set up.
    std::atomic<int> wakeSignal { 0 };
    // Real-time signals queue. There is never more than one of these.
    std::atomic<bool> isWakeOnItsWay { false };
    // Held while it is sent. WithoutWakeSignal
    Lock wakeLock;
    unsigned wakesHeldBack WTF_GUARDED_BY_LOCK(wakeLock) { 0 };
    std::atomic<int> wakeupDescriptor { -1 };
    std::atomic<bool> warnsOnFullBuffer { true };
    // The errno of a write to the wakeup descriptor that failed, until it has been reported.
    std::atomic<int> wakeupError { 0 };
    // Of the thread that handles signals. Null: there is none (yet, or any more).
    std::atomic<VM*> vm { nullptr };
    Runtime* runtime { nullptr };
    pthread_t thread { };
};

} // namespace

static SignalState s_signalState;

// ---- What runs when a signal arrives: only what is safe there ----

static void signalTrip(int number)
{
    s_signalState.tripped[number].store(true);
    // After the flag of the signal: it is cleared before that one is looked at.
    s_signalState.isTripped.store(true);
    if (VM* vm = s_signalState.vm.load())
        noteCallsOwed(*vm);
    Threads::wakeMainThreadOfProcess();

    // Last, so that whoever the byte wakes up finds the flags set.
    int descriptor = s_signalState.wakeupDescriptor.load();
    if (descriptor < 0)
        return;
    uint8_t byte = static_cast<uint8_t>(number);
    ssize_t result;
    do {
        result = write(descriptor, &byte, 1);
    } while (result < 0 && errno == EINTR);
    if (result < 0 && (s_signalState.warnsOnFullBuffer.load() || (errno != EWOULDBLOCK && errno != EAGAIN)))
        s_signalState.wakeupError.store(errno);
}

// Stands for signalArrivedOnThread() where a function of one argument is expected. Never called.
static void signalArrived(int) { }
static void signalArrivedOnThread(int, siginfo_t*, void*);

static void signalSetAction(int number, void (*function)(int), int& error)
{
    if (function == SIG_DFL && s_signalState.hasHostAction[number]) {
        error = sigaction(number, &s_signalState.hostAction[number], nullptr) ? errno : 0;
        return;
    }
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    sigemptyset(&action.sa_mask);
    // No SA_RESTART: a system call that is interrupted returns, and its caller runs the handler before trying again.
    action.sa_flags = SA_ONSTACK;
    if (function == signalArrived) {
        action.sa_sigaction = signalArrivedOnThread;
        action.sa_flags |= SA_SIGINFO;
    } else
        action.sa_handler = function;
    error = sigaction(number, &action, nullptr) ? errno : 0;
}

static bool signalActionIsOurs(const struct sigaction& action) { return (action.sa_flags & SA_SIGINFO) && action.sa_sigaction == signalArrivedOnThread; }

static void signalSetAction(int number, void (*function)(int))
{
    int error;
    signalSetAction(number, function, error);
    if (!error)
        s_signalState.isOurs[number] = function == signalArrived;
}

static void signalArrivedOnThread(int number, siginfo_t*, void* context)
{
    int savedErrno = errno;
    if (!pthread_equal(pthread_self(), s_signalState.thread) && !Threads::isOnStartedThread()) {
        // The mask in the context is what the thread has when this returns.
        sigaddset(&static_cast<ucontext_t*>(context)->uc_sigmask, number);
        kill(getpid(), number);
    } else if (s_signalState.isShielded[number].load()) {
        // It was unblocked by other means than pthread_sigmask() below. It is blocked while this function runs, and has its
        // effect when this returns.
        s_signalState.isShielded[number].store(false);
        signalSetAction(number, SIG_DFL);
        ::raise(number);
    } else
        signalTrip(number);
    errno = savedErrno;
}

// ---- The handlers ----

static bool signalIsOnMainThreadOfProcess()
{
#if OS(LINUX)
    return syscall(SYS_gettid) == getpid();
#elif OS(DARWIN)
    return pthread_main_np();
#else
    return true;
#endif
}

// A host may have a handler of its own for the signals that tell of a fault, which reports the crash and lets the process die of
// the signal, and for those that end a program, which puts the terminal in order first. To a program that is what the signal does by
// default: getsignal() says SIG_DFL, so that what signal() returns can be given back to it, SIG_DFL puts the handler of the host
// back, and SIGINT raises KeyboardInterrupt.
static bool signalIsHandledByHostAsByDefault(int number, const struct sigaction& action)
{
    switch (number) {
    case SIGINT:
    case SIGTERM:
    case SIGILL:
    case SIGTRAP:
    case SIGABRT:
    case SIGBUS:
    case SIGFPE:
    case SIGSEGV:
    case SIGSYS:
        return !signalActionIsOurs(action);
    default:
        return false;
    }
}

static ALWAYS_INLINE bool signalCanHandle(JSGlobalObject* globalObject) { return s_signalState.runtime == &runtime(globalObject); }
// Handlers run on the main thread, and only there can they be set.
static ALWAYS_INLINE bool signalIsOnHandlingThread(JSGlobalObject* globalObject) { return signalCanHandle(globalObject) && pthread_equal(pthread_self(), s_signalState.thread); }

static constexpr int signalDefault = 0; // SIG_DFL
static constexpr int signalIgnore = 1; // SIG_IGN

static ALWAYS_INLINE bool signalIsAction(JSValue handler, int action) { return isSmallInt(handler) && smallIntValue(handler) == action; }
static ALWAYS_INLINE bool signalIsFunction(JSValue handler) { return !isNone(handler) && !signalIsAction(handler, signalDefault) && !signalIsAction(handler, signalIgnore); }

// Three signals are sent to threads of Python behind the program's back. 0: there is none, or not for this thread.
// The engine suspends and resumes threads with this one.
static ALWAYS_INLINE int signalThatSuspends()
{
#if OS(DARWIN)
    return 0;
#else
    return g_wtfConfig.sigThreadSuspendResume;
#endif
}

static ALWAYS_INLINE int signalThatStops() { return Threads::isOnStartedThread() ? signalThatInterruptsThreads() : 0; }

// This one gets the main thread out of a system call. Where it would end the process by default, it is kept from the program.
static ALWAYS_INLINE int signalThatWakes()
{
#if OS(LINUX)
    return s_signalState.wakeSignal.load(std::memory_order_relaxed);
#else
    return 0;
#endif
}

// (Whichever interpreter it is that runs on that thread.)
static ALWAYS_INLINE int signalThatWakesThisThread()
{
    int number = signalThatWakes();
    return number && pthread_equal(pthread_self(), s_signalState.thread) ? number : 0;
}

// What it does when it arrives is not the program's to say.
static ALWAYS_INLINE bool signalIsReserved(int number) { return number == signalThatSuspends() || number == signalThatWakes(); }

// Whether there was one of them in the set.
static bool signalRemove(sigset_t& set, std::initializer_list<int> numbers)
{
    bool removed = false;
    for (int number : numbers) {
        if (number && sigismember(&set, number) == 1) {
            sigdelset(&set, number);
            removed = true;
        }
    }
    return removed;
}

// A list with an item for each signal: what signal.getsignal() returns.
static JSArray* signalHandlers(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    if (JSValue list = rt.slot(Runtime::Slot::SignalHandlers))
        return asList(list);

    if (!s_signalState.runtime && signalIsOnMainThreadOfProcess()) {
        s_signalState.runtime = &rt;
        s_signalState.thread = pthread_self();
        s_signalState.vm.store(&rt.vm());
    }

    MarkedArgumentBuffer handlers;
    handlers.append(jsUndefined());
    for (int number = 1; number < signalLimit; ++number) {
        // None: it is handled, but not by Python.
        JSValue handler = jsUndefined();
        struct sigaction action;
        if (!sigaction(number, nullptr, &action)) {
            bool isFunction = action.sa_flags & SA_SIGINFO;
            if (!isFunction && action.sa_handler == SIG_DFL)
                handler = jsNumber(signalDefault);
            else if (!isFunction && action.sa_handler == SIG_IGN)
                handler = jsNumber(signalIgnore);
            else if (s_signalState.runtime == &rt && signalIsHandledByHostAsByDefault(number, action)) {
                s_signalState.hostAction[number] = action;
                s_signalState.hasHostAction[number] = true;
                handler = jsNumber(signalDefault);
            }
        }
        handlers.append(handler);
    }
    JSArray* list = newList(globalObject, spanOf(handlers));
    rt.setSlot(Runtime::Slot::SignalHandlers, list);
    return list;
}

static NEVER_INLINE bool signalRunHandlers(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    // Before the flags of the signals are looked at: one that arrives from here on sets it again.
    s_signalState.isTripped.store(false);

    if (int error = s_signalState.wakeupError.exchange(0)) {
        raiseOSError(globalObject, scope, error);
        reportUnraisable(globalObject, "Exception ignored while trying to write to the signal wakeup fd"_s, { });
    }

    if (s_signalState.tripped[0].exchange(false))
        dumpTracebackForSignal(globalObject, 0);

    JSArray* handlers = signalHandlers(globalObject);
    for (int number = 1; number < signalLimit; ++number) {
        if (!s_signalState.tripped[number].exchange(false))
            continue;
        if (s_signalState.isWatched[number]) {
            dumpTracebackForSignal(globalObject, number);
            if (!s_signalState.watchChains[number])
                continue;
            auto& before = s_signalState.actionBeforeWatch[number];
            if (!signalActionIsOurs(before)) {
                struct sigaction watching;
                sigaction(number, &before, &watching);
                ::raise(number);
                sigaction(number, &watching, nullptr);
                continue;
            }
        }
        JSValue handler = listGet(handlers, number);
        if (!signalIsFunction(handler)) {
            // It was changed after the signal arrived.
            raise(globalObject, scope, rt.OSError(), makeString("Signal "_s, number, " ignored due to race condition"_s));
            reportUnraisable(globalObject, "Exception ignored while calling signal handler"_s, { });
            continue;
        }
        PyFrame* frame = currentFrame(globalObject);
        call(globalObject, handler, jsNumber(number), frame ? JSValue(frame) : jsUndefined());
        if (scope.exception()) [[unlikely]] {
            // The rest is for the next time.
            s_signalState.isTripped.store(true);
            noteCallsOwed(vm);
            return false;
        }
    }
    return true;
}

bool checkSignals(JSGlobalObject* globalObject)
{
    // What interrupted the thread may have been the end of the VM.
    Runtime& rt = runtime(globalObject);
    if (rt.threads().throwIfStopping()) [[unlikely]]
        return false;
    // Or that it owes calls: see Threads::wakeToMakeCalls(). (In CPython this is a place where the collector runs.)
    if (rt.hasPendingCalls() && !rt.isRunningPendingCalls()) [[unlikely]] {
        rt.runPendingCalls(true);
        if (rt.vm().exceptionForInspection()) [[unlikely]]
            return false;
    }
    return checkOnlySignals(globalObject);
}

bool checkOnlySignals(JSGlobalObject* globalObject)
{
    if (runtime(globalObject).threads().throwIfStopping()) [[unlikely]]
        return false;
    if (!s_signalState.isTripped.load(std::memory_order_relaxed)) [[likely]]
        return true;
    if (!signalIsOnHandlingThread(globalObject))
        return true;
    return signalRunHandlers(globalObject);
}

static JSValue signalDefaultIntHandler(JSGlobalObject* globalObject, JSValue numberValue, JSValue)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    toCInt(globalObject, numberValue);
    RETURN_IF_EXCEPTION(scope, { });
    return raiseWithArgs(globalObject, scope, rt.KeyboardInterrupt(), { });
}

bool signalMayBeHandled(JSGlobalObject* globalObject)
{
    if (!signalCanHandle(globalObject))
        return false;
    JSArray* handlers = signalHandlers(globalObject);
    for (int number = 1; number < signalLimit; ++number) {
        JSValue handler = listGet(handlers, number);
        if (!signalIsFunction(handler))
            continue;
        auto* native = PyNativeFunction::tryFrom(handler);
        if (!native || native->def().pointer.f2 != signalDefaultIntHandler)
            return true;
    }
    return false;
}

bool simulateSignal(JSGlobalObject* globalObject, int number)
{
    if (number < 1 || number >= signalLimit)
        return false;
    if (!signalCanHandle(globalObject))
        return true;
    JSValue handler = listGet(signalHandlers(globalObject), number);
    if (!signalIsAction(handler, signalDefault) && !signalIsAction(handler, signalIgnore))
        signalTrip(number);
    return true;
}

int watchSignal(JSGlobalObject* globalObject, int number, bool chains)
{
    signalHandlers(globalObject);
    if (!signalCanHandle(globalObject) || signalIsReserved(number))
        return EINVAL;
    if (!s_signalState.isWatched[number]) {
        if (sigaction(number, nullptr, &s_signalState.actionBeforeWatch[number]))
            return errno;
        int error;
        signalSetAction(number, signalArrived, error);
        if (error)
            return error;
        // A shield stands for what the signal does by default.
        if (s_signalState.isShielded[number].exchange(false)) {
            s_signalState.actionBeforeWatch[number].sa_flags &= ~SA_SIGINFO;
            s_signalState.actionBeforeWatch[number].sa_handler = SIG_DFL;
        }
    }
    s_signalState.isWatched[number] = true;
    s_signalState.watchChains[number] = chains;
    return 0;
}

void unwatchSignal(int number)
{
    if (std::exchange(s_signalState.isWatched[number], false))
        sigaction(number, &s_signalState.actionBeforeWatch[number], nullptr);
}

static void signalWake(int) { s_signalState.isWakeOnItsWay.store(false); }

// 0: there is none to be had.
static int signalChooseOneThatWakes(bool canUnblock)
{
#if OS(LINUX)
    // It may be on its way at any time, and by default it would end the process. So it is one that nobody has done anything about
    // so far, and from here on nobody can: signalIsReserved(). If there is none, one that has only been blocked, or else one that
    // is ignored. Not one that waits to be taken.
    sigset_t blocked, pending;
    pthread_sigmask(SIG_BLOCK, nullptr, &blocked);
    sigpending(&pending);
    int chosen = 0;
    unsigned best = std::numeric_limits<unsigned>::max();
    for (int number = SIGRTMAX - 2; number >= SIGRTMIN; --number) {
        struct sigaction action;
        if (signalIsReserved(number) || sigaction(number, nullptr, &action))
            continue;
        bool isShielded = s_signalState.isShielded[number].load();
        // (What another thread than the one it is sent to blocks does not matter.)
        bool isBlocked = isShielded || (canUnblock && sigismember(&blocked, number) == 1);
        if (isBlocked && (!canUnblock || sigismember(&pending, number) == 1))
            continue;
        bool isFunction = action.sa_flags & SA_SIGINFO;
        unsigned rank;
        if (isShielded || (!isFunction && action.sa_handler == SIG_DFL))
            rank = isBlocked;
        else if (!isFunction && action.sa_handler == SIG_IGN)
            rank = 2;
        else
            continue;
        if (rank < best) {
            best = rank;
            chosen = number;
        }
    }
    return chosen;
#else
    UNUSED_PARAM(canUnblock);
    return SIGURG;
#endif
}

void prepareForTracebackDumpRequests(JSGlobalObject* globalObject)
{
    signalHandlers(globalObject);
    if (!signalCanHandle(globalObject))
        return;
    bool isOnHandlingThread = signalIsOnHandlingThread(globalObject);
    if (!s_signalState.wakeSignal.load()) {
        int number = signalChooseOneThatWakes(isOnHandlingThread);
        int error = EINVAL;
        if (number)
            signalSetAction(number, signalWake, error);
        if (error)
            return;
        s_signalState.isShielded[number].store(false);
        s_signalState.isOurs[number] = false;
        s_signalState.wakeSignal.store(number);
    }
    s_signalState.isWakeOnItsWay.store(false);
    if (int number = signalThatWakesThisThread()) {
        sigset_t set;
        sigemptyset(&set);
        sigaddset(&set, number);
        pthread_sigmask(SIG_UNBLOCK, &set, nullptr);
    }
}

bool requestTracebackDump()
{
    VM* vm = s_signalState.vm.load();
    if (!vm)
        return false;
    s_signalState.tripped[0].store(true);
    s_signalState.isTripped.store(true);
    noteCallsOwed(*vm);
    return repeatTracebackDumpRequest();
}

bool repeatTracebackDumpRequest()
{
    if (!s_signalState.vm.load())
        return false;
    // Once the thread has taken note, it is out of the call.
    if (!s_signalState.tripped[0].load())
        return true;
    Threads::wakeMainThreadOfProcess();
    int number = s_signalState.wakeSignal.load();
    if (!number)
        return true;
    Locker locker { s_signalState.wakeLock };
    if (s_signalState.wakesHeldBack || (number == signalThatWakes() && s_signalState.isWakeOnItsWay.exchange(true)))
        return true;
    if (pthread_kill(s_signalState.thread, number))
        s_signalState.isWakeOnItsWay.store(false);
    return true;
}

void withdrawTracebackDumpRequest()
{
    s_signalState.tripped[0].store(false);
}

WithoutWakeSignal::WithoutWakeSignal()
{
    {
        Locker locker { s_signalState.wakeLock };
        ++s_signalState.wakesHeldBack;
    }
    // What has been sent to this thread arrives when a system call returns, at the latest.
    sigset_t pending;
    sigpending(&pending);
}

WithoutWakeSignal::~WithoutWakeSignal()
{
    Locker locker { s_signalState.wakeLock };
    --s_signalState.wakesHeldBack;
}

void installSignalHandlers(JSGlobalObject* globalObject)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    signalSetAction(SIGPIPE, SIG_IGN);
#ifdef SIGXFSZ
    signalSetAction(SIGXFSZ, SIG_IGN);
#endif
    JSValue module = importModule(globalObject, "_signal"_s);
    RETURN_IF_EXCEPTION(scope, void());
    JSArray* handlers = signalHandlers(globalObject);
    if (!signalCanHandle(globalObject) || !signalIsAction(listGet(handlers, SIGINT), signalDefault))
        return;
    JSValue handler = getAttr(globalObject, module, internString(vm, "default_int_handler"_s));
    RETURN_IF_EXCEPTION(scope, void());
    listSet(globalObject, handlers, SIGINT, handler);
    signalSetAction(SIGINT, signalArrived);
}

static void signalRestoreActions()
{
    for (int number = 1; number < signalLimit; ++number) {
        s_signalState.isShielded[number].store(false);
        unwatchSignal(number);
        if (s_signalState.isOurs[number])
            signalSetAction(number, SIG_DFL);
    }
    if (int number = s_signalState.wakeSignal.exchange(0))
        signalSetAction(number, SIG_IGN);
    s_signalState.wakeupDescriptor.store(-1);
}

void finalizeSignals(JSGlobalObject* globalObject)
{
    cancelTracebackDumpRequests(runtime(globalObject));
    if (!signalCanHandle(globalObject))
        return;
    cancelTracebackDumpRequests();
    signalRestoreActions();
    JSArray* handlers = signalHandlers(globalObject);
    for (int number = 1; number < signalLimit; ++number)
        listSet(globalObject, handlers, number, jsUndefined());
}

void detachSignals(Runtime& rt)
{
    cancelTracebackDumpRequests(rt);
    if (s_signalState.runtime != &rt || !s_signalState.vm.load())
        return;
    signalRestoreActions();
    s_signalState.vm.store(nullptr);
}

// ---- The module ----

static bool signalIsBlocked(int number)
{
    sigset_t blocked;
    return !pthread_sigmask(SIG_BLOCK, nullptr, &blocked) && sigismember(&blocked, number) == 1;
}

static bool signalNumberArgument(JSGlobalObject* globalObject, JSValue value, int& number)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    number = toCInt(globalObject, value);
    RETURN_IF_EXCEPTION(scope, false);
    if (number < 1 || number >= signalLimit) {
        raiseValueError(globalObject, scope, "signal number out of range"_s);
        return false;
    }
    return true;
}

static JSValue signalSignal(JSGlobalObject* globalObject, JSValue numberValue, JSValue handler)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int number = toCInt(globalObject, numberValue);
    RETURN_IF_EXCEPTION(scope, { });
    JSArray* handlers = signalHandlers(globalObject);
    if (!signalIsOnHandlingThread(globalObject))
        return raiseValueError(globalObject, scope, "signal only works in main thread of the main interpreter"_s);
    if (number < 1 || number >= signalLimit)
        return raiseValueError(globalObject, scope, "signal number out of range"_s);

    void (*function)(int) = signalArrived;
    bool isShielded = false;
    if (signalIsAction(handler, signalIgnore))
        function = SIG_IGN;
    else if (signalIsAction(handler, signalDefault)) {
        isShielded = number != SIGKILL && number != SIGSTOP && signalIsBlocked(number);
        if (!isShielded)
            function = SIG_DFL;
    } else if (!isCallable(globalObject, handler))
        return raiseTypeError(globalObject, scope, "signal handler must be signal.SIG_IGN, signal.SIG_DFL, or a callable object"_s);

    // What has arrived is for the handler there was.
    if (failed(scope, checkOnlySignals(globalObject)))
        return { };

    int error = EINVAL;
    if (!signalIsReserved(number)) {
        if (!isShielded)
            s_signalState.isShielded[number].store(false);
        signalSetAction(number, function, error);
    }
    if (error)
        return raiseOSError(globalObject, scope, error);
    s_signalState.isOurs[number] = function == signalArrived;
    s_signalState.isWatched[number] = false;
    if (isShielded)
        s_signalState.isShielded[number].store(true);

    JSValue previous = listGet(handlers, number);
    listSet(globalObject, handlers, number, handler);
    return previous;
}

static JSValue signalGetSignal(JSGlobalObject* globalObject, JSValue numberValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int number;
    if (failed(scope, signalNumberArgument(globalObject, numberValue, number)))
        return { };
    RELEASE_AND_RETURN(scope, listGet(signalHandlers(globalObject), number));
}

static JSValue signalAlarm(JSGlobalObject* globalObject, JSValue secondsValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int seconds = toCInt(globalObject, secondsValue);
    RETURN_IF_EXCEPTION(scope, { });
    return jsNumber(alarm(seconds));
}

static JSValue signalPause(JSGlobalObject* globalObject)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    {
        AllowThreads allow(globalObject);
        pause();
    }
    if (failed(scope, checkSignals(globalObject)))
        return { };
    return jsUndefined();
}

static JSValue signalRaiseSignal(JSGlobalObject* globalObject, JSValue numberValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int number = toCInt(globalObject, numberValue);
    RETURN_IF_EXCEPTION(scope, { });
    if (::raise(number))
        return raiseOSError(globalObject, scope, errno);
    if (failed(scope, checkOnlySignals(globalObject)))
        return { };
    return jsUndefined();
}

static JSValue signalStrSignal(JSGlobalObject* globalObject, JSValue numberValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int number;
    if (failed(scope, signalNumberArgument(globalObject, numberValue, number)))
        return { };
    errno = 0;
    const char* text = strsignal(number);
    if (errno || !text || strstr(text, "Unknown signal"))
        return jsUndefined();
    RELEASE_AND_RETURN(scope, makeStr(globalObject, String::fromUTF8(text)));
}

static JSValue signalSigInterrupt(JSGlobalObject* globalObject, JSValue numberValue, JSValue flagValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int number = toCInt(globalObject, numberValue);
    RETURN_IF_EXCEPTION(scope, { });
    int flag = toCInt(globalObject, flagValue);
    RETURN_IF_EXCEPTION(scope, { });
    if (number < 1 || number >= signalLimit)
        return raiseValueError(globalObject, scope, "signal number out of range"_s);
    struct sigaction action;
    if (signalIsReserved(number) || sigaction(number, nullptr, &action))
        return raiseOSError(globalObject, scope, EINVAL);
    if (flag)
        action.sa_flags &= ~SA_RESTART;
    else
        action.sa_flags |= SA_RESTART;
    if (sigaction(number, &action, nullptr))
        return raiseOSError(globalObject, scope, errno);
    return jsUndefined();
}

static JSValue signalSetWakeupFD(JSGlobalObject* globalObject, JSValue descriptorValue, JSValue warnValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool warns = !warnValue || isTrue(globalObject, warnValue);
    RETURN_IF_EXCEPTION(scope, { });
    int descriptor = toCInt(globalObject, descriptorValue);
    RETURN_IF_EXCEPTION(scope, { });
    signalHandlers(globalObject);
    if (!signalIsOnHandlingThread(globalObject))
        return raiseValueError(globalObject, scope, "set_wakeup_fd only works in main thread of the main interpreter"_s);
    if (descriptor != -1) {
        struct stat status;
        if (fstat(descriptor, &status))
            return raiseOSError(globalObject, scope, errno);
        int flags = fcntl(descriptor, F_GETFL, 0);
        if (flags < 0)
            return raiseOSError(globalObject, scope, errno);
        if (!(flags & O_NONBLOCK))
            return raiseValueError(globalObject, scope, "the fd "_s, descriptor, " must be in non-blocking mode"_s);
    }
    s_signalState.warnsOnFullBuffer.store(warns);
    return jsNumber(s_signalState.wakeupDescriptor.exchange(descriptor));
}

// ---- Interval timers ----

static constexpr TypeDef signalItimerErrorTypeDef { "signal.ItimerError"_s, { TypeFlag::Base, TypeFlag::Mutable, TypeFlag::HasWeakref }, { }, { }, { } };

static PyType* signalItimerError(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    return rt.typeFor(signalItimerErrorTypeDef, rt.OSError());
}

static JSValue signalRaiseItimerError(JSGlobalObject* globalObject, int error)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raiseWithArgs(globalObject, scope, signalItimerError(globalObject), { jsNumber(error), makeStr(globalObject, String::fromUTF8(strerror(error))) });
}

// Rounded up to microseconds. Not given: 0.
static bool signalToTimeval(JSGlobalObject* globalObject, JSValue seconds, struct timeval& result)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    result = { };
    if (!seconds)
        return true;
    int64_t nanoseconds;
    if (failed(scope, nanosecondsFromSeconds(globalObject, seconds, TimeRounding::Ceiling, nanoseconds)))
        return false;
    int64_t microseconds = nanoseconds / 1000;
    if (nanoseconds % 1000 > 0)
        ++microseconds;
    result.tv_sec = microseconds / 1000000;
    result.tv_usec = microseconds % 1000000;
    if (result.tv_usec < 0) {
        result.tv_usec += 1000000;
        --result.tv_sec;
    }
    return true;
}

static JSValue signalTimerValue(JSGlobalObject* globalObject, const struct itimerval& timer)
{
    auto seconds = [&](const struct timeval& time) {
        return makeFloat(globalObject, time.tv_sec + time.tv_usec / 1000000.0);
    };
    return PyTuple::create(globalObject, { seconds(timer.it_value), seconds(timer.it_interval) });
}

static JSValue signalSetItimer(JSGlobalObject* globalObject, JSValue whichValue, JSValue seconds, JSValue interval)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int which = toCInt(globalObject, whichValue);
    RETURN_IF_EXCEPTION(scope, { });
    struct itimerval timer, previous;
    if (failed(scope, signalToTimeval(globalObject, seconds, timer.it_value)) || failed(scope, signalToTimeval(globalObject, interval, timer.it_interval)))
        return { };
    if (setitimer(static_cast<decltype(ITIMER_REAL)>(which), &timer, &previous))
        RELEASE_AND_RETURN(scope, signalRaiseItimerError(globalObject, errno));
    RELEASE_AND_RETURN(scope, signalTimerValue(globalObject, previous));
}

static JSValue signalGetItimer(JSGlobalObject* globalObject, JSValue whichValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int which = toCInt(globalObject, whichValue);
    RETURN_IF_EXCEPTION(scope, { });
    struct itimerval timer;
    if (getitimer(static_cast<decltype(ITIMER_REAL)>(which), &timer))
        RELEASE_AND_RETURN(scope, signalRaiseItimerError(globalObject, errno));
    RELEASE_AND_RETURN(scope, signalTimerValue(globalObject, timer));
}

// ---- Sets of signals ----

static bool signalToSet(JSGlobalObject* globalObject, JSValue iterable, sigset_t& result)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    sigemptyset(&result);
    forEach(globalObject, iterable, [&](JSValue item) {
        JSValue index = toIndex(globalObject, item);
        RETURN_IF_EXCEPTION(scope, false);
        auto number = tryInt64(index);
        if (!number || *number <= 0 || *number >= signalLimit) {
            // What does not fit a long is reported as -1.
            raiseValueError(globalObject, scope, "signal number "_s, number.value_or(-1), " out of range [1; "_s, signalLimit - 1, ']');
            return false;
        }
        if (sigaddset(&result, static_cast<int>(*number))) {
            if (errno != EINVAL) {
                raiseOSError(globalObject, scope, errno);
                return false;
            }
            // range(1, NSIG) has long been written, though the system keeps some of the numbers to itself.
            return !failed(scope, warn(globalObject, runtime(globalObject).RuntimeWarning(), makeString("invalid signal number "_s, *number, ", please use valid_signals()"_s)));
        }
        return true;
    });
    RETURN_IF_EXCEPTION(scope, false);
    return true;
}

static JSValue signalFromSet(JSGlobalObject* globalObject, const sigset_t& set)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PySet* result = PySet::create(globalObject);
    for (int number = 1; number < signalLimit; ++number) {
        if (sigismember(&set, number) != 1)
            continue;
        result->add(globalObject, jsNumber(number));
        RETURN_IF_EXCEPTION(scope, { });
    }
    return result;
}

static JSValue signalPthreadSigmask(JSGlobalObject* globalObject, JSValue howValue, JSValue maskValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int how = toCInt(globalObject, howValue);
    RETURN_IF_EXCEPTION(scope, { });
    sigset_t mask;
    if (failed(scope, signalToSet(globalObject, maskValue, mask)))
        return { };
    JSArray* handlers = signalHandlers(globalObject);
    if (how != SIG_UNBLOCK)
        signalRemove(mask, { signalThatSuspends(), signalThatStops(), signalThatWakesThisThread() });

    sigset_t previous;
    bool shields = signalIsOnHandlingThread(globalObject) && (how == SIG_BLOCK || how == SIG_UNBLOCK || how == SIG_SETMASK);
    auto willBeBlocked = [&](int number) {
        bool isInMask = sigismember(&mask, number) == 1;
        if (how == SIG_SETMASK)
            return isInMask;
        bool isBlocked = sigismember(&previous, number) == 1;
        return how == SIG_BLOCK ? isBlocked || isInMask : isBlocked && !isInMask;
    };
    if (shields) {
        pthread_sigmask(SIG_BLOCK, nullptr, &previous);
        // Before it is unblocked: if it is pending, it has its effect at once.
        for (int number = 1; number < signalLimit; ++number) {
            if (s_signalState.isShielded[number].load() && !willBeBlocked(number)) {
                s_signalState.isShielded[number].store(false);
                signalSetAction(number, SIG_DFL);
            }
        }
    }
    if (int error = pthread_sigmask(how, &mask, &previous))
        return raiseOSError(globalObject, scope, error);
    if (shields) {
        for (int number = 1; number < signalLimit; ++number) {
            if (number == SIGKILL || number == SIGSTOP || signalIsReserved(number) || s_signalState.isShielded[number].load())
                continue;
            if (!willBeBlocked(number) || !signalIsAction(listGet(handlers, number), signalDefault))
                continue;
            s_signalState.isShielded[number].store(true);
            signalSetAction(number, signalArrived);
            if (!s_signalState.isOurs[number])
                s_signalState.isShielded[number].store(false);
        }
    }

    // What was pending and has been unblocked has arrived.
    if (failed(scope, checkOnlySignals(globalObject)))
        return { };
    RELEASE_AND_RETURN(scope, signalFromSet(globalObject, previous));
}

static JSValue signalSigPending(JSGlobalObject* globalObject)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    sigset_t pending;
    if (sigpending(&pending))
        return raiseOSError(globalObject, scope, errno);
    RELEASE_AND_RETURN(scope, signalFromSet(globalObject, pending));
}

static JSValue signalValidSignals(JSGlobalObject* globalObject)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    sigset_t all;
    if (sigemptyset(&all) || sigfillset(&all))
        return raiseOSError(globalObject, scope, errno);
    RELEASE_AND_RETURN(scope, signalFromSet(globalObject, all));
}

#if OS(LINUX) || OS(FREEBSD) || OS(NETBSD)
#define PY_HAVE_SIGWAITINFO 1

static constexpr ASCIILiteral signalInfoFields[] = { "si_signo"_s, "si_code"_s, "si_errno"_s, "si_pid"_s, "si_uid"_s, "si_status"_s, "si_band"_s };
static constexpr StructSeqDef signalInfoDef { "signal.struct_siginfo"_s, signalInfoFields, 7 };
// Never seen from Python: its dict is where struct_siginfo is kept, out of reach of what is done to the module.
static constexpr TypeDef signalStateTypeDef { "signal._state"_s, { TypeFlag::DisallowInstantiation, TypeFlag::Internal }, { }, { }, { } };

static PyType* signalInfoType(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    PyDict* state = rt.typeFor(signalStateTypeDef)->dict();
    JSString* key = internString(vm, "struct_siginfo"_s);
    if (JSValue type = state->get(key))
        return PyType::from(type);
    PyType* type = makeStructSeqType(globalObject, signalInfoDef);
    state->set(vm, key, type);
    return type;
}

static JSValue signalMakeInfo(JSGlobalObject* globalObject, const siginfo_t& info)
{
    return makeStructSeq(globalObject, signalInfoType(globalObject), {
        jsNumber(info.si_signo), jsNumber(info.si_code), jsNumber(info.si_errno), jsNumber(info.si_pid), jsNumber(info.si_uid),
        jsNumber(info.si_status), makeInt(globalObject, static_cast<int64_t>(info.si_band)) });
}

static int64_t signalMonotonicNanoseconds()
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<int64_t>(now.tv_sec) * 1000000000 + now.tv_nsec;
}

// False: time was up, or it has raised. No timeout: for as long as it takes.
static bool signalWaitFor(JSGlobalObject* globalObject, sigset_t& set, std::optional<int64_t> timeout, siginfo_t& info)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t deadline = std::numeric_limits<int64_t>::max();
    if (timeout && __builtin_add_overflow(signalMonotonicNanoseconds(), *timeout, &deadline))
        deadline = std::numeric_limits<int64_t>::max();
    while (true) {
        // A thread that took them for signals of the program's would not do what they are sent for. (In the loop: the one that
        // wakes may have been chosen since the wait began.)
        if (signalRemove(set, { signalThatSuspends(), signalThatWakesThisThread() })) {
            bool isEmpty = true;
            for (int number = 1; number < signalLimit && isEmpty; ++number)
                isEmpty = sigismember(&set, number) != 1;
            if (isEmpty) {
                raiseOSError(globalObject, scope, EINVAL);
                return false;
            }
        }
        int result;
        {
            struct timespec time;
            if (timeout) {
                time.tv_sec = *timeout / 1000000000;
                time.tv_nsec = *timeout % 1000000000;
            }
            AllowThreads allow(globalObject);
            result = sigtimedwait(&set, &info, timeout ? &time : nullptr);
        }
        if (result != -1 && result != signalThatWakesThisThread())
            return true;
        if (result != -1)
            signalWake(result);
        if (result == -1 && errno == EAGAIN)
            return false;
        if (result == -1 && errno != EINTR) {
            raiseOSError(globalObject, scope, errno);
            return false;
        }
        if (failed(scope, checkSignals(globalObject)))
            return false;
        if (timeout) {
            timeout = deadline - signalMonotonicNanoseconds();
            if (*timeout < 0)
                return false;
        }
    }
}

// (The function of that name in the C library goes on waiting when it is interrupted.)
static JSValue signalSigWait(JSGlobalObject* globalObject, JSValue setValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    sigset_t set;
    if (failed(scope, signalToSet(globalObject, setValue, set)))
        return { };
    siginfo_t info;
    if (failed(scope, signalWaitFor(globalObject, set, std::nullopt, info)))
        return { };
    return jsNumber(info.si_signo);
}

static JSValue signalSigWaitInfo(JSGlobalObject* globalObject, JSValue setValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    sigset_t set;
    if (failed(scope, signalToSet(globalObject, setValue, set)))
        return { };
    siginfo_t info;
    if (failed(scope, signalWaitFor(globalObject, set, std::nullopt, info)))
        return { };
    RELEASE_AND_RETURN(scope, signalMakeInfo(globalObject, info));
}

static JSValue signalSigTimedWait(JSGlobalObject* globalObject, JSValue setValue, JSValue timeoutValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    sigset_t set;
    if (failed(scope, signalToSet(globalObject, setValue, set)))
        return { };
    int64_t timeout;
    if (failed(scope, nanosecondsFromSeconds(globalObject, timeoutValue, TimeRounding::Ceiling, timeout)))
        return { };
    if (timeout < 0)
        return raiseValueError(globalObject, scope, "timeout must be non-negative"_s);
    siginfo_t info;
    bool hasArrived = signalWaitFor(globalObject, set, timeout, info);
    RETURN_IF_EXCEPTION(scope, { });
    if (!hasArrived)
        return jsUndefined();
    RELEASE_AND_RETURN(scope, signalMakeInfo(globalObject, info));
}
#else
#define PY_HAVE_SIGWAITINFO 0

static JSValue signalSigWait(JSGlobalObject* globalObject, JSValue setValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    sigset_t set;
    if (failed(scope, signalToSet(globalObject, setValue, set)))
        return { };
    signalRemove(set, { signalThatSuspends() });
    int number;
    int error;
    {
        AllowThreads allow(globalObject);
        error = sigwait(&set, &number);
    }
    if (error)
        return raiseOSError(globalObject, scope, error);
    return jsNumber(number);
}
#endif

static JSValue signalPthreadKill(JSGlobalObject* globalObject, JSValue threadValue, JSValue numberValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isInt(threadValue))
        return raiseTypeError(globalObject, scope, "pthread_kill() argument 1 must be int, not "_s, isNone(threadValue) ? String("None"_s) : typeName(globalObject, threadValue));
    int number = toCInt(globalObject, numberValue);
    RETURN_IF_EXCEPTION(scope, { });
    if (failed(scope, audit(globalObject, "signal.pthread_kill"_s, threadValue, jsNumber(number))))
        return { };
    auto thread = tryUInt64(intValue(threadValue));
    if (!thread || !runtime(globalObject).threads().hasThread(*thread))
        return raiseOSError(globalObject, scope, ESRCH);
    if (int error = pthread_kill(static_cast<pthread_t>(*thread), number))
        return raiseOSError(globalObject, scope, error);
    if (failed(scope, checkOnlySignals(globalObject)))
        return { };
    return jsUndefined();
}

#if OS(LINUX) && defined(SYS_pidfd_send_signal)
#define PY_HAVE_PIDFD_SEND_SIGNAL 1
static JSValue signalPidfdSendSignal(JSGlobalObject* globalObject, JSValue descriptorValue, JSValue numberValue, JSValue info, JSValue flagsValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int descriptor = toCInt(globalObject, descriptorValue);
    RETURN_IF_EXCEPTION(scope, { });
    int number = toCInt(globalObject, numberValue);
    RETURN_IF_EXCEPTION(scope, { });
    int flags = flagsValue ? toCInt(globalObject, flagsValue) : 0;
    RETURN_IF_EXCEPTION(scope, { });
    if (info && !isNone(info))
        return raiseTypeError(globalObject, scope, "siginfo must be None"_s);
    if (syscall(SYS_pidfd_send_signal, descriptor, number, nullptr, flags) < 0)
        return raiseOSError(globalObject, scope, errno);
    return jsUndefined();
}
#else
#define PY_HAVE_PIDFD_SEND_SIGNAL 0
#endif

static constexpr NativeDef signalFunctions[] = {
    PY_FUNCTION("default_int_handler", signalDefaultIntHandler, "(signalnum, frame, /)"),
    PY_FUNCTION("alarm", signalAlarm, "(seconds, /)"),
    PY_FUNCTION("setitimer", signalSetItimer, "(which, seconds, interval=0.0, /)"),
    PY_FUNCTION("getitimer", signalGetItimer, "(which, /)"),
    PY_FUNCTION("signal", signalSignal, "(signalnum, handler, /)"),
    PY_FUNCTION("raise_signal", signalRaiseSignal, "(signalnum, /)"),
    PY_FUNCTION("strsignal", signalStrSignal, "(signalnum, /)"),
    PY_FUNCTION("getsignal", signalGetSignal, "(signalnum, /)"),
    PY_FUNCTION("set_wakeup_fd", signalSetWakeupFD, "(fd, /, *, warn_on_full_buffer=True)"),
    PY_FUNCTION("siginterrupt", signalSigInterrupt, "(signalnum, flag, /)"),
    PY_FUNCTION("pause", signalPause, "()"),
#if PY_HAVE_PIDFD_SEND_SIGNAL
    PY_FUNCTION("pidfd_send_signal", signalPidfdSendSignal, "(pidfd, signalnum, siginfo=None, flags=0, /)"),
#endif
    PY_FUNCTION("pthread_kill", signalPthreadKill, "(thread_id, signalnum, /)"),
    PY_FUNCTION("pthread_sigmask", signalPthreadSigmask, "(how, mask, /)"),
    PY_FUNCTION("sigpending", signalSigPending, "()"),
    PY_FUNCTION("sigwait", signalSigWait, "(sigset, /)"),
#if PY_HAVE_SIGWAITINFO
    PY_FUNCTION("sigwaitinfo", signalSigWaitInfo, "(sigset, /)"),
    PY_FUNCTION("sigtimedwait", signalSigTimedWait, "(sigset, timeout, /)"),
#endif
    PY_FUNCTION("valid_signals", signalValidSignals, "()"),
};

namespace {

struct SignalConstant {
    ASCIILiteral name;
    int value;
};

} // namespace

static const SignalConstant signalConstants[] = {
    { "SIG_DFL"_s, signalDefault },
    { "SIG_IGN"_s, signalIgnore },
    { "NSIG"_s, signalLimit },
    { "SIG_BLOCK"_s, SIG_BLOCK },
    { "SIG_UNBLOCK"_s, SIG_UNBLOCK },
    { "SIG_SETMASK"_s, SIG_SETMASK },
    { "SIGHUP"_s, SIGHUP },
    { "SIGINT"_s, SIGINT },
    { "SIGQUIT"_s, SIGQUIT },
    { "SIGILL"_s, SIGILL },
    { "SIGTRAP"_s, SIGTRAP },
    { "SIGIOT"_s, SIGIOT },
    { "SIGABRT"_s, SIGABRT },
#ifdef SIGEMT
    { "SIGEMT"_s, SIGEMT },
#endif
    { "SIGFPE"_s, SIGFPE },
    { "SIGKILL"_s, SIGKILL },
    { "SIGBUS"_s, SIGBUS },
    { "SIGSEGV"_s, SIGSEGV },
    { "SIGSYS"_s, SIGSYS },
    { "SIGPIPE"_s, SIGPIPE },
    { "SIGALRM"_s, SIGALRM },
    { "SIGTERM"_s, SIGTERM },
    { "SIGUSR1"_s, SIGUSR1 },
    { "SIGUSR2"_s, SIGUSR2 },
#ifdef SIGCLD
    { "SIGCLD"_s, SIGCLD },
#endif
    { "SIGCHLD"_s, SIGCHLD },
#ifdef SIGPWR
    { "SIGPWR"_s, SIGPWR },
#endif
    { "SIGIO"_s, SIGIO },
    { "SIGURG"_s, SIGURG },
    { "SIGWINCH"_s, SIGWINCH },
#ifdef SIGPOLL
    { "SIGPOLL"_s, SIGPOLL },
#endif
    { "SIGSTOP"_s, SIGSTOP },
    { "SIGTSTP"_s, SIGTSTP },
    { "SIGCONT"_s, SIGCONT },
    { "SIGTTIN"_s, SIGTTIN },
    { "SIGTTOU"_s, SIGTTOU },
    { "SIGVTALRM"_s, SIGVTALRM },
    { "SIGPROF"_s, SIGPROF },
    { "SIGXCPU"_s, SIGXCPU },
    { "SIGXFSZ"_s, SIGXFSZ },
#ifdef SIGINFO
    { "SIGINFO"_s, SIGINFO },
#endif
#ifdef SIGSTKFLT
    { "SIGSTKFLT"_s, SIGSTKFLT },
#endif
    { "ITIMER_REAL"_s, ITIMER_REAL },
    { "ITIMER_VIRTUAL"_s, ITIMER_VIRTUAL },
    { "ITIMER_PROF"_s, ITIMER_PROF },
};

void initializeSignalModule(JSGlobalObject* globalObject, PyModule* module)
{
    module->addFunctions(globalObject, signalFunctions);
    for (auto& constant : signalConstants)
        module->add(globalObject, constant.name, jsNumber(constant.value));
    // Not constants in C.
#ifdef SIGRTMIN
    module->add(globalObject, "SIGRTMIN"_s, jsNumber(SIGRTMIN));
#endif
#ifdef SIGRTMAX
    module->add(globalObject, "SIGRTMAX"_s, jsNumber(SIGRTMAX));
#endif
    module->add(globalObject, "ItimerError"_s, signalItimerError(globalObject));
#if PY_HAVE_SIGWAITINFO
    module->add(globalObject, "struct_siginfo"_s, signalInfoType(globalObject));
#endif
    signalHandlers(globalObject);
}

#else // OS(WINDOWS)

// FIXME: Signals on Windows.
bool checkSignals(JSGlobalObject*) { return true; }
bool checkOnlySignals(JSGlobalObject*) { return true; }
bool signalMayBeHandled(JSGlobalObject*) { return false; }
bool simulateSignal(JSGlobalObject*, int number) { return number >= 1 && number < 23; }
void installSignalHandlers(JSGlobalObject*) { }
void finalizeSignals(JSGlobalObject*) { }
void detachSignals(Runtime&) { }
int watchSignal(JSGlobalObject*, int, bool) { return EINVAL; }
void unwatchSignal(int) { }
void prepareForTracebackDumpRequests(JSGlobalObject*) { }
bool requestTracebackDump() { return false; }
bool repeatTracebackDumpRequest() { return false; }
void withdrawTracebackDumpRequest() { }
WithoutWakeSignal::WithoutWakeSignal() = default;
WithoutWakeSignal::~WithoutWakeSignal() = default;

#endif // OS(WINDOWS)

} // namespace Python
} // namespace JSC
