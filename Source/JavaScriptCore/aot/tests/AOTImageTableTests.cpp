/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTConstantPool.h"
#include "AOTFFIStubs.h"
#include "AOTImageStamp.h"
#include "AOTImageTestBodies.h"
#include "AOTRegExpCompiler.h"
#include "AOTStubsBarriers.h"
#include "AOTStubsCallAdapters.h"
#include "AOTStubsPropertyKeys.h"
#include "AOTVector.h"
#include "CPUFeatureSet.h"
#include "MarkedBlockCage.h"
#include "RegisterSet.h"
#include <algorithm>
#include <array>
#include <initializer_list>
#include <span>
#include <utility>
#include <wtf/NeverDestroyed.h>
#include <wtf/text/MakeString.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC::AOT {

using namespace ImageTests;

using Verdict = Wire::ImageVerdict;

static constexpr uint32_t sizeOfWebAssemblyCode = 64;
static constexpr uint32_t sizeOfFFICode = 48;
static constexpr uint32_t sizeOfConstant = 8;

static constexpr ASCIILiteral regExpProgramText = "module m0 path=\"main.js\" displayPath=\"main.js\" body=f0\n"
    "entry\n"
    "function f0 name=\"main\" kind=Module results=[{type=anything}] {\n"
    "  b0():\n"
    "    v0 = Builtin builtin=GlobalThis\n"
    "    v1 = PropGet v0 key=\"sink\" [isFreeName]\n"
    "    v2 = NewRegExp pattern=\"ab+c\"\n"
    "    v3 = Call v1, -, v2\n"
    "    Return\n"
    "}\n"_s;

static const TestImage& smallImage()
{
    static NeverDestroyed<TestImage> shared([] {
        TestBuild build(4);
        return build.build(bodies(plainBody(1), plainBody(2), plainBody(3)));
    }());
    return shared.get();
}

static CompiledCode bodyWithConstant(uint32_t function)
{
    CodeSketch sketch = framedSketch(function);
    sketch.callStub(writeBarrier(), sketch.addSite());
    sketch.code().body.constants.append(ConstantData { Vector<uint8_t>(FillWith { }, sizeOfConstant, 0x5a), sizeOfConstant });
    sketch.end();
    return sketch.take();
}

static const TestImage& imageWithEveryCodePart()
{
    static NeverDestroyed<TestImage> shared([] {
        TestBuild build(makeString(regExpProgramText, runtimeFunctionsText(1)), { });
        CompiledRegExps regExps = compileRegExps(build.program(), *build.pipeline().regExpRequests, build.context());
        AOT_TEST_CHECK(regExps.results.size() == 1);
        AOT_TEST_CHECK(regExps.results[0].form == RegExpRecordForm::Code);
        WebAssemblyImageInputs webAssembly;
        webAssembly.code = Vector<uint8_t>(FillWith { }, sizeOfWebAssemblyCode, 0xcc);
        webAssembly.codeAlignment = 16;
        FFIStubs ffiStubs { Vector<uint8_t>(FillWith { }, sizeOfFFICode, 0xcc), imagePageSize };
        return build.build(bodies(bodyWithConstant(0)), WTF::move(regExps), WTF::move(webAssembly), WTF::move(ffiStubs));
    }());
    return shared.get();
}

static ImageHeader& headerOf(AlignedBytes& bytes)
{
    return *reinterpret_cast<ImageHeader*>(bytes.span().data());
}

static ImageSectionRange& rangeOf(ImageHeader& header, ImageSection section)
{
    return header.sections[static_cast<unsigned>(section)];
}

template<typename T>
static std::span<T> sectionOf(ImageHeader& header, std::span<uint8_t> bytes, ImageSection section)
{
    ImageSectionRange range = rangeOf(header, section);
    return { reinterpret_cast<T*>(bytes.data() + range.offset), range.size / sizeof(T) };
}

template<typename Change>
static Verdict verdictAfter(const TestImage& image, const Change& change)
{
    AlignedBytes copy(image.bytes.span());
    change(headerOf(copy), copy.span());
    return Image::check(copy.span(), testIdentity);
}

template<typename Change>
static void checkHeaderChange(Verdict expected, const Change& change)
{
    AOT_TEST_CHECK(verdictAfter(smallImage(), [&](ImageHeader& header, std::span<uint8_t>) {
        change(header);
    }) == expected);
}

template<typename Change>
static void checkCodePartChange(const Change& change)
{
    AOT_TEST_CHECK(verdictAfter(imageWithEveryCodePart(), [&](ImageHeader& header, std::span<uint8_t>) {
        change(header.code);
    }) == Verdict::MalformedTables);
}

template<typename Change>
static void checkProgramTablesChange(const Change& change)
{
    AOT_TEST_CHECK(verdictAfter(smallImage(), [&](ImageHeader& header, std::span<uint8_t> bytes) {
        change(*reinterpret_cast<ProgramTables*>(bytes.data() + rangeOf(header, ImageSection::ProgramTables).offset));
    }) == Verdict::MalformedTables);
}

template<typename Change>
static void checkEverySectionChange(const Change& change)
{
    for (unsigned section = 0; section < numberOfImageSections; ++section) {
        checkHeaderChange(Verdict::MalformedTables, [&](ImageHeader& header) {
            change(header, header.sections[section]);
        });
    }
}

static bool isSame(const CodeLocation& location, CodeKind kind, uint32_t function, uint32_t offset)
{
    return location.kind == kind && location.function == function && location.offset == offset;
}

static std::array<uint8_t, 16> otherIdentity()
{
    std::array<uint8_t, 16> identity = testIdentity;
    identity.back() = 2;
    return identity;
}

static void testVarintRoundTrip()
{
    struct Encoding {
        uint64_t value;
        size_t size;
    };
    static constexpr std::array<Encoding, 6> encodings { { { 0, 1 }, { 127, 1 }, { 128, 2 }, { 1ull << 32, 5 }, { 1ull << 63, 10 }, { UINT64_MAX, 10 } } };

    Vector<uint8_t> all;
    for (const Encoding& encoding : encodings) {
        Vector<uint8_t> bytes;
        appendVarint(bytes, encoding.value);
        AOT_TEST_CHECK(bytes.size() == encoding.size);
        AOT_TEST_CHECK(sizeOfVarint(encoding.value) == encoding.size);
        const uint8_t* cursor = bytes.span().data();
        AOT_TEST_CHECK(readVarint(cursor) == encoding.value);
        AOT_TEST_CHECK(cursor == bytes.span().data() + bytes.size());
        appendVarint(all, encoding.value);
    }

    const uint8_t* cursor = all.span().data();
    for (const Encoding& encoding : encodings)
        AOT_TEST_CHECK(readVarint(cursor) == encoding.value);
    AOT_TEST_CHECK(cursor == all.span().data() + all.size());
}

