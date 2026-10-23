/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Error.h"
#include "ExceptionHelpers.h"
#include "MarkedBlockCage.h"
#include "VM.h"

namespace JSC {

JS_EXPORT_PRIVATE String describeLimitOfMarkedBlockCage();

ALWAYS_INLINE bool throwOutOfMemoryErrorIfOwed(VM& vm, JSGlobalObject* globalObject, ThrowScope& scope)
{
    if (!vm.traps().needsOutOfMemoryError()) [[likely]]
        return false;
    vm.traps().setNeedsOutOfMemoryError(false);
    String limit = vm.heap.cage().describeExhaustedLimit();
    vm.heap.cage().didThrowOutOfMemoryError();
    throwOutOfMemoryError(globalObject, scope, limit);
    return true;
}

} // namespace JSC
