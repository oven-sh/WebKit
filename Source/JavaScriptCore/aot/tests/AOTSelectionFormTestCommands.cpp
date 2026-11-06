/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSelectionTestCommands.h"

#if ENABLE(AOT)

#include "AOTAnalysisTestCommands.h"
#include "AOTCodeShapes.h"
#include "AOTIRPrinter.h"
#include "AOTInlineBuiltinForms.h"
#include "AOTOperatorForms.h"
#include "AOTRuleSupport.h"
#include "AOTSelectionContext.h"
#include "AOTSupportTestCommands.h"
#include <wtf/CommaPrinter.h>

namespace JSC { namespace AOT { namespace Typed {

static ASCIILiteral nameOf(InlineForm form)
{
    switch (form) {
    case InlineForm::Call:
        return "Call"_s;
    case InlineForm::TwoWay:
        return "TwoWay"_s;
    case InlineForm::Whole:
        return "Whole"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void forEachFunctionWithBody(const Program& program, NOESCAPE const Invocable<void(FunctionID)> auto& functor)
{
    for (size_t index = 0; index < program.functions.size(); ++index) {
        FunctionID id(index);
        if (program.at(id).hasBody())
            functor(id);
    }
}

static void printCodeShapes(PrintStream& out, FunctionPassContext& pass)
{
    chooseCodeShapes(pass, ChosenShapes::ChooseAgain);
    for (size_t index = 0; index < pass.function.blocks.size(); ++index) {
        BlockID id(index);
        const Block& block = pass.function.at(id);
        out.println("f"_s, pass.id.index(), " b"_s, index, " "_s, nameOf(block.shape), " heat="_s, FixedWidthDouble(pass.heatOf(id).predicted(), 0, 2), " once="_s, static_cast<unsigned>(pass.runsAtMostOnce(id)), " recorded="_s, nameOf(block.frequency.predicted()));
    }
}

void runCodeShapesTestCommand(TestCommandContext& context)
{
    CommandWords words(context);
    if (!words.finish())
        return;

    // What is computed for the program is the same for all its functions.
    Program& program = *context.program;
    PassRunner runner(context.build);
    Pipeline pipeline(program, runner);
    OptionSet<ProgramResult> needed = inputsOf(FunctionSchedule::Select);
    computeResultsForTesting(pipeline, needed);
    OptimizationInputs inputs = pipeline.createInputs(needed);
    TaskArenas arenas(program);
    forEachFunctionWithBody(program, [&](FunctionID id) {
        Function& function = program.at(id);
        FunctionAnalyses analyses(AnalysisContext { context.build, program, &inputs.flow, &inputs.effects, id }, function);
        FunctionPassContext pass { program, inputs, id, function, analyses, arenas.types, arenas.strings, context.build };
        printCodeShapes(context.out, pass);
    });
}

static void printLegalOperatorForms(PrintStream& out)
{
    for (Opcode opcode : allMembersOf<Opcode>) {
        if (!hasOperatorForm(opcode))
            continue;
        out.print(nameOf(opcode), ": "_s);
        CommaPrinter comma;
        for (OperatorForm form : allMembersOf<OperatorForm>) {
            if (isLegalOperatorForm(opcode, form))
                out.print(comma, nameOf(form), " calls="_s, static_cast<unsigned>(mayCallRuntime(opcode, form)));
        }
        out.print("\n"_s);
    }
}

static void printOperatorForm(PrintStream& out, const OperatorSite& site, ValueID id)
{
    const Value& value = site.typing.function.at(id);
    OperatorChoice choice = chooseOperatorForm(site, value);
    out.print("v"_s, id.index(), " "_s, nameOf(value.opcode), " "_s, nameOf(choice.form), " : "_s);
    printType(out, site.typing.program, choice.result);
    out.println(" ; "_s, choice.effects, " ; calls="_s, static_cast<unsigned>(mayCallRuntime(value.opcode, choice.form)));
}

static void printOperatorForms(PrintStream& out, FunctionPassContext& pass, std::optional<CodeShape> requestedShape)
{
    if (!requestedShape)
        chooseCodeShapes(pass, ChosenShapes::ChooseAgain);

    const Function& function = pass.function;
    const CFG& cfg = pass.analyses.cfg();
    TypingContext typing { pass.program, function, pass.types };
    for (size_t index = 0; index < function.blocks.size(); ++index) {
        BlockID id(index);
        if (!cfg.isReachable(id))
            continue;
        const Block& block = function.at(id);
        OperatorSite site { typing, pass.analyses.effectContext(), requestedShape.value_or(block.shape) };
        for (ValueID value : block.values) {
            if (hasOperatorForm(function.at(value).opcode))
                printOperatorForm(out, site, value);
        }
    }
}

void runOperatorFormsTestCommand(TestCommandContext& context)
{
    if (isOnlyWord(context.arguments, "legal"_s)) {
        printLegalOperatorForms(context.out);
        return;
    }

    CommandWords words(context);
    std::optional<CodeShape> requestedShape = words.parseOptional("shape"_s, filterParser(parseEnum<CodeShape>, [](CodeShape shape) {
        return shape != CodeShape::Unchosen;
    }));
    if (!words.finish())
        return;

    forEachFunctionWithBody(*context.program, [&](FunctionID id) {
        SelectionTestSession session(context, id);
        context.out.println("function f"_s, id.index());
        printOperatorForms(context.out, session.pass(), requestedShape);
    });
}

static void printInlineForm(PrintStream& out, SelectionContext& selection, ValueID id)
{
    const Program& program = selection.program();
    const Value& call = selection.function().at(id);
    std::optional<InlineBuiltin> builtin = inlineImplementationOf(program, call);
    std::optional<InlineBuiltinSite> site = findInlineBuiltinSite(selection, id);

    out.print("v"_s, id.index(), " "_s, program.builtins().textOf(overloadOf(program, call).builtin), " "_s, nameOf(call.builtinCallForm()), " "_s, builtin ? nameOf(*builtin) : "-"_s, " slow-path="_s, static_cast<unsigned>(site && site->hasSlowPath), " "_s);
    out.println(site ? nameOf(chooseInlineForm(*site)) : "-"_s);
}

void runInlineFormsTestCommand(TestCommandContext& context)
{
    CommandWords words(context);
    if (!words.finish())
        return;

    forEachFunctionWithBody(*context.program, [&](FunctionID id) {
        SelectionTestSession session(context, id);
        SelectionContext selection(session.pass());
        context.out.println("function f"_s, id.index());
        for (ValueID call : selection.candidates(Opcode::CallBuiltin))
            printInlineForm(context.out, selection, call);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
