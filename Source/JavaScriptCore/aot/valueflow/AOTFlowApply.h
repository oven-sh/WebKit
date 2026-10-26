/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCFG.h"
#include "AOTFlowApplication.h"

namespace JSC { namespace AOT { namespace Typed {

void applyFlowResults(Program&, FlowResults&, BuildContext&);
void computeOperandSummaries(const Program&, FlowResults&, BuildContext&);
void checkLockedBuiltinWrites(const Program&, const FlowResults&, BuildContext&);

Vector<BitVector> findAppendedFieldsDeclaredInFamilies(const Program&, const Vector<FlowLayoutUpdate>&, const ParallelLoops&);

struct FlowFirstTimeMarks {
    Vector<VariableID> storesByVariable;
    Vector<VariableID> evaluationsByLayout;
};

struct FlowBodyApplication {
    const Program& program;
    const FlowResults& results;
    const FlowApplication& application;
    FunctionID id;
    Function& function;
    TypeArena& types;
    BuildContext& build;
    const Vector<BitVector>& fieldsExpectedAsRead;
    VariableID firstRunMark;
    const FlowFirstTimeMarks* firstTimeMarks;
    bool provesEverything { false };
};

struct FlowBodyCounts {
    uint64_t removedChecks { 0 };
    uint64_t remainingChecks { 0 };
};

FlowBodyCounts applyBodyUpdate(const FlowBodyApplication&);
void demoteTailCallsWithChecks(const Program&, FunctionID, Function&, TypeArena&, BuildContext&);

Value makeAssumeType(ValueID operand, ProvenType);
Value makeValidateType(ValueID operand, TypeID validatedType);
Value makeValidateCallee(ValueID callee, TypeID validatedType);

bool isWorthNarrowing(const Program&, const Type& current, const Type& narrowed);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
