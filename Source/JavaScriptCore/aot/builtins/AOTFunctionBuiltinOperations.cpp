/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTOperationScope.h"
#include "JSBoundFunction.h"
#include "JSCInlines.h"

namespace JSC { namespace AOT {

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTPrepareBoundFunctionAllocation, void, (Instance* instance))
{
    prepareInlineAllocation<JSBoundFunction>(instance, InlineAllocation::BoundFunction, instance->globalObject->boundFunctionStructure());
    RELEASE_ASSERT(static_cast<LocalAllocator*>(instance->allocation.inlineAllocations[static_cast<unsigned>(InlineAllocation::BoundFunction)].allocator)->cellSize() == sizeof(JSBoundFunction));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
