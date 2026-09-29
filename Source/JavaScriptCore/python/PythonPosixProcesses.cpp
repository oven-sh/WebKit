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
#include "PythonPosix.h"

#if OS(UNIX)

#include <errno.h>
#include <signal.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wtf/Scope.h>
#if OS(DARWIN)
#include <crt_externs.h>
#endif

// The functions of posix that start other programs and wait for them: Modules/posixmodule.c of CPython.
//
// There is no fork(). What it would leave in the new process is this thread alone, and the collector and the compilers have threads of their own that would be waited for and never answer. os has none on the systems
// where it cannot be had, so a program that can do without it looks to see.

namespace JSC { namespace Python {

#define CONVERT(variable, expression) \
    auto variable##Converted = expression; \
    RETURN_IF_EXCEPTION(scope, { }); \
    auto variable = *variable##Converted;
#define CONVERT_BOOL(variable, value, defaultValue) \
    bool variable = defaultValue; \
    if (JSValue given = value) { \
        variable = isTrue(globalObject, given); \
        RETURN_IF_EXCEPTION(scope, { }); \
    }

// Strings as a program is given them: each ended by a zero, and after the last of them, null.
class StringArray {
public:
    void append(CString&& string) { m_strings.append(WTF::move(string)); }
    size_t size() const { return m_strings.size(); }
    const CString& at(size_t i) const { return m_strings[i]; }
    char** pointers()
    {
        m_pointers.shrink(0);
        for (auto& string : m_strings)
            m_pointers.append(const_cast<char*>(string.data()));
        m_pointers.append(nullptr);
        return m_pointers.mutableSpan().data();
    }

private:
    Vector<CString> m_strings;
    Vector<char*> m_pointers;
};

static bool isListOrTuple(JSGlobalObject* globalObject, JSValue value)
{
    PyRealm* realm = globalObject->pyRealm();
    return isInstance(globalObject, value, realm->typeList()) || isInstance(globalObject, value, realm->typeTuple());
}

// PyMapping_Check(): whether it can be subscripted.
static bool isMapping(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = globalObject->vm();
    return !!typeOf(globalObject, value)->lookup(vm, vm.pythonNames().dunder_getitem);
}

// parse_arglist(). False if it raised.
static bool parseArguments(JSGlobalObject* globalObject, JSValue given, int64_t count, StringArray& result)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    for (int64_t i = 0; i < count; ++i) {
        JSValue item = getItem(globalObject, given, intFromInt64(globalObject, i));
        RETURN_IF_EXCEPTION(scope, false);
        auto string = toFileSystemEncoded(globalObject, item);
        RETURN_IF_EXCEPTION(scope, false);
        result.append(WTF::move(*string));
    }
    return true;
}

// parse_envlist(). False if it raised.
static bool parseEnvironment(JSGlobalObject* globalObject, JSValue environment, StringArray& result)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRealm* realm = globalObject->pyRealm();
    auto count = length(globalObject, environment);
    RETURN_IF_EXCEPTION(scope, false);
    // PyMapping_Keys() and PyMapping_Values(): a list of what the method of the name returns.
    auto listOf = [&] (ASCIILiteral method) -> JSValue {
        JSValue view = callMethodNamed(globalObject, environment, Identifier::fromString(vm, method));
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_AND_RETURN(scope, call(globalObject, realm->typeList(), view));
    };
    JSValue keys = listOf("keys"_s);
    RETURN_IF_EXCEPTION(scope, false);
    JSValue values = listOf("values"_s);
    RETURN_IF_EXCEPTION(scope, false);
    for (int64_t i = 0; i < count; ++i) {
        JSValue index = intFromInt64(globalObject, i);
        JSValue key = getItem(globalObject, keys, index);
        RETURN_IF_EXCEPTION(scope, false);
        JSValue value = getItem(globalObject, values, index);
        RETURN_IF_EXCEPTION(scope, false);
        auto name = toFileSystemEncoded(globalObject, key);
        RETURN_IF_EXCEPTION(scope, false);
        auto content = toFileSystemEncoded(globalObject, value);
        RETURN_IF_EXCEPTION(scope, false);
        if (!name->length() || WTF::find(name->span().subspan(1), '=') != notFound) {
            raiseValueError(globalObject, scope, "illegal environment variable name"_s);
            return false;
        }
        Vector<char> entry;
        entry.append(name->span());
        entry.append('=');
        entry.append(content->span());
        result.append(CString(entry.span()));
    }
    return true;
}

