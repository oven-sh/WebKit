/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTFFIStubs.h"
#include "AOTHeat.h"
#include "AOTImageSiteTestBodies.h"
#include "AOTImageSymbols.h"
#include "AOTImageTestBodies.h"
#include "AOTObfuscatedSites.h"
#include "AOTRegExpCompiler.h"
#include "AOTRuntimeTable.h"
#include "AOTStubsOperations.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <wtf/FileHandle.h>
#include <wtf/FileSystem.h>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringToIntegerConversion.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC::AOT::ImageTests {

using SymbolKind = ImageSymbol::Kind;
using Wire::ObfuscatedSiteRecord;

namespace {

struct DecodedSymbol {
    uint32_t offset;
    uint32_t size;
    String name;
};

} // anonymous namespace

static constexpr size_t symbolHeaderWords = 4;
static constexpr size_t wordsInSymbol = 3;

static uint32_t wordAt(std::span<const uint8_t> bytes, size_t index)
{
    uint32_t word;
    memcpy(&word, bytes.data() + index * sizeof(word), sizeof(word));
    return word;
}

static Vector<DecodedSymbol> decodeSymbols(std::span<const uint8_t> bytes)
{
    size_t count = wordAt(bytes, 0);
    size_t startOfNames = (symbolHeaderWords + count * wordsInSymbol) * sizeof(uint32_t);
    std::span<const uint8_t> names = bytes.subspan(startOfNames, wordAt(bytes, 1));
    AOT_TEST_CHECK(startOfNames + names.size() == bytes.size());
    Vector<DecodedSymbol> symbols;
    for (size_t index = 0; index < count; ++index) {
        size_t word = symbolHeaderWords + index * wordsInSymbol;
        std::span<const uint8_t> name = names.subspan(wordAt(bytes, word + 2));
        name = name.first(std::ranges::find(name, 0) - name.begin());
        symbols.append({ wordAt(bytes, word), wordAt(bytes, word + 1), String::fromUTF8(byteCast<char8_t>(name)) });
    }
    return symbols;
}

static const DecodedSymbol* symbolAt(std::span<const DecodedSymbol> symbols, uint32_t offset)
{
    auto found = std::ranges::lower_bound(symbols, offset, { }, &DecodedSymbol::offset);
    return found != symbols.end() && found->offset == offset ? &*found : nullptr;
}

static ImageSymbolInputs symbolInputs(const TestBuild& build, const TestImage& image)
{
    return { image.built, build.program(), *build.pipeline().heat, *build.pipeline().regExpRequests };
}

static Vector<ImageSymbol> symbolsOf(const TestBuild& build, const TestImage& image)
{
    return collectSymbols(symbolInputs(build, image));
}

namespace {

class SymbolImage {
public:
    SymbolImage(unsigned numberOfFunctions, Vector<CompiledCode>&& code, const TestOptions& options = { })
        : build(numberOfFunctions, options)
        , image(builtImage(build, WTF::move(code)))
    {
        symbols = symbolsOf(build, image);
        encoded = encodeSymbols(symbols.span(), image.bytes.span());
        decoded = decodeSymbols(encoded.span());
    }

    TestBuild build;
    TestImage image;
    Vector<ImageSymbol> symbols;
    Vector<uint8_t> encoded;
    Vector<DecodedSymbol> decoded;
};

} // anonymous namespace

static Vector<uint8_t> encodeWithoutKind(const SymbolImage& image, SymbolKind kind)
{
    Vector<ImageSymbol> symbols;
    for (const ImageSymbol& symbol : image.symbols) {
        if (symbol.kind != kind)
            symbols.append(symbol);
    }
    return encodeSymbols(symbols.span(), image.image.bytes.span());
}

