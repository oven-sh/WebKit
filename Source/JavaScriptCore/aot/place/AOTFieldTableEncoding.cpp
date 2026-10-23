/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTProgramTableEncoder.h"

#if ENABLE(AOT)

#include "AOTClassMemberTable.h"
#include "AOTFieldTable.h"
#include "AOTSort.h"
#include "AOTVector.h"
#include <algorithm>
#include <span>
#include <wtf/MathExtras.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct NamedField {
    friend auto operator<=>(const NamedField&, const NamedField&) = default;

    uint32_t name;
    uint32_t entry;
    uint16_t field;
};

// The row of a shape, or of the class with a rank where the shape is zero.
struct Row {
    uint32_t size() const { return end - begin; }

    uint32_t shape;
    uint32_t begin;
    uint32_t end;
    uint32_t rankOfClass { 0 };
};

// The fields of every compiled object layout whose names have a column, layout by layout, each in ascending order of names. Behind them, class by class, the declared members.
struct NamedFields {
    std::span<const NamedField> fieldsOf(const Row& row) const { return fields.span().subspan(row.begin, row.size()); }

    Vector<NamedField> fields;
    Vector<Row> rows;
};

class Bitmap {
public:
    static constexpr uint32_t bitsPerBatch = 64;

    uint64_t clearBitsFrom(uint32_t index) const
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
        if (word >= m_words.size())
            m_words.insertFill(m_words.size(), 0, std::max(word + 1, m_words.size() * 2) - m_words.size());
        m_words[word] |= 1ull << index % 64;
    }

    // Every bit below the result is set.
    uint32_t firstBitThatMayBeClear()
    {
        while (m_firstWordWithClearBit < m_words.size() && !~m_words[m_firstWordWithClearBit])
            ++m_firstWordWithClearBit;
        return static_cast<uint32_t>(m_firstWordWithClearBit * 64);
    }

private:
    uint64_t wordAt(size_t word) const { return word < m_words.size() ? m_words[word] : 0; }

    Vector<uint64_t> m_words;
    size_t m_firstWordWithClearBit { 0 };
};

} // anonymous namespace

static NamedFields collectNamedFields(TableEncoder& encoder, uint32_t highestObjectShape)
{
    const uint32_t typeOfAnyValue = encoder.typeOffset(TypeTable::anything);
    TableRange layouts = encoder.header.ranges[static_cast<unsigned>(ProgramTableIndex::layouts)];
    NamedFields result;
    for (uint32_t shape = 1; shape <= highestObjectShape; ++shape) {
        uint32_t offset = encoder.writer.at<uint32_t>(layouts.offset + shape * sizeof(uint32_t));
        if (!offset)
            continue;
        const LayoutData& layout = encoder.writer.at<LayoutData>(offset);
        auto begin = static_cast<uint32_t>(result.fields.size());
        for (const FieldData& field : layout.fields()) {
            if (FieldTableEntry::hasColumn(field.name))
                result.fields.append({ field.name, FieldTableEntry::forField(layout, field).bits, FieldRecordIndex::create(field, layout.indexOf(field), encoder.writer.at<RuntimeType>(field.type), typeOfAnyValue).bits });
        }
        auto end = static_cast<uint32_t>(result.fields.size());
        if (begin == end)
            continue;
        AOT::sort(result.fields.mutableSpan().subspan(begin));
        result.rows.append({ shape, begin, end });
    }
    return result;
}

static std::optional<uint32_t> findNumberOfConstructorName(const TableEncoder& encoder)
{
    const auto numberOfColumns = static_cast<uint32_t>(std::min<uint64_t>(encoder.identifiers.count(), FieldTableEntry::maxName + 1));
    for (uint32_t number = 1; number < numberOfColumns; ++number) {
        PropertyKeyID key = encoder.identifiers.at(number).key;
        if (key && encoder.program.nameClassOf(key) == NameClass::Constructor)
            return number;
    }
    return std::nullopt;
}

