/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include "JSCInlines.h"
#include "PyAbstract.h"
#include "PyErrors.h"
#include "PyException.h"
#include "PyInternals.h"
#include "PyModule.h"
#include "PyNumbers.h"
#include "PyStr.h"
#include "ExecutableAllocator.h"
#include "MachineContext.h"
#include "PyCodeInfo.h"
#include "PyThreads.h"
#include "TopExceptionScope.h"
#include <atomic>
#include <wtf/Condition.h>
#include <wtf/HexNumber.h>
#include <wtf/Lock.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/Threading.h>
#include <wtf/text/StringBuilder.h>

#if !OS(WINDOWS)

#include <pthread.h>
#include <signal.h>
#include <unistd.h>

namespace JSC {
namespace Python {

// The module faulthandler. (Modules/faulthandler.c and the end of Python/traceback.c in CPython are the reference.)
//
// CPython reads the stack of a thread from wherever it happens to be: within the handler of a signal, or from another thread. Here
// the thread of Python writes its own traceback, at its next safe point: a signal or the watchdog only ask for it (PySignal.cpp).
// A system call it waits in is interrupted for that. A long computation in C++ is not.
//
// Only a fatal signal (enable()) is different. There will be no safe point, so its handler walks the frames where they are,
// with nothing but what a signal handler may use. It takes the place of the handlers of the host, which report a crash in their
// own way: whoever enables this module has asked for Python's.

static constexpr unsigned faultMaxStringLength = 500;
static constexpr unsigned faultMaxFrames = 100;

namespace {

enum class FaultHeader : uint8_t { Stack, CurrentThread, Thread };

struct FaultUserSignal {
    bool isEnabled { false };
    bool allThreads { false };
    int fd { -1 };
    int64_t maxThreads { 100 };
};

struct FaultWatchdog {
    Lock lock;
    Condition condition;
    RefPtr<Thread> thread;
    bool isCancelled WTF_GUARDED_BY_LOCK(lock) { false };
    // These do not change while the thread runs.
    int fd { -1 };
    int64_t timeoutInMicroseconds { 0 };
    bool repeats { false };
    bool exits { false };
    int64_t maxThreads { 100 };
    Vector<char> header;
    std::atomic<bool> isDumped { false };
};

} // namespace

struct FaultFatalSignal {
    int number;
    ASCIILiteral name;
    bool isEnabled { false };
    struct sigaction previous { };
};

static bool s_faultIsEnabled;
static int s_faultFatalDescriptor = -1;
static bool s_faultFatalAllThreads;
static bool s_faultFatalCStack;
static int64_t s_faultFatalMaxThreads;
static JSGlobalObject* s_faultFatalGlobalObject;
static FaultFatalSignal s_faultFatalSignals[] = {
    { SIGBUS, "Bus error"_s },
    { SIGILL, "Illegal instruction"_s },
    { SIGFPE, "Floating-point exception"_s },
    { SIGABRT, "Aborted"_s },
    // The last, so that it is the first to be restored.
    { SIGSEGV, "Segmentation fault"_s },
};
static FaultUserSignal s_faultUserSignals[NSIG];

static FaultWatchdog& faultWatchdog()
{
    static NeverDestroyed<FaultWatchdog> watchdog;
    return watchdog;
}

static Vector<char> faultToBytes(StringBuilder& builder)
{
    auto utf8 = builder.toString().utf8();
    return Vector<char>(byteCast<char>(utf8.span()));
}

static void faultWrite(int fd, std::span<const char> text)
{
    while (!text.empty()) {
        ssize_t written = write(fd, text.data(), text.size());
        if (written < 0) {
            if (errno == EINTR)
                continue;
            return;
        }
        text = text.subspan(static_cast<size_t>(written));
    }
}

static void faultAppendEscaped(StringBuilder& builder, StringView text)
{
    unsigned count = 0;
    for (char32_t character : text.codePoints()) {
        if (count++ == faultMaxStringLength) {
            builder.append("..."_s);
            return;
        }
        if (character >= ' ' && character <= 126)
            builder.append(static_cast<Latin1Character>(character));
        else if (character <= 0xff)
            builder.append("\\x"_s, hex(character, 2, Lowercase));
        else if (character <= 0xffff)
            builder.append("\\u"_s, hex(character, 4, Lowercase));
        else
            builder.append("\\U"_s, hex(character, 8, Lowercase));
    }
}

// Raises nothing.
static void faultAppendFrames(JSGlobalObject* globalObject, StringBuilder& builder, JSValue frame)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    if (!frame)
        builder.append("  <no Python frame>\n"_s);
    auto text = [&](JSValue object, ASCIILiteral name) -> String {
        JSValue value = getAttr(globalObject, object, internString(vm, name));
        if (!value || !isStr(value))
            return { };
        return strValue(value)->value(globalObject);
    };
    for (unsigned depth = 0; frame && !isNone(frame) && depth < faultMaxFrames; ++depth) {
        JSValue code = getAttr(globalObject, frame, internString(vm, "f_code"_s));
        JSValue line = code ? getAttr(globalObject, frame, internString(vm, "f_lineno"_s)) : JSValue();
        String fileName = line ? text(code, "co_filename"_s) : String();
        String name = line ? text(code, "co_name"_s) : String();
        if (scope.exception() || fileName.isNull() || name.isNull()) {
            builder.append("  <invalid frame>\n"_s);
            break;
        }
        builder.append("  File \""_s);
        faultAppendEscaped(builder, fileName);
        builder.append("\", line "_s);
        if (line.isInt32() && line.asInt32() >= 0)
            builder.append(line.asInt32());
        else
            builder.append("???"_s);
        builder.append(" in "_s);
        faultAppendEscaped(builder, name);
        builder.append('\n');
        frame = getAttr(globalObject, frame, internString(vm, "f_back"_s));
    }
    if (scope.exception())
        takePendingException(globalObject);
}

// Raises nothing.
static void faultDumpTraceback(JSGlobalObject* globalObject, int fd, FaultHeader header, int64_t maxThreads)
{
    StrBuilder builder;
    if (header == FaultHeader::Stack) {
        builder.append("Stack (most recent call first):\n"_s);
        faultAppendFrames(globalObject, builder, currentFrame(globalObject));
        faultWrite(fd, faultToBytes(builder).span());
        return;
    }

    // All the threads, the one started last first.
    Threads& threads = runtime(globalObject).threads();
    ThreadState& current = threads.current();
    Vector<Ref<ThreadState>, 8> all;
    threads.forEachThread([&](ThreadState& thread) {
        // One that is neither has not run yet.
        if (&thread == &current || thread.parked)
            all.append(thread);
    });
    all.reverse();
    int64_t count = 0;
    for (auto& thread : all) {
        if (count)
            builder.append('\n');
        if (count++ >= maxThreads) {
            builder.append("...\n"_s);
            break;
        }
        bool isCurrent = thread.ptr() == &current;
        builder.append(isCurrent && header == FaultHeader::CurrentThread ? "Current thread 0x"_s : "Thread 0x"_s, hex(thread->ident, 16, Lowercase));
        char name[100];
        if (!pthread_getname_np(static_cast<pthread_t>(thread->ident), name, sizeof(name)) && name[0])
            builder.append(" ["_s, String::fromLatin1(name), ']');
        builder.append(" (most recent call first):\n"_s);
        JSValue frame = isCurrent ? currentFrame(globalObject) : JSValue(topFrameOf(globalObject, *thread->parked));
        faultAppendFrames(globalObject, builder, frame);
    }
    faultWrite(fd, faultToBytes(builder).span());
}

void dumpTracebackForSignal(JSGlobalObject* globalObject, int number)
{
    if (!number) {
        auto& watchdog = faultWatchdog();
        faultDumpTraceback(globalObject, watchdog.fd, FaultHeader::Thread, watchdog.maxThreads);
        watchdog.isDumped.store(true);
        return;
    }
    auto& user = s_faultUserSignals[number];
    if (user.isEnabled)
        faultDumpTraceback(globalObject, user.fd, user.allThreads ? FaultHeader::CurrentThread : FaultHeader::Stack, user.maxThreads);
}

// ---- Arguments ----

// Never seen from Python: its dict holds the files that are written to, so that they stay open.
static constexpr TypeDef faultStateTypeDef { "faulthandler._state"_s, { TypeFlag::DisallowInstantiation }, { }, { }, { } };

static void faultKeepFile(JSGlobalObject* globalObject, const String& purpose, JSValue file)
{
    Runtime& rt = runtime(globalObject);
    rt.typeFor(faultStateTypeDef)->dict()->set(rt.vm(), internString(rt.vm(), purpose), file ? file : jsUndefined());
}

// The descriptor of the file, which is sys.stderr if none is given. `file` is left empty if it is a number. -1: an exception.
static int faultGetDescriptor(JSGlobalObject* globalObject, JSValue& file)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!file || isNone(file)) {
        file = rt.sysDict()->get(rt.names.stderr_());
        if (!file) {
            raiseRuntimeError(globalObject, scope, "lost sys.stderr"_s);
            return -1;
        }
        if (isNone(file)) {
            raiseRuntimeError(globalObject, scope, "sys.stderr is None"_s);
            return -1;
        }
    } else if (isInt(file)) {
        if (file.isBoolean()) {
            warn(globalObject, rt.RuntimeWarning(), "bool is used as a file descriptor"_s);
            RETURN_IF_EXCEPTION(scope, -1);
        }
        int fd = toCInt(globalObject, file);
        RETURN_IF_EXCEPTION(scope, -1);
        if (fd < 0) {
            raiseValueError(globalObject, scope, "file is not a valid file descriptor"_s);
            return -1;
        }
        file = { };
        return fd;
    }

    JSValue result = callMethod(globalObject, file, internString(vm, "fileno"_s));
    RETURN_IF_EXCEPTION(scope, -1);
    std::optional<int64_t> fd;
    if (isInt(result))
        fd = tryInt64(intValue(result));
    if (!fd || *fd < 0 || *fd >= std::numeric_limits<int>::max()) {
        raiseRuntimeError(globalObject, scope, "file.fileno() is not a valid file descriptor"_s);
        return -1;
    }
    callMethod(globalObject, file, internString(vm, "flush"_s));
    if (scope.exception())
        takePendingException(globalObject);
    return static_cast<int>(*fd);
}

