/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTestHarness.h"

#if ENABLE(AOT)

#include "AOTBodiesThatCodeDependsOn.h"
#include "AOTChildProcess.h"
#include "AOTCodedTexts.h"
#include "AOTCompileUnit.h"
#include "AOTDriver.h"
#include "AOTIRPrinter.h"
#include "AOTImageFlagNames.h"
#include "AOTImageFormat.h"
#include "AOTImageText.h"
#include "AOTPrefixCode.h"
#include "AOTProgramFile.h"
#include "AOTProgramTables.h"
#include "AOTSchemaEnums.h"
#include "AOTShellProcess.h"
#include "AOTSideTableCodes.h"
#include "AOTStages.h"
#include "AOTTemporaryFile.h"
#include "AOTTestRunner.h"
#include "AOTVector.h"
#include <algorithm>
#include <utility>
#include <wtf/ASCIICType.h>
#include <wtf/Noncopyable.h>
#include <wtf/URL.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct WordsOfShellRun {
    Vector<String> options;
    String statements;
};

struct SourceFileOfTestProgram {
    String name;
    String text;
};

struct NamedImageSection {
    ASCIILiteral name;
    ImageSection section;
};

struct NamedImageFunctionFlag {
    ASCIILiteral name;
    ImageFunctionFlag flag;
};

class BodiesTakenFromProgram {
    WTF_MAKE_NONCOPYABLE(BodiesTakenFromProgram);
public:
    explicit BodiesTakenFromProgram(Program& program)
        : m_program(program)
    {
        m_bodies.reserveInitialCapacity(program.functions.size());
        for (Function& function : program.functions)
            m_bodies.append(std::exchange(function.body, withoutWords(function.body)));
    }

    ~BodiesTakenFromProgram()
    {
        for (size_t index = 0; index < m_bodies.size(); ++index)
            m_program.functions[index].body = m_bodies[index];
    }

    void putBack(FunctionID function) { m_program.at(function).body = m_bodies[function.index()]; }
    void takeAgain(FunctionID function) { m_program.at(function).body = withoutWords(m_bodies[function.index()]); }

    Vector<FunctionID> functionsWithoutTheirBodies() const
    {
        Vector<FunctionID> functions;
        for (uint32_t index = 0; index < m_bodies.size(); ++index) {
            if (m_bodies[index] && !m_program.functions[index].body.words().data())
                functions.append(FunctionID(index));
        }
        return functions;
    }

private:
    static Body withoutWords(const Body& body) { return body ? Body({ }, body.numberOfBlocks(), body.numberOfValues(), body.unit()) : Body(); }

    Program& m_program;
    Vector<Body> m_bodies;
};

} // anonymous namespace

static void failRunOfProgramCommand(BuildContext& build, ASCIILiteral why)
{
    build.diagnostics().add(InternalError { .kind = InternalErrorKind::Unknown, .detail = why });
}

static std::unique_ptr<CompileOutput> compileProgramOfCommand(TestCommandContext& context)
{
    std::unique_ptr<CompileOutput> output = compileWithMechanisms(WTF::move(context.program), context.build, context.mechanisms);
    reopenDiagnostics(context.build);
    return output;
}

static void runCompileCommand(TestCommandContext& context)
{
    Vector<StatisticID> printedStatistics;
    for (const String& word : context.arguments) {
        std::optional<StatisticID> statistic = parseEnum<StatisticID>(word);
        if (!statistic || *statistic == StatisticID::None) {
            context.out.println("error: unknown statistic ", word);
            return;
        }
        printedStatistics.append(*statistic);
    }

    std::unique_ptr<CompileOutput> output = compileProgramOfCommand(context);
    for (const Wire::UserDiagnosticRecord& record : output->records<Wire::UserDiagnosticRecord>(Wire::ResultSection::UserDiagnostics))
        context.out.println("diagnostic ", nameOf(record.kind));
    for (const Wire::InternalErrorRecord& record : output->records<Wire::InternalErrorRecord>(Wire::ResultSection::InternalErrors))
        context.out.println("diagnostic ", nameOf(record.kind));
    context.out.println("image ", output->hasImage() ? "not empty"_s : "empty"_s);
    std::span<const uint64_t> values = output->records<uint64_t>(Wire::ResultSection::Statistics);
    for (StatisticID statistic : printedStatistics) {
        size_t index = static_cast<size_t>(statistic);
        context.out.println("statistic ", nameOf(statistic), " ", index < values.size() ? values[index] : 0);
    }
}

static WordsOfShellRun splitWordsOfShellRun(std::span<const String> words)
{
    WordsOfShellRun run;
    StringBuilder statements;
    statements.append("globalThis.id = value => value; "_s);
    bool isOptionOfShell = true;
    for (const String& word : words) {
        isOptionOfShell = isOptionOfShell && word.startsWith("--"_s);
        if (isOptionOfShell)
            run.options.append(word);
        else
            statements.append(word, ' ');
    }
    run.statements = statements.toString();
    return run;
}

