/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTestRunReportSizes.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTImageFormat.h"
#include "AOTProgramTables.h"
#include "AOTStatistics.h"
#include <iterator>
#include <wtf/DataLog.h>
#include <wtf/StringPrintStream.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

namespace {

enum class CellOfTablesInSizesReport : uint8_t {
    ProgramTables,
    Text,
    StringHeaders,
    StringRecords,
    Identifiers,
    StringConstants,
    NamesOfSites,
    SideTables,
    Positions,
    FaultSites,
    FunctionRecords,
    FunctionStarts,
    CodeGranules,
    InnerEntries,
    FrameMaps,
    ClosureWords,
    RegularExpressions,
    Other,
};

} // anonymous namespace

static constexpr ASCIILiteral namesOfCellsOfTablesInSizesReport[] = {
    "program tables"_s, "text"_s, "string headers"_s, "string records"_s, "identifiers"_s, "string constants"_s, "names of sites"_s, "side tables"_s, "positions"_s,
    "fault sites"_s, "function records"_s, "function starts"_s, "code granules"_s, "inner entries"_s, "frame maps"_s, "closure words"_s, "regular expressions"_s, "other"_s,
};
static_assert(std::size(namesOfCellsOfTablesInSizesReport) == static_cast<size_t>(CellOfTablesInSizesReport::Other) + 1);

static CellOfTablesInSizesReport cellOfSectionInSizesReport(ImageSection section)
{
    using Cell = CellOfTablesInSizesReport;
    switch (section) {
    case ImageSection::Functions:
        return Cell::FunctionRecords;
    case ImageSection::FunctionStarts:
        return Cell::FunctionStarts;
    case ImageSection::CodeGranules:
        return Cell::CodeGranules;
    case ImageSection::InnerEntries:
        return Cell::InnerEntries;
    case ImageSection::FrameMaps:
        return Cell::FrameMaps;
    case ImageSection::ClosureWords:
        return Cell::ClosureWords;
    case ImageSection::Positions:
        return Cell::Positions;
    case ImageSection::FaultSites:
        return Cell::FaultSites;
    case ImageSection::SideTables:
        return Cell::SideTables;
    case ImageSection::Strings:
    case ImageSection::LongStrings:
        return Cell::StringRecords;
    case ImageSection::Text:
        return Cell::Text;
    case ImageSection::Identifiers:
        return Cell::Identifiers;
    case ImageSection::StringConstants:
        return Cell::StringConstants;
    case ImageSection::NamesOfSites:
        return Cell::NamesOfSites;
    case ImageSection::StringHeaders:
        return Cell::StringHeaders;
    case ImageSection::RegExps:
    case ImageSection::RegExpHashes:
    case ImageSection::RegExpDetails:
        return Cell::RegularExpressions;
    case ImageSection::ProgramTables:
        return Cell::ProgramTables;
    case ImageSection::Frames:
    case ImageSection::SharedCodeCopies:
    case ImageSection::WebAssemblyDirectory:
    case ImageSection::StubAddresses:
    case ImageSection::StubReturnSites:
    case ImageSection::MethodTable:
    case ImageSection::SlotHandlers:
        return Cell::Other;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static constexpr std::pair<ASCIILiteral, TablePartID> partsWithCellInSizesReport[] = {
    { "functions"_s, TablePartID::Functions }, { "function sources"_s, TablePartID::FunctionSources }, { "layouts"_s, TablePartID::Layouts }, { "field table"_s, TablePartID::FieldTable },
    { "types"_s, TablePartID::Types }, { "data"_s, TablePartID::Data }, { "modules"_s, TablePartID::Modules }, { "requests"_s, TablePartID::Requests },
    { "statics"_s, TablePartID::Statics }, { "constants"_s, TablePartID::Constants },
};

SizesOfImage measureImageForTestRunReport(std::span<const uint8_t> image, const std::array<uint64_t, numberOfTableParts>& bytesOfTableParts, BuildContext& context)
{
    RELEASE_ASSERT(image.size() >= sizeof(ImageHeader));
    const auto& header = *reinterpret_cast<const ImageHeader*>(image.data());
    auto rangeOf = [&](ImageSection section) {
        return header.sections[static_cast<unsigned>(section)];
    };
    ImageSectionRange lastSection = header.sections[numberOfImageSections - 1];
    uint64_t tableBytes = static_cast<uint64_t>(lastSection.offset) + lastSection.size;
    uint64_t programTableBytes = rangeOf(ImageSection::ProgramTables).size;
    RELEASE_ASSERT(programTableBytes >= sizeof(ProgramTables) && tableBytes <= image.size());

    SizesOfImage sizes;
    auto addToImage = [&](ASCIILiteral name, uint64_t number) {
        sizes.image.append({ name, number });
    };
    addToImage("functions emitted"_s, header.counts.functions);
    addToImage("code bytes"_s, header.codeSize);
    addToImage("table bytes"_s, tableBytes);
    addToImage("program table bytes"_s, programTableBytes);
    addToImage("text bytes"_s, rangeOf(ImageSection::Text).size);
    addToImage("identifiers"_s, header.numbers.endOfIdentifiers);
    addToImage("string constants"_s, context.statistics().values()[static_cast<unsigned>(StatisticID::StringConstantsNumbered)]);
    addToImage("shapes"_s, header.numbers.lastShape);

    std::array<uint64_t, std::size(namesOfCellsOfTablesInSizesReport)> bytesOfCells { };
    uint64_t bytesOfSections = 0;
    for (unsigned index = 0; index < numberOfImageSections; ++index) {
        bytesOfCells[static_cast<unsigned>(cellOfSectionInSizesReport(static_cast<ImageSection>(index)))] += header.sections[index].size;
        bytesOfSections += header.sections[index].size;
    }
    bytesOfCells[static_cast<unsigned>(CellOfTablesInSizesReport::Other)] += tableBytes - bytesOfSections;
    for (size_t cell = 0; cell < bytesOfCells.size(); ++cell)
        sizes.tables.append({ namesOfCellsOfTablesInSizesReport[cell], bytesOfCells[cell] });

    uint64_t bytesOfParts = 0;
    for (auto [name, part] : partsWithCellInSizesReport) {
        uint64_t bytes = bytesOfTableParts[static_cast<unsigned>(part)];
        sizes.programTables.append({ name, bytes });
        bytesOfParts += bytes;
    }
    sizes.programTables.append({ "other"_s, programTableBytes - bytesOfParts });
    return sizes;
}

static void printLineOfSizesReport(PrintStream& out, ASCIILiteral head, const SizesOfImage::Cells& cells)
{
    out.print(head, ":");
    for (auto [name, number] : cells)
        out.print(" | ", name, " ", number);
    out.println();
}

void logSizesOfImageForTestRunReport(std::span<const uint8_t> image, const std::array<uint64_t, numberOfTableParts>& bytesOfTableParts, BuildContext& context)
{
    if (!context.options().logging.logsPhaseTimes || image.empty()) [[likely]]
        return;
    SizesOfImage sizes = measureImageForTestRunReport(image, bytesOfTableParts, context);
    StringPrintStream lines;
    printLineOfSizesReport(lines, "AOT size of the image"_s, sizes.image);
    printLineOfSizesReport(lines, "AOT size of the tables"_s, sizes.tables);
    printLineOfSizesReport(lines, "AOT size of the program tables"_s, sizes.programTables);
    dataLog(lines.toUTF8CString());
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
