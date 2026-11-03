/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyPosix.h"

#if !OS(WINDOWS)

#include "JSCInlines.h"
#include "PySet.h"
#include <dlfcn.h>
#include <grp.h>
#include <limits.h>
#include <sched.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/wait.h>

#if OS(LINUX)
#include <sys/syscall.h>
#endif

#if OS(DARWIN)
#include <crt_externs.h>
#else
extern char** environ;
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// The functions of the module posix that are about processes: who they run as, how they are scheduled, how they start and
// end. There is no fork(): a copy of this process would have one thread, and a heap that the others were in the middle of
// using. And the module resource, whose struct_rusage wait3() and wait4() return.

// ---- Who the process is ----

static JSValue posixGetpid(JSGlobalObject*) { return jsNumber(getpid()); }
static JSValue posixGetppid(JSGlobalObject*) { return jsNumber(getppid()); }
static JSValue posixGetuid(JSGlobalObject* globalObject) { return posixFromUid(globalObject, getuid()); }
static JSValue posixGeteuid(JSGlobalObject* globalObject) { return posixFromUid(globalObject, geteuid()); }
static JSValue posixGetgid(JSGlobalObject* globalObject) { return posixFromGid(globalObject, getgid()); }
static JSValue posixGetegid(JSGlobalObject* globalObject) { return posixFromGid(globalObject, getegid()); }
static JSValue posixGetpgrp(JSGlobalObject*) { return jsNumber(getpgrp()); }

static JSValue posixNoneUnlessFailed(JSGlobalObject* globalObject, int result)
{
    if (result < 0)
        return posixError(globalObject);
    return jsUndefined();
}

static JSValue posixSetuid(JSGlobalObject* globalObject, JSValue value)
{
    uid_t uid;
    if (!posixToUid(globalObject, value, uid))
        return { };
    return posixNoneUnlessFailed(globalObject, setuid(uid));
}

static JSValue posixSeteuid(JSGlobalObject* globalObject, JSValue value)
{
    uid_t uid;
    if (!posixToUid(globalObject, value, uid))
        return { };
    return posixNoneUnlessFailed(globalObject, seteuid(uid));
}

static JSValue posixSetgid(JSGlobalObject* globalObject, JSValue value)
{
    gid_t gid;
    if (!posixToGid(globalObject, value, gid))
        return { };
    return posixNoneUnlessFailed(globalObject, setgid(gid));
}

static JSValue posixSetegid(JSGlobalObject* globalObject, JSValue value)
{
    gid_t gid;
    if (!posixToGid(globalObject, value, gid))
        return { };
    return posixNoneUnlessFailed(globalObject, setegid(gid));
}

static JSValue posixSetreuid(JSGlobalObject* globalObject, JSValue realValue, JSValue effectiveValue)
{
    uid_t real;
    uid_t effective;
    if (!posixToUid(globalObject, realValue, real) || !posixToUid(globalObject, effectiveValue, effective))
        return { };
    return posixNoneUnlessFailed(globalObject, setreuid(real, effective));
}

static JSValue posixSetregid(JSGlobalObject* globalObject, JSValue realValue, JSValue effectiveValue)
{
    gid_t real;
    gid_t effective;
    if (!posixToGid(globalObject, realValue, real) || !posixToGid(globalObject, effectiveValue, effective))
        return { };
    return posixNoneUnlessFailed(globalObject, setregid(real, effective));
}

#if OS(LINUX)
static JSValue posixSetresuid(JSGlobalObject* globalObject, JSValue realValue, JSValue effectiveValue, JSValue savedValue)
{
    uid_t real;
    uid_t effective;
    uid_t saved;
    if (!posixToUid(globalObject, realValue, real) || !posixToUid(globalObject, effectiveValue, effective) || !posixToUid(globalObject, savedValue, saved))
        return { };
    return posixNoneUnlessFailed(globalObject, setresuid(real, effective, saved));
}

static JSValue posixSetresgid(JSGlobalObject* globalObject, JSValue realValue, JSValue effectiveValue, JSValue savedValue)
{
    gid_t real;
    gid_t effective;
    gid_t saved;
    if (!posixToGid(globalObject, realValue, real) || !posixToGid(globalObject, effectiveValue, effective) || !posixToGid(globalObject, savedValue, saved))
        return { };
    return posixNoneUnlessFailed(globalObject, setresgid(real, effective, saved));
}

static JSValue posixGetresuid(JSGlobalObject* globalObject)
{
    uid_t real;
    uid_t effective;
    uid_t saved;
    if (getresuid(&real, &effective, &saved) < 0)
        return posixError(globalObject);
    return PyTuple::create(globalObject, { posixFromUid(globalObject, real), posixFromUid(globalObject, effective), posixFromUid(globalObject, saved) });
}

static JSValue posixGetresgid(JSGlobalObject* globalObject)
{
    gid_t real;
    gid_t effective;
    gid_t saved;
    if (getresgid(&real, &effective, &saved) < 0)
        return posixError(globalObject);
    return PyTuple::create(globalObject, { posixFromGid(globalObject, real), posixFromGid(globalObject, effective), posixFromGid(globalObject, saved) });
}
#endif

static JSValue posixGetlogin(JSGlobalObject* globalObject)
{
    errno = 0;
    const char* name = getlogin();
    if (!name) {
        if (errno)
            return posixError(globalObject);
        VM& vm = getVM(globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        return raise(globalObject, scope, runtime(globalObject).OSError(), "unable to determine login name"_s);
    }
    return posixDecode(globalObject, name);
}

// ---- Groups ----

static JSValue posixGetgroups(JSGlobalObject* globalObject)
{
    int count = getgroups(0, nullptr);
    if (count < 0)
        return posixError(globalObject);
    Vector<gid_t, 32> groups(static_cast<size_t>(count));
    if (count) {
        count = getgroups(count, groups.mutableSpan().data());
        if (count < 0)
            return posixError(globalObject);
    }
    JSArray* result = newList(globalObject);
    for (int i = 0; i < count; ++i)
        listAppend(globalObject, result, posixFromGid(globalObject, groups[i]));
    return result;
}

static JSValue posixGetgrouplist(JSGlobalObject* globalObject, JSValue userValue, JSValue groupValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isStr(userValue))
        return raiseTypeError(globalObject, scope, "getgrouplist() argument 1 must be str, not "_s, posixTypeNameOfArgument(globalObject, userValue));
    JSValue encoded = strEncode(globalObject, strValue(userValue), nullptr, nullptr);
    RETURN_IF_EXCEPTION(scope, { });
    PosixBytes user;
    user.assign(byteCast<char>(PyBytes::from(encoded)->span()));
    if (user.contains('\0'))
        return raiseValueError(globalObject, scope, "embedded null character"_s);
#if OS(DARWIN)
    using Group = int;
    int base;
    if (!posixToInt(globalObject, groupValue, base))
        return { };
#else
    using Group = gid_t;
    gid_t base;
    if (!posixToGid(globalObject, groupValue, base))
        return { };
#endif
    Vector<Group, 64> groups;
    int count = 1 + NGROUPS_MAX;
    while (true) {
        groups.grow(static_cast<size_t>(count));
        int capacity = count;
        if (getgrouplist(user.characters(), base, groups.mutableSpan().data(), &count) != -1)
            break;
        // Some systems say how many there are, the others leave it to be guessed.
        if (count <= capacity) {
            if (count > INT_MAX / 2)
                return raise(globalObject, scope, runtime(globalObject).MemoryError(), String());
            count = capacity * 2;
        }
    }
    JSArray* result = newList(globalObject);
    for (int i = 0; i < count; ++i) {
#if OS(DARWIN)
        listAppend(globalObject, result, makeInt(globalObject, static_cast<uint64_t>(static_cast<unsigned long>(groups[i]))));
#else
        listAppend(globalObject, result, posixFromGid(globalObject, groups[i]));
#endif
    }
    return result;
}

static JSValue posixSetgroups(JSGlobalObject* globalObject, JSValue groupsValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!posixIsSequence(globalObject, groupsValue))
        return raiseTypeError(globalObject, scope, "setgroups argument must be a sequence"_s);
    int64_t count = length(globalObject, groupsValue);
    RETURN_IF_EXCEPTION(scope, { });
    if (count > NGROUPS_MAX)
        return raiseValueError(globalObject, scope, "too many groups"_s);
    Vector<gid_t, 32> groups(static_cast<size_t>(count));
    for (int64_t i = 0; i < count; ++i) {
        JSValue item = getItem(globalObject, groupsValue, makeInt(globalObject, i));
        RETURN_IF_EXCEPTION(scope, { });
        if (!hasIndex(globalObject, item))
            return raiseTypeError(globalObject, scope, "groups must be integers"_s);
        if (!posixToGid(globalObject, item, groups[static_cast<size_t>(i)]))
            return { };
    }
    return posixNoneUnlessFailed(globalObject, setgroups(static_cast<decltype(NGROUPS_MAX)>(count), groups.span().data()));
}

