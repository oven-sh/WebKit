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
#include "AOTFunctionAnalyses.h"
#include "AOTFunctionKinds.h"
#include "AOTHeat.h"
#include "AOTLayoutData.h"
#include "AOTNarrowFieldTypes.h"
#include "AOTPhaseTimes.h"
#include "AOTRegExpFlags.h"
#include "AOTRegExpRequests.h"
#include "AOTSchemaOperations.h"
#include "AOTThreads.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"
#include <algorithm>
#include <atomic>
#include <optional>
#include <span>
#include <tuple>
#include <wtf/StdLibExtras.h>
#include <wtf/Variant.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(defaultLimitValue(LimitID::MaxEvalBindingOffset, hostArchitecture()) == SlotData::maxOffset);

static_assert(Value::opcodesWithAllocation == OpcodeSet { Opcode::NewClosure, Opcode::NewObject, Opcode::MaterializeEnvironment });
static_assert(Value::opcodesWithStaticsOffset == OpcodeSet { Opcode::NewArray, Opcode::NewRegExp, Opcode::SharedRegExpLiteral, Opcode::NewTemplateObject });
static_assert(Value::opcodesWithGuardBit == OpcodeSet { Opcode::AreMembersUnchanged });
static_assert(Value::opcodesWithRequest == OpcodeSet {
    Opcode::TryPutByIds, Opcode::NewClass, Opcode::NewTemplateObject, Opcode::DirectEval, Opcode::Data, Opcode::NewSetFromData,
    Opcode::NewMapFromData, Opcode::NewFormatterWithPattern, Opcode::FormatDateWithPattern, Opcode::FormatNumberWithPattern, Opcode::CopyDataProperties, Opcode::CopyObject
});

static constexpr OpcodeSet opcodesWithOtherWords = Value::opcodesWithAllocation | Value::opcodesWithRegExpRecord | Value::opcodesWithRequest | Value::opcodesWithStaticsOffset | Value::opcodesWithOrderWord | Value::opcodesWithGuardBit;
static constexpr OpcodeSet opcodesWithWordsOfSites = opcodesWithOtherWords | Value::opcodesWithFieldAccess | Value::opcodesWithPlacedKeys;

