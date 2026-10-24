/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTestRunner.h"

#if ENABLE(AOT)

#include "AOTCheckLines.h"
#include "AOTChildProcess.h"
#include "AOTCompileOptionWords.h"
#include "AOTIRParser.h"
#include "AOTProgramFile.h"
#include "AOTSchemaEnums.h"
#include "AOTTestFile.h"
#include "AOTTestSubcommands.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include <algorithm>
#include <wtf/FileSystem.h>
#include <wtf/NumberOfCores.h>
#include <wtf/StringPrintStream.h>
#include <wtf/text/MakeString.h>

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

static size_t countOf(const Diagnostics& diagnostics, const DiagnosticName& name)
{
    return WTF::switchOn(name, [&](UserDiagnosticKind kind) -> size_t {
        return std::ranges::count(diagnostics.userDiagnostics(), kind, &UserDiagnostic::kind);
    }, [&](InternalErrorKind kind) -> size_t {
        return std::ranges::count(diagnostics.internalErrors(), kind, &InternalError::kind);
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
    }
};

static void compareDiagnostics(const TestFile& file, const RunDirective& run, const Diagnostics& diagnostics, Vector<TestFailure>& failures)
{
    for (const ExpectedDiagnostic& expected : file.diagnostics) {
        size_t count = countOf(diagnostics, expected.kind);
        if (!count)
            appendFailure(failures, run, expected.line, "expected diagnostic "_s, nameOf(expected.kind), ", none was reported"_s);
        else if (expected.count && *expected.count != count)
            appendFailure(failures, run, expected.line, "expected "_s, *expected.count, " of "_s, nameOf(expected.kind), ", got "_s, count);
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

static Vector<Wire::LossRecord> joinByLimit(std::span<const Wire::LossRecord> records)
{
    Vector<Wire::LossRecord> joined;
    for (const Wire::LossRecord& record : records) {
        size_t index = joined.findIf([&](const Wire::LossRecord& other) {
            return other.limit == record.limit;
        });
        if (index == notFound)
            joined.append(record);
        else
            joined[index].count += record.count;
    }
    return joined;
}

template<typename ID>
static void compareLossSubject(const ExpectedLoss& expected, const RunDirective& run, char letter, std::optional<ID> wanted, uint32_t actual, Vector<TestFailure>& failures)
{
    if (!wanted || wanted->index() == actual)
        return;
    String actualWord = actual == Wire::none ? makeString(letter, '-') : makeString(letter, actual);
    appendFailure(failures, run, expected.line, "limit "_s, nameOf(expected.limit), ": expected "_s, letter, wanted->index(), ", got "_s, actualWord);
}

enum class UnlistedLosses : uint8_t { Fail, Allow };

static void compareLosses(const TestFile& file, const RunDirective& run, std::span<const Wire::LossRecord> records, UnlistedLosses unlistedLosses, Vector<TestFailure>& failures)
{
    Vector<Wire::LossRecord> joined = joinByLimit(records);
    for (const ExpectedLoss& expected : file.losses) {
        size_t index = joined.findIf([&](const Wire::LossRecord& record) {
            return record.limit == expected.limit;
        });
        if (index == notFound) {
            if (expected.count != 0u)
                appendFailure(failures, run, expected.line, "expected a loss at limit "_s, nameOf(expected.limit), ", none was reported"_s);
            continue;
        }
        const Wire::LossRecord& record = joined[index];
        if (expected.count && *expected.count != record.count)
            appendFailure(failures, run, expected.line, "limit "_s, nameOf(expected.limit), ": expected x"_s, *expected.count, ", got x"_s, record.count);
        compareLossSubject(expected, run, 'f', expected.function, record.witness.function, failures);
        compareLossSubject(expected, run, 'v', expected.value, record.witness.value, failures);
        compareLossSubject(expected, run, 'L', expected.layout, record.layout, failures);
    }

    if (unlistedLosses == UnlistedLosses::Allow)
        return;
    for (const Wire::LossRecord& record : joined) {
        bool isExpected = file.losses.containsIf([&](const ExpectedLoss& expected) {
            return expected.limit == record.limit;
        });
        if (isExpected)
            continue;
        String function = record.witness.function == Wire::none ? "f-"_str : makeString('f', record.witness.function);
        appendFailure(failures, run, run.line, "unexpected loss at limit "_s, nameOf(record.limit), " (x"_s, record.count, ", "_s, function, ')');
    }
}

struct CommandResult {
    String output;
    Vector<Wire::LossRecord> losses;
};

static CommandResult runCommand(const TestCommand& command, const TestFile& file, const RunDirective& run, BuildContext& build)
{
    IRForm form = IRForm::AsRead;
    std::unique_ptr<Program> program;
    if (command.defaultForm) {
        form = file.form.value_or(*command.defaultForm);
        bool needsLibrary = form == IRForm::AsRead && command.programAsRead != ProgramAsRead::AsParsed;
        program = parseProgram(needsLibrary ? withStandInLibrary(file.text) : file.text, form, build, VerifierInputs { .isOptimizing = file.isOptimizing });
        if (!program) {
            build.diagnostics().finish(nullptr);
            return { emptyString(), build.takeLosses(nullptr) };
        }
        if (form == IRForm::AsRead && command.programAsRead == ProgramAsRead::TypedAndVerified && !runReadStage(*program, build)) {
            build.diagnostics().finish(program.get());
            return { emptyString(), build.takeLosses(program.get()) };
        }
    }

    StringPrintStream out;
    TestCommandContext context { WTF::move(program), form, run.arguments.span(), out, build };
    context.isOptimizing = file.isOptimizing;
    command.run(context);
    build.diagnostics().finish(context.program.get());
    Vector<Wire::LossRecord> losses = WTF::move(context.losses);
    losses.appendVector(build.takeLosses(context.program.get()));
    return { out.toStringWithLatin1Fallback(), WTF::move(losses) };
}

static void runOneDirective(const TestFile& file, const RunDirective& run, const Wire::CompileOptions& options, const TestVariant& variant, Vector<TestFailure>& failures, Vector<RunOutput>* outputs)
{
    const TestCommand* command = findTestCommand(run.command);
    if (!command) {
        failures.append(TestFailure { run.line, makeString("unknown command "_s, run.command) });
        return;
    }
    if (!command->defaultForm && file.form) {
        failures.append(TestFailure { file.formLine, makeString(run.command, " takes no program"_s) });
        return;
    }

    runOnBuildThread([&] {
        BuildContext build(options);
        CommandResult result = runCommand(*command, file, run, build);
        if (variant.onlyDiagnostic) {
            reportDiagnostic(*variant.onlyDiagnostic, run, build.diagnostics(), failures);
            if (outputs)
                outputs->append(RunOutput { run.line, WTF::move(result.output) });
            return;
        }

        compareDiagnostics(file, run, build.diagnostics(), failures);
        compareStatistics(file, run, build.statistics(), failures);
        bool runsWholePipeline = command->programAsRead == ProgramAsRead::WithLibrary;
        compareLosses(file, run, result.losses.span(), runsWholePipeline ? UnlistedLosses::Allow : UnlistedLosses::Fail, failures);
        if (std::optional<CheckFailure> failure = matchCheckLines(file.checkLinesFor(run.prefix), result.output))
            appendFailure(failures, run, failure->line, failure->message);
        if (outputs)
            outputs->append(RunOutput { run.line, WTF::move(result.output) });
    });
}

Vector<TestFailure> runTestText(String&& text, Vector<RunOutput>* outputs, const TestVariant& variant, std::optional<TestTodo>* todo, const String& folder)
{
    TestFile file = parseTestFile(WTF::move(text), hostArchitecture(), folder);
    if (variant.onlyDiagnostic) {
        // The plain run of the file says what is wrong with its directives, and a file that plants the cause of the diagnostic has nothing to show here.
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
    Wire::CompileOptions options = makeDefaultCompileOptions();
    options.checks = Wire::CheckSet::VerifyEachStage;
    for (const OptionWord& optionWord : file.optionWords) {
        if (std::optional<String> sentence = applyOptionWord(options, optionWord.word))
            failures.append(TestFailure { optionWord.line, WTF::move(*sentence) });
    }
    for (const String& word : variant.optionWords) {
        if (std::optional<String> sentence = applyOptionWord(options, word))
            failures.append(TestFailure { 1, WTF::move(*sentence) });
    }
    if (!failures.isEmpty())
        return failures;

    for (const RunDirective& run : file.runs)
        runOneDirective(file, run, options, variant, failures, outputs);
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

// A stream that collects text cuts a very long string short.
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

    bool isPlainRun = request.variant.optionWords.isEmpty() && !request.variant.onlyDiagnostic;
    bool isOwed = todo && !failures.isEmpty();
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

static FileResult runOneFileInChildProcess(const ChildProcessLauncher& launcher, const String& path, const TestRunRequest& request)
{
    Vector<String, 3> arguments { String(runSubcommandName), path };
    if (request.showsOutput)
        arguments.append(String(showOutputFlag));
    for (const String& word : request.variant.optionWords)
        arguments.append(makeString(optionFlag, '=', word));
    if (request.variant.onlyDiagnostic)
        arguments.append(makeString(onlyDiagnosticFlag, '=', nameOf(*request.variant.onlyDiagnostic)));
    ChildProcessResult result = launcher.run(arguments.span());

    StringView output = result.output;
    if (result.exitedWith(exitCodeForFailure))
        return { FileOutcome::Failed, output.left(startOfLastLines(output, 1)).toString() };
    if (result.exitedWith(exitCodeForSuccess))
        return { output.startsWith(todoLineStart) ? FileOutcome::Todo : FileOutcome::Passed, output.left(startOfLastLines(output, 1)).toString() };

    StringView lastLines = output.substring(startOfLastLines(output, maxCrashOutputLinesShown));
    ASCIILiteral lineEnd = lastLines.isEmpty() || lastLines.endsWith('\n') ? ""_s : "\n"_s;
    return { FileOutcome::Failed, makeString(lastLines, lineEnd, "FAIL "_s, path, ":1: "_s, result.description(), '\n') };
}

TestRunSummary runTestFiles(const TestRunRequest& request, PrintStream& out)
{
    Vector<String> files = collectTestFiles(request);
    Vector<FileResult> results(files.size());
    const ChildProcessLauncher* launcher = files.size() > 1 ? request.launcher : nullptr;

    ParallelLoops loops(std::clamp<unsigned>(files.size(), 1, request.jobs ? request.jobs : WTF::numberOfProcessorCores()));
    bool printsPaths = request.jobs == 1;
    forEachIndexInParallel(loops, files.size(), [&](unsigned index) {
        if (printsPaths) {
            out.println(files[index]);
            out.flush();
        }
        if (launcher)
            results[index] = runOneFileInChildProcess(*launcher, files[index], request);
        else
            results[index] = runOneFileInThisProcess(files[index], request);
    });

    TestRunSummary summary;
    for (const FileResult& result : results) {
        out.print(result.report);
        switch (result.outcome) {
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
