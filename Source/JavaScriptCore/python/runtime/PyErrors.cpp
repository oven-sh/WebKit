/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyErrors.h"

#include "Exception.h"
#include "FrameTracers.h"
#include "JSCInlines.h"
#include "PyAbstract.h"
#include "PyException.h"
#include "PyNumbers.h"
#include "PyStr.h"
#include "PyType.h"
#include "TopExceptionScope.h"

namespace JSC {
namespace Python {

String typeName(JSGlobalObject* globalObject, JSValue value)
{
    return typeOf(globalObject, value)->nameInMessages();
}

String typeNameOfArgument(JSGlobalObject* globalObject, JSValue value)
{
    return isNone(value) ? "None"_str : typeName(globalObject, value);
}

// An exception raised while another is being handled remembers it, as its __context__.
static void chainContext(JSGlobalObject* globalObject, PyException* exception, JSValue handled)
{
    VM& vm = getVM(globalObject);
    if (!handled || handled.asCell() == exception)
        return;
    // The chain must not come back to the exception. A program may have made it a cycle already, which a second walk at half the
    // speed finds out.
    PyException* link = PyException::tryFrom(handled);
    PyException* slowLink = link;
    bool advanceSlowLink = false;
    while (link) {
        JSValue context = link->context();
        if (!context)
            break;
        if (context.asCell() == exception) {
            link->setContext(vm, { });
            break;
        }
        link = PyException::tryFrom(context);
        if (link == slowLink)
            break;
        if (advanceSlowLink)
            slowLink = PyException::tryFrom(slowLink->context());
        advanceSlowLink = !advanceSlowLink;
    }
    exception->setContext(vm, handled);
}

void willThrowInJS(JSGlobalObject* globalObject, JSValue thrown)
{
    // What a `finally` throws again is on its way already, and has the context it was raised in.
    auto* exception = PyException::tryFrom(thrown);
    if (exception && !exception->context() && !exception->traceback())
        chainContext(globalObject, exception, runtime(globalObject).handledException());
}

static JSValue thrownValueFor(JSGlobalObject* globalObject, JSValue value)
{
    auto* exception = PyException::tryFrom(value);
    if (!exception || !exception->type()->isSubtypeOf(runtime(globalObject).JSError()))
        return value;
    JSValue thrown = exception->field(ExceptionField::jsErrorValue);
    return thrown ? thrown : value;
}

void throwPythonException(JSGlobalObject* globalObject, ThrowScope& scope, JSValue exception)
{
    scope.throwException(globalObject, Exception::create(getVM(globalObject), thrownValueFor(globalObject, exception), Exception::StackCaptureAction::DoNotCaptureStack));
}

void unwrapPendingException(JSGlobalObject* globalObject)
{
    VM& vm = getVM(globalObject);
    Exception* exception = vm.exceptionForInspection();
    if (!exception || thrownValueFor(globalObject, exception->value()) == exception->value())
        return;
    JSValue wrapper = exception->value();
    {
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
        scope.clearException();
    }
    auto scope = DECLARE_THROW_SCOPE(vm);
    throwPythonException(globalObject, scope, wrapper);
}

JSValue raiseException(JSGlobalObject* globalObject, ThrowScope& scope, JSValue exception)
{
    if (auto* pyException = PyException::tryFrom(exception))
        chainContext(globalObject, pyException, runtime(globalObject).handledException());
    throwPythonException(globalObject, scope, exception);
    return { };
}

JSValue raiseExceptionThrownIntoGenerator(JSGlobalObject* globalObject, ThrowScope& scope, JSValue exception)
{
    if (auto* pyException = PyException::tryFrom(exception))
        chainContext(globalObject, pyException, runtime(globalObject).handledExceptionInTopSlot());
    throwPythonException(globalObject, scope, exception);
    return { };
}

JSValue raise(JSGlobalObject* globalObject, ThrowScope& scope, PyType* type, const String& message)
{
    PyException* exception = newException(globalObject, type, message);
    RETURN_IF_EXCEPTION(scope, { });
    return raiseException(globalObject, scope, exception);
}

JSValue raiseWithArgs(JSGlobalObject* globalObject, ThrowScope& scope, PyType* type, std::initializer_list<JSValue> args)
{
    PyException* exception = newException(globalObject, type, std::span<const JSValue> { args.begin(), args.size() });
    RETURN_IF_EXCEPTION(scope, { });
    return raiseException(globalObject, scope, exception);
}

JSValue raiseKeyError(JSGlobalObject* globalObject, ThrowScope& scope, JSValue key)
{
    return raiseWithArgs(globalObject, scope, runtime(globalObject).KeyError(), { key });
}

JSValue raiseAttributeError(JSGlobalObject* globalObject, ThrowScope& scope, JSValue object, JSString* name, const String& message)
{
    VM& vm = getVM(globalObject);
    PyException* exception = newException(globalObject, runtime(globalObject).AttributeError(), message);
    RETURN_IF_EXCEPTION(scope, { });
    exception->setField(vm, ExceptionField::attributeErrorName, name);
    exception->setField(vm, ExceptionField::attributeErrorObj, object);
    return raiseException(globalObject, scope, exception);
}

JSValue raiseStopIteration(JSGlobalObject* globalObject, ThrowScope& scope, JSValue value)
{
    // What StopIteration(value) makes, without the call: every iterator and every coroutine ends with one of these.
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    bool hasValue = value && !isNone(value);
    PyException* exception = PyException::create(vm, rt.structure<PyException>(), rt.StopIteration());
    exception->setArgs(vm, hasValue ? PyTuple::create(globalObject, { value }) : rt.emptyTuple());
    exception->setField(vm, ExceptionField::stopIterationValue, hasValue ? value : jsUndefined());
    return raiseException(globalObject, scope, exception);
}

JSValue raiseOSError(JSGlobalObject* globalObject, ThrowScope& scope, int errorNumber, JSValue filename, JSValue filename2)
{
    VM& vm = getVM(globalObject);
    JSValue message = makeStr(vm, String::fromUTF8(strerror(errorNumber)));
    PyType* type = runtime(globalObject).OSError();
    if (filename2)
        return raiseWithArgs(globalObject, scope, type, { jsNumber(errorNumber), message, filename, jsUndefined(), filename2 });
    if (filename)
        return raiseWithArgs(globalObject, scope, type, { jsNumber(errorNumber), message, filename });
    return raiseWithArgs(globalObject, scope, type, { jsNumber(errorNumber), message });
}

JSValue raiseBadInternalCall(JSGlobalObject* globalObject, ThrowScope& scope)
{
    return raiseSystemError(globalObject, scope, "bad argument to internal function"_s);
}

// ---- The pending exception ----

JSValue pendingException(JSGlobalObject* globalObject)
{
    VM& vm = getVM(globalObject);
    Exception* exception = vm.exceptionForInspection();
    if (!exception)
        return { };
    JSValue value = exception->value();
    if (PyException::is(value))
        return value;
    if (vm.isTerminationException(exception))
        return { };
    // Wrapping it needs the way clear. Then it goes on as it came.
    SuspendExceptionScope suspended(vm);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    JSValue wrapped = exceptionFromThrownValue(globalObject, value);
    scope.clearException();
    return wrapped;
}

bool pendingExceptionMatches(JSGlobalObject* globalObject, PyType* type)
{
    JSValue exception = pendingException(globalObject);
    return exception && typeOf(globalObject, exception)->isSubtypeOf(type);
}

bool clearPendingExceptionIfMatches(JSGlobalObject* globalObject, PyType* type)
{
    if (!pendingExceptionMatches(globalObject, type))
        return false;
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(getVM(globalObject));
    scope.clearException();
    return true;
}

JSValue takePendingException(JSGlobalObject* globalObject)
{
    JSValue exception = pendingException(globalObject);
    if (!exception)
        return { };
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(getVM(globalObject));
    scope.clearException();
    return exception;
}

bool exceptionMatches(JSGlobalObject* globalObject, JSValue exception, JSValue typeOrTuple)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    Runtime& rt = runtime(globalObject);

    auto isExceptionClass = [&](JSValue value) {
        PyType* type = PyType::tryFrom(value);
        return type && type->has(TypeFlag::ExceptionSubclass);
    };
    static constexpr auto message = "catching classes that do not inherit from BaseException is not allowed"_s;

    PyType* exceptionType = rt.typeOf(exception);
    if (auto* tuple = PyTuple::tryFrom(typeOrTuple)) {
        for (unsigned i = 0; i < tuple->length(); ++i) {
            if (!isExceptionClass(tuple->at(i))) {
                raiseTypeError(globalObject, scope, message);
                return false;
            }
        }
        for (unsigned i = 0; i < tuple->length(); ++i) {
            if (exceptionType->isSubtypeOf(PyType::from(tuple->at(i))))
                return true;
        }
        return false;
    }
    if (!isExceptionClass(typeOrTuple)) {
        raiseTypeError(globalObject, scope, message);
        return false;
    }
    return exceptionType->isSubtypeOf(PyType::from(typeOrTuple));
}

} // namespace Python
} // namespace JSC
