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
#include "PythonLifecycle.h"
#include "PythonOperations.h"
#include "PythonSequences.h"
#include "TopExceptionScope.h"

// The module atexit: Modules/atexitmodule.c of CPython.

namespace JSC { namespace Python {

namespace {

struct AtExitState final : NativeState {
    PYTHON_NATIVE_STATE(AtExitState);
    WriteBarrier<JSArray> callbacks; // A list of (function, arguments, keywords or None), the last that was registered first.
};

template<typename Visitor> void AtExitState::visit(Visitor& visitor) { visitor.append(callbacks); }

JSArray* callbacksOf(JSGlobalObject* globalObject)
{
    PyRealm* realm = globalObject->pyRealm();
    auto& state = realm->moduleState<AtExitState>();
    if (!state.callbacks)
        state.callbacks.set(globalObject->vm(), realm, newList(globalObject));
    return state.callbacks.get();
}

void clear(JSGlobalObject* globalObject, JSArray* callbacks)
{
    listReplaceRange(globalObject, callbacks, 0, callbacks->length(), ArgList());
}

} // anonymous namespace

// atexit_callfuncs(). What one of them raises is said, and the rest are called all the same.
void callAtExitFunctions(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    JSArray* callbacks = callbacksOf(globalObject);
    // Those that there are now. What they register is not called, and goes with the rest.
    MarkedArgumentBuffer copy;
    for (unsigned i = 0; i < callbacks->length(); ++i)
        copy.append(callbacks->getIndexQuickly(i));
    for (unsigned i = 0; i < copy.size(); ++i) {
        PyTuple* entry = asTuple(copy.at(i));
        JSValue function = entry->at(0);
        MarkedArgumentBuffer arguments;
        for (auto& argument : asTuple(entry->at(1))->span())
            arguments.append(argument.get());
        if (isNone(entry->at(2)))
            call(globalObject, function, arguments);
        else
            callWithKeywordDict(globalObject, function, arguments, asDict(entry->at(2)));
        if (!scope.exception()) [[likely]]
            continue;
        if (vm.hasPendingTerminationException())
            return;
        reportUnraisableShowing(globalObject, "Exception ignored in atexit callback"_s, function);
        if (scope.exception())
            return;
    }
    clear(globalObject, callbacks);
}

// register(func, *args, **kwargs)
PYTHON_NATIVE(atExitRegister)
{
    NATIVE_PROLOGUE();
    if (!args.size())
        return JSValue::encode(raiseTypeError(globalObject, scope, "register() takes at least 1 argument (0 given)"_s));
    JSValue function = args[0];
    if (!isCallable(globalObject, function))
        return JSValue::encode(raiseTypeError(globalObject, scope, "the first argument must be callable"_s));
    PyTuple* arguments = PyTuple::create(globalObject, args.size() - 1);
    for (unsigned i = 1; i < args.size(); ++i)
        arguments->initializeAt(vm, i - 1, args[i]);
    JSValue keywords = jsUndefined();
    if (unsigned count = args.keywordCount()) {
        PyDict* dict = PyDict::create(globalObject);
        for (unsigned i = 0; i < count; ++i) {
            dict->set(globalObject, args.keywordName(i), args.keywordValue(i));
            RETURN_IF_EXCEPTION(scope, { });
        }
        keywords = dict;
    }
    listInsert(globalObject, callbacksOf(globalObject), 0, PyTuple::create(globalObject, { function, arguments, keywords }));
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(function);
}

PYTHON_NATIVE(atExitRunExitFunctions)
{
    UNUSED_PARAM(callFrame);
    callAtExitFunctions(globalObject);
    RETURN_NONE();
}

PYTHON_NATIVE(atExitClear)
{
    UNUSED_PARAM(callFrame);
    clear(globalObject, callbacksOf(globalObject));
    RETURN_NONE();
}

PYTHON_NATIVE(atExitCallbackCount)
{
    UNUSED_PARAM(callFrame);
    return JSValue::encode(intFromUInt64(globalObject, callbacksOf(globalObject)->length()));
}

// unregister(func, /)
PYTHON_NATIVE(atExitUnregister)
{
    NATIVE_PROLOGUE();
    JSArray* callbacks = callbacksOf(globalObject);
    for (int64_t i = static_cast<int64_t>(callbacks->length()) - 1; i >= 0; --i) {
        JSValue entry = callbacks->getIndexQuickly(static_cast<unsigned>(i));
        bool isSame = isEqual(globalObject, args[0], asTuple(entry)->at(0));
        RETURN_IF_EXCEPTION(scope, { });
        if (isSame) {
            // Where it is may have changed, if comparing them registered or unregistered anything.
            for (int64_t j = std::min<int64_t>(static_cast<int64_t>(callbacks->length()) - 1, i); j >= 0; --j) {
                if (callbacks->getIndexQuickly(static_cast<unsigned>(j)) != entry)
                    continue;
                listReplaceRange(globalObject, callbacks, static_cast<unsigned>(j), 1, ArgList());
                RETURN_IF_EXCEPTION(scope, { });
                i = j;
                break;
            }
        }
        i = std::min<int64_t>(i, callbacks->length());
    }
    RETURN_NONE();
}

JSObject* createAtExitModule(JSGlobalObject* globalObject)
{
    JSObject* module = newBuiltinModule(globalObject, "atexit"_s);
    addFunction(globalObject, module, "register"_s, atExitRegister, 0, "($module, /, *args, **kwargs)"_s, PyNativeFunction::Arguments::AreNotChecked);
    addFunction(globalObject, module, "_clear"_s, atExitClear, 0, "($module, /)"_s);
    addFunction(globalObject, module, "unregister"_s, atExitUnregister, 0, "($module, func, /)"_s);
    addFunction(globalObject, module, "_run_exitfuncs"_s, atExitRunExitFunctions, 0, "($module, /)"_s);
    addFunction(globalObject, module, "_ncallbacks"_s, atExitCallbackCount, 0, "($module, /)"_s);
    return module;
}

} } // namespace JSC::Python