static void testVarintStopsAfterTenBytes()
{
    std::array<uint8_t, 11> bytes;
    bytes.fill(0xff);
    const uint8_t* cursor = bytes.data();
    readVarint(cursor);
    AOT_TEST_CHECK(cursor == bytes.data() + 10);
}

static void testZigZag()
{
    for (int64_t value : { INT64_MIN, static_cast<int64_t>(-1), static_cast<int64_t>(0), static_cast<int64_t>(1), INT64_MAX })
        AOT_TEST_CHECK(zigZagDecode(zigZagEncode(value)) == value);
    AOT_TEST_CHECK(!zigZagEncode(0));
    AOT_TEST_CHECK(zigZagEncode(-1) == 1);
    AOT_TEST_CHECK(zigZagEncode(1) == 2);
    AOT_TEST_CHECK(zigZagEncode(INT64_MIN) == UINT64_MAX);
}

static void checkSideTableParts(OptionSet<ImageFunctionFlag> flags, bool hasFrameRange)
{
    static constexpr std::array<uint8_t, 3> handlerBytes { 1, 2, 3 };
    static constexpr std::array<uint8_t, 2> callSiteBytes { 9, 9 };
    constexpr uint32_t fillerSize = 3;
    constexpr uint32_t frameStart = 2 * codeOffsetUnit;

    Vector<uint8_t> frameRanges;
    if (hasFrameRange)
        SideTableParts::appendFrameRange(frameRanges, 0, frameStart, frameStart + codeOffsetUnit);
    SideTableInputs inputs { };
    inputs.frameRanges = frameRanges.span();
    inputs.frame = 200;
    if (flags.contains(ImageFunctionFlag::HasFrameMaps))
        inputs.frameMapRange = std::pair<uint32_t, uint32_t>(7, 3);
    if (flags.contains(ImageFunctionFlag::HasHandlers))
        inputs.handlers = handlerBytes;
    if (flags.contains(ImageFunctionFlag::HasCallSites))
        inputs.callSites = callSiteBytes;

    Vector<uint8_t> stream(FillWith { }, fillerSize, 0);
    SideTableParts::append(stream, inputs);

    const uint8_t* start = stream.span().data();
    SideTableParts parts = SideTableParts::read(ImageFunction::create(fillerSize, 0, flags), start);

    AOT_TEST_CHECK(std::ranges::equal(parts.frameRanges, frameRanges.span()));
    AOT_TEST_CHECK(parts.hasFrameAt(frameStart) == hasFrameRange);
    AOT_TEST_CHECK(parts.frame == (hasFrameRange ? 200u : 0u));
    AOT_TEST_CHECK(parts.firstFrameMapWord == (inputs.frameMapRange ? 7u : 0u));
    AOT_TEST_CHECK(parts.numberOfFrameMapWords == (inputs.frameMapRange ? 3u : 0u));
    AOT_TEST_CHECK(std::ranges::equal(parts.handlers, inputs.handlers));
    if (inputs.callSites.empty()) {
        AOT_TEST_CHECK(!parts.callSites);
        return;
    }
    AOT_TEST_CHECK(parts.callSites == start + stream.size() - callSiteBytes.size());
    AOT_TEST_CHECK(*parts.callSites == 9);
}

static void testSideTableParts()
{
    using enum ImageFunctionFlag;
    static constexpr std::array groups { HasFrameMaps, HasCallSites, HasHandlers };
    for (unsigned chosen = 0; chosen < 1u << groups.size(); ++chosen) {
        OptionSet<ImageFunctionFlag> flags;
        for (unsigned group = 0; group < groups.size(); ++group)
            flags.set(groups[group], chosen >> group & 1);
        for (bool hasFrameRange : { false, true })
            checkSideTableParts(flags, hasFrameRange);
    }
}

static void testSideTablePartsWithoutGroups()
{
    static constexpr std::array<uint8_t, 2> callSites { 9, 9 };
    constexpr size_t sizeOfEmptyFrameRanges = 1;

    Vector<uint8_t> withoutAnything;
    SideTableParts::append(withoutAnything, { });
    AOT_TEST_CHECK(withoutAnything.size() == sizeOfEmptyFrameRanges);
    AOT_TEST_CHECK(!withoutAnything[0]);

    SideTableInputs inputs { };
    inputs.callSites = callSites;
    Vector<uint8_t> withoutRange;
    SideTableParts::append(withoutRange, inputs);
    AOT_TEST_CHECK(withoutRange.size() == sizeOfEmptyFrameRanges + callSites.size());

    inputs.frameMapRange = std::pair<uint32_t, uint32_t>(7, 0);
    Vector<uint8_t> withEmptyRange;
    SideTableParts::append(withEmptyRange, inputs);
    AOT_TEST_CHECK(withEmptyRange.size() == sizeOfEmptyFrameRanges + callSites.size());
}

static Vector<uint8_t> streamWithFrameRanges(std::initializer_list<CodeRange> ranges)
{
    Vector<uint8_t> frameRanges;
    uint32_t previousEnd = 0;
    for (const CodeRange& range : ranges) {
        SideTableParts::appendFrameRange(frameRanges, previousEnd, range.begin, range.end);
        previousEnd = range.end;
    }
    SideTableInputs inputs { };
    inputs.frameRanges = frameRanges.span();
    Vector<uint8_t> stream;
    SideTableParts::append(stream, inputs);
    return stream;
}

static void testFrameRanges()
{
    constexpr uint32_t unit = codeOffsetUnit;
    ImageFunction function = ImageFunction::create(0, 0, { });

    Vector<uint8_t> withoutRange = streamWithFrameRanges({ });
    SideTableParts frameless = SideTableParts::read(function, withoutRange.span().data());
    AOT_TEST_CHECK(!frameless.hasFrameAt(0));
    AOT_TEST_CHECK(!frameless.hasFrameAt(100 * unit));

    Vector<uint8_t> stream = streamWithFrameRanges({ { 2 * unit, 5 * unit }, { 5 * unit, 7 * unit }, { 9 * unit, 300 * unit } });
    SideTableParts parts = SideTableParts::read(function, stream.span().data());
    for (uint32_t offset : { 2 * unit, 5 * unit - 1, 5 * unit, 7 * unit - 1, 9 * unit, 300 * unit - 1 })
        AOT_TEST_CHECK(parts.hasFrameAt(offset));
    for (uint32_t offset : { 0u, 2 * unit - 1, 7 * unit, 8 * unit, 9 * unit - 1, 300 * unit, 1000 * unit })
        AOT_TEST_CHECK(!parts.hasFrameAt(offset));
}

