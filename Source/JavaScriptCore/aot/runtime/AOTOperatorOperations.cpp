/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTBuiltinOperationSupport.h"
#include "AOTChainDependents.h"
#include "AOTClosure.h"
#include "AOTCompiledCall.h"
#include "AOTCompiledCallee.h"
#include "AOTGrowingText.h"
#include "AOTInstance.h"
#include "AOTObject.h"
#include "AOTOperationData.h"
#include "AOTOperationScope.h"
#include "AOTPrimitives.h"
#include "AOTPropertyCaches.h"
#include "AOTPropertyCachesKeys.h"
#include "AOTRuntimeFunctions.h"
#include "AOTRuntimeTable.h"
#include "AOTState.h"
#include "AOTTextsOfFewDigits.h"
#include "AOTBigIntArithmetic.h"
#include "DateInstance.h"
#include "DatePrototype.h"
#include "FunctionPrototype.h"
#include "JSBigInt.h"
#include "JSCInlines.h"
#include "MathCommon.h"
#include "NumberPrototype.h"
#include "NumericStrings.h"
#include "Operations.h"
#include "TypeofType.h"

namespace JSC { namespace AOT {

static ALWAYS_INLINE const Slot* findReadWithoutCode(Instance* instance, Object& object, const Identifier& name)
{
    const Slot* slot = instance->state->propertyCaches->findByKey(object.structure(), name.impl(), SlotAccess::Read);
    if (!slot) [[unlikely]]
        fileReadWithoutCode(instance, &object, name);
    return slot;
}

static ALWAYS_INLINE bool isKnownAbsent(Instance* instance, Object& object, const Identifier& name)
{
    const Slot* slot = findReadWithoutCode(instance, object, name);
    return slot && slot->kind == Slot::Kind::Absent;
}

static ALWAYS_INLINE JSCell* inheritedMethodIn(const Slot& method)
{
    if (method.kind != Slot::Kind::Value || method.has(Slot::isOwn))
        return nullptr;
    JSValue function = valueAtOffset(asObject(method.holder), method.offsetInStorage());
    return function.isCell() ? function.asCell() : nullptr;
}

static ALWAYS_INLINE JSCell* findInheritedMethod(Instance* instance, Object& object, const Identifier& name)
{
    const Slot* method = findReadWithoutCode(instance, object, name);
    return method ? inheritedMethodIn(*method) : nullptr;
}

static JSValue primitiveFromValueOf(JSGlobalObject* globalObject, JSObject* object)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue function = object->get(globalObject, vm.propertyNames->valueOf);
    RETURN_IF_EXCEPTION(scope, { });
    CallData callData = JSC::getCallData(function);
    if (callData.type != CallData::Type::None) {
        JSValue result = call(globalObject, function, callData, object, ArgList());
        RETURN_IF_EXCEPTION(scope, { });
        if (!result.isObject())
            return result;
    }
    throwTypeError(globalObject, scope, "No default value"_s);
    return { };
}

static ALWAYS_INLINE JSValue tryPrimitiveByString(Instance* instance, Object& object, JSCell* method)
{
    VM& vm = *instance->vm;
    JSGlobalObject* globalObject = instance->globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!method)
        return { };
    if (CallData callData = compiledCallDataOf(method); callData.type == CallData::Type::Compiled)
        RELEASE_AND_RETURN(scope, callCompiledCode(vm, asObject(method), callData, &object, nullptr, ArgList()));
    if (method == globalObject->objectProtoToStringFunction() && isKnownAbsent(instance, object, vm.propertyNames->toStringTagSymbol))
        return vm.smallStrings.objectObjectString();
    return { };
}

static NEVER_INLINE JSValue tryPrimitiveByHint(Instance* instance, Object& object, const Slot& exotic, PreferredPrimitiveType hint)
{
    VM& vm = *instance->vm;
    JSGlobalObject* globalObject = instance->globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSCell* method = inheritedMethodIn(exotic);
    CallData callData = method ? compiledCallDataOf(method) : CallData();
    if (callData.type != CallData::Type::Compiled)
        return { };
    JSString* name = hint == PreferString ? vm.smallStrings.stringString() : hint == PreferNumber ? vm.smallStrings.numberString() : vm.smallStrings.defaultString();
    std::array<EncodedJSValue, 1> arguments { JSValue::encode(name) };
    JSValue result = callCompiledCode(vm, asObject(method), callData, &object, nullptr, ArgList { arguments.data(), arguments.size() });
    RETURN_IF_EXCEPTION(scope, { });
    if (result.isObject()) [[unlikely]] {
        throwTypeError(globalObject, scope, "Symbol.toPrimitive returned an object"_s);
        return { };
    }
    return result;
}

static ALWAYS_INLINE JSValue tryPrimitiveOfCompiledObject(Instance* instance, Object& object, PreferredPrimitiveType hint)
{
    ASSERT(hint != PreferString);
    VM& vm = *instance->vm;
    JSGlobalObject* globalObject = instance->globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    const Slot* exotic = findReadWithoutCode(instance, object, vm.propertyNames->toPrimitiveSymbol);
    if (!exotic)
        return { };
    if (exotic->kind != Slot::Kind::Absent)
        RELEASE_AND_RETURN(scope, tryPrimitiveByHint(instance, object, *exotic, hint));
    JSCell* valueOf = findInheritedMethod(instance, object, vm.propertyNames->valueOf);
    if (!valueOf)
        return { };
    if (valueOf != globalObject->objectProtoValueOfFunction()) {
        CallData callData = compiledCallDataOf(valueOf);
        if (callData.type != CallData::Type::Compiled)
            return { };
        JSValue result = callCompiledCode(vm, asObject(valueOf), callData, &object, nullptr, ArgList());
        RETURN_IF_EXCEPTION(scope, { });
        if (!result.isObject()) [[likely]]
            return result;
        JSValue function = object.get(globalObject, vm.propertyNames->toString);
        RETURN_IF_EXCEPTION(scope, { });
        if (CallData callDataOfFunction = JSC::getCallData(function); callDataOfFunction.type != CallData::Type::None) {
            result = call(globalObject, function, callDataOfFunction, &object, ArgList());
            RETURN_IF_EXCEPTION(scope, { });
            if (!result.isObject())
                return result;
        }
        throwTypeError(globalObject, scope, "No default value"_s);
        return { };
    }
    JSValue result = tryPrimitiveByString(instance, object, findInheritedMethod(instance, object, vm.propertyNames->toString));
    RETURN_IF_EXCEPTION(scope, { });
    if (result && result.isObject()) [[unlikely]] {
        throwTypeError(globalObject, scope, "No default value"_s);
        return { };
    }
    return result;
}

static ALWAYS_INLINE bool isCompiledObject(JSValue value)
{
    return value.isCell() && value.asCell()->type() == CompiledObjectType;
}

NEVER_INLINE JSValue primitiveIfKnown(Instance* instance, JSValue object, PreferredPrimitiveType hint)
{
    JSValue primitive = tryPrimitiveOfCompiledObject(instance, *uncheckedDowncast<Object>(object.asCell()), hint);
    return primitive ? primitive : object;
}

static ALWAYS_INLINE bool isHostFunctionNamed(JSValue value, NativeFunction function)
{
    JSFunction* held = value && value.isCell() ? dynamicDowncast<JSFunction>(value.asCell()) : nullptr;
    return held && held->isHostFunction() && held->nativeFunction() == toTagged(function);
}

static ALWAYS_INLINE bool isCompiledFunction(JSValue value)
{
    return value.isCell() && isCompiledCalleeType(value.asCell()->type());
}

static NEVER_INLINE bool isFunctionConvertedToText(JSGlobalObject* globalObject, JSObject* function)
{
    VM& vm = globalObject->vm();
    const CommonIdentifiers& names = *vm.propertyNames;
    JSObject* functionPrototype = globalObject->functionPrototype();
    JSObject* objectPrototype = globalObject->objectPrototype();
    const std::array<UniquedStringImpl*, 3> conversions { names.toPrimitiveSymbol.impl(), names.valueOf.impl(), names.toString.impl() };
    auto isAbsent = [&](JSObject* object, PropertyName name) {
        return !isValidOffset(object->structure()->get(vm, name));
    };
    for (JSObject* object = function; object != functionPrototype; object = object->getPrototypeDirect().getObject()) {
        if (!object || !isCompiledCalleeType(object->type()))
            return false;
        if (object->type() == CompiledClosureType ? uncheckedDowncast<Closure>(object)->mayHoldOwnProperties(vm, conversions) : !std::ranges::all_of(conversions, [&](UniquedStringImpl* name) { return isAbsent(object, PropertyName(name)); }))
            return false;
    }
    return functionPrototype->getPrototypeDirect() == JSValue(objectPrototype) && isAbsent(functionPrototype, names.toPrimitiveSymbol) && isAbsent(objectPrototype, names.toPrimitiveSymbol) && isAbsent(functionPrototype, names.valueOf)
        && isHostFunctionNamed(functionPrototype->getDirect(vm, names.toString), functionProtoFuncToString) && objectPrototype->getDirect(vm, names.valueOf) == globalObject->objectProtoValueOfFunction();
}

