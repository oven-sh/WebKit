/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSupportTestCommands.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCurrentWork.h"
#include "AOTIRParser.h"
#include "AOTLimits.h"
#include "AOTTestHarness.h"
#include "ResourceExhaustion.h"
#include <memory>

namespace JSC { namespace AOT { namespace Typed {

void printBadArgument(PrintStream& out, StringView word)
{
    out.println("error: bad argument "_s, word);
}

void printMissingArgument(PrintStream& out)
{
    out.println("error: missing argument"_s);
}

void printCheck(PrintStream& out, ASCIILiteral name, bool passed)
{
    out.println(name, ": "_s, passed ? "ok"_s : "FAILED"_s);
}

ASCIILiteral yesOrNo(bool value)
{
    return value ? "yes"_s : "no"_s;
}

bool isOnlyWord(std::span<const String> words, ASCIILiteral word)
{
    return words.size() == 1 && words[0] == word;
}

std::optional<unsigned> parseThreadCount(StringView text)
{
    auto threads = parseTestNumber<unsigned>(text);
    return threads && *threads && *threads <= Limits::maxWorkers ? threads : std::nullopt;
}

bool takeCommonTestWord(StringView word, CommonTestWords& words)
{
    constexpr ASCIILiteral threadsPrefix = "threads="_s;
    constexpr ASCIILiteral seedPrefix = "seed="_s;
    if (word.startsWith(threadsPrefix)) {
        auto threads = parseThreadCount(word.substring(threadsPrefix.length()));
        if (!threads)
            return false;
        words.threads = *threads;
        return true;
    }
    if (word.startsWith(seedPrefix)) {
        auto seed = parseTestNumber<unsigned>(word.substring(seedPrefix.length()));
        if (!seed)
            return false;
        words.seed = *seed;
        return true;
    }
    return false;
}

std::optional<CommonTestWords> parseCommonTestWords(PrintStream& out, std::span<const String> words)
{
    CommonTestWords commonWords;
    for (const String& word : words) {
        if (!takeCommonTestWord(word, commonWords)) {
            printBadArgument(out, word);
            return std::nullopt;
        }
    }
    return commonWords;
}

static void runBuildContext(TestCommandContext& context)
{
    std::span<const String> words = context.arguments;
    if (words.empty())
        return printMissingArgument(context.out);
    if (words[0] != "thread-count"_s)
        return printBadArgument(context.out, words[0]);
    if (words.size() > 1)
        return printBadArgument(context.out, words[1]);
    context.out.println("thread count ["_s, context.build.threadCount(), ']');
}

static constexpr ASCIILiteral programOfCrashCommand =
    "module m0 path=\"/crash.ts\" displayPath=\"crash.ts\"\n"
    "function f0 name=\"stopsTheCompiler\" parameters=[{type=number}] results=[{type=number}] {\n"
    "  b0(v0: number):\n"
    "    v1 = Negate v0\n"
    "    Return v1\n"
    "}\n"_s;

static void runCrashCommand(TestCommandContext& context)
{
    bool isWithoutMemory = context.arguments.size() == 1 && context.arguments[0] == "without-memory"_s;
    if (context.arguments.size() == 1 && context.arguments[0] == "without-memory-at-no-work"_s)
        RELEASE_ASSERT_RESOURCE_AVAILABLE(false, MemoryExhaustion, "Crash intentionally because memory is exhausted.");
    if (!context.arguments.empty() && !isWithoutMemory)
        return printBadArgument(context.out, context.arguments[0]);

    std::unique_ptr<Program> program = parseProgram(programOfCrashCommand, context.build);
    RELEASE_ASSERT(program);
    CurrentWorkScope work(*program, FunctionID(0u), "walk"_s);
    RELEASE_ASSERT_RESOURCE_AVAILABLE(!isWithoutMemory, MemoryExhaustion, "Crash intentionally because memory is exhausted.");
    __builtin_trap();
}

std::span<const TestCommand> supportTestCommands()
{
    static constexpr TestCommand commands[] = {
        { "diagnostics"_s, runDiagnosticsTestCommand, ProgramOfCommand::AsWritten, "templates | { no-program | threads=<n> | seed=<n> | <kind>{,f<n>|,b<n>|,v<n>|,L<n>|,limit=<LimitID>|,count=<n>|,at=<module>:<position>} }"_s },
        { "parallel-loops"_s, runParallelLoopsTestCommand, ProgramOfCommand::None, "[threads=<n>]"_s },
        { "build-context"_s, runBuildContext, ProgramOfCommand::None, "thread-count"_s },
        { "hashed-index"_s, runHashedIndexTestCommand, ProgramOfCommand::None, "<hash>{,<hash>}|- <hash> <position>"_s },
        { "crash"_s, runCrashCommand, ProgramOfCommand::None, "[without-memory|without-memory-at-no-work]"_s },
    };
    return commands;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
