/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTGenerator.h"
#include "AOTInstance.h"
#include "AOTOperationArguments.h"
#include "AOTOperationScope.h"
#include "AOTStubsCoroutines.h"
#include "ArrayPrototypeInlines.h"
#include "GeneratorPrototype.h"
#include "IteratorOperations.h"
#include "JSCInlines.h"
#include "JSGenerator.h"

namespace JSC { namespace AOT {

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTIsConstructor, size_t, (Instance*, EncodedJSValue value))
{
    return JSValue::decode(value).isConstructor();
}

JSC_DEFINE_JIT_OPERATION(operationAOTArraySpeciesCreate, EncodedJSValue, (Instance* instance, EncodedJSValue encodedLength, JSCell* array))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTArraySpeciesCreate);
    uint64_t length = truncateDoubleToUint64(JSValue::decode(encodedLength).asNumber());
    std::pair<SpeciesConstructResult, JSObject*> speciesResult = speciesConstructArray(globalObject, cellArgument<JSObject>(array, Entry::operationAOTArraySpeciesCreate), length);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    if (speciesResult.first == SpeciesConstructResult::CreatedObject)
        OPERATION_RETURN(scope, JSValue::encode(speciesResult.second));
    if (length > std::numeric_limits<unsigned>::max()) [[unlikely]] {
        throwException(globalObject, scope, createRangeError(globalObject, ArrayInvalidLengthError));
        OPERATION_RETURN(scope, encodedJSValue());
    }
    OPERATION_RETURN(scope, JSValue::encode(constructEmptyArray(globalObject, nullptr, static_cast<unsigned>(length))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTGetByValWithThis, EncodedJSValue, (Instance* instance, EncodedJSValue encodedBase, EncodedJSValue thisValue, EncodedJSValue encodedProperty))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTGetByValWithThis);
    JSValue base = JSValue::decode(encodedBase);
    JSValue property = JSValue::decode(encodedProperty);
    PropertySlot slot(JSValue::decode(thisValue), PropertySlot::InternalMethodType::Get);
    if (property.isUInt32()) {
        uint32_t index = property.asUInt32();
        if (isJSString(base) && asString(base)->canGetIndex(index))
            OPERATION_RETURN(scope, JSValue::encode(asString(base)->getIndex(globalObject, index)));
        OPERATION_RETURN(scope, JSValue::encode(base.get(globalObject, index, slot)));
    }
    base.requireObjectCoercible(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    auto propertyKey = property.toPropertyKey(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    OPERATION_RETURN(scope, JSValue::encode(base.get(globalObject, propertyKey, slot)));
}

static JSValue resumeBytecodeGenerator(JSGlobalObject* globalObject, VM& vm, JSGenerator* generator, JSGenerator::ResumeMode mode, JSValue value)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto field = [&](JSGenerator::Field which) -> WriteBarrier<Unknown>& {
        return generator->internalField(which);
    };
    auto setState = [&](JSGenerator::State state) {
        field(JSGenerator::Field::State).setWithoutWriteBarrier(jsNumber(static_cast<int32_t>(state)));
    };
    int32_t state = field(JSGenerator::Field::State).get().asInt32();
    if (state == static_cast<int32_t>(JSGenerator::State::Executing)) {
        throwException(globalObject, scope, createGeneratorIsExecutingError(globalObject));
        return { };
    }
    if (state == static_cast<int32_t>(JSGenerator::State::Completed)) {
        if (mode == JSGenerator::ResumeMode::ThrowMode) {
            throwException(globalObject, scope, value);
            return { };
        }
        RELEASE_AND_RETURN(scope, createIteratorResultObject(globalObject, mode == JSGenerator::ResumeMode::ReturnMode ? value : jsUndefined(), true));
    }
    setState(JSGenerator::State::Executing);
    JSValue body = field(JSGenerator::Field::Next).get();
    MarkedArgumentBuffer arguments;
    arguments.append(generator);
    arguments.append(jsNumber(state));
    arguments.append(value);
    arguments.append(jsNumber(static_cast<int32_t>(mode)));
    arguments.append(field(JSGenerator::Field::Frame).get());
    ASSERT(!arguments.hasOverflowed());
    JSValue result = call(globalObject, body, JSC::getCallData(body), field(JSGenerator::Field::This).get(), arguments);
    if (scope.exception()) [[unlikely]] {
        setState(JSGenerator::State::Completed);
        return { };
    }
    bool isDone = field(JSGenerator::Field::State).get().asInt32() == static_cast<int32_t>(JSGenerator::State::Executing);
    if (isDone)
        setState(JSGenerator::State::Completed);
    RELEASE_AND_RETURN(scope, createIteratorResultObject(globalObject, result, isDone));
}

