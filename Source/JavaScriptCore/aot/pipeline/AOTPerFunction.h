/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include "AOTProvisionalFunctions.h"
#include "AOTVector.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

template<typename T>
class PerFunction {
public:
    PerFunction() = default;

    // Vector(size_t) leaves trivially constructible entries uninitialized.
    explicit PerFunction(unsigned numberOfFunctions)
        : m_entries(numberOfFunctions, [](size_t) { return T { }; })
    {
    }

    unsigned size() const { return m_entries.size(); }
    T& operator[](FunctionID id) { return m_entries[id.index()]; }
    const T& of(const Program&, FunctionID) const;
    void appendEntryOfLaterFunction(FunctionID, T&&);

private:
    struct EntryOfLaterFunction {
        FunctionID function;
        T entry;
    };

    Vector<T> m_entries;
    Vector<EntryOfLaterFunction> m_entriesOfLaterFunctions;
    T m_default { };
};

template<typename T>
void PerFunction<T>::appendEntryOfLaterFunction(FunctionID id, T&& entry)
{
    RELEASE_ASSERT(id.index() >= m_entries.size() && !ProvisionalFunctions::contains(id));
    RELEASE_ASSERT(m_entriesOfLaterFunctions.isEmpty() || m_entriesOfLaterFunctions.last().function.index() < id.index());
    m_entriesOfLaterFunctions.append(EntryOfLaterFunction { id, WTF::move(entry) });
}

template<typename T>
const T& PerFunction<T>::of(const Program& program, FunctionID id) const
{
    while (id && id.index() >= m_entries.size()) {
        if (!m_entriesOfLaterFunctions.isEmpty()) {
            auto later = std::ranges::lower_bound(m_entriesOfLaterFunctions, id.index(), { }, [](const EntryOfLaterFunction& entry) { return entry.function.index(); });
            if (later != m_entriesOfLaterFunctions.end() && later->function == id)
                return later->entry;
        }
        FunctionID origin = program.at(id).derivation.source;
        RELEASE_ASSERT(!origin || ProvisionalFunctions::contains(id) || origin.index() < id.index());
        id = origin;
    }
    return id ? m_entries[id.index()] : m_default;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
