/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "JSCJSValue.h"
#include <stdint.h>
#include <type_traits>

namespace JSC {

class JSCell;

namespace AOT {

class AsyncGeneratorStepInProgress;

struct CoroutineWords {
    uint64_t savedFrameHeader;
    uint32_t emptyGeneratorPrototypeStructureID;
    uint32_t emptyAsyncGeneratorPrototypeStructureID;
    JSCell* waiterToResume;
    EncodedJSValue waiterResolution;
    EncodedJSValue waiterAsyncContext;
    AsyncGeneratorStepInProgress* stepInProgress;
};

static_assert(std::is_trivially_default_constructible_v<CoroutineWords>);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
