/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyGenerator.h"

#include "JSCInlines.h"
#include "JSInternalFieldObjectImpl.h"
#include "PyAbstract.h"
#include "PyAsyncio.h"
#include "PyErrors.h"
#include "PyException.h"
#include "PyInternals.h"
#include "PyIterators.h"
#include "PyMonitoring.h"
#include "PyStr.h"
#include "PyTypes.h"

namespace JSC {
namespace Python {

// Generators, coroutines and asynchronous generators. (Objects/genobject.c in CPython is the reference.)
//
// The body of one is compiled as that of a JS generator is: a function of (generator, state, value, resume mode, frame) that
// returns at each yield, having saved its registers in the frame and where to resume in the generator.
//
// A resume mode other than NormalMode raises `value` where the generator is suspended. If that is in `yield from` or `await`,
// the exception goes on to what is being waited on; a GeneratorExit closes that instead, unless the mode is ReturnMode, which
// asynchronous generators use: what they await has to be let run to its end.

using ResumeMode = JSGenerator::ResumeMode;
static constexpr ResumeMode throwAndClose = ResumeMode::ThrowMode;
static constexpr ResumeMode throwThrough = ResumeMode::ReturnMode;

PY_DEFINE_CELL(PyGenerator);
PY_DEFINE_CELL(PyAsyncGenAwaitable);

PyGenerator::PyGenerator(VM& vm, Structure* structure, PyType* type, Kind kind, unsigned slotCount)
    : Base(vm, structure, type)
    , m_slotCount(slotCount)
    , m_kind(kind)
{
    for (unsigned i = 0; i < slotCount; ++i)
        slots()[i].clear();
}

PyGenerator* PyGenerator::create(JSGlobalObject* globalObject, PyFunction* function, PyFunction* body, unsigned slotCount)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto flags = body->codeInfo().flags;
    Kind kind = flags.contains(CodeFlag::AsyncGenerator) ? Kind::AsyncGenerator : flags.contains(CodeFlag::Coroutine) ? Kind::Coroutine : Kind::Generator;
    PyType* type = kind == Kind::AsyncGenerator ? rt.asyncGeneratorType() : kind == Kind::Coroutine ? rt.coroutineType() : rt.generatorType();
    auto* generator = new (NotNull, allocateCell<PyGenerator>(vm, allocationSize(slotCount))) PyGenerator(vm, rt.structure<PyGenerator>(), type, kind, slotCount);
    generator->finishCreation(vm);
    generator->m_state.setWithoutWriteBarrier(jsNumber(NotStarted));
    generator->m_body.set(vm, generator, body);
    generator->m_function.set(vm, generator, function);
    generator->m_name.set(vm, generator, function->name());
    generator->m_qualname.set(vm, generator, function->qualname());
    generator->isIterableCoroutine = function->isIterableCoroutine();
    if (kind == Kind::Coroutine && rt.coroutineOriginTrackingDepth > 0) [[unlikely]]
        generator->m_origin.set(vm, generator, coroutineOrigin(globalObject, rt.coroutineOriginTrackingDepth));

    bool hasHandlers = body->functionExecutable()->unlinkedExecutable()->pythonCodeHasExceptionHandlers();
    if (hasHandlers || kind != Kind::Generator) {
        generator->m_finalization = Finalization::Registered;
        rt.registerNewGenerator(generator);
    }
    return generator;
}

template<typename Visitor>
void PyGenerator::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<PyGenerator>(cell);
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_state);
    visitor.append(thisObject->m_body);
    visitor.appendValues(thisObject->slots(), thisObject->m_slotCount);
    visitor.append(thisObject->m_function);
    visitor.append(thisObject->m_name);
    visitor.append(thisObject->m_qualname);
    visitor.append(thisObject->m_delegate);
    visitor.append(thisObject->m_handledException);
    visitor.append(thisObject->m_frameObject);
    visitor.append(thisObject->m_finalizer);
    visitor.append(thisObject->m_origin);
}

DEFINE_VISIT_CHILDREN(PyGenerator);

void PyGenerator::complete()
{
    setState(Completed);
    for (unsigned i = 0; i < m_slotCount; ++i)
        slots()[i].clear();
    m_delegate.clear();
    m_handledException.clear();
    m_frameObject.clear();
    m_finalization = Finalization::None;
}

static ASCIILiteral describe(PyGenerator::Kind kind)
{
    switch (kind) {
    case PyGenerator::Kind::Generator:
        return "generator"_s;
    case PyGenerator::Kind::Coroutine:
        return "coroutine"_s;
    case PyGenerator::Kind::AsyncGenerator:
        return "async generator"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// A StopIteration that leaves the body would pass for the end of the generator.
static void replaceStopIteration(JSGlobalObject* globalObject, PyGenerator::Kind kind)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    ASCIILiteral what;
    if (pendingExceptionMatches(globalObject, rt.StopIteration()))
        what = "StopIteration"_s;
    else if (kind == PyGenerator::Kind::AsyncGenerator && pendingExceptionMatches(globalObject, rt.StopAsyncIteration()))
        what = "StopAsyncIteration"_s;
    else
        return;
    JSValue original = takePendingException(globalObject);
    PyException* error = newException(globalObject, rt.RuntimeError(), makeString(describe(kind), " raised "_s, what));
    RETURN_IF_EXCEPTION(scope, void());
    raiseException(globalObject, scope, error);
    error->setCause(vm, original);
    error->setSuppressContext(true);
    error->setContext(vm, original);
}

bool PyGenerator::enterBody(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    ASSERT(state() >= 0);
    setState(Executing);
    m_delegate.clear();
    m_resumedBy = rt.runningGenerator();
    rt.setRunningGenerator(this);
    // The unwinder counts the frame out again.
    if (++rt.frameDepth <= rt.recursionLimit) [[likely]]
        return true;
    auto scope = DECLARE_THROW_SCOPE(rt.vm());
    raiseRecursionError(globalObject, scope, "maximum recursion depth exceeded"_s);
    return false;
}

ALWAYS_INLINE void PyGenerator::leaveBody(Runtime& rt)
{
    ASSERT(rt.runningGenerator() == this);
    rt.setRunningGenerator(m_resumedBy);
    m_resumedBy = nullptr;
    rt.leaveFrame();
}

void PyGenerator::suspendBody(JSGlobalObject* globalObject, unsigned resumeOffset)
{
    leaveBody(runtime(globalObject));
    setState(static_cast<int32_t>(resumeOffset));
    if (PyFrame* frame = m_frameObject.get())
        frame->generatorHasStopped(false);
}

void PyGenerator::returnFromBody(JSGlobalObject* globalObject)
{
    leaveBody(runtime(globalObject));
    if (PyFrame* frame = m_frameObject.get())
        frame->generatorHasStopped(true);
    complete();
}

void PyGenerator::bodyIsUnwinding(JSGlobalObject* globalObject, bool isTermination)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    // The exception may come from before the body's first instruction.
    if (!isRunning())
        return;
    leaveBody(runtime(globalObject));
    if (PyFrame* frame = m_frameObject.get())
        frame->generatorHasStopped(true);
    complete();
    if (isTermination)
        return;
    if (!body()->codeInfo().isLambda)
        replaceStopIteration(globalObject, m_kind);
    EXCEPTION_ASSERT(scope.exception());
    unwrapPendingException(globalObject);
    EXCEPTION_ASSERT_UNUSED(scope, scope.exception());
}

