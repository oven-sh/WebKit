/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedLoweringInternal.h"

#if ENABLE(AOT)

#include "AOTLockedOperation.h"
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

JSC_DEFINE_JIT_OPERATION(operationAOTTypedDeleteByValSloppy, size_t, (Instance* instance, EncodedJSValue base, EncodedJSValue key))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSObject* object = JSValue::decode(base).toObject(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    auto property = JSValue::decode(key).toPropertyKey(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    bool isDeleted = JSCell::deleteProperty(object, globalObject, property);
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    if (!isDeleted && object->recordIfOnlyLockRejects(vm, property.impl(), true, true)) {
        throwTypeError(globalObject, scope, AOT::lockedOperationErrorMessage(vm, object, AOT::LockedOperation::Delete, property.impl()));
        OPERATION_RETURN(scope, 0);
    }
    OPERATION_RETURN(scope, isDeleted);
}

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
        OPERATION_RETURN(scope, JSValue::encode(resumeBytecodeGenerator(globalObject, vm, ofBytecode, mode, value)));
    throwTypeError(globalObject, scope, "|this| should be a generator"_s);
    OPERATION_RETURN(scope, encodedJSValue());
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedCreateIteratorResult, JSCell*, (Instance* instance, EncodedJSValue value, uint32_t isDone))
{
    return createIteratorResultObject(instance->globalObject, JSValue::decode(value), isDone);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedIsGeneratorSuspendedAtStart, size_t, (Instance*, JSCell* generator))
{
    return uncheckedDowncast<Typed::Generator>(generator)->isSuspendedAtStart();
}

