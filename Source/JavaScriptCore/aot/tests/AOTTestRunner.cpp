/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTestRunner.h"

#if ENABLE(AOT)

#include "AOTBodyVerifier.h"
#include "AOTCheckLines.h"
#include "AOTChildProcess.h"
#include "AOTCompileOptionWords.h"
#include "AOTIRParser.h"
#include "AOTMechanismWords.h"
#include "AOTOptionWords.h"
#include "AOTProgramFile.h"
#include "AOTProgramVerifier.h"
#include "AOTSchemaEnums.h"
#include "AOTTestFile.h"
#include "AOTTestHarness.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include "JSCConfig.h"
#include "Options.h"
#include <algorithm>
#include <wtf/FileSystem.h>
#include <wtf/NumberOfCores.h>
#include <wtf/Scope.h>
#include <wtf/StringPrintStream.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/text/StringToIntegerConversion.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr size_t maxCrashOutputLinesShown = 10;

static void collectDirectoryTestFiles(const String& directory, Vector<String>& enclosingDirectories, Vector<String>& files)
{
    String realPath = FileSystem::realPath(directory);
    if (enclosingDirectories.contains(realPath))
        return;
    enclosingDirectories.append(WTF::move(realPath));

    Vector<String> names = FileSystem::listDirectory(directory);
    std::ranges::sort(names, codePointCompareLessThan);
    for (const String& name : names) {
        String path = FileSystem::pathByAppendingComponent(directory, name);
        std::optional<FileSystem::FileType> type = FileSystem::fileTypeFollowingSymlinks(path);
        if (type == FileSystem::FileType::Directory)
            collectDirectoryTestFiles(path, enclosingDirectories, files);
        else if (type == FileSystem::FileType::Regular && name.endsWith(".ir"_s))
            files.append(WTF::move(path));
    }

    enclosingDirectories.removeLast();
}

static Vector<String> collectTestFiles(const TestRunRequest& request)
{
    Vector<String> files;
    Vector<String> enclosingDirectories;
    for (const String& path : request.paths) {
        if (FileSystem::fileTypeFollowingSymlinks(path) == FileSystem::FileType::Directory)
            collectDirectoryTestFiles(path, enclosingDirectories, files);
        else
            files.append(path.isolatedCopy());
    }

    if (!request.filter.isEmpty()) {
        files.removeAllMatching([&](const String& path) {
            return !path.contains(request.filter);
        });
    }
    return files;
}

template<typename... Types>
static void appendFailure(Vector<TestFailure>& failures, const RunDirective& run, unsigned line, const Types&... sentence)
{
    failures.append(TestFailure { line, makeString(run.command, ": "_s, sentence...) });
}

static size_t countOf(const Diagnostics& diagnostics, const DiagnosticName& name, std::optional<FunctionID> function)
{
    auto isCounted = [&](const auto& reported, auto kind) {
        return reported.kind == kind && (!function || reported.function == *function);
    };
    return WTF::switchOn(name, [&](UserDiagnosticKind kind) -> size_t {
        return std::ranges::count_if(diagnostics.userDiagnostics(), [&](const UserDiagnostic& reported) { return isCounted(reported, kind); });
    }, [&](InternalErrorKind kind) -> size_t {
        return std::ranges::count_if(diagnostics.internalErrors(), [&](const InternalError& reported) { return isCounted(reported, kind); });
    });
}

struct UnexpectedDiagnostics {
    const TestFile& file;
    const RunDirective& run;
    Vector<TestFailure>& failures;
    Vector<DiagnosticName> seen { };

    void visit(DiagnosticName name, const String& message)
    {
        bool isExpected = file.diagnostics.containsIf([&](const ExpectedDiagnostic& expected) {
            return expected.kind == name;
        });
        if (isExpected || seen.contains(name))
            return;
        seen.append(name);
        appendFailure(failures, run, run.line, "unexpected diagnostic "_s, nameOf(name), ": "_s, message);
        failures.last().isInternalError = std::holds_alternative<InternalErrorKind>(name);
    }
};

