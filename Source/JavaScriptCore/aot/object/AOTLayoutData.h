/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTSlotRepresentation.h"
#include "PropertySlot.h"
#include <array>
#include <bit>
#include <cstddef>
#include <optional>
#include <span>
#include <wtf/OptionSet.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

static constexpr uint32_t noBitSet = UINT32_MAX;

struct ShapeTestData {
    uint32_t last;
    uint32_t count;
    uint32_t bitSetOffset;

    uint32_t first() const { return last - count + 1; }

    static constexpr ptrdiff_t offsetOfLast() { return 0; }
    static constexpr ptrdiff_t offsetOfCount() { return 4; }
    static constexpr ptrdiff_t offsetOfBitSetOffset() { return 8; }
};
static_assert(sizeof(ShapeTestData) == 12 && isImageRecord<ShapeTestData>);

struct LayoutSetData {
    ShapeTestData members;
    uint32_t name;
};
static_assert(sizeof(LayoutSetData) == 16 && isImageRecord<LayoutSetData>);

static constexpr uint16_t fieldIsOptional = 1u << 0;
static constexpr uint16_t fieldIsOutOfLine = 1u << 1;
static constexpr uint16_t fieldTakesAccessorsOfLibraryAlone = 1u << 2;
static constexpr uint16_t fieldIsNeverStoredWithOtherType = 1u << 3;
static constexpr uint16_t fieldMayExistWithoutStorage = 1u << 4;
static constexpr uint16_t fieldDeferredClosureCapturesObject = 1u << 5;
static constexpr uint16_t fieldIsPrivate = 1u << 6;
static constexpr uint16_t fieldIsBrand = 1u << 7;
static constexpr uint16_t fieldDeferredClosureCapturesEnvironment = 1u << 8;
static constexpr uint16_t fieldIsNeverDeleted = 1u << 9;
static constexpr uint16_t fieldIsNeverRedefined = 1u << 10;
static constexpr unsigned fieldAttributesShift = 10;
static constexpr uint16_t fieldIsReadOnly = static_cast<unsigned>(PropertyAttribute::ReadOnly) << fieldAttributesShift;
static constexpr uint16_t fieldIsNotEnumerable = static_cast<unsigned>(PropertyAttribute::DontEnum) << fieldAttributesShift;
static constexpr uint16_t fieldIsNotConfigurable = static_cast<unsigned>(PropertyAttribute::DontDelete) << fieldAttributesShift;
static constexpr uint16_t fieldIsAccessor = static_cast<unsigned>(PropertyAttribute::Accessor) << fieldAttributesShift;
static constexpr uint16_t fieldAttributeFlags = fieldIsReadOnly | fieldIsNotEnumerable | fieldIsNotConfigurable | fieldIsAccessor;
static constexpr uint16_t allFieldFlags = (1u << 15) - 1;
static constexpr uint16_t fieldRecordIsCopyInFieldReturn = 1u << 15;
static constexpr uint16_t fieldTypeIsLeftToRunTime = fieldRecordIsCopyInFieldReturn;
static_assert(fieldAttributeFlags == (allFieldFlags & ~((1u << 11) - 1)));

struct FieldData {
    static constexpr unsigned nameBits = 24;

    uint32_t name : nameBits;
    uint32_t isFilledAtBirth : 1;
    uint32_t holdsRawValueFromBirth : 1;
    uint32_t slotHoldsUndefinedAndNull : 1;
    uint32_t getterIsProven : 1;
    uint32_t setterIsProven : 1;
    uint32_t isHeldFilled : 1;
    uint32_t isWrittenPrivateName : 1;
    uint32_t isKeptFromComparisons : 1;
    uint16_t flags;
    uint16_t typeNumber;
    SlotData slot;

    bool isOutOfLine() const { return flags & fieldIsOutOfLine; }
    bool isPrivate() const { return flags & fieldIsPrivate; }
    bool isBrand() const { return flags & fieldIsBrand; }
    bool isAccessor() const { return flags & fieldIsAccessor; }
    bool takesAccessorsOfLibraryAlone() const { return flags & fieldTakesAccessorsOfLibraryAlone; }
    unsigned attributes() const { return (flags & fieldAttributeFlags) >> fieldAttributesShift; }
    bool canLeaveSlot() const { return AOT::canLeaveSlot(slot.representation()); }
    bool isRequired() const { return !(flags & fieldIsOptional); }
};
static_assert(sizeof(FieldData) == 12 && isImageRecord<FieldData> && FieldData::nameBits + 8 == 32);

