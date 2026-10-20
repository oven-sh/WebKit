/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTProgramTableEncoder.h"

#if ENABLE(AOT)

#include <algorithm>
#include <optional>
#include <span>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint32_t evalOperandsBeforeEnvironments = 2;

namespace {

struct EvalBindingStorage {
    uint32_t operand { EvalBindingData::noOperand };
    SlotData slot { SlotData::create(0, SlotRepresentation::JSValue) };
    TypeID type { };
    uint32_t privateKey { 0 };
};

} // anonymous namespace

static void encodeNewClass(const NewClassRecord& record, std::span<uint8_t>& bytes)
{
    consumeAndReinterpretCastTo<NewClassRequest>(bytes).classIndex = record.classLayout.index();
}

static void encodeCloneAs(const TableEncoder& encoder, const CloneAsRecord& record, std::span<uint8_t>& bytes)
{
    auto& request = consumeAndReinterpretCastTo<CloneAsRequest>(bytes);
    request.shape = record.shape;
    request.count = static_cast<uint32_t>(record.fields.size());
    for (FieldIndex field : record.fields) {
        std::optional<uint16_t> index = encoder.slots.slot(record.layout, field).indexInLayoutRecord;
        RELEASE_ASSERT(index);
        consumeAndReinterpretCastTo<uint32_t>(bytes) = *index;
    }
}

static void encodeTemplateObject(TableEncoder& encoder, const TemplateObjectRecord& record, std::span<uint8_t>& bytes)
{
    consumeAndReinterpretCastTo<TemplateObjectRequest>(bytes).count = static_cast<uint32_t>(record.parts.size());
    for (const TemplateString& string : record.parts) {
        auto& part = consumeAndReinterpretCastTo<TemplateObjectRequest::Part>(bytes);
        part.cookedPlusOne = string.cooked ? encoder.stringConstantNumber(string.cooked) + 1 : 0;
        part.raw = encoder.stringConstantNumber(string.raw);
    }
}

