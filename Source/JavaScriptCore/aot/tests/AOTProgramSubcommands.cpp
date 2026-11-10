/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTestHarness.h"

#if ENABLE(AOT)

#include "AOTBodyVerifier.h"
#include "AOTCompileOptionWords.h"
#include "AOTDiagnostics.h"
#include "AOTDriver.h"
#include "AOTIRPrinter.h"
#include "AOTMechanismWords.h"
#include "AOTOptionWords.h"
#include "AOTProgramFile.h"
#include "AOTProgramVerifier.h"
#include "AOTSchemaEnums.h"
#include "AOTStages.h"
#include "AOTTestFile.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include "Options.h"
#include <algorithm>
#include <expected>
#include <initializer_list>
#include <wtf/FilePrintStream.h>
#include <wtf/FileSystem.h>
#include <wtf/StdLibExtras.h>
#include <wtf/StringPrintStream.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct SubcommandFlag {
    StringView name;
    StringView value;
};

struct ProgramArguments {
    String programPath;
    Vector<String> words;
    Vector<SubcommandFlag> flags;
};

struct FlagsOfProgramSubcommand {
    String outputPath;
    SchemaHashCheck schemaHashCheck { SchemaHashCheck::Required };
};

struct InputsOfCommand {
    Wire::CompileOptions options { makeOptionsOfTestedProgram() };
    Vector<String> arguments;
    Vector<TestSection> sections;
    std::optional<FunctionID> functionOfTypesLine;
    Mechanisms mechanisms { mechanismsOfProcess() };
};

} // anonymous namespace

static constexpr ASCIILiteral wireFlag = "--wire"_s;
static constexpr ASCIILiteral outputFlag = "--output"_s;
static constexpr ASCIILiteral programOfTagFlag = "--program-of-tag"_s;

static constexpr ASCIILiteral checksOfEveryTestRun = "check=VerifyEachStage"_s;

static String unknownFlagSentence(StringView name)
{
    return makeString("unknown flag "_s, name);
}

static String repeatedFlagSentence(StringView name)
{
    return makeString(name, " may be specified only once"_s);
}

static String missingValueSentence(StringView name)
{
    return makeString(name, " needs a value"_s);
}

static SubcommandFlag splitSubcommandFlag(StringView word)
{
    size_t equalsSign = word.find('=');
    if (equalsSign == notFound)
        return { word, { } };
    return { word.left(equalsSign), word.substring(equalsSign + 1) };
}

static std::expected<ProgramArguments, String> parseProgramArguments(std::span<const String> words)
{
    ProgramArguments arguments;
    for (const String& word : words) {
        if (word.startsWith("--"_s))
            arguments.flags.append(splitSubcommandFlag(word));
        else if (arguments.programPath.isNull())
            arguments.programPath = word;
        else
            arguments.words.append(word);
    }
    if (arguments.programPath.isNull())
        return makeUnexpected(String("missing program"_s));
    return arguments;
}

static std::expected<FlagsOfProgramSubcommand, String> parseFlagsOfProgramSubcommand(std::span<const SubcommandFlag> flags, std::initializer_list<ASCIILiteral> flagsOfSubcommand)
{
    FlagsOfProgramSubcommand result;
    for (const SubcommandFlag& flag : flags) {
        bool isFlagOfSubcommand = std::ranges::any_of(flagsOfSubcommand, [&](ASCIILiteral name) {
            return flag.name == name;
        });
        if (!isFlagOfSubcommand)
            return makeUnexpected(unknownFlagSentence(flag.name));

        if (flag.name == wireFlag || flag.name == programOfTagFlag) {
            if (!flag.value.isEmpty())
                return makeUnexpected(makeString(flag.name, " takes no value"_s));
            if (flag.name == programOfTagFlag)
                result.schemaHashCheck = SchemaHashCheck::Skipped;
            continue;
        }

        String& path = result.outputPath;
        if (flag.value.isEmpty())
            return makeUnexpected(missingValueSentence(flag.name));
        if (!path.isNull())
            return makeUnexpected(repeatedFlagSentence(flag.name));
        path = flag.value.toString();
    }
    return result;
}

