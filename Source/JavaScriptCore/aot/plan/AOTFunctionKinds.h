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

JS_EXPORT_PRIVATE ClosureCaptureCounts closureCaptureCountsOf(const Program&, const CallingConventions&, FunctionID);
JS_EXPORT_PRIVATE unsigned closureCaptureSlotOf(const Program&, const CallingConventions&, FunctionID, unsigned capture);
JS_EXPORT_PRIVATE Representation closureCaptureRepresentationOf(const Program&, const CallingConventions&, FunctionID, unsigned capture);

bool isAsyncFunctionWithoutAwait(const Program&, const Function& source);
bool rejectsOnThrow(const Program&, const CallingConventions&, CodeUnit);

JS_EXPORT_PRIVATE FunctionKindData functionKindOf(const Program&, const SlotPlan&, const CallingConventions&, CodeUnit closureEntry);
JS_EXPORT_PRIVATE FunctionTraceRecord functionTraceOf(const Program&, const CallingConventions&, CodeUnit, bool isEntryThatWalksBody = false);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