static std::optional<Vector<DeclaredClassMember>> collectDeclaredClassMembers(const TableEncoder& encoder, const LayoutData& classLayout, std::optional<uint32_t> nameOfConstructor)
{
    Vector<DeclaredClassMember> members;
    auto memberNamed = [&](uint32_t name) -> DeclaredClassMember& {
        size_t index = members.findIf([&](const DeclaredClassMember& member) {
            return member.name == name;
        });
        if (index != notFound)
            return members[index];
        members.append(DeclaredClassMember { name });
        return members.last();
    };
    auto cellOf = [&](uint32_t function) -> uint32_t {
        uint32_t cell = encoder.placement.closureCellNumber(FunctionID(function));
        return cell <= ClassRowEntry::maxNumber ? cell : 0;
    };

    if (!classLayout.classData().classConstructor)
        return std::nullopt;
    // ClassTable::DeclaredMember::define(): a later definition of a name replaces an earlier one, and the halves of an accessor add up.
    for (const MethodData& method : classLayout.methods()) {
        if (method.flags & (methodHasComputedName | methodIsCreatedAtDefinition))
            return std::nullopt;
        uint32_t cell = cellOf(method.function);
        if (!cell)
            return std::nullopt;
        // Nobody asks the table for a name without a column.
        if (!FieldTableEntry::hasColumn(method.name))
            continue;
        DeclaredClassMember& member = memberNamed(method.name);
        switch (method.kind()) {
        case MethodKind::Method:
            member = DeclaredClassMember { method.name, cell };
            break;
        case MethodKind::Getter:
            member.method = 0;
            member.getter = cell;
            break;
        case MethodKind::Setter:
            member.method = 0;
            member.setter = cell;
            break;
        }
    }
    bool declaresConstructor = nameOfConstructor && members.containsIf([&](const DeclaredClassMember& member) {
        return member.name == *nameOfConstructor;
    });
    if (nameOfConstructor && !declaresConstructor) {
        uint32_t cell = cellOf(classLayout.classData().classConstructor);
        if (!cell)
            return std::nullopt;
        members.append(DeclaredClassMember { *nameOfConstructor, cell });
    }
    AOT::sort(members, { }, &DeclaredClassMember::name);
    return members;
}

std::optional<Vector<DeclaredClassMember>> collectDeclaredClassMembers(const TableEncoder& encoder, const LayoutData& classLayout)
{
    return collectDeclaredClassMembers(encoder, classLayout, findNumberOfConstructorName(encoder));
}

// Adds the rows of the classes, and returns for each class by its rank whether it has one.
static Vector<bool> collectClassMembers(TableEncoder& encoder, NamedFields& named, Vector<uint64_t>& accessors)
{
    const TableRange classes = encoder.header.ranges[static_cast<unsigned>(ProgramTableIndex::classes)];
    const uint32_t numberOfGroupWords = (encoder.header.numberOfLayoutIndices + 31) / 32 * 2;
    const uint32_t numberOfClasses = classes.count - numberOfGroupWords;
    const std::optional<uint32_t> nameOfConstructor = findNumberOfConstructorName(encoder);
    Vector<bool> hasRow(FillWith { }, numberOfClasses, false);
    for (uint32_t rank = 0; rank < numberOfClasses; ++rank) {
        uint32_t offset = encoder.writer.at<uint32_t>(classes.offset + (numberOfGroupWords + rank) * sizeof(uint32_t));
        std::optional<Vector<DeclaredClassMember>> members = collectDeclaredClassMembers(encoder, encoder.writer.at<LayoutData>(offset), nameOfConstructor);
        if (!members)
            continue;
        hasRow[rank] = true;
        auto begin = static_cast<uint32_t>(named.fields.size());
        for (const DeclaredClassMember& member : *members) {
            ClassRowEntry::Form form = member.method ? ClassRowEntry::Form::Method : ClassRowEntry::Form::Accessor;
            uint32_t number = member.method;
            if (!member.method) {
                number = static_cast<uint32_t>(accessors.size());
                accessors.append(std::bit_cast<uint64_t>(ClassAccessorData { member.getter, member.setter }));
            }
            named.fields.append({ member.name, ClassRowEntry::create(member.name, form, number).bits, ClassRowEntry::upperBitsOf(number) });
        }
        auto end = static_cast<uint32_t>(named.fields.size());
        if (begin != end)
            named.rows.append({ 0, begin, end, rank });
    }
    return hasRow;
}

// The largest rows first, while there is room for them. Rows that say the same are neighbors.
static void sortLargestFirst(NamedFields& named)
{
    AOT::sort(named.rows, [&](const Row& a, const Row& b) {
        if (a.size() != b.size())
            return a.size() > b.size();
        auto fieldsOfA = named.fieldsOf(a);
        auto fieldsOfB = named.fieldsOf(b);
        auto order = std::lexicographical_compare_three_way(fieldsOfA.begin(), fieldsOfA.end(), fieldsOfB.begin(), fieldsOfB.end());
        if (order != 0)
            return order < 0;
        if (a.shape != b.shape)
            return a.shape < b.shape;
        return a.rankOfClass < b.rankOfClass;
    });
}

