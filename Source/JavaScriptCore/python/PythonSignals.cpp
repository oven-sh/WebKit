/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */


#include "config.h"
#include "PythonSignals.h"

#include "JSCInlines.h"
#include "PyDict.h"
#include "PyFrame.h"
#include "PyRealm.h"
#include "PyStateObject.h"
#include "PyTuple.h"
#include "PythonBuiltins.h"
#include "PythonIO.h"
#include "PythonImport.h"
#include "PythonOperations.h"
#include "PythonPosix.h"
#include "PythonTime.h"
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#if OS(LINUX)
#include <sys/syscall.h>
#endif

// Signals, and the module _signal: Modules/signalmodule.c of CPython.

namespace JSC { namespace Python {

namespace {

// Py_NSIG
constexpr int signalCount = NSIG;

// ---- What is done when a signal comes
//
// The system calls a function with the number of the signal and nothing else, on whatever thread it likes, in the middle of whatever that thread is doing. So what the function is to find has to be the process's, and it
// can do nothing to it but load and store. This is `_PyRuntime.signals` of CPython, without the functions that the program has, which the collector has to know about: those are the realm's.
struct ArrivedSignals {
    std::array<std::atomic<bool>, signalCount> isTripped { };
    std::atomic<VM*> vm { nullptr }; // Whose Python code is to see to them.
    std::atomic<pthread_t> thread { }; // The thread that it runs in, which means something for as long as there is a `vm`.
#if OS(LINUX)
    std::atomic<pid_t> threadID { 0 }; // The same, as the kernel knows it
#endif
    // What is to be done with a signal that is being kept for that thread: see keepForPythonThread().
    enum class Kept : uint8_t { No, ToDoTheUsual, ToBeIgnored };
    std::array<std::atomic<Kept>, signalCount> kept { };
    std::atomic<unsigned> handlersRunning { 0 }; // How many threads are somewhere between looking at `vm` and having done with it.
    std::atomic<int> wakeupDescriptor { -1 };
    std::atomic<bool> warnsOnFullBuffer { true };
    // What went wrong with writing to that, which is said later. As many as CPython has room for things to be done later: MAXPENDINGCALLS_MAIN
    std::array<std::atomic<int>, 32> wakeupErrors { };
    std::atomic<unsigned> wakeupErrorCount { 0 };
};

ArrivedSignals& arrivedSignals()
{
    static ArrivedSignals signals;
    return signals;
}

// trip_signal()
void tripSignal(int signal)
{
    auto& arrived = arrivedSignals();
    arrived.handlersRunning.fetch_add(1);
    VM* vm = arrived.vm.load();
    arrived.isTripped[signal].store(true);
    if (vm)
        vm->notePythonSignal();
    // It is written to after all that, so that what it wakes finds something to do.
    if (int descriptor = arrived.wakeupDescriptor.load(); descriptor != -1) {
        uint8_t byte = static_cast<uint8_t>(signal);
        ssize_t written;
        do {
            written = write(descriptor, &byte, 1);
        } while (written < 0 && errno == EINTR);
        if (written < 0 && (arrived.warnsOnFullBuffer.load() || (errno != EWOULDBLOCK && errno != EAGAIN))) {
            unsigned index = arrived.wakeupErrorCount.fetch_add(1);
            if (index < arrived.wakeupErrors.size())
                arrived.wakeupErrors[index].store(errno);
            else
                arrived.wakeupErrorCount.store(arrived.wakeupErrors.size());
            if (vm)
                vm->notePythonSignal();
        }
    }
    arrived.handlersRunning.fetch_sub(1);
}

// From a thread that a signal has come to, to the thread that Python runs in.
void sendOnToPythonThread(int signal, siginfo_t* information)
{
    auto& arrived = arrivedSignals();
#if OS(LINUX)
    // With what came with it, which says who sent it: sigwaitinfo() tells of that. A process may say what it likes of a signal that it sends itself.
    if (information && !syscall(SYS_rt_tgsigqueueinfo, getpid(), arrived.threadID.load(), signal, information))
        return;
#else
    UNUSED_PARAM(information);
#endif
    pthread_kill(arrived.thread.load(), signal);
}

// signal_handler()
void signalHandler(int signal, siginfo_t* information, void*)
{
    int savedError = errno;
    // A signal that is for the process is given to any thread that will have it, and the engine and its host have threads that CPython has not. It is for the thread that Python runs in, and is sent on. If that
    // thread has said that it is not to have it yet, with pthread_sigmask(), it is kept for it until it will. And what that thread is waiting in the system for is interrupted, as it would not be otherwise.
    auto& arrived = arrivedSignals();
    arrived.handlersRunning.fetch_add(1);
    bool isElsewhere = arrived.vm.load() && !pthread_equal(pthread_self(), arrived.thread.load());
    if (isElsewhere)
        sendOnToPythonThread(signal, information);
    arrived.handlersRunning.fetch_sub(1);
    if (!isElsewhere)
        tripSignal(signal);
    errno = savedError;
}

using Handler = void (*)(int);
// What is told what came with the signal: SA_SIGINFO
using InformedHandler = void (*)(int, siginfo_t*, void*);

// PyOS_getsig() and PyOS_setsig()
Handler getSystemHandler(int signal)
{
    struct sigaction action;
    if (sigaction(signal, nullptr, &action) == -1)
        return SIG_ERR;
    return action.sa_handler;
}

Handler setSystemHandler(int signal, Handler handler)
{
    struct sigaction action;
    struct sigaction previous;
    action.sa_handler = handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_ONSTACK;
    if (sigaction(signal, &action, &previous) == -1)
        return SIG_ERR;
    return previous.sa_handler;
}

Handler setSystemHandler(int signal, InformedHandler handler)
{
    struct sigaction action;
    struct sigaction previous;
    action.sa_sigaction = handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_ONSTACK | SA_SIGINFO;
    if (sigaction(signal, &action, &previous) == -1)
        return SIG_ERR;
    return previous.sa_handler;
}

// A thread can say that a signal is not to come to it yet, with pthread_sigmask(), and later ask what has come, with sigpending(), or wait for it, with sigwait(). But a signal that is for the process is given to any
// thread that will have it, and the engine and its host have threads that CPython has not. One of those would do with it what is to be done: nothing, or the usual, which as a rule is to end the process.
//
// So for as long as the thread that Python runs in is keeping a signal back, and no function is to be called for it, this is what is done with it. It is sent on to that thread, and is kept for it there.
void keepForPythonThread(int signal, siginfo_t* information, void*)
{
    int savedError = errno;
    auto& arrived = arrivedSignals();
    arrived.handlersRunning.fetch_add(1);
    if (arrived.vm.load() && !pthread_equal(pthread_self(), arrived.thread.load()))
        sendOnToPythonThread(signal, information);
    else if (arrived.kept[signal].load() == ArrivedSignals::Kept::ToDoTheUsual) {
        // It has come to that thread after all, between this being arranged and the signal being kept back. It comes again when this returns.
        setSystemHandler(signal, SIG_DFL);
        ::raise(signal);
    }
    arrived.handlersRunning.fetch_sub(1);
    errno = savedError;
}

// What a program can say is to be done with a signal
enum class ToBeDone : uint8_t { TheUsual, Nothing, CallAFunction };

// What the program has said is to be done with a signal is done, but that one that its thread is keeping back is kept for it. SIG_ERR, and errno, if it cannot be.
Handler setHandlerOfProgram(int signal, ToBeDone toBeDone, bool isKeptBack)
{
    auto& kept = arrivedSignals().kept[signal];
    kept.store(ArrivedSignals::Kept::No);
    if (toBeDone == ToBeDone::CallAFunction)
        return setSystemHandler(signal, signalHandler);
    Handler handler = toBeDone == ToBeDone::TheUsual ? SIG_DFL : SIG_IGN;
    // To say that one is to be ignored is to be rid of any that has come already, which the system sees to.
    Handler previous = setSystemHandler(signal, handler);
    if (previous == SIG_ERR || !isKeptBack)
        return previous;
#if OS(LINUX)
    bool isKept = true;
#else
    // One that is to be ignored is not kept at all, though it is being kept back, and no more is one that nothing is done about as a rule: SA_IGNORE, in the kernels that come from BSD.
    bool isKept = handler == SIG_DFL && signal != SIGURG && signal != SIGCONT && signal != SIGCHLD && signal != SIGIO && signal != SIGWINCH && signal != SIGINFO;
#endif
    if (isKept) {
        kept.store(handler == SIG_DFL ? ArrivedSignals::Kept::ToDoTheUsual : ArrivedSignals::Kept::ToBeIgnored);
        setSystemHandler(signal, keepForPythonThread);
    }
    return previous;
}

bool isKeptBack(int signal)
{
    sigset_t mask;
    return !pthread_sigmask(SIG_BLOCK, nullptr, &mask) && sigismember(&mask, signal) == 1;
}

// The thread that Python runs in is about to keep back the signals in `next`, and no others.
void arrangeForSignalsKeptBack(const sigset_t& next)
{
    auto& arrived = arrivedSignals();
    for (int signal = 1; signal < signalCount; ++signal) {
        bool willBeKeptBack = sigismember(&next, signal) == 1;
        auto kept = arrived.kept[signal].load();
        if (kept != ArrivedSignals::Kept::No) {
            if (!willBeKeptBack)
                setHandlerOfProgram(signal, kept == ArrivedSignals::Kept::ToDoTheUsual ? ToBeDone::TheUsual : ToBeDone::Nothing, false);
            continue;
        }
        if (!willBeKeptBack)
            continue;
        // What is somebody else's is left alone. SIGKILL and SIGSTOP cannot be kept back, or have anything done about them, and it is not said.
        if (Handler handler = getSystemHandler(signal); handler == SIG_DFL || handler == SIG_IGN)
            setHandlerOfProgram(signal, handler == SIG_DFL ? ToBeDone::TheUsual : ToBeDone::Nothing, true);
    }
}

// ---- What the program has said is to be done

struct SignalState final : NativeState {
    PYTHON_NATIVE_STATE(SignalState);
    ~SignalState();

