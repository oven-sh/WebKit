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
#include "AOTRelationalSummaries.h"
#include "AOTTypeArena.h"
#include "AOTVector.h"
#include <optional>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

class FunctionAnalyses;
class IRBuilder;

// Ends with computeOperandSummaries().
void applyFlowResults(Program&, FlowResults&, BuildContext&, LinkedFunctions = LinkedFunctions::All);
// What becomes of the values that arrive at the entry parameters of the functions and of those that lie in the fields of environments.
void computeOperandSummaries(const Program&, FlowResults&, BuildContext&);
void checkLockedBuiltinWrites(const Program&, const FlowResults&, BuildContext&);
void validateFlowResults(const Program&, const FlowResults&, BuildContext&);

// A test of the validating build: whatever the types of the program say counts as proven, so the program has to end wherever a value of another type arrives.
bool provesEverything(const BuildContext&);
struct FlowBodyApplication;
bool provesEverythingIn(const FlowBodyApplication&);

struct FlowApplyContext {
    Program& program;
    const FlowResults& results;
    const FlowApplication& application;
    BuildContext& build;
};

void applyPosition(DeclaredType&, const FlowPositionUpdate&);

void applyLayoutUpdates(const FlowApplyContext&);
// By layout, then by appended field: the type that the families of the layout declare for the key. None where they declare none, or anything. Reads the field types as declared.
Vector<Vector<std::optional<Type>>> findTypesDeclaredInFamilies(const Program&, const Vector<FlowLayoutUpdate>&, TypeArena&);
Vector<BitVector> findAppendedFieldsDeclaredInFamilies(const Program&, const Vector<FlowLayoutUpdate>&, const ParallelLoops&);
void applyVariableUpdates(const FlowApplyContext&);

void createBoundFunctions(const FlowApplyContext&, ProgramEditor&);
void applySignatureUpdates(const FlowApplyContext&);
void removeUnreachedBodies(const FlowApplyContext&, ProgramEditor&);
void cutCallsOfUnreachedVersions(const FlowApplyContext&);
void dropDataOfUnreachedCode(const FlowApplyContext&);
void narrowContentsOfData(const FlowApplyContext&);

// By layout: the fields whose type was only expected while flow looked at the program.
void applyBodyUpdates(const FlowApplyContext&, ProgramEditor&, const Vector<BitVector>& fieldsExpectedAsRead);

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
    const Vector<BitVector>& fieldsExpectedAsRead;
    VariableID firstRunMark { };
    const FlowFirstTimeMarks* firstTimeMarks { nullptr };
};

struct FlowBodyCounts {
    uint64_t removedChecks { 0 };
    uint64_t remainingChecks { 0 };
};

FlowBodyCounts applyBodyUpdate(const FlowBodyApplication&);
void demoteTailCallsWithChecks(const Program&, FunctionID, Function&, TypeArena&, BuildContext&);

void applyStamps(const FlowBodyApplication&, const FlowBodyUpdate&);

struct FlowCastInsertion {
    const FlowBodyApplication& body;
    FunctionAnalyses& analyses;
    IRBuilder& builder;
};

// Answers the number of checks that flow proves to pass. Each has become an AssumeType.
uint64_t applyCheckUpdates(const FlowBodyApplication&, const FlowBodyUpdate&);
void insertProofCasts(const FlowCastInsertion&, const FlowBodyUpdate&);
void insertSymbolConstants(const FlowCastInsertion&, const FlowBodyUpdate&);
void createBoundClosures(const FlowCastInsertion&, const FlowBodyUpdate&);
void replaceDefinedValues(const FlowCastInsertion&, const FlowBodyUpdate&);
Vector<ValueID> insertCastsForRequiredTypes(const FlowCastInsertion&);
void insertRuntimeValidation(const FlowCastInsertion&);

Value makeAssumeType(ValueID operand, TypeID castType);
Value makeValidateType(ValueID operand, TypeID validatedType);
Value makeValidateCallee(ValueID callee, TypeID validatedType);

bool isWorthNarrowing(const Program&, const Type& current, const Type& narrowed);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
