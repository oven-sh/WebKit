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
#include <grp.h>
#include <sched.h>
#include <signal.h>
#include <string.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/times.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wtf/CryptographicallyRandomNumber.h>
#if OS(DARWIN)
#include <crt_externs.h>
#endif

// The functions of posix about this process and others: Modules/posixmodule.c of CPython.

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

static char** environmentOfProcess()
{
#if OS(DARWIN)
    return *_NSGetEnviron();
#else
    return environ;
#endif
}

// ---- The environment

// convertenviron()
JSValue newEnvironmentDict(JSGlobalObject* globalObject)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    PyDict* result = PyDict::create(globalObject);
    char** entries = environmentOfProcess();
    if (!entries)
        return result;
    for (; *entries; ++entries) {
        auto entry = unsafeSpan(*entries);
        size_t equals = WTF::find(entry, '=');
        if (equals == notFound)
            continue;
        JSValue key = newBytes(globalObject, byteCast<uint8_t>(entry.first(equals)));
        RETURN_IF_EXCEPTION(scope, { });
        JSValue value = newBytes(globalObject, byteCast<uint8_t>(entry.subspan(equals + 1)));
        RETURN_IF_EXCEPTION(scope, { });
        // The first of a name is the one that getenv() finds.
        bool has = result->contains(globalObject, key);
        RETURN_IF_EXCEPTION(scope, { });
        if (has)
            continue;
        result->set(globalObject, key, value);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return result;
}

PYTHON_NATIVE(posixCreateEnviron)
{
    UNUSED_PARAM(callFrame);
    return JSValue::encode(newEnvironmentDict(globalObject));
}

// What was given as bytes, for those who listen to what is done.
static JSValue bytesFrom(JSGlobalObject* globalObject, const CString& string)
{
    return newBytes(globalObject, byteCast<uint8_t>(string.span()));
}

PYTHON_NATIVE(posixPutenv)
{
    NATIVE_PROLOGUE();
    CONVERT(name, toFileSystemEncoded(globalObject, args.at(0)));
    CONVERT(value, toFileSystemEncoded(globalObject, args.at(1)));
    if (WTF::find(name.span(), '=') != notFound)
        return JSValue::encode(raiseValueError(globalObject, scope, "illegal environment variable name"_s));
    if (!audit(globalObject, "os.putenv"_s, bytesFrom(globalObject, name), bytesFrom(globalObject, value)))
        return { };
    if (::setenv(name.data(), value.data(), 1))
        return JSValue::encode(raisePosixError(globalObject, scope));
    RETURN_NONE();
}

PYTHON_NATIVE(posixUnsetenv)
{
    NATIVE_PROLOGUE();
    CONVERT(name, toFileSystemEncoded(globalObject, args.at(0)));
    if (!audit(globalObject, "os.unsetenv"_s, bytesFrom(globalObject, name)))
        return { };
    if (::unsetenv(name.data()))
        return JSValue::encode(raisePosixError(globalObject, scope));
    RETURN_NONE();
}

// ---- Who this is

enum class Identity : uint8_t { Getegid, Geteuid, Getgid, Getuid, Getpid, Getppid, Getpgrp };

