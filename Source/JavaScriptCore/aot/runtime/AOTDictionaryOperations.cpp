/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTBuiltinOperationSupport.h"
#include "AOTInstance.h"
#include "AOTOperationScope.h"
#include "AOTRuntimeTable.h"
#include "AOTUnwinding.h"
#include "DeferTermination.h"
#include "JSCInlines.h"
#include "ObjectConstructor.h"

namespace JSC { namespace AOT {

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTNewDictionary, JSCell*, (Instance* instance))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNewDictionary);
    DeferTerminationForAWhile deferTermination(vm);
    ThrowsOwedOutOfMemoryErrorAtReturn owedError(instance, frameRecord);
    Structure* structure = globalObject->objectStructureForObjectConstructor();
    prepareFinalObjectAllocation(instance, InlineAllocation::DefaultFinalObject, structure);
    OPERATION_RETURN(scope, constructEmptyObject(vm, structure));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