void generatorBodyIsUnwinding(JSGlobalObject* globalObject, CallFrame* callFrame, bool isTermination)
{
    uncheckedDowncast<PyGenerator>(callFrame->uncheckedArgument(0).asCell())->bodyIsUnwinding(globalObject, isTermination);
}

PyGenerator::Outcome PyGenerator::resume(JSGlobalObject* globalObject, JSValue value, ResumeMode mode, JSValue& result)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    ASSERT(state() >= 0);
    ASSERT(state() || mode == ResumeMode::NormalMode);

    throwIsWithDelegate = mode != ResumeMode::NormalMode && m_delegate;
    JSValue arguments[] = { this, m_state.get(), value, jsNumber(static_cast<int32_t>(mode)), jsUndefined() };
    JSValue returned = callBodyOfGenerator(globalObject, body(), arguments);
    if (scope.exception()) [[unlikely]] {
        // It did not get as far as the body: there was no room on the stack for its frame.
        if (!isCompleted())
            complete();
        return Outcome::Threw;
    }
    result = returned;
    return isCompleted() ? Outcome::Returned : Outcome::Yielded;
}

static JSValue raiseAlreadyExecuting(JSGlobalObject* globalObject, PyGenerator* generator)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raiseValueError(globalObject, scope, describe(generator->kind()), " already executing"_s);
}

JSValue generatorSend(JSGlobalObject* globalObject, PyGenerator* generator, JSValue value, JSValue& result)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    result = JSValue();
    if (!generator->isStarted() && !isNone(value))
        return raiseTypeError(globalObject, scope, "can't send non-None value to a just-started "_s, describe(generator->kind()));
    if (generator->isRunning())
        RELEASE_AND_RETURN(scope, raiseAlreadyExecuting(globalObject, generator));
    if (generator->isCompleted()) {
        if (generator->kind() == PyGenerator::Kind::Coroutine)
            return raiseRuntimeError(globalObject, scope, "cannot reuse already awaited coroutine"_s);
        result = jsUndefined();
        return { };
    }
    JSValue out;
    auto outcome = generator->resume(globalObject, value, ResumeMode::NormalMode, out);
    scope.release();
    switch (outcome) {
    case PyGenerator::Outcome::Yielded:
        return out;
    case PyGenerator::Outcome::Returned:
        result = out;
        return { };
    case PyGenerator::Outcome::Threw:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static JSValue generatorThrowWithMode(JSGlobalObject* globalObject, PyGenerator* generator, JSValue exception, ResumeMode mode, JSValue& result)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    result = JSValue();
    if (generator->isRunning())
        RELEASE_AND_RETURN(scope, raiseAlreadyExecuting(globalObject, generator));
    if (generator->isCompleted()) {
        if (generator->kind() == PyGenerator::Kind::Coroutine)
            return raiseRuntimeError(globalObject, scope, "cannot reuse already awaited coroutine"_s);
        // As it is: see raiseExceptionThrownIntoGenerator().
        throwPythonException(globalObject, scope, exception);
        return { };
    }
    if (!generator->isStarted()) {
        // There is nothing in it that could catch the exception, which passes through it all the same.
        if (auto* thrown = PyException::tryFrom(exception))
            thrown->setTraceback(vm, PyTraceback::create(globalObject, thrown->traceback(), PyFrame::of(globalObject, generator), generator->body()->codeInfo().firstLineNumber, 0));
        generator->complete();
        throwPythonException(globalObject, scope, exception);
        return { };
    }
    JSValue out;
    auto outcome = generator->resume(globalObject, exception, mode, out);
    scope.release();
    switch (outcome) {
    case PyGenerator::Outcome::Yielded:
        return out;
    case PyGenerator::Outcome::Returned:
        result = out;
        return { };
    case PyGenerator::Outcome::Threw:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

JSValue generatorThrow(JSGlobalObject* globalObject, PyGenerator* generator, JSValue exception, JSValue& result)
{
    return generatorThrowWithMode(globalObject, generator, exception, throwAndClose, result);
}

// What close() returns. Empty: an exception.
static JSValue generatorCloseWithResult(JSGlobalObject* globalObject, PyGenerator* generator)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!generator->isStarted()) {
        generator->complete();
        return jsUndefined();
    }
    if (generator->isCompleted())
        return jsUndefined();
    if (generator->isRunning())
        RELEASE_AND_RETURN(scope, raiseAlreadyExecuting(globalObject, generator));

    PyException* exit = newException(globalObject, rt.GeneratorExit(), std::span<const JSValue> { });
    RETURN_IF_EXCEPTION(scope, { });
    // It is raised here, by whoever closes the generator, and then thrown in.
    if (JSValue handled = rt.handledException())
        exit->setContext(vm, handled);
    JSValue out;
    PyGenerator::Outcome outcome = generator->resume(globalObject, exit, throwAndClose, out);
    EXCEPTION_ASSERT(!!scope.exception() == (outcome == PyGenerator::Outcome::Threw));
    switch (outcome) {
    case PyGenerator::Outcome::Returned:
        return out;
    case PyGenerator::Outcome::Yielded:
        return raiseRuntimeError(globalObject, scope, describe(generator->kind()), " ignored GeneratorExit"_s);
    case PyGenerator::Outcome::Threw:
        break;
    }
    scope.release();
    if (clearPendingExceptionIfMatches(globalObject, rt.GeneratorExit()))
        return jsUndefined();
    return { };
}

void generatorClose(JSGlobalObject* globalObject, PyGenerator* generator)
{
    generatorCloseWithResult(globalObject, generator);
}

void generatorIsDropped(JSGlobalObject* globalObject, PyGenerator* generator)
{
    if (generator->isCompleted() || generator->isRunning())
        return;
    // GeneratorExit would go through it unnoticed.
    if (!generator->body()->functionExecutable()->unlinkedExecutable()->pythonCodeHasExceptionHandlers() && !runtime(globalObject).monitoring().wantsExceptions()) {
        generator->complete();
        return;
    }
    finalizeGeneratorNow(globalObject, generator);
}

// ---- yield from, await ----

// If a StopIteration is propagating, it stops, and this is its value. Empty: something else is.
static JSValue takeStopIterationValue(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    if (!pendingExceptionMatches(globalObject, rt.StopIteration()))
        return { };
    JSValue value = PyException::from(takePendingException(globalObject))->field(ExceptionField::stopIterationValue);
    return value ? value : jsUndefined();
}

static ALWAYS_INLINE PyGenerator* asGeneratorOrCoroutine(JSValue value)
{
    auto* generator = PyGenerator::tryFrom(value);
    return generator && generator->kind() != PyGenerator::Kind::AsyncGenerator ? generator : nullptr;
}

// `yield from` a list, a tuple, a str or a bytes makes no iterator that anything could see (CPython goes through them by index):
// gi_yieldfrom is the sequence, and it is the sequence that lacks send(), throw() and close(). This stands for it in the
// register of the generator. Fields: the sequence, its iterator.
static constexpr TypeDef sequenceDelegateTypeDef { "sequence_delegate"_s, { }, { }, { }, { } };

static ALWAYS_INLINE PyRecord* asSequenceDelegate(JSGlobalObject* globalObject, JSValue iterator)
{
    if (!iterator.isCell() || iterator.asCell()->type() != PyObjectType)
        return nullptr;
    return PyRecord::tryFrom(iterator, runtime(globalObject).typeFor(sequenceDelegateTypeDef));
}