struct FieldOperandsData {
    uint32_t field;
    uint32_t deferredClosure;
    uint32_t environmentSlot;
};
static_assert(sizeof(FieldOperandsData) == 12 && isImageRecord<FieldOperandsData>);

struct alignas(64) FieldExit {
    static constexpr unsigned numberOfPlaces = 64;
    static constexpr unsigned numberOfPlacesBySlot = 16;
    static constexpr unsigned sizeShift = 6;

    const FieldData* field;
    uint64_t absentBits;
    StructureID structureBefore;
    StructureID structureAfter;
    StructureID extraPropertiesBefore;
    StructureID extraPropertiesAfter;
    uint32_t orderWordBefore;
    uint32_t orderWordAfter;
    PropertyOffset offset;
    uint32_t name;
    uint32_t generationForStores;
    uint32_t offsetOfSlot;
    uint16_t extraPropertiesCellSize;
    uint16_t offsetOfExtraPropertiesWord;
    bool wasInSlot;
    static constexpr uint8_t objectMayBearMark = 1;
    uint8_t widthOfSlot;
    bool isDefinition;
    uint8_t attributes;
};
static_assert(sizeof(FieldExit) == 1u << FieldExit::sizeShift && std::is_trivially_copyable_v<FieldExit>);

struct alignas(64) FieldReturn {
    static constexpr unsigned numberOfPlaces = 16;
    static constexpr unsigned sizeShift = 6;

    FieldData markedCopyOfField;
    const FieldData* field;
    StructureID structureBefore;
    StructureID structureAfter;
    StructureID extraPropertiesBefore;
    StructureID extraPropertiesAfter;
    uint32_t orderWord;
    uint32_t name;
    PropertyOffset offset;
    uint16_t offsetOfExtraPropertiesWord;
};
static_assert(sizeof(FieldReturn) == 1u << FieldReturn::sizeShift && std::is_trivially_copyable_v<FieldReturn> && !offsetof(FieldReturn, markedCopyOfField));

struct AddedExtraProperty {
    static constexpr unsigned numberOfPlaces = 64;

    UniquedStringImpl* name;
    StructureID structureBefore;
    StructureID structureAfter;
    StructureID extraPropertiesBefore;
    StructureID extraPropertiesAfter;
    uint32_t orderWordBefore;
    PropertyOffset offset;
    unsigned attributes;
};
static_assert(std::is_trivially_copyable_v<AddedExtraProperty>);

struct VariableData {
    static constexpr uint32_t noName = UINT32_MAX;
    static constexpr uint32_t isVariableOfModuleFlag = 1u << 31;

    constexpr bool isVariableOfModule() const { return name != noName && name & isVariableOfModuleFlag; }
    constexpr uint32_t string() const { return name & ~isVariableOfModuleFlag; }

    uint32_t name;
    SlotData slot;
};
static_assert(sizeof(VariableData) == 8 && isImageRecord<VariableData>);

static constexpr uint32_t methodHasComputedName = 1;
static constexpr uint32_t methodTakesEnvironment = 2;
static constexpr uint32_t methodIsCreatedAtDefinition = 4;
static constexpr uint32_t methodMayBeStoredOver = 8;
static constexpr uint32_t methodIsStoredFirst = 16;

struct MethodData {
    static constexpr unsigned nameBits = FieldData::nameBits;
    uint32_t name : nameBits;
    uint32_t kindBits : 2;
    uint32_t flags : 30 - nameBits;
    uint32_t function;

    MethodKind kind() const { return static_cast<MethodKind>(kindBits); }
};
static_assert(sizeof(MethodData) == 8 && isImageRecord<MethodData>);
static_assert(numberOfEnumerators<MethodKind> <= 1u << 2 && methodIsStoredFirst < 1u << (30 - MethodData::nameBits));

static constexpr unsigned numberOfClassSides = numberOfEnumerators<ClassSide>;

