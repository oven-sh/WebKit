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

JS_EXPORT_PRIVATE Ref<Thread> startBuildThread(ASCIILiteral name, Function<void()>&&);

JS_EXPORT_PRIVATE void exitIfBuildIsOutOfMemory();

JS_EXPORT_PRIVATE void* tryReserveAddresses(size_t bytes);
JS_EXPORT_PRIVATE void releaseAddresses(void*, size_t bytes);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