// all_threads, chain and the like, and max_threads. False: an exception.
static bool faultGetFlags(JSGlobalObject* globalObject, std::initializer_list<std::pair<JSValue, bool*>> flags, JSValue maxThreads, int64_t& maxThreadCount)
{
    maxThreadCount = 100;
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    for (auto [value, flag] : flags) {
        if (!value)
            continue;
        *flag = isTrue(globalObject, value);
        RETURN_IF_EXCEPTION(scope, false);
    }
    if (maxThreads) {
        maxThreadCount = toSsize(globalObject, maxThreads, rt.OverflowError());
        RETURN_IF_EXCEPTION(scope, false);
    }
    return true;
}

// ---- Functions ----

static JSValue faultDumpTracebackFunction(JSGlobalObject* globalObject, JSValue file, JSValue allThreadsValue, JSValue maxThreads)
{
    bool allThreads = true;
    int64_t maxThreadCount;
    if (!faultGetFlags(globalObject, { { allThreadsValue, &allThreads } }, maxThreads, maxThreadCount))
        return { };
    int fd = faultGetDescriptor(globalObject, file);
    if (fd < 0)
        return { };
    faultDumpTraceback(globalObject, fd, allThreads ? FaultHeader::CurrentThread : FaultHeader::Stack, maxThreadCount);
    if (!checkSignals(globalObject))
        return { };
    return jsUndefined();
}

