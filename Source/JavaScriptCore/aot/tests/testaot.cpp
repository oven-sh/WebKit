/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <wtf/DataLog.h>
#include <wtf/MainThread.h>
#include <wtf/StringPrintStream.h>

#if OS(WINDOWS)
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#endif

#if ENABLE(AOT)

#include "AOTBodyVerifier.h"
#include "AOTBuiltinTableBindings.h"
#include "AOTChildProcess.h"
#include "AOTCrashExitCode.h"
#include "AOTDiagnostics.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsNet.h"
#include "AOTIRPrinter.h"
#include "AOTImageFormat.h"
#include "AOTMechanismWords.h"
#include "AOTNativeTest.h"
#include "AOTOptionWords.h"
#include "AOTProgramFile.h"
#include "AOTProgramTables.h"
#include "AOTRuntimeDescription.h"
#include "AOTRuntimeReporter.h"
#include "AOTRuntimeTable.h"
#include "AOTSchemaEnums.h"
#include "AOTStubDescriptor.h"
#include "AOTStubRecording.h"
#include "AOTTestHarness.h"
#include "AOTTestRunner.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include "AOTWire.h"
#include "InitializeThreading.h"
#include "JSCConfig.h"
#include "Options.h"
#include <algorithm>
#include <stdlib.h>
#include <wtf/FilePrintStream.h>
#include <wtf/FileSystem.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/StringToIntegerConversion.h>

