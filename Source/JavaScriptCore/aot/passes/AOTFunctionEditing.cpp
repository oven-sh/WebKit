/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFunctionEditing.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTBuildContext.h"
#include "AOTFunctionAnalyses.h"
#include "AOTPassRunner.h"
#include "AOTProgramCaches.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include "AOTThreshold.h"
#include "AOTVector.h"
#include <algorithm>
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

// An edit of a large function, and what is run on the function behind it, lasts long: it begins first, so that the loop does not end with it alone.
static Vector<uint32_t> findLargestFirst(const Program& program, const ParallelLoops& loops, std::span<const FunctionID> functions)
{
    AOT_THRESHOLD(size_t, fewestValues, 4096);
    Vector<uint64_t> keys = collectInParallel<uint64_t>(loops, static_cast<unsigned>(functions.size()), [&](unsigned index, Vector<uint64_t>& found) {
        size_t numberOfValues = program.at(functions[index]).values.size();
        if (numberOfValues >= fewestValues)
            found.append((UINT32_MAX - static_cast<uint64_t>(numberOfValues)) << 32 | index);
    });
    AOT::sort(keys);
    return keys.map([](uint64_t key) {
        return static_cast<uint32_t>(key);
    });
}

void editFunctionsInParallelImpl(ProgramEditor& editor, BuildContext& build, KindOfEdit kind, BehindEdit behindEdit, std::span<const FunctionID> functions, std::span<const FunctionID> functionsBuiltByEdits, NOESCAPE const FunctionEdit& edit)
{
    if (functions.empty())
        return;
    auto functionAt = [&](FunctionID id) -> Function& {
        return kind == KindOfEdit::Lowering ? editor.atForLowering(id) : editor.at(id);
    };

    BodyIndex* bodyIndex = behindEdit == BehindEdit::FitAndWriteRow ? editor.program().bodyIndex() : nullptr;
    BodyIDs idsInRows = editor.program().form() == IRForm::Prepared ? BodyIDs::Final : BodyIDs::MayBeProvisional;
    auto writeRow = [&](FunctionID id, const Function& function) {
        if (bodyIndex)
            bodyIndex->write(id, function, idsInRows);
    };

    const ParallelLoops& loops = build.parallelLoops();
    unsigned count = static_cast<unsigned>(functions.size());
    auto phases = makeUnique<InterningPhases>(editor, count);
    {
        // An edit changes functions only: layouts, variables and what flow found stay as they are until all edits are done.
        ProgramCaches caches(editor.program(), build);
        forEachIndexInParallel(loops, count, findLargestFirst(editor.program(), loops, functions).span(), [&](unsigned index) {
            TaskArenas arenas(*phases, index);
            Function& function = functionAt(functions[index]);
            edit(index, functions[index], function, arenas);
            if (behindEdit == BehindEdit::Nothing)
                return;
            shrinkToFit(function);
            if (!arenas.types.hasUsedProvisionalRecords())
                writeRow(functions[index], function);
        });
    }

    BodyRemaps remaps = phases->finish(&loops);
    build.destroyLater(WTF::move(phases));
    if (remaps.isIdentity())
        return;

    forEachIndexInParallel(loops, count, [&](unsigned index) {
        if (remaps.types.isIdentityFor(index) && remaps.strings.isIdentityFor(index))
            return;
        Function& function = functionAt(functions[index]);
        applyRemaps(remaps, function);
        writeRow(functions[index], function);
    });
    forEachIndexInParallel(loops, static_cast<unsigned>(functionsBuiltByEdits.size()), [&](unsigned index) {
        applyRemaps(remaps, editor.at(functionsBuiltByEdits[index]));
    });
}

void readFunctionsInParallel(const Program& program, BuildContext& build, std::span<const FunctionID> functions, NOESCAPE const FunctionReader& read)
{
    if (functions.empty())
        return;
    const ParallelLoops& loops = build.parallelLoops();
    std::optional<TypeInterningPhase> scratch;
    if (!program.types.hasOpenPhase())
        scratch.emplace(program.types, 0u, TypeInterningPhase::Scratch);
    {
        ProgramCaches caches(program, build);
        forEachIndexInParallel(loops, static_cast<unsigned>(functions.size()), findLargestFirst(program, loops, functions).span(), [&](unsigned index) {
            read(functions[index]);
        });
    }
    if (scratch)
        scratch->discard();
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
