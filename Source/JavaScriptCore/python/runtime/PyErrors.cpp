/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyErrors.h"

#include "Exception.h"
#include "JSCInlines.h"
#include "PyAbstract.h"
#include "PyException.h"
#include "PyInternals.h"
#include "PyNumbers.h"
#include "PyStr.h"
#include "PyThreads.h"
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

JSValue thrownValueFor(JSGlobalObject* globalObject, JSValue value)
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
    if (auto* pyException = PyException::tryFrom(exception)) {
        pyException->willBeSeen();
        chainContext(globalObject, pyException, runtime(globalObject).handledException());
    }
    throwPythonException(globalObject, scope, exception);
    return { };
}

JSValue raiseNewException(JSGlobalObject* globalObject, ThrowScope& scope, PyException* exception)
{
    if (isMadeWithoutCodeOfTheProgram(globalObject, exception->type()))
        exception->wasMadeToBeRaised();
    chainContext(globalObject, exception, runtime(globalObject).handledException());
    throwPythonException(globalObject, scope, exception);
    return { };
}

JSValue raiseExceptionThrownIntoGenerator(JSGlobalObject* globalObject, ThrowScope& scope, JSValue exception)
{
    if (auto* pyException = PyException::tryFrom(exception)) {
        pyException->willBeSeen();
        chainContext(globalObject, pyException, runtime(globalObject).handledExceptionInTopSlot());
    }
    throwPythonException(globalObject, scope, exception);
    return { };
}

JSValue raise(JSGlobalObject* globalObject, ThrowScope& scope, PyType* type, const String& message)
{
    PyException* exception = newException(globalObject, type, message);
    RETURN_IF_EXCEPTION(scope, { });
    return raiseNewException(globalObject, scope, exception);
}

JSValue raiseUnlessTooLong(JSGlobalObject* globalObject, ThrowScope& scope, PyType* type, String&& message)
{
    if (message.isNull()) [[unlikely]]
        return raise(globalObject, scope, runtime(globalObject).MemoryError(), String());
    return raise(globalObject, scope, type, message);
}

JSValue raiseWithArgs(JSGlobalObject* globalObject, ThrowScope& scope, PyType* type, std::initializer_list<JSValue> args)
{
    PyException* exception = newException(globalObject, type, std::span<const JSValue> { args.begin(), args.size() });
    RETURN_IF_EXCEPTION(scope, { });
    return raiseNewException(globalObject, scope, exception);
}

JSValue raiseKeyError(JSGlobalObject* globalObject, ThrowScope& scope, JSValue key)
{
    return raiseWithArgs(globalObject, scope, runtime(globalObject).KeyError(), { key });
}

JSValue raiseAttributeError(JSGlobalObject* globalObject, ThrowScope& scope, JSValue object, JSValue name, const String& message)
{
    VM& vm = getVM(globalObject);
    PyException* exception = newException(globalObject, runtime(globalObject).AttributeError(), message);
    RETURN_IF_EXCEPTION(scope, { });
    exception->setField(vm, ExceptionField::attributeErrorName, name);
    exception->setField(vm, ExceptionField::attributeErrorObj, object);
    return raiseNewException(globalObject, scope, exception);
}

JSValue raiseStopIteration(JSGlobalObject* globalObject, ThrowScope& scope, JSValue value)
{
    // What StopIteration(value) makes, without the call: every iterator and every coroutine ends with one of these.
    if (value && !isNone(value))
        return raiseStopIterationWithValue(globalObject, scope, value);
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    PyException* exception = PyException::create(vm, rt.structure<PyException>(), rt.StopIteration());
    exception->setArgs(vm, rt.emptyTuple());
    exception->setField(vm, ExceptionField::stopIterationValue, jsUndefined());
    return raiseNewException(globalObject, scope, exception);
}

