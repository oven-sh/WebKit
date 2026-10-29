/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTImageTestBodies.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <span>
#include <utility>
#include <wtf/MathExtras.h>
#include <wtf/StdLibExtras.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC::AOT::ImageTests {

namespace {

enum class EntryShape : uint8_t { AtStart, AfterPrefix, TwoEntries };

} // anonymous namespace

static constexpr std::array<unsigned, 3> threadCounts { 1, 2, 16 };
static constexpr unsigned mostThreads = threadCounts.back();
static constexpr uint32_t numberOfChainedBodies = 300;
static constexpr uint32_t numberOfLargeBodies = 64;
static constexpr unsigned pinnedUnitsInLargeBody = 16;
static constexpr std::array entryShapes { EntryShape::AtStart, EntryShape::AfterPrefix, EntryShape::TwoEntries };
static constexpr uint32_t prefixWord = 0x0105;
static constexpr uint16_t secondEntry = 1;
static_assert(numberOfLargeBodies * pinnedUnitsInLargeBody * wordsInPinnedUnit * sizeof(uint32_t) >= 3 * loweredSharedCodeReach);

// An image in memory holds addresses inside itself. Two images are compared as the compiler wrote them: for the address 0.
static Vector<uint8_t> copyBytes(const TestImage& image)
{
    Vector<uint8_t> bytes(image.bytes.span());
    relocateImage(bytes.mutableSpan(), 0);
    return bytes;
}

template<typename BuildBytes>
static void checkSameForAllThreadCounts(const BuildBytes& buildBytes)
{
    Vector<uint8_t> expected = buildBytes(threadCounts[0]);
    for (unsigned threadCount : std::span(threadCounts).subspan(1))
        AOT_TEST_CHECK(buildBytes(threadCount) == expected);
}

static Vector<uint8_t> mixedImageBytes(unsigned threadCount, Vector<CompiledCode>&& code = mixedBodies(numberOfMixedBodies))
{
    TestBuild build(numberOfMixedBodies + 1, { .threadCount = threadCount });
    TestImage image = builtImage(build, WTF::move(code));
    AOT_TEST_CHECK(build.statistic(StatisticID::JumpOnlyFunctionsRemoved));
    AOT_TEST_CHECK(build.statistic(StatisticID::FunctionsFolded));
    AOT_TEST_CHECK(build.statistic(StatisticID::BodiesOutlined));
    AOT_TEST_CHECK(build.statistic(StatisticID::JumpsToNextFunctionRemoved));
    AOT_TEST_CHECK(image.built.aliases.containsIf([](const EntryAlias& alias) { return alias.placement == AliasPlacement::AtPrimaryEntry; }));
    AOT_TEST_CHECK(image.built.aliases.containsIf([](const EntryAlias& alias) { return alias.placement == AliasPlacement::BeforeBody; }));
    return copyBytes(image);
}

static void testDeterminismThreads()
{
    checkSameForAllThreadCounts([](unsigned threadCount) {
        return mixedImageBytes(threadCount);
    });
}

static void testDeterminismTwoBuilds() { AOT_TEST_CHECK(mixedImageBytes(mostThreads) == mixedImageBytes(mostThreads)); }

static Vector<CompiledCode> shuffledMixedBodies()
{
    constexpr size_t stride = 11;
    static_assert(numberOfMixedBodies % stride);
    Vector<CompiledCode> ordered = mixedBodies(numberOfMixedBodies);
    Vector<CompiledCode> shuffled;
    shuffled.reserveInitialCapacity(ordered.size());
    for (size_t index = 0; index < ordered.size(); ++index)
        shuffled.append(WTF::move(ordered[index * stride % ordered.size()]));
    return shuffled;
}

static void testDeterminismInputOrder()
{
    Vector<uint8_t> expected = mixedImageBytes(mostThreads);
    Vector<CompiledCode> reversed = mixedBodies(numberOfMixedBodies);
    reversed.reverse();
    AOT_TEST_CHECK(mixedImageBytes(mostThreads, WTF::move(reversed)) == expected);
    AOT_TEST_CHECK(mixedImageBytes(mostThreads, shuffledMixedBodies()) == expected);
}

static bool isAllZero(std::span<const uint8_t> bytes)
{
    return std::ranges::all_of(bytes, [](uint8_t byte) {
        return !byte;
    });
}

