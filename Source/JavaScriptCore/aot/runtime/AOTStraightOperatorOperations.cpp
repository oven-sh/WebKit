/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTOperationScope.h"
#include "ExceptionHelpers.h"
#include "JSCInlines.h"
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT {

JSC_DEFINE_JIT_OPERATION(operationAOTConcatenateValues, JSCell*, (Instance* instance, uint32_t count, EncodedJSValue* parts))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTConcatenateValues);
    std::span<EncodedJSValue> texts = unsafeMakeSpan(parts, count);
    for (EncodedJSValue& text : texts) {
        JSString* converted = JSValue::decode(text).toString(globalObject);
        OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
        text = JSValue::encode(converted);
    }
    JSRopeString::RopeBuilder<RecordOverflow> joined(vm);
    for (EncodedJSValue text : texts) {
        if (!joined.append(asString(JSValue::decode(text)))) [[unlikely]] {
            throwOutOfMemoryError(globalObject, scope);
            OPERATION_RETURN(scope, nullptr);
        }
    }
    OPERATION_RETURN(scope, joined.release());
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
