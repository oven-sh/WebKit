/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTNativeTest.h"

#if ENABLE(AOT)

namespace JSC { namespace AOT {

std::span<const NativeTestProvider> nativeTestProvidersOfHost()
{
    return { };
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
