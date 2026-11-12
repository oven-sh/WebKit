/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTableParts.h"

#if ENABLE(AOT)

#include "AOTClassMemberTable.h"
#include "AOTFieldTable.h"
#include "AOTIRProgram.h"
#include "AOTPhaseTimes.h"
#include "AOTPlan.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include <algorithm>
#include <array>
#include <bit>
#include <compare>
#include <cstddef>
#include <optional>
#include <span>
#include <tuple>
#include <wtf/MathExtras.h>

namespace JSC { namespace AOT {

static constexpr uint32_t fieldTableRowWithoutEntries = 0;

namespace {

using WordsOfFieldRecord = std::array<uint32_t, sizeof(FieldData) / sizeof(uint32_t)>;

struct FieldTableRowEntry {
    friend auto operator<=>(const FieldTableRowEntry&, const FieldTableRowEntry&) = default;

    uint32_t name;
    uint32_t entry;
    uint16_t recordIndex;
    WordsOfFieldRecord recordThatStoresGoBy { };
};

struct FieldTableRow {
    uint32_t numberOfReaders() const { return firstShape ? endOfShapes - firstShape : 1; }

    uint32_t firstShape { 0 };
    uint32_t endOfShapes { 0 };
    uint32_t rankOfClass { 0 };
    uint32_t size { 0 };
    size_t begin { 0 };
    uint32_t firstEntry { fieldTableRowWithoutEntries };
    bool repeatsRowInFront { false };
};

struct FieldTableRows {
    std::span<const FieldTableRowEntry> entriesOf(const FieldTableRow& row) const { return entries.span().subspan(row.begin, row.size); }

    Vector<FieldTableRowEntry> entries;
    Vector<FieldTableRow> rows;
};

struct ClassInFieldTable {
    std::optional<Vector<DeclaredClassMember>> members;
    uint32_t numberOfAccessors { 0 };
    uint32_t firstAccessor { 0 };
};

struct LayoutRecordForFieldTable {
    ExtensionLayoutData extension { };
    LayoutData layout { };
};
static_assert(offsetof(LayoutRecordForFieldTable, layout) == sizeof(ExtensionLayoutData));

struct TypeRecordForFieldTable {
    RuntimeType header { };
    std::array<uint32_t, 4> integerRange { };
};
static_assert(offsetof(TypeRecordForFieldTable, integerRange) == sizeof(RuntimeType));

class FieldTableBits {
public:
    static constexpr uint32_t bitsPerWindow = 64;

    uint64_t clearBitsInWindowAt(uint32_t index) const
    {
        size_t word = index / 64;
        unsigned shift = index % 64;
        uint64_t bits = wordAt(word) >> shift;
        if (shift)
            bits |= wordAt(word + 1) << (64 - shift);
        return ~bits;
    }

    void set(uint32_t index)
    {
        size_t word = index / 64;
        growToHold(m_words, word);
        m_words[word] |= 1ull << index % 64;
        if (~m_words[word])
            return;
        growToHold(m_fullWords, word / 64);
        m_fullWords[word / 64] |= 1ull << word % 64;
    }

    uint32_t firstClearBitFrom(uint32_t index) const
    {
        size_t word = index / 64;
        uint64_t clearBits = ~wordAt(word) & ~0ull << index % 64;
        if (!clearBits) {
            word = firstWordWithClearBitFrom(word + 1);
            clearBits = ~wordAt(word);
        }
        return static_cast<uint32_t>(word * 64 + WTF::ctz(clearBits));
    }

private:
    static void growToHold(Vector<uint64_t>& words, size_t index)
    {
        if (index >= words.size())
            words.insertFill(words.size(), 0, std::max(index + 1, words.size() * 2) - words.size());
    }

    uint64_t wordAt(size_t word) const { return word < m_words.size() ? m_words[word] : 0; }

    size_t firstWordWithClearBitFrom(size_t word) const
    {
        uint64_t mask = ~0ull << word % 64;
        for (size_t group = word / 64; group < m_fullWords.size(); ++group, mask = ~0ull) {
            if (uint64_t wordsWithClearBit = ~m_fullWords[group] & mask)
                return group * 64 + WTF::ctz(wordsWithClearBit);
        }
        return std::max(word, m_fullWords.size() * 64);
    }

    Vector<uint64_t> m_words;
    Vector<uint64_t> m_fullWords;
};

class FieldTablePacker {
public:
    FieldTablePacker()
    {
        m_firstEntries.set(fieldTableRowWithoutEntries);
    }

    uint32_t highestFirstEntry() const { return m_highestFirstEntry; }

