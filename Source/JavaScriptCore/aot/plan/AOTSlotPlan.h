/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"
#include "AOTIRProgram.h"
#include "AOTLayout.h"
#include "AOTSchemaEnums.h"
#include "AOTShapeTest.h"
#include "AOTSlotRepresentation.h"
#include "AOTTypeIDs.h"
#include "AOTVector.h"
#include "JSExportMacros.h"
#include <array>
#include <bit>
#include <memory>
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/FastMalloc.h>
#include <wtf/HashMap.h>
#include <wtf/Lock.h>

namespace JSC { namespace AOT { namespace Typed {

class Program;

using FieldIndex = uint32_t;

inline constexpr unsigned numberOfHiddenWords = 2;

enum class LayoutStorage : uint8_t {
    None,
    CompiledObject,
    OrdinaryObject,
    Environment,
};

struct StorageOfLayout {
    bool hasCells() const { return holdsCompiledObjects() || storage == LayoutStorage::Environment; }
    bool holdsCompiledObjects() const { return storage == LayoutStorage::CompiledObject && !isRecordAlone; }

    LayoutStorage storage { LayoutStorage::None };
    bool isRecordAlone { false };
};

struct StorageOfLayouts {
    StorageOfLayout of(LayoutID id) const { return id.index() < layouts.size() ? layouts[id.index()] : StorageOfLayout { }; }
    bool isNeverAllocated(LayoutID id) const { return layoutsNeverAllocated.get(id.index()); }
    bool holdsCompiledObjectsInUse(LayoutID id) const { return of(id).holdsCompiledObjects() && !isNeverAllocated(id); }

    BitVector layoutsNeverAllocated;
    Vector<StorageOfLayout> layouts;
};

enum class SlotStorage : uint8_t {
    None,
    Inline,
    OutOfLine,
};

struct PlacedSlot {
    bool hasStorage() const { return storage != SlotStorage::None; }
    bool isOutOfLine() const { return storage == SlotStorage::OutOfLine; }
    bool needsExtension() const { return isOutOfLine(); }
    uint32_t width() const { return widthOf(representation); }
    uint32_t end() const { return offset + width(); }
    SlotData data() const { return SlotData::create(offset, representation); }
    TypeID typeInLayoutRecord() const { return typeOfRawValue ? typeOfRawValue : typeThatStoresAreHeldTo(); }
    TypeID typeThatStoresAreHeldTo() const { return provenType ? provenType : type; }
    bool hasLocationOf(const PlacedSlot& other) const { return storage == other.storage && offset == other.offset && representation == other.representation; }

    SlotRepresentation representation { SlotRepresentation::JSValue };
    SlotStorage storage { SlotStorage::None };
    bool mayExistWithoutStorage { false };
    bool isLeftOutByMakers { false };
    bool holdsAccessor { false };
    bool isFilledAtBirth { false };
    bool everyStoreFits { false };
    bool deferredClosureCapturesObject { false };
    bool deferredClosureCapturesEnvironment { false };
    uint32_t offset { 0 };
    std::optional<uint16_t> indexInLayoutRecord;
    FunctionID deferredFunction;
    TypeID type;
    TypeID provenType;
    TypeID typeOfRawValue;
};

struct PlannedLayout {
    bool hasSlots() const { return storage == LayoutStorage::CompiledObject || storage == LayoutStorage::Environment; }
    bool holdsCompiledObjects() const { return storage == LayoutStorage::CompiledObject && !isRecordAlone; }
    unsigned numberOfFields() const { return slots.isEmpty() ? 0 : slots.size() - numberOfHiddenWords; }
    FieldIndex extensionWordIndex() const { return numberOfFields(); }
    FieldIndex extraPropertiesWordIndex() const { return numberOfFields() + 1; }
    bool hasExtension() const { return !slots.isEmpty() && slots[extensionWordIndex()].hasStorage(); }
    bool hasExtraPropertiesWord() const { return !slots.isEmpty() && slots[extraPropertiesWordIndex()].hasStorage(); }
    bool holdsRawValueFromBirth(FieldIndex field) const { return fieldsThatHoldRawValuesFromBirth.get(field); }

