/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTOperationScope.h"
#include "AOTRemovableFeature.h"
#include "IntlDateTimeFormat.h"
#include "IntlNumberFormat.h"
#include <wtf/DateMath.h>

namespace JSC { namespace AOT {

static ALWAYS_INLINE bool hasStructure(EncodedJSValue encodedValue, Structure* structure)
{
    JSValue value = JSValue::decode(encodedValue);
    return value.isCell() && value.asCell()->structureID() == structure->id();
}

template<typename Formatter>
static ALWAYS_INLINE JSCell* tryFormat(JSGlobalObject* globalObject, EncodedJSValue formatter, Structure* structure, double value)
{
    if (!hasStructure(formatter, structure)) [[unlikely]]
        return nullptr;
    auto topScope = DECLARE_TOP_EXCEPTION_SCOPE(globalObject->vm());
    JSValue text = uncheckedDowncast<Formatter>(JSValue::decode(formatter).asCell())->format(globalObject, value);
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
    return tryFormat<IntlDateTimeFormat>(globalObject, formatter, globalObject->dateTimeFormatStructure(), clippedTimeValue);
}

static JSCell* tryFormatNumber(JSGlobalObject* globalObject, EncodedJSValue formatter, double number)
{
    return tryFormat<IntlNumberFormat>(globalObject, formatter, globalObject->numberFormatStructure(), number);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTFormatDateTime, JSCell*, (Instance* instance, EncodedJSValue formatter, double timeValue))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTFormatDateTime);
    OPERATION_RETURN(scope, callRemovable<RemovableFeature::IntlDateTimeFormat, RemovedFeatureStub::Abort, tryFormatDateTime>(globalObject, formatter, timeValue));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTFormatNumber, JSCell*, (Instance* instance, EncodedJSValue formatter, double number))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTFormatNumber);
    OPERATION_RETURN(scope, callRemovable<RemovableFeature::IntlNumberFormat, RemovedFeatureStub::Abort, tryFormatNumber>(globalObject, formatter, number));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