JSC_DEFINE_JIT_OPERATION(operationAOTResumeGenerator, EncodedJSValue, (Instance* instance, EncodedJSValue encodedGenerator, int32_t encodedMode, EncodedJSValue encodedValue))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTResumeGenerator);
    JSValue generator = JSValue::decode(encodedGenerator);
    JSValue value = JSValue::decode(encodedValue);
    auto mode = static_cast<ResumeMode>(encodedMode);
    if (auto* compiled = dynamicDowncast<Generator>(generator)) [[likely]]
        OPERATION_RETURN(scope, JSValue::encode(compiled->resume(globalObject, mode, value)));
    if (auto* bytecodeGenerator = dynamicDowncast<JSGenerator>(generator))
        OPERATION_RETURN(scope, JSValue::encode(resumeBytecodeGenerator(globalObject, vm, bytecodeGenerator, engineResumeModeOf(mode), value)));
    throwException(globalObject, scope, createNotAGeneratorError(globalObject));
    OPERATION_RETURN(scope, encodedJSValue());
}

template<ResumeMode mode>
static EncodedJSValue resumeGeneratorOfEitherKind(JSGlobalObject* globalObject, CallFrame* callFrame)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue generator = callFrame->thisValue();
    JSValue value = callFrame->argument(0);
    if (auto* compiled = dynamicDowncast<Generator>(generator))
        RELEASE_AND_RETURN(scope, JSValue::encode(compiled->resume(globalObject, mode, value)));
    if (auto* bytecodeGenerator = dynamicDowncast<JSGenerator>(generator))
        RELEASE_AND_RETURN(scope, JSValue::encode(resumeBytecodeGenerator(globalObject, vm, bytecodeGenerator, engineResumeModeOf(mode), value)));
    throwException(globalObject, scope, createNotAGeneratorError(globalObject));
    return { };
}

static JSC_DECLARE_HOST_FUNCTION(generatorNextOfEitherKind);
static JSC_DECLARE_HOST_FUNCTION(generatorReturnOfEitherKind);
static JSC_DECLARE_HOST_FUNCTION(generatorThrowOfEitherKind);

JSC_DEFINE_HOST_FUNCTION(generatorNextOfEitherKind, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    return resumeGeneratorOfEitherKind<ResumeMode::Next>(globalObject, callFrame);
}

JSC_DEFINE_HOST_FUNCTION(generatorReturnOfEitherKind, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    return resumeGeneratorOfEitherKind<ResumeMode::Return>(globalObject, callFrame);
}

JSC_DEFINE_HOST_FUNCTION(generatorThrowOfEitherKind, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    return resumeGeneratorOfEitherKind<ResumeMode::Throw>(globalObject, callFrame);
}

void installGeneratorMethodsForCompiledGenerators(JSGlobalObject* realm)
{
    VM& vm = realm->vm();
    JSObject* prototype = realm->generatorPrototype();
    auto install = [&](const Identifier& name, NativeFunction function) {
        JSFunction* method = JSFunction::create(vm, realm, 1, name.string(), function, ImplementationVisibility::Public);
        prototype->putDirect(vm, name, method, static_cast<unsigned>(PropertyAttribute::DontEnum));
    };
    install(vm.propertyNames->next, generatorNextOfEitherKind);
    install(vm.propertyNames->returnKeyword, generatorReturnOfEitherKind);
    install(vm.propertyNames->throwKeyword, generatorThrowOfEitherKind);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTIsGeneratorSuspendedAtStart, size_t, (Instance*, JSCell* cell))
{
    auto* generator = dynamicDowncast<Generator>(cell);
    return generator && generator->isSuspendedAtStart();
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
