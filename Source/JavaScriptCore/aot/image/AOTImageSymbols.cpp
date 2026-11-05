/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTImageBuilder.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTIRProgram.h"
#include "AOTRegExpRecord.h"
#include "AOTRegExpRequests.h"
#include "AOTRuntimeTable.h"
#include "AOTSort.h"
#include "AOTStubDescriptor.h"
#include "YarrFlags.h"
#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <span>
#include <tuple>
#include <utility>
#include <wtf/ASCIICType.h>
#include <wtf/CheckedArithmetic.h>
#include <wtf/DataLog.h>
#include <wtf/FileSystem.h>
#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/ProcessID.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/CString.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringHash.h>
#include <wtf/text/StringView.h>
#include <wtf/text/WTFString.h>

namespace JSC { namespace AOT {

namespace {

struct ImageSymbol {
    uint32_t offset;
    // The range ends here, or where the next symbol starts if that comes first.
    uint32_t end;
    uint32_t name;
};

struct RegExpEntryInSymbols {
    static constexpr uint32_t noRegExp = std::numeric_limits<uint32_t>::max();

    uint32_t offset;
    uint32_t regExp;
    bool isFor16BitSubjects;
};

class ImageSymbolTable {
public:
    void append(ASCIILiteral text) { m_names.append(byteCast<uint8_t>(text.span())); }
    void append(std::span<const uint8_t> bytes) { m_names.append(bytes); }

    void appendNumber(uint32_t number)
    {
        std::array<uint8_t, std::numeric_limits<uint32_t>::digits10 + 1> digits;
        size_t first = digits.size();
        do {
            digits[--first] = static_cast<uint8_t>('0' + number % 10);
            number /= 10;
        } while (number);
        m_names.append(std::span<const uint8_t>(digits).subspan(first));
    }

    void appendText(const Typed::StringTable& strings, StringID string)
    {
        size_t start = m_names.size();
        if (strings.isAllASCII(string))
            m_names.append(strings[string]);
        else
            m_names.append(byteCast<uint8_t>(strings.toWTFString(string).utf8(StrictConversionReplacingUnpairedSurrogatesWithFFFD).span()));
        // A name ends at its first zero byte, and tools that list symbols print one name per line.
        for (uint8_t& byte : m_names.mutableSpan().subspan(start)) {
            if (isASCII(byte) && !isASCIIPrintable(byte))
                byte = ' ';
        }
    }

    // Returns the offset of the name appended since the last call.
    uint32_t endName()
    {
        m_names.append(0);
        return std::exchange(m_startOfNextName, static_cast<uint32_t>(m_names.size()));
    }

    void add(uint32_t offset, uint32_t end, uint32_t name) { m_symbols.append({ offset, end, name }); }

    // The map file is made of the pieces that names are made of. It ends no name and adds no symbol.
    Vector<uint8_t> takeText() { return std::exchange(m_names, { }); }

