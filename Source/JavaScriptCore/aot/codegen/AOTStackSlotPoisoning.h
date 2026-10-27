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

namespace Typed {
struct PlannedLayout;
}

struct SlotOfCell {
    B3::Air::StackSlot* slot { nullptr };
    const Typed::PlannedLayout* layout { nullptr };
};
using SlotsOfCells = Vector<SlotOfCell, 4>;

// Runs behind everything that moves a value, so the code is the code of an ordinary build with the checks beside it.
// Only the slots of cells: of a buffer that a callee fills in part, code may read every word and keep those that count.
// So it is with the padding of a layout: a load of a field may stand ahead of the test that tells the layouts apart.
JS_EXPORT_PRIVATE B3::ClientPhase reportReadsOfUnwrittenStackSlots(Typed::FunctionID, const SlotsOfCells&, bool entersOnlyAtRoot);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
