/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTObjectModel.h"
#include "AOTRuntimeValidation.h"
#include "AOTState.h"
#include "DeferGCInlines.h"
#include "JSCInlines.h"

namespace JSC { namespace AOT {

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTReportValidationFailure, void, (Instance* instance, EncodedJSValue value, uint32_t kind, uint32_t function, uint32_t valueIndex))
{
    RELEASE_ASSERT(kind < numberOfEnumerators<ValidationFailureKind>);
    DeferGCForAWhile deferGC(*instance->vm);
    auto failureKind = static_cast<ValidationFailureKind>(kind);
    if (failureKind == ValidationFailureKind::ArrayElementsNotOrdinary && hasOrdinaryElements(*instance->vm, JSValue::decode(value)))
        return;
    if (failureKind == ValidationFailureKind::ArrayNotPristine && hasPristineNames(*instance->vm, JSValue::decode(value)))
        return;
    crashOnValidationFailure(*instance->vm, { failureKind, Typed::FunctionID(function), Typed::ValueID(valueIndex), JSValue::decode(value) });
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTContentsHaveType, size_t, (Instance* instance, JSCell* container, uint32_t contentsSlot, uint32_t type))
{
    DeferGCForAWhile deferGC(*instance->vm);
    const RuntimeType* contents = instance->state->objects->layouts.typeWithIndex(type);
    RELEASE_ASSERT(contents);
    return contentsHaveType(*instance, container, static_cast<Typed::ContentsSlot>(contentsSlot), *contents);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