    bool isMain { false }; // Whether it is this realm that the signals are for.
    std::array<WriteBarrier<Unknown>, signalCount> handlers; // SIG_DFL, SIG_IGN, None for what is none of Python's business, or something to call. Empty until _signal is imported.
    WriteBarrier<PyType> itimerError;
    WriteBarrier<PyType> information; // struct_siginfo
};

template<typename Visitor>
void SignalState::visit(Visitor& visitor)
{
    for (auto& handler : handlers)
        visitor.append(handler);
    visitor.append(itimerError);
    visitor.append(information);
}

SignalState& signalState(JSGlobalObject* globalObject) { return globalObject->pyRealm()->moduleState<SignalState>(); }

JSValue defaultHandler() { return jsNumber(static_cast<int32_t>(std::bit_cast<intptr_t>(SIG_DFL))); }
JSValue ignoreHandler() { return jsNumber(static_cast<int32_t>(std::bit_cast<intptr_t>(SIG_IGN))); }

// compare_handler(): an int and nothing derived from it, and the same one.
bool isHandler(JSValue function, JSValue defaultOrIgnore) { return function && function.isInt32() && function == defaultOrIgnore; }

bool isCalled(JSValue function) { return function && !isNone(function) && !isHandler(function, defaultHandler()) && !isHandler(function, ignoreHandler()); }

// _PySignal_Fini(): nothing of this realm's is to be looked for when a signal comes.
SignalState::~SignalState()
{
    if (!isMain)
        return;
    auto& arrived = arrivedSignals();
    for (int signal = 1; signal < signalCount; ++signal) {
        arrived.isTripped[signal].store(false);
        if (isCalled(handlers[signal].get()))
            setSystemHandler(signal, SIG_DFL);
        else if (auto kept = arrived.kept[signal].exchange(ArrivedSignals::Kept::No); kept != ArrivedSignals::Kept::No)
            setSystemHandler(signal, kept == ArrivedSignals::Kept::ToDoTheUsual ? SIG_DFL : SIG_IGN);
    }
    arrived.wakeupDescriptor.store(-1);
    arrived.vm.store(nullptr);
    // One that had come already may be being dealt with on another thread.
    while (arrived.handlersRunning.load()) { }
}

// _Py_ThreadCanHandleSignals()
bool canHandleSignals(JSGlobalObject* globalObject) { return signalState(globalObject).isMain; }

bool checkSignalNumber(JSGlobalObject* globalObject, ThrowScope& scope, int signal)
{
    if (signal >= 1 && signal < signalCount)
        return true;
    raiseValueError(globalObject, scope, "signal number out of range"_s);
    return false;
}

// _Py_Sigset_Converter(). False if it raised.
bool toSignalSet(JSGlobalObject* globalObject, JSValue given, sigset_t& mask)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    sigemptyset(&mask);
    forEach(globalObject, given, [&] (JSValue item) {
        // PyLong_AsLongAndOverflow()
        JSValue integer = toInt(globalObject, item);
        RETURN_IF_EXCEPTION(scope, false);
        auto value = tryInt64(integer);
        int64_t signal = value.value_or(-1);
        if (signal <= 0 || signal >= signalCount) {
            raiseValueError(globalObject, scope, concatenate("signal number "_s, signal, " out of range [1; "_s, signalCount - 1, ']'));
            return false;
        }
        if (sigaddset(&mask, static_cast<int>(signal))) {
            if (errno != EINVAL) {
                raiseOSError(globalObject, scope, errno);
                return false;
            }
            // range(1, NSIG) has always been let pass.
            if (!warn(globalObject, BuiltinType::RuntimeWarning, concatenate("invalid signal number "_s, signal, ", please use valid_signals()"_s)))
                return false;
        }
        return true;
    });
    RETURN_IF_EXCEPTION(scope, false);
    return true;
}

// sigset_to_set()
JSValue toSet(JSGlobalObject* globalObject, const sigset_t& mask)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    PySet* result = PySet::create(globalObject);
    for (int signal = 1; signal < signalCount; ++signal) {
        if (sigismember(&mask, signal) != 1)
            continue;
        result->add(globalObject, jsNumber(signal));
        RETURN_IF_EXCEPTION(scope, { });
    }
    return result;
}

// timeval_from_double(). False if it raised.
bool toTimeval(JSGlobalObject* globalObject, JSValue given, struct timeval& result)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (!given) {
        result = { };
        return true;
    }
    auto time = timeFromSecondsObject(globalObject, given, TimeRounding::Ceiling);
    RETURN_IF_EXCEPTION(scope, false);
    RELEASE_AND_RETURN(scope, timeAsTimeval(globalObject, *time, result, TimeRounding::Ceiling));
}