// iterator.close(), if it has such a method.
static void closeDelegate(JSGlobalObject* globalObject, JSValue iterator)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (asSequenceDelegate(globalObject, iterator))
        return;
    if (auto* generator = asGeneratorOrCoroutine(iterator))
        RELEASE_AND_RETURN(scope, generatorClose(globalObject, generator));
    JSValue method = getAttrIfPresent(globalObject, iterator, runtime(globalObject).names.close());
    if (scope.exception()) [[unlikely]] {
        reportUnraisable(globalObject, makeString("Exception ignored while closing generator "_s, reprForMessage(globalObject, iterator)), { });
        return;
    }
    if (method) {
        scope.release();
        call(globalObject, method);
    }
}

JSValue sendToDelegate(JSGlobalObject* globalObject, JSValue iterator, JSValue value, ResumeMode mode)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyGenerator* waiting = rt.runningGenerator();
    ASSERT(waiting);

    // What has no throw() is not asked: the exception is raised where the generator waits. There a StopIteration is taken for the end of what it
    // waits for, wherever it comes from.
    auto raiseWhereItWaits = [&]() -> JSValue {
        auto* exception = PyException::tryFrom(value);
        if (!exception || !exception->type()->isSubtypeOf(rt.StopIteration()))
            return raiseExceptionThrownIntoGenerator(globalObject, scope, value);
        JSValue result = exception->field(ExceptionField::stopIterationValue);
        rt.setSendResult(result ? result : jsUndefined());
        return { };
    };

    // See throwWhatMakesNoException().
    if (mode != ResumeMode::NormalMode && rt.slot(Runtime::Slot::ThrowArguments).isNull() && rt.slot(Runtime::Slot::ThrownException) == value)
        return raiseWhereItWaits();

    JSValue yielded;
    JSValue result;
    JSValue visible = iterator;
    if (auto* delegate = asSequenceDelegate(globalObject, iterator)) {
        visible = delegate->get(0);
        if (mode != ResumeMode::NormalMode)
            return raiseWhereItWaits();
        if (!isNone(value)) {
            scope.release();
            callMethod(globalObject, visible, rt.names.send(), value);
            return { };
        }
        iterator = delegate->get(1);
    }
    PyGenerator* generator = asGeneratorOrCoroutine(iterator);
    if (mode == ResumeMode::NormalMode) {
        if (generator)
            yielded = generatorSend(globalObject, generator, value, result);
        else if (isNone(value)) {
            PyType* type = rt.typeOf(iterator);
            JSValue method = type->lookup(Special::Next);
            if (!method || isNone(method))
                return raiseTypeError(globalObject, scope, '\'', type->name(), "' object is not an iterator"_s);
            yielded = isNextOfFutureIter(method) ? stepFutureIter(globalObject, iterator, result) : callSpecial(globalObject, method, iterator);
        } else
            yielded = callMethod(globalObject, iterator, rt.names.send(), value);
    } else {
        if (mode == throwAndClose && rt.typeOf(value)->isSubtypeOf(rt.GeneratorExit())) {
            closeDelegate(globalObject, iterator);
            RETURN_IF_EXCEPTION(scope, { });
            return raiseExceptionThrownIntoGenerator(globalObject, scope, value);
        }
        if (generator)
            yielded = generatorThrowWithMode(globalObject, generator, value, mode, result);
        else {
            JSValue method = getAttrIfPresent(globalObject, iterator, rt.names.throw_());
            RETURN_IF_EXCEPTION(scope, { });
            if (!method)
                return raiseWhereItWaits();
            // It is given what throw() was given, which need not be an exception made already.
            if (auto* given = PyTuple::tryFrom(rt.slot(Runtime::Slot::ThrowArguments)); given && rt.slot(Runtime::Slot::ThrownException) == value)
                yielded = call(globalObject, method, CallArgs(given->span()));
            else
                yielded = call(globalObject, method, value);
        }
    }

EXCEPTION_ASSERT(!scope.exception() || !yielded);
    if (yielded) {
        waiting->setDelegate(vm, visible);
        return yielded;
    }
    if (scope.exception()) {
        result = takeStopIterationValue(globalObject);
        if (!result) {
            // What throw() brings out of the delegate is raised anew here: its context is what this generator is handling, whatever
            // it was further in.
            if (mode == ResumeMode::NormalMode || !rt.handledExceptionInTopSlot())
                return { };
            JSValue exception = takePendingException(globalObject);
            return exception ? raiseException(globalObject, scope, exception) : JSValue();
        }
    }
    rt.setSendResult(result ? result : jsUndefined());
    return { };
}

JSValue getYieldFromIter(JSGlobalObject* globalObject, JSValue iterable, bool inCoroutine)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (auto* generator = PyGenerator::tryFrom(iterable)) {
        if (generator->kind() == PyGenerator::Kind::Generator)
            return iterable;
        if (generator->kind() == PyGenerator::Kind::Coroutine) {
            if (!inCoroutine)
                return raiseTypeError(globalObject, scope, "cannot 'yield from' a coroutine object in a non-coroutine generator"_s);
            return iterable;
        }
    }
    JSValue iterator = getIter(globalObject, iterable);
    RETURN_IF_EXCEPTION(scope, { });
    Runtime& rt = runtime(globalObject);
    PyType* type = rt.typeOf(iterable);
    if (type != rt.listType() && type != rt.tupleType() && type != rt.strType() && type != rt.bytesType())
        return iterator;
    PyRecord* delegate = PyRecord::create(globalObject, rt.typeFor(sequenceDelegateTypeDef), 2);
    RETURN_IF_EXCEPTION(scope, { });
    delegate->set(vm, 0, iterable);
    delegate->set(vm, 1, iterator);
    return delegate;
}

static bool isCoroutineLike(JSValue value)
{
    auto* generator = PyGenerator::tryFrom(value);
    return generator && (generator->kind() == PyGenerator::Kind::Coroutine || (generator->kind() == PyGenerator::Kind::Generator && generator->isIterableCoroutine));
}

// The iterator that `await object` runs. `hasAwait`: whether the object can be awaited at all.
static JSValue awaitableIterator(JSGlobalObject* globalObject, JSValue object, bool* hasAwait = nullptr)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (hasAwait)
        *hasAwait = true;
    if (isCoroutineLike(object))
        return object;
    PyType* type = rt.typeOf(object);
    JSValue method = type->lookup(Special::Await);
    if (!method) {
        if (hasAwait)
            *hasAwait = false;
        return raiseTypeError(globalObject, scope, '\'', type->name(), "' object can't be awaited"_s);
    }
    JSValue iterator = callSpecial(globalObject, method, object);
    RETURN_IF_EXCEPTION(scope, { });
    if (isCoroutineLike(iterator))
        return raiseTypeError(globalObject, scope, typeNameForMessage(globalObject, type), ".__await__() must return an iterator, not coroutine"_s);
    if (!isIterator(globalObject, iterator))
        return raiseTypeError(globalObject, scope, typeNameForMessage(globalObject, type), ".__await__() must return an iterator, not "_s, typeNameForMessage(globalObject, rt.typeOf(iterator)));
    return iterator;
}

// ---- anext(iterator, default) ----
//
// An awaitable around iterator.__anext__() that returns the default where that raises StopAsyncIteration.
// Fields: the awaitable, the default.

static JSValue anextAwaitableIterator(JSGlobalObject* globalObject, PyRecord* self)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue iterator = awaitableIterator(globalObject, self->get(0));
    RETURN_IF_EXCEPTION(scope, { });
    // Of what that returns, only coroutines have no __next__.
    if (auto* coroutine = PyGenerator::tryFrom(iterator); coroutine && coroutine->kind() == PyGenerator::Kind::Coroutine)
        RELEASE_AND_RETURN(scope, PyWrapperObject::create(globalObject, rt.coroutineWrapperType(), iterator));
    return iterator;
}

