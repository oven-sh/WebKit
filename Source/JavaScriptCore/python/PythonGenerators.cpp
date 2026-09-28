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
#include "TopExceptionScope.h"

namespace JSC { namespace Python {

// What generatorResume() of builtins/GeneratorPrototype.js does.
JSValue resumeGenerator(JSGlobalObject* globalObject, JSGenerator* generator, JSValue sent, JSGenerator::ResumeMode mode, JSValue& returned)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    returned = { };

    auto field = [&] (JSGenerator::Field field) { return generator->internalField(static_cast<unsigned>(field)).get(); };
    auto setState = [&] (int32_t state) { generator->internalField(static_cast<unsigned>(JSGenerator::Field::State)).set(vm, generator, jsNumber(state)); };

    int32_t state = field(JSGenerator::Field::State).asInt32();
    GeneratorKind kind = generatorKindOf(globalObject, generator);
    ASCIILiteral what = kind == GeneratorKind::Generator ? "generator"_s : kind == GeneratorKind::Coroutine ? "coroutine"_s : "async generator"_s;
    if (state == static_cast<int32_t>(JSGenerator::State::Executing))
        return raiseValueError(globalObject, scope, makeString(what, " already executing"_s));
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
        return raiseTypeError(globalObject, scope, makeString("can't send non-None value to a just-started "_s, what));

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
        bool isStop = !vm.isTerminationException(exception) && isInstance(globalObject, exception->value(), globalObject->pyRealm()->typeStopIteration());
        bool isAsyncStop = !isStop && kind == GeneratorKind::AsyncGenerator && !vm.isTerminationException(exception) && isInstance(globalObject, exception->value(), globalObject->pyRealm()->typeStopAsyncIteration());
        if (isStop || isAsyncStop) {
            JSValue cause = exception->value();
            if (scope.tryClearException()) {
                JSObject* error = createException(globalObject, globalObject->pyRealm()->typeRuntimeError(), makeString(what, isStop ? " raised StopIteration"_s : " raised StopAsyncIteration"_s));
                error->putDirect(vm, vm.pythonNames().private_cause, cause);
                error->putDirect(vm, vm.pythonNames().private_context, cause);
                error->putDirect(vm, vm.pythonNames().private_suppressContext, jsBoolean(true));
                throwException(globalObject, scope, error);
            }
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

JSValue stepIterator(JSGlobalObject* globalObject, JSValue iterator, JSValue received, bool wasThrown, JSValue& returned)
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

    if (wasThrown && isInstance(globalObject, received, realm->typeGeneratorExit())) {
        // Whatever is iterating it is being closed, so it is too.
        if (generator)
            generatorClose(globalObject, generator);
        else {
            JSValue close = getAttributeIfPresent(globalObject, iterator, Identifier::fromString(vm, "close"_s));
            RETURN_IF_EXCEPTION(scope, { });
            if (close)
                call(globalObject, close);
        }
        RETURN_IF_EXCEPTION(scope, { });
        throwException(globalObject, scope, received);
        return { };
    }

    if (generator)
        RELEASE_AND_RETURN(scope, resumeGenerator(globalObject, generator, received, wasThrown ? JSGenerator::ResumeMode::ThrowMode : JSGenerator::ResumeMode::NormalMode, returned));

    if (wasThrown) {
        JSValue method = getAttributeIfPresent(globalObject, iterator, Identifier::fromString(vm, "throw"_s));
        RETURN_IF_EXCEPTION(scope, { });
        if (!method) {
            throwException(globalObject, scope, received);
            return { };
        }
        return finishCall(call(globalObject, method, received));
    }
    if (isNone(received)) {
        if (auto* native = tryIterator(iterator); native && !native->isOfDerivedClass()) {
            JSValue yielded = native->next(globalObject);
            RETURN_IF_EXCEPTION(scope, { });
            if (!yielded)
                returned = jsUndefined();
            return yielded;
        }
        JSValue self;
        JSValue method = lookupSpecial(globalObject, iterator, vm.pythonNames().dunder_next, self);
        RETURN_IF_EXCEPTION(scope, { });
        if (!method)
            return raiseTypeError(globalObject, scope, makeString('\'', typeName(globalObject, iterator), "' object is not an iterator"_s));
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

} } // namespace JSC::Python
