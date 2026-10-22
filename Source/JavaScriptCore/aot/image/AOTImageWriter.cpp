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
#include "AOTImageText.h"
#include "AOTProgramTables.h"
#include "AOTSlotRepresentation.h"
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

struct TextRecords {
    explicit TextRecords(const ImageSections& sections)
        : strings(spanReinterpretCast<const ImageString>(sections[ImageSection::Strings].span()))
        , longStrings(spanReinterpretCast<const ImageLongString>(sections[ImageSection::LongStrings].span()))
        , identifiers(spanReinterpretCast<const ImageIdentifier>(sections[ImageSection::Identifiers].span()))
    {
    }

    std::span<const ImageString> strings;
    std::span<const ImageLongString> longStrings;
    std::span<const ImageIdentifier> identifiers;
};

struct StringHeaderNumbers {
#if ENABLE(AOT_POSITION_INDEPENDENT_IMAGE)
    uint64_t sizeOfHeaders() const { return static_cast<uint64_t>(count) * sizeof(ImageStringRecord); }
    uint64_t sizeOfList() const { return static_cast<uint64_t>(numberOfListedWords) * sizeof(uint32_t); }

    Vector<uint32_t> byString;
    Vector<uint32_t> listedWordIndexes;
    Vector<uint32_t> literalStringWords;
    uint32_t count { 0 };
    uint32_t numberOfListedWords { 0 };
};

} // anonymous namespace

static std::span<const uint32_t> findLiteralStrings(const ImageSections& sections)
{
    std::span<const uint8_t> tableBytes = sections[ImageSection::ProgramTables].span();
    RELEASE_ASSERT(tableBytes.size() >= sizeof(ProgramTables));
    return reinterpret_cast<const ProgramTables*>(tableBytes.data())->literalStrings();
}

static bool needsRecord(const TextRecords& text, const ImageString& string)
{
    return string.hasHeader() && string.range(text.longStrings).length;
}

static std::optional<StringHeaderNumbers> numberStringHeaders(const TextRecords& text, std::span<const uint32_t> literalStrings, BuildContext& context)
{
    StringHeaderNumbers numbers;
    numbers.byString.fill(0, text.strings.size());
    numbers.listedWordIndexes.fill(0, text.strings.size());
    numbers.literalStringWords.fill(0, text.strings.size());
    for (uint32_t number = 0; number < literalStrings.size(); ++number)
        numbers.literalStringWords[literalStrings[number]] = literalStringWord(number);
    uint64_t count = text.identifiers.size();
    uint64_t numberOfListedWords = 0;
    for (size_t index = 0; index < text.strings.size(); ++index) {
        const ImageString& string = text.strings[index];
        if (needsRecord(text, string) && (numbers.literalStringWords[index] || !ImageListedString::fitsOneWord(string.range(text.longStrings)))) {
            numbers.listedWordIndexes[index] = static_cast<uint32_t>(numberOfListedWords);
            numberOfListedWords += ImageListedString::numberOfWords(string.range(text.longStrings)) + !!numbers.literalStringWords[index];
        }
        if (!string.hasText() || string.hasIdentifierNumber())
            continue;
        if (string.hasHeader()) {
            numbers.byString[index] = static_cast<uint32_t>(count++);
            continue;
        }
        numbers.byString[index] = static_cast<uint32_t>(numberOfListedWords);
        numberOfListedWords += ImageListedString::numberOfWords(string.range(text.longStrings)) + string.hasListedHash();
    }
#else
    uint64_t sizeOfHeaders() const { return static_cast<uint64_t>(count) * sizeof(ImageStringHeader); }
    uint64_t sizeOfList() const { return static_cast<uint64_t>(numberOfListedWords) * sizeof(uint32_t); }

    // The number of the header of a string that is no identifier's text, or the first word in the image's list of a string without a header.
    Vector<uint32_t> byString;
    uint32_t count { 0 };
    uint32_t numberOfListedWords { 0 };
};

} // anonymous namespace