static JSValue faultDumpCStack(JSGlobalObject* globalObject, JSValue file)
{
    int fd = faultGetDescriptor(globalObject, file);
    if (fd < 0)
        return { };
    faultWrite(fd, "Current thread's C stack trace (most recent call first):\n  <cannot get C stack on this system>\n"_span);
    if (!checkSignals(globalObject))
        return { };
    return jsUndefined();
}

// ---- Fatal signals ----

namespace {

// Text for a descriptor, from within a signal handler.
class FaultWriter {
public:
    explicit FaultWriter(int fd)
        : m_fd(fd)
    {
    }
    ~FaultWriter() { flush(); }

    void flush()
    {
        faultWrite(m_fd, std::span { m_buffer, m_used });
        m_used = 0;
    }
    void put(char character)
    {
        if (m_used == sizeof(m_buffer))
            flush();
        m_buffer[m_used++] = character;
    }
    void put(ASCIILiteral text)
    {
        for (char character : text.span())
            put(character);
    }
    void putDecimal(uint64_t number)
    {
        char digits[20];
        unsigned count = 0;
        do {
            digits[count++] = static_cast<char>('0' + number % 10);
            number /= 10;
        } while (number);
        while (count)
            put(digits[--count]);
    }
    void putHex(uint64_t number, unsigned width)
    {
        for (unsigned i = width; i--;)
            put("0123456789abcdef"[(number >> (i * 4)) & 15]);
    }
    // As faultAppendEscaped().
    void putEscaped(StringView text)
    {
        unsigned count = 0;
        for (char32_t character : text.codePoints()) {
            if (count++ == faultMaxStringLength) {
                put("..."_s);
                return;
            }
            if (character >= ' ' && character <= 126)
                put(static_cast<char>(character));
            else if (character <= 0xff) {
                put("\\x"_s);
                putHex(character, 2);
            } else if (character <= 0xffff) {
                put("\\u"_s);
                putHex(character, 4);
            } else {
                put("\\U"_s);
                putHex(character, 8);
            }
        }
    }

private:
    int m_fd;
    size_t m_used { 0 };
    char m_buffer[256];
};

} // namespace