// itimer_retval()
JSValue toTuple(JSGlobalObject* globalObject, const struct itimerval& timer)
{
    auto seconds = [] (const struct timeval& time) { return floatFromDouble(static_cast<double>(time.tv_sec) + static_cast<double>(time.tv_usec) / 1000000.0); };
    return PyTuple::create(globalObject, { seconds(timer.it_value), seconds(timer.it_interval) });
}

} // anonymous namespace

void handleSignals(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& state = signalState(globalObject);
    if (!state.isMain)
        return;
    auto& arrived = arrivedSignals();
    // What went wrong with the descriptor is said at the next chance there is, which is when the first function begins, if it is written in Python.
    if (arrived.wakeupErrorCount.load())
        vm.setHasPythonWork(true);
    CallFrame* callFrame = innermostPythonFrame(vm);
    JSValue frame = callFrame ? JSValue(PyFrame::forCallFrame(vm, callFrame)) : jsUndefined();
    for (int signal = 1; signal < signalCount; ++signal) {
        if (!arrived.isTripped[signal].exchange(false))
            continue;
        // What is to be done about it may have been changed since it came.
        JSValue function = state.handlers[signal].get();
        if (!isCalled(function)) {
            raise(globalObject, scope, BuiltinType::OSError, concatenate("Signal "_s, signal, " ignored due to race condition"_s));
            reportUnraisable(globalObject, "Exception ignored while calling signal handler"_s);
            RETURN_IF_EXCEPTION(scope, void());
            continue;
        }
        call(globalObject, function, jsNumber(signal), frame);
        if (scope.exception()) [[unlikely]] {
            // The rest are for the next time.
            vm.notePythonSignal();
            return;
        }
    }
}

