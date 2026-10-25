/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTCompileOptionWords.h"
#include "AOTIRParser.h"
#include "AOTIRPrinter.h"
#include "AOTProgramFile.h"
#include "AOTReaderFixture.h"
#include "AOTTestCommand.h"
#include "AOTTestSubcommands.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include "AOTVerifier.h"
#include "AOTWire.h"
#include <algorithm>
#include <array>
#include <wtf/ASCIICType.h>
#include <wtf/FilePrintStream.h>
#include <wtf/MathExtras.h>
#include <wtf/StringPrintStream.h>
#include <wtf/WeakRandom.h>
#include <wtf/text/StringToIntegerConversion.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

enum class InputKind : uint8_t { RandomBytes, RandomBytesAfterHeader, DamagedProgram };
enum class InputValidity : uint8_t { Unknown, Invalid };
enum class ByteDamage : uint8_t { SetWord, FlipBit, CopyWords, Truncate, TruncateAndFixSize };
enum class TextDamage : uint8_t { SetCharacter, DeleteRange, DuplicateLine, EnlargeNumber, Truncate };

struct Input {
    ProgramFile::Kind kind;
    Vector<uint8_t> bytes;
    InputValidity validity { InputValidity::Unknown };
};

struct ByteRange {
    size_t start;
    size_t length;
};

struct Fuzzer {
    explicit Fuzzer(PrintStream&);

    void addProgram(ProgramFile&&);
    void loadPrograms(const Vector<String>& paths);
    void runFixedInputs();
    void runNumberedInputs(uint32_t seed, unsigned count);
    InputKind chooseInputKind(WeakRandom&) const;
    Input createInput(uint32_t seed) const;
    Input createDamagedProgram(WeakRandom&) const;
    bool printsSameTextAfterParsing(const Program&) const;
    std::optional<ASCIILiteral> checkInput(const Input&);
    void reportFailure(ASCIILiteral label, uint32_t number, ASCIILiteral violation);

    const Wire::CompileOptions wire;
    Vector<ProgramFile> programs;
    PrintStream& out;
    unsigned inputs { 0 };
    unsigned failures { 0 };
};

} // anonymous namespace

static constexpr ASCIILiteral seedFlag = "--seed"_s;
static constexpr ASCIILiteral countFlag = "--count"_s;

static constexpr unsigned seedsPerBlock = 256;
static constexpr unsigned damagedProgramWeight = 7;
static constexpr unsigned randomBytesWeight = 1;
static constexpr unsigned maxRandomLength = 65536;
static constexpr uint32_t maxRandomSectionCount = 63;
static constexpr size_t sectionAlignment = alignof(Wire::Header);
static constexpr unsigned maxDamageSteps = 4;
static constexpr unsigned maxCopiedWords = 64;
static constexpr unsigned maxDeletedBytes = 40;

static constexpr std::array byteDamages { ByteDamage::SetWord, ByteDamage::FlipBit, ByteDamage::CopyWords, ByteDamage::Truncate, ByteDamage::TruncateAndFixSize };
static constexpr std::array textDamages { TextDamage::SetCharacter, TextDamage::DeleteRange, TextDamage::DuplicateLine, TextDamage::EnlargeNumber, TextDamage::Truncate };
static constexpr std::array largeNumbers { "4294967295"_s, "99999999999999999999"_s };

static size_t chooseIndex(WeakRandom& random, size_t size)
{
    ASSERT(size);
    return random.getUint32(static_cast<unsigned>(size));
}

template<typename T, size_t size>
static T chooseOne(WeakRandom& random, const std::array<T, size>& choices)
{
    return choices[chooseIndex(random, size)];
}

static Vector<uint8_t> createRandomBytes(WeakRandom& random, size_t length)
{
    Vector<uint8_t> bytes(length);
    std::span<uint8_t> remaining = bytes.mutableSpan();
    while (!remaining.empty()) {
        uint32_t word = random.getUint32();
        size_t chunkSize = std::min(remaining.size(), sizeof(word));
        memcpySpan(remaining, asByteSpan(word).first(chunkSize));
        remaining = remaining.subspan(chunkSize);
    }
    return bytes;
}