static Vector<uint8_t> streamWithFrameChanges(std::initializer_list<uint32_t> changes)
{
    Vector<uint8_t> frameRanges;
    uint32_t previousChange = 0;
    for (uint32_t change : changes) {
        SideTableParts::appendFrameChange(frameRanges, previousChange, change);
        previousChange = change;
    }
    SideTableInputs inputs { };
    inputs.frameRanges = frameRanges.span();
    Vector<uint8_t> stream;
    SideTableParts::append(stream, inputs);
    return stream;
}

static void testFrameRangeWithoutEnd()
{
    constexpr uint32_t unit = codeOffsetUnit;
    ImageFunction function = ImageFunction::create(0, 0, { });

    Vector<uint8_t> fromStart = streamWithFrameChanges({ 0 });
    SideTableParts everywhere = SideTableParts::read(function, fromStart.span().data());
    AOT_TEST_CHECK(everywhere.frameRanges.size() == 1);
    for (uint32_t offset : { 0u, unit, 1000000 * unit })
        AOT_TEST_CHECK(everywhere.hasFrameAt(offset));

    Vector<uint8_t> stream = streamWithFrameChanges({ 3 * unit, 5 * unit, 9 * unit });
    SideTableParts parts = SideTableParts::read(function, stream.span().data());
    for (uint32_t offset : { 3 * unit, 5 * unit - 1, 9 * unit, 1000000 * unit })
        AOT_TEST_CHECK(parts.hasFrameAt(offset));
    for (uint32_t offset : { 0u, 3 * unit - 1, 5 * unit, 9 * unit - 1 })
        AOT_TEST_CHECK(!parts.hasFrameAt(offset));
}

static void testRegisterPacking()
{
    RegisterSet calleeSaves = RegisterSet::vmCalleeSaveRegisters();
    uint64_t allRegisters = 0;
    for (Reg reg : calleeSaves) {
        uint64_t mask = 1ull << reg.index();
        AOT_TEST_CHECK(ImageFrame::unpackRegisters(ImageFrame::packRegisters(mask)) == mask);
        allRegisters |= mask;
    }
    AOT_TEST_CHECK(allRegisters);
    AOT_TEST_CHECK(ImageFrame::unpackRegisters(ImageFrame::packRegisters(allRegisters)) == allRegisters);
}

static void testImageFrameBits()
{
    ImageFrame frame { 1, 2, 3 };
    ImageFrame otherRegisters { 4, 2, 3 };
    ImageFrame otherStart { 1, 4, 3 };
    ImageFrame otherSize { 1, 2, 4 };
    AOT_TEST_CHECK(otherRegisters.bits() != frame.bits());
    AOT_TEST_CHECK(otherStart.bits() != frame.bits());
    AOT_TEST_CHECK(otherSize.bits() != frame.bits());
}

static void testFunctionRecordWords()
{
    constexpr uint32_t sideTables = 0x12345678;
    OptionSet<ImageFunctionFlag> flags { ImageFunctionFlag::HasInlineFrames, ImageFunctionFlag::HasPositions };

    ImageFunction largest = ImageFunction::create(sideTables, ImageFunction::functionMask, flags);
    AOT_TEST_CHECK(largest.sideTables == sideTables);
    AOT_TEST_CHECK(largest.functionAndFlags == (ImageFunction::functionMask | static_cast<uint32_t>(flags.toRaw()) << ImageFunction::flagsShift));
    AOT_TEST_CHECK(largest.function() == ImageFunction::functionMask);
    AOT_TEST_CHECK(largest.flags() == flags);
    AOT_TEST_CHECK(largest.has(ImageFunctionFlag::HasPositions));
    AOT_TEST_CHECK(!largest.has(ImageFunctionFlag::HasHandlers));

    ImageFunction smallest = ImageFunction::create(0, 0, { });
    AOT_TEST_CHECK(!smallest.sideTables && !smallest.functionAndFlags);
}

static void testAccepts()
{
    AOT_TEST_CHECK(verdictAfter(smallImage(), [](ImageHeader&, std::span<uint8_t>) { }) == Verdict::Accepted);
    AOT_TEST_CHECK(verdictAfter(imageWithEveryCodePart(), [](ImageHeader&, std::span<uint8_t>) { }) == Verdict::Accepted);
}

static void testSevenBytes()
{
    AOT_TEST_CHECK(Image::check(smallImage().bytes.span().first(7), testIdentity) == Verdict::NotAnImage);
}

static void testThirtyOneBytes()
{
    AOT_TEST_CHECK(Image::check(smallImage().bytes.span().first(31), testIdentity) == Verdict::Truncated);
}

static void testHeaderCut()
{
    AOT_TEST_CHECK(Image::check(smallImage().bytes.span().first(sizeof(ImageHeader) - 1), testIdentity) == Verdict::Truncated);
}

static void testWrongMagic()
{
    checkHeaderChange(Verdict::NotAnImage, [](ImageHeader& header) { header.magic ^= 1; });
}

static void testOtherStamp()
{
    checkHeaderChange(Verdict::StampMismatch, [](ImageHeader& header) { header.stamp ^= 1; });
}

static void testSizeAboveSpan()
{
    checkHeaderChange(Verdict::Truncated, [](ImageHeader& header) { header.size += imagePageSize; });
}

static void testSizeNotWholePages()
{
    checkHeaderChange(Verdict::MalformedTables, [](ImageHeader& header) { header.size -= imageSectionAlignment; });
}

static void testMisaligned()
{
    std::span<const uint8_t> bytes = smallImage().bytes.span();
    Vector<uint8_t> zeros(FillWith { }, bytes.size() + 16, 0);
    AlignedBytes buffer(zeros.span());
    std::span<uint8_t> shifted = buffer.span().subspan(8, bytes.size());
    std::ranges::copy(bytes, shifted.begin());
    AOT_TEST_CHECK(Image::check(shifted, testIdentity) == Verdict::Misaligned);
}

static void testOtherIdentity()
{
    AOT_TEST_CHECK(Image::check(smallImage().bytes.span(), otherIdentity()) == Verdict::IdentityMismatch);
}

static void useNarrowReferences(ImageHeader& header)
{
    header.options.referenceWidth = static_cast<uint8_t>(Wire::ReferenceWidth::Narrow);
}

