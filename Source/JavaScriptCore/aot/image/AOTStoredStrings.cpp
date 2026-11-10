/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStoredStrings.h"

#if ENABLE(AOT)

#include "AOTImageFormat.h"
#include "AOTImageText.h"
#include "AOTProgramTables.h"
#include "AOTSchemaPackedRecords.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <wtf/StdLibExtras.h>
#include <wtf/StringPrintStream.h>
#include <wtf/text/CString.h>
#include <wtf/text/StringView.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

namespace {

static constexpr size_t smallestAlignmentOfImageInFile = 512;
static constexpr uint64_t smallestPageOfImage = 16 * KB;
static constexpr size_t sizeOfHeaderInEveryFormat = offsetof(ImageHeader, options);

template<typename Record>
class RecordsInFile {
public:
    RecordsInFile(std::span<const uint8_t> bytes)
        : m_bytes(bytes)
    {
    }

    size_t size() const { return m_bytes.size() / sizeof(Record); }

    Record operator[](size_t index) const
    {
        Record record;
        memcpy(&record, m_bytes.data() + index * sizeof(Record), sizeof(Record));
        return record;
    }

private:
    std::span<const uint8_t> m_bytes;
};

struct RangeOfBytes {
    uint64_t begin;
    uint64_t end;
};

class ReaderOfImage {
public:
    ReaderOfImage(std::span<const uint8_t> file, size_t offsetInFile, unsigned number, StoredStrings& result, DecompressSourceFunction decompress)
        : m_image(file.subspan(offsetInFile))
        , m_offsetInFile(offsetInFile)
        , m_number(number)
        , m_result(result)
        , m_decompress(decompress)
    {
    }

    VerdictOnStoredStrings read();

private:
    template<typename... Parts>
    VerdictOnStoredStrings refuse(const Parts&... parts)
    {
        report(parts...);
        return VerdictOnStoredStrings::CannotBeRead;
    }

    template<typename... Parts>
    void report(const Parts&... parts)
    {
        StringPrintStream line;
        line.print("image ", m_number, " | ", parts..., "\n");
        m_result.report.append(byteCast<uint8_t>(line.toUTF8CString().span()));
    }

    std::optional<std::span<const uint8_t>> section(ImageSection) const;
    std::optional<ImageLongString> listedString(RecordsInFile<uint32_t>, uint32_t at) const;
    VerdictOnStoredStrings readTextsOfProgramTables();
    void appendString(std::span<const uint8_t> utf8);
    void appendLatin1String(std::span<const uint8_t>);
    VerdictOnStoredStrings reportSection(ASCIILiteral name, uint64_t offsetInImage, std::span<const uint8_t> bytes, Vector<RangeOfBytes>& covered);

    std::span<const uint8_t> m_image;
    size_t m_offsetInFile;
    unsigned m_number;
    StoredStrings& m_result;
    DecompressSourceFunction m_decompress;
    ImageHeader m_header;
};

} // anonymous namespace

std::optional<std::span<const uint8_t>> ReaderOfImage::section(ImageSection which) const
{
    ImageSectionRange range = m_header.sections[static_cast<unsigned>(which)];
    if (static_cast<uint64_t>(range.offset) + range.size > m_header.size)
        return std::nullopt;
    return m_image.subspan(range.offset, range.size);
}

std::optional<ImageLongString> ReaderOfImage::listedString(RecordsInFile<uint32_t> words, uint32_t at) const
{
    if (at >= words.size())
        return std::nullopt;
    std::array<uint32_t, ImageListedString::maxNumberOfWords> copy { words[at], 0, 0 };
    if (!(copy[0] & ImageString::lengthMask)) {
        if (at + copy.size() > words.size())
            return std::nullopt;
        for (unsigned index = 1; index < copy.size(); ++index)
            copy[index] = words[at + index];
    }
    return ImageListedString::read(copy);
}

void ReaderOfImage::appendString(std::span<const uint8_t> utf8)
{
    m_result.strings.append(utf8);
    m_result.strings.append(0);
}

void ReaderOfImage::appendLatin1String(std::span<const uint8_t> characters)
{
    appendString(byteCast<uint8_t>(StringView(byteCast<Latin1Character>(characters)).utf8().span()));
}

