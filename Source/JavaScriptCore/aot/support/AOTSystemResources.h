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

void exitIfBuildIsOutOfMemory();

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