static JSValue posixInitgroups(JSGlobalObject* globalObject, JSValue userValue, JSValue groupValue)
{
    PosixBytes user;
    if (!posixToFSBytes(globalObject, userValue, user))
        return { };
#if OS(DARWIN)
    int group;
    if (!posixToInt(globalObject, groupValue, group))
        return { };
#else
    gid_t group;
    if (!posixToGid(globalObject, groupValue, group))
        return { };
#endif
    return posixNoneUnlessFailed(globalObject, initgroups(user.characters(), group));
}

// ---- Process groups and sessions ----

static JSValue posixGetpgid(JSGlobalObject* globalObject, JSValue pidValue)
{
    pid_t pid;
    if (!posixToPid(globalObject, pidValue, pid))
        return { };
    pid_t group = getpgid(pid);
    if (group < 0)
        return posixError(globalObject);
    return jsNumber(group);
}

static JSValue posixSetpgid(JSGlobalObject* globalObject, JSValue pidValue, JSValue groupValue)
{
    pid_t pid;
    pid_t group;
    if (!posixToPid(globalObject, pidValue, pid) || !posixToPid(globalObject, groupValue, group))
        return { };
    return posixNoneUnlessFailed(globalObject, setpgid(pid, group));
}

static JSValue posixSetpgrp(JSGlobalObject* globalObject)
{
    return posixNoneUnlessFailed(globalObject, setpgid(0, 0));
}

static JSValue posixGetsid(JSGlobalObject* globalObject, JSValue pidValue)
{
    pid_t pid;
    if (!posixToPid(globalObject, pidValue, pid))
        return { };
    pid_t session = getsid(pid);
    if (session < 0)
        return posixError(globalObject);
    return jsNumber(session);
}

static JSValue posixSetsid(JSGlobalObject* globalObject)
{
    return posixNoneUnlessFailed(globalObject, setsid());
}

// ---- Priorities and scheduling ----

static JSValue posixNice(JSGlobalObject* globalObject, JSValue incrementValue)
{
    int increment;
    if (!posixToInt(globalObject, incrementValue, increment))
        return { };
    // -1 is a niceness like any other.
    errno = 0;
    int value = nice(increment);
    if (value == -1 && errno)
        return posixError(globalObject);
    return jsNumber(value);
}

static JSValue posixGetpriority(JSGlobalObject* globalObject, JSValue whichValue, JSValue whoValue)
{
    int which;
    int who;
    if (!posixToInt(globalObject, whichValue, which) || !posixToInt(globalObject, whoValue, who))
        return { };
    errno = 0;
    int value = getpriority(static_cast<decltype(PRIO_PROCESS)>(which), static_cast<id_t>(who));
    if (errno)
        return posixError(globalObject);
    return jsNumber(value);
}

static JSValue posixSetpriority(JSGlobalObject* globalObject, JSValue whichValue, JSValue whoValue, JSValue priorityValue)
{
    int which;
    int who;
    int priority;
    if (!posixToInt(globalObject, whichValue, which) || !posixToInt(globalObject, whoValue, who) || !posixToInt(globalObject, priorityValue, priority))
        return { };
    return posixNoneUnlessFailed(globalObject, setpriority(static_cast<decltype(PRIO_PROCESS)>(which), static_cast<id_t>(who), priority));
}

static JSValue posixSchedGetPriorityMax(JSGlobalObject* globalObject, JSValue policyValue)
{
    int policy;
    if (!posixToInt(globalObject, policyValue, policy))
        return { };
    errno = 0;
    int value = sched_get_priority_max(policy);
    if (value == -1 && errno)
        return posixError(globalObject);
    return jsNumber(value);
}

static JSValue posixSchedGetPriorityMin(JSGlobalObject* globalObject, JSValue policyValue)
{
    int policy;
    if (!posixToInt(globalObject, policyValue, policy))
        return { };
    errno = 0;
    int value = sched_get_priority_min(policy);
    if (value == -1 && errno)
        return posixError(globalObject);
    return jsNumber(value);
}

static JSValue posixSchedYield(JSGlobalObject* globalObject)
{
    return posixNoneUnlessFailed(globalObject, sched_yield());
}

#if OS(LINUX)
static constexpr ASCIILiteral posixSchedParamFields[] = { "sched_priority"_s };
static constexpr StructSeqDef posixSchedParamDef { "posix.sched_param"_s, posixSchedParamFields, 1 };

static JSValue posixSchedParamNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args)
{
    static constexpr ASCIILiteral keywords[] = { "sched_priority"_s };
    JSValue priority;
    if (!unpackKeywordArguments(globalObject, "sched_param"_s, keywords, 1, 1, false, args, &priority))
        return { };
    return makeStructSeq(globalObject, PyType::from(cls), { priority });
}

static JSValue posixSchedParamReduce(JSGlobalObject* globalObject, JSValue self)
{
    PyTuple* tuple = PyTuple::from(self);
    return PyTuple::create(globalObject, { tuple->type(), PyTuple::create(globalObject, { tuple->at(0) }) });
}

static constexpr NativeDef posixSchedParamMethods[] = {
    PY_NEW(posixSchedParamNew, "($type, /, sched_priority, *args)"),
    PY_METHOD("__reduce__", posixSchedParamReduce, "($self, /)"),
};

static PyType* posixSchedParamType(JSGlobalObject* globalObject)
{
    VM& vm = getVM(globalObject);
    if (JSValue type = posixState(globalObject)->get(internString(vm, posixSchedParamDef.name)))
        return PyType::from(type);
    PyType* type = cachedStructSeqType(globalObject, posixSchedParamDef);
    type->addMethods(globalObject, posixSchedParamMethods);
    type->didChange(vm);
    return type;
}

static bool posixToSchedParam(JSGlobalObject* globalObject, JSValue value, struct sched_param& result)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* tuple = PyTuple::tryFrom(value);
    if (!tuple || tuple->type() != posixSchedParamType(globalObject)) {
        raiseTypeError(globalObject, scope, "must have a sched_param object"_s);
        return false;
    }
    int64_t priority;
    if (!posixToLong(globalObject, tuple->at(0), priority))
        return false;
    if (priority > INT_MAX || priority < INT_MIN) {
        raiseOverflowError(globalObject, scope, "sched_priority out of range"_s);
        return false;
    }
    result.sched_priority = static_cast<int>(priority);
    return true;
}

static JSValue posixSchedGetscheduler(JSGlobalObject* globalObject, JSValue pidValue)
{
    pid_t pid;
    if (!posixToPid(globalObject, pidValue, pid))
        return { };
    int policy = sched_getscheduler(pid);
    if (policy < 0)
        return posixError(globalObject);
    return jsNumber(policy);
}

static JSValue posixSchedSetscheduler(JSGlobalObject* globalObject, JSValue pidValue, JSValue policyValue, JSValue paramValue)
{
    pid_t pid;
    int policy;
    struct sched_param param;
    if (!posixToPid(globalObject, pidValue, pid) || !posixToInt(globalObject, policyValue, policy) || !posixToSchedParam(globalObject, paramValue, param))
        return { };
    if (sched_setscheduler(pid, policy, &param) == -1)
        return posixError(globalObject);
    return jsUndefined();
}

static JSValue posixSchedGetparam(JSGlobalObject* globalObject, JSValue pidValue)
{
    pid_t pid;
    if (!posixToPid(globalObject, pidValue, pid))
        return { };
    struct sched_param param;
    if (sched_getparam(pid, &param))
        return posixError(globalObject);
    return makeStructSeq(globalObject, posixSchedParamType(globalObject), { jsNumber(param.sched_priority) });
}

static JSValue posixSchedSetparam(JSGlobalObject* globalObject, JSValue pidValue, JSValue paramValue)
{
    pid_t pid;
    struct sched_param param;
    if (!posixToPid(globalObject, pidValue, pid) || !posixToSchedParam(globalObject, paramValue, param))
        return { };
    if (sched_setparam(pid, &param))
        return posixError(globalObject);
    return jsUndefined();
}

static JSValue posixSchedRrGetInterval(JSGlobalObject* globalObject, JSValue pidValue)
{
    pid_t pid;
    if (!posixToPid(globalObject, pidValue, pid))
        return { };
    struct timespec interval;
    if (sched_rr_get_interval(pid, &interval))
        return posixError(globalObject);
    return makeFloat(globalObject, static_cast<double>(interval.tv_sec) + 1e-9 * static_cast<double>(interval.tv_nsec));
}

// A set of processors, of whatever size it takes.
class PosixCpuSet {
    WTF_MAKE_NONCOPYABLE(PosixCpuSet);
public:
    explicit PosixCpuSet(int count) { allocate(count); }
    ~PosixCpuSet() { CPU_FREE(m_set); }

    int count() const { return m_count; }
    size_t size() const { return CPU_ALLOC_SIZE(m_count); }
    cpu_set_t* set() { return m_set; }

    void growTo(int count)
    {
        cpu_set_t* old = m_set;
        size_t oldSize = size();
        allocate(count);
        memcpy(m_set, old, oldSize);
        CPU_FREE(old);
    }

private:
    void allocate(int count)
    {
        m_count = count;
        m_set = CPU_ALLOC(count);
        RELEASE_ASSERT(m_set);
        CPU_ZERO_S(size(), m_set);
    }

