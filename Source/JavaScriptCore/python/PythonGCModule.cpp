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
#include "PyDict.h"
#include "PyRealm.h"
#include "PyStateObject.h"
#include "PyTuple.h"
#include "PyType.h"
#include "PythonBytes.h"
#include "PythonOperations.h"
#include "PythonSequences.h"
#include "TopExceptionScope.h"

// The module gc: Modules/gcmodule.c of CPython.
//
// CPython counts references, and has a collector besides for what refers to itself. This module is how a program talks to that. Here there is one collector, JavaScriptCore's, for everything of both languages, and it is not
// a program's to tune. So collect() collects, which is what most programs want of the module, and the rest is here for a program that asks to find what it would find of a collector with nothing to report: what it sets it
// gets back, and there is nothing in any generation. When anything goes is no different for this being here.

namespace JSC { namespace Python {

namespace {

constexpr int numberOfGenerations = 3;

struct GCState final : NativeState {
    PYTHON_NATIVE_STATE(GCState);
    WriteBarrier<JSArray> garbage;
    WriteBarrier<JSArray> callbacks;
    bool isEnabled { true };
    bool isCollecting { false };
    int debug { 0 };
    // _PyGC_InitState()
    int thresholds[numberOfGenerations] { 2000, 10, 10 };
    int64_t collections[numberOfGenerations] { 0, 0, 0 };
};

template<typename Visitor> void GCState::visit(Visitor& visitor)
{
    visitor.append(garbage);
    visitor.append(callbacks);
}

GCState& stateOf(JSGlobalObject* globalObject)
{
    return globalObject->pyRealm()->moduleState<GCState>();
}

// invoke_gc_callback() of CPython's Python/gc.c
void invokeCallbacks(JSGlobalObject* globalObject, ASCIILiteral phase, int generation)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    JSArray* callbacks = stateOf(globalObject).callbacks.get();
    if (!callbacks->length())
        return;
    PyDict* info = PyDict::create(globalObject);
    info->set(globalObject, jsNontrivialString(vm, "generation"_s), jsNumber(generation));
    info->set(globalObject, jsNontrivialString(vm, "collected"_s), jsNumber(0));
    info->set(globalObject, jsNontrivialString(vm, "uncollectable"_s), jsNumber(0));
    if (scope.exception()) [[unlikely]] {
        reportUnraisable(globalObject, "Exception ignored on invoking gc callbacks"_s);
        return;
    }
    JSString* phaseString = jsNontrivialString(vm, phase);
    // As many as there are by then.
    for (unsigned i = 0; i < callbacks->length(); ++i) {
        JSValue callback = callbacks->getIndexQuickly(i);
        MarkedArgumentBuffer arguments;
        arguments.append(phaseString);
        arguments.append(info);
        call(globalObject, callback, arguments);
        if (!scope.exception()) [[likely]]
            continue;
        if (vm.hasPendingTerminationException())
            return;
        reportUnraisableShowing(globalObject, "Exception ignored while calling GC callback"_s, callback);
        if (scope.exception())
            return;
    }
}

bool isTrackedIfNotATuple(JSGlobalObject* globalObject, JSValue value)
{
    // PyObject_IS_GC(): its class says whether it could, which is one of the flags that are CPython's for what CPython has. A class is one that could if it was made while the program ran: type_is_gc().
    constexpr unsigned long haveGC = 1ul << 14;
    constexpr unsigned long heapType = 1ul << 9;
    if (!(typeOf(globalObject, value)->flagsForPython() & haveGC))
        return false;
    return !isType(value) || (asType(value)->flagsForPython() & heapType);
}

// PyObject_GC_IsTracked(): whether it is something that could refer to itself, by way of whatever it refers to. Nothing here goes by the answer.
// A tuple is if anything in it is, however far down: _PyTuple_MaybeUntrack(), which CPython comes to a level at a time as it collects. They can go down a long way, and the same one can be come to by a great many ways.
bool isTracked(JSGlobalObject* globalObject, JSValue value)
{
    Vector<JSValue, 16> toLookAt { value };
    UncheckedKeyHashSet<JSCell*> seen;
    while (!toLookAt.isEmpty()) {
        JSValue next = toLookAt.takeLast();
        if (!isExactly(globalObject, next, BuiltinType::Tuple)) {
            if (isTrackedIfNotATuple(globalObject, next))
                return true;
            continue;
        }
        if (!seen.add(next.asCell()).isNewEntry)
            continue;
        for (auto& item : asTuple(next)->span())
            toLookAt.append(item.get());
    }
    return false;
}

} // anonymous namespace

PYTHON_NATIVE(gcEnable)
{
    UNUSED_PARAM(callFrame);
    stateOf(globalObject).isEnabled = true;
    RETURN_NONE();
}

PYTHON_NATIVE(gcDisable)
{
    UNUSED_PARAM(callFrame);
    stateOf(globalObject).isEnabled = false;
    RETURN_NONE();
}

PYTHON_NATIVE(gcIsEnabled)
{
    UNUSED_PARAM(callFrame);
    return JSValue::encode(jsBoolean(stateOf(globalObject).isEnabled));
}

// collect(generation=2). How many it found that nothing could reach, which is not known here.
PYTHON_NATIVE(gcCollect)
{
    NATIVE_PROLOGUE();
    int generation = numberOfGenerations - 1;
    if (JSValue given = args.at(0)) {
        auto value = toCInt(globalObject, given);
        RETURN_IF_EXCEPTION(scope, { });
        generation = *value;
    }
    if (generation < 0 || generation >= numberOfGenerations)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::ValueError, "invalid generation"_s));

    // gc_collect_main(): one at a time, so what a callback asks for is not done.
    GCState& state = stateOf(globalObject);
    if (state.isCollecting)
        return JSValue::encode(jsNumber(0));
    state.isCollecting = true;
    invokeCallbacks(globalObject, "start"_s, generation);
    if (!vm.hasPendingTerminationException()) {
        vm.heap.collectNow(Sync, generation == numberOfGenerations - 1 ? CollectionScope::Full : CollectionScope::Eden);
        ++state.collections[generation];
        // What is to be told that something has gone is told before this comes back, as in CPython.
        if (vm.hasPythonWork())
            doPendingWork(globalObject);
    }
    if (!scope.exception())
        invokeCallbacks(globalObject, "stop"_s, generation);
    state.isCollecting = false;
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(jsNumber(0));
}