    LayoutStorage storage { LayoutStorage::None };
    bool isRecordAlone { false };
    bool liesOnlyInFrames { false };
    bool cellsAreHandedBack { false };
    uint16_t numberOfRecordFields { 0 };
    uint32_t size { 0 };
    uint32_t extensionSize { 0 };
    uint32_t offsetOfLinkToNextCellHandedBack { 0 };
    uint64_t granulesOfFirstExtension { 0 };
    Vector<PlacedSlot> slots;
    Vector<PropertyKeyID> appendedKeys;
    Vector<uint16_t> placesOfAppendedKeys;
    void indexAppendedKeys();
    size_t findAppendedKey(PropertyKeyID) const;
    std::optional<FieldIndex> environmentOfDeferredClosures;
    BitVector fieldsThatHoldRawValuesFromBirth;
    bool mayBearMark { false };
};

inline size_t PlannedLayout::findAppendedKey(PropertyKeyID key) const
{
    if (placesOfAppendedKeys.isEmpty())
        return appendedKeys.find(key);
    uint32_t mask = static_cast<uint32_t>(placesOfAppendedKeys.size()) - 1;
    uint32_t hash = key.bits() * 0x9e3779b1u;
    for (uint32_t at = (hash ^ hash >> 15) & mask;; at = (at + 1) & mask) {
        uint16_t place = placesOfAppendedKeys[at];
        if (!place)
            return notFound;
        if (appendedKeys[place - 1] == key)
            return place - 1;
    }
}

inline void PlannedLayout::indexAppendedKeys()
{
    static constexpr size_t fewEnoughToLookThrough = 8;
    placesOfAppendedKeys.shrink(0);
    if (appendedKeys.size() <= fewEnoughToLookThrough || appendedKeys.size() >= UINT16_MAX)
        return;
    placesOfAppendedKeys.fill(0, std::bit_ceil(2 * appendedKeys.size()));
    uint32_t mask = static_cast<uint32_t>(placesOfAppendedKeys.size()) - 1;
    for (size_t place = 0; place < appendedKeys.size(); ++place) {
        uint32_t hash = appendedKeys[place].bits() * 0x9e3779b1u;
        uint32_t at = (hash ^ hash >> 15) & mask;
        while (placesOfAppendedKeys[at])
            at = (at + 1) & mask;
        placesOfAppendedKeys[at] = static_cast<uint16_t>(place + 1);
    }
}

ALWAYS_INLINE std::optional<FieldIndex> findFieldOfPlannedLayout(const Program& program, const PlannedLayout& planned, LayoutID id, PropertyKeyID key)
{
    const Layout& declaring = program.at(id);
    if (std::optional<uint32_t> declared = declaring.findField(key)) {
        bool isSpelledByTypeAlone = planned.storage == LayoutStorage::CompiledObject && *declared < planned.numberOfFields() && !planned.slots.span()[*declared].indexInLayoutRecord;
        return isSpelledByTypeAlone ? std::nullopt : declared;
    }
    size_t place = planned.findAppendedKey(key);
    if (place == notFound)
        return std::nullopt;
    return static_cast<FieldIndex>(declaring.fields.size() + place);
}

class PlannedLayouts {
public:
    explicit PlannedLayouts(std::span<const PlannedLayout> layouts)
        : m_layouts(layouts)
    {
    }

    const PlannedLayout& layout(LayoutID id) const { return id.index() < m_layouts.size() ? m_layouts[id.index()] : m_unplannedLayout; }
    std::optional<FieldIndex> findField(const Program& program, LayoutID id, PropertyKeyID key) const
    {
        if (!id || !key)
            return std::nullopt;
        return findFieldOfPlannedLayout(program, layout(id), id, key);
    }

private:
    std::span<const PlannedLayout> m_layouts;
    PlannedLayout m_unplannedLayout;
};

struct FieldAccess {
    enum class Coverage : uint8_t {
        NoMember,
        SomeMembers,
        EveryMember,
    };

