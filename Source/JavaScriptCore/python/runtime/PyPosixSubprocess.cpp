/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyPosix.h"

#if !OS(WINDOWS)

#include "JSCInlines.h"
#include <dirent.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// The module _posixsubprocess: what subprocess.Popen starts a process with. (Modules/_posixsubprocess.c in CPython.)
//
// A process that runs JSC has threads that a child made by fork() does not get, with locks they may hold, so no code of the
// engine, let alone of the program, can run in the child. There is no preexec_fn. What is done between fork() and exec()
// is written as C, works on memory that the parent has prepared, and calls nothing but what signal-safety(7) lists.

struct SubprocessRequest {
    char* const* executables; // To be tried in turn.
    char* const* arguments;
    char* const* environment; // Null: that of this process.
    const char* directory;
    // The ends of the pipes for the standard streams: p2c is parent to child. -1: none.
    int p2cRead;
    int p2cWrite;
    int c2pRead;
    int c2pWrite;
    int errRead;
    int errWrite;
    // Where the child reports what went wrong before exec(). That it is closed by exec() tells that nothing did.
    int errorPipeRead;
    int errorPipeWrite;
    bool closesDescriptors;
    bool restoresSignals;
    bool startsSession;
    pid_t processGroup; // Negative: as it is.
    gid_t gid; // -1: as it is.
    uid_t uid;
    ssize_t extraGroupCount; // Negative: as they are.
    const gid_t* extraGroups;
    int umask; // Negative: as it is.
    // Not null: the child shares memory with the parent (vfork), all signals are blocked, and this is the mask to run with.
    const sigset_t* signalMask;
    const int* descriptorsToKeep; // Sorted.
    size_t descriptorsToKeepCount;
    long descriptorLimit; // No descriptor is as high as this.
    int failureStatus; // What the child exits with if there is no new program.
};

// ---- In the child ----

static int subprocessSetInheritable(int fd, bool inheritable)
{
    int flags = fcntl(fd, F_GETFD);
    if (flags < 0)
        return -1;
    int wanted = inheritable ? flags & ~FD_CLOEXEC : flags | FD_CLOEXEC;
    return wanted == flags ? 0 : fcntl(fd, F_SETFD, wanted);
}

static void subprocessWrite(int fd, const char* bytes, size_t length)
{
    while (length) {
        ssize_t written = write(fd, bytes, length);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            return;
        }
        bytes += written;
        length -= written;
    }
}

static bool subprocessKeeps(const SubprocessRequest& request, int fd)
{
    size_t low = 0;
    size_t high = request.descriptorsToKeepCount;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (request.descriptorsToKeep[middle] == fd)
            return true;
        if (request.descriptorsToKeep[middle] < fd)
            low = middle + 1;
        else
            high = middle;
    }
    return false;
}

// Closes first..last. False: the system cannot.
static bool subprocessCloseRange(int first, int last)
{
#if OS(LINUX) && defined(SYS_close_range)
    return !syscall(SYS_close_range, static_cast<unsigned>(first), static_cast<unsigned>(last), 0u);
#elif OS(FREEBSD)
    return !close_range(first, last, 0);
#else
    UNUSED_PARAM(first);
    UNUSED_PARAM(last);
    return false;
#endif
}

static bool subprocessCloseOneByOne(int first, int last)
{
    for (int fd = first; fd <= last; ++fd)
        close(fd);
    return true;
}

// All from `first` up to `last` but those to keep.
static bool subprocessCloseAllBetween(const SubprocessRequest& request, int first, int last, bool (*closeRange)(int, int))
{
    for (size_t i = 0; i < request.descriptorsToKeepCount; ++i) {
        int kept = request.descriptorsToKeep[i];
        if (kept < first)
            continue;
        if (kept > last)
            break;
        if (kept > first && !closeRange(first, kept - 1))
            return false;
        first = kept + 1;
    }
    return first > last || closeRange(first, last);
}