static void testNeedsCellCage()
{
    bool hasCellCage = MarkedBlockCage::isEnabled();
    AOT_TEST_CHECK(smallImage().image->header().options.useWideReferences());
    checkHeaderChange(hasCellCage ? Verdict::Accepted : Verdict::NeedsCellCage, useNarrowReferences);
    checkHeaderChange(hasCellCage ? Verdict::OptionsConflict : Verdict::NeedsCellCage, [](ImageHeader& header) {
        useNarrowReferences(header);
        header.heap.markedBlockCageShift += 1;
    });
    checkHeaderChange(hasCellCage ? Verdict::OptionsConflict : Verdict::NeedsCellCage, [](ImageHeader& header) {
        useNarrowReferences(header);
        header.heap.sizeClassProgression *= 2;
    });
}

static void testOtherCageShift()
{
    checkHeaderChange(Verdict::OptionsConflict, [](ImageHeader& header) { header.heap.markedBlockCageShift += 1; });
}

static void testOtherImageForm()
{
    checkHeaderChange(Verdict::OptionsConflict, [](ImageHeader& header) { header.options.imageForm ^= 1; });
}

static void testOtherSizeClasses()
{
    checkHeaderChange(Verdict::OptionsConflict, [](ImageHeader& header) { header.heap.sizeClassProgression *= 2; });
}

static void testUnalignedCode()
{
    checkHeaderChange(Verdict::MalformedTables, [](ImageHeader& header) { header.codeOffset += 16; });
}

static void testCodePastEnd()
{
    checkHeaderChange(Verdict::MalformedTables, [](ImageHeader& header) { header.codeSize = header.size; });
}

static void testSectionOffsetUnaligned()
{
    checkEverySectionChange([](ImageHeader&, ImageSectionRange& range) { range.offset += 8; });
}

static void testSectionBeforePreviousEnd()
{
    checkEverySectionChange([](ImageHeader&, ImageSectionRange& range) { range.offset = 0; });
}

static void testSectionPastCode()
{
    checkEverySectionChange([](ImageHeader& header, ImageSectionRange& range) { range.size = static_cast<uint32_t>(header.codeOffset); });
}

static void testSectionSizeNotWholeRecords()
{
    using enum ImageSection;
    static constexpr std::array sections {
        Functions, FunctionStarts, CodeGranules, InnerEntries, Frames, FrameMaps, ClosureWords, SharedCodeCopies,
        Strings, LongStrings, Identifiers, StringConstants, RegExps, RegExpHashes, StubAddresses, StubReturnSites,
    };
    for (ImageSection section : sections) {
        checkHeaderChange(Verdict::MalformedTables, [&](ImageHeader& header) {
            rangeOf(header, section).size += 1;
        });
    }
}

static void testFunctionCount()
{
    checkHeaderChange(Verdict::MalformedTables, [](ImageHeader& header) { header.counts.functions += 1; });
}

static void testFunctionsPastCode()
{
    checkHeaderChange(Verdict::MalformedTables, [](ImageHeader& header) { header.code.functionsEnd = static_cast<uint32_t>(header.codeSize) + 1; });
}

static void testTooFewGranules()
{
    checkHeaderChange(Verdict::MalformedTables, [](ImageHeader& header) { rangeOf(header, ImageSection::CodeGranules).size = 0; });
}

static void testGranuleShift()
{
    checkHeaderChange(Verdict::MalformedTables, [](ImageHeader& header) { header.code.codeGranuleShift = 32; });
}

static void testSharedCodeCopyPastFunctions()
{
    AOT_TEST_CHECK(verdictAfter(smallImage(), [](ImageHeader& header, std::span<uint8_t> bytes) {
        sectionOf<uint32_t>(header, bytes, ImageSection::SharedCodeCopies)[0] = header.code.functionsEnd + 16;
    }) == Verdict::MalformedTables);
}

static void testSharedCodeEndsPastFunctions()
{
    checkHeaderChange(Verdict::MalformedTables, [](ImageHeader& header) { header.code.sharedCodeSize = header.code.functionsEnd + 1; });
}

static void testSharedCodeCopiesOverlap()
{
    static_assert(2 * sizeof(uint32_t) <= imageSectionAlignment);
    AOT_TEST_CHECK(smallImage().image->sharedCodeCopies().size() == 1);
    AOT_TEST_CHECK(verdictAfter(smallImage(), [](ImageHeader& header, std::span<uint8_t> bytes) {
        rangeOf(header, ImageSection::SharedCodeCopies).size += sizeof(uint32_t);
        std::span<uint32_t> copies = sectionOf<uint32_t>(header, bytes, ImageSection::SharedCodeCopies);
        copies[1] = copies[0] + header.code.sharedCodeSize - 1;
    }) == Verdict::MalformedTables);
}

static void testRegExpCodeBeforeEndOfFunctions()
{
    checkCodePartChange([](ImageCodeRanges& code) { code.regExpCodeOffset = code.functionsEnd - 1; });
}

static void testWebAssemblyCodeBeforeRegExpCode()
{
    checkCodePartChange([](ImageCodeRanges& code) { code.webAssemblyCodeOffset = code.regExpCodeOffset - 1; });
}

static void testWebAssemblyCodePastConstants()
{
    checkCodePartChange([](ImageCodeRanges& code) { code.webAssemblyCodeSize = code.constantsOffset - code.webAssemblyCodeOffset + 1; });
}

static void testConstantsPastFFICode()
{
    checkCodePartChange([](ImageCodeRanges& code) { code.constantsSize = code.ffiCodeOffset - code.constantsOffset + 1; });
}

static void testFFICodePastEnd()
{
    checkCodePartChange([](ImageCodeRanges& code) { code.ffiCodeSize += 1; });
}

static void testFFICodeOffPageBoundary()
{
    AOT_TEST_CHECK(!(imageWithEveryCodePart().image->header().code.ffiCodeOffset % imagePageSize));
    checkCodePartChange([](ImageCodeRanges& code) {
        code.ffiCodeOffset += 16;
        code.ffiCodeSize -= 16;
    });
}

static void testProgramTablesMagic()
{
    checkProgramTablesChange([](ProgramTables& tables) { tables.magic ^= 1; });
}

static void testProgramTablesCut()
{
    checkHeaderChange(Verdict::MalformedTables, [](ImageHeader& header) { rangeOf(header, ImageSection::ProgramTables).size = sizeof(ProgramTables) - 1; });
}

static void testClosureFunctionRange()
{
    checkProgramTablesChange([](ProgramTables& tables) { tables.closureFunctions.omittedWordCount = tables.closureFunctions.first + 1; });
    checkProgramTablesChange([](ProgramTables& tables) { tables.closureFunctions.first = tables.closureFunctions.end + 1; });
    checkProgramTablesChange([](ProgramTables& tables) { tables.closureFunctions.end += 1; });
}

static void testStringCellCount()
{
    checkProgramTablesChange([](ProgramTables& tables) { tables.instanceSizes.numberOfStringCells += 1; });
}