JSValue raiseStopIterationWithValue(JSGlobalObject* globalObject, ThrowScope& scope, JSValue value)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    PyException* exception = PyException::create(vm, rt.structure<PyException>(), rt.StopIteration());
    exception->setArgs(vm, PyTuple::create(globalObject, { value }));
    exception->setField(vm, ExceptionField::stopIterationValue, value);
    exception->wasMadeToBeRaised();
    throwPythonException(globalObject, scope, exception);
    return { };
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
//
// The VM remembers the last exception thrown when it has long been handled, until something else is thrown or code is entered from outside, for an embedder that has caught a
// value and wants to report it. With it stay the traceback and all that the frames in it refer to. Whoever catches for Python makes the VM forget: here, and op_py_catch.

// For a look at it that leaves nothing behind.
static JSValue pendingExceptionToLookAt(JSGlobalObject* globalObject)
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
    ExceptionSetAside suspended(globalObject);
    return exceptionFromThrownValue(globalObject, value);
}

JSValue pendingException(JSGlobalObject* globalObject)
{
    JSValue exception = pendingExceptionToLookAt(globalObject);
    if (exception)
        PyException::from(exception)->willBeSeen();
    return exception;
}

bool pendingExceptionMatches(JSGlobalObject* globalObject, PyType* type)
{
    JSValue exception = pendingExceptionToLookAt(globalObject);
    return exception && typeOf(globalObject, exception)->isSubtypeOf(type);
}

bool clearPendingExceptionIfMatches(JSGlobalObject* globalObject, PyType* type)
{
    if (!pendingExceptionMatches(globalObject, type))
        return false;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    scope.clearException();
    vm.clearLastException();
    return true;
}

JSValue takePendingException(JSGlobalObject* globalObject)
{
    JSValue exception = pendingException(globalObject);
    if (!exception)
        return { };
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    scope.clearException();
    vm.clearLastException();
    return exception;
}

static void askForTerminationAgain(JSGlobalObject* globalObject)
{
    // The trap is spent once it has thrown. (The threads that Python starts are told at every safe point: Threads::throwIfStopping().)
    // Whoever withdraws the termination withdraws this with it: VM::cancelTermination().
    if (runtime(globalObject).threads().isMainThread())
        getVM(globalObject).traps().fireTrap(VMTraps::NeedTermination);
}

void dropPendingException(JSGlobalObject* globalObject, ExceptionScope& scope)
{
    // The VM forgets it too, as where Python catches: see clearPendingExceptionIfMatches().
    scope.vm().clearLastException();
    if (scope.tryClearException()) [[likely]]
        return;
    DECLARE_TOP_EXCEPTION_SCOPE(scope.vm()).clearException();
    askForTerminationAgain(globalObject);
}

bool exceptionMatches(JSGlobalObject* globalObject, JSValue exception, JSValue typeOrTuple)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    Runtime& rt = runtime(globalObject);

    // A class of JS catches what JS threw that is an instance of it: `except js.RangeError`.
    auto isExceptionClass = [&](JSValue value) {
        PyType* type = PyType::tryFrom(value);
        return type ? type->has(TypeFlag::ExceptionSubclass) : isClassOfJSToCatch(globalObject, value);
    };
    static constexpr auto message = "catching classes that do not inherit from BaseException is not allowed"_s;

    PyType* exceptionType = rt.typeOf(exception);
    auto isInstance = [&](JSValue cls) {
        PyType* type = PyType::tryFrom(cls);
        return type ? exceptionType->isSubtypeOf(type) : standsForInstanceOfJS(globalObject, exception, cls);
    };
    if (auto* tuple = PyTuple::tryFrom(typeOrTuple)) {
        for (unsigned i = 0; i < tuple->length(); ++i) {
            if (!isExceptionClass(tuple->at(i))) {
                raiseTypeError(globalObject, scope, message);
                return false;
            }
        }
        for (unsigned i = 0; i < tuple->length(); ++i) {
            if (isInstance(tuple->at(i)))
                return true;
        }
        return false;
    }
    if (!isExceptionClass(typeOrTuple)) {
        raiseTypeError(globalObject, scope, message);
        return false;
    }
    return isInstance(typeOrTuple);
}

} // namespace Python
} // namespace JSC
