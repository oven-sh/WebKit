/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTFunctionTable.h"
#include "AOTFunctionTables.h"
#include "AOTImageTestSupport.h"
#include "AOTStubsBarriers.h"
#include "AOTStubsExceptions.h"
#include <algorithm>
#include <bit>
#include <limits>
#include <optional>
#include <span>
#include <wtf/MathExtras.h>
#include <wtf/WeakRandom.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC::AOT {

using namespace ImageTests;

static CodeSketch sketchWithSite(uint32_t function, unsigned frameSize)
{
    CodeSketch sketch(function);
    sketch.withFrame(frameSize);
    sketch.plain(0x1000 + function);
    sketch.callStub(writeBarrier(), sketch.addSite());
    return sketch;
}

static CompiledCode plainBody(uint32_t function, unsigned frameSize = 16)
{
    CodeSketch sketch = sketchWithSite(function, frameSize);
    sketch.end();
    return sketch.take();
}

static CompiledCode jumpToStubBody(uint32_t function)
{
    CodeSketch sketch(function);
    sketch.plain(7);
    sketch.jumpToStub(catchException());
    return sketch.take();
}

static CompiledCode wordThenJumpBody(uint32_t function, uint32_t word, uint32_t target)
{
    CodeSketch sketch(function);
    sketch.plain(word);
    sketch.jumpToFunction(target);
    return sketch.take();
}

static CompiledCode jumpOnlyBody(uint32_t function, uint32_t target)
{
    CodeSketch sketch(function);
    sketch.jumpToFunction(target);
    return sketch.take();
}

static Vector<CompiledCode> mergedBodies()
{
    return bodies(jumpToStubBody(1), jumpToStubBody(2), jumpOnlyBody(3, 4), plainBody(4), wordThenJumpBody(5, 5, 4), plainBody(6), wordThenJumpBody(7, 6, 4));
}

namespace {

struct MergedImage {
    TestBuild build { 8 };
    TestImage image { build.build(mergedBodies()) };
};

} // anonymous namespace

static const EntryAlias* findAlias(const BuiltImage& built, uint32_t function)
{
    for (const EntryAlias& alias : built.aliases) {
        if (alias.function == FunctionID(function))
            return &alias;
    }
    return nullptr;
}

static const EntryAlias& findAliasPlaced(const BuiltImage& built, AliasPlacement placement)
{
    const EntryAlias* found = nullptr;
    for (const EntryAlias& alias : built.aliases) {
        if (alias.placement != placement)
            continue;
        AOT_TEST_CHECK(!found);
        found = &alias;
    }
    AOT_TEST_CHECK(found);
    return *found;
}

static void testImageFunctionOf()
{
    MergedImage merged;
    const BuiltImage& built = merged.image.built;
    const Image& loaded = *merged.image.image;
    auto hasOwnRecord = [&](uint32_t function) {
        std::optional<uint32_t> index = loaded.imageFunctionOf(FunctionID(function));
        return index && loaded.function(*index).function() == function;
    };

    AOT_TEST_CHECK(hasOwnRecord(4));
    AOT_TEST_CHECK(hasOwnRecord(6));
    AOT_TEST_CHECK(built.foldedFunctions.size() == 1);
    uint32_t canonical = built.foldedFunctions[0].canonicalFunction.index();
    uint32_t folded = built.foldedFunctions[0].function.index();
    AOT_TEST_CHECK((canonical == 1 && folded == 2) || (canonical == 2 && folded == 1));
    AOT_TEST_CHECK(hasOwnRecord(canonical));
    AOT_TEST_CHECK(!loaded.imageFunctionOf(FunctionID(folded)));

    for (uint32_t function : { 3u, 5u, 7u }) {
        AOT_TEST_CHECK(findAlias(built, function));
        AOT_TEST_CHECK(!loaded.imageFunctionOf(FunctionID(function)));
    }
    AOT_TEST_CHECK(!loaded.imageFunctionOf(FunctionID(merged.build.program().functions.size() + 100)));
}