static JSValue anextAwaitableFinish(JSGlobalObject* globalObject, PyRecord* self, JSValue result)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (result)
        return result;
    if (!clearPendingExceptionIfMatches(globalObject, rt.StopAsyncIteration()))
        return { };
    return raiseStopIterationWithValue(globalObject, scope, self->get(1));
}

static JSValue anextAwaitableSelf(JSGlobalObject*, JSValue self) { return self; }

static JSValue anextAwaitableNext(JSGlobalObject* globalObject, JSValue selfValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* self = PyRecord::from(selfValue);
    JSValue iterator = anextAwaitableIterator(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue method = typeOf(globalObject, iterator)->lookup(Special::Next);
    JSValue result = callSpecial(globalObject, method, iterator);
    EXCEPTION_ASSERT(!!scope.exception() == !result);
    RELEASE_AND_RETURN(scope, anextAwaitableFinish(globalObject, self, result));
}

static JSValue anextAwaitableProxy(JSGlobalObject* globalObject, JSValue selfValue, JSString* name, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* self = PyRecord::from(selfValue);
    JSValue iterator = anextAwaitableIterator(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue method = getAttr(globalObject, iterator, name);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue result = call(globalObject, method, args);
    EXCEPTION_ASSERT(!!scope.exception() == !result);
    RELEASE_AND_RETURN(scope, anextAwaitableFinish(globalObject, self, result));
}

static JSValue anextAwaitableSend(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    return anextAwaitableProxy(globalObject, self, runtime(globalObject).names.send(), CallArgs(&value, 1));
}

static JSValue anextAwaitableThrow(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    return anextAwaitableProxy(globalObject, self, runtime(globalObject).names.throw_(), args);
}

static JSValue anextAwaitableClose(JSGlobalObject* globalObject, JSValue self)
{
    return anextAwaitableProxy(globalObject, self, runtime(globalObject).names.close(), CallArgs());
}

static constexpr NativeDef anextAwaitableMethods[] = {
    PY_SLOT("__await__", anextAwaitableSelf, "($self, /)"),
    PY_SLOT("__iter__", anextAwaitableSelf, "($self, /)"),
    PY_SLOT("__next__", anextAwaitableNext, "($self, /)"),
    PY_METHOD("send", anextAwaitableSend, "($self, value, /)"),
    PY_METHOD("throw", anextAwaitableThrow, "($self, *args)"),
    PY_METHOD("close", anextAwaitableClose, "($self, /)"),
};

static constexpr TypeDef anextAwaitableTypeDef { "anext_awaitable"_s, { }, anextAwaitableMethods, { }, { } };

JSValue makeANextAwaitable(JSGlobalObject* globalObject, JSValue awaitable, JSValue defaultValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* result = PyRecord::create(globalObject, runtime(globalObject).typeFor(anextAwaitableTypeDef), 2);
    RETURN_IF_EXCEPTION(scope, { });
    result->set(vm, 0, awaitable);
    result->set(vm, 1, defaultValue);
    return result;
}

JSValue getAwaitable(JSGlobalObject* globalObject, JSValue object, int where)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool hasAwait;
    JSValue iterator = awaitableIterator(globalObject, object, &hasAwait);
    if (failed(scope, iterator)) {
        if (!hasAwait && where) {
            takePendingException(globalObject);
            return raiseTypeError(globalObject, scope, "'async with' received an object from "_s, where == 1 ? "__aenter__"_s : "__aexit__"_s, " that does not implement __await__: "_s, typeName(globalObject, object));
        }
        return { };
    }
    if (auto* coroutine = PyGenerator::tryFrom(iterator); coroutine && coroutine->kind() == PyGenerator::Kind::Coroutine && coroutine->delegate() && !coroutine->isRunning())
        return raiseRuntimeError(globalObject, scope, "coroutine is being awaited already"_s);
    return iterator;
}

JSValue getAIter(JSGlobalObject* globalObject, JSValue object)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = rt.typeOf(object);
    JSValue method = type->lookup(Special::AIter);
    if (!method || isNone(method))
        return raiseTypeError(globalObject, scope, "'async for' requires an object with __aiter__ method, got "_s, type->name());
    JSValue iterator = callSpecial(globalObject, method, object);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue next = rt.typeOf(iterator)->lookup(Special::ANext);
    if (!next || isNone(next))
        return raiseTypeError(globalObject, scope, "'async for' received an object from __aiter__ that does not implement __anext__: "_s, typeName(globalObject, iterator));
    return iterator;
}

static JSValue asyncGeneratorANext(JSGlobalObject*, JSValue self);

JSValue getANext(JSGlobalObject* globalObject, JSValue iterator)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (auto* generator = PyGenerator::tryFrom(iterator); generator && generator->kind() == PyGenerator::Kind::AsyncGenerator)
        RELEASE_AND_RETURN(scope, asyncGeneratorANext(globalObject, iterator));
    PyType* type = rt.typeOf(iterator);
    JSValue method = type->lookup(Special::ANext);
    if (!method || isNone(method))
        return raiseTypeError(globalObject, scope, "'async for' requires an iterator with __anext__ method, got "_s, type->name());
    JSValue next = callSpecial(globalObject, method, iterator);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue awaitable = awaitableIterator(globalObject, next);
    if (!failed(scope, awaitable))
        return awaitable;
    JSValue cause = takePendingException(globalObject);
    PyException* error = newException(globalObject, rt.TypeError(), makeString("'async for' received an invalid object from __anext__: "_s, typeName(globalObject, next)));
    RETURN_IF_EXCEPTION(scope, { });
    raiseException(globalObject, scope, error);
    error->setCause(vm, cause);
    error->setSuppressContext(true);
    error->setContext(vm, cause);
    return { };
}

// ---- The methods generators and coroutines share ----

static ALWAYS_INLINE PyGenerator* generatorOf(JSValue value) { return PyGenerator::from(value); }

// The end of a generator, as a method reports it.
static JSValue raiseStop(JSGlobalObject* globalObject, PyGenerator* generator, JSValue result)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (generator->kind() == PyGenerator::Kind::AsyncGenerator)
        return raiseWithArgs(globalObject, scope, rt.StopAsyncIteration(), { });
    return raiseStopIteration(globalObject, scope, result);
}

static JSValue generatorSendMethod(JSGlobalObject* globalObject, JSValue selfValue, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue result;
    JSValue yielded = generatorSend(globalObject, generatorOf(selfValue), value, result);
    RETURN_IF_EXCEPTION(scope, { });
    if (yielded)
        return yielded;
    RELEASE_AND_RETURN(scope, raiseStop(globalObject, generatorOf(selfValue), result));
}

static JSValue generatorNext(JSGlobalObject* globalObject, JSValue selfValue) { return generatorSendMethod(globalObject, selfValue, jsUndefined()); }
static JSValue generatorIter(JSGlobalObject*, JSValue selfValue) { return selfValue; }
static JSValue generatorCloseMethod(JSGlobalObject* globalObject, JSValue selfValue) { return generatorCloseWithResult(globalObject, generatorOf(selfValue)); }

