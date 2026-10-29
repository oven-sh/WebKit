/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTestSubcommands.h"

#if ENABLE(AOT)

#include "AOTCompile.h"
#include "AOTCompileOptionWords.h"
#include "AOTDiagnostics.h"
#include "AOTIRParser.h"
#include "AOTIRPrinter.h"
#include "AOTPass.h"
#include "AOTPassID.h"
#include "AOTPipeline.h"
#include "AOTProgramFile.h"
#include "AOTReaderInternal.h"
#include "AOTSchemaEnums.h"
#include "AOTTestCommand.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include "AOTVerifier.h"
#include <expected>
#include <wtf/ASCIICType.h>
#include <wtf/FilePrintStream.h>
#include <wtf/FileSystem.h>
#include <wtf/StringPrintStream.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringToIntegerConversion.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct Flag {
    StringView name;
    StringView value;
};

struct ProgramArguments {
    String programPath;
    Wire::CompileOptions options { makeDefaultCompileOptions() };
    Vector<Flag> flags;
};

struct PrintFlags {
    std::optional<PassID> until;
    Vector<uint32_t> functionIndices;
    const TestCommand* command { nullptr };
    Vector<String> commandArguments;
    bool isWire { false };
};

} // anonymous namespace

static constexpr ASCIILiteral untilFlag = "--until"_s;
static constexpr ASCIILiteral functionFlag = "--function"_s;
static constexpr ASCIILiteral wireFlag = "--wire"_s;
static constexpr ASCIILiteral commandFlag = "--command"_s;
static constexpr ASCIILiteral outputFlag = "--output"_s;

static String unknownFlagSentence(StringView name)
{
    return makeString("unknown flag "_s, name);
}

static String repeatedFlagSentence(ASCIILiteral name)
{
    return makeString(name, " may be specified only once"_s);
}

static Flag splitFlag(StringView word)
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
        if (word.startsWith("--"_s)) {
            arguments.flags.append(splitFlag(word));
            continue;
        }
        if (arguments.programPath.isNull()) {
            arguments.programPath = word;
            continue;
        }
        if (std::optional<String> sentence = applyOptionWord(arguments.options, word))
            return makeUnexpected(makeString(word, ": "_s, *sentence));
    }
    if (arguments.programPath.isNull())
        return makeUnexpected(String("missing program"_s));
    return arguments;
}

static String missingValueSentence(StringView name)
{
    return makeString(name, " needs a value"_s);
}

static std::optional<uint32_t> parseFunctionIndex(StringView id)
{
    if (!id.startsWith('f'))
        return std::nullopt;
    StringView digits = id.substring(1);
    if (!digits.containsOnly<isASCIIDigit>())
        return std::nullopt;
    return parseInteger<uint32_t>(digits, 10, WTF::ParseIntegerWhitespacePolicy::Disallow);
}

static std::expected<PrintFlags, String> parsePrintFlags(std::span<const Flag> flags)
{
    PrintFlags result;
    for (const Flag& flag : flags) {
        if (flag.name == wireFlag) {
            if (!flag.value.isEmpty())
                return makeUnexpected(makeString(wireFlag, " takes no value"_s));
            result.isWire = true;
            continue;
        }
        if (flag.name != functionFlag && flag.name != untilFlag && flag.name != commandFlag)
            return makeUnexpected(unknownFlagSentence(flag.name));
        if (flag.value.isEmpty())
            return makeUnexpected(missingValueSentence(flag.name));
        if (flag.name == commandFlag) {
            if (result.command)
                return makeUnexpected(repeatedFlagSentence(commandFlag));
            for (StringView word : flag.value.split(' '))
                result.commandArguments.append(word.toString());
            result.command = findTestCommand(result.commandArguments[0]);
            if (!result.command || result.command->programAsRead != ProgramAsRead::WithLibrary)
                return makeUnexpected(makeString(commandFlag, " takes a command that runs the whole pipeline, not "_s, result.commandArguments[0]));
            result.commandArguments.removeAt(0);
            continue;
        }
        if (flag.name == functionFlag) {
            std::optional<uint32_t> index = parseFunctionIndex(flag.value);
            if (!index)
                return makeUnexpected(makeString(functionFlag, " takes a function id such as f12, not "_s, flag.value));
            result.functionIndices.append(*index);
            continue;
        }
        if (result.until)
            return makeUnexpected(repeatedFlagSentence(untilFlag));
        result.until = passNamed(flag.value);
        if (!result.until)
            return makeUnexpected(makeString("unknown pass "_s, flag.value));
    }
    if (result.isWire && (result.until || !result.functionIndices.isEmpty() || result.command))
        return makeUnexpected(makeString(wireFlag, " takes no other flag"_s));
    if (result.command && (result.until || !result.functionIndices.isEmpty()))
        return makeUnexpected(makeString(commandFlag, " takes no other flag"_s));
    return result;
}

