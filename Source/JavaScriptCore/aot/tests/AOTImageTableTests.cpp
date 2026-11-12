/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTClassMemberTable.h"
#include "AOTCompiledFrame.h"
#include "AOTConstantPool.h"
#include "AOTFieldTable.h"
#include "AOTIRProgram.h"
#include "AOTIdentifierNumbering.h"
#include "AOTIdentifierTable.h"
#include "AOTImageBuildSteps.h"
#include "AOTImageTestBodies.h"
#include "AOTLinkedFunctions.h"
#include "AOTNumbering.h"
#include "AOTRegExpCompiler.h"
#include "AOTRuntimeDescription.h"
#include "AOTStringTable.h"
#include "AOTStubsBarriers.h"
#include "AOTStubsCallAdapters.h"
#include "AOTStubsPropertyKeys.h"
#include "AOTTextEncoder.h"
#include "AOTThreads.h"
#include "AOTVarint.h"
#include "AOTVector.h"
#include "CPUFeatureSet.h"
#include "FFIStubs.h"
#include "MarkedBlockCage.h"
#include "RegisterSet.h"
#include <algorithm>
#include <array>
#include <initializer_list>
#include <optional>
#include <span>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC::AOT {

using namespace ImageTests;

using Verdict = Wire::ImageVerdict;

namespace {

struct SizeOfProgramWithRecords {
    uint32_t numberOfLayouts;
    uint32_t numberOfNames;
    uint32_t mostFieldsInLayout;
};

} // anonymous namespace

static constexpr uint32_t sizeOfConstant = 8;
static constexpr uint8_t byteOfConstant = 0x5a;

static constexpr std::array<unsigned, 3> threadCountsOfTableGroups { 1, 3, 8 };
static constexpr size_t fewestSortedOnPoolForTableGroups = 1 << 13;
static constexpr SizeOfProgramWithRecords recordsThatShareNames { 9000, 300, 12 };
static constexpr SizeOfProgramWithRecords recordsWithOneName { 13000, 13000, 1 };
static constexpr uint32_t recordsHandedOutByCall = 50;

static constexpr ASCIILiteral patternOfRegExpProgram = "ab+c"_s;
static constexpr ASCIILiteral regExpProgramText = "module m0 path=\"main.js\" displayPath=\"main.js\" body=f0\n"
    "entry\n"
    "function f0 name=\"main\" kind=Module results=[{type=anything}] {\n"
    "  b0():\n"
    "    v0 = Builtin builtin=GlobalThis\n"
    "    v1 = PropGet v0 key=\"sink\" [isFreeName]\n"
    "    v2 = NewRegExp pattern=\"ab+c\"\n"
    "    v3 = Call v1, -, v2\n"
    "    v4 = PropGet v0 key=\"text\" [isFreeName]\n"
    "    v5 = PropGet v0 key=\"eval\" [isFreeName]\n"
    "    v6 = Call v5, -, v4\n"
    "    Return\n"
    "}\n"_s;

static const TestImage& smallImage()
{
    static NeverDestroyed<TestImage> shared([] {
        TestBuild build(4);
        return builtImage(build, bodies(plainBody(1), plainBody(2), plainBody(3)));
    }());
    return shared.get();
}

static CompiledCode bodyWithConstantAndRegExp(uint32_t function, StringID pattern)
{
    CodeSketch sketch = framedSketch(function);
    sketch.callStub(writeBarrier(), sketch.addSite());
    sketch.code().body.rareLists.ensure().constants.append(ConstantData { Vector<uint8_t>(FillWith { }, sizeOfConstant, byteOfConstant), sizeOfConstant });
    sketch.end();
    CompiledCode code = sketch.take();
    MentionList mentions;
    mentions.regExp(pattern, { });
    mentions.moveTo(code.body);
    return code;
}

