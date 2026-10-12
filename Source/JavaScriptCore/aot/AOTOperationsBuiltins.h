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
    v(operationAOTMapGet) \
    v(operationAOTMapHas) \
    v(operationAOTSetHas) \
    v(operationAOTMapSetGeneric) \
    v(operationAOTSetAddGeneric) \
    v(operationAOTMapSetWithStringKey) \
    v(operationAOTSetAddWithStringKey) \

enum class MathFunction : uint32_t { Sin, Cos, Tan, Asin, Acos, Atan, Sinh, Cosh, Tanh, Asinh, Acosh, Atanh, Log, Log2, Log10, Log1p, Exp, Expm1, Cbrt };
enum class DateField : uint32_t { FullYear, Month, Date, Day, Hours, Minutes, Seconds, TimezoneOffset };
static constexpr uint32_t dateFieldIsUTC = 16;

JSC_DECLARE_JIT_OPERATION(operationAOTMapGet, EncodedJSValue, (Instance*, JSCell*, EncodedJSValue));
JSC_DECLARE_JIT_OPERATION(operationAOTMapHas, EncodedJSValue, (Instance*, JSCell*, EncodedJSValue));
JSC_DECLARE_JIT_OPERATION(operationAOTSetHas, EncodedJSValue, (Instance*, JSCell*, EncodedJSValue));
JSC_DECLARE_JIT_OPERATION(operationAOTMapSetGeneric, EncodedJSValue, (Instance*, JSCell*, EncodedJSValue key, EncodedJSValue value));
JSC_DECLARE_JIT_OPERATION(operationAOTSetAddGeneric, EncodedJSValue, (Instance*, JSCell*, EncodedJSValue));
JSC_DECLARE_JIT_OPERATION(operationAOTMapSetWithStringKey, EncodedJSValue, (Instance*, JSCell*, EncodedJSValue key, EncodedJSValue value));
JSC_DECLARE_JIT_OPERATION(operationAOTSetAddWithStringKey, EncodedJSValue, (Instance*, JSCell*, EncodedJSValue));

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
