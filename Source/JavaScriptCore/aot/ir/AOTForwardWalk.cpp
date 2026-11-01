/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTForwardWalk.h"

#if ENABLE(AOT)

#include "AOTCFG.h"
#include "AOTSort.h"

namespace JSC { namespace AOT { namespace Typed {

ForwardWalk::ForwardWalk(Function& function)
    : m_function(function)
{
}

void ForwardWalk::mapToOutput(ValueID& value, UseInFrontOfDefinition use)
{
    if (ValueID output = m_outputOf[value.index()]) [[likely]] {
        value = output;
        return;
    }
    m_usesInFrontOfDefinitions.append(use);
}

void ForwardWalk::mapUsesInFrontOfDefinitions()
{
    for (UseInFrontOfDefinition use : m_usesInFrontOfDefinitions) {
        uint32_t indexOfChild = 0;
        auto mapIfMeant = [&](ValueID& child) {
            if (indexOfChild++ != use.indexOfChild)
                return;
            child = m_outputOf[child.index()];
            RELEASE_ASSERT(child);
        };
        if (use.isOutsideValues)
            m_function.storageOfBlocks()[use.indexOfHolder].forEachChildOutsideValues(mapIfMeant);
        else
            m_function.storageOfValues()[use.indexOfHolder].forEachChild(mapIfMeant);
    }
}

ValueID ForwardWalk::emit(ValueID input)
{
    RELEASE_ASSERT(!m_outputOf[input.index()]);
    Function::Values::Storage& output = m_function.storageOfValues();
    Value value = WTF::move(m_inputValues[input.index()]);
    uint32_t indexOfChild = 0;
    value.forEachChild([&](ValueID& child) { mapToOutput(child, { false, static_cast<uint32_t>(output.size()), indexOfChild++ }); });
    ValueID id = m_function.idOfValueAt(output.size());
    m_outputOf[input.index()] = id;
    output.append(WTF::move(value));
    return id;
}

void ForwardWalk::walkBlock(BlockID input)
{
    Block block = WTF::move(m_inputBlocks[input.index()]);
    for (ValueID& parameter : block.parameters)
        parameter = emit(parameter);
    for (ValueID& value : block.values)
        value = emit(value);
    uint32_t indexOfChild = 0;
    block.forEachChildOutsideValues([&](ValueID& child) { mapToOutput(child, { true, static_cast<uint32_t>(m_function.storageOfBlocks().size()), indexOfChild++ }); });
    block.forEachTarget([&](Target& target) {
        target.block = m_outputBlockOf[target.block.index()];
        RELEASE_ASSERT(target.block);
    });
    m_function.storageOfBlocks().append(WTF::move(block));
}

Renumbering ForwardWalk::run(std::span<const BlockID> order)
{
    RELEASE_ASSERT(!order.empty() && order[0] == entryBlock);
    m_outputBlockOf = Vector<BlockID>(m_function.blocks.size());
    size_t numberOfValues = 0;
    for (size_t index = 0; index < order.size(); ++index) {
        m_outputBlockOf[order[index].index()] = BlockID(static_cast<unsigned>(index));
        const Block& block = m_function.at(order[index]);
        numberOfValues += block.parameters.size() + block.values.size();
    }

    m_inputBlocks = m_function.takeBlocks();
    m_inputValues = m_function.takeValues();
    m_outputOf = Vector<ValueID>(m_inputValues.size());
    m_function.storageOfBlocks().reserveInitialCapacity(order.size());
    m_function.storageOfValues().reserveInitialCapacity(numberOfValues);
#if AOT_STAMPS_VALUE_IDS
    m_function.stampOfValues = m_function.stampOfValues % ValueID::numberOfStamps + 1;
#endif
    for (BlockID block : order)
        walkBlock(block);
    mapUsesInFrontOfDefinitions();

    if (!m_function.writtenPositions.isEmpty()) {
        m_function.writtenPositions.removeAllMatching([&](WrittenPosition& written) {
            written.value = written.value.index() < m_outputOf.size() ? m_outputOf[written.value.index()] : ValueID();
            return !written.value;
        });
        AOT::sortInLine(m_function.writtenPositions, { }, [](const WrittenPosition& written) { return written.value.index(); });
    }
    return { WTF::move(m_outputBlockOf), WTF::move(m_outputOf) };
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