static void copyHeaderMembers(ImageHeader& copy, const ImageHeader& header)
{
    static_assert(std::has_unique_object_representations_v<RecordedOptions> && std::has_unique_object_representations_v<ImageCounts> && std::has_unique_object_representations_v<ImageCodeRanges>);
    copy.magic = header.magic;
    copy.stamp = header.stamp;
    copy.identity = header.identity;
    copy.size = header.size;
    copy.codeOffset = header.codeOffset;
    copy.codeSize = header.codeSize;
    copy.address = header.address;
    copy.executableSize = header.executableSize;
    copy.formatVersion = header.formatVersion;
    copy.options = header.options;
    copy.heap.sizeClassProgression = header.heap.sizeClassProgression;
    copy.heap.markedBlockCageShift = header.heap.markedBlockCageShift;
    std::ranges::copy(header.sections, copy.sections);
    copy.counts = header.counts;
    copy.code = header.code;
    copy.flags = header.flags;
    std::ranges::copy(header.entriesUsed, copy.entriesUsed);
    std::ranges::copy(header.singleReturnSites, copy.singleReturnSites);
    copy.numberOfStubsFoundByKey = header.numberOfStubsFoundByKey;
}

static void testGapsAreZero()
{
    TestBuild build(numberOfMixedBodies + 1, { .threadCount = mostThreads });
    TestImage image = builtImage(build, mixedBodies(numberOfMixedBodies));
    std::span<const uint8_t> bytes = std::as_const(image.bytes).span();
    const ImageHeader& header = image.image->header();

    size_t end = sizeof(ImageHeader);
    for (const ImageSectionRange& range : header.sections) {
        AOT_TEST_CHECK(isAllZero(bytes.subspan(end, range.offset - end)));
        end = range.offset + range.size;
    }
    AOT_TEST_CHECK(isAllZero(bytes.subspan(end, header.codeOffset - end)));
    size_t endOfCode = header.codeOffset + header.codeSize;
    AOT_TEST_CHECK(isAllZero(bytes.subspan(endOfCode, header.size - endOfCode)));

    for (const StubAddressInImage& address : image.image->section<StubAddressInImage>(ImageSection::StubAddresses))
        AOT_TEST_CHECK(address.usesCallConvention <= 1);

    alignas(ImageHeader) std::array<uint8_t, sizeof(ImageHeader)> storage { };
    copyHeaderMembers(*reinterpret_cast<ImageHeader*>(storage.data()), header);
    AOT_TEST_CHECK(!memcmp(storage.data(), &header, sizeof(ImageHeader)));
}

static uint32_t calleeOf(uint32_t function) { return function % numberOfChainedBodies + 1; }
static EntryShape entryShapeOf(uint32_t function) { return entryShapes[function % entryShapes.size()]; }
static uint16_t calledEntryOf(uint32_t function) { return entryShapeOf(function) == EntryShape::TwoEntries ? secondEntry : primaryEntry; }

static CompiledCode chainedBody(uint32_t function)
{
    uint32_t callee = calleeOf(function);
    CodeSketch sketch(function);
    sketch.withFrame();
    if (entryShapeOf(function) == EntryShape::AfterPrefix) {
        sketch.pinned(prefixWord);
        sketch.code().body.entries[0].offset = sketch.size();
    }
    sketch.pinned(0x1000 + function);
    if (entryShapeOf(function) == EntryShape::TwoEntries)
        sketch.code().body.entries.append(CodeEntry { sketch.size(), secondEntry });
    sketch.callFunction(callee, sketch.addSite(), calledEntryOf(callee));
    sketch.end();
    return sketch.take();
}

static size_t startOf(const TestImage& image, uint32_t function) { return image.built.layout.bodyStarts[image.indexOf(function)]; }

static size_t calledAddressOf(const TestImage& image, uint32_t function)
{
    std::optional<uint32_t> offset = image.built.functions[image.indexOf(function)].body.offsetOfEntry(calledEntryOf(function));
    AOT_TEST_CHECK(offset);
    return startOf(image, function) + *offset;
}

static void checkLinked(const TestImage& image, Vector<uint8_t>& expectedCode, size_t start, std::span<const uint8_t> bytes, const Relocation& relocation, size_t target)
{
    size_t site = start + relocation.offset;
    memcpySpan(expectedCode.mutableSpan().subspan(site), bytes);
    AOT_TEST_CHECK(applyRelocation(expectedCode.mutableSpan(), start, relocation, target));
    AOT_TEST_CHECK(equalSpans(image.image->code().subspan(site, bytes.size()), expectedCode.subspan(site, bytes.size())));
}

