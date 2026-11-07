/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyPosix.h"

#if !OS(WINDOWS)

#include "JSCInlines.h"
#include "PyStructSeq.h"
#include <dirent.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// The module _remote_debugging. In CPython it reads the memory of another CPython process, whose layout it knows. Nothing here is
// laid out so, in this process or in the other, so this is the module of a platform where that cannot be done: the records and
// constants that `asyncio.tools` and `profiling.sampling` import, and a RemoteUnwinder that says so.

static constexpr ASCIILiteral remoteFrameFields[] = { "filename"_s, "location"_s, "funcname"_s, "opcode"_s };
static constexpr ASCIILiteral remoteLocationFields[] = { "lineno"_s, "end_lineno"_s, "col_offset"_s, "end_col_offset"_s };
static constexpr ASCIILiteral remoteCoroFields[] = { "call_stack"_s, "task_name"_s };
static constexpr ASCIILiteral remoteTaskFields[] = { "task_id"_s, "task_name"_s, "coroutine_stack"_s, "awaited_by"_s };
static constexpr ASCIILiteral remoteThreadFields[] = { "thread_id"_s, "status"_s, "frame_info"_s };
static constexpr ASCIILiteral remoteInterpreterFields[] = { "interpreter_id"_s, "threads"_s };
static constexpr ASCIILiteral remoteAwaitedFields[] = { "thread_id"_s, "awaited_by"_s };
static constexpr ASCIILiteral remoteGCStatsFields[] = { "gen"_s, "iid"_s, "ts_start"_s, "ts_stop"_s, "collections"_s, "collected"_s, "uncollectable"_s, "candidates"_s, "heap_size"_s, "duration"_s };

static constexpr StructSeqDef remoteRecords[] = {
    { "_remote_debugging.FrameInfo"_s, remoteFrameFields, 4 },
    { "_remote_debugging.LocationInfo"_s, remoteLocationFields, 4 },
    { "_remote_debugging.CoroInfo"_s, remoteCoroFields, 2 },
    { "_remote_debugging.TaskInfo"_s, remoteTaskFields, 4 },
    { "_remote_debugging.ThreadInfo"_s, remoteThreadFields, 3 },
    { "_remote_debugging.InterpreterInfo"_s, remoteInterpreterFields, 2 },
    { "_remote_debugging.AwaitedInfo"_s, remoteAwaitedFields, 2 },
    { "_remote_debugging.GCStatsInfo"_s, remoteGCStatsFields, 10 },
};

static JSValue remoteUnwinderInit(JSGlobalObject* globalObject, JSValue, const CallArgs&)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raiseRuntimeError(globalObject, scope, "Reading the PyRuntime section is not supported on this platform"_s);
}

static constexpr NativeDef remoteUnwinderMethods[] = {
    PY_SLOT("__init__", remoteUnwinderInit, "($self, /, *args, **kwargs)"),
};

static constexpr TypeDef remoteUnwinderTypeDef { "_remote_debugging.RemoteUnwinder"_s, { }, remoteUnwinderMethods, { }, { } };

static JSValue remoteIsPythonProcess(JSGlobalObject* globalObject, JSValue pidValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    pid_t pid;
    if (failed(scope, posixToPid(globalObject, pidValue, pid)))
        return { };
    return jsBoolean(false);
}

static JSValue remoteZstdAvailable(JSGlobalObject*) { return jsBoolean(false); }

// The processes started by `pid`, and with `recursive` those started by them, level by level.
static JSValue remoteGetChildPids(JSGlobalObject* globalObject, JSValue pidValue, JSValue recursiveValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    pid_t pid;
    if (failed(scope, posixToPid(globalObject, pidValue, pid)))
        return { };
    bool recursive = !recursiveValue || isTrue(globalObject, recursiveValue);
    RETURN_IF_EXCEPTION(scope, { });
#if OS(LINUX)
    DIR* directory = opendir("/proc");
    if (!directory)
        return raiseOSError(globalObject, scope, errno, makeStr(vm, "/proc"_s));
    Vector<std::pair<pid_t, pid_t>> processes; // The process, who started it.
    while (struct dirent* entry = readdir(directory)) {
        if (!isASCIIDigit(entry->d_name[0]))
            continue;
        char name[64];
        snprintf(name, sizeof(name), "/proc/%s/stat", entry->d_name);
        FILE* file = fopen(name, "r");
        if (!file)
            continue;
        char text[1024];
        size_t length = fread(text, 1, sizeof(text) - 1, file);
        fclose(file);
        text[length] = 0;
        // "pid (name) state parent ...", and anything may be in the name.
        const char* afterName = strrchr(text, ')');
        int parent;
        if (afterName && sscanf(afterName + 1, " %*c %d", &parent) == 1)
            processes.append({ static_cast<pid_t>(atoi(entry->d_name)), static_cast<pid_t>(parent) });
    }
    closedir(directory);
    JSArray* result = newList(globalObject);
    Vector<pid_t> found { pid };
    for (size_t next = 0; next < found.size(); ++next) {
        if (next && !recursive)
            break;
        for (auto [process, parent] : processes) {
            if (parent != found[next])
                continue;
            found.append(process);
            listAppend(globalObject, result, jsNumber(process));
            RETURN_IF_EXCEPTION(scope, { });
        }
    }
    return result;
#else
    UNUSED_PARAM(recursive);
    return raiseNotImplementedError(globalObject, scope, "Subprocess enumeration not supported on this platform"_s);
#endif
}

static constexpr NativeDef remoteFunctions[] = {
    PY_FUNCTION("get_child_pids", remoteGetChildPids, "(pid, *, recursive=True)"),
    PY_FUNCTION("is_python_process", remoteIsPythonProcess, "(pid)"),
    PY_FUNCTION("zstd_available", remoteZstdAvailable, "()"),
};

void initializeRemoteDebuggingModule(JSGlobalObject* globalObject, PyModule* module)
{
    module->addFunctions(globalObject, remoteFunctions);
    for (auto& record : remoteRecords)
        module->add(globalObject, ASCIILiteral::fromLiteralUnsafe(record.name.characters() + strlen("_remote_debugging.")), structSeqTypeFor(globalObject, record));
    module->add(globalObject, "RemoteUnwinder"_s, runtime(globalObject).typeFor(remoteUnwinderTypeDef));
    module->add(globalObject, "PROCESS_VM_READV_SUPPORTED"_s, jsNumber(0));
    module->add(globalObject, "THREAD_STATUS_HAS_GIL"_s, jsNumber(1));
    module->add(globalObject, "THREAD_STATUS_ON_CPU"_s, jsNumber(2));
    module->add(globalObject, "THREAD_STATUS_UNKNOWN"_s, jsNumber(4));
    module->add(globalObject, "THREAD_STATUS_GIL_REQUESTED"_s, jsNumber(8));
    module->add(globalObject, "THREAD_STATUS_HAS_EXCEPTION"_s, jsNumber(16));
    module->add(globalObject, "THREAD_STATUS_MAIN_THREAD"_s, jsNumber(32));
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // !OS(WINDOWS)