#if OS(LINUX)
// The open descriptors are the names in /proc/self/fd. readdir() allocates memory, so this is the system call under it.
static bool subprocessCloseListed(const SubprocessRequest& request, int first)
{
    struct Entry {
        uint64_t inode;
        int64_t offset;
        unsigned short length;
        unsigned char type;
        char name[256];
    };
    int directory = open("/proc/self/fd", O_RDONLY | O_DIRECTORY);
    if (directory < 0)
        return false;
    alignas(Entry) char buffer[sizeof(Entry)];
    long count;
    while ((count = syscall(SYS_getdents64, directory, buffer, sizeof(buffer))) > 0) {
        for (long offset = 0; offset < count;) {
            auto* entry = reinterpret_cast<Entry*>(buffer + offset);
            offset += entry->length;
            int fd = 0;
            const char* digit = entry->name;
            while (*digit >= '0' && *digit <= '9')
                fd = fd * 10 + (*digit++ - '0');
            if (*digit || digit == entry->name)
                continue;
            if (fd != directory && fd >= first && !subprocessKeeps(request, fd))
                close(fd);
        }
    }
    close(directory);
    return true;
}
#endif

static void subprocessCloseDescriptors(const SubprocessRequest& request, int first)
{
    if (subprocessCloseAllBetween(request, first, INT_MAX, subprocessCloseRange))
        return;
#if OS(LINUX)
    if (subprocessCloseListed(request, first))
        return;
#endif
    subprocessCloseAllBetween(request, first, static_cast<int>(request.descriptorLimit) - 1, subprocessCloseOneByOne);
}

// While the child shares memory with the parent, a handler run in it would do its work on the parent's data.
static void subprocessResetSignalHandlers(const sigset_t* mask)
{
    struct sigaction standard { };
    standard.sa_handler = SIG_DFL;
    for (int signal = 1; signal < NSIG; ++signal) {
        // Those that stay blocked are reset by exec().
        if (signal == SIGKILL || signal == SIGSTOP || sigismember(mask, signal) == 1)
            continue;
        struct sigaction current;
        if (sigaction(signal, nullptr, &current) < 0)
            continue;
        void* handler = current.sa_flags & SA_SIGINFO ? reinterpret_cast<void*>(current.sa_sigaction) : reinterpret_cast<void*>(current.sa_handler);
        if (handler == reinterpret_cast<void*>(SIG_IGN) || handler == reinterpret_cast<void*>(SIG_DFL))
            continue;
        sigaction(signal, &standard, nullptr);
    }
}