// set_debug(flags, /)
PYTHON_NATIVE(gcSetDebug)
{
    NATIVE_PROLOGUE();
    auto flags = toCInt(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    stateOf(globalObject).debug = *flags;
    RETURN_NONE();
}

PYTHON_NATIVE(gcGetDebug)
{
    UNUSED_PARAM(callFrame);
    return JSValue::encode(jsNumber(stateOf(globalObject).debug));
}

// set_threshold(threshold0, [threshold1, [threshold2]])
PYTHON_NATIVE(gcSetThreshold)
{
    NATIVE_PROLOGUE();
    if (args.keywordCount())
        return JSValue::encode(raiseTypeError(globalObject, scope, "set_threshold() takes no keyword arguments"_s));
    if (!args.size() || args.size() > numberOfGenerations)
        return JSValue::encode(raiseTypeError(globalObject, scope, "gc.set_threshold requires 1 to 3 arguments"_s));
    int given[numberOfGenerations];
    for (unsigned i = 0; i < args.size(); ++i) {
        auto value = toCIntOfFormat(globalObject, args[i]);
        RETURN_IF_EXCEPTION(scope, { });
        given[i] = *value;
    }
    for (unsigned i = 0; i < args.size(); ++i)
        stateOf(globalObject).thresholds[i] = given[i];
    RETURN_NONE();
}

PYTHON_NATIVE(gcGetThreshold)
{
    UNUSED_PARAM(callFrame);
    GCState& state = stateOf(globalObject);
    return JSValue::encode(PyTuple::create(globalObject, { jsNumber(state.thresholds[0]), jsNumber(state.thresholds[1]), jsNumber(state.thresholds[2]) }));
}

PYTHON_NATIVE(gcGetCount)
{
    UNUSED_PARAM(callFrame);
    return JSValue::encode(PyTuple::create(globalObject, { jsNumber(0), jsNumber(0), jsNumber(0) }));
}

// get_referrers(*objs)
PYTHON_NATIVE(gcGetReferrers)
{
    NATIVE_PROLOGUE();
    PyTuple* objects = PyTuple::create(globalObject, args.size());
    for (unsigned i = 0; i < args.size(); ++i)
        objects->initializeAt(vm, i, args[i]);
    if (!audit(globalObject, "gc.get_referrers"_s, objects))
        return { };
    return JSValue::encode(newList(globalObject));
}

// get_referents(*objs)
PYTHON_NATIVE(gcGetReferents)
{
    NATIVE_PROLOGUE();
    PyTuple* objects = PyTuple::create(globalObject, args.size());
    for (unsigned i = 0; i < args.size(); ++i)
        objects->initializeAt(vm, i, args[i]);
    if (!audit(globalObject, "gc.get_referents"_s, objects))
        return { };
    return JSValue::encode(newList(globalObject));
}

// get_objects(generation=None)
PYTHON_NATIVE(gcGetObjects)
{
    NATIVE_PROLOGUE();
    // _Py_convert_optional_to_ssize_t()
    auto given = toOptionalSsize(globalObject, args.at(0), -1);
    RETURN_IF_EXCEPTION(scope, { });
    int64_t generation = *given;
    if (!audit(globalObject, "gc.get_objects"_s, intFromInt64(globalObject, generation)))
        return { };
    if (generation >= numberOfGenerations)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::ValueError, "generation parameter must be less than the number of available generations (3)"_s));
    if (generation < -1)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::ValueError, "generation parameter cannot be negative"_s));
    return JSValue::encode(newList(globalObject));
}