static void testNoCode()
{
    checkHeaderChange(Verdict::NoCode, [](ImageHeader& header) {
        header.codeSize = 0;
        header.code = { };
        rangeOf(header, ImageSection::SharedCodeCopies).size = 0;
    });
}

static bool isInSharedCode(const Image& image, uint64_t offset)
{
    return std::ranges::any_of(image.sharedCodeCopies(), [&](uint32_t copy) {
        return offset >= copy && offset - copy < image.header().code.sharedCodeSize;
    });
}

static void testFunctionBytes()
{
    const TestImage& image = smallImage();
    const Image& loaded = *image.image;
    for (uint32_t function : { 1u, 2u, 3u }) {
        uint32_t index = image.indexOf(function);
        uint32_t size = static_cast<uint32_t>(image.built.functions[index].body.bytes.size());
        AOT_TEST_CHECK(isSame(loaded.locate(image.address(function, 0)), CodeKind::Function, index, 0));
        AOT_TEST_CHECK(isSame(loaded.locate(image.address(function, size - 1)), CodeKind::Function, index, size - 1));
        AOT_TEST_CHECK(loaded.locate(image.address(function, 0)).runsCompiledCode());
        AOT_TEST_CHECK(loaded.codeOf(index) == loaded.code().data() + image.built.layout.bodyStarts[index]);
        AOT_TEST_CHECK(loaded.sizeOfCodeOf(index) >= size);
        AOT_TEST_CHECK(loaded.rootFunctionOf(index) == FunctionID(function));
    }
}

static void testEveryByte()
{
    const Image& loaded = *smallImage().image;
    const ImageHeader& header = loaded.header();
    std::span<const uint32_t> starts = loaded.section<uint32_t>(ImageSection::FunctionStarts).first(header.counts.functions);
    for (uint64_t offset = starts[0]; offset < header.code.functionsEnd; ++offset) {
        if (isInSharedCode(loaded, offset))
            continue;
        uint32_t expected = 0;
        for (uint32_t function = 0; function < starts.size(); ++function) {
            if (starts[function] <= offset)
                expected = function;
        }
        AOT_TEST_CHECK(isSame(loaded.locate(loaded.code().data() + offset), CodeKind::Function, expected, static_cast<uint32_t>(offset - starts[expected])));
    }
}

static void testStubBytes()
{
    const Image& loaded = *smallImage().image;
    uint32_t sharedCodeSize = loaded.header().code.sharedCodeSize;
    AOT_TEST_CHECK(!loaded.sharedCodeCopies().empty());
    AOT_TEST_CHECK(sharedCodeSize);
    for (unsigned copy = 0; copy < loaded.sharedCodeCopies().size(); ++copy) {
        AOT_TEST_CHECK(isSame(loaded.locate(loaded.sharedCodeCopy(copy)), CodeKind::Stub, 0, 0));
        AOT_TEST_CHECK(isSame(loaded.locate(loaded.sharedCodeCopy(copy) + sharedCodeSize - 1), CodeKind::Stub, 0, sharedCodeSize - 1));
        AOT_TEST_CHECK(loaded.locate(loaded.sharedCodeCopy(copy)).runsCompiledCode());
    }
}

static void testStubReturnSites()
{
    const Image& loaded = *smallImage().image;
    std::span<const StubReturnSite> sites = loaded.stubReturnSites();
    const uint8_t* copy = loaded.sharedCodeCopy(0);
    AOT_TEST_CHECK(!sites.empty());
    for (size_t index = 0; index < sites.size(); ++index) {
        const StubReturnSite& site = sites[index];
        AOT_TEST_CHECK(!index || sites[index - 1].offset < site.offset);
        CodeLocation location = loaded.locate(copy + site.offset);
        AOT_TEST_CHECK(location.kind == CodeKind::Stub);
        AOT_TEST_CHECK(location.stubFrameKind == site.kind);
        bool nextSiteFollows = index + 1 < sites.size() && sites[index + 1].offset == site.offset + 1;
        if (!nextSiteFollows)
            AOT_TEST_CHECK(loaded.locate(copy + site.offset + 1).stubFrameKind == StubFrameKind::Plain);
    }
}

static void testOutside()
{
    const Image& loaded = *smallImage().image;
    const ImageHeader& header = loaded.header();
    const uint8_t* code = loaded.code().data();
    AOT_TEST_CHECK(loaded.locate(code - 1).kind == CodeKind::NotInImage);
    AOT_TEST_CHECK(loaded.locate(code + header.codeSize).kind == CodeKind::NotInImage);
    AOT_TEST_CHECK(loaded.locate(nullptr).kind == CodeKind::NotInImage);
    AOT_TEST_CHECK(!loaded.locate(nullptr).runsCompiledCode());
    for (uint64_t offset = header.code.functionsEnd; offset < header.codeSize; ++offset)
        AOT_TEST_CHECK(loaded.locate(code + offset).kind == CodeKind::NotInImage);
}

static void checkCodePart(const Image& image, CodeKind kind, std::span<const uint8_t> part)
{
    AOT_TEST_CHECK(!part.empty());
    AOT_TEST_CHECK(isSame(image.locate(part.data()), kind, 0, 0));
    AOT_TEST_CHECK(isSame(image.locate(&part.back()), kind, 0, static_cast<uint32_t>(part.size() - 1)));
    AOT_TEST_CHECK(!image.locate(part.data()).runsCompiledCode());
}

static void testRegExpAndWebAssemblyCode()
{
    const Image& loaded = *imageWithEveryCodePart().image;
    const ImageCodeRanges& ranges = loaded.header().code;
    AOT_TEST_CHECK(!(ranges.regExpCodeOffset % RegExpSection::alignment));
    AOT_TEST_CHECK(!(ranges.webAssemblyCodeOffset % 16));
    AOT_TEST_CHECK(ranges.webAssemblyCodeSize == sizeOfWebAssemblyCode);
    AOT_TEST_CHECK(loaded.regExpCode().data() == loaded.code().data() + ranges.regExpCodeOffset);
    AOT_TEST_CHECK(loaded.regExpCode().data() + loaded.regExpCode().size() == loaded.webAssemblyCode().data());

    checkCodePart(loaded, CodeKind::RegExp, loaded.regExpCode());
    checkCodePart(loaded, CodeKind::WebAssembly, loaded.webAssemblyCode());
    if (ranges.functionsEnd < ranges.regExpCodeOffset)
        AOT_TEST_CHECK(loaded.locate(loaded.regExpCode().data() - 1).kind == CodeKind::NotInImage);
}