// Returns only if there is no new program, having told the parent why.
static NEVER_INLINE void subprocessRunChild(const SubprocessRequest& request)
{
    // What follows the number of the error. "noexec": it happened before exec() was tried.
    const char* detail = "noexec";
    int c2pWrite = request.c2pWrite;
    int errWrite = request.errWrite;
    int firstError = 0;

#define SUBPROCESS_TRY(call) \
    do { \
        if ((call) == -1) \
            goto failed; \
    } while (0)

    for (size_t i = 0; i < request.descriptorsToKeepCount; ++i) {
        // This one is to stay open until exec(), and no longer.
        if (request.descriptorsToKeep[i] != request.errorPipeWrite)
            SUBPROCESS_TRY(subprocessSetInheritable(request.descriptorsToKeep[i], true));
    }

    // The parent's ends of the pipes.
    if (request.p2cWrite != -1)
        SUBPROCESS_TRY(close(request.p2cWrite));
    if (request.c2pRead != -1)
        SUBPROCESS_TRY(close(request.c2pRead));
    if (request.errRead != -1)
        SUBPROCESS_TRY(close(request.errRead));
    if (request.errorPipeRead != -1)
        SUBPROCESS_TRY(close(request.errorPipeRead));

    // One that is to become 1 or 2 and is now 0 or 1 would be overwritten before its turn.
    if (!c2pWrite) {
        SUBPROCESS_TRY(c2pWrite = dup(c2pWrite));
        SUBPROCESS_TRY(subprocessSetInheritable(c2pWrite, false));
    }
    while (!errWrite || errWrite == 1) {
        SUBPROCESS_TRY(errWrite = dup(errWrite));
        SUBPROCESS_TRY(subprocessSetInheritable(errWrite, false));
    }

    // dup2() makes a descriptor that exec() leaves open, unless it has nothing to do.
    if (!request.p2cRead)
        SUBPROCESS_TRY(subprocessSetInheritable(0, true));
    else if (request.p2cRead != -1)
        SUBPROCESS_TRY(dup2(request.p2cRead, 0));
    if (c2pWrite == 1)
        SUBPROCESS_TRY(subprocessSetInheritable(1, true));
    else if (c2pWrite != -1)
        SUBPROCESS_TRY(dup2(c2pWrite, 1));
    if (errWrite == 2)
        SUBPROCESS_TRY(subprocessSetInheritable(2, true));
    else if (errWrite != -1)
        SUBPROCESS_TRY(dup2(errWrite, 2));

    if (request.directory && chdir(request.directory) == -1) {
        detail = "noexec:chdir";
        goto failed;
    }
    if (request.umask >= 0)
        umask(static_cast<mode_t>(request.umask));
    if (request.restoresSignals) {
        // Those that an interpreter ignores, and most programs expect to be killed by.
        signal(SIGPIPE, SIG_DFL);
#ifdef SIGXFSZ
        signal(SIGXFSZ, SIG_DFL);
#endif
    }
    if (request.signalMask) {
        subprocessResetSignalHandlers(request.signalMask);
        if ((errno = pthread_sigmask(SIG_SETMASK, request.signalMask, nullptr)))
            goto failed;
    }
    if (request.startsSession)
        SUBPROCESS_TRY(setsid());
    if (request.processGroup >= 0)
        SUBPROCESS_TRY(setpgid(0, request.processGroup));
    if (request.extraGroupCount >= 0)
        SUBPROCESS_TRY(setgroups(request.extraGroupCount, request.extraGroups));
    if (request.gid != static_cast<gid_t>(-1))
        SUBPROCESS_TRY(setregid(request.gid, request.gid));
    if (request.uid != static_cast<uid_t>(-1))
        SUBPROCESS_TRY(setreuid(request.uid, request.uid));

    detail = "";
    if (request.closesDescriptors)
        subprocessCloseDescriptors(request, 3);

    // As os.execvpe() searches PATH. The first failure that is not "it is not there" is the one to report.
    for (size_t i = 0; request.executables[i]; ++i) {
        if (request.environment)
            execve(request.executables[i], request.arguments, request.environment);
        else
            execv(request.executables[i], request.arguments);
        if (errno != ENOENT && errno != ENOTDIR && !firstError)
            firstError = errno;
    }
    if (firstError)
        errno = firstError;

#undef SUBPROCESS_TRY

failed:
    if (request.errorPipeWrite == -1)
        return;
    // "OSError:<errno in hex>:<detail>". The parent looks the message up: strerror() is not for here.
    int error = errno;
    if (error) {
        char digits[sizeof(error) * 2];
        char* first = digits + sizeof(digits);
        for (unsigned rest = error; rest && first != digits; rest /= 16)
            *--first = "0123456789abcdef"[rest % 16];
        subprocessWrite(request.errorPipeWrite, "OSError:", 8);
        subprocessWrite(request.errorPipeWrite, first, digits + sizeof(digits) - first);
        subprocessWrite(request.errorPipeWrite, ":", 1);
    } else
        subprocessWrite(request.errorPipeWrite, "SubprocessError:0:", 18);
    subprocessWrite(request.errorPipeWrite, detail, strlen(detail));
}

