/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "PyRuntime.h"
#include "ThrowScope.h"
#include <wtf/text/MakeString.h>

namespace JSC {
namespace Python {

// Raising from C++. All return an empty value, to be returned in turn.

// raise type(message)
JSValue raise(JSGlobalObject*, ThrowScope&, PyType*, const String& message);
// raise type(*args)
JSValue raiseWithArgs(JSGlobalObject*, ThrowScope&, PyType*, std::initializer_list<JSValue> args);
// raise exception, an instance of BaseException
JSValue raiseException(JSGlobalObject*, ThrowScope&, JSValue exception);

#define PY_DECLARE_RAISE(Name) \
    inline JSValue raise##Name(JSGlobalObject* globalObject, ThrowScope& scope, const String& message) { return raise(globalObject, scope, runtime(globalObject).Name(), message); } \
    template<typename... Parts> \
    requires(sizeof...(Parts) > 1) \
    JSValue raise##Name(JSGlobalObject* globalObject, ThrowScope& scope, Parts&&... parts) { return raise(globalObject, scope, runtime(globalObject).Name(), makeString(std::forward<Parts>(parts)...)); }

PY_DECLARE_RAISE(TypeError)
PY_DECLARE_RAISE(ValueError)
PY_DECLARE_RAISE(IndexError)
PY_DECLARE_RAISE(OverflowError)
PY_DECLARE_RAISE(ZeroDivisionError)
PY_DECLARE_RAISE(RuntimeError)
PY_DECLARE_RAISE(SystemError)
PY_DECLARE_RAISE(NotImplementedError)
PY_DECLARE_RAISE(NameError)
PY_DECLARE_RAISE(UnboundLocalError)
PY_DECLARE_RAISE(ImportError)
PY_DECLARE_RAISE(LookupError)
PY_DECLARE_RAISE(BufferError)
PY_DECLARE_RAISE(AssertionError)
PY_DECLARE_RAISE(RecursionError)
PY_DECLARE_RAISE(EOFError)
PY_DECLARE_RAISE(MemoryError)
#undef PY_DECLARE_RAISE

JSValue raiseKeyError(JSGlobalObject*, ThrowScope&, JSValue key);
// With the name and obj attributes that suggestions are made from.
JSValue raiseAttributeError(JSGlobalObject*, ThrowScope&, JSValue object, JSString* name, const String& message);
JSValue raiseStopIteration(JSGlobalObject*, ThrowScope&, JSValue value = { });
// OSError(errno, strerror(errno)[, filename]), of the subclass for that errno.
JSValue raiseOSError(JSGlobalObject*, ThrowScope&, int errorNumber, JSValue filename = { }, JSValue filename2 = { });
// "bad argument type for built-in operation"
JSValue raiseBadInternalCall(JSGlobalObject*, ThrowScope&);

// How messages name the type of the value: see PyType::nameInMessages().
String typeName(JSGlobalObject*, JSValue);
// The same in "f() argument 1 must be str, not X", where None is "None" (_PyArg_BadArgument() in CPython).
String typeNameOfArgument(JSGlobalObject*, JSValue);

// ---- What JS threw ----
//
// To Python, what JS throws is a JSError that has it as its `value`. What propagates is always the value itself, so that JS
// code further up catches what JS code threw, whatever Python did in between short of raising something else: raising the
// JSError throws the value. An object is given one JSError, once, which it keeps: catching it again finds the same exception.

// Throws the exception, an instance of BaseException (for a JSError that stands for a value, the value), as it is: nothing is chained.
void throwPythonException(JSGlobalObject*, ThrowScope&, JSValue exception);
// If such a JSError is propagating (a generator was made to throw it), the value does from now on.
void unwrapPendingException(JSGlobalObject*);

// ---- The pending exception ----

// The exception that is propagating, as a Python exception: for what JS threw, its JSError. Empty: none.
JSValue pendingException(JSGlobalObject*);
// Whether one is propagating that `except type:` would catch.
bool pendingExceptionMatches(JSGlobalObject*, PyType*);
// If so, it no longer propagates.
bool clearPendingExceptionIfMatches(JSGlobalObject*, PyType*);
// It no longer propagates. Empty: there was none.
JSValue takePendingException(JSGlobalObject*);
// Whether `except typeOrTuple:` catches the exception. May throw.
bool exceptionMatches(JSGlobalObject*, JSValue exception, JSValue typeOrTuple);

} // namespace Python
} // namespace JSC
