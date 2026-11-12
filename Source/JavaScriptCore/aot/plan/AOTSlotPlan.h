/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBitVector.h"
#include "AOTID.h"
#include "AOTIRProgram.h"
#include "AOTLayout.h"
#include "AOTSchemaEnums.h"
#include "AOTShapeTest.h"
#include "AOTSlotRepresentation.h"
#include "AOTTypeIDs.h"
#include "AOTVector.h"
#include "JSExportMacros.h"
#include <algorithm>
#include <array>
#include <bit>
#include <memory>
#include <optional>
#include <span>
#include <mutex>
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
    BitVector layoutsWithoutDirectInstances;
    BitVector layoutsThatRepeatTheirBases;
    Vector<LayoutID> nearestLayoutsAboveThatDoNotRepeat;
    Vector<StorageOfLayout> layouts;
};

inline uint32_t numberOfFieldsNotRepeated(const Program& program, const BitVector& layoutsThatRepeatTheirBases, LayoutID id)
{
    const Layout& layout = program.at(id);
    return layoutsThatRepeatTheirBases.get(id.index()) ? layout.ownFieldCount : static_cast<uint32_t>(layout.fields.size());
}

FieldReference fieldAboveLayoutThatRepeats(const Program&, std::span<const LayoutID> nearestLayoutsAboveThatDoNotRepeat, FieldReference);

inline FieldReference fieldNotRepeated(const Program& program, const BitVector& layoutsThatRepeatTheirBases, std::span<const LayoutID> nearestLayoutsAboveThatDoNotRepeat, FieldReference field)
{
    return layoutsThatRepeatTheirBases.get(field.layout.index()) ? fieldAboveLayoutThatRepeats(program, nearestLayoutsAboveThatDoNotRepeat, field) : field;
}

enum class SlotStorage : uint8_t {
    None,
    Inline,
    OutOfLine,
};

struct PlacedSlot {
    bool hasStorage() const { return storage != SlotStorage::None; }
    bool isOutOfLine() const { return storage == SlotStorage::OutOfLine; }
    bool needsExtension() const { return isOutOfLine(); }
    bool liesInCellOnTrial() const { return storage == SlotStorage::Inline && mayTakeWordLeftInCell; }
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
    bool isHeldFilled { false };
    bool everyStoreFits { false };
    bool deferredClosureCapturesObject { false };
    bool deferredClosureCapturesEnvironment { false };
    bool mayTakeWordLeftInCell { false };
    bool isStable { false };
    bool isStableWithoutNullish { false };
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
    bool marksFrozenObjects { false };
    bool marksOtherPrototypes { false };
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
    bool isFilledByInitializer { false };
    Vector<FieldIndex> fieldsHeldFilled;
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

template<size_t inlineCapacity>
class LayoutsOwnedOrLent {
public:
    LayoutsOwnedOrLent() = default;

    static LayoutsOwnedOrLent lentBy(const LayoutsOwnedOrLent& keptForBuild LIFETIME_BOUND)
    {
        LayoutsOwnedOrLent layouts;
        layouts.m_lent = keptForBuild.span();
        return layouts;
    }

    std::span<const LayoutID> span() const LIFETIME_BOUND { return m_own.isEmpty() ? m_lent : m_own.span(); }
    size_t size() const { return span().size(); }
    bool isEmpty() const { return span().empty(); }
    auto begin() const LIFETIME_BOUND { return span().begin(); }
    auto end() const LIFETIME_BOUND { return span().end(); }
    bool contains(LayoutID layout) const { return std::ranges::contains(span(), layout); }
    LayoutID operator[](size_t index) const
    {
        std::span<const LayoutID> layouts = span();
        RELEASE_ASSERT(index < layouts.size());
        return layouts[index];
    }

    void reserveInitialCapacity(size_t capacity) { m_own.reserveInitialCapacity(capacity); }
    void append(LayoutID layout)
    {
        RELEASE_ASSERT(m_lent.empty());
        m_own.append(layout);
    }

private:
    Vector<LayoutID, inlineCapacity> m_own;
    std::span<const LayoutID> m_lent;
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
    LayoutSetID layoutsThatTestInCodeLetThrough;
    PlacedSlot slot;
    PlacedSlot extensionWord;
    LayoutsOwnedOrLent<4> membersWithField;
};

class FieldAccessKeptForBuild {
    WTF_MAKE_NONCOPYABLE(FieldAccessKeptForBuild);
public:
    explicit FieldAccessKeptForBuild(FieldAccess&& access)
        : m_membersWithField(WTF::move(access.membersWithField))
        , m_access(WTF::move(access))
    {
        m_access.membersWithField = LayoutsOwnedOrLent<4>::lentBy(m_membersWithField);
    }