// On its own, and not inlined: after vfork() the child runs on the parent's stack, while the parent waits, and nothing that
// the parent needs afterwards may be in the frame that both return into.
static NEVER_INLINE pid_t subprocessFork(const SubprocessRequest& request)
{
    pid_t pid = -1;
#if OS(LINUX)
    if (request.signalMask)
        pid = vfork();
#endif
    // Where it is not allowed, vfork() fails.
    if (pid == -1)
        pid = fork();
    if (pid)
        return pid;
    subprocessRunChild(request);
    _exit(request.failureStatus);
}

// ---- In the parent ----

// The process id of the child. -1: there is none, and errno says why.
static pid_t subprocessStart(SubprocessRequest& request)
{
    // vfork() does not copy the address space, which for this process is a lot of work. It will not do if the child is to be
    // somebody else: the C library changes the identity of all threads at once, by means that are not for a child that
    // shares memory; nor should processes of different privileges share any.
    sigset_t previousMask;
    bool sharesMemory = false;
#if OS(LINUX)
    sharesMemory = request.uid == static_cast<uid_t>(-1) && request.gid == static_cast<gid_t>(-1) && request.extraGroupCount < 0;
#endif
    if (sharesMemory) {
        sigset_t everything;
        sigfillset(&everything);
        if (int error = pthread_sigmask(SIG_BLOCK, &everything, &previousMask)) {
            errno = error;
            return -1;
        }
        request.signalMask = &previousMask;
    }
    pid_t pid = subprocessFork(request);
    int error = errno;
    // The parent goes on only when the child has left its memory.
    if (sharesMemory)
        pthread_sigmask(SIG_SETMASK, &previousMask, nullptr);
    errno = error;
    return pid;
}

// The strings of an argv or the like, and the array of pointers to them that ends with a null.
class SubprocessStrings {
public:
    void append(std::span<const uint8_t> bytes)
    {
        m_offsets.append(m_characters.size());
        m_characters.append(byteCast<char>(bytes));
        m_characters.append('\0');
    }

    char* const* finish()
    {
        m_pointers.shrink(0);
        for (size_t offset : m_offsets)
            m_pointers.append(m_characters.mutableSpan().data() + offset);
        m_pointers.append(nullptr);
        return m_pointers.span().data();
    }

private:
    Vector<char, 512> m_characters;
    Vector<size_t, 16> m_offsets;
    Vector<char*, 16> m_pointers;
};

// A sequence of bytes objects.
static bool subprocessGatherBytes(JSGlobalObject* globalObject, JSValue sequence, SubprocessStrings& result)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t count = length(globalObject, sequence);
    RETURN_IF_EXCEPTION(scope, false);
    for (int64_t i = 0; i < count; ++i) {
        JSValue item = getItem(globalObject, sequence, makeInt(globalObject, i));
        RETURN_IF_EXCEPTION(scope, false);
        auto* bytes = PyBytes::tryFrom(item);
        if (!bytes) {
            raiseTypeError(globalObject, scope, "expected bytes, "_s, typeName(globalObject, item), " found"_s);
            return false;
        }
        if (memchr(bytes->data(), 0, bytes->length())) {
            raiseValueError(globalObject, scope, "embedded null byte"_s);
            return false;
        }
        result.append(bytes->span());
    }
    return true;
}

enum SubprocessArgument : unsigned {
    ArgumentArgs,
    ArgumentExecutableList,
    ArgumentCloseFds,
    ArgumentPassFds,
    ArgumentCwd,
    ArgumentEnv,
    ArgumentP2CRead,
    ArgumentP2CWrite,
    ArgumentC2PRead,
    ArgumentC2PWrite,
    ArgumentErrRead,
    ArgumentErrWrite,
    ArgumentErrPipeRead,
    ArgumentErrPipeWrite,
    ArgumentRestoreSignals,
    ArgumentCallSetsid,
    ArgumentPgidToSet,
    ArgumentGid,
    ArgumentExtraGroups,
    ArgumentUid,
    ArgumentChildUmask,
    ArgumentPreexecFn,
    ArgumentCount
};