// The first entry at or behind `from` where no other row begins and from where every field of the row finds its entry free.
static uint32_t findFirstEntry(const Bitmap& occupiedEntries, const Bitmap& firstEntries, std::span<const NamedField> fields, uint32_t from)
{
    for (;; from += Bitmap::bitsPerBatch) {
        uint64_t fitting = firstEntries.clearBitsFrom(from);
        for (size_t i = 0; i < fields.size() && fitting; ++i)
            fitting &= occupiedEntries.clearBitsFrom(from + fields[i].name);
        if (fitting)
            return from + WTF::ctz(fitting);
    }
}

void encodeFieldTable(TableEncoder& encoder)
{
    const uint32_t highestObjectShape = encoder.placement.shapes().firstEnvironmentShape() - 1;
    const auto numberOfColumns = static_cast<uint32_t>(std::min<uint64_t>(encoder.identifiers.count(), FieldTableEntry::maxName + 1));
    NamedFields named = collectNamedFields(encoder, highestObjectShape);
    Vector<uint64_t> accessors;
    Vector<bool> classHasRow = collectClassMembers(encoder, named, accessors);
    sortLargestFirst(named);

    // A shape without fields has a row of its own, where no other row begins: what it finds there are the entries of other rows under other names.
    constexpr uint32_t rowWithoutFields = 0;
    Vector<uint32_t> firstEntryOfShape(FillWith { }, highestObjectShape + 1, rowWithoutFields);
    Vector<uint32_t> firstEntryOfClass(classHasRow.size(), [&](size_t rank) -> uint32_t {
        return classHasRow[rank] ? rowWithoutFields : ClassRowEntry::noRow;
    });
    auto firstEntryOf = [&](const Row& row) -> uint32_t& {
        return row.shape ? firstEntryOfShape[row.shape] : firstEntryOfClass[row.rankOfClass];
    };
    Bitmap occupiedEntries;
    Bitmap firstEntries;
    firstEntries.set(rowWithoutFields);
    uint32_t highestFirstEntry = rowWithoutFields;

    // Among rows of one size the search goes on where the last one found room: a row of the same size rarely fits where that one did not.
    uint32_t entryOfFirstFieldOfLastRow = 0;
    const Row* lastRow = nullptr;
    for (const Row& row : named.rows) {
        auto fields = named.fieldsOf(row);
        if (lastRow && std::ranges::equal(fields, named.fieldsOf(*lastRow))) {
            firstEntryOf(row) = firstEntryOf(*lastRow);
            continue;
        }
        if (!lastRow || lastRow->size() != row.size() || row.size() == 1)
            entryOfFirstFieldOfLastRow = 0;
        uint32_t lowestEntryOfFirstField = std::max(occupiedEntries.firstBitThatMayBeClear(), entryOfFirstFieldOfLastRow);
        uint32_t firstEntry = findFirstEntry(occupiedEntries, firstEntries, fields, lowestEntryOfFirstField > fields[0].name ? lowestEntryOfFirstField - fields[0].name : 0);
        firstEntries.set(firstEntry);
        for (const NamedField& field : fields)
            occupiedEntries.set(firstEntry + field.name);
        firstEntryOf(row) = firstEntry;
        highestFirstEntry = std::max(highestFirstEntry, firstEntry);
        entryOfFirstFieldOfLastRow = firstEntry + fields[0].name;
        lastRow = &row;
    }

    // A lookup tests no bounds: every row has an entry for every name that has a column.
    const uint32_t numberOfEntries = highestFirstEntry + numberOfColumns;
    Vector<uint32_t> table(FillWith { }, highestObjectShape + numberOfEntries, 0);
    Vector<uint16_t> recordIndices(FillWith { }, numberOfEntries, 0);
    for (uint32_t shape = 1; shape <= highestObjectShape; ++shape)
        table[highestObjectShape - shape] = firstEntryOfShape[shape];
    for (const Row& row : named.rows) {
        for (const NamedField& field : named.fieldsOf(row)) {
            uint32_t index = firstEntryOf(row) + field.name;
            table[highestObjectShape + index] = field.entry;
            recordIndices[index] = field.field;
        }
    }
    encoder.header.numberOfFieldTableRows = highestObjectShape;
    encoder.setTable(ProgramTableIndex::fieldTable, table);
    encoder.setTable(ProgramTableIndex::fieldRecordIndices, recordIndices);
    encoder.setTable(ProgramTableIndex::classMemberRows, firstEntryOfClass);
    encoder.setTable(ProgramTableIndex::classAccessors, accessors);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