PYTHON_NATIVE(posixExecv)
{
    NATIVE_PROLOGUE();
    PathArgument path("execv"_s, "path"_s);
    if (!path.convert(globalObject, args.at(0)))
        return { };
    JSValue given = args.at(1);
    if (!isListOrTuple(globalObject, given))
        return JSValue::encode(raiseTypeError(globalObject, scope, "execv() arg 2 must be a tuple or list"_s));
    auto count = length(globalObject, given);
    RETURN_IF_EXCEPTION(scope, { });
    if (count < 1)
        return JSValue::encode(raiseValueError(globalObject, scope, "execv() arg 2 must not be empty"_s));
    StringArray arguments;
    if (!parseArguments(globalObject, given, count, arguments))
        return { };
    if (!arguments.at(0).length())
        return JSValue::encode(raiseValueError(globalObject, scope, "execv() arg 2 first element cannot be empty"_s));
    if (!audit(globalObject, "os.exec"_s, path.object, given, jsUndefined()))
        return { };
    ::execv(path.narrow(), arguments.pointers());
    // If it comes back, it did not work.
    return JSValue::encode(raisePosixError(globalObject, scope));
}

PYTHON_NATIVE(posixExecve)
{
    NATIVE_PROLOGUE();
#if OS(LINUX)
    PathArgument path("execve"_s, "path"_s, PathArgument::AllowsDescriptor);
#else
    PathArgument path("execve"_s, "path"_s);
#endif
    if (!path.convert(globalObject, args.at(0)))
        return { };
    JSValue given = args.at(1);
    JSValue environment = args.at(2);
    if (!isListOrTuple(globalObject, given))
        return JSValue::encode(raiseTypeError(globalObject, scope, "execve: argv must be a tuple or list"_s));
    auto count = length(globalObject, given);
    RETURN_IF_EXCEPTION(scope, { });
    if (count < 1)
        return JSValue::encode(raiseValueError(globalObject, scope, "execve: argv must not be empty"_s));
    if (!isMapping(globalObject, environment))
        return JSValue::encode(raiseTypeError(globalObject, scope, "execve: environment must be a mapping object"_s));
    StringArray arguments;
    if (!parseArguments(globalObject, given, count, arguments))
        return { };
    if (!arguments.at(0).length())
        return JSValue::encode(raiseValueError(globalObject, scope, "execve: argv first element cannot be empty"_s));
    StringArray variables;
    if (!parseEnvironment(globalObject, environment, variables))
        return { };
    if (!audit(globalObject, "os.exec"_s, path.object, given, environment))
        return { };
#if OS(LINUX)
    if (path.descriptor > -1)
        ::fexecve(path.descriptor, arguments.pointers(), variables.pointers());
    else
#endif
        ::execve(path.narrow(), arguments.pointers(), variables.pointers());
    return JSValue::encode(raisePathError(globalObject, scope, path));
}

// ---- posix_spawn()

// Py_NSIG
#if defined(NSIG)
static constexpr long signalCount = NSIG;
#else
static constexpr long signalCount = 64;
#endif

