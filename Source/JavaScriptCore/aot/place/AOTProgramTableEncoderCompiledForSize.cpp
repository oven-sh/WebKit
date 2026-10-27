/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTProgramTableEncoder.h"

#if ENABLE(AOT)

#include "AOTOperationData.h"
#include "AOTPlacementSteps.h"
#include "AOTSort.h"
#include "AOTVector.h"
#include <algorithm>
#include <wtf/BitVector.h>
#include <wtf/Hasher.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

template<typename... Records>
static constexpr bool areWordMultiples = (!(sizeof(Records) % sizeof(uint32_t)) && ...);

static_assert(areWordMultiples<NewClassRequest, TemplateObjectRequest, TemplateObjectRequest::Part, DirectEvalRequest, EvalBindingData, DataRequest, CollectionDataRequest>);
static_assert(!(offsetOfFirstRequest % sizeof(uint32_t)));

uint32_t sizeInBytesOf(const Program& program, const RequestRecord& request)
{
    size_t size = WTF::switchOn(request,
        [](const NewClassRecord&) -> size_t {
            return sizeof(NewClassRequest);
        },
        [](const TemplateObjectRecord& record) -> size_t {
            return sizeof(TemplateObjectRequest) + sizeof(TemplateObjectRequest::Part) * record.parts.size();
        },
        [&](const DirectEvalRecord& record) -> size_t {
            return sizeof(DirectEvalRequest) + (sizeof(EvalBindingData) + sizeof(uint32_t)) * program.at(record.scope).bindings.size();
        },
        [](const DataRecord&) -> size_t {
            return sizeof(DataRequest);
        },
        [](const CollectionDataRecord&) -> size_t {
            return sizeof(CollectionDataRequest);
        });
    RELEASE_ASSERT(size <= UINT32_MAX);
    return static_cast<uint32_t>(size);
}

Request RequestTable::ensure(const Program& program, RequestRecord&& request)
{
    unsigned hash = computeHash(request);
    std::optional<uint32_t> equalRequest = m_index.find(hash, [&](uint32_t position) {
        return m_records[position] == request;
    });
    if (equalRequest) {
        ++m_numberOfSharedRequests;
        return Request(m_offsets[*equalRequest]);
    }

    uint32_t offset = m_sizeInBytes;
    uint64_t end = static_cast<uint64_t>(offset) + sizeInBytesOf(program, request);
    RELEASE_ASSERT(end <= UINT32_MAX);
    m_index.add(hash, static_cast<uint32_t>(m_records.size()));
    m_records.append(WTF::move(request));
    m_offsets.append(offset);
    m_sizeInBytes = static_cast<uint32_t>(end);
    return Request(offset);
}

const RequestRecord* RequestTable::recordAt(Request request) const
{
    auto found = std::ranges::lower_bound(m_offsets, request.value());
    if (found == m_offsets.end() || *found != request.value())
        return nullptr;
    return &m_records[found - m_offsets.begin()];
}

Request PlacedPropertyRuns::ensure(PlacedPropertyRun&& run)
{
    unsigned hash = computeHash(run.keys);
    std::optional<uint32_t> equalRun = m_index.find(hash, [&](uint32_t position) {
        return m_runs[position] == run;
    });
    if (equalRun)
        return Request(*equalRun);

    uint32_t position = static_cast<uint32_t>(m_runs.size());
    m_index.add(hash, position);
    m_runs.append(WTF::move(run));
    return Request(position);
}

void collectStringConstantsOf(const Program& program, const RequestRecord& request, BitVector& stringConstants)
{
    WTF::switchOn(request,
        [](const NewClassRecord&) { },
        [&](const TemplateObjectRecord& record) {
            for (const TemplateString& part : record.parts) {
                stringConstants.set(part.raw.index());
                if (part.cooked)
                    stringConstants.set(part.cooked.index());
            }
        },
        [](const DirectEvalRecord&) { },
        [&](const DataRecord& record) {
            collectStringConstantsOf(program, program.at(record.data).root, stringConstants);
        },
        [&](const CollectionDataRecord& record) {
            collectStringConstantsOf(program, program.at(record.data).root, stringConstants);
        });
}

