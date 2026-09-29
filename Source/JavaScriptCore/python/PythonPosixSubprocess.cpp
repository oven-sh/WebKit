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
#include "PythonPosixModule.h"

#include "JSCInlines.h"
#include "PyRealm.h"
#include "PyTuple.h"
#include "PythonBuiltins.h"
#include "PythonBytes.h"
#include "PythonOperations.h"
#include "PythonPosix.h"
#include "PythonSequences.h"
#include "PythonSignals.h"
#include <dirent.h>
#include <fcntl.h>
#include <grp.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#if OS(LINUX)
#include <sys/syscall.h>
#endif

// The module _posixsubprocess: Modules/_posixsubprocess.c of CPython.
//
// There is no os.fork() here, because what it would leave in the new process is this thread alone, without those of the collector and the compilers. This forks all the same. What it does in the new process before that
// becomes another program is what can be done in the middle of anything: no memory is asked for, nothing is locked, and none of the engine is run.

namespace JSC { namespace Python {

namespace {

#if OS(DARWIN) || OS(FREEBSD)
constexpr const char* descriptorDirectory = "/dev/fd";
#else
constexpr const char* descriptorDirectory = "/proc/self/fd";
#endif

#if OS(LINUX)
// Only there is it known what can be done after it.
#define PYTHON_VFORK_USABLE 1
#endif

// All that the new process goes by, worked out beforehand.
struct ChildSetup {
    char* const* executables;
    char* const* arguments;
    char* const* environment;
    const char* directory;
    int p2cread, p2cwrite;
    int c2pread, c2pwrite;
    int errread, errwrite;
    int errpipeRead, errpipeWrite;
    bool closesDescriptors;
    bool restoresSignals;
    bool callsSetsid;
    pid_t processGroup;
    gid_t group;
    int64_t extraGroupCount;
    const gid_t* extraGroups;
    uid_t user;
    int umask;
    const sigset_t* signalMask;
    std::span<const int> descriptorsToKeep;
};

// ---- What is run in the new process

// _pos_int_from_ascii()
int positiveIntFromASCII(const char* name)
{
    int number = 0;
    while (*name >= '0' && *name <= '9') {
        number = number * 10 + (*name - '0');
        ++name;
    }
    return *name ? -1 : number;
}

// _is_fd_in_sorted_fd_sequence()
bool isKept(int descriptor, std::span<const int> sorted)
{
    int64_t low = 0;
    int64_t high = static_cast<int64_t>(sorted.size()) - 1;
    while (low <= high) {
        int64_t middle = (low + high) / 2;
        if (descriptor == sorted[middle])
            return true;
        if (descriptor > sorted[middle])
            low = middle + 1;
        else
            high = middle - 1;
    }
    return false;
}

// _Py_set_inheritable_async_safe()
int setInheritable(int descriptor, bool isInheritable)
{
    int flags = fcntl(descriptor, F_GETFD);
    if (flags < 0)
        return -1;
    int newFlags = isInheritable ? flags & ~FD_CLOEXEC : flags | FD_CLOEXEC;
    if (newFlags == flags)
        return 0;
    return fcntl(descriptor, F_SETFD, newFlags) < 0 ? -1 : 0;
}

// _Py_write_noraise()
void writeAll(int descriptor, const char* bytes, size_t count)
{
    ssize_t written;
    do {
        written = write(descriptor, bytes, count);
    } while (written < 0 && errno == EINTR);
}

// safe_get_max_fd()
long largestDescriptor()
{
    long result = sysconf(_SC_OPEN_MAX);
    return result == -1 ? 256 : result;
}

// _close_range_except(), with _brute_force_closer()
void closeAllExcept(int start, std::span<const int> kept)
{
    int end = static_cast<int>(std::min<long>(largestDescriptor(), std::numeric_limits<int>::max()));
    auto closeRange = [] (int first, int last) {
        for (int i = first; i <= last; ++i)
            close(i);
    };
    for (int descriptor : kept) {
        if (descriptor < start)
            continue;
        closeRange(start, descriptor - 1);
        start = descriptor + 1;
    }
    if (start <= end)
        closeRange(start, end);
}

#if OS(LINUX)
struct LinuxDirectoryEntry {
    unsigned long long d_ino;
    long long d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[256];
};

// _close_open_fds_safe(): the system is asked directly, opendir() and the like being free to ask for memory.
void closeOpenDescriptors(int start, std::span<const int> kept)
{
    int directory = open(descriptorDirectory, O_RDONLY | O_CLOEXEC);
    if (directory == -1) {
        closeAllExcept(start, kept);
        return;
    }
    alignas(LinuxDirectoryEntry) char buffer[sizeof(LinuxDirectoryEntry)];
    long bytes;
    while ((bytes = syscall(SYS_getdents64, directory, buffer, sizeof(buffer))) > 0) {
        LinuxDirectoryEntry* entry;
        for (long offset = 0; offset < bytes; offset += entry->d_reclen) {
            entry = reinterpret_cast<LinuxDirectoryEntry*>(buffer + offset);
            int descriptor = positiveIntFromASCII(entry->d_name);
            if (descriptor >= 0 && descriptor != directory && descriptor >= start && !isKept(descriptor, kept))
                close(descriptor);
        }
    }
    close(directory);
}
#else
// _close_open_fds_maybe_unsafe(): opendir() asks for memory, which is not among what can be done here. CPython does it, and so does the Java VM.
void closeOpenDescriptors(int start, std::span<const int> kept)
{
    DIR* directory = opendir(descriptorDirectory);
    if (!directory) {
        closeAllExcept(start, kept);
        return;
    }
    int usedByDirectory = dirfd(directory);
    errno = 0;
    while (struct dirent* entry = readdir(directory)) {
        int descriptor = positiveIntFromASCII(entry->d_name);
        if (descriptor >= 0 && descriptor != usedByDirectory && descriptor >= start && !isKept(descriptor, kept))
            close(descriptor);
        errno = 0;
    }
    // If reading it went wrong, there is no telling what is left.
    if (errno)
        closeAllExcept(start, kept);
    closedir(directory);
}
#endif

#if PYTHON_VFORK_USABLE
// reset_signal_handlers(): so that nothing of this process's is called when signals are let through again, in one that shares its memory.
void resetSignalHandlers(const sigset_t* mask)
{
    struct sigaction byDefault { };
    byDefault.sa_handler = SIG_DFL;
    for (int signal = 1; signal < _NSIG; ++signal) {
        if (signal == SIGKILL || signal == SIGSTOP || sigismember(mask, signal) == 1)
            continue;
        struct sigaction action;
        if (sigaction(signal, nullptr, &action) == -1)
            continue;
        void* handler = action.sa_flags & SA_SIGINFO ? reinterpret_cast<void*>(action.sa_sigaction) : reinterpret_cast<void*>(action.sa_handler);
        if (handler == reinterpret_cast<void*>(SIG_IGN) || handler == reinterpret_cast<void*>(SIG_DFL))
            continue;
        sigaction(signal, &byDefault, nullptr);
    }
}
#endif

// child_exec(). If it returns, there was no becoming another program, and whoever is at the other end of the pipe has been told why.
NEVER_INLINE void executeInChild(ChildSetup& setup)
{
    // Until it is otherwise, what has gone wrong went wrong before there was any trying.
    const char* message = "noexec";
    int savedError;
    char hexadecimal[sizeof(savedError) * 2 + 1];
#define POSIX_CALL(call) do { if ((call) == -1) goto error; } while (0)

    // make_inheritable()
    for (int descriptor : setup.descriptorsToKeep) {
        // This one is to be closed by exec(), though it is kept until then.
        if (descriptor == setup.errpipeWrite)
            continue;
        if (setInheritable(descriptor, true) < 0)
            goto error;
    }

    // The other ends of the pipes
    if (setup.p2cwrite != -1)
        POSIX_CALL(close(setup.p2cwrite));
    if (setup.c2pread != -1)
        POSIX_CALL(close(setup.c2pread));
    if (setup.errread != -1)
        POSIX_CALL(close(setup.errread));
    POSIX_CALL(close(setup.errpipeRead));

    // One that is 0, 1 or 2 already would be written over before it was copied.
    if (!setup.c2pwrite) {
        POSIX_CALL(setup.c2pwrite = dup(setup.c2pwrite));
        if (setInheritable(setup.c2pwrite, false) < 0)
            goto error;
    }
    while (!setup.errwrite || setup.errwrite == 1) {
        POSIX_CALL(setup.errwrite = dup(setup.errwrite));
        if (setInheritable(setup.errwrite, false) < 0)
            goto error;
    }

    // dup2() makes what it makes inheritable, and does nothing if there is nothing to copy.
    if (!setup.p2cread) {
        if (setInheritable(setup.p2cread, true) < 0)
            goto error;
    } else if (setup.p2cread != -1)
        POSIX_CALL(dup2(setup.p2cread, 0));
    if (setup.c2pwrite == 1) {
        if (setInheritable(setup.c2pwrite, true) < 0)
            goto error;
    } else if (setup.c2pwrite != -1)
        POSIX_CALL(dup2(setup.c2pwrite, 1));
    if (setup.errwrite == 2) {
        if (setInheritable(setup.errwrite, true) < 0)
            goto error;
    } else if (setup.errwrite != -1)
        POSIX_CALL(dup2(setup.errwrite, 2));

    if (setup.directory && chdir(setup.directory) == -1) {
        message = "noexec:chdir";
        goto error;
    }
    if (setup.umask >= 0)
        umask(static_cast<mode_t>(setup.umask));
    if (setup.restoresSignals)
        restoreSignals();
#if PYTHON_VFORK_USABLE
    if (setup.signalMask) {
        resetSignalHandlers(setup.signalMask);
        if ((errno = pthread_sigmask(SIG_SETMASK, setup.signalMask, nullptr)))
            goto error;
    }
#endif
    if (setup.callsSetsid)
        POSIX_CALL(setsid());
    if (setup.processGroup >= 0)
        POSIX_CALL(setpgid(0, setup.processGroup));
    if (setup.extraGroupCount >= 0)
        POSIX_CALL(setgroups(static_cast<int>(setup.extraGroupCount), setup.extraGroups));
    if (setup.group != static_cast<gid_t>(-1))
        POSIX_CALL(setregid(setup.group, setup.group));
    if (setup.user != static_cast<uid_t>(-1))
        POSIX_CALL(setreuid(setup.user, setup.user));
    message = "";

    if (setup.closesDescriptors)
        closeOpenDescriptors(3, setup.descriptorsToKeep);

    // As os._execvpe() looks along PATH
    savedError = 0;
    for (size_t i = 0; setup.executables[i]; ++i) {
        if (setup.environment)
            execve(setup.executables[i], setup.arguments, setup.environment);
        else
            execv(setup.executables[i], setup.arguments);
        // It is the first thing to go wrong that is told of, and not the last.
        if (errno != ENOENT && errno != ENOTDIR && !savedError)
            savedError = errno;
    }
    if (savedError)
        errno = savedError;

error:
#undef POSIX_CALL
    savedError = errno;
    if (savedError) {
        writeAll(setup.errpipeWrite, "OSError:", 8);
        char* cursor = hexadecimal + sizeof(hexadecimal);
        while (savedError && cursor != hexadecimal) {
            *--cursor = "0123456789abcdef"[savedError % 16];
            savedError /= 16;
        }
        writeAll(setup.errpipeWrite, cursor, static_cast<size_t>(hexadecimal + sizeof(hexadecimal) - cursor));
        writeAll(setup.errpipeWrite, ":", 1);
    } else
        writeAll(setup.errpipeWrite, "SubprocessError:0:", 18);
    writeAll(setup.errpipeWrite, message, strlen(message));
}

// do_fork_exec(). It is a function of its own so that vfork(), after which the two processes run on the one stack, is by itself.
NEVER_INLINE pid_t forkAndExecute(ChildSetup& setup)
{
    pid_t pid;
#if PYTHON_VFORK_USABLE
    if (setup.signalMask) {
        pid = vfork();
        // It can be forbidden where fork() is not.
        if (pid == static_cast<pid_t>(-1))
            pid = fork();
    } else
#endif
        pid = fork();
    if (pid)
        return pid;
    executeInChild(setup);
    _exit(255);
}

// ---- What is done beforehand

// _PySequence_BytesToCharpArray(). False if it raised.
bool toStringArray(JSGlobalObject* globalObject, JSValue sequence, StringArray& result)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    auto count = length(globalObject, sequence);
    RETURN_IF_EXCEPTION(scope, false);
    for (int64_t i = 0; i < count; ++i) {
        JSValue item = getItem(globalObject, sequence, intFromInt64(globalObject, i));
        RETURN_IF_EXCEPTION(scope, false);
        // PyBytes_AsStringAndSize()
        if (!isInstance(globalObject, item, globalObject->pyRealm()->typeBytes())) {
            raiseTypeError(globalObject, scope, concatenate("expected bytes, "_s, typeName(globalObject, item), " found"_s));
            return false;
        }
        Buffer buffer(item);
        auto bytes = buffer.span();
        if (WTF::find(bytes, static_cast<uint8_t>(0)) != notFound) {
            raiseValueError(globalObject, scope, "embedded null byte"_s);
            return false;
        }
        result.append(CString(byteCast<char>(bytes)));
    }
    return true;
}

} // anonymous namespace

// fork_exec(args, executable_list, close_fds, pass_fds, cwd, env, p2cread, p2cwrite, c2pread, c2pwrite, errread, errwrite, errpipe_read, errpipe_write, restore_signals, call_setsid, pgid_to_set, gid, extra_groups, uid,
// child_umask, preexec_fn, /)
PYTHON_NATIVE(subprocessForkExec)
{
    NATIVE_PROLOGUE();
    ChildSetup setup { };
    JSValue processArguments = args[0];
    JSValue executableList = args[1];
    setup.closesDescriptors = isTrue(globalObject, args[2]);
    RETURN_IF_EXCEPTION(scope, { });
    if (!isTuple(args[3]))
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("fork_exec() argument 4 must be tuple, not "_s, typeNameOfArgument(globalObject, args[3]))));
    PyTuple* descriptorsToKeep = asTuple(args[3]);
    JSValue directory = args[4];
    JSValue environmentList = args[5];
    for (auto [index, member] : std::initializer_list<std::pair<unsigned, int ChildSetup::*>> { { 6, &ChildSetup::p2cread }, { 7, &ChildSetup::p2cwrite }, { 8, &ChildSetup::c2pread }, { 9, &ChildSetup::c2pwrite }, { 10, &ChildSetup::errread }, { 11, &ChildSetup::errwrite }, { 12, &ChildSetup::errpipeRead }, { 13, &ChildSetup::errpipeWrite } }) {
        auto descriptor = toCInt(globalObject, args[index]);
        RETURN_IF_EXCEPTION(scope, { });
        setup.*member = *descriptor;
    }
    setup.restoresSignals = isTrue(globalObject, args[14]);
    RETURN_IF_EXCEPTION(scope, { });
    setup.callsSetsid = isTrue(globalObject, args[15]);
    RETURN_IF_EXCEPTION(scope, { });
    auto processGroup = toCInt(globalObject, args[16]);
    RETURN_IF_EXCEPTION(scope, { });
    setup.processGroup = *processGroup;
    JSValue group = args[17];
    JSValue extraGroups = args[18];
    JSValue user = args[19];
    auto mask = toCInt(globalObject, args[20]);
    RETURN_IF_EXCEPTION(scope, { });
    setup.umask = *mask;

    // It would be Python that was run in the new process, where there is no running it.
    if (!isNone(args[21]))
        return JSValue::encode(raise(globalObject, scope, BuiltinType::RuntimeError, "preexec_fn is not supported: nothing of Python's can be run between fork() and exec()"_s));
    if (setup.closesDescriptors && setup.errpipeWrite < 3)
        return JSValue::encode(raiseValueError(globalObject, scope, "errpipe_write must be >= 3"_s));

    // _sanity_check_python_fd_sequence(): ints, none of them less than nothing, each more than the last
    Vector<int, 8> kept;
    int64_t previous = -1;
    for (auto& item : descriptorsToKeep->span()) {
        auto descriptor = isInstance(globalObject, item.get(), realm->typeInt()) ? tryInt64(toInt(globalObject, item.get())) : std::nullopt;
        RETURN_IF_EXCEPTION(scope, { });
        if (!descriptor || *descriptor < 0 || *descriptor <= previous || *descriptor > std::numeric_limits<int>::max())
            return JSValue::encode(raiseValueError(globalObject, scope, "bad value(s) in fds_to_keep"_s));
        previous = *descriptor;
        kept.append(static_cast<int>(*descriptor));
    }
    setup.descriptorsToKeep = kept.span();

    StringArray executables;
    toStringArray(globalObject, executableList, executables);
    RETURN_IF_EXCEPTION(scope, { });
    setup.executables = executables.pointers();

    StringArray arguments;
    if (!isNone(processArguments)) {
        // PySequence_Fast()
        JSValue fast = processArguments;
        if (!isInstance(globalObject, fast, realm->typeList()) && !isTuple(fast)) {
            JSValue iterator = getIterator(globalObject, fast);
            if (scope.exception()) [[unlikely]] {
                if (catchException(globalObject, BuiltinType::TypeError))
                    raiseTypeError(globalObject, scope, "argv must be a tuple"_s);
                return { };
            }
            fast = call(globalObject, realm->typeList(), iterator);
            RETURN_IF_EXCEPTION(scope, { });
        }
        auto sizeOf = [&] { return isTuple(fast) ? asTuple(fast)->length() : asList(fast)->length(); };
        unsigned count = sizeOf();
        for (unsigned i = 0; i < count; ++i) {
            if (sizeOf() != count)
                return JSValue::encode(raise(globalObject, scope, BuiltinType::RuntimeError, "args changed during iteration"_s));
            auto encoded = toFileSystemEncoded(globalObject, isTuple(fast) ? asTuple(fast)->at(i) : asList(fast)->getIndexQuickly(i));
            RETURN_IF_EXCEPTION(scope, { });
            arguments.append(WTF::move(*encoded));
        }
        setup.arguments = arguments.pointers();
    }

    StringArray environment;
    if (!isNone(environmentList)) {
        toStringArray(globalObject, environmentList, environment);
        RETURN_IF_EXCEPTION(scope, { });
        setup.environment = environment.pointers();
    }

    CString directoryName;
    if (!isNone(directory)) {
        auto encoded = toFileSystemEncoded(globalObject, directory);
        RETURN_IF_EXCEPTION(scope, { });
        directoryName = WTF::move(*encoded);
        setup.directory = directoryName.data();
    }

    // Less than nothing is for setgroups() not to be called at all, which is not the same as calling it with none.
    setup.extraGroupCount = -2;
    Vector<gid_t, 16> groups;
    if (!isNone(extraGroups)) {
        if (!isInstance(globalObject, extraGroups, realm->typeList()))
            return JSValue::encode(raiseTypeError(globalObject, scope, "setgroups argument must be a list"_s));
        auto count = length(globalObject, extraGroups);
        RETURN_IF_EXCEPTION(scope, { });
        if (count > NGROUPS_MAX)
            return JSValue::encode(raiseValueError(globalObject, scope, "too many extra_groups"_s));
        for (int64_t i = 0; i < count; ++i) {
            JSValue item = getItem(globalObject, extraGroups, intFromInt64(globalObject, i));
            RETURN_IF_EXCEPTION(scope, { });
            if (!isInstance(globalObject, item, realm->typeInt()))
                return JSValue::encode(raiseTypeError(globalObject, scope, "extra_groups must be integers"_s));
            auto id = toGroupID(globalObject, item);
            if (scope.exception()) [[unlikely]] {
                if (catchException(globalObject, BuiltinType::BaseException))
                    raiseValueError(globalObject, scope, "invalid group id"_s);
                return { };
            }
            groups.append(*id);
        }
        setup.extraGroupCount = count;
        setup.extraGroups = count ? groups.span().data() : nullptr;
    }
    setup.group = static_cast<gid_t>(-1);
    if (!isNone(group)) {
        auto id = toGroupID(globalObject, group);
        RETURN_IF_EXCEPTION(scope, { });
        setup.group = *id;
    }
    setup.user = static_cast<uid_t>(-1);
    if (!isNone(user)) {
        auto id = toUserID(globalObject, user);
        RETURN_IF_EXCEPTION(scope, { });
        setup.user = *id;
    }

    if (!audit(globalObject, "_posixsubprocess.fork_exec"_s, executableList, processArguments, environmentList))
        return { };

    int savedError = 0;
