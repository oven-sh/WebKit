/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include "JSCInlines.h"
#include "JSWeakMapInlines.h"
#include "PyAbstract.h"
#include "PyCodeInfo.h"
#include "PyErrors.h"
#include "PyInternals.h"
#include "PyList.h"
#include "PyModule.h"
#include "PyMonitoring.h"
#include "PyNative.h"
#include "PyNumbers.h"
#include "PyPosix.h"
#include "PyStr.h"
#include "PyStructSeq.h"
#include "TopExceptionScope.h"
#include <wtf/MonotonicTime.h>

namespace JSC {
namespace Python {

// The modules _lsprof, _tracemalloc and _sysconfig. (Modules/_lsprof.c, _tracemalloc.c and _sysconfig.c in CPython.)
//
// _tracemalloc measures no memory: see there.

// ---- _lsprof ----
//
// A Profiler is a tool of sys.monitoring: it asks to be told when functions begin and end, and looks at the clock.

namespace {

struct ProfilerCounts {
    int64_t totalTime { 0 }; // Not counting what a call spends in calls of the same function further down.
    int64_t inlineTime { 0 }; // Not counting what it spends in other calls that are measured.
    int64_t callCount { 0 };
    int64_t recursiveCallCount { 0 };
    int64_t recursionLevel { 0 };
};

// A function.
struct ProfilerEntry : ProfilerCounts {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(ProfilerEntry);
    unsigned userObject { 0 }; // What stands for it in the statistics: an index in the list the profiler keeps.
    // The calls it made, by the function called.
    UncheckedKeyHashMap<ProfilerEntry*, ProfilerCounts> calls;
    Vector<ProfilerEntry*> callees; // In the order they were first called.
};

// A call that is under way.
struct ProfilerContext {
    int64_t start;
    int64_t timeInCalls;
    ProfilerEntry* entry;
};

struct ProfilerState {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(ProfilerState);
    // By the code object, or the definition of the built-in function.
    UncheckedKeyHashMap<const void*, std::unique_ptr<ProfilerEntry>> entries;
    Vector<ProfilerEntry*> entriesInOrder;
    Vector<ProfilerContext> contexts;
    bool isEnabled { false };
    bool subcalls { true };
    bool builtins { true };
    unsigned activeTimers { 0 };
    double externalTimerUnit { 0 };
};

enum ProfilerField : unsigned { ProfilerTimer, ProfilerUserObjects, ProfilerFieldCount };
constexpr int profilerToolID = 2; // sys.monitoring.PROFILER_ID

ProfilerState& profilerState(JSValue self) { return *std::bit_cast<ProfilerState*>(static_cast<uintptr_t>(PyRecord::from(self)->number)); }

} // namespace

static JSValue profilerNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs&)
{
    VM& vm = getVM(globalObject);
    PyRecord* self = PyRecord::create(globalObject, PyType::from(cls), ProfilerFieldCount);
    self->set(vm, ProfilerUserObjects, newList(globalObject));
    auto* state = new ProfilerState;
    self->number = static_cast<int64_t>(std::bit_cast<uintptr_t>(state));
    vm.heap.addFinalizerForAnyThread(self, [state](JSCell*) {
        delete state;
    });
    return self;
}

static JSValue profilerInit(JSGlobalObject* globalObject, JSValue self, JSValue timer, JSValue timeUnit, JSValue subcalls, JSValue builtins)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    ProfilerState& state = profilerState(self);
    if (state.activeTimers)
        return raise(globalObject, scope, runtime(globalObject).RuntimeError(), "cannot initialize profiler in external timer"_s);
    double unit = 0;
    if (timeUnit) {
        unit = toDouble(globalObject, timeUnit);
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (subcalls) {
        state.subcalls = isTrue(globalObject, subcalls);
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (builtins) {
        state.builtins = isTrue(globalObject, builtins);
        RETURN_IF_EXCEPTION(scope, { });
    }
    state.externalTimerUnit = unit;
    PyRecord::from(self)->set(vm, ProfilerTimer, timer && !isNone(timer) ? timer : JSValue());
    return jsUndefined();
}

static int64_t profilerExternalTime(JSGlobalObject* globalObject, JSValue self, JSValue timer)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    ProfilerState& state = profilerState(self);
    int64_t result = 0;
    JSValue value = call(globalObject, timer);
    if (value) {
        if (state.externalTimerUnit > 0) {
            // A whole number of units.
            JSValue number = toIndex(globalObject, value);
            if (number) {
                if (auto small = tryInt64(number))
                    result = *small;
                else {
                    auto throwScope = DECLARE_THROW_SCOPE(vm);
                    raise(globalObject, throwScope, runtime(globalObject).OverflowError(), "Python int too large to convert to C int64_t"_s);
                }
            }
        } else {
            double seconds = toDouble(globalObject, value);
            if (!scope.exception())
                result = static_cast<int64_t>(std::floor(seconds * 1e9));
        }
    }
    if (scope.exception()) {
        reportUnraisable(globalObject, makeString("Exception ignored while calling _lsprof timer "_s, reprForMessage(globalObject, timer)), { });
        return 0;
    }
    return result;
}

static int64_t profilerTime(JSGlobalObject* globalObject, JSValue self)
{
    if (JSValue timer = PyRecord::from(self)->get(ProfilerTimer)) {
        // It may run any code. What it runs is not measured.
        ProfilerState& state = profilerState(self);
        ++state.activeTimers;
        int64_t result = profilerExternalTime(globalObject, self, timer);
        --state.activeTimers;
        return result;
    }
    return static_cast<int64_t>(MonotonicTime::now().secondsSinceEpoch().nanoseconds());
}

// A built-in function is shown as a str: to keep the object a method is bound to would be to keep it alive.
static JSValue profilerUserObject(JSGlobalObject* globalObject, JSValue object)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    Runtime& rt = runtime(globalObject);
    auto* native = PyNativeFunction::tryFrom(object);
    if (!native)
        return object;
    ASCIILiteral name = native->def().name;
    if (JSValue self = native->self(); self && !PyModule::is(self)) {
        if (JSValue method = rt.typeOf(self)->lookup(internString(vm, name))) {
            JSString* text = repr(globalObject, method);
            if (!failed(scope, text))
                return text;
            scope.clearException();
        }
    }
    if (JSValue owner = native->owner(); owner && isStr(owner))
        return makeStr(globalObject, makeString("<built-in method "_s, stringOf(strValue(owner)), '.', name, '>'));
    return makeStr(globalObject, makeString("<built-in method "_s, name, '>'));
}