static void writeValidHeader(std::span<uint8_t> bytes)
{
    Wire::Header header;
    memcpySpan(asMutableByteSpan(header), bytes.first(sizeof(header)));

    header.magic = Wire::Magic;
    header.schemaHash = Wire::schemaHash;
    header.sizeInBytes = static_cast<uint32_t>(bytes.size());
    for (Wire::Section& section : header.sections) {
        section.offsetInBytes = static_cast<uint32_t>(roundDownToMultipleOf<sectionAlignment>(section.offsetInBytes % bytes.size()));
        section.count %= maxRandomSectionCount + 1;
    }

    memcpySpan(bytes, asByteSpan(header));
}

static void setHeaderSize(std::span<uint8_t> bytes)
{
    if (bytes.size() < sizeof(Wire::Header))
        return;

    uint32_t size = static_cast<uint32_t>(bytes.size());
    memcpySpan(bytes.subspan(offsetof(Wire::Header, sizeInBytes)), asByteSpan(size));
}

static std::span<uint32_t> wordsOf(Vector<uint8_t>& bytes)
{
    return spanReinterpretCast<uint32_t>(bytes.mutableSpan().first(roundDownToMultipleOf<sizeof(uint32_t)>(bytes.size())));
}

static void setWord(WeakRandom& random, std::span<uint32_t> words)
{
    if (words.empty())
        return;

    uint32_t& word = words[chooseIndex(random, words.size())];
    std::array<uint32_t, 8> replacements { 0, 1, 0x7fffffff, 0xffffffff, word + 1, word - 1, random.getUint32(), words[chooseIndex(random, words.size())] };
    word = chooseOne(random, replacements);
}

static void flipBit(WeakRandom& random, std::span<uint8_t> bytes)
{
    if (bytes.empty())
        return;

    uint8_t& byte = bytes[chooseIndex(random, bytes.size())];
    byte ^= 1 << random.getUint32(8);
}

static void copyWords(WeakRandom& random, std::span<uint32_t> words)
{
    if (words.empty())
        return;

    size_t count = std::min<size_t>(1 + random.getUint32(maxCopiedWords), words.size());
    size_t source = chooseIndex(random, words.size() - count + 1);
    size_t destination = chooseIndex(random, words.size() - count + 1);
    memmoveSpan(words.subspan(destination, count), words.subspan(source, count));
}

static void truncate(WeakRandom& random, Vector<uint8_t>& bytes)
{
    if (bytes.isEmpty())
        return;

    bytes.shrink(chooseIndex(random, bytes.size()));
}

static void applyByteDamage(WeakRandom& random, Vector<uint8_t>& bytes)
{
    switch (chooseOne(random, byteDamages)) {
    case ByteDamage::SetWord:
        setWord(random, wordsOf(bytes));
        return;
    case ByteDamage::FlipBit:
        flipBit(random, bytes.mutableSpan());
        return;
    case ByteDamage::CopyWords:
        copyWords(random, wordsOf(bytes));
        return;
    case ByteDamage::Truncate:
        truncate(random, bytes);
        return;
    case ByteDamage::TruncateAndFixSize:
        truncate(random, bytes);
        setHeaderSize(bytes.mutableSpan());
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Vector<ByteRange> findLines(std::span<const uint8_t> text)
{
    Vector<ByteRange> lines;
    size_t start = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '\n')
            continue;
        lines.append(ByteRange { start, i + 1 - start });
        start = i + 1;
    }
    if (start < text.size())
        lines.append(ByteRange { start, text.size() - start });
    return lines;
}