static void faultPutFrames(FaultWriter& writer, CallFrame* topCallFrame, EntryFrame* entryFrame)
{
    unsigned depth = 0;
    forEachPythonFrameWithoutAllocating(topCallFrame, entryFrame, [&](const CodeInfo& info, int line) {
        if (depth++ == faultMaxFrames)
            return false;
        writer.put("  File \""_s);
        writer.putEscaped(info.filename);
        writer.put("\", line "_s);
        if (line >= 0)
            writer.putDecimal(static_cast<uint64_t>(line));
        else
            writer.put("???"_s);
        writer.put(" in "_s);
        writer.putEscaped(info.name.string());
        writer.put('\n');
        return true;
    });
    if (!depth)
        writer.put("  <no Python frame>\n"_s);
}

static void faultPutThreadHeader(FaultWriter& writer, uint64_t ident, bool isCurrent)
{
    writer.put(isCurrent ? "Current thread 0x"_s : "Thread 0x"_s);
    writer.putHex(ident, 16);
    char name[100];
    if (!pthread_getname_np(static_cast<pthread_t>(ident), name, sizeof(name)) && name[0]) {
        writer.put(" ["_s);
        for (const char* character = name; *character; ++character)
            writer.put(*character);
        writer.put(']');
    }
    writer.put(" (most recent call first):\n"_s);
}

// What faultDumpTraceback() writes, for a thread that cannot go on. It may or may not hold the GIL.
static void faultDumpTracebackInHandler(FaultWriter& writer)
{
    JSGlobalObject* globalObject = s_faultFatalGlobalObject;
    VM& vm = getVM(globalObject);
    Runtime& rt = runtime(globalObject);
    uint64_t self = currentThreadIdent();
    // The frames of a thread without the GIL have been set aside; those the VM knows are those of the thread that has it.
    JSLock::ParkedThread* parkedSelf = nullptr;
    bool isKnown = false;
    rt.threads().forEachThread([&](ThreadState& thread) {
        if (thread.ident != self)
            return;
        isKnown = true;
        parkedSelf = thread.parked;
    });
    auto putOwnFrames = [&] {
        if (rt.isRunningPendingCalls())
            writer.put("  Garbage-collecting\n"_s);
        if (parkedSelf)
            faultPutFrames(writer, parkedSelf->topCallFrame(), parkedSelf->topEntryFrame());
        else if (isKnown)
            faultPutFrames(writer, vm.topCallFrame, vm.topEntryFrame);
        else
            writer.put("  <no Python frame>\n"_s);
    };
    if (!s_faultFatalAllThreads) {
        writer.put("Stack (most recent call first):\n"_s);
        putOwnFrames();
        return;
    }

    // The one started last first.
    unsigned total = 0;
    rt.threads().forEachThread([&](ThreadState&) { ++total; });
    int64_t count = 0;
    for (unsigned index = total; index--;) {
        unsigned position = 0;
        rt.threads().forEachThread([&](ThreadState& thread) {
            if (position++ != index)
                return;
            bool isCurrent = thread.ident == self;
            // One that is neither running nor set aside has not run yet, unless it is the one that holds the GIL.
            if (count)
                writer.put('\n');
            if (count++ >= s_faultFatalMaxThreads) {
                if (count == s_faultFatalMaxThreads + 1)
                    writer.put("...\n"_s);
                return;
            }
            faultPutThreadHeader(writer, thread.ident, isCurrent);
            if (isCurrent)
                putOwnFrames();
            else if (thread.parked)
                faultPutFrames(writer, thread.parked->topCallFrame(), thread.parked->topEntryFrame());
            else
                faultPutFrames(writer, vm.topCallFrame, vm.topEntryFrame);
        });
        if (count > s_faultFatalMaxThreads)
            break;
    }
    if (!isKnown) {
        if (count)
            writer.put('\n');
        faultPutThreadHeader(writer, self, true);
        putOwnFrames();
    }
}

