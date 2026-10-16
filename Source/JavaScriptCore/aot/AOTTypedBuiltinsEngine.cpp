/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedLoweringInternal.h"

#if ENABLE(AOT)

#include "AOTLockWords.h"
#include "AOTOperationHelpers.h"
#include "AOTRuntime.h"
#include "AOTTypedObject.h"
#include "ArrayPrototypeInlines.h"
#include "BuiltinExecutables.h"
#include "IterationKind.h"
#include "IteratorOperations.h"
#include "JSArrayIterator.h"
#include "JSAsyncDisposableStack.h"
#include "JSCInlines.h"
#include "JSDisposableStack.h"
#include "JSGenerator.h"
#include "JSIteratorHelper.h"
#include "JSWrapForValidIterator.h"
#include "MathCommon.h"

namespace JSC { namespace AOT {

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedIsConstructor, size_t, (Instance*, EncodedJSValue value))
{
    return JSValue::decode(value).isConstructor();
}

// As slow_path_new_array_with_species.
JSC_DEFINE_JIT_OPERATION(operationAOTTypedArraySpeciesCreate, EncodedJSValue, (Instance* instance, EncodedJSValue encodedLength, JSCell* array))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    uint64_t length = truncateDoubleToUint64(JSValue::decode(encodedLength).asNumber());
    std::pair<SpeciesConstructResult, JSObject*> speciesResult = speciesConstructArray(globalObject, asObject(array), length);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    if (speciesResult.first == SpeciesConstructResult::CreatedObject)
        OPERATION_RETURN(scope, JSValue::encode(speciesResult.second));
    if (length > std::numeric_limits<unsigned>::max()) [[unlikely]] {
        throwException(globalObject, scope, createRangeError(globalObject, ArrayInvalidLengthError));
        OPERATION_RETURN(scope, encodedJSValue());
    }
    OPERATION_RETURN(scope, JSValue::encode(constructEmptyArray(globalObject, nullptr, static_cast<unsigned>(length))));
}

