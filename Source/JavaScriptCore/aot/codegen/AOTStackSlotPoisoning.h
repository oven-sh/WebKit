/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"
#include "B3Policy.h"
#include "JSExportMacros.h"

namespace JSC { namespace AOT {

// Runs behind everything that moves a value, so the code is the code of an ordinary build with the checks beside it.
JS_EXPORT_PRIVATE B3::ClientPhase reportReadsOfUnwrittenStackSlots(Typed::FunctionID, bool entersOnlyAtRoot);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