static void faultHandleFatalSignal(int number, siginfo_t* info, void* context)
{
    if (!s_faultIsEnabled)
        return;
    int savedErrno = errno;
    FaultFatalSignal* signal = nullptr;
    for (auto& candidate : s_faultFatalSignals) {
        if (candidate.number == number)
            signal = &candidate;
    }
    if (!signal)
        return;

    // A fault in code that the engine has generated may be one it means to happen, and handles: WebAssembly's bounds checks.
    if ((number == SIGSEGV || number == SIGBUS) && context && (signal->previous.sa_flags & SA_SIGINFO) && signal->previous.sa_sigaction) {
        auto pc = MachineContext::instructionPointer(static_cast<ucontext_t*>(context)->uc_mcontext);
        if (isJITPC(pc.untaggedPtr())) {
            signal->previous.sa_sigaction(number, info, context);
            errno = savedErrno;
            return;
        }
    }

    // Whatever goes wrong from here on ends the process.
    struct sigaction standard { };
    standard.sa_handler = SIG_DFL;
    sigaction(number, &standard, nullptr);
    signal->isEnabled = false;

    {
        FaultWriter writer(s_faultFatalDescriptor);
        writer.put("Fatal Python error: "_s);
        writer.put(signal->name);
        writer.put("\n\n"_s);
        writer.flush();
        faultDumpTracebackInHandler(writer);
        if (s_faultFatalCStack)
            writer.put("\nCurrent thread's C stack trace (most recent call first):\n  <cannot get C stack on this system>\n"_s);
    }
    errno = savedErrno;
    // It is not blocked (SA_NODEFER): this is the end.
    ::raise(number);
}

static void faultRestoreFatalSignals()
{
    for (auto& signal : s_faultFatalSignals) {
        if (std::exchange(signal.isEnabled, false))
            sigaction(signal.number, &signal.previous, nullptr);
    }
}

static JSValue faultEnable(JSGlobalObject* globalObject, JSValue file, JSValue allThreadsValue, JSValue cStackValue, JSValue maxThreads)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool allThreads = true;
    bool cStack = true;
    int64_t maxThreadCount;
    if (!faultGetFlags(globalObject, { { allThreadsValue, &allThreads }, { cStackValue, &cStack } }, maxThreads, maxThreadCount))
        return { };
    int fd = faultGetDescriptor(globalObject, file);
    if (fd < 0)
        return { };
    faultKeepFile(globalObject, "enable"_s, file);
    s_faultFatalDescriptor = fd;
    s_faultFatalAllThreads = allThreads;
    s_faultFatalCStack = cStack;
    s_faultFatalMaxThreads = maxThreadCount;
    s_faultFatalGlobalObject = globalObject;
    if (std::exchange(s_faultIsEnabled, true))
        return jsUndefined();
    for (auto& signal : s_faultFatalSignals) {
        struct sigaction action { };
        action.sa_sigaction = faultHandleFatalSignal;
        sigemptyset(&action.sa_mask);
        // The handler raises the signal again. On the other stack, if the thread has one: its own may be what is full.
        action.sa_flags = SA_SIGINFO | SA_NODEFER | SA_ONSTACK;
        if (sigaction(signal.number, &action, &signal.previous)) {
            int error = errno;
            faultRestoreFatalSignals();
            s_faultIsEnabled = false;
            return raiseOSError(globalObject, scope, error);
        }
        signal.isEnabled = true;
    }
    return jsUndefined();
}

