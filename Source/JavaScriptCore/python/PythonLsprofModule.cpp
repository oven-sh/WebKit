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
#include "PythonBuiltins.h"

#include "JSCInlines.h"
#include "PyNativeFunction.h"
#include "PyObjects.h"
#include "PyRealm.h"
#include "PyStateObject.h"
#include "PyType.h"
#include "PythonIO.h"
#include "PythonMonitoring.h"
#include "PythonNumbers.h"
#include "PythonOperations.h"
#include "PythonSequences.h"
#include "PythonTime.h"
#include <wtf/HashMap.h>

// The module _lsprof, which cProfile is written over: Modules/_lsprof.c of CPython. It is told of calls and returns by sys.monitoring, like anything else that asks.

namespace JSC { namespace Python {

namespace {

struct Entry;

// A function as it is called from another
struct SubEntry {
    WTF_MAKE_STRUCT_TZONE_ALLOCATED(SubEntry);
    Entry* callee;
    int64_t totalTime { 0 };
    int64_t inlineTime { 0 };
    int64_t callCount { 0 };
    int64_t recursiveCallCount { 0 };
    int64_t recursionLevel { 0 };
};

// A function
struct Entry {
    WTF_MAKE_STRUCT_TZONE_ALLOCATED(Entry);
    JSValue userObject; // A code object, or a str that describes what is written in C++. ProfilerState::kept has it.
    int64_t totalTime { 0 };
    int64_t inlineTime { 0 }; // Not in what it calls
    int64_t callCount { 0 };
    int64_t recursiveCallCount { 0 };
    int64_t recursionLevel { 0 };
    // CPython has these in a tree that is put in a different order as it is looked in, and gives them in the order of the tree, which goes by where things are in memory. Here they are given in the order in which they came.
    UncheckedKeyHashMap<Entry*, SubEntry*> calls;
    Vector<std::unique_ptr<SubEntry>> callsInOrder;
};

// A call that has not returned
struct Context {
    WTF_MAKE_STRUCT_TZONE_ALLOCATED(Context);
    int64_t startTime;
    int64_t timeInCalls;
    Context* previous;
    Entry* entry;
};

WTF_MAKE_STRUCT_TZONE_ALLOCATED_IMPL(SubEntry);
WTF_MAKE_STRUCT_TZONE_ALLOCATED_IMPL(Entry);
WTF_MAKE_STRUCT_TZONE_ALLOCATED_IMPL(Context);

struct LsprofModuleState final : NativeState {
    PYTHON_NATIVE_STATE(LsprofModuleState);
    WriteBarrier<PyType> profiler;
    WriteBarrier<PyType> entry;
    WriteBarrier<PyType> subentry;
};

template<typename Visitor> void LsprofModuleState::visit(Visitor& visitor)
{
    visitor.append(profiler);
    visitor.append(entry);
    visitor.append(subentry);
}

// A _lsprof.Profiler
struct ProfilerState final : NativeState {
    PYTHON_NATIVE_STATE(ProfilerState);
    ~ProfilerState() final
    {
        freeAll(currentContext);
        freeAll(freeContexts);
    }

    static void freeAll(Context*& list)
    {
        while (list)
            delete std::exchange(list, list->previous);
    }