    cpu_set_t* m_set;
    int m_count;
};

static constexpr int posixInitialCpuCount = sizeof(unsigned long) * CHAR_BIT;

static JSValue posixSchedSetaffinity(JSGlobalObject* globalObject, JSValue pidValue, JSValue mask)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    pid_t pid;
    if (!posixToPid(globalObject, pidValue, pid))
        return { };
    PosixCpuSet cpus(posixInitialCpuCount);
    forEach(globalObject, mask, [&](JSValue item) {
        if (!hasIndex(globalObject, item)) {
            raiseTypeError(globalObject, scope, "expected an iterator of ints, but iterator yielded "_s, reprForMessage(globalObject, typeOf(globalObject, item)));
            return false;
        }
        int64_t cpu;
        if (!posixToLong(globalObject, item, cpu))
            return false;
        if (cpu < 0) {
            raiseValueError(globalObject, scope, "negative CPU number"_s);
            return false;
        }
        if (cpu > INT_MAX - 1) {
            raiseOverflowError(globalObject, scope, "CPU number too large"_s);
            return false;
        }
        if (cpu >= cpus.count()) {
            int count = cpus.count();
            while (count <= cpu)
                count = count > INT_MAX / 2 ? static_cast<int>(cpu) + 1 : count * 2;
            cpus.growTo(count);
        }
        CPU_SET_S(static_cast<size_t>(cpu), cpus.size(), cpus.set());
        return true;
    });
    RETURN_IF_EXCEPTION(scope, { });
    if (sched_setaffinity(pid, cpus.size(), cpus.set()))
        return posixError(globalObject);
    return jsUndefined();
}

static JSValue posixSchedGetaffinity(JSGlobalObject* globalObject, JSValue pidValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    pid_t pid;
    if (!posixToPid(globalObject, pidValue, pid))
        return { };
    PosixCpuSet cpus(posixInitialCpuCount);
    while (sched_getaffinity(pid, cpus.size(), cpus.set())) {
        // EINVAL: the kernel knows of more processors than the set has room for.
        if (errno != EINVAL)
            return posixError(globalObject);
        if (cpus.count() > INT_MAX / 2)
            return raiseOverflowError(globalObject, scope, "could not allocate a large enough CPU set"_s);
        cpus.growTo(cpus.count() * 2);
    }
    PySet* result = PySet::create(globalObject);
    int remaining = CPU_COUNT_S(cpus.size(), cpus.set());
    for (int cpu = 0; remaining; ++cpu) {
        if (!CPU_ISSET_S(static_cast<size_t>(cpu), cpus.size(), cpus.set()))
            continue;
        --remaining;
        result->add(globalObject, jsNumber(cpu));
        RETURN_IF_EXCEPTION(scope, { });
    }
    return result;
}

static JSValue posixPidfdOpen(JSGlobalObject* globalObject, JSValue pidValue, JSValue flagsValue)
{
    pid_t pid;
    unsigned flags = 0;
    if (!posixToPid(globalObject, pidValue, pid) || (flagsValue && !posixToUnsigned(globalObject, flagsValue, flags)))
        return { };
    int fd = static_cast<int>(syscall(SYS_pidfd_open, pid, flags));
    if (fd < 0)
        return posixError(globalObject);
    return jsNumber(fd);
}

static JSValue posixSetns(JSGlobalObject* globalObject, JSValue fdValue, JSValue typeValue)
{
    int fd;
    int type = 0;
    if (!posixToFileDescriptor(globalObject, fdValue, fd) || (typeValue && !posixToInt(globalObject, typeValue, type)))
        return { };
    if (setns(fd, type))
        return posixError(globalObject);
    return jsUndefined();
}

static JSValue posixUnshare(JSGlobalObject* globalObject, JSValue flagsValue)
{
    int flags;
    if (!posixToInt(globalObject, flagsValue, flags))
        return { };
    if (unshare(flags))
        return posixError(globalObject);
    return jsUndefined();
}
#endif // OS(LINUX)

// ---- Signals, and the end ----

static JSValue posixKill(JSGlobalObject* globalObject, JSValue pidValue, JSValue signalValue)
{
    pid_t pid;
    int64_t signal;
    if (!posixToPid(globalObject, pidValue, pid) || !posixToSsize(globalObject, signalValue, signal))
        return { };
    if (!audit(globalObject, "os.kill"_s, jsNumber(pid), signalValue))
        return { };
    // A program that sends itself a signal counts on it having arrived, or being pending if it is blocked, when this returns.
    // The system might hand the signal to one of the threads that the program knows nothing of (see PySignal.cpp).
    // But if the program has several threads, the system chooses among them: the main one, unless it blocks the signal.
    auto isForAnotherThread = [&] {
        Threads& threads = runtime(globalObject).threads();
        if (!threads.isMainThread())
            return true;
        sigset_t blocked;
        return threads.runningCount() && signal > 0 && signal < NSIG && !pthread_sigmask(SIG_BLOCK, nullptr, &blocked) && sigismember(&blocked, static_cast<int>(signal)) == 1;
    };
    if (pid == getpid() && !isForAnotherThread()) {
        if (int error = pthread_kill(pthread_self(), static_cast<int>(signal)))
            return posixError(globalObject, error);
        return jsUndefined();
    }
    if (kill(pid, static_cast<int>(signal)) == -1)
        return posixError(globalObject);
    return jsUndefined();
}

static JSValue posixKillpg(JSGlobalObject* globalObject, JSValue groupValue, JSValue signalValue)
{
    pid_t group;
    int signal;
    if (!posixToPid(globalObject, groupValue, group) || !posixToInt(globalObject, signalValue, signal))
        return { };
    if (!audit(globalObject, "os.killpg"_s, jsNumber(group), jsNumber(signal)))
        return { };
    if (killpg(group, signal) == -1)
        return posixError(globalObject);
    return jsUndefined();
}

static JSValue posixExit(JSGlobalObject* globalObject, JSValue statusValue)
{
    int status;
    if (!posixToInt(globalObject, statusValue, status))
        return { };
    _exit(status);
}

static JSValue posixAbort(JSGlobalObject*)
{
    abort();
}

static JSValue posixUmask(JSGlobalObject* globalObject, JSValue maskValue)
{
    int mask;
    if (!posixToInt(globalObject, maskValue, mask))
        return { };
    return jsNumber(static_cast<int>(umask(static_cast<mode_t>(mask))));
}

static JSValue posixSystem(JSGlobalObject* globalObject, JSValue commandValue)
{
    PosixBytes command;
    if (!posixToFSBytes(globalObject, commandValue, command))
        return { };
    if (isAudited(globalObject) && !audit(globalObject, "os.system"_s, PyBytes::create(globalObject, command.span())))
        return { };
    int status;
    {
        AllowThreads allow(globalObject);
        status = system(command.characters());
    }
    return jsNumber(status);
}

// There is nothing to call them after.
static JSValue posixRegisterAtFork(JSGlobalObject* globalObject, JSValue before, JSValue afterInChild, JSValue afterInParent)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!before && !afterInChild && !afterInParent)
        return raiseTypeError(globalObject, scope, "At least one argument is required."_s);
    std::pair<JSValue, ASCIILiteral> hooks[] = { { before, "before"_s }, { afterInChild, "after_in_child"_s }, { afterInParent, "after_in_parent"_s } };
    for (auto& [hook, name] : hooks) {
        if (hook && !isCallable(globalObject, hook))
            return raiseTypeError(globalObject, scope, '\'', name, "' must be callable, not "_s, typeName(globalObject, hook));
    }
    return jsUndefined();
}

// ---- Running a program ----

// The strings of argv or of the environment, as the system wants them.
class PosixStringArray {
public:
    void append(const PosixBytes& string)
    {
        m_offsets.append(m_characters.size());
        m_characters.append(string.span());
        m_characters.append('\0');
    }

    void appendPair(const PosixBytes& key, const PosixBytes& value)
    {
        m_offsets.append(m_characters.size());
        m_characters.append(key.span());
        m_characters.append('=');
        m_characters.append(value.span());
        m_characters.append('\0');
    }

    size_t size() const { return m_offsets.size(); }
    const char* at(size_t index) const { return m_characters.span().data() + m_offsets[index]; }

    // Good until something more is appended.
    char** pointers()
    {
        m_pointers.shrink(0);
        for (size_t offset : m_offsets)
            m_pointers.append(m_characters.mutableSpan().data() + offset);
        m_pointers.append(nullptr);
        return m_pointers.mutableSpan().data();
    }

private:
    Vector<char, 512> m_characters;
    Vector<size_t, 32> m_offsets;
    Vector<char*, 32> m_pointers;
};

static bool posixIsListOrTuple(JSValue value) { return isList(value) || PyTuple::is(value); }