// The exception that throw(type[, value[, traceback]]) raises. Empty: the arguments are wrong. `makeNoException`: it is then set if there are as many
// as there may be, and they are not what an exception is made from.
static JSValue exceptionForThrow(JSGlobalObject* globalObject, ASCIILiteral function, const CallArgs& args, bool warnsOfSignature = true, bool* makeNoException = nullptr)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, rejectKeywords(globalObject, function, args)) || failed(scope, checkPositional(globalObject, function, args, 1, 3)))
        return { };
    if (args.size() > 1 && warnsOfSignature) {
        bool goOn = warn(globalObject, rt.DeprecationWarning(), makeString("the (type, exc, tb) signature of "_s, function, "() is deprecated, use the single-arg signature instead."_s));
        RETURN_IF_EXCEPTION(scope, { });
        if (!goOn)
            return { };
    }
    JSValue type = args[0];
    JSValue value = args.get(1);
    JSValue traceback = args.get(2);
    if (traceback && isNone(traceback))
        traceback = JSValue();
    bool ignored;
    bool& isWrong = makeNoException ? *makeNoException : ignored;
    isWrong = true;
    if (traceback && !PyTraceback::is(traceback))
        return raiseTypeError(globalObject, scope, "throw() third argument must be a traceback object"_s);

    JSValue exception;
    if (auto* cls = PyType::tryFrom(type); cls && cls->has(TypeFlag::ExceptionSubclass)) {
        // As `raise type(value)`, unless the value is such an exception already.
        if (value && PyException::is(value) && PyException::from(value)->type()->isSubtypeOf(cls))
            exception = value;
        else if (!value || isNone(value))
            exception = call(globalObject, cls);
        else if (auto* tuple = PyTuple::tryFrom(value))
            exception = call(globalObject, cls, CallArgs(tuple->span()));
        else
            exception = call(globalObject, cls, value);
        if (!scope.exception() && !PyException::is(exception))
            raiseTypeError(globalObject, scope, "calling "_s, reprForMessage(globalObject, cls), " should have returned an instance of BaseException, not "_s, typeName(globalObject, exception));
        // If it cannot be made, what says so is thrown in its place.
        if (scope.exception()) [[unlikely]] {
            exception = takePendingException(globalObject);
            if (!exception) {
                isWrong = false;
                return { };
            }
        }
    } else if (PyException::is(type)) {
        if (value && !isNone(value))
            return raiseTypeError(globalObject, scope, "instance exception may not have a separate value"_s);
        exception = type;
    } else
        return raiseTypeError(globalObject, scope, "exceptions must be classes or instances deriving from BaseException, not "_s, typeName(globalObject, type));
    isWrong = false;
    if (traceback)
        PyException::from(exception)->setTraceback(vm, traceback);
    return exception;
}

static JSValue throwInto(JSGlobalObject* globalObject, PyGenerator* generator, JSValue exception, ResumeMode mode)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue result;
    JSValue yielded = generatorThrowWithMode(globalObject, generator, exception, mode, result);
    RETURN_IF_EXCEPTION(scope, { });
    if (yielded)
        return yielded;
    RELEASE_AND_RETURN(scope, raiseStop(globalObject, generator, result));
}

// `arguments`: what throw() was given, for whom the generator waits for; null: that one is not asked.
static JSValue throwIntoWithArguments(JSGlobalObject* globalObject, PyGenerator* generator, JSValue exception, JSValue arguments)
{
    Runtime& rt = runtime(globalObject);
    JSValue outerException = rt.slot(Runtime::Slot::ThrownException);
    JSValue outerArguments = rt.slot(Runtime::Slot::ThrowArguments);
    rt.setSlot(Runtime::Slot::ThrownException, exception);
    rt.setSlot(Runtime::Slot::ThrowArguments, arguments);
    JSValue result = throwInto(globalObject, generator, exception, throwAndClose);
    rt.setSlot(Runtime::Slot::ThrownException, outerException);
    rt.setSlot(Runtime::Slot::ThrowArguments, outerArguments);
    return result;
}

// The arguments of throw() make no exception, which is what `error` says. As in CPython, a generator that waits for another hands them on as they are before it
// looks at them. The one that looks stays as it is, and what it raises is raised in the one that waits for it.
static JSValue throwWhatMakesNoException(JSGlobalObject* globalObject, PyGenerator* generator, const CallArgs& args, JSValue error)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (generator->isCompleted() && generator->kind() == PyGenerator::Kind::Coroutine)
        return raiseRuntimeError(globalObject, scope, "cannot reuse already awaited coroutine"_s);
    JSValue delegate = generator->isRunning() || generator->isCompleted() ? JSValue() : generator->delegate();
    JSValue method;
    PyGenerator* inner = delegate ? asGeneratorOrCoroutine(delegate) : nullptr;
    if (delegate && !inner) {
        method = getAttrIfPresent(globalObject, delegate, rt.names.throw_());
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (!inner && !method) {
        throwPythonException(globalObject, scope, error);
        return { };
    }
    JSValue yielded = inner ? throwWhatMakesNoException(globalObject, inner, args, error) : call(globalObject, method, args);
    EXCEPTION_ASSERT(!scope.exception() || !yielded);
    if (yielded)
        return yielded;
    JSValue raised = takePendingException(globalObject);
    if (!raised)
        return { };
    RELEASE_AND_RETURN(scope, throwIntoWithArguments(globalObject, generator, raised, jsNull()));
}

static JSValue generatorThrowMethod(JSGlobalObject* globalObject, JSValue selfValue, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool makeNoException = false;
    JSValue exception = exceptionForThrow(globalObject, "throw"_s, args, true, &makeNoException);
    if (scope.exception() && makeNoException) [[unlikely]] {
        JSValue error = takePendingException(globalObject);
        if (!error)
            return { };
        RELEASE_AND_RETURN(scope, throwWhatMakesNoException(globalObject, generatorOf(selfValue), args, error));
    }
    RETURN_IF_EXCEPTION(scope, { });
    if (!exception)
        return { };
    RELEASE_AND_RETURN(scope, throwIntoWithArguments(globalObject, generatorOf(selfValue), exception, PyTuple::create(globalObject, args.positional())));
}

static JSValue generatorRepr(JSGlobalObject* globalObject, JSValue selfValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyGenerator* generator = generatorOf(selfValue);
    JSString* qualname = str(globalObject, generator->qualname());
    RETURN_IF_EXCEPTION(scope, { });
    String text = qualname->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, makeStr(vm, makeString('<', generator->type()->name(), " object "_s, text, " at "_s, addressForRepr(globalObject, selfValue), '>')));
}

static JSValue generatorGetName(JSGlobalObject*, JSValue selfValue) { return generatorOf(selfValue)->name(); }
static JSValue generatorGetQualname(JSGlobalObject*, JSValue selfValue) { return generatorOf(selfValue)->qualname(); }

static void generatorSetName(JSGlobalObject* globalObject, JSValue selfValue, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!value || !isStr(value)) {
        raiseTypeError(globalObject, scope, "__name__ must be set to a string object"_s);
        return;
    }
    generatorOf(selfValue)->setName(vm, value);
}

static void generatorSetQualname(JSGlobalObject* globalObject, JSValue selfValue, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!value || !isStr(value)) {
        raiseTypeError(globalObject, scope, "__qualname__ must be set to a string object"_s);
        return;
    }
    generatorOf(selfValue)->setQualname(vm, value);
}

static JSValue generatorGetDelegate(JSGlobalObject*, JSValue selfValue)
{
    PyGenerator* generator = generatorOf(selfValue);
    JSValue delegate = generator->delegate();
    return delegate && !generator->isRunning() ? delegate : jsUndefined();
}

