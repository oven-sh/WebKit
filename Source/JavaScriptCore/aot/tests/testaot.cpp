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
#include "AOTIRPrinter.h"
#include "AOTImageFormat.h"
#include "AOTMechanismWords.h"
#include "AOTNativeTest.h"
#include "AOTOptionWords.h"
#include "AOTProgramFile.h"
#include "AOTProgramTables.h"
#include "AOTRuntimeDescription.h"
#include "AOTSchemaEnums.h"
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

#if USE(MIMALLOC)
#include <bmalloc/mimalloc.h>
#endif

#if OS(LINUX)
#include <fcntl.h>
#include <sched.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wtf/Threading.h>
#endif

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
    { "list"_s, Subcommand::List, { }, "commands|options|mechanisms|statistics|diagnostics|image-format"_s },
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
    // --mechanisms=<list> as it was written. Null: the command line has none.
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
    out.print(nativeTests.passed + files.passed, " passed, ", failed, " failed, ", nativeTests.todo + files.todo, " todo");
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
    if (!summary.passed && !summary.failed && !summary.todo && !summary.leftOut && hasFilter && request.part.count == 1)
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

// For tools that read an executable without these headers. The facts are the ones that the executable's run time states about itself, so a tool can tell whether this list is about that run time.
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

// As Bun has it while a build is under way (BuildUnderWay in src/threading/ThreadPool.rs): what is freed stays with the process, and the next stage takes it.
#if USE(MIMALLOC) && OS(LINUX) && !ASAN_ENABLED
// What the file holds. Nothing where it cannot be read or does not fit.
static std::optional<StringView> readFileOfSystem(int file, std::span<char> text)
{
    size_t length = 0;
    while (true) {
        std::span<char> rest = text.subspan(length);
        if (rest.empty())
            return std::nullopt;
        ssize_t read = pread(file, rest.data(), rest.size(), length);
        if (read < 0)
            return std::nullopt;
        if (!read)
            return StringView(text.first(length));
        length += read;
    }
}

// How many blocks of 2 MB are free on the node that has the fewest. None where the system cannot be understood.
static size_t numberOfFreeBlocksForHugePages(int blocksBySize)
{
    constexpr size_t orderOfBlock = 9;
    std::array<char, 16 * KB> text;
    std::optional<StringView> lines = readFileOfSystem(blocksBySize, text);
    if (!lines)
        return 0;

    size_t fewest = std::numeric_limits<size_t>::max();
    std::optional<unsigned> node;
    size_t blocksOfNode = 0;
    for (StringView line : lines->split('\n')) {
        // Node 0, zone   Normal  16068 434773 367939 ..
        size_t place = 0;
        for (StringView word : line.split(' ')) {
            if (place == 1) {
                std::optional<unsigned> name = parseInteger<unsigned>(word.left(word.length() - 1));
                if (!name)
                    return 0;
                if (name != node) {
                    if (node)
                        fewest = std::min(fewest, blocksOfNode);
                    node = name;
                    blocksOfNode = 0;
                }
            }
            if (place >= 4 + orderOfBlock) {
                std::optional<size_t> count = parseInteger<size_t>(word);
                if (!count)
                    return 0;
                blocksOfNode += *count << (place - 4 - orderOfBlock);
            }
            ++place;
        }
    }
    return node ? std::min(fewest, blocksOfNode) : 0;
}

// Bun's src/threading/huge_pages.rs in C++, because this program links no Rust. That file says why; whoever changes one changes the other. This process is one build, so nobody ends the watch.
static void takeHugePagesWhileSystemHasThemFree()
{
    constexpr size_t bytesOfLargestArena = 16 * GB;
    constexpr size_t blocksAThreadTakesBetweenLooks = 16 * 4;
    if (static_cast<size_t>(sysconf(_SC_PAGESIZE)) != 4 * KB)
        return;
    int whoGetsHugePages = open("/sys/kernel/mm/transparent_hugepage/enabled", O_RDONLY | O_CLOEXEC);
    if (whoGetsHugePages < 0)
        return;
    std::array<char, 256> text;
    std::optional<StringView> answer = readFileOfSystem(whoGetsHugePages, text);
    close(whoGetsHugePages);
    if (!answer || !answer->contains("[madvise]"_s))
        return;
    cpu_set_t processors;
    CPU_ZERO(&processors);
    if (sched_getaffinity(0, sizeof(processors), &processors))
        return;
    size_t fewestBlocks = CPU_COUNT(&processors) * blocksAThreadTakesBetweenLooks;
    int blocksBySize = open("/proc/buddyinfo", O_RDONLY | O_CLOEXEC);
    if (blocksBySize < 0)
        return;
    if (numberOfFreeBlocksForHugePages(blocksBySize) < 2 * fewestBlocks) {
        close(blocksBySize);
        return;
    }

    mi_arena_id_t arena;
    size_t bytesOfMachine = static_cast<size_t>(sysconf(_SC_PHYS_PAGES)) * (4 * KB);
    size_t size = 0;
    void* start = mi_reserve_os_memory_ex(std::min(bytesOfMachine, bytesOfLargestArena), true, false, false, &arena) ? nullptr : mi_arena_area(arena, &size);
    if (!start) {
        close(blocksBySize);
        return;
    }
    // ArenaOfBuilds::take_huge_pages() in Bun's src/bun_alloc/lib.rs. Meanwhile only whole huge pages go back to the system, which splits one that a piece is taken out of.
    auto takeHugePages = [=](bool takes) {
        madvise(start, size, takes ? MADV_HUGEPAGE : MADV_NOHUGEPAGE);
        mi_option_set_default(mi_option_minimal_purge_size, takes ? 2048 : 0);
    };
    takeHugePages(true);
    Thread::create("AOT huge pages"_s, [=] {
        bool takesHugePages = true;
        while (true) {
            bool hasEnough = numberOfFreeBlocksForHugePages(blocksBySize) >= (takesHugePages ? fewestBlocks : 2 * fewestBlocks);
            if (hasEnough != takesHugePages) {
                takesHugePages = hasEnough;
                takeHugePages(hasEnough);
            }
            WTF::sleep(2_ms);
        }
    })->detach();
}
#endif

static void treatMemoryAsBuildInBunDoes()
{
#if USE(MIMALLOC)
    mi_option_set_default(mi_option_purge_delay, 24 * 60 * 60 * 1000);
#if OS(LINUX) && !ASAN_ENABLED
    takeHugePagesWhileSystemHasThemFree();
#endif
#endif
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
    BuiltinBindings::bindNativeHelpersOfEngine();
    installTrapReporter();

    checkTestCommandNames();
    setProgramPath(String::fromUTF8(commandLine[0]));

    if (const String& argument = parsedCommandLine->mechanismsArgument; !argument.isNull()) {
        // Behind the two dashes stands the word.
        Mechanisms mechanisms = allMechanisms;
        if (std::optional<String> sentence = applyMechanismsWord(mechanisms, StringView(argument).substring(2)))
            return reportError(exitCodeForInvalidCommandLine, "testaot: ", argument, ": ", *sentence);
        setMechanismsOfProcess(mechanisms);
        // What a launcher hands to every child.
        parsedCommandLine->jscOptions.append(argument);
    }

    // Only on request: the runner reads a child's output from its first line, stderr included, and the tests of the verifier hold bodies that are refused.
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
        // Without --wire it is the command of that name.
        [[fallthrough]];
    case Subcommand::Command:
        enterTemporaryFolder();
        return runCommandSubcommand(StringView::fromLatin1(commandLine[1]), arguments);
    case Subcommand::Compile:
        treatMemoryAsBuildInBunDoes();
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