    // What an entry is found by is a cell: a code object, or what stands here for a PyMethodDef. It is not to be another's while it is gone by, so `kept` has that too.
    UncheckedKeyHashMap<JSCell*, Entry*> entries;
    Vector<std::unique_ptr<Entry>> entriesInOrder;
    Context* currentContext { nullptr };
    Context* freeContexts { nullptr };
    WriteBarrier<Unknown> kept; // A list. The collector goes through this while the program is running, so it is told of nothing that is in what grows.
    WriteBarrier<Unknown> externalTimer;
    WriteBarrier<Unknown> missing; // sys.monitoring.MISSING
    double externalTimerUnit { 0 };
    int toolID { 0 };
    bool isEnabled { false };
    bool recordsSubcalls { false };
    bool recordsBuiltins { false };
    bool isInExternalTimer { false };
};

template<typename Visitor> void ProfilerState::visit(Visitor& visitor)
{
    visitor.append(kept);
    visitor.append(externalTimer);
    visitor.append(missing);
}

constexpr int profilerToolID = 2; // PY_MONITORING_PROFILER_ID

// CallExternalTimer()
int64_t callExternalTimer(JSGlobalObject* globalObject, ProfilerState& self)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    JSValue timer = self.externalTimer.get();
    // It can do anything at all, and some things it had better not.
    self.isInExternalTimer = true;
    JSValue result = call(globalObject, timer);
    self.isInExternalTimer = false;
    std::optional<int64_t> time;
    if (!scope.exception()) {
        // An int, of units that getstats() makes seconds of. Or a number of seconds.
        time = self.externalTimerUnit > 0.0 ? timeFromNanosecondsObject(globalObject, result) : timeFromSecondsObject(globalObject, result, TimeRounding::Floor);
    }
    if (scope.exception()) [[unlikely]] {
        reportUnraisableShowing(globalObject, "Exception ignored while calling _lsprof timer"_s, timer);
        return 0;
    }
    return *time;
}

int64_t callTimer(JSGlobalObject* globalObject, ProfilerState& self)
{
    return self.externalTimer ? callExternalTimer(globalObject, self) : monotonicClockRaw();
}

// A PyCFunctionObject: what is written in C++, with what it is bound to.
struct NativeCallable {
    explicit operator bool() const { return !!definition; }
    PyNativeFunction* definition { nullptr }; // For m_ml, which all that are made from one definition have in common
    JSValue object; // The builtin_function_or_method itself
};

// PyCFunction_Check(), and what there is to one
NativeCallable nativeCallableIn(JSGlobalObject* globalObject, JSValue value)
{
    if (typeOf(globalObject, value) != globalObject->pyRealm()->type(BuiltinType::BuiltinFunction))
        return { };
    if (auto* function = dynamicDowncast<PyNativeFunction>(value))
        return { function, value };
    if (auto* bound = dynamicDowncast<PyBoundMethod>(value)) {
        if (auto* function = dynamicDowncast<PyNativeFunction>(bound->function()))
            return { function, value };
    }
    return { };
}

// get_cfunc_from_callable(). Nothing if it is not written in C++. It does not raise.
NativeCallable nativeCallableFrom(JSGlobalObject* globalObject, ProfilerState& self, JSValue callable, JSValue selfArgument)
{
    if (auto native = nativeCallableIn(globalObject, callable))
        return native;
    if (typeOf(globalObject, callable) != globalObject->pyRealm()->type(BuiltinType::MethodDescriptor))
        return { };
    // list.append(l, x) has always been told of as l.append(x) is.
    if (selfArgument == self.missing.get())
        return { };
    auto* descriptor = dynamicDowncast<PyNativeFunction>(callable);
    // descr_check()
    if (!descriptor || !descriptor->owner() || !isInstance(globalObject, selfArgument, asType(descriptor->owner())))
        return { };
    return { descriptor, PyBoundMethod::create(globalObject, callable, selfArgument) };
}

// normalizeUserObj(): in place of what is written in C++, a str that describes it, so as not to keep what it is bound to. Empty if it raised.
JSValue normalizeUserObject(JSGlobalObject* globalObject, JSValue object)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& names = vm.pythonNames();
    auto native = nativeCallableIn(globalObject, object);
    if (!native)
        return object;
    JSValue boundTo = getAttribute(globalObject, object, names.dunder_self);
    RETURN_IF_EXCEPTION(scope, { });
    // This goes by m_self and not by __self__, and what a static method has there is its class, though it is not given it.
    if (native.definition->kind() == PyNativeFunction::Kind::StaticMethod && native.definition->owner())
        boundTo = native.definition->owner();
    JSValue module = getAttribute(globalObject, object, names.dunder_module);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue nameObject = getAttribute(globalObject, object, names.dunder_name);
    RETURN_IF_EXCEPTION(scope, { });
    String name = str(globalObject, nameObject);
    RETURN_IF_EXCEPTION(scope, { });
    bool hasModuleName = !!stringIn(module);
    String moduleName;
    if (hasModuleName) {
        moduleName = str(globalObject, module);
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (isNone(boundTo)) {
        if (hasModuleName && moduleName != "builtins"_s)
            return jsString(vm, concatenate('<', moduleName, '.', name, '>'));
        return jsString(vm, concatenate('<', name, '>'));
    }
    // repr(getattr(type(__self__), __name__)), if that can be had
    if (JSValue found = typeOf(globalObject, boundTo)->lookup(vm, Identifier::fromString(vm, name))) {
        String shown = repr(globalObject, found);
        if (!scope.exception())
            return jsString(vm, shown);
        if (!scope.tryClearException())
            return { };
    }
    if (hasModuleName)
        return jsString(vm, concatenate("<built-in method "_s, moduleName, '.', name, '>'));
    return jsString(vm, concatenate("<built-in method "_s, name, '>'));
}

// newProfilerEntry(). Null if there is no describing it, which is passed over.
Entry* newEntry(JSGlobalObject* globalObject, JSObject* owner, ProfilerState& self, JSCell* key, JSValue userObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    userObject = normalizeUserObject(globalObject, userObject);
    if (scope.exception()) [[unlikely]] {
        (void)scope.tryClearException();
        return nullptr;
    }
    if (!self.kept) {
        MarkedArgumentBuffer nothing;
        self.kept.set(vm, owner, newList(globalObject, nothing));
    }
    JSArray* kept = asList(self.kept.get());
    listAppend(globalObject, kept, key);
    RETURN_IF_EXCEPTION(scope, nullptr);
    listAppend(globalObject, kept, userObject);
    RETURN_IF_EXCEPTION(scope, nullptr);
    auto entry = makeUnique<Entry>();
    entry->userObject = userObject;
    Entry* result = entry.get();
    self.entries.add(key, result);
    self.entriesInOrder.append(WTF::move(entry));
    return result;
}

SubEntry* subEntryOf(Entry& caller, Entry& callee, bool makesOne)
{
    if (SubEntry* found = caller.calls.get(&callee))
        return found;
    if (!makesOne)
        return nullptr;
    auto made = makeUnique<SubEntry>();
    made->callee = &callee;
    SubEntry* result = made.get();
    caller.calls.add(&callee, result);
    caller.callsInOrder.append(WTF::move(made));
    return result;
}

// clearEntries()
void clearEntries(ProfilerState& self)
{
    self.entries.clear();
    self.entriesInOrder.clear();
    self.kept.clear();
    // What has not returned has an entry that is no more.
    ProfilerState::freeAll(self.currentContext);
    ProfilerState::freeAll(self.freeContexts);
}

// Stop()
void stop(JSGlobalObject* globalObject, ProfilerState& self, Context& context, Entry& entry)
{
    int64_t total = callTimer(globalObject, self) - context.startTime;
    int64_t inlineTime = total - context.timeInCalls;
    if (context.previous)
        context.previous->timeInCalls += total;
    self.currentContext = context.previous;
    if (!--entry.recursionLevel)
        entry.totalTime += total;
    else
        ++entry.recursiveCallCount;
    entry.inlineTime += inlineTime;
    ++entry.callCount;
    if (self.recordsSubcalls && context.previous) {
        if (SubEntry* subentry = subEntryOf(*context.previous->entry, entry, false)) {
            if (!--subentry->recursionLevel)
                subentry->totalTime += total;
            else
                ++subentry->recursiveCallCount;
            subentry->inlineTime += inlineTime;
            ++subentry->callCount;
        }
    }
}

// ptrace_enter_call(). What has been raised, as when a generator is thrown into, is left as it is.
void enterCall(JSGlobalObject* globalObject, JSObject* owner, ProfilerState& self, JSCell* key, JSValue userObject)
{
    VM& vm = globalObject->vm();
    Exception* raised = takeRaisedException(vm);
    Entry* entry = self.entries.get(key);
    if (!entry)
        entry = newEntry(globalObject, owner, self, key, userObject);
    if (entry) {
        Context* context = self.freeContexts;
        if (context)
            self.freeContexts = context->previous;
        else
            context = new Context;
        // initContext()
        context->entry = entry;
        context->timeInCalls = 0;
        context->previous = self.currentContext;
        self.currentContext = context;
        ++entry->recursionLevel;
        if (self.recordsSubcalls && context->previous)
            ++subEntryOf(*context->previous->entry, *entry, true)->recursionLevel;
        context->startTime = callTimer(globalObject, self);
    }
    restoreRaisedException(globalObject, raised);
}

// ptrace_leave_call()
void leaveCall(JSGlobalObject* globalObject, ProfilerState& self, JSCell* key)
{
    Context* context = self.currentContext;
    if (!context)
        return;
    if (Entry* entry = self.entries.get(key))
        stop(globalObject, self, *context, *entry);
    else
        self.currentContext = context->previous;
    context->previous = self.freeContexts;
    self.freeContexts = context;
}

// flush_unmatched()
void flushUnmatched(JSGlobalObject* globalObject, ProfilerState& self)
{
    while (Context* context = self.currentContext) {
        stop(globalObject, self, *context, *context->entry);
        delete context;
    }
}

// PyImport_ImportModuleAttrString("sys", "monitoring"). Empty if it raised.
JSValue monitoringModule(JSGlobalObject* globalObject)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    JSValue monitoring = sysAttribute(globalObject, "monitoring"_s);
    RETURN_IF_EXCEPTION(scope, { });
    if (!monitoring)
        return raise(globalObject, scope, BuiltinType::AttributeError, "module 'sys' has no attribute 'monitoring'"_s);
    return monitoring;
}

struct Callback {
    MonitoringEvent event;
    ASCIILiteral method;
    int bit() const { return 1 << static_cast<unsigned>(event); }
};

// callback_table
constexpr Callback callbacks[] = {
    { MonitoringEvent::PyStart, "_pystart_callback"_s },
    { MonitoringEvent::PyResume, "_pystart_callback"_s },
    { MonitoringEvent::PyThrow, "_pythrow_callback"_s },
    { MonitoringEvent::PyReturn, "_pyreturn_callback"_s },
    { MonitoringEvent::PyYield, "_pyreturn_callback"_s },
    { MonitoringEvent::PyUnwind, "_pyreturn_callback"_s },
    { MonitoringEvent::Call, "_ccall_callback"_s },
    { MonitoringEvent::CReturn, "_creturn_callback"_s },
    { MonitoringEvent::CRaise, "_creturn_callback"_s },
};

} // anonymous namespace