// _Py_Sigset_Converter(). False if it raised.
bool toSignalSet(JSGlobalObject* globalObject, JSValue given, sigset_t& mask)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    sigemptyset(&mask);
    JSValue iterator = getIterator(globalObject, given);
    RETURN_IF_EXCEPTION(scope, false);
    while (true) {
        JSValue item = iteratorNext(globalObject, iterator);
        RETURN_IF_EXCEPTION(scope, false);
        if (!item)
            return true;
        JSValue integer = toInt(globalObject, item);
        RETURN_IF_EXCEPTION(scope, false);
        // One that does not fit is shown as -1, as it is by CPython.
        long number = static_cast<long>(tryInt64(integer).value_or(-1));
        if (number <= 0 || number >= signalCount) {
            raiseValueError(globalObject, scope, concatenate("signal number "_s, static_cast<int64_t>(number), " out of range [1; "_s, static_cast<int64_t>(signalCount - 1), ']'));
            return false;
        }
        if (sigaddset(&mask, static_cast<int>(number))) {
            if (errno != EINVAL) {
                raisePosixError(globalObject, scope);
                return false;
            }
            // range(1, NSIG) has long been written, and there are numbers in it that are no signal's.
            if (!warn(globalObject, BuiltinType::RuntimeWarning, concatenate("invalid signal number "_s, static_cast<int64_t>(number), ", please use valid_signals()"_s)))
                return false;
        }
    }
}

enum SpawnAction : long { SpawnOpen, SpawnClose, SpawnDup2 };

// parse_file_actions(). It has been initialized. False if it raised.
static bool parseFileActions(JSGlobalObject* globalObject, JSValue given, posix_spawn_file_actions_t& actions)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    PyRealm* realm = globalObject->pyRealm();
    MarkedArgumentBuffer items;
    // PySequence_Fast()
    JSValue iterator = getIterator(globalObject, given);
    if (scope.exception()) {
        if (catchException(globalObject, BuiltinType::TypeError))
            raiseTypeError(globalObject, scope, "file_actions must be a sequence or None"_s);
        return false;
    }
    while (true) {
        JSValue item = iteratorNext(globalObject, iterator);
        RETURN_IF_EXCEPTION(scope, false);
        if (!item)
            break;
        items.append(item);
    }
    auto failed = [&] (int error) {
        if (!error)
            return false;
        errno = error;
        raisePosixError(globalObject, scope);
        return true;
    };
    for (unsigned i = 0; i < items.size(); ++i) {
        JSValue item = items.at(i);
        if (!isInstance(globalObject, item, realm->typeTuple()) || !asTuple(item)->length()) {
            raiseTypeError(globalObject, scope, "Each file_actions element must be a non-empty tuple"_s);
            return false;
        }
        PyTuple* action = asTuple(item);
        auto tag = toCLong(globalObject, action->at(0));
        RETURN_IF_EXCEPTION(scope, false);
        auto hasLength = [&] (unsigned length, ASCIILiteral message) {
            if (action->length() == length)
                return true;
            raiseTypeError(globalObject, scope, message);
            return false;
        };
        switch (*tag) {
        case SpawnOpen: {
            constexpr auto message = "A open file_action tuple must have 5 elements"_s;
            if (!hasLength(5, message))
                return false;
            auto descriptor = toCIntOfFormat(globalObject, action->at(1));
            RETURN_IF_EXCEPTION(scope, false);
            auto path = toFileSystemEncoded(globalObject, action->at(2));
            RETURN_IF_EXCEPTION(scope, false);
            auto flags = toCIntOfFormat(globalObject, action->at(3));
            RETURN_IF_EXCEPTION(scope, false);
            if (!isInstance(globalObject, action->at(4), realm->typeInt())) {
                raiseTypeError(globalObject, scope, message);
                return false;
            }
            if (failed(posix_spawn_file_actions_addopen(&actions, *descriptor, path->data(), *flags, static_cast<mode_t>(lowBitsOfInt(action->at(4))))))
                return false;
            break;
        }
        case SpawnClose: {
            constexpr auto message = "A close file_action tuple must have 2 elements"_s;
            if (!hasLength(2, message))
                return false;
            auto descriptor = toCIntOfFormat(globalObject, action->at(1));
            RETURN_IF_EXCEPTION(scope, false);
            if (failed(posix_spawn_file_actions_addclose(&actions, *descriptor)))
                return false;
            break;
        }
        case SpawnDup2: {
            constexpr auto message = "A dup2 file_action tuple must have 3 elements"_s;
            if (!hasLength(3, message))
                return false;
            auto descriptor = toCIntOfFormat(globalObject, action->at(1));
            RETURN_IF_EXCEPTION(scope, false);
            auto descriptor2 = toCIntOfFormat(globalObject, action->at(2));
            RETURN_IF_EXCEPTION(scope, false);
            if (failed(posix_spawn_file_actions_adddup2(&actions, *descriptor, *descriptor2)))
                return false;
            break;
        }
        default:
            raiseTypeError(globalObject, scope, "Unknown file_actions identifier"_s);
            return false;
        }
    }
    return true;
}

