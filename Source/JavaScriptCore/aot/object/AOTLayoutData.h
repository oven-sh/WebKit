/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTSlotRepresentation.h"
#include <cstddef>
#include <optional>
#include <span>
#include <wtf/OptionSet.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

static constexpr uint32_t noBitSet = UINT32_MAX;
static constexpr uint32_t noDeclaredPath = UINT32_MAX;

struct ShapeTestData {
    uint32_t first;
    uint32_t count;
    uint32_t bitSetOffset;

    static constexpr ptrdiff_t offsetOfFirst() { return 0; }
    static constexpr ptrdiff_t offsetOfCount() { return 4; }
    static constexpr ptrdiff_t offsetOfBitSetOffset() { return 8; }
};
static_assert(sizeof(ShapeTestData) == 12 && isImageRecord<ShapeTestData>);

struct LayoutSetData {
    ShapeTestData members;
    uint32_t name;
    uint32_t declaredPath;
    uint32_t declaredLine;
};
static_assert(sizeof(LayoutSetData) == 24 && isImageRecord<LayoutSetData>);

static constexpr uint16_t fieldIsOptional = 1u << 0;
static constexpr uint16_t fieldIsOutOfLine = 1u << 1;
static constexpr uint16_t fieldIsStoredOnce = 1u << 2;
static constexpr uint16_t fieldAllowsUndefined = 1u << 3;
static constexpr uint16_t fieldMayExistWithoutStorage = 1u << 4;
static constexpr uint16_t fieldDeferredClosureCapturesObject = 1u << 5;
static constexpr uint16_t fieldIsPrivate = 1u << 6;
static constexpr uint16_t fieldIsBrand = 1u << 7;
static constexpr uint16_t fieldHasOperand = 1u << 8;
static constexpr uint16_t fieldIsNeverDeleted = 1u << 9;
static constexpr uint16_t fieldIsNeverRedefined = 1u << 10;
static constexpr uint16_t fieldIsNeverStoredWithOtherType = 1u << 11;
static constexpr uint16_t allFieldFlags = (1u << 12) - 1;

struct FieldData {
    uint32_t name;
    uint16_t flags;
    uint16_t operandIndex;
    SlotData slot;
    uint32_t type;

    bool isOutOfLine() const { return flags & fieldIsOutOfLine; }
    bool isPrivate() const { return flags & fieldIsPrivate; }
    bool isBrand() const { return flags & fieldIsBrand; }
    bool canLeaveSlot() const { return AOT::canLeaveSlot(slot.representation()); }
};
static_assert(sizeof(FieldData) == 16 && isImageRecord<FieldData>);

struct VariableData {
    uint32_t name;
    SlotData slot;
};
static_assert(sizeof(VariableData) == 8 && isImageRecord<VariableData>);

static constexpr uint32_t methodHasComputedName = 1;
static constexpr uint32_t methodTakesEnvironment = 2;
static constexpr uint32_t methodIsCreatedAtDefinition = 4;

struct MethodData {
    static constexpr unsigned nameBits = 24;
    uint32_t name : nameBits;
    uint32_t kindBits : 2;
    uint32_t flags : 30 - nameBits;
    uint32_t function;

    MethodKind kind() const { return static_cast<MethodKind>(kindBits); }
};
static_assert(sizeof(MethodData) == 8 && isImageRecord<MethodData>);
static_assert(numberOfEnumerators<MethodKind> <= 1u << 2 && methodIsCreatedAtDefinition < 1u << (30 - MethodData::nameBits));

static constexpr unsigned numberOfClassSides = numberOfEnumerators<ClassSide>;

enum class MemberGuardKind : uint8_t { Member, PrototypeLink, EvaluatedOnce };

struct MemberGuardData {
    static constexpr unsigned bitsPerWord = 64;

    uint32_t name : MethodData::nameBits;
    uint32_t kindBits : 2;
    uint32_t sideBits : 1;
    uint32_t unused : 29 - MethodData::nameBits { 0 };
    uint32_t bit;

    MemberGuardKind kind() const { return static_cast<MemberGuardKind>(kindBits); }
    ClassSide side() const { return static_cast<ClassSide>(sideBits); }
    uint32_t wordIndex() const { return bit / bitsPerWord; }
    uint64_t mask() const { return 1ull << bit % bitsPerWord; }
};
static_assert(sizeof(MemberGuardData) == 8 && isImageRecord<MemberGuardData>);
static_assert(static_cast<unsigned>(MemberGuardKind::EvaluatedOnce) < 1u << 2 && numberOfClassSides <= 1u << 1);

