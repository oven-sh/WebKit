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
#include "PythonGenerators.h"

#include "JSCInlines.h"
#include "PyInstance.h"
#include "PyObjects.h"
#include "PythonText.h"
#include "TopExceptionScope.h"

namespace JSC { namespace Python {

// What generatorResume() of builtins/GeneratorPrototype.js does.
static bool isThrowArguments(JSValue value) { return value.isCell() && value.asCell()->type() == JSCellButterflyType; }

static ASCIILiteral nameOf(GeneratorKind kind)
{
    return kind == GeneratorKind::Generator ? "generator"_s : kind == GeneratorKind::Coroutine ? "coroutine"_s : "async generator"_s;
}

// A StopIteration that gets out of a generator would look like the end of whatever is iterating it.
static bool wouldLookLikeTheEnd(JSGlobalObject* globalObject, GeneratorKind kind, JSValue raised)
{
    PyRealm* realm = globalObject->pyRealm();
    return isInstance(globalObject, raised, realm->typeStopIteration()) || (kind == GeneratorKind::AsyncGenerator && isInstance(globalObject, raised, realm->typeStopAsyncIteration()));
}

// What is raised in its place.
static JSObject* errorForStopIteration(JSGlobalObject* globalObject, GeneratorKind kind, JSValue raised)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    bool isStop = isInstance(globalObject, raised, realm->typeStopIteration());
    JSObject* error = createException(globalObject, realm->typeRuntimeError(), concatenate(nameOf(kind), isStop ? " raised StopIteration"_s : " raised StopAsyncIteration"_s));
    error->putDirect(vm, vm.pythonNames().private_cause, raised);
    error->putDirect(vm, vm.pythonNames().private_context, raised);
    error->putDirect(vm, vm.pythonNames().private_suppressContext, jsBoolean(true));
    return error;
}

JSValue resumeGenerator(JSGlobalObject* globalObject, JSGenerator* generator, JSValue sent, JSGenerator::ResumeMode mode, JSValue& returned)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    returned = { };

    auto field = [&] (JSGenerator::Field field) { return generator->internalField(static_cast<unsigned>(field)).get(); };
    auto setState = [&] (int32_t state) { generator->internalField(static_cast<unsigned>(JSGenerator::Field::State)).set(vm, generator, jsNumber(state)); };

    // What is about to be raised in it, if anything is.
    JSValue raisedInIt;
    int32_t state = field(JSGenerator::Field::State).asInt32();
    bool isSuspended = state != static_cast<int32_t>(JSGenerator::State::Executing) && state != static_cast<int32_t>(JSGenerator::State::Completed) && state != static_cast<int32_t>(JSGenerator::State::Init);
    if (mode == JSGenerator::ResumeMode::ThrowMode) {
        // _gen_throw()
        if (isThrowArguments(sent) && !checkThrowArguments(globalObject, sent, "throw"_s))
            return { };
        JSValue waitingOn = isSuspended ? generator->getDirect(vm, vm.pythonNames().private_yieldFrom) : JSValue();
        if (waitingOn && !waitingOn.isUndefined()) {
            // It is waiting on something, with `yield from` or `await`, and that is what is thrown into, with what throw() was given as it was given. This one is not woken for that. If what it is waiting on
            // yields, that is what this one yields, from where it is. Only if that has come to an end, one way or the other, does this one go on, and then it is sent what came of it: see yieldFromStep().
            setState(static_cast<int32_t>(JSGenerator::State::Executing));
            JSValue returnedToIt;
            bool isForThisOne = false;
            PyRealm* realm = globalObject->pyRealm();
            PyRealm::WaitingGenerator waiting { generator, state, vm.topCallFrame, true, realm->innermostWaitingGenerator() };
            realm->setInnermostWaitingGenerator(&waiting);
            JSValue yielded = stepIterator(globalObject, waitingOn, sent, true, returnedToIt, &isForThisOne);
            realm->setInnermostWaitingGenerator(waiting.outer);
            setState(state);
            Exception* raised = scope.exception();
            if (!raised && yielded)
                return yielded;
            if (raised && (isForThisOne || vm.isTerminationException(raised)))
                return { };
            JSValue thrownHere;
            if (isForThisOne && !raised) {
                // What it is waiting on cannot be thrown into, or has been closed. So it is thrown where that is waited on, if it is something that can be thrown. If not, this one is left waiting.
                thrownHere = sent;
                if (isThrowArguments(sent)) {
                    auto* packet = uncheckedDowncast<JSCellButterfly>(sent.asCell());
                    thrownHere = exceptionToThrow(globalObject, packet->get(0), packet->get(1), packet->get(2));
                    RETURN_IF_EXCEPTION(scope, { });
                }
            }
            auto* outcome = JSCellButterfly::create(vm, CopyOnWriteArrayWithContiguous, 2);
            outcome->setIndex(vm, 0, jsBoolean(raised || thrownHere));
            outcome->setIndex(vm, 1, raised ? raised->value() : thrownHere ? thrownHere : returnedToIt ? returnedToIt : jsUndefined());
            if (raised || thrownHere)
                raisedInIt = outcome->get(1);
            if (raised && !scope.tryClearException())
                return { };
            sent = outcome;
            mode = JSGenerator::ResumeMode::NormalMode;
        } else if (isThrowArguments(sent)) {
            // They are looked at before the generator is, and if they will not do it is left as it was.
            auto* packet = uncheckedDowncast<JSCellButterfly>(sent.asCell());
            sent = exceptionToThrow(globalObject, packet->get(0), packet->get(1), packet->get(2));
            RETURN_IF_EXCEPTION(scope, { });
        }
    }
    GeneratorKind kind = generatorKindOf(globalObject, generator);
    ASCIILiteral what = nameOf(kind);
    if (state == static_cast<int32_t>(JSGenerator::State::Executing))
        return raiseValueError(globalObject, scope, concatenate(what, " already executing"_s));
    if (state == static_cast<int32_t>(JSGenerator::State::Completed)) {
        if (kind == GeneratorKind::Coroutine)
            return raise(globalObject, scope, BuiltinType::RuntimeError, "cannot reuse already awaited coroutine"_s);
        if (mode == JSGenerator::ResumeMode::ThrowMode) {
            throwException(globalObject, scope, sent);
            return { };
        }
        returned = jsUndefined();
        return { };
    }
    if (state == static_cast<int32_t>(JSGenerator::State::Init) && mode == JSGenerator::ResumeMode::NormalMode && !isNone(sent))
        return raiseTypeError(globalObject, scope, concatenate("can't send non-None value to a just-started "_s, what));

