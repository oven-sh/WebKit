/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTChildProcess.h"
#include <wtf/FileSystem.h>

namespace JSC { namespace AOT {

inline String shellPath()
{
    return FileSystem::pathByAppendingComponent(FileSystem::parentPath(programPath()), "jsc"_s);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
