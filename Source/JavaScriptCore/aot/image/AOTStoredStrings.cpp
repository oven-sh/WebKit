/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStoredStrings.h"

#if ENABLE(AOT)

#include "AOTCodedTexts.h"
#include "AOTImageFormat.h"
#include "AOTImageText.h"
#include "AOTProgramTables.h"
#include "AOTSchemaPackedRecords.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <wtf/HashSet.h>
#include <wtf/StdLibExtras.h>
#include <wtf/StringPrintStream.h>
#include <wtf/text/CString.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/text/StringView.h>
#include <wtf/text/WTFString.h>

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
    uint64_t size() const { return end - begin; }

    uint64_t begin;
    uint64_t end;
};

struct StringSection {
    RangeOfBytes rangeInFile(RangeOfBytes characters) const { return { bytes.begin + characters.begin, bytes.begin + characters.end }; }

    ASCIILiteral name;
    RangeOfBytes bytes;
    Vector<RangeOfBytes> covered { };
};

class ReaderOfImage {
public:
    ReaderOfImage(std::span<const uint8_t> file, size_t offsetInFile, unsigned number, StoredStrings& result, StoredStringsForm form)
        : m_image(file.subspan(offsetInFile))
        , m_offsetInFile(offsetInFile)
        , m_number(number)
        , m_result(result)
        , m_form(form)
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
    void appendLine(ASCIILiteral type, const auto&... members);
    void beginSection(const StringSection&);
    void appendString(StringSection&, RangeOfBytes characters, ASCIILiteral encoding, StringView);
    void appendUTF8String(StringSection&, RangeOfBytes characters, std::span<const uint8_t> utf8);
    void appendCodedStrings(const CodedTexts&, std::span<const uint8_t> bytes, uint64_t offsetInFile);
    void appendUncoveredRuns(const StringSection&);
    VerdictOnStoredStrings reportSection(StringSection&, std::span<const uint8_t> text);