struct ClassData {
    uint32_t base;
    uint32_t predictedBase;
    uint32_t classConstructor;
    uint32_t environment;
    uint32_t numberOfMethods;
    uint32_t numberOfStaticMethods;
    uint32_t numberOfStaticFields;
    uint32_t numberOfMemberGuards;
    uint32_t guardWordsOffset[numberOfClassSides];

    std::optional<uint32_t> guardWordsOffsetFor(ClassSide side) const
    {
        uint32_t offset = guardWordsOffset[static_cast<unsigned>(side)];
        if (!offset)
            return std::nullopt;
        return offset;
    }
};
static_assert(sizeof(ClassData) == 40 && isImageRecord<ClassData>);

struct RareLayoutData {
    SlotData extension;
    SlotData extraProperties;
    uint32_t extensionSize;
};
static_assert(sizeof(RareLayoutData) == 12 && isImageRecord<RareLayoutData>);

struct MessageLayoutData {
    uint32_t typeStringsOffset;
    uint32_t declaredPath;
    uint32_t declaredLine;
};
static_assert(sizeof(MessageLayoutData) == 12 && isImageRecord<MessageLayoutData>);

struct LayoutData {
    static constexpr uint32_t noBase = UINT32_MAX;

    static constexpr unsigned impossibleMutationBits = 13;

    uint32_t kindBits : 7;
    // A layout that the compiler derived from its base: the base plus the names that some of its objects get. Its objects are instances of the class of the base.
    uint32_t isDerived : 1;
    uint32_t impossibleMutationsRaw : impossibleMutationBits;
    uint32_t evaluationBits : 2;
    uint32_t keysAreNeverEnumerated : 1;
    uint32_t hasFieldsWithSymbolKeys : 1;
    uint32_t isUntyped : 1;
    uint32_t hasUntypedBase : 1;
    uint32_t createsMembersLazily : 1;
    uint32_t createsPrototypeBeforeClassObject : 1;
    uint32_t createsClassObjectOnMutation : 1;
    uint32_t hasRareData : 1;
    uint32_t hasMessageData : 1;
    uint32_t index;
    uint32_t shape;
    uint32_t name;
    uint32_t size;
    uint16_t numberOfFields;
    uint16_t numberOfFieldOperands;
    uint32_t numberOfOrders;
    int32_t referencesOffset;
    uint32_t ordersOffset;

    LayoutKind kind() const { return static_cast<LayoutKind>(kindBits); }
    bool isClass() const { return kind() == LayoutKind::Instance; }
    EvaluationState evaluation() const { return static_cast<EvaluationState>(evaluationBits); }
    OptionSet<ObjectMutation> impossibleMutations() const { return OptionSet<ObjectMutation>::fromRaw(impossibleMutationsRaw); }

    const RareLayoutData& rareData() const
    {
        ASSERT(hasRareData);
        return reinterpret_cast<const RareLayoutData*>(this)[-1];
    }
    RareLayoutData& rareData() { return const_cast<RareLayoutData&>(const_cast<const LayoutData*>(this)->rareData()); }
    const MessageLayoutData& messageData() const
    {
        ASSERT(hasMessageData);
        return reinterpret_cast<const MessageLayoutData*>(reinterpret_cast<const uint8_t*>(this) - (hasRareData ? sizeof(RareLayoutData) : 0))[-1];
    }
    MessageLayoutData& messageData() { return const_cast<MessageLayoutData&>(const_cast<const LayoutData*>(this)->messageData()); }
    const ClassData& classData() const
    {
        ASSERT(isClass());
        return reinterpret_cast<const ClassData*>(reinterpret_cast<const uint8_t*>(this) - (hasRareData ? sizeof(RareLayoutData) : 0) - (hasMessageData ? sizeof(MessageLayoutData) : 0))[-1];
    }
    ClassData& classData() { return const_cast<ClassData&>(const_cast<const LayoutData*>(this)->classData()); }

    std::optional<SlotData> extensionSlot() const
    {
        if (!hasRareData || !rareData().extension.offset)
            return std::nullopt;
        return rareData().extension;
    }
    std::optional<SlotData> extraPropertiesSlot() const
    {
        if (!hasRareData || !rareData().extraProperties.offset)
            return std::nullopt;
        return rareData().extraProperties;
    }
    bool hasExtension() const { return extensionSlot().has_value(); }
    uint32_t extensionSize() const { return hasRareData ? rareData().extensionSize : 0; }

