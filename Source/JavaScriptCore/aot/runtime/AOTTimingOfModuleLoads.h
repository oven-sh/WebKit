/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "JSExportMacros.h"

namespace JSC { namespace AOT {

// By default a compiled program loads its modules in as many jobs as the loader takes for files, which a program can count.
// Relaxed, a graph that is all in the image is linked and run without the loader going through it,
// and imported CommonJS files run in the order of their requests. Before the first VM is made.
JS_EXPORT_PRIVATE void relaxTimingOfModuleLoads();
bool isTimingOfModuleLoadsRelaxed();

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
