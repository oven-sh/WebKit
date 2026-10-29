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
static constexpr uint32_t noDeclaredPath = UINT32_MAX;

// Passes the shapes from last - count + 1 to last or, with a bit set, those of them whose distance from last is in the set. bitSetOffset is a location of PagedBitSet in the table shapeSetBits.
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
static constexpr uint16_t fieldIsStoredOnce = 1u << 2;
static constexpr uint16_t fieldIsNeverStoredWithOtherType = 1u << 3;
static constexpr uint16_t fieldMayExistWithoutStorage = 1u << 4;
static constexpr uint16_t fieldDeferredClosureCapturesObject = 1u << 5;
static constexpr uint16_t fieldIsPrivate = 1u << 6;
static constexpr uint16_t fieldIsBrand = 1u << 7;
static constexpr uint16_t fieldDeferredClosureCapturesEnvironment = 1u << 8;
static constexpr uint16_t fieldIsNeverDeleted = 1u << 9;
static constexpr uint16_t fieldIsNeverRedefined = 1u << 10;
// The form that the layout gives the property: what it is while it is in its slot. A definition of any other form moves it to the extra properties.
static constexpr unsigned fieldAttributesShift = 10;
static constexpr uint16_t fieldIsReadOnly = static_cast<unsigned>(PropertyAttribute::ReadOnly) << fieldAttributesShift;
static constexpr uint16_t fieldIsNotEnumerable = static_cast<unsigned>(PropertyAttribute::DontEnum) << fieldAttributesShift;
static constexpr uint16_t fieldIsNotConfigurable = static_cast<unsigned>(PropertyAttribute::DontDelete) << fieldAttributesShift;
static constexpr uint16_t fieldIsAccessor = static_cast<unsigned>(PropertyAttribute::Accessor) << fieldAttributesShift;
static constexpr uint16_t fieldAttributeFlags = fieldIsReadOnly | fieldIsNotEnumerable | fieldIsNotConfigurable | fieldIsAccessor;
static constexpr uint16_t allFieldFlags = (1u << 15) - 1;
static_assert(fieldAttributeFlags == (allFieldFlags & ~((1u << 11) - 1)));

struct FieldData {
    static constexpr unsigned nameBits = 24;

    uint32_t name : nameBits;
    uint32_t isFilledAtBirth : 1;
    // A number or a boolean among those fields: whoever stores the marker for undefined or null sets the mark of the cell.
    uint32_t holdsRawValueFromBirth : 1;
    // Compiled code has a guard that says that a store under the name passes the prototype object of the class and what is above it. The guard holds once a store by name has made the run time look.
    uint32_t hasGuardOfStores : 1;
    // Compiled code reads the field without the test of the cell.
    uint32_t keepsStateOfBirth : 1;
    uint32_t slotHoldsUndefinedAndNull : 1;
    uint32_t unused : 27 - nameBits { 0 };
    uint16_t flags;
    uint16_t operandIndex;
    SlotData slot;
    uint32_t type;

    bool isOutOfLine() const { return flags & fieldIsOutOfLine; }
    bool isPrivate() const { return flags & fieldIsPrivate; }
    bool isBrand() const { return flags & fieldIsBrand; }
    bool isAccessor() const { return flags & fieldIsAccessor; }
    // Bits of PropertyAttribute. An object that is sealed or frozen adds its own.
    unsigned attributes() const { return (flags & fieldAttributeFlags) >> fieldAttributesShift; }
    bool canLeaveSlot() const { return AOT::canLeaveSlot(slot.representation()); }
    bool isRequired() const { return !(flags & fieldIsOptional); }
};
static_assert(sizeof(FieldData) == 16 && isImageRecord<FieldData>);

// What a store of a value that the slot of a field does not take, or a definition with attributes that the slot does not have, made of an object. It makes the same of every object in the same state.
struct alignas(64) FieldExit {
    static constexpr unsigned numberOfPlaces = 64;
    static constexpr unsigned sizeShift = 6;

