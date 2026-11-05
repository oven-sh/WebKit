/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"
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

enum class SlotStorage : uint8_t {
    // No maker stores the field: AllocationEmitter::collectInitialFields() and the table of data pass over it, and its value is lost.
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
    // FieldData::type.
    TypeID typeInLayoutRecord() const { return typeOfRawValue ? typeOfRawValue : type; }
    bool hasLocationOf(const PlacedSlot& other) const { return storage == other.storage && offset == other.offset && representation == other.representation; }

    // Milestone one: JSValue in every slot of a field. All that follows but storage, offset and indexInLayoutRecord stays as it stands here.
    SlotRepresentation representation { SlotRepresentation::JSValue };
    SlotStorage storage { SlotStorage::None };
    bool mayExistWithoutStorage { false };
    // Of a slot out of line: the layout lists the field and no maker fills it (mayLieOutOfLineThoughListedInSlotPlan()), so most objects have no granule for it, and many no Extension. It decides the form of a read and nothing else.
    bool isLeftOutByMakers { false };
    // What lies there is the pair of the functions of an accessor, and no value: Field::form of the declaration or FieldFact::IsAccessor says so. Of several members: of some.
    bool holdsAccessor { false };
    bool isFilledAtBirth { false };
    bool deferredClosureCapturesObject { false };
    bool deferredClosureCapturesEnvironment { false };
    uint32_t offset { 0 };
    // The place of the field's FieldData in the record of the layout. The fields that have one are numbered in the order of their indices.
    // Milestone one: every field of a layout with LayoutStorage::CompiledObject has one, with storage or, under isRecordAlone, without, so that PrivateGet, PrivateSet, PrivateDefine and PrivateHas find theirs.
    // BUT FOR A FIELD THAT A TYPE ALONE SPELLS (planLayoutForSlotPlan()): the record would hold its name. SlotPlan::findField() does not find it: whoever comes with the name finds what the run time finds.
    // No field of an environment has one, and neither hidden word.
    std::optional<uint16_t> indexInLayoutRecord;
    FunctionID deferredFunction;
    // Null: anything.
    TypeID type;
    TypeID typeOfRawValue;
};

struct ReservedBytes {
    uint32_t offset { 0 };
    uint32_t width { 0 };
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

    // Milestone one: CompiledObject for Record, Instance, Container and BuiltinResult, Environment for Environment and CoroutineFrame, OrdinaryObject for a Record with Layout::holdsOrdinaryObjects.
    // None for Closure and ModuleNamespace, for a layout that is never allocated and for one with Layout::writtenBuiltinBase, whose instances are cells of the built-in class.
    LayoutStorage storage { LayoutStorage::None };
    // Milestone one: a layout of the last two kinds that declares a private name is a CompiledObject all the same. Its fields have a place in the record and no storage.
    bool isRecordAlone { false };
    // Milestone one: false.
    bool liesOnlyInFrames { false };
    // How many slots have indexInLayoutRecord.
    uint16_t numberOfRecordFields { 0 };
    // Of the cell, a multiple of 8.
    uint32_t size { 0 };
    // Behind the last slot out of line. 0: the layout has none.
    uint32_t extensionSize { 0 };
    // Milestone one: 0.
    uint64_t granulesOfFirstExtension { 0 };
    // Milestone one: none.
    std::optional<uint32_t> isEscapedOffset;
    // Milestone one: none.
    Vector<ReservedBytes> bytesKeptZero;
    // The fields that the layout declares, the fields that Link appended (appendedKeys), the word of the Extension, the word of the extra properties.
    // Milestone one: the fields in the cell, 8 bytes each, in the order of declaration, and the cell is as large as they need.
    // That order is of the whole row of classes: what the written base has, at the base's offsets, a hiding field in the slot of what it hides, then the layout's own fields by Layout::declaredOrder, then those that it does not list.
    // Every field of a layout that holdsCompiledObjects() and of an environment has storage. SlotStorage::None occurs under isRecordAlone alone.
    // What the records of the image cannot say, planProgram() reports as InternalErrorKind::InvalidLayout: a CompiledObject with more than maxFieldsInOrder fields (LimitID::FieldsInKeyOrder), an offset above SlotData::maxOffset (LimitID::MaxFieldOffset).
    // An object has the word of the extra properties, in the form hiddenReferenceRepresentation(). An environment has neither hidden word.
    // AN APPENDED FIELD HAS A SLOT OF ITS OWN IN EACH LAYOUT THAT HAS IT, and no allocation fills it. It lies in the cell, behind all that is declared, where the written type of `this` requires the name: whoever has such an object made fills it.
    // Every other one lies out of line, a granule each, so an object pays for those that it gets. A layout with such slots has the word of the Extension at offsetOfFirstField, where the stubs look for it.
    // The word of the extra properties then lies in the four bytes behind it, and with wide references behind the last slot of the cell.
    Vector<PlacedSlot> slots;
    // LinkTables::appendedKeysOf(). The field of the key at `place` has the index numberOfFields() - appendedKeys.size() + place.
    Vector<PropertyKeyID> appendedKeys;
    // By the hash of the key: its place in appendedKeys and one more. Empty: few enough to look through.
    Vector<uint16_t> placesOfAppendedKeys;
    void indexAppendedKeys();
    // notFound: none.
    size_t findAppendedKey(PropertyKeyID) const;
    // Milestone one: none.
    std::optional<FieldIndex> environmentOfDeferredClosures;
    // Milestone one: none.
    BitVector fieldsThatHoldRawValuesFromBirth;
    // Some slot is isFilledAtBirth. An object of any other layout never bears the mark: Object::mayBearMark().
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

// What one operation on a slot does where every receiver has one of the given layouts.
struct FieldAccess {
    enum class Coverage : uint8_t {
        NoMember,
        SomeMembers,
        // Of a read alone: some members lack the field, and a load at the location finds an empty slot in their objects.
        EveryMemberHasSlot,
        EveryMember,
    };