static JSValue faultDisable(JSGlobalObject* globalObject)
{
    faultKeepFile(globalObject, "enable"_s, { });
    faultRestoreFatalSignals();
    return jsBoolean(std::exchange(s_faultIsEnabled, false));
}

static JSValue faultIsEnabled(JSGlobalObject*) { return jsBoolean(s_faultIsEnabled); }

// ---- The watchdog ----

void cancelTracebackDumpRequests()
{
    auto& watchdog = faultWatchdog();
    if (!watchdog.thread)
        return;
    {
        Locker locker { watchdog.lock };
        watchdog.isCancelled = true;
        watchdog.condition.notifyAll();
    }
    watchdog.thread->waitForCompletion();
    watchdog.thread = nullptr;
}

static void faultRunWatchdog()
{
    // (CPython blocks all signals in this thread. Here no thread may block the one the engine suspends threads with: the
    // allocator's scavenger would wait for ever. And a signal that arrives here is sent on to the thread of Python anyway.)
    auto& watchdog = faultWatchdog();
    Locker locker { watchdog.lock };
    do {
        watchdog.condition.waitFor(watchdog.lock, Seconds::fromMicroseconds(static_cast<double>(watchdog.timeoutInMicroseconds)), [&]() WTF_REQUIRES_LOCK(watchdog.lock) { return watchdog.isCancelled; });
        if (watchdog.isCancelled)
            return;
        faultWrite(watchdog.fd, watchdog.header.span());
        watchdog.isDumped.store(false);
        requestTracebackDump();
        if (watchdog.exits) {
            // The thread of Python may be beyond reach. It is given a moment.
            for (unsigned i = 0; i < 200 && !watchdog.isDumped.load(); ++i)
                WTF::sleep(5_ms);
            _exit(1);
        }
    } while (watchdog.repeats);
}

static JSValue faultDumpTracebackLater(JSGlobalObject* globalObject, JSValue timeoutValue, JSValue repeatValue, JSValue file, JSValue exitValue, JSValue maxThreads)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool repeats = false;
    bool exits = false;
    int64_t maxThreadCount;
    if (!faultGetFlags(globalObject, { { repeatValue, &repeats }, { exitValue, &exits } }, maxThreads, maxThreadCount))
        return { };
    int64_t nanoseconds;
    if (!nanosecondsFromSeconds(globalObject, timeoutValue, TimeRounding::AwayFromZero, nanoseconds))
        return { };
    int64_t microseconds = nanoseconds / 1000 + (nanoseconds % 1000 > 0);
    if (microseconds <= 0)
        return raiseValueError(globalObject, scope, "timeout must be greater than 0"_s);
    int fd = faultGetDescriptor(globalObject, file);
    if (fd < 0)
        return { };

    cancelTracebackDumpRequests();
    faultKeepFile(globalObject, "dump_traceback_later"_s, file);
    auto& watchdog = faultWatchdog();
    int64_t seconds = microseconds / 1000000;
    auto twoDigits = [](int64_t number) { return makeString(number < 10 ? "0"_s : ""_s, number); };
    StrBuilder header;
    header.append("Timeout ("_s, seconds / 3600, ':', twoDigits(seconds / 60 % 60), ':', twoDigits(seconds % 60));
    if (int64_t rest = microseconds % 1000000) {
        String digits = String::number(rest);
        header.append('.');
        for (unsigned i = digits.length(); i < 6; ++i)
            header.append('0');
        header.append(digits);
    }
    header.append(")!\n"_s);
    watchdog.header = faultToBytes(header);
    watchdog.fd = fd;
    watchdog.timeoutInMicroseconds = microseconds;
    watchdog.repeats = repeats;
    watchdog.exits = exits;
    watchdog.maxThreads = maxThreadCount;
    {
        Locker locker { watchdog.lock };
        watchdog.isCancelled = false;
    }
    prepareForTracebackDumpRequests(globalObject);
    watchdog.thread = Thread::create("faulthandler watchdog"_s, faultRunWatchdog);
    return jsUndefined();
}

static JSValue faultCancelDumpTracebackLater(JSGlobalObject* globalObject)
{
    cancelTracebackDumpRequests();
    faultKeepFile(globalObject, "dump_traceback_later"_s, { });
    return jsUndefined();
}