static const TestImage& imageWithEveryCodePart()
{
    static NeverDestroyed<TestImage> shared([] {
        TestBuild build(makeString(regExpProgramText, runtimeFunctionsText(1)), { });
        StringID pattern = build.program().strings.find(Typed::encodeProgramString(patternOfRegExpProgram).span());
        AOT_TEST_CHECK(pattern);
        TestImage image = builtImage(build, bodies(entryOfBody(build, 0), bodyWithConstantAndRegExp(0, pattern)));
        AOT_TEST_CHECK(image.built.contents.numberOfRegExpsWithCode == 1);
        return image;
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

static void testOtherFormatVersion()
{
    checkHeaderChange(Verdict::StampMismatch, [](ImageHeader& header) { header.formatVersion += 1; });
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

static void testSectionOffsetUnaligned()
{
    checkEverySectionChange([](ImageHeader&, ImageSectionRange& range) { range.offset += 8; });
}

static void testSectionSizeNotWholeRecords()
{
    using enum ImageSection;
    static constexpr std::array sections {
        Functions, FunctionStarts, CodeGranules, InnerEntries, Frames, FrameMaps, ClosureWords, SharedCodeCopies,
        Strings, LongStrings, Identifiers, StringConstants, NamesOfSites, RegExps, RegExpHashes, StubAddresses, StubReturnSites,
    };
    for (ImageSection section : sections) {
        checkHeaderChange(Verdict::MalformedTables, [&](ImageHeader& header) {
            rangeOf(header, section).size += 1;
        });
    }
}

static void testFunctionStartsInGroups()
{
    for (size_t count : { 0u, 1u, 31u, 32u, 33u, 200u }) {
        for (uint32_t distance : { 1u, 2000u, 3000u, 70000u, 0x1000000u }) {
            Vector<uint32_t> starts;
            for (size_t index = 0; index < count; ++index)
                starts.append(static_cast<uint32_t>(16 + index * distance + (index % 7 == 5 ? distance / 2 : 0)));
            Vector<uint8_t> bytes = FunctionStarts::encode(starts.span());
            AOT_TEST_CHECK(!(bytes.size() % sizeof(uint32_t)) && bytes.size() / sizeof(uint32_t) >= FunctionStarts::numberOfWordsWithoutLists(count));
            FunctionStarts decoded(reinterpret_cast<const uint32_t*>(bytes.span().data()), count);
            uint32_t lastGroup = static_cast<uint32_t>(FunctionStarts::numberOfGroupsFor(count) - 1);
            for (uint32_t index = 0; index < count; ++index) {
                AOT_TEST_CHECK(decoded[index] == starts[index]);
                uint32_t end = index + 1 < count ? starts[index + 1] : starts[index] + distance;
                for (uint32_t offset : { starts[index], end - 1 }) {
                    if (offset < starts[index])
                        continue;
                    for (uint32_t firstGroup : { 0u, index >> FunctionStarts::groupShift }) {
                        for (uint32_t last : { index >> FunctionStarts::groupShift, lastGroup })
                            AOT_TEST_CHECK(decoded.find(offset, firstGroup, last) == index);
                    }
                }
            }
            AOT_TEST_CHECK(decoded[count] == std::numeric_limits<uint32_t>::max());
        }
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

static void testSharedCodeEndsPastFunctions()
{
    checkHeaderChange(Verdict::MalformedTables, [](ImageHeader& header) { header.code.sharedCodeSize = header.code.functionsEnd + 1; });
}

#if FFI_STUBS_SUPPORTED
static void testFFICodeOffPageBoundary()
{
    AOT_TEST_CHECK(!(imageWithEveryCodePart().image->header().code.ffiCodeOffset % imagePageSize));
    checkCodePartChange([](ImageCodeRanges& code) {
        code.ffiCodeOffset += 16;
        code.ffiCodeSize -= 16;
    });
}
#endif

static void testStubsInHeader()
{
    checkHeaderChange(Verdict::MalformedTables, [](ImageHeader& header) { header.numberOfStubsFoundByKey = rangeOf(header, ImageSection::StubAddresses).size / sizeof(StubAddressInImage) + 1; });
    checkHeaderChange(Verdict::MalformedTables, [](ImageHeader& header) { header.singleReturnSites[static_cast<unsigned>(StubFrameKind::Adapter)] = header.code.sharedCodeSize + 1; });
}

static void testNoCode()
{
    checkHeaderChange(Verdict::NoCode, [](ImageHeader& header) {
        header.codeSize = 0;
        header.executableSize = 0;
        header.code = { };
        std::ranges::fill(header.singleReturnSites, 0);
        rangeOf(header, ImageSection::SharedCodeCopies).size = 0;
    });
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

    for (unsigned kind = 0; kind < numberOfStubFrameKinds; ++kind) {
        auto site = std::ranges::find(sites, static_cast<StubFrameKind>(kind), &StubReturnSite::kind);
        bool isListed = hasSingleReturnSite(static_cast<StubFrameKind>(kind)) && site != sites.end();
        AOT_TEST_CHECK(loaded.header().singleReturnSites[kind] == (isListed ? site->offset : 0));
    }
}

#if defined(BUN_HARDENED)
static void testWordsInThePlaceOfAReturnAddress()
{
    const TestImage& image = smallImage();
    const Image& loaded = *image.image;
    const uint8_t* stubs = loaded.sharedCodeCopy(0);
    for (const StubReturnSite& site : loaded.stubReturnSites())
        AOT_TEST_CHECK(faultOfReturnAddressInRun(loaded, stubs + site.offset).isNull());
    AOT_TEST_CHECK(faultOfReturnAddressInRun(loaded, reinterpret_cast<const void*>(&testWordsInThePlaceOfAReturnAddress)).isNull());

    AOT_TEST_CHECK(!faultOfReturnAddressInRun(loaded, image.address(1, 0)).isNull());
    AOT_TEST_CHECK(!faultOfReturnAddressInRun(loaded, stubs).isNull());

    auto onHeap = makeUniqueArray<uint8_t>(1);
    static const uint8_t constant = 0;
    AOT_TEST_CHECK(!faultOfReturnAddressInRun(loaded, &onHeap).isNull());
    AOT_TEST_CHECK(!faultOfReturnAddressInRun(loaded, onHeap.get()).isNull());
    AOT_TEST_CHECK(!faultOfReturnAddressInRun(loaded, &constant).isNull());
    AOT_TEST_CHECK(!faultOfReturnAddressInRun(loaded, &loaded.header()).isNull());
    AOT_TEST_CHECK(!faultOfReturnAddressInRun(loaded, loaded.code().data() + loaded.header().codeSize).isNull());
    AOT_TEST_CHECK(!faultOfReturnAddressInRun(loaded, nullptr).isNull());
}
#endif

static void checkCodePart(const Image& image, CodeKind kind, std::span<const uint8_t> part)
{
    AOT_TEST_CHECK(!part.empty());
    AOT_TEST_CHECK(isSame(image.locate(part.data()), kind, 0, 0));
    AOT_TEST_CHECK(isSame(image.locate(&part.back()), kind, 0, static_cast<uint32_t>(part.size() - 1)));
    AOT_TEST_CHECK(!image.locate(part.data()).runsCompiledCode());
}

static void testRegExpCodePart()
{
    const Image& loaded = *imageWithEveryCodePart().image;
    const ImageCodeRanges& ranges = loaded.header().code;
    AOT_TEST_CHECK(!(ranges.regExpCodeOffset % RegExpSection::alignment));
    AOT_TEST_CHECK(loaded.webAssemblyCode().empty());
    AOT_TEST_CHECK(loaded.regExpCode().data() == loaded.code().data() + ranges.regExpCodeOffset);
    AOT_TEST_CHECK(loaded.regExpCode().data() + loaded.regExpCode().size() == loaded.code().data() + ranges.constantsOffset);
    AOT_TEST_CHECK(ranges.regExpCodeOffset < loaded.header().executableSize);

    checkCodePart(loaded, CodeKind::RegExp, loaded.regExpCode());
    if (ranges.ffiCodeOffset + ranges.ffiCodeSize < ranges.regExpCodeOffset)
        AOT_TEST_CHECK(loaded.locate(loaded.regExpCode().data() - 1).kind == CodeKind::NotInImage);
}

static void testConstantsAndFFICode()
{
    const Image& loaded = *imageWithEveryCodePart().image;
    const ImageHeader& header = loaded.header();
    const ImageCodeRanges& ranges = header.code;
    const uint8_t* code = loaded.code().data();
    std::span<const uint8_t> constants = loaded.code().subspan(ranges.constantsOffset, ranges.constantsSize);
    AOT_TEST_CHECK(constants.size() >= sizeOfConstant);
    AOT_TEST_CHECK(std::ranges::all_of(constants.last(sizeOfConstant), [](uint8_t byte) { return byte == byteOfConstant; }));
    AOT_TEST_CHECK(!(ranges.constantsOffset % imageConstantsAlignment));
    AOT_TEST_CHECK(!(header.executableSize % imagePageSize));
    AOT_TEST_CHECK(ranges.ffiCodeOffset + ranges.ffiCodeSize <= header.executableSize && header.executableSize <= ranges.constantsOffset);

    for (uint32_t offset = ranges.webAssemblyCodeOffset + ranges.webAssemblyCodeSize; offset < ranges.ffiCodeOffset; ++offset)
        AOT_TEST_CHECK(loaded.locate(code + offset).kind == CodeKind::NotInImage);
    for (uint32_t offset = ranges.constantsOffset; offset < header.codeSize; ++offset)
        AOT_TEST_CHECK(loaded.locate(code + offset).kind == CodeKind::NotInImage);
#if FFI_STUBS_SUPPORTED
    AOT_TEST_CHECK(!(ranges.ffiCodeSize % imagePageSize));
    AOT_TEST_CHECK(!loaded.ffiDirectory().empty());
    checkCodePart(loaded, CodeKind::FFI, loaded.ffiCode());
#else
    AOT_TEST_CHECK(!ranges.ffiCodeSize);
    AOT_TEST_CHECK(ranges.constantsOffset + ranges.constantsSize == header.codeSize);
#endif
    AOT_TEST_CHECK(loaded.locate(code + header.codeSize).kind == CodeKind::NotInImage);
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

static void testRuntimeDescriptionNamesFormatVersion()
{
    std::span<const uint8_t> record = descriptionOfThisRuntime();
    uint32_t named;
    memcpy(&named, record.data() + 28, sizeof(named));
    AOT_TEST_CHECK(named == imageFormatVersion && named == smallImage().image->header().formatVersion);
    AOT_TEST_CHECK(!findRuntimeDifference(record));

    Vector<uint8_t> other(record);
    ++named;
    memcpy(other.mutableSpan().data() + 28, &named, sizeof(named));
    std::optional<RuntimeDifference> difference = findRuntimeDifference(other.span());
    AOT_TEST_CHECK(difference && difference->name == "the version of the image format"_s && difference->here == imageFormatVersion && difference->there == imageFormatVersion + 1);

    named = 0;
    memcpy(other.mutableSpan().data() + 28, &named, sizeof(named));
    difference = findRuntimeDifference(other.span());
    AOT_TEST_CHECK(difference && difference->name == "the version of the image format"_s && !difference->there);
}

static void testRuntimeDescriptionNamesPlatformOfBuiltinTable()
{
    Vector<uint8_t> record(descriptionOfThisRuntime());
    uint8_t& platform = record[offsetof(RuntimeDescription, platform)];
    AOT_TEST_CHECK(platform == static_cast<uint8_t>(targetOfThisBuild().platform));

    static_assert(BuiltinPlatform::Android != BuiltinPlatform::Linux);
    platform = static_cast<uint8_t>(targetOfThisBuild().platform == BuiltinPlatform::Android ? BuiltinPlatform::Linux : BuiltinPlatform::Android);
    std::optional<RuntimeDifference> difference = findRuntimeDifference(record.span());
    AOT_TEST_CHECK(difference && difference->name == "the operating system"_s && difference->there == platform);
}

static void testRuntimeFactsNameWhatEmittedCodeTakes()
{
    std::span<const ASCIILiteral> names = namesOfRuntimeFacts();
    for (ASCIILiteral name : {
        "the names of the entries in the table of the run time in the order of their numbers"_s,
        "isHardenedBuild"_s,
        "sizeOfLoopPoll"_s,
        "sizeof(InlineAllocationData)"_s,
        "sizeof(BuiltinDirectFunctionBinding)"_s,
        "OBJECT_OFFSETOF(BuiltinDirectFunctionBinding, function)"_s,
        "sizeof(StackStringBuilder)"_s,
        "OBJECT_OFFSETOF(StackStringBuilder, limit)"_s,
        "MutatorMarkStackLayout::offsetOfTop()"_s,
        "OBJECT_OFFSETOF(ProgramSymbols, entries)"_s,
        "Yarr::MatchingContextHolder::offsetOfStackLimit()"_s,
    })
        AOT_TEST_CHECK(std::ranges::find(names, name) != names.end());
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
    AOT_TEST_CHECK(flagsAfter([](ImageHeader&) { }) == Flags::fromRaw(smallImage().image->header().flags));
    AOT_TEST_CHECK(flagsAfter([](ImageHeader& header) { header.flags = 0; }) == Flags { });
    AOT_TEST_CHECK(flagsAfter([](ImageHeader& header) { header.flags = Flags { ImageFlag::LoadsModulesByName }.toRaw(); }) == Flags { ImageFlag::LoadsModulesByName });
    AOT_TEST_CHECK(!flagsAfter([](ImageHeader& header) { header.magic ^= 1; }));
    AOT_TEST_CHECK(!flagsAfter([](ImageHeader& header) { header.stamp ^= 1; }));
    AOT_TEST_CHECK(!flagsAfter([](ImageHeader& header) { header.identity = otherIdentity(); }));

    std::span<const uint8_t> bytes = smallImage().bytes.span();
    AOT_TEST_CHECK(Image::flags(bytes.first(sizeof(ImageHeader))));
    AOT_TEST_CHECK(!Image::flags(bytes.first(sizeof(ImageHeader) - 1)));
    AOT_TEST_CHECK(!Image::flags(bytes.subspan(8)));
}

static ConstantData doubleConstant(double value)
{
    return { Vector<uint8_t>(asByteSpan(value)), sizeof(double) };
}

static CompiledCode bodyWithConstants(uint32_t function, Vector<ConstantData>&& constants, unsigned numberOfJumpTables = 0)
{
    CodeSketch sketch = framedSketch(function);
    uint32_t target = sketch.end();
    RareListsOfBody& rareLists = sketch.code().body.rareLists.ensure();
    rareLists.constants = WTF::move(constants);
    for (unsigned table = 0; table < numberOfJumpTables; ++table)
        rareLists.jumpTables.append(JumpTable { { 0, target } });
    return sketch.take();
}

static void testConstantsOfBodiesAreFoundAmongSharedConstants()
{
    std::array<double, 4> shared { 0.25, 0.5, 0.75, 0 };
    std::array<ConstantRange, 2> parts { { { 0, 2 * sizeof(double) }, { 2 * sizeof(double), sizeof(double) } } };
    auto constantOf = [&](size_t first, size_t count) {
        std::span<const uint8_t> bytes = asBytes(std::span(shared).subspan(first, count));
        return ConstantData { Vector<uint8_t>(bytes), alignmentOfConstant(bytes.size()) };
    };
    BuildContext context(wireOptions({ }));
    LinkedFunctions functions(bodies(bodyWithConstants(1, { constantOf(0, 2), constantOf(2, 1), constantOf(1, 1) }), bodyWithConstants(2, { doubleConstant(0.1), constantOf(2, 1), constantOf(0, 4) })));
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

static constexpr uint32_t layoutWithFieldsOfRecord(uint32_t layout) { return layout % 3 == 2 ? layout - 1 : layout; }
static constexpr uint32_t numberOfFieldsOfRecord(SizeOfProgramWithRecords size, uint32_t layout) { return 1 + layoutWithFieldsOfRecord(layout) % size.mostFieldsInLayout; }
static constexpr uint32_t nameOfFieldOfRecord(SizeOfProgramWithRecords size, uint32_t layout, uint32_t field) { return (layoutWithFieldsOfRecord(layout) * 7 + field * 13) % size.numberOfNames; }
static constexpr bool fieldOfRecordHoldsAnyValue(uint32_t layout, uint32_t field) { return !field && !(layoutWithFieldsOfRecord(layout) % 8); }

static String textOfProgramWithRecords(SizeOfProgramWithRecords size)
{
    constexpr unsigned anyValue = 2;
    constexpr unsigned number = 3;
    StringBuilder text;
    for (uint32_t layout = 0; layout < size.numberOfLayouts; ++layout) {
        text.append("layout L"_s, layout, " name=\"R"_s, layout, "\" fields=["_s);
        for (uint32_t field = 0; field < numberOfFieldsOfRecord(size, layout); ++field)
            text.append(field ? ", "_s : ""_s, "{key=\"n"_s, nameOfFieldOfRecord(size, layout, field), "\" declared={type="_s, fieldOfRecordHoldsAnyValue(layout, field) ? "anything"_s : "number"_s, "} flags=[IsRequired]}"_s);
        text.append("]\n"_s);
    }
    text.append("module m0 path=\"main.js\" displayPath=\"main.js\" body=f0\n"
        "entry module=m0\n"
        "function f0 name=\"main\" kind=Module results=[{type=anything}] {\n"
        "  b0():\n"
        "    v0 = Builtin builtin=GlobalThis\n"
        "    v1 = PropGet v0 key=\"sink\" [isFreeName]\n"
        "    v2 = PropGet v0 key=\"input\" [isFreeName]\n"
        "    v3 = Integer integer=1\n"
        "    v4 = NewClosure function=f1\n"
        "    v5 = Call v1, -, v4\n"_s);
    uint32_t nextValue = 6;
    for (uint32_t first = 0; first < size.numberOfLayouts; first += recordsHandedOutByCall) {
        uint32_t end = std::min(first + recordsHandedOutByCall, size.numberOfLayouts);
        uint32_t firstRecord = nextValue;
        for (uint32_t layout = first; layout < end; ++layout) {
            text.append("    v"_s, nextValue++, " = NewObject -"_s);
            for (uint32_t field = 0; field < numberOfFieldsOfRecord(size, layout); ++field)
                text.append(", v"_s, fieldOfRecordHoldsAnyValue(layout, field) ? anyValue : number);
            text.append(" layout=L"_s, layout, " fields=["_s);
            for (uint32_t field = 0; field < numberOfFieldsOfRecord(size, layout); ++field)
                text.append(field ? ", "_s : ""_s, field);
            text.append("]\n"_s);
        }
        uint32_t endOfRecords = nextValue;
        text.append("    v"_s, nextValue++, " = Call v1, -"_s);
        for (uint32_t record = firstRecord; record < endOfRecords; ++record)
            text.append(", v"_s, record);
        text.append('\n');
    }
    text.append("    Return\n}\n"
        "function f1 name=\"f1\" results=[{type=anything}] {\n  b0():\n    Return\n}\n"_s);
    text.append(runtimeFunctionsText(2));
    return text.toString();
}

static Vector<uint8_t> bytesOfImageForAddressZero(const TestImage& image)
{
    Vector<uint8_t> bytes(image.bytes.span());
    relocateImage(bytes.mutableSpan(), 0);
    return bytes;
}

template<typename BuildBytes>
static void checkSameBytesOnThreadsOfTableGroups(const BuildBytes& buildBytes)
{
    Vector<uint8_t> expected = buildBytes(threadCountsOfTableGroups[0]);
    for (unsigned threadCount : std::span(threadCountsOfTableGroups).subspan(1))
        AOT_TEST_CHECK(buildBytes(threadCount) == expected);
}

static void checkFieldTableReadsBack(const ProgramTables& tables, SizeOfProgramWithRecords size)
{
    constexpr uint16_t noField = UINT16_MAX;
    const uint32_t numberOfColumns = tables.numberOfFieldTableColumns;
    std::span<const uint32_t> entries = tables.fieldTableEntries();
    AOT_TEST_CHECK(numberOfColumns > size.numberOfNames && tables.numberOfFieldTableRows >= size.numberOfLayouts);
    AOT_TEST_CHECK(tables.fieldRecordIndices().size() == entries.size());

    Vector<uint32_t> numberOfEntriesOfRow(FillWith { }, entries.size(), 0u);
    for (uint32_t index = 0; index < entries.size(); ++index) {
        FieldTableEntry entry { entries[index] };
        if (entry.isEmpty())
            continue;
        AOT_TEST_CHECK(entry.name() <= index);
        ++numberOfEntriesOfRow[index - entry.name()];
    }

    BitVector rowsWithReader(entries.size());
    Vector<uint16_t> fieldOfName(FillWith { }, numberOfColumns, noField);
    uint32_t numberOfShapesWithEntries = 0;
    uint32_t numberOfRowsOfShapes = 0;
    for (uint32_t shape = 1; shape <= tables.numberOfFieldTableRows; ++shape) {
        uint32_t row = tables.fieldTableRowOf(shape);
        AOT_TEST_CHECK(row <= entries.size() && entries.size() - row >= numberOfColumns);
        std::span<const FieldData> fields;
        if (tables.hasLayoutForShape(shape))
            fields = tables.layoutForShape(shape).fields();
        uint32_t numberOfFieldsWithColumn = 0;
        for (const FieldData& field : fields) {
            if (!tables.hasFieldTableColumn(field.name))
                continue;
            ++numberOfFieldsWithColumn;
            fieldOfName[field.name] = tables.layoutForShape(shape).indexOf(field);
            AOT_TEST_CHECK(FieldTableEntry { entries[row + field.name] } == FieldTableEntry::forField(tables.layoutForShape(shape), field));
        }
        for (uint32_t name = 1; name < numberOfColumns; ++name)
            AOT_TEST_CHECK(findFieldInTable(tables, shape, name).value_or(noField) == fieldOfName[name]);
        for (const FieldData& field : fields) {
            if (tables.hasFieldTableColumn(field.name))
                fieldOfName[field.name] = noField;
        }
        AOT_TEST_CHECK(numberOfEntriesOfRow[row] == numberOfFieldsWithColumn);
        if (numberOfFieldsWithColumn) {
            ++numberOfShapesWithEntries;
            numberOfRowsOfShapes += !rowsWithReader.set(row);
        }
    }
    for (uint32_t row : tables.classMemberRows()) {
        if (row != ClassRowEntry::noRow)
            rowsWithReader.set(row);
    }
    for (uint32_t row = 0; row < entries.size(); ++row)
        AOT_TEST_CHECK(!numberOfEntriesOfRow[row] || rowsWithReader.get(row));
    AOT_TEST_CHECK(numberOfShapesWithEntries >= size.numberOfLayouts && numberOfRowsOfShapes < numberOfShapesWithEntries);
}

static Vector<uint8_t> bytesOfImageWithManyRowsInFieldTable(unsigned threadCount)
{
    TestBuild build(textOfProgramWithRecords(recordsThatShareNames), { .threadCount = threadCount });
    TestImage image = builtImage(build, bodies(plainBody(1)));
    checkFieldTableReadsBack(image.image->programTables(), recordsThatShareNames);
    return bytesOfImageForAddressZero(image);
}

static void testFieldTableOfManyShapes() { checkSameBytesOnThreadsOfTableGroups(bytesOfImageWithManyRowsInFieldTable); }

static CompiledCode bodyThatHoldsEveryTextKeyAsCell(const Typed::Program& program, uint32_t function)
{
    CompiledCode code = plainBody(function);
    MentionList mentions;
    for (unsigned index = 0; index < program.types.numberOfPropertyKeys(); ++index) {
        if (program.at(PropertyKeyID(index)).kind == PropertyKeyKind::Text)
            mentions.add(NumberSpace::KeyCell, PropertyKeyID(index));
    }
    mentions.moveTo(code.body);
    return code;
}

static Vector<uint8_t> bytesOfImageWithLongConstantTables(unsigned threadCount)
{
    TestBuild build(textOfProgramWithRecords(recordsWithOneName), { .threadCount = threadCount });
    TestImage image = builtImage(build, bodies(bodyThatHoldsEveryTextKeyAsCell(build.program(), 1)));
    const ProgramTables& tables = image.image->programTables();
    const IdentifierNumbering& identifiers = image.built.numbers->identifiers();

    std::span<const PropertyKeyID> keyCells = image.built.numbers->keyCells();
    AOT_TEST_CHECK(keyCells.size() > fewestSortedOnPoolForTableGroups && tables.keyCellIdentifiers().size() == keyCells.size());
    for (size_t place = 0; place < keyCells.size(); ++place)
        AOT_TEST_CHECK(tables.keyCellIdentifiers()[place] == identifiers.numberOf(keyCells[place]));

    IdentifierTable identifiersByHash(tables.identifiersByHash());
    size_t numberOfIdentifiersFoundByText = 0;
    for (uint32_t number = 1; number < identifiers.count(); ++number) {
        StringID text = identifiers.at(number).text;
        if (identifiers.numberOfText(text) != number)
            continue;
        ++numberOfIdentifiersFoundByText;
        std::optional<uint32_t> found = identifiersByHash.find(decodeForImage(build.program().strings[text]).hash, [&](uint32_t candidate) {
            return candidate == number;
        });
        AOT_TEST_CHECK(found == number);
    }
    AOT_TEST_CHECK(numberOfIdentifiersFoundByText > fewestSortedOnPoolForTableGroups);
    return bytesOfImageForAddressZero(image);
}

static void testLongConstantTables() { checkSameBytesOnThreadsOfTableGroups(bytesOfImageWithLongConstantTables); }

static constexpr ASCIILiteral textOfProgramWithThreeClasses = "module m0 path=\"main.js\" displayPath=\"main.js\" body=f0\n"
    "entry module=m0\n"
    "layout L0 kind=Instance name=\"Above\" constructor=f2\n"
    "layout L1 kind=Instance name=\"Below\" base=L0 constructor=f3 flags=[HasHeritage]\n"
    "layout L2 kind=Instance name=\"Old\" constructor=f4 flags=[IsConstructorFunction]\n"
    "function f0 name=\"main\" kind=Module results=[{type=anything}] {\n"
    "  b0():\n"
    "    v0 = Builtin builtin=GlobalThis\n"
    "    v1 = PropGet v0 key=\"sink\" [isFreeName]\n"
    "    v2 = NewClosure function=f1\n"
    "    v3 = Call v1, -, v2\n"
    "    v4 = NewClosure function=f2\n"
    "    v5 = NewClass v4 layout=L0\n"
    "    v6 = FinishClass v5 layout=L0\n"
    "    v7 = NewClosure function=f3\n"
    "    v8 = ResolveHeritage v6\n"
    "    v9 = NewClass v7, v6, v8 layout=L1\n"
    "    v10 = FinishClass v9 layout=L1\n"
    "    v11 = NewClosure function=f4\n"
    "    v12 = Construct v6\n"
    "    v13 = Construct v10\n"
    "    v14 = Construct v11\n"
    "    v15 = Call v1, -, v12, v13, v14\n"
    "    Return\n"
    "}\n"
    "function f1 name=\"f1\" results=[{type=anything}] {\n"
    "  b0():\n"
    "    Return\n"
    "}\n"
    "function f2 name=\"Above\" kind=BaseConstructor flags=[HasNewTarget, HasExplicitName] thisType={type=anything} results=[{type=object flags=[IsFromCompiler]}] constructedClass=L0 {\n"
    "  b0(v0: anything):\n"
    "    v1 = GetConstructPrototype v0 layout=L0\n"
    "    v2 = NewObject v1 layout=L0\n"
    "    Return v2\n"
    "}\n"
    "function f3 name=\"Below\" kind=DerivedConstructor flags=[HasNewTarget, TakesCallee, HasExplicitName] thisType={type=anything} results=[{type=object flags=[IsFromCompiler]}] constructedClass=L1 {\n"
    "  b0(v0: function, v1: anything):\n"
    "    v2 = GetSuperBase v0\n"
    "    v3 = SuperConstruct v2, v1 layout=L1\n"
    "    Return v3\n"
    "}\n"
    "function f4 name=\"Old\" flags=[HasThis, HasExplicitName] thisType={type=L2 flags=[IsExpected]} results=[{type=anything}] constructedClass=L2 {\n"
    "  b0(v0: L2):\n"
    "    v1 = Undefined\n"
    "    Return v1\n"
    "}\n"_s;

static void testWordsThatCountClassGuards()
{
    TestBuild build(makeString(textOfProgramWithThreeClasses, runtimeFunctionsText(5)), { });
    LayoutID above(0u);
    LayoutID below(1u);
    LayoutID old(2u);
    uint32_t note = build.program().strings.intern(StringView("note"_s)).index();

    std::array<ClassGuardKey, 3> guards {
        MentionList::keyOfClassGuardOfName(old, ClassSide::Prototype, NumberSpace::NameOfText, note),
        MentionList::keyOfClassGuard(below, ClassSide::Static),
        MentionList::keyOfClassGuardOfName(above, ClassSide::Prototype, NumberSpace::NameOfText, note, AskerOfClassGuardOfName::StoresAlone),
    };
    CompiledCode code = plainBody(1);
    MentionList mentions;
    uint32_t mentionOfThree = mentions.classGuardCount(guards);
    uint32_t mentionOfTwo = mentions.classGuardCount(std::span(guards).subspan(1));
    std::ranges::reverse(guards);
    AOT_TEST_CHECK(mentions.classGuardCount(guards) == mentionOfThree && mentionOfTwo != mentionOfThree);
    mentions.moveTo(code.body);
    Vector<uint32_t> keyOfThree(keyOf(code.body.mentions[mentionOfThree], code.body.mentionWords.span()));
    Vector<uint32_t> keyOfTwo(keyOf(code.body.mentions[mentionOfTwo], code.body.mentionWords.span()));

    TestImage image = builtImage(build, bodies(WTF::move(code)));
    const ImageNumbers& numbers = *image.built.numbers;
    const ProgramTables& tables = image.image->programTables();
    AOT_TEST_CHECK(build.statistic(StatisticID::ClassGuardCounts) == 2 && build.statistic(StatisticID::GuardsOfClassGuardCounts) == 5);

    auto offsetOfCount = [&](const Vector<uint32_t>& key) {
        std::optional<uint32_t> bit = numbers.numberOf(NumberSpace::ClassGuard, key.span());
        AOT_TEST_CHECK(bit && !(*bit % 32) && *bit / 8 + sizeof(uint32_t) <= tables.instanceSizes.sizeOfStatics);
        return *bit / 8;
    };
    uint32_t three = offsetOfCount(keyOfThree);
    uint32_t two = offsetOfCount(keyOfTwo);
    AOT_TEST_CHECK(three != two);

    std::span<const ClassGuardCountData> rows = tables.classGuardCounts();
    AOT_TEST_CHECK(rows.size() == 5 && std::ranges::is_sorted(rows));
    for (const ClassGuardKey& guard : guards) {
        std::optional<uint32_t> bit = numbers.numberOf(NumberSpace::ClassGuard, guard.span());
        AOT_TEST_CHECK(bit && std::ranges::contains(rows, ClassGuardCountData { *bit, three }));
        AOT_TEST_CHECK(std::ranges::contains(rows, ClassGuardCountData { *bit, two }) == (guard[0] != old.index()));
    }
}

std::span<const NativeTest> imageTableTests()
{
    static constexpr NativeTest tests[] = {
        { "image/format/varint-round-trip"_s, testVarintRoundTrip },
        { "image/check/seven-bytes"_s, testSevenBytes },
        { "image/check/thirty-one-bytes"_s, testThirtyOneBytes },
        { "image/check/header-cut"_s, testHeaderCut },
        { "image/check/wrong-magic"_s, testWrongMagic },
        { "image/check/other-stamp"_s, testOtherStamp },
        { "image/check/other-format-version"_s, testOtherFormatVersion },
        { "image/check/size-not-whole-pages"_s, testSizeNotWholePages },
        { "image/check/misaligned"_s, testMisaligned },
        { "image/check/other-identity"_s, testOtherIdentity },
        { "image/check/needs-cell-cage"_s, testNeedsCellCage },
        { "image/check/other-cage-shift"_s, testOtherCageShift },
        { "image/check/other-image-form"_s, testOtherImageForm },
        { "image/check/section-offset-unaligned"_s, testSectionOffsetUnaligned },
        { "image/check/section-size-not-whole-records"_s, testSectionSizeNotWholeRecords },
        { "image/check/function-count"_s, testFunctionCount },
        { "image/check/function-starts-in-groups"_s, testFunctionStartsInGroups },
        { "image/check/functions-past-code"_s, testFunctionsPastCode },
        { "image/check/too-few-granules"_s, testTooFewGranules },
        { "image/check/shared-code-ends-past-functions"_s, testSharedCodeEndsPastFunctions },
#if FFI_STUBS_SUPPORTED
        { "image/check/ffi-code-off-page-boundary"_s, testFFICodeOffPageBoundary },
#endif
        { "image/check/stubs-in-header"_s, testStubsInHeader },
        { "image/check/no-code"_s, testNoCode },
        { "image/locate/stub-return-sites"_s, testStubReturnSites },
#if defined(BUN_HARDENED)
        { "image/locate/words-in-the-place-of-a-return-address"_s, testWordsInThePlaceOfAReturnAddress },
#endif
        { "image/locate/regexp-code"_s, testRegExpCodePart },
        { "image/locate/constants-and-ffi-code"_s, testConstantsAndFFICode },
        { "image/stamp/stamp"_s, testStamp },
        { "image/stamp/runtime-description-names-format-version"_s, testRuntimeDescriptionNamesFormatVersion },
        { "image/stamp/runtime-description-names-platform-of-builtin-table"_s, testRuntimeDescriptionNamesPlatformOfBuiltinTable },
        { "image/stamp/runtime-facts-name-what-emitted-code-takes"_s, testRuntimeFactsNameWhatEmittedCodeTakes },
        { "image/admitted-code/needs-header-of-this-format"_s, testAdmittedCodeNeedsHeaderOfThisFormat },
        { "image/flags/need-header-of-this-format"_s, testFlagsNeedHeaderOfThisFormat },
        { "image/constants/constants-of-bodies-are-found-among-shared-constants"_s, testConstantsOfBodiesAreFoundAmongSharedConstants },
        { "image/field-table/many-shapes-read-back-on-any-number-of-threads"_s, testFieldTableOfManyShapes },
        { "image/constant-tables/long-lists-read-back-on-any-number-of-threads"_s, testLongConstantTables },
        { "image/statics/words-that-count-class-guards"_s, testWordsThatCountClassGuards },
    };
    return tests;
}

} // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