void collectStringConstantsOf(const Program&, const DataNode& root, BitVector& stringConstants)
{
    Vector<const DataNode*, 16> nodesToVisit { &root };
    while (!nodesToVisit.isEmpty()) {
        const DataNode& node = *nodesToVisit.takeLast();
        switch (node.kind()) {
        case DataNode::Kind::Undefined:
        case DataNode::Kind::Null:
        case DataNode::Kind::Hole:
        case DataNode::Kind::Boolean:
        case DataNode::Kind::Number:
        case DataNode::Kind::BigInt:
            break;
        case DataNode::Kind::String:
            stringConstants.set(node.as<DataNode::String>().value.index());
            break;
        case DataNode::Kind::Array:
            for (const DataNode& element : node.as<DataNode::Array>().elements)
                nodesToVisit.append(&element);
            break;
        case DataNode::Kind::Object:
            for (const DataMember& member : node.as<DataNode::Object>().members)
                nodesToVisit.append(&member.value);
            break;
        }
    }
}

static void encodeRequest(TableEncoder&, const NewClassRecord& record, std::span<uint8_t>& bytes)
{
    consumeAndReinterpretCastTo<NewClassRequest>(bytes).classIndex = record.classLayout.index();
}

static void encodeRequest(TableEncoder& encoder, const TemplateObjectRecord& record, std::span<uint8_t>& bytes)
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

// The key of the field of an instance that a private name stands for.
static PropertyKeyID privateKeyOf(const Program& program, const EvalBinding& binding)
{
    if (binding.variable || !binding.layout || binding.kind == EvalBindingKind::MappedArgument || program.at(binding.layout).isEnvironment())
        return { };
    return fieldAt(program, declarationOf(program, { binding.layout, *binding.field })).key;
}

static void encodeEvalBinding(const TableEncoder& encoder, const EvalBinding& binding, ModuleID module, EvalBindingData& data)
{
    constexpr uint32_t operandsBeforeEnvironments = 2;
    const Program& program = encoder.program;
    auto operandOf = [](std::optional<uint32_t> environmentOperand) {
        return environmentOperand ? operandsBeforeEnvironments + *environmentOperand : EvalBindingData::noOperand;
    };
    PropertyKeyID privateKey = privateKeyOf(program, binding);
    data.name = binding.name ? encoder.identifierNumberOfText(binding.name, "what a direct eval sees in"_s, module) : 0;
    data.privateKey = privateKey ? encoder.identifierNumber(privateKey, "the key of a private name that a direct eval sees in"_s, module) : 0;
    data.kind = static_cast<uint32_t>(binding.kind);
    data.depth = binding.depth;
    data.operand = EvalBindingData::noOperand;
    data.function = operandOf(binding.function);
    data.setter = operandOf(binding.setter);
    data.slot = SlotData::create(0, SlotRepresentation::JSValue);
    TypeID type;
    if (binding.variable) {
        data.slot = placedVariableSlot(encoder, binding.variable);
        type = program.at(binding.variable).declared.written();
    } else {
        RELEASE_ASSERT(binding.operand);
        data.operand = operandOf(binding.operand);
        RELEASE_ASSERT(binding.field || (binding.kind != EvalBindingKind::MappedArgument && !binding.layout));
        if (binding.kind == EvalBindingKind::MappedArgument)
            data.slot = SlotData::create(*binding.field, SlotRepresentation::JSValue);
        else if (binding.layout) {
            FieldReference field { binding.layout, *binding.field };
            const Field& declaration = fieldAt(program, declarationOf(program, field));
            const PlacedSlot& slot = encoder.slots.slot(field);
            RELEASE_ASSERT(!program.at(binding.layout).isEnvironment() || slot.hasStorage());
            data.slot = slot.data();
            type = declaration.declared.written();
        }
    }
    data.type = encoder.typeOffset(type);
}

