/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTCodeRole.h"
#include "AOTImageBuilder.h"
#include "AOTImageSiteTestBodies.h"
#include "AOTImageTestBodies.h"
#include "AOTObfuscatedSites.h"
#include "AOTRuntimeTable.h"
#include "AOTStubBlob.h"
#include "AOTStubRecording.h"
#include "AOTStubsOperations.h"
#include "AOTVector.h"
#include "YarrFlags.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <utility>
#include <wtf/HashMap.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringToIntegerConversion.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC::AOT::ImageTests {

using Wire::ObfuscatedSiteRecord;

namespace {

struct DecodedSymbol {
    uint64_t end() const { return static_cast<uint64_t>(offset) + size; }

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

// TestBuild::build() takes the bytes out of BuiltImage::image, where encodeSymbols() reads the header and the records of regular expressions.
static Vector<uint8_t> encodeSymbolsOf(TestBuild& build, TestImage& image)
{
    image.built.image = Vector<uint8_t>(std::as_const(image.bytes).span());
    return encodeSymbols(build.program(), image.built, build.context());
}

namespace {

class SymbolImage {
public:
    SymbolImage(unsigned numberOfFunctions, Vector<CompiledCode>&& code)
        : build(numberOfFunctions)
        , image(builtImage(build, WTF::move(code)))
        , encoded(encodeSymbolsOf(build, image))
        , decoded(decodeSymbols(encoded.span()))
    {
    }