PYTHON_NATIVE(posixIdentity)
{
    switch (unpack<Identity>(callFrame, 0)) {
    case Identity::Getegid:
        return JSValue::encode(intFromUserID(globalObject, ::getegid()));
    case Identity::Geteuid:
        return JSValue::encode(intFromUserID(globalObject, ::geteuid()));
    case Identity::Getgid:
        return JSValue::encode(intFromUserID(globalObject, ::getgid()));
    case Identity::Getuid:
        return JSValue::encode(intFromUserID(globalObject, ::getuid()));
    case Identity::Getpid:
        return JSValue::encode(jsNumber(::getpid()));
    case Identity::Getppid:
        return JSValue::encode(jsNumber(::getppid()));
    case Identity::Getpgrp:
        return JSValue::encode(jsNumber(::getpgrp()));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

enum class SetIdentity : uint8_t { Setuid, Seteuid, Setgid, Setegid };

PYTHON_NATIVE(posixSetIdentity)
{
    NATIVE_PROLOGUE();
    auto which = unpack<SetIdentity>(callFrame, 0);
    bool isUser = which == SetIdentity::Setuid || which == SetIdentity::Seteuid;
    CONVERT(id, isUser ? toUserID(globalObject, args.at(0)) : toGroupID(globalObject, args.at(0)));
    int result = which == SetIdentity::Setuid ? ::setuid(id) : which == SetIdentity::Seteuid ? ::seteuid(id) : which == SetIdentity::Setgid ? ::setgid(id) : ::setegid(id);
    if (result < 0)
        return JSValue::encode(raisePosixError(globalObject, scope));
    RETURN_NONE();
}

// setreuid(ruid, euid) and setregid(rgid, egid)
PYTHON_NATIVE(posixSetRealAndEffective)
{
    NATIVE_PROLOGUE();
    bool isUser = unpack<bool>(callFrame, 0);
    CONVERT(real, isUser ? toUserID(globalObject, args.at(0)) : toGroupID(globalObject, args.at(0)));
    CONVERT(effective, isUser ? toUserID(globalObject, args.at(1)) : toGroupID(globalObject, args.at(1)));
    if ((isUser ? ::setreuid(real, effective) : ::setregid(real, effective)) < 0)
        return JSValue::encode(raisePosixError(globalObject, scope));
    RETURN_NONE();
}

// getpgid(pid) and getsid(pid)
PYTHON_NATIVE(posixGroupOfProcess)
{
    NATIVE_PROLOGUE();
    CONVERT(process, toCInt(globalObject, args.at(0)));
    pid_t result = unpack<bool>(callFrame, 0) ? ::getpgid(process) : ::getsid(process);
    if (result < 0)
        return JSValue::encode(raisePosixError(globalObject, scope));
    return JSValue::encode(jsNumber(result));
}

// setpgrp() and setsid()
PYTHON_NATIVE(posixBecomeLeader)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(args);
    if ((unpack<bool>(callFrame, 0) ? ::setpgid(0, 0) : ::setsid()) < 0)
        return JSValue::encode(raisePosixError(globalObject, scope));
    RETURN_NONE();
}

PYTHON_NATIVE(posixSetpgid)
{
    NATIVE_PROLOGUE();
    CONVERT(process, toCInt(globalObject, args.at(0)));
    CONVERT(group, toCInt(globalObject, args.at(1)));
    if (::setpgid(process, group) < 0)
        return JSValue::encode(raisePosixError(globalObject, scope));
    RETURN_NONE();
}

// MAX_GROUPS
static constexpr int mostGroups = NGROUPS_MAX;

PYTHON_NATIVE(posixGetgrouplist)
{
    NATIVE_PROLOGUE();
    auto user = toTextArgument(globalObject, args.at(0), "getgrouplist"_s, "argument 1"_s, false);
    RETURN_IF_EXCEPTION(scope, { });
    CString name = user->utf8();
#if OS(DARWIN)
    using Group = int;
    CONVERT(base, toCInt(globalObject, args.at(1)));
#else
    using Group = gid_t;
    CONVERT(base, toGroupID(globalObject, args.at(1)));
#endif
    // What comes back has the group that was given in it as well.
    int count = 1 + mostGroups;
    Vector<Group> groups;
    while (true) {
        groups.grow(static_cast<size_t>(count));
        int before = count;
        if (::getgrouplist(name.data(), base, groups.mutableSpan().data(), &count) != -1)
            break;
        // There was not room. Some say how much room it takes, and some do not.
        if (count <= before) {
            if (count > std::numeric_limits<int>::max() / 2)
                return JSValue::encode(raiseMemoryError(globalObject, scope));
            count *= 2;
        }
    }
    JSArray* list = newList(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    for (int i = 0; i < count; ++i) {
#if OS(DARWIN)
        listAppend(globalObject, list, intFromUInt64(globalObject, static_cast<unsigned long>(groups[i])));
#else
        listAppend(globalObject, list, intFromUserID(globalObject, groups[i]));
#endif
        RETURN_IF_EXCEPTION(scope, { });
    }
    return JSValue::encode(list);
}

PYTHON_NATIVE(posixGetgroups)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(args);
    int count = ::getgroups(0, nullptr);
    if (count < 0)
        return JSValue::encode(raisePosixError(globalObject, scope));
    Vector<gid_t> groups(static_cast<size_t>(count));
    if (count) {
        count = ::getgroups(count, groups.mutableSpan().data());
        if (count == -1)
            return JSValue::encode(raisePosixError(globalObject, scope));
    }
    JSArray* list = newList(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    for (int i = 0; i < count; ++i) {
        listAppend(globalObject, list, intFromUserID(globalObject, groups[i]));
        RETURN_IF_EXCEPTION(scope, { });
    }
    return JSValue::encode(list);
}

PYTHON_NATIVE(posixSetgroups)
{
    NATIVE_PROLOGUE();
    JSValue given = args.at(0);
    if (!isSequence(globalObject, given))
        return JSValue::encode(raiseTypeError(globalObject, scope, "setgroups argument must be a sequence"_s));
    int64_t count = length(globalObject, given);
    RETURN_IF_EXCEPTION(scope, { });
    if (count > mostGroups)
        return JSValue::encode(raiseValueError(globalObject, scope, "too many groups"_s));
    Vector<gid_t> groups;
    for (int64_t i = 0; i < count; ++i) {
        JSValue item = getItem(globalObject, given, intFromInt64(globalObject, i));
        RETURN_IF_EXCEPTION(scope, { });
        if (!classify(item).isInt() && !typeOf(globalObject, item)->lookup(vm, names.dunder_index))
            return JSValue::encode(raiseTypeError(globalObject, scope, "groups must be integers"_s));
        CONVERT(group, toGroupID(globalObject, item));
        groups.append(group);
    }
    if (::setgroups(static_cast<int>(groups.size()), groups.span().data()) < 0)
        return JSValue::encode(raisePosixError(globalObject, scope));
    RETURN_NONE();
}

PYTHON_NATIVE(posixInitgroups)
{
    NATIVE_PROLOGUE();
    CONVERT(name, toFileSystemEncoded(globalObject, args.at(0)));
#if OS(DARWIN)
    CONVERT(group, toCInt(globalObject, args.at(1)));
#else
    CONVERT(group, toGroupID(globalObject, args.at(1)));
#endif
    if (::initgroups(name.data(), group) == -1)
        return JSValue::encode(raisePosixError(globalObject, scope));
    RETURN_NONE();
}

PYTHON_NATIVE(posixGetlogin)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(args);
    errno = 0;
    char* name = ::getlogin();
    if (!name) {
        if (errno)
            return JSValue::encode(raisePosixError(globalObject, scope));
        return JSValue::encode(raise(globalObject, scope, BuiltinType::OSError, "unable to determine login name"_s));
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(decodeFileSystemBytes(globalObject, unsafeSpan(name))));
}

// ---- The system

PYTHON_NATIVE(posixUname)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(args);
    struct utsname name;
    if (::uname(&name) < 0)
        return JSValue::encode(raisePosixError(globalObject, scope));
    MarkedArgumentBuffer values;
    for (const char* field : { name.sysname, name.nodename, name.release, name.version, name.machine }) {
        values.append(decodeFileSystemBytes(globalObject, unsafeSpan(field)));
        RETURN_IF_EXCEPTION(scope, { });
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(newStructSequence(globalObject, posixState(globalObject).unameResult.get(), values)));
}