static void profilerEnterCall(JSGlobalObject* globalObject, JSValue self, const void* key, JSValue object)
{
    auto scope = DECLARE_THROW_SCOPE(getVM(globalObject));
    ProfilerState& state = profilerState(self);
    // (A program can call the callbacks itself, with anything for the code.)
    if (state.activeTimers || !key)
        return;
    ProfilerEntry* entry;
    if (auto it = state.entries.find(key); it != state.entries.end())
        entry = it->value.get();
    else {
        JSArray* userObjects = asArray(PyRecord::from(self)->get(ProfilerUserObjects));
        JSValue userObject = profilerUserObject(globalObject, object);
        auto owned = makeUnique<ProfilerEntry>();
        entry = owned.get();
        entry->userObject = listLength(userObjects);
        listAppend(globalObject, userObjects, userObject);
        RETURN_IF_EXCEPTION(scope, void());
        // What the key is the address of stays where it is.
        if (userObject != object && !PyNativeFunction::is(object)) {
            listAppend(globalObject, userObjects, object);
            RETURN_IF_EXCEPTION(scope, void());
        }
        state.entries.add(key, WTF::move(owned));
        state.entriesInOrder.append(entry);
    }
    ++entry->recursionLevel;
    if (state.subcalls && !state.contexts.isEmpty()) {
        ProfilerEntry* caller = state.contexts.last().entry;
        auto result = caller->calls.add(entry, ProfilerCounts { });
        if (result.isNewEntry)
            caller->callees.append(entry);
        ++result.iterator->value.recursionLevel;
    }
    state.contexts.append({ 0, 0, entry });
    size_t index = state.contexts.size() - 1;
    int64_t now = profilerTime(globalObject, self);
    if (index < state.contexts.size())
        state.contexts[index].start = now;
}