// Whether a word of this kind, which an operation gets at placement, is written: here, with the global bindings or with the site caches.
static bool isWordWrittenAtPlacement(Opcode opcode, PayloadKind kind)
{
    switch (kind) {
    case PayloadKind::FieldAccess:
        return Value::opcodesWithFieldAccess.contains(opcode);
    case PayloadKind::Allocation:
        return Value::opcodesWithAllocation.contains(opcode);
    case PayloadKind::RegExpRecord:
        return Value::opcodesWithRegExpRecord.contains(opcode);
    case PayloadKind::Request:
        return Value::opcodesWithRequest.contains(opcode);
    case PayloadKind::StaticsOffset:
        return Value::opcodesWithStaticsOffset.contains(opcode);
    case PayloadKind::OrderWord:
        return Value::opcodesWithOrderWord.contains(opcode);
    case PayloadKind::GuardBit:
        return Value::opcodesWithGuardBit.contains(opcode);
    case PayloadKind::GlobalBinding:
    case PayloadKind::SiteCache:
        return true;
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
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

template<typename Key>
static uint32_t offsetOfKey(const Vector<Key>& ascendingKeys, const Vector<uint32_t>& offsets, const Key& key)
{
    auto found = std::ranges::lower_bound(ascendingKeys, key);
    RELEASE_ASSERT(found != ascendingKeys.end() && *found == key);
    return offsets[found - ascendingKeys.begin()];
}

namespace {

struct PendingRequest {
    ValueID value;
    Variant<RequestRecord, PlacedPropertyRun> record;
    Request request;
};

struct WordResolver {
    void fail(LayoutID layout, ValueID value) const { context.build.diagnostics().add(InternalError { .kind = InternalErrorKind::PlacementCheckFailed, .function = id, .value = value, .layout = layout }); }

    void resolve(ValueID valueID, Value& value)
    {
        if (Value::opcodesWithAllocation.contains(value.opcode))
            value.setAllocation(allocationOf(valueID, value));
        if (Value::opcodesWithRegExpRecord.contains(value.opcode)) {
            RegExpRecordID record = context.inputs.regExpRequests.recordOf(value.pattern(), toYarrFlags(value.regExpFlags()));
            if (!record)
                fail({ }, valueID);
            value.setRegExpRecord(record);
        }
        if (Value::opcodesWithRequest.contains(value.opcode)) {
            if (namesPropertyRun(value.opcode)) {
                if (auto run = propertyRunOf(context, id, valueID))
                    requests.append(PendingRequest { valueID, WTF::move(*run), { } });
            } else if (auto record = requestRecordOf(valueID, value))
                requests.append(PendingRequest { valueID, WTF::move(*record), { } });
        }
        if (Value::opcodesWithStaticsOffset.contains(value.opcode))
            value.setStaticsOffset(staticsOffsetOf(value));
        if (Value::opcodesWithOrderWord.contains(value.opcode))
            value.setOrderWord(orderWordOf(value));
        if (Value::opcodesWithGuardBit.contains(value.opcode))
            value.setGuardBit(steps.placement.guardBits().find(value.conditions()));
    }

    AllocationID allocationOf(ValueID valueID, const Value& value) const
    {
        if (value.opcode == Opcode::NewClosure) {
            switch (value.closureForm()) {
            case ClosureForm::Full:
                return steps.placement.closureAllocation(value.numberOfVariadicOperands());
            case ClosureForm::Slim:
            case ClosureForm::Deferred:
            case ClosureForm::Singleton:
                return { };
            }
            RELEASE_ASSERT_NOT_REACHED();
        }
        LayoutID layout = value.layout();
        const ShapeTable& shapes = steps.placement.shapes();
        AllocationID allocation = value.opcode == Opcode::NewObject
            ? steps.placement.allocations().find(layout, shapes.shapeOf(layout, value.allocationClass()), value.fields())
            : steps.placement.allocations().find(layout, shapes.generalShapeOf(layout), { });
        if (!allocation)
            fail(layout, valueID);
        return allocation;
    }

    std::optional<StaticsOffset> staticsOffsetOf(const Value& value) const
    {
        auto offsetAmong = [&](const Vector<SourceSite>& ascendingSites, const Vector<uint32_t>& offsets) {
            return StaticsOffset(offsetOfKey(ascendingSites, offsets, sourceSiteOf(context.program, id, value)));
        };
        switch (value.opcode) {
        case Opcode::NewArray:
            if (!hasArrayProfile(value.literalStorage()))
                return std::nullopt;
            return offsetAmong(steps.sites.profiledArrayLiterals, steps.literals.profiledLiteralOffsets);
        case Opcode::NewRegExp:
        case Opcode::SharedRegExpLiteral:
            return offsetAmong(steps.sites.regExpLiterals, steps.literals.regExpCellOffsets);
        case Opcode::NewTemplateObject:
            return offsetAmong(steps.sites.templateObjects, steps.literals.templateObjectOffsets);
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
    }

    std::optional<OrderWord> orderWordOf(const Value& value) const
    {
        auto& tests = steps.sites.keyOrderTests;
        LayoutID layout = value.layout();
        auto fields = value.fields();
        auto test = std::ranges::partition_point(tests, [&](const KeyOrderTestUse& candidate) {
            if (candidate.layout != layout)
                return candidate.layout < layout;
            return std::ranges::lexicographical_compare(candidate.fields, fields);
        });
        RELEASE_ASSERT(test != tests.end() && test->layout == layout && equalSpans(test->fields.span(), fields));
        std::optional<uint32_t> word = steps.orderWords.ofTest[test - tests.begin()];
        if (!word)
            return std::nullopt;
        return OrderWord(*word);
    }

    std::optional<RequestRecord> requestRecordOf(ValueID valueID, const Value& value) const
    {
        const Program& program = context.program;
        switch (value.opcode) {
        case Opcode::NewClass:
            return RequestRecord { NewClassRecord { value.layout() } };
        case Opcode::NewTemplateObject: {
            auto strings = value.templateStrings();
            return RequestRecord { TemplateObjectRecord { Vector<TemplateString>(strings.size(), [&](size_t index) {
                return strings[index];
            }) } };
        }
        case Opcode::DirectEval: {
            auto reportIfExceeded = [&](LimitID limit, uint64_t amount) {
                if (amount > context.build.limit(limit))
                    context.build.reportLimitExceeded(limit, amount, { .function = id, .value = valueID });
            };
            for (const EvalBinding& binding : program.at(value.evalScope()).bindings) {
                reportIfExceeded(LimitID::MaxEvalBindingDepth, binding.depth);
                reportIfExceeded(LimitID::MaxEvalBindingOffset, slotOffsetOf(binding));
            }
            return RequestRecord { DirectEvalRecord { value.evalScope(), program.sourceFunctionOf(*function, value).module } };
        }
        case Opcode::Data:
            return dataRecordOf(value);
        case Opcode::NewSetFromData:
        case Opcode::NewMapFromData:
            return RequestRecord { CollectionDataRecord { value.data(), value.opcode == Opcode::NewMapFromData } };
        case Opcode::NewFormatterWithPattern:
        case Opcode::FormatDateWithPattern:
        case Opcode::FormatNumberWithPattern:
            return RequestRecord { FormatPatternRecord { Vector<uint32_t>(value.formatPattern()) } };
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
    }

    RequestRecord dataRecordOf(const Value& value) const
    {
        DataID data = value.data();
        switch (value.encoding()) {
        case DataEncoding::ReadOnlyTree:
            return DataRecord { data, std::nullopt, std::nullopt };
        case DataEncoding::MadeOnce:
            return DataRecord { data, std::nullopt, std::nullopt, offsetOfKey(steps.sites.dataMadeOnce, steps.literals.cellOffsetsOfDataMadeOnce, data) };
        case DataEncoding::CopyOnWriteArray:
            if (isStorageOfArray(context.program.at(data).root.as<DataNode::Array>()))
                return DataRecord { data, std::nullopt, offsetOfKey(steps.sites.arraysWithStorageInImage, steps.literals.nodesOfStorageInImage, data) };
            return DataRecord { data, offsetOfKey(steps.sites.copyOnWriteArrays, steps.literals.sharedStorageOffsets, data), std::nullopt };
        case DataEncoding::Unselected:
        case DataEncoding::Expanded:
            break;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    // SlotData::create asserts what the limit reports, so the offset is read here without creating one.
    uint32_t slotOffsetOf(const EvalBinding& binding) const
    {
        if (binding.variable) {
            const PlacedVariable& variable = steps.placement.variable(binding.variable);
            return variable.hasWordInStatics() ? variable.offset : 0;
        }
        if (binding.kind == EvalBindingKind::MappedArgument) {
            RELEASE_ASSERT(binding.field);
            return *binding.field;
        }
        if (!binding.layout)
            return 0;
        RELEASE_ASSERT(binding.field);
        return steps.placement.slot(binding.layout, *binding.field).offset;
    }

    const PlacementContext& context;
    const SiteResolutionInputs& steps;
    FunctionID id;
    Function* function { nullptr };
    Vector<PendingRequest> requests;
};

} // anonymous namespace

void resolveFieldAccesses(const Placement& placement, Function& function, Vector<ValueID>& valuesWithOtherWords)
{
    auto fieldAccessOf = [&](PropertyKeyID key, LayoutSetID receivers, bool isStore) {
        FieldAccessID access = placement.findFieldAccess(key, receivers, isStore);
        RELEASE_ASSERT(access);
        return access;
    };
    for (auto& block : function.blocks) {
        for (ValueID id : block.values) {
            if (!opcodesWithWordsOfSites.contains(function.at(id).opcode))
                continue;
            Value& value = function.edit(id);
            if (Value::opcodesWithPlacedKeys.contains(value.opcode)) {
                auto placedKeys = value.placedKeys();
                Vector<PlacedKey, 8> keys(placedKeys.size(), [&](size_t index) {
                    PlacedKey placedKey = placedKeys[index];
                    placedKey.fieldAccess = fieldAccessOf(placedKey.key, value.receivers(), true);
                    return placedKey;
                });
                value.setPlacedKeys(keys.span());
            }
            if (Value::opcodesWithFieldAccess.contains(value.opcode))
                value.setFieldAccess(fieldAccessOf(value.key(), value.receivers(), isSlotStore(value.opcode)));
            if (opcodesWithOtherWords.contains(value.opcode))
                valuesWithOtherWords.append(id);
        }
    }
}

ResolvedSites resolveOtherWords(ProgramEditor& editor, const PlacementContext& context, const SiteResolutionInputs& steps, std::span<const Vector<ValueID>> valuesWithOtherWords)
{
    for (TerminatorKind kind : allMembersOf<TerminatorKind>) {
        for (const PayloadFieldInfo& field : payloadFieldsOf(kind))
            RELEASE_ASSERT(field.firstForm != IRForm::Placed);
    }
    for (Opcode opcode : allMembersOf<Opcode>) {
        for (const PayloadFieldInfo& field : payloadFieldsOf(opcode))
            RELEASE_ASSERT(field.firstForm != IRForm::Placed || isWordWrittenAtPlacement(opcode, field.kind));
    }
    const Program& program = context.program;
    const ParallelLoops& loops = context.build.parallelLoops();
    RELEASE_ASSERT(valuesWithOtherWords.size() == program.functions.size());
    Vector<WordResolver> functions;
    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        if (program.functions[index].hasBody() && !valuesWithOtherWords[index].isEmpty())
            functions.append(WordResolver { context, steps, FunctionID(index), &editor.atForPlacedWords(FunctionID(index)), { } });
    }
    forEachIndexInParallel(loops, functions.size(), [&](unsigned index) {
        WordResolver& resolver = functions[index];
        for (ValueID id : valuesWithOtherWords[resolver.id.index()])
            resolver.resolve(id, resolver.function->edit(id));
    });

    ResolvedSites resolved;
    for (WordResolver& resolver : functions) {
        for (PendingRequest& pending : resolver.requests) {
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
    forEachIndexInParallel(loops, functions.size(), [&](unsigned index) {
        for (PendingRequest& pending : functions[index].requests)
            functions[index].function->edit(pending.value).setRequest(pending.request);
    });
    return resolved;
}

static_assert(std::ranges::none_of(allMembersOf<TerminatorKind>, [](TerminatorKind kind) {
    return siteCachesOf(kind) > 0;
}));
static_assert(std::ranges::all_of(allMembersOf<Opcode>, [](Opcode opcode) {
    return !siteCachesOf(opcode) || (siteCachesOf(opcode) == 1 && Value::opcodesWithSiteCache.contains(opcode));
}));

// A module body runs again only after its entry left the cache of `require`.
bool mayReadSiteCachesAgain(FunctionAnalyses& analyses, BlockID block)
{
    if (analyses.executionCountOf(block) == ExecutionCount::AtMostOnce)
        return false;
    const AnalysisContext& context = analyses.context();
    return !context.program.at(context.id).isModuleBody() || analyses.isInCycle(block);
}

// The question ahead of a loop speaks for all its turns, however seldom it is asked.
bool usesItsSiteCaches(FunctionAnalyses& analyses, const Value& value, BlockID block)
{
    return value.opcode == Opcode::GetCachedValue || mayReadSiteCachesAgain(analyses, block);
}

// FieldRecordIndex::isStoredWithoutTypeTest: the stubs by name store into such a slot in fewer instructions than PutKnownField, and file nothing either.
static bool isStoredThroughTableOfFields(const Program& program, const Field& declaration, SlotRepresentation representation)
{
    const Type& held = program.at(slotTypeOf(program, declaration));
    switch (representation) {
    case SlotRepresentation::JSValue:
        return held.isAnything();
    case SlotRepresentation::Int32:
    case SlotRepresentation::OptionalInt32:
        return held.primitives & Primitive::Integer && held.integers.minimum <= minInlineInt32 && held.integers.maximum >= INT32_MAX;
    case SlotRepresentation::Cell:
    case SlotRepresentation::Cell32:
    case SlotRepresentation::OptionalCell32:
    case SlotRepresentation::Uint32:
    case SlotRepresentation::Int64:
    case SlotRepresentation::OptionalInt64:
    case SlotRepresentation::Double:
    case SlotRepresentation::OptionalDouble:
    case SlotRepresentation::Float:
    case SlotRepresentation::Boolean:
    case SlotRepresentation::OptionalBoolean:
    case SlotRepresentation::Uint8:
    case SlotRepresentation::Int8:
    case SlotRepresentation::Uint16:
    case SlotRepresentation::Int16:
    case SlotRepresentation::LiteralString:
    case SlotRepresentation::LiteralString32:
    case SlotRepresentation::DeferredClosure:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool storedFieldTakesAnyValue(const Program& program, const Placement& placement, const Function& function, const Value& store)
{
    return std::ranges::all_of(membersOf(program, program.typeOf(function, store.objectOperand())), [&](LayoutID member) {
        uint32_t field = *program.at(member).findField(store.key());
        const PlacedSlot& slot = placement.layout(member).slots[field];
        return slot.representation == SlotRepresentation::JSValue && !slot.isOutOfLine() && isStoredThroughTableOfFields(program, fieldAt(program, declarationOf(program, { member, field })), slot.representation);
    });
}

bool storedFieldAcceptsAnyNumber(const Program& program, const Placement& placement, const Function& function, const Value& store)
{
    return std::ranges::all_of(membersOf(program, program.typeOf(function, store.objectOperand())), [&](LayoutID member) {
        uint32_t field = *program.at(member).findField(store.key());
        const PlacedSlot& slot = placement.layout(member).slots[field];
        const Type& held = program.at(slotTypeOf(program, fieldAt(program, declarationOf(program, { member, field }))));
        bool holdsDouble = slot.representation == SlotRepresentation::Double || slot.representation == SlotRepresentation::OptionalDouble;
        return holdsDouble && !slot.isOutOfLine() && (held.primitives & Primitive::Number) == Primitive::Number && held.integers == Range::full();
    });
}

std::optional<uint32_t> findRecordOfStoredField(const Program& program, const Placement& placement, const Function& function, const Value& store, NameHasColumn nameHasColumn)
{
    if ((store.opcode != Opcode::PropSet || store.isFreeName()) && store.opcode != Opcode::PropDefine)
        return std::nullopt;
    const Type& receiver = program.typeOf(function, store.objectOperand());
    if (!receiver.isObject() || !receiver.layouts || !receiver.hasOnlyLayouts())
        return std::nullopt;
    std::optional<uint16_t> index;
    for (LayoutID member : membersOf(program, receiver)) {
        const PlannedLayout& planned = placement.layout(member);
        std::optional<uint32_t> field = program.at(member).findField(store.key());
        if (!planned.holdsCompiledObjects() || !field)
            return std::nullopt;
        const Field& declaration = fieldAt(program, declarationOf(program, { member, *field }));
        const PlacedSlot& slot = planned.slots[*field];
        if (!declaration.form.isEmpty() || declaration.flags.contains(FieldFlags::IsPrivate) || !slot.hasStorage() || slot.representation == SlotRepresentation::DeferredClosure || !slot.indexInLayoutRecord)
            return std::nullopt;
        // A definition mostly creates the field, asks nothing of what is above the object, and has no site: told the record, a stub does in half the instructions what the way by name does.
        bool isDefinitionOfBoxedValue = store.opcode == Opcode::PropDefine && slot.representation == SlotRepresentation::JSValue && !slot.isOutOfLine();
        if ((nameHasColumn == NameHasColumn::Yes && !isDefinitionOfBoxedValue && isStoredThroughTableOfFields(program, declaration, slot.representation)) || (index && *index != *slot.indexInLayoutRecord))
            return std::nullopt;
        index = slot.indexInLayoutRecord;
    }
    if (!index)
        return std::nullopt;
    return static_cast<uint32_t>(sizeof(LayoutData) + *index * sizeof(FieldData));
}

static unsigned numberOfSiteCachesUsedByOperationWithRequest(const Program& program, const Placement& placement, const Function& function, const Value& value)
{
    if (findRecordOfStoredField(program, placement, function, value, NameHasColumn::Yes))
        return 0;
    if (value.opcode == Opcode::PropGet && value.asksOnlyTables())
        return 0;
    // Flow has seen compiled objects come to this read. The table of fields answers for all their layouts alike, and the stub without a site asks it first: a site in front of it would stay empty.
    // A name that none of the layouts has is found above the object, and the table has nothing to say.
    if (value.opcode == Opcode::PropGet && !value.isFreeName() && value.receivers() && program.nameClassOf(value.key()) != NameClass::Length) {
        bool isFieldOfReceiver = std::ranges::any_of(program.at(value.receivers()).seen().predicted(), [&](LayoutID member) {
            return !builtinBaseClassOf(program, member) && findField(program, member, value.key(), BaseChain::Proven);
        });
        if (isFieldOfReceiver)
            return 0;
    }
    // The slot of the field was found empty, so the table of fields has nothing to say: `Message.prototype.retries = 3` answers for most objects.
    if (value.opcode == Opcode::SuperGet && value.key() && value.baseOperand() == value.receiverOperand())
        return 1;
    return siteCachesOf(value.opcode);
}

unsigned numberOfSiteCachesUsedBy(const Program& program, const Placement& placement, const Function& function, const Value& value)
{
    if (value.opcode == Opcode::TryPutByIds && !value.request())
        return 0;
    return numberOfSiteCachesUsedByOperationWithRequest(program, placement, function, value);
}

SiteCachePosition siteCachePositionOf(const Program& program, FunctionID function, ValueID value)
{
    FunctionID source = firstSourceOf(program, function);
    ModuleID module = program.at(source).module;
    return { module ? module.index() : UINT32_MAX, source.index(), function.index(), value.index() };
}

void collectSiteCacheRequests(const Program& program, const Placement& placement, const FlowResults& flow, const Heat& heat, BuildContext& build, FunctionID id, bool needsWeights, Vector<SiteCacheRequest>& requests)
{
    const Function& function = program.at(id);
    std::optional<FunctionAnalyses> analyses;
    std::optional<FunctionSiteWeights> weights;
    for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        BlockID block(blockIndex);
        for (ValueID valueID : function.blocks[blockIndex].values) {
            const Value& value = function.at(valueID);
            if ((value.opcode == Opcode::TryPutByIds && !hasPropertyRun(program, build, value)) || !numberOfSiteCachesUsedByOperationWithRequest(program, placement, function, value))
                continue;
            if (!analyses)
                analyses.emplace(AnalysisContext { build, program, &flow, nullptr, id }, function);
            if (!usesItsSiteCaches(*analyses, value, block)) {
                requests.append(SiteCacheRequest { 0, id, valueID, true });
                continue;
            }
            if (needsWeights && !weights)
                weights.emplace(program, heat, id, *analyses);
            requests.append(SiteCacheRequest { needsWeights ? weights->weightOf(block) : SiteWeight { 0 }, id, valueID });
        }
    }
}

static bool isSameOperationAtSamePlace(const Value& value, const Value& other)
{
    return value.opcode == other.opcode && value.hasPosition() && value.position == other.position && value.inlineFrame == other.inlineFrame;
}

bool mayShareSiteCache(const Function& function, const Value& value, const Value& other)
{
    if (!isSameOperationAtSamePlace(value, other) || !value.hasSamePayloadAs(other) || !value.numberOfOperands() || value.numberOfOperands() != other.numberOfOperands())
        return false;
    ValueID receiver = sameObjectThroughCasts(function, value.operands()[0]).valueInFrontOfFacts();
    ValueID otherReceiver = sameObjectThroughCasts(function, other.operands()[0]).valueInFrontOfFacts();
    if (receiver == otherReceiver)
        return true;
    const Value& definition = function.at(receiver);
    const Value& otherDefinition = function.at(otherReceiver);
    if (isRematerializable(definition))
        return definition.opcode == otherDefinition.opcode && definition.hasSamePayloadAs(otherDefinition);
    return definition.opcode != Opcode::Parameter && isSameOperationAtSamePlace(definition, otherDefinition);
}

static void findRequestsThatShareCache(const Function& function, std::span<SiteCacheRequest> requests)
{
    Vector<uint32_t, 16> byOrigin;
    for (uint32_t index = 0; index < requests.size(); ++index) {
        if (!requests[index].runsAtMostOnce && function.at(requests[index].value).hasPosition())
            byOrigin.append(index);
    }
    if (byOrigin.size() < 2)
        return;
    // All that two requests have in common before they are compared. The reads of one object pattern stand at one place, and each has a name of its own.
    auto originOf = [&](uint32_t index) {
        const Value& value = function.at(requests[index].value);
        return std::tuple { value.position, value.inlineFrame, value.opcode, value.flags, value.inlinePayload() };
    };
    AOT::sort(byOrigin, { }, [&](uint32_t index) {
        return std::tuple_cat(originOf(index), std::tuple { requests[index].value });
    });
    Vector<uint32_t, 4> owners;
    for (size_t position = 0; position < byOrigin.size(); ++position) {
        if (position && originOf(byOrigin[position]) != originOf(byOrigin[position - 1]))
            owners.shrink(0);
        SiteCacheRequest& request = requests[byOrigin[position]];
        auto owner = std::ranges::find_if(owners, [&](uint32_t index) {
            return mayShareSiteCache(function, function.at(requests[index].value), function.at(request.value));
        });
        if (owner == owners.end()) {
            owners.append(byOrigin[position]);
            continue;
        }
        request.sharesCacheOf = requests[*owner].value;
        if ((request.weight > requests[*owner].weight).predicted())
            requests[*owner].weight = request.weight;
    }
}

static void sortByPosition(const Program& program, const ParallelLoops& loops, std::span<SiteCacheRequest> requests)
{
    Vector<std::pair<SiteCachePosition, SiteCacheRequest>> positioned(requests.size());
    forEachIndexInParallel(loops, requests.size(), [&](unsigned index) {
        positioned[index] = { siteCachePositionOf(program, requests[index].function, requests[index].value), requests[index] };
    });
    parallelSort(loops, positioned.mutableSpan(), [](const auto& a, const auto& b) {
        return a.first < b.first;
    });
    forEachIndexInParallel(loops, requests.size(), [&](unsigned index) {
        requests[index] = positioned[index].second;
    });
}

SiteCaches placeSiteCaches(ProgramEditor& editor, const Placement& placement, const FlowResults& flow, const Heat& heat, BuildContext& build, std::span<const Vector<SiteCacheRequest>> requestsOfFunctions)
{
    PhaseTimes times(build, PassID::PlaceSiteCaches);
    const Program& program = editor.program();
    const ParallelLoops& loops = build.parallelLoops();

    uint32_t numberOfSitesWithoutCache = 0;
    Vector<SiteCacheRequest> requestsThatShareCache;
    auto collectRequests = [&](bool needsWeights) {
        auto requests = collectInParallel<SiteCacheRequest>(loops, program.functions.size(), [&](unsigned index, Vector<SiteCacheRequest>& found) {
            if (!program.functions[index].hasBody())
                return;
            size_t numberFoundBefore = found.size();
            if (requestsOfFunctions.empty())
                collectSiteCacheRequests(program, placement, flow, heat, build, FunctionID(index), needsWeights, found);
            else
                found.appendVector(requestsOfFunctions[index]);
            findRequestsThatShareCache(program.functions[index], found.mutableSpan().subspan(numberFoundBefore));
        });
        numberOfSitesWithoutCache = static_cast<uint32_t>(requests.removeAllMatching([](const SiteCacheRequest& request) {
            return request.runsAtMostOnce;
        }));
        requestsThatShareCache.shrink(0);
        for (const SiteCacheRequest& request : requests) {
            if (request.sharesCacheOf)
                requestsThatShareCache.append(request);
        }
        requests.removeAllMatching([](const SiteCacheRequest& request) {
            return !!request.sharesCacheOf;
        });
        return requests;
    };
    // Where every request is served, the order by position below is the only one that anybody sees.
    auto requests = collectRequests(false);
    if (requests.size() > build.limit(LimitID::MaxSiteCaches)) {
        requests = collectRequests(true);
        parallelSort(loops, requests.mutableSpan(), [](const SiteCacheRequest& a, const SiteCacheRequest& b) {
            if (!a.weight.isSamePredictionAs(b.weight))
                return (a.weight > b.weight).predicted();
            return std::tie(a.function, a.value) < std::tie(b.function, b.value);
        });
    }

    times.didFinish("collect requests"_s);
    auto count = static_cast<uint32_t>(std::min<uint64_t>(requests.size(), build.limit(LimitID::MaxSiteCaches)));
    sortByPosition(program, loops, requests.mutableSpan().first(count));
    times.didFinish("sortByPosition"_s);
    // The position of a request begins with what its function has alone, so the requests of a function stand together.
    std::span<const SiteCacheRequest> servedRequests = requests.span().first(count);
    auto groups = groupSitesByFunction(servedRequests);
    forEachIndexInParallel(loops, groups.size(), [&](unsigned group) {
        Function& function = editor.atForPlacedWords(groups[group].front().function);
        for (const SiteCacheRequest& request : groups[group])
            function.edit(request.value, ValueEdit::Definition).setSiteCache(SiteCacheID(static_cast<uint32_t>(&request - servedRequests.data())));
    });
    std::atomic<uint32_t> numberOfUnservedCopies { 0 };
    auto groupsThatShareCache = groupSitesByFunction(requestsThatShareCache.span());
    forEachIndexInParallel(loops, groupsThatShareCache.size(), [&](unsigned group) {
        Function& function = editor.atForPlacedWords(groupsThatShareCache[group].front().function);
        for (const SiteCacheRequest& request : groupsThatShareCache[group]) {
            if (SiteCacheID cache = std::as_const(function).at(request.sharesCacheOf).siteCache()) {
                function.edit(request.value, ValueEdit::Definition).setSiteCache(cache);
                continue;
            }
            build.gaveUp(LimitID::MaxSiteCaches, { .function = request.function, .value = request.value });
            numberOfUnservedCopies.fetch_add(1, std::memory_order_relaxed);
        }
    });
    times.didFinish("writeSiteCaches"_s);

    auto unservedRequests = requests.span().subspan(count);
    forEachIndexInParallel(loops, unservedRequests.size(), [&](unsigned index) {
        build.gaveUp(LimitID::MaxSiteCaches, { .function = unservedRequests[index].function, .value = unservedRequests[index].value });
    });

    numberOfSitesWithoutCache += static_cast<uint32_t>(unservedRequests.size()) + numberOfUnservedCopies.load(std::memory_order_relaxed);
    build.statistics().count(StatisticID::SiteCachesPlaced, count);
    build.statistics().count(StatisticID::SitesWithoutSiteCache, numberOfSitesWithoutCache);
    return SiteCaches(count, numberOfSitesWithoutCache);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
