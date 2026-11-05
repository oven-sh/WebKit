/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCodeRole.h"
#include "AOTFunctionData.h"
#include "AOTID.h"
#include "AOTSchemaEnums.h"
#include "AOTVector.h"
#include "JSExportMacros.h"

namespace JSC { namespace AOT { namespace Typed {

class CallingConventions;
class Program;
class SlotPlan;
struct Function;

struct ClosureCaptureCounts {
    unsigned references { 0 };
    unsigned captures { 0 };
};

// A closure keeps its references in front of its other captures. Of a function, all go by closureCaptureRepresentationOf(): a capture in the form Void is not counted and has no slot.
// Milestone one: closureCaptureRepresentationOf() is CallingConventions::parameterRepresentation(program, { function, CodeRole::Body }, capture), never Void.
// The counts and the slots go by holdsCells() of that form: the collector visits the references and nothing behind them. Where every form holds a cell, the slot is the index.
JS_EXPORT_PRIVATE ClosureCaptureCounts closureCaptureCountsOf(const Program&, const CallingConventions&, FunctionID);
JS_EXPORT_PRIVATE unsigned closureCaptureSlotOf(const Program&, const CallingConventions&, FunctionID, unsigned capture);
// The form in which every closure of the function holds the capture.
JS_EXPORT_PRIVATE Representation closureCaptureRepresentationOf(const Program&, const CallingConventions&, FunctionID, unsigned capture);

bool isAsyncFunctionWithoutAwait(const Program&, const Function& source);
bool rejectsOnThrow(const Program&, const CallingConventions&, CodeUnit);

// Of the function objects whose word names the unit, which is a generic entry.
JS_EXPORT_PRIVATE FunctionKindData functionKindOf(const Program&, const SlotPlan&, const CallingConventions&, CodeUnit closureEntry);
// How a stack trace shows a frame of the unit.
JS_EXPORT_PRIVATE FunctionTraceRecord functionTraceOf(const Program&, const CallingConventions&, CodeUnit);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