PYTHON_NATIVE(gcGetStats)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(args);
    GCState& state = stateOf(globalObject);
    JSArray* result = newList(globalObject);
    for (int64_t collections : state.collections) {
        PyDict* dict = PyDict::create(globalObject);
        dict->set(globalObject, jsNontrivialString(vm, "collections"_s), intFromInt64(globalObject, collections));
        dict->set(globalObject, jsNontrivialString(vm, "collected"_s), jsNumber(0));
        dict->set(globalObject, jsNontrivialString(vm, "uncollectable"_s), jsNumber(0));
        RETURN_IF_EXCEPTION(scope, { });
        listAppend(globalObject, result, dict);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return JSValue::encode(result);
}

// is_tracked(obj, /)
PYTHON_NATIVE(gcIsTracked)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    return JSValue::encode(jsBoolean(isTracked(globalObject, args[0])));
}

// is_finalized(obj, /). Nothing is finalized and still there to be asked about.
PYTHON_NATIVE(gcIsFinalized)
{
    UNUSED_PARAM(globalObject);
    UNUSED_PARAM(callFrame);
    return JSValue::encode(jsBoolean(false));
}

// freeze() and unfreeze(). There is no generation to put anything in that is not gone through again.
PYTHON_NATIVE(gcDoNothing)
{
    UNUSED_PARAM(globalObject);
    UNUSED_PARAM(callFrame);
    RETURN_NONE();
}

PYTHON_NATIVE(gcGetFreezeCount)
{
    UNUSED_PARAM(globalObject);
    UNUSED_PARAM(callFrame);
    return JSValue::encode(jsNumber(0));
}

JSObject* createGCModule(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    GCState& state = stateOf(globalObject);
    if (!state.garbage) {
        state.garbage.set(vm, realm, newList(globalObject));
        state.callbacks.set(vm, realm, newList(globalObject));
    }

    JSObject* module = newBuiltinModule(globalObject, "gc"_s);
    addFunction(globalObject, module, "enable"_s, gcEnable, 0, "($module, /)"_s);
    addFunction(globalObject, module, "disable"_s, gcDisable, 0, "($module, /)"_s);
    addFunction(globalObject, module, "isenabled"_s, gcIsEnabled, 0, "($module, /)"_s);
    addFunction(globalObject, module, "set_debug"_s, gcSetDebug, 0, "($module, flags, /)"_s);
    addFunction(globalObject, module, "get_debug"_s, gcGetDebug, 0, "($module, /)"_s);
    addFunction(globalObject, module, "get_count"_s, gcGetCount, 0, "($module, /)"_s);
    addFunction(globalObject, module, "set_threshold"_s, gcSetThreshold, 0, "($module, /, *args, **kwargs)"_s, PyNativeFunction::Arguments::AreNotChecked);
    addFunction(globalObject, module, "get_threshold"_s, gcGetThreshold, 0, "($module, /)"_s);
    addFunction(globalObject, module, "collect"_s, gcCollect, 0, "($module, /, generation=2)"_s);
    addFunction(globalObject, module, "get_objects"_s, gcGetObjects, 0, "($module, /, generation=None)"_s);
    addFunction(globalObject, module, "get_stats"_s, gcGetStats, 0, "($module, /)"_s);
    addFunction(globalObject, module, "is_tracked"_s, gcIsTracked, 0, "($module, obj, /)"_s);
    addFunction(globalObject, module, "is_finalized"_s, gcIsFinalized, 0, "($module, obj, /)"_s);
    addFunction(globalObject, module, "get_referrers"_s, gcGetReferrers, 0, "($module, /, *objs)"_s);
    addFunction(globalObject, module, "get_referents"_s, gcGetReferents, 0, "($module, /, *objs)"_s);
    addFunction(globalObject, module, "freeze"_s, gcDoNothing, 0, "($module, /)"_s);
    addFunction(globalObject, module, "unfreeze"_s, gcDoNothing, 0, "($module, /)"_s);
    addFunction(globalObject, module, "get_freeze_count"_s, gcGetFreezeCount, 0, "($module, /)"_s);

    module->putDirect(vm, Identifier::fromString(vm, "garbage"_s), state.garbage.get());
    module->putDirect(vm, Identifier::fromString(vm, "callbacks"_s), state.callbacks.get());
    module->putDirect(vm, Identifier::fromString(vm, "DEBUG_STATS"_s), jsNumber(1));
    module->putDirect(vm, Identifier::fromString(vm, "DEBUG_COLLECTABLE"_s), jsNumber(2));
    module->putDirect(vm, Identifier::fromString(vm, "DEBUG_UNCOLLECTABLE"_s), jsNumber(4));
    module->putDirect(vm, Identifier::fromString(vm, "DEBUG_SAVEALL"_s), jsNumber(32));
    module->putDirect(vm, Identifier::fromString(vm, "DEBUG_LEAK"_s), jsNumber(2 | 4 | 32));
    return module;
}

} } // namespace JSC::Python