    bool hasLocation { false };
    Coverage coverage { Coverage::NoMember };
    bool mayBeFilledWithoutStorage { false };
    bool findsOffsetByShape { false };
    PropertyKeyID key;
    PlacedSlot slot;
    PlacedSlot extensionWord;
    Vector<LayoutID, 4> membersWithField;
};

enum class StoredFieldForm : uint8_t {
    Any,
    TakesAnyValue,
    AcceptsAnyNumber,
    KeepsNumbersAsValues,
};

enum class VariableStorage : uint8_t {
    Statics,
    ClosureTable,
};

enum class VariableEncoding : uint8_t {
    Plain,
    FunctionAsZero,
};

struct PlannedVariable {
    bool hasWordInStatics() const { return storage == VariableStorage::Statics; }

    VariableStorage storage { VariableStorage::Statics };
    VariableEncoding encoding { VariableEncoding::Plain };
    SlotRepresentation representation { SlotRepresentation::JSValue };
    bool isDescribedToRunTime { false };
    FunctionID function;
    uint64_t initialWord { 0 };
};

enum class MemberCreation : uint8_t {
    AtDefinition,
    ByClassRecord,
};

struct ClassCreationPlan {
    bool createsMembersLazily { false };
    bool createsPrototypeBeforeClassObject { false };
    bool createsClassObjectOnMutation { false };
    bool guardsHoldWhileClear { false };
    bool guardsOfPrototypeSideHoldWhileClear { false };
    Vector<MemberCreation> members;
};

struct PlacedAllocation {
    LayoutID layout;
    uint32_t shape { 0 };
    uint32_t size { 0 };
    uint32_t orderWord { 0 };
    FunctionID constructedBy;
    uint16_t numberOfCaptures { 0 };
    bool isBornUnfinished { false };
};

struct MentionedKeys {
    bool isSpelled(PropertyKeyID key) const { return key.index() >= isMentioned.size() || isMentioned[key.index()] || isSpelledAsText.get(key.index()); }

    Vector<uint8_t> isMentioned;
    BitVector isSpelledAsText;
};

struct SlotPlanParts {
    Vector<PlannedLayout> layouts;
    Vector<PlannedVariable> variables;
    Vector<ClosureForm> closureForms;
    Vector<ClassCreationPlan> classCreation;
    BitVector layoutsNeverAllocated;
    BitVector keysWithColumn;
    BitVector keysOfSlotsThatHoldMarks;
    Vector<SlotRepresentation> narrowFormsOfKeys;
    BitVector keysOfFields;
};

struct FieldAccessesOfSets {
    struct Part {
        Lock lock;
        std::array<HashMap<uint64_t, std::unique_ptr<FieldAccess>>, 2> accesses;
        struct OfMembers {
            PropertyKeyID key;
            Vector<LayoutID> members;
            FieldAccess access;
        };
        std::array<HashMap<uint64_t, Vector<std::unique_ptr<OfMembers>, 1>>, 2> accessesOfMembers;
    };
    std::array<Part, 64> parts;
};

bool fieldStaysInSlot(const Program&, const PlacedSlot&);
class SlotPlan;
struct LinkTables;
Vector<FieldIndex, 8> fieldsHeldFilledInSlots(const Program&, const LinkTables&, const SlotPlan&, LayoutID);
enum class RootOfConstant : uint8_t { IsAsked, EndsWhereItLeaves };
bool constantListsFieldsHeldFilledInSlots(const Program&, const LinkTables&, const SlotPlan&, DataID, RootOfConstant = RootOfConstant::IsAsked);

class SlotPlan {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(SlotPlan);
public:
    SlotPlan() = default;
    explicit SlotPlan(SlotPlanParts&& parts)
        : m_parts(WTF::move(parts))
        , m_fieldAccessesOfSets(makeUniqueWithoutFastMallocCheck<FieldAccessesOfSets>())
    {
    }