    setState(static_cast<int32_t>(JSGenerator::State::Executing));
    MarkedArgumentBuffer arguments;
    arguments.append(generator);
    arguments.append(jsNumber(state));
    arguments.append(sent);
    arguments.append(jsNumber(static_cast<int32_t>(mode)));
    arguments.append(field(JSGenerator::Field::Frame));
    JSValue next = field(JSGenerator::Field::Next);

    // What it was handling when it yielded, it is handling again, and beyond that whatever is being handled here.
    PyRealm* realm = globalObject->pyRealm();
    auto& handledName = vm.pythonNames().private_handled;
    Exception* callersOwn = realm->ownHandledException();
    Exception* callersOuter = realm->outerHandledException();
    JSValue generatorsOwn = generator->getDirect(vm, handledName);
    realm->setOuterHandledException(vm, realm->handledThrown());
    realm->setOwnHandledException(vm, generatorsOwn && !isNone(generatorsOwn) ? uncheckedDowncast<Exception>(generatorsOwn.asCell()) : nullptr);
    // _PyErr_ChainStackItem(): what is thrown into it is raised while it is handling that, and has that for its __context__, whatever it had. It goes no further out than the generator.
    if (mode == JSGenerator::ResumeMode::ThrowMode)
        raisedInIt = sent;
    if (raisedInIt && raisedInIt.isObject() && realm->ownHandledException())
        setContext(globalObject, asObject(raisedInIt));

    JSValue value = JSC::call(globalObject, next, JSC::getCallData(next), field(JSGenerator::Field::This), arguments);

    Exception* nowHandling = realm->ownHandledException();
    if (nowHandling || generatorsOwn)
        generator->putDirect(vm, handledName, nowHandling ? JSValue(nowHandling) : jsUndefined());
    realm->setOwnHandledException(vm, callersOwn);
    realm->setOuterHandledException(vm, callersOuter);


    if (scope.exception()) [[unlikely]] {
        setState(static_cast<int32_t>(JSGenerator::State::Completed));
        // A StopIteration that gets out of a generator would look like the end of whatever is iterating it.
        Exception* exception = scope.exception();
        // What is thrown into one that has not begun is thrown before anything of it has run, and comes out as it went in.
        bool hadBegun = state != static_cast<int32_t>(JSGenerator::State::Init) || mode != JSGenerator::ResumeMode::ThrowMode;
        if (hadBegun && !vm.isTerminationException(exception) && wouldLookLikeTheEnd(globalObject, kind, exception->value())) {
            JSValue raised = exception->value();
            if (scope.tryClearException())
                throwException(globalObject, scope, errorForStopIteration(globalObject, kind, raised));
        }
        return { };
    }
    // If it yielded, it said where to go on from.
    if (field(JSGenerator::Field::State).asInt32() == static_cast<int32_t>(JSGenerator::State::Executing)) {
        setState(static_cast<int32_t>(JSGenerator::State::Completed));
        returned = value;
        return { };
    }
    return value;
}