    const FieldData* field;
    // What an empty slot holds.
    uint64_t absentBits;
    StructureID structureBefore;
    StructureID structureAfter;
    // None where the object had no extra properties.
    StructureID extraPropertiesBefore;
    StructureID extraPropertiesAfter;
    uint32_t orderWordBefore;
    uint32_t orderWordAfter;
    PropertyOffset offset;
    uint32_t name;
    // Of the filters of absent names, when the run time last asked them whether a store creates the field, and they said so. None until it has asked: the field may have left by a definition.
    uint32_t generationForStores;
    uint32_t offsetOfSlot;
    uint16_t extraPropertiesCellSize;
    uint16_t offsetOfExtraPropertiesWord;
    bool wasInSlot;
    // Four or eight for a slot in the cell. Zero for any other and for a definition, which the stubs leave to the run time.
    uint8_t widthOfSlot;
    bool isDefinition;
    // Bits of PropertyAttribute that the property has outside the slot.
    uint8_t attributes;
};
static_assert(sizeof(FieldExit) == 1u << FieldExit::sizeShift && std::is_trivially_copyable_v<FieldExit>);

struct VariableData {
    uint32_t name;
    SlotData slot;
};
static_assert(sizeof(VariableData) == 8 && isImageRecord<VariableData>);

static constexpr uint32_t methodHasComputedName = 1;
static constexpr uint32_t methodTakesEnvironment = 2;
static constexpr uint32_t methodIsCreatedAtDefinition = 4;

struct MethodData {
    static constexpr unsigned nameBits = FieldData::nameBits;
    uint32_t name : nameBits;
    uint32_t kindBits : 2;
    uint32_t flags : 30 - nameBits;
    uint32_t function;

    MethodKind kind() const { return static_cast<MethodKind>(kindBits); }
};
static_assert(sizeof(MethodData) == 8 && isImageRecord<MethodData>);
static_assert(numberOfEnumerators<MethodKind> <= 1u << 2 && methodIsCreatedAtDefinition < 1u << (30 - MethodData::nameBits));

static constexpr unsigned numberOfClassSides = numberOfEnumerators<ClassSide>;

enum class MemberGuardKind : uint8_t { Member, PrototypeLink, EvaluatedOnce, InstancePrototype, NotHidden, PassedByStores };

struct MemberGuardData {
    static constexpr unsigned bitsPerWord = 64;

    uint32_t name : MethodData::nameBits;
    uint32_t kindBits : 3;
    uint32_t sideBits : 1;
    uint32_t unused : 28 - MethodData::nameBits { 0 };
    uint32_t bit;

    MemberGuardKind kind() const { return static_cast<MemberGuardKind>(kindBits); }
    ClassSide side() const { return static_cast<ClassSide>(sideBits); }
    uint32_t wordIndex() const { return bit / bitsPerWord; }
    uint64_t mask() const { return 1ull << bit % bitsPerWord; }
};
static_assert(sizeof(MemberGuardData) == 8 && isImageRecord<MemberGuardData>);
static_assert(static_cast<unsigned>(MemberGuardKind::PassedByStores) < 1u << 3 && numberOfClassSides <= 1u << 1);

// A site with several conditions tests one bit, which is set while every guard of the set is.
struct MemberGuardAggregates {
    uint32_t offset;
    uint32_t count;

    static constexpr uint32_t isAwaitedBeforeClassExists = 1u << 31;
    static constexpr uint32_t holdsWhileClear = 1u << 30;

    static constexpr uint32_t bitNumber(uint32_t byteOffsetOfWords, uint32_t bit) { return byteOffsetOfWords * 8 + bit; }
    static constexpr uint32_t numberInListEntry(uint32_t entry) { return entry & ~(isAwaitedBeforeClassExists | holdsWhileClear); }
    static constexpr uint32_t wordIndex(uint32_t number) { return number / MemberGuardData::bitsPerWord; }
    static constexpr uint64_t mask(uint32_t number) { return 1ull << number % MemberGuardData::bitsPerWord; }
};
static_assert(sizeof(MemberGuardAggregates) == 8 && isImageRecord<MemberGuardAggregates>);

struct ClassData {
    // The parent that is proven, and the one that the checker names where none is: that one holds only while the link guards of the class are set.
    uint32_t base;
    uint32_t expectedBase;
    // The constructor of a built-in class that is proven to be the parent, as a BuiltinID, where the run time can make the class without the code of its definition.
    uint32_t builtinBase;
    uint32_t classConstructor;
    uint32_t environment;
    uint32_t numberOfMethods;
    uint32_t numberOfStaticMethods;
    uint32_t numberOfStaticFields;
    uint32_t numberOfMemberGuards : 29;
    // Objects of the class are cells of a built-in class, and their Structures identify the program class they were created for.
    uint32_t instancesCarryBrand : 1;
    uint32_t hasMemberGuardAggregates : 1;
    // The first definition can only make what the record says: every member has a constant name, and the same goes for each class above.
    uint32_t guardsHoldWhileClear : 1;
    uint32_t guardWordsOffset[numberOfClassSides];
    // In the program tables: how many classes have this one as their assumed base, then their indexes, ascending. 0 where there is none.
    uint32_t subclassesOffset;
    // In the program tables, for a class whose objects carry its brand: the number of fields, then their name numbers in ascending order. 0 if a field has no name known at build time.
    uint32_t namesOfFieldsOffset;