// report_wakeup_write_error(), for each time
void reportSignalWakeupErrors(JSGlobalObject* globalObject)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    auto& arrived = arrivedSignals();
    if (!arrived.wakeupErrorCount.load() || !signalState(globalObject).isMain) [[likely]]
        return;
    unsigned count = std::min<unsigned>(arrived.wakeupErrorCount.exchange(0), arrived.wakeupErrors.size());
    for (unsigned i = 0; i < count; ++i) {
        raiseOSError(globalObject, scope, arrived.wakeupErrors[i].load());
        reportUnraisable(globalObject, "Exception ignored while trying to write to the signal wakeup fd"_s);
        RETURN_IF_EXCEPTION(scope, void());
    }
}

bool simulateSignal(JSGlobalObject* globalObject, int signal)
{
    if (signal < 1 || signal >= signalCount)
        return false;
    JSValue function = signalState(globalObject).handlers[signal].get();
    if (!isHandler(function, ignoreHandler()) && !isHandler(function, defaultHandler()))
        tripSignal(signal);
    return true;
}

void restoreSignals()
{
    setSystemHandler(SIGPIPE, SIG_DFL);
#ifdef SIGXFSZ
    setSystemHandler(SIGXFSZ, SIG_DFL);
#endif
}

int exitByInterrupt()
{
    if (setSystemHandler(SIGINT, SIG_DFL) != SIG_ERR)
        kill(getpid(), SIGINT);
    return SIGINT + 128;
}

void initializeSignals(JSGlobalObject* globalObject, bool installsHandlers)
{
    VM& vm = globalObject->vm();
    auto& arrived = arrivedSignals();
    VM* none = nullptr;
    if (!arrived.vm.compare_exchange_strong(none, &vm))
        return;
    // Nothing looks at it until there is a function to be called, and there is none yet.
    arrived.thread.store(pthread_self());
#if OS(LINUX)
    arrived.threadID.store(static_cast<pid_t>(syscall(SYS_gettid)));
#endif
    signalState(globalObject).isMain = true;
    for (int signal = 1; signal < signalCount; ++signal)
        arrived.isTripped[signal].store(false);
    if (!installsHandlers)
        return;
    // signal_install_handlers()
    setSystemHandler(SIGPIPE, SIG_IGN);
#ifdef SIGXFSZ
    setSystemHandler(SIGXFSZ, SIG_IGN);
#endif
    // Which sees to SIGINT
    importModule(globalObject, "_signal"_s);
}

// ---- The module

