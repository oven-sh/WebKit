/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "JSExportMacros.h"
#include <wtf/Function.h>
#include <wtf/Ref.h>
#include <wtf/Threading.h>
#include <wtf/text/ASCIILiteral.h>

namespace JSC { namespace AOT {

// Threads and pages that a build needs. When the system has none left, the process prints an error and exits with code 1.

// The thread has a stack of ParallelLoops::workerStackSize.
JS_EXPORT_PRIVATE Ref<Thread> startBuildThread(ASCIILiteral name, Function<void()>&&);

// The pages are zero-filled. `bytes` is a multiple of the page size.
void* allocatePages(size_t bytes);
void* reservePages(size_t bytes);
void commitPages(void* pages, size_t bytes);
// Takes everything that one call of allocatePages or reservePages returned.
void releasePages(void* pages, size_t bytes);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