VerdictOnStoredStrings ReaderOfImage::reportSection(ASCIILiteral name, uint64_t offsetInImage, std::span<const uint8_t> bytes, Vector<RangeOfBytes>& covered)
{
    std::ranges::sort(covered, [](const RangeOfBytes& a, const RangeOfBytes& b) {
        return a.begin < b.begin;
    });
    covered.append({ bytes.size(), bytes.size() });
    uint64_t numberOfBytesBesideStrings = 0;
    std::optional<uint64_t> firstThatIsNotZero;
    uint64_t end = 0;
    for (const RangeOfBytes& range : covered) {
        for (uint64_t at = end; at < range.begin; ++at) {
            ++numberOfBytesBesideStrings;
            if (bytes[at] && !firstThatIsNotZero)
                firstThatIsNotZero = at;
        }
        end = std::max(end, range.end);
    }
    uint64_t offsetInFile = m_offsetInFile + offsetInImage;
    report(name, " | offset ", offsetInFile, " | length ", bytes.size(), " | ", numberOfBytesBesideStrings, " bytes that no string covers");
    if (!firstThatIsNotZero)
        return VerdictOnStoredStrings::NothingLiesBesideStrings;
    report(name, " | the byte at offset ", offsetInFile + *firstThatIsNotZero, " is covered by no string and is not zero");
    return VerdictOnStoredStrings::BytesBesideStringsAreNotZero;
}

VerdictOnStoredStrings ReaderOfImage::read()
{
    uint32_t formatVersion;
    memcpy(&formatVersion, m_image.data() + offsetof(ImageHeader, formatVersion), sizeof(formatVersion));
    if (formatVersion != imageFormatVersion)
        return refuse("this Bun reads format ", imageFormatVersion, "; the image says ", formatVersion);
    if (m_image.size() < sizeof(ImageHeader))
        return refuse("the file ends inside its header");
    memcpy(&m_header, m_image.data(), sizeof(ImageHeader));

    auto bytesOfWords = section(ImageSection::Strings);
    auto bytesOfListedStrings = section(ImageSection::LongStrings);
    auto bytesOfHeaders = section(ImageSection::StringHeaders);
    auto text = section(ImageSection::Text);
    if (!bytesOfWords || !bytesOfListedStrings || !bytesOfHeaders || !text)
        return refuse("a section lies outside the image");
    RecordsInFile<ImageStringWord> words(*bytesOfWords);
    RecordsInFile<uint32_t> listedStrings(*bytesOfListedStrings);
    RecordsInFile<ImageStringHeader> headers(*bytesOfHeaders);
    RecordsInFile<ImageStringRecord> records(*bytesOfHeaders);
    bool hasRecords = m_header.options.form() == ImageForm::PositionIndependent;
    uint64_t addressOfText = m_header.address + m_header.sections[static_cast<unsigned>(ImageSection::Text)].offset;

    Vector<RangeOfBytes> covered;
    for (size_t index = 0; index < words.size(); ++index) {
        ImageStringWord word = words[index];
        if (!word.hasText())
            continue;
        std::optional<ImageLongString> range = ImageLongString { 0, 0 };
        if (!word.hasHeader())
            range = listedString(listedStrings, word.number());
        else if (word.number() >= (hasRecords ? records.size() : headers.size()))
            range = std::nullopt;
        else if (hasRecords) {
            ImageStringRecord record = records[word.number()];
            if (uint32_t length = record.rangeOrIndex & ImageString::lengthMask)
                range = ImageLongString { record.rangeOrIndex >> ImageString::lengthBits, length };
            else if (record.hasText())
                range = listedString(listedStrings, record.rangeOrIndex >> ImageString::lengthBits);
        } else if (ImageStringHeader header = headers[word.number()]; header.length) {
            uint64_t offset = header.characters - addressOfText;
            range = offset <= text->size() ? std::optional(ImageLongString { static_cast<uint32_t>(offset), header.length }) : std::nullopt;
        }
        uint64_t sizeOfUnit = word.is16Bit() ? sizeof(char16_t) : sizeof(Latin1Character);
        if (!range || range->offset + range->length * sizeOfUnit > text->size())
            return refuse("string ", index, " lies outside the section of text");
        if (!range->length)
            continue;
        std::span<const uint8_t> bytes = text->subspan(range->offset, range->length * sizeOfUnit);
        covered.append({ range->offset, range->offset + bytes.size() });
        if (word.is16Bit()) {
            Vector<char16_t> units(range->length);
            memcpy(units.mutableSpan().data(), bytes.data(), bytes.size());
            appendString(byteCast<uint8_t>(StringView(units.span()).utf8().span()));
        } else
            appendLatin1String(bytes);
    }
    VerdictOnStoredStrings verdictOnText = reportSection("Text"_s, m_header.sections[static_cast<unsigned>(ImageSection::Text)].offset, *text, covered);
    return std::max(verdictOnText, readTextsOfProgramTables());
}