static std::expected<String, String> parseOutputPath(std::span<const Flag> flags)
{
    String outputPath;
    for (const Flag& flag : flags) {
        if (flag.name != outputFlag)
            return makeUnexpected(unknownFlagSentence(flag.name));
        if (flag.value.isEmpty())
            return makeUnexpected(missingValueSentence(flag.name));
        if (!outputPath.isNull())
            return makeUnexpected(repeatedFlagSentence(outputFlag));
        outputPath = flag.value.toString();
    }
    if (outputPath.isNull())
        return makeUnexpected(makeString(outputFlag, " is required"_s));
    return outputPath;
}

static int reportInvalidCommandLine(ASCIILiteral subcommand, const String& sentence, PrintStream& errors)
{
    errors.println("testaot ", subcommand, ": ", sentence);
    return exitCodeForInvalidCommandLine;
}

static int reportDiagnostics(const Diagnostics& diagnostics, PrintStream& errors)
{
    printDiagnostics(diagnostics, errors);
    return exitCodeForFailure;
}

static std::unique_ptr<Program> computeProgramAfterPass(PassID last, std::unique_ptr<Program> program, BuildContext& build)
{
    std::unique_ptr<PipelineOutput> output = runPipelineUntil(last, WTF::move(program), build);
    if (!output)
        return nullptr;
    return WTF::move(output->program);
}

static std::unique_ptr<CompileOutput> compileParsed(std::unique_ptr<Program> program, BuildContext& build)
{
    return compileParsedProgram(WTF::move(program), build);
}

static std::optional<uint32_t> findMissingFunction(const Program& program, std::span<const uint32_t> functionIndices)
{
    for (uint32_t index : functionIndices) {
        if (index >= program.functions.size())
            return index;
    }
    return std::nullopt;
}

static void printSelectedFunctions(PrintStream& out, const Program& program, std::span<const uint32_t> functionIndices, BuildContext& build)
{
    if (functionIndices.empty() && !build.options().logging.functionToDump.span().empty()) {
        build.resolveFunctionToDump(program);
        for (uint32_t index = 0; index < program.functions.size(); ++index) {
            FunctionID id(index);
            if (program.at(id).hasBody() && build.shouldDump(id))
                printFunction(out, program, id, program.at(id));
        }
        return;
    }
    if (functionIndices.empty()) {
        printProgram(out, program);
        return;
    }
    for (uint32_t index : functionIndices) {
        FunctionID id(index);
        printFunction(out, program, id, program.at(id));
    }
}

static std::unique_ptr<Program> createProgramWithLibrary(const ProgramFile& file, BuildContext& build)
{
    if (file.kind == ProgramFile::Kind::Text)
        return parseProgram(withStandInLibrary(file.text), IRForm::AsRead, build);
    return createProgram(file, build);
}

static bool hasDiagnostics(const Diagnostics& diagnostics)
{
    return !diagnostics.userDiagnostics().isEmpty() || !diagnostics.internalErrors().isEmpty();
}

// The program as it arrived: a type names `L5`, whatever set of layouts the compiler takes `L5` for. Encoded again, the text gives the bytes that were read.
static std::unique_ptr<Program> createProgramAsWritten(const ProgramFile& file, BuildContext& build)
{
    if (file.kind != ProgramFile::Kind::Bytes)
        return createProgram(file, build);
    auto program = makeUnique<Program>(BuiltinTable::singleton());
    program->borrowBuffer(file.bytes.span());
    Reader reader(*program, IRForm::AsRead, build);
    reader.keepLayoutsAsWritten();
    if (!reader.run())
        return nullptr;
    return program;
}

static int printWire(const ProgramFile& file, Wire::CompileOptions options)
{
    FilePrintStream out(stdout, FilePrintStream::Borrow);
    FilePrintStream errors(stderr, FilePrintStream::Borrow);

    options.action = Wire::CompileAction::VerifyOnly;
    StringPrintStream text;
    {
        BuildContext build(options);
        std::unique_ptr<Program> arrived = createProgramAsWritten(file, build);
        if (!arrived) {
            build.diagnostics().finish(nullptr);
            return reportDiagnostics(build.diagnostics(), errors);
        }
        printProgram(text, *arrived, PrintedFields::WireOnly);
    }

    BuildContext build(options);
    std::unique_ptr<Program> program = createProgram(file, build);
    if (!program) {
        build.diagnostics().finish(nullptr);
        return reportDiagnostics(build.diagnostics(), errors);
    }

    std::unique_ptr<CompileOutput> output = compileParsed(WTF::move(program), build);
    if (hasDiagnostics(output->diagnostics))
        return reportDiagnostics(output->diagnostics, errors);
    out.print(text.toUTF8CString());
    return exitCodeForSuccess;
}

