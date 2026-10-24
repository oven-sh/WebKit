/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFormsExpectedOfNames.h"
#include "AOTIR.h"
#include "AOTSlotRepresentation.h"
#include "AOTVector.h"
#include "JSExportMacros.h"
#include <compare>
#include <memory>
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/FastMalloc.h>
#include <wtf/PrintStream.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class CallGraph;
class FlowResults;
class FrameAllocation;
class Heat;
class SlotAgreementMemo;
struct SlotPlanParts;

using FieldIndex = uint32_t;

inline constexpr unsigned numberOfHiddenWords = 2;

enum class LayoutStorage : uint8_t {
    None,
    CompiledObject,
    OrdinaryObject,
    Environment,
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
    // A store sees that it creates the field only where the slot can be absent.
    bool canBeCreatedByStore() const { return hasStorage() && canLeaveSlot(representation); }
    uint32_t width() const { return widthOf(representation); }
    uint32_t end() const { return offset + width(); }
    SlotData data() const { return SlotData::create(offset, representation); }

    bool hasLocationOf(const PlacedSlot& other) const
    {
        return storage == other.storage && offset == other.offset && representation == other.representation;
    }

    SlotRepresentation representation { SlotRepresentation::JSValue };
    SlotStorage storage { SlotStorage::None };
    // The key order may list the field while the Extension has no granule for it. Then the field holds undefined.
    bool mayExistWithoutStorage { false };
    bool isFilledByConstructor { false };
    bool deferredClosureCapturesObject { false };
    bool deferredClosureCapturesEnvironment { false };
    // What the slot leaves unmade is no closure: it is the object whose fields lie in this object.
    uint32_t offset { 0 };
    std::optional<uint16_t> indexInLayoutRecord;
    FunctionID deferredFunction;
};

struct ReservedBytes {
    uint32_t offset { 0 };
    uint32_t width { 0 };

    friend auto operator<=>(const ReservedBytes&, const ReservedBytes&) = default;
};

// An object that lies in the cell of another, behind the fields of that one, with a header of its own.
struct PlacedInnerObject {
    FieldIndex field { 0 };
    // None for an array: its cell, and behind that its first storage.
    LayoutID layout;
    uint32_t arrayCapacity { 0 };
    uint32_t offset { 0 };
    // Every object of the outer layout has it, and the field holds nothing else: a read of the field is an addition.
    bool isAlwaysInPlace { false };
};

struct PlannedLayout {
    bool hasSlots() const { return storage == LayoutStorage::CompiledObject || storage == LayoutStorage::Environment; }
    unsigned numberOfFields() const { return slots.isEmpty() ? 0 : slots.size() - numberOfHiddenWords; }
    FieldIndex extensionWordIndex() const { return numberOfFields(); }
    FieldIndex extraPropertiesWordIndex() const { return numberOfFields() + 1; }
    bool hasExtension() const { return !slots.isEmpty() && slots[extensionWordIndex()].hasStorage(); }
    bool hasExtraPropertiesWord() const { return !slots.isEmpty() && slots[extraPropertiesWordIndex()].hasStorage(); }
    JS_EXPORT_PRIVATE bool keepsZero(uint32_t offset, uint32_t width) const;
    const PlacedInnerObject* innerObjectIn(FieldIndex field) const
    {
        for (const PlacedInnerObject& inner : innerObjects) {
            if (inner.field == field)
                return &inner;
        }
        return nullptr;
    }

    LayoutStorage storage { LayoutStorage::None };
    bool holdsWideReferences { false };
    bool keysAreNeverEnumerated { false };
    bool mayBeInnerObject { false };
    uint16_t numberOfRecordFields { 0 };
    uint32_t size { 0 };
    uint32_t sizeWithInnerObjects { 0 };
    uint32_t extensionSize { 0 };
    Vector<PlacedSlot> slots;
    Vector<ReservedBytes> bytesKeptZero;
    Vector<PlacedInnerObject> innerObjects;
};

struct SlotGroup {
    PlacedSlot slot;
    PlacedSlot extensionWord;
    Vector<LayoutID, 4> members;
};

struct SlotAgreement {
    bool hasOneLocation() const { return groups.size() == 1; }
    bool coversEveryMember() const { return membersWithoutSlot.isEmpty(); }
    // An access finds the offset in a table by the shape of the object. A store needs a slot in every member.
    bool hasOffsetByShape(bool isStore) const { return differsInOffsetAlone && (!isStore || coversEveryMember()); }
    // All groups as one, with the slot that has the lowest offset.
    JS_EXPORT_PRIVATE SlotGroup groupOfAllOffsets() const;

    Vector<SlotGroup, 1> groups;
    Vector<LayoutID, 1> membersWithoutSlot;
    bool membersWithoutSlotKeepBytesZero { false };
    // There are several groups, and each keeps the field in the cell in the same representation.
    bool differsInOffsetAlone { false };
};