    const FieldAccess& withMembersLent() const LIFETIME_BOUND { return m_access; }

private:
    LayoutsOwnedOrLent<4> m_membersWithField;
    FieldAccess m_access;
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
    bool hasViewMirror { false };
    VariableID firstViewOfBuffer;
    VariableID firstViewOfElementSize;
    uint64_t locationsOfViewsOfBuffer { 0 };
    FunctionID function;
    uint64_t initialWord { 0 };
};

constexpr int32_t viewMirrorVectorOffset = 8;
constexpr int32_t viewMirrorLengthOffset = 16;
constexpr int32_t bufferLengthOffsetInViewMirror = 24;
constexpr unsigned viewMirrorVariableSize = 32;

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

enum class InlineFieldsOfModule : uint8_t { None, Few, Many };

struct ModulesThatWriteFields {
    UncheckedKeyHashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> namesDefinedAbove;
    Vector<uint32_t> firstOfKeys;
    Vector<uint32_t> modules;
    Vector<InlineFieldsOfModule> inlineFields;
};

struct SlotPlanParts {
    Vector<PlannedLayout> layouts;
    Vector<PlannedVariable> variables;
    Vector<ClosureForm> closureForms;
    BitVector functionsWhoseClosuresGetOnlyListedNames;
    BitVector functionsWhosePrototypePropertyStaysAsMade;
    Vector<ClassCreationPlan> classCreation;
    BitVector layoutsNeverAllocated;
    BitVector layoutsWithoutDirectInstances;
    BitVector keysWithColumn;
    BitVector keysOfSlotsThatHoldMarks;
    BitVector keysOfAccessors;
};

struct FieldAccessesOfSets {
    struct Part {
        Lock lock;
        std::array<HashMap<uint64_t, std::unique_ptr<FieldAccessKeptForBuild>>, 2> accesses;
        struct OfMembers {
            OfMembers(PropertyKeyID key, std::span<const LayoutID> members, FieldAccess&& access)
                : key(key)
                , members(members)
                , access(WTF::move(access))
            {
            }

            PropertyKeyID key;
            Vector<LayoutID> members;
            FieldAccessKeptForBuild access;
        };
        std::array<HashMap<uint64_t, Vector<std::unique_ptr<OfMembers>, 1>>, 2> accessesOfMembers;
        struct MembersByName {
            bool areListed { false };
            uint32_t numberInUse { 0 };
            Vector<LayoutID> askedAboutEveryName;
        };
        HashMap<uint32_t, std::unique_ptr<MembersByName>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> membersByName;
    };
    std::array<Part, 64> parts;
    std::once_flag layoutsByNameAreListed;
    Vector<uint64_t> layoutsByName;
    Vector<uint32_t> namesWrittenInViews;
};

bool fieldStaysInSlot(const Program&, const PlacedSlot&);
class SlotPlan;
bool takesAccessorsOfLibraryAlone(const Program&, const SlotPlan&, LayoutID, PropertyKeyID);
struct LinkTables;
void markFieldsHeldFilledInSlotPlan(const Program&, const LinkTables&, const ParallelLoops&, SlotPlanParts&);
Vector<FieldIndex, 8> fieldsHeldFilledInSlots(const Program&, const LinkTables&, const SlotPlan&, LayoutID);
enum class RootOfConstant : uint8_t { IsAsked, EndsWhereItLeaves };
bool constantListsFieldsHeldFilledInSlots(const Program&, const LinkTables&, const SlotPlan&, DataID, RootOfConstant = RootOfConstant::IsAsked);

class SlotPlan {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(SlotPlan);
    friend class FactMisstater;
public:
    SlotPlan() = default;
    explicit SlotPlan(SlotPlanParts&& parts)
        : m_parts(WTF::move(parts))
        , m_fieldAccessesOfSets(makeUniqueWithoutFastMallocCheck<FieldAccessesOfSets>())
    {
        m_layoutsInUseThatHoldOtherThanCompiledObjects.fill(0, (std::max<size_t>(m_parts.layouts.size(), m_parts.layoutsNeverAllocated.size()) + 63) / 64);
        for (size_t index = 0; index < 64 * m_layoutsInUseThatHoldOtherThanCompiledObjects.size(); ++index) {
            if (!holdsCompiledObjects(LayoutID(static_cast<uint32_t>(index))) && !m_parts.layoutsNeverAllocated.get(index))
                m_layoutsInUseThatHoldOtherThanCompiledObjects[index / 64] |= uint64_t { 1 } << index % 64;
        }
    }