static bool posixParseArguments(JSGlobalObject* globalObject, JSValue argv, int64_t count, PosixStringArray& result)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PosixBytes string;
    for (int64_t i = 0; i < count; ++i) {
        JSValue item = getItem(globalObject, argv, makeInt(globalObject, i));
        RETURN_IF_EXCEPTION(scope, false);
        if (!posixToFSBytes(globalObject, item, string))
            return false;
        result.append(string);
    }
    return true;
}

static bool posixParseEnvironment(JSGlobalObject* globalObject, JSValue env, PosixStringArray& result)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t count = length(globalObject, env);
    RETURN_IF_EXCEPTION(scope, false);
    JSValue keysView = callMethod(globalObject, env, rt.names.keys());
    RETURN_IF_EXCEPTION(scope, false);
    JSArray* keys = listFromIterable(globalObject, keysView);
    RETURN_IF_EXCEPTION(scope, false);
    JSValue valuesView = callMethod(globalObject, env, rt.names.values());
    RETURN_IF_EXCEPTION(scope, false);
    JSArray* values = listFromIterable(globalObject, valuesView);
    RETURN_IF_EXCEPTION(scope, false);
    PosixBytes key;
    PosixBytes value;
    for (int64_t i = 0; i < count; ++i) {
        if (i >= listLength(keys) || i >= listLength(values)) {
            raiseIndexError(globalObject, scope, "list index out of range"_s);
            return false;
        }
        if (!posixToFSBytes(globalObject, listGet(keys, static_cast<unsigned>(i)), key) || !posixToFSBytes(globalObject, listGet(values, static_cast<unsigned>(i)), value))
            return false;
        // An '=' may come first in a name, as it does in some that shells keep for themselves.
        if (!key.length() || memchr(key.characters() + 1, '=', key.length() - 1)) {
            raiseValueError(globalObject, scope, "illegal environment variable name"_s);
            return false;
        }
        result.appendPair(key, value);
    }
    return true;
}

static bool posixIsMapping(JSGlobalObject* globalObject, JSValue value)
{
    return !!typeOf(globalObject, value)->lookup(Special::GetItem);
}

static JSValue posixExecv(JSGlobalObject* globalObject, JSValue pathValue, JSValue argv)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PosixPath path("execv"_s, PosixPath::VirtualUnsupported);
    if (!path.convert(globalObject, pathValue))
        return { };
    if (!posixIsListOrTuple(argv))
        return raiseTypeError(globalObject, scope, "execv() arg 2 must be a tuple or list"_s);
    int64_t count = length(globalObject, argv);
    RETURN_IF_EXCEPTION(scope, { });
    if (count < 1)
        return raiseValueError(globalObject, scope, "execv() arg 2 must not be empty"_s);
    PosixStringArray arguments;
    if (!posixParseArguments(globalObject, argv, count, arguments))
        return { };
    if (!arguments.at(0)[0])
        return raiseValueError(globalObject, scope, "execv() arg 2 first element cannot be empty"_s);
    if (!audit(globalObject, "os.exec"_s, path.object(), argv, jsUndefined()))
        return { };
    execv(path.narrow(), arguments.pointers());
    return posixError(globalObject);
}

static JSValue posixExecve(JSGlobalObject* globalObject, JSValue pathValue, JSValue argv, JSValue env)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
#if OS(LINUX)
    PosixPath path("execve"_s, PosixPath::AllowFd | PosixPath::VirtualUnsupported);
#else
    PosixPath path("execve"_s, PosixPath::VirtualUnsupported);
#endif
    if (!path.convert(globalObject, pathValue))
        return { };
    if (!posixIsListOrTuple(argv))
        return raiseTypeError(globalObject, scope, "execve: argv must be a tuple or list"_s);
    int64_t count = length(globalObject, argv);
    RETURN_IF_EXCEPTION(scope, { });
    if (count < 1)
        return raiseValueError(globalObject, scope, "execve: argv must not be empty"_s);
    if (!posixIsMapping(globalObject, env))
        return raiseTypeError(globalObject, scope, "execve: environment must be a mapping object"_s);
    PosixStringArray arguments;
    if (!posixParseArguments(globalObject, argv, count, arguments))
        return { };
    if (!arguments.at(0)[0])
        return raiseValueError(globalObject, scope, "execve: argv first element cannot be empty"_s);
    PosixStringArray environment;
    if (!posixParseEnvironment(globalObject, env, environment))
        return { };
    if (!audit(globalObject, "os.exec"_s, path.object(), argv, env))
        return { };
#if OS(LINUX)
    if (path.isFd())
        fexecve(path.fd(), arguments.pointers(), environment.pointers());
    else
#endif
        execve(path.narrow(), arguments.pointers(), environment.pointers());
    return path.error(globalObject);
}

// An iterable of signal numbers.
static bool posixToSignalSet(JSGlobalObject* globalObject, JSValue iterable, sigset_t& result)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    sigemptyset(&result);
    forEach(globalObject, iterable, [&](JSValue item) {
        JSValue index = toIndex(globalObject, item);
        RETURN_IF_EXCEPTION(scope, false);
        auto number = tryInt64(index);
        if (!number || *number <= 0 || *number >= NSIG) {
            // What does not fit a long is reported as -1.
            raiseValueError(globalObject, scope, "signal number "_s, number.value_or(-1), " out of range [1; "_s, NSIG - 1, ']');
            return false;
        }
        if (sigaddset(&result, static_cast<int>(*number))) {
            if (errno != EINVAL) {
                posixError(globalObject);
                return false;
            }
            // range(1, NSIG) has long been written, though the system keeps some of the numbers to itself.
            return warn(globalObject, runtime(globalObject).RuntimeWarning(), makeString("invalid signal number "_s, *number, ", please use valid_signals()"_s));
        }
        return true;
    });
    RETURN_IF_EXCEPTION(scope, false);
    return true;
}

enum PosixSpawnFileAction : int {
    PosixSpawnOpen,
    PosixSpawnClose,
    PosixSpawnDup2,
    PosixSpawnCloseFrom,
};

// Newer than the oldest C library this may run with, so it is looked for when it is wanted. Null: it is not there.
using PosixAddCloseFrom = int (*)(posix_spawn_file_actions_t*, int);
static PosixAddCloseFrom posixAddCloseFrom()
{
    static PosixAddCloseFrom function;
    static bool isLookedUp;
    if (!std::exchange(isLookedUp, true))
        function = reinterpret_cast<PosixAddCloseFrom>(dlsym(RTLD_DEFAULT, "posix_spawn_file_actions_addclosefrom_np"));
    return function;
}

class PosixSpawnSettings {
    WTF_MAKE_NONCOPYABLE(PosixSpawnSettings);
public:
    PosixSpawnSettings() = default;
    ~PosixSpawnSettings()
    {
        if (m_hasActions)
            posix_spawn_file_actions_destroy(&m_actions);
        if (m_hasAttributes)
            posix_spawnattr_destroy(&m_attributes);
    }

    posix_spawn_file_actions_t* actions() { return m_hasActions ? &m_actions : nullptr; }
    posix_spawnattr_t* attributes() { return m_hasAttributes ? &m_attributes : nullptr; }

