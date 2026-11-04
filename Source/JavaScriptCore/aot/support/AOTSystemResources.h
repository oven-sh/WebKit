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

// Threads and memory that a build needs. When the system has none left, the process prints an error and exits with code 1.

// The thread has a stack of ParallelLoops::workerStackSize.
JS_EXPORT_PRIVATE Ref<Thread> startBuildThread(ASCIILiteral name, Function<void()>&&);

// The engine has no memory for a cell or a block of a heap, and is about to crash. On a thread that is at work for a build that is an error of the build. On every other thread this returns.
// A build has a heap where it runs code of the program: facts/AOTFactsRegExpTexts.cpp.
void exitIfBuildIsOutOfMemory();

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