static void compareDiagnostics(const TestFile& file, const RunDirective& run, const Diagnostics& diagnostics, Vector<TestFailure>& failures)
{
    for (const ExpectedDiagnostic& expected : file.diagnostics) {
        size_t count = countOf(diagnostics, expected.kind, expected.function);
        String place = expected.function ? makeString(" of f"_s, expected.function->index()) : emptyString();
        if (!count)
            appendFailure(failures, run, expected.line, "expected diagnostic "_s, nameOf(expected.kind), place, ", none was reported"_s);
        else if (expected.count && *expected.count != count)
            appendFailure(failures, run, expected.line, "expected "_s, *expected.count, " of "_s, nameOf(expected.kind), place, ", got "_s, count);
    }

    UnexpectedDiagnostics unexpected { file, run, failures };
    for (size_t i = 0; i < diagnostics.userDiagnostics().size(); ++i)
        unexpected.visit(diagnostics.userDiagnostics()[i].kind, diagnostics.messageOfUserDiagnostic(i));
    for (size_t i = 0; i < diagnostics.internalErrors().size(); ++i)
        unexpected.visit(diagnostics.internalErrors()[i].kind, diagnostics.messageOfInternalError(i));
}

static void reportDiagnostic(const DiagnosticName& name, const RunDirective& run, const Diagnostics& diagnostics, Vector<TestFailure>& failures)
{
    TestFile fileWithoutExpectations;
    UnexpectedDiagnostics unexpected { fileWithoutExpectations, run, failures };
    for (size_t i = 0; i < diagnostics.userDiagnostics().size(); ++i) {
        if (name == DiagnosticName(diagnostics.userDiagnostics()[i].kind))
            unexpected.visit(name, diagnostics.messageOfUserDiagnostic(i));
    }
    for (size_t i = 0; i < diagnostics.internalErrors().size(); ++i) {
        if (name == DiagnosticName(diagnostics.internalErrors()[i].kind))
            unexpected.visit(name, diagnostics.messageOfInternalError(i));
    }
}

static void compareStatistics(const TestFile& file, const RunDirective& run, const Statistics& statistics, Vector<TestFailure>& failures)
{
    Vector<uint64_t> values = statistics.values();
    for (const ExpectedStatistic& expected : file.statistics) {
        uint64_t actual = values[static_cast<size_t>(expected.statistic)];
        if (!holds(expected.comparison, actual, expected.value))
            appendFailure(failures, run, expected.line, nameOf(expected.statistic), " is "_s, actual);
    }
}

static bool takesUnverifiedProgram(const TestCommand& command)
{
    return command.name == "verify"_s || command.name == "print"_s;
}

