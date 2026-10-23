/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"
#include "AOTIRForm.h"
#include "AOTVector.h"
#include <wtf/Forward.h>

namespace JSC { namespace AOT {

class BuildContext;
struct InternalError;

namespace Typed {

class EffectSummaries;
class FlowResults;
class Program;
struct Function;

struct PlacedTableSizes {
    unsigned fieldAccesses { 0 };
    unsigned allocations { 0 };
    unsigned regExpRecords { 0 };
    unsigned guardBits { 0 };
};

struct VerifierInputs {
    const FlowResults* flow { nullptr };
    const PlacedTableSizes* placed { nullptr };
    bool isOptimizing { false };
    const EffectSummaries* effects { nullptr };
    // The summaries know less than those that the guards of the body were placed by, so they cannot say whether a guard still holds where it is used.
    bool cannotJudgeGuards { false };
    // Data and the small records are checked once, in form AsRead. IR text of a later form was never in that form.
    bool hasUncheckedRecords { false };
    // The parser of IR text states the engine's fields of a function, its inline frames among them, after the reader has run, and verifies the structure again.
    bool lacksEngineFields { false };
};

bool verifyProgram(const Program&, IRForm, BuildContext&, const VerifierInputs& = { });
// The one check of the Prepared form that no later form can make.
bool verifyUnspecifiedValues(const Program&, BuildContext&);
bool hoistsFunctionsIntoVariables(ModuleKind);
bool verifyFunction(const Program&, FunctionID, const Function&, IRForm, BuildContext&, const VerifierInputs& = { });
// For a caller that has something to add to the errors before they are reported. A valid body has none.
Vector<InternalError> findErrorsInFunction(const Program&, FunctionID, const Function&, IRForm, BuildContext&, const VerifierInputs& = { });

// A provisional id means nothing to a reader of the diagnostics. This is the function of the source text that the provisional function descends from.
FunctionID findFunctionAsReadOfProvisional(const Program&, const Function& provisional);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