    std::optional<uint32_t> guardWordsOffsetFor(ClassSide side) const
    {
        uint32_t offset = guardWordsOffset[static_cast<unsigned>(side)];
        if (!offset)
            return std::nullopt;
        return offset;
    }
};
static_assert(sizeof(ClassData) == 52 && isImageRecord<ClassData>);

struct ExtensionLayoutData {
    SlotData extension;
    uint32_t extensionSize;
    // The granules of the fields out of line that the constructor gives a value at every activation.
    uint32_t granulesFilledByConstructorLow;
    uint32_t granulesFilledByConstructorHigh;
};
static_assert(sizeof(ExtensionLayoutData) == 16 && isImageRecord<ExtensionLayoutData>);

struct LayoutData {
    static constexpr uint32_t noBase = UINT32_MAX;

    static constexpr unsigned impossibleMutationBits = 13;

    uint32_t kindBits : 4;
    // A layout that the compiler derived from its base: the base plus the names that some of its objects get. Its objects are instances of the class of the base.
    uint32_t isDerived : 1;
    uint32_t impossibleMutationsRaw : impossibleMutationBits;
    uint32_t evaluationBits : 2;
    uint32_t keysAreNeverEnumerated : 1;
    uint32_t hasFieldsWithSymbolKeys : 1;
    // The bits of every slot are all there is to its field, so a copy of the slots is a copy of the fields.
    uint32_t hasOnlyPlainSlots : 1;
    uint32_t isUntyped : 1;
    uint32_t hasUntypedBase : 1;
    uint32_t createsMembersLazily : 1;
    uint32_t createsPrototypeBeforeClassObject : 1;
    uint32_t createsClassObjectOnMutation : 1;
    // What lies in front of the record, from the record backwards: the slot of the extra properties, the data of the extension, the data of the class.
    uint32_t hasExtraPropertiesSlot : 1;
    uint32_t hasExtensionData : 1;
    uint32_t hasSlotWithUnusedMarkers : 1;
    uint32_t isCopiedByWords : 1;
    uint32_t index;
    uint32_t shape;
    uint32_t name;
    uint32_t size;
    uint16_t numberOfFields;
    uint16_t numberOfFieldOperands;
    uint32_t numberOfOrders;
    int32_t referencesOffset;
    uint32_t ordersOffset;

    // The bit fields above, as one load of compiled code finds them.
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
    uint64_t granulesFilledByConstructor() const { return hasExtensionData ? static_cast<uint64_t>(extensionData().granulesFilledByConstructorHigh) << 32 | extensionData().granulesFilledByConstructorLow : 0; }

    uint32_t base() const { return isClass() ? classData().base : noBase; }
    uint32_t assumedBase() const
    {
        if (!isClass())
            return noBase;
        return classData().base == noBase ? classData().expectedBase : classData().base;
    }

    std::span<const FieldData> fields() const { return { reinterpret_cast<const FieldData*>(this + 1), numberOfFields }; }
    std::span<const uint32_t> fieldOperands() const { return { reinterpret_cast<const uint32_t*>(fields().data() + numberOfFields), numberOfFieldOperands }; }
    std::span<const MethodData> methods() const { return { reinterpret_cast<const MethodData*>(fieldOperands().data() + numberOfFieldOperands), isClass() ? classData().numberOfMethods : 0 }; }
    std::span<const MethodData> staticMethods() const { return { methods().data() + methods().size(), isClass() ? classData().numberOfStaticMethods : 0 }; }
    std::span<const FieldData> staticFields() const { return { reinterpret_cast<const FieldData*>(staticMethods().data() + staticMethods().size()), isClass() ? classData().numberOfStaticFields : 0 }; }
    size_t numberOfMemberGuards() const { return isClass() ? classData().numberOfMemberGuards : 0; }
    std::span<const MemberGuardData> memberGuards() const { return { reinterpret_cast<const MemberGuardData*>(staticFields().data() + staticFields().size()), numberOfMemberGuards() }; }
    // One record for each guard, in the order of the guards, or none at all.
    std::span<const MemberGuardAggregates> memberGuardAggregates() const { return { reinterpret_cast<const MemberGuardAggregates*>(memberGuards().data() + numberOfMemberGuards()), isClass() && classData().hasMemberGuardAggregates ? numberOfMemberGuards() : 0 }; }
    MemberGuardAggregates aggregatesOf(const MemberGuardData& guard) const
    {
        std::span<const MemberGuardAggregates> aggregates = memberGuardAggregates();
        return aggregates.empty() ? MemberGuardAggregates { 0, 0 } : aggregates[&guard - memberGuards().data()];
    }