// Profiler(timer=None, timeunit=0.0, subcalls=True, builtins=True)
PYTHON_NATIVE(profilerInit)
{
    NATIVE_PROLOGUE();
    auto& self = stateOf<ProfilerState>(args[0]);
    double unit = 0;
    if (JSValue given = args.at(2)) {
        auto converted = toDouble(globalObject, given);
        RETURN_IF_EXCEPTION(scope, { });
        unit = *converted;
    }
    bool subcalls = true;
    if (JSValue given = args.at(3)) {
        subcalls = isTrue(globalObject, given);
        RETURN_IF_EXCEPTION(scope, { });
    }
    bool builtins = true;
    if (JSValue given = args.at(4)) {
        builtins = isTrue(globalObject, given);
        RETURN_IF_EXCEPTION(scope, { });
    }
    self.recordsSubcalls = subcalls;
    self.recordsBuiltins = builtins;
    self.externalTimerUnit = unit;
    // None that is given is not the same as none given: it is what is called.
    if (JSValue timer = args.at(1))
        self.externalTimer.set(vm, args[0].asCell(), timer);
    else
        self.externalTimer.clear();
    self.toolID = profilerToolID;
    JSValue monitoring = monitoringModule(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue missing = getAttribute(globalObject, monitoring, Identifier::fromString(vm, "MISSING"_s));
    RETURN_IF_EXCEPTION(scope, { });
    self.missing.set(vm, args[0].asCell(), missing);
    RETURN_NONE();
}

PYTHON_NATIVE(profilerGetstats)
{
    NATIVE_PROLOGUE();
    auto& self = stateOf<ProfilerState>(args[0]);
    auto& types = realm->moduleState<LsprofModuleState>();
    double factor = !self.externalTimer || !self.externalTimerUnit ? 1.0 / nanosecondsPerSecond : self.externalTimerUnit;
    MarkedArgumentBuffer result;
    for (auto& entry : self.entriesInOrder) {
        if (!entry->callCount)
            continue;
        JSValue calls = jsUndefined();
        if (!entry->callsInOrder.isEmpty()) {
            MarkedArgumentBuffer list;
            for (auto& subentry : entry->callsInOrder) {
                MarkedArgumentBuffer values;
                values.append(subentry->callee->userObject);
                values.append(intFromInt64(globalObject, subentry->callCount));
                values.append(intFromInt64(globalObject, subentry->recursiveCallCount));
                values.append(floatFromDouble(factor * subentry->totalTime));
                values.append(floatFromDouble(factor * subentry->inlineTime));
                list.append(newStructSequence(globalObject, types.subentry.get(), values));
                RETURN_IF_EXCEPTION(scope, { });
            }
            calls = newList(globalObject, list);
        }
        MarkedArgumentBuffer values;
        values.append(entry->userObject);
        values.append(intFromInt64(globalObject, entry->callCount));
        values.append(intFromInt64(globalObject, entry->recursiveCallCount));
        values.append(floatFromDouble(factor * entry->totalTime));
        values.append(floatFromDouble(factor * entry->inlineTime));
        values.append(calls);
        result.append(newStructSequence(globalObject, types.entry.get(), values));
        RETURN_IF_EXCEPTION(scope, { });
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(newList(globalObject, result)));
}

// _pystart_callback(code, instruction_offset, /) and _pythrow_callback(code, instruction_offset, exception, /)
PYTHON_NATIVE(profilerPythonStartCallback)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    // What is found by where it is has to be somewhere.
    if (args[1].isCell())
        enterCall(globalObject, asObject(args[0]), stateOf<ProfilerState>(args[0]), args[1].asCell(), args[1]);
    RETURN_NONE();
}

