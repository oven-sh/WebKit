/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTImageBuilder.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCodeLinker.h"
#include "AOTImageStamp.h"
#include "AOTThreads.h"
#include "MarkedBlockCage.h"
#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <type_traits>
#include <wtf/CheckedArithmetic.h>
#include <wtf/MathExtras.h>
#include <wtf/StdLibExtras.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

static constexpr size_t bytesPerChunk = MB;

template<typename T>
static constexpr bool hasNoPadding = std::has_unique_object_representations_v<T>;

namespace {

struct ImageExtent {
    std::array<ImageSectionRange, numberOfImageSections> sections;
    uint64_t codeOffset { 0 };
    uint64_t size { 0 };
};

struct CopyRange {
    size_t offset;
    std::span<const uint8_t> source;
};

} // anonymous namespace

static std::optional<ImageExtent> computeImageExtent(const ImageSections& sections, uint64_t codeSize, BuildContext& context)
{
    ImageExtent extent;
    uint64_t tablesEnd = sizeof(ImageHeader);
    for (unsigned index = 0; index < numberOfImageSections; ++index) {
        size_t size = sections.bytes[index].size();
        tablesEnd = WTF::roundUpToMultipleOf<imageSectionAlignment>(tablesEnd);
        // An offset that does not fit is never read: the image is then too large for one of the two tests below.
        extent.sections[index] = { static_cast<uint32_t>(tablesEnd), static_cast<uint32_t>(size) };
        tablesEnd += size;
    }
    if (tablesEnd > context.limit(LimitID::TableBytes)) {
        context.reportLimitExceeded(LimitID::TableBytes, tablesEnd);
        return std::nullopt;
    }
    extent.codeOffset = WTF::roundUpToMultipleOf<imagePageSize>(tablesEnd);
    extent.size = WTF::roundUpToMultipleOf<imagePageSize>(extent.codeOffset + codeSize);
    static_assert(WTF::isValidCapacityForVector<uint8_t>(defaultLimitValue(LimitID::VectorBytes, hostArchitecture())));
    if (extent.size > context.limit(LimitID::VectorBytes)) {
        context.reportLimitExceeded(LimitID::VectorBytes, extent.size);
        return std::nullopt;
    }
    return extent;
}

static Vector<uint8_t> allocateZeroedImage(size_t size, const ParallelLoops& loops)
{
    Vector<uint8_t> image;
    image.grow(size);
    forEachIndexInParallel(loops, static_cast<unsigned>((image.size() + bytesPerChunk - 1) / bytesPerChunk), [&](unsigned i) {
        size_t start = i * bytesPerChunk;
        zeroSpan(image.mutableSpan().subspan(start, std::min(bytesPerChunk, image.size() - start)));
    });
    return image;
}

static void writeCodeRanges(ImageCodeRanges& code, const ImageWriterInputs& inputs)
{
    const CodeLayout& layout = inputs.linker.layout;
    code.functionsEnd = layout.functionsEnd;
    code.codeGranuleShift = layout.codeGranuleShift;
    code.sharedCodeSize = layout.sharedCodeSize;
    code.regExpCodeOffset = layout.regExpCodeOffset;
    code.webAssemblyCodeOffset = layout.webAssemblyCodeOffset;
    code.webAssemblyCodeSize = safeCast<uint32_t>(inputs.webAssemblyCode.size());
    code.constantsOffset = layout.constantsOffset;
    code.constantsSize = safeCast<uint32_t>(inputs.linker.constants.bytes.size());
    code.ffiCodeOffset = layout.ffiCodeOffset;
    code.ffiCodeSize = safeCast<uint32_t>(inputs.ffiCode.size());
}