struct NamedClassGuardData {
    static constexpr uint32_t isAskedByStoresAlone = 1u << 31;
    static constexpr uint32_t isAskedBehindLookAtObject = 1u << 0;

    uint32_t numberOfBit() const { return bit & ~(isAskedByStoresAlone | isAskedBehindLookAtObject); }
    bool answersStoresAlone() const { return bit & isAskedByStoresAlone; }
    bool answersBehindLookAtObject() const { return bit & isAskedBehindLookAtObject; }
    bool answersReadsOffEveryObject() const { return !(bit & (isAskedByStoresAlone | isAskedBehindLookAtObject)); }

    uint32_t name;
    uint32_t bit;
};
static_assert(sizeof(NamedClassGuardData) == 8 && isImageRecord<NamedClassGuardData>);

struct ClassBelowWithGuardOfNameData {
    uint32_t classLayout() const { return classAndSide / numberOfClassSides; }
    ClassSide side() const { return static_cast<ClassSide>(classAndSide % numberOfClassSides); }
    friend auto operator<=>(const ClassBelowWithGuardOfNameData&, const ClassBelowWithGuardOfNameData&) = default;

    uint32_t name;
    uint32_t classAndSide;
};
static_assert(sizeof(ClassBelowWithGuardOfNameData) == 8 && isImageRecord<ClassBelowWithGuardOfNameData>);

struct AssignedMemberData {
    uint32_t classLayout() const { return classAndSide / numberOfClassSides; }
    ClassSide side() const { return static_cast<ClassSide>(classAndSide % numberOfClassSides); }
    uint64_t place() const { return static_cast<uint64_t>(classAndSide) << 32 | name; }

    uint32_t classAndSide;
    uint32_t name;
    uint32_t guardBit;
    uint32_t method;
    uint32_t getter;
    uint32_t setter;
    uint32_t holdsValue;
};
static_assert(sizeof(AssignedMemberData) == 28 && isImageRecord<AssignedMemberData>);

struct ClassGuardCountData {
    uint32_t guardBit;
    uint32_t offsetOfCount;

    friend auto operator<=>(const ClassGuardCountData&, const ClassGuardCountData&) = default;
};
static_assert(sizeof(ClassGuardCountData) == 8 && isImageRecord<ClassGuardCountData>);

struct PropertiesOfHeirsData {
    static constexpr uint32_t mayBeStoredLate = 1u << 31;
    static constexpr uint32_t extendsMixin = 1u << 31;

    uint32_t builtinClassAndForm;
    uint32_t numberOfNames;

    uint32_t builtinClass() const { return builtinClassAndForm & ~extendsMixin; }
    bool isBelowMixin() const { return builtinClassAndForm & extendsMixin; }
    std::span<const uint32_t> names() const { return { reinterpret_cast<const uint32_t*>(this + 1), numberOfNames }; }
};
static_assert(sizeof(PropertiesOfHeirsData) == 8 && isImageRecord<PropertiesOfHeirsData>);

struct ClassData {
    uint32_t base;
    uint32_t expectedBase;
    uint32_t classConstructor;
    uint32_t environment;
    uint32_t numberOfMethods;
    uint32_t numberOfStaticMethods;
    uint32_t numberOfStaticFields;
    uint32_t subclassesOffset;
    uint32_t propertiesOfHeirsOffset;
    uint32_t guardBit[numberOfClassSides];
    uint32_t namedGuardsOffset[numberOfClassSides];
    uint32_t guardsHoldWhileClear : 1;
    uint32_t guardsOfPrototypeSideHoldWhileClear : 1;
    uint32_t sidesWithGuardsHereOrBelow : numberOfClassSides;
    uint32_t guardsOfNamesAwaitBeginning : 1;
    uint32_t sidesChangedInSightAlone : numberOfClassSides;
    uint32_t namesAreLaidUnderWrittenKeysAlone : 1;
    uint32_t classObjectLacksHasInstance : 1;
    uint32_t privateNamesAreKnownToTexts : 1;
    uint32_t numberOfFieldsToLastOfBirth : 16;
    uint32_t hasComputedMemberNames : 1;
    uint32_t unusedClassBits : 9 - 2 * numberOfClassSides { 0 };