static void encodeRequest(TableEncoder& encoder, const DirectEvalRecord& record, std::span<uint8_t>& bytes)
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
        encodeEvalBinding(encoder, binding, record.module, data);
        names.append(data.name);
    }
    Vector<uint32_t> bindingsByName(names.size(), [](size_t index) {
        return static_cast<uint32_t>(index);
    });
    AOT::stableSort(bindingsByName, { }, [&](uint32_t index) {
        return names[index];
    });
    for (uint32_t index : bindingsByName)
        consumeAndReinterpretCastTo<uint32_t>(bytes) = index;
}

static void encodeRequest(TableEncoder& encoder, const DataRecord& record, std::span<uint8_t>& bytes)
{
    auto& request = consumeAndReinterpretCastTo<DataRequest>(bytes);
    request.root = encoder.dataNodeOf(record.data);
    request.sharedStorageOffset = record.sharedStorageOffset.value_or(DataRequest::noSharedStorage);
}

void encodeRequestTable(TableEncoder& encoder)
{
    const RequestTable& requests = encoder.placement.requests();
    std::span<const RequestRecord> records = requests.records();
    Vector<uint32_t> words(FillWith { }, requests.sizeInBytes() / sizeof(uint32_t), 0);
    for (unsigned index = 0; index < records.size(); ++index) {
        std::span<uint8_t> bytes = asMutableByteSpan(words.mutableSpan()).subspan(requests.offsetOf(index), sizeInBytesOf(encoder.program, records[index]));
        WTF::switchOn(records[index],
            [&](const CollectionDataRecord& record) {
                auto& request = consumeAndReinterpretCastTo<CollectionDataRequest>(bytes);
                if (std::optional<uint32_t> tableInImage = encoder.tablesInImage[index])
                    request.tableInImage = *tableInImage;
                else
                    request.elements = encoder.dataNodeOf(record.data);
            },
            [&](const auto& record) {
                encodeRequest(encoder, record, bytes);
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
            words.append(encoder.identifierNumber(key, "in a run of properties stored together"_s));
    }
    RELEASE_ASSERT(words.size() == numberOfWords);
    encoder.setTable(ProgramTableIndex::propertyRuns, words);
}

void encodeMethodTable(TableEncoder& encoder)
{
    const MethodTable& table = encoder.placement.methodTable();
    encoder.setTableFrom<uint32_t>(ProgramTableIndex::methodTableFunctions, table.entries, [](FunctionID function) {
        return function ? function.index() : MethodTableData::noFunction;
    });
    encoder.setTableFrom<uint32_t>(ProgramTableIndex::methodLookupFunctions, table.selectors, [](const auto& selector) {
        return selector.lookup.index();
    });
    Vector<MethodTableDependentData> dependents(table.dependents.size(), [&](size_t index) {
        const MethodTableDependent& dependent = table.dependents[index];
        return MethodTableDependentData { numberInStaticsOf(encoder.placement.guardBit(dependent.guard)), dependent.entry, dependent.selector };
    });
    AOT::sort(dependents);
    encoder.setTable(ProgramTableIndex::methodTableDependents, dependents);
}

static Vector<StaticsRange> joinAdjacentRoots(std::span<const uint32_t> ascendingOffsets, uint32_t width)
{
    Vector<StaticsRange> ranges;
    for (uint32_t offset : ascendingOffsets) {
        RELEASE_ASSERT(!(offset % width));
        if (!ranges.isEmpty() && ranges.last().end() == offset)
            ranges.last().size += width;
        else
            ranges.append({ offset, width });
    }
    return ranges;
}

void encodeStaticsTables(TableEncoder& encoder)
{
    struct InitialWord {
        uint32_t index;
        uint64_t bits;
    };
    constexpr unsigned wordSize = sizeof(uint64_t);
    constexpr unsigned bitsInByte = 8;
    const StaticsPlan& statics = encoder.placement.statics();
    encoder.setTable(ProgramTableIndex::staticsLayout, Vector<StaticsLayout, 1> { StaticsLayout {
        .sizeFilledAtProgramEntry = statics.sizeFilledAtProgramEntry,
        .distanceFromArrayProfileToLastArray = statics.distanceFromArrayProfileToLastArray,
    } });
    Vector<StaticsOfFiles> files;
    Vector<uint32_t> importedFiles;
    for (const PlacedStaticsOfFiles& group : statics.groupsOfFilesFilledWhenEntered) {
        files.append({ group.firstWord, static_cast<uint32_t>(importedFiles.size()) });
        importedFiles.appendVector(group.importedGroups);
    }
    if (!files.isEmpty()) {
        RELEASE_ASSERT(statics.offsetOfFilledFiles == statics.endWordFilledWithFiles * wordSize);
        files.append({ statics.endWordFilledWithFiles, static_cast<uint32_t>(importedFiles.size()) });
    }
    encoder.setTable(ProgramTableIndex::staticsOfFiles, files);
    encoder.setTable(ProgramTableIndex::staticsOfImportedFiles, importedFiles);

    Vector<InitialWord> parts;
    Vector<uint32_t> narrowRoots;
    Vector<FunctionVariableData> functionVariables;
    for (unsigned index = 0; index < statics.variables.size(); ++index) {
        const PlacedVariable& placed = statics.variables[index];
        if (!placed.hasWordInStatics())
            continue;
        if (traitsOf(placed.representation).isNarrowReference)
            narrowRoots.append(placed.offset);
        if (placed.encoding == VariableEncoding::FunctionAsZero)
            functionVariables.append({ placed.offset, encoder.entrypointOf(placed.function, "what the first read makes of the variable"_s, VariableID(index)) });
        if (!placed.initialWord)
            continue;
        unsigned width = widthOf(placed.representation);
        RELEASE_ASSERT(!(placed.offset % width));
        RELEASE_ASSERT(width == wordSize || !(placed.initialWord >> (bitsInByte * width)));
        parts.append({ placed.offset / wordSize, placed.initialWord << (bitsInByte * (placed.offset % wordSize)) });
    }
    for (const PlacedStaticsPadding& padding : statics.paddingOfFilesFilledWhenEntered) {
        if (traitsOf(padding.representation).isNarrowReference)
            narrowRoots.append(padding.offset);
        parts.append({ padding.offset / wordSize, padding.initialWord << (bitsInByte * (padding.offset % wordSize)) });
    }
    AOT::sort(parts, { }, &InitialWord::index);
    Vector<InitialWord> initialWords;
    for (const InitialWord& part : parts) {
        if (!initialWords.isEmpty() && initialWords.last().index == part.index)
            initialWords.last().bits |= part.bits;
        else
            initialWords.append(part);
    }

    // As ProgramStatics divides them: a word that lies on both sides is filled with the later ones, when the instance is created. A run goes on from one group of files into the next.
    uint32_t firstWordFilledAtInstanceCreation = statics.sizeFilledAtProgramEntry / wordSize;
    uint32_t firstWordFilledWithFiles = files.isEmpty() ? UINT32_MAX : statics.firstWordFilledWithFiles;
    Vector<StaticsRun> runs;
    for (unsigned first = 0; first < initialWords.size();) {
        unsigned end = first + 1;
        while (end < initialWords.size() && initialWords[end].bits == initialWords[first].bits && initialWords[end].index == initialWords[end - 1].index + 1 && initialWords[end].index != firstWordFilledAtInstanceCreation && initialWords[end].index != firstWordFilledWithFiles)
            ++end;
        uint64_t bits = initialWords[first].bits;
        runs.append({ initialWords[first].index, end - first, static_cast<uint32_t>(bits), static_cast<uint32_t>(bits >> 32) });
        first = end;
    }
    encoder.setTable(ProgramTableIndex::staticsRuns, runs);
    AOT::sort(narrowRoots);
    encoder.setTable(ProgramTableIndex::staticsRoots, joinAdjacentRoots(statics.roots.span(), wordSize));
    encoder.setTable(ProgramTableIndex::staticsNarrowRoots, joinAdjacentRoots(narrowRoots.span(), sizeof(uint32_t)));
    encoder.setTable(ProgramTableIndex::staticsClearedEachCollection, statics.clearedEachCollection);
    AOT::sort(functionVariables, { }, &FunctionVariableData::offset);
    encoder.setTable(ProgramTableIndex::functionVariables, functionVariables);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