PYTHON_NATIVE(posixTimes)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(args);
    struct tms process;
    errno = 0;
    clock_t elapsed = ::times(&process);
    if (elapsed == static_cast<clock_t>(-1))
        return JSValue::encode(raisePosixError(globalObject, scope));
    double ticksPerSecond = static_cast<double>(::sysconf(_SC_CLK_TCK));
    MarkedArgumentBuffer values;
    for (clock_t ticks : { process.tms_utime, process.tms_stime, process.tms_cutime, process.tms_cstime, elapsed })
        values.append(floatFromDouble(static_cast<double>(ticks) / ticksPerSecond));
    RELEASE_AND_RETURN(scope, JSValue::encode(newStructSequence(globalObject, posixState(globalObject).timesResult.get(), values)));
}

PYTHON_NATIVE(posixGetloadavg)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(args);
    double load[3];
    if (::getloadavg(load, 3) != 3)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::OSError, "Load averages are unobtainable"_s));
    RELEASE_AND_RETURN(scope, JSValue::encode(PyTuple::create(globalObject, { floatFromDouble(load[0]), floatFromDouble(load[1]), floatFromDouble(load[2]) })));
}

PYTHON_NATIVE(posixCpuCount)
{
    UNUSED_PARAM(callFrame);
    if (int said = globalObject->pyRealm()->configuration().cpuCount; said > 0)
        return JSValue::encode(jsNumber(said));
    long count = ::sysconf(_SC_NPROCESSORS_ONLN);
    if (count < 1)
        RETURN_NONE();
    return JSValue::encode(jsNumber(static_cast<int32_t>(count)));
}