    std::span<const uint8_t> m_image;
    size_t m_offsetInFile;
    unsigned m_number;
    StoredStrings& m_result;
    StoredStringsForm m_form;
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

void ReaderOfImage::appendLine(ASCIILiteral type, const auto&... members)
{
    if (m_form != StoredStringsForm::JSONLines)
        return;
    StringBuilder line;
    line.append("{\"type\":\""_s, type, "\",\"image\":"_s, m_number);
    (line.append(members), ...);
    line.append("}\n"_s);
    m_result.strings.append(byteCast<uint8_t>(line.toString().utf8().span()));
}

void ReaderOfImage::beginSection(const StringSection& section)
{
    appendLine("section"_s, ",\"section\":\""_s, section.name, "\",\"offset\":"_s, section.bytes.begin, ",\"length\":"_s, section.bytes.size());
}

void ReaderOfImage::appendString(StringSection& section, RangeOfBytes characters, ASCIILiteral encoding, StringView string)
{
    section.covered.append(characters);
    switch (m_form) {
    case StoredStringsForm::ZeroTerminated:
        m_result.strings.append(byteCast<uint8_t>(string.utf8().span()));
        m_result.strings.append(0);
        return;
    case StoredStringsForm::JSONLines: {
        RangeOfBytes range = section.rangeInFile(characters);
        StringBuilder quoted;
        quoted.appendQuotedJSONString(string.toString());
        appendLine("string"_s, ",\"section\":\""_s, section.name, "\",\"offset\":"_s, range.begin, ",\"length\":"_s, range.size(), ",\"encoding\":\""_s, encoding, "\",\"text\":"_s, quoted.toString());
        return;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void ReaderOfImage::appendUTF8String(StringSection& section, RangeOfBytes characters, std::span<const uint8_t> utf8)
{
    if (m_form == StoredStringsForm::JSONLines) {
        appendString(section, characters, "utf8"_s, String::fromUTF8ReplacingInvalidSequences(utf8));
        return;
    }
    section.covered.append(characters);
    m_result.strings.append(utf8);
    m_result.strings.append(0);
}

void ReaderOfImage::appendCodedStrings(const CodedTexts& texts, std::span<const uint8_t> bytes, uint64_t offsetInFile)
{
    if (!texts.count())
        return;
    StringSection section { "coded text"_s, { offsetInFile, offsetInFile + bytes.size() } };
    beginSection(section);
    RangeOfBytes group { 0, 0 };
    for (uint32_t number = 0; number < texts.count(); ++number) {
        if (!(number % CodedTexts::textsInGroup)) {
            std::span<const uint8_t> bytesOfGroup = texts.bytesOfGroup(number / CodedTexts::textsInGroup);
            group.begin = bytesOfGroup.data() - bytes.data();
            group.end = group.begin + bytesOfGroup.size();
        }
        auto text = texts.at(number);
        appendString(section, group, "coded"_s, StringView(byteCast<Latin1Character>(text.span())));
    }
    appendUncoveredRuns(section);
}

static void forEachUncoveredRun(Vector<RangeOfBytes>& covered, uint64_t size, NOESCAPE const Invocable<void(RangeOfBytes)> auto& functor)
{
    std::ranges::sort(covered, { }, &RangeOfBytes::begin);
    uint64_t end = 0;
    for (const RangeOfBytes& range : covered) {
        if (end < range.begin)
            functor(RangeOfBytes { end, range.begin });
        end = std::max(end, range.end);
    }
    if (end < size)
        functor(RangeOfBytes { end, size });
}

void ReaderOfImage::appendUncoveredRuns(const StringSection& section)
{
    if (m_form != StoredStringsForm::JSONLines)
        return;
    Vector<RangeOfBytes> covered = section.covered.map([&](RangeOfBytes characters) {
        RangeOfBytes range = section.rangeInFile(characters);
        return RangeOfBytes { range.begin - section.bytes.begin, range.end - section.bytes.begin };
    });
    forEachUncoveredRun(covered, section.bytes.size(), [&](RangeOfBytes run) {
        uint64_t offset = section.bytes.begin + run.begin;
        bool isZero = std::ranges::all_of(m_image.subspan(static_cast<size_t>(offset - m_offsetInFile), static_cast<size_t>(run.size())), [](uint8_t byte) {
            return !byte;
        });
        appendLine("uncovered"_s, ",\"section\":\""_s, section.name, "\",\"offset\":"_s, offset, ",\"length\":"_s, run.size(), ",\"isZero\":"_s, isZero ? "true"_s : "false"_s);
    });
}

VerdictOnStoredStrings ReaderOfImage::reportSection(StringSection& section, std::span<const uint8_t> text)
{
    appendUncoveredRuns(section);
    uint64_t numberOfBytesBesideStrings = 0;
    std::optional<uint64_t> firstThatIsNotZero;
    forEachUncoveredRun(section.covered, text.size(), [&](RangeOfBytes run) {
        numberOfBytesBesideStrings += run.size();
        for (uint64_t at = run.begin; at < run.end && !firstThatIsNotZero; ++at) {
            if (text[at])
                firstThatIsNotZero = at;
        }
    });
    uint64_t offsetInFile = section.bytes.begin;
    report(section.name, " | offset ", offsetInFile, " | length ", text.size(), " | ", numberOfBytesBesideStrings, " bytes that no string covers");
    if (!firstThatIsNotZero)
        return VerdictOnStoredStrings::NothingLiesBesideStrings;
    report(section.name, " | the byte at offset ", offsetInFile + *firstThatIsNotZero, " is covered by no string and is not zero");
    return VerdictOnStoredStrings::BytesBesideStringsAreNotZero;
}

static_assert(imageFormatVersion == 5 && sizeof(ImageHeader) == 576 && sizeof(ProgramTables) == 892 && sizeof(FunctionSources) == 16 && sizeof(FunctionSourceEntry) == 11 && sizeof(QuotedSourceGroupData) == 16, "an image of another build is read by its format alone: whoever changes one of these raises imageFormatVersion");

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
    auto bytesOfConstants = section(ImageSection::StringConstants);
    auto text = section(ImageSection::Text);
    if (!bytesOfWords || !bytesOfListedStrings || !bytesOfHeaders || !bytesOfConstants || !text)
        return refuse("a section lies outside the image");
    RecordsInFile<ImageStringWord> words(*bytesOfWords);
    RecordsInFile<ImageStringWord> constantsAndHashes(*bytesOfConstants);
    size_t numberOfPlacedConstants = m_header.numbers.numberOfPlacedConstants;
    if (2 * numberOfPlacedConstants > constantsAndHashes.size())
        return refuse("a section lies outside the image");
    size_t numberOfConstants = constantsAndHashes.size() - numberOfPlacedConstants;
    RecordsInFile<uint32_t> listedStrings(*bytesOfListedStrings);
    RecordsInFile<ImageStringHeader> headers(*bytesOfHeaders);
    RecordsInFile<ImageStringRecord> records(*bytesOfHeaders);
    bool hasRecords = m_header.options.form() == ImageForm::PositionIndependent;
    uint64_t addressOfText = m_header.address + m_header.sections[static_cast<unsigned>(ImageSection::Text)].offset;

    size_t numberOfWords = m_header.numbers.numberOfStringWords;
    if (numberOfWords > words.size())
        return refuse("a section lies outside the image");
    std::span<const uint8_t> bytesOfCodedTexts = bytesOfWords->subspan(numberOfWords * sizeof(ImageStringWord));
    CodedTexts codedTexts(bytesOfCodedTexts, m_header.numbers.numberOfCodedTexts);

    appendLine("image"_s, ",\"offset\":"_s, static_cast<uint64_t>(m_offsetInFile), ",\"length\":"_s, m_header.size, ",\"format\":"_s, formatVersion);
    uint64_t offsetOfText = m_offsetInFile + m_header.sections[static_cast<unsigned>(ImageSection::Text)].offset;
    StringSection sectionOfText { "Text"_s, { offsetOfText, offsetOfText + text->size() } };
    beginSection(sectionOfText);
    HashSet<uint32_t> bitsOfWords;
    size_t endOfIdentifiers = m_header.numbers.endOfIdentifiers;
    size_t endOfWords = endOfIdentifiers + numberOfWords;
    for (size_t index = 0; index < endOfWords + numberOfConstants; ++index) {
        bool isPlaced = index >= endOfWords && index - endOfWords < numberOfPlacedConstants;
        ImageStringWord word = isPlaced ? ImageStringWord::ofPlacedConstant(static_cast<uint32_t>(index - endOfWords)) : index < endOfIdentifiers ? ImageStringWord::ofIdentifier(static_cast<uint32_t>(index)) : index < endOfWords ? words[index - endOfIdentifiers] : constantsAndHashes[index - endOfWords];
        bool is16Bit = word.is16Bit();
        if (!word.hasText() || !bitsOfWords.add(word.bits).isNewEntry)
            continue;
        std::optional<ImageLongString> range = ImageLongString { 0, 0 };
        if (isPlaced)
            range = ImagePlacedString::rangeOf(constantsAndHashes[word.number()].bits);
        else if (!word.hasHeader())
            range = listedString(listedStrings, word.number());
        else if (word.number() >= (hasRecords ? records.size() : headers.size()))
            range = std::nullopt;
        else if (hasRecords) {
            ImageStringRecord record = records[word.number()];
            is16Bit = record.is16Bit();
            if (uint32_t length = record.rangeOrIndex & ImageString::lengthMask)
                range = ImageLongString { record.rangeOrIndex >> ImageString::lengthBits, length };
            else if (record.hasText())
                range = listedString(listedStrings, record.rangeOrIndex >> ImageString::lengthBits);
        } else if (ImageStringHeader header = headers[word.number()]; header.length) {
            is16Bit = !header.is8Bit();
            uint64_t offset = header.characters - addressOfText;
            range = offset <= text->size() ? std::optional(ImageLongString { static_cast<uint32_t>(offset), header.length }) : std::nullopt;
        }
        uint64_t sizeOfUnit = is16Bit ? sizeof(char16_t) : sizeof(Latin1Character);
        if (!range || range->offset + range->length * sizeOfUnit > text->size())
            return refuse("string ", index, " lies outside the section of text");
        if (!range->length)
            continue;
        std::span<const uint8_t> bytes = text->subspan(range->offset, range->length * sizeOfUnit);
        RangeOfBytes characters { range->offset, range->offset + bytes.size() };
        if (is16Bit) {
            Vector<char16_t> units(range->length);
            memcpy(units.mutableSpan().data(), bytes.data(), bytes.size());
            appendString(sectionOfText, characters, "utf16le"_s, StringView(units.span()));
        } else
            appendString(sectionOfText, characters, "latin1"_s, StringView(byteCast<Latin1Character>(bytes)));
    }
    appendCodedStrings(codedTexts, bytesOfCodedTexts, m_offsetInFile + static_cast<uint64_t>(bytesOfCodedTexts.data() - m_image.data()));
    VerdictOnStoredStrings verdictOnText = reportSection(sectionOfText, *text);
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
        || !tableLiesInTables(ProgramTableIndex::quotedSourceText, 1) || !tableLiesInTables(ProgramTableIndex::functionSources, sizeof(FunctionSources)))
        return refuse("a table of function sources or quoted sources lies outside the program tables");

    VerdictOnStoredStrings worst = VerdictOnStoredStrings::NothingLiesBesideStrings;
    std::span<const uint8_t> quotedSourceText = tables.quotedSourceText();
    uint64_t offsetOfQuotedSources = m_offsetInFile + offsetOfTables + tables.ranges[static_cast<unsigned>(ProgramTableIndex::quotedSourceText)].offset;
    StringSection sectionOfQuotedSources { "quoted text"_s, { offsetOfQuotedSources, offsetOfQuotedSources + quotedSourceText.size() } };
    if (std::span<const uint8_t> text = quotedSourceText; !text.empty()) {
        std::span<const QuotedSourceGroupData> groups = tables.quotedSourceGroups();
        std::span<const uint8_t> rows = tables.quotedSourceRows();
        beginSection(sectionOfQuotedSources);
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
                RangeOfBytes characters { static_cast<uint64_t>(start), static_cast<uint64_t>(start) + row.length };
                appendString(sectionOfQuotedSources, characters, "latin1"_s, StringView(byteCast<Latin1Character>(text.subspan(static_cast<size_t>(start), row.length))));
            }
        }
        worst = std::max(worst, reportSection(sectionOfQuotedSources, text));
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
        uint64_t offsetOfSource = m_offsetInFile + offsetOfTables + sources.sourceOffset;
        StringSection sectionOfSource { "function text"_s, { offsetOfSource, offsetOfSource + size } };
        if (size > sources.sourceSize || !liesInTables(sources.sourceOffset, size))
            return refuse("the function sources lie outside the program tables");
        std::span<const uint8_t> text { tables.at<uint8_t>(sources.sourceOffset), static_cast<size_t>(size) };
        beginSection(sectionOfSource);
        for (const FunctionSourceEntry& entry : entries) {
            uint32_t start = entry.start;
            uint32_t end = entry.end;
            if (start >= end)
                continue;
            appendUTF8String(sectionOfSource, { start, end }, text.subspan(start, end - start));
        }
        worst = std::max(worst, reportSection(sectionOfSource, text));
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

StoredStrings readStoredStrings(std::span<const uint8_t> file, StoredStringsForm form)
{
    StoredStrings result;
    unsigned numberOfImages = 0;
    VerdictOnStoredStrings worst = VerdictOnStoredStrings::NothingLiesBesideStrings;
    for (size_t at = 0; at + sizeof(imageMagic) <= file.size(); at += smallestAlignmentOfImageInFile) {
        if (memcmp(file.data() + at, &imageMagic, sizeof(imageMagic)) || !isHeaderOfImage(file, at))
            continue;
        worst = std::max(worst, ReaderOfImage(file, at, numberOfImages++, result, form).read());
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