static ChildProcessResult startProgramInShell(const WordsOfShellRun& run, Vector<String>&& arguments, const String& promiseOfProgram)
{
    arguments.appendVector(run.options);
    CString statements = makeString(run.statements, promiseOfProgram, ".catch(error => print(\"the program threw \" + error));"_s).utf8();
    TemporaryFile fileOfStatements(".js"_s, byteCast<uint8_t>(statements.span()));
    arguments.append(fileOfStatements.path());
    ChildProcessResult result = ChildProcessLauncher(shellPath(), { }).run(arguments.span());
    result.output = makeStringByReplacingAll(result.output, "\r\n"_s, "\n"_s);
    return result;
}

static void printOutputOfShell(TestCommandContext& context, const ChildProcessResult& result)
{
    context.out.print(result.output);
    if (!result.output.isEmpty() && !result.output.endsWith('\n'))
        context.out.println();
    context.out.println(result.descriptionOfOutcome());
    if (result.hasCrashedOrHung())
        context.notesForFailure.append(makeString("the shell "_s, result.description()));
}

static Vector<SourceFileOfTestProgram> findSourcesOfTestProgram(StringView programText)
{
    constexpr ASCIILiteral startOfFile = "# -- source: "_s;
    constexpr ASCIILiteral endOfSources = "# -- end of source"_s;

    Vector<SourceFileOfTestProgram> files;
    StringBuilder text;
    auto finishFile = [&] {
        if (!files.isEmpty())
            files.last().text = text.toString();
        text.clear();
    };
    for (StringView line : programText.splitAllowingEmptyEntries('\n')) {
        if (line.endsWith('\r'))
            line = line.left(line.length() - 1);
        if (line.startsWith(startOfFile)) {
            finishFile();
            files.append(SourceFileOfTestProgram { line.substring(startOfFile.length()).trim(isTabOrSpace<char16_t>).toString(), { } });
            continue;
        }
        if (files.isEmpty())
            continue;
        if (line == endOfSources || !line.startsWith('#'))
            break;
        text.append(line.substring(line.length() > 1 && line[1] == ' ' ? 2 : 1), '\n');
    }
    finishFile();
    return files;
}

static bool canBeRunByShellItself(const Vector<SourceFileOfTestProgram>& sources)
{
    return sources.size() == 1 && (sources[0].name.endsWith(".js"_s) || sources[0].name.endsWith(".mjs"_s));
}

static ChildProcessResult startSourceInShell(const WordsOfShellRun& run, const SourceFileOfTestProgram& source)
{
    CString utf8 = source.text.utf8();
    TemporaryFile file(".js"_s, asBytes(utf8.span()));
    StringBuilder promiseOfProgram;
    promiseOfProgram.append("import("_s);
    promiseOfProgram.appendQuotedJSONString(URL::fileURLWithFileSystemPath(file.path()).string());
    promiseOfProgram.append(')');
    return startProgramInShell(run, { }, promiseOfProgram.toString());
}

static bool hasOwnLibraryLine(StringView programText)
{
    for (StringView line : programText.splitAllowingEmptyEntries('\n')) {
        if (line == ownLibraryLine)
            return true;
    }
    return false;
}

static void runInShellCommand(TestCommandContext& context)
{
    std::unique_ptr<CompileOutput> output = compileProgramOfCommand(context);
    std::span<const uint32_t> entryModuleIDs = output->records<uint32_t>(Wire::ResultSection::EntryModuleIds);
    if (!output->hasImage() || entryModuleIDs.size() != 1) {
        context.out.println("image ", output->hasImage() ? "without one entry module"_s : "empty"_s);
        return;
    }

    WordsOfShellRun run = splitWordsOfShellRun(context.arguments);
    TemporaryFile image(".image"_s, output->section(Wire::ResultSection::Image));
    ChildProcessResult compiled = startProgramInShell(run, { makeString("--aotImagePath="_s, image.path()) }, makeString("runCompiledEntryPoint("_s, entryModuleIDs[0], ')'));
    printOutputOfShell(context, compiled);
    if (compiled.output.contains(lineOfCalledStandIn))
        failRunOfProgramCommand(context.build, "the program or the statements of the shell called a built-in written in JavaScript that the text brings no runtimefunction line for, and got undefined: the text brings the line, or `# LIBRARY: own`"_s);
    if (compiled.outcome == ChildProcessOutcome::Crashed && !hasOwnLibraryLine(context.programText))
        context.out.println("a built-in written in JavaScript that the text brings no runtimefunction line for returns undefined, to the engine's own code too: the line `", ownLibraryLine, "` lets the engine's functions run");

    Vector<SourceFileOfTestProgram> sources = findSourcesOfTestProgram(context.programText);
    if (sources.isEmpty())
        return;
    if (!canBeRunByShellItself(sources)) {
        context.out.println("the shell cannot run the source itself");
        return;
    }
    ChildProcessResult plain = startSourceInShell(run, sources[0]);
    if (plain.output == compiled.output && plain.descriptionOfOutcome() == compiled.descriptionOfOutcome()) {
        context.out.println("the source prints the same");
        return;
    }
    context.out.println("the source prints:");
    printOutputOfShell(context, plain);
    failRunOfProgramCommand(context.build, "the compiled program and its source, run by the shell itself, do not print the same"_s);
}

