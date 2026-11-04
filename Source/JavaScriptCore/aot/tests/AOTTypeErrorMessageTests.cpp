/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTNativeTest.h"

namespace JSC::AOT {

// Nothing calls this: no list in AOTNativeTest.h names it, because the runtime has no function that describes a type, so there is nothing to test.
// The file exists only because JavaScriptCore_AOT_TEST_SOURCES in AOT.cmake lists it. Delete the file and that line together.
std::span<const NativeTest> typeErrorMessageTests();

std::span<const NativeTest> typeErrorMessageTests()
{
    return { };
}

} // namespace JSC::AOT

#endif // ENABLE(AOT)
