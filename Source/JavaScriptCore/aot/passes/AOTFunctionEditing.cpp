/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFunctionEditing.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTFunctionAnalyses.h"
#include "AOTPassRunner.h"
#include "AOTThreads.h"

namespace JSC { namespace AOT { namespace Typed {

void editFunctionsInParallelImpl(ProgramEditor& editor, BuildContext& build, std::span<const FunctionID> functions, std::span<const FunctionID> functionsBuiltByEdits, NOESCAPE const FunctionEdit& edit)
{
    if (functions.empty())
        return;

    const ParallelLoops& loops = build.parallelLoops();
    unsigned count = static_cast<unsigned>(functions.size());
    auto phases = makeUnique<InterningPhases>(editor, count);
    forEachIndexInParallel(loops, count, [&](unsigned index) {
        TaskArenas arenas(*phases, index);
        edit(index, functions[index], editor.at(functions[index]), arenas);
    });

    BodyRemaps remaps = phases->finish();
    build.destroyLater(WTF::move(phases));
    if (remaps.isIdentity())
        return;

    forEachIndexInParallel(loops, count, [&](unsigned index) {
        if (remaps.types.isIdentityFor(index) && remaps.strings.isIdentityFor(index))
            return;
        applyRemaps(remaps, editor.at(functions[index]));
    });
    forEachIndexInParallel(loops, static_cast<unsigned>(functionsBuiltByEdits.size()), [&](unsigned index) {
        applyRemaps(remaps, editor.at(functionsBuiltByEdits[index]));
    });
}

void simplifyEditedFunction(const PassRunner& runner, const Program& program, const OptimizationInputs& inputs, FunctionID id, Function& function, TaskArenas& arenas)
{
    runner.runSchedule(FunctionSchedule::Simplify, program, inputs, id, function, arenas);
    FunctionAnalyses analyses(AnalysisContext { runner.build(), program, &inputs.flow, &inputs.effects, id }, function);
    FunctionPassContext context { program, inputs, id, function, analyses, arenas.types, arenas.strings, runner.build() };
    if (runner.runFunctionPass(PassID::RemoveDeadCountedLoops, context) == PassOutcome::Changed)
        runner.runSchedule(FunctionSchedule::Simplify, program, inputs, id, function, arenas);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
