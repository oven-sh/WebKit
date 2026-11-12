/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTableParts.h"

#if ENABLE(AOT)

#include "AOTIDPrefix.h"
#include "AOTIRProgram.h"
#include "AOTPlan.h"
#include "AOTSort.h"
#include "AOTStringUses.h"
#include "AOTThreads.h"
#include "AOTTypeText.h"
#include <algorithm>
#include <bit>
#include <utility>
#include <wtf/MathExtras.h>
#include <wtf/StdLibExtras.h>
#include <wtf/StringPrintStream.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

namespace JSC { namespace AOT {

static constexpr size_t alignmentOfTablePart = 8;
static_assert(sizeof(ProgramTables) >= alignmentOfTablePart);

namespace {

struct ElementOfJoinedTable {
    ASCIILiteral table;
    uint32_t size;
    uint32_t alignment;
};

struct SharedBytesOfTablePart {
    uint32_t offset;
    uint32_t size;
    uint32_t timesHandedOut;
};

struct PlacesOfTableParts {
    std::array<uint64_t, numberOfTableParts> starts { };
    std::array<uint64_t, numberOfTableParts> sizes { };
};

} // anonymous namespace

static constexpr std::array<ElementOfJoinedTable, numberOfProgramTables> elementsOfJoinedTables { {
#define AOT_ELEMENT_OF_JOINED_TABLE(accessor, ElementType) { #accessor ""_s, sizeof(ElementType), alignof(ElementType) },
    FOR_EACH_AOT_PROGRAM_TABLE(AOT_ELEMENT_OF_JOINED_TABLE)
#undef AOT_ELEMENT_OF_JOINED_TABLE
} };

static ASCIILiteral nameOfTablePartInMessage(TablePartID id)
{
    switch (id) {
    case TablePartID::Header:
        return "Header"_s;
    case TablePartID::Types:
        return "Types"_s;
    case TablePartID::Data:
        return "Data"_s;
    case TablePartID::Layouts:
        return "Layouts"_s;
    case TablePartID::FieldTable:
        return "FieldTable"_s;
    case TablePartID::Statics:
        return "Statics"_s;
    case TablePartID::Functions:
        return "Functions"_s;
    case TablePartID::FunctionSources:
        return "FunctionSources"_s;
    case TablePartID::Modules:
        return "Modules"_s;
    case TablePartID::Requests:
        return "Requests"_s;
    case TablePartID::Constants:
        return "Constants"_s;
    case TablePartID::ShapeSets:
        return "ShapeSets"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void reportBrokenRuleOfTables(BuildContext& build, ASCIILiteral rule, FunctionID function, LayoutID layout, const String& circumstances)
{
    String detail = circumstances.isNull() ? String(rule) : makeString(rule, ": "_s, circumstances);
    build.diagnostics().add(InternalError { .kind = InternalErrorKind::ProgramTableCheckFailed, .function = function, .layout = layout, .detail = WTF::move(detail) });
}

static uint64_t hashOfBytesInTablePart(std::span<const uint8_t> bytes)
{
    constexpr uint64_t multiplier = 1099511628211ull;
    uint64_t hash = 1469598103934665603ull;
    for (; bytes.size() >= sizeof(uint64_t); bytes = bytes.subspan(sizeof(uint64_t))) {
        uint64_t word;
        memcpySpan(asMutableByteSpan(word), bytes.first(sizeof(uint64_t)));
        hash = (hash ^ word) * multiplier;
        hash ^= hash >> 32;
    }
    for (uint8_t byte : bytes)
        hash = (hash ^ byte) * multiplier;
    return hash;
}

static uint64_t keyOfSharedBytesInTablePart(unsigned kindOfBytes, std::span<const uint8_t> bytes)
{
    constexpr unsigned bitsOfKind = 4;
    static_assert(numberOfTableParts + 1 < (1u << bitsOfKind) - 1);
    RELEASE_ASSERT(kindOfBytes <= numberOfTableParts);
    return hashOfBytesInTablePart(bytes) << bitsOfKind | (kindOfBytes + 1);
}

TablePart::TablePart(TablePartID id, BuildContext& build)
    : m_id(id)
    , m_build(build)
{
    if (id != TablePartID::Header)
        return;
    growSectionTo(m_bytes, sizeof(ProgramTables));
    zeroSpan(m_bytes.mutableSpan());
}

std::optional<uint32_t> TablePart::tryAllocate(size_t size, size_t alignment, NewBytes newBytes)
{
    RELEASE_ASSERT(std::has_single_bit(alignment) && alignment <= alignmentOfTablePart);
    size_t end = m_bytes.size();
    size_t offset = roundUpToMultipleOf(alignment, std::max(end, alignmentOfTablePart));
    if (!m_didOverflow)
        m_didOverflow = !checkSectionSize(static_cast<uint64_t>(offset) + size, m_build);
    if (m_didOverflow)
        return std::nullopt;
    growSectionTo(m_bytes, offset + size);
    zeroSpan(m_bytes.mutableSpan().subspan(end, newBytes == NewBytes::AreZero ? offset + size - end : offset - end));
    return static_cast<uint32_t>(offset);
}

uint32_t TablePart::allocateUnfilled(size_t size, size_t alignment)
{
    if (std::optional<uint32_t> offset = tryAllocate(size, alignment, NewBytes::AreLeftToCaller))
        return *offset;
    return allocate(size, alignment);
}

void TablePart::reserveRoomFor(uint64_t furtherBytes)
{
    uint64_t size = m_bytes.size() + furtherBytes;
    if (size <= defaultLimitValue(LimitID::VectorBytes, hostArchitecture()) && size > m_bytes.capacity())
        m_bytes.reserveCapacity(static_cast<size_t>(size));
}

uint32_t TablePart::allocate(size_t size, size_t alignment)
{
    if (std::optional<uint32_t> offset = tryAllocate(size, alignment))
        return *offset;
    RELEASE_ASSERT(size <= defaultLimitValue(LimitID::VectorBytes, hostArchitecture()));
    if (size_t end = m_bytes.size(); size > end) {
        growSectionTo(m_bytes, size);
        zeroSpan(m_bytes.mutableSpan().subspan(end));
    }
    return 0;
}

uint32_t TablePart::appendSharedAmong(unsigned kindOfBytes, std::span<const uint8_t> bytes, size_t alignment)
{
    auto& returnedRanges = m_appended.add(keyOfSharedBytesInTablePart(kindOfBytes, bytes), Vector<std::pair<uint32_t, uint32_t>, 1> { }).iterator->value;
    size_t equal = returnedRanges.findIf([&](const std::pair<uint32_t, uint32_t>& range) {
        return range.second == bytes.size() && !(range.first % alignment) && equalSpans(m_bytes.span().subspan(range.first, range.second), bytes);
    });
    if (equal != notFound) {
        std::pair<uint32_t, uint32_t> range = returnedRanges[equal];
        if (range.second)
            returnedRanges.append(range);
        return range.first;
    }
    std::optional<uint32_t> offset = tryAllocate(bytes.size(), alignment, NewBytes::AreLeftToCaller);
    if (!offset)
        return 0;
    memcpySpan(m_bytes.mutableSpan().subspan(*offset), bytes);
    returnedRanges.insert(0, std::pair<uint32_t, uint32_t> { *offset, static_cast<uint32_t>(bytes.size()) });
    return *offset;
}

uint32_t TablePart::shareBytesWrittenInPlace(size_t endInFront, uint32_t offset, size_t size, size_t alignment)
{
    if (m_didOverflow)
        return 0;
    RELEASE_ASSERT(endInFront <= offset && offset + size == m_bytes.size());
    std::span<const uint8_t> bytes = m_bytes.span().subspan(offset, size);
    auto& returnedRanges = m_appended.add(keyOfSharedBytesInTablePart(0, bytes), Vector<std::pair<uint32_t, uint32_t>, 1> { }).iterator->value;
    size_t equal = returnedRanges.findIf([&](const std::pair<uint32_t, uint32_t>& range) {
        return range.second == size && !(range.first % alignment) && equalSpans(m_bytes.span().subspan(range.first, range.second), bytes);
    });
    if (equal == notFound) {
        returnedRanges.insert(0, std::pair<uint32_t, uint32_t> { offset, static_cast<uint32_t>(size) });
        return offset;
    }
    std::pair<uint32_t, uint32_t> range = returnedRanges[equal];
    if (range.second)
        returnedRanges.append(range);
    m_bytes.shrink(endInFront);
    return range.first;
}

uint32_t TablePart::append(std::span<const uint8_t> bytes, size_t alignment)
{
    return appendSharedAmong(0, bytes, alignment);
}

uint32_t TablePart::appendOffsets(std::span<const uint32_t> offsets, TablePartID target)
{
    uint32_t place = appendSharedAmong(1 + static_cast<unsigned>(target), asBytes(offsets), alignof(uint32_t));
    linkWords(place, offsets.size(), target);
    return place;
}

uint32_t TablePart::appendUnshared(std::span<const uint8_t> bytes, size_t alignment)
{
    std::optional<uint32_t> offset = tryAllocate(bytes.size(), alignment, NewBytes::AreLeftToCaller);
    if (!offset)
        return 0;
    memcpySpan(m_bytes.mutableSpan().subspan(*offset), bytes);
    return *offset;
}

void TablePart::setTableRange(ProgramTableIndex index, uint32_t offset, uint32_t count)
{
    m_tables.append(WrittenTable { index, offset, count });
}

static Vector<SharedBytesOfTablePart> findSharedBytesOfTablePart(const auto& appended)
{
    Vector<uint64_t> sharedRanges;
    for (const auto& returnedRanges : appended.values()) {
        if (returnedRanges.size() <= 1)
            continue;
        for (auto [offset, size] : returnedRanges)
            sharedRanges.append(static_cast<uint64_t>(offset) << 32 | size);
    }
    radixSortKeepingEqualKeysInOrder<64>(sharedRanges, [](uint64_t offsetAndSize) {
        return offsetAndSize;
    });
    Vector<SharedBytesOfTablePart> shared;
    for (uint64_t offsetAndSize : sharedRanges) {
        uint32_t offset = static_cast<uint32_t>(offsetAndSize >> 32);
        uint32_t size = static_cast<uint32_t>(offsetAndSize);
        if (!size)
            continue;
        if (!shared.isEmpty() && shared.last().offset == offset)
            ++shared.last().timesHandedOut;
        else
            shared.append({ offset, size, 1 });
    }
    shared.removeAllMatching([](const SharedBytesOfTablePart& bytes) {
        return bytes.timesHandedOut == 1;
    });
    return shared;
}

static void applyLinksOfTablePart(TablePartID id, std::span<uint8_t> bytes, std::span<Vector<uint32_t>> linksByTarget, std::span<const SharedBytesOfTablePart> shared, const PlacesOfTableParts& places, BuildContext& build)
{
    for (unsigned target = 0; target < numberOfTableParts; ++target) {
        Vector<uint32_t>& links = linksByTarget[target];
        radixSortKeepingEqualKeysInOrder<32>(links, [](uint32_t offsetOfWord) {
            return offsetOfWord;
        });
        auto report = [&](ASCIILiteral rule, uint32_t offsetOfWord) {
            reportBrokenRuleOfTables(build, rule, { }, { }, makeString("the word at "_s, offsetOfWord, " of "_s, nameOfTablePartInMessage(id), " is linked to "_s, nameOfTablePartInMessage(static_cast<TablePartID>(target))));
        };
        size_t nextShared = 0;
        size_t next = 0;
        for (size_t index = 0; index < links.size(); index = next) {
            uint32_t offsetOfWord = links[index];
            while (next < links.size() && links[next] == offsetOfWord)
                ++next;
            if (offsetOfWord % sizeof(uint32_t) || offsetOfWord + sizeof(uint32_t) > bytes.size()) {
                report("LinkOutsideItsPart"_s, offsetOfWord);
                break;
            }
            uint32_t& word = *reinterpret_cast<uint32_t*>(bytes.subspan(offsetOfWord, sizeof(uint32_t)).data());
            if (!word)
                continue;
            if (word > places.sizes[target]) {
                report("OffsetOutsideItsPart"_s, offsetOfWord);
                break;
            }
            while (nextShared < shared.size() && shared[nextShared].offset + shared[nextShared].size <= offsetOfWord)
                ++nextShared;
            if (nextShared < shared.size() && shared[nextShared].offset <= offsetOfWord && next - index < shared[nextShared].timesHandedOut) {
                report("SharedBytesLinkedByOnlySome"_s, offsetOfWord);
                break;
            }
            word += static_cast<uint32_t>(places.starts[target]);
        }
    }
}

Vector<uint8_t> joinTableParts(std::span<TablePart> parts, ProgramTables& header, BuildContext& build)
{
    RELEASE_ASSERT(parts.size() == numberOfTableParts);
    PlacesOfTableParts places;
    for (unsigned index = 0; index < numberOfTableParts; ++index) {
        const TablePart& part = parts[index];
        RELEASE_ASSERT(part.m_id == static_cast<TablePartID>(index));
        if (part.m_didOverflow)
            return { };
        places.starts[index] = startInJoinedTables(parts, part.m_id);
        places.sizes[index] = part.size();
    }
    uint64_t size = places.starts.back() + places.sizes.back();
    if (!checkSectionSize(size, build))
        return { };

    std::span<TableRange> ranges(header.ranges);
    std::ranges::fill(ranges, TableRange { 0, 0 });
    std::array<bool, numberOfProgramTables> isWritten { };
    for (unsigned index = 0; index < numberOfTableParts; ++index) {
        for (const auto& table : parts[index].m_tables) {
            unsigned tableIndex = static_cast<unsigned>(table.index);
            const ElementOfJoinedTable& element = elementsOfJoinedTables[tableIndex];
            if (std::exchange(isWritten[tableIndex], true)) {
                reportBrokenRuleOfTables(build, "TableWrittenTwice"_s, { }, { }, String(element.table));
                continue;
            }
            if (table.offset % element.alignment || table.offset + static_cast<uint64_t>(table.count) * element.size > places.sizes[index]) {
                reportBrokenRuleOfTables(build, "TableOutOfBounds"_s, { }, { }, String(element.table));
                continue;
            }
            ranges[tableIndex] = { static_cast<uint32_t>(places.starts[index] + table.offset), table.count };
        }
    }
    for (unsigned tableIndex = 0; tableIndex < numberOfProgramTables; ++tableIndex) {
        if (!isWritten[tableIndex])
            reportBrokenRuleOfTables(build, "TableNotWritten"_s, { }, { }, String(elementsOfJoinedTables[tableIndex].table));
    }

    Vector<uint8_t> bytes;
    growSectionTo(bytes, size);
    std::span<uint8_t> joined = bytes.mutableSpan();
    constexpr size_t bytesInCopiedRange = MB;
    Vector<std::pair<unsigned, size_t>> copiedRanges;
    for (unsigned index = 0; index < numberOfTableParts; ++index) {
        for (size_t start = 0; start < parts[index].size(); start += bytesInCopiedRange)
            copiedRanges.append({ index, start });
    }
    forEachIndexInParallel(build.parallelLoops(), static_cast<unsigned>(copiedRanges.size()), [&](unsigned range) {
        auto [index, start] = copiedRanges[range];
        std::span<const uint8_t> source = parts[index].m_bytes.span().subspan(start);
        memcpySpan(joined.subspan(places.starts[index] + start), source.first(std::min(bytesInCopiedRange, source.size())));
    });
    forEachIndexInParallel(build.parallelLoops(), numberOfTableParts, [&](unsigned index) {
        TablePart& part = parts[index];
        uint64_t end = index + 1 < numberOfTableParts ? places.starts[index + 1] : size;
        std::span<uint8_t> destination = joined.subspan(places.starts[index], end - places.starts[index]);
        zeroSpan(destination.subspan(part.size()));
        if (std::ranges::all_of(part.m_links, [](const Vector<uint32_t>& links) { return links.isEmpty(); }))
            return;
        Vector<SharedBytesOfTablePart> shared = findSharedBytesOfTablePart(part.m_appended);
        applyLinksOfTablePart(part.m_id, destination.first(part.size()), part.m_links, shared.span(), places, build);
    });
    memcpySpan(joined, asByteSpan(header));
    return bytes;
}

template<typename Record>
static String describeRecordOfRow(const Typed::Program& program, const Vector<Record>& records, Typed::IDPrefix prefix, uint32_t index)
{
    const Record* record = index < records.size() ? &records[index] : nullptr;
    StringBuilder builder;
    if (record && record->name)
        builder.append('\'', program.strings.toWTFString(record->name), "' "_s);
    builder.append(letterOf(prefix), index);
    if (record && record->module)
        builder.append(" of "_s, program.strings.toWTFString(program.at(record->module).displayPath));
    return builder.toString();
}

static String describeRowOwner(const Typed::Program& program, RowOwner owner)
{
    if (owner.function)
        return describeRecordOfRow(program, program.functions, Typed::IDPrefix::Function, owner.function.index());
    if (owner.layout)
        return describeRecordOfRow(program, program.layouts, Typed::IDPrefix::Layout, owner.layout.index());
    if (owner.variable)
        return describeRecordOfRow(program, program.variables, Typed::IDPrefix::Variable, owner.variable.index());
    if (owner.module)
        return makeString(letterOf(Typed::IDPrefix::Module), owner.module.index(), ", "_s, program.strings.toWTFString(program.at(owner.module).displayPath));
    return { };
}

static String describeNameWithoutIdentifier(const Typed::Program& program, StringID text, ASCIILiteral use, RowOwner owner)
{
    return makeString('\'', text ? program.strings.toWTFString(text) : String(), "': the name is "_s, use, ' ', describeRowOwner(program, owner));
}

static String describeTypeWithoutRecord(const Typed::Program& program, TypeID type)
{
    if (Typed::liesInThread(type))
        return "a type that a thread made while it compiled"_s;
    constexpr uint32_t mostMembersInMessage = 8;
    StringPrintStream out;
    Typed::printType(out, program.types, type, { .mostMembersPrinted = mostMembersInMessage });
    return out.toString();
}

bool showsVariablesOf(const TableInputs& inputs, ModuleID id)
{
    if (inputs.options.obfuscates() || !id)
        return false;
    const Typed::Module& module = inputs.program.at(id);
    return Typed::isProgramModuleOf(module.kind) && !module.flags.contains(Typed::ModuleFlags::IsLibrary);
}

TableEncoder::TableEncoder(const TableInputs& inputs, const ImageNumbers& numbers, const FirstTableParts* firstParts, TablePartID id, BuildContext& build)
    : inputs(inputs)
    , program(inputs.program)
    , facts(inputs.facts)
    , plan(inputs.plan)
    , numbers(numbers)
    , firstParts(firstParts)
    , build(build)
    , part(id, build)
{
}

uint32_t TableEncoder::stringNumber(StringID id) const
{
    StringID string = id ? id : program.strings.emptyString();
    std::optional<uint32_t> number = numbers.stringNumbers().numberOf(string);
    if (!number)
        fail("StringWithoutRecord"_s, { }, makeString('\'', program.strings.toWTFString(string), '\''));
    return number.value_or(0);
}

uint32_t TableEncoder::stringConstantNumber(StringID id) const
{
    std::optional<uint32_t> number = numbers.stringConstants().numberOf(id);
    if (!number)
        fail("StringWithoutConstantNumber"_s, { }, id ? makeString('\'', program.strings.toWTFString(id), '\'') : String());
    return number.value_or(0);
}

uint32_t TableEncoder::identifierNumber(PropertyKeyID key, ASCIILiteral use, RowOwner owner) const
{
    std::optional<uint32_t> number = numbers.identifiers().numberOf(key);
    if (!number)
        reportBrokenRuleOfTables(build, "NameWithoutIdentifier"_s, owner.function, owner.layout, describeNameWithoutIdentifier(program, key ? program.at(key).text : StringID(), use, owner));
    return number.value_or(0);
}

uint32_t TableEncoder::identifierNumberOfText(StringID text, ASCIILiteral use, RowOwner owner) const
{
    std::optional<uint32_t> number = numbers.identifiers().numberOfText(text);
    if (!number)
        reportBrokenRuleOfTables(build, "NameWithoutIdentifier"_s, owner.function, owner.layout, describeNameWithoutIdentifier(program, text, use, owner));
    return number.value_or(0);
}

uint32_t TableEncoder::identifierOrStringNumber(StringID text) const
{
    if (std::optional<uint32_t> number = numbers.identifiers().numberOfText(text))
        return *number;
    uint32_t number = stringNumber(text);
    if (number > Number24::max)
        build.reportLimitExceeded(LimitID::MaxIdentifiers, number);
    return number;
}

uint32_t TableEncoder::typeOffset(TypeID id) const
{
    TypeID type = id ? id : Typed::TypeTable::anything;
    std::optional<uint32_t> place = numbers.placeOfType(type);
    if (firstParts && place && *place < firstParts->typeOffsets.size()) {
        if (uint32_t offset = firstParts->typeOffsets[*place])
            return offset;
    }
    fail("TypeNotEncoded"_s, { }, describeTypeWithoutRecord(program, type));
    return 0;
}

uint16_t TableEncoder::typeNumber(TypeID id) const
{
    uint32_t offset = typeOffset(id);
    return offset ? safeCast<uint16_t>((FirstTableParts::startOfTypes + offset) >> firstParts->shiftOfTypeNumbers) : 0;
}

uint32_t TableEncoder::dataNodeOf(DataID id) const
{
    if (firstParts && id && id.index() < firstParts->dataNodes.size()) {
        if (uint32_t node = firstParts->dataNodes[id.index()])
            return node;
    }
    fail("DataNotEncoded"_s, { }, id ? makeString(letterOf(Typed::IDPrefix::Data), id.index()) : String());
    return 0;
}

std::optional<FunctionID> TableEncoder::findEntrypoint(FunctionID function) const
{
    if (!function)
        return std::nullopt;
    const Typed::CallingConventions& conventions = plan.conventions();
    FunctionID entrypoint = numbers.functions().idOf(conventions.indexOf(conventions.closureEntryOf(function)));
    return entrypoint ? std::optional(entrypoint) : std::nullopt;
}

uint32_t TableEncoder::entrypointOf(FunctionID function, ASCIILiteral use, RowOwner owner) const
{
    std::optional<FunctionID> entrypoint = findEntrypoint(function);
    if (function && !entrypoint)
        reportBrokenRuleOfTables(build, "FunctionWithoutEntrypoint"_s, function, { }, makeString("the function is "_s, use, ' ', describeRowOwner(program, owner)));
    return entrypoint.value_or(FunctionID()).index();
}

bool TableEncoder::hasCode(CodeUnit unit) const
{
    const Typed::CallingConventions& conventions = plan.conventions();
    return unit && conventions.exists(unit) && inputs.functions.positionOfUnit(unit, conventions.indexOf(unit));
}

void TableEncoder::fail(ASCIILiteral rule, RowOwner owner, const String& circumstances) const
{
    if (!owner.variable && !owner.module) {
        reportBrokenRuleOfTables(build, rule, owner.function, owner.layout, circumstances);
        return;
    }
    String row = makeString("in the row of "_s, describeRowOwner(program, owner));
    reportBrokenRuleOfTables(build, rule, owner.function, owner.layout, circumstances.isNull() ? row : makeString(circumstances, ", "_s, row));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
