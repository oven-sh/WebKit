/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTVector.h"
#include "B3HeapRange.h"
#include "B3Origin.h"
#include "GPRInfo.h"
#include "JSExportMacros.h"
#include <optional>
#include <wtf/text/WTFString.h>

namespace JSC {

namespace B3 {
class Procedure;
namespace Air {
class Code;
}
}

namespace AOT {

// An inner pointer is a word loaded out of a cell that points at what the cell owns, or an address computed from such a word. The cell is its owner.
struct InnerPointerViolation {
    B3::Origin pointer;
    B3::Origin safepoint;
    String detail;
};

struct InnerPointerFindings {
    Vector<InnerPointerViolation> violations;
    uint32_t numberOfPointersAcrossSafepoints { 0 };
};

struct InnerPointerRules {
    // A load from one of these gives an inner pointer, and the base of its address is the owner.
    Vector<B3::HeapRange, 8> fieldsThatHoldPointers;
    // Those of them that lie in what a cell owns and in no cell, so what they are loaded through is an inner pointer too.
    Vector<B3::HeapRange, 2> fieldsOutsideCells;
    // Those of them whose pointer the scan of the stack knows: it keeps what it points at, and none of the cells that are named there.
    Vector<B3::HeapRange, 2> fieldsThatHoldPointersTheScanKnows;
    // What lies behind such a pointer and names no cell.
    Vector<B3::HeapRange, 4> contentsWithoutCells;
    // A cell loaded from one of these stays for as long as the program runs.
    Vector<B3::HeapRange, 4> permanentCells;
    // A value that may collect writes both.
    B3::HeapRange collectorState;
    B3::HeapRange weakTableStorage;
    // A sum with this register, and a shift by this amount, give another form of the same reference.
    std::optional<GPRReg> cellCageBase;
    unsigned referenceShift { 0 };
};

// Reads code that is ready for generation. Wherever the collector can run and an inner pointer is read afterwards, its owner has to be read afterwards too.
JS_EXPORT_PRIVATE void findInnerPointerViolations(B3::Air::Code&, const InnerPointerRules&, InnerPointerFindings&);

// For tests. In front of every place where the collector can run, each register and each spilled word whose value is not read again gets a word that names nothing.
// So a cell that the code no longer reads is held by nothing that the code has left behind. Returns the number of registers and words.
JS_EXPORT_PRIVATE uint32_t overwriteDeadValuesAtSafepoints(B3::Air::Code&, const InnerPointerRules&);

InnerPointerRules innerPointerRulesOf(B3::Procedure&, bool usesNarrowReferences);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
