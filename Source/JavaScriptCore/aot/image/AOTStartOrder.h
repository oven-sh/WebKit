/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCompiledCode.h"
#include "AOTIRProgram.h"
#include "AOTLinkedFunctions.h"
#include "AOTNumbering.h"
#include "AOTVector.h"
#include <concepts>
#include <limits>
#include <optional>
#include <span>

namespace JSC { namespace AOT {

inline constexpr uint32_t noStartRank = std::numeric_limits<uint32_t>::max();

Vector<uint32_t> rankFunctionsByListedUnits(std::span<const uint32_t> positionsOfListedUnits, const LinkedFunctions&, size_t numberOfFunctions);

Vector<uint32_t> positionsOfRankedUnits(std::span<const uint32_t> startRanks, const LinkedFunctions&);

void forEachMentionOfListedUnits(const NumberingInputs& inputs, std::span<const uint32_t> startRanks, NOESCAPE const std::invocable<FunctionID, const Mention&, std::span<const uint32_t>> auto& visit)
{
    for (uint32_t position : positionsOfRankedUnits(startRanks, inputs.functions)) {
        const CompiledCode& code = inputs.functions[position];
        std::span<const uint32_t> words = code.body.mentionWords.span();
        for (const Mention& mention : code.body.mentions)
            visit(code.unit.function, mention, keyOf(mention, words));
    }
}

std::optional<DataID> findDataNamedBy(const Mention&, std::span<const uint32_t> key);

void forEachNodeOfData(const Typed::DataNode& root, NOESCAPE const std::invocable<const Typed::DataNode&> auto& visit)
{
    Vector<const Typed::DataNode*, 32> nodesToVisit { &root };
    while (!nodesToVisit.isEmpty()) {
        const Typed::DataNode& node = *nodesToVisit.takeLast();
        visit(node);
        if (node.kind() == Typed::DataNode::Kind::Array) {
            for (const Typed::DataNode& element : node.as<Typed::DataNode::Array>().elements)
                nodesToVisit.append(&element);
        } else if (node.kind() == Typed::DataNode::Kind::Object) {
            for (const Typed::DataMember& member : node.as<Typed::DataNode::Object>().members)
                nodesToVisit.append(&member.value);
        }
    }
}

Vector<uint32_t> listedNumbersFirst(std::span<const uint32_t> listed, size_t count);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
