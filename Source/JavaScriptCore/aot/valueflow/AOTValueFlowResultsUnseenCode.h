/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTValueFlowAnalysis.h"

namespace JSC { namespace AOT { namespace Typed {

Vector<UnseenCodeRecord> unseenCodeRecordsOf(ValueFlowSolution&);
inline bool isRaised(const ValueFlowSolution& solution, ValueFlowHostNode flag)
{
    return solution.sets().contains(solution.solver().possibleValuesOf(solution.bodies().nodes().ofHost(flag)), ValueFlowUniverse::unknownObject());
}
inline bool unseenCodeCanRun(const ValueFlowSolution& solution) { return isRaised(solution, ValueFlowHostNode::UnseenCodeRuns); }
inline bool unseenCodeChangesRealm(const ValueFlowSolution& solution) { return isRaised(solution, ValueFlowHostNode::UnseenCodeChangesRealm); }
inline bool unseenCodeHasLoader(const ValueFlowSolution& solution) { return isRaised(solution, ValueFlowHostNode::UnseenCodeHasLoader); }
// A copy of a file from disk names what it imports to the loader of the host, which begins a file that nothing has run yet.
inline bool copiesOfFilesNameWhatTheyImport(const ValueFlowSolution& solution)
{
    return isRaised(solution, ValueFlowHostNode::UnseenCopiesOfFilesMayRun)
        && std::ranges::any_of(solution.program().modules, [](const Module& module) { return module.flags.contains(ModuleFlags::MayBeEvicted); });
}
inline bool filesOfProgramAreLoadedByName(const ValueFlowSolution& solution) { return isRaised(solution, ValueFlowHostNode::ModulesAreRequiredByName) || isRaised(solution, ValueFlowHostNode::ModulesAreImportedByName) || copiesOfFilesNameWhatTheyImport(solution); }
inline bool modulesAreLoadedByName(const ValueFlowSolution& solution) { return filesOfProgramAreLoadedByName(solution) || isRaised(solution, ValueFlowHostNode::LibraryModulesAreRequiredByName) || isRaised(solution, ValueFlowHostNode::LibraryModulesAreLoadedUnderSpelledNames); }
Vector<FunctionID> functionsHeldButNotCalled(const ValueFlowSolution&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
