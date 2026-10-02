/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "OperationResult.h"

namespace JSC {

class DateInstance;
class JSArray;
class JSCell;
class JSGlobalObject;
class JSObject;
class JSString;
class VM;

namespace AOT {

#define FOR_EACH_AOT_BUILTIN_OPERATION(v) \
    v(operationAOTMath) \
    v(operationAOTMathAtan2) \
    v(operationAOTRandom) \
    v(operationAOTWeakMapGet) \
    v(operationAOTWeakMapHas) \
    v(operationAOTWeakSetHas) \
    v(operationAOTMapDelete) \
    v(operationAOTSetDelete) \
    v(operationAOTArrayPushMultiple) \
    v(operationAOTArraySlice) \
    v(operationAOTDateField) \
    v(operationAOTNewArrayFromValues) \

enum class MathFunction : uint32_t { Sin, Cos, Tan, Asin, Acos, Atan, Sinh, Cosh, Tanh, Asinh, Acosh, Atanh, Log, Log2, Log10, Log1p, Exp, Expm1, Cbrt };
enum class DateField : uint32_t { FullYear, Month, Date, Day, Hours, Minutes, Seconds, TimezoneOffset };
static constexpr uint32_t dateFieldIsUTC = 16;

JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTMath, double, (double, uint32_t));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTMathAtan2, double, (double, double));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTRandom, double, (Instance*));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTWeakMapGet, EncodedJSValue, (JSCell*, EncodedJSValue));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTWeakMapHas, size_t, (JSCell*, EncodedJSValue));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTWeakSetHas, size_t, (JSCell*, EncodedJSValue));
JSC_DECLARE_JIT_OPERATION(operationAOTMapDelete, size_t, (Instance*, JSCell*, EncodedJSValue));
JSC_DECLARE_JIT_OPERATION(operationAOTSetDelete, size_t, (Instance*, JSCell*, EncodedJSValue));
JSC_DECLARE_JIT_OPERATION(operationAOTArrayPushMultiple, EncodedJSValue, (Instance*, JSArray*, EncodedJSValue*, uint32_t));
JSC_DECLARE_JIT_OPERATION(operationAOTArraySlice, JSArray*, (Instance*, JSArray*, int32_t, int32_t));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTDateField, EncodedJSValue, (VM*, DateInstance*, uint32_t));
JSC_DECLARE_JIT_OPERATION(operationAOTNewArrayFromValues, JSCell*, (Instance*, const EncodedJSValue*, uint32_t, uint32_t));

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
