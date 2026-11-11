/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTModules.h"
#include "AOTOperationArguments.h"
#include "AOTOperationData.h"
#include "AOTOperationScope.h"

namespace JSC { namespace AOT {

JSC_DEFINE_JIT_OPERATION(operationAOTThrowIfImportIsMissing, void, (Instance* instance, uint32_t request))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTThrowIfImportIsMissing);
    const auto& requested = requestAt<ImportChecksRequest>(instance, request);
    for (const ImportChecksRequest::Check& check : unsafeMakeSpan(requested.checks, requested.count)) {
        if (*reinterpret_cast<const EncodedJSValue*>(instance->statics() + check.offsetInStatics))
            continue;
        modulesOf(*instance).throwFailure(ModuleFailureID(check.failure), false);
        break;
    }
    OPERATION_RETURN(scope);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
