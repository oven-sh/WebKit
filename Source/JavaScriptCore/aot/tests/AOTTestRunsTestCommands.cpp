/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTestRunsTestCommands.h"

#if ENABLE(AOT)

#include "AOTFacts.h"
#include "AOTIRPrinter.h"
#include "AOTIRProgram.h"
#include "AOTTestRuns.h"

namespace JSC { namespace AOT { namespace Typed {

static void runTestRunCommand(TestCommandContext& context)
{
    OrderOfWorklist order = OrderOfWorklist::NewestFirst;
    for (const String& word : context.arguments) {
        if (word == "oldest-first"_s)
            order = OrderOfWorklist::OldestFirst;
        else if (word == "scattered"_s)
            order = OrderOfWorklist::Scattered;
        else {
            context.out.println("bad argument "_s, word);
            return;
        }
    }

    StagedBuild stage = context.stage(order);
    const Facts* facts = stage.facts();
    if (!facts)
        return;
    const Program& program = stage.program();
    IRPrinter printer(context.out, stage.program());

    for (uint32_t index = 0; index < program.modules.size(); ++index) {
        const auto& exports = program.modules[index].exports;
        for (uint32_t place = 0; place < exports.size(); ++place) {
            if (!mayComeThroughMock(program, facts->mocks(), ModuleID(index), place))
                continue;
            context.out.print("through-mock "_s);
            printer.print(ModuleID(index));
            context.out.print(" "_s);
            printer.print(exports[place].name);
            context.out.println();
        }
        if (mayComeThroughMock(program, facts->mocks(), ModuleID(index), nameThatModuleLacks)) {
            context.out.print("any-other-name "_s);
            printer.print(ModuleID(index));
            context.out.println();
        }
    }
    for (BuiltinID member : facts->builtinMembersThatTestsReplace()) {
        context.out.print("replaces "_s);
        printer.print(member);
        context.out.println();
    }
    for (const OpenedName& opened : program.openedNames) {
        context.out.print("adds "_s);
        printer.print(opened.holder);
        context.out.print(" "_s);
        printer.print(opened.name);
        context.out.println();
    }
    context.out.println("left-to-program "_s, program.builtinsWhoseLockIsLeftToProgram().size());
}

std::span<const TestCommand> testRunsTestCommands()
{
    static constexpr TestCommand commands[] {
        { "test-run"_s, runTestRunCommand, ProgramOfCommand::AsWritten, "[oldest-first|scattered]"_s },
    };
    return commands;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