JSValue stepIterator(JSGlobalObject* globalObject, JSValue iterator, JSValue received, bool wasThrown, JSValue& returned, bool* isForWhatWaits)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRealm* realm = globalObject->pyRealm();
    returned = { };
    auto* generator = iterator.isCell() && iterator.asCell()->type() == JSGeneratorType ? uncheckedDowncast<JSGenerator>(iterator.asCell()) : nullptr;
    auto finishCall = [&] (JSValue yielded) -> JSValue {
        if (!scope.exception())
            return yielded;
        returned = catchStopIteration(globalObject);
        return { };
    };

    // What was thrown is an exception, or what throw() was given, as it was given.
    JSValue thrownType = received;
    JSValue thrownValue;
    JSValue thrownTraceback;
    if (wasThrown && isThrowArguments(received)) {
        auto* packet = uncheckedDowncast<JSCellButterfly>(received.asCell());
        thrownType = packet->get(0);
        thrownValue = packet->get(1);
        thrownTraceback = packet->get(2);
    }
    // throw_here: it is thrown where the iterator is being waited on. Whether it can be thrown is for what is waiting to say, if it is asking, since if not it is left waiting.
    auto throwHere = [&] () -> JSValue {
        if (isForWhatWaits) {
            *isForWhatWaits = true;
            return { };
        }
        JSValue exception = received == thrownType ? received : exceptionToThrow(globalObject, thrownType, thrownValue, thrownTraceback);
        RETURN_IF_EXCEPTION(scope, { });
        throwException(globalObject, scope, exception);
        return { };
    };
    // PyErr_GivenExceptionMatches()
    bool isExit = wasThrown && (isClass(thrownType) ? asType(thrownType)->isSubtypeOf(realm->typeGeneratorExit()) : isInstance(globalObject, thrownType, realm->typeGeneratorExit()));

    if (isExit) {
        // gen_close_iter() is called with nothing linked in.
        if (isForWhatWaits)
            realm->innermostWaitingGenerator()->isLinked = false;
        // Whatever is iterating it is being closed, so it is too.
        if (generator)
            generatorClose(globalObject, generator);
        else {
            // gen_close_iter(): what goes wrong with asking whether it can be closed is nobody's to catch.
            JSValue close = getAttributeIfPresent(globalObject, iterator, Identifier::fromString(vm, "close"_s));
            if (scope.exception()) [[unlikely]] {
                reportUnraisableShowing(globalObject, "Exception ignored while closing generator"_s, iterator);
                RETURN_IF_EXCEPTION(scope, { });
                close = { };
            }
            if (close)
                call(globalObject, close);
        }
        RETURN_IF_EXCEPTION(scope, { });
        return throwHere();
    }

    if (generator)
        RELEASE_AND_RETURN(scope, resumeGenerator(globalObject, generator, received, wasThrown ? JSGenerator::ResumeMode::ThrowMode : JSGenerator::ResumeMode::NormalMode, returned));

    if (wasThrown) {
        JSValue method = getAttributeIfPresent(globalObject, iterator, Identifier::fromString(vm, "throw"_s));
        if (scope.exception()) [[unlikely]] {
            // Nothing has been thrown into anything, and what is waiting goes on waiting.
            if (isForWhatWaits)
                *isForWhatWaits = true;
            return { };
        }
        if (!method)
            return throwHere();
        MarkedArgumentBuffer arguments;
        arguments.append(thrownType);
        if (thrownValue)
            arguments.append(thrownValue);
        if (thrownValue && thrownTraceback)
            arguments.append(thrownTraceback);
        return finishCall(call(globalObject, method, arguments));
    }
    if (isNone(received)) {
        if (auto* native = tryIterator(iterator); native && !native->isOfDerivedClass()) {
            JSValue yielded = native->next(globalObject);
            // One that goes through another lets by what that one said as it ended.
            if (scope.exception()) [[unlikely]]
                return finishCall({ });
            if (!yielded)
                returned = jsUndefined();
            return yielded;
        }
        JSValue self;
        JSValue method = lookupSpecial(globalObject, iterator, vm.pythonNames().dunder_next, self);
        RETURN_IF_EXCEPTION(scope, { });
        if (!method)
            return raiseTypeError(globalObject, scope, concatenate('\'', typeName(globalObject, iterator), "' object is not an iterator"_s));
        return finishCall(callMethod(globalObject, method, self));
    }
    JSValue send = getAttribute(globalObject, iterator, Identifier::fromString(vm, "send"_s));
    RETURN_IF_EXCEPTION(scope, { });
    return finishCall(call(globalObject, send, received));
}