static void testConstantsAndFFICode()
{
    const Image& loaded = *imageWithEveryCodePart().image;
    const ImageHeader& header = loaded.header();
    const ImageCodeRanges& ranges = header.code;
    const uint8_t* code = loaded.code().data();
    AOT_TEST_CHECK(ranges.constantsSize == sizeOfConstant);
    AOT_TEST_CHECK(!(ranges.constantsOffset % imageConstantsAlignment));
    AOT_TEST_CHECK(ranges.ffiCodeSize == sizeOfFFICode);
    AOT_TEST_CHECK(ranges.ffiCodeOffset + ranges.ffiCodeSize == header.codeSize);

    for (uint32_t offset = ranges.webAssemblyCodeOffset + ranges.webAssemblyCodeSize; offset < ranges.ffiCodeOffset; ++offset)
        AOT_TEST_CHECK(loaded.locate(code + offset).kind == CodeKind::NotInImage);
    checkCodePart(loaded, CodeKind::FFI, loaded.ffiCode());
    AOT_TEST_CHECK(loaded.locate(code + header.codeSize).kind == CodeKind::NotInImage);
}

static void testStubAddress()
{
    const Image& loaded = *smallImage().image;
    std::span<const StubAddressInImage> addresses = loaded.section<StubAddressInImage>(ImageSection::StubAddresses);
    AOT_TEST_CHECK(!addresses.empty());
    for (size_t index = 1; index < addresses.size(); ++index)
        AOT_TEST_CHECK(addresses[index - 1].key < addresses[index].key);

    auto found = std::ranges::find(addresses, enterClosure().bits(), &StubAddressInImage::key);
    AOT_TEST_CHECK(found != addresses.end());
    AOT_TEST_CHECK(loaded.stubAddress(enterClosure()) == loaded.sharedCodeCopy(0) + found->offset);
    AOT_TEST_CHECK(!loaded.stubAddress(writeBarrier().withCount(1)));
}

static void testSlotHandlerOffsets()
{
    const Image& loaded = *smallImage().image;
    std::span<const uint32_t> offsets = loaded.slotHandlerOffsets();
    AOT_TEST_CHECK(offsets.size() == numberOfSlotHandlerTables * numberOfSlotKinds);
    for (SlotHandlerTable table : allSlotHandlerTables) {
        for (Slot::Kind kind : allSlotKinds) {
            uint32_t offset = offsets[static_cast<unsigned>(table) * numberOfSlotKinds + static_cast<unsigned>(kind)];
            const void* address = loaded.stubAddress(slotHandlerFor(table, kind));
            AOT_TEST_CHECK(address ? address == loaded.sharedCodeCopy(0) + offset : offset == offsetOfAbsentStub);
        }
    }
}

static void testStamp()
{
    constexpr CPUFeatureSet buildTarget = CPUFeatureSet::buildTarget();
    CPUFeatureSet noFeatures;
    CPUFeatureSet oneFeature = noFeatures.with(static_cast<CPUFeature>(0));

    AOT_TEST_CHECK(smallImage().image->header().stamp == imageStamp(buildTarget, testIdentity));
    AOT_TEST_CHECK(imageStamp(buildTarget, testIdentity) == imageStamp(buildTarget, testIdentity));
    AOT_TEST_CHECK(imageStamp(buildTarget, testIdentity) != imageStamp(buildTarget, otherIdentity()));
    AOT_TEST_CHECK(imageStamp(noFeatures, testIdentity) != imageStamp(oneFeature, testIdentity));
    AOT_TEST_CHECK(imageStamp(buildTarget, testIdentity) == imageStamp(buildTarget, testIdentity, imageFormOfThisRuntime()));
    AOT_TEST_CHECK(imageStamp(buildTarget, testIdentity, Wire::ImageForm::Absolute) != imageStamp(buildTarget, testIdentity, Wire::ImageForm::PositionIndependent));
}

static void testAdmittedCodeIsInHeader()
{
    AOT_TEST_CHECK(Image::admittedCode(smallImage().bytes.span()) == Wire::AdmittedCode { });
    TestBuild build(4, { .admittedCode = Wire::AdmittedCode::Inspector });
    TestImage image = build.build(bodies(plainBody(1), plainBody(2), plainBody(3)));
    AOT_TEST_CHECK(image.image->header().options.admitted() == Wire::AdmittedCode::Inspector);
    AOT_TEST_CHECK(Image::admittedCode(image.bytes.span()) == Wire::AdmittedCode::Inspector);
}

static void testAdmittedCodeNeedsHeaderOfThisFormat()
{
    auto admittedAfter = [](auto change) {
        AlignedBytes copy(smallImage().bytes.span());
        change(headerOf(copy));
        return Image::admittedCode(copy.span());
    };
    AOT_TEST_CHECK(admittedAfter([](ImageHeader&) { }) == Wire::AdmittedCode { });
    AOT_TEST_CHECK(admittedAfter([](ImageHeader& header) { header.options.admittedCode = static_cast<uint8_t>(Wire::AdmittedCode::Inspector); }) == Wire::AdmittedCode::Inspector);
    AOT_TEST_CHECK(!admittedAfter([](ImageHeader& header) { header.magic ^= 1; }));
    AOT_TEST_CHECK(!admittedAfter([](ImageHeader& header) { header.stamp ^= 1; }));
    AOT_TEST_CHECK(!admittedAfter([](ImageHeader& header) { header.identity = otherIdentity(); }));

    std::span<const uint8_t> bytes = smallImage().bytes.span();
    AOT_TEST_CHECK(Image::admittedCode(bytes.first(sizeof(ImageHeader))));
    AOT_TEST_CHECK(!Image::admittedCode(bytes.first(sizeof(ImageHeader) - 1)));
    AOT_TEST_CHECK(!Image::admittedCode(bytes.first(0)));
    AOT_TEST_CHECK(!Image::admittedCode(bytes.subspan(8)));
}

static void testFlagsNeedHeaderOfThisFormat()
{
    using Flags = OptionSet<ImageFlag>;
    auto flagsAfter = [](auto change) {
        AlignedBytes copy(smallImage().bytes.span());
        change(headerOf(copy));
        return Image::flags(copy.span());
    };
    AOT_TEST_CHECK(flagsAfter([](ImageHeader&) { }) == Flags { });
    AOT_TEST_CHECK(flagsAfter([](ImageHeader& header) { header.flags = Flags { ImageFlag::LoadsModulesByName }.toRaw(); }) == Flags { ImageFlag::LoadsModulesByName });
    AOT_TEST_CHECK(!flagsAfter([](ImageHeader& header) { header.magic ^= 1; }));
    AOT_TEST_CHECK(!flagsAfter([](ImageHeader& header) { header.stamp ^= 1; }));
    AOT_TEST_CHECK(!flagsAfter([](ImageHeader& header) { header.identity = otherIdentity(); }));

    std::span<const uint8_t> bytes = smallImage().bytes.span();
    AOT_TEST_CHECK(Image::flags(bytes.first(sizeof(ImageHeader))));
    AOT_TEST_CHECK(!Image::flags(bytes.first(sizeof(ImageHeader) - 1)));
    AOT_TEST_CHECK(!Image::flags(bytes.subspan(8)));
}