VerdictOnStoredStrings ReaderOfImage::readTextsOfProgramTables()
{
    auto bytes = section(ImageSection::ProgramTables);
    if (!bytes)
        return refuse("a section lies outside the image");
    if (bytes->size() < sizeof(ProgramTables))
        return VerdictOnStoredStrings::NothingLiesBesideStrings;
    static constexpr size_t wordsBehindLastRow = 4;
    Vector<uint64_t> aligned(bytes->size() / sizeof(uint64_t) + 1 + wordsBehindLastRow);
    zeroSpan(aligned.mutableSpan());
    memcpy(aligned.mutableSpan().data(), bytes->data(), bytes->size());
    auto& tables = *reinterpret_cast<const ProgramTables*>(aligned.span().data());
    if (tables.magic != ProgramTables::expectedMagic)
        return refuse("the tables of the program do not begin as they should");
    uint64_t offsetOfTables = m_header.sections[static_cast<unsigned>(ImageSection::ProgramTables)].offset;
    auto liesInTables = [&](uint64_t offset, uint64_t size) {
        return offset + size <= bytes->size();
    };
    auto tableLiesInTables = [&](ProgramTableIndex index, size_t sizeOfElement) {
        TableRange range = tables.ranges[static_cast<unsigned>(index)];
        return liesInTables(range.offset, static_cast<uint64_t>(range.count) * sizeOfElement);
    };
    if (!tableLiesInTables(ProgramTableIndex::quotedSourceGroups, sizeof(QuotedSourceGroupData)) || !tableLiesInTables(ProgramTableIndex::quotedSourceRows, 1)
        || !tableLiesInTables(ProgramTableIndex::quotedSourceText, 1) || !tableLiesInTables(ProgramTableIndex::quotedSourceBlocks, sizeof(uint32_t)) || !tableLiesInTables(ProgramTableIndex::functionSources, sizeof(FunctionSources)))
        return refuse("a table of function sources or quoted sources lies outside the program tables");

    auto decompressBlocks = [&](Vector<uint8_t>& decompressed, const uint8_t* compressed, std::span<const uint32_t> ends, uint32_t sourceSize) {
        decompressed.grow(sourceSize);
        for (size_t block = 0; block < ends.size(); ++block) {
            uint32_t first = block ? ends[block - 1] : 0;
            size_t offset = block * sourceBlockSize;
            if (!m_decompress(decompressed.mutableSpan().subspan(offset, std::min<size_t>(sourceBlockSize, decompressed.size() - offset)), { compressed + first, ends[block] - first }))
                return false;
        }
        return true;
    };

    VerdictOnStoredStrings worst = VerdictOnStoredStrings::NothingLiesBesideStrings;
    std::span<const uint8_t> quotedSourceText = tables.quotedSourceText();
    Vector<uint8_t> decompressedQuotedSources;
    if (std::span<const uint32_t> blocks = tables.quotedSourceBlocks(); !blocks.empty()) {
        std::span<const uint32_t> ends = blocks.subspan(1);
        if (!m_decompress || ends.empty() || ends.size() != (static_cast<uint64_t>(blocks[0]) + sourceBlockSize - 1) / sourceBlockSize || !std::ranges::is_sorted(ends) || ends.back() != quotedSourceText.size()
            || !decompressBlocks(decompressedQuotedSources, quotedSourceText.data(), ends, blocks[0]))
            return refuse("the quoted source text is compressed and cannot be decompressed");
        quotedSourceText = decompressedQuotedSources.span();
    }
    if (std::span<const uint8_t> text = quotedSourceText; !text.empty()) {
        std::span<const QuotedSourceGroupData> groups = tables.quotedSourceGroups();
        std::span<const uint8_t> rows = tables.quotedSourceRows();
        Vector<RangeOfBytes> covered;
        for (size_t index = 0; index < groups.size(); ++index) {
            uint64_t first = groups[index].offsetOfRows;
            uint64_t end = index + 1 < groups.size() ? groups[index + 1].offsetOfRows : rows.size();
            if (first > end || end > rows.size())
                return refuse("the quoted source rows lie outside their table");
            const uint8_t* cursor = rows.data() + first;
            int64_t start = 0;
            while (cursor < rows.data() + end) {
                QuotedSourceRow row = QuotedSourceRow::read(cursor);
                start += row.startChange;
                if (start < 0 || static_cast<uint64_t>(start) + row.length > text.size())
                    return refuse("a quoted source lies outside the quoted source text");
                if (!row.length)
                    continue;
                appendLatin1String(text.subspan(static_cast<size_t>(start), row.length));
                covered.append({ static_cast<uint64_t>(start), static_cast<uint64_t>(start) + row.length });
            }
        }
        worst = std::max(worst, reportSection("quoted text"_s, offsetOfTables + tables.ranges[static_cast<unsigned>(ProgramTableIndex::quotedSourceText)].offset, text, covered));
    }
    for (const FunctionSources& sources : tables.functionSources()) {
        if (!sources.numberOfEntries)
            continue;
        if (!liesInTables(sources.entriesOffset, static_cast<uint64_t>(sources.numberOfEntries) * sizeof(FunctionSourceEntry)))
            return refuse("the function source entries lie outside the program tables");
        std::span entries { tables.at<FunctionSourceEntry>(sources.entriesOffset), sources.numberOfEntries };
        uint64_t size = 0;
        for (const FunctionSourceEntry& entry : entries)
            size = std::max<uint64_t>(size, entry.end);
        Vector<uint8_t> decompressedSource;
        if (sources.numberOfBlocks) {
            uint64_t expectedBlockCount = (static_cast<uint64_t>(sources.sourceSize) + sourceBlockSize - 1) / sourceBlockSize;
            if (!m_decompress || size > sources.sourceSize || sources.numberOfBlocks != expectedBlockCount || !liesInTables(sources.blockEndsOffset, static_cast<uint64_t>(sources.numberOfBlocks) * sizeof(uint32_t)))
                return refuse("the function sources are compressed and cannot be decompressed");
            std::span ends { tables.at<uint32_t>(sources.blockEndsOffset), sources.numberOfBlocks };
            if (!std::ranges::is_sorted(ends) || !liesInTables(sources.sourceOffset, ends.back()))
                return refuse("the function sources lie outside the program tables");
            if (!decompressBlocks(decompressedSource, tables.at<uint8_t>(sources.sourceOffset), ends, sources.sourceSize))
                return refuse("the function sources are compressed and cannot be decompressed");
        } else if (!liesInTables(sources.sourceOffset, size))
            return refuse("the function sources lie outside the program tables");
        std::span<const uint8_t> text = sources.numberOfBlocks ? decompressedSource.span().first(static_cast<size_t>(size)) : std::span<const uint8_t> { tables.at<uint8_t>(sources.sourceOffset), static_cast<size_t>(size) };
        Vector<RangeOfBytes> covered;
        for (const FunctionSourceEntry& entry : entries) {
            uint32_t start = entry.start;
            uint32_t end = entry.end;
            if (start >= end)
                continue;
            appendString(text.subspan(start, end - start));
            covered.append({ start, end });
        }
        worst = std::max(worst, reportSection("function text"_s, offsetOfTables + sources.sourceOffset, text, covered));
    }
    return worst;
}