// posix_spawn() and posix_spawnp(): py_posix_spawn()
PYTHON_NATIVE(posixSpawn)
{
    NATIVE_PROLOGUE();
    bool searchesPath = unpack<bool>(callFrame, 0);
    ASCIILiteral function = searchesPath ? "posix_spawnp"_s : "posix_spawn"_s;
    PathArgument path(function, "path"_s);
    if (!path.convert(globalObject, args.at(0)))
        return { };
    JSValue given = args.at(1);
    JSValue environment = args.at(2);
    JSValue fileActions = args.at(3);
    JSValue processGroup = args.at(4);
    CONVERT_BOOL(resetsIDs, args.at(5), false);
    CONVERT_BOOL(setsSession, args.at(6), false);
    JSValue signalMask = args.at(7);
    JSValue defaultSignals = args.at(8);
    JSValue scheduler = args.at(9);

    if (!isListOrTuple(globalObject, given))
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate(function, ": argv must be a tuple or list"_s)));
    auto count = length(globalObject, given);
    RETURN_IF_EXCEPTION(scope, { });
    if (count < 1)
        return JSValue::encode(raiseValueError(globalObject, scope, concatenate(function, ": argv must not be empty"_s)));
    if (!isNone(environment) && !isMapping(globalObject, environment))
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate(function, ": environment must be a mapping object or None"_s)));
    if (scheduler && !isNone(scheduler) && !isInstance(globalObject, scheduler, realm->typeTuple()))
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate(function, ": scheduler must be a tuple or None"_s)));
    StringArray arguments;
    if (!parseArguments(globalObject, given, count, arguments))
        return { };
    if (!arguments.at(0).length())
        return JSValue::encode(raiseValueError(globalObject, scope, concatenate(function, ": argv first element cannot be empty"_s)));
    StringArray variables;
    char** environmentPointers;
    if (isNone(environment)) {
#if OS(DARWIN)
        environmentPointers = *_NSGetEnviron();
#else
        environmentPointers = environ;
#endif
    } else {
        if (!parseEnvironment(globalObject, environment, variables))
            return { };
        environmentPointers = variables.pointers();
    }

    posix_spawn_file_actions_t actions;
    bool hasActions = false;
    posix_spawnattr_t attributes;
    bool hasAttributes = false;
    auto cleanUp = makeScopeExit([&] {
        if (hasActions)
            posix_spawn_file_actions_destroy(&actions);
        if (hasAttributes)
            posix_spawnattr_destroy(&attributes);
    });
    auto failed = [&] (int error) {
        if (!error)
            return false;
        errno = error;
        raisePosixError(globalObject, scope);
        return true;
    };
    if (fileActions && !isNone(fileActions)) {
        if (failed(posix_spawn_file_actions_init(&actions)))
            return { };
        hasActions = true;
        if (!parseFileActions(globalObject, fileActions, actions))
            return { };
    }

    // parse_posix_spawn_flags()
    if (failed(posix_spawnattr_init(&attributes)))
        return { };
    hasAttributes = true;
    short flags = 0;
    if (processGroup && !isNone(processGroup)) {
        CONVERT(group, toCInt(globalObject, processGroup));
        if (failed(posix_spawnattr_setpgroup(&attributes, group)))
            return { };
        flags |= POSIX_SPAWN_SETPGROUP;
    }
    if (resetsIDs)
        flags |= POSIX_SPAWN_RESETIDS;
    if (setsSession) {
#if defined(POSIX_SPAWN_SETSID)
        flags |= POSIX_SPAWN_SETSID;
#elif defined(POSIX_SPAWN_SETSID_NP)
        flags |= POSIX_SPAWN_SETSID_NP;
#else
        return JSValue::encode(raiseArgumentUnavailable(globalObject, scope, function, "setsid"_s));
#endif
    }
    if (signalMask) {
        sigset_t set;
        if (!toSignalSet(globalObject, signalMask, set))
            return { };
        if (failed(posix_spawnattr_setsigmask(&attributes, &set)))
            return { };
        flags |= POSIX_SPAWN_SETSIGMASK;
    }
    if (defaultSignals) {
        sigset_t set;
        if (!toSignalSet(globalObject, defaultSignals, set))
            return { };
        if (failed(posix_spawnattr_setsigdefault(&attributes, &set)))
            return { };
        flags |= POSIX_SPAWN_SETSIGDEF;
    }
    if (scheduler && !isNone(scheduler))
        return JSValue::encode(raise(globalObject, scope, BuiltinType::NotImplementedError, "The scheduler option is not supported in this system."_s));
    if (failed(posix_spawnattr_setflags(&attributes, flags)))
        return { };

    if (!audit(globalObject, "os.posix_spawn"_s, path.object, given, environment))
        return { };
    pid_t process;
    int error = searchesPath ? ::posix_spawnp(&process, path.narrow(), hasActions ? &actions : nullptr, &attributes, arguments.pointers(), environmentPointers)
        : ::posix_spawn(&process, path.narrow(), hasActions ? &actions : nullptr, &attributes, arguments.pointers(), environmentPointers);
    if (error) {
        errno = error;
        return JSValue::encode(raisePathError(globalObject, scope, path));
    }
    return JSValue::encode(jsNumber(process));
}