PYTHON_NATIVE(posixStrerror)
{
    NATIVE_PROLOGUE();
    CONVERT(code, toCInt(globalObject, args.at(0)));
    RELEASE_AND_RETURN(scope, JSValue::encode(decodeFileSystemBytes(globalObject, unsafeSpan(::strerror(code)))));
}

PYTHON_NATIVE(posixUrandom)
{
    NATIVE_PROLOGUE();
    CONVERT(size, toSsize(globalObject, args.at(0)));
    if (size < 0)
        return JSValue::encode(raiseValueError(globalObject, scope, "negative argument not allowed"_s));
    ByteVector bytes;
    bytes.appendFill(0, static_cast<size_t>(size));
    if (bytes.hasOverflowed())
        return JSValue::encode(raiseMemoryError(globalObject, scope));
    cryptographicallyRandomValues(bytes.mutableSpan());
    RELEASE_AND_RETURN(scope, JSValue::encode(newBytes(globalObject, bytes.span())));
}

PYTHON_NATIVE(posixUmask)
{
    NATIVE_PROLOGUE();
    CONVERT(mask, toCInt(globalObject, args.at(0)));
    return JSValue::encode(jsNumber(static_cast<int32_t>(::umask(static_cast<mode_t>(mask)))));
}

// ---- Priorities

PYTHON_NATIVE(posixNice)
{
    NATIVE_PROLOGUE();
    CONVERT(increment, toCInt(globalObject, args.at(0)));
    // -1 may be what it has become.
    errno = 0;
    int value = ::nice(increment);
    if (value == -1 && errno)
        return JSValue::encode(raisePosixError(globalObject, scope));
    return JSValue::encode(jsNumber(value));
}

PYTHON_NATIVE(posixGetpriority)
{
    NATIVE_PROLOGUE();
    CONVERT(which, toCInt(globalObject, args.at(0)));
    CONVERT(who, toCInt(globalObject, args.at(1)));
    errno = 0;
    int value = ::getpriority(which, static_cast<id_t>(who));
    if (errno)
        return JSValue::encode(raisePosixError(globalObject, scope));
    return JSValue::encode(jsNumber(value));
}

PYTHON_NATIVE(posixSetpriority)
{
    NATIVE_PROLOGUE();
    CONVERT(which, toCInt(globalObject, args.at(0)));
    CONVERT(who, toCInt(globalObject, args.at(1)));
    CONVERT(priority, toCInt(globalObject, args.at(2)));
    if (::setpriority(which, static_cast<id_t>(who), priority) == -1)
        return JSValue::encode(raisePosixError(globalObject, scope));
    RETURN_NONE();
}

// sched_get_priority_max(policy) and sched_get_priority_min(policy)
PYTHON_NATIVE(posixSchedulerPriority)
{
    NATIVE_PROLOGUE();
    CONVERT(policy, toCInt(globalObject, args.at(0)));
    errno = 0;
    int value = unpack<bool>(callFrame, 0) ? ::sched_get_priority_max(policy) : ::sched_get_priority_min(policy);
    if (value == -1 && errno)
        return JSValue::encode(raisePosixError(globalObject, scope));
    return JSValue::encode(jsNumber(value));
}

PYTHON_NATIVE(posixSchedYield)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(args);
    if (::sched_yield() < 0)
        return JSValue::encode(raisePosixError(globalObject, scope));
    RETURN_NONE();
}

// ---- Signals, and the end

PYTHON_NATIVE(posixKill)
{
    NATIVE_PROLOGUE();
    CONVERT(process, toCInt(globalObject, args.at(0)));
    CONVERT(signal, toSsize(globalObject, args.at(1)));
    if (!audit(globalObject, "os.kill"_s, jsNumber(process), intFromInt64(globalObject, signal)))
        return { };
    if (::kill(process, static_cast<int>(signal)) == -1)
        return JSValue::encode(raisePosixError(globalObject, scope));
    // It may have been sent to this process.
    if (!checkSignals(globalObject))
        return { };
    RETURN_NONE();
}

PYTHON_NATIVE(posixKillpg)
{
    NATIVE_PROLOGUE();
    CONVERT(group, toCInt(globalObject, args.at(0)));
    CONVERT(signal, toCInt(globalObject, args.at(1)));
    if (!audit(globalObject, "os.killpg"_s, jsNumber(group), jsNumber(signal)))
        return { };
    if (::killpg(group, signal) == -1)
        return JSValue::encode(raisePosixError(globalObject, scope));
    RETURN_NONE();
}