static void testInnerEntryAfterBody()
{
    MergedImage merged;
    const TestImage& image = merged.image;
    const Image& loaded = *image.image;
    const EntryAlias& alias = findAliasPlaced(image.built, AliasPlacement::AfterBody);
    AOT_TEST_CHECK(alias.function == FunctionID(5u) || alias.function == FunctionID(7u));
    AOT_TEST_CHECK(alias.start);

    uint32_t host = alias.body.index();
    const CodeBody& hostBody = image.built.functions[image.indexOf(host)].body;
    const uint8_t* entry = image.address(host, *alias.innerEntryOffset());

    AOT_TEST_CHECK(loaded.innerEntryContaining(entry) == alias.function);
    AOT_TEST_CHECK(loaded.innerEntryContaining(image.address(host, static_cast<uint32_t>(hostBody.bytes.size()) - 1)) == alias.function);
    AOT_TEST_CHECK(!loaded.innerEntryContaining(entry - 1));
    AOT_TEST_CHECK(!loaded.innerEntryContaining(image.address(host, hostBody.primaryEntryOffset())));
    AOT_TEST_CHECK(!loaded.innerEntryContaining(image.address(image.built.foldedFunctions[0].canonicalFunction.index(), 0)));
    AOT_TEST_CHECK(!loaded.innerEntryContaining(loaded.sharedCodeCopy(0)));
    AOT_TEST_CHECK(!loaded.innerEntryContaining(nullptr));
}

static void testInnerEntryBeforeBody()
{
    MergedImage merged;
    const TestImage& image = merged.image;
    const Image& loaded = *image.image;
    const EntryAlias& alias = findAliasPlaced(image.built, AliasPlacement::BeforeBody);
    AOT_TEST_CHECK(alias.function == FunctionID(5u) || alias.function == FunctionID(7u));
    AOT_TEST_CHECK(alias.body == FunctionID(4u));
    AOT_TEST_CHECK(!*alias.innerEntryOffset());

    const EntryAlias& appended = findAliasPlaced(image.built, AliasPlacement::AfterBody);
    const CodeBody& body = image.built.functions[image.indexOf(4)].body;
    uint32_t entryOfBody = body.primaryEntryOffset();
    uint32_t endOfOwnCode = appended.body == alias.body ? appended.start : static_cast<uint32_t>(body.bytes.size());
    AOT_TEST_CHECK(entryOfBody);

    AOT_TEST_CHECK(loaded.innerEntryContaining(image.address(4, 0)) == alias.function);
    AOT_TEST_CHECK(loaded.innerEntryContaining(image.address(4, entryOfBody - 1)) == alias.function);
    AOT_TEST_CHECK(!loaded.innerEntryContaining(image.address(4, entryOfBody)));
    AOT_TEST_CHECK(!loaded.innerEntryContaining(image.address(4, endOfOwnCode - 1)));
}

static void testInnerEntryAtPrimaryEntry()
{
    MergedImage merged;
    const EntryAlias* alias = findAlias(merged.image.built, 3);
    AOT_TEST_CHECK(alias);
    AOT_TEST_CHECK(alias->placement == AliasPlacement::AtPrimaryEntry);
    AOT_TEST_CHECK(alias->body == FunctionID(4u));
    AOT_TEST_CHECK(!alias->innerEntryOffset());
    for (const ImageInnerEntry& entry : merged.image.image->section<ImageInnerEntry>(ImageSection::InnerEntries))
        AOT_TEST_CHECK(entry.function != 3);
}

static void testInnerEntriesOfFoldedBodies()
{
    TestBuild build(5);
    TestImage image = build.build(bodies(jumpToStubBody(1), jumpToStubBody(2), wordThenJumpBody(3, 9, 1), wordThenJumpBody(4, 9, 2)));
    AOT_TEST_CHECK(image.built.foldedFunctions.size() == 1);
    uint32_t canonical = image.built.foldedFunctions[0].canonicalFunction.index();
    uint32_t bodyStart = image.built.layout.bodyStarts[image.indexOf(canonical)];
    uint32_t entryOfBody = image.built.functions[image.indexOf(canonical)].body.primaryEntryOffset();
    AOT_TEST_CHECK(entryOfBody);

    std::span<const ImageInnerEntry> entries = image.image->section<ImageInnerEntry>(ImageSection::InnerEntries);
    AOT_TEST_CHECK(entries.size() == 3);
    AOT_TEST_CHECK(entries[0].codeOffset == bodyStart && entries[0].function == 3);
    AOT_TEST_CHECK(entries[1].codeOffset == bodyStart && entries[1].function == 4);
    AOT_TEST_CHECK(entries[2].codeOffset == bodyStart + entryOfBody && entries[2].function == canonical);
}

static Vector<CompiledCode> recordBodies()
{
    CodeSketch withHandler = sketchWithSite(3, 48);
    uint32_t handlerOffset = withHandler.plain(0x2000);
    withHandler.code().sites.handlers.append(SiteHandler { 1, handlerOffset, HandlerType::Catch });
    withHandler.end();
    return bodies(plainBody(1, 32), plainBody(2, 48), withHandler.take(), jumpToStubBody(4));
}