static void setDirectEvalFlag(DirectEvalRequest& request, EvalScopeFlags flag)
{
    switch (flag) {
    case EvalScopeFlags::IsInFunction:
        request.isInFunction = true;
        return;
    case EvalScopeFlags::IsInArrowFunction:
        request.isInArrowFunction = true;
        return;
    case EvalScopeFlags::IsInFieldInitializer:
        request.isInFieldInitializer = true;
        return;
    case EvalScopeFlags::IsInDerivedConstructor:
        request.isInDerivedConstructor = true;
        return;
    case EvalScopeFlags::AllowsSuperProperty:
        request.allowsSuperProperty = true;
        return;
    case EvalScopeFlags::AllowsSuperCall:
        request.allowsSuperCall = true;
        return;
    case EvalScopeFlags::AllowsNewTarget:
        request.allowsNewTarget = true;
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static uint32_t evalOperandOf(std::optional<uint32_t> environmentOperand)
{
    return environmentOperand ? evalOperandsBeforeEnvironments + *environmentOperand : EvalBindingData::noOperand;
}

static EvalBindingStorage storageOf(const TableEncoder& encoder, const EvalBinding& binding)
{
    const Program& program = encoder.program;
    if (binding.variable)
        return { .slot = placedVariableSlot(encoder, binding.variable), .type = program.at(binding.variable).declared.type };
    RELEASE_ASSERT(binding.operand);
    uint32_t operand = evalOperandOf(binding.operand);
    if (binding.kind == EvalBindingKind::MappedArgument) {
        RELEASE_ASSERT(binding.field);
        return { .operand = operand, .slot = SlotData::create(*binding.field, SlotRepresentation::JSValue) };
    }
    if (!binding.layout)
        return { .operand = operand };
    RELEASE_ASSERT(binding.field);
    FieldReference field { binding.layout, *binding.field };
    const Field& declaration = fieldAt(program, declarationOf(program, field));
    const PlacedSlot& slot = encoder.slots.slot(field);
    if (program.at(binding.layout).isEnvironment()) {
        RELEASE_ASSERT(slot.hasStorage());
        return { .operand = operand, .slot = slot.data(), .type = declaration.declared.type };
    }
    // A private name of instances is found by its key. A class that is never allocated has no slot for it.
    return { .operand = operand, .slot = slot.data(), .type = declaration.declared.type, .privateKey = encoder.identifierNumber(declaration.key) };
}

static void encodeEvalBinding(const TableEncoder& encoder, const EvalBinding& binding, EvalBindingData& data)
{
    EvalBindingStorage storage = storageOf(encoder, binding);
    data.name = binding.name ? encoder.identifierNumberOfText(binding.name) : 0;
    data.kind = static_cast<uint32_t>(binding.kind);
    data.depth = binding.depth;
    data.operand = storage.operand;
    data.function = evalOperandOf(binding.function);
    data.setter = evalOperandOf(binding.setter);
    data.privateKey = storage.privateKey;
    data.slot = storage.slot;
    data.type = encoder.typeOffset(storage.type);
}

static void encodeDirectEval(const TableEncoder& encoder, const DirectEvalRecord& record, std::span<uint8_t>& bytes)
{
    const EvalScope& scope = encoder.program.at(record.scope);
    auto& request = consumeAndReinterpretCastTo<DirectEvalRequest>(bytes);
    request.isStrict = scope.mode == ECMAMode::Strict;
    for (EvalScopeFlags flag : scope.flags)
        setDirectEvalFlag(request, flag);
    request.module = record.module.index();
    request.numberOfBindings = static_cast<uint32_t>(scope.bindings.size());
    Vector<uint32_t> names;
    for (const EvalBinding& binding : scope.bindings) {
        auto& data = consumeAndReinterpretCastTo<EvalBindingData>(bytes);
        encodeEvalBinding(encoder, binding, data);
        names.append(data.name);
    }
    Vector<uint32_t> bindingsByName(names.size(), [](size_t index) {
        return static_cast<uint32_t>(index);
    });
    std::ranges::stable_sort(bindingsByName, { }, [&](uint32_t index) {
        return names[index];
    });
    for (uint32_t index : bindingsByName)
        consumeAndReinterpretCastTo<uint32_t>(bytes) = index;
}

static void encodeData(const TableEncoder& encoder, const DataRecord& record, std::span<uint8_t>& bytes)
{
    auto& request = consumeAndReinterpretCastTo<DataRequest>(bytes);
    request.root = encoder.dataNodeOf(record.data);
    request.sharedStorageOffset = record.sharedStorageOffset.value_or(DataRequest::noSharedStorage);
}

static void encodeCollectionData(const TableEncoder& encoder, const CollectionDataRecord& record, std::optional<uint32_t> tableInImage, std::span<uint8_t>& bytes)
{
    auto& request = consumeAndReinterpretCastTo<CollectionDataRequest>(bytes);
    if (tableInImage) {
        request.tableInImage = *tableInImage;
        return;
    }
    request.elements = encoder.dataNodeOf(record.data);
}

void encodeRequestTable(TableEncoder& encoder)
{
    const RequestTable& requests = encoder.placement.requests();
    std::span<const RequestRecord> records = requests.records();
    Vector<uint32_t> words(FillWith { }, requests.sizeInBytes() / sizeof(uint32_t), 0);
    for (unsigned index = 0; index < records.size(); ++index) {
        const RequestRecord& request = records[index];
        std::span<uint8_t> bytes = asMutableByteSpan(words.mutableSpan()).subspan(requests.offsetOf(index), sizeInBytesOf(encoder.program, request));
        WTF::switchOn(request,
            [&](const NewClassRecord& record) {
                encodeNewClass(record, bytes);
            },
            [&](const CloneAsRecord& record) {
                encodeCloneAs(encoder, record, bytes);
            },
            [&](const TemplateObjectRecord& record) {
                encodeTemplateObject(encoder, record, bytes);
            },
            [&](const DirectEvalRecord& record) {
                encodeDirectEval(encoder, record, bytes);
            },
            [&](const DataRecord& record) {
                encodeData(encoder, record, bytes);
            },
            [&](const CollectionDataRecord& record) {
                encodeCollectionData(encoder, record, encoder.tablesInImage[index], bytes);
            });
        RELEASE_ASSERT(bytes.empty());
    }
    encoder.setTable(ProgramTableIndex::requests, words);
}

void encodePropertyRunTable(TableEncoder& encoder)
{
    std::span<const PlacedPropertyRun> runs = encoder.placement.propertyRuns().runs();
    size_t numberOfWords = runs.size() + 1;
    for (const PlacedPropertyRun& run : runs)
        numberOfWords += 1 + run.keys.size();
    RELEASE_ASSERT(numberOfWords <= UINT32_MAX);

    Vector<uint32_t> words;
    words.reserveInitialCapacity(numberOfWords);
    uint32_t start = static_cast<uint32_t>(runs.size() + 1);
    for (const PlacedPropertyRun& run : runs) {
        words.append(start);
        start += static_cast<uint32_t>(1 + run.keys.size());
    }
    words.append(start);

    for (const PlacedPropertyRun& run : runs) {
        PropertyRunHeader header { };
        header.count = static_cast<uint32_t>(run.keys.size());
        RELEASE_ASSERT(header.count == run.keys.size());
        words.append(header.encoded());
        for (PropertyKeyID key : run.keys)
            words.append(encoder.identifierNumber(key));
    }
    RELEASE_ASSERT(words.size() == numberOfWords);
    encoder.setTable(ProgramTableIndex::propertyRuns, words);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