// ---- Waiting

// wait_helper()
static JSValue waitResult(JSGlobalObject* globalObject, pid_t process, int status, struct rusage& usage)
{
    // If nothing was ready to be told of, nothing has been put in it.
    if (!process)
        zeroBytes(usage);
    auto seconds = [] (const struct timeval& time) { return floatFromDouble(multiplyAdd(static_cast<double>(time.tv_usec), 0.000001, static_cast<double>(time.tv_sec))); };
    MarkedArgumentBuffer values;
    values.append(seconds(usage.ru_utime));
    values.append(seconds(usage.ru_stime));
    for (long value : { usage.ru_maxrss, usage.ru_ixrss, usage.ru_idrss, usage.ru_isrss, usage.ru_minflt, usage.ru_majflt, usage.ru_nswap, usage.ru_inblock, usage.ru_oublock, usage.ru_msgsnd, usage.ru_msgrcv,
             usage.ru_nsignals, usage.ru_nvcsw, usage.ru_nivcsw })
        values.append(intFromInt64(globalObject, value));
    JSValue result = newStructSequence(globalObject, posixState(globalObject).resourceUsage.get(), values);
    return PyTuple::create(globalObject, { jsNumber(process), jsNumber(status), result });
}

enum class Wait : uint8_t { Wait, Waitpid, Wait3, Wait4 };