static void testRecords()
{
    using enum ImageFunctionFlag;
    TestBuild build(5);
    TestImage image = build.build(recordBodies());
    const Image& loaded = *image.image;
    uint32_t first = image.indexOf(1);
    uint32_t second = image.indexOf(2);
    uint32_t third = image.indexOf(3);
    uint32_t frameless = image.indexOf(4);

    AOT_TEST_CHECK(loaded.frameOf(first).frameSizeInBytes() == 32);
    AOT_TEST_CHECK(loaded.frameOf(second).frameSizeInBytes() == 48);
    AOT_TEST_CHECK(loaded.frameOf(third).frameSizeInBytes() == 48);
    AOT_TEST_CHECK(!loaded.frameOf(frameless).frameSizeInBytes());
    AOT_TEST_CHECK(loaded.function(second).frame() == loaded.function(third).frame());
    AOT_TEST_CHECK(loaded.function(first).frame() != loaded.function(second).frame());

    AOT_TEST_CHECK(loaded.function(first).flags() == OptionSet<ImageFunctionFlag>({ HasCallSites, HasPositions }));
    AOT_TEST_CHECK(loaded.function(second).flags() == OptionSet<ImageFunctionFlag>({ HasCallSites, HasPositions }));
    AOT_TEST_CHECK(loaded.function(third).flags() == OptionSet<ImageFunctionFlag>({ HasCallSites, HasPositions, HasHandlers }));
    AOT_TEST_CHECK(!loaded.function(frameless).has(HasHandlers));
    AOT_TEST_CHECK(!loaded.function(frameless).has(HasFrameMaps));
    for (uint32_t function : { 1u, 2u, 3u, 4u })
        AOT_TEST_CHECK(loaded.function(image.indexOf(function)).function() == function);
}

static void testManyFrameShapes()
{
    constexpr uint32_t numberOfShapes = 5000;
    static_assert(numberOfShapes * stackAlignmentBytes() <= defaultLimitValue(LimitID::MaxFrameSizeInBytes, hostArchitecture()));

    TestBuild build(numberOfShapes + 1);
    Vector<CompiledCode> code;
    for (uint32_t function = 1; function <= numberOfShapes; ++function)
        code.append(plainBody(function, function * stackAlignmentBytes()));
    TestImage image = build.build(WTF::move(code));
    AOT_TEST_CHECK(image.image);
    AOT_TEST_CHECK(image.image->section<ImageFrame>(ImageSection::Frames).size() == numberOfShapes);
    AOT_TEST_CHECK(build.statistic(StatisticID::FrameShapesEmitted) == numberOfShapes);
    for (uint32_t function : { 1u, 4097u, numberOfShapes })
        AOT_TEST_CHECK(image.image->frameOf(image.indexOf(function)).frameSizeInBytes() == function * stackAlignmentBytes());
}

static CompiledCode bodyWithFrameMap(uint32_t function, const Vector<uint32_t>& frameMapWords)
{
    CodeSketch sketch = sketchWithSite(function, 16);
    sketch.code().sites.frameMapWords = frameMapWords;
    sketch.end();
    return sketch.take();
}

static void testFrameMaps()
{
    Vector<uint32_t> firstWords { 1, 2, 3 };
    Vector<uint32_t> secondWords { 4, 5 };

    TestBuild build(4);
    TestImage image = build.build(bodies(bodyWithFrameMap(1, firstWords), bodyWithFrameMap(2, secondWords), bodyWithFrameMap(3, { })));
    const Image& loaded = *image.image;

    AOT_TEST_CHECK(std::ranges::equal(loaded.frameMapWords(image.indexOf(1)), firstWords.span()));
    AOT_TEST_CHECK(std::ranges::equal(loaded.frameMapWords(image.indexOf(2)), secondWords.span()));
    AOT_TEST_CHECK(loaded.frameMapWords(image.indexOf(3)).empty());
}

static CompiledCode bodyWithFrameInTheMiddle(uint32_t function, CodeRange& range)
{
    CodeSketch sketch(function);
    sketch.pinned(1, 2);
    range.begin = sketch.pinned(2, 3);
    sketch.callStub(writeBarrier(), sketch.addSite());
    range.end = sketch.pinned(3, 2);
    sketch.end();
    sketch.code().frame.frameSizeInBytes = 16;
    sketch.code().frame.frameRanges.append(range);
    return sketch.take();
}

