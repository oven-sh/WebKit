/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPlacementSteps.h"

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTBuildContext.h"
#include "AOTFlowResults.h"
#include "AOTPlacementCheck.h"
#include "AOTRegExpFlags.h"
#include "AOTRegExpRequests.h"
#include "AOTSchemaOperations.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include <algorithm>
#include <optional>
#include <span>
#include <wtf/StdLibExtras.h>
#include <wtf/Variant.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(defaultLimitValue(LimitID::MaxEvalBindingOffset, hostArchitecture()) == SlotData::maxOffset);

static_assert(Value::opcodesWithAllocation == OpcodeSet { Opcode::NewClosure, Opcode::NewObject, Opcode::MaterializeEnvironment });
static_assert(Value::opcodesWithStaticsOffset == OpcodeSet { Opcode::NewArray, Opcode::NewRegExp, Opcode::SharedRegExpLiteral, Opcode::NewTemplateObject });
static_assert(Value::opcodesWithGuardBit == OpcodeSet { Opcode::AreMembersUnchanged });
static_assert(Value::opcodesWithRequest == OpcodeSet {
    Opcode::TryPutByIds, Opcode::NewClass, Opcode::CloneAs, Opcode::NewTemplateObject, Opcode::DirectEval, Opcode::Data, Opcode::NewSetFromData,
    Opcode::NewMapFromData, Opcode::CopyDataProperties, Opcode::CopyObject
});

namespace {

struct PendingRequest {
    ValueID value;
    Variant<RequestRecord, PlacedPropertyRun> record;
    Request request;
};

struct FunctionRequests {
    FunctionID id;
    Function* function { nullptr };
    Vector<PendingRequest> requests;
};

struct ResolutionInputs {
    const PlacementContext& context;
    const SiteResolutionInputs& steps;
    PlacementFailureSink& failures;
    OpcodeSet resolvedOpcodes;
};

class FunctionResolver {
public:
    FunctionResolver(const ResolutionInputs&, FunctionID, Function&);

    Vector<PendingRequest> run();
    void resolveFieldAccesses(Vector<ValueID>& valuesWithOtherWords);
    Vector<PendingRequest> resolveOtherWordsOf(std::span<const ValueID>);

private:
    const Program& program() const { return m_inputs.context.program; }
    const Placement& placement() const { return m_inputs.steps.placement; }
    const PlacedSites& sites() const { return m_inputs.steps.sites; }
    const LiteralSites& literals() const { return m_inputs.steps.literals; }

    void resolveValue(ValueID, Value&);
    void resolveField(ValueID, Value&, const PayloadFieldInfo&);
    void resolvePlacedKeys(Value&);

    FieldAccessID fieldAccessOf(PropertyKeyID, LayoutSetID receivers, bool isStore) const;
    AllocationID allocationOf(ValueID, const Value&) const;
    AllocationID closureAllocationOf(const Value&) const;
    AllocationID cellAllocationOf(ValueID, LayoutID, uint32_t shape, std::span<const FieldIndex> initialFields, bool holdsInnerObjects = false) const;
    RegExpRecordID regExpRecordOf(ValueID, const Value&) const;
    std::optional<StaticsOffset> staticsOffsetOf(const Value&) const;
    std::optional<StaticsOffset> arrayProfileOffsetOf(const Value&) const;
    StaticsOffset sourceSiteOffsetOf(const Value&, const Vector<SourceSite>& ascendingSites, const Vector<uint32_t>& offsets) const;
    std::optional<OrderWord> orderWordOf(const Value&) const;
    GuardBitID guardBitOf(const Value&) const;

    void addRequest(ValueID, const Value&);
    std::optional<RequestRecord> requestRecordOf(ValueID, const Value&) const;
    std::optional<RequestRecord> cloneAsRecordOf(ValueID, const Value&) const;
    RequestRecord templateObjectRecordOf(const Value&) const;
    RequestRecord directEvalRecordOf(ValueID, const Value&) const;
    RequestRecord dataRecordOf(const Value&) const;
    uint32_t slotOffsetOf(const EvalBinding&) const;
    void reportIfExceeded(ValueID, LimitID, uint64_t amount) const;

    const ResolutionInputs& m_inputs;
    FunctionID m_id;
    Function& m_function;
    Vector<PendingRequest> m_requests;
};

} // anonymous namespace