    // The format that SymbolTable::parse() in Bun's src/exe_format/elf_symbols.rs reads, in little-endian 32-bit words: the number of symbols, the size of the names, the offset of the code part in the image, and the size of the image from that offset to its end.
    // Then, for each symbol, its offset in the code part, its size and the offset of its name among the names. Then the names, each null-terminated.
    Vector<uint8_t> encode(const ImageHeader& header)
    {
        constexpr size_t wordsInHeader = 4;
        constexpr size_t wordsInSymbol = 3;
        AOT::stableSort(m_symbols, { }, &ImageSymbol::offset);

        Vector<uint8_t> result;
        result.reserveInitialCapacity((wordsInHeader + wordsInSymbol * m_symbols.size()) * sizeof(uint32_t) + m_names.size());
        auto appendWord = [&](uint32_t word) {
            for (unsigned shift = 0; shift < 32; shift += 8)
                result.append(static_cast<uint8_t>(word >> shift));
        };
        appendWord(safeCast<uint32_t>(m_symbols.size()));
        appendWord(safeCast<uint32_t>(m_names.size()));
        appendWord(safeCast<uint32_t>(header.codeOffset));
        appendWord(safeCast<uint32_t>(header.size - header.codeOffset));
        for (size_t index = 0; index < m_symbols.size(); ++index) {
            const ImageSymbol& symbol = m_symbols[index];
            uint32_t end = index + 1 < m_symbols.size() ? std::min(symbol.end, m_symbols[index + 1].offset) : symbol.end;
            appendWord(symbol.offset);
            appendWord(end - symbol.offset);
            appendWord(symbol.name);
        }
        result.appendVector(m_names);
        return result;
    }

private:
    Vector<ImageSymbol> m_symbols;
    Vector<uint8_t> m_names;
    uint32_t m_startOfNextName { 0 };
};

} // anonymous namespace

// Program::lineColumnFor() returns line 1 for a module without positions.
static std::optional<unsigned> lineOfFunctionInSymbol(const Typed::Program& program, const Typed::Function& function)
{
    if (!hasModule(function.declarationLocation()))
        return std::nullopt;
    const auto& positions = program.at(function.module).positions;
    if (positions.lineStarts.empty() && positions.columnAdjustments.isEmpty())
        return std::nullopt;
    return program.lineColumnFor(function.module, function.position).line;
}

static void appendFunctionToSymbolName(ImageSymbolTable& table, const Typed::Program& program, const Typed::Function& function)
{
    if (function.isModuleBody())
        table.append("(module)"_s);
    else if (function.name && function.name != program.strings.emptyString())
        table.appendText(program.strings, function.name);
    else
        table.append("(anonymous)"_s);
    if (!function.module)
        return;
    table.append(" "_s);
    table.appendText(program.strings, program.at(function.module).displayPath);
    if (function.isModuleBody())
        return;
    if (std::optional<unsigned> line = lineOfFunctionInSymbol(program, function)) {
        table.append(":"_s);
        table.appendNumber(*line);
    }
}

static void addSymbolsOfUnits(ImageSymbolTable& table, const Typed::Program& program, const BuiltImage& built, bool hidesNames)
{
    const CodeLayout& layout = built.layout;
    RELEASE_ASSERT(layout.bodyStarts.size() == built.functions.size());
    for (uint32_t position = 0; position < built.functions.size(); ++position) {
        const CompiledCode& code = built.functions[position];
        if (code.body.bytes.isEmpty())
            continue;
        if (hidesNames) {
            // obfuscatedPositionOf() reports this number as the line in stack traces of an obfuscated build, so the obfuscation map decodes it.
            table.append("(function "_s);
            table.appendNumber(position + 1);
            table.append(")"_s);
        } else
            appendFunctionToSymbolName(table, program, program.at(code.unit.function));
        if (code.unit.role != Typed::CodeRole::Body) {
            table.append(" "_s);
            table.append(nameOf(code.unit.role));
        }
        table.add(layout.bodyStarts[position], layout.functionsEnd, table.endName());
    }
}

static void appendStubToSymbolName(ImageSymbolTable& table, StubKey key)
{
    const StubDescriptor& descriptor = descriptorOf(key.family());
    table.append(descriptor.name);
    if (key.form() < descriptor.formNames.size()) {
        table.append("."_s);
        table.append(descriptor.formNames[key.form()]);
    }
    if (descriptor.axes.contains(StubAxis::Count)) {
        table.append(".n"_s);
        table.appendNumber(key.count());
    }
    switch (key.callerFrame()) {
    case CallerFrame::HasFrame:
        break;
    case CallerFrame::Frameless:
        table.append(".Frameless"_s);
        break;
    case CallerFrame::StubFrame:
        table.append(".StubFrame"_s);
        break;
    case CallerFrame::KeepsNoRegisters:
        table.append(".KeepsNoRegisters"_s);
        break;
    case CallerFrame::KeepsNoFPRs:
        table.append(".KeepsNoFPRs"_s);
        break;
    }
    if (std::optional<GPRReg> operandRegister = key.operandRegister()) {
        table.append("("_s);
        table.append(Reg(*operandRegister).debugName());
        table.append(")"_s);
    }
    if (isOperationCallHead(key)) {
        table.append(" to "_s);
        table.append(nameOf(entryAtOffset(*key.immediate())));
    } else if (std::optional<uint32_t> immediate = key.immediate()) {
        table.append("#"_s);
        table.appendNumber(*immediate);
    }
}

static void addSymbolsOfSharedCode(ImageSymbolTable& table, const BuiltImage& built)
{
    const SharedCode& sharedCode = built.sharedCode;
    const CodeLayout& layout = built.layout;
    auto addInEachCopy = [&](uint32_t offset, uint32_t name) {
        for (uint32_t copy : layout.sharedCodeCopies)
            table.add(copy + offset, copy + layout.sharedCodeSize, name);
    };

    // The placements are sorted by key. Where several entries share an offset, the one with the smallest key names it.
    Vector<StubPlacement> placements = sharedCode.blob.placements;
    AOT::stableSort(placements, { }, &StubPlacement::offset);
    removeRepeatedElements(placements, [](const StubPlacement& a, const StubPlacement& b) {
        return a.offset == b.offset;
    });
    if (placements.isEmpty() || placements.first().offset) {
        table.append("(stubs)"_s);
        addInEachCopy(0, table.endName());
    }
    for (const StubPlacement& placement : placements) {
        appendStubToSymbolName(table, placement.key);
        addInEachCopy(placement.offset, table.endName());
    }

    // The lists of moves are sorted by their words, and the first word is the callee, so the thunks of one callee are adjacent.
    const ArgumentThunks& thunks = sharedCode.argumentThunks;
    RELEASE_ASSERT(thunks.offsets.size() == sharedCode.argumentMoves.size());
    uint64_t calleeOfName = 0;
    uint32_t name = 0;
    for (size_t index = 0; index < thunks.offsets.size(); ++index) {
        uint64_t callee = sharedCode.argumentMoves[index]->at(0);
        if (!index || callee != calleeOfName) {
            appendStubToSymbolName(table, StubKey::fromBits(callee));
            table.append(" with arguments"_s);
            name = table.endName();
            calleeOfName = callee;
        }
        addInEachCopy(sharedCode.argumentThunksOffset + thunks.offsets[index], name);
    }

    if (sharedCode.veneersOffset < layout.sharedCodeSize) {
        table.append("(veneers)"_s);
        addInEachCopy(sharedCode.veneersOffset, table.endName());
    }
}

// A predicted text that gets neither code nor a record leaves fewer records than requests, so a record is named by what it holds itself.
static void appendRegExpToSymbolName(ImageSymbolTable& table, const Typed::Program& program, const ImageRegExp& record)
{
    table.append("/"_s);
    table.appendText(program.strings, StringID(record.pattern));
    table.append("/"_s);
    table.append(byteCast<uint8_t>(Yarr::flagsString(record.flags()).span()));
}

static std::span<const ImageRegExp> regExpRecordsOf(const BuiltImage& built)
{
    // The caller must not move BuiltImage::image out before this call.
    RELEASE_ASSERT(built.image.size() >= sizeof(ImageHeader));
    const ImageHeader& header = spanReinterpretCast<const ImageHeader>(built.image.span().first(sizeof(ImageHeader)))[0];
    ImageSectionRange range = header.sections[static_cast<unsigned>(ImageSection::RegExps)];
    return spanReinterpretCast<const ImageRegExp>(built.image.span().subspan(range.offset, range.size));
}

// encodeRegExpRecords() wrote the entry offsets of each expression's code into the records of the image. Ascending, each offset once: an entry ends where the next begins.
static Vector<RegExpEntryInSymbols> collectRegExpEntries(std::span<const ImageRegExp> records)
{
    // The shared routines before the first body belong to no single expression. Neither does the failure routine, which 16-bit subjects enter when an expression has only 8-bit code.
    Vector<RegExpEntryInSymbols> entries { RegExpEntryInSymbols { 0, RegExpEntryInSymbols::noRegExp, false } };
    for (uint32_t number = 0; number < records.size(); ++number) {
        const ImageRegExp& record = records[number];
        if (record.form() != RegExpRecordForm::Code)
            continue;
        entries.append({ record.codeFor8Bit, number, false });
        entries.append({ record.codeFor16Bit, record.hasCodeFor8BitSubjectsAlone ? RegExpEntryInSymbols::noRegExp : number, true });
    }
    // Expressions with equal code share it, and the first of them names it.
    AOT::stableSort(entries, [](const RegExpEntryInSymbols& a, const RegExpEntryInSymbols& b) {
        return std::tie(a.offset, a.regExp) < std::tie(b.offset, b.regExp);
    });
    removeRepeatedElements(entries, [](const RegExpEntryInSymbols& a, const RegExpEntryInSymbols& b) {
        return a.offset == b.offset;
    });
    return entries;
}

static void addSymbolsOfRegExps(ImageSymbolTable& table, const Typed::Program& program, const BuiltImage& built, bool hidesNames)
{
    const uint32_t start = built.layout.regExpCodeOffset;
    const uint32_t end = built.layout.executableSize;
    if (start == end)
        return;

    std::span<const ImageRegExp> records = regExpRecordsOf(built);
    for (const RegExpEntryInSymbols& entry : collectRegExpEntries(records)) {
        if (entry.regExp == RegExpEntryInSymbols::noRegExp)
            table.append("(regular expressions)"_s);
        else if (hidesNames) {
            table.append("(regular expression "_s);
            table.appendNumber(entry.regExp);
            table.append(")"_s);
        } else
            appendRegExpToSymbolName(table, program, records[entry.regExp]);
        if (entry.regExp != RegExpEntryInSymbols::noRegExp && entry.isFor16BitSubjects)
            table.append(" (16-bit)"_s);
        table.add(start + entry.offset, end, table.endName());
    }
}

Vector<uint8_t> encodeSymbols(const Typed::Program& program, const BuiltImage& built, BuildContext& context)
{
    // The caller must not move BuiltImage::image out before this call.
    RELEASE_ASSERT(built.image.size() >= sizeof(ImageHeader));
    const ImageHeader& header = spanReinterpretCast<const ImageHeader>(built.image.span().first(sizeof(ImageHeader)))[0];
    const CodeLayout& layout = built.layout;
    bool hidesNames = context.options().obfuscates();

    ImageSymbolTable table;
    addSymbolsOfUnits(table, program, built, hidesNames);
    addSymbolsOfSharedCode(table, built);
    if (layout.ffiCodeSize) {
        table.append("(FFI code)"_s);
        table.add(layout.ffiCodeOffset, layout.ffiCodeOffset + layout.ffiCodeSize, table.endName());
    }
    addSymbolsOfRegExps(table, program, built, hidesNames);
    return table.encode(header);
}

static void appendNumberToMapRow(ImageSymbolTable& table, uint32_t number)
{
    table.append("\t"_s);
    table.appendNumber(number);
}

static void appendUnitsToMap(ImageSymbolTable& table, const Typed::Program& program, const BuiltImage& built)
{
    const CodeLayout& layout = built.layout;
    RELEASE_ASSERT(layout.bodyStarts.size() == built.functions.size());
    for (uint32_t position = 0; position < built.functions.size(); ++position) {
        const CompiledCode& code = built.functions[position];
        if (code.body.bytes.isEmpty())
            continue;
        const Typed::Function& function = program.at(code.unit.function);
        table.append("F"_s);
        appendNumberToMapRow(table, position);
        appendNumberToMapRow(table, layout.bodyStarts[position]);
        appendNumberToMapRow(table, safeCast<uint32_t>(code.body.bytes.size()));
        appendNumberToMapRow(table, function.module ? function.module.index() + 1 : 0);
        appendNumberToMapRow(table, code.unit.function.index());
        appendNumberToMapRow(table, static_cast<uint32_t>(code.unit.role));
        table.append("\t"_s);
        if (function.name)
            table.appendText(program.strings, function.name);
        table.append("\t"_s);
        table.append(nameOf(code.unit.role));
        appendNumberToMapRow(table, code.unit.numberOfBoundArguments);
        table.append("\t"_s);
        if (function.module)
            table.appendText(program.strings, program.at(function.module).path);
        appendNumberToMapRow(table, function.position);
        table.append("\n"_s);

        table.append("FO"_s);
        for (const BytesOfForm& entry : code.bytesByForm) {
            table.append("\t"_s);
            table.append(nameOf(entry.opcode));
            table.append("\t"_s);
            table.append(nameOf(entry.form));
            appendNumberToMapRow(table, entry.count);
        }
        table.append("\n"_s);
    }
}

static void appendSharedCodeToMap(ImageSymbolTable& table, const BuiltImage& built, uint32_t copy)
{
    const SharedCode& sharedCode = built.sharedCode;
    auto beginRow = [&](uint32_t offset) {
        table.append("T"_s);
        appendNumberToMapRow(table, copy + offset);
        table.append("\t"_s);
    };

    for (const StubPlacement& placement : sharedCode.blob.placements) {
        beginRow(placement.offset);
        appendStubToSymbolName(table, placement.key);
        table.append("\n"_s);
    }

    const ArgumentThunks& thunks = sharedCode.argumentThunks;
    RELEASE_ASSERT(thunks.offsets.size() == sharedCode.argumentMoves.size());
    for (size_t index = 0; index < thunks.offsets.size(); ++index) {
        beginRow(sharedCode.argumentThunksOffset + thunks.offsets[index]);
        appendStubToSymbolName(table, StubKey::fromBits(sharedCode.argumentMoves[index]->at(0)));
        table.append(" with arguments\n"_s);
    }

    if (sharedCode.veneersOffset < built.layout.sharedCodeSize) {
        beginRow(sharedCode.veneersOffset);
        table.append("(veneers)\n"_s);
    }
}

// This code lies behind the last unit and the last stub. Without rows of its own whoever reads a profile by the map files it under the row in front of it.
static void appendRegExpCodeToMap(ImageSymbolTable& table, const Typed::Program& program, const BuiltImage& built)
{
    const uint32_t start = built.layout.regExpCodeOffset;
    const uint32_t end = built.layout.executableSize;
    if (start == end)
        return;

    std::span<const ImageRegExp> records = regExpRecordsOf(built);
    Vector<RegExpEntryInSymbols> entries = collectRegExpEntries(records);
    for (size_t index = 0; index < entries.size(); ++index) {
        const RegExpEntryInSymbols& entry = entries[index];
        uint32_t endOfEntry = index + 1 < entries.size() ? entries[index + 1].offset : end - start;
        bool belongsToOneRegExp = entry.regExp != RegExpEntryInSymbols::noRegExp;
        table.append(belongsToOneRegExp ? "X"_s : "XR"_s);
        appendNumberToMapRow(table, start + entry.offset);
        appendNumberToMapRow(table, endOfEntry - entry.offset);
        if (belongsToOneRegExp) {
            const ImageRegExp& record = records[entry.regExp];
            appendNumberToMapRow(table, entry.isFor16BitSubjects ? 16 : 8);
            appendNumberToMapRow(table, record.rawFlags);
            table.append("\t"_s);
            appendRegExpToSymbolName(table, program, record);
        }
        table.append("\n"_s);
    }
}

// What the tools of Bun's test/aot read. A row a line, its fields parted by tabs. Offsets count from the start of the code part of the image.
//   F   a unit that has code: position, offset, size, module + 1 or 0, function, role, name, name of the role, number of bound arguments, path, position in the text
//   FO  behind the row F of its unit, what the code of the unit was made of. For each entry of CompiledCode::bytesByForm: operation, form, number of values. No field where that list is empty
//   T   a stub, a thunk or the veneers, in each copy of the shared code: offset, name
//   TN  a stub that never returns: offset
//   R   where the code of the units ends
//   N   the stubs that native functions are called through: offset, size
//   X   the code of a regular expression: offset, size up to the next row, 8 or 16 for the width of its subjects, flags, the expression as it is written. Expressions with equal code share a row
//   XR  code that regular expressions share: offset, size up to the next row
Vector<uint8_t> encodeRowsOfMapFile(const Typed::Program& program, const BuiltImage& built)
{
    const CodeLayout& layout = built.layout;

    ImageSymbolTable table;
    appendUnitsToMap(table, program, built);
    for (uint32_t copy : layout.sharedCodeCopies)
        appendSharedCodeToMap(table, built, copy);
    for (uint32_t copy : layout.sharedCodeCopies) {
        for (const StubPlacement& placement : built.sharedCode.blob.placements) {
            if (!neverReturns(placement.key))
                continue;
            table.append("TN"_s);
            appendNumberToMapRow(table, copy + placement.offset);
            table.append("\n"_s);
        }
    }
    table.append("R"_s);
    appendNumberToMapRow(table, layout.functionsEnd);
    table.append("\n"_s);
    if (layout.ffiCodeSize) {
        table.append("N"_s);
        appendNumberToMapRow(table, layout.ffiCodeOffset);
        appendNumberToMapRow(table, layout.ffiCodeSize);
        table.append("\n"_s);
    }
    appendRegExpCodeToMap(table, program, built);
    return table.takeText();
}

void writeMapFile(const UTF8CString& path, const Typed::Program& program, const BuiltImage& built, BuildContext&)
{
    if (path.isEmpty())
        return;

    // A build with several entry points compiles an image for each: the first map has the name that was asked for, the next ones that name and ".1", ".2" ..
    // The processes of a test run all get the one name from their environment: "%p" stands for the number of the process.
    String name = makeStringByReplacingAll(String::fromUTF8ReplacingInvalidSequences(path.span()), "%p"_s, String::number(getCurrentProcessID()));
    unsigned number = [&] {
        static Lock lock;
        Locker locker { lock };
        static NeverDestroyed<HashMap<String, unsigned>> mapsWritten;
        return mapsWritten->add(name, 0).iterator->value++;
    }();
    if (number)
        name = makeString(name, '.', number);

    Vector<uint8_t> rows = encodeRowsOfMapFile(program, built);
    if (FileSystem::overwriteEntireFile(name, rows.span()) != rows.size())
        dataLogLn("AOT: cannot write the map file ", name);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
