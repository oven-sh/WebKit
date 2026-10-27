/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include "AOTThreshold.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>

namespace JSC { namespace AOT { namespace Typed {

enum class RefusalKind : uint8_t { BuiltinUse, CalleeSplit, Call, TableCall, Access };

inline constexpr unsigned numberOfRefusalKinds = 5;

class RefusedSites {
public:
    static uint32_t stateOf(const Function& function, ValueID id)
    {
        constexpr uint64_t multiplier = 0x9e3779b97f4a7c15ull;
        AOT_THRESHOLD(unsigned, maxCasts, 8);
        const Value& site = function.at(id);
        auto mix = [&](uint64_t hash, const Value& value) {
            hash = (hash ^ value.hashOfWords() ^ static_cast<uint64_t>(value.opcode) << 48) * multiplier;
            hash = (hash ^ hash >> 29 ^ (static_cast<uint64_t>(value.type.index()) << 32 | value.expected().predicted().index())) * multiplier;
            return hash ^ hash >> 32;
        };
        uint64_t hash = mix(0x243f6a8885a308d3ull, site);
        for (ValueID operand : site.operands()) {
            for (unsigned casts = 0; operand; ++casts) {
                const Value& definition = function.at(operand);
                hash = mix(hash, definition);
                if (casts == maxCasts || !isCastOrConversion(definition.opcode))
                    break;
                operand = definition.operand(0);
            }
        }
        uint32_t state = static_cast<uint32_t>(hash ^ hash >> 32);
        return state ? state : 1;
    }

    bool contains(RefusalKind kind, ValueID id, uint32_t state) const
    {
        const Vector<uint32_t>& states = m_states[static_cast<unsigned>(kind)];
        return id.index() < states.size() && states[id.index()] == state;
    }

    void add(RefusalKind kind, const Function& function, ValueID id, uint32_t state)
    {
        Vector<uint32_t>& states = m_states[static_cast<unsigned>(kind)];
        if (id.index() >= states.size()) {
            size_t formerSize = states.size();
            states.grow(function.values.size());
            std::ranges::fill(states.mutableSpan().subspan(formerSize), 0);
        }
        states[id.index()] = state;
    }

    void clear(RefusalKind kind) { m_states[static_cast<unsigned>(kind)].shrink(0); }

private:
    std::array<Vector<uint32_t>, numberOfRefusalKinds> m_states;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