static JSValue generatorGetRunning(JSGlobalObject*, JSValue selfValue) { return jsBoolean(generatorOf(selfValue)->isRunning()); }
static JSValue generatorGetSuspended(JSGlobalObject*, JSValue selfValue) { return jsBoolean(generatorOf(selfValue)->state() > 0); }
static JSValue generatorGetCode(JSGlobalObject* globalObject, JSValue selfValue) { return codeOf(globalObject, generatorOf(selfValue)->body()); }
static JSValue generatorGetOrigin(JSGlobalObject*, JSValue selfValue)
{
    JSValue origin = generatorOf(selfValue)->origin();
    return origin ? origin : jsUndefined();
}
static JSValue asyncGeneratorGetRunning(JSGlobalObject*, JSValue selfValue) { return jsBoolean(generatorOf(selfValue)->isRunningAsync); }

static JSValue generatorGetFrame(JSGlobalObject* globalObject, JSValue selfValue)
{
    PyGenerator* generator = generatorOf(selfValue);
    if (generator->isCompleted())
        return jsUndefined();
    return PyFrame::of(globalObject, generator);
}

static JSValue generatorGetState(JSGlobalObject* globalObject, JSValue selfValue)
{
    PyGenerator* generator = generatorOf(selfValue);
    ASCIILiteral prefix = generator->kind() == PyGenerator::Kind::Generator ? "GEN_"_s : generator->kind() == PyGenerator::Kind::Coroutine ? "CORO_"_s : "AGEN_"_s;
    ASCIILiteral state = generator->isCompleted() ? "CLOSED"_s : generator->isRunning() ? "RUNNING"_s : generator->isStarted() ? "SUSPENDED"_s : "CREATED"_s;
    return internString(getVM(globalObject), makeString(prefix, state));
}

static constexpr NativeDef generatorMethods[] = {
    PY_SLOT("__iter__", generatorIter, "($self, /)"),
    PY_SLOT("__next__", generatorNext, "($self, /)"),
    PY_SLOT("__repr__", generatorRepr, "($self, /)"),
    PY_METHOD("send", generatorSendMethod, "($self, object, /)"),
    PY_METHOD("throw", generatorThrowMethod, "($self, *args)"),
    PY_METHOD("close", generatorCloseMethod, "($self, /)"),
    PY_CLASSMETHOD("__class_getitem__", classGetItem, "($type, object, /)"),
};

static constexpr NativeGetSet generatorGetSets[] = {
    { "__name__"_s, generatorGetName, generatorSetName },
    { "__qualname__"_s, generatorGetQualname, generatorSetQualname },
    { "gi_yieldfrom"_s, generatorGetDelegate },
    { "gi_running"_s, generatorGetRunning },
    { "gi_frame"_s, generatorGetFrame },
    { "gi_suspended"_s, generatorGetSuspended },
    { "gi_code"_s, generatorGetCode },
    { "gi_state"_s, generatorGetState },
};

// ---- Coroutines ----

static JSValue coroutineAwait(JSGlobalObject* globalObject, JSValue selfValue)
{
    return PyWrapperObject::create(globalObject, runtime(globalObject).coroutineWrapperType(), selfValue);
}

static constexpr NativeDef coroutineMethods[] = {
    PY_SLOT("__await__", coroutineAwait, "($self, /)"),
    PY_SLOT("__repr__", generatorRepr, "($self, /)"),
    PY_METHOD("send", generatorSendMethod, "($self, object, /)"),
    PY_METHOD("throw", generatorThrowMethod, "($self, *args)"),
    PY_METHOD("close", generatorCloseMethod, "($self, /)"),
    PY_CLASSMETHOD("__class_getitem__", classGetItem, "($type, object, /)"),
};

static constexpr NativeGetSet coroutineGetSets[] = {
    { "__name__"_s, generatorGetName, generatorSetName },
    { "__qualname__"_s, generatorGetQualname, generatorSetQualname },
    { "cr_await"_s, generatorGetDelegate },
    { "cr_running"_s, generatorGetRunning },
    { "cr_frame"_s, generatorGetFrame },
    { "cr_code"_s, generatorGetCode },
    { "cr_suspended"_s, generatorGetSuspended },
    { "cr_state"_s, generatorGetState },
    PY_MEMBER("cr_origin", generatorGetOrigin),
};

// What coroutine.__await__() returns: an iterator over the coroutine.
static ALWAYS_INLINE JSValue wrappedCoroutine(JSValue wrapper) { return PyWrapperObject::from(wrapper)->wrapped(); }
static JSValue coroutineWrapperNext(JSGlobalObject* globalObject, JSValue selfValue) { return generatorSendMethod(globalObject, wrappedCoroutine(selfValue), jsUndefined()); }
static JSValue coroutineWrapperSend(JSGlobalObject* globalObject, JSValue selfValue, JSValue value) { return generatorSendMethod(globalObject, wrappedCoroutine(selfValue), value); }
static JSValue coroutineWrapperThrow(JSGlobalObject* globalObject, JSValue selfValue, const CallArgs& args) { return generatorThrowMethod(globalObject, wrappedCoroutine(selfValue), args); }
static JSValue coroutineWrapperClose(JSGlobalObject* globalObject, JSValue selfValue) { return generatorCloseMethod(globalObject, wrappedCoroutine(selfValue)); }

static constexpr NativeDef coroutineWrapperMethods[] = {
    PY_SLOT("__iter__", generatorIter, "($self, /)"),
    PY_SLOT("__next__", coroutineWrapperNext, "($self, /)"),
    PY_METHOD("send", coroutineWrapperSend, "($self, value, /)"),
    PY_METHOD("throw", coroutineWrapperThrow, "($self, *args)"),
    PY_METHOD("close", coroutineWrapperClose, "($self, /)"),
};

// ---- Asynchronous generators ----
//
// The body yields both what it awaits gives it to pass up to the event loop and, wrapped, the values of its `yield`s.
// asend(), athrow() and aclose() return awaitables that run it up to the next of the latter.

JSValue wrapAsyncGeneratorValue(JSGlobalObject* globalObject, JSValue value)
{
    return PyWrapperObject::create(globalObject, runtime(globalObject).asyncGeneratorWrappedValueType(), value);
}

static bool isWrappedValue(JSGlobalObject* globalObject, JSValue value)
{
    auto* wrapper = PyWrapperObject::tryFrom(value);
    return wrapper && wrapper->type() == runtime(globalObject).asyncGeneratorWrappedValueType();
}

PyAsyncGenAwaitable::PyAsyncGenAwaitable(VM& vm, Structure* structure, PyType* type, Kind kind)
    : Base(vm, structure, type)
    , m_kind(kind)
{
}

PyAsyncGenAwaitable* PyAsyncGenAwaitable::create(JSGlobalObject* globalObject, Kind kind, PyGenerator* generator, JSValue value)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    PyType* type = kind == Kind::Send ? rt.asyncGeneratorASendType() : rt.asyncGeneratorAThrowType();
    auto* awaitable = new (NotNull, allocateCell<PyAsyncGenAwaitable>(vm)) PyAsyncGenAwaitable(vm, rt.structure<PyAsyncGenAwaitable>(), type, kind);
    awaitable->finishCreation(vm);
    awaitable->m_generator.set(vm, awaitable, generator);
    awaitable->m_value.set(vm, awaitable, value);
    // One that is dropped before it is awaited is warned of.
    rt.registerFinalizable(awaitable);
    return awaitable;
}

void PyAsyncGenAwaitable::setState(State state)
{
    m_state = state;
}

template<typename Visitor>
void PyAsyncGenAwaitable::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<PyAsyncGenAwaitable>(cell);
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_generator);
    visitor.append(thisObject->m_value);
}