// default_int_handler(signalnum, frame, /)
PYTHON_NATIVE(signalDefaultIntHandler)
{
    NATIVE_PROLOGUE();
    toCInt(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(raise(globalObject, scope, BuiltinType::KeyboardInterrupt, JSValue()));
}

// alarm(seconds, /)
PYTHON_NATIVE(signalAlarm)
{
    NATIVE_PROLOGUE();
    auto seconds = toCInt(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(intFromInt64(globalObject, static_cast<long>(alarm(*seconds))));
}

PYTHON_NATIVE(signalPause)
{
    NATIVE_PROLOGUE();
    pause();
    checkSignals(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    RETURN_NONE();
}

// raise_signal(signalnum, /)
PYTHON_NATIVE(signalRaiseSignal)
{
    NATIVE_PROLOGUE();
    auto signal = toCInt(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    if (::raise(*signal))
        return JSValue::encode(raiseOSError(globalObject, scope, errno));
    checkSignals(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    RETURN_NONE();
}

// signal(signalnum, handler, /)
PYTHON_NATIVE(signalSignal)
{
    NATIVE_PROLOGUE();
    auto signal = toCInt(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue handler = args[1];
    if (!canHandleSignals(globalObject))
        return JSValue::encode(raiseValueError(globalObject, scope, "signal only works in main thread of the main interpreter"_s));
    if (!checkSignalNumber(globalObject, scope, *signal))
        return { };
    ToBeDone function;
    if (isCallable(globalObject, handler))
        function = ToBeDone::CallAFunction;
    else if (isHandler(handler, ignoreHandler()))
        function = ToBeDone::Nothing;
    else if (isHandler(handler, defaultHandler()))
        function = ToBeDone::TheUsual;
    else
        return JSValue::encode(raiseTypeError(globalObject, scope, "signal handler must be signal.SIG_IGN, signal.SIG_DFL, or a callable object"_s));
    // What has come already is dealt with as it was to be.
    checkSignals(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    if (setHandlerOfProgram(*signal, function, isKeptBack(*signal)) == SIG_ERR)
        return JSValue::encode(raiseOSError(globalObject, scope, errno));
    auto& slot = signalState(globalObject).handlers[*signal];
    JSValue previous = slot.get();
    slot.set(vm, realm, handler);
    return JSValue::encode(previous ? previous : jsUndefined());
}

// getsignal(signalnum, /)
PYTHON_NATIVE(signalGetSignal)
{
    NATIVE_PROLOGUE();
    auto signal = toCInt(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    if (!checkSignalNumber(globalObject, scope, *signal))
        return { };
    JSValue handler = signalState(globalObject).handlers[*signal].get();
    return JSValue::encode(handler ? handler : jsUndefined());
}

// strsignal(signalnum, /)
PYTHON_NATIVE(signalStrSignal)
{
    NATIVE_PROLOGUE();
    auto signal = toCInt(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    if (!checkSignalNumber(globalObject, scope, *signal))
        return { };
    errno = 0;
    const char* description = strsignal(*signal);
    if (errno || !description || strstr(description, "Unknown signal"))
        RETURN_NONE();
    return JSValue::encode(jsString(vm, String::fromUTF8(description)));
}

// siginterrupt(signalnum, flag, /)
PYTHON_NATIVE(signalSigInterrupt)
{
    NATIVE_PROLOGUE();
    auto signal = toCInt(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    auto flag = toCInt(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    if (!checkSignalNumber(globalObject, scope, *signal))
        return { };
    struct sigaction action;
    sigaction(*signal, nullptr, &action);
    if (*flag)
        action.sa_flags &= ~SA_RESTART;
    else
        action.sa_flags |= SA_RESTART;
    if (sigaction(*signal, &action, nullptr) < 0)
        return JSValue::encode(raiseOSError(globalObject, scope, errno));
    RETURN_NONE();
}

// set_wakeup_fd(fd, /, *, warn_on_full_buffer=True)
PYTHON_NATIVE(signalSetWakeupDescriptor)
{
    NATIVE_PROLOGUE();
    bool warnsOnFullBuffer = true;
    if (JSValue given = args.at(1)) {
        warnsOnFullBuffer = isTrue(globalObject, given);
        RETURN_IF_EXCEPTION(scope, { });
    }
    auto descriptor = toCInt(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    if (!canHandleSignals(globalObject))
        return JSValue::encode(raiseValueError(globalObject, scope, "set_wakeup_fd only works in main thread of the main interpreter"_s));
    if (*descriptor != -1) {
        struct stat status;
        if (fstat(*descriptor, &status))
            return JSValue::encode(raiseOSError(globalObject, scope, errno));
        int flags = fcntl(*descriptor, F_GETFL, 0);
        if (flags < 0)
            return JSValue::encode(raiseOSError(globalObject, scope, errno));
        if (!(flags & O_NONBLOCK))
            return JSValue::encode(raiseValueError(globalObject, scope, concatenate("the fd "_s, *descriptor, " must be in non-blocking mode"_s)));
    }
    auto& arrived = arrivedSignals();
    int previous = arrived.wakeupDescriptor.exchange(*descriptor);
    arrived.warnsOnFullBuffer.store(warnsOnFullBuffer);
    return JSValue::encode(jsNumber(previous));
}

// setitimer(which, seconds, interval=0.0, /)
PYTHON_NATIVE(signalSetITimer)
{
    NATIVE_PROLOGUE();
    auto which = toCInt(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    struct itimerval timer;
    toTimeval(globalObject, args[1], timer.it_value);
    RETURN_IF_EXCEPTION(scope, { });
    toTimeval(globalObject, args.at(2), timer.it_interval);
    RETURN_IF_EXCEPTION(scope, { });
    struct itimerval previous;
    if (setitimer(*which, &timer, &previous))
        return JSValue::encode(raiseOSError(globalObject, scope, errno, JSValue(), signalState(globalObject).itimerError.get()));
    return JSValue::encode(toTuple(globalObject, previous));
}

// getitimer(which, /)
PYTHON_NATIVE(signalGetITimer)
{
    NATIVE_PROLOGUE();
    auto which = toCInt(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    struct itimerval timer;
    if (getitimer(*which, &timer))
        return JSValue::encode(raiseOSError(globalObject, scope, errno, JSValue(), signalState(globalObject).itimerError.get()));
    return JSValue::encode(toTuple(globalObject, timer));
}

// pthread_sigmask(how, mask, /)
PYTHON_NATIVE(signalPthreadSigmask)
{
    NATIVE_PROLOGUE();
    auto how = toCInt(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    sigset_t mask;
    toSignalSet(globalObject, args[1], mask);
    RETURN_IF_EXCEPTION(scope, { });
    sigset_t previous;
    if (canHandleSignals(globalObject) && (*how == SIG_BLOCK || *how == SIG_UNBLOCK || *how == SIG_SETMASK) && !pthread_sigmask(SIG_BLOCK, nullptr, &previous)) {
        sigset_t next = *how == SIG_SETMASK ? mask : previous;
        for (int signal = 1; signal < signalCount && *how != SIG_SETMASK; ++signal) {
            if (sigismember(&mask, signal) != 1)
                continue;
            if (*how == SIG_BLOCK)
                sigaddset(&next, signal);
            else
                sigdelset(&next, signal);
        }
        arrangeForSignalsKeptBack(next);
    }
    if (int error = pthread_sigmask(*how, &mask, &previous))
        return JSValue::encode(raiseOSError(globalObject, scope, error));
    // What was being kept back has come.
    checkSignals(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(toSet(globalObject, previous)));
}

PYTHON_NATIVE(signalSigPending)
{
    NATIVE_PROLOGUE();
    sigset_t mask;
    if (sigpending(&mask))
        return JSValue::encode(raiseOSError(globalObject, scope, errno));
    RELEASE_AND_RETURN(scope, JSValue::encode(toSet(globalObject, mask)));
}

// sigwait(sigset, /)
PYTHON_NATIVE(signalSigWait)
{
    NATIVE_PROLOGUE();
    sigset_t mask;
    toSignalSet(globalObject, args[0], mask);
    RETURN_IF_EXCEPTION(scope, { });
    int signal;
    if (int error = sigwait(&mask, &signal))
        return JSValue::encode(raiseOSError(globalObject, scope, error));
    return JSValue::encode(jsNumber(signal));
}

#if OS(LINUX)

// fill_siginfo()
static JSValue signalInformation(JSGlobalObject* globalObject, const siginfo_t& information)
{
    MarkedArgumentBuffer values;
    values.append(jsNumber(information.si_signo));
    values.append(jsNumber(information.si_code));
    values.append(jsNumber(information.si_errno));
    values.append(jsNumber(information.si_pid));
    values.append(intFromUserID(globalObject, information.si_uid));
    values.append(jsNumber(information.si_status));
    values.append(intFromInt64(globalObject, information.si_band));
    return newStructSequence(globalObject, signalState(globalObject).information.get(), values);
}

// sigwaitinfo(sigset, /)
PYTHON_NATIVE(signalSigWaitInfo)
{
    NATIVE_PROLOGUE();
    sigset_t mask;
    toSignalSet(globalObject, args[0], mask);
    RETURN_IF_EXCEPTION(scope, { });
    siginfo_t information;
    while (sigwaitinfo(&mask, &information) == -1) {
        if (errno != EINTR)
            return JSValue::encode(raiseOSError(globalObject, scope, errno));
        checkSignals(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(signalInformation(globalObject, information)));
}

// sigtimedwait(sigset, timeout, /)
PYTHON_NATIVE(signalSigTimedWait)
{
    NATIVE_PROLOGUE();
    sigset_t mask;
    toSignalSet(globalObject, args[0], mask);
    RETURN_IF_EXCEPTION(scope, { });
    auto converted = timeFromSecondsObject(globalObject, args[1], TimeRounding::Ceiling);
    RETURN_IF_EXCEPTION(scope, { });
    int64_t timeout = *converted;
    if (timeout < 0)
        return JSValue::encode(raiseValueError(globalObject, scope, "timeout must be non-negative"_s));
    int64_t deadline = deadlineAfter(timeout);
    siginfo_t information;
    while (true) {
        struct timespec time;
        if (!timeAsTimespec(globalObject, timeout, time))
            return { };
        if (sigtimedwait(&mask, &information, &time) != -1)
            break;
        if (errno == EAGAIN)
            RETURN_NONE();
        if (errno != EINTR)
            return JSValue::encode(raiseOSError(globalObject, scope, errno));
        checkSignals(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        timeout = timeUntil(deadline);
        if (timeout < 0)
            break;
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(signalInformation(globalObject, information)));
}

// pidfd_send_signal(pidfd, signalnum, siginfo=None, flags=0, /)
PYTHON_NATIVE(signalPidfdSendSignal)
{
    NATIVE_PROLOGUE();
    auto descriptor = toCInt(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    auto signal = toCInt(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    int flags = 0;
    if (JSValue given = args.at(3)) {
        auto number = toCInt(globalObject, given);
        RETURN_IF_EXCEPTION(scope, { });
        flags = *number;
    }
    if (JSValue information = args.at(2); information && !isNone(information))
        return JSValue::encode(raiseTypeError(globalObject, scope, "siginfo must be None"_s));
    if (syscall(SYS_pidfd_send_signal, *descriptor, *signal, nullptr, flags) < 0)
        return JSValue::encode(raiseOSError(globalObject, scope, errno));
    // It may be this process that it was sent to.
    checkSignals(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    RETURN_NONE();
}

#endif // OS(LINUX)

PYTHON_NATIVE(signalValidSignals)
{
    NATIVE_PROLOGUE();
    sigset_t mask;
    if (sigemptyset(&mask) || sigfillset(&mask))
        return JSValue::encode(raiseOSError(globalObject, scope, errno));
    RELEASE_AND_RETURN(scope, JSValue::encode(toSet(globalObject, mask)));
}

// pthread_kill(thread_id, signalnum, /)
PYTHON_NATIVE(signalPthreadKill)
{
    NATIVE_PROLOGUE();
    // PyIndex_Check()
    if (!typeOf(globalObject, args[0])->lookup(vm, vm.pythonNames().dunder_index))
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("pthread_kill() argument 1 must be int, not "_s, typeNameOfArgument(globalObject, args[0]))));
    JSValue integer = toInt(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    uint64_t thread = lowBitsOfInt(integer);
    auto signal = toCInt(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    if (!audit(globalObject, "signal.pthread_kill"_s, intFromUInt64(globalObject, thread), jsNumber(*signal)))
        return { };
    if (int error = pthread_kill(std::bit_cast<pthread_t>(static_cast<uintptr_t>(thread)), *signal))
        return JSValue::encode(raiseOSError(globalObject, scope, error));
    // It may be this thread that it was sent to.
    checkSignals(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    RETURN_NONE();
}

JSObject* createSignalModule(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRealm* realm = globalObject->pyRealm();
    auto& state = signalState(globalObject);
    JSObject* module = newBuiltinModule(globalObject, "_signal"_s);
    addFunction(globalObject, module, "default_int_handler"_s, signalDefaultIntHandler);
    addFunction(globalObject, module, "alarm"_s, signalAlarm);
    addFunction(globalObject, module, "setitimer"_s, signalSetITimer);
    addFunction(globalObject, module, "getitimer"_s, signalGetITimer);
    addFunction(globalObject, module, "signal"_s, signalSignal);
    addFunction(globalObject, module, "raise_signal"_s, signalRaiseSignal);
    addFunction(globalObject, module, "strsignal"_s, signalStrSignal);
    addFunction(globalObject, module, "getsignal"_s, signalGetSignal);
    addFunction(globalObject, module, "set_wakeup_fd"_s, signalSetWakeupDescriptor);
    addFunction(globalObject, module, "siginterrupt"_s, signalSigInterrupt);
    addFunction(globalObject, module, "pause"_s, signalPause);
    addFunction(globalObject, module, "pthread_kill"_s, signalPthreadKill);
    addFunction(globalObject, module, "pthread_sigmask"_s, signalPthreadSigmask);
    addFunction(globalObject, module, "sigpending"_s, signalSigPending);
    addFunction(globalObject, module, "sigwait"_s, signalSigWait);
    addFunction(globalObject, module, "valid_signals"_s, signalValidSignals);
#if OS(LINUX)
    addFunction(globalObject, module, "sigwaitinfo"_s, signalSigWaitInfo);
    addFunction(globalObject, module, "sigtimedwait"_s, signalSigTimedWait);
    addFunction(globalObject, module, "pidfd_send_signal"_s, signalPidfdSendSignal);
    if (!state.information) {
        static constexpr ASCIILiteral fields[] = { "si_signo"_s, "si_code"_s, "si_errno"_s, "si_pid"_s, "si_uid"_s, "si_status"_s, "si_band"_s };
        PyType* type = createBuiltinType(globalObject, "signal.struct_siginfo"_s, realm->typeTuple(), PyType::Layout::Tuple, PyType::IsSequence | PyType::IsDerivedFromBuiltin);
        state.information.set(vm, realm, type);
        makeStructSequenceType(globalObject, type, std::span(fields), std::size(fields));
    }
    module->putDirect(vm, Identifier::fromString(vm, "struct_siginfo"_s), state.information->object());
#endif

    // PyErr_NewException("signal.ItimerError", PyExc_OSError, NULL)
    if (!state.itimerError) {
        PyDict* contents = PyDict::create(globalObject);
        contents->setString(globalObject, "__module__"_s, jsNontrivialString(vm, "signal"_s));
        JSValue type = newType(globalObject, realm->typeType(), jsNontrivialString(vm, "ItimerError"_s), PyTuple::create(globalObject, { realm->typeOSError() }), contents, nullptr);
        RETURN_IF_EXCEPTION(scope, nullptr);
        state.itimerError.set(vm, realm, asType(type));
    }

    // signal_add_constants()
    auto add = [&] (ASCIILiteral name, JSValue value) { module->putDirect(vm, Identifier::fromString(vm, name), value); };
#define ADD(name) add(#name ""_s, jsNumber(name))
    add("NSIG"_s, jsNumber(signalCount));
    ADD(SIG_BLOCK);
    ADD(SIG_UNBLOCK);
    ADD(SIG_SETMASK);
    ADD(SIGHUP);
    ADD(SIGINT);
    ADD(SIGQUIT);
    ADD(SIGILL);
    ADD(SIGTRAP);
    ADD(SIGIOT);
    ADD(SIGABRT);
#ifdef SIGEMT
    ADD(SIGEMT);
#endif
    ADD(SIGFPE);
    ADD(SIGKILL);
    ADD(SIGBUS);
    ADD(SIGSEGV);
    ADD(SIGSYS);
    ADD(SIGPIPE);
    ADD(SIGALRM);
    ADD(SIGTERM);
    ADD(SIGUSR1);
    ADD(SIGUSR2);
#ifdef SIGCLD
    ADD(SIGCLD);
#endif
    ADD(SIGCHLD);
#ifdef SIGPWR
    ADD(SIGPWR);
#endif
    ADD(SIGIO);
    ADD(SIGURG);
    ADD(SIGWINCH);
#ifdef SIGPOLL
    ADD(SIGPOLL);
#endif
    ADD(SIGSTOP);
    ADD(SIGTSTP);
    ADD(SIGCONT);
    ADD(SIGTTIN);
    ADD(SIGTTOU);
    ADD(SIGVTALRM);
    ADD(SIGPROF);
    ADD(SIGXCPU);
    ADD(SIGXFSZ);
#ifdef SIGRTMIN
    ADD(SIGRTMIN);
#endif
#ifdef SIGRTMAX
    ADD(SIGRTMAX);
#endif
#ifdef SIGINFO
    ADD(SIGINFO);
#endif
#ifdef SIGSTKFLT
    ADD(SIGSTKFLT);
#endif
#undef ADD
    // An enum, in glibc
#define ADD(name) add(#name ""_s, jsNumber(static_cast<int>(name)))
    ADD(ITIMER_REAL);
    ADD(ITIMER_VIRTUAL);
    ADD(ITIMER_PROF);
#undef ADD
    add("SIG_DFL"_s, defaultHandler());
    add("SIG_IGN"_s, ignoreHandler());
    add("ItimerError"_s, state.itimerError.get());

    if (!state.isMain)
        return module;
    // signal_get_set_handlers(): what is done about each as things are
    for (int signal = 1; signal < signalCount; ++signal) {
        Handler handler = getSystemHandler(signal);
        state.handlers[signal].set(vm, realm, handler == SIG_DFL ? defaultHandler() : handler == SIG_IGN ? ignoreHandler() : jsUndefined());
    }
    // And SIGINT raises KeyboardInterrupt, unless something else has been done about it.
    if (state.handlers[SIGINT].get() == defaultHandler()) {
        state.handlers[SIGINT].set(vm, realm, module->getDirect(vm, Identifier::fromString(vm, "default_int_handler"_s)));
        setSystemHandler(SIGINT, signalHandler);
    }
    return module;
}

} } // namespace JSC::Python