static Wire::CompileOptions optionsToCompileBy(const ProgramFile& file, const String& path, PrintStream& errors)
{
    Wire::CompileOptions ofTest = makeOptionsOfTestedProgram();
    if (file.options) {
        Wire::CompileOptions ofBuild = *file.options;
        ofBuild.threadCount = ofTest.threadCount;
        ofBuild.executableIdentity = ofTest.executableIdentity;
        return ofBuild;
    }
    if (file.kind == ProgramFile::Kind::Bytes)
        errors.println(path, " brings no options of its build: it is compiled as if code from disk were allowed and nothing were left out");
    return ofTest;
}

static std::optional<String> applyWordsOfCommandLine(Wire::CompileOptions& options, Mechanisms& mechanisms, std::span<const String> words, Vector<String>* argumentsOfCommand)
{
    for (const String& word : words) {
        if (isMechanismsWord(word)) {
            if (std::optional<String> sentence = applyMechanismsWordOfTest(mechanisms, word))
                return makeString(word, ": "_s, *sentence);
            continue;
        }
        std::optional<String> sentence = applyOptionWord(options, word);
        if (!sentence)
            continue;
        if (!argumentsOfCommand || word.contains('='))
            return makeString(word, ": "_s, *sentence);
        argumentsOfCommand->append(word);
    }
    return std::nullopt;
}

static int reportInvalidCommandLine(StringView subcommand, const String& sentence, PrintStream& errors)
{
    errors.println("testaot ", subcommand, ": ", sentence);
    return exitCodeForInvalidCommandLine;
}

static int reportDiagnostics(const Diagnostics& diagnostics, PrintStream& errors)
{
    printDiagnostics(diagnostics, errors);
    return exitCodeForFailure;
}

static bool saysWhatTestsOfFrontEndWaitFor(const CompileOutput& output)
{
    for (const Wire::UserDiagnosticRecord& record : output.records<Wire::UserDiagnosticRecord>(Wire::ResultSection::UserDiagnostics)) {
        if (failsBuild(record.kind) || record.kind == UserDiagnosticKind::UnprovenDependencyInt32)
            return true;
    }
    return !output.section(Wire::ResultSection::InternalErrors).empty();
}

static bool takeDirectivesOfProgramText(const TestCommand& command, const ProgramFile& file, const String& path, InputsOfCommand& inputs, PrintStream& errors)
{
    TestFile test = parseTestFile(makeString(String::fromUTF8(file.bytes.span()), "\n# RUN: "_s, command.name, '\n'), hostArchitecture(), FileSystem::parentPath(path));
    for (const TestFileError& error : test.errors)
        errors.println(path, ":", error.line, ": ", error.message);
    if (!test.errors.isEmpty())
        return false;

    bool isUsable = true;
    for (const OptionWord& optionWord : test.optionWords) {
        std::optional<String> sentence = isMechanismsWord(optionWord.word) ? applyMechanismsWordOfTest(inputs.mechanisms, optionWord.word) : applyOptionWord(inputs.options, optionWord.word);
        if (sentence) {
            errors.println(path, ":", optionWord.line, ": ", *sentence);
            isUsable = false;
        }
    }
    for (const OptionWord& option : test.jscOptions) {
        if (!Options::setOption(byteCast<char>(option.word.utf8().span().subspan(2).data()), false)) {
            errors.println(path, ":", option.line, ": unknown JSC option ", option.word);
            isUsable = false;
        }
    }

    inputs.sections = WTF::move(test.sections);
    inputs.functionOfTypesLine = test.functionOfTypesLine;
    return isUsable;
}

static bool isHandedUnverifiedProgram(const TestCommand& command)
{
    return command.name == "verify"_s || command.name == "print"_s;
}

static int runCommandOnProgramFile(const TestCommand& command, ProgramFile&& file, SchemaHashCheck schemaHashCheck, const InputsOfCommand& inputs)
{
    FilePrintStream out(stdout, FilePrintStream::Borrow);
    FilePrintStream errors(stderr, FilePrintStream::Borrow);

    BuildContext build(inputs.options);
    String programText;
    std::unique_ptr<Program> program;
    if (command.program != ProgramOfCommand::None) {
        if (command.program == ProgramOfCommand::WithLibrary && file.kind == ProgramFile::Kind::Text)
            file.text = withStandInLibrary(file.text);
        programText = file.text;
        program = createProgram(WTF::move(file), build, schemaHashCheck);
        bool mayBeHandedOver = program && (isHandedUnverifiedProgram(command) || (verifyRecords(*program, build) && verifyBodies(*program, build)));
        if (!mayBeHandedOver) {
            build.diagnostics().finish(program.get());
            return reportDiagnostics(build.diagnostics(), errors);
        }
    }

    TestCommandContext context { WTF::move(program), inputs.arguments.span(), out, build };
    context.options = &inputs.options;
    context.sections = inputs.sections.span();
    context.functionOfTypesLine = inputs.functionOfTypesLine;
    context.programText = programText;
    context.mechanisms = inputs.mechanisms;
    command.run(context);
    out.flush();
    for (const String& note : context.notesForFailure)
        errors.println(note);

    build.diagnostics().finish(context.program.get());
    printDiagnostics(build.diagnostics(), errors);
    return build.diagnostics().hasErrors() ? exitCodeForFailure : exitCodeForSuccess;
}

