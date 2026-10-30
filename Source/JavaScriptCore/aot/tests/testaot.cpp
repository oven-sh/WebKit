/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <wtf/DataLog.h>
#include <wtf/MainThread.h>

#if OS(WINDOWS)
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#endif

#if ENABLE(AOT)

#include "AOTChildProcess.h"
#include "AOTCompileOptionWords.h"
#include "AOTCrashExitCode.h"
#include "AOTDiagnostics.h"
#include "AOTImageFormat.h"
#include "AOTLimits.h"
#include "AOTNativeTest.h"
#include "AOTPass.h"
#include "AOTPassID.h"
#include "AOTProgramFile.h"
#include "AOTProgramTables.h"
#include "AOTRuntimeDescription.h"
#include "AOTSchemaEnums.h"
#include "AOTTestCommand.h"
#include "AOTTestRunner.h"
#include "AOTTestSubcommands.h"
#include "AOTThreads.h"
#include "AOTValueFlowWidenedReport.h"
#include "AOTValueFlowWidenedSolve.h"
#include "AOTVector.h"
#include "AOTWire.h"
#include "InitializeThreading.h"
#include "JSCConfig.h"
#include "Options.h"
#include <algorithm>
#include <wtf/FilePrintStream.h>
#include <wtf/FileSystem.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/StringToIntegerConversion.h>