static ConstantData constantFilledWith(uint8_t byte, uint32_t size)
{
    return { Vector<uint8_t>(FillWith { }, size, byte), size };
}

static ConstantData doubleConstant(double value)
{
    return { Vector<uint8_t>(asByteSpan(value)), sizeof(double) };
}

static CompiledCode bodyWithConstants(uint32_t function, Vector<ConstantData>&& constants, unsigned numberOfJumpTables = 0)
{
    CodeSketch sketch = framedSketch(function);
    uint32_t target = sketch.end();
    sketch.code().body.constants = WTF::move(constants);
    for (unsigned table = 0; table < numberOfJumpTables; ++table)
        sketch.code().body.jumpTables.append(JumpTable { { 0, target } });
    return sketch.take();
}

static ConstantPool poolOf(Vector<CompiledCode>&& code, const TestOptions& options = { })
{
    BuildContext context(wireOptions(options));
    return buildConstantPool(createLinkedFunctions(WTF::move(code), context), context);
}

// The pool begins with the shared constants as they are. A constant of a body that equals a part is that part; one that equals only a piece of a part, or two parts in a row, has bytes of its own.
static void testConstantsOfBodiesAreFoundAmongSharedConstants()
{
    std::array<double, 4> shared { 0.25, 0.5, 0.75, 0 };
    std::array<ConstantRange, 2> parts { { { 0, 2 * sizeof(double) }, { 2 * sizeof(double), sizeof(double) } } };
    auto constantOf = [&](size_t first, size_t count) {
        std::span<const uint8_t> bytes = asBytes(std::span(shared).subspan(first, count));
        return ConstantData { Vector<uint8_t>(bytes), alignmentOfConstant(bytes.size()) };
    };
    BuildContext context(wireOptions({ }));
    LinkedFunctions functions = createLinkedFunctions(bodies(bodyWithConstants(1, { constantOf(0, 2), constantOf(2, 1), constantOf(1, 1) }), bodyWithConstants(2, { doubleConstant(0.1), constantOf(2, 1), constantOf(0, 4) })), context);
    ConstantPool pool = buildConstantPool(functions, context, { asBytes(std::span(shared)), parts });
    AOT_TEST_CHECK(pool.sizeOfSharedConstants == sizeof(shared) && equalSpans(pool.bytes.span().first(sizeof(shared)), asBytes(std::span(shared))));
    AOT_TEST_CHECK(!pool.constantOffsetsByBody[0][0] && pool.constantOffsetsByBody[0][1] == parts[1].offset && pool.constantOffsetsByBody[1][1] == parts[1].offset);
    for (uint32_t offset : { pool.constantOffsetsByBody[0][2], pool.constantOffsetsByBody[1][0], pool.constantOffsetsByBody[1][2] })
        AOT_TEST_CHECK(offset >= sizeof(shared));
    AOT_TEST_CHECK(pool.bytes.size() == sizeof(shared) + sizeof(shared) + 2 * sizeof(double));
    AOT_TEST_CHECK(equalSpans(pool.bytes.span().subspan(pool.constantOffsetsByBody[0][2], sizeof(double)), asBytes(std::span(shared).subspan(1, 1))));
    AOT_TEST_CHECK(equalSpans(pool.bytes.span().subspan(pool.constantOffsetsByBody[1][2], sizeof(shared)), asBytes(std::span(shared))));

    ConstantPool withoutShared = buildConstantPool(functions, context);
    AOT_TEST_CHECK(!withoutShared.sizeOfSharedConstants && withoutShared.bytes.size() == sizeof(shared) + 2 * sizeof(double) + 3 * sizeof(double));
}

static void testEqualConstantsShare()
{
    ConstantPool pool = poolOf(bodies(bodyWithConstants(1, { doubleConstant(0.1) }), bodyWithConstants(2, { doubleConstant(0.5), doubleConstant(0.1) })));
    AOT_TEST_CHECK(pool.bytes.size() == 2 * sizeof(double));
    AOT_TEST_CHECK(pool.constantOffsetsByBody[0][0] == pool.constantOffsetsByBody[1][1]);
    AOT_TEST_CHECK(pool.constantOffsetsByBody[0][0] != pool.constantOffsetsByBody[1][0]);
    AOT_TEST_CHECK(equalSpans(pool.bytes.span().subspan(pool.constantOffsetsByBody[0][0], sizeof(double)), doubleConstant(0.1).bytes.span()));
    AOT_TEST_CHECK(equalSpans(pool.bytes.span().subspan(pool.constantOffsetsByBody[1][0], sizeof(double)), doubleConstant(0.5).bytes.span()));
}

static void testConstantsAreAligned()
{
    Vector<Vector<ConstantData>> constants { { constantFilledWith(1, 4), constantFilledWith(2, 16) }, { constantFilledWith(3, 1), constantFilledWith(4, 16), constantFilledWith(5, 8) } };
    ConstantPool pool = poolOf(bodies(bodyWithConstants(1, Vector<ConstantData>(constants[0])), bodyWithConstants(2, Vector<ConstantData>(constants[1]))));
    AOT_TEST_CHECK(pool.bytes.size() == 4 + 16 + 1 + 16 + 8);
    for (size_t position = 0; position < constants.size(); ++position) {
        for (size_t index = 0; index < constants[position].size(); ++index) {
            const ConstantData& constant = constants[position][index];
            uint32_t offset = pool.constantOffsetsByBody[position][index];
            AOT_TEST_CHECK(!(offset % constant.alignment));
            AOT_TEST_CHECK(equalSpans(pool.bytes.span().subspan(offset, constant.bytes.size()), constant.bytes.span()));
        }
    }
}

static void testJumpTablesHaveOwnRanges()
{
    constexpr uint32_t sizeOfTable = 2 * sizeof(int32_t);
    ConstantPool pool = poolOf(bodies(bodyWithConstants(1, { constantFilledWith(1, 1) }, 2), bodyWithConstants(2, { }, 1)));
    std::array<uint32_t, 3> offsets { pool.jumpTableOffsetsByBody[0][0], pool.jumpTableOffsetsByBody[0][1], pool.jumpTableOffsetsByBody[1][0] };
    AOT_TEST_CHECK(offsets[0] == sizeof(int32_t));
    AOT_TEST_CHECK(offsets[1] == offsets[0] + sizeOfTable);
    AOT_TEST_CHECK(offsets[2] == offsets[1] + sizeOfTable);
    AOT_TEST_CHECK(pool.bytes.size() == offsets[2] + sizeOfTable);
}

