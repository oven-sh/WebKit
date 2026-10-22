/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinIDTypes.h"
#include "AOTValueFlowAnalysis.h"
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

// The events of the solved program, in the order of the program: each operation of a reached function that lets in code the build has not seen, or that names a module at run time.
// Behind them, in the order of the table, each built-in whose application is an event and that is applied where no operation spells the call out: as a callback or an accessor, through a built-in that passes the call on,
// or by a caller that nobody follows, because it has escaped or the analysis has stopped following it.
// A call of `callee` loads machine code: an N-API addon, a library, or C that bun:ffi compiles on the spot. `named` is what a program names to get there.
struct LoaderOfMachineCode {
    BuiltinID callee;
    BuiltinID named;
};
inline constexpr LoaderOfMachineCode loadersOfMachineCode[] = {
    { Builtins::ProcessDlopen, Builtins::ProcessDlopen },
    { Builtins::NativeFunctionsFfiRsBun__FFI__cc, Builtins::BunFfiCc },
    { Builtins::BunFFIDlopen, Builtins::BunFfiDlopen },
    { Builtins::BunFFILinkSymbols, Builtins::BunFfiLinkSymbols },
    { Builtins::BunFFICfunction, Builtins::BunFfiCFunction },
};

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
// In ascending order. Code that nobody compiles holds the closure of each of these through what a module of the library exports, and the solve has not let that code call it.
// Such a function is in the image only if something else enters it, so a call of it by value may throw the error of a function that was not compiled.
Vector<FunctionID> functionsHeldButNotCalled(const ValueFlowSolution&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
