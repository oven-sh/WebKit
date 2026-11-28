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

std::span<const NativeTest> symbolTests()
{
    static constexpr NativeTest tests[] = {
        { "image/symbols/names"_s, testSymbolNames },
        { "image/symbols/operation-stub-name"_s, testOperationStubName },
        { "image/obfuscated-sites/names-stored-once"_s, testObfuscatedSitesNamesStoredOnce },
    };
    return tests;
}

} // namespace JSC::AOT::ImageTests

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
