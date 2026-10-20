/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCFG.h"
#include "AOTFlowApplication.h"
#include "AOTIR.h"
#include "AOTTypeArena.h"
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

class FunctionAnalyses;
class IRBuilder;

struct FlowApplyContext {
    Program& program;
    const FlowResults& results;
    const FlowApplication& application;
    BuildContext& build;
};

void applyPosition(DeclaredType&, const FlowPositionUpdate&);

void applyLayoutUpdates(const FlowApplyContext&);
void keepExpectedIntegersInSlots(Program&, BuildContext&);
void applyVariableUpdates(const FlowApplyContext&);

void createVersions(const FlowApplyContext&, ProgramEditor&);
void createBoundFunctions(const FlowApplyContext&, ProgramEditor&);
void applySignatureUpdates(const FlowApplyContext&);
void predictReceiversOfMembers(const FlowApplyContext&, ProgramEditor&);
void removeUnreachedBodies(const FlowApplyContext&, ProgramEditor&);

void applyBodyUpdates(const FlowApplyContext&, ProgramEditor&);

// The marks of a validating build for the one store of a variable and the one evaluation of a class. Empty, or one place for each variable and each layout.
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
    VariableID firstRunMark { };
    const FlowFirstTimeMarks* firstTimeMarks { nullptr };
};

struct FlowBodyCounts {
    uint64_t removedClaims { 0 };
    uint64_t remainingClaims { 0 };
};

FlowBodyCounts applyBodyUpdate(const FlowBodyApplication&);
void demoteTailCallsWithClaims(const Program&, FunctionID, Function&, TypeArena&, BuildContext&);

void applyStamps(const FlowBodyApplication&, const FlowBodyUpdate&);

struct FlowCastInsertion {
    const FlowBodyApplication& body;
    FunctionAnalyses& analyses;
    IRBuilder& builder;
};

FlowBodyCounts applyClaimUpdates(const FlowBodyApplication&, const FlowBodyUpdate&);
void insertResultClaims(const FlowCastInsertion&, const FlowBodyUpdate&, FlowBodyCounts&);
void insertProofCasts(const FlowCastInsertion&, const FlowBodyUpdate&);
void insertSymbolConstants(const FlowCastInsertion&, const FlowBodyUpdate&);
void startReadsAtPrototype(const FlowCastInsertion&, const FlowBodyUpdate&);
void createBoundClosures(const FlowCastInsertion&, const FlowBodyUpdate&);
void replaceDefinedValues(const FlowCastInsertion&, const FlowBodyUpdate&);
void createDictionaries(const FlowCastInsertion&, const FlowBodyUpdate&);
Vector<ValueID> insertCastsForRequiredTypes(const FlowCastInsertion&);
void insertRuntimeValidation(const FlowCastInsertion&);

Value makeAssumeType(ValueID operand, TypeID castType);
Value makePredict(ValueID operand, TypeID predicted);
Value makeValidateType(ValueID operand, TypeID validatedType);

bool isWorthNarrowing(const Program&, const Type& current, const Type& narrowed);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
