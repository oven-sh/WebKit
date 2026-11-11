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

ALWAYS_INLINE void didThrowOwedOutOfMemoryError(VM& vm)
{
    MarkedBlockCage& cage = vm.heap.cage();
    // Creating the error may already have notified the cage, which then waits for the poll that refills the Structure reserve, and the error's own cells may have
    // taken a reserve since. Report Structures again so that the cage settles that reserve too and keeps waiting for its poll.
    if (cage.errorWaitsForPoll())
        cage.addExhaustedHeap(MarkedBlockCage::ExhaustedHeap::Structures);
    vm.traps().setNeedsOutOfMemoryError(false);
    cage.didThrowOutOfMemoryError(vm);
}

ALWAYS_INLINE bool throwOutOfMemoryErrorIfOwed(VM& vm, JSGlobalObject* globalObject, ThrowScope& scope)
{
    if (!vm.traps().needsOutOfMemoryError()) [[likely]]
        return false;
    if (vm.heap.cage().errorWaitsForPoll())
        return false;
    throwOutOfMemoryError(globalObject, scope, vm.heap.cage().describeExhaustedLimit());
    didThrowOwedOutOfMemoryError(vm);
    return true;
}

} // namespace JSC