    bool parseFileActions(JSGlobalObject* globalObject, JSValue fileActions)
    {
        VM& vm = getVM(globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        PyTuple* sequence = PyTuple::tryFrom(fileActions);
        if (!sequence) {
            sequence = tupleFromIterable(globalObject, fileActions);
            if (!sequence) {
                if (clearPendingExceptionIfMatches(globalObject, runtime(globalObject).TypeError()))
                    raiseTypeError(globalObject, scope, "file_actions must be a sequence or None"_s);
                return false;
            }
        }
        if (int error = posix_spawn_file_actions_init(&m_actions)) {
            posixError(globalObject, error);
            return false;
        }
        m_hasActions = true;
        for (unsigned i = 0; i < sequence->length(); ++i) {
            auto* action = PyTuple::tryFrom(sequence->at(i));
            if (!action || !action->length()) {
                raiseTypeError(globalObject, scope, "Each file_actions element must be a non-empty tuple"_s);
                return false;
            }
            int64_t tag;
            if (!posixToLong(globalObject, action->at(0), tag))
                return false;
            auto hasLength = [&](unsigned expected, ASCIILiteral name) {
                if (action->length() == expected)
                    return true;
                raiseTypeError(globalObject, scope, "A "_s, name, " file_action tuple must have "_s, expected, " elements"_s);
                return false;
            };
            int error = 0;
            switch (tag) {
            case PosixSpawnOpen: {
                int fd;
                int flags;
                unsigned mode;
                PosixBytes path;
                if (!hasLength(5, "open"_s) || !posixToInt(globalObject, action->at(1), fd) || !posixToFSBytes(globalObject, action->at(2), path)
                    || !posixToInt(globalObject, action->at(3), flags) || !posixToUnsignedMask(globalObject, action->at(4), mode))
                    return false;
                error = posix_spawn_file_actions_addopen(&m_actions, fd, path.characters(), flags, static_cast<mode_t>(mode));
                break;
            }
            case PosixSpawnClose: {
                int fd;
                if (!hasLength(2, "close"_s) || !posixToInt(globalObject, action->at(1), fd))
                    return false;
                error = posix_spawn_file_actions_addclose(&m_actions, fd);
                break;
            }
            case PosixSpawnDup2: {
                int fd;
                int fd2;
                if (!hasLength(3, "dup2"_s) || !posixToInt(globalObject, action->at(1), fd) || !posixToInt(globalObject, action->at(2), fd2))
                    return false;
                error = posix_spawn_file_actions_adddup2(&m_actions, fd, fd2);
                break;
            }
            case PosixSpawnCloseFrom:
                if (auto addCloseFrom = posixAddCloseFrom()) {
                    int fd;
                    if (!hasLength(2, "closefrom"_s) || !posixToInt(globalObject, action->at(1), fd))
                        return false;
                    error = addCloseFrom(&m_actions, fd);
                    break;
                }
                [[fallthrough]];
            default:
                raiseTypeError(globalObject, scope, "Unknown file_actions identifier"_s);
                return false;
            }
            if (error) {
                posixError(globalObject, error);
                return false;
            }
        }
        return true;
    }

    bool parseFlags(JSGlobalObject* globalObject, ASCIILiteral function, JSValue setpgroup, bool resetids, bool setsid, JSValue setsigmask, JSValue setsigdef, JSValue scheduler)
    {
        VM& vm = getVM(globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        auto failed = [&](int error) {
            if (error)
                posixError(globalObject, error);
            return !!error;
        };
        if (failed(posix_spawnattr_init(&m_attributes)))
            return false;
        m_hasAttributes = true;
        int flags = 0;
        if (setpgroup && !isNone(setpgroup)) {
            pid_t group;
            if (!posixToPid(globalObject, setpgroup, group) || failed(posix_spawnattr_setpgroup(&m_attributes, group)))
                return false;
            flags |= POSIX_SPAWN_SETPGROUP;
        }
        if (resetids)
            flags |= POSIX_SPAWN_RESETIDS;
        if (setsid) {
#ifdef POSIX_SPAWN_SETSID
            flags |= POSIX_SPAWN_SETSID;
#else
            raiseNotImplementedError(globalObject, scope, function, ": setsid unavailable on this platform"_s);
            return false;
#endif
        }
        UNUSED_PARAM(function);
        if (setsigmask) {
            sigset_t set;
            if (!posixToSignalSet(globalObject, setsigmask, set) || failed(posix_spawnattr_setsigmask(&m_attributes, &set)))
                return false;
            flags |= POSIX_SPAWN_SETSIGMASK;
        }
        if (setsigdef) {
            sigset_t set;
            if (!posixToSignalSet(globalObject, setsigdef, set) || failed(posix_spawnattr_setsigdefault(&m_attributes, &set)))
                return false;
            flags |= POSIX_SPAWN_SETSIGDEF;
        }
        if (scheduler && !isNone(scheduler)) {
#if OS(LINUX)
            PyTuple* pair = PyTuple::from(scheduler);
            if (pair->length() != 2) {
                raiseTypeError(globalObject, scope, "A scheduler tuple must have two elements"_s);
                return false;
            }
            struct sched_param param;
            if (!posixToSchedParam(globalObject, pair->at(1), param))
                return false;
            if (!isNone(pair->at(0))) {
                int policy;
                if (!posixToInt(globalObject, pair->at(0), policy) || failed(posix_spawnattr_setschedpolicy(&m_attributes, policy)))
                    return false;
                flags |= POSIX_SPAWN_SETSCHEDULER;
            }
            if (failed(posix_spawnattr_setschedparam(&m_attributes, &param)))
                return false;
            flags |= POSIX_SPAWN_SETSCHEDPARAM;
#else
            raiseNotImplementedError(globalObject, scope, "The scheduler option is not supported in this system."_s);
            return false;
#endif
        }
        return !failed(posix_spawnattr_setflags(&m_attributes, static_cast<short>(flags)));
    }

private:
    posix_spawn_file_actions_t m_actions;
    posix_spawnattr_t m_attributes;
    bool m_hasActions { false };
    bool m_hasAttributes { false };
};

static JSValue posixSpawnNamed(JSGlobalObject* globalObject, ASCIILiteral function, bool searchPath, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (args.size() != 3)
        return raiseTypeError(globalObject, scope, function, "() takes exactly 3 positional arguments ("_s, args.size(), " given)"_s);
    static constexpr ASCIILiteral names[] = { "file_actions"_s, "setpgroup"_s, "resetids"_s, "setsid"_s, "setsigmask"_s, "setsigdef"_s, "scheduler"_s };
    JSValue options[std::size(names)];
    if (!parseKeywordArguments(globalObject, function, args, names, options))
        return { };
    auto [fileActions, setpgroup, resetidsValue, setsidValue, setsigmask, setsigdef, scheduler] = options;

    PosixPath path(function, PosixPath::VirtualUnsupported);
    bool resetids;
    bool setsid;
    if (!path.convert(globalObject, args[0]) || !posixToBool(globalObject, resetidsValue, false, resetids) || !posixToBool(globalObject, setsidValue, false, setsid))
        return { };
    JSValue argv = args[1];
    JSValue env = args[2];
    if (!posixIsListOrTuple(argv))
        return raiseTypeError(globalObject, scope, function, ": argv must be a tuple or list"_s);
    int64_t count = length(globalObject, argv);
    RETURN_IF_EXCEPTION(scope, { });
    if (count < 1)
        return raiseValueError(globalObject, scope, function, ": argv must not be empty"_s);
    if (!isNone(env) && !posixIsMapping(globalObject, env))
        return raiseTypeError(globalObject, scope, function, ": environment must be a mapping object or None"_s);
    if (scheduler && !isNone(scheduler) && !PyTuple::is(scheduler))
        return raiseTypeError(globalObject, scope, function, ": scheduler must be a tuple or None"_s);
    PosixStringArray arguments;
    if (!posixParseArguments(globalObject, argv, count, arguments))
        return { };
    if (!arguments.at(0)[0])
        return raiseValueError(globalObject, scope, function, ": argv first element cannot be empty"_s);
    PosixStringArray environment;
    char** environmentPointers;
    if (isNone(env)) {
#if OS(DARWIN)
        environmentPointers = *_NSGetEnviron();
#else
        environmentPointers = environ;
#endif
    } else {
        if (!posixParseEnvironment(globalObject, env, environment))
            return { };
        environmentPointers = environment.pointers();
    }
    PosixSpawnSettings settings;
    if (fileActions && !isNone(fileActions) && !settings.parseFileActions(globalObject, fileActions))
        return { };
    if (!settings.parseFlags(globalObject, function, setpgroup, resetids, setsid, setsigmask, setsigdef, scheduler))
        return { };
    if (!audit(globalObject, "os.posix_spawn"_s, path.object(), argv, env))
        return { };
    pid_t pid;
    int error = searchPath
        ? posix_spawnp(&pid, path.narrow(), settings.actions(), settings.attributes(), arguments.pointers(), environmentPointers)
        : posix_spawn(&pid, path.narrow(), settings.actions(), settings.attributes(), arguments.pointers(), environmentPointers);
    if (error)
        return raiseOSError(globalObject, scope, error, path.object());
    return jsNumber(pid);
}

static JSValue posixSpawn(JSGlobalObject* globalObject, const CallArgs& args) { return posixSpawnNamed(globalObject, "posix_spawn"_s, false, args); }
static JSValue posixSpawnp(JSGlobalObject* globalObject, const CallArgs& args) { return posixSpawnNamed(globalObject, "posix_spawnp"_s, true, args); }

// os.spawnv() and its kin. Lib/os.py writes them with fork(), which there is none of, unless the module posix has them, as
// that of Windows does. They do what those of os.py do.
enum : int { PosixSpawnWait = 0, PosixSpawnNoWait = 1 };

static JSValue posixSpawnFamily(JSGlobalObject* globalObject, JSValue modeValue, JSValue file, JSValue argv, JSValue env, bool searchPath)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int mode;
    if (!posixToInt(globalObject, modeValue, mode))
        return { };
    if (!posixIsListOrTuple(argv))
        return raiseTypeError(globalObject, scope, "argv must be a tuple or a list"_s);
    int64_t count = length(globalObject, argv);
    RETURN_IF_EXCEPTION(scope, { });
    bool firstIsEmpty = !count;
    if (count) {
        JSValue first = getItem(globalObject, argv, jsNumber(0));
        RETURN_IF_EXCEPTION(scope, { });
        firstIsEmpty = !isTrue(globalObject, first);
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (firstIsEmpty)
        return raiseValueError(globalObject, scope, "argv first element cannot be empty"_s);
    if (!audit(globalObject, "os.spawn"_s, jsNumber(mode), file, argv, env))
        return { };
    // What exec() would not take is found out by the child of os.py, which then exits like one that found no program.
    PosixBytes program;
    PosixStringArray arguments;
    PosixStringArray environment;
    bool canRun = posixToFSBytes(globalObject, file, program) && posixParseArguments(globalObject, argv, count, arguments)
        && (!env || (posixIsMapping(globalObject, env) && posixParseEnvironment(globalObject, env, environment)));
    if (!canRun)
        takePendingException(globalObject);

    // As os._execvpe() looks for it.
    PosixStringArray executables;
    if (!canRun) {
    } else if (!searchPath || program.contains('/'))
        executables.append(program);
    else {
        const char* path = nullptr;
        if (!env)
            path = getenv("PATH");
        else {
            for (size_t i = 0; i < environment.size(); ++i) {
                if (!strncmp(environment.at(i), "PATH=", 5))
                    path = environment.at(i) + 5;
            }
        }
        if (!path)
            path = "/bin:/usr/bin";
        PosixBytes candidate;
        Vector<char, 256> joined;
        for (const char* start = path;;) {
            const char* end = start;
            while (*end && *end != ':')
                ++end;
            joined.shrink(0);
            joined.append(std::span { start, static_cast<size_t>(end - start) });
            if (end != start && end[-1] != '/')
                joined.append('/');
            joined.append(program.span());
            candidate.assign(joined.span());
            executables.append(candidate);
            if (!*end)
                break;
            start = end + 1;
        }
    }

    pid_t pid = posixSpawnChild(executables.pointers(), arguments.pointers(), env ? environment.pointers() : nullptr);
    if (pid == -1)
        return posixError(globalObject);
    if (mode == PosixSpawnNoWait)
        return jsNumber(pid);
    for (;;) {
        int status = 0;
        pid_t result;
        do {
            AllowThreads allow(globalObject);
            result = waitpid(pid, &status, 0);
        } while (result < 0 && errno == EINTR && checkSignals(globalObject));
        RETURN_IF_EXCEPTION(scope, { });
        if (result < 0)
            return posixError(globalObject);
        if (WIFSTOPPED(status))
            continue;
        return jsNumber(WIFSIGNALED(status) ? -WTERMSIG(status) : WEXITSTATUS(status));
    }
}

static JSValue posixSpawnv(JSGlobalObject* globalObject, JSValue mode, JSValue file, JSValue argv) { return posixSpawnFamily(globalObject, mode, file, argv, { }, false); }
static JSValue posixSpawnve(JSGlobalObject* globalObject, JSValue mode, JSValue file, JSValue argv, JSValue env) { return posixSpawnFamily(globalObject, mode, file, argv, env, false); }
static JSValue posixSpawnvp(JSGlobalObject* globalObject, JSValue mode, JSValue file, JSValue argv) { return posixSpawnFamily(globalObject, mode, file, argv, { }, true); }
static JSValue posixSpawnvpe(JSGlobalObject* globalObject, JSValue mode, JSValue file, JSValue argv, JSValue env) { return posixSpawnFamily(globalObject, mode, file, argv, env, true); }

// ---- Waiting for children ----

static constexpr ASCIILiteral posixRusageFields[] = {
    "ru_utime"_s, "ru_stime"_s, "ru_maxrss"_s, "ru_ixrss"_s, "ru_idrss"_s, "ru_isrss"_s, "ru_minflt"_s, "ru_majflt"_s,
    "ru_nswap"_s, "ru_inblock"_s, "ru_oublock"_s, "ru_msgsnd"_s, "ru_msgrcv"_s, "ru_nsignals"_s, "ru_nvcsw"_s, "ru_nivcsw"_s,
};
static constexpr StructSeqDef posixRusageDef { "resource.struct_rusage"_s, posixRusageFields, 16 };

PyType* posixRusageType(JSGlobalObject* globalObject)
{
    return cachedStructSeqType(globalObject, posixRusageDef);
}

static JSValue posixRusage(JSGlobalObject* globalObject, const struct rusage& usage)
{
    auto inSeconds = [&](const struct timeval& time) { return makeFloat(globalObject, static_cast<double>(time.tv_sec) + static_cast<double>(time.tv_usec) * 0.000001); };
    auto number = [&](long value) { return posixMakeInt(globalObject, value); };
    return makeStructSeq(globalObject, posixRusageType(globalObject), {
        inSeconds(usage.ru_utime), inSeconds(usage.ru_stime), number(usage.ru_maxrss), number(usage.ru_ixrss), number(usage.ru_idrss), number(usage.ru_isrss),
        number(usage.ru_minflt), number(usage.ru_majflt), number(usage.ru_nswap), number(usage.ru_inblock), number(usage.ru_oublock), number(usage.ru_msgsnd),
        number(usage.ru_msgrcv), number(usage.ru_nsignals), number(usage.ru_nvcsw), number(usage.ru_nivcsw) });
}

static JSValue posixWait(JSGlobalObject* globalObject)
{
    int status = 0;
    pid_t pid;
    do {
        AllowThreads allow(globalObject);
        pid = wait(&status);
    } while (pid < 0 && errno == EINTR && checkSignals(globalObject));
    if (pid < 0)
        return posixError(globalObject);
    return PyTuple::create(globalObject, { jsNumber(pid), jsNumber(status) });
}

static JSValue posixWaitpid(JSGlobalObject* globalObject, JSValue pidValue, JSValue optionsValue)
{
    pid_t pid;
    int options;
    if (!posixToPid(globalObject, pidValue, pid) || !posixToInt(globalObject, optionsValue, options))
        return { };
    int status = 0;
    pid_t result;
    do {
        AllowThreads allow(globalObject);
        result = waitpid(pid, &status, options);
    } while (result < 0 && errno == EINTR && checkSignals(globalObject));
    if (result < 0)
        return posixError(globalObject);
    return PyTuple::create(globalObject, { jsNumber(result), jsNumber(status) });
}

static JSValue posixWaitResult(JSGlobalObject* globalObject, pid_t pid, int status, struct rusage& usage)
{
    // No child has changed state: there is nothing to account for.
    if (!pid)
        memset(&usage, 0, sizeof(usage));
    return PyTuple::create(globalObject, { jsNumber(pid), jsNumber(status), posixRusage(globalObject, usage) });
}

static JSValue posixWait3(JSGlobalObject* globalObject, JSValue optionsValue)
{
    int options;
    if (!posixToInt(globalObject, optionsValue, options))
        return { };
    int status = 0;
    struct rusage usage;
    pid_t pid;
    do {
        AllowThreads allow(globalObject);
        pid = wait4(-1, &status, options, &usage);
    } while (pid < 0 && errno == EINTR && checkSignals(globalObject));
    if (pid < 0)
        return posixError(globalObject);
    return posixWaitResult(globalObject, pid, status, usage);
}

static JSValue posixWait4(JSGlobalObject* globalObject, JSValue pidValue, JSValue optionsValue)
{
    pid_t pid;
    int options;
    if (!posixToPid(globalObject, pidValue, pid) || !posixToInt(globalObject, optionsValue, options))
        return { };
    int status = 0;
    struct rusage usage;
    pid_t result;
    do {
        AllowThreads allow(globalObject);
        result = wait4(pid, &status, options, &usage);
    } while (result < 0 && errno == EINTR && checkSignals(globalObject));
    if (result < 0)
        return posixError(globalObject);
    return posixWaitResult(globalObject, result, status, usage);
}

static constexpr ASCIILiteral posixWaitidResultFields[] = { "si_pid"_s, "si_uid"_s, "si_signo"_s, "si_status"_s, "si_code"_s };
static constexpr StructSeqDef posixWaitidResultDef { "posix.waitid_result"_s, posixWaitidResultFields, 5 };

static JSValue posixWaitid(JSGlobalObject* globalObject, JSValue typeValue, JSValue idValue, JSValue optionsValue)
{
    int type;
    pid_t id;
    int options;
    if (!posixToInt(globalObject, typeValue, type) || !posixToPid(globalObject, idValue, id) || !posixToInt(globalObject, optionsValue, options))
        return { };
    siginfo_t info;
    info.si_pid = 0;
    int result;
    do {
        AllowThreads allow(globalObject);
        result = waitid(static_cast<idtype_t>(type), static_cast<id_t>(id), &info, options);
    } while (result < 0 && errno == EINTR && checkSignals(globalObject));
    if (result < 0)
        return posixError(globalObject);
    if (!info.si_pid)
        return jsUndefined();
    return makeStructSeq(globalObject, cachedStructSeqType(globalObject, posixWaitidResultDef), {
        jsNumber(info.si_pid), posixFromUid(globalObject, info.si_uid), jsNumber(info.si_signo), jsNumber(info.si_status), jsNumber(info.si_code) });
}

// What the status of a child that wait() reported means.
#define POSIX_DEFINE_STATUS_FUNCTION(name, macro, convert) \
    static JSValue name(JSGlobalObject* globalObject, JSValue statusValue) \
    { \
        int status; \
        if (!posixToInt(globalObject, statusValue, status)) \
            return { }; \
        return convert(macro(status)); \
    }

POSIX_DEFINE_STATUS_FUNCTION(posixWCOREDUMP, WCOREDUMP, jsBoolean)
POSIX_DEFINE_STATUS_FUNCTION(posixWIFCONTINUED, WIFCONTINUED, jsBoolean)
POSIX_DEFINE_STATUS_FUNCTION(posixWIFSTOPPED, WIFSTOPPED, jsBoolean)
POSIX_DEFINE_STATUS_FUNCTION(posixWIFSIGNALED, WIFSIGNALED, jsBoolean)
POSIX_DEFINE_STATUS_FUNCTION(posixWIFEXITED, WIFEXITED, jsBoolean)
POSIX_DEFINE_STATUS_FUNCTION(posixWEXITSTATUS, WEXITSTATUS, jsNumber)
POSIX_DEFINE_STATUS_FUNCTION(posixWTERMSIG, WTERMSIG, jsNumber)
POSIX_DEFINE_STATUS_FUNCTION(posixWSTOPSIG, WSTOPSIG, jsNumber)

#undef POSIX_DEFINE_STATUS_FUNCTION

static JSValue posixWaitstatusToExitcode(JSGlobalObject* globalObject, JSValue statusValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int status;
    if (!posixToInt(globalObject, statusValue, status))
        return { };
    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        if (code < 0)
            return raiseValueError(globalObject, scope, "invalid WEXITSTATUS: "_s, code);
        return jsNumber(code);
    }
    if (WIFSIGNALED(status)) {
        int signal = WTERMSIG(status);
        if (signal <= 0)
            return raiseValueError(globalObject, scope, "invalid WTERMSIG: "_s, signal);
        return jsNumber(-signal);
    }
    if (WIFSTOPPED(status))
        return raiseValueError(globalObject, scope, "process stopped by delivery of signal "_s, WSTOPSIG(status));
    return raiseValueError(globalObject, scope, "invalid wait status: "_s, status);
}

// ---- The table ----

static constexpr NativeDef posixProcessFunctions[] = {
    PY_FUNCTION("getpid", posixGetpid, "()"),
    PY_FUNCTION("getppid", posixGetppid, "()"),
    PY_FUNCTION("getuid", posixGetuid, "()"),
    PY_FUNCTION("geteuid", posixGeteuid, "()"),
    PY_FUNCTION("getgid", posixGetgid, "()"),
    PY_FUNCTION("getegid", posixGetegid, "()"),
    PY_FUNCTION("setuid", posixSetuid, "(uid, /)"),
    PY_FUNCTION("seteuid", posixSeteuid, "(euid, /)"),
    PY_FUNCTION("setgid", posixSetgid, "(gid, /)"),
    PY_FUNCTION("setegid", posixSetegid, "(egid, /)"),
    PY_FUNCTION("setreuid", posixSetreuid, "(ruid, euid, /)"),
    PY_FUNCTION("setregid", posixSetregid, "(rgid, egid, /)"),
#if OS(LINUX)
    PY_FUNCTION("setresuid", posixSetresuid, "(ruid, euid, suid, /)"),
    PY_FUNCTION("setresgid", posixSetresgid, "(rgid, egid, sgid, /)"),
    PY_FUNCTION("getresuid", posixGetresuid, "()"),
    PY_FUNCTION("getresgid", posixGetresgid, "()"),
#endif
    PY_FUNCTION("getlogin", posixGetlogin, "()"),
    PY_FUNCTION("getgroups", posixGetgroups, "()"),
    PY_FUNCTION("getgrouplist", posixGetgrouplist, "(user, group, /)"),
    PY_FUNCTION("setgroups", posixSetgroups, "(groups, /)"),
    PY_FUNCTION("initgroups", posixInitgroups, "(username, gid, /)"),
    PY_FUNCTION("getpgid", posixGetpgid, "(pid)"),
    PY_FUNCTION("getpgrp", posixGetpgrp, "()"),
    PY_FUNCTION("setpgid", posixSetpgid, "(pid, pgrp, /)"),
    PY_FUNCTION("setpgrp", posixSetpgrp, "()"),
    PY_FUNCTION("getsid", posixGetsid, "(pid, /)"),
    PY_FUNCTION("setsid", posixSetsid, "()"),
    PY_FUNCTION("nice", posixNice, "(increment, /)"),
    PY_FUNCTION("getpriority", posixGetpriority, "(which, who)"),
    PY_FUNCTION("setpriority", posixSetpriority, "(which, who, priority)"),
    PY_FUNCTION("sched_get_priority_max", posixSchedGetPriorityMax, "(policy)"),
    PY_FUNCTION("sched_get_priority_min", posixSchedGetPriorityMin, "(policy)"),
    PY_FUNCTION("sched_yield", posixSchedYield, "()"),
#if OS(LINUX)
    PY_FUNCTION("sched_getscheduler", posixSchedGetscheduler, "(pid, /)"),
    PY_FUNCTION("sched_setscheduler", posixSchedSetscheduler, "(pid, policy, param, /)"),
    PY_FUNCTION("sched_getparam", posixSchedGetparam, "(pid, /)"),
    PY_FUNCTION("sched_setparam", posixSchedSetparam, "(pid, param, /)"),
    PY_FUNCTION("sched_rr_get_interval", posixSchedRrGetInterval, "(pid, /)"),
    PY_FUNCTION("sched_setaffinity", posixSchedSetaffinity, "(pid, mask, /)"),
    PY_FUNCTION("sched_getaffinity", posixSchedGetaffinity, "(pid, /)"),
    PY_FUNCTION("pidfd_open", posixPidfdOpen, "(pid, flags=0)"),
    PY_FUNCTION("setns", posixSetns, "(fd, nstype=0)"),
    PY_FUNCTION("unshare", posixUnshare, "(flags)"),
#endif
    PY_FUNCTION("kill", posixKill, "(pid, signal, /)"),
    PY_FUNCTION("killpg", posixKillpg, "(pgid, signal, /)"),
    PY_FUNCTION("_exit", posixExit, "(status)"),
    PY_FUNCTION("abort", posixAbort, "()"),
    PY_FUNCTION("umask", posixUmask, "(mask, /)"),
    PY_FUNCTION("system", posixSystem, "(command)"),
    PY_FUNCTION("register_at_fork", posixRegisterAtFork, "(*, before=<unrepresentable>, after_in_child=<unrepresentable>, after_in_parent=<unrepresentable>)"),
    PY_FUNCTION("spawnv", posixSpawnv, "(mode, file, args)"),
    PY_FUNCTION("spawnve", posixSpawnve, "(mode, file, args, env)"),
    PY_FUNCTION("spawnvp", posixSpawnvp, "(mode, file, args)"),
    PY_FUNCTION("spawnvpe", posixSpawnvpe, "(mode, file, args, env)"),
    PY_FUNCTION("execv", posixExecv, "(path, argv, /)"),
    PY_FUNCTION("execve", posixExecve, "(path, argv, env)"),
    PY_FUNCTION("posix_spawn", posixSpawn, "(path, argv, env, /, *args, file_actions=(), setpgroup=None, resetids=False, setsid=False, setsigmask=(), setsigdef=(), scheduler=None)"),
    PY_FUNCTION("posix_spawnp", posixSpawnp, "(path, argv, env, /, *args, file_actions=(), setpgroup=None, resetids=False, setsid=False, setsigmask=(), setsigdef=(), scheduler=None)"),
    PY_FUNCTION("wait", posixWait, "()"),
    PY_FUNCTION("waitpid", posixWaitpid, "(pid, options, /)"),
    PY_FUNCTION("wait3", posixWait3, "(options)"),
    PY_FUNCTION("wait4", posixWait4, "(pid, options)"),
    PY_FUNCTION("waitid", posixWaitid, "(idtype, id, options, /)"),
    PY_FUNCTION("WCOREDUMP", posixWCOREDUMP, "(status, /)"),
    PY_FUNCTION("WIFCONTINUED", posixWIFCONTINUED, "(status)"),
    PY_FUNCTION("WIFSTOPPED", posixWIFSTOPPED, "(status)"),
    PY_FUNCTION("WIFSIGNALED", posixWIFSIGNALED, "(status)"),
    PY_FUNCTION("WIFEXITED", posixWIFEXITED, "(status)"),
    PY_FUNCTION("WEXITSTATUS", posixWEXITSTATUS, "(status)"),
    PY_FUNCTION("WTERMSIG", posixWTERMSIG, "(status)"),
    PY_FUNCTION("WSTOPSIG", posixWSTOPSIG, "(status)"),
    PY_FUNCTION("waitstatus_to_exitcode", posixWaitstatusToExitcode, "(status)"),
};

void addPosixProcessFunctions(JSGlobalObject* globalObject, PyModule* module)
{
    module->addFunctions(globalObject, posixProcessFunctions);
    module->add(globalObject, "waitid_result"_s, cachedStructSeqType(globalObject, posixWaitidResultDef));
#if OS(LINUX)
    module->add(globalObject, "sched_param"_s, posixSchedParamType(globalObject));
#endif
    module->add(globalObject, "POSIX_SPAWN_OPEN"_s, jsNumber(PosixSpawnOpen));
    module->add(globalObject, "POSIX_SPAWN_CLOSE"_s, jsNumber(PosixSpawnClose));
    module->add(globalObject, "POSIX_SPAWN_DUP2"_s, jsNumber(PosixSpawnDup2));
    if (posixAddCloseFrom())
        module->add(globalObject, "POSIX_SPAWN_CLOSEFROM"_s, jsNumber(PosixSpawnCloseFrom));
}

// ---- The module resource ----

static JSValue resourceGetrusage(JSGlobalObject* globalObject, JSValue whoValue)
{
    int who;
    if (!posixToInt(globalObject, whoValue, who))
        return { };
    struct rusage usage;
    if (getrusage(static_cast<decltype(RUSAGE_SELF)>(who), &usage) == -1) {
        if (errno == EINVAL) {
            VM& vm = getVM(globalObject);
            auto scope = DECLARE_THROW_SCOPE(vm);
            return raiseValueError(globalObject, scope, "invalid who parameter"_s);
        }
        return posixError(globalObject);
    }
    return posixRusage(globalObject, usage);
}

static bool resourceToLimit(JSGlobalObject* globalObject, JSValue value, rlim_t& result)
{
    static_assert(sizeof(rlim_t) == sizeof(uint64_t));
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue index = toIndex(globalObject, value);
    RETURN_IF_EXCEPTION(scope, false);
    if (intSign(index) < 0) {
        // -1 used to be how "no limit" was written.
        auto converted = tryInt64(index);
        if (converted && static_cast<rlim_t>(*converted) == RLIM_INFINITY) {
            result = RLIM_INFINITY;
            return warn(globalObject, runtime(globalObject).DeprecationWarning(), "Use RLIM_INFINITY instead of negative limit value."_s);
        }
        raiseValueError(globalObject, scope, "Cannot convert negative int"_s);
        return false;
    }
    auto converted = tryUInt64(index);
    if (!converted) {
        raiseOverflowError(globalObject, scope, "Python int too large to convert to C rlim_t"_s);
        return false;
    }
    result = static_cast<rlim_t>(*converted);
    return true;
}

static bool resourceToLimits(JSGlobalObject* globalObject, JSValue value, struct rlimit& result)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyTuple* limits = tupleFromIterable(globalObject, value);
    RETURN_IF_EXCEPTION(scope, false);
    if (limits->length() != 2) {
        raiseValueError(globalObject, scope, "expected a tuple of 2 integers"_s);
        return false;
    }
    return resourceToLimit(globalObject, limits->at(0), result.rlim_cur) && resourceToLimit(globalObject, limits->at(1), result.rlim_max);
}