namespace JSC { namespace AOT {

using namespace Typed;

enum class Subcommand : uint8_t { All, Run, Native, List, Command, Print, Compile, WriteOut, FuzzAllocation };

struct SubcommandEntry {
    ASCIILiteral name;
    Subcommand subcommand;
    std::span<const ASCIILiteral> ownFlags;
    ASCIILiteral synopsis;
};

static constexpr ASCIILiteral filterFlag = "--filter"_s;
static constexpr ASCIILiteral jobsFlag = "--jobs"_s;
static constexpr ASCIILiteral programOfTagFlag = "--program-of-tag"_s;
static constexpr ASCIILiteral wireFlag = "--wire"_s;
static constexpr ASCIILiteral showRefusedBodiesFlag = "--show-refused-bodies"_s;

static constexpr ASCIILiteral runFlags[] { filterFlag, jobsFlag, showOutputFlag, singleProcessRunFlag, optionFlag, onlyDiagnosticFlag };
static constexpr ASCIILiteral nativeFlags[] { previousNativeTestFlag, singleProcessFlag, nativeTestPartFlag };
static constexpr ASCIILiteral commandFlags[] { programOfTagFlag };
static constexpr ASCIILiteral printFlags[] { wireFlag, programOfTagFlag };
static constexpr ASCIILiteral compileFlags[] { "--output"_s, programOfTagFlag };
static constexpr ASCIILiteral fuzzAllocationFlags[] { "--seed"_s, "--count"_s, "--blocks"_s, "--show"_s };

static constexpr SubcommandEntry subcommandEntries[] {
    { "all"_s, Subcommand::All, { }, "<folder>"_s },
    { runSubcommandName, Subcommand::Run, runFlags, "<file or folder>.. [--filter=<text>] [--jobs=<n>] [--show-output] [--single-process] [--option=<option word>].. [--only-diagnostic=<kind>]"_s },
    { nativeSubcommandName, Subcommand::Native, nativeFlags, "[<filter>] [--after=<name>] [--part=<i>/<n>] [--single-process]"_s },
    { "list"_s, Subcommand::List, { }, "commands|options|mechanisms|statistics|diagnostics|image-format|rows-of-effects"_s },
    { "<command>"_s, Subcommand::Command, commandFlags, "<program> { argument } { option word } [mechanisms=<list>] [--program-of-tag]"_s },
    { "print"_s, Subcommand::Print, printFlags, "<program> --wire"_s },
    { "compile"_s, Subcommand::Compile, compileFlags, "<program> --output=<image> { option word | statistic } [mechanisms=<list>] [--program-of-tag]"_s },
    { "write-out"_s, Subcommand::WriteOut, { }, "<file>"_s },
    { "fuzz-allocation"_s, Subcommand::FuzzAllocation, fuzzAllocationFlags, "[--seed=<n>] [--count=<n>] [--blocks=<n>] [--show=<seed>]"_s },
};

static const SubcommandEntry* findSubcommand(StringView name)
{
    const SubcommandEntry* commandEntry = nullptr;
    for (const SubcommandEntry& entry : subcommandEntries) {
        if (entry.subcommand == Subcommand::Command) {
            commandEntry = &entry;
            continue;
        }
        if (name == entry.name)
            return &entry;
    }
    return findTestCommand(name) ? commandEntry : nullptr;
}

template<typename... Types>
static int reportError(int exitCode, const Types&... sentence)
{
    FilePrintStream errors(stderr, FilePrintStream::Borrow);
    errors.println(sentence...);
    return exitCode;
}

static int reportUsage(std::optional<Subcommand> subcommand = std::nullopt)
{
    FilePrintStream errors(stderr, FilePrintStream::Borrow);
    for (const SubcommandEntry& entry : subcommandEntries) {
        if (!subcommand || entry.subcommand == *subcommand)
            errors.println("usage: testaot ", entry.name, " ", entry.synopsis);
    }
    errors.println("each takes ", mechanismsFlag, "=<list>: `testaot list mechanisms` prints the grammar");
    errors.println("each takes ", showRefusedBodiesFlag, ": each body that the verifier of bodies refuses is printed to stderr");
    return exitCodeForInvalidCommandLine;
}

static StringView flagNameOf(StringView argument)
{
    size_t equalsSign = argument.find('=');
    return equalsSign == notFound ? argument : argument.left(equalsSign);
}

static bool containsFlag(std::span<const String> arguments, ASCIILiteral flag)
{
    return std::ranges::any_of(arguments, [&](const String& argument) {
        return flagNameOf(argument) == flag;
    });
}

struct CommandLine {
    Vector<String> arguments;
    Vector<String> jscOptions;
    String mechanismsArgument;
    bool showsRefusedBodies { false };
};

static std::optional<CommandLine> takeJSCOptions(std::span<char*> arguments, std::span<const ASCIILiteral> ownFlags)
{
    CommandLine commandLine;
    for (const char* argument : arguments) {
        StringView flagName = flagNameOf(StringView::fromLatin1(argument));
        if (flagName == mechanismsFlag) {
            commandLine.mechanismsArgument = String::fromLatin1(argument);
            continue;
        }
        if (flagName == showRefusedBodiesFlag) {
            commandLine.showsRefusedBodies = true;
            continue;
        }
        bool isOwnFlag = std::ranges::any_of(ownFlags, [&](ASCIILiteral flag) {
            return flagName == flag;
        });
        if (flagName.startsWith("--"_s) && !isOwnFlag) {
            if (!JSC::Options::setOption(unsafeSpan(argument).subspan(2).data(), false)) {
                reportError(exitCodeForInvalidCommandLine, "testaot: unknown option ", argument);
                return std::nullopt;
            }
            commandLine.jscOptions.append(String::fromLatin1(argument));
            continue;
        }

        String word = String::fromUTF8(argument);
        if (word.isNull()) {
            reportError(exitCodeForInvalidCommandLine, "testaot: ", argument, " is not UTF-8");
            return std::nullopt;
        }
        commandLine.arguments.append(WTF::move(word));
    }
    return commandLine;
}

static int reportTestResults(PrintStream& out, const NativeTestSummary& nativeTests, const TestRunSummary& files)
{
    unsigned failed = nativeTests.failed + files.failed;
    out.println(nativeTests.passed + files.passed, " passed, ", failed, " failed, ", nativeTests.todo + files.todo, " todo");
    return failed ? exitCodeForFailure : exitCodeForSuccess;
}

static int runAllSubcommand(std::span<const String> arguments, const ChildProcessLauncher& launcher)
{
    if (arguments.size() != 1)
        return reportUsage(Subcommand::All);

    FilePrintStream out(stdout, FilePrintStream::Borrow);
    NativeTestSummary nativeTests = runNativeTests({ { }, { }, &launcher }, out);
    TestRunRequest request;
    request.paths.append(arguments[0]);
    request.launcher = &launcher;
    TestRunSummary files = runTestFiles(request, out);
    return reportTestResults(out, nativeTests, files);
}

static int runRunSubcommand(std::span<const String> arguments, const ChildProcessLauncher& launcher)
{
    TestRunRequest request;
    request.launcher = &launcher;
    for (const String& argument : arguments) {
        if (!argument.startsWith("--"_s)) {
            request.paths.append(argument);
            continue;
        }

        if (argument == showOutputFlag) {
            request.showsOutput = true;
            continue;
        }
        if (argument == singleProcessRunFlag) {
            request.launcher = nullptr;
            continue;
        }

        StringView flagName = flagNameOf(argument);
        StringView value = StringView(argument).substring(flagName.length() + 1);
        if (flagName == showOutputFlag || flagName == singleProcessRunFlag)
            return reportError(exitCodeForInvalidCommandLine, "testaot run: ", flagName, " takes no value");
        if (value.isEmpty())
            return reportError(exitCodeForInvalidCommandLine, "testaot run: ", flagName, " needs a value");
        if (flagName == filterFlag) {
            request.filter = value.toString();
            continue;
        }
        if (flagName == optionFlag) {
            request.variant.optionWords.append(value.toString());
            continue;
        }
        if (flagName == onlyDiagnosticFlag) {
            request.variant.onlyDiagnostic = parseDiagnosticName(value);
            if (!request.variant.onlyDiagnostic)
                return reportError(exitCodeForInvalidCommandLine, "testaot run: ", value, " is no diagnostic");
            continue;
        }

        ASSERT(flagName == jobsFlag);
        request.jobs = parseInteger<unsigned>(value).value_or(0);
        if (!request.jobs)
            return reportError(exitCodeForInvalidCommandLine, "testaot run: ", jobsFlag, " takes a decimal number above 0");
    }
    if (request.paths.isEmpty())
        return reportUsage(Subcommand::Run);

    FilePrintStream out(stdout, FilePrintStream::Borrow);
    TestRunSummary summary = runTestFiles(request, out);
    int exitCode = reportTestResults(out, { }, summary);
    if (!summary.passed && !summary.failed && !summary.todo)
        return reportError(exitCodeForFailure, "testaot: no test matches");
    return exitCode;
}

static std::optional<NativeTestPart> parseNativeTestPart(StringView text)
{
    size_t slash = text.find('/');
    if (slash == notFound)
        return std::nullopt;
    std::optional<unsigned> number = parseInteger<unsigned>(text.left(slash));
    std::optional<unsigned> count = parseInteger<unsigned>(text.substring(slash + 1));
    if (!number || !count || !*number || *number > *count)
        return std::nullopt;
    return NativeTestPart { *number - 1, *count };
}

static int runNativeSubcommand(std::span<const String> arguments, const ChildProcessLauncher& launcher)
{
    NativeTestRequest request;
    request.launcher = &launcher;
    bool hasFilter = false;
    for (const String& argument : arguments) {
        if (argument == singleProcessFlag) {
            request.launcher = nullptr;
            continue;
        }
        if (flagNameOf(argument) == previousNativeTestFlag) {
            request.previousTest = StringView(argument).substring(previousNativeTestFlag.length() + 1);
            if (request.previousTest.isEmpty())
                return reportError(exitCodeForInvalidCommandLine, "testaot native: ", previousNativeTestFlag, " needs a value");
            continue;
        }
        if (flagNameOf(argument) == nativeTestPartFlag) {
            std::optional<NativeTestPart> part = parseNativeTestPart(StringView(argument).substring(nativeTestPartFlag.length() + 1));
            if (!part)
                return reportError(exitCodeForInvalidCommandLine, "testaot native: ", nativeTestPartFlag, " needs <i>/<n> with 1 <= i <= n");
            request.part = *part;
            continue;
        }
        if (argument.startsWith("--"_s) || hasFilter)
            return reportUsage(Subcommand::Native);
        request.filter = argument;
        hasFilter = true;
    }

    FilePrintStream out(stdout, FilePrintStream::Borrow);
    NativeTestSummary summary = runNativeTests(request, out);
    int exitCode = reportTestResults(out, summary, { });
    if (!summary.passed && !summary.failed && !summary.todo && hasFilter && request.part.count == 1)
        return reportError(exitCodeForFailure, "testaot: no native test matches ", request.filter);
    return exitCode;
}

static void listStatistics(PrintStream& out)
{
    for (StatisticID statistic : allMembersOf<StatisticID>) {
        if (statistic != StatisticID::None)
            out.println(nameOf(statistic));
    }
}

static void listDiagnostics(PrintStream& out)
{
    for (UserDiagnosticKind kind : allMembersOf<UserDiagnosticKind>) {
        if (kind != UserDiagnosticKind { })
            out.println(nameOf(kind));
    }
    for (InternalErrorKind kind : allMembersOf<InternalErrorKind>) {
        if (kind != InternalErrorKind { })
            out.println(nameOf(kind));
    }
}

static void listImageFormat(PrintStream& out)
{
    std::span<const ASCIILiteral> names = namesOfRuntimeFacts();
    auto facts = spanReinterpretCast<const uint32_t>(descriptionOfThisRuntime().subspan(sizeof(RuntimeDescription)));
    for (size_t i = 0; i < names.size(); ++i)
        out.println("fact ", facts[i], " ", names[i]);
#define AOT_PRINT_IMAGE_SECTION(name, Element) out.println("section " #name);
    FOR_EACH_AOT_IMAGE_SECTION(AOT_PRINT_IMAGE_SECTION)
#undef AOT_PRINT_IMAGE_SECTION
    out.println("offset ProgramTables::firstEnvironmentShape ", offsetof(ProgramTables, firstEnvironmentShape));
    out.println("offset ProgramTables::closureFunctions ", offsetof(ProgramTables, closureFunctions));
    out.println("offset ProgramTables::ranges ", offsetof(ProgramTables, ranges));
}

static void listRowsOfEffects(PrintStream& out)
{
    auto print = [&](ASCIILiteral part, unsigned row) {
        out.print("effects:", part, ":");
        printRowOfEffectsNet(out, row);
        out.println();
    };
    auto printParts = [&](unsigned row, bool runsNoOtherCode, bool throwsNothing, bool leavesHeapsAlone, bool doesNotCollect) {
        if (runsNoOtherCode)
            print("runs-no-other-code"_s, row);
        if (throwsNothing)
            print("throws-nothing"_s, row);
        if (leavesHeapsAlone)
            print("leaves-heaps-alone"_s, row);
        if (doesNotCollect)
            print("does-not-collect"_s, row);
    };
    auto printEffects = [&](unsigned row, const Typed::Effects& effects) {
        printParts(row, !effects.mayRunOtherCode(), !effects.mayThrowByRule(), !!Typed::lanesNotWrittenInEffectsNet(effects), !effects.mayCollect);
    };
    for (Typed::Opcode opcode : allMembersOf<Typed::Opcode>)
        printEffects(Typed::firstOperationRowInEffectsNet + static_cast<unsigned>(opcode), Typed::declaredBoundOf(opcode));
    for (unsigned entry = 0; entry < numberOfEntries; ++entry) {
        if (runtimeOperationOf(static_cast<AOT::Entry>(entry)))
            printEffects(Typed::firstRuntimeOperationRowInEffectsNet + entry, effectsOf(static_cast<AOT::Entry>(entry)));
    }
    for (unsigned family = 0; family < numberOfStubFamilies; ++family)
        print("any"_s, Typed::firstStubRowInEffectsNet + family);
    const BuiltinTable& table = BuiltinTable::singleton();
    for (unsigned builtin = 1; builtin < table.builtinCount(); ++builtin) {
        bool runsNoOtherCode = false;
        bool throwsNothing = false;
        bool leavesHeapsAlone = false;
        bool doesNotCollect = false;
        for (const BuiltinOverload& overload : table.overloadsOf(BuiltinID(builtin))) {
            if (overload.isUndeclared())
                continue;
            bool runsOperandCodeAtMost = overload.effects.capability != BuiltinCapability::JS || overload.flags.contains(BuiltinOverloadFlag::RunsOnlyOperandCode);
            runsNoOtherCode |= runsOperandCodeAtMost;
            leavesHeapsAlone |= runsOperandCodeAtMost;
            throwsNothing |= !overload.throws();
            doesNotCollect |= overload.effects.capability == BuiltinCapability::None;
        }
        printParts(Typed::firstBuiltinRowInEffectsNet + builtin, runsNoOtherCode, throwsNothing, leavesHeapsAlone, doesNotCollect);
    }
}

struct ListEntry {
    ASCIILiteral name;
    void (*print)(PrintStream&);
};

static constexpr ListEntry listEntries[] {
    { "commands"_s, listTestCommands },
    { "options"_s, listOptionWords },
    { "mechanisms"_s, listMechanismsWord },
    { "statistics"_s, listStatistics },
    { "diagnostics"_s, listDiagnostics },
    { "image-format"_s, listImageFormat },
    { "rows-of-effects"_s, listRowsOfEffects },
};

static int runListSubcommand(std::span<const String> arguments)
{
    if (arguments.size() != 1)
        return reportUsage(Subcommand::List);

    for (const ListEntry& entry : listEntries) {
        if (arguments[0] != entry.name)
            continue;
        FilePrintStream out(stdout, FilePrintStream::Borrow);
        entry.print(out);
        return exitCodeForSuccess;
    }
    return reportUsage(Subcommand::List);
}

static int runWriteOutSubcommand(std::span<const String> arguments)
{
    if (arguments.size() != 1)
        return reportUsage(Subcommand::WriteOut);

    FilePrintStream errors(stderr, FilePrintStream::Borrow);
    std::optional<ProgramFile> file = readProgramFile(arguments[0], errors);
    if (!file || file->kind != ProgramFile::Kind::Text)
        return exitCodeForFailure;
    FilePrintStream out(stdout, FilePrintStream::Borrow);
    out.print(file->text);
    return exitCodeForSuccess;
}

} } // namespace JSC::AOT

