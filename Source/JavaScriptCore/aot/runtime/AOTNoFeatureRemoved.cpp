/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRemovableFeature.h"

#include <wtf/Assertions.h>

namespace JSC {

extern "C" const char* Bun__removableFeatureName(uint16_t feature);

extern "C" const RemovableFeatureState Bun__removableFeatureState { };

extern "C" void Bun__abortForRemovedFeature(uint16_t)
{
    CRASH();
}

extern "C" EncodedJSValue Bun__throwForRemovedFeature(JSGlobalObject*, uint16_t)
{
    CRASH();
}

extern "C" EncodedJSValue Bun__throwForRemovedBuiltin(JSGlobalObject*, uint16_t, uint32_t)
{
    CRASH();
}

extern "C" const char* Bun__removableFeatureName(uint16_t)
{
    return nullptr;
}

} // namespace JSC
