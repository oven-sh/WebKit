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
    // computeEffectSummariesByOperandTypes(), for when they cannot be computed on demand because other functions are being edited.
    const EffectSummaries* effectsByOperandTypes { nullptr };
    bool cannotJudgeGuards { false };
    bool hasUncheckedRecords { false };
    bool lacksEngineFields { false };
    bool otherFunctionsMayChange { false };
    // Where operations are about to be moved, and behind a pass that has moved some: see AOTCastsWithoutFacts.h.
    bool castsNameTheirFacts { false };
};

bool verifyProgram(const Program&, IRForm, BuildContext&, const VerifierInputs& = { });
// Everything but the bodies of the functions.
bool verifyRecords(const Program&, IRForm, BuildContext&, const VerifierInputs& = { });
bool verifyUnspecifiedValues(const Program&, BuildContext&);
bool hoistsFunctionsIntoVariables(ModuleKind);
bool verifyFunction(const Program&, FunctionID, const Function&, IRForm, BuildContext&, const VerifierInputs& = { });
Vector<InternalError> findErrorsInFunction(const Program&, FunctionID, const Function&, IRForm, BuildContext&, const VerifierInputs& = { });
String findValueListedWrongly(const Function&);

FunctionID findFunctionAsReadOfProvisional(const Program&, const Function& provisional);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