    // False: the receivers agree on no one location and differ in more than the offset.
    bool hasLocation { false };
    Coverage coverage { Coverage::NoMember };
    bool mayBeFilledWithoutStorage { false };
    // The slot has the lowest of the offsets, and distancesByShape() says how far behind it the slot of an object lies.
    bool findsOffsetByShape { false };
    PropertyKeyID key;
    PlacedSlot slot;
    PlacedSlot extensionWord;
    // Ascending.
    Vector<LayoutID, 4> membersWithField;
};

enum class StoredFieldForm : uint8_t {
    Any,
    // The slot lies in the cell, holds any value as it is, and takes any value.
    TakesAnyValue,
    // The slot lies in the cell and holds a double. It takes every number, and beside numbers at most undefined and null.
    AcceptsAnyNumber,
    // The same of a slot that holds any value as it is.
    KeepsNumbersAsValues,
};

enum class VariableStorage : uint8_t {
    Statics,
    ClosureTable,
};

enum class VariableEncoding : uint8_t {
    Plain,
    UndefinedAsZero,
    FunctionAsZero,
};

struct PlannedVariable {
    bool hasWordInStatics() const { return storage == VariableStorage::Statics; }

    VariableStorage storage { VariableStorage::Statics };
    VariableEncoding encoding { VariableEncoding::Plain };
    SlotRepresentation representation { SlotRepresentation::JSValue };
    bool isDescribedToRunTime { false };
    // With FunctionAsZero: what the first read makes a closure of.
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
    // By member of the class, in the order of Facts::membersOf().
    Vector<MemberCreation> members;
};

// The whole content of the cell's row in the image's table of allocations: AOTNumbering.h, NumberSpace::Allocation.
struct PlacedAllocation {
    LayoutID layout;
    uint32_t shape { 0 };
    uint32_t size { 0 };
    uint32_t orderWord { 0 };
    // Of an object whose declaredLayoutOf() is of the kind Instance: the constructor of that layout, by which the run time finds the class. Null for every other cell.
    FunctionID constructedBy;
    uint16_t numberOfCaptures { 0 };
    // Some slot with isFilledAtBirth is not among the initial fields. Milestone one: false.
    bool isBornUnfinished { false };
};

struct FieldTableProbePlan {
    bool readsTestOutOfLineValuesSecond { false };
    bool readsTestDoublesSecond { false };
};

// By key: in how many reached functions an operation spells the name, once for reads, once for calls, once for stores.
struct MentionsOfKeys {
    bool isSpelled(PropertyKeyID key) const { return key.index() >= all.size() || all[key.index()] || isSpelledAsText.get(key.index()); }