    bool bitsAreClearWhileGuardsHold(ClassSide side) const { return side == ClassSide::Prototype ? guardsOfPrototypeSideHoldWhileClear : guardsHoldWhileClear; }
    bool hasGuardsHereOrBelow(ClassSide side) const { return sidesWithGuardsHereOrBelow >> static_cast<unsigned>(side) & 1; }
    bool isChangedInSightAlone(ClassSide side) const { return sidesChangedInSightAlone >> static_cast<unsigned>(side) & 1; }
};
static_assert(sizeof(ClassData) == 56 && isImageRecord<ClassData>);

struct ExtensionLayoutData {
    SlotData extension;
    uint32_t extensionSize;
    uint32_t granulesOfFirstExtensionLow;
    uint32_t granulesOfFirstExtensionHigh;
};
static_assert(sizeof(ExtensionLayoutData) == 16 && isImageRecord<ExtensionLayoutData>);
static_assert(offsetof(ExtensionLayoutData, granulesOfFirstExtensionHigh) == offsetof(ExtensionLayoutData, granulesOfFirstExtensionLow) + sizeof(uint32_t) && offsetof(ExtensionLayoutData, granulesOfFirstExtensionHigh) + sizeof(uint32_t) == sizeof(ExtensionLayoutData));

struct LayoutData {
    static constexpr uint32_t noBase = UINT32_MAX;

    static constexpr unsigned impossibleMutationBits = 13;

    uint32_t kindBits : 4;
    uint32_t isDerived : 1;
    uint32_t impossibleMutationsRaw : impossibleMutationBits;
    uint32_t isSaidNeverToReachUnseenCode : 1;
    uint32_t evaluationBits : 2;
    uint32_t hasFieldsWithSymbolKeys : 1;
    uint32_t hasOnlyPlainSlots : 1;
    uint32_t isUntyped : 1;
    uint32_t hasUntypedBase : 1;
    uint32_t createsMembersLazily : 1;
    uint32_t createsPrototypeBeforeClassObject : 1;
    uint32_t createsClassObjectOnMutation : 1;
    uint32_t hasExtraPropertiesSlot : 1;
    uint32_t hasExtensionData : 1;
    uint32_t hasSlotWithUnusedMarkers : 1;
    uint32_t isCopiedByWords : 1;
    uint32_t index;
    uint32_t shape;
    uint32_t size;
    uint16_t numberOfFields;
    uint16_t numberOfFieldOperands;
    uint32_t numberOfOrders;
    int32_t referencesOffset;
    uint32_t ordersOffset;

    uint32_t firstWord() const { return std::bit_cast<std::array<uint32_t, sizeof(LayoutData) / sizeof(uint32_t)>>(*this)[0]; }
    LayoutKind kind() const { return static_cast<LayoutKind>(kindBits); }
    bool isClass() const { return kind() == LayoutKind::Instance; }
    EvaluationState evaluation() const { return static_cast<EvaluationState>(evaluationBits); }
    OptionSet<ObjectMutation> impossibleMutations() const { return OptionSet<ObjectMutation>::fromRaw(impossibleMutationsRaw); }

    const SlotData& slotOfExtraProperties() const
    {
        ASSERT(hasExtraPropertiesSlot);
        return reinterpret_cast<const SlotData*>(this)[-1];
    }
    SlotData& slotOfExtraProperties() { return const_cast<SlotData&>(const_cast<const LayoutData*>(this)->slotOfExtraProperties()); }
    const ExtensionLayoutData& extensionData() const
    {
        ASSERT(hasExtensionData);
        return reinterpret_cast<const ExtensionLayoutData*>(reinterpret_cast<const uint8_t*>(this) - prefixSize(false, false, hasExtraPropertiesSlot))[-1];
    }
    ExtensionLayoutData& extensionData() { return const_cast<ExtensionLayoutData&>(const_cast<const LayoutData*>(this)->extensionData()); }
    const ClassData& classData() const
    {
        ASSERT(isClass());
        return reinterpret_cast<const ClassData*>(reinterpret_cast<const uint8_t*>(this) - prefixSize(false, hasExtensionData, hasExtraPropertiesSlot))[-1];
    }
    ClassData& classData() { return const_cast<ClassData&>(const_cast<const LayoutData*>(this)->classData()); }