static constexpr ASCIILiteral namedProgramText =
    "module m0 path=\"main.js\" displayPath=\"main.js\" body=f0\n"
    "entry\n"
    "function f0 name=\"main\" kind=Module results=[{type=anything}] {\n"
    "  b0():\n"
    "    v0 = Builtin builtin=GlobalThis\n"
    "    v1 = PropGet v0 key=\"sink\" [isFreeName]\n"
    "    v2 = NewClosure function=f1\n"
    "    v3 = Call v1, -, v2\n"
    "    v4 = NewClosure function=f2\n"
    "    v5 = Call v1, -, v4\n"
    "    v6 = NewClosure function=f3\n"
    "    v7 = Call v1, -, v6\n"
    "    Return\n"
    "}\n"
    "function f1 results=[{type=anything}] {\n"
    "  b0():\n"
    "    Return\n"
    "}\n"
    "function f2 name=\"caf\\u{e9}\" results=[{type=anything}] {\n"
    "  b0():\n"
    "    Return\n"
    "}\n"
    "function f3 name=\"a\\u{d800}\" results=[{type=anything}] {\n"
    "  b0():\n"
    "    Return\n"
    "}\n"_s;

static constexpr ASCIILiteral mapProgramText =
    "module m0 path=\"main.js\" displayPath=\"main.js\" body=f0\n"
    "entry\n"
    "function f0 name=\"main\" kind=Module results=[{type=anything}] {\n"
    "  b0():\n"
    "    v0 = Builtin builtin=GlobalThis\n"
    "    v1 = PropGet v0 key=\"sink\" [isFreeName]\n"
    "    v2 = NewRegExp pattern=\"a\\x09b+\\\\\\\\\"\n"
    "    v3 = Call v1, -, v2\n"
    "    Return\n"
    "}\n"_s;

static void testSymbolTable()
{
    SymbolImage mixed(numberOfMixedBodies + 1, mixedBodies(numberOfMixedBodies));
    const ImageHeader& header = mixed.image.image->header();
    std::span<const uint8_t> encoded = mixed.encoded.span();
    size_t count = mixed.decoded.size();
    AOT_TEST_CHECK(count);
    AOT_TEST_CHECK(wordAt(encoded, 0) == count);
    AOT_TEST_CHECK(wordAt(encoded, 1) == encoded.size() - (symbolHeaderWords + count * wordsInSymbol) * sizeof(uint32_t));
    AOT_TEST_CHECK(wordAt(encoded, 2) == header.codeOffset);
    AOT_TEST_CHECK(wordAt(encoded, 3) == header.size - header.codeOffset);

    for (size_t index = 0; index + 1 < count; ++index) {
        AOT_TEST_CHECK(mixed.decoded[index].offset < mixed.decoded[index + 1].offset);
        AOT_TEST_CHECK(mixed.decoded[index].offset + mixed.decoded[index].size == mixed.decoded[index + 1].offset);
    }
    AOT_TEST_CHECK(mixed.decoded.last().offset + mixed.decoded.last().size == header.codeSize);

    for (const ImageSymbol& symbol : mixed.symbols) {
        if (symbol.kind != SymbolKind::Function)
            continue;
        const DecodedSymbol* entry = symbolAt(mixed.decoded.span(), mixed.image.built.layout.bodyStarts[symbol.index]);
        AOT_TEST_CHECK(entry);
        AOT_TEST_CHECK(entry->name == symbol.name);
    }
}

static constexpr uint32_t numberOfBodiesWithStubCalls = 80;

static Vector<CompiledCode> bodiesWithStubCalls()
{
    constexpr unsigned pinnedUnitsInBody = 12;
    static_assert(numberOfBodiesWithStubCalls * pinnedUnitsInBody * wordsInPinnedUnit * sizeof(uint32_t) >= 3 * loweredSharedCodeReach);
    Vector<CompiledCode> result;
    for (uint32_t function = 1; function <= numberOfBodiesWithStubCalls; ++function) {
        CodeSketch sketch = framedSketch(function);
        for (unsigned unit = 0; unit < pinnedUnitsInBody; ++unit)
            sketch.pinned(function, wordsInPinnedUnit);
        sketch.callStub(writeBarrier(), sketch.addSite());
        sketch.end();
        result.append(sketch.take());
    }
    return result;
}