namespace Typed {

using namespace B3;

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
    case InlineBuiltin::GeneratorStepStatus:
    case InlineBuiltin::IsUnmodifiedGenerator:
    case InlineBuiltin::DrivenIteratorMarker:
    case InlineBuiltin::DeclaredGenerator:
        return Writes::Nothing;
    case InlineBuiltin::SloppyThis:
    case InlineBuiltin::MakeIteratorResult:
        return Writes::RuntimeState;
    case InlineBuiltin::ArraySpeciesCreate:
    case InlineBuiltin::DeleteByValSloppy:
    case InlineBuiltin::GetByValWithThis:
    case InlineBuiltin::ToObject:
    case InlineBuiltin::ToPropertyKey:
    case InlineBuiltin::ResumeGenerator:
    case InlineBuiltin::StepGenerator:
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
        return booleanResult(AOT_TYPED_OPERATION(IsConstructor, in<Boxed>(arguments[0])), type);
    case InlineBuiltin::ArraySpeciesCreate:
        return ensure(AOT_TYPED_OPERATION(ArraySpeciesCreate, in<Boxed>(arguments[0]), asCell(in<Boxed>(arguments[1]))), type);
    case InlineBuiltin::DeleteByValSloppy:
        return booleanResult(AOT_TYPED_OPERATION(DeleteByValSloppy, in<Boxed>(arguments[0]), in<Boxed>(arguments[1])), type);
    case InlineBuiltin::GetByValWithThis:
        return ensure(AOT_TYPED_OPERATION(GetByValWithThis, in<Boxed>(arguments[0]), in<Boxed>(arguments[1]), in<Boxed>(arguments[2])), type);
    case InlineBuiltin::ToObject: {
        RELEASE_ASSERT(constant(1).opcode == Opcode::String);
        Boxed operand = in<Boxed>(arguments[0]);
        auto genericPath = [&] {
            return box(AOT_TYPED_OPERATION(ToObject, operand, word32(constant(1).string().index())));
        };
        return ensure(either<Boxed>(isCell(operand), [&] {
            return either<Boxed>(aboveOrEqual(loadCellType(asCell(operand)), word32(ObjectType)), [&] {
                return operand;
            }, genericPath);
        }, genericPath), type);
    }
    case InlineBuiltin::SloppyThis: {
        Boxed operand = in<Boxed>(arguments[0]);
        auto genericPath = [&] {
            return AOT_TYPED_OPERATION(SloppyThis, operand);
        };
        return ensure(either<Boxed>(isCell(operand), [&] {
            return either<Boxed>(aboveOrEqual(loadCellType(asCell(operand)), word32(ObjectType)), [&] {
                return operand;
            }, genericPath);
        }, genericPath), type);
    }
    case InlineBuiltin::ToPropertyKey:
        return ensure(AOT_TYPED_OPERATION(ToPropertyKey, in<Boxed>(arguments[0])), type);
    case InlineBuiltin::ResumeGenerator:
        RELEASE_ASSERT(constant(1).opcode == Opcode::Integer);
        return ensure(AOT_TYPED_OPERATION(ResumeGenerator, in<Boxed>(arguments[0]), word32(static_cast<uint32_t>(constant(1).integer())), in<Boxed>(arguments[2])), type);
    case InlineBuiltin::IsGeneratorSuspendedAtStart:
        return booleanResult(AOT_TYPED_OPERATION(IsGeneratorSuspendedAtStart, asCell(in<Boxed>(arguments[0]))), type);
    case InlineBuiltin::StepGenerator: {
        static constexpr Stub stubs[] = { Stub::StepGenerator, Stub::StepGeneratorUnmodifiedUnlessNext, Stub::StepGeneratorIfUnmodified, Stub::StepIfGenerator, Stub::StepIfUnmodifiedGenerator };
        static_assert(static_cast<unsigned>(Generator::StepCheck::IsUnmodifiedGenerator) + 1 == std::size(stubs) && static_cast<unsigned>(Generator::StepCheck::IsGenerator) == 3);
        RELEASE_ASSERT(constant(3).opcode == Opcode::Integer && static_cast<uint64_t>(constant(3).integer()) < std::size(stubs));
        return ensure(Boxed(callICStub(stubs[constant(3).integer()], Int64, { { in<Boxed>(arguments[0]).value(), argumentGPR(0) }, { in<Integer>(arguments[1]).value(), argumentGPR(1) }, { in<Boxed>(arguments[2]).value(), argumentGPR(2) } }, std::nullopt, builtinCallEffects(value))), type);
    }
    case InlineBuiltin::GeneratorStepStatus:
        return ensure(zeroExtended(Word32(m_out.load32(TypedPointer(m_heaps.root, m_out.add(m_instance, m_out.constIntPtr(Instance::offsetOfGeneratorStepStatus())))))), type, type);
    case InlineBuiltin::IsUnmodifiedGenerator: {
        if (!typeOf(arguments[0]).anyObject)
            return booleanResult(boolean(false), type);
        auto at = [&](LValue base, ptrdiff_t offset) {
            return TypedPointer(m_heaps.root, m_out.add(base, m_out.constIntPtr(offset)));
        };
        Boxed operand = in<Boxed>(arguments[0]);
        if (arguments.size() > 1) {
            LValue generator = asCell(operand).value();
            LValue hasItsPrototype = m_out.isZero32(m_out.load8ZeroExt32(at(generator, Generator::offsetOfHasOtherPrototype())));
            return booleanResult(Word32(m_out.bitAnd(hasItsPrototype, m_out.isNull(m_out.loadPtr(generator, m_heaps.JSObject_butterfly)))), type);
        }
        LBasicBlock isCellCase = m_out.newBlock();
        LBasicBlock isGenerator = m_out.newBlock();
        LBasicBlock hasItsPrototype = m_out.newBlock();
        LBasicBlock isOther = m_out.newBlock();
        Join join(m_out, Rep::Int32, Int32);
        m_out.branch(isCell(operand).value(), unsure(isCellCase), unsure(isOther));
        m_out.appendTo(isCellCase);
        LValue cell = asCell(operand).value();
        LValue classInfo = m_out.loadPtr(loadStructure(cell), m_heaps.Structure_classInfo);
        m_out.branch(m_out.equal(classInfo, m_out.loadPtr(at(loadImmutablePointer(Instance::offsetOfRuntimeTable()), offsetInTable(Entry::GeneratorClassInfo)))), unsure(isGenerator), unsure(isOther));
        m_out.appendTo(isGenerator);
        m_out.branch(m_out.isZero32(m_out.load8ZeroExt32(at(cell, Generator::offsetOfHasOtherPrototype()))), usually(hasItsPrototype), rarely(isOther));
        m_out.appendTo(hasItsPrototype);
        join.add(Word32(m_out.isNull(m_out.loadPtr(cell, m_heaps.JSObject_butterfly))));
        m_out.appendTo(isOther);
        join.add(boolean(false));
        return booleanResult(join.finish().as<Word32>(), type);
    }
    case InlineBuiltin::DeclaredGenerator:
        return valueResult(arguments[0], type);
    case InlineBuiltin::DrivenIteratorMarker:
        return ensure(Boxed(m_out.loadPtr(TypedPointer(m_heaps.root, m_out.add(loadImmutablePointer(Instance::offsetOfRuntimeTable()), m_out.constIntPtr(offsetInTable(Entry::DrivenIteratorMarker)))))), type);
    case InlineBuiltin::MakeIteratorResult: {
        constexpr size_t sizeOfResult = JSObject::offsetOfInlineStorage() + 2 * sizeof(EncodedJSValue);
        static_assert(JSValue::ValueTrue == (JSValue::ValueFalse | 1));
        Boxed iteratorValue = in<Boxed>(arguments[0]);
        LValue isDone = in<Word32>(arguments[1]).value();
        LBasicBlock slowPath = newColdBlock();
        Join join(m_out, Rep::Cell, Int64);
        LValue allocator = m_out.loadPtr(TypedPointer(m_heaps.root, m_out.add(loadImmutablePointer(Instance::offsetOfCellSpace()), m_out.constIntPtr(CompleteSubspace::offsetOfAllocatorForSizeStep() + MarkedSpace::sizeClassToIndex(sizeOfResult) * sizeof(Allocator)))));
        Emitter::branchUnless(m_out.notNull(allocator), slowPath);
        CellPointer result = allocateCell(allocator, sizeOfResult, slowPath);
        m_out.store64(loadMutable64(Instance::offsetOfIteratorResultHeader()), result.value(), m_heaps.JSCell_header);
        m_out.storePtr(m_out.intPtrZero, result.value(), m_heaps.JSObject_butterfly);
        m_out.store64(iteratorValue.value(), inlineProperty(result, iteratorResultObjectValuePropertyOffset));
        m_out.store64(m_out.bitOr(m_out.constInt64(JSValue::ValueFalse), m_out.zeroExt(isDone, Int64)), inlineProperty(result, iteratorResultObjectDonePropertyOffset));
        fenceAfterInitialization();
        join.add(result);
        m_out.appendTo(slowPath);
        join.add(AOT_TYPED_OPERATION(CreateIteratorResult, iteratorValue, Word32(isDone)));
        return ensure(join.finish(), m_program.at(m_program.types.object), type);
    }
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