    std::optional<SlotData> extensionSlot() const
    {
        if (!hasExtensionData)
            return std::nullopt;
        return extensionData().extension;
    }
    std::optional<SlotData> extraPropertiesSlot() const
    {
        if (!hasExtraPropertiesSlot)
            return std::nullopt;
        return slotOfExtraProperties();
    }
    bool hasExtension() const { return hasExtensionData; }
    uint32_t extensionSize() const { return hasExtensionData ? extensionData().extensionSize : 0; }
    uint64_t granulesOfFirstExtension() const { return hasExtensionData ? static_cast<uint64_t>(extensionData().granulesOfFirstExtensionHigh) << 32 | extensionData().granulesOfFirstExtensionLow : 0; }
    uint32_t base() const { return isClass() ? classData().base : noBase; }
    uint32_t assumedBase() const
    {
        if (!isClass())
            return noBase;
        return classData().base == noBase ? classData().expectedBase : classData().base;
    }

    std::span<const FieldData> fields() const { return { reinterpret_cast<const FieldData*>(this + 1), numberOfFields }; }
    std::span<const FieldData> fieldsToLastOfBirth() const { return fields().first(isClass() ? classData().numberOfFieldsToLastOfBirth : numberOfFields); }
    std::span<const uint32_t> fieldOperands() const { return { reinterpret_cast<const uint32_t*>(fields().data() + numberOfFields), numberOfFieldOperands }; }
    std::span<const MethodData> methods() const { return { reinterpret_cast<const MethodData*>(fieldOperands().data() + numberOfFieldOperands), isClass() ? classData().numberOfMethods : 0 }; }
    std::span<const MethodData> staticMethods() const { return { methods().data() + methods().size(), isClass() ? classData().numberOfStaticMethods : 0 }; }
    std::span<const FieldData> staticFields() const { return { reinterpret_cast<const FieldData*>(staticMethods().data() + staticMethods().size()), isClass() ? classData().numberOfStaticFields : 0 }; }

    uint16_t indexOf(const FieldData& field) const { return static_cast<uint16_t>(&field - fields().data()); }
    const FieldOperandsData& operandsOf(const FieldData& field) const
    {
        auto operands = spanReinterpretCast<const FieldOperandsData>(fieldOperands());
        auto found = std::ranges::find(operands, indexOf(field), &FieldOperandsData::field);
        RELEASE_ASSERT(found != operands.end());
        return *found;
    }
    uint32_t deferredClosureNumberOf(const FieldData& field) const
    {
        ASSERT(field.slot.representation() == SlotRepresentation::DeferredClosure);
        return operandsOf(field).deferredClosure;
    }
    SlotData closureEnvironmentSlotOf(const FieldData& field) const
    {
        ASSERT(field.flags & fieldDeferredClosureCapturesEnvironment);
        return SlotData::decode(operandsOf(field).environmentSlot);
    }

    static constexpr size_t prefixSize(bool isClass, bool hasExtensionData, bool hasExtraPropertiesSlot) { return (isClass ? sizeof(ClassData) : 0) + (hasExtensionData ? sizeof(ExtensionLayoutData) : 0) + (hasExtraPropertiesSlot ? sizeof(SlotData) : 0); }
    size_t prefixSize() const { return prefixSize(isClass(), hasExtensionData, hasExtraPropertiesSlot); }
    struct TrailingCounts {
        uint64_t fields { 0 };
        uint64_t fieldOperands { 0 };
        uint64_t methods { 0 };
        uint64_t staticFields { 0 };
    };
    static constexpr uint64_t trailingSize(TrailingCounts counts)
    {
        return (counts.fields + counts.staticFields) * sizeof(FieldData) + counts.fieldOperands * sizeof(uint32_t) + counts.methods * sizeof(MethodData);
    }
    uint64_t trailingSize() const
    {
        TrailingCounts counts { .fields = numberOfFields, .fieldOperands = numberOfFieldOperands };
        if (isClass()) {
            counts.methods = static_cast<uint64_t>(classData().numberOfMethods) + classData().numberOfStaticMethods;
            counts.staticFields = classData().numberOfStaticFields;
        }
        return trailingSize(counts);
    }
};
static_assert(sizeof(LayoutData) == 32 && offsetof(LayoutData, index) == 4 && isImageRecord<LayoutData>);
static_assert(numberOfEnumerators<LayoutKind> <= 1u << 5 && numberOfEnumerators<EvaluationState> <= 1u << 2);
static_assert(numberOfEnumerators<ObjectMutation> <= LayoutData::impossibleMutationBits);
static_assert(numberOfEnumerators<LayoutKind> <= 1u << 4);
static_assert(maxFieldsInOrder <= UINT16_MAX);