static Vector<ByteRange> findDigitRuns(std::span<const uint8_t> text)
{
    Vector<ByteRange> runs;
    size_t i = 0;
    while (i < text.size()) {
        size_t start = i;
        while (i < text.size() && isASCIIDigit(text[i]))
            ++i;
        if (i > start)
            runs.append(ByteRange { start, i - start });
        else
            ++i;
    }
    return runs;
}

static void setCharacter(WeakRandom& random, std::span<uint8_t> text)
{
    if (text.empty())
        return;

    constexpr unsigned printableCharacterCount = '~' - ' ' + 1;
    uint8_t& character = text[chooseIndex(random, text.size())];
    unsigned choice = random.getUint32(printableCharacterCount + 1);
    character = choice < printableCharacterCount ? static_cast<uint8_t>(' ' + choice) : '\n';
}

static void deleteRange(WeakRandom& random, Vector<uint8_t>& text)
{
    if (text.isEmpty())
        return;

    size_t length = std::min<size_t>(1 + random.getUint32(maxDeletedBytes), text.size());
    text.removeAt(chooseIndex(random, text.size() - length + 1), length);
}

static void duplicateLine(WeakRandom& random, Vector<uint8_t>& text)
{
    Vector<ByteRange> lines = findLines(text.span());
    if (lines.isEmpty())
        return;

    ByteRange source = lines[chooseIndex(random, lines.size())];
    ByteRange destination = lines[chooseIndex(random, lines.size())];
    Vector<uint8_t> line = text.subvector(source.start, source.length);
    if (line.last() != '\n')
        line.append('\n');
    text.insertVector(destination.start, line);
}

static void enlargeNumber(WeakRandom& random, Vector<uint8_t>& text)
{
    Vector<ByteRange> runs = findDigitRuns(text.span());
    if (runs.isEmpty())
        return;

    ByteRange run = runs[chooseIndex(random, runs.size())];
    text.removeAt(run.start, run.length);
    text.insertSpan(run.start, asByteSpan(chooseOne(random, largeNumbers).span()));
}