static void profilerStop(JSGlobalObject* globalObject, JSValue self)
{
    ProfilerState& state = profilerState(self);
    int64_t now = profilerTime(globalObject, self);
    if (state.contexts.isEmpty())
        return;
    ProfilerContext context = state.contexts.takeLast();
    ProfilerEntry* entry = context.entry;
    int64_t total = now - context.start;
    int64_t inlineTime = total - context.timeInCalls;
    auto count = [&](ProfilerCounts& counts) {
        if (!--counts.recursionLevel)
            counts.totalTime += total;
        else
            ++counts.recursiveCallCount;
        counts.inlineTime += inlineTime;
        ++counts.callCount;
    };
    count(*entry);
    if (state.contexts.isEmpty())
        return;
    state.contexts.last().timeInCalls += total;
    if (!state.subcalls)
        return;
    auto& calls = state.contexts.last().entry->calls;
    if (auto it = calls.find(entry); it != calls.end())
        count(it->value);
}

static void profilerLeaveCall(JSGlobalObject* globalObject, JSValue self, const void* key)
{
    ProfilerState& state = profilerState(self);
    if (state.activeTimers || state.contexts.isEmpty() || !key)
        return;
    if (state.entries.contains(key))
        profilerStop(globalObject, self);
    else
        state.contexts.removeLast();
}

static JSValue profilerPyStart(JSGlobalObject* globalObject, JSValue self, JSValue code, JSValue)
{
    auto scope = DECLARE_THROW_SCOPE(getVM(globalObject));
    profilerEnterCall(globalObject, self, code.isCell() ? code.asCell() : nullptr, code);
    RETURN_IF_EXCEPTION(scope, { });
    return jsUndefined();
}

static JSValue profilerPyThrow(JSGlobalObject* globalObject, JSValue self, JSValue code, JSValue, JSValue)
{
    auto scope = DECLARE_THROW_SCOPE(getVM(globalObject));
    profilerEnterCall(globalObject, self, code.isCell() ? code.asCell() : nullptr, code);
    RETURN_IF_EXCEPTION(scope, { });
    return jsUndefined();
}

static JSValue profilerPyReturn(JSGlobalObject* globalObject, JSValue self, JSValue code, JSValue, JSValue)
{
    profilerLeaveCall(globalObject, self, code.isCell() ? code.asCell() : nullptr);
    return jsUndefined();
}

// The built-in function that is being called, bound to the object if it is a method. Null: it is not one.
static PyNativeFunction* profilerBuiltin(JSGlobalObject* globalObject, JSValue callable, JSValue first)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    Runtime& rt = runtime(globalObject);
    auto* native = PyNativeFunction::tryFrom(callable);
    if (!native)
        return nullptr;
    switch (native->kind()) {
    case NativeKind::Function:
    case NativeKind::StaticMethod:
    case NativeKind::New:
        return native;
    case NativeKind::Method: {
        if (first == rt.monitoring().missing())
            return nullptr;
        JSValue bound = bindDescriptor(globalObject, callable, first, rt.typeOf(first));
        if (failed(scope, bound)) {
            scope.clearException();
            return nullptr;
        }
        return PyNativeFunction::tryFrom(bound);
    }
    case NativeKind::SlotWrapper:
    case NativeKind::ClassMethod:
        return nullptr;
    }
    return nullptr;
}