NEVER_INLINE JSValue textIfFunction(JSGlobalObject* globalObject, JSValue function)
{
    return isFunctionConvertedToText(globalObject, asObject(function)) ? CompiledCallee::sourceText(globalObject, asObject(function)) : function;
}

JSValue timeIfDate(JSValue value)
{
    if (!value.isCell() || value.asCell()->type() != JSDateType) [[likely]]
        return value;
    auto* date = uncheckedDowncast<DateInstance>(value.asCell());
    Structure* structure = date->structure();
    if (!isHostFunctionNamed(structure->cachedSpecialProperty(CachedSpecialPropertyKey::ToPrimitive), dateProtoFuncToPrimitiveSymbol) || !isHostFunctionNamed(structure->cachedSpecialProperty(CachedSpecialPropertyKey::ValueOf), dateProtoFuncGetTime))
        return value;
    return jsNumber(date->internalNumber());
}

#define AOT_TAKE_PRIMITIVES(left, right, hint, returned) \
    if (hint == PreferNumber) \
        left = timeIfDate(left); \
    if (isCompiledObject(left)) { \
        left = primitiveIfKnown(instance, left, hint); \
        OPERATION_RETURN_IF_EXCEPTION(scope, returned); \
    } \
    if (hint == PreferNumber && !left.isObject()) \
        right = timeIfDate(right); \
    if (isCompiledObject(right) && !left.isObject() && (hint == NoPreference || !left.isSymbol())) { \
        right = primitiveIfKnown(instance, right, hint); \
        OPERATION_RETURN_IF_EXCEPTION(scope, returned); \
    }

#define AOT_DEFINE_BINARY_OPERATION(name, type, result) \
    JSC_DEFINE_JIT_OPERATION(name, type, (Instance* instance, EncodedJSValue encodedLeft, EncodedJSValue encodedRight)) \
    { \
        AOT_OPERATION_BEGIN(instance, Entry::name); \
        JSValue left = JSValue::decode(encodedLeft); \
        JSValue right = JSValue::decode(encodedRight); \
        OPERATION_RETURN(scope, result); \
    }

static ALWAYS_INLINE double numberOfValueThatIsNoCell(JSValue value)
{
    ASSERT(!value.isCell());
    if (value.isInt32())
        return value.asInt32();
    if (value.isDouble())
        return value.asDouble();
    if (value.isUndefined())
        return PNaN;
    return value.isTrue() ? 1 : 0;
}

template<unsigned... lengths>
static ALWAYS_INLINE bool areBigIntsOfLengths(std::same_as<EncodedJSValue> auto... values)
{
    if ((values | ...) & JSValue::NotCellMask)
        return false;
    auto isBigIntOfLength = [](EncodedJSValue value, unsigned length) {
        return JSValue::decode(value).asCell()->type() == HeapBigIntType && JSValue::decode(value).asHeapBigInt()->length() == length;
    };
    return (isBigIntOfLength(values, lengths) && ...);
}

template<unsigned leftLength, JSBigInt* (*tryBigInts)(VM&, JSBigInt*, JSBigInt*)>
static ALWAYS_INLINE JSBigInt* tryBigIntsOfUsualLength(Instance* instance, EncodedJSValue left, EncodedJSValue right)
{
    if (!areBigIntsOfLengths<leftLength, BigIntArithmetic::usualLength>(left, right))
        return nullptr;
    return tryBigInts(*instance->vm, JSValue::decode(left).asHeapBigInt(), JSValue::decode(right).asHeapBigInt());
}