    std::optional<uint32_t> place(std::span<const FieldTableRowEntry> row)
    {
        bool startsOver = row.size() != m_sizeOfLastRow || row.size() == 1;
        if (startsOver && somePlaceInFrontMayTake(row.size()))
            m_entryOfFirstNameOfLastRow = 0;

        const uint32_t firstName = row[0].name;
        uint32_t firstEntry = m_entryOfFirstNameOfLastRow > firstName ? m_entryOfFirstNameOfLastRow - firstName : 0;
        for (;;) {
            if (firstEntry > maxFirstEntry)
                return std::nullopt;
            uint64_t fitting = m_firstEntries.clearBitsInWindowAt(firstEntry);
            size_t numberOfNamesTried = 0;
            while (fitting && numberOfNamesTried < row.size())
                fitting &= m_occupiedEntries.clearBitsInWindowAt(firstEntry + row[numberOfNamesTried++].name);
            if (fitting) {
                firstEntry += WTF::ctz(fitting);
                break;
            }
            firstEntry += FieldTableBits::bitsPerWindow;
            if (numberOfNamesTried) {
                uint32_t name = row[numberOfNamesTried - 1].name;
                firstEntry = m_occupiedEntries.firstClearBitFrom(firstEntry + name) - name;
            } else
                firstEntry = m_firstEntries.firstClearBitFrom(firstEntry);
        }

        m_firstEntries.set(firstEntry);
        for (const FieldTableRowEntry& entry : row)
            m_occupiedEntries.set(firstEntry + entry.name);
        m_numberOfOccupiedEntries += row.size();
        m_endOfOccupiedEntries = std::max<uint64_t>(m_endOfOccupiedEntries, static_cast<uint64_t>(firstEntry) + row.back().name + 1);
        m_highestFirstEntry = std::max(m_highestFirstEntry, firstEntry);
        m_entryOfFirstNameOfLastRow = firstEntry + firstName;
        m_sizeOfLastRow = row.size();
        return firstEntry;
    }

private:
    bool somePlaceInFrontMayTake(size_t numberOfNames) const
    {
        constexpr unsigned fractionBits = 16;
        constexpr uint64_t searchesThatMayFindOnePlace = 16;
        if (!m_endOfOccupiedEntries)
            return true;
        uint64_t shareOfFreeEntries = ((m_endOfOccupiedEntries - m_numberOfOccupiedEntries) << fractionBits) / m_endOfOccupiedEntries;
        uint64_t placesExpectedToFit = (m_entryOfFirstNameOfLastRow * searchesThatMayFindOnePlace) << fractionBits;
        for (size_t name = 0; name < numberOfNames && placesExpectedToFit >> fractionBits; ++name)
            placesExpectedToFit = (placesExpectedToFit * shareOfFreeEntries) >> fractionBits;
        return placesExpectedToFit >> fractionBits;
    }

    static constexpr uint32_t maxFirstEntry = UINT32_MAX / sizeof(FieldTableEntry);

