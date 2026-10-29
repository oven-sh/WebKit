/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTProgramTables.h"
#include <optional>

namespace JSC { namespace AOT {

// Where a compiled object has the property of a name, found from the object's shape in read-only data: entries[rows[shape] + name].
struct FieldTableEntry {
    // In the order in which a lookup tests them.
    enum class Form : uint8_t {
        // A field in the cell whose eight bytes are the JSValue, zero for an empty slot. The operand is the index of the 64-bit word in the cell.
        InlineValue,
        // A field in the cell that holds a 32-bit integer. A word below minInlineInt32 says something else, which the field's record explains. The operand is the index of the 32-bit word in the cell.
        InlineInt32,
        // A field in the cell that holds a 32-bit reference.
        InlineNarrowCell,
        // A field in the Extension whose eight bytes are the JSValue, zero for an empty slot. The operand is the index of the 64-bit word in the Extension, whose reference is the first field of the cell.
        OutOfLineValue,
        // Any other field.
        Field,
    };

    static constexpr unsigned operandBits = 13;
    static constexpr unsigned formBits = 3;
    static constexpr unsigned formShift = operandBits;
    static constexpr unsigned nameShift = operandBits + formBits;
    static constexpr uint32_t maxOperand = (1u << operandBits) - 1;
    // Operands of the form Field. Zero: by the record. Up to maxSlotNumber: kept free.
    static constexpr uint32_t maxSlotNumber = 6015;
    // A field in the Extension that holds a 32-bit reference, in the first or in the second half of its granule. Less this: the number of the granule and one more.
    static constexpr uint32_t firstNarrowCellInExtension = maxSlotNumber + 1;
    static constexpr uint32_t firstNarrowCellInSecondHalfOfGranule = firstNarrowCellInExtension + 64;
    // A field in the cell whose byte is the low byte of the JSValue: false, true, undefined or null, zero for an empty slot. Less this: the index of the byte in the cell.
    static constexpr uint32_t firstEncodedByte = firstNarrowCellInSecondHalfOfGranule + 64;
    // A field in the cell whose byte is 0 for false and 1 for true. Less this: the index of the byte in the cell.
    static constexpr uint32_t firstBoolean = firstEncodedByte + 512;
    // A field in the cell that holds a double. A NaN may be a marker, which the field's record explains. Less this: the index of the 64-bit word in the cell.
    static constexpr uint32_t firstDouble = firstBoolean + 512;
    static constexpr uint32_t endOfDoubles = firstDouble + 1024;
    static constexpr uint32_t maxFormAndOperand = (1u << nameShift) - 1;
    static constexpr uint32_t noName = 0;
    // More names than the front table has columns: a large program reads and stores fields under names that are too cold for a column there.
    static constexpr uint32_t maxName = UINT16_MAX;
    static constexpr uint32_t minNarrowValue = nullCell32;

    static constexpr bool hasColumn(uint32_t identifierNumber) { return identifierNumber != noName && identifierNumber <= maxName; }

    static constexpr FieldTableEntry create(uint32_t name, Form form, uint32_t operand)
    {
        RELEASE_ASSERT_UNDER_CONSTEXPR_CONTEXT(hasColumn(name) && operand <= maxOperand);
        return { name << nameShift | static_cast<uint32_t>(form) << formShift | operand };
    }