#define AOT_DEFINE_STEP_ON_BIGINTS(name) \
    struct name { \
        static constexpr bool asksCachedDivisor = false; \
        static ALWAYS_INLINE JSBigInt* run(auto place, JSBigInt* left, JSBigInt* right) { return BigIntArithmetic::try##name(place, left, right); } \
    };
#define AOT_DEFINE_STEP_WITH_CACHED_DIVISOR(name) \
    struct name { \
        static constexpr bool asksCachedDivisor = true; \
        static ALWAYS_INLINE JSBigInt* run(VM& vm, auto place, JSBigInt* dividend, JSBigInt* divisor) { return BigIntArithmetic::try##name(vm, place, dividend, divisor); } \
    };
namespace StepOnBigInts {
AOT_DEFINE_STEP_ON_BIGINTS(Add)
AOT_DEFINE_STEP_ON_BIGINTS(AddOfUsualLength)
AOT_DEFINE_STEP_ON_BIGINTS(Sub)
AOT_DEFINE_STEP_ON_BIGINTS(SubOfUsualLength)
AOT_DEFINE_STEP_ON_BIGINTS(Multiply)
AOT_DEFINE_STEP_ON_BIGINTS(MultiplyOfUsualLength)
AOT_DEFINE_STEP_ON_BIGINTS(MultiplyByDigit)
AOT_DEFINE_STEP_ON_BIGINTS(Divide)
AOT_DEFINE_STEP_ON_BIGINTS(BitwiseAnd)
AOT_DEFINE_STEP_ON_BIGINTS(SignedRightShift)
AOT_DEFINE_STEP_WITH_CACHED_DIVISOR(Remainder)
struct RemainderOfUsualProduct {
    static constexpr bool asksCachedDivisor = false;
    static ALWAYS_INLINE JSBigInt* run(auto place, JSBigInt* dividend, JSBigInt* divisor) { return BigIntArithmetic::tryRemainderOfUsualLength<2 * BigIntArithmetic::usualLength>(place, dividend, divisor, false); }
};
}

template<typename Step>
static ALWAYS_INLINE JSBigInt* tryInHeap(VM& vm, JSBigInt* left, JSBigInt* right)
{
    if constexpr (Step::asksCachedDivisor)
        return Step::run(vm, BigIntArithmetic::HeapPlace(vm), left, right);
    else
        return Step::run(BigIntArithmetic::HeapPlace(vm), left, right);
}

static ALWAYS_INLINE JSBigInt* findNoBigInt(Instance*, EncodedJSValue, EncodedJSValue)
{
    return nullptr;
}

template<JSBigInt* (*tryBigInts)(VM&, JSBigInt*, JSBigInt*)>
static ALWAYS_INLINE JSBigInt* tryShortBigInts(Instance* instance, EncodedJSValue encodedLeft, EncodedJSValue encodedRight)
{
    JSValue left = JSValue::decode(encodedLeft);
    JSValue right = JSValue::decode(encodedRight);
    if (!left.isHeapBigInt() || !right.isHeapBigInt())
        return nullptr;
    return tryBigInts(*instance->vm, left.asHeapBigInt(), right.asHeapBigInt());
}

#define AOT_DEFINE_NUMERIC_BINARY_OPERATION(name, function, bigIntFunction, tryBigInts, tryBigIntsFirst, ofNumbers) \
    static NEVER_INLINE OperationReturnType<EncodedJSValue> JIT_OPERATION_ATTRIBUTES name##OfAnyValues(Instance*, EncodedJSValue, EncodedJSValue); \
    static NEVER_INLINE OperationReturnType<EncodedJSValue> JIT_OPERATION_ATTRIBUTES name##OfShortBigInts(Instance* instance, EncodedJSValue encodedLeft, EncodedJSValue encodedRight) \
    { \
        if (JSBigInt* result = tryShortBigInts<tryBigInts>(instance, encodedLeft, encodedRight)) \
            return { JSValue::encode(result), nullptr }; \
        AOT_MUST_TAIL_CALL return name##OfAnyValues(instance, encodedLeft, encodedRight); \
    } \
    JSC_DEFINE_JIT_OPERATION(name, EncodedJSValue, (Instance* instance, EncodedJSValue encodedLeft, EncodedJSValue encodedRight)) \
    { \
        if (JSBigInt* result = tryBigIntsFirst(instance, encodedLeft, encodedRight)) \
            return { JSValue::encode(result), nullptr }; \
        AOT_MUST_TAIL_CALL return name##OfShortBigInts(instance, encodedLeft, encodedRight); \
    } \
    OperationReturnType<EncodedJSValue> JIT_OPERATION_ATTRIBUTES name##OfAnyValues(Instance* instance, EncodedJSValue encodedLeft, EncodedJSValue encodedRight) \
    { \
        AOT_OPERATION_BEGIN(instance, Entry::name); \
        JSValue left = BigIntArithmetic::valueInHeap(vm, JSValue::decode(encodedLeft)); \
        JSValue right = BigIntArithmetic::valueInHeap(vm, JSValue::decode(encodedRight)); \
        if (left.isHeapBigInt() && right.isHeapBigInt()) \
            OPERATION_RETURN(scope, JSValue::encode(JSBigInt::bigIntFunction(globalObject, left.asHeapBigInt(), right.asHeapBigInt()))); \
        if (!left.isCell() && !right.isCell()) { \
            double x = numberOfValueThatIsNoCell(left); \
            double y = numberOfValueThatIsNoCell(right); \
            OPERATION_RETURN(scope, JSValue::encode(jsNumber(ofNumbers))); \
        } \
        AOT_TAKE_PRIMITIVES(left, right, PreferNumber, encodedJSValue()) \
        OPERATION_RETURN(scope, JSValue::encode(function(globalObject, left, right))); \
    }

#define AOT_DEFINE_BITWISE_OPERATION_WITH_FAST_PATH(name, function, bigIntFunction, tryBigInts) \
    static NEVER_INLINE OperationReturnType<EncodedJSValue> JIT_OPERATION_ATTRIBUTES name##OfAnyValues(Instance*, EncodedJSValue, EncodedJSValue); \
    JSC_DEFINE_JIT_OPERATION(name, EncodedJSValue, (Instance* instance, EncodedJSValue encodedLeft, EncodedJSValue encodedRight)) \
    { \
        if (JSBigInt* result = tryShortBigInts<tryBigInts>(instance, encodedLeft, encodedRight)) \
            return { JSValue::encode(result), nullptr }; \
        AOT_MUST_TAIL_CALL return name##OfAnyValues(instance, encodedLeft, encodedRight); \
    } \
    OperationReturnType<EncodedJSValue> JIT_OPERATION_ATTRIBUTES name##OfAnyValues(Instance* instance, EncodedJSValue encodedLeft, EncodedJSValue encodedRight) \
    { \
        AOT_OPERATION_BEGIN(instance, Entry::name); \
        JSValue left = JSValue::decode(encodedLeft); \
        JSValue right = JSValue::decode(encodedRight); \
        if (left.isHeapBigInt() && right.isHeapBigInt()) \
            OPERATION_RETURN(scope, JSValue::encode(JSBigInt::bigIntFunction(globalObject, left.asHeapBigInt(), right.asHeapBigInt()))); \
        AOT_TAKE_PRIMITIVES(left, right, PreferNumber, encodedJSValue()) \
        OPERATION_RETURN(scope, JSValue::encode(function(globalObject, left, right))); \
    }

#define AOT_DEFINE_BITWISE_OPERATION(name, function, bigIntFunction) \
    JSC_DEFINE_JIT_OPERATION(name, EncodedJSValue, (Instance* instance, EncodedJSValue encodedLeft, EncodedJSValue encodedRight)) \
    { \
        AOT_OPERATION_BEGIN(instance, Entry::name); \
        JSValue left = JSValue::decode(encodedLeft); \
        JSValue right = JSValue::decode(encodedRight); \
        if (left.isHeapBigInt() && right.isHeapBigInt()) \
            OPERATION_RETURN(scope, JSValue::encode(JSBigInt::bigIntFunction(globalObject, left.asHeapBigInt(), right.asHeapBigInt()))); \
        AOT_TAKE_PRIMITIVES(left, right, PreferNumber, encodedJSValue()) \
        OPERATION_RETURN(scope, JSValue::encode(function(globalObject, left, right))); \
    }

#define AOT_DEFINE_COMPARISON_OPERATION(name, ofHeapBigInts, relation, result) \
    JSC_DEFINE_JIT_OPERATION(name, size_t, (Instance* instance, EncodedJSValue encodedLeft, EncodedJSValue encodedRight)) \
    { \
        AOT_OPERATION_BEGIN(instance, Entry::name); \
        JSValue left = JSValue::decode(encodedLeft); \
        JSValue right = JSValue::decode(encodedRight); \
        if (left.isHeapBigInt() && right.isHeapBigInt()) \
            OPERATION_RETURN(scope, JSBigInt::compare(left.asHeapBigInt(), right.asHeapBigInt()) ofHeapBigInts); \
        if (!left.isCell() && !right.isCell()) \
            OPERATION_RETURN(scope, numberOfValueThatIsNoCell(left) relation numberOfValueThatIsNoCell(right)); \
        AOT_TAKE_PRIMITIVES(left, right, PreferNumber, 0) \
        OPERATION_RETURN(scope, result); \
    }

AOT_DEFINE_NUMERIC_BINARY_OPERATION(operationAOTValueSub, jsSub, sub, tryInHeap<StepOnBigInts::Sub>, (tryBigIntsOfUsualLength<BigIntArithmetic::usualLength, tryInHeap<StepOnBigInts::SubOfUsualLength>>), x - y)
AOT_DEFINE_NUMERIC_BINARY_OPERATION(operationAOTValueMul, jsMul, multiply, tryInHeap<StepOnBigInts::Multiply>, (tryBigIntsOfUsualLength<BigIntArithmetic::usualLength, tryInHeap<StepOnBigInts::MultiplyOfUsualLength>>), x * y)
AOT_DEFINE_NUMERIC_BINARY_OPERATION(operationAOTValueDiv, jsDiv, divide, tryInHeap<StepOnBigInts::Divide>, findNoBigInt, x / y)
AOT_DEFINE_NUMERIC_BINARY_OPERATION(operationAOTValueMod, jsRemainder, remainder, tryInHeap<StepOnBigInts::Remainder>, (tryBigIntsOfUsualLength<2 * BigIntArithmetic::usualLength, tryInHeap<StepOnBigInts::RemainderOfUsualProduct>>), Math::fmodDouble(x, y))
AOT_DEFINE_BINARY_OPERATION(operationAOTValuePow, EncodedJSValue, JSValue::encode(jsPow(globalObject, left, right)))
AOT_DEFINE_BITWISE_OPERATION_WITH_FAST_PATH(operationAOTValueBitAnd, jsBitwiseAnd, bitwiseAnd, tryInHeap<StepOnBigInts::BitwiseAnd>)
AOT_DEFINE_BITWISE_OPERATION(operationAOTValueBitOr, jsBitwiseOr, bitwiseOr)
AOT_DEFINE_BITWISE_OPERATION(operationAOTValueBitXor, jsBitwiseXor, bitwiseXor)
AOT_DEFINE_BITWISE_OPERATION(operationAOTValueLShift, jsLShift, leftShift)
AOT_DEFINE_BITWISE_OPERATION_WITH_FAST_PATH(operationAOTValueRShift, jsRShift, signedRightShift, tryInHeap<StepOnBigInts::SignedRightShift>)
AOT_DEFINE_COMPARISON_OPERATION(operationAOTCompareLess, == JSBigInt::ComparisonResult::LessThan, <, jsLess<true>(globalObject, left, right))
AOT_DEFINE_COMPARISON_OPERATION(operationAOTCompareLessEq, != JSBigInt::ComparisonResult::GreaterThan, <=, jsLessEq<true>(globalObject, left, right))
AOT_DEFINE_COMPARISON_OPERATION(operationAOTCompareGreater, == JSBigInt::ComparisonResult::GreaterThan, >, jsLess<false>(globalObject, right, left))
AOT_DEFINE_COMPARISON_OPERATION(operationAOTCompareGreaterEq, != JSBigInt::ComparisonResult::LessThan, >=, jsLessEq<false>(globalObject, right, left))
JSC_DEFINE_JIT_OPERATION(operationAOTCompareEq, size_t, (Instance* instance, EncodedJSValue encodedLeft, EncodedJSValue encodedRight))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTCompareEq);
    JSValue left = JSValue::decode(encodedLeft);
    JSValue right = JSValue::decode(encodedRight);
    if (isCompiledObject(left) ? !right.isObject() && !right.isUndefinedOrNull() : isCompiledObject(right) && !left.isObject() && !left.isUndefinedOrNull()) {
        JSValue& object = isCompiledObject(left) ? left : right;
        object = primitiveIfKnown(instance, object, NoPreference);
        OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    }
    if (isCompiledFunction(left) ? !right.isObject() && !right.isUndefinedOrNull() : isCompiledFunction(right) && !left.isObject() && !left.isUndefinedOrNull()) {
        JSValue& function = isCompiledFunction(left) ? left : right;
        if ((left.isSymbol() || right.isSymbol()) && isFunctionConvertedToText(globalObject, asObject(function)))
            OPERATION_RETURN(scope, false);
        function = textIfFunction(globalObject, function);
        OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    }
    OPERATION_RETURN(scope, JSValue::equal(globalObject, left, right));
}

AOT_DEFINE_BINARY_OPERATION(operationAOTCompareStrictEq, size_t, JSValue::strictEqual(globalObject, left, right))