// The operations that take the offset of the slot from a table where their receivers differ in the offset alone. Any other gets an arm for each group.
inline constexpr OpcodeSet operationsWithOffsetByShape { Opcode::GetSlot, Opcode::GetSlotOrUndefined, Opcode::IsSlotFilled, Opcode::PutSlot, Opcode::PutSlotOrAdd };

enum class VariableStorage : uint8_t {
    Statics,
    None,
    ClosureTable,
};

enum class VariableEncoding : uint8_t {
    Plain,
    UndefinedAsZero,
    FunctionAsZero,
};

struct PlannedVariable {
    VariableStorage storage { VariableStorage::Statics };
    VariableEncoding encoding { VariableEncoding::Plain };
    SlotRepresentation representation { SlotRepresentation::JSValue };
    bool isUsedBeforeProgramEntry { false };
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
    Vector<MemberCreation> members;
};

struct SlotPlanInputs {
    const FlowResults& flow;
    const CallGraph& callGraph;
    const Heat& heat;
    const FrameAllocation& frameAllocation;
};

class SlotPlan {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(SlotPlan);
public:
    JS_EXPORT_PRIVATE SlotPlan();
    JS_EXPORT_PRIVATE explicit SlotPlan(SlotPlanParts&&);
    JS_EXPORT_PRIVATE SlotPlan(SlotPlan&&);
    JS_EXPORT_PRIVATE SlotPlan& operator=(SlotPlan&&);
    JS_EXPORT_PRIVATE ~SlotPlan();

    const PlannedLayout& layout(LayoutID id) const { return id.index() < m_layouts.size() ? m_layouts[id.index()] : m_unplannedLayout; }
    LayoutStorage storageOf(LayoutID id) const { return layout(id).storage; }

    const PlacedSlot& slot(LayoutID id, FieldIndex field) const
    {
        const PlannedLayout& planned = layout(id);
        return field < planned.slots.size() ? planned.slots[field] : m_unplannedSlot;
    }
    const PlacedSlot& slot(FieldReference field) const { return slot(field.layout, field.field); }
    const PlacedSlot& extensionWord(LayoutID id) const { return slot(id, layout(id).extensionWordIndex()); }
    const PlacedSlot& extraPropertiesWord(LayoutID id) const { return slot(id, layout(id).extraPropertiesWordIndex()); }

    JS_EXPORT_PRIVATE SlotAgreement agreementOf(const Program&, PropertyKeyID, std::span<const LayoutID> members) const;
    // Computed once for a key and a set, whichever thread asks first. The answer lives as long as the plan.
    JS_EXPORT_PRIVATE const SlotAgreement& agreementOf(const Program&, PropertyKeyID, LayoutSetID receivers) const LIFETIME_BOUND;

    PlannedVariable variable(VariableID id) const { return id.index() < m_variables.size() ? m_variables[id.index()] : PlannedVariable { }; }

    ClosureForm closureFormOf(FunctionID id) const { return id.index() < m_closureForms.size() ? m_closureForms[id.index()] : ClosureForm::Full; }

    const ClassCreationPlan& classCreation(LayoutID id) const { return id.index() < m_classCreation.size() ? m_classCreation[id.index()] : m_unplannedClass; }
    JS_EXPORT_PRIVATE MemberCreation creationOf(const Program&, LayoutID classLayout, FunctionID member) const;

    bool hasColumn(PropertyKeyID key) const { return m_keysWithColumn.get(key.index()); }
    const BitVector& keysWithColumn() const { return m_keysWithColumn; }

    const FormsExpectedOfNames& formsExpectedOfNames() const { return m_formsExpectedOfNames; }

    unsigned numberOfLayouts() const { return m_layouts.size(); }
    unsigned numberOfVariables() const { return m_variables.size(); }

    JS_EXPORT_PRIVATE void dump(PrintStream&, const Program&) const;

private:
    Vector<PlannedLayout> m_layouts;
    Vector<PlannedVariable> m_variables;
    Vector<ClosureForm> m_closureForms;
    Vector<ClassCreationPlan> m_classCreation;
    BitVector m_keysWithColumn;
    FormsExpectedOfNames m_formsExpectedOfNames;
    PlannedLayout m_unplannedLayout;
    PlacedSlot m_unplannedSlot;
    ClassCreationPlan m_unplannedClass;
    SlotAgreement m_agreementOfNoReceivers;
    std::unique_ptr<SlotAgreementMemo> m_agreements;
};

JS_EXPORT_PRIVATE SlotPlan planSlots(const Program&, const SlotPlanInputs&, BuildContext&);

// The pass that pairs allocations runs before the plan, which may find that the two objects do not fit one cell.
inline bool takesCellWithInnerObjects(const SlotPlan& plan, const Value& newObject)
{
    return newObject.takesCellOfInnerObject() && !plan.layout(newObject.layout()).innerObjects.isEmpty();
}

JS_EXPORT_PRIVATE BitVector findVariablesDescribedToRunTime(const Program&, const FlowResults&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
