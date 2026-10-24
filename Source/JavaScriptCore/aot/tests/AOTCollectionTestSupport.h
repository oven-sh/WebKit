/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTNativeTest.h"
#include "AOTVMState.h"
#include "Heap.h"
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

} // namespace JSC::AOT

#endif // ENABLE(AOT)