static constexpr NamedImageSection namedImageSections[] {
#define AOT_NAME_IMAGE_SECTION(name, ...) { #name ""_s, ImageSection::name },
    FOR_EACH_AOT_IMAGE_SECTION(AOT_NAME_IMAGE_SECTION)
#undef AOT_NAME_IMAGE_SECTION
};

static constexpr ImageSection imageSectionsOfWords[] {
    ImageSection::FunctionStarts, ImageSection::CodeGranules, ImageSection::FrameMaps, ImageSection::SharedCodeCopies,
    ImageSection::LongStrings, ImageSection::StringConstants, ImageSection::SlotHandlers,
};

static constexpr NamedImageFunctionFlag namedImageFunctionFlags[] {
    { "HasInlineFrames"_s, ImageFunctionFlag::HasInlineFrames },
    { "HasHandlers"_s, ImageFunctionFlag::HasHandlers },
    { "HasFrameMaps"_s, ImageFunctionFlag::HasFrameMaps },
    { "HasCallSites"_s, ImageFunctionFlag::HasCallSites },
    { "HasPositions"_s, ImageFunctionFlag::HasPositions },
    { "SpillsArgumentRegisters"_s, ImageFunctionFlag::SpillsArgumentRegisters },
    { "HasConstructedCallees"_s, ImageFunctionFlag::HasConstructedCallees },
};

static const NamedImageSection* findImageSectionNamed(StringView name)
{
    for (const NamedImageSection& named : namedImageSections) {
        if (name == named.name)
            return &named;
    }
    return nullptr;
}

template<typename Element, typename Functor>
static void printElementsOfImageSection(PrintStream& out, std::span<const uint8_t> bytes, const Functor& printElement)
{
    size_t index = 0;
    for (const Element& element : spanReinterpretCast<const Element>(bytes)) {
        out.print("  ", index++, ": ");
        printElement(out, element);
        out.println();
    }
}

static void printBytesOfImageSection(PrintStream& out, std::span<const uint8_t> bytes)
{
    constexpr size_t bytesInLine = 16;
    for (size_t start = 0; start < bytes.size(); start += bytesInLine) {
        out.printf("  %06zx:", start);
        for (uint8_t byte : bytes.subspan(start, std::min(bytesInLine, bytes.size() - start)))
            out.printf(" %02x", byte);
        out.println();
    }
}

static void printImageFunctionRecord(PrintStream& out, uint32_t number, ImageFunction function)
{
    out.print("f", number, " sideTables=", function.sideTables, " flags=[");
    ASCIILiteral separator = ""_s;
    for (const NamedImageFunctionFlag& named : namedImageFunctionFlags) {
        if (!function.has(named.flag))
            continue;
        out.print(separator, named.name);
        separator = ", "_s;
    }
    out.print("]");
}

static void printImageFrameRecord(PrintStream& out, const ImageFrame& frame)
{
    out.print("size=", frame.frameSizeInBytes(), " calleeSaves=");
    out.printf("%08x", frame.calleeSaveRegisters);
    out.print(" startOfCalleeSaves=", frame.startOfCalleeSaves(), " bytesAboveFrameRecord=", frame.bytesAboveFrameRecord());
    if (!frame.hasFrameRecord())
        out.print(" without frame record");
}

static void printImageStringWord(PrintStream& out, const ImageStringWord& word)
{
    if (!word.hasText()) {
        out.print("without text");
        return;
    }
    out.print(word.hasIdentifierNumber() ? "identifier="_s : "number="_s, word.number());
    if (word.isAtom())
        out.print(" atom");
    if (word.is16Bit())
        out.print(" 16-bit");
    if (!word.hasHeader())
        out.print(" without header");
    if (word.hasListedHash())
        out.print(" with listed hash");
}

static void printImageIdentifierByte(PrintStream& out, uint8_t byte)
{
    ImageIdentifierWord word = ImageIdentifierWord::ofByteInImage(byte);
    out.print(nameOf(word.kind()));
    if (word.kind() == PropertyKeyKind::WellKnownSymbol)
        out.print(" symbol=", nameOf(word.wellKnownSymbol()));
    else
        out.print(" nameClass=", nameOf(word.nameClass()));
}