PYTHON_NATIVE(posixAbort)
{
    UNUSED_PARAM(globalObject);
    UNUSED_PARAM(callFrame);
    ::abort();
}

PYTHON_NATIVE(posixExit)
{
    NATIVE_PROLOGUE();
    CONVERT(status, toCInt(globalObject, args.at(0)));
    ::_exit(status);
}

PYTHON_NATIVE(posixSystem)
{
    NATIVE_PROLOGUE();
    CONVERT(command, toFileSystemEncoded(globalObject, args.at(0)));
    if (!audit(globalObject, "os.system"_s, bytesFrom(globalObject, command)))
        return { };
    return JSValue::encode(intFromInt64(globalObject, ::system(command.data())));
}

// _inputhook() and _is_inputhook_installed(): there is none, as there is none in CPython unless something has put one there.
PYTHON_NATIVE(posixInputHook)
{
    UNUSED_PARAM(globalObject);
    if (unpack<bool>(callFrame, 0))
        return JSValue::encode(jsBoolean(false));
    return JSValue::encode(jsNumber(0));
}

void addPosixIdentityFunctions(JSGlobalObject* globalObject, JSObject* module)
{
    auto add = [&] (ASCIILiteral name, NativeFunction function, unsigned data = 0) { addFunction(globalObject, module, name, function, data); };
    add("_create_environ"_s, posixCreateEnviron);
    add("putenv"_s, posixPutenv);
    add("unsetenv"_s, posixUnsetenv);
    add("getegid"_s, posixIdentity, pack(Identity::Getegid));
    add("geteuid"_s, posixIdentity, pack(Identity::Geteuid));
    add("getgid"_s, posixIdentity, pack(Identity::Getgid));
    add("getuid"_s, posixIdentity, pack(Identity::Getuid));
    add("getpid"_s, posixIdentity, pack(Identity::Getpid));
    add("getppid"_s, posixIdentity, pack(Identity::Getppid));
    add("getpgrp"_s, posixIdentity, pack(Identity::Getpgrp));
    add("setuid"_s, posixSetIdentity, pack(SetIdentity::Setuid));
    add("seteuid"_s, posixSetIdentity, pack(SetIdentity::Seteuid));
    add("setgid"_s, posixSetIdentity, pack(SetIdentity::Setgid));
    add("setegid"_s, posixSetIdentity, pack(SetIdentity::Setegid));
    add("setreuid"_s, posixSetRealAndEffective, pack(true));
    add("setregid"_s, posixSetRealAndEffective, pack(false));
    add("getpgid"_s, posixGroupOfProcess, pack(true));
    add("getsid"_s, posixGroupOfProcess, pack(false));
    add("setpgrp"_s, posixBecomeLeader, pack(true));
    add("setsid"_s, posixBecomeLeader, pack(false));
    add("setpgid"_s, posixSetpgid);
    add("getgrouplist"_s, posixGetgrouplist);
    add("getgroups"_s, posixGetgroups);
    add("setgroups"_s, posixSetgroups);
    add("initgroups"_s, posixInitgroups);
    add("getlogin"_s, posixGetlogin);
    add("uname"_s, posixUname);
    add("times"_s, posixTimes);
    add("getloadavg"_s, posixGetloadavg);
    add("cpu_count"_s, posixCpuCount);
    add("strerror"_s, posixStrerror);
    add("urandom"_s, posixUrandom);
    add("umask"_s, posixUmask);
    add("nice"_s, posixNice);
    add("getpriority"_s, posixGetpriority);
    add("setpriority"_s, posixSetpriority);
    add("sched_get_priority_max"_s, posixSchedulerPriority, pack(true));
    add("sched_get_priority_min"_s, posixSchedulerPriority, pack(false));
    add("sched_yield"_s, posixSchedYield);
    add("kill"_s, posixKill);
    add("killpg"_s, posixKillpg);
    add("abort"_s, posixAbort);
    add("_exit"_s, posixExit);
    add("system"_s, posixSystem);
    add("_inputhook"_s, posixInputHook, pack(false));
    add("_is_inputhook_installed"_s, posixInputHook, pack(true));
}

} } // namespace JSC::Python

#endif // OS(UNIX)
