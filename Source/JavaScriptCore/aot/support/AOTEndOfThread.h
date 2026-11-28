/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "JSExportMacros.h"

namespace JSC { namespace AOT {

JS_EXPORT_PRIVATE void runAtEndOfThread(void (*)());

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