static void writeHeader(ImageHeader& header, const ImageWriterInputs& inputs, const ImageExtent& extent, BuildContext& context)
{
    static_assert(hasNoPadding<RecordedOptions> && hasNoPadding<ImageSectionRange> && hasNoPadding<ImageCounts> && hasNoPadding<ImageCodeRanges>, "padding in a member assigned as a whole would make the bytes of the image differ from build to build");
    static_assert(sizeof(ImageHeapOptions) == sizeof(double) + 2 * sizeof(uint32_t), "a double has no unique object representation, so the sizes say that there is no padding");
    header.magic = imageMagic;
    header.identity = context.options().executableIdentity;
    header.stamp = imageStamp(context.targetFeatures(), header.identity);
    header.size = extent.size;
    header.codeOffset = extent.codeOffset;
    header.codeSize = inputs.linker.layout.codeSize;
    header.options = context.recordedOptions();
    header.heap = { context.options().sizeClassProgression, MarkedBlockCage::shift, 0 };
    for (unsigned index = 0; index < numberOfImageSections; ++index)
        header.sections[index] = extent.sections[index];
    header.counts = inputs.counts;
    writeCodeRanges(header.code, inputs);
    header.flags = inputs.flags.toRaw();
    for (size_t entry : inputs.entriesUsed)
        header.entriesUsed[entry / 32] |= 1u << entry % 32;
}

static void copySections(std::span<uint8_t> image, const ImageWriterInputs& inputs, const ImageExtent& extent, const ParallelLoops& loops)
{
    const CodeLayout& layout = inputs.linker.layout;
    Vector<CopyRange> chunks;
    auto addChunks = [&](uint64_t offset, std::span<const uint8_t> source) {
        RELEASE_ASSERT(isSumSmallerThanOrEqual<uint64_t>(offset, source.size(), image.size()));
        for (size_t start = 0; start < source.size(); start += bytesPerChunk)
            chunks.append({ static_cast<size_t>(offset) + start, source.subspan(start, std::min(bytesPerChunk, source.size() - start)) });
    };
    for (unsigned index = 0; index < numberOfImageSections; ++index)
        addChunks(extent.sections[index].offset, inputs.sections.bytes[index].span());
    addChunks(extent.codeOffset + layout.regExpCodeOffset, inputs.linker.layoutInputs.regExpSection.bytes.span());
    addChunks(extent.codeOffset + layout.webAssemblyCodeOffset, inputs.webAssemblyCode);
    addChunks(extent.codeOffset + layout.ffiCodeOffset, inputs.ffiCode);
    forEachIndexInParallel(loops, chunks.size(), [&](unsigned index) {
        memcpySpan(image.subspan(chunks[index].offset), chunks[index].source);
    });
}

static bool isEmptyPageRange(std::span<const uint8_t> code, const ReservedRange& range)
{
    if (range.offset % imagePageSize || range.size % imagePageSize)
        return false;
    if (!isSumSmallerThanOrEqual<uint64_t>(range.offset, range.size, code.size()))
        return false;
    return std::ranges::all_of(code.subspan(range.offset, range.size), [](uint8_t byte) {
        return !byte;
    });
}

// The pages of an image are shared between processes: none is written, and none holds an address. What the run time writes is memory of its own, which may take the place of pages that hold nothing.
static void checkImageIsReadOnly(std::span<const uint8_t> code, const ImageWriterInputs& inputs, BuildContext& context)
{
    for (const ReservedRange& range : inputs.reservedRanges) {
        if (!isEmptyPageRange(code, range))
            context.diagnostics().add(InternalError { .kind = InternalErrorKind::WritableSectionInImage, .count = clampTo<uint32_t>(range.offset) });
    }
    if (context.validates())
        validateNoMappedAddress(code.subspan(inputs.linker.layout.constantsOffset, inputs.linker.constants.bytes.size()), context);
}

Vector<uint8_t> writeImage(const ImageWriterInputs& inputs, BuildContext& context)
{
    uint64_t codeSize = inputs.linker.layout.codeSize;
    std::optional<ImageExtent> extent = computeImageExtent(inputs.sections, codeSize, context);
    if (!extent)
        return { };

    const ParallelLoops& loops = context.parallelLoops();
    Vector<uint8_t> image = allocateZeroedImage(extent->size, loops);
    writeHeader(*reinterpret_cast<ImageHeader*>(image.mutableSpan().data()), inputs, *extent, context);
    runTasksInParallel(loops, {
        [&] { copySections(image.mutableSpan(), inputs, *extent, loops); },
        [&] { linkCode(inputs.linker, image.mutableSubspan(extent->codeOffset, codeSize), context); },
    });
    checkImageIsReadOnly(image.subspan(extent->codeOffset, codeSize), inputs, context);
    if (context.diagnostics().hasErrors())
        return { };
    context.statistics().count(StatisticID::TableBytesEmitted, extent->codeOffset);
    return image;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