    const PlannedLayout& layout(LayoutID id) const { return id.index() < m_parts.layouts.size() ? m_parts.layouts[id.index()] : m_unplannedLayout; }
    bool holdsCompiledObjects(LayoutID id) const { return layout(id).holdsCompiledObjects(); }
    bool isNeverAllocated(LayoutID id) const { return m_parts.layoutsNeverAllocated.get(id.index()); }
    bool hasDirectInstances(LayoutID id) const { return !m_parts.layoutsWithoutDirectInstances.get(id.index()); }
    bool someLayoutInUseHoldsOtherThanCompiledObjects(uint32_t wordOfLayouts, uint64_t layoutsInWord) const { return wordOfLayouts >= m_layoutsInUseThatHoldOtherThanCompiledObjects.size() || (m_layoutsInUseThatHoldOtherThanCompiledObjects[wordOfLayouts] & layoutsInWord); }

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
    JS_EXPORT_PRIVATE FieldAccess fieldAccessOfLayouts(const Program&, PropertyKeyID, std::span<const LayoutID> receivers, bool isStore, std::optional<uint32_t> numberInUseAmongAllReceivers = std::nullopt) const;
    enum class WhatIsWrittenAbove : bool { IsNotAsked, IsAsked };
    JS_EXPORT_PRIVATE bool listMembersAskedAbout(const Program&, PropertyKeyID, LayoutSetID, WhatIsWrittenAbove, Vector<LayoutID, 16>& asked, uint32_t& numberInUse) const;
    JS_EXPORT_PRIVATE FieldAccess fieldAccess(const Program&, PropertyKeyID, LayoutSetID, std::span<const LayoutID> members, bool isStore) const;
    bool storeMayListKey(const FieldAccess& access) const
    {
        return mayBeCreatedByStore(access.slot.representation) && !access.membersWithField.isEmpty();
    }
    JS_EXPORT_PRIVATE std::optional<uint32_t> recordOfStoredField(const Program&, PropertyKeyID, std::span<const LayoutID> receivers) const;
    JS_EXPORT_PRIVATE StoredFieldForm formOfStoredField(const Program&, PropertyKeyID, std::span<const LayoutID> receivers) const;

    PlannedVariable variable(VariableID id) const { return id.index() < m_parts.variables.size() ? m_parts.variables[id.index()] : PlannedVariable { }; }
    ClosureForm closureFormOf(FunctionID id) const { return id.index() < m_parts.closureForms.size() ? m_parts.closureForms[id.index()] : ClosureForm::Full; }
    bool closuresGetOnlyListedNames(FunctionID id) const { return m_parts.functionsWhoseClosuresGetOnlyListedNames.get(id.index()); }
    bool prototypePropertyStaysAsMade(FunctionID id) const { return m_parts.functionsWhosePrototypePropertyStaysAsMade.get(id.index()); }
    size_t closedClosureFunctionCount() const { return m_parts.functionsWhoseClosuresGetOnlyListedNames.bitCount(); }
    const ClassCreationPlan& classCreation(LayoutID id) const { return id.index() < m_parts.classCreation.size() ? m_parts.classCreation[id.index()] : m_unplannedClass; }

    bool someSlotUnderKeyHoldsMark(PropertyKeyID key) const { return m_parts.keysOfSlotsThatHoldMarks.get(key.index()); }
    bool someLayoutHasAccessor(PropertyKeyID key) const { return m_parts.keysOfAccessors.get(key.index()); }
    const BitVector& keysWithColumn() const { return m_parts.keysWithColumn; }
    void setModulesThatWriteFields(ModulesThatWriteFields&& modules) { m_modulesThatWriteFields = WTF::move(modules); }
    bool someLayoutHasField(PropertyKeyID key) const
    {
        std::span<const uint32_t> first = m_modulesThatWriteFields.firstOfKeys.span();
        return static_cast<size_t>(key.index()) + 1 >= first.size() || first[key.index()] != first[key.index() + 1];
    }
    static uint64_t nameInModule(ModuleID module, PropertyKeyID key) { return static_cast<uint64_t>(module.index()) << 32 | key.bits(); }
    bool mayBeDefinedAbove(ModuleID module, PropertyKeyID key) const
    {
        const auto& names = m_modulesThatWriteFields.namesDefinedAbove;
        return module && key && !names.isEmpty() && names.contains(nameInModule(module, key));
    }
    bool someLayoutWrittenInHasField(ModuleID module, PropertyKeyID key) const
    {
        std::span<const uint32_t> first = m_modulesThatWriteFields.firstOfKeys.span();
        if (!module || static_cast<size_t>(key.index()) + 1 >= first.size())
            return false;
        return std::ranges::binary_search(m_modulesThatWriteFields.modules.span().subspan(first[key.index()], first[key.index() + 1] - first[key.index()]), module.index());
    }

    InlineFieldsOfModule inlineFieldsWrittenIn(ModuleID module, PropertyKeyID key) const
    {
        std::span<const uint32_t> first = m_modulesThatWriteFields.firstOfKeys.span();
        if (!module || static_cast<size_t>(key.index()) + 1 >= first.size())
            return InlineFieldsOfModule::None;
        auto modules = m_modulesThatWriteFields.modules.span().subspan(first[key.index()], first[key.index() + 1] - first[key.index()]);
        auto found = std::ranges::lower_bound(modules, module.index());
        return found != modules.end() && *found == module.index() ? m_modulesThatWriteFields.inlineFields[first[key.index()] + (found - modules.begin())] : InlineFieldsOfModule::None;
    }

    unsigned numberOfLayouts() const { return m_parts.layouts.size(); }
    unsigned numberOfVariables() const { return m_parts.variables.size(); }

private:
    SlotPlanParts m_parts;
    ModulesThatWriteFields m_modulesThatWriteFields;
    PlannedLayout m_unplannedLayout;
    PlacedSlot m_unplannedSlot;
    ClassCreationPlan m_unplannedClass;
    std::unique_ptr<FieldAccessesOfSets> m_fieldAccessesOfSets;
    Vector<uint64_t> m_layoutsInUseThatHoldOtherThanCompiledObjects;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