static void applyTextDamage(WeakRandom& random, Vector<uint8_t>& text)
{
    switch (chooseOne(random, textDamages)) {
    case TextDamage::SetCharacter:
        setCharacter(random, text.mutableSpan());
        return;
    case TextDamage::DeleteRange:
        deleteRange(random, text);
        return;
    case TextDamage::DuplicateLine:
        duplicateLine(random, text);
        return;
    case TextDamage::EnlargeNumber:
        enlargeNumber(random, text);
        return;
    case TextDamage::Truncate:
        truncate(random, text);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Wire::CompileOptions createCompileOptions()
{
    Wire::CompileOptions options = makeDefaultCompileOptions();
    std::optional<String> error = applyOptionWord(options, "threads=1"_s);
    RELEASE_ASSERT(!error);
    return options;
}

static std::unique_ptr<Program> readInput(const Input& input, BuildContext& build)
{
    switch (input.kind) {
    case ProgramFile::Kind::Bytes:
        return Program::read(input.bytes.span(), build);
    case ProgramFile::Kind::Text:
        return parseProgram(String::fromUTF8ReplacingInvalidSequences(input.bytes.span()), IRForm::AsRead, build);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Fuzzer::Fuzzer(PrintStream& out)
    : wire(createCompileOptions())
    , out(out)
{
}

void Fuzzer::addProgram(ProgramFile&& program)
{
    if (program.kind == ProgramFile::Kind::Text) {
        BuildContext build(wire);
        Vector<uint8_t> bytes = encodeProgramText(program.text, build);
        if (!bytes.isEmpty())
            programs.append(ProgramFile { ProgramFile::Kind::Bytes, WTF::move(bytes), { } });
    }
    programs.append(WTF::move(program));
}

void Fuzzer::loadPrograms(const Vector<String>& paths)
{
    if (paths.isEmpty()) {
        ProgramFile fixture;
        fixture.text = String(readerFixtureText);
        fixture.bytes = Vector<uint8_t>(readerFixtureText.span8());
        addProgram(WTF::move(fixture));
        return;
    }

    for (const String& path : paths) {
        if (std::optional<ProgramFile> program = readProgramFile(path, out)) {
            addProgram(WTF::move(*program));
            continue;
        }
        ++inputs;
        ++failures;
    }
}

void Fuzzer::runFixedInputs()
{
    unsigned index = 0;
    auto runInput = [&](Vector<uint8_t>&& bytes) {
        if (std::optional<ASCIILiteral> violation = checkInput({ ProgramFile::Kind::Bytes, WTF::move(bytes), InputValidity::Invalid }))
            reportFailure("fixed input"_s, index, *violation);
        ++index;
    };

    runInput({ });
    runInput(Vector<uint8_t>(FillWith { }, 1, 0));
    runInput(Vector<uint8_t>(FillWith { }, 64, 0));
    runInput(Vector<uint8_t>(FillWith { }, 4096, 0xff));

    uint64_t state = 1;
    for (size_t length = 1; length <= maxRandomLength; length *= 4) {
        runInput(Vector<uint8_t>(length, [&](size_t) {
            state = (state * 1103515245 + 12345) & 0x7fffffff;
            return static_cast<uint8_t>(state >> 8);
        }));
    }
}

void Fuzzer::runNumberedInputs(uint32_t seed, unsigned count)
{
    for (unsigned i = 0; i < count; ++i) {
        if (!(i % seedsPerBlock)) {
            unsigned lastInBlock = i + std::min(seedsPerBlock, count - i) - 1;
            out.println("seeds ", seed + i, " to ", seed + lastInBlock);
            out.flush();
        }

        uint32_t inputSeed = seed + i;
        if (std::optional<ASCIILiteral> violation = checkInput(createInput(inputSeed)))
            reportFailure("seed"_s, inputSeed, *violation);
    }
}

InputKind Fuzzer::chooseInputKind(WeakRandom& random) const
{
    if (!programs.isEmpty() && random.getUint32(damagedProgramWeight + randomBytesWeight) < damagedProgramWeight)
        return InputKind::DamagedProgram;
    return random.getUint32(2) ? InputKind::RandomBytesAfterHeader : InputKind::RandomBytes;
}

Input Fuzzer::createInput(uint32_t seed) const
{
    WeakRandom random(seed);
    switch (chooseInputKind(random)) {
    case InputKind::RandomBytes:
        return { ProgramFile::Kind::Bytes, createRandomBytes(random, random.getUint32(maxRandomLength + 1)) };
    case InputKind::RandomBytesAfterHeader: {
        Vector<uint8_t> bytes = createRandomBytes(random, sizeof(Wire::Header) + random.getUint32(maxRandomLength));
        writeValidHeader(bytes.mutableSpan());
        return { ProgramFile::Kind::Bytes, WTF::move(bytes) };
    }
    case InputKind::DamagedProgram:
        return createDamagedProgram(random);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Input Fuzzer::createDamagedProgram(WeakRandom& random) const
{
    const ProgramFile& program = programs[chooseIndex(random, programs.size())];
    Input input { program.kind, program.bytes };
    unsigned stepCount = 1 + random.getUint32(maxDamageSteps);
    for (unsigned step = 0; step < stepCount; ++step) {
        switch (input.kind) {
        case ProgramFile::Kind::Bytes:
            applyByteDamage(random, input.bytes);
            break;
        case ProgramFile::Kind::Text:
            applyTextDamage(random, input.bytes);
            break;
        }
    }
    return input;
}

bool Fuzzer::printsSameTextAfterParsing(const Program& program) const
{
    StringPrintStream text;
    printProgram(text, program, PrintedFields::WireOnly);
    String firstText = text.toString();

    BuildContext build(wire);
    std::unique_ptr<Program> parsed = parseProgram(firstText, IRForm::AsRead, build);
    if (!parsed)
        return false;
    StringPrintStream secondText;
    printProgram(secondText, *parsed, PrintedFields::WireOnly);
    return firstText == secondText.toString();
}

std::optional<ASCIILiteral> Fuzzer::checkInput(const Input& input)
{
    ++inputs;

    std::optional<ASCIILiteral> violation;
    auto addViolation = [&](ASCIILiteral description) {
        if (!violation)
            violation = description;
    };

    BuildContext build(wire);
    Diagnostics& diagnostics = build.diagnostics();
    std::unique_ptr<Program> program = readInput(input, build);
    if (!program) {
        if (!diagnostics.hasErrors())
            addViolation("rejected without a diagnostic"_s);
    } else if (!runReadStage(*program, build)) {
        if (!diagnostics.hasErrors())
            addViolation("does not verify, without a diagnostic"_s);
    } else if (!printsSameTextAfterParsing(*program))
        addViolation("the printed text does not parse to the same text"_s);

    diagnostics.finish(program.get());
    for (size_t i = 0; i < diagnostics.userDiagnostics().size(); ++i) {
        if (diagnostics.messageOfUserDiagnostic(i).isEmpty())
            addViolation("a diagnostic without text"_s);
    }
    for (size_t i = 0; i < diagnostics.internalErrors().size(); ++i) {
        if (diagnostics.messageOfInternalError(i).isEmpty())
            addViolation("a diagnostic without text"_s);
    }

    if (program && input.validity == InputValidity::Invalid)
        addViolation("accepted"_s);
    return violation;
}

void Fuzzer::reportFailure(ASCIILiteral label, uint32_t number, ASCIILiteral violation)
{
    ++failures;
    out.println("FAIL ", label, " ", number, ": ", violation);
}

unsigned fuzzReader(const ReaderFuzzRequest& request, PrintStream& out)
{
    Fuzzer fuzzer(out);
    fuzzer.loadPrograms(request.programPaths);
    fuzzer.runFixedInputs();
    fuzzer.runNumberedInputs(request.seed, request.count);
    out.println(fuzzer.inputs, " inputs, ", fuzzer.failures, " failed");
    return fuzzer.failures;
}

static StringView flagNameOf(StringView argument)
{
    size_t equalsSign = argument.find('=');
    return equalsSign == notFound ? argument : argument.left(equalsSign);
}

static std::optional<ASCIILiteral> parseNumberFlag(StringView text, std::optional<uint32_t>& value)
{
    if (value)
        return "may be specified only once"_s;

    value = parseInteger<uint32_t>(text, 10, WTF::ParseIntegerWhitespacePolicy::Disallow);
    if (!value)
        return "takes a decimal number below 4294967296"_s;
    return std::nullopt;
}

int runFuzzReaderSubcommand(std::span<const String> arguments)
{
    ReaderFuzzRequest request;
    std::optional<uint32_t> seed;
    std::optional<uint32_t> count;
    for (const String& argument : arguments) {
        StringView flagName = flagNameOf(argument);
        std::optional<uint32_t>* flagValue = nullptr;
        if (flagName == seedFlag)
            flagValue = &seed;
        else if (flagName == countFlag)
            flagValue = &count;

        if (!flagValue) {
            request.programPaths.append(argument);
            continue;
        }

        if (std::optional<ASCIILiteral> error = parseNumberFlag(StringView(argument).substring(flagName.length() + 1), *flagValue)) {
            FilePrintStream errors(stderr, FilePrintStream::Borrow);
            errors.println("testaot fuzz-reader: ", flagName, " ", *error);
            return exitCodeForInvalidCommandLine;
        }
    }
    request.seed = seed.value_or(request.seed);
    request.count = count.value_or(request.count);

    FilePrintStream out(stdout, FilePrintStream::Borrow);
    unsigned failures = 0;
    runOnBuildThread([&] {
        failures = fuzzReader(request, out);
    });
    return failures ? exitCodeForFailure : exitCodeForSuccess;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
