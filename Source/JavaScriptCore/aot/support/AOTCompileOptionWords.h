/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTWire.h"
#include "JSExportMacros.h"

namespace JSC { namespace AOT {

JS_EXPORT_PRIVATE Wire::CompileOptions makeDefaultCompileOptions();

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