static void checkChainedBody(const TestImage& image, uint32_t function, Vector<uint8_t>& expectedCode)
{
    uint32_t position = image.indexOf(function);
    const CodeBody& body = image.built.functions[position].body;
    size_t start = startOf(image, function);
    AOT_TEST_CHECK(!((start + body.primaryEntryOffset()) % imageFunctionAlignment));
    if (entryShapeOf(function) == EntryShape::AfterPrefix) {
        AOT_TEST_CHECK(body.primaryEntryOffset() == sizeof(prefixWord));
        AOT_TEST_CHECK(equalSpans(image.image->code().subspan(start, sizeof(prefixWord)), asByteSpan(prefixWord)));
    }

    AOT_TEST_CHECK(image.built.layout.veneersByBody[position].size() == 1);
    const RelocatableCode& veneerTemplate = testTemplates().veneer;
    size_t veneer = image.built.layout.firstVeneerOffset(position, body.bytes.size());
    AOT_TEST_CHECK(veneer >= start + body.bytes.size());
    checkLinked(image, expectedCode, veneer, veneerTemplate.bytes.span(), veneerTemplate.relocations[0], calledAddressOf(image, calleeOf(function)));

    AOT_TEST_CHECK(body.relocations.size() == 1);
    checkLinked(image, expectedCode, start, testTemplates().nearCall.span(), body.relocations[0], veneer);
}

static Vector<uint8_t> chainedImageBytes(unsigned threadCount)
{
    TestBuild build(numberOfChainedBodies + 1, { .loweredLimit = LimitID::FunctionCallReach, .loweredLimitValue = 0, .threadCount = threadCount });
    Vector<CompiledCode> code;
    for (uint32_t function = 1; function <= numberOfChainedBodies; ++function)
        code.append(chainedBody(function));
    TestImage image = builtImage(build, WTF::move(code));
    AOT_TEST_CHECK(image.built.functions.size() == numberOfChainedBodies);
    AOT_TEST_CHECK(build.statistic(StatisticID::Veneers) == numberOfChainedBodies);

    Vector<uint8_t> expectedCode(FillWith { }, image.image->code().size(), 0);
    for (uint32_t function = 1; function <= numberOfChainedBodies; ++function)
        checkChainedBody(image, function, expectedCode);
    return copyBytes(image);
}

static void testVeneers() { checkSameForAllThreadCounts(chainedImageBytes); }

static CompiledCode largeBody(uint32_t function)
{
    CodeSketch sketch = framedSketch(function);
    for (unsigned unit = 0; unit < pinnedUnitsInLargeBody; ++unit)
        sketch.pinned(function, wordsInPinnedUnit);
    sketch.callStub(writeBarrier(), sketch.addSite());
    sketch.end();
    return sketch.take();
}

static Vector<uint8_t> largeImageBytes(unsigned threadCount)
{
    TestOptions options = sharedCodeReachOptions();
    options.threadCount = threadCount;
    TestBuild build(numberOfLargeBodies + 1, options);
    Vector<CompiledCode> code;
    for (uint32_t function = 1; function <= numberOfLargeBodies; ++function)
        code.append(largeBody(function));
    TestImage image = builtImage(build, WTF::move(code));
    AOT_TEST_CHECK(image.built.functions.size() == numberOfLargeBodies);

    std::span<const uint32_t> copies = image.image->sharedCodeCopies();
    AOT_TEST_CHECK(image.built.layout.sharedCodeSize < loweredSharedCodeReach);
    AOT_TEST_CHECK(copies.size() > 1 && copies.size() < numberOfLargeBodies);
    AOT_TEST_CHECK(copies.size() == image.built.layout.sharedCodeCopies.size());
    AOT_TEST_CHECK(build.statistic(StatisticID::StubCopiesEmitted) == copies.size());
    AOT_TEST_CHECK(!build.statistic(StatisticID::Veneers));
    return copyBytes(image);
}

static void testSharedCodeCopies() { checkSameForAllThreadCounts(largeImageBytes); }

std::span<const NativeTest> determinismTests()
{
    static constexpr NativeTest tests[] = {
        { "image/determinism/threads"_s, testDeterminismThreads },
        { "image/determinism/two-builds"_s, testDeterminismTwoBuilds },
        { "image/determinism/input-order"_s, testDeterminismInputOrder },
        { "image/determinism/gaps-are-zero"_s, testGapsAreZero },
        { "image/determinism/veneers"_s, testVeneers },
        { "image/determinism/shared-code-copies"_s, testSharedCodeCopies },
    };
    return tests;
}

} // namespace JSC::AOT::ImageTests

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
