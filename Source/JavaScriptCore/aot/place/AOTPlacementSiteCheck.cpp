/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPlacementCheck.h"

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTFlowSiteData.h"
#include "AOTFunctionAnalyses.h"
#include "AOTGuardFactDetails.h"
#include "AOTNarrowFieldTypes.h"
#include "AOTPlacementSteps.h"
#include "AOTRegExpFlags.h"
#include "AOTRegExpRecord.h"
#include "AOTRegExpRequests.h"
#include "AOTSchemaWalkers.h"
#include <algorithm>
#include <optional>
#include <tuple>

namespace JSC { namespace AOT { namespace Typed {

static_assert(Value::opcodesWithAllocation == OpcodeSet { Opcode::NewClosure, Opcode::NewObject, Opcode::MaterializeEnvironment });
static_assert(Value::opcodesWithGuardBit == OpcodeSet { Opcode::AreMembersUnchanged });
static_assert(Value::opcodesWithStaticsOffset == OpcodeSet { Opcode::NewArray, Opcode::NewRegExp, Opcode::SharedRegExpLiteral, Opcode::NewTemplateObject });
static_assert(Value::opcodesWithRequest == OpcodeSet { Opcode::TryPutByIds, Opcode::NewClass, Opcode::CloneAs, Opcode::NewTemplateObject, Opcode::DirectEval, Opcode::Data, Opcode::NewSetFromData, Opcode::NewMapFromData, Opcode::CopyDataProperties, Opcode::CopyObject });

namespace {

enum class SiteCellKind : uint8_t {
    ArrayProfile,
    RegExp,
    TemplateObject,
};

struct SiteCellUse {
    uint32_t offset { 0 };
    SiteCellKind kind { SiteCellKind::ArrayProfile };
    SourceSite site;
    SiteKey user;
    RegExpRecordID regExpRecord;
};

struct SiteCacheUse {
    SiteCacheID cache;
    SiteWeight weight { 0 };
    SiteKey user;
};

struct EntriesWithPlacedFields {
    OpcodeSet opcodes;
    TerminatorKindSet terminators;
};

} // anonymous namespace

static bool hasPlacedField(std::span<const PayloadFieldInfo> fields)
{
    return std::ranges::any_of(fields, [](const PayloadFieldInfo& field) {
        return field.firstForm == IRForm::Placed;
    });
}

static EntriesWithPlacedFields findEntriesWithPlacedFields()
{
    EntriesWithPlacedFields entries;
    for (Opcode opcode : allMembersOf<Opcode>) {
        if (hasPlacedField(payloadFieldsOf(opcode)))
            entries.opcodes.add(opcode);
    }
    for (TerminatorKind kind : allMembersOf<TerminatorKind>) {
        if (hasPlacedField(payloadFieldsOf(kind)))
            entries.terminators.add(kind);
    }
    return entries;
}

static SiteCellKind siteCellKindOf(Opcode opcode)
{
    switch (opcode) {
    case Opcode::NewArray:
        return SiteCellKind::ArrayProfile;
    case Opcode::NewRegExp:
    case Opcode::SharedRegExpLiteral:
        return SiteCellKind::RegExp;
    case Opcode::NewTemplateObject:
        return SiteCellKind::TemplateObject;
    default:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// A slot keeps its markers whatever the type of its field says, so undefined and null stay in it and are no values of another type.
static bool slotAdmits(const Program& program, SlotRepresentation representation, const Type& contents, const Type& stored)
{
    SlotTraits traits = traitsOf(representation);
    uint16_t markers = 0;
    if (traits.hasUndefined)
        markers |= Primitive::Undefined;
    if (traits.hasNull)
        markers |= Primitive::Null;
    if (!(stored.primitives & markers & ~contents.primitives))
        return stored.isSubsetOf(contents, program);
    return stored.isSubsetOf(contents.withPrimitives(markers), program);
}

static bool isRoot(const StaticsPlan& statics, uint32_t offset)
{
    return std::ranges::binary_search(statics.roots, offset);
}

static bool isClearedEachCollection(const StaticsPlan& statics, uint64_t offset)
{
    return std::ranges::any_of(statics.clearedEachCollection, [&](const StaticsRange& range) {
        return offset >= range.offset && offset + sizeof(uint64_t) <= static_cast<uint64_t>(range.offset) + range.size;
    });
}

namespace {

class SiteChecker {
public:
    SiteChecker(const PlacementCheckContext& context, const EntriesWithPlacedFields& placedFields, FunctionID id, Vector<SiteCellUse>& siteCells)
        : m_context(context)
        , m_placedFields(placedFields)
        , m_program(context.program)
        , m_placement(context.placement)
        , m_slots(context.inputs.slots)
        , m_id(id)
        , m_function(context.program.at(id))
        , m_siteCells(siteCells)
    {
    }

    void run()
    {
        for (const Block& block : m_function.blocks) {
            for (ValueID value : block.values)
                checkSite(value);
            checkTerminator(block.terminator);
        }
        std::ranges::sort(m_closuresStoredInDeferredSlots);
        for (ValueID closure : m_deferredClosures) {
            if (!std::ranges::binary_search(m_closuresStoredInDeferredSlots, closure))
                fail(closure, PlacementRule::DeferredClosureStore);
        }
    }

private:
    void fail(ValueID id, PlacementRule rule) { m_context.failures.fail(rule, { }, m_id, id); }

    FunctionAnalyses& analyses()
    {
        if (!m_analyses)
            m_analyses.emplace(AnalysisContext { m_context.build, m_program, &m_context.inputs.flow, nullptr, m_id }, m_function);
        return *m_analyses;
    }

    const UseLists& useLists() { return analyses().useLists(); }

    const FieldAccess* accessAt(FieldAccessID id) const
    {
        return id && id.index() < m_placement.numberOfFieldAccesses() ? &m_placement.fieldAccess(id) : nullptr;
    }

    void checkSite(ValueID id)
    {
        const Value& value = m_function.at(id);
        if (m_placedFields.opcodes.contains(value.opcode)) {
            for (const PayloadFieldInfo& field : payloadFieldsOf(value.opcode)) {
                if (field.firstForm == IRForm::Placed)
                    checkPlacedField(id, value, field.kind);
            }
        }
        if (Value::opcodesWithPlacedKeys.contains(value.opcode))
            checkPlacedKeys(id, value);
        if (Value::opcodesWithFieldAccess.contains(value.opcode) && isSlotStore(value.opcode))
            checkAccessStore(id, value.fieldAccess(), value.operand(*storedOf(value.opcode)));
        if (value.opcode == Opcode::NewObject)
            checkInitialValues(id, value);
        if (value.opcode == Opcode::GetSlot)
            checkReadOfFilledSlot(id, value);
        if (value.opcode == Opcode::PutSlot)
            checkStoreWithoutAllocation(id, value);
        if (value.opcode == Opcode::PrivateDefine)
            checkDefinitionByName(id, value);
        if (value.opcode == Opcode::NewClosure)
            checkClosureForm(id, value);
        if (value.opcode == Opcode::GetEncodedModuleVariable && m_placement.variable(value.variable()).encoding != VariableEncoding::UndefinedAsZero)
            fail(id, PlacementRule::EncodingNotAllowed);
    }

    void checkTerminator(const Terminator& terminator)
    {
        // No rule below knows a field of a terminator that placement fills.
        if (m_placedFields.terminators.contains(terminator.kind))
            fail({ }, PlacementRule::SiteRefersElsewhere);
        forEachVariable(terminator, [&](VariableID variable) {
            checkVariable({ }, variable, false);
        });
    }

    void checkPlacedField(ValueID id, const Value& value, PayloadKind kind)
    {
        switch (kind) {
        case PayloadKind::FieldAccess:
            checkFieldAccess(id, value.fieldAccess(), { value.key(), value.receivers(), isSlotStore(value.opcode) });
            return;
        case PayloadKind::Allocation:
            checkAllocation(id, value);
            return;
        case PayloadKind::RegExpRecord:
            checkRegExpRecord(id, value);
            return;
        case PayloadKind::Request:
            checkRequest(id, value);
            return;
        case PayloadKind::StaticsOffset:
            checkStaticsOffset(id, value);
            return;
        case PayloadKind::OrderWord:
            checkOrderWord(id, value);
            return;
        case PayloadKind::GuardBit:
            checkGuardBit(id, value);
            return;
        case PayloadKind::GlobalBinding:
            checkGlobalBinding(id, value);
            return;
        case PayloadKind::SiteCache:
            return;
        case PayloadKind::String:
        case PayloadKind::PropertyKey:
        case PayloadKind::Function:
        case PayloadKind::Layout:
        case PayloadKind::Field:
        case PayloadKind::Variable:
        case PayloadKind::Module:
        case PayloadKind::ModuleFailure:
        case PayloadKind::Type:
        case PayloadKind::TypeSite:
        case PayloadKind::EvalScope:
        case PayloadKind::Data:
        case PayloadKind::ExpectedResult:
        case PayloadKind::Builtin:
        case PayloadKind::Enum:
        case PayloadKind::Flags:
        case PayloadKind::Bool:
        case PayloadKind::UInt32:
        case PayloadKind::Int64:
        case PayloadKind::Double:
        case PayloadKind::External:
        case PayloadKind::Record:
        case PayloadKind::Variant:
        case PayloadKind::List:
        case PayloadKind::LayoutSet:
        case PayloadKind::CalleeSet:
        case PayloadKind::AccessSite:
        case PayloadKind::AllocationSite:
        case PayloadKind::AllocationClass:
        case PayloadKind::Origin:
        case PayloadKind::Opcode:
            fail(id, PlacementRule::SiteRefersElsewhere);
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    void checkFieldAccess(ValueID id, FieldAccessID accessID, const SlotAccessKey& key)
    {
        if (!accessID) {
            fail(id, PlacementRule::PayloadMissing);
            return;
        }
        const FieldAccess* access = accessAt(accessID);
        if (!access || SlotAccessKey { access->key, access->receivers, access->isStore } != key)
            fail(id, PlacementRule::SiteRefersElsewhere);
    }

    void checkPlacedKeys(ValueID id, const Value& value)
    {
        PayloadListView<PlacedKey> placedKeys = value.placedKeys();
        std::span<const ValueID> values = value.valuesOperands();
        if (placedKeys.size() != values.size()) {
            fail(id, PlacementRule::SiteRefersElsewhere);
            return;
        }
        for (size_t index = 0; index < placedKeys.size(); ++index) {
            PlacedKey placedKey = placedKeys[index];
            checkFieldAccess(id, placedKey.fieldAccess, { placedKey.key, value.receivers(), true });
            checkAccessStore(id, placedKey.fieldAccess, values[index]);
        }
    }

    bool isAllocationOf(const PlacedAllocation& allocation, const Value& value) const
    {
        switch (value.opcode) {
        case Opcode::NewClosure:
            return allocation.layout == m_placement.closureLayout() && allocation.numberOfCaptures >= value.capturesOperands().size();
        case Opcode::NewObject:
            return allocation.layout == value.layout()
                && std::ranges::equal(allocation.initialFields, value.fields())
                && allocation.holdsInnerObjects == takesCellWithInnerObjects(m_placement.slots(), value)
                && allocation.shape == m_placement.shapes().shapeOf(value.layout(), value.allocationClass());
        case Opcode::MaterializeEnvironment:
            return allocation.layout == value.layout() && allocation.initialFields.isEmpty();
        default:
            return false;
        }
    }

    // A second cell would be a second function object with a prototype object of its own, and instances inherit from the one of the closure table.
    void checkClosureForm(ValueID id, const Value& closure)
    {
        ClosureForm form = closure.closureForm();
        if (form != ClosureForm::Singleton && m_placement.closureIdentityOf(closure.function()) == ClosureIdentity::TableCell)
            fail(id, PlacementRule::ClosureFormNotProven);
        if (form == ClosureForm::Deferred) {
            m_deferredClosures.append(id);
            return;
        }
        if ((form == ClosureForm::Singleton) != namesClosureOfTable(m_program, closure))
            fail(id, PlacementRule::ClosureFormNotProven);
    }

    void checkAllocation(ValueID id, const Value& value)
    {
        AllocationID allocation = value.allocation();
        bool allocatesByRecord = value.opcode != Opcode::NewClosure || value.closureForm() == ClosureForm::Full;
        if (!allocation) {
            if (allocatesByRecord)
                fail(id, PlacementRule::PayloadMissing);
            return;
        }
        if (!allocatesByRecord || allocation.index() >= m_placement.numberOfAllocations() || !isAllocationOf(m_placement.allocation(allocation), value))
            fail(id, PlacementRule::SiteRefersElsewhere);
    }

    void checkRegExpRecord(ValueID id, const Value& value)
    {
        const RegExpRequests& requests = m_context.inputs.regExpRequests;
        RegExpRecordID record = value.regExpRecord();
        if (!record) {
            fail(id, PlacementRule::PayloadMissing);
            return;
        }
        if (record.index() >= requests.all().size() || requests.at(record).pattern != value.pattern() || requests.at(record).flags != ImageRegExp::significantFlags(toYarrFlags(value.regExpFlags())))
            fail(id, PlacementRule::RegExpRecordWrong);
    }

    bool isCloneAsRequestOf(const CloneAsRecord& record, const Value& value) const
    {
        LayoutID layout = value.layout();
        ShapeRange shapes = m_placement.shapes().shapesOf(layout);
        if (record.layout != layout || record.shape < shapes.first || record.shape >= shapes.end())
            return false;
        return std::ranges::equal(record.fields, value.fields()) && std::ranges::all_of(record.fields, [&](FieldIndex field) {
            return m_slots.slot(layout, field).indexInLayoutRecord.has_value();
        });
    }

    bool isDataRequestOf(const DataRecord& record, const Value& value) const
    {
        if (record.data != value.data() || (record.sharedStorageOffset || record.node) != (value.encoding() == DataEncoding::CopyOnWriteArray))
            return false;
        if (record.node)
            return !record.sharedStorageOffset && isStorageOfArray(m_program.at(record.data).root.as<DataNode::Array>());
        return !record.sharedStorageOffset || isRoot(m_placement.statics(), *record.sharedStorageOffset);
    }

    bool isRequestOf(const RequestRecord& request, const Value& value) const
    {
        switch (value.opcode) {
        case Opcode::NewClass: {
            auto* record = std::get_if<NewClassRecord>(&request);
            return record && record->classLayout == value.layout();
        }
        case Opcode::CloneAs: {
            auto* record = std::get_if<CloneAsRecord>(&request);
            return record && isCloneAsRequestOf(*record, value);
        }
        case Opcode::NewTemplateObject: {
            auto* record = std::get_if<TemplateObjectRecord>(&request);
            return record && std::ranges::equal(record->parts, value.templateStrings());
        }
        case Opcode::DirectEval: {
            auto* record = std::get_if<DirectEvalRecord>(&request);
            return record && record->scope == value.evalScope() && record->module == m_program.sourceFunctionOf(m_function, value).module;
        }
        case Opcode::Data: {
            auto* record = std::get_if<DataRecord>(&request);
            return record && isDataRequestOf(*record, value);
        }
        case Opcode::NewSetFromData:
        case Opcode::NewMapFromData: {
            auto* record = std::get_if<CollectionDataRecord>(&request);
            return record && record->data == value.data() && record->hasValues == (value.opcode == Opcode::NewMapFromData);
        }
        default:
            return false;
        }
    }

    void checkRequest(ValueID id, const Value& value)
    {
        std::optional<Request> request = value.request();
        if (namesPropertyRun(value.opcode)) {
            std::span<const PlacedPropertyRun> runs = m_placement.propertyRuns().runs();
            if (request && (request->value() >= runs.size() || !std::ranges::equal(runs[request->value()].keys, keysOfPropertyRun(value))))
                fail(id, PlacementRule::PropertyRunWrong);
            return;
        }
        if (!request) {
            fail(id, PlacementRule::PayloadMissing);
            return;
        }
        const RequestRecord* record = m_placement.requests().recordAt(*request);
        if (!record || !isRequestOf(*record, value))
            fail(id, PlacementRule::RequestWrong);
    }

    bool isSiteCell(uint32_t offset, SiteCellKind kind) const
    {
        const StaticsPlan& statics = m_placement.statics();
        if (offset % sizeof(uint64_t) || offset >= statics.size)
            return false;
        switch (kind) {
        case SiteCellKind::ArrayProfile:
            return !isRoot(statics, offset) && isClearedEachCollection(statics, static_cast<uint64_t>(offset) + statics.distanceFromArrayProfileToLastArray);
        case SiteCellKind::RegExp:
        case SiteCellKind::TemplateObject:
            return isRoot(statics, offset);
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    void checkStaticsOffset(ValueID id, const Value& value)
    {
        SiteCellKind kind = siteCellKindOf(value.opcode);
        bool hasCell = kind != SiteCellKind::ArrayProfile || value.literalStorage() == ArrayLiteralStorage::Profiled;
        std::optional<StaticsOffset> offset = value.staticsOffset();
        if (!offset) {
            if (hasCell)
                fail(id, PlacementRule::PayloadMissing);
            return;
        }
        if (!hasCell || !isSiteCell(offset->value(), kind)) {
            fail(id, PlacementRule::SiteRefersElsewhere);
            return;
        }
        RegExpRecordID regExpRecord = kind == SiteCellKind::RegExp ? value.regExpRecord() : RegExpRecordID();
        m_siteCells.append({ offset->value(), kind, sourceSiteOf(m_program, m_id, value), { m_id, id }, regExpRecord });
    }

    void checkOrderWord(ValueID id, const Value& value)
    {
        std::optional<uint32_t> expected = m_placement.keyOrders().findOrderWord(value.layout(), value.fields());
        std::optional<OrderWord> word = value.orderWord();
        if (!word) {
            if (expected)
                fail(id, PlacementRule::PayloadMissing);
            return;
        }
        if (expected != word->value())
            fail(id, PlacementRule::SiteRefersElsewhere);
    }

    // One condition is tested by its own bit, several by the bit of their set. Without a bit the test fails.
    void checkGuardBit(ValueID id, const Value& value)
    {
        GuardBitID expected = m_placement.guardBits().find(value.conditions());
        GuardBitID bit = value.guardBit();
        if (!bit) {
            if (expected)
                fail(id, PlacementRule::PayloadMissing);
            return;
        }
        if (bit != expected)
            fail(id, PlacementRule::SiteRefersElsewhere);
    }

    // A name beyond the limit goes back to an access by name, so what is left has a record.
    void checkGlobalBinding(ValueID id, const Value& value)
    {
        if (!value.binding())
            fail(id, PlacementRule::PayloadMissing);
    }

    bool isUsedOnlyByStore(ValueID stored, ValueID closure)
    {
        const UseLists& uses = useLists();
        for (ValueID id = stored; id != closure; id = m_function.at(id).operand(0)) {
            if (uses.useCount(id) != 1)
                return false;
        }
        return uses.useCount(closure) == 1;
    }

    void checkStoredValue(ValueID site, ValueID stored, SlotRepresentation representation, const Type& contents, const Invocable<bool(FunctionID)> auto& isDeferredFunction)
    {
        if (!slotAdmits(m_program, representation, contents, m_program.typeOf(m_function, stored)))
            fail(site, PlacementRule::StoredTypeTooWide);
        if (representation != SlotRepresentation::DeferredClosure)
            return;

        // What is stored is the closure that the slot leaves unmade, or what a narrow reference holds.
        ValueID definition = definitionThroughUpcasts(m_function, stored);
        const Value& closure = m_function.at(definition);
        if (closure.opcode != Opcode::NewClosure || closure.closureForm() != ClosureForm::Deferred) {
            if (m_program.typeOf(m_function, stored).primitives & ~Primitive::UndefinedOrNull)
                fail(site, PlacementRule::DeferredClosureStore);
            return;
        }
        if (!isDeferredFunction(closure.function()) || !isUsedOnlyByStore(stored, definition))
            fail(site, PlacementRule::DeferredClosureStore);
        else
            m_closuresStoredInDeferredSlots.append(definition);
    }

    void checkAccessStore(ValueID id, FieldAccessID accessID, ValueID stored)
    {
        const FieldAccess* access = accessAt(accessID);
        if (!access || !access->slot.hasStorage())
            return;
        checkStoredValue(id, stored, access->slot.representation, m_program.at(access->contents), [&](FunctionID function) {
            return std::ranges::all_of(m_program.at(access->membersWithField).members, [&](LayoutID member) {
                std::optional<uint32_t> field = m_program.at(member).findField(access->key);
                return field && m_slots.slot(member, *field).deferredFunction == function;
            });
        });
    }

    void checkInitialValues(ValueID id, const Value& value)
    {
        LayoutID layout = value.layout();
        std::span<const uint32_t> fields = value.fields();
        std::span<const ValueID> values = value.valuesOperands();
        if (fields.size() != values.size()) {
            fail(id, PlacementRule::SiteRefersElsewhere);
            return;
        }
        size_t numberOfFields = m_program.at(layout).fields.size();
        bool listsClosureOverEnvironment = false;
        for (size_t index = 0; index < fields.size(); ++index) {
            const PlacedSlot& slot = m_slots.slot(layout, fields[index]);
            if (fields[index] >= numberOfFields || !slot.hasStorage())
                continue;
            const Field& declaration = fieldAt(m_program, declarationOf(m_program, { layout, fields[index] }));
            checkStoredValue(id, values[index], slot.representation, m_program.at(slotTypeOf(m_program, declaration)), [&](FunctionID function) {
                return slot.deferredFunction == function;
            });
            listsClosureOverEnvironment |= slot.deferredClosureCapturesEnvironment;
        }
        // Whoever asks for such a closure first finds its environment in the hidden field of the object.
        std::optional<uint32_t> environmentField = m_program.at(layout).flow.closureEnvironmentField;
        if (listsClosureOverEnvironment && (!environmentField || std::ranges::find(fields, *environmentField) == fields.end()))
            fail(id, PlacementRule::DeferredClosureStore);
    }

    void checkReadOfFilledSlot(ValueID id, const Value& value)
    {
        const FieldAccess* access = accessAt(value.fieldAccess());
        if (!access)
            return;
        // A load from an Extension that is missing or too short answers absent, and code generation reads that as undefined.
        bool mayReadAbsent = !access->slot.isOutOfLine() && (m_program.at(access->contents).primitives & Primitive::Absent);
        if (access->coverage == FieldAccess::Coverage::EveryMember && !mayReadAbsent)
            return;
        // Where some member may leave the slot empty, the verifier asks the read for a guard that proves SlotFilled.
        bool isAlwaysFilled = std::ranges::all_of(m_program.at(access->receivers).members, [&](LayoutID member) {
            std::optional<uint32_t> field = m_program.at(member).findField(access->key);
            return field && flowOf(m_program, { member, *field }).occupancy == Occupancy::AlwaysFilled;
        });
        if (isAlwaysFilled)
            fail(id, PlacementRule::ContentsTooNarrow);
    }

    // PutSlot declares no collection. A filled slot has storage unless the object may list the field without it: the row says so, and the store then gets the granule without a collection.
    void checkStoreWithoutAllocation(ValueID id, const Value& value)
    {
        const FieldAccess* access = accessAt(value.fieldAccess());
        if (!access || !access->slot.hasStorage())
            return;
        bool mayLackStorage = std::ranges::any_of(m_program.at(access->membersWithField).members, [&](LayoutID member) {
            std::optional<uint32_t> field = m_program.at(member).findField(access->key);
            return field && m_slots.slot(member, *field).mayExistWithoutStorage;
        });
        bool findsStorage = !access->slot.isOutOfLine() || (access->mayBeFilledWithoutStorage == mayLackStorage && guardProvesSlotFilled(analyses(), value));
        if (!findsStorage)
            fail(id, PlacementRule::OutOfLineNotAllowed);
    }

    // The run time throws where the private field exists already, and a slot without an absent state always looks filled.
    void checkDefinitionByName(ValueID id, const Value& value)
    {
        if (!m_slots.layout(value.layout()).hasSlots())
            return;
        const PlacedSlot& slot = m_slots.slot(value.layout(), value.field());
        if (slot.hasStorage() && !canLeaveSlot(slot.representation))
            fail(id, PlacementRule::AbsentStateMissing);
    }

    void checkVariable(ValueID id, VariableID variable, bool isOnlyRead)
    {
        const PlacedVariable& placed = m_placement.variable(variable);
        if (!placed.hasWordInStatics() && !(isOnlyRead && placed.storage == VariableStorage::ClosureTable))
            fail(id, PlacementRule::VariableWithoutStorage);
    }

    const PlacementCheckContext& m_context;
    const EntriesWithPlacedFields& m_placedFields;
    const Program& m_program;
    const Placement& m_placement;
    const SlotPlan& m_slots;
    FunctionID m_id;
    const Function& m_function;
    Vector<SiteCellUse>& m_siteCells;
    std::optional<FunctionAnalyses> m_analyses;
    Vector<ValueID> m_closuresStoredInDeferredSlots;
    Vector<ValueID> m_deferredClosures;
};

} // anonymous namespace

static void checkSiteCells(const PlacementCheckContext& context, Vector<SiteCellUse>&& uses)
{
    const ParallelLoops& loops = context.build.parallelLoops();
    auto cellOf = [](const SiteCellUse& use) {
        return std::tuple { use.kind, use.site };
    };
    auto fail = [&](const SiteCellUse& use) {
        context.failures.fail(PlacementRule::SiteRefersElsewhere, { }, use.user.function, use.user.value);
    };
    auto valueOf = [&](const SiteCellUse& use) -> const Value& {
        return context.program.at(use.user.function).at(use.user.value);
    };

    parallelSort(loops, uses.mutableSpan(), [&](const SiteCellUse& a, const SiteCellUse& b) {
        return std::tuple { a.offset, cellOf(a), a.user } < std::tuple { b.offset, cellOf(b), b.user };
    });
    for (size_t index = 1; index < uses.size(); ++index) {
        if (uses[index].offset == uses[index - 1].offset && cellOf(uses[index]) != cellOf(uses[index - 1]))
            fail(uses[index]);
    }

    parallelSort(loops, uses.mutableSpan(), [&](const SiteCellUse& a, const SiteCellUse& b) {
        return std::tuple { cellOf(a), a.offset, a.user } < std::tuple { cellOf(b), b.offset, b.user };
    });
    for (size_t index = 1; index < uses.size(); ++index) {
        if (cellOf(uses[index]) != cellOf(uses[index - 1]))
            continue;
        if (uses[index].offset != uses[index - 1].offset)
            fail(uses[index]);
        if (uses[index].regExpRecord != uses[index - 1].regExpRecord)
            context.failures.fail(PlacementRule::RegExpRecordWrong, { }, uses[index].user.function, uses[index].user.value);
        // The first to run makes the object that the cell keeps, and the others take it for theirs.
        if (uses[index].kind == SiteCellKind::TemplateObject && !std::ranges::equal(valueOf(uses[index]).templateStrings(), valueOf(uses[index - 1]).templateStrings()))
            fail(uses[index]);
    }
}

static void checkDataObject(const PlacementCheckContext& context, const DataNode::Object& object)
{
    if (!object.layout || context.inputs.slots.storageOf(object.layout) != LayoutStorage::CompiledObject)
        return;

    const Layout& layout = context.program.at(object.layout);
    Vector<FieldIndex, 16> fields;
    for (const DataMember& member : object.members) {
        if (std::optional<uint32_t> field = layout.findField(member.key))
            fields.append(*field);
    }

    ShapeRange shapes = context.placement.shapes().shapesOf(object.layout);
    bool hasAllocation = false;
    for (uint32_t shape = shapes.first; shape < shapes.end(); ++shape)
        hasAllocation |= !!context.placement.allocations().find(object.layout, shape, fields.span());
    if (!fitsReadOnlyEncoding(context.program, object) || !hasAllocation)
        context.failures.fail(PlacementRule::DataNotPlaced, object.layout);
}

static void checkDataTree(const PlacementCheckContext& context, const DataNode& root)
{
    Vector<const DataNode*, 16> worklist { &root };
    while (!worklist.isEmpty()) {
        const DataNode& node = *worklist.takeLast();
        switch (node.kind()) {
        case DataNode::Kind::Undefined:
        case DataNode::Kind::Null:
        case DataNode::Kind::Hole:
        case DataNode::Kind::Boolean:
        case DataNode::Kind::Number:
        case DataNode::Kind::String:
        case DataNode::Kind::BigInt:
            continue;
        case DataNode::Kind::Array:
            for (const DataNode& element : node.as<DataNode::Array>().elements)
                worklist.append(&element);
            continue;
        case DataNode::Kind::Object:
            checkDataObject(context, node.as<DataNode::Object>());
            for (const DataMember& member : node.as<DataNode::Object>().members)
                worklist.append(&member.value);
            continue;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }
}

static void checkPlacedData(const PlacementCheckContext& context)
{
    std::span<const RequestRecord> records = context.placement.requests().records();
    forEachIndexInParallel(context.build.parallelLoops(), records.size(), [&](unsigned index) {
        DataID data = WTF::switchOn(records[index],
            [](const NewClassRecord&) { return DataID(); },
            [](const CloneAsRecord&) { return DataID(); },
            [](const TemplateObjectRecord&) { return DataID(); },
            [](const DirectEvalRecord&) { return DataID(); },
            [](const DataRecord& record) { return record.data; },
            [](const CollectionDataRecord& record) { return record.data; });
        if (data)
            checkDataTree(context, context.program.at(data).root);
    });
}

void checkSites(const PlacementCheckContext& context)
{
    if (context.failures.hasFailed())
        return;

    const Program& program = context.program;
    EntriesWithPlacedFields placedFields = findEntriesWithPlacedFields();
    Vector<SiteCellUse> siteCells = collectInParallel<SiteCellUse>(context.build.parallelLoops(), program.functions.size(), [&](unsigned index, Vector<SiteCellUse>& found) {
        FunctionID id(index);
        if (program.at(id).hasBody())
            SiteChecker(context, placedFields, id, found).run();
    });
    checkSiteCells(context, WTF::move(siteCells));
    checkPlacedData(context);
}

static void collectSiteCacheUses(const PlacementCheckContext& context, FunctionID id, Vector<SiteCacheUse>& uses)
{
    const Function& function = context.program.at(id);
    std::optional<FunctionAnalyses> analyses;
    std::optional<FunctionSiteWeights> weights;
    auto ensureAnalyses = [&]() -> FunctionAnalyses& {
        if (!analyses)
            analyses.emplace(AnalysisContext { context.build, context.program, &context.inputs.flow, nullptr, id }, function);
        return *analyses;
    };
    auto weightOf = [&](BlockID block) {
        if (!weights)
            weights.emplace(context.program, context.inputs.heat, id, ensureAnalyses());
        return weights->weightOf(block);
    };

    for (unsigned blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        BlockID block(blockIndex);
        for (ValueID valueID : function.at(block).values) {
            const Value& value = function.at(valueID);
            if (!Value::opcodesWithSiteCache.contains(value.opcode))
                continue;
            unsigned numberUsed = numberOfSiteCachesUsedBy(context.program, context.placement, function, value);
            if (numberUsed && !mayReadSiteCachesAgain(ensureAnalyses(), block))
                numberUsed = 0;
            unsigned numberSet = 0;
            forEachSiteCache(value, [&](SiteCacheID cache) {
                ++numberSet;
                uses.append({ cache, weightOf(block), { id, valueID } });
            });
            if (numberSet > numberUsed)
                context.failures.fail(PlacementRule::SiteCacheNotUsed, { }, id, valueID);
            for (unsigned number = numberSet; number < numberUsed; ++number)
                uses.append({ SiteCacheID(), weightOf(block), { id, valueID } });
        }
    }
}

void checkSiteCaches(const PlacementCheckContext& context)
{
    if (context.failures.hasFailed())
        return;

    const ParallelLoops& loops = context.build.parallelLoops();
    Vector<SiteCacheUse> uses = collectInParallel<SiteCacheUse>(loops, context.program.functions.size(), [&](unsigned index, Vector<SiteCacheUse>& found) {
        collectSiteCacheUses(context, FunctionID(index), found);
    });
    parallelSort(loops, uses.mutableSpan(), [](const SiteCacheUse& a, const SiteCacheUse& b) {
        return std::tuple { a.cache, a.user } < std::tuple { b.cache, b.user };
    });
    auto fail = [&](PlacementRule rule, const SiteCacheUse& use) {
        context.failures.fail(rule, { }, use.user.function, use.user.value);
    };
    auto positionOf = [&](const SiteCacheUse& use) {
        return siteCachePositionOf(context.program, use.user.function, use.user.value);
    };

    // The null id of a site without a cache is greater than every number.
    size_t numberOfPlacedCaches = std::ranges::partition_point(uses, [](const SiteCacheUse& use) {
        return !!use.cache;
    }) - uses.begin();
    std::span<const SiteCacheUse> placed = uses.span().first(numberOfPlacedCaches);
    std::span<const SiteCacheUse> sitesWithoutCache = uses.span().subspan(numberOfPlacedCaches);

    for (size_t index = 0; index < placed.size(); ++index) {
        uint32_t expected = index ? placed[index - 1].cache.index() + 1 : 0;
        if (placed[index].cache.index() != expected)
            fail(PlacementRule::SiteCacheNotDistinct, placed[index]);
        if (index && positionOf(placed[index]) < positionOf(placed[index - 1]))
            fail(PlacementRule::SiteCacheOrder, placed[index]);
    }
    if (placed.size() != context.siteCaches.count())
        context.failures.fail(PlacementRule::SiteCacheNotDistinct);

    if (sitesWithoutCache.empty())
        return;
    if (context.siteCaches.count() != context.build.limit(LimitID::MaxSiteCaches))
        fail(PlacementRule::SiteCacheOrder, sitesWithoutCache.front());
    SiteWeight lowestPlacedWeight = placed.empty() ? 0 : std::ranges::min(placed, { }, &SiteCacheUse::weight).weight;
    for (const SiteCacheUse& use : sitesWithoutCache) {
        if (!placed.empty() && use.weight > lowestPlacedWeight)
            fail(PlacementRule::SiteCacheOrder, use);
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
