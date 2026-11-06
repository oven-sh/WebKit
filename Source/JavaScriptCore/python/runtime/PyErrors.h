/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "FrameTracers.h"
#include "PyRuntime.h"
#include "ThrowScope.h"
#include "TopExceptionScope.h"
#include "VMTrapsInlines.h"
#include <wtf/text/MakeString.h>

namespace JSC {
namespace Python {

// Many functions here say in what they return that they have raised: false, an empty value, null. To test that is to check for
// the exception, and exception scope verification (Options::validateExceptionChecks()) has to be told so:
//     if (failed(scope, parseArguments(globalObject, args, count)))
//         return { };
// It also verifies that such a result and an exception go together. Where an empty result may also mean that there is nothing
// to return, this is not the test: see RETURN_IF_EXCEPTION, or EXCEPTION_ASSERT(!scope.exception() || !result).
template<typename Scope, typename Result>
ALWAYS_INLINE bool failed(Scope& scope, Result result)
{
    EXCEPTION_ASSERT_UNUSED(scope, !!scope.exception() == !result);
    return !result;
}

// Raising from C++. All return an empty value, to be returned in turn.

// raise type(message)
JSValue raise(JSGlobalObject*, ThrowScope&, PyType*, const String& message);
// The same for what tryMakeString() returned. Null: the message would be too long to be a str, which is a MemoryError.
JSValue raiseUnlessTooLong(JSGlobalObject*, ThrowScope&, PyType*, String&& message);
// raise type(*args)
JSValue raiseWithArgs(JSGlobalObject*, ThrowScope&, PyType*, std::initializer_list<JSValue> args);
// raise exception, an instance of BaseException
JSValue raiseException(JSGlobalObject*, ThrowScope&, JSValue exception);
// One that was made for this, and that nothing else refers to.
JSValue raiseNewException(JSGlobalObject*, ThrowScope&, PyException*);
// What throw() was given is raised in the generator that is running. Its context becomes what the generator itself is handling. If
// that is nothing it keeps the one it has: what those who called throw() are handling does not count.
JSValue raiseExceptionThrownIntoGenerator(JSGlobalObject*, ThrowScope&, JSValue exception);

#define PY_DECLARE_RAISE(Name) \
    inline JSValue raise##Name(JSGlobalObject* globalObject, ThrowScope& scope, const String& message) { return raise(globalObject, scope, runtime(globalObject).Name(), message); } \
    template<typename... Parts> \
    requires(sizeof...(Parts) > 1) \
    JSValue raise##Name(JSGlobalObject* globalObject, ThrowScope& scope, Parts&&... parts) { return raiseUnlessTooLong(globalObject, scope, runtime(globalObject).Name(), tryMakeString(std::forward<Parts>(parts)...)); }

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
JSValue raiseAttributeError(JSGlobalObject*, ThrowScope&, JSValue object, JSValue name, const String& message);
// How an iterator ends. With a value that is not None: how a generator or a coroutine returns it, which is the next one.
JSValue raiseStopIteration(JSGlobalObject*, ThrowScope&, JSValue value = { });
// StopIteration(value), None or not. As in CPython (_PyGen_SetStopIterationValue()) it does not remember what is being handled: it is a
// result on its way, not something that went wrong meanwhile.
JSValue raiseStopIterationWithValue(JSGlobalObject*, ThrowScope&, JSValue value);
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
// What that throws.
JSValue thrownValueFor(JSGlobalObject*, JSValue exception);
// If such a JSError is propagating (a generator was made to throw it), the value does from now on.
void unwrapPendingException(JSGlobalObject*);

// ---- The pending exception ----

// The exception that is propagating, as a Python exception: for what JS threw, its JSError. Empty: none, or a termination.
JSValue pendingException(JSGlobalObject*);
// Whether one is propagating that `except type:` would catch.
bool pendingExceptionMatches(JSGlobalObject*, PyType*);
// If so, it no longer propagates.
bool clearPendingExceptionIfMatches(JSGlobalObject*, PyType*);
// It no longer propagates. Empty: there was none, or it is a termination, which goes on.
JSValue takePendingException(JSGlobalObject*);
// For where it is of no consequence what went wrong: nothing propagates any more, whatever it was. That the host is ending what runs
// here is not for the caller to call off, so that is asked for again, and the next safe point throws it again.
void dropPendingException(JSGlobalObject*, ExceptionScope&);

// The exception that is propagating is set aside for as long as this lives, for what has to be done all the same: nothing can be
// called while one is pending. Then it goes on. What is pending by then is dropped, as by dropPendingException().
class ExceptionSetAside {
    WTF_MAKE_NONCOPYABLE(ExceptionSetAside);
    WTF_FORBID_HEAP_ALLOCATION;
public:
    ALWAYS_INLINE explicit ExceptionSetAside(JSGlobalObject* globalObject)
        : m_globalObject(globalObject)
        , m_suspended(getVM(globalObject))
        , m_scope(DECLARE_TOP_EXCEPTION_SCOPE(getVM(globalObject)))
    {
    }

    ALWAYS_INLINE ~ExceptionSetAside() { dropPendingException(m_globalObject, m_scope); }

private:
    JSGlobalObject* m_globalObject;
    SuspendExceptionScope m_suspended;
    TopExceptionScope m_scope;
};

// For code of the program that is called where terminations are deferred (DeferTermination): it has to be possible to end it. For as
// long as this lives they are thrown. None gets out, so whoever deferred them notices nothing: one that is pending at the end is
// dropped, as by dropPendingException(), before the deferral is put back. Any other exception goes on. It lifts one deferral, that
// of the caller or of whoever called it: the count is the VM's, and another may be that of a thread that waits for the GIL. Where
// they are not deferred, this does nothing.
class TerminationNotDeferred {
    WTF_MAKE_NONCOPYABLE(TerminationNotDeferred);
    WTF_FORBID_HEAP_ALLOCATION;
public:
    ALWAYS_INLINE explicit TerminationNotDeferred(JSGlobalObject* globalObject)
        : m_globalObject(globalObject)
        , m_scope(DECLARE_TOP_EXCEPTION_SCOPE(getVM(globalObject)))
        , m_wasDeferred(getVM(globalObject).traps().isDeferringTermination())
    {
        ASSERT(!m_scope.exception());
        if (m_wasDeferred)
            m_scope.vm().traps().undoDeferTermination(VMTraps::DeferAction::DeferForAWhile);
    }

    ALWAYS_INLINE ~TerminationNotDeferred()
    {
        bool isTermination = m_scope.exception() && m_scope.vm().hasPendingTerminationException();
        if (!m_wasDeferred)
            return;
        if (isTermination) [[unlikely]]
            dropPendingException(m_globalObject, m_scope);
        m_scope.vm().traps().deferTermination(VMTraps::DeferAction::DeferForAWhile);
    }

private:
    JSGlobalObject* m_globalObject;
    TopExceptionScope m_scope;
    bool m_wasDeferred;
};

// Whether `except typeOrTuple:` catches the exception. May throw.
bool exceptionMatches(JSGlobalObject*, JSValue exception, JSValue typeOrTuple);

} // namespace Python
} // namespace JSC