static void testSharedCodeCopiesRepeat()
{
    SymbolImage image(numberOfBodiesWithStubCalls + 1, bodiesWithStubCalls(), sharedCodeReachOptions());
    const CodeLayout& layout = image.image.built.layout;
    const Vector<uint32_t, 4>& copies = layout.sharedCodeCopies;
    AOT_TEST_CHECK(layout.sharedCodeSize < loweredSharedCodeReach);
    AOT_TEST_CHECK(copies.size() >= 2);
    AOT_TEST_CHECK(copies.size() < numberOfBodiesWithStubCalls);
    bool hasSharedCode = false;
    for (const ImageSymbol& symbol : image.symbols) {
        if (symbol.kind != SymbolKind::SharedCode)
            continue;
        hasSharedCode = true;
        for (uint32_t copy : copies)
            AOT_TEST_CHECK(symbolAt(image.decoded.span(), copy + symbol.offset));
    }
    AOT_TEST_CHECK(hasSharedCode);
}

static void testEqualNamesShare()
{
    TestBuild build(2);
    TestImage image = builtImage(build, bodies(plainBody(1)));
    Vector<ImageSymbol> symbols;
    symbols.append(ImageSymbol { SymbolKind::Function, 64, 8, 0, { }, "same"_s, { } });
    symbols.append(ImageSymbol { SymbolKind::Function, 72, 8, 1, { }, "other"_s, { } });
    symbols.append(ImageSymbol { SymbolKind::Function, 80, 8, 2, { }, "same"_s, { } });
    Vector<uint8_t> encoded = encodeSymbols(symbols.span(), image.bytes.span());
    auto nameOffsetOf = [&](size_t index) {
        return wordAt(encoded.span(), symbolHeaderWords + index * wordsInSymbol + 2);
    };
    AOT_TEST_CHECK(nameOffsetOf(0) == nameOffsetOf(2));
    AOT_TEST_CHECK(nameOffsetOf(0) != nameOffsetOf(1));
}

static void checkNameAt(std::span<const DecodedSymbol> symbols, uint32_t offset, ASCIILiteral name)
{
    const DecodedSymbol* entry = symbolAt(symbols, offset);
    AOT_TEST_CHECK(entry);
    AOT_TEST_CHECK(entry->name == name);
}

static void testWithoutSharedCode()
{
    SymbolImage mixed(numberOfMixedBodies + 1, mixedBodies(numberOfMixedBodies));
    Vector<uint8_t> encoded = encodeWithoutKind(mixed, SymbolKind::SharedCode);
    Vector<DecodedSymbol> decoded = decodeSymbols(encoded.span());
    const CodeLayout& layout = mixed.image.built.layout;
    AOT_TEST_CHECK(mixed.image.built.constants.bytes.isEmpty());
    AOT_TEST_CHECK(decoded.size() == mixed.image.built.functions.size() + layout.sharedCodeCopies.size());
    for (uint32_t copy : layout.sharedCodeCopies)
        checkNameAt(decoded.span(), copy, "(stubs of compiled code)"_s);
}

static CompiledCode bodyWithConstant(uint32_t function)
{
    CodeSketch sketch = framedSketch(function);
    sketch.callStub(writeBarrier(), sketch.addSite());
    sketch.code().body.constants.append(ConstantData { Vector<uint8_t>(FillWith { }, 8, 0x5a), 8 });
    sketch.end();
    return sketch.take();
}