// _pyreturn_callback(code, instruction_offset, retval, /)
PYTHON_NATIVE(profilerPythonReturnCallback)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    if (args[1].isCell())
        leaveCall(globalObject, stateOf<ProfilerState>(args[0]), args[1].asCell());
    RETURN_NONE();
}

// _ccall_callback(code, instruction_offset, callable, self_arg, /)
PYTHON_NATIVE(profilerNativeCallCallback)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    auto& self = stateOf<ProfilerState>(args[0]);
    if (self.recordsBuiltins) {
        if (auto native = nativeCallableFrom(globalObject, self, args[3], args[4]))
            enterCall(globalObject, asObject(args[0]), self, native.definition, native.object);
    }
    RETURN_NONE();
}

// _creturn_callback(code, instruction_offset, callable, self_arg, /)
PYTHON_NATIVE(profilerNativeReturnCallback)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    auto& self = stateOf<ProfilerState>(args[0]);
    if (self.recordsBuiltins) {
        if (auto native = nativeCallableFrom(globalObject, self, args[3], args[4]))
            leaveCall(globalObject, self, native.definition);
    }
    RETURN_NONE();
}

// enable(subcalls=True, builtins=True)
PYTHON_NATIVE(profilerEnable)
{
    NATIVE_PROLOGUE();
    auto& self = stateOf<ProfilerState>(args[0]);
    bool subcalls = true;
    if (JSValue given = args.at(1)) {
        subcalls = isTrue(globalObject, given);
        RETURN_IF_EXCEPTION(scope, { });
    }
    bool builtins = true;
    if (JSValue given = args.at(2)) {
        builtins = isTrue(globalObject, given);
        RETURN_IF_EXCEPTION(scope, { });
    }
    self.recordsSubcalls = subcalls;
    self.recordsBuiltins = builtins;

    JSValue monitoring = monitoringModule(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    callMethodNamed(globalObject, monitoring, Identifier::fromString(vm, "use_tool_id"_s), jsNumber(self.toolID), jsNontrivialString(vm, "cProfile"_s));
    RETURN_IF_EXCEPTION(scope, { });
    JSValue registerCallback = getAttribute(globalObject, monitoring, Identifier::fromString(vm, "register_callback"_s));
    RETURN_IF_EXCEPTION(scope, { });
    int allEvents = 0;
    for (auto& callback : callbacks) {
        int event = callback.bit();
        JSValue method = getAttribute(globalObject, args[0], Identifier::fromString(vm, callback.method));
        RETURN_IF_EXCEPTION(scope, { });
        call(globalObject, registerCallback, jsNumber(self.toolID), jsNumber(event), method);
        RETURN_IF_EXCEPTION(scope, { });
        allEvents |= event;
    }
    callMethodNamed(globalObject, monitoring, Identifier::fromString(vm, "set_events"_s), jsNumber(self.toolID), jsNumber(allEvents));
    RETURN_IF_EXCEPTION(scope, { });
    self.isEnabled = true;
    RETURN_NONE();
}

PYTHON_NATIVE(profilerDisable)
{
    NATIVE_PROLOGUE();
    auto& self = stateOf<ProfilerState>(args[0]);
    if (self.isInExternalTimer)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::RuntimeError, "cannot disable profiler in external timer"_s));
    if (self.isEnabled) {
        JSValue monitoring = monitoringModule(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        JSValue registerCallback = getAttribute(globalObject, monitoring, Identifier::fromString(vm, "register_callback"_s));
        RETURN_IF_EXCEPTION(scope, { });
        for (auto& callback : callbacks) {
            call(globalObject, registerCallback, jsNumber(self.toolID), jsNumber(callback.bit()), jsUndefined());
            RETURN_IF_EXCEPTION(scope, { });
        }
        callMethodNamed(globalObject, monitoring, Identifier::fromString(vm, "set_events"_s), jsNumber(self.toolID), jsNumber(0));
        RETURN_IF_EXCEPTION(scope, { });
        callMethodNamed(globalObject, monitoring, Identifier::fromString(vm, "free_tool_id"_s), jsNumber(self.toolID));
        RETURN_IF_EXCEPTION(scope, { });
        self.isEnabled = false;
        flushUnmatched(globalObject, self);
    }
    RETURN_NONE();
}

