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

struct FieldTableEntry {
    enum class Form : uint8_t {
        InlineValue,
        InlineInt32,
        InlineNarrowCell,
        OutOfLineValue,
        Field,
    };

    static constexpr unsigned operandBits = 13;
    static constexpr unsigned formBits = 3;
    static constexpr unsigned formShift = operandBits;
    static constexpr unsigned nameShift = operandBits + formBits;
    static constexpr uint32_t maxOperand = (1u << operandBits) - 1;
    static constexpr uint32_t maxSlotNumber = 4991;
    static constexpr uint32_t firstInt64 = maxSlotNumber + 1;
    static constexpr uint32_t firstNarrowCellInExtension = firstInt64 + 1024;
    static constexpr uint32_t firstNarrowCellInSecondHalfOfGranule = firstNarrowCellInExtension + 64;
    static constexpr uint32_t firstEncodedByte = firstNarrowCellInSecondHalfOfGranule + 64;
    static constexpr uint32_t firstBoolean = firstEncodedByte + 512;
    static constexpr uint32_t firstDouble = firstBoolean + 512;
    static constexpr uint32_t endOfDoubles = firstDouble + 1024;
    static constexpr uint32_t maxFormAndOperand = (1u << nameShift) - 1;
    static constexpr uint32_t noName = 0;
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
        case SlotRepresentation::Int64:
        case SlotRepresentation::OptionalInt64:
            if (field.isOutOfLine() || offset % sizeof(int64_t) || offset / sizeof(int64_t) >= firstNarrowCellInExtension - firstInt64)
                return otherField;
            return create(field.name, Form::Field, firstInt64 + offset / sizeof(int64_t));
        case SlotRepresentation::Uint32:
        case SlotRepresentation::Float:
        case SlotRepresentation::Uint8:
        case SlotRepresentation::Int8:
        case SlotRepresentation::Uint16:
        case SlotRepresentation::Int16:
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

struct FieldRecordIndex {
    static constexpr uint16_t isStoredWithoutTypeTest = 1u << 15;
    static constexpr uint16_t indexMask = isStoredWithoutTypeTest - 1;

    static FieldRecordIndex create(const FieldData& field, uint16_t indexInLayoutRecord, const RuntimeType& type, uint32_t typeOfAnyValue)
    {
        RELEASE_ASSERT(indexInLayoutRecord <= indexMask);
        constexpr uint16_t flagsBarringStore = fieldAttributeFlags;
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
        case SlotRepresentation::DeferredClosure:
            return false;
        }
    }
};

inline std::optional<uint16_t> findFieldInTable(const ProgramTables& tables, uint32_t shape, uint32_t name)
{
    ASSERT(shape && shape <= tables.numberOfFieldTableRows && tables.hasFieldTableColumn(name));
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
static_assert(absentInteger > INT32_MAX && undefinedInteger > INT32_MAX && nullInteger > INT32_MAX);
static_assert(shapeWordOf(1) & compiledObjectShapeBit && !(pointerUpperHalfBits & compiledObjectShapeBit) && !(bigIntHashMask & compiledObjectShapeBit));

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