static void printStartsOfModulesInImage(PrintStream& out, const ProgramTables& tables)
{
    using Operation = ModuleStartStepData::Operation;
    std::span<const uint32_t> starts = tables.moduleStarts();
    std::span<const ModuleStartStepData> steps = tables.moduleStartSteps();
    for (size_t number = 0; number < starts.size(); ++number) {
        if (!starts[number])
            continue;
        out.print("  moduleStart ", number, ":");
        bool hasEnded = false;
        for (size_t place = starts[number]; place < steps.size() && !hasEnded; ++place) {
            ModuleStartStepData step = steps[place];
            if (place != starts[number])
                out.print(",");
            switch (step.operation()) {
            case Operation::Trap:
                out.print(" trap");
                hasEnded = true;
                break;
            case Operation::CallStart:
                out.print(" start ", step.operand());
                break;
            case Operation::CallBody:
                out.print(" body ", step.operand());
                break;
            case Operation::Begin:
                out.print(" begin m", step.operand(), step.hasFlag() ? " by the run time"_s : ""_s);
                break;
            case Operation::BeginOrReturn:
                out.print(" begin-or-return m", step.operand(), step.hasFlag() ? " by the run time"_s : ""_s);
                break;
            case Operation::Finish:
                out.print(" finish m", step.operand(), step.hasFlag() ? " unless in a record"_s : ""_s);
                break;
            case Operation::ReturnIfLoadingStops:
                out.print(" return-if-loading-stops-behind m", step.operand());
                break;
            case Operation::AbandonOnThrow:
                out.print(" abandon-on-throw m", step.operand());
                break;
            case Operation::Return:
                out.print(" return");
                hasEnded = true;
                break;
            }
        }
        out.println(hasEnded ? ""_s : " and no end"_s);
    }
}

using TokensOfRuntimeFunctions = Vector<std::pair<RuntimeFunctionID, StringView>>;

static TokensOfRuntimeFunctions collectTokensOfRuntimeFunctions(const BuiltinTable& builtins)
{
    TokensOfRuntimeFunctions tokens;
    for (const RuntimeFunctionData& function : builtins.runtimeFunctions())
        tokens.append({ function.id, tokenOf(builtins, function.id) });
    return tokens;
}

static StringView tokenOf(const TokensOfRuntimeFunctions& tokens, RuntimeFunctionID id)
{
    auto found = std::ranges::find(tokens, id, &std::pair<RuntimeFunctionID, StringView>::first);
    return found == tokens.end() ? StringView() : found->second;
}