    uint32_t typeStringsOffset() const { return hasMessageData ? messageData().typeStringsOffset : 0; }
    uint32_t declaredPath() const { return hasMessageData ? messageData().declaredPath : noDeclaredPath; }
    uint32_t declaredLine() const { return hasMessageData ? messageData().declaredLine : 0; }
    uint32_t base() const { return isClass() ? classData().base : noBase; }
    uint32_t assumedBase() const
    {
        if (!isClass())
            return noBase;
        return classData().base == noBase ? classData().predictedBase : classData().base;
    }

    std::span<const FieldData> fields() const { return { reinterpret_cast<const FieldData*>(this + 1), numberOfFields }; }
    std::span<const uint32_t> fieldOperands() const { return { reinterpret_cast<const uint32_t*>(fields().data() + numberOfFields), numberOfFieldOperands }; }
    std::span<const MethodData> methods() const { return { reinterpret_cast<const MethodData*>(fieldOperands().data() + numberOfFieldOperands), isClass() ? classData().numberOfMethods : 0 }; }
    std::span<const MethodData> staticMethods() const { return { methods().data() + methods().size(), isClass() ? classData().numberOfStaticMethods : 0 }; }
    std::span<const FieldData> staticFields() const { return { reinterpret_cast<const FieldData*>(staticMethods().data() + staticMethods().size()), isClass() ? classData().numberOfStaticFields : 0 }; }
    std::span<const MemberGuardData> memberGuards() const { return { reinterpret_cast<const MemberGuardData*>(staticFields().data() + staticFields().size()), isClass() ? classData().numberOfMemberGuards : 0 }; }

    uint16_t indexOf(const FieldData& field) const { return static_cast<uint16_t>(&field - fields().data()); }
    uint32_t operandOf(const FieldData& field) const
    {
        ASSERT(field.flags & fieldHasOperand);
        return fieldOperands()[field.operandIndex];
    }
    uint32_t overflowOffsetOf(const FieldData& field) const
    {
        ASSERT(field.slot.representation() == SlotRepresentation::Int32OrDouble);
        return operandOf(field);
    }
    FunctionID deferredClosureOf(const FieldData& field) const
    {
        ASSERT(field.slot.representation() == SlotRepresentation::DeferredClosure);
        return FunctionID(operandOf(field));
    }

    static constexpr size_t prefixSize(bool isClass, bool hasMessageData, bool hasRareData) { return (isClass ? sizeof(ClassData) : 0) + (hasMessageData ? sizeof(MessageLayoutData) : 0) + (hasRareData ? sizeof(RareLayoutData) : 0); }
    size_t prefixSize() const { return prefixSize(isClass(), hasMessageData, hasRareData); }
    struct TrailingCounts {
        uint64_t fields { 0 };
        uint64_t fieldOperands { 0 };
        uint64_t methods { 0 };
        uint64_t staticFields { 0 };
        uint64_t memberGuards { 0 };
    };
    static constexpr uint64_t trailingSize(TrailingCounts counts)
    {
        return (counts.fields + counts.staticFields) * sizeof(FieldData) + counts.fieldOperands * sizeof(uint32_t) + counts.methods * sizeof(MethodData) + counts.memberGuards * sizeof(MemberGuardData);
    }
    uint64_t trailingSize() const
    {
        TrailingCounts counts { .fields = numberOfFields, .fieldOperands = numberOfFieldOperands };
        if (isClass()) {
            counts.methods = static_cast<uint64_t>(classData().numberOfMethods) + classData().numberOfStaticMethods;
            counts.staticFields = classData().numberOfStaticFields;
            counts.memberGuards = classData().numberOfMemberGuards;
        }
        return trailingSize(counts);
    }
};
static_assert(sizeof(LayoutData) == 36 && offsetof(LayoutData, index) == 4 && isImageRecord<LayoutData>);
static_assert(numberOfEnumerators<LayoutKind> <= 1u << 7 && numberOfEnumerators<EvaluationState> <= 1u << 2);
static_assert(numberOfEnumerators<ObjectMutation> <= LayoutData::impossibleMutationBits);
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

struct AllocationData {
    static constexpr uint32_t notConstructed = UINT32_MAX;

    uint32_t shape;
    uint32_t orderWord;
    uint32_t size;
    uint32_t constructedBy;
};
static_assert(sizeof(AllocationData) == 16 && isImageRecord<AllocationData>);

static constexpr uint32_t maxKeyOrderEntry = UINT16_MAX - 1;

struct KeyOrderData {
    uint16_t length;

    std::span<const uint16_t> entries() const { return { reinterpret_cast<const uint16_t*>(this + 1), length }; }
};
static_assert(sizeof(KeyOrderData) == 2 && isImageRecord<KeyOrderData>);

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
