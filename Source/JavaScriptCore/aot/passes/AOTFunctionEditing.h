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

enum class KindOfEdit : uint8_t { Any, Lowering };
// Nothing: for a pass that changes the records of functions by itself behind its edits. A row also holds what stands in the record.
enum class BehindEdit : uint8_t { FitAndWriteRow, Nothing };

void editFunctionsInParallelImpl(ProgramEditor&, BuildContext&, KindOfEdit, BehindEdit, std::span<const FunctionID> functions, std::span<const FunctionID> functionsBuiltByEdits, NOESCAPE const FunctionEdit&);

void editFunctionsInParallel(ProgramEditor& editor, BuildContext& build, std::span<const FunctionID> functions, NOESCAPE const Invocable<void(unsigned, FunctionID, Function&, TaskArenas&)> auto& edit)
{
    editFunctionsInParallelImpl(editor, build, KindOfEdit::Any, BehindEdit::FitAndWriteRow, functions, { }, edit);
}

void editFunctionsInParallel(ProgramEditor& editor, BuildContext& build, BehindEdit behindEdit, std::span<const FunctionID> functions, NOESCAPE const Invocable<void(unsigned, FunctionID, Function&, TaskArenas&)> auto& edit)
{
    editFunctionsInParallelImpl(editor, build, KindOfEdit::Any, behindEdit, functions, { }, edit);
}

void editFunctionsInParallel(ProgramEditor& editor, BuildContext& build, std::span<const FunctionID> functions, std::span<const FunctionID> functionsBuiltByEdits, NOESCAPE const Invocable<void(unsigned, FunctionID, Function&, TaskArenas&)> auto& edit)
{
    editFunctionsInParallelImpl(editor, build, KindOfEdit::Any, BehindEdit::FitAndWriteRow, functions, functionsBuiltByEdits, edit);
}

// See ProgramEditor::atForLowering().
void lowerFunctionsInParallel(ProgramEditor& editor, BuildContext& build, std::span<const FunctionID> functions, std::span<const FunctionID> functionsBuiltByEdits, NOESCAPE const Invocable<void(unsigned, FunctionID, Function&, TaskArenas&)> auto& edit)
{
    editFunctionsInParallelImpl(editor, build, KindOfEdit::Lowering, BehindEdit::FitAndWriteRow, functions, functionsBuiltByEdits, edit);
}

void simplifyEditedFunction(const PassRunner&, const Program&, const OptimizationInputs&, FunctionID, Function&, TaskArenas&);

// Reads one function, which has its final ids, and no other body.
using FunctionReader = ScopedLambda<void(FunctionID)>;
// With the caches of the program and an interning phase for scratch arenas open around the loop, as they are around the edits above.
void readFunctionsInParallel(const Program&, BuildContext&, std::span<const FunctionID>, NOESCAPE const FunctionReader&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
