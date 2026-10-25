/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <wtf/Compiler.h>
#include <wtf/PlatformCallingConventions.h>

#if COMPILER(CLANG) && !OS(WINDOWS) && (CPU(ARM64) || CPU(X86_64))
#define AOT_LEAVES_PRESERVE_MOST 1
#define AOT_PRESERVE_MOST_LEAF_ATTRIBUTES __attribute__((preserve_most))
#else
#define AOT_LEAVES_PRESERVE_MOST 0
#define AOT_PRESERVE_MOST_LEAF_ATTRIBUTES JIT_OPERATION_ATTRIBUTES
#endif

#endif // ENABLE(AOT)
