/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTestCommand.h"

#if ENABLE(AOT)

#include "AOTCompile.h"
#include "AOTDerivedFunctions.h"
#include "AOTDiagnostics.h"
#include "AOTFunctionAnalyses.h"
#include "AOTIRPrinter.h"
#include "AOTInterningPhases.h"
#include "AOTLosses.h"
#include "AOTPass.h"
#include "AOTPassID.h"
#include "AOTPipeline.h"
#include "AOTPipelineInternal.h"
#include "AOTSchemaEnums.h"
#include "AOTStatistics.h"
#include "AOTTestRunner.h"
#include <wtf/ASCIICType.h>
#include <wtf/StringPrintStream.h>
#include <wtf/text/StringToIntegerConversion.h>

namespace JSC { namespace AOT { namespace Typed {

bool runReadStage(Program& program, BuildContext& build)
{
    PassRunner runner(build);
    Pipeline pipeline(program, runner);
    pipeline.runReadStage();
    return !build.diagnostics().hasErrors();
}

static std::optional<PassID> parsePassArgument(TestCommandContext& context, ASCIILiteral command)
{
    std::optional<PassID> pass;
    if (context.arguments.size() == 1)
        pass = passNamed(context.arguments[0]);
    if (!pass)
        context.out.println("error: ", command, " takes the name of a pass");
    return pass;
}

static bool checkFormIsAsRead(TestCommandContext& context, ASCIILiteral command)
{
    if (context.form == IRForm::AsRead)
        return true;
    context.out.println("error: ", command, " starts from AsRead");
    return false;
}

static bool runPassAndPrintResults(TestCommandContext& context, Pipeline& pipeline, PassID pass)
{
    if (!canRunPassForTesting(pass, context.program->form())) {
        context.out.println("error: ", nameOf(pass), " does not run in form ", nameOf(context.program->form()));
        return false;
    }

    StringPrintStream results;
    PassOutcome outcome = runPassForTesting(pipeline, pass, results);
    context.form = context.program->form();
    context.out.print(results.toUTF8CString());
    context.out.println("outcome: ", outcome == PassOutcome::Changed ? "Changed"_s : "Unchanged"_s);
    return !context.build.diagnostics().hasErrors();
}

static void printWitnessID(PrintStream& out, char letter, uint32_t index)
{
    if (index == Wire::none)
        out.print(" -");
    else
        out.print(" ", letter, index);
}

// A build keeps declines only under report=Declines.
static void printDeclines(TestCommandContext& context)
{
    for (const Wire::DeclineRecord& decline : context.build.takeDeclines(context.program.get())) {
        context.out.print("decline ", nameOf(decline.kind));
        printWitnessID(context.out, 'f', decline.witness.function);
        printWitnessID(context.out, 'v', decline.witness.value);
        context.out.println(" ", nameOf(decline.limit), " [", decline.count, "]", DeclineReasonField { decline });
    }
}

static void runPassCommand(TestCommandContext& context)
{
    Vector<PassID> passes;
    for (const String& word : context.arguments) {
        std::optional<PassID> pass = passNamed(word);
        if (!pass) {
            context.out.println("error: unknown pass ", word);
            return;
        }
        passes.append(*pass);
    }
    if (passes.isEmpty()) {
        context.out.println("error: pass takes the name of a pass");
        return;
    }

    context.build.resolveFunctionToDump(*context.program);
    PassRunner runner(context.build);
    Pipeline pipeline(*context.program, runner);
    for (PassID pass : passes) {
        if (!runPassAndPrintResults(context, pipeline, pass))
            return;
    }
    printDeclines(context);
    printProgram(context.out, *context.program);
}

static void runPipelineCommand(TestCommandContext& context)
{
    std::optional<PassID> last = parsePassArgument(context, "pipeline"_s);
    if (!last || !checkFormIsAsRead(context, "pipeline"_s))
        return;

    context.build.resolveFunctionToDump(*context.program);
    PassRunner runner(context.build, *last);
    Pipeline pipeline(*context.program, runner);
    pipeline.runAllStages();
    context.form = context.program->form();
    if (context.build.diagnostics().hasErrors())
        return;
    pipeline.dumpResultOf(*last, context.out);
    printDeclines(context);
    printProgram(context.out, *context.program);
}

static void runCompileCommand(TestCommandContext& context)
{
    if (!checkFormIsAsRead(context, "compile"_s))
        return;
    Vector<StatisticID> printedStatistics;
    for (const String& word : context.arguments) {
        std::optional<StatisticID> statistic = parseEnum<StatisticID>(word);
        if (!statistic || *statistic == StatisticID::None) {
            context.out.println("error: unknown statistic ", word);
            return;
        }
        printedStatistics.append(*statistic);
    }

    std::unique_ptr<CompileOutput> output = compileParsedProgram(WTF::move(context.program), context.build);
    for (const UserDiagnostic& diagnostic : output->diagnostics.userDiagnostics())
        context.out.println("diagnostic ", nameOf(diagnostic.kind));
    for (const InternalError& error : output->diagnostics.internalErrors())
        context.out.println("diagnostic ", nameOf(error.kind));
    for (const Wire::LossRecord& loss : output->losses)
        context.out.println("loss ", nameOf(loss.limit), " x", loss.count);
    context.out.println("image ", output->image.isEmpty() ? "empty"_s : "not empty"_s);
    Vector<uint64_t> values = output->statistics.values();
    for (StatisticID statistic : printedStatistics)
        context.out.println("statistic ", nameOf(statistic), " ", values[static_cast<size_t>(statistic)]);

    context.build.diagnostics().append(Vector<UserDiagnostic>(output->diagnostics.userDiagnostics()));
    context.build.diagnostics().append(Vector<InternalError>(output->diagnostics.internalErrors()));
    for (StatisticID statistic : allMembersOf<StatisticID>) {
        if (uint64_t value = values[static_cast<size_t>(statistic)]; value && statistic != StatisticID::None)
            context.build.statistics().count(statistic, value);
    }
    context.losses = WTF::move(output->losses);
}

[[noreturn]] static void runCrashCommand(TestCommandContext&)
{
    CRASH();
}

static void runListPassesCommand(TestCommandContext& context)
{
    for (PassID id : allMembersOf<PassID>) {
        if (id == PassID::None)
            continue;
        const PassDescriptor& descriptor = descriptorOf(id);
        context.out.print(nameOf(id), " ", descriptor.scope == PassScope::Program ? "Program"_s : "Function"_s, " ");
        context.out.print(descriptor.necessity == PassNecessity::Required ? "Required"_s : "Optional"_s, " ");
        context.out.print(nameOf(descriptor.firstForm), "..", nameOf(descriptor.lastForm));
        if (descriptor.produces)
            context.out.print(" -> ", nameOf(*descriptor.produces));
        context.out.println();
    }
}

static constexpr OptionSet<ProgramResult> functionPassInputs {
    ProgramResult::Flow, ProgramResult::CallGraph, ProgramResult::EffectSummaries, ProgramResult::Heat, ProgramResult::InliningSummaries
};

static void printSubject(PrintStream& out, ValueID value, const Subject& subject)
{
    out.print("  v", value.index(), " subject=f", subject.function.index());
    if (subject.value)
        out.print(" v", subject.value.index());
    out.println(" @", subject.location.position);
}

static void printPassContext(PrintStream& out, FunctionPassContext& pass)
{
    out.println("f", pass.id.index(), ":");
    for (unsigned index = 0; index < pass.function.blocks.size(); ++index) {
        BlockID block(index);
        out.print("  b", index, " heat=");
        out.printf("%.3g", pass.heatOf(block));
        out.println(" runsAtMostOnce=", pass.runsAtMostOnce(block) ? "true"_s : "false"_s);
    }
    for (const Block& block : pass.function.blocks) {
        for (ValueID value : block.values) {
            if (pass.function.at(value).hasPosition())
                printSubject(out, value, pass.subject(value));
        }
    }
}

static void runPassContextCommand(TestCommandContext& context)
{
    if (!context.arguments.empty()) {
        context.out.println("error: pass-context takes no argument");
        return;
    }

    Program& program = *context.program;
    PassRunner runner(context.build);
    Pipeline pipeline(program, runner);
    computeResultsForTesting(pipeline, functionPassInputs);
    if (context.build.diagnostics().hasErrors())
        return;

    OptimizationInputs inputs = pipeline.createInputs(functionPassInputs);
    TaskArenas arenas(program);
    for (unsigned index = 0; index < program.functions.size(); ++index) {
        Function& function = program.functions[index];
        if (!function.hasBody())
            continue;
        FunctionID id(index);
        FunctionAnalyses analyses(AnalysisContext { context.build, program, &inputs.flow, &inputs.effects, id }, function);
        FunctionPassContext pass { program, inputs, id, function, analyses, arenas.types, arenas.strings, context.build };
        printPassContext(context.out, pass);
    }
}

static std::optional<uint32_t> parseNumber(StringView digits)
{
    if (digits.isEmpty() || !digits.containsOnly<isASCIIDigit>())
        return std::nullopt;
    return parseInteger<uint32_t>(digits, 10, WTF::ParseIntegerWhitespacePolicy::Disallow);
}

static std::optional<DerivedFunctionKey> parseValidationNodeKey(const Program& program, FunctionRole role, StringView target)
{
    size_t dot = target.find('.');
    if (dot == notFound)
        return std::nullopt;
    std::optional<uint32_t> expectedResult = parseNumber(target.substring(1, dot - 1));
    std::optional<uint32_t> node = parseNumber(target.substring(dot + 1));
    if (!expectedResult || !node || *expectedResult >= program.expectedResults.size())
        return std::nullopt;
    if (*node >= program.at(ExpectedResultID(*expectedResult)).validation.size())
        return std::nullopt;
    return derivedFunctionKey(role, ExpectedResultID(*expectedResult), *node);
}

static std::optional<DerivedFunctionTarget> targetNamedBy(char letter)
{
    switch (letter) {
    case 'f':
        return DerivedFunctionTarget::Function;
    case 'L':
        return DerivedFunctionTarget::Layout;
    case 'g':
        return DerivedFunctionTarget::Type;
    case 'x':
        return DerivedFunctionTarget::ValidationNode;
    default:
        return std::nullopt;
    }
}

static std::optional<DerivedFunctionKey> parseDerivedFunctionKey(const Program& program, const DerivedFunctionRole& description, StringView target)
{
    FunctionRole role = description.role;
    if (target.isEmpty() || targetNamedBy(target[0]) != description.target)
        return std::nullopt;
    switch (target[0]) {
    case 'x':
        return parseValidationNodeKey(program, role, target);
    case 'f': {
        size_t colon = target.find(':');
        std::optional<uint32_t> function = parseNumber(target.substring(1, colon == notFound ? target.length() - 1 : colon - 1));
        std::optional<uint32_t> parameter = colon == notFound ? std::optional<uint32_t>(0) : parseNumber(target.substring(colon + 1));
        if (!function || !parameter || *function >= program.functions.size())
            return std::nullopt;
        return derivedFunctionKey(role, FunctionID(*function), *parameter);
    }
    case 'L': {
        std::optional<uint32_t> layout = parseNumber(target.substring(1));
        if (!layout || *layout >= program.layouts.size())
            return std::nullopt;
        return derivedFunctionKey(role, LayoutID(*layout));
    }
    case 'g': {
        std::optional<uint32_t> variable = parseNumber(target.substring(1));
        if (!variable || *variable >= program.variables.size())
            return std::nullopt;
        return derivedFunctionKey(role, program.at(VariableID(*variable)).declared.type);
    }
    default:
        return std::nullopt;
    }
}

static void runDerivedFunctionsCommand(TestCommandContext& context)
{
    Program& program = *context.program;
    Vector<DerivedFunctionKey> keys;
    for (const String& word : context.arguments) {
        size_t colon = word.find(':');
        std::optional<FunctionRole> role = colon == notFound ? std::nullopt : parseEnum<FunctionRole>(StringView(word).left(colon));
        if (!role) {
            context.out.println("error: bad argument ", word);
            return;
        }
        const DerivedFunctionRole* description = findDerivedFunctionRole(*role);
        if (!description || description->creation == DerivedFunctionCreation::ByProgramPass) {
            context.out.println("error: ", nameOf(*role), " is not derived on request");
            return;
        }
        std::optional<DerivedFunctionKey> key = parseDerivedFunctionKey(program, *description, StringView(word).substring(colon + 1));
        if (!key) {
            context.out.println("error: bad argument ", word);
            return;
        }
        keys.append(*key);
    }
    if (keys.isEmpty()) {
        context.out.println("error: derived-functions takes at least one key");
        return;
    }

    PassRunner runner(context.build);
    Pipeline pipeline(program, runner);
    computeResultsForTesting(pipeline, ProgramResult::Flow);
    if (context.build.diagnostics().hasErrors())
        return;
    commitRequestedFunctionsForTesting(pipeline, keys.span(), context.out);
    if (context.build.diagnostics().hasErrors())
        return;
    printDeclines(context);
    printProgram(context.out, program);
}

std::span<const TestCommand> pipelineTestCommands()
{
    static constexpr TestCommand commands[] {
        { "pass"_s, IRForm::Prepared, runPassCommand },
        { "pipeline"_s, IRForm::AsRead, runPipelineCommand, RawWordCheck::Run, ProgramAsRead::WithLibrary },
        { "compile"_s, IRForm::AsRead, runCompileCommand, RawWordCheck::Run, ProgramAsRead::WithLibrary },
        { "crash"_s, std::nullopt, runCrashCommand },
        { "list-passes"_s, std::nullopt, runListPassesCommand },
        { "pass-context"_s, IRForm::Prepared, runPassContextCommand },
        { "derived-functions"_s, IRForm::Prepared, runDerivedFunctionsCommand },
    };
    return commands;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