static void printImageProgramTables(PrintStream& out, const TokensOfRuntimeFunctions& tokens, std::span<const uint8_t> bytes)
{
    if (bytes.size() < sizeof(ProgramTables)) {
        out.println("  shorter than its header");
        return;
    }
    const ProgramTables& tables = spanReinterpretCast<const ProgramTables>(bytes.first(sizeof(ProgramTables)))[0];
#define AOT_PRINT_SIZE_OF_PROGRAM_TABLE(accessor, ElementType) \
    if (size_t numberOfRows = tables.accessor().size()) \
        out.println("  " #accessor " ", numberOfRows);
    FOR_EACH_AOT_PROGRAM_TABLE(AOT_PRINT_SIZE_OF_PROGRAM_TABLE)
#undef AOT_PRINT_SIZE_OF_PROGRAM_TABLE
    for (const SourceStampData& row : tables.sourceStamps())
        out.println("  sourceStamp module=m", row.module, " low=", row.low, " high=", row.high);
    for (const CompiledRuntimeFunctionData& row : tables.compiledRuntimeFunctions())
        out.println("  compiledRuntimeFunction ", tokenOf(tokens, RuntimeFunctionID(row.id)), " function=f", row.function);
    printStartsOfModulesInImage(out, tables);
    size_t modulesWithNamedVariables = 0;
    forEachNamedVariable(decodeVarintBytes(tables.namedVariables()).span(), [&](uint32_t module, size_t numberOfVariables) {
        out.println("  moduleInHeapSnapshots place=", modulesWithNamedVariables, " module=m", module, " variables=", numberOfVariables);
        ++modulesWithNamedVariables;
    }, [&](const VariableData& row) {
        out.println("  namedVariable module=", modulesWithNamedVariables - 1, " name=s", row.name, row.slot.representation() == SlotRepresentation::DeferredClosure ? " closureOf=f"_s : " offset="_s, row.slot.offset);
    }, [&](uint32_t first, uint32_t length) {
        out.println("  closuresOfModule module=", modulesWithNamedVariables - 1, " first=", first, " length=", length);
    });
    forEachNamedCapture(decodeVarintBytes(tables.namedCaptures()).span(), [&](const NamedCaptureData& row) {
        out.println("  namedCapture closure=", static_cast<uint32_t>(row.closureNumber), " slot=", static_cast<unsigned>(row.slot), " name=s", row.name, row.isVariableOfModule ? " isVariableOfModule"_s : ""_s);
    });
}

static void printCodedTextsOfImage(PrintStream& out, const ImageHeader& header, std::span<const uint8_t> image)
{
    ImageSectionRange range = header.sections[static_cast<unsigned>(ImageSection::Strings)];
    size_t sizeOfWords = header.numbers.numberOfStringWords * sizeof(ImageStringWord);
    CodedTexts texts(image.subspan(range.offset + sizeOfWords, range.size - sizeOfWords), header.numbers.numberOfCodedTexts);
    for (uint32_t number = 0; number < texts.count(); ++number)
        out.println("  coded ", String(byteCast<Latin1Character>(texts.at(number).span())));
}

static void printTextOfImageSection(PrintStream& out, const TokensOfRuntimeFunctions& tokens, ImageSection section, std::span<const uint8_t> bytes)
{
    if (section == ImageSection::ProgramTables) {
        printImageProgramTables(out, tokens, bytes);
        return;
    }
    if (section == ImageSection::InnerEntries) {
        printElementsOfImageSection<ImageInnerEntry>(out, bytes, [](PrintStream& line, const ImageInnerEntry& entry) {
            line.print("f", entry.function, " codeOffset=", entry.codeOffset);
        });
        return;
    }
    if (section == ImageSection::Frames) {
        printElementsOfImageSection<ImageFrame>(out, bytes, printImageFrameRecord);
        return;
    }
    if (section == ImageSection::Strings) {
        printElementsOfImageSection<ImageStringWord>(out, bytes, printImageStringWord);
        return;
    }
    if (section == ImageSection::Identifiers) {
        printElementsOfImageSection<uint8_t>(out, bytes, printImageIdentifierByte);
        return;
    }
    if (section == ImageSection::StubAddresses) {
        printElementsOfImageSection<StubAddressInImage>(out, bytes, [](PrintStream& line, const StubAddressInImage& stub) {
            line.printf("key=%016llx", static_cast<unsigned long long>(stub.key));
            line.print(" offset=", stub.offset, stub.usesCallConvention ? " usesCallConvention"_s : ""_s);
        });
        return;
    }
    if (section == ImageSection::ClosureWords) {
        printElementsOfImageSection<uint64_t>(out, bytes, [](PrintStream& line, uint64_t word) {
            line.printf("%016llx", static_cast<unsigned long long>(word));
        });
        return;
    }
    if (std::ranges::contains(imageSectionsOfWords, section)) {
        printElementsOfImageSection<uint32_t>(out, bytes, [](PrintStream& line, uint32_t word) {
            line.print(word);
        });
        return;
    }
    printBytesOfImageSection(out, bytes);
}

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

static ImageSectionRange rangeOfImageSection(const ImageHeader& header, ImageSection section)
{
    return header.sections[static_cast<unsigned>(section)];
}

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

static void runImageCommand(TestCommandContext& context)
{
    Vector<NamedImageSection> printedSections;
    for (const String& word : context.arguments) {
        const NamedImageSection* named = findImageSectionNamed(word);
        if (!named) {
            context.out.println("error: no section ", word);
            return;
        }
        printedSections.append(*named);
    }

    TokensOfRuntimeFunctions tokens = collectTokensOfRuntimeFunctions(context.program->builtins());
    std::unique_ptr<CompileOutput> output = compileProgramOfCommand(context);
    std::span<const uint8_t> image = output->section(Wire::ResultSection::Image);
    if (image.size() < sizeof(ImageHeader)) {
        context.out.println("image ", image.empty() ? "empty"_s : "shorter than its header"_s);
        return;
    }

    const ImageHeader& header = spanReinterpretCast<const ImageHeader>(image.first(sizeof(ImageHeader)))[0];
    context.out.println("image ", image.size(), " bytes");
    context.out.println("code ", header.codeSize, " bytes, ", header.executableSize, " executable");
    context.out.print("flags");
    for (auto [flag, name] : namedImageFlags) {
        if (header.has(flag))
            context.out.print(" ", name);
    }
    context.out.println();
    for (const NamedImageSection& named : namedImageSections)
        context.out.println("section ", named.name, " ", rangeOfImageSection(header, named.section).size);

    for (const NamedImageSection& named : printedSections) {
        ImageSectionRange range = rangeOfImageSection(header, named.section);
        context.out.println(named.name, ":");
        if (range.offset > image.size() || range.size > image.size() - range.offset) {
            context.out.println("  outside the image");
            continue;
        }
        if (named.section == ImageSection::Functions) {
            ImageSectionRange rangeOfSideTables = rangeOfImageSection(header, ImageSection::SideTables);
            std::span<const uint8_t> sideTables = image.subspan(rangeOfSideTables.offset, rangeOfSideTables.size);
            ImageFunctions functions(spanReinterpretCast<const uint32_t>(image.subspan(range.offset, range.size)).data(), header.counts.functions);
            for (uint32_t index = 0; index < header.counts.functions; ++index) {
                context.out.print("  ", index, ": ");
                uint32_t place = functions.sideTableOf(index);
                printImageFunctionRecord(context.out, functions.numberOf(index), { place, SideTableReader(sideTables, sideTables.data() + place).next(SideTableByte::Flags) });
                context.out.println();
            }
            continue;
        }
        size_t size = named.section == ImageSection::Strings ? header.numbers.numberOfStringWords * sizeof(ImageStringWord) : named.section == ImageSection::Identifiers ? header.numbers.endOfIdentifiers : range.size;
        printTextOfImageSection(context.out, tokens, named.section, image.subspan(range.offset, size));
        if (named.section == ImageSection::Text)
            printCodedTextsOfImage(context.out, header, image);
    }
}

static void appendFunctionsMentionedBy(const CompiledCode& code, const CompileInputs& inputs, Vector<FunctionID>& functions)
{
    functions.append(code.unit.function);
    for (CodeUnit callee : code.body.callees)
        functions.append(callee.function);
    forEachUnitMentionedBy(code, inputs, [&](CodeUnit unit) {
        functions.append(unit.function);
    });
}

static bool appendFunctionsInlinedBy(CompileWorker& worker, CodeUnit unit, Vector<FunctionID>& functions)
{
    if (!worker.walkAndSweep(unit))
        return false;
    for (const InlineFrame& frame : worker.walker().code().inlineFrames()) {
        if (frame.callee)
            functions.append(frame.callee);
    }
    return true;
}

static bool codeBodiesHaveSameMentions(const CodeBody& a, const CodeBody& b)
{
    return a.mentionWords == b.mentionWords && std::ranges::equal(a.mentions, b.mentions, [](const Mention& first, const Mention& second) {
        return first.space == second.space && first.numberOfWords == second.numberOfWords && first.firstWord == second.firstWord && first.addend == second.addend;
    });
}

static bool codeBodiesHaveSameStubKeys(const CodeBody& a, const CodeBody& b)
{
    return a.stubKeys == b.stubKeys && std::ranges::equal(a.rareLists->numberedStubKeys, b.rareLists->numberedStubKeys, [](const NumberedStubKey& first, const NumberedStubKey& second) {
        return first.stubKey == second.stubKey && first.mention == second.mention;
    });
}

static bool codeBodiesHaveSameEntries(const CodeBody& a, const CodeBody& b)
{
    return std::ranges::equal(a.entries, b.entries, [](const CodeEntry& first, const CodeEntry& second) {
        return first.offset == second.offset && first.id == second.id;
    });
}

static String firstDifferenceOfCodeBodies(const CodeBody& inside, const CodeBody& alone)
{
    if (inside.bytes != alone.bytes) {
        size_t place = static_cast<size_t>(std::ranges::mismatch(inside.bytes, alone.bytes).in1 - inside.bytes.begin());
        return makeString("byte "_s, place, " of "_s, inside.bytes.size(), " inside the whole program and "_s, alone.bytes.size(), " alone"_s);
    }
    if (inside.relocations != alone.relocations) {
        size_t place = static_cast<size_t>(std::ranges::mismatch(inside.relocations, alone.relocations).in1 - inside.relocations.begin());
        return makeString("relocation "_s, place, " of "_s, inside.relocations.size(), " inside the whole program and "_s, alone.relocations.size(), " alone"_s);
    }

    std::pair<ASCIILiteral, bool> lists[] {
        { "entries"_s, codeBodiesHaveSameEntries(inside, alone) },
        { "callees"_s, inside.callees == alone.callees },
        { "stub keys"_s, codeBodiesHaveSameStubKeys(inside, alone) },
        { "argument moves"_s, inside.argumentMoves == alone.argumentMoves },
        { "constants"_s, inside.rareLists->constants == alone.rareLists->constants },
        { "jump tables"_s, inside.rareLists->jumpTables == alone.rareLists->jumpTables },
        { "mentions"_s, codeBodiesHaveSameMentions(inside, alone) },
    };
    for (auto [name, isSame] : lists) {
        if (!isSame)
            return makeString("its "_s, name);
    }
    return { };
}

static const CompiledCode* findCodeCompiledAlone(std::span<const CompiledCode> finished, CodeUnit unit)
{
    for (const CompiledCode& code : finished) {
        if (code.unit == unit)
            return &code;
    }
    return nullptr;
}

static void runSameBytesAloneCommand(TestCommandContext& context)
{
    std::optional<Vector<CodeUnit>> namedUnits;
    if (!context.arguments.empty()) {
        namedUnits = unitsNamedBy(*context.program, context.arguments, context.out);
        if (!namedUnits)
            return;
    }

    StagedBuild staged = context.stage();
    std::unique_ptr<CompileInputs> verifyingInputs = staged.prepareInputsForVerification();
    const PreparedProgram* prepared = verifyingInputs ? staged.prepared() : nullptr;
    if (!prepared)
        return;
    Program& program = staged.program();
    const CompileInputs& inputs = *prepared->compileInputs;
    const CallingConventions& conventions = prepared->plan->conventions();

    CompiledProgram whole = compileQueuedUnits(inputs, program.types, context.build);
    if (!whole.succeeded || context.build.diagnostics().hasErrors())
        return;
    auto findCodeInWholeProgram = [&](CodeUnit unit) -> const CompiledCode* {
        std::optional<uint32_t> position = whole.functions.positionOfUnit(unit, conventions.indexOf(unit));
        return position ? &whole.functions[*position] : nullptr;
    };

    Vector<CodeUnit> units;
    if (namedUnits)
        units = WTF::move(*namedUnits);
    else {
        for (const CompiledCode& code : whole.functions)
            units.append(code.unit);
    }
    std::ranges::sort(units);
    removeRepeatedElements(units);
    auto hasCodeInWholeProgram = [&](CodeUnit unit) {
        return conventions.exists(unit) && findCodeInWholeProgram(unit);
    };

    Vector<Vector<FunctionID>> functionsInlinedBy(units.size());
    {
        CompileWorker worker(inputs, context.build, program.types);
        for (size_t index = 0; index < units.size(); ++index) {
            if (hasCodeInWholeProgram(units[index]) && !appendFunctionsInlinedBy(worker, units[index], functionsInlinedBy[index]))
                return;
        }
    }

    CompileInputs inputsAlone = *verifyingInputs;
    inputsAlone.walk.options.verifiesCode = !inputs.walk.options.verifiesCode;

    bool isSameEverywhere = true;
    BodiesTakenFromProgram bodies(program);
    for (size_t index = 0; index < units.size(); ++index) {
        CodeUnit unit = units[index];
        if (!hasCodeInWholeProgram(unit)) {
            printCodeUnit(context.out, unit);
            context.out.println(" has no code in the whole program");
            continue;
        }

        CodeUnit comparedUnits[] { unit };
        std::span<const CodeUnit> compared = std::span<const CodeUnit>(comparedUnits);

        Vector<FunctionID> functionsWithBodies = WTF::move(functionsInlinedBy[index]);
        for (CodeUnit comparedUnit : compared) {
            if (const CompiledCode* inside = findCodeInWholeProgram(comparedUnit))
                appendFunctionsMentionedBy(*inside, inputs, functionsWithBodies);
        }

        auto compileAloneBeside = [&](std::span<const FunctionID> others, Vector<CompiledCode, 2>& compiled) {
            for (FunctionID function : functionsWithBodies)
                bodies.putBack(function);
            for (FunctionID function : others)
                bodies.putBack(function);
            CompileWorker workerAlone(inputsAlone, context.build, program.types);
            bool didCompile = workerAlone.compile(unit, compiled);
            for (FunctionID function : others)
                bodies.takeAgain(function);
            for (FunctionID function : functionsWithBodies)
                bodies.takeAgain(function);
            return didCompile;
        };
        Vector<CompiledCode, 2> finished;
        if (!compileAloneBeside({ }, finished))
            return;

        bool isSameAlone = true;
        for (CodeUnit comparedUnit : compared) {
            const CompiledCode* inside = findCodeInWholeProgram(comparedUnit);
            const CompiledCode* alone = findCodeCompiledAlone(finished.span(), comparedUnit);
            if (!inside && !alone)
                continue;
            printCodeUnit(context.out, comparedUnit);
            if (!inside || !alone) {
                context.out.println(" differs: it has code only ", inside ? "inside the whole program"_s : "alone"_s);
                isSameAlone = false;
                continue;
            }
            String difference = firstDifferenceOfCodeBodies(inside->body, alone->body);
            if (difference.isNull()) {
                context.out.println(" same: ", alone->body.bytes.size(), " bytes, ", alone->body.relocations.size(), " relocations");
                continue;
            }
            context.out.println(" differs: ", difference);
            isSameAlone = false;
        }
        if (isSameAlone)
            continue;
        isSameEverywhere = false;

        Vector<FunctionID> functionsWithoutBodies = bodies.functionsWithoutTheirBodies();
        functionsWithoutBodies.removeAllMatching([&](FunctionID function) {
            return functionsWithBodies.contains(function);
        });
        bool didFailToCompile = false;
        std::optional<Vector<FunctionID>> dependedOn = findBodiesThatCodeDependsOn(functionsWithoutBodies.span(), [&](std::span<const FunctionID> others) -> std::optional<bool> {
            Vector<CompiledCode, 2> compiled;
            if (!compileAloneBeside(others, compiled)) {
                didFailToCompile = true;
                return std::nullopt;
            }
            return std::ranges::all_of(compared, [&](CodeUnit comparedUnit) {
                const CompiledCode* inside = findCodeInWholeProgram(comparedUnit);
                const CompiledCode* alone = findCodeCompiledAlone(compiled.span(), comparedUnit);
                if (!inside || !alone)
                    return !inside && !alone;
                return firstDifferenceOfCodeBodies(inside->body, alone->body).isNull();
            });
        });
        if (didFailToCompile)
            return;
        if (!dependedOn) {
            printCodeUnit(context.out, unit);
            context.out.println(" differs with every body in its place too");
            continue;
        }
        IRPrinter printer(context.out, program);
        for (FunctionID function : *dependedOn) {
            printCodeUnit(context.out, unit);
            context.out.print(" depends on the body of f", function.index(), " ");
            printer.print(program.at(program.at(function).module).path);
            context.out.print(":", program.at(function).position, " ");
            printer.print(program.at(function).name);
            context.out.println(", which its code neither mentions nor inlines");
        }
    }
    if (!isSameEverywhere)
        context.build.diagnostics().add(InternalError { .kind = InternalErrorKind::UnitCompiledAloneDiffers });
}

static void runUnitsWithCodeCommand(TestCommandContext& context)
{
    std::optional<Vector<CodeUnit>> namedUnits;
    if (!context.arguments.empty()) {
        namedUnits = unitsNamedBy(*context.program, context.arguments, context.out);
        if (!namedUnits)
            return;
    }

    StagedBuild staged = context.stage();
    const PreparedProgram* prepared = staged.prepared();
    if (!prepared)
        return;
    const CallingConventions& conventions = prepared->plan->conventions();
    CompiledProgram whole = compileQueuedUnits(*prepared->compileInputs, staged.program().types, context.build);
    if (!whole.succeeded || context.build.diagnostics().hasErrors())
        return;

    auto print = [&](const CompiledCode& code) {
        context.out.print("unit "_s);
        printCodeUnit(context.out, code.unit);
        context.out.println(" ", code.body.bytes.size(), " bytes");
    };
    uint64_t numberOfBytes = 0;
    uint64_t numberOfCallEntries = 0;
    Vector<const CompiledCode*> inOrderOfUnits;
    for (const CompiledCode& code : whole.functions) {
        numberOfBytes += code.body.bytes.size();
        inOrderOfUnits.append(&code);
        numberOfCallEntries += code.unit.role == CodeRole::GenericCallEntry;
    }
    std::ranges::sort(inOrderOfUnits, { }, [](const CompiledCode* code) { return code->unit; });

    auto bytesOfFusedCallEntry = [&](CodeUnit unit) -> uint64_t {
        if (conventions.callEntryIsNumberOfModuleStart(unit.function))
            return 0;
        const CodeBody& body = whole.functions[*whole.functions.positionOfUnit(unit, conventions.indexOf(unit))].body;
        return body.primaryEntryOffset() - *body.offsetOfEntry(fusedCallEntry);
    };
    auto printFusedCallEntry = [&](CodeUnit unit) {
        context.out.print("unit "_s);
        printCodeUnit(context.out, unit);
        context.out.print(" fused into "_s);
        printCodeUnit(context.out, conventions.genericEntry(unit).target);
        context.out.println(", ", bytesOfFusedCallEntry(unit), " bytes");
    };
    auto isFusedCallEntry = [&](CodeUnit unit) {
        return std::ranges::binary_search(whole.functions.fusedCallEntries(), unit, { }, &FusedCallEntry::unit);
    };

    if (!namedUnits) {
        for (const CompiledCode* code : inOrderOfUnits)
            print(*code);
        for (const FusedCallEntry& fused : whole.functions.fusedCallEntries())
            printFusedCallEntry(fused.unit);
    } else {
        for (CodeUnit unit : *namedUnits) {
            if (isFusedCallEntry(unit)) {
                printFusedCallEntry(unit);
                continue;
            }
            std::optional<uint32_t> position = conventions.exists(unit) ? whole.functions.positionOfUnit(unit, conventions.indexOf(unit)) : std::nullopt;
            if (position) {
                print(whole.functions[*position]);
                continue;
            }
            context.out.print("unit "_s);
            printCodeUnit(context.out, unit);
            context.out.println(" has no code");
        }
    }
    context.out.println("units ", whole.functions.size(), ", bytes ", numberOfBytes);
    uint64_t numberOfBytesOfFusedCallEntries = 0;
    for (const FusedCallEntry& fused : whole.functions.fusedCallEntries())
        numberOfBytesOfFusedCallEntries += bytesOfFusedCallEntry(fused.unit);
    context.out.println("fused call entries ", whole.functions.fusedCallEntries().size(), ", bytes ", numberOfBytesOfFusedCallEntries);
    context.out.println("call entries ", numberOfCallEntries);
}

std::span<const TestCommand> programTestCommands()
{
    static constexpr TestCommand commands[] {
        { "compile"_s, runCompileCommand, ProgramOfCommand::WithLibrary, "{ statistic }"_s },
        { "run-in-shell"_s, runInShellCommand, ProgramOfCommand::WithLibrary, "{ --<option> } { word }"_s },
        { "image"_s, runImageCommand, ProgramOfCommand::WithLibrary, "[section..]"_s },
        { "same-bytes-alone"_s, runSameBytesAloneCommand, ProgramOfCommand::WithLibrary, "unit.."_s },
        { "units-with-code"_s, runUnitsWithCodeCommand, ProgramOfCommand::WithLibrary, "unit.."_s },
    };
    return commands;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