static String programTextFor(const TestCommand& command, const TestFile& file)
{
    switch (command.program) {
    case ProgramOfCommand::None:
        return { };
    case ProgramOfCommand::AsWritten:
        return file.text;
    case ProgramOfCommand::WithLibrary:
        return withStandInLibrary(file.text);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

struct WordsOfRun {
    Vector<String> arguments;
    Mechanisms mechanisms;
};

static String runCommand(const TestCommand& command, const TestFile& file, const WordsOfRun& words, BuildContext& build, const Wire::CompileOptions& options, Vector<String>* notesForFailure = nullptr)
{
    String programText = programTextFor(command, file);
    std::unique_ptr<Program> program;
    if (command.program != ProgramOfCommand::None) {
        program = parseProgram(programText, build);
        bool isReadyForCommand = program && (takesUnverifiedProgram(command) || (verifyRecords(*program, build) && verifyBodies(*program, build)));
        if (!isReadyForCommand) {
            build.diagnostics().finish(program.get());
            return emptyString();
        }
    }

    StringPrintStream out;
    TestCommandContext context { WTF::move(program), words.arguments.span(), out, build };
    context.options = &options;
    context.sections = file.sections.span();
    context.functionOfTypesLine = file.functionOfTypesLine;
    context.programText = programText;
    context.mechanisms = words.mechanisms;
    command.run(context);
    build.diagnostics().finish(context.program.get());
    if (notesForFailure)
        *notesForFailure = WTF::move(context.notesForFailure);
    return out.toStringWithLatin1Fallback();
}

static bool runOneDirective(const TestFile& file, const RunDirective& run, const Wire::CompileOptions& optionsOfFile, std::optional<Mechanisms> mechanismsAskedFor, const TestVariant& variant, Vector<TestFailure>& failures, Vector<RunOutput>* outputs)
{
    const TestCommand* command = findTestCommand(run.command);
    if (!command) {
        failures.append(TestFailure { run.line, makeString("unknown command "_s, run.command) });
        return true;
    }

    WordsOfRun words { run.arguments, mechanismsOfProcess() };
    if (std::optional<String> sentence = takeMechanismsWords(words.arguments, mechanismsAskedFor)) {
        failures.append(TestFailure { run.line, WTF::move(*sentence) });
        return true;
    }
    if (mechanismsAskedFor) {
        if (!mechanismsOfProcess().containsAll(*mechanismsAskedFor))
            return false;
        words.mechanisms = *mechanismsAskedFor;
    }
    Wire::CompileOptions options = optionsOfFile;
    std::optional<String> sentenceOfTestingWord;
    words.arguments.removeAllMatching([&](const String& word) {
        if (!word.startsWith("testing="_s))
            return false;
        std::optional<String> sentence = applyOptionWord(options, word);
        if (sentence && !sentenceOfTestingWord)
            sentenceOfTestingWord = WTF::move(sentence);
        return true;
    });
    if (sentenceOfTestingWord) {
        failures.append(TestFailure { run.line, WTF::move(*sentenceOfTestingWord) });
        return true;
    }

    runOnBuildThread([&] {
        ScopeOfTestThatRunsAsUsersBuild scopeOfChecks(!!file.asUsersBuild);
        BuildContext build(options);
        Vector<String> notesForFailure;
        String output = runCommand(*command, file, words, build, options, &notesForFailure);
        if (variant.onlyDiagnostic) {
            reportDiagnostic(*variant.onlyDiagnostic, run, build.diagnostics(), failures);
            if (outputs)
                outputs->append(RunOutput { run.line, WTF::move(output) });
            return;
        }

        size_t numberOfEarlierFailures = failures.size();
        compareDiagnostics(file, run, build.diagnostics(), failures);
        compareStatistics(file, run, build.statistics(), failures);
        if (std::optional<CheckFailure> failure = matchCheckLines(file.checkLinesFor(run.prefix), output))
            appendFailure(failures, run, failure->line, failure->message);
        if (failures.size() > numberOfEarlierFailures) {
            for (const String& note : notesForFailure)
                appendFailure(failures, run, run.line, note);
        }
        if (outputs)
            outputs->append(RunOutput { run.line, WTF::move(output) });
    });
    return true;
}

static void reportBreachesOfClosure(const TestFile& file, const Wire::CompileOptions& options, Vector<TestFailure>& failures)
{
    const TestCommand* command = findTestCommand("closure"_s);
    RELEASE_ASSERT(command);
    runOnBuildThread([&] {
        BuildContext build(options);
        String output = runCommand(*command, file, WordsOfRun { { }, mechanismsOfProcess() }, build, options);
        if (output.isEmpty())
            return;
        unsigned numberOfLines = 0;
        for (size_t end = output.find('\n'); end != notFound; end = output.find('\n', end + 1))
            ++numberOfLines;
        failures.append(TestFailure { 1, makeString("Link's flow does not end closed, `closure` prints "_s, numberOfLines, " lines: "_s, StringView(output).left(output.find('\n'))) });
    });
}

static void reportTablesThatHangOnOrder(const TestFile& file, const Wire::CompileOptions& options, Vector<TestFailure>& failures)
{
    const TestCommand* command = findTestCommand("link"_s);
    RELEASE_ASSERT(command);
    auto tablesIn = [&](ASCIILiteral reading, ASCIILiteral order) {
        String output;
        runOnBuildThread([&] {
            BuildContext build(options);
            WordsOfRun words { { }, mechanismsOfProcess() };
            if (!reading.isNull())
                words.arguments.append(reading);
            if (!order.isNull())
                words.arguments.append(order);
            output = runCommand(*command, file, words, build, options);
        });
        return output;
    };
    for (ASCIILiteral reading : { ASCIILiteral(), "held"_s }) {
        ASCIILiteral nameOfReading = reading.isNull() ? ""_s : " held"_s;
        String tables = tablesIn(reading, { });
        for (ASCIILiteral order : { "oldest-first"_s, "scattered"_s }) {
            String others = tablesIn(reading, order);
            if (others == tables)
                continue;
            if (file.hangsOnOrder)
                return;
            Vector<String> lines = tables.split('\n');
            Vector<String> otherLines = others.split('\n');
            size_t line = 0;
            while (line < lines.size() && line < otherLines.size() && lines[line] == otherLines[line])
                ++line;
            failures.append(TestFailure { 1, makeString("Link's tables hang on the order of the worklist: `link"_s, nameOfReading, "` prints `"_s, line < lines.size() ? lines[line] : emptyString(), "`, `link"_s, nameOfReading, " "_s, order, "` prints `"_s, line < otherLines.size() ? otherLines[line] : emptyString(), '`') });
            return;
        }
    }
    if (file.hangsOnOrder && mechanismsOfProcess() == allMechanisms)
        failures.append(TestFailure { file.hangsOnOrder->line, "Link's tables are the same in every order of the worklist: remove the HANGS-ON-ORDER line"_s });
}

static ASCIILiteral findWhatTurnsChecksOnFromOutside(const TestVariant& variant)
{
    if (ASSERT_ENABLED)
        return "this testaot has assertions"_s;
    if (ASAN_ENABLED)
        return "this testaot has the sanitizer"_s;
    if (Options::verifyAOTMachineCode())
        return "verifyAOTMachineCode is set for the process"_s;
    if (variant.optionWords.containsIf([](const String& word) { return word.startsWith("check="_s); }))
        return "the command line asks for a check"_s;
    return { };
}

Vector<TestFailure> runTestText(String&& text, Vector<RunOutput>* outputs, const TestVariant& variant, std::optional<TestTodo>* todo, const String& folder)
{
    TestFile file = parseTestFile(WTF::move(text), hostArchitecture(), folder);
    if (variant.onlyDiagnostic) {
        bool expectsDiagnostic = file.diagnostics.containsIf([&](const ExpectedDiagnostic& expected) {
            return expected.kind == *variant.onlyDiagnostic;
        });
        if (!file.errors.isEmpty() || expectsDiagnostic)
            return { };
    }
    if (!file.errors.isEmpty()) {
        return WTF::map(WTF::move(file.errors), [](TestFileError&& error) {
            return TestFailure { error.line, WTF::move(error.message) };
        });
    }
    if (todo && !file.runs.isEmpty())
        *todo = file.todo;

    Vector<TestFailure> failures;
    Wire::CompileOptions options = makeOptionsOfTestedProgram();
    if (file.asUsersBuild) {
        for (const OptionWord& optionWord : file.optionWords) {
            if (optionWord.word.startsWith("check="_s))
                failures.append(TestFailure { optionWord.line, "a file with an AS-USERS-BUILD line asks for no check"_s });
        }
        for (const OptionWord& option : file.jscOptions) {
            if (option.word.contains("verifyAOTMachineCode"_s))
                failures.append(TestFailure { option.line, "a file with an AS-USERS-BUILD line does not set verifyAOTMachineCode"_s });
        }
        if (ASCIILiteral obstacle = findWhatTurnsChecksOnFromOutside(variant); !obstacle.isNull() && failures.isEmpty()) {
            dataLogLn("NOT RUN, not as users build here: ", obstacle, ": AS-USERS-BUILD: ", file.asUsersBuild->reason);
            if (todo)
                *todo = std::nullopt;
            return { };
        }
    } else
        RELEASE_ASSERT(!applyOptionWord(options, "check=VerifyEachStage"_s));
    std::optional<Mechanisms> mechanismsAskedFor;
    auto applyWord = [&](const String& word, unsigned line) {
        std::optional<String> sentence = isMechanismsWord(word) ? applyMechanismsWordAskedFor(mechanismsAskedFor, word) : applyOptionWord(options, word);
        if (sentence)
            failures.append(TestFailure { line, WTF::move(*sentence) });
    };
    for (const OptionWord& optionWord : file.optionWords)
        applyWord(optionWord.word, optionWord.line);
    for (const String& word : variant.optionWords)
        applyWord(word, 1);
    OptionsStorage optionsOfProcess = g_jscConfig.options;
    auto restoreOptionsOfProcess = makeScopeExit([&] {
        g_jscConfig.options = optionsOfProcess;
    });
    for (const OptionWord& option : file.jscOptions) {
        if (!Options::setOption(byteCast<char>(option.word.utf8().span().subspan(2).data()), false))
            failures.append(TestFailure { option.line, makeString("unknown JSC option "_s, option.word) });
    }
    if (!failures.isEmpty())
        return failures;

    bool hasRun = false;
    for (const RunDirective& run : file.runs) {
        if (!run.isLeftOutUnderFurtherOptionWords || variant.optionWords.isEmpty())
            hasRun |= runOneDirective(file, run, options, mechanismsAskedFor, variant, failures, outputs);
    }
    bool hasProgram = file.runs.containsIf([](const RunDirective& run) {
        const TestCommand* command = findTestCommand(run.command);
        return command && command->program != ProgramOfCommand::None;
    });
    if (hasRun && hasProgram && variant.optionWords.isEmpty() && !variant.onlyDiagnostic) {
        reportBreachesOfClosure(file, options, failures);
        reportTablesThatHangOnOrder(file, options, failures);
    }
    if (todo && !hasRun)
        *todo = std::nullopt;
    std::ranges::stable_sort(failures, { }, &TestFailure::line);
    return failures;
}

static Vector<TestFailure> runOneFile(const String& path, Vector<RunOutput>* outputs, const TestVariant& variant, std::optional<TestTodo>* todo)
{
    std::optional<Vector<uint8_t>> bytes = FileSystem::readEntireFile(path);
    if (!bytes) {
        bool exists = !!FileSystem::fileTypeFollowingSymlinks(path);
        return { TestFailure { 1, exists ? "is empty or cannot be read"_s : "no such file or folder"_s } };
    }

    String text = String::fromUTF8(bytes->span());
    if (text.isNull())
        return { TestFailure { 1, "is not UTF-8"_s } };
    return runTestText(WTF::move(text), outputs, variant, todo, FileSystem::parentPath(path));
}

enum class FileOutcome : uint8_t { Passed, Failed, Todo };

struct FileResult {
    FileOutcome outcome { FileOutcome::Failed };
    String report;
};

static constexpr ASCIILiteral todoLineStart = "TODO "_s;

static void printLineByLine(PrintStream& out, StringView text)
{
    constexpr size_t leastLengthOfPiece = 1 << 20;
    while (text.length() > leastLengthOfPiece) {
        size_t lineEnd = text.find('\n', leastLengthOfPiece);
        if (lineEnd == notFound)
            break;
        out.print(text.left(lineEnd + 1));
        text = text.substring(lineEnd + 1);
    }
    out.print(text);
}

static FileResult runOneFileInThisProcess(const String& path, const TestRunRequest& request)
{
    Vector<RunOutput> outputs;
    std::optional<TestTodo> todo;
    Vector<TestFailure> failures = runOneFile(path, request.showsOutput ? &outputs : nullptr, request.variant, &todo);

    bool isPlainRun = request.variant.optionWords.isEmpty() && !request.variant.onlyDiagnostic && mechanismsOfProcess() == allMechanisms;
    bool isOwed = todo && !failures.isEmpty() && !failures.containsIf([](const TestFailure& failure) { return failure.isInternalError; });
    if (todo && failures.isEmpty() && isPlainRun)
        failures.append(TestFailure { todo->line, "passes: remove the TODO line"_s });

    String optionWords = emptyString();
    for (const String& word : request.variant.optionWords)
        optionWords = makeString(optionWords, optionWords.isEmpty() ? " (with "_s : " "_s, word);
    if (!optionWords.isEmpty())
        optionWords = makeString(optionWords, ')');

    StringPrintStream report;
    if (isOwed)
        report.println(todoLineStart, path, ":", todo->line, ": ", todo->reason, optionWords);
    for (const TestFailure& failure : failures)
        report.println(isOwed ? "  still fails: " : "FAIL ", path, ":", failure.line, ": ", failure.message, optionWords);
    for (const RunOutput& output : outputs) {
        report.print("OUTPUT ", path, ":", output.line, ":\n");
        printLineByLine(report, output.text);
    }
    if (isOwed)
        return { FileOutcome::Todo, report.toString() };
    return { failures.isEmpty() ? FileOutcome::Passed : FileOutcome::Failed, report.toString() };
}

static size_t startOfLastLines(StringView text, size_t lineCount)
{
    size_t start = text.length();
    for (size_t i = 0; i < lineCount && start; ++i) {
        size_t previousLineEnd = start > 1 ? text.reverseFind('\n', static_cast<unsigned>(start - 2)) : notFound;
        start = previousLineEnd == notFound ? 0 : previousLineEnd + 1;
    }
    return start;
}

static Vector<String> childArguments(std::span<const String> paths, const TestRunRequest& request)
{
    Vector<String> arguments { String(runSubcommandName), String(singleProcessRunFlag) };
    arguments.append(paths);
    if (request.showsOutput)
        arguments.append(String(showOutputFlag));
    for (const String& word : request.variant.optionWords)
        arguments.append(makeString(optionFlag, '=', word));
    if (request.variant.onlyDiagnostic)
        arguments.append(makeString(onlyDiagnosticFlag, '=', nameOf(*request.variant.onlyDiagnostic)));
    return arguments;
}

static ChildProcessResult runInChildProcess(const ChildProcessLauncher& launcher, std::span<const String> paths, const TestRunRequest& request)
{
    ChildProcessResult result = launcher.run(childArguments(paths, request).span());
    for (ASCIILiteral lineEnd : { "\r\n"_s, "\n"_s })
        result.output = makeStringByReplacingAll(result.output, makeString(lineAfterChildProcess, lineEnd), ""_s);
    return result;
}

struct BatchResult {
    TestRunSummary summary;
    String report;
};

static void count(TestRunSummary& summary, FileOutcome outcome)
{
    switch (outcome) {
    case FileOutcome::Passed:
        ++summary.passed;
        break;
    case FileOutcome::Failed:
        ++summary.failed;
        break;
    case FileOutcome::Todo:
        ++summary.todo;
        break;
    }
}

static std::optional<TestRunSummary> parseSummaryLine(StringView line)
{
    TestRunSummary summary;
    unsigned* counts[] { &summary.passed, &summary.failed, &summary.todo };
    ASCIILiteral words[] { " passed"_s, " failed"_s, " todo"_s };
    unsigned index = 0;
    for (StringView part : line.trim(isASCIIWhitespace<char16_t>).splitAllowingEmptyEntries(',')) {
        part = part.trim(isASCIIWhitespace<char16_t>);
        if (index == std::size(words) || !part.endsWith(words[index]))
            return std::nullopt;
        std::optional<unsigned> number = parseInteger<unsigned>(part.left(part.length() - words[index].length()));
        if (!number)
            return std::nullopt;
        *counts[index++] = *number;
    }
    return index == std::size(words) ? std::optional(summary) : std::nullopt;
}

static FileResult runOneFileInChildProcess(const ChildProcessLauncher& launcher, const String& path, const TestRunRequest& request)
{
    ChildProcessResult result = runInChildProcess(launcher, singleElementSpan(path), request);

    StringView output = result.output;
    size_t summaryStart = startOfLastLines(output, 1);
    std::optional<TestRunSummary> summary = parseSummaryLine(output.substring(summaryStart));
    bool hasComeToItsEnd = summary && summary->passed + summary->failed + summary->todo == 1;
    if (hasComeToItsEnd && result.exitedWith(exitCodeForFailure))
        return { FileOutcome::Failed, output.left(summaryStart).toString() };
    if (hasComeToItsEnd && result.exitedWith(exitCodeForSuccess))
        return { output.startsWith(todoLineStart) ? FileOutcome::Todo : FileOutcome::Passed, output.left(summaryStart).toString() };

    StringView lastLines = output.substring(startOfLastLines(output, maxCrashOutputLinesShown));
    ASCIILiteral lineEnd = lastLines.isEmpty() || lastLines.endsWith('\n') ? ""_s : "\n"_s;
    return { FileOutcome::Failed, makeString(lastLines, lineEnd, "FAIL "_s, path, ":1: "_s, result.description(), '\n') };
}

static constexpr unsigned filesPerChildProcess = 16;

static BatchResult runFilesInChildProcesses(const ChildProcessLauncher& launcher, std::span<const String> paths, const TestRunRequest& request)
{
    BatchResult batch;
    StringBuilder report;
    if (paths.size() > 1) {
        ChildProcessResult result = runInChildProcess(launcher, paths, request);
        if (result.exitedWith(exitCodeForSuccess) || result.exitedWith(exitCodeForFailure)) {
            StringView output = result.output;
            size_t summaryStart = startOfLastLines(output, 1);
            std::optional<TestRunSummary> summary = parseSummaryLine(output.substring(summaryStart));
            if (summary && summary->passed + summary->failed + summary->todo == paths.size())
                return { *summary, output.left(summaryStart).toString() };
        }
        if (result.hasCrashedOrHung())
            report.append("The process of "_s, paths.size(), " files "_s, result.description(), '\n');
    }

    for (const String& path : paths) {
        FileResult result = runOneFileInChildProcess(launcher, path, request);
        count(batch.summary, result.outcome);
        report.append(result.report);
    }
    batch.report = report.toString();
    return batch;
}

TestRunSummary runTestFiles(const TestRunRequest& request, PrintStream& out)
{
    Vector<String> files = collectTestFiles(request);
    TestRunSummary summary;
    if (!request.launcher || files.size() <= 1) {
        if (!request.launcher)
            printLineAfterEachChildProcess();
        for (const String& file : files) {
            FileResult result = runOneFileInThisProcess(file, request);
            count(summary, result.outcome);
            out.print(result.report);
            out.flush();
        }
        return summary;
    }

    bool printsPaths = request.jobs == 1;
    unsigned batchSize = printsPaths ? 1 : filesPerChildProcess;
    unsigned batchCount = (files.size() + batchSize - 1) / batchSize;
    Vector<BatchResult> results(batchCount);
    ParallelLoops loops(std::clamp<unsigned>(batchCount, 1, request.jobs ? request.jobs : WTF::numberOfProcessorCores()));
    forEachIndexInParallel(loops, batchCount, [&](unsigned index) {
        std::span<const String> batch = files.span().subspan(index * batchSize, std::min<size_t>(batchSize, files.size() - index * batchSize));
        if (printsPaths) {
            out.println(batch[0]);
            out.flush();
        }
        results[index] = runFilesInChildProcesses(*request.launcher, batch, request);
    });

    for (const BatchResult& result : results) {
        out.print(result.report);
        summary.passed += result.summary.passed;
        summary.failed += result.summary.failed;
        summary.todo += result.summary.todo;
    }
    return summary;
}

void reopenDiagnostics(BuildContext& build)
{
    Diagnostics finished = build.takeDiagnostics();
    build.diagnostics().append(Vector<UserDiagnostic>(finished.userDiagnostics()));
    build.diagnostics().append(Vector<InternalError>(finished.internalErrors()));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
