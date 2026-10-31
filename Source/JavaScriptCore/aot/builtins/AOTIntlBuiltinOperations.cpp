/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTOperationScope.h"
#include "AOTRemovableFeature.h"
#include "DeferTermination.h"
#include "IntlDateTimeFormat.h"
#include "IntlNumberFormat.h"
#include <wtf/DateMath.h>

namespace JSC { namespace AOT {

// The function is bound to the formatter: what the formatter has got under a name since makes no difference to it.
template<typename Formatter>
static ALWAYS_INLINE JSCell* tryFormat(JSGlobalObject* globalObject, EncodedJSValue encodedFormatter, double value)
{
    auto* formatter = dynamicDowncast<Formatter>(JSValue::decode(encodedFormatter));
    if (!formatter) [[unlikely]]
        return nullptr;
    auto topScope = DECLARE_TOP_EXCEPTION_SCOPE(globalObject->vm());
    JSValue text = formatter->format(globalObject, value);
    if (topScope.exception()) [[unlikely]] {
        topScope.clearExceptionExceptTermination();
        return nullptr;
    }
    return text.asCell();
}

static JSCell* tryFormatDateTime(JSGlobalObject* globalObject, EncodedJSValue formatter, double timeValue)
{
    double clippedTimeValue = WTF::timeClip(timeValue);
    if (!std::isfinite(clippedTimeValue)) [[unlikely]]
        return nullptr;
    return tryFormat<IntlDateTimeFormat>(globalObject, formatter, clippedTimeValue);
}

static JSCell* tryFormatNumber(JSGlobalObject* globalObject, EncodedJSValue formatter, double number)
{
    return tryFormat<IntlNumberFormat>(globalObject, formatter, number);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTFormatDateTime, JSCell*, (Instance* instance, EncodedJSValue formatter, double timeValue))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTFormatDateTime);
    DeferTerminationForAWhile deferTermination(vm);
    OPERATION_RETURN(scope, callRemovable<RemovableFeature::IntlDateTimeFormat, RemovedFeatureStub::Abort, tryFormatDateTime>(globalObject, formatter, timeValue));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTFormatNumber, JSCell*, (Instance* instance, EncodedJSValue formatter, double number))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTFormatNumber);
    DeferTerminationForAWhile deferTermination(vm);
    OPERATION_RETURN(scope, callRemovable<RemovableFeature::IntlNumberFormat, RemovedFeatureStub::Abort, tryFormatNumber>(globalObject, formatter, number));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
