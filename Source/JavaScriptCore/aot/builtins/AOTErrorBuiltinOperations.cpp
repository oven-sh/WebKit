/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTOperationArguments.h"
#include "AOTOperationScope.h"
#include "AOTRuntimeFunctions.h"
#include "ErrorInstance.h"
#include "JSCInlines.h"

namespace JSC { namespace AOT {

JSC_DEFINE_JIT_OPERATION(operationAOTNewError, JSCell*, (Instance* instance, uint32_t errorKind, EncodedJSValue message, EncodedJSValue options))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNewError);
    auto kind = static_cast<Typed::ErrorKind>(errorKind);
    if (kind == Typed::ErrorKind::AggregateError) [[unlikely]]
        crashOnWrongOperand(Entry::operationAOTNewError);
    ErrorType errorType = errorTypeFor(kind);
    constexpr bool useCurrentFrame = true;
    OPERATION_RETURN(scope, ErrorInstance::create(globalObject, globalObject->errorStructure(errorType), JSValue::decode(message), JSValue::decode(options), nullptr, TypeNothing, errorType, useCurrentFrame));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