#define AOT_DEFINE_UNARY_OPERATION_ON_NUMBER(name, type, resultOfFewDigits, result) \
    JSC_DEFINE_JIT_OPERATION(name, type, (Instance* instance, EncodedJSValue encodedOperand)) \
    { \
        AOT_OPERATION_BEGIN(instance, Entry::name); \
        JSValue operand = JSValue::decode(encodedOperand); \
        if (std::optional<double> number = valueOfTextOfFewDigits(operand)) \
            OPERATION_RETURN(scope, resultOfFewDigits); \
        operand = timeIfDate(operand); \
        if (isCompiledObject(operand)) { \
            operand = primitiveIfKnown(instance, operand, PreferNumber); \
            OPERATION_RETURN_IF_EXCEPTION(scope, type()); \
        } \
        OPERATION_RETURN(scope, result); \
    }

AOT_DEFINE_UNARY_OPERATION_ON_NUMBER(operationAOTValueBitNot, EncodedJSValue, JSValue::encode(jsNumber(~toInt32(*number))), JSValue::encode(jsBitwiseNot(globalObject, operand)))
AOT_DEFINE_UNARY_OPERATION_ON_NUMBER(operationAOTToNumber, double, *number, operand.toNumber(globalObject))
AOT_DEFINE_UNARY_OPERATION_ON_NUMBER(operationAOTToNumeric, EncodedJSValue, JSValue::encode(jsNumber(*number)), JSValue::encode(operand.toNumeric(globalObject)))

static ALWAYS_INLINE JSValue tryStringPrimitive(Instance* instance, Object& object)
{
    VM& vm = *instance->vm;
    JSGlobalObject* globalObject = instance->globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    const Slot* exotic = findReadWithoutCode(instance, object, vm.propertyNames->toPrimitiveSymbol);
    bool isOrdinary = exotic && exotic->kind == Slot::Kind::Absent;
    JSValue result;
    if (isOrdinary) [[likely]]
        result = tryPrimitiveByString(instance, object, findInheritedMethod(instance, object, vm.propertyNames->toString));
    else if (exotic)
        result = tryPrimitiveByHint(instance, object, *exotic, PreferString);
    RETURN_IF_EXCEPTION(scope, { });
    if (result && result.isObject()) [[unlikely]]
        RELEASE_AND_RETURN(scope, primitiveFromValueOf(globalObject, &object));
    return result;
}

NEVER_INLINE JSValue primitiveKeyIfKnown(Instance* instance, JSValue object)
{
    JSValue primitive = tryStringPrimitive(instance, *uncheckedDowncast<Object>(object.asCell()));
    return primitive ? primitive : object;
}

NEVER_INLINE JSString* toStringOfCompiledObject(Instance* instance, Object& object)
{
    VM& vm = *instance->vm;
    JSGlobalObject* globalObject = instance->globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue result = tryStringPrimitive(instance, object);
    RETURN_IF_EXCEPTION(scope, jsEmptyString(vm));
    if (!result)
        RELEASE_AND_RETURN(scope, JSValue(&object).toString(globalObject));
    if (result.isString()) [[likely]]
        return asString(result);
    RELEASE_AND_RETURN(scope, result.toString(globalObject));
}

static NEVER_INLINE OperationReturnType<EncodedJSValue> JIT_OPERATION_ATTRIBUTES addAnyValues(Instance*, EncodedJSValue, EncodedJSValue);

static NEVER_INLINE OperationReturnType<EncodedJSValue> JIT_OPERATION_ATTRIBUTES addListToText(Instance* instance, EncodedJSValue encodedLeft, EncodedJSValue encodedRight)
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTValueAdd);
    JSValue text = JSValue::decode(encodedLeft);
    JSValue list = JSValue::decode(encodedRight);
    JSString* joined = tryConvertListToString(instance, globalObject, asArray(list.asCell()));
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    if (!joined) [[unlikely]]
        OPERATION_RETURN(scope, JSValue::encode(jsAdd(globalObject, text, list)));
    OPERATION_RETURN(scope, JSValue::encode(joinTexts(globalObject, *instance, asString(text), joined)));
}

static NEVER_INLINE OperationReturnType<EncodedJSValue> JIT_OPERATION_ATTRIBUTES addShortBigInts(Instance* instance, EncodedJSValue encodedLeft, EncodedJSValue encodedRight)
{
    if (JSBigInt* result = tryShortBigInts<tryInHeap<StepOnBigInts::Add>>(instance, encodedLeft, encodedRight))
        return { JSValue::encode(result), nullptr };
    AOT_MUST_TAIL_CALL return addAnyValues(instance, encodedLeft, encodedRight);
}

JSC_DEFINE_JIT_OPERATION(operationAOTValueAdd, EncodedJSValue, (Instance* instance, EncodedJSValue encodedLeft, EncodedJSValue encodedRight))
{
    if (JSValue left = JSValue::decode(encodedLeft), right = JSValue::decode(encodedRight); right.isCell() && right.asCell()->type() == ArrayType && left.isString())
        AOT_MUST_TAIL_CALL return addListToText(instance, encodedLeft, encodedRight);
    if (JSBigInt* result = tryBigIntsOfUsualLength<BigIntArithmetic::usualLength, tryInHeap<StepOnBigInts::AddOfUsualLength>>(instance, encodedLeft, encodedRight))
        return { JSValue::encode(result), nullptr };
    AOT_MUST_TAIL_CALL return addShortBigInts(instance, encodedLeft, encodedRight);
}

OperationReturnType<EncodedJSValue> JIT_OPERATION_ATTRIBUTES addAnyValues(Instance* instance, EncodedJSValue encodedLeft, EncodedJSValue encodedRight)
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTValueAdd);
    JSValue left = BigIntArithmetic::valueInHeap(vm, JSValue::decode(encodedLeft));
    JSValue right = BigIntArithmetic::valueInHeap(vm, JSValue::decode(encodedRight));
    auto takeTextOfList = [&](JSValue& operand) {
        if (!operand.isCell() || !isJSArray(operand.asCell()))
            return;
        if (JSString* joined = tryConvertListToString(instance, globalObject, asArray(operand.asCell())))
            operand = joined;
    };
    if (left.isString()) {
        takeTextOfList(right);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    }
    if (left.isHeapBigInt() && right.isHeapBigInt())
        OPERATION_RETURN(scope, JSValue::encode(JSBigInt::add(globalObject, left.asHeapBigInt(), right.asHeapBigInt())));
    if (left.isString() && right.isString())
        OPERATION_RETURN(scope, JSValue::encode(joinTexts(globalObject, *instance, asString(left), asString(right))));
    if (left.isString() && right.isInt32())
        OPERATION_RETURN(scope, JSValue::encode(joinTexts(globalObject, *instance, asString(left), int32ToString(vm, right.asInt32(), 10))));
    if (left.isInt32() && right.isString())
        OPERATION_RETURN(scope, JSValue::encode(jsString(globalObject, int32ToString(vm, left.asInt32(), 10), asString(right))));
    if (!left.isCell() && !right.isCell())
        OPERATION_RETURN(scope, JSValue::encode(jsNumber(numberOfValueThatIsNoCell(left) + numberOfValueThatIsNoCell(right))));
    takeTextOfList(left);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    if (isCompiledFunction(left)) {
        left = textIfFunction(globalObject, left);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    }
    if (!left.isObject()) {
        takeTextOfList(right);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        if (isCompiledFunction(right)) {
            right = textIfFunction(globalObject, right);
            OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        }
        if (left.isString() && right.isString())
            OPERATION_RETURN(scope, JSValue::encode(joinTexts(globalObject, *instance, asString(left), asString(right))));
    }
    AOT_TAKE_PRIMITIVES(left, right, NoPreference, encodedJSValue())
    OPERATION_RETURN(scope, JSValue::encode(jsAdd(globalObject, left, right)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTToString, JSCell*, (Instance* instance, EncodedJSValue encodedOperand))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTToString);
    JSValue operand = JSValue::decode(encodedOperand);
    if (operand.isCell()) {
        JSCell* cell = operand.asCell();
        if (cell->type() == CompiledObjectType)
            OPERATION_RETURN(scope, toStringOfCompiledObject(instance, *uncheckedDowncast<Object>(cell)));
        if (isCompiledCalleeType(cell->type())) {
            JSValue text = textIfFunction(globalObject, operand);
            OPERATION_RETURN_IF_EXCEPTION(scope, jsEmptyString(vm));
            if (text.isString())
                OPERATION_RETURN(scope, asString(text));
        }
        if (isJSArray(cell)) {
            JSString* joined = tryConvertListToString(instance, globalObject, asArray(cell));
            OPERATION_RETURN_IF_EXCEPTION(scope, jsEmptyString(vm));
            if (joined)
                OPERATION_RETURN(scope, joined);
        }
        JSString* string = cell->toStringInline(globalObject);
        OPERATION_RETURN_IF_EXCEPTION(scope, jsEmptyString(vm));
        OPERATION_RETURN(scope, string);
    }
    if (operand.isInt32())
        OPERATION_RETURN(scope, int32ToString(vm, operand.asInt32(), 10));
    if (operand.isDouble())
        OPERATION_RETURN(scope, numberToString(vm, operand.asDouble(), 10));
    OPERATION_RETURN(scope, operand.toString(globalObject));
}