// ---- Signals ----

static bool faultCheckSignal(JSGlobalObject* globalObject, int number)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    for (int fatal : { SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGSEGV }) {
        if (number == fatal) {
            raiseRuntimeError(globalObject, scope, "signal "_s, number, " cannot be registered, use enable() instead"_s);
            return false;
        }
    }
    if (number < 1 || number >= NSIG) {
        raiseValueError(globalObject, scope, "signal number out of range"_s);
        return false;
    }
    return true;
}

static JSValue faultRegister(JSGlobalObject* globalObject, JSValue numberValue, JSValue file, JSValue allThreadsValue, JSValue chainValue, JSValue maxThreads)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int number = toCInt(globalObject, numberValue);
    RETURN_IF_EXCEPTION(scope, { });
    bool allThreads = true;
    bool chains = false;
    int64_t maxThreadCount;
    if (!faultGetFlags(globalObject, { { allThreadsValue, &allThreads }, { chainValue, &chains } }, maxThreads, maxThreadCount))
        return { };
    if (!faultCheckSignal(globalObject, number))
        return { };
    int fd = faultGetDescriptor(globalObject, file);
    if (fd < 0)
        return { };
    auto& user = s_faultUserSignals[number];
    if (int error = watchSignal(globalObject, number, chains))
        return raiseOSError(globalObject, scope, error);
    faultKeepFile(globalObject, makeString("signal "_s, number), file);
    user.fd = fd;
    user.allThreads = allThreads;
    user.maxThreads = maxThreadCount;
    user.isEnabled = true;
    return jsUndefined();
}

static JSValue faultUnregister(JSGlobalObject* globalObject, JSValue numberValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int number = toCInt(globalObject, numberValue);
    RETURN_IF_EXCEPTION(scope, { });
    if (!faultCheckSignal(globalObject, number))
        return { };
    auto& user = s_faultUserSignals[number];
    if (!std::exchange(user.isEnabled, false))
        return jsBoolean(false);
    unwatchSignal(number);
    faultKeepFile(globalObject, makeString("signal "_s, number), { });
    user.fd = -1;
    return jsBoolean(true);
}

// ---- For tests: ways to crash ----

static JSValue faultRaise(int number)
{
    // More than once: a handler of the engine steps aside and returns, for the instruction that failed to fail again.
    for (;;)
        ::raise(number);
    return jsUndefined();
}

static JSValue faultSigSegv(JSGlobalObject*, JSValue) { return faultRaise(SIGSEGV); }
static JSValue faultSigFpe(JSGlobalObject*) { return faultRaise(SIGFPE); }

static JSValue faultSigAbrt(JSGlobalObject*)
{
    abort();
    return jsUndefined();
}

static constexpr NativeDef faultFunctions[] = {
    PY_FUNCTION("enable", faultEnable, "(file=None, all_threads=True, c_stack=True, *, max_threads=100)"),
    PY_FUNCTION("disable", faultDisable, "()"),
    PY_FUNCTION("is_enabled", faultIsEnabled, "()"),
    PY_FUNCTION("dump_traceback", faultDumpTracebackFunction, "(file=None, all_threads=True, *, max_threads=100)"),
    PY_FUNCTION("dump_c_stack", faultDumpCStack, "(file=None)"),
    PY_FUNCTION("dump_traceback_later", faultDumpTracebackLater, "(timeout, repeat=False, file=None, exit=False, *, max_threads=100)"),
    PY_FUNCTION("cancel_dump_traceback_later", faultCancelDumpTracebackLater, "()"),
    PY_FUNCTION("register", faultRegister, "(signum, file=None, all_threads=True, chain=False, *, max_threads=100)"),
    PY_FUNCTION("unregister", faultUnregister, "(signum, /)"),
    PY_FUNCTION("_sigsegv", faultSigSegv, "(release_gil=False, /)"),
    PY_FUNCTION("_sigabrt", faultSigAbrt, "()"),
    PY_FUNCTION("_sigfpe", faultSigFpe, "()"),
};

void initializeFaultHandlerModule(JSGlobalObject* globalObject, PyModule* module)
{
    module->addFunctions(globalObject, faultFunctions);
}

} // namespace Python
} // namespace JSC

#endif // !OS(WINDOWS)