static JSValue profilerCCall(JSGlobalObject* globalObject, JSValue self, JSValue, JSValue, JSValue callable, JSValue first)
{
    auto scope = DECLARE_THROW_SCOPE(getVM(globalObject));
    if (profilerState(self).builtins) {
        if (auto* native = profilerBuiltin(globalObject, callable, first)) {
            profilerEnterCall(globalObject, self, &native->def(), native);
            RETURN_IF_EXCEPTION(scope, { });
        }
    }
    return jsUndefined();
}

static JSValue profilerCReturn(JSGlobalObject* globalObject, JSValue self, JSValue, JSValue, JSValue callable, JSValue first)
{
    if (profilerState(self).builtins) {
        if (auto* native = profilerBuiltin(globalObject, callable, first))
            profilerLeaveCall(globalObject, self, &native->def());
    }
    return jsUndefined();
}

static constexpr struct {
    unsigned event;
    ASCIILiteral method;
} profilerCallbacks[] = {
    { Event::PyStart, "_pystart_callback"_s },
    { Event::PyResume, "_pystart_callback"_s },
    { Event::PyThrow, "_pythrow_callback"_s },
    { Event::PyReturn, "_pyreturn_callback"_s },
    { Event::PyYield, "_pyreturn_callback"_s },
    { Event::PyUnwind, "_pyreturn_callback"_s },
    { Event::Call, "_ccall_callback"_s },
    { Event::CReturn, "_creturn_callback"_s },
    { Event::CRaise, "_creturn_callback"_s },
};

static JSValue sysMonitoring(JSGlobalObject* globalObject)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    Runtime& rt = runtime(globalObject);
    if (JSValue monitoring = rt.sysDict()->get(internString(vm, "monitoring"_s)))
        return monitoring;
    return raise(globalObject, scope, rt.RuntimeError(), "lost sys.monitoring"_s);
}

static JSValue profilerEnable(JSGlobalObject* globalObject, JSValue self, JSValue subcalls, JSValue builtins)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    ProfilerState& state = profilerState(self);
    bool wantsSubcalls = !subcalls || isTrue(globalObject, subcalls);
    RETURN_IF_EXCEPTION(scope, { });
    bool wantsBuiltins = !builtins || isTrue(globalObject, builtins);
    RETURN_IF_EXCEPTION(scope, { });
    state.subcalls = wantsSubcalls;
    state.builtins = wantsBuiltins;

    JSValue monitoring = sysMonitoring(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    callMethod(globalObject, monitoring, internString(vm, "use_tool_id"_s), jsNumber(profilerToolID), internString(vm, "cProfile"_s));
    RETURN_IF_EXCEPTION(scope, { });
    int events = 0;
    for (auto& callback : profilerCallbacks) {
        JSValue method = getAttr(globalObject, self, internString(vm, callback.method));
        RETURN_IF_EXCEPTION(scope, { });
        callMethod(globalObject, monitoring, internString(vm, "register_callback"_s), jsNumber(profilerToolID), jsNumber(1 << callback.event), method);
        RETURN_IF_EXCEPTION(scope, { });
        events |= 1 << callback.event;
    }
    callMethod(globalObject, monitoring, internString(vm, "set_events"_s), jsNumber(profilerToolID), jsNumber(events));
    RETURN_IF_EXCEPTION(scope, { });
    state.isEnabled = true;
    return jsUndefined();
}

