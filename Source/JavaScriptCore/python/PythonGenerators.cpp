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
    if (state == static_cast<int32_t>(JSGenerator::State::Executing))
        return raiseValueError(globalObject, scope, "generator already executing"_s);
    if (state == static_cast<int32_t>(JSGenerator::State::Completed)) {
        if (mode == JSGenerator::ResumeMode::ThrowMode) {
            throwException(globalObject, scope, sent);
            return { };
        }
        returned = jsUndefined();
        return { };
    }
    if (state == static_cast<int32_t>(JSGenerator::State::Init) && mode == JSGenerator::ResumeMode::NormalMode && !isNone(sent))
        return raiseTypeError(globalObject, scope, "can't send non-None value to a just-started generator"_s);

    setState(static_cast<int32_t>(JSGenerator::State::Executing));
    MarkedArgumentBuffer arguments;
    arguments.append(generator);
    arguments.append(jsNumber(state));
    arguments.append(sent);
    arguments.append(jsNumber(static_cast<int32_t>(mode)));
    arguments.append(field(JSGenerator::Field::Frame));
    JSValue next = field(JSGenerator::Field::Next);
    JSValue value = JSC::call(globalObject, next, JSC::getCallData(next), field(JSGenerator::Field::This), arguments);

    if (scope.exception()) [[unlikely]] {
        setState(static_cast<int32_t>(JSGenerator::State::Completed));
        // A StopIteration that gets out of a generator would look like the end of whatever is iterating it.
        Exception* exception = scope.exception();
        if (!vm.isTerminationException(exception) && isInstance(globalObject, exception->value(), globalObject->pyRealm()->typeStopIteration())) {
            JSValue cause = exception->value();
            if (scope.tryClearException()) {
                JSObject* error = createException(globalObject, globalObject->pyRealm()->typeRuntimeError(), "generator raised StopIteration"_str);
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

void generatorClose(JSGlobalObject* globalObject, JSGenerator* generator)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    int32_t state = generator->internalField(static_cast<unsigned>(JSGenerator::Field::State)).get().asInt32();
    if (state == static_cast<int32_t>(JSGenerator::State::Completed))
        return;
    if (state == static_cast<int32_t>(JSGenerator::State::Init)) {
        generator->internalField(static_cast<unsigned>(JSGenerator::Field::State)).set(vm, generator, jsNumber(static_cast<int32_t>(JSGenerator::State::Completed)));
        return;
    }
    JSObject* exit = createException(globalObject, globalObject->pyRealm()->typeGeneratorExit(), JSValue());
    JSValue returned;
    JSValue yielded = resumeGenerator(globalObject, generator, exit, JSGenerator::ResumeMode::ThrowMode, returned);
    if (scope.exception()) {
        if (!catchException(globalObject, BuiltinType::GeneratorExit))
            catchException(globalObject, BuiltinType::StopIteration);
        return;
    }
    if (yielded)
        raise(globalObject, scope, BuiltinType::RuntimeError, "generator ignored GeneratorExit"_s);
}

} } // namespace JSC::Python
