/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTOperationsBuiltins.h"

#if ENABLE(FTL_JIT)

#include "AOTOperationHelpers.h"
#include "DateInstance.h"
#include "FrameTracers.h"
#include "JSArrayInlines.h"
#include "JSCInlines.h"
#include "JSMapInlines.h"
#include "JSSetInlines.h"
#include "JSWeakMap.h"
#include "JSWeakSet.h"
#include "MathCommon.h"
#include "WeakMapImplInlines.h"

namespace JSC { namespace AOT {

#define AOT_OPERATION_PROLOGUE(globalObject) \
    VM& vm = (globalObject)->vm(); \
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm); \
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame); \
    auto scope = DECLARE_THROW_SCOPE(vm); \
    UNUSED_VARIABLE(scope)

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTMath, double, (double x, uint32_t which))
{
    switch (static_cast<MathFunction>(which)) {
    case MathFunction::Sin:
        return Math::sinDouble(x);
    case MathFunction::Cos:
        return Math::cosDouble(x);
    case MathFunction::Tan:
        return Math::tanDouble(x);
    case MathFunction::Asin:
        return Math::asinDouble(x);
    case MathFunction::Acos:
        return Math::acosDouble(x);
    case MathFunction::Atan:
        return Math::atanDouble(x);
    case MathFunction::Sinh:
        return Math::sinhDouble(x);
    case MathFunction::Cosh:
        return Math::coshDouble(x);
    case MathFunction::Tanh:
        return Math::tanhDouble(x);
    case MathFunction::Asinh:
        return Math::asinhDouble(x);
    case MathFunction::Acosh:
        return Math::acoshDouble(x);
    case MathFunction::Atanh:
        return Math::atanhDouble(x);
    case MathFunction::Log:
        return Math::logDouble(x);
    case MathFunction::Log2:
        return Math::log2Double(x);
    case MathFunction::Log10:
        return Math::log10Double(x);
    case MathFunction::Log1p:
        return Math::log1pDouble(x);
    case MathFunction::Exp:
        return Math::expDouble(x);
    case MathFunction::Expm1:
        return Math::expm1Double(x);
    case MathFunction::Cbrt:
        return Math::cbrtDouble(x);
    }
    RELEASE_ASSERT_NOT_REACHED();
    return 0;
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTMathAtan2, double, (double y, double x))
{
    return atan2(y, x);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTRandom, double, (JSGlobalObject* globalObject))
{
    return globalObject->weakRandomNumber();
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTWeakMapGet, EncodedJSValue, (JSCell* map, EncodedJSValue encodedKey))
{
    JSValue key = JSValue::decode(encodedKey);
    if (!key.isCell()) [[unlikely]]
        return JSValue::encode(jsUndefined());
    return JSValue::encode(uncheckedDowncast<JSWeakMap>(map)->get(key.asCell()));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTWeakMapHas, size_t, (JSCell* map, EncodedJSValue encodedKey))
{
    JSValue key = JSValue::decode(encodedKey);
    return key.isCell() && uncheckedDowncast<JSWeakMap>(map)->has(key.asCell());
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTWeakSetHas, size_t, (JSCell* set, EncodedJSValue encodedKey))
{
    JSValue key = JSValue::decode(encodedKey);
    return key.isCell() && uncheckedDowncast<JSWeakSet>(set)->has(key.asCell());
}

JSC_DEFINE_JIT_OPERATION(operationAOTMapDelete, size_t, (JSGlobalObject* globalObject, JSCell* map, EncodedJSValue key))
{
    AOT_OPERATION_PROLOGUE(globalObject);
    OPERATION_RETURN(scope, uncheckedDowncast<JSMap>(map)->remove(globalObject, JSValue::decode(key)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTSetDelete, size_t, (JSGlobalObject* globalObject, JSCell* set, EncodedJSValue key))
{
    AOT_OPERATION_PROLOGUE(globalObject);
    OPERATION_RETURN(scope, uncheckedDowncast<JSSet>(set)->remove(globalObject, JSValue::decode(key)));
}

// (The values are in the frame of whoever calls, where the collector finds them.)
JSC_DEFINE_JIT_OPERATION(operationAOTArrayPushMultiple, EncodedJSValue, (JSGlobalObject* globalObject, JSArray* array, EncodedJSValue* values, uint32_t count))
{
    AOT_OPERATION_PROLOGUE(globalObject);
    for (uint32_t i = 0; i < count; ++i) {
        array->pushInline(globalObject, JSValue::decode(values[i]));
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    }
    OPERATION_RETURN(scope, JSValue::encode(jsNumber(array->length())));
}

// end: what nobody passes is the most there is.
JSC_DEFINE_JIT_OPERATION(operationAOTArraySlice, JSArray*, (JSGlobalObject* globalObject, JSArray* array, int32_t start, int32_t end))
{
    AOT_OPERATION_PROLOGUE(globalObject);
    int64_t length = array->length();
    auto clamp = [&](int64_t index) { return index < 0 ? std::max<int64_t>(length + index, 0) : std::min<int64_t>(index, length); };
    int64_t from = clamp(start);
    int64_t to = std::max(clamp(end), from);
    OPERATION_RETURN(scope, JSArray::fastSlice(globalObject, array, from, to - from));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTDateField, EncodedJSValue, (VM* vm, DateInstance* date, uint32_t which))
{
    auto time = which & dateFieldIsUTC ? date->gregorianDateTimeUTC(vm->dateCache) : date->gregorianDateTime(vm->dateCache);
    if (!time)
        return JSValue::encode(jsNaN());
    switch (static_cast<DateField>(which & ~dateFieldIsUTC)) {
    case DateField::FullYear:
        return JSValue::encode(jsNumber(time.year()));
    case DateField::Month:
        return JSValue::encode(jsNumber(time.month()));
    case DateField::Date:
        return JSValue::encode(jsNumber(time.monthDay()));
    case DateField::Day:
        return JSValue::encode(jsNumber(time.weekDay()));
    case DateField::Hours:
        return JSValue::encode(jsNumber(time.hour()));
    case DateField::Minutes:
        return JSValue::encode(jsNumber(time.minute()));
    case DateField::Seconds:
        return JSValue::encode(jsNumber(time.second()));
    case DateField::TimezoneOffset:
        return JSValue::encode(jsNumber(-time.utcOffsetInMinute()));
    }
    RELEASE_ASSERT_NOT_REACHED();
    return 0;
}

// What code falls back on when it cannot make the array itself (Lowering::newArrayOf()): with those elements, one after the other, kept in that way.
JSC_DEFINE_JIT_OPERATION(operationAOTNewArrayOfValues, JSCell*, (JSGlobalObject* globalObject, const EncodedJSValue* values, uint32_t count, uint32_t indexingType))
{
    AOT_OPERATION_PROLOGUE(globalObject);
    Structure* structure = globalObject->arrayStructureForIndexingTypeDuringAllocation(static_cast<IndexingType>(indexingType));
    OPERATION_RETURN(scope, constructArray(globalObject, structure, std::bit_cast<const JSValue*>(values), count));
}

} } // namespace JSC::AOT

#endif // ENABLE(FTL_JIT)