static JSValue profilerDisable(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    ProfilerState& state = profilerState(self);
    if (state.activeTimers)
        return raise(globalObject, scope, runtime(globalObject).RuntimeError(), "cannot disable profiler in external timer"_s);
    if (!state.isEnabled)
        return jsUndefined();
    JSValue monitoring = sysMonitoring(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    for (auto& callback : profilerCallbacks) {
        callMethod(globalObject, monitoring, internString(vm, "register_callback"_s), jsNumber(profilerToolID), jsNumber(1 << callback.event), jsUndefined());
        RETURN_IF_EXCEPTION(scope, { });
    }
    callMethod(globalObject, monitoring, internString(vm, "set_events"_s), jsNumber(profilerToolID), jsNumber(0));
    RETURN_IF_EXCEPTION(scope, { });
    callMethod(globalObject, monitoring, internString(vm, "free_tool_id"_s), jsNumber(profilerToolID));
    RETURN_IF_EXCEPTION(scope, { });
    state.isEnabled = false;
    // The calls that are still under way end here.
    while (!state.contexts.isEmpty())
        profilerStop(globalObject, self);
    return jsUndefined();
}

static JSValue profilerClear(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    ProfilerState& state = profilerState(self);
    if (state.activeTimers)
        return raise(globalObject, scope, runtime(globalObject).RuntimeError(), "cannot clear profiler in external timer"_s);
    state.contexts.clear();
    state.entriesInOrder.clear();
    state.entries.clear();
    PyRecord::from(self)->set(vm, ProfilerUserObjects, newList(globalObject));
    return jsUndefined();
}

static constexpr ASCIILiteral profilerEntryFields[] = { "code"_s, "callcount"_s, "reccallcount"_s, "totaltime"_s, "inlinetime"_s, "calls"_s };
static constexpr StructSeqDef profilerEntryDef { "_lsprof.profiler_entry"_s, profilerEntryFields, 6 };
static constexpr StructSeqDef profilerSubentryDef { "_lsprof.profiler_subentry"_s, std::span { profilerEntryFields }.first(5), 5 };

static JSValue profilerGetStats(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (args.totalCount())
        return raiseTypeError(globalObject, scope, "getstats() takes no arguments"_s);
    ProfilerState& state = profilerState(self);
    JSArray* userObjects = asArray(PyRecord::from(self)->get(ProfilerUserObjects));
    double factor = PyRecord::from(self)->get(ProfilerTimer) && state.externalTimerUnit ? state.externalTimerUnit : 1e-9;
    PyType* entryType = cachedStructSeqType(globalObject, profilerEntryDef);
    PyType* subentryType = cachedStructSeqType(globalObject, profilerSubentryDef);
    JSArray* result = newList(globalObject);
    for (ProfilerEntry* entry : state.entriesInOrder) {
        if (!entry->callCount)
            continue;
        JSValue calls = jsUndefined();
        if (!entry->callees.isEmpty()) {
            JSArray* list = newList(globalObject);
            calls = list;
            for (ProfilerEntry* callee : entry->callees) {
                const ProfilerCounts& counts = entry->calls.find(callee)->value;
                listAppend(globalObject, list, makeStructSeq(globalObject, subentryType, { listGet(userObjects, callee->userObject), makeInt(globalObject, counts.callCount),
                    makeInt(globalObject, counts.recursiveCallCount), makeFloat(globalObject, factor * counts.totalTime), makeFloat(globalObject, factor * counts.inlineTime) }));
                RETURN_IF_EXCEPTION(scope, { });
            }
        }
        listAppend(globalObject, result, makeStructSeq(globalObject, entryType, { listGet(userObjects, entry->userObject), makeInt(globalObject, entry->callCount),
            makeInt(globalObject, entry->recursiveCallCount), makeFloat(globalObject, factor * entry->totalTime), makeFloat(globalObject, factor * entry->inlineTime), calls }));
        RETURN_IF_EXCEPTION(scope, { });
    }
    return result;
}

static constexpr NativeDef profilerMethods[] = {
    PY_NEW(profilerNew, "($type, *args, **kwargs)"),
    PY_SLOT("__init__", profilerInit, "($self, /, timer=None, timeunit=0.0, subcalls=True, builtins=True)"),
    PY_METHOD("enable", profilerEnable, "($self, /, subcalls=True, builtins=True)"),
    PY_METHOD("disable", profilerDisable, "($self, /)"),
    PY_METHOD("clear", profilerClear, "($self, /)"),
    PY_METHOD("getstats", profilerGetStats, "!($self, /)"),
    PY_METHOD("_pystart_callback", profilerPyStart, "($self, code, instruction_offset, /)"),
    PY_METHOD("_pythrow_callback", profilerPyThrow, "($self, code, instruction_offset, exception, /)"),
    PY_METHOD("_pyreturn_callback", profilerPyReturn, "($self, code, instruction_offset, retval, /)"),
    PY_METHOD("_ccall_callback", profilerCCall, "($self, code, instruction_offset, callable, self_arg, /)"),
    PY_METHOD("_creturn_callback", profilerCReturn, "($self, code, instruction_offset, callable, self_arg, /)"),
};

static constexpr TypeDef profilerTypeDef { "_lsprof.Profiler"_s, { TypeFlag::Base, TypeFlag::OwnLayout }, profilerMethods, { }, { } };

void initializeLsprofModule(JSGlobalObject* globalObject, PyModule* module)
{
    module->add(globalObject, "Profiler"_s, runtime(globalObject).typeFor(profilerTypeDef));
    module->add(globalObject, "profiler_entry"_s, cachedStructSeqType(globalObject, profilerEntryDef));
    module->add(globalObject, "profiler_subentry"_s, cachedStructSeqType(globalObject, profilerSubentryDef));
}

// ---- _tracemalloc ----
//
// CPython's hooks into its allocators, and knows for each block of memory how large it is and which lines of Python asked for it.
// The allocators here are those of the engine, with no place for a hook: most objects are made by a few instructions inline.
// So no memory is measured: there are no traces, and the traced memory is 0.
//
// What there is: where the objects were made that have a finalizer (files, sockets, instances of classes with __del__), which
// register with the runtime when they are made. That is what a ResourceWarning shows ("Object allocated at"), and they are the
// objects one can be about.

static JSWeakMap* tracemallocTracebacks(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    if (JSValue map = rt.slot(Runtime::Slot::AllocationTracebacks))
        return uncheckedDowncast<JSWeakMap>(map);
    JSWeakMap* map = JSWeakMap::create(rt.vm(), globalObject->weakMapStructure());
    rt.setSlot(Runtime::Slot::AllocationTracebacks, map);
    return map;
}

void noteWhereObjectWasMade(JSGlobalObject* globalObject, JSCell* cell)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto* object = cell->getObject();
    if (!object || tracemallocTracebacks(globalObject)->has(object))
        return;
    // The innermost frame first.
    Vector<std::pair<String, int>, 8> frames;
    forEachPythonFrameWithoutAllocating(vm.topCallFrame, vm.topEntryFrame, [&](const CodeInfo& info, int line) {
        frames.append({ info.filename, line });
        return frames.size() < rt.allocationTracebackLimit;
    });
    if (frames.isEmpty())
        frames.append({ "<unknown>"_s, 0 });
    MarkedArgumentBuffer entries;
    for (auto& [filename, line] : frames)
        entries.append(PyTuple::create(globalObject, { makeStr(vm, filename), jsNumber(line) }));
    Vector<JSValue, 8> values;
    for (unsigned i = 0; i < entries.size(); ++i)
        values.append(entries.at(i));
    tracemallocTracebacks(globalObject)->set(vm, object, PyTuple::create(globalObject, values.span()));
}