static JSValue raiseStopIteration(JSGlobalObject* globalObject, ThrowScope& scope, JSValue returned)
{
    return raise(globalObject, scope, BuiltinType::StopIteration, isNone(returned) ? JSValue() : returned);
}

JSValue generatorSend(JSGlobalObject* globalObject, JSGenerator* generator, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue returned;
    JSValue yielded = resumeGenerator(globalObject, generator, value, JSGenerator::ResumeMode::NormalMode, returned);
    RETURN_IF_EXCEPTION(scope, { });
    if (yielded)
        return yielded;
    return raiseStopIteration(globalObject, scope, returned);
}

JSValue generatorThrow(JSGlobalObject* globalObject, JSGenerator* generator, JSValue exception)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue returned;
    JSValue yielded = resumeGenerator(globalObject, generator, exception, JSGenerator::ResumeMode::ThrowMode, returned);
    RETURN_IF_EXCEPTION(scope, { });
    if (yielded)
        return yielded;
    return raiseStopIteration(globalObject, scope, returned);
}

bool isWrittenInPython(JSGenerator* generator)
{
    auto* body = dynamicDowncast<JSFunction>(generator->internalField(static_cast<unsigned>(JSGenerator::Field::Next)).get());
    return body && !body->isHostOrBuiltinFunction() && body->jsExecutable()->isPython();
}

JSValue generatorClose(JSGlobalObject* globalObject, JSGenerator* generator)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    int32_t state = generator->internalField(static_cast<unsigned>(JSGenerator::Field::State)).get().asInt32();
    if (state == static_cast<int32_t>(JSGenerator::State::Completed))
        return jsUndefined();
    if (state == static_cast<int32_t>(JSGenerator::State::Init)) {
        generator->internalField(static_cast<unsigned>(JSGenerator::Field::State)).set(vm, generator, jsNumber(static_cast<int32_t>(JSGenerator::State::Completed)));
        return jsUndefined();
    }
    JSValue returned;
    // How it is told to stop is up to the language that it is written in, whichever asks. One of JavaScript's is made to return where it is.
    if (!isWrittenInPython(generator)) {
        JSValue yielded = resumeGenerator(globalObject, generator, jsUndefined(), JSGenerator::ResumeMode::ReturnMode, returned);
        RETURN_IF_EXCEPTION(scope, { });
        if (yielded)
            return raise(globalObject, scope, BuiltinType::RuntimeError, "generator ignored GeneratorExit"_s);
        return jsUndefined();
    }
    JSObject* exit = createException(globalObject, globalObject->pyRealm()->typeGeneratorExit(), JSValue());
    JSValue yielded = resumeGenerator(globalObject, generator, exit, JSGenerator::ResumeMode::ThrowMode, returned);
    if (scope.exception()) {
        if (!catchException(globalObject, BuiltinType::GeneratorExit))
            catchException(globalObject, BuiltinType::StopIteration);
        RETURN_IF_EXCEPTION(scope, { });
        return jsUndefined();
    }
    if (yielded)
        return raise(globalObject, scope, BuiltinType::RuntimeError, generatorKindOf(globalObject, generator) == GeneratorKind::Coroutine ? "coroutine ignored GeneratorExit"_s : "generator ignored GeneratorExit"_s);
    return returned;
}

} // namespace Python

JSC_DEFINE_HOST_FUNCTION(pythonGeneratorNextSlow, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    return JSValue::encode(Python::iteratorNext(globalObject, callFrame->argument(0)));
}

JSC_DEFINE_HOST_FUNCTION(pythonGeneratorRaised, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue raised = callFrame->argument(1);
    auto kind = Python::generatorKindOf(globalObject, uncheckedDowncast<JSGenerator>(callFrame->argument(0).asCell()));
    if (Python::wouldLookLikeTheEnd(globalObject, kind, raised))
        raised = Python::errorForStopIteration(globalObject, kind, raised);
    throwException(globalObject, scope, raised);
    return { };
}

} // namespace JSC