static void testHasFrameAt()
{
    TestBuild build(4);
    CodeRange range { };
    TestImage image = build.build(bodies(bodyWithFrameInTheMiddle(1, range), plainBody(2), jumpToStubBody(3)));
    const Image& loaded = *image.image;
    uint32_t index = image.indexOf(1);

    AOT_TEST_CHECK(!loaded.hasFrameAt(index, 0));
    AOT_TEST_CHECK(!loaded.hasFrameAt(index, range.begin - 1));
    AOT_TEST_CHECK(loaded.hasFrameAt(index, range.begin));
    AOT_TEST_CHECK(loaded.hasFrameAt(index, range.end - 1));
    AOT_TEST_CHECK(!loaded.hasFrameAt(index, range.end));
    AOT_TEST_CHECK(loaded.hasFrameAt(image.indexOf(2), 0));
    AOT_TEST_CHECK(!loaded.hasFrameAt(image.indexOf(3), 0));

    AOT_TEST_CHECK(!loaded.hasFrameAtReturnOffset(index, 0));
    AOT_TEST_CHECK(!loaded.hasFrameAtReturnOffset(index, range.begin));
    AOT_TEST_CHECK(loaded.hasFrameAtReturnOffset(index, range.begin + codeOffsetUnit));
    AOT_TEST_CHECK(loaded.hasFrameAtReturnOffset(index, range.end));
    AOT_TEST_CHECK(!loaded.hasFrameAtReturnOffset(index, range.end + codeOffsetUnit));
}

#if CPU(X86_64)
static void checkFrameAtFaultingAccesses(unsigned sizeOfAccess)
{
    static_assert(sizeOfNearCall == 5);
    CodeSketch sketch(1);
    uint32_t accessBeforeFrame = sketch.pinnedBytes(sizeOfAccess);
    uint32_t begin = sketch.pinnedBytes(8);
    uint32_t lastAccessInFrame = sketch.pinnedBytes(sizeOfAccess);
    uint32_t end = sketch.pinnedBytes(8);
    sketch.end();
    sketch.code().frame.frameSizeInBytes = 16;
    sketch.code().frame.frameRanges.append(CodeRange { begin, end });
    for (uint32_t access : { accessBeforeFrame, lastAccessInFrame })
        sketch.code().sites.faultingAccesses.append(FaultingAccess { access, sketch.addSite(), { 1 } });

    TestBuild build(2);
    TestImage image = build.build(bodies(sketch.take()));
    const Image& loaded = *image.image;
    uint32_t index = image.indexOf(1);
    uint32_t returnOffsetBeforeFrame = accessBeforeFrame + sizeOfNearCall;
    uint32_t returnOffsetInFrame = lastAccessInFrame + sizeOfNearCall;

    AOT_TEST_CHECK(loaded.hasFrameAt(index, returnOffsetBeforeFrame - 1));
    AOT_TEST_CHECK(!loaded.hasFrameAtReturnOffset(index, returnOffsetBeforeFrame));
    AOT_TEST_CHECK(!loaded.hasFrameAt(index, returnOffsetInFrame - 1));
    AOT_TEST_CHECK(loaded.hasFrameAtReturnOffset(index, returnOffsetInFrame));
}

static void testFrameAtFaultingAccesses()
{
    for (unsigned sizeOfAccess : { 2u, 3u, 4u })
        checkFrameAtFaultingAccesses(sizeOfAccess);
}
#endif

static std::span<const uint32_t> wordsOf(const Vector<uint8_t>& bytes)
{
    return { reinterpret_cast<const uint32_t*>(bytes.span().data()), bytes.size() / sizeof(uint32_t) };
}

static void checkGranules(const EncodedFunctionTables& tables, const CodeLayout& layout)
{
    const Vector<uint32_t>& starts = layout.bodyStarts;
    uint64_t granuleSize = 1ull << layout.codeGranuleShift;
    std::span<const uint32_t> granules = wordsOf(tables.codeGranules);
    AOT_TEST_CHECK(granules.size() == (layout.functionsEnd + granuleSize - 1) / granuleSize);
    for (uint64_t granule = 0; granule < granules.size(); ++granule) {
        uint32_t expected = 0;
        for (uint32_t position = 0; position < starts.size(); ++position) {
            if (starts[position] <= granule << layout.codeGranuleShift)
                expected = position;
        }
        AOT_TEST_CHECK(granules[granule] == expected);
    }

    std::span<const uint32_t> encodedStarts = wordsOf(tables.functionStarts);
    AOT_TEST_CHECK(encodedStarts.size() == starts.size() + 1);
    AOT_TEST_CHECK(encodedStarts.back() == std::numeric_limits<uint32_t>::max());

    std::span<const uint32_t> functionsByID = wordsOf(tables.functionsByID);
    AOT_TEST_CHECK(functionsByID.size() == starts.size());
    for (uint32_t position = 0; position < functionsByID.size(); ++position)
        AOT_TEST_CHECK(functionsByID[position] == position);
}

