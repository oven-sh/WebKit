/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include "AOTProvisionalFunctions.h"
#include "AOTVector.h"

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

private:
    Vector<T> m_entries;
    T m_default { };
};

template<typename T>
const T& PerFunction<T>::of(const Program& program, FunctionID id) const
{
    while (id && id.index() >= m_entries.size()) {
        FunctionID origin = program.at(id).derivation.source;
        RELEASE_ASSERT(!origin || ProvisionalFunctions::contains(id) || origin.index() < id.index());
        id = origin;
    }
    return id ? m_entries[id.index()] : m_default;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
