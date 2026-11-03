/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include "JSCInlines.h"
#include "PyAbstract.h"
#include "PyDict.h"
#include "PyErrors.h"
#include "PyException.h"
#include "PyFunction.h"
#include "PyInternals.h"
#include "PyList.h"
#include "PyModule.h"
#include "PyNamespace.h"
#include "PyNumbers.h"
#include "PyStr.h"
#include "PyTuple.h"
#include <wtf/HexNumber.h>

namespace JSC {
namespace Python {

// The parts of sys that are about how CPython works inside. Programs, the standard library among them (bdb, test.support), expect
// them to be there, so they are, and say that there is nothing to see.

// ---- sys._jit: CPython's, which is not here ----

static JSValue jitReturnFalse(JSGlobalObject*) { return jsBoolean(false); }

static constexpr NativeDef jitFunctions[] = {
    PY_FUNCTION("is_active", jitReturnFalse, "()"),
    PY_FUNCTION("is_available", jitReturnFalse, "()"),
    PY_FUNCTION("is_enabled", jitReturnFalse, "()"),
};

// ---- Functions ----

// What is not a cell is never freed, having never been allocated.
static JSValue sysIsImmortal(JSGlobalObject*, JSValue object)
{
    return jsBoolean(!object.isCell());
}

static JSValue sysDebugMallocStats(JSGlobalObject*) { return jsUndefined(); }

static JSValue sysActivateStackTrampoline(JSGlobalObject* globalObject, JSValue)
{
    auto scope = DECLARE_THROW_SCOPE(getVM(globalObject));
    return raiseValueError(globalObject, scope, "perf trampoline not available"_s);
}

static JSValue sysDeactivateStackTrampoline(JSGlobalObject*) { return jsUndefined(); }

static JSValue sysRemoteExec(JSGlobalObject* globalObject, JSValue, JSValue)
{
    auto scope = DECLARE_THROW_SCOPE(getVM(globalObject));
    return raiseRuntimeError(globalObject, scope, "Remote debugging is not supported"_s);
}

static JSValue sysDumpTracelets(JSGlobalObject* globalObject, JSValue)
{
    Runtime& rt = runtime(globalObject);
    auto scope = DECLARE_THROW_SCOPE(rt.vm());
    return raise(globalObject, scope, rt.NotImplementedError(), "Executors are not available in this build"_s);
}

static constexpr NativeDef sysExtraFunctions[] = {
    PY_FUNCTION("_debugmallocstats", sysDebugMallocStats, "()"),
    PY_FUNCTION("_dump_tracelets", sysDumpTracelets, "(outpath)"),
    PY_FUNCTION("_is_immortal", sysIsImmortal, "(op, /)"),
    PY_FUNCTION("activate_stack_trampoline", sysActivateStackTrampoline, "(backend, /)"),
    PY_FUNCTION("deactivate_stack_trampoline", sysDeactivateStackTrampoline, "()"),
    PY_FUNCTION("remote_exec", sysRemoteExec, "(pid, script)"),
};

void addSysExtras(JSGlobalObject* globalObject, PyModule* sys)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    sys->addFunctions(globalObject, sysExtraFunctions);
    sys->add(globalObject, "monitoring"_s, makeMonitoringModule(globalObject));
    PyModule* jit = PyModule::create(globalObject, "sys._jit"_s);
    jit->addFunctions(globalObject, jitFunctions);
    sys->add(globalObject, "_jit"_s, jit);

    PyDict* abi = PyDict::create(globalObject);
    abi->set(vm, internString(vm, "pointer_bits"_s), jsNumber(static_cast<int>(sizeof(void*) * 8)));
    abi->set(vm, internString(vm, "free_threaded"_s), jsBoolean(false));
    abi->set(vm, internString(vm, "debug"_s), jsBoolean(false));
    abi->set(vm, internString(vm, "byteorder"_s), makeStr(vm, "little"_s));
    sys->add(globalObject, "abi_info"_s, makeSimpleNamespace(globalObject, abi));
}

} // namespace Python
} // namespace JSC