static void testGranules()
{
    constexpr uint32_t numberOfBodies = 200;

    TestBuild build(numberOfBodies + 1);
    Vector<CompiledCode> code;
    for (uint32_t function = 1; function <= numberOfBodies; ++function)
        code.append(plainBody(function));

    LinkedFunctions functions = createLinkedFunctions(WTF::move(code), build.context());
    CodeLayout layout;
    WeakRandom random(1);
    uint32_t start = 64;
    for (uint32_t position = 0; position < numberOfBodies; ++position) {
        layout.bodyStarts.append(start);
        start += static_cast<uint32_t>(WTF::roundUpToMultipleOf<4>(8 + random.getUint32(3993)));
    }
    layout.functionsEnd = start;

    Vector<EntryAlias> aliases;
    CodeIndex index(functions.span());
    Vector<uint32_t> zeroOffsets(FillWith { }, numberOfBodies, 0);
    Vector<uint8_t> tableBytes = build.encodeTables();
    auto& programTables = *reinterpret_cast<const ProgramTables*>(tableBytes.span().data());
    for (unsigned shift : { minCodeGranuleShift, maxCodeGranuleShift }) {
        layout.codeGranuleShift = shift;
        FunctionTableInputs inputs { functions, aliases, layout, index, zeroOffsets.span(), zeroOffsets.span(), programTables, true };
        checkGranules(encodeFunctionTables(inputs, build.context()), layout);
    }
}

static uint64_t expectedClosureWord(const Image& image, FunctionID function, uint32_t codeOffset, unsigned numberOfParameters)
{
    return ClosureWord::encode({ codeOffset, numberOfParameters }, function.index(), closureWordFlags(image.programTables().functionKinds()[function.index()]));
}

static void testClosureWords()
{
    TestBuild build(6);
    FunctionID entrypoint = build.genericEntrypointOf(2);
    Vector<CompiledCode> code;
    for (uint32_t function = 1; function <= 5; ++function)
        code.append(plainBody(function));
    CodeSketch entrypointSketch = sketchWithSite(entrypoint.index(), 16);
    entrypointSketch.code().entry.numberOfParameters = 3;
    entrypointSketch.end();
    code.append(entrypointSketch.take());
    TestImage image = build.build(WTF::move(code));

    const Image& loaded = *image.image;
    const ClosureFunctionRangeData& range = loaded.programTables().closureFunctions;
    AOT_TEST_CHECK(range.omittedWordCount == range.first);
    AOT_TEST_CHECK(range.first >= 6);

    uint64_t word = loaded.closureWord(entrypoint);
    AOT_TEST_CHECK(word);
    AOT_TEST_CHECK(word == expectedClosureWord(loaded, entrypoint, image.built.layout.bodyStarts[image.indexOf(entrypoint.index())], 3));
    AOT_TEST_CHECK(ClosureWord::functionOf(word) == entrypoint.index());
    AOT_TEST_CHECK(ClosureWord::numberOfParametersOf(word) == 3);
    AOT_TEST_CHECK(ClosureWord::addressOf(word, std::bit_cast<uintptr_t>(loaded.code().data())) == std::bit_cast<uintptr_t>(image.address(entrypoint.index(), 0)));

    AOT_TEST_CHECK(!loaded.closureWord(FunctionID(2u)));
    AOT_TEST_CHECK(!loaded.closureWord(build.genericEntrypointOf(3)));
    AOT_TEST_CHECK(!loaded.closureWord(FunctionID(range.end)));
}