    uint16_t indexOf(const FieldData& field) const { return static_cast<uint16_t>(&field - fields().data()); }
    // Only a field whose slot defers a closure has operands: the function and, where the closure captures an environment, the slot of the hidden field that holds it.
    FunctionID deferredClosureOf(const FieldData& field) const
    {
        ASSERT(field.slot.representation() == SlotRepresentation::DeferredClosure);
        return FunctionID(fieldOperands()[field.operandIndex]);
    }
    SlotData closureEnvironmentSlotOf(const FieldData& field) const
    {
        ASSERT(field.flags & fieldDeferredClosureCapturesEnvironment);
        return SlotData::decode(fieldOperands()[field.operandIndex + 1]);
    }

    static constexpr size_t prefixSize(bool isClass, bool hasExtensionData, bool hasExtraPropertiesSlot) { return (isClass ? sizeof(ClassData) : 0) + (hasExtensionData ? sizeof(ExtensionLayoutData) : 0) + (hasExtraPropertiesSlot ? sizeof(SlotData) : 0); }
    size_t prefixSize() const { return prefixSize(isClass(), hasExtensionData, hasExtraPropertiesSlot); }
    struct TrailingCounts {
        uint64_t fields { 0 };
        uint64_t fieldOperands { 0 };
        uint64_t methods { 0 };
        uint64_t staticFields { 0 };
        uint64_t memberGuards { 0 };
        uint64_t memberGuardAggregates { 0 };
    };
    static constexpr uint64_t trailingSize(TrailingCounts counts)
    {
        return (counts.fields + counts.staticFields) * sizeof(FieldData) + counts.fieldOperands * sizeof(uint32_t) + counts.methods * sizeof(MethodData) + counts.memberGuards * sizeof(MemberGuardData) + counts.memberGuardAggregates * sizeof(MemberGuardAggregates);
    }
    uint64_t trailingSize() const
    {
        TrailingCounts counts { .fields = numberOfFields, .fieldOperands = numberOfFieldOperands };
        if (isClass()) {
            counts.methods = static_cast<uint64_t>(classData().numberOfMethods) + classData().numberOfStaticMethods;
            counts.staticFields = classData().numberOfStaticFields;
            counts.memberGuards = numberOfMemberGuards();
            counts.memberGuardAggregates = memberGuardAggregates().size();
        }
        return trailingSize(counts);
    }
};
static_assert(sizeof(LayoutData) == 36 && offsetof(LayoutData, index) == 4 && isImageRecord<LayoutData>);
static_assert(numberOfEnumerators<LayoutKind> <= 1u << 5 && numberOfEnumerators<EvaluationState> <= 1u << 2);
static_assert(numberOfEnumerators<ObjectMutation> <= LayoutData::impossibleMutationBits);
static_assert(numberOfEnumerators<LayoutKind> <= 1u << 4);
static_assert(maxFieldsInOrder <= UINT16_MAX);

struct EnvironmentData {
    LayoutKind kind;
    uint8_t unused[3] { };
    uint32_t index;
    uint32_t shape;
    uint32_t name;
    uint32_t size;
    uint32_t numberOfDescribedVariables;
    int32_t referencesOffset;

    std::span<const VariableData> describedVariables() const { return { reinterpret_cast<const VariableData*>(this + 1), numberOfDescribedVariables }; }
};
static_assert(sizeof(EnvironmentData) == 28 && isImageRecord<EnvironmentData>);

// The function that constructs what the allocation makes: ProgramTables::constructorOfAllocation().
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

// In the order of the allocations.
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