static void testPartsAfterFunctions()
{
    constexpr uint32_t sizeOfPart = 32;
    TestBuild build(makeString(programText(3), "webassemblymodule module=m0\n"_s), { });
    WebAssemblyImageInputs webAssembly;
    webAssembly.code = Vector<uint8_t>(FillWith { }, sizeOfPart, 0xcc);
    webAssembly.codeAlignment = 16;
    TestImage image = build.build(bodies(plainBody(1), bodyWithConstant(2)), { }, WTF::move(webAssembly), FFIStubs { Vector<uint8_t>(FillWith { }, sizeOfPart, 0xcc), imagePageSize });
    AOT_TEST_CHECK(image.image);

    Vector<ImageSymbol> symbols = symbolsOf(build, image);
    symbols.removeAllMatching([](const ImageSymbol& symbol) {
        return symbol.kind == SymbolKind::SharedCode;
    });
    Vector<uint8_t> encoded = encodeSymbols(symbols.span(), image.bytes.span());
    Vector<DecodedSymbol> decoded = decodeSymbols(encoded.span());
    const CodeLayout& layout = image.built.layout;
    AOT_TEST_CHECK(decoded.size() == image.built.functions.size() + layout.sharedCodeCopies.size() + 3);
    checkNameAt(decoded.span(), layout.webAssemblyCodeOffset, "(WebAssembly code)"_s);
    checkNameAt(decoded.span(), layout.constantsOffset, "(constants)"_s);
    checkNameAt(decoded.span(), layout.ffiCodeOffset, "(FFI code)"_s);
    AOT_TEST_CHECK(symbolAt(decoded.span(), layout.webAssemblyCodeOffset)->size == sizeOfPart);
    AOT_TEST_CHECK(symbolAt(decoded.span(), layout.ffiCodeOffset)->size == sizeOfPart);
}

static void testNoEntry()
{
    SymbolImage mixed(numberOfMixedBodies + 1, mixedBodies(numberOfMixedBodies));
    AOT_TEST_CHECK(!mixed.image.built.foldedFunctions.isEmpty());
    for (const ImageSymbol& symbol : mixed.symbols) {
        if (symbol.kind == SymbolKind::OwnOutlinedBody)
            AOT_TEST_CHECK(!symbolAt(mixed.decoded.span(), symbol.offset));
    }

    Vector<uint8_t> encoded = encodeWithoutKind(mixed, SymbolKind::FoldedFunction);
    AOT_TEST_CHECK(decodeSymbols(encoded.span()).size() == mixed.decoded.size());

    uint32_t insideBody = mixed.image.built.layout.bodyStarts[0] + 1;
    Vector<ImageSymbol> withOwnBody = mixed.symbols;
    withOwnBody.append(ImageSymbol { SymbolKind::OwnOutlinedBody, insideBody, 1, 0, { }, "outlined"_s, { } });
    encoded = encodeSymbols(withOwnBody.span(), mixed.image.bytes.span());
    Vector<DecodedSymbol> decoded = decodeSymbols(encoded.span());
    AOT_TEST_CHECK(decoded.size() == mixed.decoded.size());
    AOT_TEST_CHECK(!symbolAt(decoded.span(), insideBody));
}

static String symbolNameOf(std::span<const ImageSymbol> symbols, FunctionID function)
{
    for (const ImageSymbol& symbol : symbols) {
        if (symbol.kind == SymbolKind::Function && symbol.function == function)
            return symbol.name;
    }
    AOT_TEST_CHECK(false);
    return { };
}

static void testSymbolNames()
{
    TestBuild build(makeString(namedProgramText, runtimeFunctionsText(4)), { });
    FunctionID entrypoint = build.genericEntrypointOf(1);
    TestImage image = builtImage(build, bodies(plainBody(0), plainBody(1), plainBody(entrypoint.index())));
    Vector<ImageSymbol> symbols = symbolsOf(build, image);
    AOT_TEST_CHECK(symbolNameOf(symbols.span(), FunctionID(0u)) == "(module) main.js"_s);
    AOT_TEST_CHECK(symbolNameOf(symbols.span(), FunctionID(1u)) == "(anonymous)"_s);
    AOT_TEST_CHECK(symbolNameOf(symbols.span(), entrypoint).endsWith(" (entry)"_s));
}

