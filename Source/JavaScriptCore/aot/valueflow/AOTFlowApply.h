/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCFG.h"
#include "AOTFlowApplication.h"
#include <span>

namespace JSC { namespace AOT { namespace Typed {

class IRBuilder;

void applyFlowResults(Program&, FlowResults&, BuildContext&);
// Flow follows what a check turns away through the same operations, so what it holds of a position holds whether the checks of a body stand or go. The type that the rules give a stored value may be owed to those checks.
// So flow's word is written down in front of each store of a value that stands behind a check. Not for the very value that a check tests: flow takes a value that it knows nothing of to lie in the type of the check.
// applyBodyUpdate() does it. For whoever puts a check into a body behind that.
void assumeTypesOfPositionsAtStores(const Program&, FunctionID, Function&, TypeArena&, BuildContext&);
// The copy of a body that runs without its checks takes values from the typed code as well. Where nothing tells two classes apart, a test gives its value the tested type, which need not lie inside the type in front of the test.
// The body without its checks stores what fits each position, and so does the typed code. So flow's word is written down in front of each store of the copy whose value is typed outside its position.
bool assumeTypesOfPositionsAtStoresInCopy(IRBuilder&, Function&, TypeArena&, std::span<const BlockID> copiedBlocks);
// False for the slot of an object that a store tests: whoever takes a check away stores what no longer fits behind the allocation, and of such a field flow leaves out the primitives that those stores turn away.
bool takesOnlyWhatFlowHolds(const Program&, const Value& store, uint32_t operandIndex);
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
    // Every entry into the function is a construction.
    bool isOnlyConstructed { false };
};

struct FlowBodyCounts {
    uint64_t removedChecks { 0 };
    uint64_t remainingChecks { 0 };
};

FlowBodyCounts applyBodyUpdate(const FlowBodyApplication&);
void demoteTailCallsWithChecks(const Program&, FunctionID, Function&, TypeArena&, BuildContext&);
void initializeMembersOfListedHolders(const Program&, const FlowResults&, FunctionID, Function&, TypeArena&, BuildContext&);

Value makeAssumeType(ValueID operand, ProvenType);
Value makeValidateType(ValueID operand, TypeID validatedType);
Value makeValidateCallee(ValueID callee, TypeID validatedType);

bool isWorthNarrowing(const Program&, const Type& current, const Type& narrowed);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