    const PlannedLayout& layout(LayoutID id) const { return id.index() < m_parts.layouts.size() ? m_parts.layouts[id.index()] : m_unplannedLayout; }
    bool holdsCompiledObjects(LayoutID id) const { return layout(id).holdsCompiledObjects(); }
    bool isNeverAllocated(LayoutID id) const { return m_parts.layoutsNeverAllocated.get(id.index()); }

    const PlacedSlot& slot(LayoutID id, FieldIndex field) const
    {
        const PlannedLayout& planned = layout(id);
        return field < planned.slots.size() ? planned.slots[field] : m_unplannedSlot;
    }
    ALWAYS_INLINE std::optional<FieldIndex> findField(const Program& program, LayoutID id, PropertyKeyID key) const
    {
        if (!id || !key)
            return std::nullopt;
        return findFieldOfPlannedLayout(program, layout(id), id, key);
    }
    JS_EXPORT_PRIVATE PropertyKeyID keyOf(const Program&, FieldReference) const;
    const PlacedSlot& extensionWord(LayoutID id) const { return slot(id, layout(id).extensionWordIndex()); }
    const PlacedSlot& extraPropertiesWord(LayoutID id) const { return slot(id, layout(id).extraPropertiesWordIndex()); }

    FieldAccess fieldAccess(const Program& program, PropertyKeyID key, std::span<const LayoutID> receivers, bool isStore) const
    {
        if (receivers.size() == 1)
            return fieldAccessOfOneLayout(program, key, receivers[0]);
        return fieldAccessOfLayouts(program, key, receivers, isStore);
    }
    JS_EXPORT_PRIVATE FieldAccess fieldAccessOfOneLayout(const Program&, PropertyKeyID, LayoutID receiver) const;
    JS_EXPORT_PRIVATE FieldAccess fieldAccessOfLayouts(const Program&, PropertyKeyID, std::span<const LayoutID> receivers, bool isStore) const;
    JS_EXPORT_PRIVATE FieldAccess fieldAccess(const Program&, PropertyKeyID, LayoutSetID, std::span<const LayoutID> members, bool isStore) const;
    bool storeMayListKey(const FieldAccess& access) const
    {
        return canLeaveSlot(access.slot.representation) && !access.membersWithField.isEmpty();
    }
    JS_EXPORT_PRIVATE std::optional<uint32_t> recordOfStoredField(const Program&, PropertyKeyID, std::span<const LayoutID> receivers) const;
    JS_EXPORT_PRIVATE StoredFieldForm formOfStoredField(const Program&, PropertyKeyID, std::span<const LayoutID> receivers) const;

    PlannedVariable variable(VariableID id) const { return id.index() < m_parts.variables.size() ? m_parts.variables[id.index()] : PlannedVariable { }; }
    ClosureForm closureFormOf(FunctionID id) const { return id.index() < m_parts.closureForms.size() ? m_parts.closureForms[id.index()] : ClosureForm::Full; }
    const ClassCreationPlan& classCreation(LayoutID id) const { return id.index() < m_parts.classCreation.size() ? m_parts.classCreation[id.index()] : m_unplannedClass; }

    bool hasColumn(PropertyKeyID key) const { return m_parts.keysWithColumn.get(key.index()); }
    bool someSlotUnderKeyHoldsMark(PropertyKeyID key) const { return m_parts.keysOfSlotsThatHoldMarks.get(key.index()); }
    const BitVector& keysWithColumn() const { return m_parts.keysWithColumn; }
    bool namesField(PropertyKeyID key) const { return m_parts.keysOfFields.get(key.index()); }
    SlotRepresentation narrowFormOf(PropertyKeyID key) const { return key.index() < m_parts.narrowFormsOfKeys.size() ? m_parts.narrowFormsOfKeys[key.index()] : SlotRepresentation::JSValue; }

    unsigned numberOfLayouts() const { return m_parts.layouts.size(); }
    unsigned numberOfVariables() const { return m_parts.variables.size(); }

private:
    SlotPlanParts m_parts;
    PlannedLayout m_unplannedLayout;
    PlacedSlot m_unplannedSlot;
    ClassCreationPlan m_unplannedClass;
    std::unique_ptr<FieldAccessesOfSets> m_fieldAccessesOfSets;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
