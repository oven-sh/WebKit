/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinIDTypes.h"
#include "AOTValueFlowAnalysis.h"
#include "AOTVector.h"

namespace JSC { namespace AOT { namespace Typed {

// The events of the solved program, in the order of the program: each operation of a reached function that lets in code the build has not seen, or that names a module at run time.
// Among them, with a cause of its own, each call that may load machine code. That is no event of the solve: see FlowProgramTables::loadsOfMachineCode.
Vector<UnseenCodeRecord> unseenCodeRecordsOf(ValueFlowSolution&);
bool unseenCodeCanRun(const ValueFlowSolution&);
// Text that an evaluator runs finds the realm closed and locked, as native code does. So does a module that stays outside the image. Any other unseen code changes it.
bool unseenCodeChangesRealm(const ValueFlowSolution&);
// A module that stays outside the image, or code that changes the realm: it comes to every built-in that there is.
bool unseenCodeHasLoader(const ValueFlowSolution&);
bool modulesAreLoadedByName(const ValueFlowSolution&);
// A load of the program's own takes a name that nobody knows. Nothing else names a file of the program: the run time refuses evaluated text an import(), and code from outside the image the files of the image.
bool filesOfProgramAreLoadedByName(const ValueFlowSolution&);
// In ascending order.
Vector<FunctionID> functionsHeldButNotCalled(const ValueFlowSolution&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
