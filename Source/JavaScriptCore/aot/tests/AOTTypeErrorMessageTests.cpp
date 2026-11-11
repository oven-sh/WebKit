/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTNativeTest.h"

namespace JSC::AOT {

std::span<const NativeTest> typeErrorMessageTests();

std::span<const NativeTest> typeErrorMessageTests()
{
    return { };
}

} // namespace JSC::AOT

#endif // ENABLE(AOT)