JSC_DEFINE_JIT_OPERATION(operationAOTToKey, EncodedJSValue, (Instance* instance, EncodedJSValue encodedOperand))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTToKey);
    JSValue operand = JSValue::decode(encodedOperand);
    if (operand.isInt32() && static_cast<uint32_t>(operand.asInt32()) < NumericStrings::cacheSize)
        OPERATION_RETURN(scope, JSValue::encode(vm.numericStrings.addJSString(vm, operand.asInt32())));
    if (isCompiledObject(operand)) {
        operand = primitiveKeyIfKnown(instance, operand);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    }
    OPERATION_RETURN(scope, JSValue::encode(operand.toPropertyKeyValue(globalObject)));
}

#undef AOT_DEFINE_BINARY_OPERATION
#undef AOT_DEFINE_NUMERIC_BINARY_OPERATION
#undef AOT_DEFINE_COMPARISON_OPERATION

JSC_DEFINE_JIT_OPERATION(operationAOTValueURShift, EncodedJSValue, (Instance* instance, EncodedJSValue encodedLeft, EncodedJSValue encodedRight))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTValueURShift);
    JSValue bits = jsURShift(globalObject, JSValue::decode(encodedLeft), JSValue::decode(encodedRight));
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    OPERATION_RETURN(scope, JSValue::encode(jsNumber(static_cast<uint32_t>(bits.asInt32()))));
}

static NEVER_INLINE OperationReturnType<EncodedJSValue> JIT_OPERATION_ATTRIBUTES negateAnyValue(Instance*, EncodedJSValue);

JSC_DEFINE_JIT_OPERATION(operationAOTValueNegate, EncodedJSValue, (Instance* instance, EncodedJSValue operand))
{
    if (JSValue value = JSValue::decode(operand); value.isHeapBigInt()) {
        if (JSBigInt* result = BigIntArithmetic::tryNegate(BigIntArithmetic::HeapPlace(*instance->vm), value.asHeapBigInt()))
            return { JSValue::encode(result), nullptr };
    }
    AOT_MUST_TAIL_CALL return negateAnyValue(instance, operand);
}

OperationReturnType<EncodedJSValue> JIT_OPERATION_ATTRIBUTES negateAnyValue(Instance* instance, EncodedJSValue operand)
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTValueNegate);
    JSValue numeric = timeIfDate(JSValue::decode(operand)).toNumeric(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    if (numeric.isBigInt())
        OPERATION_RETURN(scope, JSValue::encode(JSBigInt::unaryMinus(globalObject, numeric.asHeapBigInt())));
    OPERATION_RETURN(scope, JSValue::encode(jsNumber(-numeric.asNumber())));
}

static ALWAYS_INLINE EncodedJSValue encodeResult(JSBigInt* result)
{
    return JSValue::encode(result ? JSValue(result) : JSValue());
}

template<typename Step, unsigned lengthOfPlace>
static ALWAYS_INLINE JSBigInt* tryInFrame(JSBigInt* left, JSBigInt* right, void* place)
{
    if constexpr (Step::asksCachedDivisor) {
        if (BigIntArithmetic::isInFrame(right)) [[unlikely]]
            return nullptr;
        return Step::run(right->vm(), BigIntArithmetic::FramePlace<lengthOfPlace>(place), left, right);
    } else
        return Step::run(BigIntArithmetic::FramePlace<lengthOfPlace>(place), left, right);
}

template<typename Step, unsigned lengthOfPlace>
static ALWAYS_INLINE JSBigInt* tryInFrameOrHeap(JSBigInt* left, JSBigInt* right, void* place)
{
    if (JSBigInt* result = tryInFrame<Step, lengthOfPlace>(left, right, place)) [[likely]]
        return result;
    JSBigInt* inHeap = BigIntArithmetic::isInFrame(left) ? right : left;
    return BigIntArithmetic::isInFrame(inHeap) ? nullptr : tryInHeap<Step>(inHeap->vm(), left, right);
}

#define AOT_DEFINE_OPERATION_ON_BIGINTS(name, step, firstStep, lengthOfLeft, lengthOfPlace) \
    static NEVER_INLINE EncodedJSValue JIT_OPERATION_ATTRIBUTES try##name##ShortBigInts(EncodedJSValue encodedLeft, EncodedJSValue encodedRight, void* instance) \
    { \
        return encodeResult(tryShortBigInts<tryInHeap<StepOnBigInts::step>>(static_cast<Instance*>(instance), encodedLeft, encodedRight)); \
    } \
    JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTry##name##BigInts, EncodedJSValue, (EncodedJSValue encodedLeft, EncodedJSValue encodedRight, void* instance)) \
    { \
        if (areBigIntsOfLengths<lengthOfLeft, BigIntArithmetic::usualLength>(encodedLeft, encodedRight)) { \
            if (JSBigInt* result = tryInHeap<StepOnBigInts::firstStep>(*static_cast<Instance*>(instance)->vm, JSValue::decode(encodedLeft).asHeapBigInt(), JSValue::decode(encodedRight).asHeapBigInt())) \
                return JSValue::encode(result); \
        } \
        AOT_MUST_TAIL_CALL return try##name##ShortBigInts(encodedLeft, encodedRight, instance); \
    } \
    static NEVER_INLINE EncodedJSValue JIT_OPERATION_ATTRIBUTES try##name##ShortBigIntsInFrame(EncodedJSValue encodedLeft, EncodedJSValue encodedRight, void* place) \
    { \
        JSValue left = JSValue::decode(encodedLeft); \
        JSValue right = JSValue::decode(encodedRight); \
        if (!left.isHeapBigInt() || !right.isHeapBigInt()) \
            return JSValue::encode(JSValue()); \
        return encodeResult(tryInFrameOrHeap<StepOnBigInts::step, lengthOfPlace>(left.asHeapBigInt(), right.asHeapBigInt(), place)); \
    } \
    JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTry##name##BigIntsInFrame, EncodedJSValue, (EncodedJSValue encodedLeft, EncodedJSValue encodedRight, void* place)) \
    { \
        if (areBigIntsOfLengths<lengthOfLeft, BigIntArithmetic::usualLength>(encodedLeft, encodedRight)) { \
            if (JSBigInt* result = tryInFrame<StepOnBigInts::firstStep, lengthOfPlace>(JSValue::decode(encodedLeft).asHeapBigInt(), JSValue::decode(encodedRight).asHeapBigInt(), place)) \
                return JSValue::encode(result); \
        } \
        AOT_MUST_TAIL_CALL return try##name##ShortBigIntsInFrame(encodedLeft, encodedRight, place); \
    }

#if AOT_HAS_FUNCTIONS_WITH_MULX
#define AOT_TAIL_CALL_BY_MULX_IF_POSSIBLE(name, ...) \
    if (BigIntArithmetic::hasMULX()) [[likely]] \
        AOT_MUST_TAIL_CALL return name##ByMULX(__VA_ARGS__);

#define AOT_DEFINE_FIRST_STEP_BY_MULX(name, firstStep, lengthOfPlace) \
    static NEVER_INLINE AOT_WITH_MULX EncodedJSValue JIT_OPERATION_ATTRIBUTES try##name##UsualBigIntsByMULX(EncodedJSValue left, EncodedJSValue right, void* instance) \
    { \
        return encodeResult(tryInHeap<StepOnBigInts::firstStep>(*static_cast<Instance*>(instance)->vm, JSValue::decode(left).asHeapBigInt(), JSValue::decode(right).asHeapBigInt())); \
    } \
    static NEVER_INLINE AOT_WITH_MULX EncodedJSValue JIT_OPERATION_ATTRIBUTES try##name##UsualBigIntsInFrameByMULX(EncodedJSValue left, EncodedJSValue right, void* place) \
    { \
        return encodeResult(tryInFrame<StepOnBigInts::firstStep, lengthOfPlace>(JSValue::decode(left).asHeapBigInt(), JSValue::decode(right).asHeapBigInt(), place)); \
    }
AOT_DEFINE_FIRST_STEP_BY_MULX(Mul, MultiplyOfUsualLength, productSlotLength)
#else
#define AOT_TAIL_CALL_BY_MULX_IF_POSSIBLE(name, ...)
#endif