    FieldTableBits m_occupiedEntries;
    FieldTableBits m_firstEntries;
    uint64_t m_numberOfOccupiedEntries { 0 };
    uint64_t m_endOfOccupiedEntries { 0 };
    size_t m_sizeOfLastRow { 0 };
    uint32_t m_entryOfFirstNameOfLastRow { 0 };
    uint32_t m_highestFirstEntry { fieldTableRowWithoutEntries };
};

} // anonymous namespace

static LayoutRecordForFieldTable describeLayoutForFieldTable(const Typed::Plan& plan, LayoutID id)
{
    LayoutRecordForFieldTable record;
    if (plan.layout(id).hasExtension()) {
        record.layout.hasExtensionData = true;
        record.extension.extension = plan.slots().extensionWord(id).data();
    }
    return record;
}

static TypeRecordForFieldTable describeTypeForFieldTable(const Typed::Type& type)
{
    auto lowHalf = [](int64_t integer) { return static_cast<uint32_t>(integer); };
    auto highHalf = [](int64_t integer) { return static_cast<uint32_t>(static_cast<uint64_t>(integer) >> 32); };

    TypeRecordForFieldTable record;
    record.header.primitives = type.primitives;
    if (type.primitives & Typed::Primitive::Integer && type.integers != Typed::Range::full()) {
        record.header.flags = RuntimeType::hasIntegerRange;
        record.integerRange = { lowHalf(type.integers.minimum), highHalf(type.integers.minimum), lowHalf(type.integers.maximum), highHalf(type.integers.maximum) };
    }
    return record;
}

static LayoutID layoutOfRecordForFieldTable(const TableEncoder& encoder, uint32_t shape)
{
    const Typed::ShapeTable& shapes = encoder.plan.shapes();
    if (!shapes.isAssigned(shape) || !encoder.plan.isAllocatable(shapes.shape(shape).layout))
        return { };
    return shapes.shape(shape).layout;
}

static void fillFieldTableRowOfShapes(const TableEncoder& encoder, uint32_t highestObjectShape, uint16_t numberOfAnyType, FieldTableRow& row, std::span<FieldTableRowEntry> entriesOfAllRows)
{
    const IdentifierNumbering& identifiers = encoder.numbers.identifiers();
    LayoutID id = layoutOfRecordForFieldTable(encoder, row.firstShape);
    row.endOfShapes = row.firstShape + 1;
    while (row.endOfShapes <= highestObjectShape && layoutOfRecordForFieldTable(encoder, row.endOfShapes) == id)
        ++row.endOfShapes;

    const Typed::PlannedLayout& planned = encoder.plan.layout(id);
    const Vector<FieldData>& fields = encoder.firstParts->fieldsOfLayoutRecords[id.index()].fields;
    LayoutRecordForFieldTable record = describeLayoutForFieldTable(encoder.plan, id);
    std::span<FieldTableRowEntry> room = entriesOfAllRows.subspan(row.begin, fields.size());
    for (const Typed::PlacedSlot& slot : planned.slots.span().first(planned.numberOfFields())) {
        if (!slot.indexInLayoutRecord || *slot.indexInLayoutRecord >= fields.size())
            continue;
        const FieldData& field = fields[*slot.indexInLayoutRecord];
        if (!field.name || !identifiers.hasFieldTableColumn(field.name))
            continue;
        TypeRecordForFieldTable type = describeTypeForFieldTable(encoder.program.at(slot.typeInLayoutRecord() ? slot.typeInLayoutRecord() : TypeID(Typed::TypeTable::anything)));
        RELEASE_ASSERT(row.size < room.size());
        const uint16_t recordIndex = FieldRecordIndex::create(field, *slot.indexInLayoutRecord, type.header, numberOfAnyType).bits;
        room[row.size++] = { field.name, FieldTableEntry::forField(record.layout, field).bits, recordIndex, recordIndex & FieldRecordIndex::isStoredWithoutTypeTest ? WordsOfFieldRecord { } : std::bit_cast<WordsOfFieldRecord>(field) };
    }
    AOT::sortInLine(room.first(row.size));
}

static void fillFieldTableRowOfClass(const ClassInFieldTable& declared, FieldTableRow& row, std::span<FieldTableRowEntry> entriesOfAllRows, std::span<uint64_t> accessors)
{
    std::span<FieldTableRowEntry> room = entriesOfAllRows.subspan(row.begin, declared.members->size());
    uint32_t nextAccessor = declared.firstAccessor;
    for (const DeclaredClassMember& member : *declared.members) {
        ClassRowEntry::Form form = ClassRowEntry::Form::Method;
        uint32_t number = member.method;
        if (!member.method) {
            form = ClassRowEntry::Form::Accessor;
            number = nextAccessor++;
            accessors[number] = std::bit_cast<uint64_t>(ClassAccessorData { member.getter, member.setter });
        }
        room[row.size++] = { member.name, ClassRowEntry::create(member.name, form, number).bits, ClassRowEntry::upperBitsOf(number) };
    }
    AOT::sortInLine(room);
}

static std::optional<Vector<ClassInFieldTable>> collectClassesForFieldTable(const TableEncoder& encoder)
{
    const ParallelLoops& loops = encoder.build.parallelLoops();
    const IdentifierNumbering& identifiers = encoder.numbers.identifiers();
    Vector<uint32_t> layouts = findIndicesInParallel(loops, static_cast<unsigned>(encoder.program.layouts.size()), [&](unsigned index) {
        return encoder.numbers.rankOfClass(LayoutID(index)).has_value();
    });
    for (uint32_t rank = 0; rank < layouts.size(); ++rank) {
        if (encoder.numbers.rankOfClass(LayoutID(layouts[rank])) != rank) {
            encoder.fail("RanksOfClassesDoNotCountClassesInFront"_s, LayoutID(layouts[rank]));
            return std::nullopt;
        }
    }

    Vector<ClassInFieldTable> classes(layouts.size());
    forEachIndexInParallel(loops, static_cast<unsigned>(classes.size()), [&](unsigned rank) {
        ClassInFieldTable& declared = classes[rank];
        declared.members = collectDeclaredClassMembers(encoder, LayoutID(layouts[rank]));
        if (!declared.members)
            return;
        declared.members->removeAllMatching([&](const DeclaredClassMember& member) {
            return !identifiers.hasFieldTableColumn(member.name);
        });
        declared.numberOfAccessors = static_cast<uint32_t>(std::ranges::count(*declared.members, 0u, &DeclaredClassMember::method));
    });
    uint32_t numberOfAccessors = 0;
    for (ClassInFieldTable& declared : classes) {
        declared.firstAccessor = numberOfAccessors;
        numberOfAccessors += declared.numberOfAccessors;
    }
    return classes;
}

static FieldTableRows collectFieldTableRows(const TableEncoder& encoder, uint32_t highestObjectShape, std::span<const ClassInFieldTable> classes, std::span<uint64_t> accessors)
{
    const ParallelLoops& loops = encoder.build.parallelLoops();
    const Typed::ShapeTable& shapes = encoder.plan.shapes();
    const Vector<FieldsOfLayoutRecord>& fieldsOfLayoutRecords = encoder.firstParts->fieldsOfLayoutRecords;
    const uint16_t numberOfAnyType = encoder.typeNumber(Typed::TypeTable::anything);

    Vector<uint32_t> firstShapes = findIndicesInParallel(loops, highestObjectShape + 1, [&](unsigned shape) {
        LayoutID id = layoutOfRecordForFieldTable(encoder, shape);
        return id && id != layoutOfRecordForFieldTable(encoder, shape - 1);
    });

    FieldTableRows rows;
    rows.rows = Vector<FieldTableRow>(firstShapes.size() + classes.size());
    size_t room = 0;
    for (size_t index = 0; index < firstShapes.size(); ++index) {
        FieldTableRow& row = rows.rows[index];
        row.firstShape = firstShapes[index];
        row.begin = room;
        room += fieldsOfLayoutRecords[shapes.shape(row.firstShape).layout.index()].fields.size();
    }
    for (size_t rank = 0; rank < classes.size(); ++rank) {
        FieldTableRow& row = rows.rows[firstShapes.size() + rank];
        row.rankOfClass = static_cast<uint32_t>(rank);
        row.begin = room;
        room += classes[rank].members ? classes[rank].members->size() : 0;
    }
    rows.entries.grow(room);

    forEachIndexInParallel(loops, static_cast<unsigned>(rows.rows.size()), [&](unsigned index) {
        FieldTableRow& row = rows.rows[index];
        if (row.firstShape)
            fillFieldTableRowOfShapes(encoder, highestObjectShape, numberOfAnyType, row, rows.entries.mutableSpan());
        else if (classes[row.rankOfClass].members)
            fillFieldTableRowOfClass(classes[row.rankOfClass], row, rows.entries.mutableSpan(), accessors);
    });
    rows.rows.removeAllMatching([](const FieldTableRow& row) {
        return !row.size;
    });
    return rows;
}

static void sortFieldTableRows(const ParallelLoops& loops, FieldTableRows& rows)
{
    parallelSort(loops, rows.rows.mutableSpan(), [&](const FieldTableRow& a, const FieldTableRow& b) {
        if (a.size != b.size)
            return a.size > b.size;
        std::span<const FieldTableRowEntry> entriesOfA = rows.entriesOf(a);
        std::span<const FieldTableRowEntry> entriesOfB = rows.entriesOf(b);
        auto order = std::lexicographical_compare_three_way(entriesOfA.begin(), entriesOfA.end(), entriesOfB.begin(), entriesOfB.end());
        return order != 0 ? order < 0 : std::tie(a.firstShape, a.rankOfClass) < std::tie(b.firstShape, b.rankOfClass);
    });
    forEachRangeOfIndicesInParallel(loops, rows.rows.size(), [&](size_t first, size_t end) {
        for (size_t index = std::max<size_t>(first, 1); index < end; ++index)
            rows.rows[index].repeatsRowInFront = std::ranges::equal(rows.entriesOf(rows.rows[index]), rows.entriesOf(rows.rows[index - 1]));
    });
}

static bool placeFieldTableRows(FieldTableRows& rows, FieldTablePacker& packer)
{
    for (size_t index = 0; index < rows.rows.size(); ++index) {
        FieldTableRow& row = rows.rows[index];
        if (row.repeatsRowInFront) {
            row.firstEntry = rows.rows[index - 1].firstEntry;
            continue;
        }
        std::optional<uint32_t> firstEntry = packer.place(rows.entriesOf(row));
        if (!firstEntry)
            return false;
        row.firstEntry = *firstEntry;
    }
    return true;
}

template<typename Word>
static void writeEntriesOfFieldTableRows(const ParallelLoops& loops, const FieldTableRows& rows, std::span<Word> entries, Word FieldTableRowEntry::* member)
{
    forEachIndexInParallel(loops, static_cast<unsigned>(rows.rows.size()), [&](unsigned index) {
        const FieldTableRow& row = rows.rows[index];
        if (row.repeatsRowInFront)
            return;
        for (const FieldTableRowEntry& entry : rows.entriesOf(row))
            entries[row.firstEntry + entry.name] = entry.*member;
    });
}

template<typename Word>
static void zeroWordsOfFieldTable(const ParallelLoops& loops, std::span<Word> words)
{
    forEachRangeOfIndicesInParallel(loops, words.size(), [&](size_t first, size_t end) {
        zeroSpan(words.subspan(first, end - first));
    });
}

void encodeFieldTable(TableEncoder& encoder)
{
    RELEASE_ASSERT(encoder.plan.shapes().firstEnvironmentShape());
    const uint32_t highestObjectShape = encoder.plan.shapes().firstEnvironmentShape() - 1;
    const uint32_t numberOfColumns = encoder.numbers.identifiers().countWithFieldTableColumn();
    RELEASE_ASSERT(numberOfColumns && numberOfColumns <= FieldTableEntry::maxName + 1);
    const ParallelLoops& loops = encoder.build.parallelLoops();
    StepTimes times(encoder.build, "image: field table"_s);

    std::optional<Vector<ClassInFieldTable>> classes = collectClassesForFieldTable(encoder);
    if (!classes)
        return;
    Vector<uint64_t> accessors;
    accessors.grow(classes->isEmpty() ? 0 : classes->last().firstAccessor + classes->last().numberOfAccessors);
    FieldTableRows rows = collectFieldTableRows(encoder, highestObjectShape, classes->span(), accessors.mutableSpan());
    times.didFinish("collect rows"_s);
    sortFieldTableRows(loops, rows);
    times.didFinish("sort rows"_s);

    FieldTablePacker packer;
    if (!placeFieldTableRows(rows, packer)) {
        encoder.fail("FieldTableOutgrowsOffsetsOfTables"_s);
        return;
    }
    Vector<uint32_t> firstEntryOfClass(classes->size(), [&](size_t rank) {
        return classes->at(rank).members ? fieldTableRowWithoutEntries : ClassRowEntry::noRow;
    });
    for (const FieldTableRow& row : rows.rows) {
        if (!row.firstShape)
            firstEntryOfClass[row.rankOfClass] = row.firstEntry;
    }
    times.didFinish("place rows"_s);

    const uint32_t numberOfEntries = packer.highestFirstEntry() + numberOfColumns;
    const size_t numberOfWords = static_cast<size_t>(highestObjectShape) + numberOfEntries;
    constexpr size_t mostBytesInFrontOfTable = 8;
    encoder.part.reserveRoomFor(numberOfWords * sizeof(uint32_t) + numberOfEntries * sizeof(uint16_t) + firstEntryOfClass.size() * sizeof(uint32_t) + accessors.size() * sizeof(uint64_t) + 4 * mostBytesInFrontOfTable);

    encoder.part.setTableInPlace<uint32_t>(ProgramTableIndex::fieldTable, numberOfWords, [&](std::span<uint32_t> table) {
        static_assert(!fieldTableRowWithoutEntries);
        zeroWordsOfFieldTable(loops, table);
        forEachIndexInParallel(loops, static_cast<unsigned>(rows.rows.size()), [&](unsigned index) {
            const FieldTableRow& row = rows.rows[index];
            for (uint32_t shape = row.firstShape; shape < row.endOfShapes; ++shape)
                table[highestObjectShape - shape] = row.firstEntry;
        });
        writeEntriesOfFieldTableRows(loops, rows, table.subspan(highestObjectShape), &FieldTableRowEntry::entry);
    });
    encoder.part.setTableInPlace<uint16_t>(ProgramTableIndex::fieldRecordIndices, numberOfEntries, [&](std::span<uint16_t> recordIndices) {
        zeroWordsOfFieldTable(loops, recordIndices);
        writeEntriesOfFieldTableRows(loops, rows, recordIndices, &FieldTableRowEntry::recordIndex);
    });
    encoder.part.setTable(ProgramTableIndex::classMemberRows, firstEntryOfClass);
    encoder.part.setTable(ProgramTableIndex::classAccessors, accessors);
    times.didFinish("write"_s);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