    TestBuild build;
    TestImage image;
    Vector<uint8_t> encoded;
    Vector<DecodedSymbol> decoded;
};

} // anonymous namespace

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

static CompiledCode plainEntrypointBody(CodeUnit entrypoint)
{
    CodeSketch sketch(entrypoint);
    sketch.withFrame();
    sketch.callStub(writeBarrier(), sketch.addSite());
    sketch.end();
    return sketch.take();
}

static void testSymbolTable()
{
    SymbolImage mixed(numberOfMixedBodies + 1, mixedBodies(numberOfMixedBodies));
    const ImageHeader& header = mixed.image.image->header();
    const BuiltImage& built = mixed.image.built;
    std::span<const uint8_t> encoded = mixed.encoded.span();
    std::span<const DecodedSymbol> decoded = mixed.decoded.span();
    size_t count = decoded.size();
    AOT_TEST_CHECK(count);
    AOT_TEST_CHECK(wordAt(encoded, 0) == count);
    AOT_TEST_CHECK(wordAt(encoded, 1) == encoded.size() - (symbolHeaderWords + count * wordsInSymbol) * sizeof(uint32_t));
    AOT_TEST_CHECK(wordAt(encoded, 2) == header.codeOffset);
    AOT_TEST_CHECK(wordAt(encoded, 3) == header.size - header.codeOffset);

    for (size_t index = 0; index + 1 < count; ++index) {
        uint32_t next = decoded[index + 1].offset;
        AOT_TEST_CHECK(decoded[index].offset == next || decoded[index].end() <= next);
    }
    AOT_TEST_CHECK(decoded.back().end() <= header.codeSize);

    AOT_TEST_CHECK(built.functions.size() == numberOfMixedBodies);
    for (uint32_t position = 0; position < built.functions.size(); ++position) {
        const DecodedSymbol* entry = symbolAt(decoded, built.layout.bodyStarts[position]);
        AOT_TEST_CHECK(entry);
        AOT_TEST_CHECK(entry->name.contains(makeString('f', built.functions[position].unit.function.index())));
    }

    AOT_TEST_CHECK(!built.layout.sharedCodeCopies.isEmpty());
    AOT_TEST_CHECK(!built.sharedCode.blob.placements.isEmpty());
    for (uint32_t copy : built.layout.sharedCodeCopies) {
        for (const StubPlacement& placement : built.sharedCode.blob.placements)
            AOT_TEST_CHECK(symbolAt(decoded, copy + placement.offset));
    }
}

static String symbolNameOf(std::span<const DecodedSymbol> symbols, const BuiltImage& built, CodeUnit unit)
{
    for (uint32_t position = 0; position < built.functions.size(); ++position) {
        if (built.functions[position].unit != unit)
            continue;
        const DecodedSymbol* symbol = symbolAt(symbols, built.layout.bodyStarts[position]);
        AOT_TEST_CHECK(symbol);
        return symbol->name;
    }
    AOT_TEST_CHECK(false);
    return { };
}

static void testSymbolNames()
{
    TestBuild build(makeString(namedProgramText, runtimeFunctionsText(4)), { });
    CodeUnit entrypoint = build.genericEntrypointOf(1);
    TestImage image = builtImage(build, bodies(entryOfBody(build, 0), plainBody(0), plainBody(1), plainEntrypointBody(entrypoint)));
    Vector<DecodedSymbol> symbols = decodeSymbols(encodeSymbolsOf(build, image).span());
    AOT_TEST_CHECK(symbolNameOf(symbols.span(), image.built, bodyUnitOf(0)).contains("main.js"_s));
    String nameOfBody = symbolNameOf(symbols.span(), image.built, bodyUnitOf(1));
    AOT_TEST_CHECK(!nameOfBody.isEmpty());
    AOT_TEST_CHECK(!nameOfBody.contains(nameOf(entrypoint.role)));
    AOT_TEST_CHECK(symbolNameOf(symbols.span(), image.built, entrypoint).contains(nameOf(entrypoint.role)));
}

static void testOperationStubName()
{
    constexpr Entry entry = Entry::operationAOTValueAdd;
    static_assert(effectsOf(entry).mayThrow);
    constexpr StubKey body = callOperation(OperationCall::Returns::Value, OperationCall::FirstArgument::Instance);
    constexpr StubKey head = body.withImmediate(offsetInRuntimeTable(entry));
    CodeSketch sketch = framedSketch(1);
    sketch.callStub(head, sketch.addSite());
    sketch.end();
    SymbolImage image(2, bodies(sketch.take()));

    const BuiltImage& built = image.image.built;
    std::optional<uint32_t> headOffset = built.sharedCode.blob.offsetOf(head);
    std::optional<uint32_t> bodyOffset = built.sharedCode.blob.offsetOf(body);
    AOT_TEST_CHECK(headOffset && bodyOffset && *headOffset != *bodyOffset);
    AOT_TEST_CHECK(!built.layout.sharedCodeCopies.isEmpty());
    for (uint32_t copy : built.layout.sharedCodeCopies) {
        const DecodedSymbol* headSymbol = symbolAt(image.decoded.span(), copy + *headOffset);
        const DecodedSymbol* bodySymbol = symbolAt(image.decoded.span(), copy + *bodyOffset);
        AOT_TEST_CHECK(headSymbol && bodySymbol);
        AOT_TEST_CHECK(headSymbol->name.contains(nameOf(entry)));
        AOT_TEST_CHECK(!bodySymbol->name.contains(nameOf(entry)));
    }
}

static Vector<Vector<String>> rowsOfMap(const String& map, ASCIILiteral kind)
{
    Vector<Vector<String>> rows;
    for (const String& line : map.split('\n')) {
        Vector<String> fields = line.splitAllowingEmptyEntries('\t');
        if (fields[0] == kind)
            rows.append(WTF::move(fields));
    }
    return rows;
}

static uint32_t numberInMap(const String& field)
{
    std::optional<uint32_t> number = parseInteger<uint32_t>(field);
    AOT_TEST_CHECK(number);
    return *number;
}

// The code of the regular expressions is the last of the code. A reader of a profile gives what lies outside a unit the name of the row in front of it, which was the last thunk.
static void testMapNamesCodeOfRegularExpressions()
{
    TestBuild build(2);
    Typed::Program& program = build.program();
    const uint32_t ignoreCase = OptionSet<Yarr::Flags> { Yarr::Flags::IgnoreCase }.toRaw();
    MentionList mentions;
    mentions.addPair(NumberSpace::RegExp, program.strings.intern(StringView("a+b"_s)).index(), 0);
    mentions.addPair(NumberSpace::RegExp, program.strings.intern(StringView("[0-9]+\tx"_s)).index(), ignoreCase);
    CompiledCode code = plainBody(1);
    mentions.moveTo(code.body);
    TestImage image = builtImage(build, bodies(WTF::move(code)));
    image.built.image = Vector<uint8_t>(std::as_const(image.bytes).span());
    const CodeLayout& layout = image.built.layout;
    AOT_TEST_CHECK(layout.regExpCodeOffset < layout.executableSize);

    Vector<uint8_t> bytes = encodeRowsOfMapFile(program, image.built);
    String map = String::fromUTF8(byteCast<char8_t>(bytes.span()));
    Vector<Vector<String>> bodiesInMap = rowsOfMap(map, "X"_s);
    auto hasBody = [&](ASCIILiteral width, uint32_t flags, ASCIILiteral name) {
        return bodiesInMap.containsIf([&](const Vector<String>& fields) {
            return fields.size() == 6 && fields[3] == width && numberInMap(fields[4]) == flags && fields[5] == name;
        });
    };
    AOT_TEST_CHECK(hasBody("8"_s, 0, "/a+b/"_s) && hasBody("16"_s, 0, "/a+b/"_s));
    // A row is a line, and a tab parts its fields.
    AOT_TEST_CHECK(hasBody("8"_s, ignoreCase, "/[0-9]+ x/i"_s));

    Vector<std::pair<uint32_t, uint32_t>> ranges;
    for (ASCIILiteral kind : { "X"_s, "XR"_s }) {
        for (const Vector<String>& fields : rowsOfMap(map, kind))
            ranges.append({ numberInMap(fields[1]), numberInMap(fields[2]) });
    }
    std::ranges::sort(ranges);
    AOT_TEST_CHECK(ranges.first().first == layout.regExpCodeOffset);
    for (size_t index = 0; index + 1 < ranges.size(); ++index)
        AOT_TEST_CHECK(ranges[index].second && ranges[index].first + ranges[index].second == ranges[index + 1].first);
    AOT_TEST_CHECK(ranges.last().first + ranges.last().second == layout.executableSize);
    for (const Vector<String>& fields : rowsOfMap(map, "T"_s))
        AOT_TEST_CHECK(numberInMap(fields[1]) < layout.regExpCodeOffset);
}

static void numberSourceOffsets(CompiledCode& code)
{
    uint32_t sourceOffset = 1000 * code.unit.function.index();
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
    Vector<SitePosition> positions(body.code.sites.positions.span());
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

// The entry point lies in front of the body of its function: rankInCodeOf().
static void testObfuscatedSitesNames()
{
    TestBuild build(makeString(namedProgramText, runtimeFunctionsText(4)), { });
    CodeUnit entrypoint = build.genericEntrypointOf(2);
    ObfuscatedSites sites = collectSites(build, bodies(plainBody(0), plainBody(1), plainEntrypointBody(entrypoint), plainBody(2), plainBody(3)));
    constexpr size_t sitesInBody = 2;
    AOT_TEST_CHECK(sites.records.size() == 5 * sitesInBody);
    AOT_TEST_CHECK(nameIn(sites, sites.records[0]) == "main"_s);
    AOT_TEST_CHECK(sites.records[sitesInBody].name == Wire::TextRange { });
    AOT_TEST_CHECK(nameIn(sites, sites.records[2 * sitesInBody]) == String::fromUTF8("caf\xc3\xa9"));
    AOT_TEST_CHECK(sites.records[3 * sitesInBody].name == sites.records[2 * sitesInBody].name);
    AOT_TEST_CHECK(nameIn(sites, sites.records[4 * sitesInBody]) == String::fromUTF8("a\xef\xbf\xbd"));
}

std::span<const NativeTest> symbolTests()
{
    static constexpr NativeTest tests[] = {
        { "image/symbols/table"_s, testSymbolTable },
        { "image/symbols/names"_s, testSymbolNames },
        { "image/symbols/operation-stub-name"_s, testOperationStubName },
        { "image/symbols/map-names-code-of-regular-expressions"_s, testMapNamesCodeOfRegularExpressions },
        { "image/obfuscated-sites/one-record-per-site"_s, testObfuscatedSitesOneRecordPerSite },
        { "image/obfuscated-sites/inlined-frame"_s, testObfuscatedSitesInlinedFrame },
        { "image/obfuscated-sites/names-stored-once"_s, testObfuscatedSitesNamesStoredOnce },
        { "image/obfuscated-sites/names"_s, testObfuscatedSitesNames },
    };
    return tests;
}

} // namespace JSC::AOT::ImageTests

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