#if PYTHON_VFORK_USABLE
    // vfork() only where it is safe. Every signal is kept back meanwhile, so that nothing this process has for one is called in a process that shares its memory.
    sigset_t previousMask;
    if (setup.user == static_cast<uid_t>(-1) && setup.group == static_cast<gid_t>(-1) && setup.extraGroupCount < 0) {
        sigset_t all;
        sigfillset(&all);
        if (int error = pthread_sigmask(SIG_BLOCK, &all, &previousMask))
            return JSValue::encode(raiseOSError(globalObject, scope, error));
        setup.signalMask = &previousMask;
    }
#endif
    pid_t pid = forkAndExecute(setup);
    if (pid == static_cast<pid_t>(-1))
        savedError = errno;
#if PYTHON_VFORK_USABLE
    if (setup.signalMask)
        pthread_sigmask(SIG_SETMASK, setup.signalMask, nullptr);
#endif
    if (savedError)
        return JSValue::encode(raiseOSError(globalObject, scope, savedError));
    return JSValue::encode(jsNumber(pid));
}

JSObject* createPosixSubprocessModule(JSGlobalObject* globalObject)
{
    JSObject* module = newBuiltinModule(globalObject, "_posixsubprocess"_s);
    addFunction(globalObject, module, "fork_exec"_s, subprocessForkExec);
    return module;
}

} } // namespace JSC::Python
