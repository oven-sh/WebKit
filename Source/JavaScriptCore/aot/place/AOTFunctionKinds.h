/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFunctionData.h"
#include "AOTIR.h"
#include "JSExportMacros.h"
#include <span>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

class ParallelLoops;

namespace Typed {

class FlowResults;
class SlotPlan;

struct ClosureCaptureCounts {
    unsigned references { 0 };
    unsigned captures { 0 };
};

struct ClosureFunctionRange {
    FunctionID first;
    FunctionID end;
    uint32_t omittedWordCount { 0 };
};

JS_EXPORT_PRIVATE ClosureForm closureFormOf(const Program&, const FlowResults&, FunctionID);
JS_EXPORT_PRIVATE ClosureCaptureCounts closureCaptureCountsOf(const Program&, const Function&);
JS_EXPORT_PRIVATE unsigned closureCaptureSlotOf(const Program&, std::span<const TypeID> captureTypes, unsigned capture);
JS_EXPORT_PRIVATE unsigned closureCaptureSlotOf(const Program&, const Function&, unsigned capture);
JS_EXPORT_PRIVATE FunctionID closureTargetOf(const Program&, FunctionID);
JS_EXPORT_PRIVATE FunctionID firstSourceOf(const Program&, FunctionID);
bool isInRuntimeModule(const Program&, const Function&);
bool rejectsOnThrow(const Program&, const Function&);
JS_EXPORT_PRIVATE FunctionKindData functionKindOf(const Program&, const SlotPlan&, FunctionID);
JS_EXPORT_PRIVATE FunctionTraceRecord functionTraceOf(const Program&, FunctionID);
JS_EXPORT_PRIVATE ClosureFunctionRange closureFunctionRangeOf(const Program&);
// Indexed by function. 0 means the function has no cell: cell 0 is reserved and never written.
JS_EXPORT_PRIVATE Vector<uint32_t> numberClosureCells(const Program&, const ParallelLoops&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