// wait(), waitpid(pid, options), wait3(options) and wait4(pid, options)
PYTHON_NATIVE(posixWait)
{
    NATIVE_PROLOGUE();
    auto which = unpack<Wait>(callFrame, 0);
    pid_t process = -1;
    int options = 0;
    unsigned next = 0;
    if (which == Wait::Waitpid || which == Wait::Wait4) {
        CONVERT(given, toCInt(globalObject, args.at(next++)));
        process = given;
    }
    if (which != Wait::Wait) {
        CONVERT(given, toCInt(globalObject, args.at(next++)));
        options = given;
    }
    int status = 0;
    struct rusage usage;
    pid_t result;
    bool isDone = retryIfInterrupted(globalObject, result, [&] () -> pid_t {
        switch (which) {
        case Wait::Wait:
            return ::wait(&status);
        case Wait::Waitpid:
            return ::waitpid(process, &status, options);
        case Wait::Wait3:
            return ::wait3(&status, options, &usage);
        case Wait::Wait4:
            return ::wait4(process, &status, options, &usage);
        }
        RELEASE_ASSERT_NOT_REACHED();
    });
    if (!isDone)
        return { };
    if (result < 0)
        return JSValue::encode(raisePosixError(globalObject, scope));
    if (which == Wait::Wait3 || which == Wait::Wait4)
        RELEASE_AND_RETURN(scope, JSValue::encode(waitResult(globalObject, result, status, usage)));
    RELEASE_AND_RETURN(scope, JSValue::encode(PyTuple::create(globalObject, { jsNumber(result), jsNumber(status) })));
}

PYTHON_NATIVE(posixWaitid)
{
    NATIVE_PROLOGUE();
    CONVERT(type, toCInt(globalObject, args.at(0)));
    CONVERT(id, toCInt(globalObject, args.at(1)));
    CONVERT(options, toCInt(globalObject, args.at(2)));
    siginfo_t information;
    information.si_pid = 0;
    int result;
    if (!retryIfInterrupted(globalObject, result, [&] { return ::waitid(static_cast<idtype_t>(type), static_cast<id_t>(id), &information, options); }))
        return { };
    if (result < 0)
        return JSValue::encode(raisePosixError(globalObject, scope));
    if (!information.si_pid)
        RETURN_NONE();
    MarkedArgumentBuffer values;
    values.append(jsNumber(information.si_pid));
    values.append(intFromUserID(globalObject, information.si_uid));
    values.append(jsNumber(information.si_signo));
    values.append(jsNumber(information.si_status));
    values.append(jsNumber(information.si_code));
    RELEASE_AND_RETURN(scope, JSValue::encode(newStructSequence(globalObject, posixState(globalObject).waitidResult.get(), values)));
}

enum class OfStatus : uint8_t { CoreDump, IfContinued, IfStopped, IfSignaled, IfExited, ExitStatus, TermSig, StopSig };

