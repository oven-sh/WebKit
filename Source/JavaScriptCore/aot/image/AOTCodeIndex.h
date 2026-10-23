/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTLinkedFunctions.h"
#include "AOTVector.h"
#include <algorithm>
#include <optional>
#include <span>

namespace JSC::AOT {

template<typename T>
Vector<OptionalIndex> indexByFunction(std::span<const T> elements)
{
    uint32_t endOfFunctions = 0;
    for (const T& element : elements) {
        RELEASE_ASSERT(element.function);
        endOfFunctions = std::max(endOfFunctions, element.function.index() + 1);
    }
    Vector<OptionalIndex> indices;
    indices.fill(OptionalIndex { }, endOfFunctions);
    uint32_t index = 0;
    for (const T& element : elements) {
        OptionalIndex& indexed = indices[element.function.index()];
        RELEASE_ASSERT(!indexed);
        indexed = { index++ };
    }
    return indices;
}

// Valid for one order of the bodies: a step that removes or moves a body needs a new index afterwards.
class CodeIndex {
public:
    explicit CodeIndex(std::span<const CompiledCode> functions)
        : m_positions(indexByFunction(functions))
    {
    }

    std::optional<uint32_t> indexOf(FunctionID function) const
    {
        return function.index() < m_positions.size() && m_positions[function.index()] ? std::optional<uint32_t>(*m_positions[function.index()]) : std::nullopt;
    }

private:
    Vector<OptionalIndex> m_positions;
};

// Valid for one list of aliases: a step that adds, removes or moves an alias needs a new index afterwards.
class AliasIndex {
public:
    explicit AliasIndex(std::span<const EntryAlias> aliases)
        : m_aliases(indexByFunction(aliases))
    {
    }

    OptionalIndex aliasOf(FunctionID function) const
    {
        return function.index() < m_aliases.size() ? m_aliases[function.index()] : OptionalIndex { };
    }

private:
    Vector<OptionalIndex> m_aliases;
};

} // namespace JSC::AOT

#endif // ENABLE(AOT)
