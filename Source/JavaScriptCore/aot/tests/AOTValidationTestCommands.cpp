/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTestCommand.h"

#if ENABLE(AOT)

#include "AOTCompile.h"
#include "AOTCompileOptionWords.h"
#include "AOTCompileOptions.h"
#include "AOTIRParser.h"
#include "AOTIRPrinter.h"
#include "AOTSchemaEnums.h"
#include "AOTStatistics.h"
#include "AOTVector.h"
#include <algorithm>
#include <wtf/StringPrintStream.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr ASCIILiteral validateWord = "validate"_s;

static std::optional<Wire::CompileOptions> parseReleaseOptions(TestCommandContext& context)
{
    Wire::CompileOptions options = makeDefaultCompileOptions();
    for (const String& word : context.arguments) {
        if (std::optional<String> sentence = applyOptionWord(options, word)) {
            context.out.println("error: ", word, ": ", *sentence);
            return std::nullopt;
        }
    }
    if (hasFlag(options.checks, Wire::CheckSet::Validate)) {
        context.out.println("error: validation-differences adds ", validateWord, " to its second build");
        return std::nullopt;
    }
    return options;
}

static std::unique_ptr<CompileOutput> tryCompileText(const String& text, const Wire::CompileOptions& options)
{
    BuildContext build(options);
    std::unique_ptr<Program> program = parseProgram(text, IRForm::AsRead, build);
    if (!program)
        return nullptr;
    return compileParsedProgram(WTF::move(program), build);
}

static void printBuild(PrintStream& out, ASCIILiteral name, const CompileOutput& output)
{
    for (const UserDiagnostic& diagnostic : output.diagnostics.userDiagnostics())
        out.println(name, " diagnostic ", nameOf(diagnostic.kind));
    for (const InternalError& error : output.diagnostics.internalErrors())
        out.println(name, " diagnostic ", nameOf(error.kind));
    out.println(name, " image ", output.image.isEmpty() ? "empty"_s : "not empty"_s);
}

static bool isStageTime(StatisticID statistic)
{
    return std::ranges::any_of(allMembersOf<Stage>, [&](Stage stage) {
        return Wire::timeOf(stage) == statistic;
    });
}

// Two statements that run side by side on two threads log the same edge twice; on one thread after the other, the second finds it known. The graph is the same.
static bool dependsOnWhichThreadRanWhat(StatisticID statistic)
{
    return statistic == StatisticID::FlowEdgesLogged || statistic == StatisticID::FlowBodiesTakenBack;
}

static void printDifferentStatistics(PrintStream& out, const Statistics& release, const Statistics& validated)
{
    Vector<uint64_t> releaseValues = release.values();
    Vector<uint64_t> validatedValues = validated.values();
    for (StatisticID statistic : allMembersOf<StatisticID>) {
        size_t index = static_cast<size_t>(statistic);
        if (releaseValues[index] == validatedValues[index] || isStageTime(statistic) || dependsOnWhichThreadRanWhat(statistic))
            continue;
        out.println("statistic ", nameOf(statistic), " release=", releaseValues[index], " validate=", validatedValues[index]);
    }
}

static void appendDiagnostics(BuildContext& build, const Diagnostics& diagnostics)
{
    build.diagnostics().append(Vector<UserDiagnostic>(diagnostics.userDiagnostics()));
    build.diagnostics().append(Vector<InternalError>(diagnostics.internalErrors()));
}

static void runValidationDifferences(TestCommandContext& context)
{
    if (context.form != IRForm::AsRead) {
        context.out.println("error: validation-differences starts from AsRead");
        return;
    }
    std::optional<Wire::CompileOptions> releaseOptions = parseReleaseOptions(context);
    if (!releaseOptions)
        return;
    Wire::CompileOptions validateOptions = *releaseOptions;
    std::optional<String> sentence = applyOptionWord(validateOptions, validateWord);
    RELEASE_ASSERT(!sentence);

    std::unique_ptr<Program> program = WTF::move(context.program);
    StringPrintStream printed;
    printProgram(printed, *program);
    String text = printed.toString();

    std::unique_ptr<CompileOutput> release = tryCompileText(text, *releaseOptions);
    std::unique_ptr<CompileOutput> validated = tryCompileText(text, validateOptions);
    if (!release || !validated) {
        context.out.println("error: the printed program does not parse");
        return;
    }
    printBuild(context.out, "release"_s, *release);
    printBuild(context.out, validateWord, *validated);
    printDifferentStatistics(context.out, release->statistics, validated->statistics);

    appendDiagnostics(context.build, release->diagnostics);
    appendDiagnostics(context.build, validated->diagnostics);
    context.build.statistics() = WTF::move(validated->statistics);
    context.losses = WTF::move(validated->losses);
}

std::span<const TestCommand> validationTestCommands()
{
    static constexpr TestCommand commands[] = {
        { "validation-differences"_s, IRForm::AsRead, runValidationDifferences, ProgramAsRead::WithLibrary },
    };
    return commands;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