static String classLetterOf(std::span<const ImageSymbol> symbols, FunctionID function)
{
    for (const ImageSymbol& symbol : symbols) {
        if (symbol.kind == SymbolKind::Function && symbol.function == function)
            return symbol.description.substring(symbol.description.reverseFind('\t') + 1);
    }
    AOT_TEST_CHECK(false);
    return { };
}

// A small function is never compact.
static String namedProgramWithLargeFunctionText()
{
    constexpr ASCIILiteral startOfFunction = "function f1 results=[{type=anything}] {\n  b0():\n"_s;
    StringBuilder text;
    text.append(startOfFunction, "    v0 = Builtin builtin=GlobalThis\n    v1 = PropGet v0 key=\"sink\" [isFreeName]\n"_s);
    for (unsigned value = 2; value <= Typed::HeatModel::maxSizeOfSmallFunction; ++value)
        text.append("    v"_s, value, " = Call v1, -, v0\n"_s);
    return makeStringByReplacingAll(String(namedProgramText), startOfFunction, text.toString());
}

// The closures are handed to code that nobody knows, once each. Only a very large program has compact functions.
static void testClassOfCompactFunction()
{
    for (bool isVeryLarge : { false, true }) {
        TestBuild build(makeString(namedProgramWithLargeFunctionText(), runtimeFunctionsText(4)), isVeryLarge ? TestOptions { .loweredLimit = LimitID::MaxWarmCodeValues, .loweredLimitValue = 1 } : TestOptions { });
        AOT_TEST_CHECK(build.pipeline().heat->isCompact(build.program(), FunctionID(1u)) == isVeryLarge);
        AOT_TEST_CHECK(!build.pipeline().heat->isCompact(build.program(), FunctionID(0u)));
        TestImage image = builtImage(build, bodies(plainBody(0), plainBody(1)));
        Vector<ImageSymbol> symbols = symbolsOf(build, image);
        AOT_TEST_CHECK(classLetterOf(symbols.span(), FunctionID(0u)) == "o"_s);
        AOT_TEST_CHECK((classLetterOf(symbols.span(), FunctionID(1u)) == "k"_s) == isVeryLarge);
    }
}

static size_t countSharedCodeNamed(std::span<const ImageSymbol> symbols, const String& name)
{
    return std::ranges::count_if(symbols, [&](const ImageSymbol& symbol) {
        return symbol.kind == SymbolKind::SharedCode && symbol.name == name;
    });
}

static void testOperationStubName()
{
    constexpr Entry entry = Entry::operationAOTValueAdd;
    static_assert(effectsOf(entry).mayThrow);
    CodeSketch sketch = framedSketch(1);
    sketch.callStub(callOperation(OperationCall::Returns::Value, OperationCall::FirstArgument::Instance).withImmediate(offsetInRuntimeTable(entry)), sketch.addSite());
    sketch.end();
    SymbolImage image(2, bodies(sketch.take()));

    AOT_TEST_CHECK(countSharedCodeNamed(image.symbols.span(), makeString("CallOperation.ValueWithInstance to "_s, nameOf(entry))) == 1);
    AOT_TEST_CHECK(countSharedCodeNamed(image.symbols.span(), "CallOperation.ValueWithInstance"_s) == 1);
    for (const ImageSymbol& symbol : image.symbols)
        AOT_TEST_CHECK(symbol.kind != SymbolKind::SharedCode || !symbol.name.startsWith("CallOperation.ValueWithInstance#"_s));
}

