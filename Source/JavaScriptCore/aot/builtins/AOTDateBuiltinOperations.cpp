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
    constexpr double firstTimeValueOfYear0 = -62167219200000.0;
    constexpr double firstTimeValueOfYear10000 = 253402300800000.0;
    bool hasFourDigitYear = timeValue >= firstTimeValueOfYear0 && timeValue < firstTimeValueOfYear10000;
    std::span<Latin1Character> characters;
    auto text = StringImpl::createUninitialized(hasFourDigitYear ? maxISODateTimeLength - 3 : maxISODateTimeLength, characters);
    Latin1Character* end = writeISODateTime(characters.data(), timeValue);
    ASSERT_UNUSED(end, end == characters.data() + characters.size());
    OPERATION_RETURN(scope, jsNontrivialString(vm, String { WTF::move(text) }));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