int runCommandSubcommand(StringView name, std::span<const String> arguments)
{
    FilePrintStream errors(stderr, FilePrintStream::Borrow);

    const TestCommand* command = findTestCommand(name);
    if (!command)
        return reportInvalidCommandLine(name, "no command has this name: `testaot list commands` prints them all"_s, errors);

    auto programArguments = parseProgramArguments(arguments);
    if (!programArguments)
        return reportInvalidCommandLine(name, programArguments.error(), errors);
    auto flags = parseFlagsOfProgramSubcommand(programArguments->flags.span(), { programOfTagFlag });
    if (!flags)
        return reportInvalidCommandLine(name, flags.error(), errors);

    const String& path = programArguments->programPath;
    std::optional<ProgramFile> file = readProgramFile(path, errors);
    if (!file)
        return exitCodeForFailure;

    InputsOfCommand inputs;
    inputs.options = optionsToCompileBy(*file, path, errors);
    RELEASE_ASSERT(!applyOptionWord(inputs.options, checksOfEveryTestRun));
    if (file->kind == ProgramFile::Kind::Text && !takeDirectivesOfProgramText(*command, *file, path, inputs, errors))
        return exitCodeForFailure;
    if (std::optional<String> sentence = applyWordsOfCommandLine(inputs.options, inputs.mechanisms, programArguments->words.span(), &inputs.arguments))
        return reportInvalidCommandLine(name, *sentence, errors);

    int exitCode = exitCodeForFailure;
    runOnBuildThread([&] {
        exitCode = runCommandOnProgramFile(*command, WTF::move(*file), flags->schemaHashCheck, inputs);
    });
    return exitCode;
}

static int printWire(ProgramFile&& file, Wire::CompileOptions options, Mechanisms mechanisms, SchemaHashCheck schemaHashCheck)
{
    FilePrintStream out(stdout, FilePrintStream::Borrow);
    FilePrintStream errors(stderr, FilePrintStream::Borrow);

    options.action = Wire::CompileAction::CheckOnly;
    BuildContext build(options);
    std::unique_ptr<Program> program = createProgram(WTF::move(file), build, schemaHashCheck);
    if (!program || !verifyRecords(*program, build) || !verifyBodies(*program, build)) {
        build.diagnostics().finish(program.get());
        return reportDiagnostics(build.diagnostics(), errors);
    }
    StringPrintStream text;
    printProgram(text, *program, PrintedFields::WireOnly);

    std::unique_ptr<CompileOutput> output = compileWithMechanisms(WTF::move(program), build, mechanisms);
    if (saysWhatTestsOfFrontEndWaitFor(*output)) {
        printDiagnosticsOf(*output, errors);
        return exitCodeForFailure;
    }

    out.print(text.toUTF8CString());
    out.flush();
    return exitCodeForSuccess;
}

int runPrintSubcommand(std::span<const String> arguments)
{
    FilePrintStream errors(stderr, FilePrintStream::Borrow);

    bool isWire = std::ranges::any_of(arguments, [](const String& argument) {
        return splitSubcommandFlag(argument).name == wireFlag;
    });
    if (!isWire)
        return runCommandSubcommand("print"_s, arguments);

    auto programArguments = parseProgramArguments(arguments);
    if (!programArguments)
        return reportInvalidCommandLine("print"_s, programArguments.error(), errors);
    auto flags = parseFlagsOfProgramSubcommand(programArguments->flags.span(), { wireFlag, programOfTagFlag });
    if (!flags)
        return reportInvalidCommandLine("print"_s, flags.error(), errors);
    std::optional<ProgramFile> file = readProgramFile(programArguments->programPath, errors);
    if (!file)
        return exitCodeForFailure;
    Wire::CompileOptions options = optionsToCompileBy(*file, programArguments->programPath, errors);
    Mechanisms mechanisms = mechanismsOfProcess();
    if (std::optional<String> sentence = applyWordsOfCommandLine(options, mechanisms, programArguments->words.span(), nullptr))
        return reportInvalidCommandLine("print"_s, *sentence, errors);

    int exitCode = exitCodeForFailure;
    runOnBuildThread([&] {
        exitCode = printWire(WTF::move(*file), options, mechanisms, flags->schemaHashCheck);
    });
    return exitCode;
}

