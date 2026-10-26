/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBodyIndex.h"

#if ENABLE(AOT)

#include "AOTCFG.h"
#include "AOTLargeVectors.h"
#include "AOTProvisionalFunctions.h"
#include "AOTSchemaWalkers.h"
#include "AOTTypeArena.h"

namespace JSC { namespace AOT { namespace Typed {

static bool hasNeverResult(const Value& value)
{
    return value.type == TypeTable::never && resultRuleOf(value.opcode) != ResultRule::None;
}

static OptionSet<BodyTrait> bodyTraitsOf(const Block& block, BlockID id)
{
    OptionSet<BodyTrait> traits;
    traits.set(BodyTrait::Handlers, !!block.handler);
    traits.set(BodyTrait::BlockParameters, id != entryBlock && !block.parameters.isEmpty());
    traits.set(BodyTrait::SeveralTargets, block.terminator.targets.size() > 1);
    return traits;
}

static OptionSet<BodyTrait> traitsBesideBlocks(const Function& function, bool listsParameters, bool hasNeverValue)
{
    OptionSet<BodyTrait> traits;
    traits.set(BodyTrait::SeveralBlocks, function.blocks.size() > 1);
    traits.set(BodyTrait::NeverValues, hasNeverValue);
    traits.set(BodyTrait::ParametersOfRuntimeFunction, function.enclosingRuntimeFunction && listsParameters);
    traits.set(BodyTrait::UnfinishedThis, function.role == FunctionRole::AllocatedThisVersion || function.kind == FunctionKind::Initializer);
    return traits;
}

BodyContents contentsOf(const Function& function)
{
    BodyContents contents;
    bool hasNeverValue = false;
    auto add = [&](ValueID id) {
        const Value& value = function.at(id);
        contents.opcodes.add(value.opcode);
        hasNeverValue |= hasNeverResult(value);
    };
    for (uint32_t index = 0; index < function.blocks.size(); ++index) {
        const Block& block = function.blocks[index];
        for (ValueID parameter : block.parameters)
            add(parameter);
        for (ValueID value : block.values)
            add(value);
        contents.terminators.add(block.terminator.kind);
        contents.traits.add(bodyTraitsOf(block, BlockID(index)));
    }
    contents.traits.add(traitsBesideBlocks(function, contents.opcodes.contains(Opcode::Parameter), hasNeverValue));
    return contents;
}

BodyIndex::Row::Row(const Function& function, BodyIDs ids)
    : dense { static_cast<uint32_t>(function.values.size()), RecordFacts(function) }
{
    if (ids == BodyIDs::Final)
        recordStamps = computeRecordStamps(function);
    if (function.blocks.size() > 1u << IndexedValue::bitsOfBlock || function.values.size() >= 1u << IndexedName::bitsOfPosition)
        return;
    hot.isComplete = true;
    size_t numberOfListedValues = 0;
    size_t numberOfParameters = 0;
    for (const Block& block : function.blocks) {
        numberOfListedValues += block.values.size();
        numberOfParameters += block.parameters.size();
    }
    hot.listsAllValues = numberOfListedValues + numberOfParameters == function.values.size();
    if (numberOfListedValues)
        cold.listedValues = MallocSpan<IndexedValue>::malloc(numberOfListedValues * sizeof(IndexedValue));
    if (!function.blocks.isEmpty())
        cold.terminatorKinds = MallocSpan<TerminatorKind>::malloc(function.blocks.size() * sizeof(TerminatorKind));
    std::span<TerminatorKind> terminatorKinds = cold.terminatorKinds.mutableSpan();
    std::span<IndexedValue> words = cold.listedValues.mutableSpan();
    TerminatorKindSet terminators;
    bool hasNeverValue = false;
    size_t next = 0;
    for (uint32_t index = 0; index < function.blocks.size(); ++index) {
        const Block& block = function.blocks[index];
        for (ValueID parameter : block.parameters) {
            RELEASE_ASSERT(function.at(parameter).opcode == Opcode::Parameter);
            hasNeverValue |= hasNeverResult(function.at(parameter));
        }
        for (ValueID id : block.values) {
            const Value& value = function.at(id);
            hot.opcodes.add(value.opcode);
            hasNeverValue |= hasNeverResult(value);
            words[next++] = IndexedValue(id, value.opcode, BlockID(index));
        }
        terminators.add(block.terminator.kind);
        terminatorKinds[index] = block.terminator.kind;
        hot.traits.add(bodyTraitsOf(block, BlockID(index)));
    }
    hot.terminatorBits = static_cast<uint16_t>(std::bit_cast<uint64_t>(terminators));
    hot.listsParameters = numberOfParameters;
    hot.traits.add(traitsBesideBlocks(function, hot.listsParameters, hasNeverValue));
    hot.holdsExpectedTypes = function.values.containsIf([](const Value& value) {
        return !!value.expected().predicted();
    });
    if (ids == BodyIDs::Final) {
        cold.makeNames(function);
        cold.stateOfNames = static_cast<uint8_t>(State::Known);
    }
}

void BodyIndex::store(FunctionID id, Row&& row)
{
    m_hotRows[id.index()] = row.hot;
    m_coldRows[id.index()] = WTF::move(row.cold);
    m_denseRows[id.index()] = row.dense;
    m_recordStamps[id.index()] = row.recordStamps.value_or(RecordStamps { });
    m_hasRecordStamps[id.index()] = row.recordStamps.has_value();
}

void BodyIndex::ColdRow::setNames(std::array<Vector<uint32_t, 64>, 4>& lists)
{
    size_t size = 0;
    for (Vector<uint32_t, 64>& list : lists) {
        AOT::sort(list);
        removeRepeatedElements(list);
        size += list.size();
    }
    names = size ? MallocSpan<uint32_t>::malloc(size * sizeof(uint32_t)) : MallocSpan<uint32_t>();
    std::span<uint32_t> words = names.mutableSpan();
    size_t next = 0;
    for (unsigned position = 0; position < lists.size(); ++position) {
        std::ranges::copy(lists[position], words.subspan(next).begin());
        next += lists[position].size();
        if (position < endOfNames.size())
            endOfNames[position] = static_cast<uint32_t>(next);
    }
}

void BodyIndex::ColdRow::makeNames(const Function& function)
{
    std::array<Vector<uint32_t, 64>, 4> lists;
    std::array<std::array<uint32_t, 64>, 4> lastNames;
    for (auto& lastNamesOfList : lastNames)
        lastNamesOfList.fill(UINT32_MAX);
    auto collector = [&]<typename Tag>(const ID<Tag>& id) {
        constexpr int list = std::same_as<ID<Tag>, TypeID> ? 0 : std::same_as<ID<Tag>, LayoutID> ? 1 : std::same_as<ID<Tag>, LayoutSetID> ? 2 : std::same_as<ID<Tag>, VariableID> ? 3 : -1;
        if constexpr (list >= 0) {
            if (!id)
                return;
            uint32_t& last = lastNames[list][id.index() % lastNames[list].size()];
            if (last == id.index())
                return;
            last = id.index();
            lists[list].append(id.index());
        }
    };
    forEachReference(static_cast<const FunctionRecord&>(function), collector);
    for (const Value& value : function.values) {
        collector(value.type);
        collector(value.expected().predicted());
        forEachReference(value, collector);
    }
    for (const Block& block : function.blocks)
        forEachReference(block.terminator, collector);
    setNames(lists);

    Vector<IndexedName, 64> namesOfListedValues;
    std::span<const IndexedValue> listed = listedValues.span();
    for (uint32_t listedPosition = 0; listedPosition < listed.size(); ++listedPosition) {
        forEachNameOf(function.at(listed[listedPosition].value()), [&](NameKind kind, uint32_t index) {
            namesOfListedValues.append(IndexedName(listedPosition, kind, index));
        });
    }
    if (!namesOfListedValues.isEmpty()) {
        listedNames = MallocSpan<IndexedName>::malloc(namesOfListedValues.size() * sizeof(IndexedName));
        std::ranges::copy(namesOfListedValues, listedNames.mutableSpan().begin());
    }
}

void BodyIndex::Row::applyRemaps(const TypeRemap& types, const FunctionRemap& functions, const Function* functionWithChangedRecord)
{
    RELEASE_ASSERT(cold.stateOfNames == static_cast<uint8_t>(State::Known));
    for (IndexedName& name : cold.listedNames.mutableSpan()) {
        if (name.kind() == NameKind::Key)
            name = IndexedName(name.position(), NameKind::Key, types(PropertyKeyID(name.index())).index());
        else if (name.kind() == NameKind::Function)
            name = IndexedName(name.position(), NameKind::Function, functions(FunctionID(name.index())).index());
    }

    NamesInBody names = cold.namesInBody();
    bool namesProvisionalRecord = (!names.types.empty() && TypeTable::isProvisional(names.types.back())) || (!names.layoutSets.empty() && TypeTable::isProvisional(names.layoutSets.back()));
    if (namesProvisionalRecord) {
        std::array<Vector<uint32_t, 64>, 4> lists;
        for (TypeID type : names.types)
            lists[0].append(types(type).index());
        for (LayoutID layout : names.layouts)
            lists[1].append(layout.index());
        for (LayoutSetID set : names.layoutSets)
            lists[2].append(types(set).index());
        for (VariableID variable : names.variables)
            lists[3].append(variable.index());
        cold.setNames(lists);
    }
    if (functionWithChangedRecord)
        recordStamps = computeRecordStamps(*functionWithChangedRecord);
}

NamesInBody BodyIndex::ColdRow::namesInBody() const
{
    static_assert(sizeof(TypeID) == sizeof(uint32_t) && sizeof(LayoutID) == sizeof(uint32_t) && sizeof(LayoutSetID) == sizeof(uint32_t) && sizeof(VariableID) == sizeof(uint32_t));
    std::span<const uint32_t> words = names.span();
    return {
        spanReinterpretCast<const TypeID>(words.first(endOfNames[0])),
        spanReinterpretCast<const LayoutID>(words.subspan(endOfNames[0], endOfNames[1] - endOfNames[0])),
        spanReinterpretCast<const LayoutSetID>(words.subspan(endOfNames[1], endOfNames[2] - endOfNames[1])),
        spanReinterpretCast<const VariableID>(words.subspan(endOfNames[2])),
    };
}

void BodyIndex::growTo(size_t numberOfFunctions, State state)
{
    if (numberOfFunctions <= m_states.size())
        return;
    if (m_loops && numberOfFunctions > m_hotRows.capacity()) {
        size_t capacity = numberOfFunctions + numberOfFunctions / 2;
        reserveCapacityInParallel(*m_loops, m_hotRows, capacity);
        reserveCapacityInParallel(*m_loops, m_coldRows, capacity);
        reserveCapacityInParallel(*m_loops, m_denseRows, capacity);
        reserveCapacityInParallel(*m_loops, m_recordStamps, capacity);
        reserveCapacityInParallel(*m_loops, m_hasRecordStamps, capacity);
        reserveCapacityInParallel(*m_loops, m_states, capacity);
    }
    m_hotRows.grow(numberOfFunctions);
    m_coldRows.grow(numberOfFunctions);
    m_denseRows.grow(numberOfFunctions);
    m_recordStamps.grow(numberOfFunctions);
    m_hasRecordStamps.grow(numberOfFunctions);
    m_states.insertFill(m_states.size(), static_cast<uint8_t>(state), numberOfFunctions - m_states.size());
}

void BodyIndex::didEndAllEdits()
{
    for (uint8_t& state : m_states) {
        if (state == static_cast<uint8_t>(State::Written))
            state = static_cast<uint8_t>(State::Known);
        else if (state == static_cast<uint8_t>(State::GivenOut))
            state = static_cast<uint8_t>(State::Unknown);
    }
}

bool BodyIndex::write(FunctionID id, const Function& function, BodyIDs ids)
{
    if (id.index() >= m_states.size() || stateOf(id).load(std::memory_order_relaxed) != static_cast<uint8_t>(State::GivenOut))
        return false;
    return write(id, Row(function, ids));
}

bool BodyIndex::write(FunctionID id, Row&& row)
{
    if (id.index() >= m_states.size() || stateOf(id).load(std::memory_order_relaxed) != static_cast<uint8_t>(State::GivenOut))
        return false;
    store(id, WTF::move(row));
    stateOf(id).store(static_cast<uint8_t>(State::Written), std::memory_order_relaxed);
    return true;
}

void BodyIndex::didEndEdits(FunctionID id)
{
    if (id.index() >= m_states.size())
        return;
    uint8_t state = stateOf(id).load(std::memory_order_relaxed);
    if (state == static_cast<uint8_t>(State::Written))
        stateOf(id).store(static_cast<uint8_t>(State::Known), std::memory_order_release);
    else if (state == static_cast<uint8_t>(State::GivenOut))
        stateOf(id).store(static_cast<uint8_t>(State::Unknown), std::memory_order_release);
}

void BodyIndex::check(const Program& program, const ParallelLoops& loops) const
{
    forEachIndexInParallel(loops, static_cast<unsigned>(std::min(m_states.size(), program.functions.size())), [&](unsigned index) {
        if (m_states[index] != static_cast<uint8_t>(State::Known))
            return;
        const HotRow& hot = m_hotRows[index];
        const ColdRow& cold = m_coldRows[index];
        Row made(program.functions[index]);
        RELEASE_ASSERT(hot.isComplete == made.hot.isComplete && hot.listsAllValues == made.hot.listsAllValues && (hot.holdsExpectedTypes || !made.hot.holdsExpectedTypes) && hot.opcodes == made.hot.opcodes && hot.terminatorBits == made.hot.terminatorBits);
        RELEASE_ASSERT(hot.listsParameters == made.hot.listsParameters && hot.traits == made.hot.traits);
        RELEASE_ASSERT(m_denseRows[index] == made.dense);
        RELEASE_ASSERT(!m_hasRecordStamps[index] || m_recordStamps[index] == computeRecordStamps(program.functions[index]));
        RELEASE_ASSERT(std::ranges::equal(cold.listedValues.span(), made.cold.listedValues.span()));
        RELEASE_ASSERT(std::ranges::equal(cold.terminatorKinds.span(), made.cold.terminatorKinds.span()));
        if (cold.stateOfNames != static_cast<uint8_t>(State::Known))
            return;
        made.cold.makeNames(program.functions[index]);
        RELEASE_ASSERT(cold.endOfNames == made.cold.endOfNames && std::ranges::equal(cold.names.span(), made.cold.names.span()));
        RELEASE_ASSERT(std::ranges::equal(cold.listedNames.span(), made.cold.listedNames.span()));
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
