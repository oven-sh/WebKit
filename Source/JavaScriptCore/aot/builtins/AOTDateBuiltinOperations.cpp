/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTOperationScope.h"
#include "DateTimeWriter.h"
#include "JSCInlines.h"
#include <wtf/DateMath.h>

namespace JSC { namespace AOT {

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTDateNow, double, (Instance* instance))
{
    return instance->globalObject->jsDateNow();
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTDateToISOString, JSCell*, (Instance* instance, double timeValue))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTDateToISOString);
    if (!std::isfinite(timeValue) || std::abs(timeValue) > WTF::maxECMAScriptTime) [[unlikely]]
        OPERATION_RETURN(scope, nullptr);
    OPERATION_RETURN(scope, jsNontrivialString(vm, makeISODateTimeString(timeValue)));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