static JSValue resourceFromLimits(JSGlobalObject* globalObject, const struct rlimit& limits)
{
    return PyTuple::create(globalObject, { makeInt(globalObject, static_cast<uint64_t>(limits.rlim_cur)), makeInt(globalObject, static_cast<uint64_t>(limits.rlim_max)) });
}

static bool resourceToResource(JSGlobalObject* globalObject, JSValue value, int& result)
{
    if (!posixToInt(globalObject, value, result))
        return false;
    if (result < 0 || result >= static_cast<int>(RLIM_NLIMITS)) {
        VM& vm = getVM(globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        raiseValueError(globalObject, scope, "invalid resource specified"_s);
        return false;
    }
    return true;
}

static JSValue resourceGetrlimit(JSGlobalObject* globalObject, JSValue resourceValue)
{
    int resource;
    if (!resourceToResource(globalObject, resourceValue, resource))
        return { };
    struct rlimit limits;
    if (getrlimit(static_cast<decltype(RLIMIT_CPU)>(resource), &limits) == -1)
        return posixError(globalObject);
    return resourceFromLimits(globalObject, limits);
}

static JSValue resourceSetrlimit(JSGlobalObject* globalObject, JSValue resourceValue, JSValue limitsValue)
{
    int resource;
    struct rlimit limits;
    if (!resourceToResource(globalObject, resourceValue, resource) || !resourceToLimits(globalObject, limitsValue, limits))
        return { };
    if (!audit(globalObject, "resource.setrlimit"_s, jsNumber(resource), limitsValue))
        return { };
    if (setrlimit(static_cast<decltype(RLIMIT_CPU)>(resource), &limits) == -1) {
        VM& vm = getVM(globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        if (errno == EINVAL)
            return raiseValueError(globalObject, scope, "current limit exceeds maximum limit"_s);
        if (errno == EPERM)
            return raiseValueError(globalObject, scope, "not allowed to raise maximum limit"_s);
        return posixError(globalObject);
    }
    return jsUndefined();
}

#if OS(LINUX)
static JSValue resourcePrlimit(JSGlobalObject* globalObject, JSValue pidValue, JSValue resourceValue, JSValue limitsValue)
{
    pid_t pid;
    int resource;
    if (!posixToPid(globalObject, pidValue, pid) || !resourceToResource(globalObject, resourceValue, resource))
        return { };
    if (!audit(globalObject, "resource.prlimit"_s, jsNumber(pid), jsNumber(resource), limitsValue))
        return { };
    struct rlimit newLimits;
    struct rlimit oldLimits;
    bool hasLimits = limitsValue && !isNone(limitsValue);
    if (hasLimits && !resourceToLimits(globalObject, limitsValue, newLimits))
        return { };
    if (prlimit(pid, static_cast<decltype(RLIMIT_CPU)>(resource), hasLimits ? &newLimits : nullptr, &oldLimits) == -1) {
        if (errno == EINVAL) {
            VM& vm = getVM(globalObject);
            auto scope = DECLARE_THROW_SCOPE(vm);
            return raiseValueError(globalObject, scope, "current limit exceeds maximum limit"_s);
        }
        return posixError(globalObject);
    }
    return resourceFromLimits(globalObject, oldLimits);
}
#endif

static JSValue resourceGetpagesize(JSGlobalObject*)
{
    return jsNumber(getpagesize());
}

static constexpr NativeDef resourceFunctions[] = {
    PY_FUNCTION("getrusage", resourceGetrusage, "(who, /)"),
    PY_FUNCTION("getrlimit", resourceGetrlimit, "(resource, /)"),
    PY_FUNCTION("setrlimit", resourceSetrlimit, "(resource, limits, /)"),
#if OS(LINUX)
    PY_FUNCTION("prlimit", resourcePrlimit, "(pid, resource, limits=None, /)"),
#endif
    PY_FUNCTION("getpagesize", resourceGetpagesize, "()"),
};

static constexpr PosixConstant resourceConstants[] = {
    { "RLIMIT_CPU"_s, RLIMIT_CPU },
    { "RLIMIT_FSIZE"_s, RLIMIT_FSIZE },
    { "RLIMIT_DATA"_s, RLIMIT_DATA },
    { "RLIMIT_STACK"_s, RLIMIT_STACK },
    { "RLIMIT_CORE"_s, RLIMIT_CORE },
    { "RLIMIT_NOFILE"_s, RLIMIT_NOFILE },
#ifdef RLIMIT_OFILE
    { "RLIMIT_OFILE"_s, RLIMIT_OFILE },
#endif
    { "RLIMIT_AS"_s, RLIMIT_AS },
    { "RLIMIT_RSS"_s, RLIMIT_RSS },
    { "RLIMIT_NPROC"_s, RLIMIT_NPROC },
    { "RLIMIT_MEMLOCK"_s, RLIMIT_MEMLOCK },
#if OS(LINUX)
    { "RLIMIT_MSGQUEUE"_s, RLIMIT_MSGQUEUE },
    { "RLIMIT_NICE"_s, RLIMIT_NICE },
    { "RLIMIT_RTPRIO"_s, RLIMIT_RTPRIO },
    { "RLIMIT_RTTIME"_s, RLIMIT_RTTIME },
    { "RLIMIT_SIGPENDING"_s, RLIMIT_SIGPENDING },
#endif
    { "RUSAGE_SELF"_s, RUSAGE_SELF },
    { "RUSAGE_CHILDREN"_s, RUSAGE_CHILDREN },
#if OS(LINUX)
    { "RUSAGE_THREAD"_s, RUSAGE_THREAD },
#endif
};

void initializeResourceModule(JSGlobalObject* globalObject, PyModule* module)
{
    module->addFunctions(globalObject, resourceFunctions);
    addPosixConstants(globalObject, module, resourceConstants);
    module->add(globalObject, "RLIM_INFINITY"_s, makeInt(globalObject, static_cast<uint64_t>(RLIM_INFINITY)));
#ifdef RLIM_SAVED_CUR
    module->add(globalObject, "RLIM_SAVED_CUR"_s, makeInt(globalObject, static_cast<uint64_t>(RLIM_SAVED_CUR)));
#endif
#ifdef RLIM_SAVED_MAX
    module->add(globalObject, "RLIM_SAVED_MAX"_s, makeInt(globalObject, static_cast<uint64_t>(RLIM_SAVED_MAX)));
#endif
    module->add(globalObject, "error"_s, runtime(globalObject).OSError());
    module->add(globalObject, "struct_rusage"_s, posixRusageType(globalObject));
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // !OS(WINDOWS)