static void checkMapLines(const Vector<String>& lines, const TestImage& image)
{
    size_t numberOfFunctionLines = 0;
    size_t numberOfRegExpLines = 0;
    size_t numberOfConstantLines = 0;
    HashSet<String> sharedCodeOffsets;
    Vector<String> offsetsOfCodeThatNeverReturns;
    for (const String& line : lines) {
        Vector<String> fields = line.splitAllowingEmptyEntries('\t');
        if (fields[0] == "F"_s) {
            AOT_TEST_CHECK(fields.size() == 13);
            ++numberOfFunctionLines;
        } else if (fields[0] == "T"_s) {
            AOT_TEST_CHECK(parseInteger<uint32_t>(fields[1]).value() < image.built.layout.sharedCodeSize);
            sharedCodeOffsets.add(fields[1]);
        } else if (fields[0] == "TN"_s)
            offsetsOfCodeThatNeverReturns.append(fields[1]);
        else if (fields[0] == "X"_s) {
            AOT_TEST_CHECK(fields.size() == 6);
            AOT_TEST_CHECK(fields[5] == "a\\tb+\\\\\\\\"_s);
            ++numberOfRegExpLines;
        } else if (fields[0] == "K"_s) {
            AOT_TEST_CHECK(fields.size() == 3);
            AOT_TEST_CHECK(parseInteger<uint32_t>(fields[1]).value() == image.built.layout.constantsOffset);
            AOT_TEST_CHECK(parseInteger<uint32_t>(fields[2]).value() == image.built.constants.bytes.size());
            ++numberOfConstantLines;
        }
    }
    AOT_TEST_CHECK(numberOfFunctionLines == image.built.functions.size());
    AOT_TEST_CHECK(numberOfRegExpLines);
    AOT_TEST_CHECK(numberOfConstantLines == !image.built.constants.bytes.isEmpty());
    AOT_TEST_CHECK(!offsetsOfCodeThatNeverReturns.isEmpty());
    for (const String& offset : offsetsOfCodeThatNeverReturns)
        AOT_TEST_CHECK(sharedCodeOffsets.contains(offset));
    AOT_TEST_CHECK(lines.last().startsWith("B\t"_s));
    AOT_TEST_CHECK(std::ranges::count_if(lines, [](const String& line) { return line.startsWith("B\t"_s); }) == 1);
}

static void checkMapFile(bool hasConstant)
{
    TestBuild build(makeString(mapProgramText, runtimeFunctionsText(1)), { });
    CompiledRegExps regExps = compileRegExps(build.program(), *build.pipeline().regExpRequests, build.context());
    AOT_TEST_CHECK(regExps.results.size() == 1);
    AOT_TEST_CHECK(regExps.results[0].form == RegExpRecordForm::Code);
    TestImage image = build.build(bodies(hasConstant ? bodyWithConstant(0) : plainBody(0)), WTF::move(regExps), { });
    AOT_TEST_CHECK(image.image);
    AOT_TEST_CHECK(image.built.constants.bytes.isEmpty() == !hasConstant);
    Vector<ImageSymbol> symbols = symbolsOf(build, image);

    String path = FileSystem::openTemporaryFile("aot-map"_s).first;
    AOT_TEST_CHECK(!path.isNull());
    writeMapFile(path.utf8(), symbols.span(), symbolInputs(build, image), image.bytes.span(), build.context());
    std::optional<Vector<uint8_t>> contents = FileSystem::readEntireFile(path);
    FileSystem::deleteFile(path);
    AOT_TEST_CHECK(contents);
    checkMapLines(String::fromUTF8(byteCast<char8_t>(contents->span())).split('\n'), image);
}

static void testMapFile()
{
    checkMapFile(false);
    checkMapFile(true);
}

static void testMapFileUnwritable()
{
    TestBuild build(2);
    TestImage image = builtImage(build, bodies(plainBody(1)));
    Vector<ImageSymbol> symbols = symbolsOf(build, image);
    String path = "/nonexistent-directory/map"_s;
    writeMapFile(path.utf8(), symbols.span(), symbolInputs(build, image), image.bytes.span(), build.context());
    AOT_TEST_CHECK(!FileSystem::fileExists(path));
}

static void numberSourceOffsets(CompiledCode& code)
{
    uint32_t sourceOffset = 1000 * code.function.index();
    for (SitePosition& position : code.sites.positions)
        position.sourceOffset = sourceOffset++;
}