static JSValue subprocessForkExec(JSGlobalObject* globalObject, const CallArgs& args)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (args.keywordCount())
        return raiseTypeError(globalObject, scope, "_posixsubprocess.fork_exec() takes no keyword arguments"_s);
    if (args.size() != ArgumentCount)
        return raiseTypeError(globalObject, scope, "fork_exec expected "_s, static_cast<unsigned>(ArgumentCount), " arguments, got "_s, args.size());

    SubprocessRequest request { };
    request.failureStatus = 255;
    request.closesDescriptors = isTrue(globalObject, args[ArgumentCloseFds]);
    RETURN_IF_EXCEPTION(scope, { });
    auto* passFds = PyTuple::tryFrom(args[ArgumentPassFds]);
    if (!passFds)
        return raiseTypeError(globalObject, scope, "fork_exec() argument 4 must be tuple, not "_s, posixTypeNameOfArgument(globalObject, args[ArgumentPassFds]));
    if (!posixToInt(globalObject, args[ArgumentP2CRead], request.p2cRead) || !posixToInt(globalObject, args[ArgumentP2CWrite], request.p2cWrite)
        || !posixToInt(globalObject, args[ArgumentC2PRead], request.c2pRead) || !posixToInt(globalObject, args[ArgumentC2PWrite], request.c2pWrite)
        || !posixToInt(globalObject, args[ArgumentErrRead], request.errRead) || !posixToInt(globalObject, args[ArgumentErrWrite], request.errWrite)
        || !posixToInt(globalObject, args[ArgumentErrPipeRead], request.errorPipeRead) || !posixToInt(globalObject, args[ArgumentErrPipeWrite], request.errorPipeWrite))
        return { };
    request.restoresSignals = isTrue(globalObject, args[ArgumentRestoreSignals]);
    RETURN_IF_EXCEPTION(scope, { });
    request.startsSession = isTrue(globalObject, args[ArgumentCallSetsid]);
    RETURN_IF_EXCEPTION(scope, { });
    if (!posixToPid(globalObject, args[ArgumentPgidToSet], request.processGroup) || !posixToInt(globalObject, args[ArgumentChildUmask], request.umask))
        return { };

    if (!isNone(args[ArgumentPreexecFn]))
        return raiseRuntimeError(globalObject, scope, "preexec_fn not supported"_s);
    if (request.closesDescriptors && request.errorPipeWrite < 3)
        return raiseValueError(globalObject, scope, "errpipe_write must be >= 3"_s);

    Vector<int, 8> descriptorsToKeep;
    for (unsigned i = 0; i < passFds->length(); ++i) {
        JSValue item = passFds->at(i);
        auto fd = isInt(item) ? tryInt64(item) : std::nullopt;
        if (!fd || *fd < 0 || *fd > INT_MAX || (i && *fd <= descriptorsToKeep.last()))
            return raiseValueError(globalObject, scope, "bad value(s) in fds_to_keep"_s);
        descriptorsToKeep.append(static_cast<int>(*fd));
    }
    request.descriptorsToKeep = descriptorsToKeep.span().data();
    request.descriptorsToKeepCount = descriptorsToKeep.size();

    SubprocessStrings executables;
    SubprocessStrings arguments;
    SubprocessStrings environment;
    PosixBytes directory;
    if (!subprocessGatherBytes(globalObject, args[ArgumentExecutableList], executables))
        return { };
    request.executables = executables.finish();
    if (!isNone(args[ArgumentArgs])) {
        JSValue sequence = args[ArgumentArgs];
        JSArray* list = tryAsList(sequence);
        auto* tuple = PyTuple::tryFrom(sequence);
        if (!list && !tuple) {
            list = listFromIterable(globalObject, sequence);
            if (scope.exception()) [[unlikely]] {
                if (clearPendingExceptionIfMatches(globalObject, rt.TypeError()))
                    raiseTypeError(globalObject, scope, "argv must be a tuple"_s);
                return { };
            }
        }
        unsigned count = list ? listLength(list) : tuple->length();
        for (unsigned i = 0; i < count; ++i) {
            // __fspath__() of an earlier one may have done anything.
            if (list && listLength(list) != count)
                return raiseRuntimeError(globalObject, scope, "args changed during iteration"_s);
            PosixBytes argument;
            if (!posixToFSBytes(globalObject, list ? listGet(list, i) : tuple->at(i), argument))
                return { };
            arguments.append(byteCast<uint8_t>(argument.span()));
        }
        request.arguments = arguments.finish();
    }
    if (!isNone(args[ArgumentEnv])) {
        if (!subprocessGatherBytes(globalObject, args[ArgumentEnv], environment))
            return { };
        request.environment = environment.finish();
    }
    if (!isNone(args[ArgumentCwd])) {
        if (!posixToFSBytes(globalObject, args[ArgumentCwd], directory))
            return { };
        request.directory = directory.characters();
    }

    Vector<gid_t, 16> extraGroups;
    request.extraGroupCount = -1;
    if (!isNone(args[ArgumentExtraGroups])) {
        JSArray* list = tryAsList(args[ArgumentExtraGroups]);
        if (!list)
            return raiseTypeError(globalObject, scope, "setgroups argument must be a list"_s);
        if (listLength(list) > 65536)
            return raiseValueError(globalObject, scope, "too many extra_groups"_s);
        for (unsigned i = 0; i < listLength(list); ++i) {
            JSValue item = listGet(list, i);
            if (!isInt(item))
                return raiseTypeError(globalObject, scope, "extra_groups must be integers"_s);
            gid_t group;
            if (!posixToGid(globalObject, item, group)) {
                takePendingException(globalObject);
                return raiseValueError(globalObject, scope, "invalid group id"_s);
            }
            extraGroups.append(group);
        }
        request.extraGroupCount = extraGroups.size();
        request.extraGroups = extraGroups.isEmpty() ? nullptr : extraGroups.span().data();
    }
    request.gid = static_cast<gid_t>(-1);
    request.uid = static_cast<uid_t>(-1);
    if (!isNone(args[ArgumentGid]) && !posixToGid(globalObject, args[ArgumentGid], request.gid))
        return { };
    if (!isNone(args[ArgumentUid]) && !posixToUid(globalObject, args[ArgumentUid], request.uid))
        return { };

    if (!audit(globalObject, "_posixsubprocess.fork_exec"_s, args[ArgumentExecutableList], args[ArgumentArgs], args[ArgumentEnv]))
        return { };

    request.descriptorLimit = sysconf(_SC_OPEN_MAX);
    if (request.descriptorLimit < 0)
        request.descriptorLimit = 256;

    pid_t pid = subprocessStart(request);
    if (pid == -1)
        return posixError(globalObject);
    return jsNumber(pid);
}

pid_t posixSpawnChild(char* const* executables, char* const* arguments, char* const* environment)
{
    SubprocessRequest request { };
    request.executables = executables;
    request.arguments = arguments;
    request.environment = environment;
    request.p2cRead = request.p2cWrite = request.c2pRead = request.c2pWrite = request.errRead = request.errWrite = -1;
    request.errorPipeRead = request.errorPipeWrite = -1;
    request.processGroup = -1;
    request.gid = static_cast<gid_t>(-1);
    request.uid = static_cast<uid_t>(-1);
    request.extraGroupCount = -1;
    request.umask = -1;
    request.failureStatus = 127;
    return subprocessStart(request);
}

static constexpr NativeDef subprocessFunctions[] = {
    PY_FUNCTION("fork_exec", subprocessForkExec, "(*args)"),
};

void initializePosixSubprocessModule(JSGlobalObject* globalObject, PyModule* module)
{
    module->addFunctions(globalObject, subprocessFunctions);
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // !OS(WINDOWS)