// As del_by_val of sloppy code.
JSC_DEFINE_JIT_OPERATION(operationAOTTypedDeleteByValSloppy, size_t, (Instance* instance, EncodedJSValue base, EncodedJSValue key))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSObject* object = JSValue::decode(base).toObject(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    auto property = JSValue::decode(key).toPropertyKey(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    bool isDeleted = JSCell::deleteProperty(object, globalObject, property);
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    if (!isDeleted && object->noteIfRefusedByLockAlone(vm, property.impl(), true, true)) {
        throwTypeError(globalObject, scope, AOT::wordsOfLock(vm, object, AOT::RefusedByLock::Delete, property.impl()));
        OPERATION_RETURN(scope, 0);
    }
    OPERATION_RETURN(scope, isDeleted);
}

// As slow_path_get_by_val_with_this.
JSC_DEFINE_JIT_OPERATION(operationAOTTypedGetByValWithThis, EncodedJSValue, (Instance* instance, EncodedJSValue encodedBase, EncodedJSValue thisValue, EncodedJSValue encodedKey))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSValue base = JSValue::decode(encodedBase);
    JSValue key = JSValue::decode(encodedKey);
    PropertySlot slot(JSValue::decode(thisValue), PropertySlot::InternalMethodType::Get);
    if (key.isUInt32()) {
        uint32_t index = key.asUInt32();
        if (isJSString(base) && asString(base)->canGetIndex(index))
            OPERATION_RETURN(scope, JSValue::encode(asString(base)->getIndex(globalObject, index)));
        OPERATION_RETURN(scope, JSValue::encode(base.get(globalObject, index, slot)));
    }
    base.requireObjectCoercible(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    auto property = key.toPropertyKey(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    OPERATION_RETURN(scope, JSValue::encode(base.get(globalObject, property, slot)));
}

// As slow_path_to_object.
JSC_DEFINE_JIT_OPERATION(operationAOTTypedToObject, JSCell*, (Instance* instance, EncodedJSValue encodedValue, uint32_t message))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSValue value = JSValue::decode(encodedValue);
    if (value.isUndefinedOrNull()) [[unlikely]] {
        if (const Identifier& text = instance->typed->name(vm, message); !text.isEmpty()) {
            throwException(globalObject, scope, createTypeError(globalObject, text.string()));
            OPERATION_RETURN(scope, nullptr);
        }
    }
    OPERATION_RETURN(scope, value.toObject(globalObject));
}

// OrdinaryCallBindThis of a function that is not strict, for what is no object.
JSC_DEFINE_JIT_OPERATION(operationAOTTypedSloppyThis, EncodedJSValue, (Instance* instance, EncodedJSValue encodedValue))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSValue value = JSValue::decode(encodedValue);
    if (value.isUndefinedOrNull())
        OPERATION_RETURN(scope, JSValue::encode(globalObject->globalThis()));
    OPERATION_RETURN(scope, JSValue::encode(value.toObject(globalObject)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedToPropertyKey, EncodedJSValue, (Instance* instance, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    OPERATION_RETURN(scope, JSValue::encode(JSValue::decode(value).toPropertyKeyValue(globalObject)));
}

// `next`, `return`, `throw` and `generatorResume` of builtins/GeneratorPrototype.js, for the generators that text evaluated at run time makes.
static JSValue resumeGeneratorOfBytecode(JSGlobalObject* globalObject, VM& vm, JSGenerator* generator, JSGenerator::ResumeMode mode, JSValue value)
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
        throwTypeError(globalObject, scope, "Generator is executing"_s);
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

JSC_DEFINE_JIT_OPERATION(operationAOTTypedResumeGenerator, EncodedJSValue, (Instance* instance, EncodedJSValue encodedGenerator, int32_t encodedMode, EncodedJSValue encodedValue))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSValue generator = JSValue::decode(encodedGenerator);
    JSValue value = JSValue::decode(encodedValue);
    auto mode = static_cast<JSGenerator::ResumeMode>(encodedMode);
    if (auto* compiled = dynamicDowncast<Typed::Generator>(generator)) [[likely]] {
        using ResumeMode = Typed::Generator::ResumeMode;
        OPERATION_RETURN(scope, JSValue::encode(compiled->resume(globalObject, mode == JSGenerator::ResumeMode::NormalMode ? ResumeMode::Next : mode == JSGenerator::ResumeMode::ReturnMode ? ResumeMode::Return : ResumeMode::Throw, value)));
    }
    if (auto* ofBytecode = dynamicDowncast<JSGenerator>(generator))
        OPERATION_RETURN(scope, JSValue::encode(resumeGeneratorOfBytecode(globalObject, vm, ofBytecode, mode, value)));
    throwTypeError(globalObject, scope, "|this| should be a generator"_s);
    OPERATION_RETURN(scope, encodedJSValue());
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedIsGeneratorSuspendedAtStart, size_t, (Instance*, JSCell* generator))
{
    return uncheckedDowncast<Typed::Generator>(generator)->isSuspendedAtStart();
}

namespace Typed {

using namespace B3;

// The family aot/builtins/engine.def: the engine's own builtins that are written in JavaScript.

#define ASSERT_NUMBER(code, number) static_assert(static_cast<unsigned>(BuiltinCodeIndex::code) == number);
FOR_EACH_AOT_TYPED_ENGINE_BUILTIN_FUNCTION(ASSERT_NUMBER)
#undef ASSERT_NUMBER

uint32_t visibilityInEngine(uint32_t builtinFunction)
{
    switch (builtinFunction) {
#define VISIBILITY_OF(name, functionName, overriddenName, length) \
    case static_cast<uint32_t>(BuiltinCodeIndex::name): \
        return static_cast<uint32_t>(s_##name##ImplementationVisibility);
    JSC_FOREACH_BUILTIN_CODE(VISIBILITY_OF)
#undef VISIBILITY_OF
    default:
        return static_cast<uint32_t>(ImplementationVisibility::Public);
    }
}

bool isStrictInEngine(uint32_t builtinFunction)
{
    return builtinFunction >= numberOfBuiltinCodes || s_JSCBuiltinSourceMetadata[builtinFunction].isInStrictContext;
}

#define AOT_TYPED_CHECK_CONSTANT(number, expression) static_assert(static_cast<int64_t>(expression) == number);
FOR_EACH_AOT_TYPED_ENGINE_CONSTANT(AOT_TYPED_CHECK_CONSTANT)
#undef AOT_TYPED_CHECK_CONSTANT

std::optional<Writes> writesOfInlineEngine(InlineBuiltin which, std::span<const Type>)
{
    switch (which) {
    case InlineBuiltin::IsConstructor:
    case InlineBuiltin::IsGeneratorSuspendedAtStart:
        return Writes::Nothing;
    case InlineBuiltin::SloppyThis:
        return Writes::RuntimeState;
    case InlineBuiltin::ArraySpeciesCreate:
    case InlineBuiltin::DeleteByValSloppy:
    case InlineBuiltin::GetByValWithThis:
    case InlineBuiltin::ToObject:
    case InlineBuiltin::ToPropertyKey:
    case InlineBuiltin::ResumeGenerator:
        return Writes::Heap;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

std::optional<Operand> Lowering::lowerInlineEngine(InlineBuiltin which, ValueID result, const Value& value, Writes)
{
    auto& arguments = value.arguments;
    auto& type = typeOf(result);
    auto constant = [&](unsigned index) -> const Value& {
        ValueID id = arguments[index];
        while (m_function.at(id).opcode == Opcode::Cast)
            id = m_function.at(id).child(0);
        return m_function.at(id);
    };

    switch (which) {
    case InlineBuiltin::IsConstructor:
        return yieldBoolean(AOT_TYPED_OPERATION(IsConstructor, in<Boxed>(arguments[0])), type);
    case InlineBuiltin::ArraySpeciesCreate:
        return ensure(AOT_TYPED_OPERATION(ArraySpeciesCreate, in<Boxed>(arguments[0]), asCell(in<Boxed>(arguments[1]))), type);
    case InlineBuiltin::DeleteByValSloppy:
        return yieldBoolean(AOT_TYPED_OPERATION(DeleteByValSloppy, in<Boxed>(arguments[0]), in<Boxed>(arguments[1])), type);
    case InlineBuiltin::GetByValWithThis:
        return ensure(AOT_TYPED_OPERATION(GetByValWithThis, in<Boxed>(arguments[0]), in<Boxed>(arguments[1]), in<Boxed>(arguments[2])), type);
    case InlineBuiltin::ToObject: {
        RELEASE_ASSERT(constant(1).opcode == Opcode::String);
        Boxed operand = in<Boxed>(arguments[0]);
        auto generally = [&] {
            return box(AOT_TYPED_OPERATION(ToObject, operand, word32(constant(1).string().index())));
        };
        return ensure(either<Boxed>(isCell(operand), [&] {
            return either<Boxed>(aboveOrEqual(typeOfCell(asCell(operand)), word32(ObjectType)), [&] {
                return operand;
            }, generally);
        }, generally), type);
    }
    case InlineBuiltin::SloppyThis: {
        Boxed operand = in<Boxed>(arguments[0]);
        auto generally = [&] {
            return AOT_TYPED_OPERATION(SloppyThis, operand);
        };
        return ensure(either<Boxed>(isCell(operand), [&] {
            return either<Boxed>(aboveOrEqual(typeOfCell(asCell(operand)), word32(ObjectType)), [&] {
                return operand;
            }, generally);
        }, generally), type);
    }
    case InlineBuiltin::ToPropertyKey:
        return ensure(AOT_TYPED_OPERATION(ToPropertyKey, in<Boxed>(arguments[0])), type);
    case InlineBuiltin::ResumeGenerator:
        RELEASE_ASSERT(constant(1).opcode == Opcode::Integer);
        return ensure(AOT_TYPED_OPERATION(ResumeGenerator, in<Boxed>(arguments[0]), word32(static_cast<uint32_t>(constant(1).integer())), in<Boxed>(arguments[2])), type);
    case InlineBuiltin::IsGeneratorSuspendedAtStart:
        return yieldBoolean(AOT_TYPED_OPERATION(IsGeneratorSuspendedAtStart, asCell(in<Boxed>(arguments[0]))), type);
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
