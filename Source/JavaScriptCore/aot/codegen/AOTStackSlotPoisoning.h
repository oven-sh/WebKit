/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"
#include "AOTVector.h"
#include "B3Policy.h"
#include "JSExportMacros.h"

namespace JSC {

namespace B3 { namespace Air {
class StackSlot;
} }

namespace AOT {

using StackSlots = Vector<B3::Air::StackSlot*, 4>;

// Runs behind everything that moves a value, so the code is the code of an ordinary build with the checks beside it.
// Only the slots of cells: of a buffer that a callee fills in part, code may read every word and keep those that count.
JS_EXPORT_PRIVATE B3::ClientPhase reportReadsOfUnwrittenStackSlots(Typed::FunctionID, const StackSlots& slotsOfCells, bool entersOnlyAtRoot);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