DEFINE_VISIT_CHILDREN(PyAsyncGenAwaitable);

// The first time it is iterated, the event loop is told: sys.set_asyncgen_hooks().
static bool initializeHooks(JSGlobalObject* globalObject, PyGenerator* generator)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (generator->hooksInitialized)
        return true;
    generator->hooksInitialized = true;
    if (JSValue finalizer = rt.asyncGenFinalizer())
        generator->setFinalizer(vm, finalizer);
    if (JSValue firstIter = rt.asyncGenFirstIter()) {
        call(globalObject, firstIter, generator);
        RETURN_IF_EXCEPTION(scope, false);
    }
    return true;
}

static JSValue makeAwaitable(JSGlobalObject* globalObject, JSValue selfValue, PyAsyncGenAwaitable::Kind kind, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, initializeHooks(globalObject, generatorOf(selfValue))))
        return { };
    RELEASE_AND_RETURN(scope, PyAsyncGenAwaitable::create(globalObject, kind, generatorOf(selfValue), value));
}

static JSValue asyncGeneratorANext(JSGlobalObject* globalObject, JSValue selfValue) { return makeAwaitable(globalObject, selfValue, PyAsyncGenAwaitable::Kind::Send, jsUndefined()); }
static JSValue asyncGeneratorASend(JSGlobalObject* globalObject, JSValue selfValue, JSValue value) { return makeAwaitable(globalObject, selfValue, PyAsyncGenAwaitable::Kind::Send, value); }
static JSValue asyncGeneratorAClose(JSGlobalObject* globalObject, JSValue selfValue) { return makeAwaitable(globalObject, selfValue, PyAsyncGenAwaitable::Kind::Close, { }); }

static JSValue asyncGeneratorAThrow(JSGlobalObject* globalObject, JSValue selfValue, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, rejectKeywords(globalObject, "athrow"_s, args)) || failed(scope, checkPositional(globalObject, "athrow"_s, args, 1, 3)))
        return { };
    if (args.size() > 1 && failed(scope, warn(globalObject, runtime(globalObject).DeprecationWarning(), "the (type, exc, tb) signature of athrow() is deprecated, use the single-arg signature instead."_s)))
        return { };
    RELEASE_AND_RETURN(scope, makeAwaitable(globalObject, selfValue, PyAsyncGenAwaitable::Kind::Throw, PyTuple::create(globalObject, args.positional())));
}

// What came out of the generator, as the awaitable passes it on: a wrapped value ends the awaitable with that value.
static JSValue unwrapResult(JSGlobalObject* globalObject, PyGenerator* generator, JSValue result)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!result) {
        if (pendingExceptionMatches(globalObject, rt.StopAsyncIteration()) || pendingExceptionMatches(globalObject, rt.GeneratorExit()))
            generator->isClosed = true;
        generator->isRunningAsync = false;
        return { };
    }
    if (isWrappedValue(globalObject, result)) {
        generator->isRunningAsync = false;
        return raiseStopIterationWithValue(globalObject, scope, PyWrapperObject::from(result)->wrapped());
    }
    return result;
}

static ALWAYS_INLINE PyAsyncGenAwaitable* awaitableOf(JSValue value) { return PyAsyncGenAwaitable::from(value); }
using AwaitableState = PyAsyncGenAwaitable::State;

static JSValue asendSend(JSGlobalObject* globalObject, JSValue selfValue, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* awaitable = awaitableOf(selfValue);
    PyGenerator* generator = awaitable->generator();
    if (awaitable->state() == AwaitableState::Closed)
        return raiseRuntimeError(globalObject, scope, "cannot reuse already awaited __anext__()/asend()"_s);
    if (awaitable->state() == AwaitableState::Initial) {
        if (generator->isRunningAsync) {
            awaitable->setState(AwaitableState::Closed);
            return raiseRuntimeError(globalObject, scope, "anext(): asynchronous generator is already running"_s);
        }
        if (isNone(value))
            value = awaitable->value();
        awaitable->setState(AwaitableState::Iterating);
    }
    generator->isRunningAsync = true;
    JSValue result = generatorSendMethod(globalObject, generator, value);
    EXCEPTION_ASSERT(!!scope.exception() == !result);
    scope.release();
    result = unwrapResult(globalObject, generator, result);
    if (!result)
        awaitable->setState(AwaitableState::Closed);
    return result;
}

static JSValue asendNext(JSGlobalObject* globalObject, JSValue selfValue) { return asendSend(globalObject, selfValue, jsUndefined()); }

static JSValue asendThrow(JSGlobalObject* globalObject, JSValue selfValue, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* awaitable = awaitableOf(selfValue);
    PyGenerator* generator = awaitable->generator();
    if (awaitable->state() == AwaitableState::Closed)
        return raiseRuntimeError(globalObject, scope, "cannot reuse already awaited __anext__()/asend()"_s);
    if (awaitable->state() == AwaitableState::Initial) {
        if (generator->isRunningAsync) {
            awaitable->setState(AwaitableState::Closed);
            return raiseRuntimeError(globalObject, scope, "anext(): asynchronous generator is already running"_s);
        }
        awaitable->setState(AwaitableState::Iterating);
        generator->isRunningAsync = true;
    }
    JSValue result = generatorThrowMethod(globalObject, generator, args);
    EXCEPTION_ASSERT(!!scope.exception() == !result);
    scope.release();
    result = unwrapResult(globalObject, generator, result);
    if (!result) {
        generator->isRunningAsync = false;
        awaitable->setState(AwaitableState::Closed);
    }
    return result;
}

// close() for either kind of awaitable: `throwMethod` is its throw().
template<JSValue (*throwMethod)(JSGlobalObject*, JSValue, const CallArgs&)>
static JSValue awaitableClose(JSGlobalObject* globalObject, JSValue selfValue)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (awaitableOf(selfValue)->state() == AwaitableState::Closed)
        return jsUndefined();
    JSValue exit = rt.GeneratorExit();
    JSValue result = throwMethod(globalObject, selfValue, CallArgs(&exit, 1));
    if (!failed(scope, result))
        return raiseRuntimeError(globalObject, scope, "coroutine ignored GeneratorExit"_s);
    scope.release();
    if (clearPendingExceptionIfMatches(globalObject, rt.StopIteration()) || clearPendingExceptionIfMatches(globalObject, rt.StopAsyncIteration()) || clearPendingExceptionIfMatches(globalObject, rt.GeneratorExit()))
        return jsUndefined();
    return { };
}

static constexpr NativeDef asendMethods[] = {
    PY_SLOT("__await__", generatorIter, "($self, /)"),
    PY_SLOT("__iter__", generatorIter, "($self, /)"),
    PY_SLOT("__next__", asendNext, "($self, /)"),
    PY_METHOD("send", asendSend, "($self, value, /)"),
    PY_METHOD("throw", asendThrow, "($self, *args)"),
    PY_METHOD("close", awaitableClose<asendThrow>, "($self, /)"),
};

static JSValue raiseAlreadyRunning(JSGlobalObject* globalObject, PyAsyncGenAwaitable* awaitable)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    awaitable->setState(AwaitableState::Closed);
    return raiseRuntimeError(globalObject, scope, awaitable->kind() == PyAsyncGenAwaitable::Kind::Close ? "aclose"_s : "athrow"_s, "(): asynchronous generator is already running"_s);
}