    static FieldTableEntry forField(const LayoutData& layout, const FieldData& field)
    {
        const uint32_t offset = field.slot.offset;
        const FieldTableEntry otherField = create(field.name, Form::Field, 0);
        // What the slot of an accessor holds is not the value of the property.
        if (field.isAccessor())
            return otherField;
        switch (field.slot.representation()) {
        case SlotRepresentation::JSValue:
        case SlotRepresentation::Cell:
            if (offset % sizeof(EncodedJSValue))
                return otherField;
            if (offset / sizeof(EncodedJSValue) > maxOperand)
                return otherField;
            if (!field.isOutOfLine())
                return create(field.name, Form::InlineValue, offset / sizeof(EncodedJSValue));
            if (layout.extensionSlot() != SlotData::create(offsetOfFirstField, SlotRepresentation::Cell32))
                return otherField;
            return create(field.name, Form::OutOfLineValue, offset / sizeof(EncodedJSValue));
        case SlotRepresentation::Cell32:
        case SlotRepresentation::OptionalCell32:
            if (field.isOutOfLine()) {
                if (offset % sizeof(uint32_t) || offset / sizeof(EncodedJSValue) >= firstNarrowCellInSecondHalfOfGranule - firstNarrowCellInExtension)
                    return otherField;
                if (layout.extensionSlot() != SlotData::create(offsetOfFirstField, SlotRepresentation::Cell32))
                    return otherField;
                return create(field.name, Form::Field, (offset % sizeof(EncodedJSValue) ? firstNarrowCellInSecondHalfOfGranule : firstNarrowCellInExtension) + offset / sizeof(EncodedJSValue));
            }
            [[fallthrough]];
        case SlotRepresentation::DeferredClosure:
            if (field.isOutOfLine() || offset % sizeof(uint32_t) || offset / sizeof(uint32_t) > maxOperand)
                return otherField;
            return create(field.name, Form::InlineNarrowCell, offset / sizeof(uint32_t));
        case SlotRepresentation::Int32:
        case SlotRepresentation::OptionalInt32:
            if (field.isOutOfLine() || offset % sizeof(uint32_t) || offset / sizeof(uint32_t) > maxOperand)
                return otherField;
            return create(field.name, Form::InlineInt32, offset / sizeof(uint32_t));
        case SlotRepresentation::Double:
        case SlotRepresentation::OptionalDouble:
            if (field.isOutOfLine() || offset % sizeof(double) || offset / sizeof(double) >= endOfDoubles - firstDouble)
                return otherField;
            return create(field.name, Form::Field, firstDouble + offset / sizeof(double));
        case SlotRepresentation::Boolean:
        case SlotRepresentation::OptionalBoolean:
            if (field.isOutOfLine() || offset >= firstBoolean - firstEncodedByte)
                return otherField;
            return create(field.name, Form::Field, (field.slot.representation() == SlotRepresentation::Boolean ? firstBoolean : firstEncodedByte) + offset);
        case SlotRepresentation::Uint32:
        case SlotRepresentation::Int64:
        case SlotRepresentation::OptionalInt64:
        case SlotRepresentation::Float:
        case SlotRepresentation::Uint8:
        case SlotRepresentation::Int8:
        case SlotRepresentation::Uint16:
        case SlotRepresentation::Int16:
        case SlotRepresentation::LiteralString:
        case SlotRepresentation::LiteralString32:
            return otherField;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    constexpr bool isEmpty() const { return !bits; }
    constexpr uint32_t name() const { return bits >> nameShift; }
    constexpr Form form() const { return static_cast<Form>(bits >> formShift & ((1u << formBits) - 1)); }
    constexpr uint32_t operand() const { return bits & maxOperand; }

    friend constexpr bool operator==(FieldTableEntry, FieldTableEntry) = default;

    uint32_t bits;
};

// Beside each entry, under the same index: which field of the layout's record it is.
struct FieldRecordIndex {
    // The field is a writable data property, and its type accepts every value that a store through the entry writes: any value, any 32-bit integer from minInlineInt32 up, either boolean, or any number.
    static constexpr uint16_t isStoredWithoutTypeTest = 1u << 15;
    static constexpr uint16_t indexMask = isStoredWithoutTypeTest - 1;

    // type is the record at field.type. typeOfAnyValue is the offset of the run-time type that every value has.
    static FieldRecordIndex create(const FieldData& field, uint16_t indexInLayoutRecord, const RuntimeType& type, uint32_t typeOfAnyValue)
    {
        RELEASE_ASSERT(indexInLayoutRecord <= indexMask);
        constexpr uint16_t flagsBarringStore = fieldIsStoredOnce | fieldAttributeFlags;
        return { static_cast<uint16_t>(indexInLayoutRecord | (typeAcceptsWhatSlotHolds(field, type, typeOfAnyValue) && !(field.flags & flagsBarringStore) ? isStoredWithoutTypeTest : 0)) };
    }

    constexpr uint16_t indexInLayoutRecord() const { return bits & indexMask; }

    friend constexpr bool operator==(FieldRecordIndex, FieldRecordIndex) = default;

    uint16_t bits;

private:
    static bool typeAcceptsWhatSlotHolds(const FieldData& field, const RuntimeType& type, uint32_t typeOfAnyValue)
    {
        switch (field.slot.representation()) {
        case SlotRepresentation::JSValue:
            return field.type == typeOfAnyValue;
        case SlotRepresentation::Int32:
        case SlotRepresentation::OptionalInt32:
            if (!type.primitiveSet().contains(PrimitiveSet::Integer))
                return false;
            if (auto range = type.integerRange())
                return range->first <= minInlineInt32 && range->second >= INT32_MAX;
            return true;
        case SlotRepresentation::Boolean:
        case SlotRepresentation::OptionalBoolean:
            return type.primitiveSet().containsAll({ PrimitiveSet::False, PrimitiveSet::True });
        case SlotRepresentation::Double:
        case SlotRepresentation::OptionalDouble:
            return type.primitiveSet().containsAll({ PrimitiveSet::Integer, PrimitiveSet::WholeNumber, PrimitiveSet::FractionalNumber }) && !type.integerRange();
        case SlotRepresentation::Cell:
        case SlotRepresentation::Cell32:
        case SlotRepresentation::OptionalCell32:
        case SlotRepresentation::Uint32:
        case SlotRepresentation::Int64:
        case SlotRepresentation::OptionalInt64:
        case SlotRepresentation::Float:
        case SlotRepresentation::Uint8:
        case SlotRepresentation::Int8:
        case SlotRepresentation::Uint16:
        case SlotRepresentation::Int16:
        case SlotRepresentation::LiteralString:
        case SlotRepresentation::LiteralString32:
        case SlotRepresentation::DeferredClosure:
            return false;
        }
    }
};

// The index of the field in the record of the shape's layout. The shape is an object's and the name has a column.
inline std::optional<uint16_t> findFieldInTable(const ProgramTables& tables, uint32_t shape, uint32_t name)
{
    ASSERT(shape && shape <= tables.numberOfFieldTableRows && FieldTableEntry::hasColumn(name));
    uint32_t index = tables.fieldTableRowOf(shape) + name;
    if (index >= tables.fieldRecordIndices().size() || FieldTableEntry { tables.fieldTableEntries()[index] }.name() != name)
        return std::nullopt;
    return FieldRecordIndex { tables.fieldRecordIndices()[index] }.indexInLayoutRecord();
}

static_assert(sizeof(FieldTableEntry) == 4 && sizeof(FieldRecordIndex) == 2);
static_assert(FieldTableEntry::nameShift == 16 && FieldTableEntry::maxName <= UINT16_MAX);
static_assert(static_cast<unsigned>(FieldTableEntry::Form::Field) < 1u << FieldTableEntry::formBits && maxFieldsInOrder <= FieldRecordIndex::indexMask);
static_assert(traitsOf(SlotRepresentation::OptionalBoolean).absentBits == JSValue::ValueEmpty && traitsOf(SlotRepresentation::OptionalBoolean).undefinedBits == JSValue::ValueUndefined && traitsOf(SlotRepresentation::OptionalBoolean).nullBits == JSValue::ValueNull);
static_assert(FieldTableEntry::endOfDoubles == FieldTableEntry::maxOperand + 1 && FieldTableEntry::firstDouble - FieldTableEntry::firstBoolean == FieldTableEntry::firstBoolean - FieldTableEntry::firstEncodedByte);
static_assert(!JSValue::ValueEmpty && !absentCell32 && nullCell32 == JSValue::ValueNull && undefinedCell32 == JSValue::ValueUndefined);
static_assert(traitsOf(SlotRepresentation::Cell).absentBits == JSValue::ValueEmpty && traitsOf(SlotRepresentation::Cell32).absentBits == absentCell32);
static_assert(absentCell32 < FieldTableEntry::minNarrowValue && uncreatedDeferredClosure < FieldTableEntry::minNarrowValue && absentDeferredClosure < FieldTableEntry::minNarrowValue);
static_assert(FieldTableEntry::minNarrowValue <= nullCell32 && FieldTableEntry::minNarrowValue <= undefinedCell32);
static_assert(absentInt32 < minInlineInt32 && undefinedInt32 < minInlineInt32 && nullInt32 < minInlineInt32);
static_assert(shapeWordOf(1) & compiledObjectShapeBit && !(pointerUpperHalfBits & compiledObjectShapeBit) && !(bigIntHashMask & compiledObjectShapeBit));

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