static String nameIn(const ObfuscatedSites& sites, const ObfuscatedSiteRecord& record)
{
    return String::fromUTF8(byteCast<char8_t>(sites.names.span().subspan(record.name.offset, record.name.length)));
}

static ObfuscatedSites collectSites(TestBuild& build, Vector<CompiledCode>&& code)
{
    for (CompiledCode& body : code)
        numberSourceOffsets(body);
    return collectObfuscatedSites(createLinkedFunctions(WTF::move(code), build.context()), build.program(), build.context());
}

static void testObfuscatedSitesOneRecordPerSite()
{
    TestBuild build(3);
    CompiledCode first = bodyWithCalls(1, 3).code;
    first.sites.positions[2].source = ModuleID();
    ObfuscatedSites sites = collectSites(build, bodies(WTF::move(first), bodyWithCalls(2, 1).code));

    constexpr std::array<uint32_t, 2> numberOfSites { 4, 2 };
    AOT_TEST_CHECK(sites.records.size() == numberOfSites[0] + numberOfSites[1]);
    size_t index = 0;
    for (uint32_t position = 0; position < numberOfSites.size(); ++position) {
        for (uint32_t site = 0; site < numberOfSites[position]; ++site) {
            const ObfuscatedSiteRecord& record = sites.records[index++];
            AOT_TEST_CHECK(record.function == position);
            AOT_TEST_CHECK(record.site == site);
            AOT_TEST_CHECK(record.location.module == (!position && site == 2 ? Wire::none : 0u));
            AOT_TEST_CHECK(record.location.position == 1000 * (position + 1) + site);
            AOT_TEST_CHECK(nameIn(sites, record) == makeString('f', position + 1));
        }
    }
}

static void testObfuscatedSitesInlinedFrame()
{
    TestBuild build(5);
    SiteBody body = bodyWithInlineFrames(1, 1, chainedFrames, chainedFramesByCall);
    Vector<SitePosition> positions = body.code.sites.positions;
    ObfuscatedSites sites = collectSites(build, bodies(WTF::move(body.code)));

    AOT_TEST_CHECK(sites.records.size() == positions.size());
    bool hasSiteInInlinedFrame = false;
    for (uint32_t site = 0; site < positions.size(); ++site) {
        const ObfuscatedSiteRecord& record = sites.records[site];
        uint32_t frame = positions[site].inlineFrame;
        hasSiteInInlinedFrame |= !!frame;
        AOT_TEST_CHECK(nameIn(sites, record) == makeString('f', frame ? chainedFrames[frame - 1].callee : 1));
        AOT_TEST_CHECK(record.location.module == positions[site].source.index());
        AOT_TEST_CHECK(record.location.position == 1000 + site);
    }
    AOT_TEST_CHECK(hasSiteInInlinedFrame);
}

static void testObfuscatedSitesNamesStoredOnce()
{
    TestBuild build(5);
    constexpr std::array<unsigned, 3> framesByCall { 1, 0, 2 };
    ObfuscatedSites sites = collectSites(build, bodies(bodyWithInlineFrames(1, 0, siblingFrames, framesByCall).code, bodyWithInlineFrames(4, 0, siblingFrames, framesByCall).code));

    AOT_TEST_CHECK(sites.names.size() == 4 * strlen("f1"));
    HashMap<String, uint32_t> offsets;
    for (const ObfuscatedSiteRecord& record : sites.records) {
        AOT_TEST_CHECK(record.name.length == strlen("f1"));
        AOT_TEST_CHECK(offsets.add(nameIn(sites, record), record.name.offset).iterator->value == record.name.offset);
    }
    AOT_TEST_CHECK(offsets.size() == 4);
}