static int printProgramFile(const ProgramFile& file, const ProgramArguments& arguments, const PrintFlags& flags)
{
    FilePrintStream out(stdout, FilePrintStream::Borrow);
    FilePrintStream errors(stderr, FilePrintStream::Borrow);

    BuildContext build(arguments.options);
    std::unique_ptr<Program> program = flags.until || flags.command ? createProgramWithLibrary(file, build) : createProgram(file, build);
    if (!program) {
        build.diagnostics().finish(nullptr);
        return reportDiagnostics(build.diagnostics(), errors);
    }
    build.resolveFunctionToDump(*program);

    if (flags.command) {
        TestCommandContext context { WTF::move(program), IRForm::AsRead, flags.commandArguments.span(), out, build };
        flags.command->run(context);
        out.flush();
        build.diagnostics().finish(context.program.get());
        return build.diagnostics().hasErrors() ? reportDiagnostics(build.diagnostics(), errors) : exitCodeForSuccess;
    }

    if (flags.until) {
        program = computeProgramAfterPass(*flags.until, WTF::move(program), build);
        if (!program)
            return reportDiagnostics(build.diagnostics(), errors);
    }

    if (std::optional<uint32_t> missingIndex = findMissingFunction(*program, flags.functionIndices.span()))
        return reportInvalidCommandLine("print"_s, makeString("there is no f"_s, *missingIndex), errors);
    printSelectedFunctions(out, *program, flags.functionIndices.span(), build);
    out.flush();

    build.diagnostics().finish(program.get());
    if (!hasDiagnostics(build.diagnostics()))
        return exitCodeForSuccess;
    printDiagnostics(build.diagnostics(), errors);
    return build.diagnostics().hasErrors() ? exitCodeForFailure : exitCodeForSuccess;
}

int runPrintSubcommand(std::span<const String> arguments)
{
    FilePrintStream errors(stderr, FilePrintStream::Borrow);

    auto programArguments = parseProgramArguments(arguments);
    if (!programArguments)
        return reportInvalidCommandLine("print"_s, programArguments.error(), errors);
    auto flags = parsePrintFlags(programArguments->flags.span());
    if (!flags)
        return reportInvalidCommandLine("print"_s, flags.error(), errors);

    std::optional<ProgramFile> file = readProgramFile(programArguments->programPath, errors);
    if (!file)
        return exitCodeForFailure;

    int exitCode = exitCodeForFailure;
    runOnBuildThread([&] {
        exitCode = flags->isWire ? printWire(*file, programArguments->options) : printProgramFile(*file, *programArguments, *flags);
    });
    return exitCode;
}

static std::unique_ptr<CompileOutput> compileProgramFile(const ProgramFile& file, const Wire::CompileOptions& options, PrintStream& errors)
{
    switch (file.kind) {
    case ProgramFile::Kind::Bytes:
        return compileProgram(file.bytes.span(), options);
    case ProgramFile::Kind::Text: {
        BuildContext build(options);
        std::unique_ptr<Program> program = createProgramWithLibrary(file, build);
        if (!program) {
            build.diagnostics().finish(nullptr);
            printDiagnostics(build.diagnostics(), errors);
            return nullptr;
        }
        return compileParsed(WTF::move(program), build);
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

int runCompileSubcommand(std::span<const String> arguments)
{
    FilePrintStream out(stdout, FilePrintStream::Borrow);
    FilePrintStream errors(stderr, FilePrintStream::Borrow);

    auto programArguments = parseProgramArguments(arguments);
    if (!programArguments)
        return reportInvalidCommandLine("compile"_s, programArguments.error(), errors);
    auto outputPath = parseOutputPath(programArguments->flags.span());
    if (!outputPath)
        return reportInvalidCommandLine("compile"_s, outputPath.error(), errors);

    std::optional<ProgramFile> file = readProgramFile(programArguments->programPath, errors);
    if (!file)
        return exitCodeForFailure;

    std::unique_ptr<CompileOutput> output;
    runOnBuildThread([&] {
        output = compileProgramFile(*file, programArguments->options, errors);
    });
    if (!output)
        return exitCodeForFailure;

    printDiagnostics(output->diagnostics, errors);
    if (output->image.isEmpty())
        return exitCodeForFailure;
    if (FileSystem::overwriteEntireFile(*outputPath, output->image.span()) != output->image.size()) {
        errors.println("cannot write ", *outputPath);
        return exitCodeForFailure;
    }
    for (size_t index = 0; index < output->entryModuleIDs.size(); ++index)
        out.println("entry ", index, " ", output->entryModuleIDs[index]);
    return exitCodeForSuccess;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