struct EnvironmentData {
    LayoutKind kind;
    Number24 moduleAfterNone;
    uint32_t index;
    uint32_t shape;
    uint32_t name;
    uint32_t size;
    uint32_t numberOfDescribedVariables;
    int32_t referencesOffset;

    std::span<const VariableData> describedVariables() const { return { reinterpret_cast<const VariableData*>(this + 1), numberOfDescribedVariables }; }
};
static_assert(sizeof(EnvironmentData) == 28 && isImageRecord<EnvironmentData>);

struct AllocationData {
    static constexpr uint32_t notConstructed = UINT32_MAX;
    static constexpr uint32_t maxSize = ((1u << 22) - 1) * sizeof(uint64_t);

    uint16_t halvesOfOrderWord[2];
    Number24 shape;
    uint8_t isBornUnfinished : 1;
    uint8_t isConstructed : 1;
    uint8_t highBitsOfSizeInWords : 6;
    uint16_t lowBitsOfSizeInWords;

    static constexpr AllocationData create(uint32_t shape, uint32_t orderWord, uint32_t size, bool isBornUnfinished, bool isConstructed)
    {
        RELEASE_ASSERT_UNDER_CONSTEXPR_CONTEXT(size <= maxSize && !(size % sizeof(uint64_t)));
        uint32_t sizeInWords = size / sizeof(uint64_t);
        return { { static_cast<uint16_t>(orderWord), static_cast<uint16_t>(orderWord >> 16) }, Number24(shape), isBornUnfinished, isConstructed, static_cast<uint8_t>(sizeInWords >> 16), static_cast<uint16_t>(sizeInWords) };
    }
    constexpr uint32_t orderWord() const { return halvesOfOrderWord[0] | static_cast<uint32_t>(halvesOfOrderWord[1]) << 16; }
    constexpr uint32_t size() const { return (static_cast<uint32_t>(highBitsOfSizeInWords) << 16 | lowBitsOfSizeInWords) * static_cast<uint32_t>(sizeof(uint64_t)); }
};
static_assert(sizeof(AllocationData) == 10 && alignof(AllocationData) == 2 && isImageRecord<AllocationData>);
static_assert(AllocationData::create(0x123456, 0x89abcdef, AllocationData::maxSize, true, false).size() == AllocationData::maxSize && AllocationData::create(0x123456, 0x89abcdef, 16, true, false).orderWord() == 0x89abcdef);

struct AllocationRow {
    Number24 shape;
    Number24 form;
};
static_assert(sizeof(AllocationRow) == 6 && isImageRecord<AllocationRow>);

class AllocationTable {
public:
    AllocationTable(std::span<const AllocationRow> rows, std::span<const AllocationData> forms)
        : m_rows(rows)
        , m_forms(forms)
    {
    }

    size_t size() const { return m_rows.size(); }
    AllocationData operator[](size_t number) const
    {
        const AllocationRow& row = m_rows[number];
        AllocationData allocation = m_forms[row.form];
        allocation.shape = row.shape;
        return allocation;
    }
    AllocationData checkedAt(size_t number) const
    {
        RELEASE_ASSERT(number < size());
        return (*this)[number];
    }

private:
    std::span<const AllocationRow> m_rows;
    std::span<const AllocationData> m_forms;
};

struct ConstructedAllocationData {
    Number24 allocation;
    Number24 constructedBy;
};
static_assert(sizeof(ConstructedAllocationData) == 6 && isImageRecord<ConstructedAllocationData>);

static constexpr uint32_t maxKeyOrderEntry = UINT16_MAX - 1;

struct KeyOrderData {
    uint16_t length;

    std::span<const uint16_t> entries() const { return { reinterpret_cast<const uint16_t*>(this + 1), length }; }
};
static_assert(sizeof(KeyOrderData) == 2 && isImageRecord<KeyOrderData>);

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