static std::unique_ptr<CompileOutput> compileProgramFile(ProgramFile&& file, const Wire::CompileOptions& options, Mechanisms mechanisms, SchemaHashCheck schemaHashCheck, PrintStream& errors)
{
    switch (file.kind) {
    case ProgramFile::Kind::Bytes:
        if (schemaHashCheck == SchemaHashCheck::Required)
            return compileProgramWithMechanisms(file.bytes.span(), options, mechanisms);
        break;
    case ProgramFile::Kind::Text:
        file.text = withStandInLibrary(file.text);
        break;
    }

    BuildContext build(options);
    std::unique_ptr<Program> program = createProgram(WTF::move(file), build, schemaHashCheck);
    if (!program) {
        build.diagnostics().finish(program.get());
        printDiagnostics(build.diagnostics(), errors);
        return nullptr;
    }
    return compileWithMechanisms(WTF::move(program), build, mechanisms);
}

int runCompileSubcommand(std::span<const String> arguments)
{
    FilePrintStream out(stdout, FilePrintStream::Borrow);
    FilePrintStream errors(stderr, FilePrintStream::Borrow);

    auto programArguments = parseProgramArguments(arguments);
    if (!programArguments)
        return reportInvalidCommandLine("compile"_s, programArguments.error(), errors);
    auto flags = parseFlagsOfProgramSubcommand(programArguments->flags.span(), { outputFlag, programOfTagFlag });
    if (!flags)
        return reportInvalidCommandLine("compile"_s, flags.error(), errors);
    if (flags->outputPath.isNull())
        return reportInvalidCommandLine("compile"_s, makeString(outputFlag, " is required"_s), errors);
    std::optional<ProgramFile> file = readProgramFile(programArguments->programPath, errors);
    if (!file)
        return exitCodeForFailure;
    Wire::CompileOptions options = optionsToCompileBy(*file, programArguments->programPath, errors);
    Vector<String> namesOfStatistics;
    Mechanisms mechanisms = mechanismsOfProcess();
    if (std::optional<String> sentence = applyWordsOfCommandLine(options, mechanisms, programArguments->words.span(), &namesOfStatistics))
        return reportInvalidCommandLine("compile"_s, *sentence, errors);
    Vector<StatisticID> printedStatistics;
    for (const String& word : namesOfStatistics) {
        std::optional<StatisticID> statistic = parseEnum<StatisticID>(word);
        if (!statistic || *statistic == StatisticID::None)
            return reportInvalidCommandLine("compile"_s, makeString("unknown statistic "_s, word), errors);
        printedStatistics.append(*statistic);
    }

    std::unique_ptr<CompileOutput> output;
    runOnBuildThread([&] {
        output = compileProgramFile(WTF::move(*file), options, mechanisms, flags->schemaHashCheck, errors);
    });
    if (!output)
        return exitCodeForFailure;

    printDiagnosticsOf(*output, errors);
    std::span<const uint64_t> statistics = output->records<uint64_t>(Wire::ResultSection::Statistics);
    for (StatisticID statistic : printedStatistics) {
        size_t index = static_cast<size_t>(statistic);
        out.println("statistic ", nameOf(statistic), " ", index < statistics.size() ? statistics[index] : 0);
    }
    std::span<const uint8_t> image = output->section(Wire::ResultSection::Image);
    if (image.empty())
        return exitCodeForFailure;
    if (FileSystem::overwriteEntireFile(flags->outputPath, image) != image.size()) {
        errors.println("cannot write ", flags->outputPath);
        return exitCodeForFailure;
    }
    std::span<const uint32_t> entryModuleIDs = output->records<uint32_t>(Wire::ResultSection::EntryModuleIds);
    for (size_t index = 0; index < entryModuleIDs.size(); ++index)
        out.println("entry ", index, " ", entryModuleIDs[index]);
    return exitCodeForSuccess;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