template<unsigned length>
static ALWAYS_INLINE bool isBigIntOfLength(EncodedJSValue value)
{
    return areBigIntsOfLengths<length>(value);
}

static ALWAYS_INLINE bool isBigIntOfUsualLengthAtMost(EncodedJSValue value)
{
    if (value & JSValue::NotCellMask)
        return false;
    return JSValue::decode(value).asCell()->type() == HeapBigIntType && JSValue::decode(value).asHeapBigInt()->length() - 1 < BigIntArithmetic::usualLength;
}

static NEVER_INLINE EncodedJSValue JIT_OPERATION_ATTRIBUTES tryMulBigIntsByDigitInFrame(EncodedJSValue left, EncodedJSValue right, void* place)
{
    return encodeResult(tryInFrame<StepOnBigInts::MultiplyByDigit, productSlotLength>(JSValue::decode(left).asHeapBigInt(), JSValue::decode(right).asHeapBigInt(), place));
}

#define AOT_DEFINE_OPERATION_ON_BIGINTS_BY_MULX(name, step, firstStep, lengthOfLeft, lengthOfPlace) \
    static NEVER_INLINE EncodedJSValue JIT_OPERATION_ATTRIBUTES try##name##ShortBigInts(EncodedJSValue encodedLeft, EncodedJSValue encodedRight, void* instance) \
    { \
        return encodeResult(tryShortBigInts<tryInHeap<StepOnBigInts::step>>(static_cast<Instance*>(instance), encodedLeft, encodedRight)); \
    } \
    static NEVER_INLINE EncodedJSValue JIT_OPERATION_ATTRIBUTES try##name##ShortBigIntsInFrame(EncodedJSValue encodedLeft, EncodedJSValue encodedRight, void* place) \
    { \
        JSValue left = JSValue::decode(encodedLeft); \
        JSValue right = JSValue::decode(encodedRight); \
        if (!left.isHeapBigInt() || !right.isHeapBigInt()) \
            return JSValue::encode(JSValue()); \
        return encodeResult(tryInFrameOrHeap<StepOnBigInts::step, lengthOfPlace>(left.asHeapBigInt(), right.asHeapBigInt(), place)); \
    } \
    static NEVER_INLINE EncodedJSValue JIT_OPERATION_ATTRIBUTES try##name##UsualBigInts(EncodedJSValue left, EncodedJSValue right, void* instance) \
    { \
        return encodeResult(tryInHeap<StepOnBigInts::firstStep>(*static_cast<Instance*>(instance)->vm, JSValue::decode(left).asHeapBigInt(), JSValue::decode(right).asHeapBigInt())); \
    } \
    static NEVER_INLINE EncodedJSValue JIT_OPERATION_ATTRIBUTES try##name##UsualBigIntsInFrame(EncodedJSValue left, EncodedJSValue right, void* place) \
    { \
        return encodeResult(tryInFrame<StepOnBigInts::firstStep, lengthOfPlace>(JSValue::decode(left).asHeapBigInt(), JSValue::decode(right).asHeapBigInt(), place)); \
    } \
    JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTry##name##BigInts, EncodedJSValue, (EncodedJSValue left, EncodedJSValue right, void* instance)) \
    { \
        if (areBigIntsOfLengths<lengthOfLeft, BigIntArithmetic::usualLength>(left, right)) { \
            AOT_TAIL_CALL_BY_MULX_IF_POSSIBLE(try##name##UsualBigInts, left, right, instance) \
            AOT_MUST_TAIL_CALL return try##name##UsualBigInts(left, right, instance); \
        } \
        AOT_MUST_TAIL_CALL return try##name##ShortBigInts(left, right, instance); \
    } \
    JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTry##name##BigIntsInFrame, EncodedJSValue, (EncodedJSValue left, EncodedJSValue right, void* place)) \
    { \
        if (areBigIntsOfLengths<lengthOfLeft, BigIntArithmetic::usualLength>(left, right)) { \
            AOT_TAIL_CALL_BY_MULX_IF_POSSIBLE(try##name##UsualBigIntsInFrame, left, right, place) \
            AOT_MUST_TAIL_CALL return try##name##UsualBigIntsInFrame(left, right, place); \
        } \
        if (isBigIntOfLength<1>(left) ? isBigIntOfUsualLengthAtMost(right) : isBigIntOfLength<1>(right) && isBigIntOfUsualLengthAtMost(left)) \
            AOT_MUST_TAIL_CALL return tryMulBigIntsByDigitInFrame(left, right, place); \
        AOT_MUST_TAIL_CALL return try##name##ShortBigIntsInFrame(left, right, place); \
    }

AOT_DEFINE_OPERATION_ON_BIGINTS(Add, Add, AddOfUsualLength, BigIntArithmetic::usualLength, bigIntSlotLength)
AOT_DEFINE_OPERATION_ON_BIGINTS(Sub, Sub, SubOfUsualLength, BigIntArithmetic::usualLength, bigIntSlotLength)
AOT_DEFINE_OPERATION_ON_BIGINTS_BY_MULX(Mul, Multiply, MultiplyOfUsualLength, BigIntArithmetic::usualLength, productSlotLength)
AOT_DEFINE_OPERATION_ON_BIGINTS(Div, Divide, Divide, BigIntArithmetic::usualLength, bigIntSlotLength)

static ALWAYS_INLINE void* addressOf(void* tagged)
{
    return std::bit_cast<void*>(std::bit_cast<uintptr_t>(tagged) & ~remainderMayBeSum);
}

static ALWAYS_INLINE bool remainderMayBeSumAt(void* tagged)
{
    return std::bit_cast<uintptr_t>(tagged) & remainderMayBeSum;
}

static NEVER_INLINE EncodedJSValue JIT_OPERATION_ATTRIBUTES tryModShortBigInts(EncodedJSValue left, EncodedJSValue right, void* taggedInstance)
{
    return encodeResult(tryShortBigInts<tryInHeap<StepOnBigInts::Remainder>>(static_cast<Instance*>(addressOf(taggedInstance)), left, right));
}

static NEVER_INLINE EncodedJSValue JIT_OPERATION_ATTRIBUTES tryModShortBigIntsInFrame(EncodedJSValue encodedLeft, EncodedJSValue encodedRight, void* taggedPlace)
{
    JSValue left = JSValue::decode(encodedLeft);
    JSValue right = JSValue::decode(encodedRight);
    if (!left.isHeapBigInt() || !right.isHeapBigInt())
        return JSValue::encode(JSValue());
    return encodeResult(tryInFrameOrHeap<StepOnBigInts::Remainder, bigIntSlotLength>(left.asHeapBigInt(), right.asHeapBigInt(), addressOf(taggedPlace)));
}

#define AOT_DEFINE_MOD_OF_USUAL_LENGTH(suffix, attributes) \
    template<unsigned dividendLength> \
    static NEVER_INLINE attributes EncodedJSValue JIT_OPERATION_ATTRIBUTES tryModUsualBigInts##suffix(EncodedJSValue left, EncodedJSValue right, void* taggedInstance) \
    { \
        VM& vm = *static_cast<Instance*>(addressOf(taggedInstance))->vm; \
        if (JSBigInt* result = BigIntArithmetic::tryRemainderOfUsualLength<dividendLength>(BigIntArithmetic::HeapPlace(vm), JSValue::decode(left).asHeapBigInt(), JSValue::decode(right).asHeapBigInt(), remainderMayBeSumAt(taggedInstance))) [[likely]] \
            return JSValue::encode(result); \
        AOT_MUST_TAIL_CALL return tryModShortBigInts(left, right, taggedInstance); \
    } \
    template<unsigned dividendLength> \
    static NEVER_INLINE attributes EncodedJSValue JIT_OPERATION_ATTRIBUTES tryModUsualBigIntsInFrame##suffix(EncodedJSValue left, EncodedJSValue right, void* taggedPlace) \
    { \
        if (JSBigInt* result = BigIntArithmetic::tryRemainderOfUsualLength<dividendLength>(BigIntArithmetic::FramePlace<bigIntSlotLength>(addressOf(taggedPlace)), JSValue::decode(left).asHeapBigInt(), JSValue::decode(right).asHeapBigInt(), remainderMayBeSumAt(taggedPlace))) [[likely]] \
            return JSValue::encode(result); \
        AOT_MUST_TAIL_CALL return tryModShortBigIntsInFrame(left, right, taggedPlace); \
    }
AOT_DEFINE_MOD_OF_USUAL_LENGTH(, )
#if AOT_HAS_FUNCTIONS_WITH_MULX
AOT_DEFINE_MOD_OF_USUAL_LENGTH(ByMULX, AOT_WITH_MULX)
#define AOT_TAIL_CALL_MOD_BY_MULX_IF_POSSIBLE(name, ...) \
    if (BigIntArithmetic::hasMULX()) [[likely]] \
        AOT_MUST_TAIL_CALL return name##ByMULX<__VA_ARGS__>(left, right, instanceOrPlace);
#else
#define AOT_TAIL_CALL_MOD_BY_MULX_IF_POSSIBLE(name, ...)
#endif

#define AOT_DEFINE_MOD_OF_BIGINTS(operation, name) \
    JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operation, EncodedJSValue, (EncodedJSValue left, EncodedJSValue right, void* instanceOrPlace)) \
    { \
        if (areBigIntsOfLengths<2 * BigIntArithmetic::usualLength, BigIntArithmetic::usualLength>(left, right)) { \
            AOT_TAIL_CALL_MOD_BY_MULX_IF_POSSIBLE(tryModUsual##name, 2 * BigIntArithmetic::usualLength) \
            AOT_MUST_TAIL_CALL return tryModUsual##name<2 * BigIntArithmetic::usualLength>(left, right, instanceOrPlace); \
        } \
        if (remainderMayBeSumAt(instanceOrPlace) && areBigIntsOfLengths<BigIntArithmetic::usualLength, BigIntArithmetic::usualLength>(left, right)) { \
            AOT_TAIL_CALL_MOD_BY_MULX_IF_POSSIBLE(tryModUsual##name, BigIntArithmetic::usualLength) \
            AOT_MUST_TAIL_CALL return tryModUsual##name<BigIntArithmetic::usualLength>(left, right, instanceOrPlace); \
        } \
        AOT_MUST_TAIL_CALL return tryModShort##name(left, right, instanceOrPlace); \
    }
AOT_DEFINE_MOD_OF_BIGINTS(operationAOTTryModBigInts, BigInts)
AOT_DEFINE_MOD_OF_BIGINTS(operationAOTTryModBigIntsInFrame, BigIntsInFrame)

template<unsigned greatestLength>
static ALWAYS_INLINE bool areBigIntsOfAtMostLength(std::same_as<EncodedJSValue> auto... values)
{
    if ((values | ...) & JSValue::NotCellMask)
        return false;
    auto isShortBigInt = [](EncodedJSValue value) {
        return JSValue::decode(value).asCell()->type() == HeapBigIntType && JSValue::decode(value).asHeapBigInt()->length() <= greatestLength;
    };
    return (isShortBigInt(values) && ...);
}

template<BigIntArithmetic::Factors factors>
static ALWAYS_INLINE EncodedJSValue remainderOfFactors(EncodedJSValue left, EncodedJSValue right, EncodedJSValue divisor, void* taggedInstance)
{
    VM& vm = *static_cast<Instance*>(addressOf(taggedInstance))->vm;
    return encodeResult(BigIntArithmetic::tryRemainderOfProduct<factors>(BigIntArithmetic::HeapPlace(vm), JSValue::decode(left).asHeapBigInt(), JSValue::decode(right).asHeapBigInt(), JSValue::decode(divisor).asHeapBigInt(), remainderMayBeSumAt(taggedInstance)));
}

static NEVER_INLINE EncodedJSValue JIT_OPERATION_ATTRIBUTES tryRemainderOfDigits(EncodedJSValue left, EncodedJSValue right, EncodedJSValue divisor, void* taggedInstance)
{
    return encodeResult(BigIntArithmetic::tryRemainderOfDigitProduct(BigIntArithmetic::HeapPlace(*static_cast<Instance*>(addressOf(taggedInstance))->vm), JSValue::decode(left).asHeapBigInt(), JSValue::decode(right).asHeapBigInt(), JSValue::decode(divisor).asHeapBigInt()));
}

template<BigIntArithmetic::Factors factors>
static ALWAYS_INLINE EncodedJSValue remainderOfFactorsInFrame(EncodedJSValue left, EncodedJSValue right, EncodedJSValue divisor, void* taggedPlace)
{
    return encodeResult(BigIntArithmetic::tryRemainderOfProduct<factors>(BigIntArithmetic::FramePlace<bigIntSlotLength>(addressOf(taggedPlace)), JSValue::decode(left).asHeapBigInt(), JSValue::decode(right).asHeapBigInt(), JSValue::decode(divisor).asHeapBigInt(), remainderMayBeSumAt(taggedPlace)));
}

static NEVER_INLINE EncodedJSValue JIT_OPERATION_ATTRIBUTES tryRemainderOfDigitsInFrame(EncodedJSValue left, EncodedJSValue right, EncodedJSValue divisor, void* taggedPlace)
{
    return encodeResult(BigIntArithmetic::tryRemainderOfDigitProduct(BigIntArithmetic::FramePlace<bigIntSlotLength>(addressOf(taggedPlace)), JSValue::decode(left).asHeapBigInt(), JSValue::decode(right).asHeapBigInt(), JSValue::decode(divisor).asHeapBigInt()));
}

#define AOT_DEFINE_REMAINDER_OF_FACTORS(name, operation, factors, attributes) \
    static NEVER_INLINE attributes EncodedJSValue JIT_OPERATION_ATTRIBUTES name(EncodedJSValue left, EncodedJSValue right, EncodedJSValue divisor, void* instanceOrPlace) \
    { \
        return operation<BigIntArithmetic::Factors::factors>(left, right, divisor, instanceOrPlace); \
    }
AOT_DEFINE_REMAINDER_OF_FACTORS(tryRemainderOfUsualFactors, remainderOfFactors, HaveUsualLength, )
AOT_DEFINE_REMAINDER_OF_FACTORS(tryRemainderOfShortFactors, remainderOfFactors, MayBeShorter, )
AOT_DEFINE_REMAINDER_OF_FACTORS(tryRemainderOfUsualFactorsInFrame, remainderOfFactorsInFrame, HaveUsualLength, )
AOT_DEFINE_REMAINDER_OF_FACTORS(tryRemainderOfShortFactorsInFrame, remainderOfFactorsInFrame, MayBeShorter, )
#if AOT_HAS_FUNCTIONS_WITH_MULX
AOT_DEFINE_REMAINDER_OF_FACTORS(tryRemainderOfUsualFactorsByMULX, remainderOfFactors, HaveUsualLength, AOT_WITH_MULX)
AOT_DEFINE_REMAINDER_OF_FACTORS(tryRemainderOfUsualFactorsInFrameByMULX, remainderOfFactorsInFrame, HaveUsualLength, AOT_WITH_MULX)
#endif

#define AOT_DEFINE_UNARY_OPERATION_ON_BIGINT(name) \
    JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTry##name##BigInt, EncodedJSValue, (EncodedJSValue encodedValue, void* instance)) \
    { \
        JSValue value = JSValue::decode(encodedValue); \
        return value.isHeapBigInt() ? encodeResult(BigIntArithmetic::try##name(BigIntArithmetic::HeapPlace(*static_cast<Instance*>(instance)->vm), value.asHeapBigInt())) : JSValue::encode(JSValue()); \
    } \
    JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTry##name##BigIntInFrame, EncodedJSValue, (EncodedJSValue encodedValue, void* place)) \
    { \
        JSValue value = JSValue::decode(encodedValue); \
        return value.isHeapBigInt() ? encodeResult(BigIntArithmetic::try##name(BigIntArithmetic::FramePlace<bigIntSlotLength>(place), value.asHeapBigInt())) : JSValue::encode(JSValue()); \
    }
AOT_DEFINE_UNARY_OPERATION_ON_BIGINT(Negate)
AOT_DEFINE_UNARY_OPERATION_ON_BIGINT(Increment)
AOT_DEFINE_UNARY_OPERATION_ON_BIGINT(Decrement)

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTryModBigIntsByQuotient, EncodedJSValue, (EncodedJSValue encodedLeft, EncodedJSValue encodedRight, EncodedJSValue encodedQuotient, void* instance))
{
    JSValue left = JSValue::decode(encodedLeft);
    JSValue right = JSValue::decode(encodedRight);
    JSValue quotient = JSValue::decode(encodedQuotient);
    if (!left.isHeapBigInt() || !right.isHeapBigInt() || !quotient.isHeapBigInt())
        return JSValue::encode(JSValue());
    return encodeResult(BigIntArithmetic::tryRemainderByQuotient(BigIntArithmetic::HeapPlace(*static_cast<Instance*>(instance)->vm), left.asHeapBigInt(), right.asHeapBigInt(), quotient.asHeapBigInt()));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTryModBigIntsByQuotientInFrame, EncodedJSValue, (EncodedJSValue encodedLeft, EncodedJSValue encodedRight, EncodedJSValue encodedQuotient, void* place))
{
    JSValue left = JSValue::decode(encodedLeft);
    JSValue right = JSValue::decode(encodedRight);
    JSValue quotient = JSValue::decode(encodedQuotient);
    if (!left.isHeapBigInt() || !right.isHeapBigInt() || !quotient.isHeapBigInt())
        return JSValue::encode(JSValue());
    return encodeResult(BigIntArithmetic::tryRemainderByQuotient(BigIntArithmetic::FramePlace<bigIntSlotLength>(place), left.asHeapBigInt(), right.asHeapBigInt(), quotient.asHeapBigInt()));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTryRemainderOfProduct, EncodedJSValue, (EncodedJSValue left, EncodedJSValue right, EncodedJSValue divisor, void* instance))
{
    if (areBigIntsOfLengths<BigIntArithmetic::usualLength, BigIntArithmetic::usualLength, BigIntArithmetic::usualLength>(left, right, divisor)) {
        AOT_TAIL_CALL_BY_MULX_IF_POSSIBLE(tryRemainderOfUsualFactors, left, right, divisor, instance)
        AOT_MUST_TAIL_CALL return tryRemainderOfUsualFactors(left, right, divisor, instance);
    }
    if (areBigIntsOfLengths<1, 1, 1>(left, right, divisor))
        AOT_MUST_TAIL_CALL return tryRemainderOfDigits(left, right, divisor, instance);
    if (areBigIntsOfLengths<BigIntArithmetic::usualLength>(divisor) && areBigIntsOfAtMostLength<BigIntArithmetic::usualLength>(left, right))
        AOT_MUST_TAIL_CALL return tryRemainderOfShortFactors(left, right, divisor, instance);
    return JSValue::encode(JSValue());
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTryRemainderOfProductInFrame, EncodedJSValue, (EncodedJSValue left, EncodedJSValue right, EncodedJSValue divisor, void* place))
{
    if (areBigIntsOfLengths<BigIntArithmetic::usualLength, BigIntArithmetic::usualLength, BigIntArithmetic::usualLength>(left, right, divisor)) {
        AOT_TAIL_CALL_BY_MULX_IF_POSSIBLE(tryRemainderOfUsualFactorsInFrame, left, right, divisor, place)
        AOT_MUST_TAIL_CALL return tryRemainderOfUsualFactorsInFrame(left, right, divisor, place);
    }
    if (areBigIntsOfLengths<1, 1, 1>(left, right, divisor))
        AOT_MUST_TAIL_CALL return tryRemainderOfDigitsInFrame(left, right, divisor, place);
    if (areBigIntsOfLengths<BigIntArithmetic::usualLength>(divisor) && areBigIntsOfAtMostLength<BigIntArithmetic::usualLength>(left, right))
        AOT_MUST_TAIL_CALL return tryRemainderOfShortFactorsInFrame(left, right, divisor, place);
    return JSValue::encode(JSValue());
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTCopyBigIntToHeap, EncodedJSValue, (Instance* instance, EncodedJSValue encodedValue))
{
    JSValue value = JSValue::decode(encodedValue);
    if (!value.isHeapBigInt() || !BigIntArithmetic::isInFrame(value.asHeapBigInt()))
        return encodedValue;
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTCopyBigIntToHeap);
    OPERATION_RETURN(scope, JSValue::encode(BigIntArithmetic::copyToHeap(vm, value.asHeapBigInt())));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTToBoolean, size_t, (Instance* instance, EncodedJSValue encodedOperand))
{
    return JSValue::decode(encodedOperand).toBoolean(instance->globalObject);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypeOf, JSCell*, (Instance* instance, EncodedJSValue operand))
{
    return jsTypeStringForValue(instance->globalObject, JSValue::decode(operand));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTIsCallable, size_t, (Instance*, JSCell* cell))
{
    return cell->isCallable();
}

#if CPU(ARM64) || defined(__FMA__)
static constexpr double maxExactQuotient = 0x1p53;
static ALWAYS_INLINE double subtractProduct(double dividend, double quotient, double divisor)
{
    return std::fma(-quotient, divisor, dividend);
}
#else
static constexpr double maxExactQuotient = 0x1p26;
static ALWAYS_INLINE double subtractProduct(double dividend, double quotient, double divisor)
{
    double high = std::bit_cast<double>(std::bit_cast<uint64_t>(divisor) & ~((1ull << 27) - 1));
    return (dividend - quotient * high) - quotient * (divisor - high);
}
#endif

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTFmod, double, (double a, double b))
{
    double dividend = std::abs(a);
    double divisor = std::abs(b);
    if (dividend < divisor)
        return a;
    double quotient = std::trunc(dividend / divisor);
    if (quotient < maxExactQuotient && dividend < 0x1p1000) [[likely]] {
        double remainder = subtractProduct(dividend, quotient, divisor);
        if (remainder < 0) [[unlikely]]
            remainder += divisor;
        return std::copysign(remainder, a);
    }

    constexpr double limit = 4611686018427387904.0;
    if (dividend < limit && divisor < limit) [[likely]] {
        int64_t wholeDividend = static_cast<int64_t>(a);
        int64_t wholeDivisor = static_cast<int64_t>(b);
        if (static_cast<double>(wholeDividend) == a && static_cast<double>(wholeDivisor) == b && wholeDivisor) [[likely]] {
            int64_t remainder = wholeDividend % wholeDivisor;
            return remainder ? static_cast<double>(remainder) : a * 0.0;
        }
    }
    return Math::fmodDouble(a, b);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTPow, double, (double a, double b))
{
    return operationMathPow(a, b);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTToInt32, size_t, (double value))
{
    return static_cast<uint32_t>(toInt32(value));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTRoundToIntegerBound, int64_t, (double number, uint32_t roundsUpAndTakesGreatestForNaN))
{
    if (number != number)
        return roundsUpAndTakesGreatestForNaN & 2 ? Typed::maxTrackedInteger : -Typed::maxTrackedInteger;
    if (number <= -0x1p62)
        return -Typed::maxTrackedInteger;
    if (number >= 0x1p62)
        return Typed::maxTrackedInteger;
    return static_cast<int64_t>(roundsUpAndTakesGreatestForNaN & 1 ? std::ceil(number) : std::floor(number));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTryConvertToInteger, int64_t, (EncodedJSValue encodedValue))
{
    JSValue value = JSValue::decode(encodedValue);
    if (value.isInt32())
        return value.asInt32();
    if (!value.isDouble())
        return std::numeric_limits<int64_t>::min();
    double number = value.asDouble();
    if (!(number >= -static_cast<double>(Typed::maxTrackedInteger) && number <= static_cast<double>(Typed::maxTrackedInteger)))
        return std::numeric_limits<int64_t>::min();
    int64_t integer = static_cast<int64_t>(number);
    return static_cast<double>(integer) == number ? integer : std::numeric_limits<int64_t>::min();
}

#define AOT_DEFINE_BINARY_ON_NUMBER_AND_VALUE(name, function, hint, ofNumbers) \
    JSC_DEFINE_JIT_OPERATION(name, double, (Instance* instance, EncodedJSValue value, uint32_t typeAndNumberSide, double number)) \
    { \
        AOT_OPERATION_BEGIN(instance, Entry::name); \
        auto bits = NumberAndValueBits::decode(typeAndNumberSide); \
        JSValue other = JSValue::decode(value); \
        if (!other.isCell()) { \
            double x = bits.numberIsOnRight ? numberOfValueThatIsNoCell(other) : withoutPayload(number); \
            double y = bits.numberIsOnRight ? withoutPayload(number) : numberOfValueThatIsNoCell(other); \
            OPERATION_RETURN(scope, ofNumbers); \
        } \
        if (hint == PreferNumber) \
            other = timeIfDate(other); \
        if (isCompiledObject(other)) { \
            other = primitiveIfKnown(instance, other, hint); \
            OPERATION_RETURN_IF_EXCEPTION(scope, 0); \
        } \
        JSValue boxed = jsNumber(withoutPayload(number)); \
        JSValue result = bits.numberIsOnRight ? function(globalObject, other, boxed) : function(globalObject, boxed, other); \
        OPERATION_RETURN_IF_EXCEPTION(scope, 0); \
        RELEASE_ASSERT(result.isNumber()); \
        OPERATION_RETURN(scope, result.asNumber()); \
    }
AOT_DEFINE_BINARY_ON_NUMBER_AND_VALUE(operationAOTAddNumberAndValueAsNumber, jsAdd, NoPreference, x + y)
AOT_DEFINE_BINARY_ON_NUMBER_AND_VALUE(operationAOTSubNumberAndValueAsNumber, jsSub, PreferNumber, x - y)
AOT_DEFINE_BINARY_ON_NUMBER_AND_VALUE(operationAOTMulNumberAndValueAsNumber, jsMul, PreferNumber, x * y)
AOT_DEFINE_BINARY_ON_NUMBER_AND_VALUE(operationAOTDivNumberAndValueAsNumber, jsDiv, PreferNumber, x / y)
#undef AOT_DEFINE_BINARY_ON_NUMBER_AND_VALUE

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