static void testObfuscatedSitesSpreadItem()
{
    CodeSketch sketch(1);
    startFramedBody(sketch, 1);
    SiteDescription& description = sketch.code().sites;
    OptionalIndex inlinedCall = sketch.addSite(0, 20, 2);
    description.inlineFrames.append(InlineFrame { 0, 0, 0, false, false });
    description.inlineFrames.append(InlineFrame { 0, *inlinedCall, sketch.calleeIndex(2), false, false });
    description.numberOfSitesWithoutCode = 1;
    OptionalIndex spreadCall = sketch.addSite(1, 30, 3);
    sketch.callStub(writeBarrier(), spreadCall);
    OptionalIndex item = sketch.addSite(0, 31, 4);
    description.spreadSites.append(SpreadSite { *spreadCall, 0, *item });
    sketch.end();

    TestBuild build(3);
    ObfuscatedSites sites = collectSites(build, bodies(sketch.take()));
    AOT_TEST_CHECK(sites.records.size() == 4);
    AOT_TEST_CHECK(nameIn(sites, sites.records[0]) == "f1"_s);
    AOT_TEST_CHECK(nameIn(sites, sites.records[*inlinedCall]) == "f1"_s);
    AOT_TEST_CHECK(nameIn(sites, sites.records[*spreadCall]) == "f2"_s);
    AOT_TEST_CHECK(nameIn(sites, sites.records[*item]) == "f2"_s);
    AOT_TEST_CHECK(sites.records[*item].location.position == 1000 + *item);
}

static void testObfuscatedSitesNames()
{
    TestBuild build(makeString(namedProgramText, runtimeFunctionsText(4)), { });
    FunctionID entrypoint = build.genericEntrypointOf(2);
    ObfuscatedSites sites = collectSites(build, bodies(plainBody(0), plainBody(1), plainBody(2), plainBody(3), plainBody(entrypoint.index())));
    constexpr size_t sitesInBody = 2;
    AOT_TEST_CHECK(sites.records.size() == 5 * sitesInBody);
    AOT_TEST_CHECK(nameIn(sites, sites.records[0]) == "main"_s);
    AOT_TEST_CHECK(sites.records[sitesInBody].name == Wire::TextRange { });
    AOT_TEST_CHECK(nameIn(sites, sites.records[2 * sitesInBody]) == String::fromUTF8("caf\xc3\xa9"));
    AOT_TEST_CHECK(nameIn(sites, sites.records[3 * sitesInBody]) == String::fromUTF8("a\xef\xbf\xbd"));
    AOT_TEST_CHECK(sites.records[4 * sitesInBody].name == sites.records[2 * sitesInBody].name);
}

std::span<const NativeTest> symbolTests()
{
    static constexpr NativeTest tests[] = {
        { "image/symbols/table"_s, testSymbolTable },
        { "image/symbols/shared-code-copies-repeat"_s, testSharedCodeCopiesRepeat },
        { "image/symbols/equal-names-share"_s, testEqualNamesShare },
        { "image/symbols/without-shared-code"_s, testWithoutSharedCode },
        { "image/symbols/parts-after-functions"_s, testPartsAfterFunctions },
        { "image/symbols/no-entry"_s, testNoEntry },
        { "image/symbols/names"_s, testSymbolNames },
        { "image/symbols/class-of-compact-function"_s, testClassOfCompactFunction },
        { "image/symbols/operation-stub-name"_s, testOperationStubName },
        { "image/symbols/map-file"_s, testMapFile },
        { "image/symbols/map-file-unwritable"_s, testMapFileUnwritable },
        { "image/obfuscated-sites/one-record-per-site"_s, testObfuscatedSitesOneRecordPerSite },
        { "image/obfuscated-sites/inlined-frame"_s, testObfuscatedSitesInlinedFrame },
        { "image/obfuscated-sites/names-stored-once"_s, testObfuscatedSitesNamesStoredOnce },
        { "image/obfuscated-sites/spread-item"_s, testObfuscatedSitesSpreadItem },
        { "image/obfuscated-sites/names"_s, testObfuscatedSitesNames },
    };
    return tests;
}

} // namespace JSC::AOT::ImageTests

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