static Vector<CompiledCode> bodiesWithManyConstants()
{
    Vector<CompiledCode> code;
    for (uint32_t function = 1; function <= 300; ++function)
        code.append(bodyWithConstants(function, { doubleConstant(function % 7), constantFilledWith(static_cast<uint8_t>(function % 5), 1u << function % 5), doubleConstant(function) }, function % 3));
    return code;
}

static void testConstantPoolWithThreads()
{
    ConstantPool serial = poolOf(bodiesWithManyConstants());
    for (unsigned threadCount : { 2u, 16u }) {
        ConstantPool parallel = poolOf(bodiesWithManyConstants(), { .threadCount = threadCount });
        AOT_TEST_CHECK(parallel.bytes == serial.bytes);
        AOT_TEST_CHECK(parallel.constantOffsetsByBody == serial.constantOffsetsByBody);
        AOT_TEST_CHECK(parallel.jumpTableOffsetsByBody == serial.jumpTableOffsetsByBody);
    }
}

std::span<const NativeTest> imageTableTests()
{
    static constexpr NativeTest tests[] = {
        { "image/format/varint-round-trip"_s, testVarintRoundTrip },
        { "image/format/varint-stops-after-ten-bytes"_s, testVarintStopsAfterTenBytes },
        { "image/format/zigzag"_s, testZigZag },
        { "image/format/side-table-parts"_s, testSideTableParts },
        { "image/format/side-table-parts-without-groups"_s, testSideTablePartsWithoutGroups },
        { "image/format/frame-ranges"_s, testFrameRanges },
        { "image/format/frame-range-without-end"_s, testFrameRangeWithoutEnd },
        { "image/format/register-packing"_s, testRegisterPacking },
        { "image/format/image-frame-bits"_s, testImageFrameBits },
        { "image/format/function-record-words"_s, testFunctionRecordWords },
        { "image/check/accepts"_s, testAccepts },
        { "image/check/seven-bytes"_s, testSevenBytes },
        { "image/check/thirty-one-bytes"_s, testThirtyOneBytes },
        { "image/check/header-cut"_s, testHeaderCut },
        { "image/check/wrong-magic"_s, testWrongMagic },
        { "image/check/other-stamp"_s, testOtherStamp },
        { "image/check/size-above-span"_s, testSizeAboveSpan },
        { "image/check/size-not-whole-pages"_s, testSizeNotWholePages },
        { "image/check/misaligned"_s, testMisaligned },
        { "image/check/other-identity"_s, testOtherIdentity },
        { "image/check/needs-cell-cage"_s, testNeedsCellCage },
        { "image/check/other-cage-shift"_s, testOtherCageShift },
        { "image/check/other-size-classes"_s, testOtherSizeClasses },
        { "image/check/other-image-form"_s, testOtherImageForm },
        { "image/check/unaligned-code"_s, testUnalignedCode },
        { "image/check/code-past-end"_s, testCodePastEnd },
        { "image/check/section-offset-unaligned"_s, testSectionOffsetUnaligned },
        { "image/check/section-before-previous-end"_s, testSectionBeforePreviousEnd },
        { "image/check/section-past-code"_s, testSectionPastCode },
        { "image/check/section-size-not-whole-records"_s, testSectionSizeNotWholeRecords },
        { "image/check/function-count"_s, testFunctionCount },
        { "image/check/functions-past-code"_s, testFunctionsPastCode },
        { "image/check/too-few-granules"_s, testTooFewGranules },
        { "image/check/granule-shift"_s, testGranuleShift },
        { "image/check/shared-code-copy-past-functions"_s, testSharedCodeCopyPastFunctions },
        { "image/check/shared-code-ends-past-functions"_s, testSharedCodeEndsPastFunctions },
        { "image/check/shared-code-copies-overlap"_s, testSharedCodeCopiesOverlap },
        { "image/check/regexp-code-before-end-of-functions"_s, testRegExpCodeBeforeEndOfFunctions },
        { "image/check/wasm-code-before-regexp-code"_s, testWebAssemblyCodeBeforeRegExpCode },
        { "image/check/wasm-code-past-constants"_s, testWebAssemblyCodePastConstants },
        { "image/check/constants-past-ffi-code"_s, testConstantsPastFFICode },
        { "image/check/ffi-code-past-end"_s, testFFICodePastEnd },
        { "image/check/ffi-code-off-page-boundary"_s, testFFICodeOffPageBoundary },
        { "image/check/program-tables-magic"_s, testProgramTablesMagic },
        { "image/check/program-tables-cut"_s, testProgramTablesCut },
        { "image/check/closure-function-range"_s, testClosureFunctionRange },
        { "image/check/string-cell-count"_s, testStringCellCount },
        { "image/check/no-code"_s, testNoCode },
        { "image/locate/function-bytes"_s, testFunctionBytes },
        { "image/locate/every-byte"_s, testEveryByte },
        { "image/locate/stub-bytes"_s, testStubBytes },
        { "image/locate/stub-return-sites"_s, testStubReturnSites },
        { "image/locate/outside"_s, testOutside },
        { "image/locate/regexp-and-wasm-code"_s, testRegExpAndWebAssemblyCode },
        { "image/locate/constants-and-ffi-code"_s, testConstantsAndFFICode },
        { "image/locate/stub-address"_s, testStubAddress },
        { "image/locate/slot-handler-offsets"_s, testSlotHandlerOffsets },
        { "image/stamp/stamp"_s, testStamp },
        { "image/admitted-code/in-header"_s, testAdmittedCodeIsInHeader },
        { "image/admitted-code/needs-header-of-this-format"_s, testAdmittedCodeNeedsHeaderOfThisFormat },
        { "image/flags/need-header-of-this-format"_s, testFlagsNeedHeaderOfThisFormat },
        { "image/constants/equal-constants-share"_s, testEqualConstantsShare },
        { "image/constants/constants-of-bodies-are-found-among-shared-constants"_s, testConstantsOfBodiesAreFoundAmongSharedConstants },
        { "image/constants/aligned"_s, testConstantsAreAligned },
        { "image/constants/jump-tables-have-own-ranges"_s, testJumpTablesHaveOwnRanges },
        { "image/constants/threads"_s, testConstantPoolWithThreads },
    };
    return tests;
}

} // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
