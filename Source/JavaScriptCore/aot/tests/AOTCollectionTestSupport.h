/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTNativeTest.h"
#include "AOTVMState.h"
#include "Heap.h"
#include "MarkedBlockInlines.h"
#include "VM.h"
#include <wtf/SetForScope.h>

namespace JSC::AOT {

inline void collectWithCollectorHooks(VM& vm, CollectionScope scope)
{
    AOT_TEST_CHECK(vm.aotState().caches && vm.aotState().instances.isEmpty());
    SetForScope hasCompiledCode(vm.aotState().hasCompiledCode, true);
    vm.heap.collectSync(scope);
    AOT_TEST_CHECK(vm.heap.lastCollectionScope() == scope);
}

// A collection gives back a block in which nothing is left alive, and nothing of such a block can be read.
inline bool hasSurvivedCollection(VM& vm, JSCell* cell)
{
    if (!cell->isPreciseAllocation() && !vm.heap.objectSpace().blocks().set().contains(MarkedBlock::blockFor(cell)))
        return false;
    return vm.heap.isMarked(cell);
}

} // namespace JSC::AOT

#endif // ENABLE(AOT)