int main(int argc, char** argv)
{
    using namespace JSC::AOT;

#if OS(WINDOWS)
    terminateWithExceptionCodeOnCrash();
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
#endif

    JSC::Config::configureForTesting();
    JSC::Options::machExceptionHandlerSandboxPolicy = JSC::Options::SandboxPolicy::Allow;

    WTF::initializeMainThread();

    std::span<char*> commandLine = unsafeMakeSpan(argv, static_cast<size_t>(argc));
    const SubcommandEntry* entry = commandLine.size() > 1 ? findSubcommand(StringView::fromLatin1(commandLine[1])) : nullptr;
    if (!entry)
        return reportUsage();

    std::optional<CommandLine> parsedCommandLine;
    {
        JSC::Options::AllowUnfinalizedAccessScope scope;
        JSC::Options::initialize([] {
            JSC::Options::useJITCage() = false;
            JSC::Options::validateGraph() = true;
        });
        parsedCommandLine = takeJSCOptions(commandLine.subspan(2), entry->ownFlags);
        if (!parsedCommandLine)
            return exitCodeForInvalidCommandLine;
        if (JSC::Options::aotImagePath()) {
            JSC::Options::useSharedArrayBuffer() = true;
            JSC::Options::useTemporal() = true;
            JSC::Options::useExplicitResourceManagement() = true;
            for (const String& option : parsedCommandLine->jscOptions)
                JSC::Options::setOption(byteCast<char>(option.utf8().span().subspan(2).data()), false);
        }
        JSC::Options::notifyOptionsChanged();
    }
    JSC::initialize();
    BuiltinBindings::bindNativeHelpersOfEngine();
    installTrapReporter();

    checkTestCommandNames();
    setProgramPath(String::fromUTF8(commandLine[0]));

    if (const String& argument = parsedCommandLine->mechanismsArgument; !argument.isNull()) {
        Mechanisms mechanisms = allMechanisms;
        if (std::optional<String> sentence = applyMechanismsWord(mechanisms, StringView(argument).substring(2)))
            return reportError(exitCodeForInvalidCommandLine, "testaot: ", argument, ": ", *sentence);
        setMechanismsOfProcess(mechanisms);
        parsedCommandLine->jscOptions.append(argument);
    }

    if (parsedCommandLine->showsRefusedBodies) {
        Typed::printerOfBodiesForVerifier = [](const Typed::Program& program, Typed::FunctionID id, ASCIILiteral heading) {
            StringPrintStream out;
            Typed::printFunction(out, program, id);
            dataLogLn("=== ", heading, "\n", out.toString());
        };
        parsedCommandLine->jscOptions.append(String(showRefusedBodiesFlag));
    }

    std::span<const String> arguments = parsedCommandLine->arguments.span();
    auto createLauncher = [&] {
        return ChildProcessLauncher(String(programPath()), WTF::move(parsedCommandLine->jscOptions));
    };

    switch (entry->subcommand) {
    case Subcommand::All:
        enterTemporaryFolder();
        return runAllSubcommand(arguments, createLauncher());
    case Subcommand::Run:
        enterTemporaryFolder();
        return runRunSubcommand(arguments, createLauncher());
    case Subcommand::Native:
        enterTemporaryFolder();
        return runNativeSubcommand(arguments, createLauncher());
    case Subcommand::List:
        return runListSubcommand(arguments);
    case Subcommand::Print:
        if (containsFlag(arguments, wireFlag))
            return runPrintSubcommand(arguments);
        [[fallthrough]];
    case Subcommand::Command:
        enterTemporaryFolder();
        return runCommandSubcommand(StringView::fromLatin1(commandLine[1]), arguments);
    case Subcommand::Compile:
        return runCompileSubcommand(arguments);
    case Subcommand::WriteOut:
        return runWriteOutSubcommand(arguments);
    case Subcommand::FuzzAllocation:
        return runFuzzAllocationSubcommand(arguments);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

#else // ENABLE(AOT)

int main(int, char**)
{
    WTF::initializeMainThread();
    dataLogLn("AOT is not enabled.");
    return 0;
}

#endif // ENABLE(AOT)

#if OS(WINDOWS)
extern "C" __declspec(dllexport) int WINAPI dllLauncherEntryPoint(int argc, const char* argv[])
{
    return main(argc, const_cast<char**>(argv));
}
#endif
