/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTBuiltinOperationSupport.h"
#include "AOTInstance.h"
#include "AOTObjectWords.h"
#include "AOTOperationScope.h"
#include "AOTSchemaRuntimeOperations.h"
#include "JSBigInt.h"
#include "JSCInlines.h"

namespace JSC { namespace AOT {

JSC_DEFINE_JIT_OPERATION(operationAOTNewBigInt64, JSCell*, (Instance* instance, uint64_t bits, uint32_t isSigned))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNewBigInt64);
    if (auto& data = instance->allocation.inlineAllocations[static_cast<unsigned>(InlineAllocation::BigInt)]; !data.allocator) {
        auto* allocator = subspaceFor<JSBigInt>(vm)->allocatorFor(JSBigInt::allocationSize(1), AllocatorForMode::EnsureAllocator).localAllocator();
        RELEASE_ASSERT(allocator->cellSize() == oneDigitBigIntSize);
        data.header = cellHeaderFor(vm.bigIntStructure.get());
        data.allocator = allocator;
    }
    JSBigInt* result = isSigned ? JSBigInt::createFrom(globalObject, static_cast<int64_t>(bits)) : JSBigInt::createFrom(globalObject, bits);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    OPERATION_RETURN(scope, result);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