template<typename Key>
static uint32_t offsetOfKey(const Vector<Key>& ascendingKeys, const Vector<uint32_t>& offsets, const Key& key)
{
    auto found = std::ranges::lower_bound(ascendingKeys, key);
    RELEASE_ASSERT(found != ascendingKeys.end() && *found == key);
    return offsets[found - ascendingKeys.begin()];
}

FunctionResolver::FunctionResolver(const ResolutionInputs& inputs, FunctionID id, Function& function)
    : m_inputs(inputs)
    , m_id(id)
    , m_function(function)
{
}

Vector<PendingRequest> FunctionResolver::run()
{
    for (auto& block : m_function.blocks) {
        for (ValueID id : block.values) {
            Value& value = m_function.edit(id);
            if (m_inputs.resolvedOpcodes.contains(value.opcode))
                resolveValue(id, value);
        }
    }
    return WTF::move(m_requests);
}

// The two halves of run(): between them they hand every field to resolveField() that run() does.
void FunctionResolver::resolveFieldAccesses(Vector<ValueID>& valuesWithOtherWords)
{
    for (auto& block : m_function.blocks) {
        for (ValueID id : block.values) {
            if (!m_inputs.resolvedOpcodes.contains(m_function.at(id).opcode))
                continue;
            Value& value = m_function.edit(id);
            if (Value::opcodesWithPlacedKeys.contains(value.opcode))
                resolvePlacedKeys(value);
            bool hasOtherWords = false;
            for (const PayloadFieldInfo& field : payloadFieldsOf(value.opcode)) {
                if (field.firstForm != IRForm::Placed)
                    continue;
                if (field.kind == PayloadKind::FieldAccess)
                    resolveField(id, value, field);
                else if (field.kind != PayloadKind::SiteCache && field.kind != PayloadKind::GlobalBinding)
                    hasOtherWords = true;
            }
            if (hasOtherWords)
                valuesWithOtherWords.append(id);
        }
    }
}

Vector<PendingRequest> FunctionResolver::resolveOtherWordsOf(std::span<const ValueID> values)
{
    for (ValueID id : values) {
        Value& value = m_function.edit(id);
        for (const PayloadFieldInfo& field : payloadFieldsOf(value.opcode)) {
            if (field.firstForm == IRForm::Placed && field.kind != PayloadKind::FieldAccess)
                resolveField(id, value, field);
        }
    }
    return WTF::move(m_requests);
}

void FunctionResolver::resolveValue(ValueID id, Value& value)
{
    if (Value::opcodesWithPlacedKeys.contains(value.opcode))
        resolvePlacedKeys(value);
    for (const PayloadFieldInfo& field : payloadFieldsOf(value.opcode)) {
        if (field.firstForm == IRForm::Placed)
            resolveField(id, value, field);
    }
}