static bool isHeaderOfImage(std::span<const uint8_t> file, size_t at)
{
    if (at % smallestAlignmentOfImageInFile || file.size() - at < sizeOfHeaderInEveryFormat)
        return false;
    uint64_t size;
    uint64_t codeOffset;
    memcpy(&size, file.data() + at + offsetof(ImageHeader, size), sizeof(size));
    memcpy(&codeOffset, file.data() + at + offsetof(ImageHeader, codeOffset), sizeof(codeOffset));
    return size && !(size % smallestPageOfImage) && codeOffset && !(codeOffset % smallestPageOfImage) && codeOffset < size && size <= file.size() - at;
}

StoredStrings readStoredStrings(std::span<const uint8_t> file, DecompressSourceFunction decompress)
{
    StoredStrings result;
    unsigned numberOfImages = 0;
    VerdictOnStoredStrings worst = VerdictOnStoredStrings::NothingLiesBesideStrings;
    for (size_t at = 0; at + sizeof(imageMagic) <= file.size(); at += smallestAlignmentOfImageInFile) {
        if (memcmp(file.data() + at, &imageMagic, sizeof(imageMagic)) || !isHeaderOfImage(file, at))
            continue;
        worst = std::max(worst, ReaderOfImage(file, at, numberOfImages++, result, decompress).read());
    }
    if (!numberOfImages) {
        result.report.append(byteCast<uint8_t>("the file holds no image\n"_s.span8()));
        return result;
    }
    result.verdict = worst;
    return result;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
