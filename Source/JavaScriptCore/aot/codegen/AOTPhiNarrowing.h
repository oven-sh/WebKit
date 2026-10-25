/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "B3Policy.h"
#include "JSExportMacros.h"

namespace JSC { namespace AOT {

JS_EXPORT_PRIVATE bool narrowTruncatedPhis(B3::Procedure&);
B3::ClientPhase narrowTruncatedPhisAfterOptimization();

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