static void testClosureWordOfBodyWithPrefix()
{
    constexpr uint32_t prefixSize = sizeof(uint32_t);
    TestBuild build(2);
    FunctionID entrypoint = build.genericEntrypointOf(1);
    CodeSketch sketch(entrypoint.index());
    sketch.withFrame();
    sketch.pinned(0x7777);
    sketch.pinned(1);
    sketch.callStub(writeBarrier(), sketch.addSite());
    sketch.end();
    sketch.code().body.entries[0].offset = prefixSize;
    TestImage image = build.build(bodies(sketch.take()));

    uint32_t bodyStart = image.built.layout.bodyStarts[image.indexOf(entrypoint.index())];
    AOT_TEST_CHECK(!((bodyStart + prefixSize) % imageFunctionAlignment));
    AOT_TEST_CHECK(image.image->closureWord(entrypoint) == expectedClosureWord(*image.image, entrypoint, bodyStart + prefixSize, 0));
}

static void testClosureWordsOfAliases()
{
    TestBuild build(4);
    FunctionID atPrimaryEntry = build.genericEntrypointOf(1);
    FunctionID firstWithCode = build.genericEntrypointOf(2);
    FunctionID secondWithCode = build.genericEntrypointOf(3);
    Vector<CompiledCode> code = bodies(plainBody(1), plainBody(2), jumpOnlyBody(atPrimaryEntry.index(), 1), wordThenJumpBody(firstWithCode.index(), 5, 1), wordThenJumpBody(secondWithCode.index(), 6, 1));
    code[3].entry.numberOfParameters = 1;
    code[4].entry.numberOfParameters = 2;
    TestImage image = build.build(WTF::move(code));
    const Image& loaded = *image.image;
    AOT_TEST_CHECK(image.built.aliases.size() == 3);

    for (const EntryAlias& alias : image.built.aliases) {
        uint32_t position = image.indexOf(alias.body.index());
        uint32_t entryOffset = alias.innerEntryOffset().value_or(image.built.functions[position].body.primaryEntryOffset());
        AOT_TEST_CHECK(loaded.closureWord(alias.function) == expectedClosureWord(loaded, alias.function, image.built.layout.bodyStarts[position] + entryOffset, alias.entry.numberOfParameters));
    }
    AOT_TEST_CHECK(findAliasPlaced(image.built, AliasPlacement::AtPrimaryEntry).function == atPrimaryEntry);
    const EntryAlias& before = findAliasPlaced(image.built, AliasPlacement::BeforeBody);
    const EntryAlias& after = findAliasPlaced(image.built, AliasPlacement::AfterBody);
    AOT_TEST_CHECK(before.entry.numberOfParameters + after.entry.numberOfParameters == 3);
    AOT_TEST_CHECK(ClosureWord::numberOfParametersOf(loaded.closureWord(firstWithCode)) == 1);
    AOT_TEST_CHECK(ClosureWord::numberOfParametersOf(loaded.closureWord(secondWithCode)) == 2);
}

static void testNoClosureWords()
{
    static constexpr ASCIILiteral text = "module m0 path=\"main.js\" displayPath=\"main.js\"\n"
        "entry\n"
        "function f0 name=\"unused\" results=[{type=anything}] {\n  b0():\n    Return\n}\n"_s;

    TestBuild build(String(text), { });
    TestImage image = build.build(bodies(plainBody(0)));
    AOT_TEST_CHECK(image.image->section<uint64_t>(ImageSection::ClosureWords).empty());
    AOT_TEST_CHECK(!image.image->closureWord(FunctionID(0u)));
}

std::span<const NativeTest> imageFunctionTableTests()
{
    static constexpr NativeTest tests[] = {
        { "image/functions/image-function-of"_s, testImageFunctionOf },
        { "image/functions/inner-entry-after-body"_s, testInnerEntryAfterBody },
        { "image/functions/inner-entry-before-body"_s, testInnerEntryBeforeBody },
        { "image/functions/inner-entry-at-primary-entry"_s, testInnerEntryAtPrimaryEntry },
        { "image/functions/inner-entries-of-folded-bodies"_s, testInnerEntriesOfFoldedBodies },
        { "image/functions/records"_s, testRecords },
        { "image/functions/many-frame-shapes"_s, testManyFrameShapes },
        { "image/functions/frame-maps"_s, testFrameMaps },
        { "image/functions/has-frame-at"_s, testHasFrameAt },
#if CPU(X86_64)
        { "image/functions/frame-at-faulting-accesses"_s, testFrameAtFaultingAccesses },
#endif
        { "image/functions/granules"_s, testGranules },
        { "image/functions/closure-words"_s, testClosureWords },
        { "image/functions/closure-word-of-body-with-prefix"_s, testClosureWordOfBodyWithPrefix },
        { "image/functions/closure-words-of-aliases"_s, testClosureWordsOfAliases },
        { "image/functions/no-closure-words"_s, testNoClosureWords },
    };
    return tests;
}

} // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
