/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTParameterUses.h"

#if ENABLE(AOT)

#include <algorithm>
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

static constexpr bool forwardsEveryOperand(Opcode opcode)
{
    return accessClassOf(opcode) == AccessClass::DirectCall && Value::opcodesWithFunction.contains(opcode) && !numberOfNamedOperandsOf(opcode);
}

Vector<ParameterUse, 8> classifyParameterUses(const Function& function, const BitVector& trackedEntryParameters, Vector<ForwardedParameter>* forwarded)
{
    auto& parameters = function.blocks[0].parameters;
    Vector<ParameterUse, 8> uses(FillWith { }, parameters.size(), ParameterUse::Used);
    uint32_t end = 0;
    unsigned remaining = 0;
    for (unsigned i = 0; i < parameters.size(); ++i) {
        if (!trackedEntryParameters.get(i))
            continue;
        uses[i] = ParameterUse::Unused;
        end = std::max<uint32_t>(end, parameters[i].index() + 1);
        ++remaining;
    }
    if (!remaining)
        return uses;
    auto markUsed = [&](uint32_t parameter) {
        auto& state = uses[parameter];
        remaining -= state != ParameterUse::Used;
        state = ParameterUse::Used;
    };
    Vector<std::optional<uint32_t>, 16> parameterIndex(FillWith { }, end, std::nullopt);
    for (unsigned i = 0; i < parameters.size(); ++i) {
        if (uses[i] != ParameterUse::Unused)
            continue;
        auto& index = parameterIndex[parameters[i].index()];
        if (index) {
            markUsed(*index);
            markUsed(i);
            continue;
        }
        index = i;
    }
    auto trackedParameter = [&](ValueID value) -> std::optional<uint32_t> {
        if (value.index() >= end)
            return std::nullopt;
        return parameterIndex[value.index()];
    };
    auto use = [&](ValueID value) {
        if (auto parameter = trackedParameter(value))
            markUsed(*parameter);
    };
    size_t firstNewEntry = forwarded ? forwarded->size() : 0;
    for (auto& block : function.blocks) {
        for (ValueID id : block.values) {
            auto& value = function.at(id);
            if (!forwarded || !forwardsEveryOperand(value.opcode)) {
                value.forEachChild(use);
                continue;
            }
            for (uint32_t i = 0, count = value.numberOfOperands(); i < count; ++i) {
                auto parameter = trackedParameter(value.operand(i));
                if (!parameter || uses[*parameter] == ParameterUse::Used)
                    continue;
                uses[*parameter] = ParameterUse::ForwardedOnly;
                forwarded->append({ *parameter, value.function(), i });
            }
        }
        block.forEachChildOutsideValues(use);
        if (!remaining)
            break;
    }
    if (forwarded) {
        forwarded->removeAllMatching([&](const ForwardedParameter& entry) {
            return uses[entry.parameter] == ParameterUse::Used;
        }, firstNewEntry);
    }
    return uses;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