namespace JSC { namespace AOT {

using namespace Typed;

enum class Subcommand : uint8_t { All, Run, Native, List, Print, Compile, WriteOut, FuzzReader };

struct SubcommandEntry {
    ASCIILiteral name;
    Subcommand subcommand;
    std::span<const ASCIILiteral> ownFlags;
    ASCIILiteral synopsis;
};

static constexpr ASCIILiteral filterFlag = "--filter"_s;
static constexpr ASCIILiteral jobsFlag = "--jobs"_s;

static constexpr ASCIILiteral runFlags[] { filterFlag, jobsFlag, showOutputFlag, singleProcessRunFlag, optionFlag, onlyDiagnosticFlag };
static constexpr ASCIILiteral nativeFlags[] { previousNativeTestFlag, singleProcessFlag, nativeTestPartFlag };
static constexpr ASCIILiteral printFlags[] { "--until"_s, "--function"_s, "--wire"_s, "--command"_s };
static constexpr ASCIILiteral compileFlags[] { "--output"_s };
static constexpr ASCIILiteral fuzzReaderFlags[] { "--seed"_s, "--count"_s };

static constexpr SubcommandEntry subcommandEntries[] {
    { "all"_s, Subcommand::All, { }, "<folder>"_s },
    { runSubcommandName, Subcommand::Run, runFlags, "<file or folder>.. [--filter=<text>] [--jobs=<n>] [--show-output] [--single-process] [--option=<option word>].. [--only-diagnostic=<kind>]"_s },
    { nativeSubcommandName, Subcommand::Native, nativeFlags, "[<filter>] [--after=<name>] [--part=<i>/<n>] [--single-process]"_s },
    { "list"_s, Subcommand::List, { }, "commands|passes|options|limits|statistics|diagnostics|image-format"_s },
    { "print"_s, Subcommand::Print, printFlags, "<program> [--until=<PassID>] [--function=f<n>].. [--wire] [--command=\"<command> { argument }\"] { option word }"_s },
    { "compile"_s, Subcommand::Compile, compileFlags, "<program> --output=<image> { option word }"_s },
    { "write-out"_s, Subcommand::WriteOut, { }, "<file>"_s },
    { "fuzz-reader"_s, Subcommand::FuzzReader, fuzzReaderFlags, "[--seed=<n>] [--count=<n>] [<program>..]"_s },
};

static const SubcommandEntry* findSubcommand(StringView name)
{
    for (const SubcommandEntry& entry : subcommandEntries) {
        if (name == entry.name)
            return &entry;
    }
    return nullptr;
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
    return exitCodeForInvalidCommandLine;
}

static StringView flagNameOf(StringView argument)
{
    size_t equalsSign = argument.find('=');
    return equalsSign == notFound ? argument : argument.left(equalsSign);
}

struct CommandLine {
    Vector<String> arguments;
    Vector<String> jscOptions;
};

static std::optional<CommandLine> takeJSCOptions(std::span<char*> arguments, std::span<const ASCIILiteral> ownFlags)
{
    CommandLine commandLine;
    for (const char* argument : arguments) {
        StringView flagName = flagNameOf(StringView::fromLatin1(argument));
        bool isOwnFlag = std::ranges::any_of(ownFlags, [&](ASCIILiteral flag) {
            return flagName == flag;
        });
        if (flagName.startsWith("--"_s) && !isOwnFlag) {
            if (!JSC::Options::setOption(unsafeSpan(argument).subspan(2).data(), /* verify = */ false)) {
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
    out.print(nativeTests.passed + files.passed, " passed, ", failed, " failed, ", files.todo, " todo");
    if (nativeTests.leftOut)
        out.print(", ", nativeTests.leftOut, " left out: they need entries of Bun, and this program has the table of the stand-in host");
    out.println();
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
    if (!summary.passed && !summary.failed && !summary.leftOut && hasFilter && request.part.count == 1)
        return reportError(exitCodeForFailure, "testaot: no native test matches ", request.filter);
    return exitCode;
}

static void listPasses(PrintStream& out)
{
    for (PassID pass : allMembersOf<PassID>) {
        if (pass != PassID::None)
            out.println(nameOf(pass));
    }
}

static void listLimits(PrintStream& out)
{
    for (LimitID limit : allMembersOf<LimitID>) {
        if (limit != LimitID::None)
            out.println(nameOf(limit), " ", defaultLimitValue(limit, hostArchitecture()), isLoweredByAll(limit) ? "" : " fixed", " ", purposeOf(limit));
    }
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

// For tools that read an executable without these headers. The facts are the ones that the executable's run time states about itself, so a tool can tell whether this list is about that run time.
static void listImageFormat(PrintStream& out)
{
    std::span<const ASCIILiteral> names = namesOfRuntimeFacts();
    auto facts = spanReinterpretCast<const uint32_t>(descriptionOfThisRuntime().subspan(sizeof(RuntimeDescription)));
    for (size_t i = 0; i < names.size(); ++i)
        out.println("fact ", facts[i], " ", names[i]);
#define AOT_PRINT_IMAGE_SECTION(name, Element, statistic) out.println("section " #name);
    FOR_EACH_AOT_IMAGE_SECTION(AOT_PRINT_IMAGE_SECTION)
#undef AOT_PRINT_IMAGE_SECTION
    out.println("offset ProgramTables::firstEnvironmentShape ", offsetof(ProgramTables, firstEnvironmentShape));
    out.println("offset ProgramTables::closureFunctions ", offsetof(ProgramTables, closureFunctions));
    out.println("offset ProgramTables::ranges ", offsetof(ProgramTables, ranges));
}

struct ListEntry {
    ASCIILiteral name;
    void (*print)(PrintStream&);
};

static constexpr ListEntry listEntries[] {
    { "commands"_s, listTestCommands },
    { "passes"_s, listPasses },
    { "options"_s, listOptionWords },
    { "limits"_s, listLimits },
    { "statistics"_s, listStatistics },
    { "diagnostics"_s, listDiagnostics },
    { "image-format"_s, listImageFormat },
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
    // A line ends the same way on every system.
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
#endif

    JSC::Config::configureForTesting();
    // As Bun says where it runs compiled code: on macOS a process without the JIT that says nothing has no SharedArrayBuffer.
    JSC::Options::machExceptionHandlerSandboxPolicy = JSC::Options::SandboxPolicy::Allow;

    WTF::initializeMainThread();
    Typed::reportWhatIsFoundOnlyFromLessBy(Typed::reportWhatIsFoundOnlyFromLessInWords);

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
                JSC::Options::setOption(byteCast<char>(option.utf8().span().subspan(2).data()), /* verify = */ false);
        }
        JSC::Options::notifyOptionsChanged();
    }
    JSC::initialize();

    checkTestCommandNames();
    setProgramPath(String::fromUTF8(commandLine[0]));

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
        return runPrintSubcommand(arguments);
    case Subcommand::Compile:
        return runCompileSubcommand(arguments);
    case Subcommand::WriteOut:
        return runWriteOutSubcommand(arguments);
    case Subcommand::FuzzReader:
        return runFuzzReaderSubcommand(arguments);
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
