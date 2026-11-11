/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTObjectModel.h"
#include "AOTOperationScope.h"
#include "AOTState.h"

namespace JSC { namespace AOT {

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTValueHasType, size_t, (Instance* instance, EncodedJSValue value, uint32_t type))
{
    AOT_OPERATION_BEGIN_WITHOUT_COLLECTION(instance, Entry::operationAOTValueHasType);
    const RuntimeLayoutTable& layouts = instance->state->objects->layouts;
    OPERATION_RETURN(scope, layouts.valueHasType(layouts.typeAt(type), JSValue::decode(value)));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