    Vector<uint32_t> all;
    // ThinSummaries::textsSpelledThatNameFieldsNeverWritten(), by key. It counts for no column: no operation looks the name up.
    BitVector isSpelledAsText;
    // A lookup that runs once gains next to nothing from a column.
    Vector<uint32_t> thatMayRunAgain;
};

struct SlotPlanParts {
    // By layout. Milestone one: see PlannedLayout.
    Vector<PlannedLayout> layouts;
    // By variable. Milestone one: the variables of the program and no other, each as PlannedVariable stands but described to the run time.
    // The first word is empty for Uninitialized and undefined for Undefined. One that is born as a function: FunctionAsZero of it, and empty.
    Vector<PlannedVariable> variables;
    // By function: chooseClosureForms() of AOTFunctionKinds.cpp. Full or Slim. Empty says Full of every function.
    Vector<ClosureForm> closureForms;
    // By layout: classCreationOfPlan() of AOTPlan.cpp. Empty, as it is with TypedOperations withheld: every member is created at its definition, and no guard holds while its bit is clear.
    Vector<ClassCreationPlan> classCreation;
    // The layouts with LayoutFlags::IsNeverAllocated, the members of Program::taggedFamilies, which no body names, and those of which Link says that reached code allocates none (LinkedLayout::isAllocated).
    BitVector layoutsNeverAllocated;
    // AOTNamePlan.cpp. The names that reached code spells, as many as there are columns, those first that are spelled where code may run again. An access under any other name calls the stub that asks the number of the name.
    BitVector keysWithColumn;
    // AOTClosuresOfLiterals.cpp. The names under which some layout has a slot that holds a mark for a closure: every typed read asks, and nearly every one is told no by this bit.
    BitVector keysOfSlotsThatHoldMarks;
    // AOTNamePlan.cpp.
    FieldTableProbePlan fieldTableProbes;
    // AOTNamePlan.cpp. By key: how most of the layouts that have the name hold it, where that is a slot in the cell that holds no boxed value and may hold the mark of an absent field. JSValue of every other name.
    Vector<SlotRepresentation> narrowFormsOfKeys;
};

// SlotPlan::fieldAccess() of the sets of the program's table that hold several layouts: each answer is kept where it is first asked for, by whichever thread.
struct FieldAccessesOfSets {
    struct Part {
        Lock lock;
        // By the set and the key: of reads, of stores.
        std::array<HashMap<uint64_t, std::unique_ptr<FieldAccess>>, 2> accesses;
        // Of a set that lies in a thread, which has no number that lasts: by a hash of its members and the key. Whoever finds an entry holds the members against those that he asks about.
        struct OfMembers {
            PropertyKeyID key;
            Vector<LayoutID> members;
            FieldAccess access;
        };
        std::array<HashMap<uint64_t, Vector<std::unique_ptr<OfMembers>, 1>>, 2> accessesOfMembers;
    };
    std::array<Part, 64> parts;
};

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
    // Among the declared fields, then among the appended ones.
    JS_EXPORT_PRIVATE std::optional<FieldIndex> findField(const Program&, LayoutID, PropertyKeyID) const;
    JS_EXPORT_PRIVATE PropertyKeyID keyOf(const Program&, FieldReference) const;
    const PlacedSlot& extensionWord(LayoutID id) const { return slot(id, layout(id).extensionWordIndex()); }
    const PlacedSlot& extraPropertiesWord(LayoutID id) const { return slot(id, layout(id).extraPropertiesWordIndex()); }

    // A function of the plan, the key and the set alone. A layout that is never allocated counts for nothing.
    JS_EXPORT_PRIVATE FieldAccess fieldAccess(const Program&, PropertyKeyID, std::span<const LayoutID> receivers, bool isStore) const;
    JS_EXPORT_PRIVATE FieldAccess fieldAccessOfOneLayout(const Program&, PropertyKeyID, LayoutID receiver) const;
    // Of the members of the set, with or without those that are never allocated. A set may hold thousands of layouts, and every access by name to one of their objects asks.
    JS_EXPORT_PRIVATE FieldAccess fieldAccess(const Program&, PropertyKeyID, LayoutSetID, std::span<const LayoutID> members, bool isStore) const;
    // A store that finds the slot empty lists the key in the order word.
    bool storeMayListKey(const FieldAccess& access) const
    {
        return canLeaveSlot(access.slot.representation) && !access.membersWithField.isEmpty();
    }
    // How far behind the LayoutData of the receiver's layout the FieldData of the field lies. None: some layout has no plain field of the name with storage, or the places differ.
    JS_EXPORT_PRIVATE std::optional<uint32_t> recordOfStoredField(const Program&, PropertyKeyID, std::span<const LayoutID> receivers) const;
    // Of a store for which recordOfStoredField() has an answer.
    JS_EXPORT_PRIVATE StoredFieldForm formOfStoredField(const Program&, PropertyKeyID, std::span<const LayoutID> receivers) const;

    PlannedVariable variable(VariableID id) const { return id.index() < m_parts.variables.size() ? m_parts.variables[id.index()] : PlannedVariable { }; }
    ClosureForm closureFormOf(FunctionID id) const { return id.index() < m_parts.closureForms.size() ? m_parts.closureForms[id.index()] : ClosureForm::Full; }
    const ClassCreationPlan& classCreation(LayoutID id) const { return id.index() < m_parts.classCreation.size() ? m_parts.classCreation[id.index()] : m_unplannedClass; }

    // Image gives each of these names a number from 1 to RowDisplacementTable::maxColumn.
    bool hasColumn(PropertyKeyID key) const { return m_parts.keysWithColumn.get(key.index()); }
    bool someSlotUnderKeyHoldsMark(PropertyKeyID key) const { return m_parts.keysOfSlotsThatHoldMarks.get(key.index()); }
    const BitVector& keysWithColumn() const { return m_parts.keysWithColumn; }
    const FieldTableProbePlan& fieldTableProbes() const { return m_parts.fieldTableProbes; }
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