void FunctionResolver::resolveField(ValueID id, Value& value, const PayloadFieldInfo& field)
{
    switch (field.kind) {
    case PayloadKind::FieldAccess:
        value.setFieldAccess(fieldAccessOf(value.key(), value.receivers(), isSlotStore(value.opcode)));
        return;
    case PayloadKind::Allocation:
        value.setAllocation(allocationOf(id, value));
        return;
    case PayloadKind::RegExpRecord:
        value.setRegExpRecord(regExpRecordOf(id, value));
        return;
    case PayloadKind::Request:
        addRequest(id, value);
        return;
    case PayloadKind::StaticsOffset:
        value.setStaticsOffset(staticsOffsetOf(value));
        return;
    case PayloadKind::OrderWord:
        value.setOrderWord(orderWordOf(value));
        return;
    case PayloadKind::GuardBit:
        value.setGuardBit(guardBitOf(value));
        return;
    case PayloadKind::GlobalBinding:
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
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static FieldAccessID fieldAccessIn(const Placement& placement, PropertyKeyID key, LayoutSetID receivers, bool isStore)
{
    FieldAccessID access = placement.findFieldAccess(key, receivers, isStore);
    RELEASE_ASSERT(access);
    return access;
}

static void resolvePlacedKeysIn(const Placement& placement, Value& value)
{
    LayoutSetID receivers = value.receivers();
    auto placedKeys = value.placedKeys();
    Vector<PlacedKey, 8> keys(placedKeys.size(), [&](size_t index) {
        PlacedKey placedKey = placedKeys[index];
        placedKey.fieldAccess = fieldAccessIn(placement, placedKey.key, receivers, true);
        return placedKey;
    });
    value.setPlacedKeys(keys.span());
}

void FunctionResolver::resolvePlacedKeys(Value& value)
{
    resolvePlacedKeysIn(placement(), value);
}

FieldAccessID FunctionResolver::fieldAccessOf(PropertyKeyID key, LayoutSetID receivers, bool isStore) const
{
    return fieldAccessIn(placement(), key, receivers, isStore);
}

AllocationID FunctionResolver::allocationOf(ValueID id, const Value& value) const
{
    const ShapeTable& shapes = placement().shapes();
    switch (value.opcode) {
    case Opcode::NewClosure:
        return closureAllocationOf(value);
    case Opcode::NewObject:
        return cellAllocationOf(id, value.layout(), shapes.shapeOf(value.layout(), value.allocationClass()), value.fields(), takesCellWithInnerObjects(m_inputs.context.slots(), value));
    case Opcode::MaterializeEnvironment:
        return cellAllocationOf(id, value.layout(), shapes.generalShapeOf(value.layout()), { });
    default:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

AllocationID FunctionResolver::closureAllocationOf(const Value& value) const
{
    switch (value.closureForm()) {
    case ClosureForm::Full:
        return placement().closureAllocation(value.numberOfVariadicOperands());
    case ClosureForm::Slim:
    case ClosureForm::Deferred:
    case ClosureForm::Singleton:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

AllocationID FunctionResolver::cellAllocationOf(ValueID id, LayoutID layout, uint32_t shape, std::span<const FieldIndex> initialFields, bool holdsInnerObjects) const
{
    AllocationID allocation = placement().allocations().find(layout, shape, initialFields, holdsInnerObjects);
    if (!allocation)
        m_inputs.failures.fail(PlacementRule::AllocationOfUnplacedLayout, layout, m_id, id);
    return allocation;
}

RegExpRecordID FunctionResolver::regExpRecordOf(ValueID id, const Value& value) const
{
    RegExpRecordID record = m_inputs.context.inputs.regExpRequests.recordOf(value.pattern(), toYarrFlags(value.regExpFlags()));
    if (!record)
        m_inputs.failures.fail(PlacementRule::RegExpRecordWrong, { }, m_id, id);
    return record;
}

std::optional<StaticsOffset> FunctionResolver::staticsOffsetOf(const Value& value) const
{
    switch (value.opcode) {
    case Opcode::NewArray:
        return arrayProfileOffsetOf(value);
    case Opcode::NewRegExp:
    case Opcode::SharedRegExpLiteral:
        return sourceSiteOffsetOf(value, sites().regExpLiterals, literals().regExpCellOffsets);
    case Opcode::NewTemplateObject:
        return sourceSiteOffsetOf(value, sites().templateObjects, literals().templateObjectOffsets);
    default:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

std::optional<StaticsOffset> FunctionResolver::arrayProfileOffsetOf(const Value& value) const
{
    switch (value.literalStorage()) {
    case ArrayLiteralStorage::Fresh:
        return std::nullopt;
    case ArrayLiteralStorage::Profiled:
        return sourceSiteOffsetOf(value, sites().profiledArrayLiterals, literals().profiledLiteralOffsets);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

StaticsOffset FunctionResolver::sourceSiteOffsetOf(const Value& value, const Vector<SourceSite>& ascendingSites, const Vector<uint32_t>& offsets) const
{
    return StaticsOffset(offsetOfKey(ascendingSites, offsets, sourceSiteOf(program(), m_id, value)));
}

std::optional<OrderWord> FunctionResolver::orderWordOf(const Value& value) const
{
    auto& tests = sites().keyOrderTests;
    LayoutID layout = value.layout();
    auto fields = value.fields();
    auto test = std::ranges::partition_point(tests, [&](const KeyOrderTestUse& candidate) {
        if (candidate.layout != layout)
            return candidate.layout < layout;
        return std::ranges::lexicographical_compare(candidate.fields, fields);
    });
    RELEASE_ASSERT(test != tests.end() && test->layout == layout && equalSpans(test->fields.span(), fields));
    std::optional<uint32_t> word = m_inputs.steps.orderWords.ofTest[test - tests.begin()];
    if (!word)
        return std::nullopt;
    return OrderWord(*word);
}

GuardBitID FunctionResolver::guardBitOf(const Value& value) const
{
    return placement().guardBits().find(value.conditions());
}

void FunctionResolver::addRequest(ValueID id, const Value& value)
{
    if (namesPropertyRun(value.opcode)) {
        if (auto run = propertyRunOf(m_inputs.context, m_id, id))
            m_requests.append(PendingRequest { id, WTF::move(*run), { } });
        return;
    }
    if (auto record = requestRecordOf(id, value))
        m_requests.append(PendingRequest { id, WTF::move(*record), { } });
}

std::optional<RequestRecord> FunctionResolver::requestRecordOf(ValueID id, const Value& value) const
{
    switch (value.opcode) {
    case Opcode::NewClass:
        return RequestRecord { NewClassRecord { value.layout() } };
    case Opcode::CloneAs:
        return cloneAsRecordOf(id, value);
    case Opcode::NewTemplateObject:
        return templateObjectRecordOf(value);
    case Opcode::DirectEval:
        return directEvalRecordOf(id, value);
    case Opcode::Data:
        return dataRecordOf(value);
    case Opcode::NewSetFromData:
    case Opcode::NewMapFromData:
        return RequestRecord { CollectionDataRecord { value.data(), value.opcode == Opcode::NewMapFromData } };
    default:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

std::optional<RequestRecord> FunctionResolver::cloneAsRecordOf(ValueID id, const Value& value) const
{
    LayoutID layout = value.layout();
    auto fields = value.fields();
    AllocationClassID allocationClass = m_inputs.context.inputs.flow.at(value.allocationSite()).allocationClass;
    uint32_t shape = placement().shapes().shapeOf(layout, allocationClass);
    bool everyFieldHasRecord = std::ranges::all_of(fields, [&](FieldIndex field) {
        return placement().slot(layout, field).indexInLayoutRecord.has_value();
    });
    if (!shape || !everyFieldHasRecord) {
        m_inputs.failures.fail(PlacementRule::RequestWrong, layout, m_id, id);
        return std::nullopt;
    }
    return RequestRecord { CloneAsRecord { layout, shape, Vector<FieldIndex>(fields) } };
}

RequestRecord FunctionResolver::templateObjectRecordOf(const Value& value) const
{
    auto strings = value.templateStrings();
    return TemplateObjectRecord { Vector<TemplateString>(strings.size(), [&](size_t index) {
        return strings[index];
    }) };
}

RequestRecord FunctionResolver::directEvalRecordOf(ValueID id, const Value& value) const
{
    EvalScopeID scope = value.evalScope();
    for (const EvalBinding& binding : program().at(scope).bindings) {
        reportIfExceeded(id, LimitID::MaxEvalBindingDepth, binding.depth);
        reportIfExceeded(id, LimitID::MaxEvalBindingOffset, slotOffsetOf(binding));
    }
    return DirectEvalRecord { scope, program().sourceFunctionOf(m_function, value).module };
}

RequestRecord FunctionResolver::dataRecordOf(const Value& value) const
{
    DataID data = value.data();
    switch (value.encoding()) {
    case DataEncoding::ReadOnlyTree:
        return DataRecord { data, std::nullopt, std::nullopt };
    case DataEncoding::CopyOnWriteArray:
        if (isStorageOfArray(program().at(data).root.as<DataNode::Array>()))
            return DataRecord { data, std::nullopt, offsetOfKey(sites().arraysWithStorageInImage, literals().nodesOfStorageInImage, data) };
        return DataRecord { data, offsetOfKey(sites().copyOnWriteArrays, literals().sharedStorageOffsets, data), std::nullopt };
    case DataEncoding::Unselected:
    case DataEncoding::Expanded:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// SlotData::create asserts what the limit reports, so the offset is read here without creating one.
uint32_t FunctionResolver::slotOffsetOf(const EvalBinding& binding) const
{
    if (binding.variable) {
        const PlacedVariable& variable = placement().variable(binding.variable);
        return variable.hasWordInStatics() ? variable.offset : 0;
    }
    if (binding.kind == EvalBindingKind::MappedArgument) {
        RELEASE_ASSERT(binding.field);
        return *binding.field;
    }
    if (!binding.layout)
        return 0;
    RELEASE_ASSERT(binding.field);
    return placement().slot(binding.layout, *binding.field).offset;
}

void FunctionResolver::reportIfExceeded(ValueID id, LimitID limit, uint64_t amount) const
{
    BuildContext& build = m_inputs.context.build;
    if (amount > build.limit(limit))
        build.reportLimitExceeded(limit, amount, { .function = m_id, .value = id });
}

static OpcodeSet computeResolvedOpcodes()
{
    OpcodeSet opcodes = Value::opcodesWithPlacedKeys;
    for (Opcode opcode : allMembersOf<Opcode>) {
        bool hasResolvedField = std::ranges::any_of(payloadFieldsOf(opcode), [](const PayloadFieldInfo& field) {
            return field.firstForm == IRForm::Placed && field.kind != PayloadKind::SiteCache && field.kind != PayloadKind::GlobalBinding;
        });
        if (hasResolvedField)
            opcodes.add(opcode);
    }
    return opcodes;
}

static void checkTerminatorPayloads()
{
    for (TerminatorKind kind : allMembersOf<TerminatorKind>) {
        for (const PayloadFieldInfo& field : payloadFieldsOf(kind))
            RELEASE_ASSERT(field.firstForm != IRForm::Placed);
    }
}

static Vector<FunctionRequests> collectFunctions(ProgramEditor& editor)
{
    const Program& program = editor.program();
    Vector<FunctionRequests> functions;
    for (uint32_t i = 0; i < program.functions.size(); ++i) {
        if (program.functions[i].hasBody())
            functions.append(FunctionRequests { FunctionID(i), &editor.atForPlacedWords(FunctionID(i)), { } });
    }
    return functions;
}

static ResolvedSites numberRequests(const Program& program, Vector<FunctionRequests>& functions)
{
    ResolvedSites resolved;
    for (auto& function : functions) {
        for (auto& pending : function.requests) {
            pending.request = WTF::switchOn(WTF::move(pending.record),
                [&](RequestRecord&& record) {
                    return resolved.requests.ensure(program, WTF::move(record));
                },
                [&](PlacedPropertyRun&& run) {
                    return resolved.propertyRuns.ensure(WTF::move(run));
                });
        }
    }
    resolved.stringConstantsInTables.ensureSize(program.strings.size());
    for (const RequestRecord& record : resolved.requests.records())
        collectStringConstantsOf(program, record, resolved.stringConstantsInTables);
    return resolved;
}

static ResolvedSites numberAndWriteRequests(const PlacementContext& context, Vector<FunctionRequests>& functions)
{
    ResolvedSites resolved = numberRequests(context.program, functions);
    forEachIndexInParallel(context.build.parallelLoops(), functions.size(), [&](unsigned index) {
        auto& entry = functions[index];
        for (auto& pending : entry.requests)
            entry.function->edit(pending.value).setRequest(pending.request);
    });
    return resolved;
}

ResolvedSites resolveSites(ProgramEditor& editor, const PlacementContext& context, const SiteResolutionInputs& steps)
{
    checkTerminatorPayloads();
    const ParallelLoops& loops = context.build.parallelLoops();
    PlacementFailureSink failures(context.build);
    ResolutionInputs inputs { context, steps, failures, computeResolvedOpcodes() };
    Vector<FunctionRequests> functions = collectFunctions(editor);
    forEachIndexInParallel(loops, functions.size(), [&](unsigned index) {
        auto& entry = functions[index];
        entry.requests = FunctionResolver(inputs, entry.id, *entry.function).run();
    });
    return numberAndWriteRequests(context, functions);
}

void resolveFieldAccesses(const PlacementContext& context, const SiteResolutionInputs& steps, FunctionID id, Function& function, Vector<ValueID>& valuesWithOtherWords)
{
    static const OpcodeSet resolvedOpcodes = computeResolvedOpcodes();
    PlacementFailureSink failures(context.build);
    ResolutionInputs inputs { context, steps, failures, resolvedOpcodes };
    FunctionResolver(inputs, id, function).resolveFieldAccesses(valuesWithOtherWords);
}

ResolvedSites resolveOtherWords(ProgramEditor& editor, const PlacementContext& context, const SiteResolutionInputs& steps, std::span<const Vector<ValueID>> valuesWithOtherWords)
{
    checkTerminatorPayloads();
    const Program& program = context.program;
    RELEASE_ASSERT(valuesWithOtherWords.size() == program.functions.size());
    PlacementFailureSink failures(context.build);
    ResolutionInputs inputs { context, steps, failures, computeResolvedOpcodes() };
    Vector<FunctionRequests> functions;
    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        if (program.functions[index].hasBody() && !valuesWithOtherWords[index].isEmpty())
            functions.append(FunctionRequests { FunctionID(index), &editor.atForPlacedWords(FunctionID(index)), { } });
    }
    forEachIndexInParallel(context.build.parallelLoops(), functions.size(), [&](unsigned index) {
        auto& entry = functions[index];
        entry.requests = FunctionResolver(inputs, entry.id, *entry.function).resolveOtherWordsOf(valuesWithOtherWords[entry.id.index()].span());
    });
    return numberAndWriteRequests(context, functions);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