static JSValue tracemallocStart(JSGlobalObject* globalObject, JSValue frameCountValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    constexpr int maxFrameCount = 65535;
    int frameCount = 1;
    if (frameCountValue) {
        frameCount = toCInt(globalObject, frameCountValue);
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (frameCount < 1 || frameCount > maxFrameCount)
        return raiseValueError(globalObject, scope, "the number of frames must be in range [1; "_s, maxFrameCount, ']');
    // Once it is on, starting it again changes nothing.
    Runtime& rt = runtime(globalObject);
    if (!rt.allocationTracebackLimit)
        rt.allocationTracebackLimit = static_cast<unsigned>(frameCount);
    return jsUndefined();
}

static JSValue tracemallocClearTraces(JSGlobalObject* globalObject)
{
    runtime(globalObject).setSlot(Runtime::Slot::AllocationTracebacks, JSValue());
    return jsUndefined();
}

static JSValue tracemallocStop(JSGlobalObject* globalObject)
{
    runtime(globalObject).allocationTracebackLimit = 0;
    return tracemallocClearTraces(globalObject);
}

static JSValue tracemallocIsTracing(JSGlobalObject* globalObject) { return jsBoolean(!!runtime(globalObject).allocationTracebackLimit); }
static JSValue tracemallocGetTracebackLimit(JSGlobalObject* globalObject) { return jsNumber(std::max(1u, runtime(globalObject).allocationTracebackLimit)); }

static JSValue tracemallocGetObjectTraceback(JSGlobalObject* globalObject, JSValue object)
{
    Runtime& rt = runtime(globalObject);
    if (!rt.allocationTracebackLimit || !object.isObject() || !rt.slot(Runtime::Slot::AllocationTracebacks))
        return jsUndefined();
    JSValue traceback = tracemallocTracebacks(globalObject)->get(asObject(object));
    return traceback ? traceback : jsUndefined();
}

static JSValue tracemallocNone(JSGlobalObject*) { return jsUndefined(); }
static JSValue tracemallocZero(JSGlobalObject*) { return jsNumber(0); }
static JSValue tracemallocNoTraces(JSGlobalObject* globalObject) { return newList(globalObject); }
static JSValue tracemallocNoMemory(JSGlobalObject* globalObject) { return PyTuple::create(globalObject, { jsNumber(0), jsNumber(0) }); }

static constexpr NativeDef tracemallocFunctions[] = {
    PY_FUNCTION("is_tracing", tracemallocIsTracing, "()"),
    PY_FUNCTION("clear_traces", tracemallocClearTraces, "()"),
    PY_FUNCTION("_get_traces", tracemallocNoTraces, "()"),
    PY_FUNCTION("_get_object_traceback", tracemallocGetObjectTraceback, "(obj, /)"),
    PY_FUNCTION("start", tracemallocStart, "(nframe=1, /)"),
    PY_FUNCTION("stop", tracemallocStop, "()"),
    PY_FUNCTION("get_traceback_limit", tracemallocGetTracebackLimit, "()"),
    PY_FUNCTION("get_tracemalloc_memory", tracemallocZero, "()"),
    PY_FUNCTION("get_traced_memory", tracemallocNoMemory, "()"),
    PY_FUNCTION("reset_peak", tracemallocNone, "()"),
};

void initializeTracemallocModule(JSGlobalObject* globalObject, PyModule* module)
{
    module->addFunctions(globalObject, tracemallocFunctions);
}

// ---- _sysconfig ----

static JSValue sysconfigConfigVars(JSGlobalObject* globalObject)
{
    VM& vm = getVM(globalObject);
    PyDict* variables = PyDict::create(globalObject);
    variables->set(vm, internString(vm, "Py_GIL_DISABLED"_s), jsNumber(0));
    variables->set(vm, internString(vm, "Py_DEBUG"_s), jsNumber(0));
    return variables;
}

static constexpr NativeDef sysconfigFunctions[] = {
    PY_FUNCTION("config_vars", sysconfigConfigVars, "()"),
};

void initializeSysconfigModule(JSGlobalObject* globalObject, PyModule* module)
{
    module->addFunctions(globalObject, sysconfigFunctions);
}

} // namespace Python
} // namespace JSC
