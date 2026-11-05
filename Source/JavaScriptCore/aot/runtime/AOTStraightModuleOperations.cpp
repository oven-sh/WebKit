/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTModules.h"
#include "AOTOperationScope.h"
#include <wtf/UnalignedAccess.h>

namespace JSC { namespace AOT {

JSC_DEFINE_JIT_OPERATION(operationAOTThrowIfImportIsMissing, void, (Instance* instance, const uint32_t* pairs, uint32_t count))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTThrowIfImportIsMissing);
    for (uint32_t index = 0; index < count; ++index) {
        uint32_t offset = WTF::unalignedLoad<uint32_t>(pairs + 2 * index);
        if (*reinterpret_cast<const EncodedJSValue*>(instance->statics() + offset))
            continue;
        modulesOf(*instance).throwFailure(ModuleFailureID(WTF::unalignedLoad<uint32_t>(pairs + 2 * index + 1)), false);
        break;
    }
    OPERATION_RETURN(scope);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
