/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTestCommand.h"

#if ENABLE(AOT)

#include "AOTFrameMaps.h"
#include "AOTFunctionAnalyses.h"
#include "AOTSupportTestCommands.h"

namespace JSC { namespace AOT { namespace Typed {

static void runFrameMaps(TestCommandContext& context)
{
    if (!context.arguments.empty()) {
        printBadArgument(context.out, context.arguments.front());
        return;
    }
    const Program& program = *context.program;
    for (size_t index = 0; index < program.functions.size(); ++index) {
        FunctionID id(index);
        const Function& function = program.at(id);
        if (!function.hasBody() || !function.representationsAreChosen)
            continue;
        FunctionAnalyses analyses(AnalysisContext { context.build, program, nullptr, nullptr, id }, function);
        FrameMaps maps = computeFrameMaps(program, function, analyses);
        if (!maps.numberOfSuspensions())
            continue;
        context.out.println("f", id.index(), ":");
        maps.dump(context.out, function);
    }
}

std::span<const TestCommand> collectorTestCommands()
{
    static constexpr TestCommand commands[] {
        { "frame-maps"_s, IRForm::Placed, runFrameMaps },
    };
    return commands;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