static std::optional<StringHeaderNumbers> numberStringHeaders(const TextRecords& text, BuildContext& context)
{
    StringHeaderNumbers numbers;
    numbers.byString.fill(0, text.strings.size());
    uint64_t count = text.identifiers.size();
    uint64_t numberOfListedWords = 0;
    for (size_t index = 0; index < text.strings.size(); ++index) {
        const ImageString& string = text.strings[index];
        if (!string.hasText() || string.hasIdentifierNumber())
            continue;
        if (string.hasHeader()) {
            numbers.byString[index] = static_cast<uint32_t>(count++);
            continue;
        }
        numbers.byString[index] = static_cast<uint32_t>(numberOfListedWords);
        numberOfListedWords += ImageListedString::numberOfWords(string.range(text.longStrings)) + string.hasListedHash();
    }
    // Not context.limit(): a test that lowers it means the list of long strings.
#endif
    static_assert(defaultLimitValue(LimitID::MaxListedStrings, hostArchitecture()) == ImageString::hashMask + 1);
    if (std::max(count, numberOfListedWords) > ImageString::hashMask + 1) {
        context.reportLimitExceeded(LimitID::MaxListedStrings, std::max(count, numberOfListedWords));
        return std::nullopt;
    }
    numbers.count = static_cast<uint32_t>(count);
    numbers.numberOfListedWords = static_cast<uint32_t>(numberOfListedWords);
    return numbers;
}