static JSValue raiseIgnoredExit(JSGlobalObject* globalObject, PyAsyncGenAwaitable* awaitable)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    awaitable->generator()->isRunningAsync = false;
    awaitable->setState(AwaitableState::Closed);
    return raiseRuntimeError(globalObject, scope, "async generator ignored GeneratorExit"_s);
}

// aclose() is over when the generator is: that is no error.
static JSValue finishAThrow(JSGlobalObject* globalObject, PyAsyncGenAwaitable* awaitable)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    awaitable->generator()->isRunningAsync = false;
    awaitable->setState(AwaitableState::Closed);
    if (awaitable->kind() == PyAsyncGenAwaitable::Kind::Close && (clearPendingExceptionIfMatches(globalObject, rt.StopAsyncIteration()) || clearPendingExceptionIfMatches(globalObject, rt.GeneratorExit())))
        return raiseStopIteration(globalObject, scope);
    return { };
}

static JSValue athrowSend(JSGlobalObject* globalObject, JSValue selfValue, JSValue value)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* awaitable = awaitableOf(selfValue);
    PyGenerator* generator = awaitable->generator();
    bool isClose = awaitable->kind() == PyAsyncGenAwaitable::Kind::Close;
    if (awaitable->state() == AwaitableState::Closed)
        return raiseRuntimeError(globalObject, scope, "cannot reuse already awaited aclose()/athrow()"_s);
    if (generator->isCompleted()) {
        awaitable->setState(AwaitableState::Closed);
        return raiseStopIteration(globalObject, scope);
    }

    JSValue result;
    if (awaitable->state() == AwaitableState::Initial) {
        if (generator->isRunningAsync)
            RELEASE_AND_RETURN(scope, raiseAlreadyRunning(globalObject, awaitable));
        if (generator->isClosed) {
            awaitable->setState(AwaitableState::Closed);
            return raiseWithArgs(globalObject, scope, rt.StopAsyncIteration(), { });
        }
        if (!isNone(value))
            return raiseRuntimeError(globalObject, scope, "can't send non-None value to a just-started coroutine"_s);
        awaitable->setState(AwaitableState::Iterating);
        generator->isRunningAsync = true;

        JSValue exception;
        if (isClose) {
            generator->isClosed = true;
            exception = newException(globalObject, rt.GeneratorExit(), std::span<const JSValue> { });
        } else
            exception = exceptionForThrow(globalObject, "athrow"_s, CallArgs(PyTuple::from(awaitable->value())->span()), false);
        EXCEPTION_ASSERT(!!scope.exception() == !exception);
        if (exception)
            result = throwInto(globalObject, generator, exception, throwThrough);
    } else
        result = generatorSendMethod(globalObject, generator, value);
EXCEPTION_ASSERT(!!scope.exception() == !result);
    scope.release();
    if (isClose) {
        if (!result)
            RELEASE_AND_RETURN(scope, finishAThrow(globalObject, awaitable));
        if (isWrappedValue(globalObject, result))
            RELEASE_AND_RETURN(scope, raiseIgnoredExit(globalObject, awaitable));
        return result;
    }
    result = unwrapResult(globalObject, generator, result);
    if (failed(scope, result))
        RELEASE_AND_RETURN(scope, finishAThrow(globalObject, awaitable));
    return result;
}

static JSValue athrowNext(JSGlobalObject* globalObject, JSValue selfValue) { return athrowSend(globalObject, selfValue, jsUndefined()); }

static JSValue athrowThrow(JSGlobalObject* globalObject, JSValue selfValue, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* awaitable = awaitableOf(selfValue);
    PyGenerator* generator = awaitable->generator();
    if (awaitable->state() == AwaitableState::Closed)
        return raiseRuntimeError(globalObject, scope, "cannot reuse already awaited aclose()/athrow()"_s);
    if (awaitable->state() == AwaitableState::Initial) {
        if (generator->isRunningAsync)
            RELEASE_AND_RETURN(scope, raiseAlreadyRunning(globalObject, awaitable));
        awaitable->setState(AwaitableState::Iterating);
        generator->isRunningAsync = true;
    }
    JSValue result = generatorThrowMethod(globalObject, generator, args);
    EXCEPTION_ASSERT(!!scope.exception() == !result);
    scope.release();
    if (awaitable->kind() == PyAsyncGenAwaitable::Kind::Close) {
        if (!result)
            RELEASE_AND_RETURN(scope, finishAThrow(globalObject, awaitable));
        if (isWrappedValue(globalObject, result))
            RELEASE_AND_RETURN(scope, raiseIgnoredExit(globalObject, awaitable));
        return result;
    }
    result = unwrapResult(globalObject, generator, result);
    if (!result) {
        generator->isRunningAsync = false;
        awaitable->setState(AwaitableState::Closed);
    }
    return result;
}

static constexpr NativeDef athrowMethods[] = {
    PY_SLOT("__await__", generatorIter, "($self, /)"),
    PY_SLOT("__iter__", generatorIter, "($self, /)"),
    PY_SLOT("__next__", athrowNext, "($self, /)"),
    PY_METHOD("send", athrowSend, "($self, value, /)"),
    PY_METHOD("throw", athrowThrow, "($self, *args)"),
    PY_METHOD("close", awaitableClose<athrowThrow>, "($self, /)"),
};

static constexpr NativeDef asyncGeneratorMethods[] = {
    PY_SLOT("__aiter__", generatorIter, "($self, /)"),
    PY_SLOT("__anext__", asyncGeneratorANext, "($self, /)"),
    PY_SLOT("__repr__", generatorRepr, "($self, /)"),
    PY_METHOD("asend", asyncGeneratorASend, "($self, object, /)"),
    PY_METHOD("athrow", asyncGeneratorAThrow, "($self, *args)"),
    PY_METHOD("aclose", asyncGeneratorAClose, "($self, /)"),
    PY_CLASSMETHOD("__class_getitem__", classGetItem, "($type, object, /)"),
};

static constexpr NativeGetSet asyncGeneratorGetSets[] = {
    { "__name__"_s, generatorGetName, generatorSetName },
    { "__qualname__"_s, generatorGetQualname, generatorSetQualname },
    { "ag_await"_s, generatorGetDelegate },
    { "ag_frame"_s, generatorGetFrame },
    { "ag_code"_s, generatorGetCode },
    { "ag_suspended"_s, generatorGetSuspended },
    { "ag_state"_s, generatorGetState },
    PY_MEMBER("ag_running", asyncGeneratorGetRunning),
};

void initializeGeneratorTypes(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    auto define = [&](PyType* type, ASCIILiteral name, std::span<const NativeDef> methods, std::span<const NativeGetSet> getsets) {
        type->initializeBuiltin(globalObject, { name, { TypeFlag::HasWeakref }, methods, getsets, { } }, rt.typeType(), rt.objectType());
    };
    define(rt.generatorType(), "generator"_s, generatorMethods, generatorGetSets);
    define(rt.coroutineType(), "coroutine"_s, coroutineMethods, coroutineGetSets);
    define(rt.coroutineWrapperType(), "coroutine_wrapper"_s, coroutineWrapperMethods, { });
    define(rt.asyncGeneratorType(), "async_generator"_s, asyncGeneratorMethods, asyncGeneratorGetSets);
    define(rt.asyncGeneratorASendType(), "async_generator_asend"_s, asendMethods, { });
    define(rt.asyncGeneratorAThrowType(), "async_generator_athrow"_s, athrowMethods, { });
    define(rt.asyncGeneratorWrappedValueType(), "async_generator_wrapped_value"_s, { }, { });
}

} // namespace Python
} // namespace JSC
