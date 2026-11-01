/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCFG.h"
#include "AOTIR.h"
#include "AOTThreshold.h"
#include "AOTVector.h"

namespace JSC { namespace AOT { namespace Typed {

// Finds a value among the parameters of the entry block. A long list is indexed on the first lookup, so that looking up every value of a function
// with n parameters takes n steps and not n * n. The list must not change between lookups.
class EntryParameterIndexes {
public:
    std::optional<uint32_t> find(const Function& function, ValueID id)
    {
        std::span<const ValueID> parameters = function.at(entryBlock).parameters.span();
        if (parameters.size() <= mostParametersSearched) {
            auto found = std::ranges::find(parameters, id);
            if (found == parameters.end())
                return std::nullopt;
            return static_cast<uint32_t>(found - parameters.begin());
        }
        if (m_indexes.isEmpty()) {
            m_indexes.fill(notAParameter, function.values.size());
            for (uint32_t index = 0; index < parameters.size(); ++index)
                m_indexes[parameters[index].index()] = index;
        }
        uint32_t index = id.index() < m_indexes.size() ? m_indexes[id.index()] : notAParameter;
        if (index == notAParameter)
            return std::nullopt;
        RELEASE_ASSERT(index < parameters.size() && parameters[index] == id);
        return index;
    }

private:
    AOT_CLASS_THRESHOLD(size_t, mostParametersSearched, 16);
    static constexpr uint32_t notAParameter = UINT32_MAX;

    Vector<uint32_t> m_indexes;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