static std::optional<ImageExtent> computeImageExtent(const ImageSections& sections, const StringHeaderNumbers& stringHeaders, uint64_t codeSize, BuildContext& context)
{
    RELEASE_ASSERT(sections[ImageSection::StringHeaders].isEmpty());
    ImageExtent extent;
    uint64_t tablesEnd = sizeof(ImageHeader);
    for (unsigned index = 0; index < numberOfImageSections; ++index) {
        uint64_t size = sections.bytes[index].size();
        if (static_cast<ImageSection>(index) == ImageSection::StringHeaders)
            size = stringHeaders.sizeOfHeaders();
        else if (static_cast<ImageSection>(index) == ImageSection::Strings)
            size = size / sizeof(ImageString) * sizeof(ImageStringWord);
        else if (static_cast<ImageSection>(index) == ImageSection::LongStrings)
            size = stringHeaders.sizeOfList();
        else if (static_cast<ImageSection>(index) == ImageSection::Identifiers)
            size = size / sizeof(ImageIdentifier) * sizeof(ImageIdentifierWord);
        tablesEnd = WTF::roundUpToMultipleOf<imageSectionAlignment>(tablesEnd);
        if (static_cast<ImageSection>(index) == ImageSection::MethodTable && size)
            tablesEnd = WTF::roundUpToMultipleOf<imagePageSize>(tablesEnd);
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

static StatisticID statisticOf(ImageSection section)
{
    switch (section) {
    case ImageSection::Functions:
        return StatisticID::FunctionRecordBytesEmitted;
    case ImageSection::FunctionStarts:
        return StatisticID::FunctionStartBytesEmitted;
    case ImageSection::CodeGranules:
        return StatisticID::CodeGranuleBytesEmitted;
    case ImageSection::InnerEntries:
        return StatisticID::InnerEntryBytesEmitted;
    case ImageSection::FrameMaps:
        return StatisticID::FrameMapBytesEmitted;
    case ImageSection::ClosureWords:
        return StatisticID::ClosureWordBytesEmitted;
    case ImageSection::Positions:
        return StatisticID::SideTableBytesPositions;
    case ImageSection::FaultSites:
        return StatisticID::FaultSiteBytesEmitted;
    case ImageSection::SideTables:
        return StatisticID::SideTableBytesEmitted;
    case ImageSection::Strings:
    case ImageSection::LongStrings:
        return StatisticID::StringRecordBytesEmitted;
    case ImageSection::Text:
        return StatisticID::TextBytesEmitted;
    case ImageSection::Identifiers:
        return StatisticID::IdentifierBytesEmitted;
    case ImageSection::StringConstants:
        return StatisticID::StringConstantBytesEmitted;
    case ImageSection::StringHeaders:
        return StatisticID::StringHeaderBytesEmitted;
    case ImageSection::RegExps:
    case ImageSection::RegExpHashes:
    case ImageSection::RegExpDetails:
        return StatisticID::RegExpRecordBytesEmitted;
    case ImageSection::ProgramTables:
        return StatisticID::ProgramTableBytesEmitted;
    case ImageSection::Frames:
    case ImageSection::SharedCodeCopies:
    case ImageSection::WebAssemblyDirectory:
    case ImageSection::StubAddresses:
    case ImageSection::StubReturnSites:
    case ImageSection::SlotHandlers:
    case ImageSection::MethodLookups:
    case ImageSection::MethodTable:
        return StatisticID::OtherTableBytesEmitted;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The statistics of the sections add up to TableBytesEmitted: the header and the padding count as other bytes.
// CallSiteBytesEmitted and the other SideTableBytes are parts of SideTableBytesEmitted.
static void countTableBytes(const ImageExtent& extent, BuildContext& context)
{
    uint64_t sizeOfSections = 0;
    for (unsigned index = 0; index < numberOfImageSections; ++index) {
        context.statistics().count(statisticOf(static_cast<ImageSection>(index)), extent.sections[index].size);
        sizeOfSections += extent.sections[index].size;
    }
    context.statistics().count(StatisticID::OtherTableBytesEmitted, extent.codeOffset - sizeOfSections);
    context.statistics().count(StatisticID::TableBytesEmitted, extent.codeOffset);
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
    header.address = 0;
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
    for (unsigned index = 0; index < numberOfImageSections; ++index) {
        // writeStringHeaders() makes the three of the encoder's records.
        if (static_cast<ImageSection>(index) != ImageSection::Strings && static_cast<ImageSection>(index) != ImageSection::LongStrings && static_cast<ImageSection>(index) != ImageSection::Identifiers)
            addChunks(extent.sections[index].offset, inputs.sections.bytes[index].span());
    }
    addChunks(extent.codeOffset + layout.regExpCodeOffset, inputs.linker.layoutInputs.regExpSection.bytes.span());
    addChunks(extent.codeOffset + layout.webAssemblyCodeOffset, inputs.webAssemblyCode);
    addChunks(extent.codeOffset + layout.ffiCodeOffset, inputs.ffiCode);
    forEachIndexInParallel(loops, chunks.size(), [&](unsigned index) {
        memcpySpan(image.subspan(chunks[index].offset), chunks[index].source);
    });
}

template<typename T>
static std::span<T> sectionIn(std::span<uint8_t> image, const ImageExtent& extent, ImageSection section)
{
    ImageSectionRange range = extent.sections[static_cast<unsigned>(section)];
    return spanReinterpretCast<T>(image.subspan(range.offset, range.size));
}

#if ENABLE(AOT_POSITION_INDEPENDENT_IMAGE)
static void writeStringHeaders(std::span<uint8_t> image, const TextRecords& text, const StringHeaderNumbers& numbers, const ImageExtent& extent, const ParallelLoops& loops)
{
    std::span records = sectionIn<ImageStringRecord>(image, extent, ImageSection::StringHeaders);
    std::span stringsInImage = sectionIn<ImageStringWord>(image, extent, ImageSection::Strings);
    std::span listedStrings = sectionIn<uint32_t>(image, extent, ImageSection::LongStrings);
    std::span identifiersInImage = sectionIn<ImageIdentifierWord>(image, extent, ImageSection::Identifiers);
    for (size_t number = 0; number < text.identifiers.size(); ++number)
        identifiersInImage[number] = text.identifiers[number].inImage();
    forEachIndexInParallel(loops, static_cast<unsigned>(text.strings.size()), [&](unsigned index) {
        const ImageString& string = text.strings[index];
        stringsInImage[index] = string.inImage(numbers.byString[index]);
        if (!string.hasText())
            return;
        ImageLongString range = string.range(text.longStrings);
        if (!string.hasHeader()) {
            ImageListedString::write(listedStrings.subspan(numbers.byString[index]), range);
            if (string.hasListedHash())
                listedStrings[numbers.byString[index] + ImageListedString::numberOfWords(range)] = string.hashOrIdentifierNumber();
            return;
        }
        if (string.hasIdentifierNumber())
            RELEASE_ASSERT(text.identifiers[string.hashOrIdentifierNumber()].kind() == PropertyKeyKind::Text && text.identifiers[string.hashOrIdentifierNumber()].text() == StringID(index));
        if (!range.length)
            return;
        uint32_t literalStringWord = numbers.literalStringWords[index];
        uint32_t rangeOrIndex = numbers.listedWordIndexes[index] << ImageString::lengthBits;
        if (!literalStringWord && ImageListedString::fitsOneWord(range))
            ImageListedString::write(std::span { &rangeOrIndex, 1 }, range);
        else {
            std::span words = listedStrings.subspan(numbers.listedWordIndexes[index]);
            ImageListedString::write(words, range);
            if (literalStringWord)
                words[ImageListedString::numberOfWords(range)] = literalStringWord;
        }
        bool isIdentifier = string.hasIdentifierNumber();
        uint32_t number = isIdentifier ? string.hashOrIdentifierNumber() : numbers.byString[index];
        records[number] = ImageStringRecord::create(rangeOrIndex, isIdentifier ? text.identifiers[number].hashOrSymbol() : string.hashOrIdentifierNumber(), string.is16Bit(), isIdentifier);
        if (literalStringWord)
            records[number].hashAndFlags |= ImageStringRecord::isLiteralStringFlag;
#else
// A header that it does not write stays zero.
static void writeStringHeaders(std::span<uint8_t> image, const TextRecords& text, const StringHeaderNumbers& numbers, const ImageExtent& extent, const ParallelLoops& loops)
{
    std::span headers = sectionIn<ImageStringHeader>(image, extent, ImageSection::StringHeaders);
    std::span stringsInImage = sectionIn<ImageStringWord>(image, extent, ImageSection::Strings);
    std::span listedStrings = sectionIn<uint32_t>(image, extent, ImageSection::LongStrings);
    uint64_t offsetOfText = extent.sections[static_cast<unsigned>(ImageSection::Text)].offset;
    std::span identifiersInImage = sectionIn<ImageIdentifierWord>(image, extent, ImageSection::Identifiers);
    for (size_t number = 0; number < text.identifiers.size(); ++number)
        identifiersInImage[number] = text.identifiers[number].inImage();
    forEachIndexInParallel(loops, static_cast<unsigned>(text.strings.size()), [&](unsigned index) {
        const ImageString& string = text.strings[index];
        stringsInImage[index] = string.inImage(numbers.byString[index]);
        if (!string.hasText())
            return;
        ImageLongString range = string.range(text.longStrings);
        if (!string.hasHeader()) {
            ImageListedString::write(listedStrings.subspan(numbers.byString[index]), range);
            if (string.hasListedHash())
                listedStrings[numbers.byString[index] + ImageListedString::numberOfWords(range)] = string.hashOrIdentifierNumber();
            return;
        }
        uint64_t characters = offsetOfText + range.offset;
        if (string.hasIdentifierNumber()) {
            uint32_t number = string.hashOrIdentifierNumber();
            const ImageIdentifier& identifier = text.identifiers[number];
            RELEASE_ASSERT(identifier.kind() == PropertyKeyKind::Text && identifier.text() == StringID(index));
            // The atom with no characters is the engine's own.
            if (range.length)
                headers[number] = ImageStringHeader::create(characters, range.length, identifier.hashOrSymbol(), string.is16Bit(), StringImpl::StringAtom);
            return;
        }
        uint32_t number = numbers.byString[index];
        headers[number] = ImageStringHeader::create(characters, range.length, string.hashOrIdentifierNumber(), string.is16Bit(), StringImpl::StringNormal);
#endif
    });
}

// The sets of shapes lie at the start of the constants of the code, behind the tables: see takeShapeSets().
static void writeRangeOfShapeSets(std::span<uint8_t> image, const ImageWriterInputs& inputs, const ImageExtent& extent)
{
    uint32_t size = inputs.linker.constants.sizeOfSharedConstants;
    if (!size)
        return;
    ImageSectionRange section = extent.sections[static_cast<unsigned>(ImageSection::ProgramTables)];
    RELEASE_ASSERT(section.size >= sizeof(ProgramTables));
    auto& tables = *reinterpret_cast<ProgramTables*>(image.subspan(section.offset).data());
    uint64_t distance = extent.codeOffset + inputs.linker.layout.constantsOffset - section.offset;
    RELEASE_ASSERT(distance <= std::numeric_limits<uint32_t>::max() && !(distance % sizeof(uint64_t)));
    tables.ranges[static_cast<unsigned>(ProgramTableIndex::shapeSetBits)] = { static_cast<uint32_t>(distance), static_cast<uint32_t>(size / sizeof(uint64_t)) };
}

#if !ENABLE(AOT_POSITION_INDEPENDENT_IMAGE)
// Runs behind writeStringHeaders().
static void writeLiteralStringWords(std::span<uint8_t> image, const TextRecords& text, const StringHeaderNumbers& numbers, const ImageExtent& extent, const ImageSections& sections)
{
    std::span headers = sectionIn<ImageStringHeader>(image, extent, ImageSection::StringHeaders);
    RELEASE_ASSERT(!headers.empty() && !headers[0].length);
    headers[0].literalStringWord = static_cast<uint32_t>(headers.size_bytes());

    std::span<const uint8_t> tableBytes = sections[ImageSection::ProgramTables].span();
    RELEASE_ASSERT(tableBytes.size() >= sizeof(ProgramTables));
    std::span<const uint32_t> literalStrings = reinterpret_cast<const ProgramTables*>(tableBytes.data())->literalStrings();
    for (uint32_t number = 0; number < literalStrings.size(); ++number) {
        const ImageString& string = text.strings[literalStrings[number]];
        if (!string.hasText() || !string.hasHeader())
            continue;
        ImageStringHeader& header = headers[string.hasIdentifierNumber() ? string.hashOrIdentifierNumber() : numbers.byString[literalStrings[number]]];
        if (header.length)
            header.literalStringWord = literalStringWord(number);
    }
}
#endif

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

// The pages of an image are shared between processes: none is written, and none but those of the string headers holds an address (see ImageAddressPlaces).
// What the run time writes is memory of its own, which may take the place of pages that hold nothing.
static void checkImageIsReadOnly(std::span<const uint8_t> code, const ImageWriterInputs& inputs, BuildContext& context)
{
    for (const ReservedRange& range : inputs.reservedRanges) {
        if (!isEmptyPageRange(code, range))
            context.diagnostics().add(InternalError { .kind = InternalErrorKind::WritableSectionInImage, .count = clampTo<uint32_t>(range.offset) });
    }
}

Vector<uint8_t> writeImage(const ImageWriterInputs& inputs, BuildContext& context)
{
    uint64_t codeSize = inputs.linker.layout.codeSize;
    TextRecords text(inputs.sections);
#if ENABLE(AOT_POSITION_INDEPENDENT_IMAGE)
    std::span<const uint32_t> literalStrings = findLiteralStrings(inputs.sections);
    std::optional<StringHeaderNumbers> stringHeaders = numberStringHeaders(text, literalStrings, context);
#else
    std::optional<StringHeaderNumbers> stringHeaders = numberStringHeaders(text, context);
#endif
    if (!stringHeaders)
        return { };
    std::optional<ImageExtent> extent = computeImageExtent(inputs.sections, *stringHeaders, codeSize, context);
    if (!extent)
        return { };

    const ParallelLoops& loops = context.parallelLoops();
    Vector<uint8_t> image = allocateZeroedImage(extent->size, loops);
    writeHeader(*reinterpret_cast<ImageHeader*>(image.mutableSpan().data()), inputs, *extent, context);
    runTasksInParallel(loops, {
        [&] {
            copySections(image.mutableSpan(), inputs, *extent, loops);
            writeStringHeaders(image.mutableSpan(), text, *stringHeaders, *extent, loops);
#if !ENABLE(AOT_POSITION_INDEPENDENT_IMAGE)
            writeLiteralStringWords(image.mutableSpan(), text, *stringHeaders, *extent, inputs.sections);
#endif
            writeRangeOfShapeSets(image.mutableSpan(), inputs, *extent);
        },
        [&] { linkCode(inputs.linker, image.mutableSubspan(extent->codeOffset, codeSize), context); },
    });
    checkImageIsReadOnly(image.subspan(extent->codeOffset, codeSize), inputs, context);
    if (context.diagnostics().hasErrors())
        return { };
    countTableBytes(*extent, context);
    return image;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