PYTHON_NATIVE(profilerClear)
{
    NATIVE_PROLOGUE();
    auto& self = stateOf<ProfilerState>(args[0]);
    if (self.isInExternalTimer)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::RuntimeError, "cannot clear profiler in external timer"_s));
    clearEntries(self);
    RETURN_NONE();
}

JSObject* createLsprofModule(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    auto& state = realm->moduleState<LsprofModuleState>();
    if (!state.profiler) {
        using Kind = PyNativeFunction::Kind;
        PyType* type = createBuiltinType(globalObject, "_lsprof.Profiler"_s, realm->typeObject(), PyType::Layout::Native, PyType::IsBaseType);
        type->setInstanceStructure(vm, PyStateObject::createStructure(vm, globalObject, type));
        // It has object's __new__().
        type->setAllocator([] (VM& vm, Structure* structure) -> JSObject* { return PyStateObject::create(vm, structure, makeUnique<ProfilerState>()); });
        state.profiler.set(vm, realm, type);
        addMethods(globalObject, type, {
            { "__init__"_s, profilerInit, Kind::Wrapper, 0, { }, PyNativeFunction::Arguments::AreThoseOfTheClass },
            { "getstats"_s, profilerGetstats, Kind::Method, 0, { }, PyNativeFunction::Arguments::AreCheckedAsWithDefiningClass },
            { "enable"_s, profilerEnable },
            { "disable"_s, profilerDisable },
            { "clear"_s, profilerClear },
            { "_pystart_callback"_s, profilerPythonStartCallback },
            { "_pythrow_callback"_s, profilerPythonStartCallback },
            { "_pyreturn_callback"_s, profilerPythonReturnCallback },
            { "_ccall_callback"_s, profilerNativeCallCallback },
            { "_creturn_callback"_s, profilerNativeReturnCallback },
        });
        auto makeSequence = [&] (WriteBarrier<PyType>& slot, ASCIILiteral name, std::initializer_list<ASCIILiteral> fields) {
            PyType* sequence = createBuiltinType(globalObject, name, realm->typeTuple(), PyType::Layout::Tuple, PyType::IsSequence | PyType::IsDerivedFromBuiltin);
            slot.set(vm, realm, sequence);
            makeStructSequenceType(globalObject, sequence, std::span(fields.begin(), fields.size()), static_cast<unsigned>(fields.size()));
        };
        makeSequence(state.entry, "_lsprof.profiler_entry"_s, { "code"_s, "callcount"_s, "reccallcount"_s, "totaltime"_s, "inlinetime"_s, "calls"_s });
        makeSequence(state.subentry, "_lsprof.profiler_subentry"_s, { "code"_s, "callcount"_s, "reccallcount"_s, "totaltime"_s, "inlinetime"_s });
    }
    JSObject* module = newBuiltinModule(globalObject, "_lsprof"_s);
    module->putDirect(vm, Identifier::fromString(vm, "Profiler"_s), state.profiler->object());
    module->putDirect(vm, Identifier::fromString(vm, "profiler_entry"_s), state.entry->object());
    module->putDirect(vm, Identifier::fromString(vm, "profiler_subentry"_s), state.subentry->object());
    return module;
}

} } // namespace JSC::Python
