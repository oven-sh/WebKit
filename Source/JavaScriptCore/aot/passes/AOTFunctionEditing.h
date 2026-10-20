/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTInterningPhases.h"
#include "AOTPass.h"
#include <span>
#include <wtf/ScopedLambda.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class PassRunner;

using FunctionEdit = ScopedLambda<void(unsigned index, FunctionID, Function&, TaskArenas&)>;

void editFunctionsInParallelImpl(ProgramEditor&, BuildContext&, std::span<const FunctionID> functions, std::span<const FunctionID> functionsBuiltByEdits, NOESCAPE const FunctionEdit&);

void editFunctionsInParallel(ProgramEditor& editor, BuildContext& build, std::span<const FunctionID> functions, NOESCAPE const Invocable<void(unsigned, FunctionID, Function&, TaskArenas&)> auto& edit)
{
    editFunctionsInParallelImpl(editor, build, functions, { }, edit);
}

void editFunctionsInParallel(ProgramEditor& editor, BuildContext& build, std::span<const FunctionID> functions, std::span<const FunctionID> functionsBuiltByEdits, NOESCAPE const Invocable<void(unsigned, FunctionID, Function&, TaskArenas&)> auto& edit)
{
    editFunctionsInParallelImpl(editor, build, functions, functionsBuiltByEdits, edit);
}

void simplifyEditedFunction(const PassRunner&, const Program&, const OptimizationInputs&, FunctionID, Function&, TaskArenas&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
