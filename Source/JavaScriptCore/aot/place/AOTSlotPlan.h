/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include "AOTSlotRepresentation.h"
#include "JSExportMacros.h"
#include <compare>
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/FastMalloc.h>
#include <wtf/PrintStream.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class CallGraph;
class EnvironmentReuse;
class FlowResults;
class FrameAllocation;
class Heat;
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
    bool needsExtension() const { return isOutOfLine() || doubleOffsetInExtension; }
    // A store sees that it creates the field only where the slot can be absent.
    bool canBeCreatedByStore() const { return hasStorage() && canLeaveSlot(representation); }
    uint32_t width() const { return widthOf(representation); }
    uint32_t end() const { return offset + width(); }
    SlotData data() const { return SlotData::create(offset, representation); }

    bool hasLocationOf(const PlacedSlot& other) const
    {
        return storage == other.storage && offset == other.offset && representation == other.representation && doubleOffsetInExtension == other.doubleOffsetInExtension;
    }

    SlotRepresentation representation { SlotRepresentation::JSValue };
    SlotStorage storage { SlotStorage::None };
    bool mayExistWithoutStorage { false };
    bool deferredClosureCapturesObject { false };
    uint32_t offset { 0 };
    std::optional<uint32_t> doubleOffsetInExtension;
    std::optional<uint16_t> indexInLayoutRecord;
    FunctionID deferredFunction;
};

struct ReservedBytes {
    uint32_t offset { 0 };
    uint32_t width { 0 };

    friend auto operator<=>(const ReservedBytes&, const ReservedBytes&) = default;
};

struct PlannedLayout {
    bool hasSlots() const { return storage == LayoutStorage::CompiledObject || storage == LayoutStorage::Environment; }
    unsigned numberOfFields() const { return slots.isEmpty() ? 0 : slots.size() - numberOfHiddenWords; }
    FieldIndex extensionWordIndex() const { return numberOfFields(); }
    FieldIndex extraPropertiesWordIndex() const { return numberOfFields() + 1; }
    bool hasExtension() const { return !slots.isEmpty() && slots[extensionWordIndex()].hasStorage(); }
    bool hasExtraPropertiesWord() const { return !slots.isEmpty() && slots[extraPropertiesWordIndex()].hasStorage(); }
    JS_EXPORT_PRIVATE bool keepsZero(uint32_t offset, uint32_t width) const;

    LayoutStorage storage { LayoutStorage::None };
    bool holdsWideReferences { false };
    bool keysAreNeverEnumerated { false };
    uint16_t numberOfRecordFields { 0 };
    uint32_t size { 0 };
    uint32_t extensionSize { 0 };
    std::optional<uint32_t> isEscapedOffset;
    Vector<PlacedSlot> slots;
    Vector<ReservedBytes> bytesKeptZero;
};

struct SlotGroup {
    PlacedSlot slot;
    PlacedSlot extensionWord;
    Vector<LayoutID, 4> members;
};

struct SlotAgreement {
    bool hasOneLocation() const { return groups.size() == 1; }
    bool coversEveryMember() const { return membersWithoutSlot.isEmpty(); }

    Vector<SlotGroup, 1> groups;
    Vector<LayoutID, 1> membersWithoutSlot;
    bool membersWithoutSlotKeepBytesZero { false };
};

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
    Vector<MemberCreation> members;
};

struct SlotPlanInputs {
    const FlowResults& flow;
    const CallGraph& callGraph;
    const Heat& heat;
    const FrameAllocation& frameAllocation;
    const EnvironmentReuse& environmentReuse;
};

class SlotPlan {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(SlotPlan);
public:
    SlotPlan() = default;
    JS_EXPORT_PRIVATE explicit SlotPlan(SlotPlanParts&&);

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
    JS_EXPORT_PRIVATE SlotAgreement agreementOf(const Program&, PropertyKeyID, LayoutSetID receivers) const;

    PlannedVariable variable(VariableID id) const { return id.index() < m_variables.size() ? m_variables[id.index()] : PlannedVariable { }; }

    ClosureForm closureFormOf(FunctionID id) const { return id.index() < m_closureForms.size() ? m_closureForms[id.index()] : ClosureForm::Full; }

    const ClassCreationPlan& classCreation(LayoutID id) const { return id.index() < m_classCreation.size() ? m_classCreation[id.index()] : m_unplannedClass; }
    JS_EXPORT_PRIVATE MemberCreation creationOf(const Program&, LayoutID classLayout, FunctionID member) const;

    bool hasColumn(PropertyKeyID key) const { return m_keysWithColumn.get(key.index()); }
    const BitVector& keysWithColumn() const { return m_keysWithColumn; }

    unsigned numberOfLayouts() const { return m_layouts.size(); }
    unsigned numberOfVariables() const { return m_variables.size(); }

    JS_EXPORT_PRIVATE void dump(PrintStream&, const Program&) const;

private:
    Vector<PlannedLayout> m_layouts;
    Vector<PlannedVariable> m_variables;
    Vector<ClosureForm> m_closureForms;
    Vector<ClassCreationPlan> m_classCreation;
    BitVector m_keysWithColumn;
    PlannedLayout m_unplannedLayout;
    PlacedSlot m_unplannedSlot;
    ClassCreationPlan m_unplannedClass;
};

JS_EXPORT_PRIVATE SlotPlan planSlots(const Program&, const SlotPlanInputs&, BuildContext&);

JS_EXPORT_PRIVATE BitVector findVariablesDescribedToRunTime(const Program&, const FlowResults&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