PYTHON_NATIVE(posixOfStatus)
{
    NATIVE_PROLOGUE();
    CONVERT(status, toCInt(globalObject, args.at(0)));
    switch (unpack<OfStatus>(callFrame, 0)) {
    case OfStatus::CoreDump:
        return JSValue::encode(jsBoolean(WCOREDUMP(status)));
    case OfStatus::IfContinued:
        return JSValue::encode(jsBoolean(WIFCONTINUED(status)));
    case OfStatus::IfStopped:
        return JSValue::encode(jsBoolean(WIFSTOPPED(status)));
    case OfStatus::IfSignaled:
        return JSValue::encode(jsBoolean(WIFSIGNALED(status)));
    case OfStatus::IfExited:
        return JSValue::encode(jsBoolean(WIFEXITED(status)));
    case OfStatus::ExitStatus:
        return JSValue::encode(jsNumber(WEXITSTATUS(status)));
    case OfStatus::TermSig:
        return JSValue::encode(jsNumber(WTERMSIG(status)));
    case OfStatus::StopSig:
        return JSValue::encode(jsNumber(WSTOPSIG(status)));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

PYTHON_NATIVE(posixWaitstatusToExitcode)
{
    NATIVE_PROLOGUE();
    CONVERT(status, toCInt(globalObject, args.at(0)));
    if (WIFEXITED(status))
        return JSValue::encode(jsNumber(WEXITSTATUS(status)));
    if (WIFSIGNALED(status)) {
        int signal = WTERMSIG(status);
        if (signal <= 0)
            return JSValue::encode(raiseValueError(globalObject, scope, concatenate("invalid WTERMSIG: "_s, signal)));
        return JSValue::encode(jsNumber(-signal));
    }
    if (WIFSTOPPED(status))
        return JSValue::encode(raiseValueError(globalObject, scope, concatenate("process stopped by delivery of signal "_s, WSTOPSIG(status))));
    return JSValue::encode(raiseValueError(globalObject, scope, concatenate("invalid wait status: "_s, status)));
}

void initializePosixProcessTypes(JSGlobalObject* globalObject, PosixModuleState& state)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    auto makeSequence = [&] (WriteBarrier<PyType>& slot, ASCIILiteral name, std::initializer_list<ASCIILiteral> fields) {
        PyType* type = createBuiltinType(globalObject, name, realm->typeTuple(), PyType::Layout::Tuple, PyType::IsSequence);
        slot.set(vm, realm, type);
        makeStructSequenceType(globalObject, type, std::span(fields.begin(), fields.size()), static_cast<unsigned>(fields.size()));
    };
    makeSequence(state.timesResult, "posix.times_result"_s, { "user"_s, "system"_s, "children_user"_s, "children_system"_s, "elapsed"_s });
    makeSequence(state.unameResult, "posix.uname_result"_s, { "sysname"_s, "nodename"_s, "release"_s, "version"_s, "machine"_s });
    makeSequence(state.waitidResult, "posix.waitid_result"_s, { "si_pid"_s, "si_uid"_s, "si_signo"_s, "si_status"_s, "si_code"_s });
    makeSequence(state.resourceUsage, "resource.struct_rusage"_s, { "ru_utime"_s, "ru_stime"_s, "ru_maxrss"_s, "ru_ixrss"_s, "ru_idrss"_s, "ru_isrss"_s, "ru_minflt"_s, "ru_majflt"_s, "ru_nswap"_s, "ru_inblock"_s,
        "ru_oublock"_s, "ru_msgsnd"_s, "ru_msgrcv"_s, "ru_nsignals"_s, "ru_nvcsw"_s, "ru_nivcsw"_s });
}

void addPosixProcessFunctions(JSGlobalObject* globalObject, JSObject* module)
{
    auto add = [&] (ASCIILiteral name, NativeFunction function, unsigned data = 0) { addFunction(globalObject, module, name, function, data); };
    add("execv"_s, posixExecv);
    add("execve"_s, posixExecve);
    add("posix_spawn"_s, posixSpawn, pack(false));
    add("posix_spawnp"_s, posixSpawn, pack(true));
    add("wait"_s, posixWait, pack(Wait::Wait));
    add("waitpid"_s, posixWait, pack(Wait::Waitpid));
    add("wait3"_s, posixWait, pack(Wait::Wait3));
    add("wait4"_s, posixWait, pack(Wait::Wait4));
    add("waitid"_s, posixWaitid);
    add("WCOREDUMP"_s, posixOfStatus, pack(OfStatus::CoreDump));
    add("WIFCONTINUED"_s, posixOfStatus, pack(OfStatus::IfContinued));
    add("WIFSTOPPED"_s, posixOfStatus, pack(OfStatus::IfStopped));
    add("WIFSIGNALED"_s, posixOfStatus, pack(OfStatus::IfSignaled));
    add("WIFEXITED"_s, posixOfStatus, pack(OfStatus::IfExited));
    add("WEXITSTATUS"_s, posixOfStatus, pack(OfStatus::ExitStatus));
    add("WTERMSIG"_s, posixOfStatus, pack(OfStatus::TermSig));
    add("WSTOPSIG"_s, posixOfStatus, pack(OfStatus::StopSig));
    add("waitstatus_to_exitcode"_s, posixWaitstatusToExitcode);
}

} } // namespace JSC::Python

#endif // OS(UNIX)
